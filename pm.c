/**
 * Process Manager (pm)
 *
 * David Yue <davidyue5819@gmail.com>
 */

#include "pm.h"
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

pm_configuration config = { .socket_file = NULL,
                            .stdout_file = NULL,
                            .shutdown = false,
                            .process_list = NULL,
                            .process_list_end = NULL};

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

        if (bind (sock_fd,
                  (struct sockaddr *)&addr,
                  sizeof (struct sockaddr_un)) < 0) {
                perror ("bind");
                log_error (
                        DAEMON,
                        "Make sure the socket file you specify does not already exist. Use --sockfile=...");
                exit (EXIT_FAILURE);
        }

        if (listen (sock_fd, 1) < 0) {
                perror ("listen");
                exit (EXIT_FAILURE);
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

        if (connect (sock_fd,
                     (struct sockaddr *)&addr,
                     sizeof (struct sockaddr_un)) != 0) {
                perror ("connect");
                exit (EXIT_FAILURE);
        }

        return sock_fd;
}

pid_t new_process (char *program,
                   char **argv,
                   char *stdout_file,
                   int max_retries)
{
        pid_t pid = fork ();

        if (pid == 0) {
                // redirect stdout if user specified another location.
                if (stdout_file) {
                        int fd = get_write_file_fd (stdout_file);
                        dup2 (fd, STDOUT_FILENO);
                        close (fd);
                }

                execvp (program, argv);

                perror ("execvp");
                fprintf(stderr, "program: %s", program);
                exit (EXIT_FAILURE);
        } else if (pid > 0) {
                add_process (pid, program, argv, stdout_file, max_retries);
                return pid;
        } else {
                perror ("fork");
                exit (EXIT_FAILURE);
        }
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
                int sock_fd =
                        setup_unix_domain_client_socket (config.socket_file);

                pm_cmd cmd;
                cmd.instruction = SHUTDOWN;

                write (sock_fd, &cmd, sizeof (pm_cmd));

                close (sock_fd);
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
                
                char *curr = *remaining_argv;
                char *write_head = &(cmd->new_process.command[0]);
                while (curr != NULL) {
                        while (*curr != '\0') {
                                *write_head = *curr;
                                curr++;
                                write_head++;
                        }
                        *write_head = *curr;
                        write_head++;

                        remaining_argv++;
                        curr = *remaining_argv;
                }

                send (sock_fd, cmd, sizeof (pm_cmd) + buffer_size, MSG_NOSIGNAL);

                close (sock_fd);
        }
}

void print_usage_statement ()
{
        printf ("usage: pm target subcommand [--sockfile=]\n"
                "target:\n"
                "  daemon\n"
                "  client\n"
                "subcommand:\n"
                "  daemon\n"
                "    start - starts the pm daemon\n"
                "    shutdown - shutdown the pm daemon\n");
}

void parse_cmd_args (int argc, char **argv)
{
        struct option long_options[] = {
                {.name = "sockfile",
                 .has_arg = required_argument,
                 .flag = NULL,
                 .val = 's'}
        };
        int option_index = 0, c;
        while ((c = getopt_long (
                        argc, argv, "s:", long_options, &option_index)) != -1) {
                switch (c) {
                case 's': config.socket_file = optarg; break;
                default: break;
                }
        }

        int i = 0;

        while (optind < argc) {
                switch (i) {
                case 0:
                        if (strcmp (argv[optind], "daemon") == 0)
                                i++;
                        else if (strcmp (argv[optind], "client") == 0)
                                i = 2;
                        else
                                i = -1;

                        break;
                case 1: process_daemon_command (argv[optind]); return;
                case 2: {
                        process_client_command (argv[optind],
                                                &argv[optind + 1]);
                        return;
                }
                default: print_usage_statement (); return;
                }

                optind++;
        }

        print_usage_statement ();
}

int main (int argc, char **argv)
{
        parse_cmd_args (argc, argv);
}
