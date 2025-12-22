#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAXLINE 1024
#define MAXARGS 128
#define MAXJOBS 16

/* =======================
 * Job control structures
 * ======================= */

typedef enum { UNDEF = 0, FG, BG, ST } job_state_t;

typedef struct job {
    pid_t pid;
    pid_t pgid;
    int jid;
    job_state_t state;
    char cmdline[MAXLINE];
} job_t;

static job_t jobs[MAXJOBS];
static int nextjid = 1;
static pid_t shell_pgid;

/* =======================
 * Job list helpers
 * ======================= */

static void clearjob(job_t *job) {
    job->pid = 0;
    job->pgid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

static void initjobs(void) {
    for (int i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
    nextjid = 1;
}

static int addjob(pid_t pid, pid_t pgid, job_state_t state, const char *cmdline) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].pgid = pgid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            snprintf(jobs[i].cmdline, MAXLINE, "%s", cmdline);
            return 1;
        }
    }
    fprintf(stderr, "tsh: too many jobs\n");
    return 0;
}

static void deletejob(pid_t pid) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return;
        }
    }
}

static job_t *getjob_jid(int jid) {
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

static job_t *getjob_pid(pid_t pid) {
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

static pid_t fgpid(void) {
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* =======================
 * Parsing
 * ======================= */

static int split_line(char *line, char **argv, int *bg) {
    int argc = 0;
    *bg = 0;

    char *token = strtok(line, " \t\r\n");
    while (token && argc < MAXARGS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;

    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        *bg = 1;
        argv[--argc] = NULL;
    }
    return argc;
}

/* =======================
 * Signal handlers
 * ======================= */

static void sigchld_handler(int sig) {
    (void)sig;
    int olderrno = errno;
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status,
                          WNOHANG | WUNTRACED | WCONTINUED)) > 0) {

        job_t *job = getjob_pid(pid);
        if (!job)
            continue;

        if (WIFSTOPPED(status)) {
            job->state = ST;
            printf("\n[%d]+ Stopped    %s", job->jid, job->cmdline);
        }
        else if (WIFCONTINUED(status)) {
            /* state already set by fg/bg */
        }
        else if (WIFSIGNALED(status)) {
            printf("\nJob [%d] (%d) terminated by signal %d\n",
                   job->jid, pid, WTERMSIG(status));
            deletejob(pid);
        }
        else if (WIFEXITED(status)) {
            deletejob(pid);
        }
    }
    errno = olderrno;
}

static void set_handler(int sig, void (*handler)(int)) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

/* =======================
 * Foreground wait
 * ======================= */

static void waitfg(pid_t pid) {
    sigset_t empty;
    sigemptyset(&empty);
    while (fgpid() == pid)
        sigsuspend(&empty);
}

/* =======================
 * Builtins: fg / bg / exit
 * ======================= */

static void do_bgfg(char **argv, bool fg) {
    if (!argv[1] || argv[1][0] != '%') {
        fprintf(stderr, "%s command requires %%jobid\n", fg ? "fg" : "bg");
        return;
    }

    int jid = atoi(argv[1] + 1);
    job_t *job = getjob_jid(jid);
    if (!job) {
        fprintf(stderr, "%s: No such job\n", argv[1]);
        return;
    }

    kill(-job->pgid, SIGCONT);

    if (fg) {
        job->state = FG;
        tcsetpgrp(STDIN_FILENO, job->pgid);
        waitfg(job->pid);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    } else {
        job->state = BG;
        printf("[%d]+ %s&\n", job->jid, job->cmdline);
    }
}

static int builtin_cmd(char **argv) {
    if (!argv[0])
        return 1;

    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "quit"))
        exit(0);

    if (!strcmp(argv[0], "fg")) {
        do_bgfg(argv, true);
        return 1;
    }

    if (!strcmp(argv[0], "bg")) {
        do_bgfg(argv, false);
        return 1;
    }

    return 0;
}

/* =======================
 * eval
 * ======================= */

static void eval(char *cmdline) {
    char *argv[MAXARGS];
    int bg;
    char buf[MAXLINE];

    snprintf(buf, MAXLINE, "%s", cmdline);

    if (split_line(buf, argv, &bg) == 0)
        return;

    if (builtin_cmd(argv))
        return;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

        setpgid(0, 0);

        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        execvp(argv[0], argv);
        perror("execvp");
        _exit(1);
    }

    setpgid(pid, pid);
    addjob(pid, pid, bg ? BG : FG, cmdline);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    if (!bg) {
        tcsetpgrp(STDIN_FILENO, pid);
        waitfg(pid);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    } else {
        printf("[%d] %d\n", nextjid - 1, pid);
    }
}

/* =======================
 * main
 * ======================= */

int main(void) {
    char line[MAXLINE];

    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    set_handler(SIGCHLD, sigchld_handler);
    set_handler(SIGINT,  SIG_IGN);
    set_handler(SIGTSTP, SIG_IGN);
    set_handler(SIGTTIN, SIG_IGN);
    set_handler(SIGTTOU, SIG_IGN);

    initjobs();

    while (1) {
        printf("tsh> ");
        fflush(stdout);

        if (!fgets(line, MAXLINE, stdin)) {
            putchar('\n');
            exit(0);
        }
        eval(line);
    }
}
