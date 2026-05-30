#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client_mng.h"
#include "client_net.h"
#include "client_groups_mng.h"
#include "ui.h"
#include "../common/protocol.h"

/* ─── Phase 1: Lifecycle ────────────────────────────────────────────────────
 *
 * ClientMng_Create / ClientMng_Destroy / ClientMng_Run (skeleton)
 *
 * Goal: get a compiling, runnable program that opens a TCP connection,
 * shows menus, and cleans up on exit — before any handler logic exists.
 * ─────────────────────────────────────────────────────────────────────────── */

/* ─── Phase 2: Helpers + Screen 1 handlers ──────────────────────────────────
 *
 * send_and_recv   — shared send + receive used by every handler
 * status_to_message — maps a StatusCode byte to a readable string
 * handle_register — builds REGISTER_REQ TLV, shows result
 * handle_login    — builds LOGIN_REQ TLV, on success switches to SCREEN_2
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── send_and_recv ────────────────────────────────────────────────────────────
 *
 * Every handler needs the same two steps: send a request, wait for a response.
 * This helper centralises that logic so no handler duplicates it.
 *
 * Steps:
 *   1. ClientNet_SendMsg  — delivers all bytes to the server.
 *   2. Loop on ClientNet_RecvMsg until a complete TLV is assembled.
 *      RecvMsg returns 0 when bytes arrived but not yet a full TLV — keep
 *      calling until it returns > 0 (complete TLV) or < 0 (error).
 *   3. On any error: show a message, set running = 0 (fatal), return -1.
 *
 * Returns the number of bytes in recv_buf on success, -1 on error.
 * ─────────────────────────────────────────────────────────────────────────── */
static int send_and_recv(ClientMng *mng,
                         uint8_t *send_buf, uint16_t send_len,
                         uint8_t *recv_buf, int recv_size)
{
    if (ClientNet_SendMsg(mng->net, send_buf, send_len) < 0)
    {
        showMessage("Error: lost connection to server.");
        mng->running = 0;
        return -1;
    }

    int n;
    do {
        n = ClientNet_RecvMsg(mng->net, recv_buf, recv_size);
    } while (n == 0);

    if (n < 0)
    {
        showMessage("Error: lost connection to server.");
        mng->running = 0;
        return -1;
    }

    return n;
}

/* ── status_to_message ────────────────────────────────────────────────────────
 *
 * Converts the single STATUS byte from a server response into a string
 * that showMessage() can display.  Centralised here so every handler
 * just calls showMessage(status_to_message(code)) without a local switch.
 * ─────────────────────────────────────────────────────────────────────────── */
static const char *status_to_message(uint8_t status)
{
    switch ((StatusCode)status)
    {
        case STATUS_SUCCESS:                 return "Success!";
        case STATUS_USERNAME_ALREADY_EXISTS: return "Error: username already exists.";
        case STATUS_USERNAME_NOT_FOUND:      return "Error: username not found.";
        case STATUS_WRONG_PASSWORD:          return "Error: wrong password.";
        case STATUS_ALREADY_LOGGED_IN:       return "Error: already logged in.";
        case STATUS_GROUP_ALREADY_EXISTS:    return "Error: group already exists.";
        case STATUS_GROUP_NOT_FOUND:         return "Error: group not found.";
        case STATUS_ALREADY_IN_GROUP:        return "Error: already in this group.";
        case STATUS_NOT_IN_GROUP:            return "Error: not in this group.";
        case STATUS_SERVER_ERROR:            return "Error: server error.";
        default:                             return "Error: unknown response.";
    }
}


/* ── ClientMng_Create ─────────────────────────────────────────────────────── */
ClientMng *ClientMng_Create(const char *server_ip, uint16_t port)
{
    ClientMng *mng = malloc(sizeof(ClientMng));
    if (!mng)
    {
        perror("ClientMng_Create: malloc");
        return NULL;
    }

    mng->net = ClientNet_Create(server_ip, port);
    if (!mng->net)
    {
        free(mng);
        return NULL;
    }

    mng->groups = ClientGroupsMng_Create();
    if (!mng->groups)
    {
        ClientNet_Destroy(mng->net);
        free(mng);
        return NULL;
    }

    mng->state   = SCREEN_1;
    mng->running = 1;

    return mng;
}

/* ── ClientMng_Destroy ────────────────────────────────────────────────────── */
void ClientMng_Destroy(ClientMng *mng)
{
    if (!mng) return;
    ClientGroupsMng_Destroy(mng->groups);
    ClientNet_Destroy(mng->net);
    free(mng);
}

/* ── handle_register ──────────────────────────────────────────────────────────
 *
 * TLV structure built here:
 *   Outer: [ TAG_REGISTER_REQ ][ total_inner_len ]
 *   Inner: [ TAG_USERNAME ][ len ][ username ]
 *          [ TAG_PASSWORD ][ len ][ password ]
 *
 * Steps:
 *   1. getCredentials — collect username + password from the user.
 *   2. tlv_encode x2  — encode the two inner field TLVs back-to-back.
 *   3. tlv_encode x1  — wrap them in the outer REGISTER_REQ TLV.
 *   4. send_and_recv  — deliver the request, wait for REGISTER_RESP.
 *   5. tlv_decode     — unpack the response outer TLV.
 *   6. tlv_find_field — locate the TAG_STATUS byte inside the response.
 *   7. showMessage    — display the result string.
 * ─────────────────────────────────────────────────────────────────────────── */
static void handle_register(ClientMng *mng)
{
    Credentials cred;
    getCredentials(&cred);

    /* Step 2: build inner fields */
    uint8_t inner[MAX_TLV_SIZE];
    int inner_len = 0;
    int n;

    n = tlv_encode(inner + inner_len, sizeof(inner) - (size_t)inner_len,
                   TAG_USERNAME,
                   (const uint8_t *)cred.username,
                   (uint16_t)strlen(cred.username));
    if (n < 0) return;
    inner_len += n;

    n = tlv_encode(inner + inner_len, sizeof(inner) - (size_t)inner_len,
                   TAG_PASSWORD,
                   (const uint8_t *)cred.password,
                   (uint16_t)strlen(cred.password));
    if (n < 0) return;
    inner_len += n;

    /* Step 3: wrap in outer REGISTER_REQ */
    uint8_t msg[MAX_TLV_SIZE];
    int msg_len = tlv_encode(msg, sizeof(msg),
                             TAG_REGISTER_REQ,
                             inner, (uint16_t)inner_len);
    if (msg_len < 0) return;

    /* Step 4: send + receive */
    uint8_t resp[MAX_TLV_SIZE];
    int resp_len = send_and_recv(mng, msg, (uint16_t)msg_len,
                                 resp, (int)sizeof(resp));
    if (resp_len < 0) return;

    /* Steps 5-6: decode response, find status */
    uint8_t out_tag;
    const uint8_t *value;
    uint16_t value_len;
    if (tlv_decode(resp, (size_t)resp_len, &out_tag, &value, &value_len) < 0) return;

    const uint8_t *status_val;
    uint16_t status_len;
    if (tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len) < 0) return;

    /* Step 7: show result */
    showMessage(status_to_message(status_val[0]));
}

/* ── handle_login ─────────────────────────────────────────────────────────────
 *
 * Identical TLV structure to handle_register but uses TAG_LOGIN_REQ.
 * The key difference: on STATUS_SUCCESS we flip mng->state to SCREEN_2,
 * transitioning the Run loop to the group menu.
 * ─────────────────────────────────────────────────────────────────────────── */
static void handle_login(ClientMng *mng)
{
    Credentials cred;
    getCredentials(&cred);

    /* Build inner fields */
    uint8_t inner[MAX_TLV_SIZE];
    int inner_len = 0;
    int n;

    n = tlv_encode(inner + inner_len, sizeof(inner) - (size_t)inner_len,
                   TAG_USERNAME,
                   (const uint8_t *)cred.username,
                   (uint16_t)strlen(cred.username));
    if (n < 0) return;
    inner_len += n;

    n = tlv_encode(inner + inner_len, sizeof(inner) - (size_t)inner_len,
                   TAG_PASSWORD,
                   (const uint8_t *)cred.password,
                   (uint16_t)strlen(cred.password));
    if (n < 0) return;
    inner_len += n;

    /* Wrap in outer LOGIN_REQ */
    uint8_t msg[MAX_TLV_SIZE];
    int msg_len = tlv_encode(msg, sizeof(msg),
                             TAG_LOGIN_REQ,
                             inner, (uint16_t)inner_len);
    if (msg_len < 0) return;

    /* Send + receive */
    uint8_t resp[MAX_TLV_SIZE];
    int resp_len = send_and_recv(mng, msg, (uint16_t)msg_len,
                                 resp, (int)sizeof(resp));
    if (resp_len < 0) return;

    /* Decode response */
    uint8_t out_tag;
    const uint8_t *value;
    uint16_t value_len;
    if (tlv_decode(resp, (size_t)resp_len, &out_tag, &value, &value_len) < 0) return;

    const uint8_t *status_val;
    uint16_t status_len;
    if (tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len) < 0) return;

    showMessage(status_to_message(status_val[0]));

    /* Only on success: switch to Screen 2 */
    if ((StatusCode)status_val[0] == STATUS_SUCCESS)
        mng->state = SCREEN_2;
}

/* ── handle_create_group ──────────────────────────────────────────────────────
 *
 * TLV structure sent:
 *   Outer: [ TAG_CREATE_GROUP_REQ ][ inner_len ]
 *   Inner: [ TAG_GROUP_NAME ][ len ][ group_name ]
 *
 * Response value block contains THREE fields (not just status):
 *   [ TAG_STATUS  ][ 1  ][ code ]
 *   [ TAG_MC_IP   ][ len][ "239.0.0.x" ]
 *   [ TAG_MC_PORT ][ 2  ][ port in network byte order ]
 *
 * We must extract all three on SUCCESS so the multicast info
 * can be passed to client_groups_mng (Phase 3) and used to
 * launch the sender/receiver terminal windows (Phase 4).
 * ─────────────────────────────────────────────────────────────────────────── */
static void handle_create_group(ClientMng *mng)
{
    char group_name[GROUP_NAME_SIZE];
    getGroupName(group_name, GROUP_NAME_SIZE);

    /* Build inner field: just the group name */
    uint8_t inner[MAX_TLV_SIZE];
    int inner_len = tlv_encode(inner, sizeof(inner),
                               TAG_GROUP_NAME,
                               (const uint8_t *)group_name,
                               (uint16_t)strlen(group_name));
    if (inner_len < 0) return;

    /* Wrap in outer CREATE_GROUP_REQ */
    uint8_t msg[MAX_TLV_SIZE];
    int msg_len = tlv_encode(msg, sizeof(msg),
                             TAG_CREATE_GROUP_REQ,
                             inner, (uint16_t)inner_len);
    if (msg_len < 0) return;

    uint8_t resp[MAX_TLV_SIZE];
    int resp_len = send_and_recv(mng, msg, (uint16_t)msg_len,
                                 resp, (int)sizeof(resp));
    if (resp_len < 0) return;

    /* Decode outer response to get the value block */
    uint8_t out_tag;
    const uint8_t *value;
    uint16_t value_len;
    if (tlv_decode(resp, (size_t)resp_len, &out_tag, &value, &value_len) < 0) return;

    /* STATUS is always present — check it first */
    const uint8_t *status_val;
    uint16_t status_len;
    if (tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len) < 0) return;

    showMessage(status_to_message(status_val[0]));

    if ((StatusCode)status_val[0] != STATUS_SUCCESS) return;

    /* On success: extract MC_IP (a UTF-8 string, NOT null-terminated in TLV) */
    const uint8_t *ip_val;
    uint16_t ip_len;
    if (tlv_find_field(value, value_len, TAG_MC_IP, &ip_val, &ip_len) < 0) return;

    /* Copy and null-terminate the IP string so we can use it as a C string */
    char mc_ip[64];
    memcpy(mc_ip, ip_val, ip_len);
    mc_ip[ip_len] = '\0';

    /* On success: extract MC_PORT — 2 bytes in network byte order (big-endian).
     * Reconstruct as host uint16_t: high byte is [0], low byte is [1]. */
    const uint8_t *port_val;
    uint16_t port_field_len;
    if (tlv_find_field(value, value_len, TAG_MC_PORT, &port_val, &port_field_len) < 0) return;

    uint16_t mc_port = ((uint16_t)port_val[0] << 8) | port_val[1];

    printf("  Multicast IP: %s  Port: %u\n", mc_ip, mc_port);

    ClientGroupsMng_Add(mng->groups, group_name, mc_ip, mc_port);

    /* TODO (Phase 4): launch mc_sender + mc_receiver via gnome-terminal,
     *   then set PIDs:
     *   GroupEntry *e = ClientGroupsMng_Find(mng->groups, group_name);
     *   e->sender_pid   = sender_pid;
     *   e->receiver_pid = receiver_pid;                                   */
}

/* ── handle_join_group ────────────────────────────────────────────────────────
 *
 * Identical flow to handle_create_group — same request structure,
 * same response structure (STATUS + MC_IP + MC_PORT).
 * Only difference is the outer tag: TAG_JOIN_GROUP_REQ.
 * ─────────────────────────────────────────────────────────────────────────── */
static void handle_join_group(ClientMng *mng)
{
    char group_name[GROUP_NAME_SIZE];
    getGroupName(group_name, GROUP_NAME_SIZE);

    uint8_t inner[MAX_TLV_SIZE];
    int inner_len = tlv_encode(inner, sizeof(inner),
                               TAG_GROUP_NAME,
                               (const uint8_t *)group_name,
                               (uint16_t)strlen(group_name));
    if (inner_len < 0) return;

    uint8_t msg[MAX_TLV_SIZE];
    int msg_len = tlv_encode(msg, sizeof(msg),
                             TAG_JOIN_GROUP_REQ,
                             inner, (uint16_t)inner_len);
    if (msg_len < 0) return;

    uint8_t resp[MAX_TLV_SIZE];
    int resp_len = send_and_recv(mng, msg, (uint16_t)msg_len,
                                 resp, (int)sizeof(resp));
    if (resp_len < 0) return;

    uint8_t out_tag;
    const uint8_t *value;
    uint16_t value_len;
    if (tlv_decode(resp, (size_t)resp_len, &out_tag, &value, &value_len) < 0) return;

    const uint8_t *status_val;
    uint16_t status_len;
    if (tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len) < 0) return;

    showMessage(status_to_message(status_val[0]));

    if ((StatusCode)status_val[0] != STATUS_SUCCESS) return;

    /* Extract MC_IP — copy and null-terminate */
    const uint8_t *ip_val;
    uint16_t ip_len;
    if (tlv_find_field(value, value_len, TAG_MC_IP, &ip_val, &ip_len) < 0) return;

    char mc_ip[64];
    memcpy(mc_ip, ip_val, ip_len);
    mc_ip[ip_len] = '\0';

    /* Extract MC_PORT — 2 bytes big-endian → host uint16_t */
    const uint8_t *port_val;
    uint16_t port_field_len;
    if (tlv_find_field(value, value_len, TAG_MC_PORT, &port_val, &port_field_len) < 0) return;

    uint16_t mc_port = ((uint16_t)port_val[0] << 8) | port_val[1];

    printf("  Multicast IP: %s  Port: %u\n", mc_ip, mc_port);

    ClientGroupsMng_Add(mng->groups, group_name, mc_ip, mc_port);

    /* TODO (Phase 4): launch mc_sender + mc_receiver via gnome-terminal,
     *   then set PIDs:
     *   GroupEntry *e = ClientGroupsMng_Find(mng->groups, group_name);
     *   e->sender_pid   = sender_pid;
     *   e->receiver_pid = receiver_pid;                                   */
}

/* ── handle_leave_group ───────────────────────────────────────────────────────
 *
 * TLV structure sent:
 *   Outer: [ TAG_LEAVE_GROUP_REQ ][ inner_len ]
 *   Inner: [ TAG_GROUP_NAME ][ len ][ group_name ]
 *
 * Response value block contains only TAG_STATUS — no multicast info
 * is returned on leave.
 *
 * On success: the server has decremented the member count and may have
 * destroyed the group.  We must kill the sender + receiver processes
 * that were launched for this group (Phase 4 — via client_groups_mng).
 * ─────────────────────────────────────────────────────────────────────────── */
static void handle_leave_group(ClientMng *mng)
{
    char group_name[GROUP_NAME_SIZE];
    getGroupName(group_name, GROUP_NAME_SIZE);

    uint8_t inner[MAX_TLV_SIZE];
    int inner_len = tlv_encode(inner, sizeof(inner),
                               TAG_GROUP_NAME,
                               (const uint8_t *)group_name,
                               (uint16_t)strlen(group_name));
    if (inner_len < 0) return;

    uint8_t msg[MAX_TLV_SIZE];
    int msg_len = tlv_encode(msg, sizeof(msg),
                             TAG_LEAVE_GROUP_REQ,
                             inner, (uint16_t)inner_len);
    if (msg_len < 0) return;

    uint8_t resp[MAX_TLV_SIZE];
    int resp_len = send_and_recv(mng, msg, (uint16_t)msg_len,
                                 resp, (int)sizeof(resp));
    if (resp_len < 0) return;

    uint8_t out_tag;
    const uint8_t *value;
    uint16_t value_len;
    if (tlv_decode(resp, (size_t)resp_len, &out_tag, &value, &value_len) < 0) return;

    const uint8_t *status_val;
    uint16_t status_len;
    if (tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len) < 0) return;

    showMessage(status_to_message(status_val[0]));

    /* ClientGroupsMng_Remove kills the sender + receiver PIDs internally
     * before freeing the entry — no need to call kill() here.            */
    if ((StatusCode)status_val[0] == STATUS_SUCCESS)
        ClientGroupsMng_Remove(mng->groups, group_name);
}

/* ── handle_logout ────────────────────────────────────────────────────────────
 *
 * LOGOUT_REQ carries no payload — the server identifies the user from the
 * TCP connection's client_id, so we send nothing but the outer tag.
 *
 * TLV structure:
 *   [ TAG_LOGOUT_REQ ][ 0x00 0x00 ]   ← length = 0, no value bytes
 *
 * Steps:
 *   1. tlv_encode with value=NULL, value_len=0 → 3-byte message.
 *   2. send_and_recv → wait for LOGOUT_RESP.
 *   3. Find TAG_STATUS in the response.
 *   4. On STATUS_SUCCESS: switch back to SCREEN_1.
 * ─────────────────────────────────────────────────────────────────────────── */
static void handle_logout(ClientMng *mng)
{
    /* Step 1: build the empty LOGOUT_REQ (no inner fields) */
    uint8_t msg[TLV_HEADER_SIZE];
    int msg_len = tlv_encode(msg, sizeof(msg),
                             TAG_LOGOUT_REQ,
                             NULL, 0);
    if (msg_len < 0) return;

    /* Step 2: send + receive */
    uint8_t resp[MAX_TLV_SIZE];
    int resp_len = send_and_recv(mng, msg, (uint16_t)msg_len,
                                 resp, (int)sizeof(resp));
    if (resp_len < 0) return;

    /* Step 3: decode response, find status */
    uint8_t out_tag;
    const uint8_t *value;
    uint16_t value_len;
    if (tlv_decode(resp, (size_t)resp_len, &out_tag, &value, &value_len) < 0) return;

    const uint8_t *status_val;
    uint16_t status_len;
    if (tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len) < 0) return;

    showMessage(status_to_message(status_val[0]));

    /* Step 4: on success — kill all chat windows, clear groups, go to Screen 1 */
    if ((StatusCode)status_val[0] == STATUS_SUCCESS)
    {
        ClientGroupsMng_RemoveAll(mng->groups);
        mng->state = SCREEN_1;
    }
}

/* ── ClientMng_Run ────────────────────────────────────────────────────────── */
void ClientMng_Run(ClientMng *mng)
{
    while (mng->running)
    {
        if (mng->state == SCREEN_1)
        {
            showMenu();
            MenuOption choice = getMenuChoice();

            switch (choice)
            {
                case MENU_REGISTER: handle_register(mng); break;
                case MENU_LOGIN:    handle_login(mng);    break;
                case MENU_EXIT:     mng->running = 0;     break;
                case MENU_INVALID:
                default:
                    showMessage("Invalid option, try again.");
                    break;
            }
        }
        else /* SCREEN_2 */
        {
            showGroupMenu();
            GroupMenuOption choice = getGroupMenuChoice();

            switch (choice)
            {
                case GROUP_MENU_CREATE:  handle_create_group(mng); break;
                case GROUP_MENU_JOIN:    handle_join_group(mng);   break;
                case GROUP_MENU_LEAVE:   handle_leave_group(mng);  break;
                case GROUP_MENU_LOGOUT:  handle_logout(mng);       break;
                case GROUP_MENU_INVALID:
                default:
                    showMessage("Invalid option, try again.");
                    break;
            }
        }
    }
}