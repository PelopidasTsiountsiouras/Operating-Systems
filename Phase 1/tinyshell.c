/*
 * TinyShell - απλό Unix-like κέλυφος (C)
 * -------------------------------------
 * Περιγραφή
 *  - Εμφανίζει ένα prompt
 *  - Διαβάζει μία γραμμή εντολής
 *  - Αναλύει τα ορίσματα (χωρισμός με whitespace)
 *  - Αντιλαμβάνεται το builtin `exit`
 *  - Αναζητά το εκτελέσιμο στο $PATH (αν δεν είναι απόλυτο/σχετικό μονοπάτι)
 *  - Εκτελεί την εντολή με fork + execve
 *  - Περιμένει την ολοκλήρωση (waitpid) και αναφέρει τον κωδικό εξόδου ή το σήμα τερματισμού
 *  - Τερματίζει ομαλά σε EOF (Ctrl+D) ή στην εντολή `exit`
 *
 * BUILD
 *  gcc -std=c11 -Wall -Wextra -O2 -o tinyshell tinyshell.c
 *
 * RUN
 *  ./tinyshell
 *
 * Παραδείγματα χρήσης
 *  > ls -l /tmp
 *  > /bin/echo hello world
 *  > exit
 *  (ή Ctrl+D)
 *
 * Σημειώσεις υλοποίησης
 *  - Χρησιμοποιεί POSIX κλήσεις: fork, execve, waitpid, access, getenv
 *  - Δεν υλοποιεί pipes, redirections ή background jobs (θα μπορούσε να προστεθεί σε επόμενη φάση)
 *  - Η παράμετρος PATH διασπώνται στο ':' και δοκιμάζεται access(X_OK) για εκτελέσιμο
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_LINE 4096
#define MAX_ARGS 256

extern char **environ; // για να προχωρήσουμε execve με το τρέχον env

/* trim() - αφαιρεί αρχικά/τελικά whitespace (in-place) */
static char *trim(char *s) {
    if (!s) return s;
    // trim leading
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (*s == '\0') return s;
    // trim trailing
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

/* split_line() - απλοί χωρισμοί ορισμάτων με strtok_r
 * Επιστρέφει τον αριθμό ορισμάτων και γεμίζει το argv (τελειώνει με NULL)
 * Δεν υποστηρίζει περίπλοκο quoting ή escape sequences σε αυτή τη φάση.
 */
static int split_line(char *line, char **argv, int max_args) {
    int argc = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(line, " \t\n", &saveptr);
    while (tok && argc < max_args - 1) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    argv[argc] = NULL;
    return argc;
}

/* find_executable_in_path() - αν command περιέχει '/' θεωρούμε ότι είναι path (absolute/relative)
 * αλλιώς ψάχνουμε στο PATH. Επιστρέφει malloc'd string με το πλήρες μονοπάτι αν βρεθεί,
 * αλλιώς NULL. Caller πρέπει να free() το αποτέλεσμα.
 */
static char *find_executable_in_path(const char *command) {
    if (!command) return NULL;
    if (strchr(command, '/')) {
        // Περιλαμβάνει slash -> treat as path
        if (access(command, X_OK) == 0) {
            return strdup(command);
        } else {
            return NULL;
        }
    }

    const char *path_env = getenv("PATH");
    if (!path_env) path_env = "/bin:/usr/bin"; // fallback

    char *path_dup = strdup(path_env);
    if (!path_dup) return NULL;

    char *saveptr = NULL;
    char *dir = strtok_r(path_dup, ":", &saveptr);
    while (dir) {
        size_t len = strlen(dir) + strlen(command) + 2; // '/' + \0
        char *candidate = malloc(len);
        if (!candidate) break;
        snprintf(candidate, len, "%s/%s", dir, command);
        if (access(candidate, X_OK) == 0) {
            free(path_dup);
            return candidate; // caller frees
        }
        free(candidate);
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_dup);
    return NULL;
}

int main(void) {
    char linebuf[MAX_LINE];
    char *argv[MAX_ARGS];

    while (1) {
        // Εμφάνιση prompt
        // Χρησιμοποιούμε fflush(stdout) για να βεβαιωθούμε ότι το prompt εμφανίζεται πριν το fgets
        printf("tinyshell$ ");
        fflush(stdout);

        if (!fgets(linebuf, sizeof(linebuf), stdin)) {
            // EOF ή σφάλμα - τερματισμός
            if (feof(stdin)) {
                printf("\nExiting (EOF)\n");
                break;
            } else {
                perror("fgets");
                break;
            }
        }

        char *line = trim(linebuf);
        if (*line == '\0') {
            // άδεια γραμμή, συνέχισε
            continue;
        }

        // Αναλύουμε ορίσματα
        int argc = split_line(line, argv, MAX_ARGS);
        if (argc == 0) continue;

        // builtin: exit
        if (strcmp(argv[0], "exit") == 0) {
            int exit_code = 0;
            if (argc >= 2) {
                exit_code = atoi(argv[1]);
            }
            printf("Exiting (exit %d)\n", exit_code);
            return exit_code;
        }

        // Εύρεση εκτελέσιμου
        char *program_path = find_executable_in_path(argv[0]);
        if (!program_path) {
            fprintf(stderr, "tinyshell: command not found: %s\n", argv[0]);
            continue;
        }

        // Προετοιμασία argv για execve: argv[] ήδη σωστά δείχνει σε tokens
        // Αλλά αν το αρχικό argv[0] ήταν σύντομη μορφή, πρέπει να αλλάξουμε την πρώτη παράμετρο
        // σε program_path ή όχι; Συνήθως αφήνουμε argv[0] όπως το πληκτρολόγησε ο χρήστης.

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            free(program_path);
            continue;
        }

        if (pid == 0) {
            // Child process
            // execve απαιτεί argv που ξεκινά με το όνομα της εντολής όπως θέλουμε
            // Θα δημιουργήσουμε ένα νέο argv_child που δείχνει στα ίδια ορίσματα
            // (μπορεί να είναι αρκετό να δώσουμε argv όπως είναι)

            // execve απαιτεί (char *const argv[]) - η argv που έχουμε είναι char *argv[] και τελειώνει με NULL
            execve(program_path, argv, environ);
            // Αν execve επιστρέψει, σημαίνει σφάλμα
            fprintf(stderr, "tinyshell: failed to exec %s: %s\n", program_path, strerror(errno));
            _exit(127);
        } else {
            // Parent: περίμενε το παιδί
            int status;
            pid_t w = waitpid(pid, &status, 0);
            if (w == -1) {
                perror("waitpid");
            } else {
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    printf("[pid %d] exited with code %d\n", (int)pid, code);
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    printf("[pid %d] killed by signal %d (%s)\n", (int)pid, sig, strsignal(sig));
                } else {
                    printf("[pid %d] ended with status 0x%x\n", (int)pid, status);
                }
            }
        }

        free(program_path);
    }

    return 0;
}
