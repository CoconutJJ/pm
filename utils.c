#include "pm.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

void *malloc_nofail (size_t size)
{
        void *mem = malloc (size);

        if (!mem) {
                perror ("malloc");
                exit (EXIT_FAILURE);
        }

        return mem;
}

void read_nofail (int fd, void *buf, size_t size)
{
        if (read (fd, buf, size) != size) {
                perror ("read");
                exit (EXIT_FAILURE);
        }
}

void send_response (int conn_fd, pm_code err)
{
        pm_response response = { .code = err };

        if (send (conn_fd, &response, sizeof (pm_response), MSG_NOSIGNAL) != sizeof (pm_response)) {
                log_warn ("error occurred when sending response back to client: %s", strerror (errno));
        }
}