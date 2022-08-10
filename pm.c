/**
 * Process Manager (pm)
 *
 * David Yue <davidyue5819@gmail.com>
 */

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
typedef enum pm_instruction {
        NEW_PROCESS,
        SIGNAL_PROCESS,
        LIST_PROCESS,
        ENABLE_AUTORESTART,
        DISABLE_AUTORESTART,
        SET_STDOUT,
        SET_STDERR,
        SHUTDOWN
} pm_instruction;

typedef enum pm_code {

        OK,
        ERR

} pm_code;

typedef struct __attribute__ ((packed)) pm_cmd {
        pm_instruction instruction;

        union {
                struct {
                        size_t size;
                        char command[];
                } new_process;

                struct {
                        int signal;
                        pid_t pid;
                } signal_process;
        };

} pm_cmd;

typedef struct __attribute__ ((packed)) pm_response {
        pm_code code;

} pm_response;

typedef struct pm_process pm_process;

typedef struct pm_process {
        char *program_name;
        char **argv;
        char *stdout_file;
        pm_process *next;
        pid_t pid;
        time_t start_time;
        int max_retries;
} pm_process;

typedef struct pm_configuration {
        char *socket_file;
        char *stdout_file;
        int max_retries;
        pm_process *process_list;
        pm_process *process_list_end;
        pthread_mutex_t process_list_lock;
        bool shutdown;
        sem_t *dead_child;

} pm_configuration;

pm_configuration config = {
        .socket_file = NULL,
        .stdout_file = NULL,
        .shutdown = false
};

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

int get_write_file_fd (char *filename)
{
        int fd = open (filename, O_CREAT | O_WRONLY);

        if (fd < 0) {
                perror ("open");
                exit (EXIT_FAILURE);
        }

        return fd;
}

void lock_process_list ()
{
        pthread_mutex_lock (&config.process_list_lock);
}

void unlock_process_list ()
{
        pthread_mutex_unlock (&config.process_list_lock);
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
        addr.sun_len = 0;
        strncpy (addr.sun_path, socket_file, 104);

        if (bind (sock_fd,
                  (struct sockaddr *)&addr,
                  sizeof (struct sockaddr_un)) < 0) {
                perror ("bind");
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
        addr.sun_len = 0;
        strncpy (addr.sun_path, socket_file, 104);

        if (connect (sock_fd,
                     (struct sockaddr *)&addr,
                     sizeof (struct sockaddr_un)) != 0) {
                perror ("connect");
                exit (EXIT_FAILURE);
        }

        return sock_fd;
}

void add_process (pid_t pid,
                  char *program,
                  char **argv,
                  char *stdout_file,
                  int max_retries)
{
        pm_process *p = malloc_nofail (sizeof (pm_process));

        p->program_name = malloc_nofail (strlen (program) + 1);
        strcpy (p->program_name, program);

        if (stdout_file) {
                p->stdout_file = malloc_nofail (strlen (stdout_file) + 1);
                strcpy (p->stdout_file, stdout_file);
        }

        p->pid = pid;
        p->max_retries = max_retries;
        p->next = NULL;

        int argc = 0;

        while (argv[argc] != NULL)
                argc++;

        p->argv = malloc_nofail ((argc + 1) * sizeof (char *));

        for (int i = 0; i < argc; i++) {
                p->argv[i] = malloc (strlen (argv[i]) + 1);
                strcpy (p->argv[i], argv[i]);
        }

        argv[argc] = NULL;

        lock_process_list ();
        if (!config.process_list_end) {
                config.process_list = p;
                config.process_list_end = p;
        } else {
                config.process_list_end->next = p;
                config.process_list_end = p;
        }
        unlock_process_list ();
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

void send_ok_response (int conn_fd)
{
        pm_response response = { .code = OK };
        if (send (conn_fd, &response, sizeof (pm_response), MSG_NOSIGNAL) !=
            sizeof (pm_response)) {
                perror ("send");
                exit (EXIT_FAILURE);
        }
}

void destroy_process (pm_process *process)
{
        if (process->program_name)
                free (process->program_name);

        if (process->argv) {
                for (int i = 0; process->argv[i] != NULL; i++) {
                        free (process->argv[i]);
                }
                free (process->argv);
        }

        if (process->stdout_file)
                free (process->stdout_file);

        free (process);
}

pm_process *find_process_with_pid (pid_t pid)
{
        for (pm_process *curr = config.process_list; curr != NULL;
             curr = curr->next)
                if (curr->pid == pid) {
                        unlock_process_list ();
                        return curr;
                }
        return NULL;
}

bool remove_process_from_list (pm_process *process)
{
        pm_process *prev = NULL;

        for (pm_process *curr = config.process_list; curr != NULL;
             prev = curr, curr = curr->next) {
                if (curr != process)
                        continue;

                if (config.process_list == curr) {
                        config.process_list = curr->next;
                }

                if (config.process_list_end == curr) {
                        config.process_list_end = prev;
                }

                if (prev) {
                        prev->next = curr->next;
                }

                destroy_process (curr);

                return true;
        }
        return false;
}

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
                if (config.shutdown)
                        pthread_exit(NULL);

                int status;
                pid_t pid;
                while ((pid = waitpid (-1, &status, WNOHANG)) > 0) {
                        // determine how child died
                        if (WIFEXITED (status)) {
                                printf ("child with pid %d exited with status code %d\n",
                                        pid,
                                        WEXITSTATUS (status));
                        } else if (WIFSIGNALED (status)) {
                                printf ("child with pid %d was killed by signal %d\n",
                                        pid,
                                        WTERMSIG (status));
                        }

                        lock_process_list ();
                        // do not allow thread to be killed while list is being
                        // modified
                        pm_process *child = find_process_with_pid (pid);

                        if (!child) {
                                printf ("erroneous SIGCHLD received. did not recognize child pid %d\n",
                                        pid);
                                unlock_process_list ();
                                continue;
                        }

                        // try to restart child if process was configured to
                        // auto restart
                        if (child->max_retries > 0) {
                                child->max_retries--;

                                printf ("autorestart enabled (retries left: %d). attempting to restart child with old pid %d...\n",
                                        child->max_retries,
                                        pid);

                                new_process (child->program_name,
                                             child->argv,
                                             child->stdout_file,
                                             child->max_retries);
                        }

                        remove_process_from_list (child);
                        // unlock mutex before potential thread cancel request
                        unlock_process_list ();
                }
        }
}

pthread_t spawn_daemon_child_monitor_thread ()
{
        config.dead_child = sem_open ("dead_child", O_CREAT, 0600, 0);
        pthread_mutex_init (&config.process_list_lock, NULL);

        pthread_t dead_child_thread;

        if (pthread_create (&dead_child_thread,
                            NULL,
                            &daemon_child_monitor_thread,
                            NULL) != 0) {
                perror ("pthread_create");
                exit (EXIT_FAILURE);
        }

        signal (SIGCHLD, handle_child_signal);

        return dead_child_thread;
}

void daemon_process (char *socket_file)
{
        int sock_fd = setup_unix_domain_server_socket (socket_file);

        fprintf (stdout, "pm daemon spawning child monitor thread...\n");
        pthread_t dead_child_monitor_thread =
                spawn_daemon_child_monitor_thread ();

        fprintf (stdout, "pm daemon initialized successfully!\n");
        fprintf (stdout, "now listening for requests...\n");

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

                        send_ok_response (conn_fd);

                        break;
                }
                case SIGNAL_PROCESS: {
                        break;
                }
                case LIST_PROCESS: {
                        break;
                }
                case ENABLE_AUTORESTART: {
                        break;
                }
                case DISABLE_AUTORESTART: {
                        break;
                }
                case SHUTDOWN: {
                        printf ("user issued SHUTDOWN command: shutting down pm daemon...\n");
                        int child_count = 0;

                        // close connection and socket. this lets client know
                        // that we are shutting down.

                        // disable the SIGCHLD handler
                        signal (SIGCHLD, SIG_IGN);

                        printf ("stopping child monitor thread...\n");
                        config.shutdown = true;
                        sem_post(config.dead_child);
                        pthread_join (dead_child_monitor_thread, NULL);
                        printf ("child monitor thread is dead\n");

                        for (pm_process *proc = config.process_list;
                             proc != NULL;
                             proc = proc->next) {
                                printf ("Sending SIGINT to child with pid %d...\n",
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
                                        printf ("Child (pid: %d) did not exit within 1 second of SIGINT. Sending SIGKILL...\n",
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
                                        printf ("Child with pid %d was terminated.\n",
                                                proc->pid);
                                }

                                child_count++;
                        }

                        printf ("closing connections...\n");
                        fflush (stdout);

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
                fprintf (
                        stderr,
                        "error: no socket file specified. use --sockfile=... to specify socket file name\n");
                exit (EXIT_FAILURE);
        }

        pid_t pid = fork ();
        if (pid == 0) {
                daemon_process (config.socket_file);
                unlink (config.socket_file);
                printf ("pm daemon shutdown successful!\n");
                exit (EXIT_SUCCESS);
        } else if (pid > 0) {
                return;
        } else {
                perror ("unable to spawn daemon process");
                exit (EXIT_FAILURE);
        }
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

        if (strcmp (argv[optind++], "daemon") == 0) {
                process_daemon_command (argv[optind++]);
        }

        // pm daemon start
        // pm daemon shutdown
}

int main (int argc, char **argv)
{
        parse_cmd_args (argc, argv);
}
