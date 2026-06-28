#ifndef GROUP_MNG_H
#define GROUP_MNG_H

#include <stdint.h>
#include <ds/genDList.h>
#include "../common/protocol.h"

#define MAX_NAME_LEN 64
#define MAX_IP_LEN   32

/* ─── GroupMng Result Codes ─────────────────────────────────────────────── */
typedef enum {
    GROUP_MNG_OK,
    GROUP_MNG_ALLOC_ERROR
} GroupMngResult;

/* ─── Opaque manager type ───────────────────────────────────────────────── */
typedef struct GroupMng GroupMng;

/* ─── Group record (public — used by ServerMng) ─────────────────────────── */
typedef struct {
    char     group_name[MAX_NAME_LEN];
    char     mc_ip[MAX_IP_LEN];
    uint16_t mc_port;
    int      member_count;
    List    *members;   /* list of int* client_ids */
} Group;

//************ GroupMng API *************

/**
 * @brief Initialize the group manager, hash table, and free multicast IP queue
 *
 * @return GroupMng* on success, NULL on failure
 */
GroupMng *GroupMng_Create(void);

/**
 * @brief Free all group records and destroy the manager
 *
 * @param group_mng : pointer to the manager pointer (set to NULL on destroy)
 */
void GroupMng_Destroy(GroupMng **group_mng);

/**
 * @brief Create a new group and assign a multicast IP and port
 *
 * @param group_mng  : the manager
 * @param group_name : name of the group to create
 * @param client_id  : client id of the creator (auto-joined)
 * @param out_mc_ip  : buffer to receive the assigned multicast IP string
 * @param out_mc_port: receives the assigned multicast port
 * @return STATUS_SUCCESS or STATUS_GROUP_ALREADY_EXISTS or STATUS_SERVER_ERROR
 */
StatusCode GroupMng_CreateGroup(GroupMng *group_mng, const char *group_name, int client_id,
                                char *out_mc_ip, uint16_t *out_mc_port);

/**
 * @brief Add a client to an existing group
 *
 * @param group_mng  : the manager
 * @param group_name : name of the group to join
 * @param client_id  : client id of the joining client
 * @param out_mc_ip  : buffer to receive the group's multicast IP string
 * @param out_mc_port: receives the group's multicast port
 * @return STATUS_SUCCESS or STATUS_GROUP_NOT_FOUND
 */
StatusCode GroupMng_Join(GroupMng *group_mng, const char *group_name, int client_id,
                         char *out_mc_ip, uint16_t *out_mc_port);

/**
 * @brief Remove a client from a group — destroys the group if it becomes empty
 *
 * @param group_mng  : the manager
 * @param group_name : name of the group to leave
 * @param client_id  : client id of the leaving client
 * @return STATUS_SUCCESS or STATUS_GROUP_NOT_FOUND
 */
StatusCode GroupMng_Leave(GroupMng *group_mng, const char *group_name, int client_id);

/**
 * @brief Remove a client from all groups they are in — called on disconnect/logout
 *
 * @param group_mng : the manager
 * @param client_id : client id to remove from all groups
 */
void GroupMng_RemoveClientFromAll(GroupMng *group_mng, int client_id);

/**
 * @brief Print all active groups and their members to stdout
 *
 * @param group_mng : the manager
 */
void GroupMng_Dump(GroupMng *group_mng);

#endif /* GROUP_MNG_H */
