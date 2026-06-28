#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ds/hashMap.h>
#include <ds/genQueue.h>
#include "group_mng.h"

#define HASH_CAPACITY    64
#define MC_PORT_BASE     5000
#define MC_IP_FIRST      1
#define MC_IP_LAST       254
#define MC_IP_PREFIX     "239.0.0."

/* ─── Manager struct (opaque outside this file) ─────────────────────────── */
struct GroupMng {
    HashMap *group_hash;          /* group_name → Group* */
    Queue   *free_mc_ip_queue;    /* pool of available multicast IP strings */
};

/* ─── RemoveContext — used by RemoveClientFromAll two-pass approach ─────── */
typedef struct {
    int   client_id;
    List *to_destroy;
} RemoveContext;

/* ─── Forward declarations ─────────────────────────────────────────────── */
static size_t      hash_string(void *key);
static int         equal_string(void *a, void *b);
static GroupMngResult init_mc_ip_pool(Queue *queue);
static void        destroy_group(Group *group);
static int         free_group_cb(const void *key, void *value, void *context);
static int         remove_client_from_group(Group *group, RemoveContext *ctx);
static int         remove_client_from_group_cb(const void *key, void *value, void *context);
static void        init_group(Group *group, const char *group_name, const char *mc_ip);
static void        add_member_to_group(Group *group, int client_id);
static void        fill_group_output(const Group *group, char *out_mc_ip, uint16_t *out_mc_port);

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

GroupMng *GroupMng_Create(void)
{
    GroupMng *mng = malloc(sizeof(GroupMng));
    if (!mng)
    {
        return NULL;
    }

    mng->group_hash = HashMap_Create(HASH_CAPACITY, hash_string, equal_string);
    if (!mng->group_hash)
    {
        free(mng);
        return NULL;
    }

    mng->free_mc_ip_queue = QueueCreate(MC_IP_LAST - MC_IP_FIRST + 1);
    if (!mng->free_mc_ip_queue)
    {
        HashMap_Destroy(&mng->group_hash, NULL, NULL);
        free(mng);
        return NULL;
    }

    if (init_mc_ip_pool(mng->free_mc_ip_queue) != GROUP_MNG_OK)
    {
        QueueDestroy(&mng->free_mc_ip_queue, free);
        HashMap_Destroy(&mng->group_hash, NULL, NULL);
        free(mng);
        return NULL;
    }

    return mng;
}

void GroupMng_Destroy(GroupMng **group_mng)
{
    if (!group_mng || !*group_mng)
    {
        return;
    }

    HashMap_ForEach((*group_mng)->group_hash, free_group_cb, NULL);
    HashMap_Destroy(&(*group_mng)->group_hash, NULL, NULL);
    QueueDestroy(&(*group_mng)->free_mc_ip_queue, free);
    free(*group_mng);
    *group_mng = NULL;
}

static int dump_group_cb(const void *key, void *value, void *context)
{
    (void)key; (void)context;
    Group *group = (Group *)value;
    printf("  %-20s  members=%-3d  IP=%-15s  port=%u\n",
           group->group_name,
           group->member_count,
           group->mc_ip,
           group->mc_port);
    return 1;
}

void GroupMng_Dump(GroupMng *group_mng)
{
    printf("=== GROUPS (%zu active) ===\n", HashMap_Size(group_mng->group_hash));
    HashMap_ForEach(group_mng->group_hash, dump_group_cb, NULL);
}

StatusCode GroupMng_CreateGroup(GroupMng *group_mng, const char *group_name, int client_id,
                                char *out_mc_ip, uint16_t *out_mc_port)
{
    void *existing = NULL;
    HashMap_Find(group_mng->group_hash, group_name, &existing);
    if (existing)
    {
        return STATUS_GROUP_ALREADY_EXISTS;
    }

    char *mc_ip = NULL;
    if (QueueRemove(group_mng->free_mc_ip_queue, (void **)&mc_ip) != QUEUE_SUCCESS)
    {
        return STATUS_SERVER_ERROR;
    }

    Group *group = calloc(1, sizeof(Group));
    init_group(group, group_name, mc_ip);
    free(mc_ip);

    add_member_to_group(group, client_id);
    HashMap_Insert(group_mng->group_hash, group->group_name, group);
    fill_group_output(group, out_mc_ip, out_mc_port);

    return STATUS_SUCCESS;
}

StatusCode GroupMng_Join(GroupMng *group_mng, const char *group_name, int client_id,
                         char *out_mc_ip, uint16_t *out_mc_port)
{
    void *val = NULL;
    if (HashMap_Find(group_mng->group_hash, group_name, &val) != MAP_SUCCESS)
    {
        return STATUS_GROUP_NOT_FOUND;
    }

    Group *group = (Group *)val;
    group->member_count++;

    int *id = malloc(sizeof(int));
    *id = client_id;
    ListPushHead(group->members, id);

    strncpy(out_mc_ip, group->mc_ip, MAX_IP_LEN - 1);
    *out_mc_port = group->mc_port;

    return STATUS_SUCCESS;
}

StatusCode GroupMng_Leave(GroupMng *group_mng, const char *group_name, int client_id)
{
    void *val = NULL;
    if (HashMap_Find(group_mng->group_hash, group_name, &val) != MAP_SUCCESS)
    {
        return STATUS_GROUP_NOT_FOUND;
    }

    Group *group = (Group *)val;

    /* Remove client from member list and decrement count */
    ListItr itr = ListItrBegin(group->members);
    ListItr end = ListItrEnd(group->members);
    while (itr != end)
    {
        int *id = (int *)ListItrGet(itr);
        if (*id == client_id)
        {
            free(id);
            ListItrRemove(itr);
            group->member_count--;
            break;
        }
        itr = ListItrNext(itr);
    }

    /* If empty — destroy the group and return IP to pool */
    if (group->member_count <= 0)
    {
        char *ip_copy = strdup(group->mc_ip);
        void *key, *removed;
        HashMap_Remove(group_mng->group_hash, group_name, &key, &removed);
        destroy_group(group);
        QueueInsert(group_mng->free_mc_ip_queue, ip_copy);
    }

    return STATUS_SUCCESS;
}

void GroupMng_RemoveClientFromAll(GroupMng *group_mng, int client_id)
{
    RemoveContext ctx;
    ctx.client_id  = client_id;
    ctx.to_destroy = ListCreate();

    /* Pass 1: remove client from all groups, collect empty ones */
    HashMap_ForEach(group_mng->group_hash, remove_client_from_group_cb, &ctx);

    /* Pass 2: safely destroy empty groups now that ForEach is done */
    ListItr itr = ListItrBegin(ctx.to_destroy);
    ListItr end = ListItrEnd(ctx.to_destroy);
    while (itr != end)
    {
        Group *group = (Group *)ListItrGet(itr);
        char *ip_copy = strdup(group->mc_ip);
        void *key, *removed;
        HashMap_Remove(group_mng->group_hash, group->group_name, &key, &removed);
        destroy_group(group);
        QueueInsert(group_mng->free_mc_ip_queue, ip_copy);
        itr = ListItrNext(itr);
    }

    ListDestroy(&ctx.to_destroy, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
   STATIC HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

static size_t hash_string(void *key)
{
    size_t hash = 5381;
    unsigned char *str = (unsigned char *)key;
    while (*str)
    {
        hash = ((hash << 5) + hash) + *str++;
    }
    return hash;
}

static int equal_string(void *a, void *b)
{
    return strcmp((char *)a, (char *)b) == 0;
}

/* Initializes a pre-allocated Group record with name, MC IP, port, and member list. */
static void init_group(Group *group, const char *group_name, const char *mc_ip)
{
    strncpy(group->group_name, group_name, MAX_NAME_LEN - 1);
    strncpy(group->mc_ip,      mc_ip,       MAX_IP_LEN - 1);
    /* All groups share the same port — isolation is enforced by the unique
     * multicast IP address (see mc_receiver's bind to the specific MC IP). */
    group->mc_port      = MC_PORT_BASE;
    group->member_count = 1;  /* creator is auto-joined */
    group->members      = ListCreate();
}

/* Appends client_id to a group's member list. */
static void add_member_to_group(Group *group, int client_id)
{
    int *id = malloc(sizeof(int));
    *id = client_id;
    ListPushHead(group->members, id);
}

/* Copies the group's multicast address info into the caller's output buffers. */
static void fill_group_output(const Group *group, char *out_mc_ip, uint16_t *out_mc_port)
{
    strncpy(out_mc_ip, group->mc_ip, MAX_IP_LEN - 1);
    *out_mc_port = group->mc_port;
}

/* Fills the queue with multicast IPs from 239.0.0.1 to 239.0.0.254. */
static GroupMngResult init_mc_ip_pool(Queue *queue)
{
    int i;
    for (i = MC_IP_FIRST; i <= MC_IP_LAST; i++)
    {
        char *ip = malloc(MAX_IP_LEN);
        if (!ip)
        {
            return GROUP_MNG_ALLOC_ERROR;
        }
        snprintf(ip, MAX_IP_LEN, "%s%d", MC_IP_PREFIX, i);
        QueueInsert(queue, ip);
    }
    return GROUP_MNG_OK;
}

/* Frees a Group record including its member list. */
static void destroy_group(Group *group)
{
    ListDestroy(&group->members, free);
    free(group);
}

/* Called by: GroupMng_Destroy via HashMap_ForEach
   Frees each Group record when the server shuts down. */
static int free_group_cb(const void *key, void *value, void *context)
{
    (void)key; (void)context;
    destroy_group((Group *)value);
    return 1;
}

/* Called by: remove_client_from_group_cb
   Removes client_id from a group's member list and decrements member_count.
   If the group becomes empty, adds it to ctx->to_destroy instead of modifying
   the hash during iteration — modifying the hash mid-ForEach causes a crash. */
static int remove_client_from_group(Group *group, RemoveContext *ctx)
{
    ListItr itr = ListItrBegin(group->members);
    ListItr end = ListItrEnd(group->members);

    while (itr != end)
    {
        int *id = (int *)ListItrGet(itr);
        if (*id == ctx->client_id)
        {
            free(id);
            ListItrRemove(itr);
            group->member_count--;

            if (group->member_count <= 0)
            {
                ListPushHead(ctx->to_destroy, group);
            }

            return 1;
        }
        itr = ListItrNext(itr);
    }
    return 1;
}

/* Called by: GroupMng_RemoveClientFromAll via HashMap_ForEach
   Visits every group and removes the disconnecting client from each one. */
static int remove_client_from_group_cb(const void *key, void *value, void *context)
{
    (void)key;
    remove_client_from_group((Group *)value, (RemoveContext *)context);
    return 1;
}
