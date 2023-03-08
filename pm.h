#ifndef pm_h
#define pm_h

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef enum pm_instruction {
        NEW_PROCESS,
        SIGNAL_PROCESS,
        LIST_PROCESS,
        SET_AUTORESTART_TRIES,
        SET_STDOUT,
        SET_STDERR,
        SHUTDOWN
} pm_instruction;

typedef enum pm_code {
        OK,
        NO_SUCH_PID,
        NO_SUCH_FILE_OR_DIRECTORY,
} pm_code;

typedef enum pm_identity { MAIN, DAEMON, MONITOR } pm_identity;

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

                struct {
                        int max_retries;
                } autorestart;
        };

} pm_cmd;

typedef struct __attribute__ ((packed)) pm_response {
        pm_code code;

} pm_response;

typedef struct pm_process pm_process;

typedef struct pm_process {
        pm_process *next;
        char *program_name;
        char *stdout_file;
        char **argv;
        int max_retries;
        time_t start_time;
        pid_t pid;
} pm_process;

typedef struct pm_configuration {
        char *socket_file;
        char *stdout_file;
        sem_t *dead_child;
        pm_process *process_list;
        pm_process *process_list_end;
        pthread_mutex_t process_list_lock;
        int max_retries;
        bool shutdown;
} pm_configuration;

void *malloc_nofail (size_t size);
void read_nofail (int fd, void *buf, size_t size);
int get_write_file_fd (char *filename);
void lock_process_list ();

void unlock_process_list ();
int setup_unix_domain_server_socket (char *socket_file);
int setup_unix_domain_client_socket (char *socket_file);
pid_t new_process (char *program,
                   char **argv,
                   char *stdout_file,
                   int max_retries);
void set_stdout (char *stdout_file);
void handle_child_signal (int signal);
void send_response (int conn_fd, pm_code err);
void free_process_list_entry (pm_process *process);
pm_process *find_process_with_pid (pid_t pid);
bool remove_process_from_list (pm_process *process);
void daemon_child_monitor_thread (void *arg);
pthread_t spawn_daemon_child_monitor_thread ();
pthread_t stop_child_monitor_thread (pthread_t thread);
void daemon_process (char *socket_file);
void spawn_daemon_process ();
void process_daemon_command (char *command);

void add_process_to_list (pid_t pid,
                  char *program,
                  char **argv,
                  char *stdout_file,
                  int max_retries);

char *get_identity_name (pm_identity id);
void log_info (char *message, ...);
void log_warn (char *message, ...);
void log_error (char *message, ...);
void print_usage_statement ();
void fatal_error ();
#endif