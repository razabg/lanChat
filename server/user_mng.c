#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ds/hashMap.h>
#include "user_mng.h"

#define HASH_CAPACITY 64

/* ─── Manager struct (opaque outside this file) ─────────────────────────── */
struct UserMng {
    HashMap *by_name;       /* username   → User* */
    HashMap *by_client_id;  /* "id" string → User* */
};

/* ─── Hash and equality functions ──────────────────────────────────────── */

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

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

UserMng *UserMng_Create(void)
{
    UserMng *mng = malloc(sizeof(UserMng));
    if (!mng)
    {
        return NULL;
    }

    mng->by_name = HashMap_Create(HASH_CAPACITY, hash_string, equal_string);
    if (!mng->by_name)
    {
        free(mng);
        return NULL;
    }

    mng->by_client_id = HashMap_Create(HASH_CAPACITY, hash_string, equal_string);
    if (!mng->by_client_id)
    {
        HashMap_Destroy(&mng->by_name, NULL, NULL);
        free(mng);
        return NULL;
    }

    return mng;
}

/* Called by: UserMng_Destroy via HashMap_ForEach
   Frees each User record — only called on by_name to avoid double-free,
   since both hash maps point to the same User records. */
static int free_user(const void *key, void *value, void *context)
{
    (void)key; (void)context;
    free(value);
    return 1;
}

void UserMng_Destroy(UserMng **user_mng)
{
    if (!user_mng || !*user_mng)
    {
        return;
    }

    HashMap_ForEach((*user_mng)->by_name, free_user, NULL);
    HashMap_Destroy(&(*user_mng)->by_name, NULL, NULL);
    HashMap_Destroy(&(*user_mng)->by_client_id, NULL, NULL);
    free(*user_mng);
    *user_mng = NULL;
}

StatusCode UserMng_Register(UserMng *user_mng, const char *username, const char *password)
{
    void *existing = NULL;
    HashMap_Find(user_mng->by_name, username, &existing);
    if (existing)
    {
        return STATUS_USERNAME_ALREADY_EXISTS;
    }

    User *user = calloc(1, sizeof(User));
    strncpy(user->username, username, MAX_NAME_LEN - 1);
    strncpy(user->password, password, MAX_NAME_LEN - 1);
    user->is_active = 0;
    user->client_id = -1;

    HashMap_Insert(user_mng->by_name, user->username, user);
    return STATUS_SUCCESS;
}

StatusCode UserMng_Login(UserMng *user_mng, const char *username, const char *password, int client_id)
{
    void *val = NULL;
    if (HashMap_Find(user_mng->by_name, username, &val) != MAP_SUCCESS)
    {
        return STATUS_USERNAME_NOT_FOUND;
    }

    User *user = (User *)val;

    if (strcmp(user->password, password) != 0)
    {
        return STATUS_WRONG_PASSWORD;
    }

    if (user->is_active)
    {
        return STATUS_ALREADY_LOGGED_IN;
    }

    user->is_active = 1;
    user->client_id = client_id;

    /* heap-allocate the key so it persists after this function returns */
    char *id_key = malloc(16);
    snprintf(id_key, 16, "%d", client_id);
    HashMap_Insert(user_mng->by_client_id, id_key, user);

    return STATUS_SUCCESS;
}

void UserMng_LogoutByClientId(UserMng *user_mng, int client_id)
{
    User *user = UserMng_GetByClientId(user_mng, client_id);
    if (!user)
    {
        return;
    }

    user->is_active = 0;
    user->client_id = -1;

    char id_key[16];
    snprintf(id_key, sizeof(id_key), "%d", client_id);
    void *key, *val;
    HashMap_Remove(user_mng->by_client_id, id_key, &key, &val);
    free(key);  /* free the heap-allocated key from UserMng_Login */
}

User *UserMng_GetByClientId(UserMng *user_mng, int client_id)
{
    char id_key[16];
    snprintf(id_key, sizeof(id_key), "%d", client_id);
    void *val = NULL;
    HashMap_Find(user_mng->by_client_id, id_key, &val);
    return (User *)val;
}

static int dump_user_cb(const void *key, void *value, void *context)
{
    (void)key; (void)context;
    User *user = (User *)value;
    printf("  %-20s  %s  client_id=%d\n",
           user->username,
           user->is_active ? "ONLINE " : "offline",
           user->client_id);
    return 1;
}

void UserMng_Dump(UserMng *user_mng)
{
    printf("=== USERS (%zu registered) ===\n", HashMap_Size(user_mng->by_name));
    HashMap_ForEach(user_mng->by_name, dump_user_cb, NULL);
}
