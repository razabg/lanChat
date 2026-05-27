#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "server_mng.h"
#include "server_net.h"
#include "user_mng.h"
#include "group_mng.h"
#include "../common/protocol.h"

#define MAX_NAME_LEN  64
#define MAX_MSG_SIZE  512

/* ─── Forward declarations ─────────────────────────────────────────────── */
static void on_message(int client_id, const uint8_t *msg, int msg_len);
static void on_disconnect(int client_id);

static void handle_register    (int client_id, const uint8_t *val, uint16_t len);
static void handle_login       (int client_id, const uint8_t *val, uint16_t len);
static void handle_logout      (int client_id);
static void handle_create_group(int client_id, const uint8_t *val, uint16_t len);
static void handle_join_group  (int client_id, const uint8_t *val, uint16_t len);
static void handle_leave_group (int client_id, const uint8_t *val, uint16_t len);

static void send_status(int client_id, uint8_t resp_tag, StatusCode code);
static void send_group_resp(int client_id, uint8_t resp_tag, const char *mc_ip, uint16_t mc_port);

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

int ServerMng_Create(int port)
{
    if (UserMng_Create() < 0)  return -1;
    if (GroupMng_Create() < 0) return -1;

    if (ServerNet_Create(port, on_message, on_disconnect) < 0) return -1;

    return 0;
}

void ServerMng_Run(void)
{
    ServerNet_Run();
}

void ServerMng_Destroy(void)
{
    ServerNet_Destroy();
    GroupMng_Destroy();
    UserMng_Destroy();
}

/* ═══════════════════════════════════════════════════════════════════════════
   STATIC HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

/* Sends a response containing only a STATUS field. */
static void send_status(int client_id, uint8_t resp_tag, StatusCode code)
{
    uint8_t inner[8];
    uint8_t buf[MAX_MSG_SIZE];
    uint8_t status_byte = (uint8_t)code;

    int inner_len = tlv_encode(inner, sizeof(inner), TAG_STATUS, &status_byte, 1);
    int total_len = tlv_encode(buf, sizeof(buf), resp_tag, inner, inner_len);
    ServerNet_SendMsg(client_id, buf, total_len);
}

/* Sends a response containing STATUS + MC_IP + MC_PORT (used for create/join). */
static void send_group_resp(int client_id, uint8_t resp_tag, const char *mc_ip, uint16_t mc_port)
{
    uint8_t inner[64];
    uint8_t buf[MAX_MSG_SIZE];
    int inner_len = 0;

    uint8_t status_byte = STATUS_SUCCESS;
    inner_len += tlv_encode(inner + inner_len, sizeof(inner) - inner_len,
                            TAG_STATUS, &status_byte, 1);
    inner_len += tlv_encode(inner + inner_len, sizeof(inner) - inner_len,
                            TAG_MC_IP, (const uint8_t *)mc_ip, strlen(mc_ip));
    uint16_t port_net = htons(mc_port);
    inner_len += tlv_encode(inner + inner_len, sizeof(inner) - inner_len,
                            TAG_MC_PORT, (uint8_t *)&port_net, 2);

    int total_len = tlv_encode(buf, sizeof(buf), resp_tag, inner, inner_len);
    ServerNet_SendMsg(client_id, buf, total_len);
}

/* Routes incoming messages to the correct handler based on the TLV tag. */
static void on_message(int client_id, const uint8_t *msg, int msg_len)
{
    uint8_t        tag;
    const uint8_t *val;
    uint16_t       val_len;

    if (tlv_decode(msg, msg_len, &tag, &val, &val_len) < 0)
        return;

    switch (tag) {
        case TAG_REGISTER_REQ:     handle_register    (client_id, val, val_len); break;
        case TAG_LOGIN_REQ:        handle_login       (client_id, val, val_len); break;
        case TAG_LOGOUT_REQ:       handle_logout      (client_id);               break;
        case TAG_CREATE_GROUP_REQ: handle_create_group(client_id, val, val_len); break;
        case TAG_JOIN_GROUP_REQ:   handle_join_group  (client_id, val, val_len); break;
        case TAG_LEAVE_GROUP_REQ:  handle_leave_group (client_id, val, val_len); break;
        default: break;
    }
}

/* Handles abrupt disconnects — removes user from all groups and logs them out. */
static void on_disconnect(int client_id)
{
    GroupMng_RemoveClientFromAll(client_id);
    UserMng_LogoutByClientId(client_id);
}

/* ─── Handlers ─────────────────────────────────────────────────────────── */

static void handle_register(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *username;  uint16_t username_len;
    const uint8_t *password;  uint16_t password_len;

    if (tlv_find_field(val, len, TAG_USERNAME, &username, &username_len) < 0) return;
    if (tlv_find_field(val, len, TAG_PASSWORD, &password, &password_len) < 0) return;

    char uname[MAX_NAME_LEN] = {0};
    char pword[MAX_NAME_LEN] = {0};
    memcpy(uname, username, username_len);
    memcpy(pword, password, password_len);

    StatusCode status = UserMng_Register(uname, pword);
    send_status(client_id, TAG_REGISTER_RESP, status);
}

static void handle_login(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *username;  uint16_t username_len;
    const uint8_t *password;  uint16_t password_len;

    if (tlv_find_field(val, len, TAG_USERNAME, &username, &username_len) < 0) return;
    if (tlv_find_field(val, len, TAG_PASSWORD, &password, &password_len) < 0) return;

    char uname[MAX_NAME_LEN] = {0};
    char pword[MAX_NAME_LEN] = {0};
    memcpy(uname, username, username_len);
    memcpy(pword, password, password_len);

    StatusCode status = UserMng_Login(uname, pword, client_id);
    send_status(client_id, TAG_LOGIN_RESP, status);
}

static void handle_logout(int client_id)
{
    GroupMng_RemoveClientFromAll(client_id);
    UserMng_LogoutByClientId(client_id);
    send_status(client_id, TAG_LOGOUT_RESP, STATUS_SUCCESS);
}

static void handle_create_group(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *grpname;  uint16_t grpname_len;
    if (tlv_find_field(val, len, TAG_GROUP_NAME, &grpname, &grpname_len) < 0) return;

    char gname[MAX_NAME_LEN] = {0};
    memcpy(gname, grpname, grpname_len);

    char mc_ip[32]   = {0};
    uint16_t mc_port = 0;

    StatusCode status = GroupMng_Create(gname, client_id, mc_ip, &mc_port);
    if (status != STATUS_SUCCESS) {
        send_status(client_id, TAG_CREATE_GROUP_RESP, status);
        return;
    }

    send_group_resp(client_id, TAG_CREATE_GROUP_RESP, mc_ip, mc_port);
}

static void handle_join_group(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *grpname;  uint16_t grpname_len;
    if (tlv_find_field(val, len, TAG_GROUP_NAME, &grpname, &grpname_len) < 0) return;

    char gname[MAX_NAME_LEN] = {0};
    memcpy(gname, grpname, grpname_len);

    char mc_ip[32]   = {0};
    uint16_t mc_port = 0;

    StatusCode status = GroupMng_Join(gname, client_id, mc_ip, &mc_port);
    if (status != STATUS_SUCCESS) {
        send_status(client_id, TAG_JOIN_GROUP_RESP, status);
        return;
    }

    send_group_resp(client_id, TAG_JOIN_GROUP_RESP, mc_ip, mc_port);
}

static void handle_leave_group(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *grpname;  uint16_t grpname_len;
    if (tlv_find_field(val, len, TAG_GROUP_NAME, &grpname, &grpname_len) < 0) return;

    char gname[MAX_NAME_LEN] = {0};
    memcpy(gname, grpname, grpname_len);

    StatusCode status = GroupMng_Leave(gname, client_id);
    send_status(client_id, TAG_LEAVE_GROUP_RESP, status);
}
