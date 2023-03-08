/**
 * Process Manager (pm)
 *
 * David Yue <davidyue5819@gmail.com>
 */

#include "pm.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DAEMON_SOCKET_FILENAME "daemon.sock"

extern pm_identity process_identity;

pm_configuration config = { .socket_file = NULL,
                            .stdout_file = NULL,
                            .shutdown = false,
                            .process_list = NULL,
                            .process_list_end = NULL };

void fatal_error ()
{
        if (config.socket_file)
                unlink (config.socket_file);

        exit (EXIT_FAILURE);
}

int get_write_file_fd (char *filename)
{
        int fd = open (filename, O_CREAT | O_WRONLY, 0666);

        if (fd < 0) {
                perror ("open");
                exit (EXIT_FAILURE);
        }

        return fd;
}

int setup_unix_domain_server_socket (char *socket_file)
{
        int sock_fd = socket (AF_UNIX, SOCK_STREAM, 0);

        if (sock_fd < 0) {
                perror ("socket");
                exit (EXIT_FAILURE);
        }

        struct sockaddr_un addr = { 0 };

        addr.sun_family = AF_UNIX;
        strncpy (addr.sun_path, socket_file, 104);

        if (bind (sock_fd, (struct sockaddr *)&addr, sizeof (struct sockaddr_un)) < 0) {
                log_error ("Error binding to socket: %s", strerror (errno));
                log_error ("Make sure the socket file you specify does not already exist. Use --sockfile=... to specify socket file name");

                fatal_error ();
        }

        if (listen (sock_fd, 1) < 0) {
                perror ("listen");
                fatal_error ();
        }

        config.socket_file = malloc_nofail (strlen (socket_file) + 1);
        strcpy (config.socket_file, socket_file);

        return sock_fd;
}

int setup_unix_domain_client_socket (char *socket_file)
{
        int sock_fd = socket (AF_UNIX, SOCK_STREAM, 0);

        struct sockaddr_un addr = { 0 };

        addr.sun_family = AF_UNIX;
        strncpy (addr.sun_path, socket_file, 104);

        if (connect (sock_fd, (struct sockaddr *)&addr, sizeof (struct sockaddr_un)) != 0) {
                perror ("connect");
                log_error ("Is the daemon running? Use pm daemon start --sockfile=... to start the daemon.");
                fatal_error ();
        }

        return sock_fd;
}

void set_stdout (char *stdout_file)
{
        if (config.stdout_file != NULL) {
                free (config.stdout_file);
        }

        config.stdout_file = malloc_nofail (strlen (stdout_file) + 1);
        strcpy (config.stdout_file, stdout_file);
}

void handle_child_signal (int signal)
{
        sem_post (config.dead_child);

        return;
}

void process_daemon_command (char *command)
{
        if (strcmp (command, "start") == 0) {
                spawn_daemon_process ();
                exit (EXIT_SUCCESS);
        } else if (strcmp (command, "shutdown") == 0) {
                int sock_fd = setup_unix_domain_client_socket (config.socket_file);

                pm_cmd cmd;
                cmd.instruction = SHUTDOWN;
                send (sock_fd, &cmd, sizeof (pm_cmd), MSG_NOSIGNAL);
                close (sock_fd);
        }
}

pm_code send_client_command (int sock_fd, pm_cmd *command)
{
        pm_code response;

        switch (command->instruction) {
        case NEW_PROCESS: send (sock_fd, command, sizeof (pm_cmd) + command->new_process.size, MSG_NOSIGNAL); break;

        default: break;
        }

        if (recv (sock_fd, &response, sizeof (pm_code), 0) != sizeof (pm_code)) {
                log_error ("");
        }
}

void join_string_list_with_null_term (char **list, char *joined)
{
        size_t total_buffer_size = 0;
        for (size_t i = 0; list[i] != NULL; i++) {
                total_buffer_size += strlen (list[i]) + 1;
        }

        char *write_head = joined;
        for (size_t i = 0; list[i] != NULL; i++) {
                strcpy (write_head, list[i]);
                write_head += strlen (list[i]);
                *write_head = '\0';
                write_head++;
        }
}

void process_client_command (char *command, char **remaining_argv)
{
        int sock_fd = setup_unix_domain_client_socket (config.socket_file);
        if (strcmp (command, "run") == 0) {
                size_t buffer_size = 0;

                for (int i = 0; remaining_argv[i] != NULL; i++) {
                        buffer_size += strlen (remaining_argv[i]) + 1;
                }

                pm_cmd *cmd = malloc_nofail (sizeof (pm_cmd) + buffer_size);

                cmd->instruction = NEW_PROCESS;
                cmd->new_process.size = buffer_size;

                join_string_list_with_null_term (remaining_argv, cmd->new_process.command);

                send_client_command (sock_fd, cmd);

        } else if (strcmp (command, "autorestart") == 0) {
                pm_cmd cmd = (pm_cmd) {
                        .instruction = SET_AUTORESTART_TRIES,
                        .autorestart = {
                                .max_retries = 3,
                        }
                };

                send_client_command (sock_fd, &cmd);

        } else {
                print_usage_statement ();
                exit (EXIT_FAILURE);
        }

        close (sock_fd);
}

void print_usage_statement ()
{
        printf ("usage: pm target subcommand --sockfile=sockfilename\n"
                "target:\n"
                "  daemon\n"
                "  client\n"
                "subcommand:\n"
                "  daemon\n"
                "    start - starts the pm daemon\n"
                "    shutdown - shutdown the pm daemon\n"
                "\n"
                "sockfilename: name of the UNIX socket file\n");
}

bool consume_argv (int argc, char **argv, int *opt_index, char *expected)
{
        if (*opt_index >= argc) {
                return false;
        }

        if (strcmp (argv[*opt_index], expected) == 0) {
                (*opt_index)++;
                return true;
        } else {
                return false;
        }
}

void parse_cmd_args (int argc, char **argv)
{
        struct option long_options[] = {
                {.name = "sockfile", .has_arg = required_argument, .flag = NULL, .val = 's'}
        };
        int option_index = 0, c;
        while ((c = getopt_long (argc, argv, "s:", long_options, &option_index)) != -1) {
                switch (c) {
                case 's': config.socket_file = optarg; break;
                default: break;
                }
        }

        if (!config.socket_file) {
                log_info ("No socket file name was specified, using default name: %s", DAEMON_SOCKET_FILENAME);
                config.socket_file = DAEMON_SOCKET_FILENAME;
        }

        if (consume_argv (argc, argv, &optind, "daemon")) {
                process_daemon_command (argv[optind]);
        } else if (consume_argv (argc, argv, &optind, "client")) {
                if (!argv[optind]) {
                        log_error ("client target requires subcommand");
                        print_usage_statement ();
                        return;
                }

                process_client_command (argv[optind], &argv[optind + 1]);
        } else {
                print_usage_statement ();
        }
}

int main (int argc, char **argv)
{
        process_identity = MAIN;
        parse_cmd_args (argc, argv);
}
