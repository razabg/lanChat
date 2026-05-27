#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ui.h"

#define PASS "\033[32mPASS\033[0m"
#define FAIL "\033[31mFAIL\033[0m"

static int failures = 0;

#define CHECK(cond, name) \
    do { \
        if (cond) { printf("[" PASS "] %s\n", name); } \
        else      { printf("[" FAIL "] %s\n", name); failures++; } \
    } while (0)

/* Redirect stdin from a temp string via a pipe trick */
static void set_stdin(const char *input)
{
    FILE *tmp = tmpfile();
    fputs(input, tmp);
    rewind(tmp);
    stdin = tmp;
}

/* ── showMenu ─────────────────────────────────────────────────────────── */
static void test_showMenu(void)
{
    printf("\n-- showMenu --\n");
    showMenu();   /* visual: must print without crash */
    CHECK(1, "showMenu runs without crash");
}

/* ── showGroupMenu ────────────────────────────────────────────────────── */
static void test_showGroupMenu(void)
{
    printf("\n-- showGroupMenu --\n");
    showGroupMenu();
    CHECK(1, "showGroupMenu runs without crash");
}

/* ── getMenuChoice ────────────────────────────────────────────────────── */
static void test_getMenuChoice(void)
{
    printf("\n-- getMenuChoice --\n");

    set_stdin("1\n");
    CHECK(getMenuChoice() == MENU_REGISTER, "choice 1 → MENU_REGISTER");

    set_stdin("2\n");
    CHECK(getMenuChoice() == MENU_LOGIN, "choice 2 → MENU_LOGIN");

    set_stdin("3\n");
    CHECK(getMenuChoice() == MENU_EXIT, "choice 3 → MENU_EXIT");

    set_stdin("abc\n");
    CHECK(getMenuChoice() == 0, "non-numeric input → 0");
}

/* ── getGroupMenuChoice ───────────────────────────────────────────────── */
static void test_getGroupMenuChoice(void)
{
    printf("\n-- getGroupMenuChoice --\n");

    set_stdin("1\n");
    CHECK(getGroupMenuChoice() == GROUP_MENU_CREATE, "choice 1 → GROUP_MENU_CREATE");

    set_stdin("2\n");
    CHECK(getGroupMenuChoice() == GROUP_MENU_JOIN, "choice 2 → GROUP_MENU_JOIN");

    set_stdin("3\n");
    CHECK(getGroupMenuChoice() == GROUP_MENU_LEAVE, "choice 3 → GROUP_MENU_LEAVE");

    set_stdin("4\n");
    CHECK(getGroupMenuChoice() == GROUP_MENU_LOGOUT, "choice 4 → GROUP_MENU_LOGOUT");

    set_stdin("xyz\n");
    CHECK(getGroupMenuChoice() == 0, "non-numeric input → 0");
}

/* ── getCredentials ───────────────────────────────────────────────────── */
static void test_getCredentials(void)
{
    printf("\n-- getCredentials --\n");

    Credentials cred;

    set_stdin("alice\nsecret123\n");
    getCredentials(&cred);
    CHECK(strcmp(cred.username, "alice") == 0,      "username parsed correctly");
    CHECK(strcmp(cred.password, "secret123") == 0,  "password parsed correctly");

    /* Newline must be stripped */
    CHECK(strchr(cred.username, '\n') == NULL, "username has no trailing newline");
    CHECK(strchr(cred.password, '\n') == NULL, "password has no trailing newline");

    /* NULL pointer must not crash */
    getCredentials(NULL);
    CHECK(1, "getCredentials(NULL) does not crash");
}

/* ── getGroupName ─────────────────────────────────────────────────────── */
static void test_getGroupName(void)
{
    printf("\n-- getGroupName --\n");

    char buf[GROUP_NAME_SIZE];

    set_stdin("mygroup\n");
    getGroupName(buf, GROUP_NAME_SIZE);
    CHECK(strcmp(buf, "mygroup") == 0,    "group name parsed correctly");
    CHECK(strchr(buf, '\n') == NULL,      "group name has no trailing newline");

    /* NULL pointer must not crash */
    getGroupName(NULL, GROUP_NAME_SIZE);
    CHECK(1, "getGroupName(NULL, ...) does not crash");

    /* Zero size must not crash */
    getGroupName(buf, 0);
    CHECK(1, "getGroupName(buf, 0) does not crash");
}

/* ────────────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("========== UI Tests ==========\n");

    test_showMenu();
    test_showGroupMenu();
    test_getMenuChoice();
    test_getGroupMenuChoice();
    test_getCredentials();
    test_getGroupName();

    printf("\n==============================\n");
    if (failures == 0)
        printf("All tests " PASS "\n");
    else
        printf("%d test(s) " FAIL "\n", failures);

    return failures == 0 ? 0 : 1;
}
