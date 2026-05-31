#include <stdio.h>
#include <stdlib.h>
#include "server_mng.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    if (ServerMng_Create(port) < 0)
        return 1;

    printf("Server listening on port %d\n", port);
    ServerMng_Run();
    ServerMng_Destroy();

    return 0;
}
