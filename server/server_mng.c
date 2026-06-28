#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "server_mng.h"
#include "server_net.h"
#include "user_mng.h"
#include "group_mng.h"
#include "logger.h"
#include "../common/protocol.h"

#define MAX_MSG_SIZE  512

/* ─── Manager struct (opaque outside this file) ─────────────────────────── */
struct ServerMng {
    UserMng   *user_mng;
    GroupMng  *group_mng;
    ServerNet *net;
};

/* Single file-level pointer to bridge the ServerNet callbacks back to
   the manager — callbacks receive no context parameter so we store it here. */
static ServerMng *s_mng = NULL;

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

ServerMng *ServerMng_Create(int port)
{
    ServerMng *mng = malloc(sizeof(ServerMng));
    if (!mng)
    {
        return NULL;
    }

    mng->user_mng = UserMng_Create();
    if (!mng->user_mng)
    {
        free(mng);
        return NULL;
    }

    mng->group_mng = GroupMng_Create();
    if (!mng->group_mng)
    {
        UserMng_Destroy(&mng->user_mng);
        free(mng);
        return NULL;
    }

    mng->net = ServerNet_Create(port, on_message, on_disconnect);
    if (!mng->net)
    {
        GroupMng_Destroy(&mng->group_mng);
        UserMng_Destroy(&mng->user_mng);
        free(mng);
        return NULL;
    }

    s_mng = mng;
    return mng;
}

void ServerMng_Run(ServerMng *mng)
{
    ServerNet_Run(mng->net);
}

void ServerMng_Destroy(ServerMng **mng)
{
    if (!mng || !*mng)
    {
        return;
    }

    ServerNet_Destroy(&(*mng)->net);
    GroupMng_Destroy(&(*mng)->group_mng);
    UserMng_Destroy(&(*mng)->user_mng);
    free(*mng);
    *mng  = NULL;
    s_mng = NULL;
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
    ServerNet_SendMsg(s_mng->net, client_id, buf, total_len);
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
    ServerNet_SendMsg(s_mng->net, client_id, buf, total_len);
}

/* Routes incoming messages to the correct handler based on the TLV tag. */
static void on_message(int client_id, const uint8_t *msg, int msg_len)
{
    uint8_t        tag;
    const uint8_t *val;
    uint16_t       val_len;

    if (tlv_decode(msg, msg_len, &tag, &val, &val_len) < 0)
    {
        return;
    }

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
    User *user = UserMng_GetByClientId(s_mng->user_mng, client_id);
    if (user)
    {
        log_event(LOG_WARN, "ServerMng", "client %d ('%s') disconnected abruptly", client_id, user->username);
    }
    else
    {
        log_event(LOG_WARN, "ServerMng", "client %d disconnected (not logged in)", client_id);
    }
    GroupMng_RemoveClientFromAll(s_mng->group_mng, client_id);
    UserMng_LogoutByClientId(s_mng->user_mng, client_id);
}

/* ─── Handlers ─────────────────────────────────────────────────────────── */

static void handle_register(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *username;  uint16_t username_len;
    const uint8_t *password;  uint16_t password_len;

    if (tlv_find_field(val, len, TAG_USERNAME, &username, &username_len) != PROTOCOL_SUCCESS)
    {
        return;
    }
    if (tlv_find_field(val, len, TAG_PASSWORD, &password, &password_len) != PROTOCOL_SUCCESS)
    {
        return;
    }

    char uname[MAX_NAME_LEN] = {0};
    char pword[MAX_NAME_LEN] = {0};
    strncpy(uname, (const char *)username, MAX_NAME_LEN - 1);
    strncpy(pword, (const char *)password, MAX_NAME_LEN - 1);

    StatusCode status = UserMng_Register(s_mng->user_mng, uname, pword);
    if (status == STATUS_SUCCESS)
    {
        log_event(LOG_INFO, "ServerMng", "user '%s' registered", uname);
    }
    else
    {
        log_event(LOG_WARN, "ServerMng", "register failed for '%s' (status=%d)", uname, status);
    }
    send_status(client_id, TAG_REGISTER_RESP, status);
}

static void handle_login(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *username;  uint16_t username_len;
    const uint8_t *password;  uint16_t password_len;

    if (tlv_find_field(val, len, TAG_USERNAME, &username, &username_len) != PROTOCOL_SUCCESS)
    {
        return;
    }
    if (tlv_find_field(val, len, TAG_PASSWORD, &password, &password_len) != PROTOCOL_SUCCESS)
    {
        return;
    }

    char uname[MAX_NAME_LEN] = {0};
    char pword[MAX_NAME_LEN] = {0};
    strncpy(uname, (const char *)username, MAX_NAME_LEN - 1);
    strncpy(pword, (const char *)password, MAX_NAME_LEN - 1);

    StatusCode status = UserMng_Login(s_mng->user_mng, uname, pword, client_id);
    if (status == STATUS_SUCCESS)
    {
        log_event(LOG_INFO, "ServerMng", "user '%s' logged in (client %d)", uname, client_id);
    }
    else
    {
        log_event(LOG_WARN, "ServerMng", "login failed for '%s' (status=%d)", uname, status);
    }
    send_status(client_id, TAG_LOGIN_RESP, status);
}

static void handle_logout(int client_id)
{
    User *user = UserMng_GetByClientId(s_mng->user_mng, client_id);
    if (user)
    {
        log_event(LOG_INFO, "ServerMng", "user '%s' logged out (client %d)", user->username, client_id);
    }
    GroupMng_RemoveClientFromAll(s_mng->group_mng, client_id);
    UserMng_LogoutByClientId(s_mng->user_mng, client_id);
    send_status(client_id, TAG_LOGOUT_RESP, STATUS_SUCCESS);
}

static void handle_create_group(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *grpname;  uint16_t grpname_len;
    if (tlv_find_field(val, len, TAG_GROUP_NAME, &grpname, &grpname_len) != PROTOCOL_SUCCESS)
    {
        return;
    }

    char gname[MAX_NAME_LEN] = {0};
    strncpy(gname, (const char *)grpname, MAX_NAME_LEN - 1);

    char mc_ip[32]   = {0};
    uint16_t mc_port = 0;

    StatusCode status = GroupMng_CreateGroup(s_mng->group_mng, gname, client_id, mc_ip, &mc_port);
    if (status != STATUS_SUCCESS)
    {
        log_event(LOG_WARN, "ServerMng", "create group '%s' failed (status=%d)", gname, status);
        send_status(client_id, TAG_CREATE_GROUP_RESP, status);
        return;
    }

    log_event(LOG_INFO, "ServerMng", "group '%s' created (IP=%s port=%u)", gname, mc_ip, mc_port);
    send_group_resp(client_id, TAG_CREATE_GROUP_RESP, mc_ip, mc_port);
}

static void handle_join_group(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *grpname;  uint16_t grpname_len;
    if (tlv_find_field(val, len, TAG_GROUP_NAME, &grpname, &grpname_len) != PROTOCOL_SUCCESS)
    {
        return;
    }

    char gname[MAX_NAME_LEN] = {0};
    strncpy(gname, (const char *)grpname, MAX_NAME_LEN - 1);

    char mc_ip[32]   = {0};
    uint16_t mc_port = 0;

    StatusCode status = GroupMng_Join(s_mng->group_mng, gname, client_id, mc_ip, &mc_port);
    if (status != STATUS_SUCCESS)
    {
        log_event(LOG_WARN, "ServerMng", "join group '%s' failed (status=%d)", gname, status);
        send_status(client_id, TAG_JOIN_GROUP_RESP, status);
        return;
    }

    log_event(LOG_INFO, "ServerMng", "client %d joined group '%s'", client_id, gname);
    send_group_resp(client_id, TAG_JOIN_GROUP_RESP, mc_ip, mc_port);
}

static void handle_leave_group(int client_id, const uint8_t *val, uint16_t len)
{
    const uint8_t *grpname;  uint16_t grpname_len;
    if (tlv_find_field(val, len, TAG_GROUP_NAME, &grpname, &grpname_len) != PROTOCOL_SUCCESS)
    {
        return;
    }

    char gname[MAX_NAME_LEN] = {0};
    strncpy(gname, (const char *)grpname, MAX_NAME_LEN - 1);

    StatusCode status = GroupMng_Leave(s_mng->group_mng, gname, client_id);
    if (status == STATUS_SUCCESS)
    {
        log_event(LOG_INFO, "ServerMng", "client %d left group '%s'", client_id, gname);
    }
    else
    {
        log_event(LOG_WARN, "ServerMng", "leave group '%s' failed (status=%d)", gname, status);
    }
    send_status(client_id, TAG_LEAVE_GROUP_RESP, status);
}
