#include "pm.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
extern pm_configuration config;
extern pm_identity process_identity;

void handle_error (int sig)
{
        printf ("Program recieved signal %d", sig);
        exit (sig);
}

bool is_dead_child (pid_t pid)
{
        int status;

        if (waitpid (pid, &status, WNOHANG) == -1) {
                log_error ("Failed to wait on child: %s", strerror (errno));
                return false;
        }

        return WIFEXITED (status) || WIFSIGNALED (status);
}

void daemon_process (char *socket_file)
{
        signal (SIGSEGV, handle_error);

        log_info ("pm daemon is starting...");
        int sock_fd = setup_unix_domain_server_socket (socket_file);

        log_info ("pm daemon spawning child monitor thread...");
        pthread_t dead_child_monitor_thread = spawn_daemon_child_monitor_thread ();

        log_info ("pm daemon initialized successfully!");
        log_info ("now listening for requests...");

        // daemon is entirely command based, it sits and waits for a command to
        // be written to the socket before doing anything.
        pm_cmd cmd;
        for (;;) {
                int conn_fd = accept (sock_fd, NULL, NULL);

                read_nofail (conn_fd, &cmd, sizeof (pm_cmd));

                switch (cmd.instruction) {
                case NEW_PROCESS: {
                        log_info ("Recieved NEW_PROCESS command...");
                        // setup process command line arguments
                        char *command = malloc_nofail (cmd.new_process.size);

                        read_nofail (conn_fd, command, cmd.new_process.size);

                        // count the number of arguments including program name
                        int args = 0;
                        for (size_t i = 0; i < cmd.new_process.size; i++)
                                if (!command[i])
                                        args++;

                        char **argv = malloc_nofail ((args + 1) * sizeof (char *));

                        int j = 0;
                        char *curr = command;
                        while (args > 0) {
                                argv[j] = curr;
                                while (*curr != '\0')
                                        curr++;

                                curr++;
                                args--;
                                j++;
                        }

                        argv[j] = NULL;

                        // spawn the new process
                        pid_t pid = new_process (argv[0], argv, config.stdout_file, config.max_retries);

                        for (int i = 0; i < cmd.new_process.size - 1; i++)
                                if (!command[i])
                                        command[i] = ' ';

                        log_info ("New process with pid %d was added: %s", pid, command);

                        free (argv);
                        free (command);
                        send_response (conn_fd, OK);
                        break;
                }
                case SIGNAL_PROCESS: {
                        lock_process_list ();
                        log_info ("Received SIGNAL command");

                        pm_process *process = find_process_with_pid (cmd.signal_process.pid);

                        if (!process) {
                                log_warn ("Could not find process with pid %d", cmd.signal_process.pid);
                                unlock_process_list ();
                                continue;
                        }

                        if (kill (process->pid, cmd.signal_process.signal) < 0) {
                                perror ("kill");
                                exit (EXIT_FAILURE);
                        }

                        unlock_process_list ();

                        break;
                }
                case LIST_PROCESS: {
                        lock_process_list ();

                        unlock_process_list ();

                        break;
                }
                case SET_AUTORESTART_TRIES: {
                        config.max_retries = cmd.autorestart.max_retries;
                }
                case SHUTDOWN: {
                        log_info ("User issued SHUTDOWN command. Shutting down pm daemon...");

                        log_info ("Stopping monitor thread...");
                        stop_child_monitor_thread (dead_child_monitor_thread);

                        for (pm_process *proc = config.process_list; proc != NULL; proc = proc->next) {
                                log_info ("Sending SIGTERM (15) to child with pid %d...", proc->pid);

                                kill (proc->pid, SIGTERM);
                        }

                        signal (SIGCHLD, SIG_IGN);

                        for (pm_process *proc = config.process_list; proc != NULL; proc = proc->next) {
                                if (!is_dead_child (proc->pid)) {
                                        log_info (
                                                "Child (pid: %d) did not exit within 1 second of SIGINT. Sending SIGKILL...",
                                                proc->pid);

                                        kill (proc->pid, SIGKILL);
                                }
                        }

                        log_info ("Closing connections...");

                        close (conn_fd);
                        close (sock_fd);

                        return;
                }
                default: break;
                }
        }
}

void spawn_daemon_process ()
{
        if (!config.socket_file) {
                log_error ("no socket file specified. use --sockfile=... to specify socket file name");
                exit (EXIT_FAILURE);
        }

        pid_t pid = fork ();
        if (pid == 0) {
                process_identity = DAEMON;
                daemon_process (config.socket_file);
                unlink (config.socket_file);
                log_info ("pm daemon shutdown successful!");
                exit (EXIT_SUCCESS);
        } else if (pid > 0) {
                return;
        } else {
                log_info ("Unable to spawn daemon process");
                exit (EXIT_FAILURE);
        }
}
