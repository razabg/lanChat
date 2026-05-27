#include <stdio.h>
#include <stdlib.h>
#include "server_net.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    if (ServerNet_Create(port, NULL, NULL) < 0)
        return 1;

    printf("Server listening on port %d\n", port);
    ServerNet_Run();
    ServerNet_Destroy();

    return 0;
}
