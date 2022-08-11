#include "pm.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
extern pm_configuration config;

void daemon_process (char *socket_file)
{
        log_info (DAEMON, "pm daemon is starting...");
        int sock_fd = setup_unix_domain_server_socket (socket_file);

        log_info (DAEMON, "pm daemon spawning child monitor thread...");
        pthread_t dead_child_monitor_thread =
                spawn_daemon_child_monitor_thread ();

        log_info (DAEMON, "pm daemon initialized successfully!");
        log_info (DAEMON, "now listening for requests...");

        // daemon is entirely command based, it sits and waits for a command to
        // be written to the socket before doing anything.
        pm_cmd cmd;
        for (;;) {
                int conn_fd = accept (sock_fd, NULL, NULL);

                read_nofail (conn_fd, &cmd, sizeof (pm_cmd));

                switch (cmd.instruction) {
                case NEW_PROCESS: {
                        // setup process command line arguments
                        char *command = malloc_nofail (cmd.new_process.size);

                        read_nofail (conn_fd, command, cmd.new_process.size);

                        // count the number of arguments including program name
                        int args = 0;
                        for (size_t i = 0; i < cmd.new_process.size; i++)
                                if (!command[i])
                                        args++;

                        char **argv =
                                malloc_nofail ((args + 1) * sizeof (char *));

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
                        new_process (argv[0],
                                     &(argv[1]),
                                     config.stdout_file,
                                     config.max_retries);

                        free (argv);
                        free (command);

                        send_response (conn_fd, OK);

                        break;
                }
                case SIGNAL_PROCESS: {
                        lock_process_list ();

                        pm_process *process =
                                find_process_with_pid (cmd.signal_process.pid);

                        if (!process) {
                        }

                        if (kill (process->pid, cmd.signal_process.signal) <
                            0) {
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
                case ENABLE_AUTORESTART: {
                        break;
                }
                case DISABLE_AUTORESTART: {
                        break;
                }
                case SHUTDOWN: {
                        log_info (
                                DAEMON,
                                "User issued SHUTDOWN command. Shutting down pm daemon...");
                        int child_count = 0;

                        // close connection and socket. this lets client know
                        // that we are shutting down.

                        // disable the SIGCHLD handler
                        signal (SIGCHLD, SIG_IGN);

                        log_info (DAEMON, "Stopping monitor thread...");
                        stop_child_monitor_thread (dead_child_monitor_thread);

                        for (pm_process *proc = config.process_list;
                             proc != NULL;
                             proc = proc->next) {
                                log_info (
                                        DAEMON,
                                        "Sending SIGINT to child with pid %d...\n",
                                        proc->pid);

                                // send SIGINT to child, this usually will do
                                // the trick
                                kill (proc->pid, SIGINT);

                                // sleep for 1 second, wait for child to die.
                                sleep (1);

                                int status = 0;

                                // child should be dead by now
                                if (waitpid (proc->pid, &status, WNOHANG) ==
                                    -1) {
                                        perror ("waitpid");
                                        exit (EXIT_FAILURE);
                                }

                                if (!WIFSIGNALED (status) &&
                                    !WIFEXITED (status)) {
                                        log_info (
                                                DAEMON,
                                                "Child (pid: %d) did not exit within 1 second of SIGINT. Sending SIGKILL...\n",
                                                proc->pid);

                                        // if child doesn't die, forcibly kill
                                        // it.
                                        kill (proc->pid, SIGKILL);

                                        if (waitpid (proc->pid, NULL, 0) ==
                                            -1) {
                                                perror ("waitpid");
                                                exit (EXIT_FAILURE);
                                        }

                                } else {
                                        log_info (
                                                DAEMON,
                                                "Child with pid %d was terminated.\n",
                                                proc->pid);
                                }

                                child_count++;
                        }

                        log_info (DAEMON, "Closing connections...\n");

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
                log_error (
                        MAIN,
                        "no socket file specified. use --sockfile=... to specify socket file name\n");
                exit (EXIT_FAILURE);
        }

        pid_t pid = fork ();
        if (pid == 0) {
                daemon_process (config.socket_file);
                unlink (config.socket_file);
                log_info (DAEMON,"pm daemon shutdown successful!\n");
                exit (EXIT_SUCCESS);
        } else if (pid > 0) {
                return;
        } else {
                log_info (MAIN,"Unable to spawn daemon process");
                exit (EXIT_FAILURE);
        }
}
