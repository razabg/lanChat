#include <stdio.h>
#include <stdlib.h>
#include "server_mng.h"
#include "logger.h"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    ServerMng *mng = ServerMng_Create(port);
    if (!mng)
    {
        return 1;
    }

    log_event(LOG_INFO, "Server", "listening on port %d", port);

    ServerMng_Run(mng);
    ServerMng_Destroy(&mng);

    return 0;
}
