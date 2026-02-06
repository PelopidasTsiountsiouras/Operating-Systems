/*
 * TinyShell - Απλό Unix-like κέλυφος (Phase 2)
 * --------------------------------------------
 * Χαρακτηριστικά
 *  - Βασικός βρόχος εντολών (prompt, ανάγνωση, parsing, fork/execve/waitpid)
 *  - Builtin εντολή: exit [code]
 *  - Αναζήτηση εκτελέσιμων στο $PATH (αν δεν υπάρχει '/' στο όνομα)
 *  - Υποστήριξη ανακατεύθυνσης εξόδου:
 *        cmd > file      (δημιουργία/overwrite)
 *        cmd >> file     (append)
 *  - Υποστήριξη αγωγών (pipes):
 *        cmd1 | cmd2
 *        cmd1 | cmd2 | cmd3 | ...
 *  - Συνδυασμοί pipes + ανακατεύθυνση εξόδου στο ΤΕΛΕΥΤΑΙΟ στάδιο:
 *        cmd1 | cmd2 >> out.txt
 *
 * Υλοποίηση
 *  - Κάθε γραμμή εντολής σπάει σε tokens με βάση το whitespace.
 *  - Γίνεται parsing σε "στάδια" pipeline (Command) χωρισμένα με '|'.
 *  - Επιτρέπεται ένα μόνο redirection εξόδου (>, >>), το οποίο εφαρμόζεται
 *    στο τελευταίο στάδιο του pipeline.
 *  - Για N στάδια δημιουργούνται N-1 pipes με pipe().
 *  - Για κάθε στάδιο γίνεται fork(), ρύθμιση stdin/stdout με dup2()
 *    (pipe ends + τυχόν ανακατεύθυνση) και execve().
 *  - Ο γονέας κλείνει όλα τα pipe descriptors και περιμένει όλα τα παιδιά
 *    με waitpid(), τυπώνοντας τους κωδικούς εξόδου όπως στο Phase 1.
 *
 * BUILD
 *  - Με το υπάρχον Makefile:
 *        make
 *  - Ή χειροκίνητα:
 *        gcc -std=c11 -Wall -Wextra -O2 -o tinyshell tinyshell.c
 *
 * ΕΚΤΕΛΕΣΗ
 *        ./tinyshell
 *
 * ΠΑΡΑΔΕΙΓΜΑΤΑ
 *   tinyshell$ ls -l > ls.txt
 *   tinyshell$ ls -l >> ls.txt
 *   tinyshell$ ls -l | grep ".c"
 *   tinyshell$ ls -l | grep ".c" | wc -l
 *   tinyshell$ cat file.txt | grep mystring >> grepresults.txt
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_LINE 4096
#define MAX_ARGS 256
#define MAX_CMDS 32   /* μέγιστος αριθμός stages σε pipeline */

extern char **environ; /* για να περάσουμε το τρέχον env στο execve */

/* trim() - αφαιρεί αρχικά/τελικά whitespace (in-place) */
static char *trim(char *s) {
    if (!s) return s;
    /* trim leading */
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
        s++;
    if (*s == '\0')
        return s;
    /* trim trailing */
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

/* split_line() - σπάει μια γραμμή σε tokens (χωρισμός σε whitespace)
 * Επιστρέφει τον αριθμό των tokens (argc) και γεμίζει τον πίνακα argv.
 * Το τελευταίο στοιχείο argv[argc] είναι πάντα NULL (για χρήση σε execve).
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
 * αλλιώς NULL. Caller πρέπει να free() το αποτέλεσμα (μόνο στον parent, ποτέ στο child μετά το exec).
 */
static char *find_executable_in_path(const char *command) {
    if (!command || *command == '\0')
        return NULL;

    /* Αν περιέχει '/', θεωρούμε ότι είναι absolute/relative path και απλά ελέγχουμε X_OK */
    if (strchr(command, '/')) {
        if (access(command, X_OK) == 0) {
            char *res = strdup(command);
            return res;
        } else {
            return NULL;
        }
    }

    const char *path_env = getenv("PATH");
    if (!path_env)
        path_env = "/bin:/usr/bin"; /* fallback */

    char *path_dup = strdup(path_env);
    if (!path_dup)
        return NULL;

    char *saveptr = NULL;
    char *dir = strtok_r(path_dup, ":", &saveptr);
    while (dir) {
        size_t len = strlen(dir) + strlen(command) + 2; /* '/' + '\0' */
        char *candidate = malloc(len);
        if (!candidate)
            break;
        snprintf(candidate, len, "%s/%s", dir, command);
        if (access(candidate, X_OK) == 0) {
            free(path_dup);
            return candidate; /* caller frees */
        }
        free(candidate);
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_dup);
    return NULL;
}

/* Δομή που αναπαριστά ένα στάδιο pipeline: μια εντολή με τα argv της. */
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
} Command;

/* Δομή που αναπαριστά ολόκληρο pipeline + τυχόν redirection εξόδου. */
typedef struct {
    Command cmds[MAX_CMDS];
    int n_cmds;
    char *outfile; /* NULL αν δεν υπάρχει redirection */
    int append;    /* 0 για '>', 1 για '>>' */
} Pipeline;

/* parse_pipeline()
 *  - Παίρνει τα tokens (argv[0..argc-1]) και γεμίζει μια δομή Pipeline.
 *  - Χωρίζει τα stages με βάση το '|'.
 *  - Εντοπίζει '>' / '>>' και αποθηκεύει το output filename + mode.
 *  - Επιστρέφει 0 σε επιτυχία, -1 σε συντακτικό σφάλμα.
 */
static int parse_pipeline(char **tokens, int argc, Pipeline *pl) {
    memset(pl, 0, sizeof(*pl));
    if (argc == 0)
        return -1;

    int cmd_idx = 0;
    pl->n_cmds = 1;
    Command *cur = &pl->cmds[0];

    for (int i = 0; i < argc; i++) {
        char *tok = tokens[i];

        if (strcmp(tok, "|") == 0) {
            /* Νέο στάδιο pipeline */
            if (cur->argc == 0) {
                fprintf(stderr, "tinyshell: syntax error near '|'\n");
                return -1;
            }
            if (pl->n_cmds >= MAX_CMDS) {
                fprintf(stderr, "tinyshell: too many pipeline stages (max %d)\n", MAX_CMDS);
                return -1;
            }
            /* Τερματίζουμε το προηγούμενο argv με NULL */
            cur->argv[cur->argc] = NULL;

            cmd_idx++;
            pl->n_cmds++;
            cur = &pl->cmds[cmd_idx];
        } else if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            /* Ανακατεύθυνση εξόδου – επιτρέπουμε μόνο μία */
            if (i + 1 >= argc) {
                fprintf(stderr, "tinyshell: redirection without filename\n");
                return -1;
            }
            if (pl->outfile != NULL) {
                fprintf(stderr, "tinyshell: multiple output redirections\n");
                return -1;
            }
            pl->outfile = tokens[i + 1];
            pl->append = (tok[1] == '>') ? 1 : 0;
            i++; /* προσπερνάμε το filename */
        } else {
            if (cur->argc >= MAX_ARGS - 1) {
                fprintf(stderr, "tinyshell: too many arguments\n");
                return -1;
            }
            cur->argv[cur->argc++] = tok;
        }
    }

    if (cur->argc == 0) {
        fprintf(stderr, "tinyshell: empty command in pipeline\n");
        return -1;
    }

    /* Τερματίζουμε όλα τα argv με NULL */
    for (int c = 0; c < pl->n_cmds; c++) {
        pl->cmds[c].argv[pl->cmds[c].argc] = NULL;
    }

    return 0;
}

/* execute_pipeline()
 *  - Εκτελεί ένα Pipeline με χρήση pipe(), dup2(), fork(), execve().
 *  - Υποστηρίζει:
 *      * μονή εντολή χωρίς pipes
 *      * μονή εντολή με ανακατεύθυνση
 *      * πολλαπλά στάδια pipeline (με ή χωρίς redirection στο τέλος)
 */
static void execute_pipeline(const Pipeline *pl) {
    int n = pl->n_cmds;
    int pipes[MAX_CMDS - 1][2];

    /* Δημιουργία pipes (αν χρειάζονται) */
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            /* Αν αποτύχει η δημιουργία pipe, δεν κάνουμε τίποτα άλλο. */
            return;
        }
    }

    /* Fork για κάθε στάδιο */
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            /* Σφάλμα fork – σταματάμε εδώ και απλά θα περιμένουμε όσα παιδιά
             * έχουν ήδη δημιουργηθεί.
             */
            break;
        }

        if (pid == 0) {
            /* === ΠΑΙΔΙ === */

            /* Αν δεν είμαστε το πρώτο στάδιο, συνδέουμε το stdin με read-end του προηγούμενου pipe */
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                    perror("dup2 stdin");
                    _exit(1);
                }
            }

            /* Αν δεν είμαστε το τελευταίο στάδιο, συνδέουμε το stdout με write-end του επόμενου pipe */
            if (i < n - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                    perror("dup2 stdout");
                    _exit(1);
                }
            }

            /* Αν είμαστε το τελευταίο στάδιο και υπάρχει redirection εξόδου, το εφαρμόζουμε */
            if (i == n - 1 && pl->outfile != NULL) {
                int flags = O_WRONLY | O_CREAT;
                if (pl->append)
                    flags |= O_APPEND;
                else
                    flags |= O_TRUNC;

                int fd = open(pl->outfile, flags, 0666);
                if (fd < 0) {
                    perror(pl->outfile);
                    _exit(1);
                }
                if (dup2(fd, STDOUT_FILENO) < 0) {
                    perror("dup2 redirect");
                    close(fd);
                    _exit(1);
                }
                close(fd);
            }

            /* Κλείνουμε όλα τα pipes που δεν χρειαζόμαστε πλέον */
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            /* Εύρεση εκτελέσιμου για την τρέχουσα εντολή */
            char *prog_path = find_executable_in_path(pl->cmds[i].argv[0]);
            if (!prog_path) {
                fprintf(stderr, "tinyshell: command not found: %s\n",
                        pl->cmds[i].argv[0]);
                _exit(127);
            }

            execve(prog_path, pl->cmds[i].argv, environ);
            perror("execve");
            _exit(127);
        } else {
            /* === ΓΟΝΕΑΣ === */
            (void)pid; /* δεν χρειαζόμαστε αποθήκευση pid εδώ */
        }
    }

    /* Γονέας: κλείνει όλα τα pipe ends */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Αναμονή για όλα τα παιδιά και εμφάνιση exit status (όπως στο phase 1) */
    int status;
    pid_t wpid;
    while ((wpid = wait(&status)) > 0) {
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            printf("[pid %d] exited with code %d\n", (int)wpid, code);
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("[pid %d] killed by signal %d (%s)\n",
                   (int)wpid, sig, strsignal(sig));
        } else {
            printf("[pid %d] ended with status 0x%x\n", (int)wpid, status);
        }
    }
}

int main(void) {
    char linebuf[MAX_LINE];
    char *tokens[MAX_ARGS];

    for (;;) {
        /* Εμφάνιση prompt */
        printf("tinyshell$ ");
        fflush(stdout);

        if (!fgets(linebuf, sizeof(linebuf), stdin)) {
            /* EOF ή σφάλμα - τερματισμός */
            if (feof(stdin)) {
                printf("\nExiting (EOF)\n");
                break;
            } else {
                perror("fgets");
                break;
            }
        }

        char *line = trim(linebuf);
        if (*line == '\0')
            continue; /* κενή γραμμή */

        /* Parsing σε tokens με βάση το whitespace */
        int argc = split_line(line, tokens, MAX_ARGS);
        if (argc == 0)
            continue;

        /* Builtin: exit [code] */
        if (strcmp(tokens[0], "exit") == 0) {
            int exit_code = 0;
            if (argc >= 2) {
                exit_code = atoi(tokens[1]);
            }
            printf("Exiting (exit %d)\n", exit_code);
            return exit_code;
        }

        /* Parsing σε pipeline + redirections */
        Pipeline pl;
        if (parse_pipeline(tokens, argc, &pl) < 0) {
            /* συντακτικό σφάλμα - μην εκτελέσεις τίποτα */
            continue;
        }

        execute_pipeline(&pl);
    }

    return 0;
}
