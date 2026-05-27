#include <stdio.h>
#include <string.h>
#include "ui.h"

/* Clear input buffer */
static void clearInputBuffer(void)
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

/* Display main menu */
void showMenu(void)
{
    printf("\n===== MAIN MENU =====\n");
    printf("1. Registration\n");
    printf("2. Login\n");
    printf("3. Exit\n");
    printf("=====================\n");
    printf("Choose an option: ");
}

/* Get menu selection safely */
MenuOption getMenuChoice(void)
{
    int choice;

    if (scanf("%d", &choice) != 1)
    {
        clearInputBuffer();
        return 0;
    }

    clearInputBuffer();

    return (MenuOption)choice;
}

/* Ask for username + password */
void getCredentials(Credentials *cred)
{
    if (cred == NULL)
    {
        return;
    }

    printf("Enter username: ");
    fgets(cred->username, USERNAME_SIZE, stdin);
    cred->username[strcspn(cred->username, "\n")] = '\0';

    printf("Enter password: ");
    fgets(cred->password, PASSWORD_SIZE, stdin);
    cred->password[strcspn(cred->password, "\n")] = '\0';
}

/* Display group menu (Screen 2) */
void showGroupMenu(void)
{
    printf("\n===== GROUP MENU =====\n");
    printf("1. Create Group\n");
    printf("2. Join Group\n");
    printf("3. Leave Group\n");
    printf("4. Logout\n");
    printf("======================\n");
    printf("Choose an option: ");
}

/* Get group menu selection safely */
GroupMenuOption getGroupMenuChoice(void)
{
    int choice;

    if (scanf("%d", &choice) != 1)
    {
        clearInputBuffer();
        return 0;
    }

    clearInputBuffer();

    return (GroupMenuOption)choice;
}

/* Ask for a group name */
void getGroupName(char *buf, int size)
{
    if (buf == NULL || size <= 0)
    {
        return;
    }

    printf("Enter group name: ");
    fgets(buf, size, stdin);
    buf[strcspn(buf, "\n")] = '\0';
}