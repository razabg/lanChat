#ifndef USER_MNG_H
#define USER_MNG_H

#include "../common/protocol.h"

#define MAX_NAME_LEN 64

/* ─── Opaque manager type ───────────────────────────────────────────────── */
typedef struct UserMng UserMng;

/* ─── User record (public — used by ServerMng and GroupMng) ─────────────── */
typedef struct {
    char username[MAX_NAME_LEN];
    char password[MAX_NAME_LEN];
    int  is_active;
    int  client_id;   /* -1 when not logged in */
} User;

//************ UserMng API *************

/**
 * @brief Initialize the user manager and its hash tables
 *
 * @return UserMng* on success, NULL on failure
 */
UserMng *UserMng_Create(void);

/**
 * @brief Free all user records and destroy the manager
 *
 * @param user_mng : pointer to the manager pointer (set to NULL on destroy)
 */
void UserMng_Destroy(UserMng **user_mng);

/**
 * @brief Register a new user
 *
 * @param user_mng : the manager
 * @param username : the desired username
 * @param password : the desired password
 * @return STATUS_SUCCESS or STATUS_USERNAME_ALREADY_EXISTS
 */
StatusCode UserMng_Register(UserMng *user_mng, const char *username, const char *password);

/**
 * @brief Validate credentials and mark user as active
 *
 * @param user_mng  : the manager
 * @param username  : the username to log in
 * @param password  : the password to validate
 * @param client_id : the TCP client id to associate with this session
 * @return STATUS_SUCCESS, STATUS_USERNAME_NOT_FOUND, STATUS_WRONG_PASSWORD, or STATUS_ALREADY_LOGGED_IN
 */
StatusCode UserMng_Login(UserMng *user_mng, const char *username, const char *password, int client_id);

/**
 * @brief Mark user as inactive and clear their client_id
 *
 * @param user_mng  : the manager
 * @param client_id : the client id of the user to log out
 */
void UserMng_LogoutByClientId(UserMng *user_mng, int client_id);

/**
 * @brief Find a user record by client_id
 *
 * @param user_mng  : the manager
 * @param client_id : the client id to search for
 * @return pointer to User record, or NULL if not found
 */
User *UserMng_GetByClientId(UserMng *user_mng, int client_id);

/**
 * @brief Print all registered users and their current status to stdout
 *
 * @param user_mng : the manager
 */
void UserMng_Dump(UserMng *user_mng);

#endif /* USER_MNG_H */
