#ifndef USER_MNG_H
#define USER_MNG_H

#include "../common/protocol.h"

#define MAX_NAME_LEN 64

typedef struct {
    char username[MAX_NAME_LEN];
    char password[MAX_NAME_LEN];
    int  is_active;
    int  client_id;   /* -1 when not logged in */
} User;

//************ UserMng API *************

/**
 * @brief Initialize the user hash tables
 *
 * @return 0 on success, -1 on failure
 */
int UserMng_Create(void);

/**
 * @brief Free all user records and destroy the hash tables
 */
void UserMng_Destroy(void);

/**
 * @brief Register a new user
 *
 * @param username : the desired username
 * @param password : the desired password
 * @return STATUS_SUCCESS or STATUS_USERNAME_ALREADY_EXISTS
 */
StatusCode UserMng_Register(const char *username, const char *password);

/**
 * @brief Validate credentials and mark user as active
 *
 * @param username  : the username to log in
 * @param password  : the password to validate
 * @param client_id : the TCP client id to associate with this session
 * @return STATUS_SUCCESS, STATUS_USERNAME_NOT_FOUND, STATUS_WRONG_PASSWORD, or STATUS_ALREADY_LOGGED_IN
 */
StatusCode UserMng_Login(const char *username, const char *password, int client_id);

/**
 * @brief Mark user as inactive and clear their client_id
 *
 * @param client_id : the client id of the user to log out
 */
void UserMng_LogoutByClientId(int client_id);

/**
 * @brief Find a user record by client_id
 *
 * @param client_id : the client id to search for
 * @return pointer to User record, or NULL if not found
 */
User *UserMng_GetByClientId(int client_id);

#endif /* USER_MNG_H */
