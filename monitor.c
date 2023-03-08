#include "pm.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern pm_configuration config;

void daemon_child_monitor_thread (void *arg)
{
        // block this thread from handling SIGCHLD
        sigset_t set;
        sigemptyset (&set);
        sigaddset (&set, SIGCHLD);
        pthread_sigmask (SIG_BLOCK, &set, NULL);
        while (1) {
                // we spend most of our time sleeping on the sem wait.
                sem_wait (config.dead_child);
                if (config.shutdown) {
                        log_info ("monitor thread has exit per request");
                        pthread_exit (NULL);
                }
                int status;
                pid_t pid;
                while ((pid = waitpid (-1, &status, WNOHANG)) > 0) {
                        // determine how child died
                        if (WIFEXITED (status)) {
                                log_info ("child with pid %d exited with status code %d", pid, WEXITSTATUS (status));
                        } else if (WIFSIGNALED (status)) {
                                log_info ("child with pid %d was killed by signal %d", pid, WTERMSIG (status));
                        }

                        lock_process_list ();

                        pm_process *child = find_process_with_pid (pid);

                        if (!child) {
                                log_warn ("erroneous SIGCHLD received. did not recognize child pid %d", pid);
                                unlock_process_list ();
                                continue;
                        }

                        // try to restart child if process was configured to
                        // auto restart
                        if (child->max_retries > 0) {
                                child->max_retries--;

                                log_info (
                                        "autorestart enabled (retries left: %d). attempting to restart child with old pid %d...",
                                        child->max_retries,
                                        pid);

                                new_process (child->program_name, child->argv, child->stdout_file, child->max_retries);
                        }

                        remove_process_from_list (child);
                        unlock_process_list ();
                }
        }
}

pthread_t spawn_daemon_child_monitor_thread ()
{
        config.dead_child = sem_open ("dead_child", O_CREAT, 0600, 0);
        pthread_mutex_init (&config.process_list_lock, NULL);

        pthread_t dead_child_thread;

        if (pthread_create (&dead_child_thread, NULL, &daemon_child_monitor_thread, NULL) != 0) {
                perror ("pthread_create");
                exit (EXIT_FAILURE);
        }

        signal (SIGCHLD, handle_child_signal);

        return dead_child_thread;
}

pthread_t stop_child_monitor_thread (pthread_t thread)
{
        config.shutdown = true;
        sem_post (config.dead_child);
        pthread_join (thread, NULL);
        config.shutdown = false;
}
