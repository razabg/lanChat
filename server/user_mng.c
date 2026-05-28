#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ds/hashMap.h>
#include "user_mng.h"

#define HASH_CAPACITY 64

/* ─── Two hash maps ────────────────────────────────────────────────────────
   s_user_hash_by_name      : username (string) → User*   for register/login lookups
   s_user_hash_by_client_id : client_id string  → User*   for message/disconnect lookups
────────────────────────────────────────────────────────────────────────── */
static HashMap *s_user_hash_by_name;
static HashMap *s_user_hash_by_client_id;

/* ─── Hash and equality functions ──────────────────────────────────────── */

static size_t hash_string(void *key)
{
    size_t hash = 5381;
    unsigned char *str = (unsigned char *)key;
    while (*str)
        hash = ((hash << 5) + hash) + *str++;
    return hash;
}

static int equal_string(void *a, void *b)
{
    return strcmp((char *)a, (char *)b) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

int UserMng_Create(void)
{
    s_user_hash_by_name = HashMap_Create(HASH_CAPACITY, hash_string, equal_string);
    if (!s_user_hash_by_name) return -1;

    s_user_hash_by_client_id = HashMap_Create(HASH_CAPACITY, hash_string, equal_string);
    if (!s_user_hash_by_client_id) {
        HashMap_Destroy(&s_user_hash_by_name, NULL, NULL);
        return -1;
    }

    return 0;
}

/* Called by: UserMng_Destroy via HashMap_ForEach
   Frees each User record — only called on s_user_hash_by_name to avoid double-free,
   since both hash maps point to the same User records. */
static int free_user(const void *key, void *value, void *context)
{
    (void)key; (void)context;
    free(value);
    return 1;
}

void UserMng_Destroy(void)
{
    HashMap_ForEach(s_user_hash_by_name, free_user, NULL);
    HashMap_Destroy(&s_user_hash_by_name, NULL, NULL);
    HashMap_Destroy(&s_user_hash_by_client_id, NULL, NULL);
}

StatusCode UserMng_Register(const char *username, const char *password)
{
    void *existing = NULL;
    HashMap_Find(s_user_hash_by_name, username, &existing);
    if (existing)
        return STATUS_USERNAME_ALREADY_EXISTS;

    User *user = calloc(1, sizeof(User));
    strncpy(user->username, username, MAX_NAME_LEN - 1);
    strncpy(user->password, password, MAX_NAME_LEN - 1);
    user->is_active = 0;
    user->client_id = -1;

    HashMap_Insert(s_user_hash_by_name, user->username, user);
    return STATUS_SUCCESS;
}

StatusCode UserMng_Login(const char *username, const char *password, int client_id)
{
    void *val = NULL;
    if (HashMap_Find(s_user_hash_by_name, username, &val) != MAP_SUCCESS)
        return STATUS_USERNAME_NOT_FOUND;

    User *user = (User *)val;

    if (strcmp(user->password, password) != 0)
        return STATUS_WRONG_PASSWORD;

    if (user->is_active)
        return STATUS_ALREADY_LOGGED_IN;

    user->is_active = 1;
    user->client_id = client_id;

    /* build string key for client_id hash e.g. "3" */
    char id_key[16];
    snprintf(id_key, sizeof(id_key), "%d", client_id);
    HashMap_Insert(s_user_hash_by_client_id, id_key, user);

    return STATUS_SUCCESS;
}

void UserMng_LogoutByClientId(int client_id)
{
    User *user = UserMng_GetByClientId(client_id);
    if (!user) return;

    user->is_active = 0;
    user->client_id = -1;

    char id_key[16];
    snprintf(id_key, sizeof(id_key), "%d", client_id);
    void *key, *val;
    HashMap_Remove(s_user_hash_by_client_id, id_key, &key, &val);
}

/* Iterates s_user_hash_by_client_id to find the user with matching client_id. */
User *UserMng_GetByClientId(int client_id)
{
    char id_key[16];
    snprintf(id_key, sizeof(id_key), "%d", client_id);
    void *val = NULL;
    HashMap_Find(s_user_hash_by_client_id, id_key, &val);
    return (User *)val;
}
