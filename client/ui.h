#ifndef UI_H
#define UI_H

#define USERNAME_SIZE  50
#define PASSWORD_SIZE  20
#define GROUP_NAME_SIZE 50

typedef struct
{
    char username[USERNAME_SIZE];
    char password[PASSWORD_SIZE];
} Credentials;

/* Menu options */
typedef enum
{
    MENU_INVALID  = 0,   /* bad / non-numeric input */
    MENU_REGISTER = 1,
    MENU_LOGIN,
    MENU_EXIT
} MenuOption;

typedef enum
{
    GROUP_MENU_INVALID = 0,  /* bad / non-numeric input */
    GROUP_MENU_CREATE  = 1,
    GROUP_MENU_JOIN,
    GROUP_MENU_LEAVE,
    GROUP_MENU_LOGOUT
} GroupMenuOption;

/* Display a message to the user (success or error) */
void showMessage(const char *msg);

/* Screen 1 */
void showMenu(void);
MenuOption getMenuChoice(void);
void getCredentials(Credentials *cred);

/* Screen 2 */
void showGroupMenu(void);
GroupMenuOption getGroupMenuChoice(void);
void getGroupName(char *buf, int size);

#endif