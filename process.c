#include "pm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern pm_configuration config;

void lock_process_list ()
{
        pthread_mutex_lock (&config.process_list_lock);
}

void unlock_process_list ()
{
        pthread_mutex_unlock (&config.process_list_lock);
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

void add_process (pid_t pid,
                  char *program,
                  char **argv,
                  char *stdout_file,
                  int max_retries)
{
        pm_process *p = malloc_nofail (sizeof (pm_process));
        p->next = NULL;

        p->pid = pid;
        p->max_retries = max_retries;

        p->program_name = malloc_nofail (strlen (program) + 1);
        strcpy (p->program_name, program);

        if (stdout_file) {
                p->stdout_file = malloc_nofail (strlen (stdout_file) + 1);
                strcpy (p->stdout_file, stdout_file);
        } else {
                p->stdout_file = NULL;
        }

        int argc = 0;

        while (argv[argc] != NULL)
                argc++;

        p->argv = malloc_nofail ((argc + 1) * sizeof (char *));

        for (int i = 0; i < argc; i++) {
                p->argv[i] = malloc_nofail (strlen (argv[i]) + 1);
                strcpy (p->argv[i], argv[i]);
        }

        p->argv[argc] = NULL;

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
