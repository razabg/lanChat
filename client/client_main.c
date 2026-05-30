#include <stdio.h>
#include <stdlib.h>
#include "client_mng.h"

/*
 * Usage: ./chat_client <server_ip> <port>
 *
 * argv[1] — dotted-decimal IPv4 address of the server, e.g. "192.168.1.5"
 * argv[2] — TCP port the server is listening on,       e.g. "9000"
 *
 * Flow:
 *   1. Validate arguments.
 *   2. ClientMng_Create  — allocates the manager, opens the TCP connection.
 *   3. ClientMng_Run     — blocks inside the menu loop until the user exits.
 *   4. ClientMng_Destroy — closes the socket, frees all memory.
 */
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];

    /* atoi returns 0 on bad input — 0 is not a valid port, so we catch it */
    int port_arg = atoi(argv[2]);
    if (port_arg <= 0 || port_arg > 65535)
    {
        fprintf(stderr, "Error: invalid port '%s'. Must be 1-65535.\n", argv[2]);
        return EXIT_FAILURE;
    }

    ClientMng *mng = ClientMng_Create(server_ip, (uint16_t)port_arg);
    if (!mng)
    {
        fprintf(stderr, "Error: could not connect to %s:%d\n", server_ip, port_arg);
        return EXIT_FAILURE;
    }

    ClientMng_Run(mng);

    ClientMng_Destroy(mng);

    return EXIT_SUCCESS;
}