/*
 * processgroup.c
 * Operating Systems I - Group Project (Custom Shell)
 *
 * processgroup: manage a BATCH of processes as one unit.
 *
 *   start      launch N copies of a command (or every command listed in a
 *              file) inside ONE new process group and print the group id
 *   status     list the members of a group with state, CPU time and memory
 *   monitor    refresh that table every few seconds (adds %CPU) until the
 *              group exits or the user presses Ctrl+C
 *   signal     send one signal to every member at once (killpg)
 *   terminate  SIGTERM the whole group, then SIGKILL whatever is left
 *
 * The process group id (PGID) is the handle for a batch, so no state file is
 * needed: every later command finds the members by scanning /proc.
 *
 * System calls and library functions used:
 *   fork, execvp, setpgid, getpgrp, getpgid   - create and place processes
 *   killpg                                    - collective signal sending
 *   pipe, fcntl (FD_CLOEXEC), read, write     - child reports exec failure
 *   sigaction                                 - graceful Ctrl+C handling
 *   opendir/readdir, fopen on /proc/<pid>/... - resource monitoring
 *   getopt                                    - argument parsing
 *   nanosleep, clock_gettime                  - timing
 *
 * Build (standalone):
 *   gcc -Wall -Wextra -O2 -o processgroup processgroup.c
 * Build (linked into the Custom Shell; the shell calls processgroup_main):
 *   gcc -Wall -Wextra -O2 -DPROCESSGROUP_NO_MAIN -c processgroup.c
 *
 * Exit status: 0 success, 1 runtime error, 2 usage error.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "processgroup.h"

#define PG_OK     0
#define PG_FAIL   1
#define PG_USAGE  2

#define MAX_JOBS        1000   /* most processes one batch may hold        */
#define MAX_JOB_ARGS    64     /* most arguments per command in a -f file  */
#define LINE_BUF        1024   /* longest line accepted in a -f file       */
#define CMD_SHOW_WIDTH  48     /* characters of the command line displayed */
#define DEFAULT_TERM_WAIT 5.0  /* seconds terminate waits before SIGKILL   */

/* ===================================================================== */
/*  Reading process information from /proc                                */
/* ===================================================================== */

struct proc_info {
    pid_t pid;
    pid_t ppid;
    char  state;                 /* R S D T Z ... from /proc/<pid>/stat    */
    unsigned long utime, stime;  /* CPU time in clock ticks                */
    long  threads;
    long  rss_kb;                /* resident memory in kilobytes           */
    char  cmd[CMD_SHOW_WIDTH + 1];
};

struct snapshot {
    struct proc_info *v;
    size_t n, cap;
};

static void snapshot_free(struct snapshot *s) {
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

static int snapshot_add(struct snapshot *s, const struct proc_info *p) {
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 16;
        struct proc_info *nv = realloc(s->v, ncap * sizeof *nv);
        if (nv == NULL) {
            return -1;
        }
        s->v = nv;
        s->cap = ncap;
    }
    s->v[s->n++] = *p;
    return 0;
}

/* Is this process still alive (not a zombie waiting to be reaped)? */
static int is_live(const struct proc_info *p) {
    return p->state != 'Z' && p->state != 'X';
}

static size_t live_count(const struct snapshot *s) {
    size_t c = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (is_live(&s->v[i])) {
            c++;
        }
    }
    return c;
}

/* Fill 'out' from /proc/<pid>/... Returns 1 if the process belongs to group
 * 'want_pgid', 0 if it does not (or vanished while we were reading it). */
static int read_proc(pid_t pid, pid_t want_pgid, struct proc_info *out) {
    static long page_kb = 0;
    if (page_kb == 0) {
        page_kb = sysconf(_SC_PAGESIZE) / 1024;
        if (page_kb < 1) {
            page_kb = 4;
        }
    }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;                       /* process exited: not an error */
    }
    char buf[1024];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';

    /* Format: "pid (comm) state ppid pgrp ...". comm may itself contain
     * spaces and parentheses, so find the LAST ')' to skip past it. */
    char *lp = strchr(buf, '(');
    char *rp = strrchr(buf, ')');
    if (lp == NULL || rp == NULL || rp < lp) {
        return 0;
    }

    char state = '?';
    int ppid = 0, pgrp = 0;
    unsigned long ut = 0, st = 0;
    long thr = 0, rss = 0;
    int got = sscanf(rp + 1,
        " %c %d %d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %ld %*d %*u %*u %ld",
        &state, &ppid, &pgrp, &ut, &st, &thr, &rss);
    if (got != 7 || pgrp != want_pgid) {
        return 0;
    }

    memset(out, 0, sizeof *out);
    out->pid = pid;
    out->ppid = ppid;
    out->state = state;
    out->utime = ut;
    out->stime = st;
    out->threads = thr;
    out->rss_kb = rss * page_kb;

    /* Display name: the full command line if readable, else [comm]. */
    char cmd[256] = "";
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    FILE *cf = fopen(path, "r");
    if (cf != NULL) {
        size_t n = fread(cmd, 1, sizeof cmd - 1, cf);
        fclose(cf);
        cmd[n] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (cmd[i] == '\0') {
                cmd[i] = ' ';
            }
        }
        while (n > 0 && cmd[n - 1] == ' ') {
            cmd[--n] = '\0';
        }
    }
    if (cmd[0] == '\0') {
        size_t clen = (size_t)(rp - lp - 1);
        if (clen > 60) {
            clen = 60;
        }
        snprintf(cmd, sizeof cmd, "[%.*s]", (int)clen, lp + 1);
    }
    /* Deliberately truncated to the display width. */
    strncpy(out->cmd, cmd, sizeof out->cmd - 1);
    out->cmd[sizeof out->cmd - 1] = '\0';
    return 1;
}

static int cmp_pid(const void *a, const void *b) {
    const struct proc_info *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

/* Collect every process whose group id is 'pgid', sorted by pid. */
static int take_snapshot(pid_t pgid, struct snapshot *s) {
    s->v = NULL;
    s->n = s->cap = 0;

    DIR *d = opendir("/proc");
    if (d == NULL) {
        perror("processgroup: opendir /proc");
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char *end;
        long v = strtol(e->d_name, &end, 10);
        if (*end != '\0' || v <= 0) {
            continue;                   /* not a pid directory */
        }
        struct proc_info p;
        if (read_proc((pid_t)v, pgid, &p)) {
            if (snapshot_add(s, &p) == -1) {
                fprintf(stderr, "processgroup: out of memory\n");
                closedir(d);
                snapshot_free(s);
                return -1;
            }
        }
    }
    closedir(d);
    if (s->n > 1) {
        qsort(s->v, s->n, sizeof *s->v, cmp_pid);
    }
    return 0;
}

static const char *state_name(char c) {
    switch (c) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "disk-wait";
    case 'T': return "stopped";
    case 't': return "traced";
    case 'Z': return "zombie";
    case 'X': return "dead";
    case 'I': return "idle";
    default:  return "other";
    }
}

/* Print the member table. 'pct' (one entry per row) adds a %CPU column;
 * pass NULL for the one-shot status view. */
static void print_table(const struct snapshot *s, const double *pct) {
    double tick = (double)sysconf(_SC_CLK_TCK);
    double total_cpu = 0.0, total_pct = 0.0;
    long total_rss = 0;

    if (pct != NULL) {
        printf("  %7s %7s  %-10s %6s %9s %9s %4s  %s\n",
               "PID", "PPID", "STATE", "%CPU", "CPU(s)", "RSS(KB)", "THR", "COMMAND");
    } else {
        printf("  %7s %7s  %-10s %9s %9s %4s  %s\n",
               "PID", "PPID", "STATE", "CPU(s)", "RSS(KB)", "THR", "COMMAND");
    }
    for (size_t i = 0; i < s->n; i++) {
        const struct proc_info *p = &s->v[i];
        double cpu = (double)(p->utime + p->stime) / tick;
        total_cpu += cpu;
        total_rss += p->rss_kb;
        if (pct != NULL) {
            total_pct += pct[i];
            printf("  %7d %7d  %-10s %6.1f %9.2f %9ld %4ld  %s\n",
                   (int)p->pid, (int)p->ppid, state_name(p->state), pct[i],
                   cpu, p->rss_kb, p->threads, p->cmd);
        } else {
            printf("  %7d %7d  %-10s %9.2f %9ld %4ld  %s\n",
                   (int)p->pid, (int)p->ppid, state_name(p->state),
                   cpu, p->rss_kb, p->threads, p->cmd);
        }
    }
    if (pct != NULL) {
        printf("  total: %zu process(es), %.1f %%CPU, %.2f s CPU time, %ld KB resident\n",
               s->n, total_pct, total_cpu, total_rss);
    } else {
        printf("  total: %zu process(es), %.2f s CPU time, %ld KB resident\n",
               s->n, total_cpu, total_rss);
    }
}

/* ===================================================================== */
/*  Small helpers: argument parsing, signals, safety checks               */
/* ===================================================================== */

static volatile sig_atomic_t g_interrupted = 0;

static void on_interrupt(int sig) {
    (void)sig;
    g_interrupted = 1;
}

struct saved_handlers {
    struct sigaction sigint, sigterm;
};

/* Ctrl+C / SIGTERM set a flag instead of killing us, so long-running
 * commands can stop cleanly. No SA_RESTART: sleeps return early. */
static void install_handlers(struct saved_handlers *sh) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_interrupt;
    sigemptyset(&sa.sa_mask);
    g_interrupted = 0;
    sigaction(SIGINT, &sa, &sh->sigint);
    sigaction(SIGTERM, &sa, &sh->sigterm);
}

static void restore_handlers(const struct saved_handlers *sh) {
    sigaction(SIGINT, &sh->sigint, NULL);
    sigaction(SIGTERM, &sh->sigterm, NULL);
}

/* Sleep for 'secs' seconds, returning early if interrupted. */
static void pg_sleep(double secs) {
    struct timespec ts;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        if (g_interrupted) {
            return;
        }
    }
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_long(const char *s, long min, long max, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_seconds(const char *s, double min, double max, double *out) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v) || v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_pgid(const char *s, pid_t *out) {
    long v;
    if (parse_long(s, 1, INT_MAX, &v) == -1) {
        fprintf(stderr, "processgroup: '%s' is not a valid process group id\n", s);
        return -1;
    }
    *out = (pid_t)v;
    return 0;
}

static const struct { const char *name; int num; } sig_table[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
    {"TERM", SIGTERM}, {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"TSTP", SIGTSTP},
    {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"ALRM", SIGALRM},
};

/* Accepts TERM, SIGTERM, term, or a number from 0 to 64. */
static int parse_signal(const char *s, int *out) {
    long num;
    if (parse_long(s, 0, 64, &num) == 0) {
        *out = (int)num;
        return 0;
    }
    if (strncasecmp(s, "SIG", 3) == 0) {
        s += 3;
    }
    for (size_t i = 0; i < sizeof sig_table / sizeof sig_table[0]; i++) {
        if (strcasecmp(s, sig_table[i].name) == 0) {
            *out = sig_table[i].num;
            return 0;
        }
    }
    return -1;
}

/* Refuse groups that would take down the system, the user's shell, or this
 * very program. Used before any signal is sent. */
static int check_signal_target(pid_t pgid) {
    if (pgid <= 1) {
        fprintf(stderr, "processgroup: refusing to signal group %d (system group)\n", (int)pgid);
        return -1;
    }
    pid_t parent_pgrp = getpgid(getppid());
    if (pgid == getpgrp() || pgid == parent_pgrp) {
        fprintf(stderr,
                "processgroup: refusing to signal group %d: it contains this program or the shell running it\n",
                (int)pgid);
        return -1;
    }
    return 0;
}

/* A bad option inside a subcommand. */
static int option_error(const char *cmd, int opt) {
    if (opt == ':') {
        fprintf(stderr, "processgroup %s: option -%c requires an argument\n", cmd, optopt);
    } else {
        fprintf(stderr, "processgroup %s: unknown option -%c\n", cmd, optopt);
    }
    return PG_USAGE;
}

/* ===================================================================== */
/*  start                                                                 */
/* ===================================================================== */

struct job {
    char **args;    /* NULL-terminated argument vector for execvp          */
    int owned;      /* 1 if args and its strings were malloc'd by us       */
};

static void free_jobs(struct job *jobs, int njobs) {
    for (int i = 0; i < njobs; i++) {
        if (jobs[i].owned && jobs[i].args != NULL) {
            for (char **a = jobs[i].args; *a != NULL; a++) {
                free(*a);
            }
            free(jobs[i].args);
        }
    }
    free(jobs);
}

/* Read a batch file: one command per line, blank lines and lines starting
 * with '#' ignored, arguments split on whitespace. */
static int load_job_file(const char *path, struct job *jobs, int *njobs) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "processgroup: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[LINE_BUF];
    int lineno = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        lineno++;
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(f)) {
            fprintf(stderr, "processgroup: %s:%d: line too long\n", path, lineno);
            fclose(f);
            return -1;
        }
        char *p = line;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (*njobs >= MAX_JOBS) {
            fprintf(stderr, "processgroup: %s: more than %d commands\n", path, MAX_JOBS);
            fclose(f);
            return -1;
        }
        struct job *j = &jobs[(*njobs)++];
        j->owned = 1;
        j->args = calloc(MAX_JOB_ARGS + 1, sizeof(char *));
        if (j->args == NULL) {
            fprintf(stderr, "processgroup: out of memory\n");
            fclose(f);
            return -1;
        }
        int n = 0;
        while (*p != '\0') {
            while (isspace((unsigned char)*p)) {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            char *start = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
            if (n >= MAX_JOB_ARGS) {
                fprintf(stderr, "processgroup: %s:%d: more than %d arguments\n", path, lineno, MAX_JOB_ARGS);
                fclose(f);
                return -1;
            }
            j->args[n] = strndup(start, (size_t)(p - start));
            if (j->args[n] == NULL) {
                fprintf(stderr, "processgroup: out of memory\n");
                fclose(f);
                return -1;
            }
            n++;
        }
    }
    fclose(f);
    return 0;
}

/* Can 'cmd' be executed? Checked before anything is forked so a typo does
 * not leave half a batch running. */
static int command_exists(const char *cmd) {
    struct stat st;
    if (strchr(cmd, '/') != NULL) {
        return stat(cmd, &st) == 0 && S_ISREG(st.st_mode) && access(cmd, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (path == NULL) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char *copy = strdup(path);
    if (copy == NULL) {
        return 1;                       /* cannot check; let exec decide */
    }
    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir != NULL && !found; dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, cmd);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
            found = 1;
        }
    }
    free(copy);
    return found;
}

struct exec_err {
    pid_t pid;
    int   err;
    int   stage;    /* 0 = could not join the group, 1 = exec failed */
};

static void start_usage(FILE *out) {
    fprintf(out,
        "Usage: processgroup start [-q] [-d] [-n N] [--] COMMAND [ARG...]\n"
        "       processgroup start [-q] [-d] -f FILE\n"
        "Launch a batch inside one new process group and print its group id.\n"
        "  -n N     run N copies of COMMAND (1-%d, default 1)\n"
        "  -f FILE  run every command listed in FILE (one per line, # comments)\n"
        "  -q       print only the group id\n"
        "  -d       discard the jobs' output (lets PGID=$(processgroup start -qd ...) return)\n"
        "  -h       show this help\n", MAX_JOBS);
}

static int cmd_start(int argc, char **argv) {
    long copies = 1;
    const char *jobfile = NULL;
    int quiet = 0, discard = 0, n_given = 0, opt;

    optind = 0;
    opterr = 0;
    while ((opt = getopt(argc, argv, "+:n:f:qdh")) != -1) {
        switch (opt) {
        case 'n':
            if (parse_long(optarg, 1, MAX_JOBS, &copies) == -1) {
                fprintf(stderr, "processgroup start: -n expects a number from 1 to %d\n", MAX_JOBS);
                return PG_USAGE;
            }
            n_given = 1;
            break;
        case 'f': jobfile = optarg; break;
        case 'q': quiet = 1; break;
        case 'd': discard = 1; break;
        case 'h': start_usage(stdout); return PG_OK;
        default:  return option_error("start", opt);
        }
    }

    struct job *jobs = calloc(MAX_JOBS, sizeof *jobs);
    if (jobs == NULL) {
        fprintf(stderr, "processgroup: out of memory\n");
        return PG_FAIL;
    }
    int njobs = 0, rc = PG_FAIL;

    if (jobfile != NULL) {
        if (n_given || optind != argc) {
            fprintf(stderr, "processgroup start: -f cannot be combined with -n or a command\n");
            rc = PG_USAGE;
            goto done;
        }
        if (load_job_file(jobfile, jobs, &njobs) == -1) {
            goto done;
        }
        if (njobs == 0) {
            fprintf(stderr, "processgroup: '%s' contains no commands\n", jobfile);
            goto done;
        }
    } else {
        if (optind >= argc) {
            fprintf(stderr, "processgroup start: no command given\n");
            start_usage(stderr);
            rc = PG_USAGE;
            goto done;
        }
        for (long i = 0; i < copies; i++) {
            jobs[njobs].args = &argv[optind];
            jobs[njobs].owned = 0;
            njobs++;
        }
    }

    /* Refuse the whole batch if any command cannot be found. */
    for (int i = 0; i < njobs; i++) {
        if (!command_exists(jobs[i].args[0])) {
            fprintf(stderr, "processgroup: command not found or not executable: %s\n", jobs[i].args[0]);
            goto done;
        }
    }

    /* Pipe used by children to report a failure; closes itself on exec. */
    int errpipe[2];
    if (pipe(errpipe) == -1) {
        perror("processgroup: pipe");
        goto done;
    }
    fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);

    /* Children are never left as zombies: ignore SIGCHLD while launching,
     * restore the caller's handler afterwards. */
    struct sigaction ign, old_chld;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGCHLD, &ign, &old_chld);

    pid_t pgid = 0;
    pid_t *pids = calloc((size_t)njobs, sizeof *pids);
    int launched = 0;
    if (pids == NULL) {
        fprintf(stderr, "processgroup: out of memory\n");
        sigaction(SIGCHLD, &old_chld, NULL);
        close(errpipe[0]);
        close(errpipe[1]);
        goto done;
    }

    for (int i = 0; i < njobs; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("processgroup: fork");
            break;
        }
        if (pid == 0) {
            /* ---- child ---- */
            struct exec_err e = { getpid(), 0, 0 };
            if (setpgid(0, pgid) == -1) {      /* pgid == 0: new group */
                e.err = errno;
                ssize_t w = write(errpipe[1], &e, sizeof e);
                (void)w;
                _exit(127);
            }
            /* Start the program with default signal behaviour; an ignored
             * SIGCHLD would break programs that wait for their children. */
            int sigs[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD };
            for (size_t k = 0; k < sizeof sigs / sizeof sigs[0]; k++) {
                signal(sigs[k], SIG_DFL);
            }
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, NULL);
            int nfd = open("/dev/null", O_RDONLY);   /* batch jobs never read the terminal */
            if (nfd != -1) {
                dup2(nfd, STDIN_FILENO);
                if (nfd != STDIN_FILENO) {
                    close(nfd);
                }
            }
            if (discard) {                       /* do not hold the caller's pipe open */
                int dn = open("/dev/null", O_WRONLY);
                if (dn != -1) {
                    dup2(dn, STDOUT_FILENO);
                    dup2(dn, STDERR_FILENO);
                    if (dn > STDERR_FILENO) {
                        close(dn);
                    }
                }
            }
            execvp(jobs[i].args[0], jobs[i].args);
            e.err = errno;
            e.stage = 1;
            ssize_t w = write(errpipe[1], &e, sizeof e);
            (void)w;
            _exit(127);
        }
        /* ---- parent ---- */
        if (pgid == 0) {
            pgid = pid;                        /* the first child leads the group */
        }
        setpgid(pid, pgid);                    /* also done in the child: avoids a race */
        pids[launched++] = pid;
    }

    close(errpipe[1]);
    int failed = 0;
    struct exec_err e;
    for (;;) {
        ssize_t r = read(errpipe[0], &e, sizeof e);
        if (r == -1 && errno == EINTR) {
            continue;
        }
        if (r != (ssize_t)sizeof e) {
            break;                             /* EOF: every child has exec'd or exited */
        }
        failed++;
        fprintf(stderr, "processgroup: process %d could not start (%s): %s\n",
                (int)e.pid, e.stage == 0 ? "joining the group" : "exec", strerror(e.err));
    }
    close(errpipe[0]);
    sigaction(SIGCHLD, &old_chld, NULL);

    if (launched == 0 || failed == launched) {
        free(pids);
        goto done;
    }

    if (quiet) {
        printf("%d\n", (int)pgid);
    } else {
        printf("Started %d process(es) in process group %d.\n", launched - failed, (int)pgid);
        for (int i = 0; i < launched; i++) {
            printf("  PID %d\n", (int)pids[i]);
        }
        printf("Next: processgroup status %d | monitor %d | signal %d STOP | terminate %d\n",
               (int)pgid, (int)pgid, (int)pgid, (int)pgid);
    }
    free(pids);
    rc = (failed > 0 || launched < njobs) ? PG_FAIL : PG_OK;

done:
    free_jobs(jobs, njobs);
    return rc;
}

/* ===================================================================== */
/*  status / monitor                                                      */
/* ===================================================================== */

static int cmd_status(int argc, char **argv) {
    int opt;
    optind = 0;
    opterr = 0;
    while ((opt = getopt(argc, argv, "+:h")) != -1) {
        if (opt == 'h') {
            printf("Usage: processgroup status PGID\nShow every process in the group once.\n");
            return PG_OK;
        }
        return option_error("status", opt);
    }
    if (argc - optind != 1) {
        fprintf(stderr, "Usage: processgroup status PGID\n");
        return PG_USAGE;
    }
    pid_t pgid;
    if (parse_pgid(argv[optind], &pgid) == -1) {
        return PG_USAGE;
    }
    struct snapshot s;
    if (take_snapshot(pgid, &s) == -1) {
        return PG_FAIL;
    }
    if (s.n == 0) {
        fprintf(stderr, "processgroup: no processes found in group %d\n", (int)pgid);
        snapshot_free(&s);
        return PG_FAIL;
    }
    printf("Process group %d\n", (int)pgid);
    print_table(&s, NULL);
    snapshot_free(&s);
    return PG_OK;
}

struct prev_sample {
    pid_t pid;
    unsigned long ticks;
};

static int cmd_monitor(int argc, char **argv) {
    double interval = 1.0;
    long count = 0;             /* 0 = keep going until the group ends */
    int opt;

    optind = 0;
    opterr = 0;
    while ((opt = getopt(argc, argv, "+:i:c:h")) != -1) {
        switch (opt) {
        case 'i':
            if (parse_seconds(optarg, 0.1, 3600.0, &interval) == -1) {
                fprintf(stderr, "processgroup monitor: -i expects seconds between 0.1 and 3600\n");
                return PG_USAGE;
            }
            break;
        case 'c':
            if (parse_long(optarg, 1, 1000000, &count) == -1) {
                fprintf(stderr, "processgroup monitor: -c expects a positive number\n");
                return PG_USAGE;
            }
            break;
        case 'h':
            printf("Usage: processgroup monitor [-i SECS] [-c COUNT] PGID\n"
                   "Refresh the group's resource table every SECS seconds (default 1)\n"
                   "until it exits, COUNT samples were shown, or Ctrl+C is pressed.\n");
            return PG_OK;
        default:
            return option_error("monitor", opt);
        }
    }
    if (argc - optind != 1) {
        fprintf(stderr, "Usage: processgroup monitor [-i SECS] [-c COUNT] PGID\n");
        return PG_USAGE;
    }
    pid_t pgid;
    if (parse_pgid(argv[optind], &pgid) == -1) {
        return PG_USAGE;
    }

    struct saved_handlers sh;
    install_handlers(&sh);

    double tick = (double)sysconf(_SC_CLK_TCK);
    struct prev_sample *prev = NULL;
    size_t nprev = 0;
    double prev_time = 0.0;
    int rc = PG_OK;

    for (long sample = 1; !g_interrupted; sample++) {
        struct snapshot s;
        if (take_snapshot(pgid, &s) == -1) {
            rc = PG_FAIL;
            break;
        }
        if (s.n == 0) {
            if (sample == 1) {
                fprintf(stderr, "processgroup: no processes found in group %d\n", (int)pgid);
                rc = PG_FAIL;
            } else {
                printf("Group %d has no more processes.\n", (int)pgid);
            }
            snapshot_free(&s);
            break;
        }

        double t = now_seconds();
        double *pct = calloc(s.n, sizeof *pct);
        struct prev_sample *cur = malloc(s.n * sizeof *cur);
        if (pct == NULL || cur == NULL) {
            fprintf(stderr, "processgroup: out of memory\n");
            free(pct);
            free(cur);
            snapshot_free(&s);
            rc = PG_FAIL;
            break;
        }
        for (size_t i = 0; i < s.n; i++) {
            unsigned long ticks = s.v[i].utime + s.v[i].stime;
            cur[i].pid = s.v[i].pid;
            cur[i].ticks = ticks;
            for (size_t k = 0; k < nprev; k++) {      /* %CPU needs the earlier reading */
                if (prev[k].pid == s.v[i].pid && t > prev_time && ticks >= prev[k].ticks) {
                    pct[i] = ((double)(ticks - prev[k].ticks) / tick) / (t - prev_time) * 100.0;
                    break;
                }
            }
        }

        char stamp[16];
        time_t wall = time(NULL);
        strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&wall));
        printf("[%s] group %d, sample %ld\n", stamp, (int)pgid, sample);
        print_table(&s, pct);
        fflush(stdout);

        free(pct);
        free(prev);
        prev = cur;
        nprev = s.n;
        prev_time = t;
        snapshot_free(&s);

        if (count > 0 && sample >= count) {
            break;
        }
        pg_sleep(interval);
    }

    if (g_interrupted) {
        printf("\nMonitoring stopped; group %d was left running.\n", (int)pgid);
    }
    free(prev);
    restore_handlers(&sh);
    return rc;
}

/* ===================================================================== */
/*  signal / terminate                                                    */
/* ===================================================================== */

static int cmd_signal(int argc, char **argv) {
    int opt;
    optind = 0;
    opterr = 0;
    while ((opt = getopt(argc, argv, "+:h")) != -1) {
        if (opt == 'h') {
            printf("Usage: processgroup signal PGID SIGNAL\n"
                   "Send SIGNAL (name such as STOP, CONT, TERM, KILL, USR1, or a number)\n"
                   "to every process in the group. Signal 0 only tests that the group exists.\n");
            return PG_OK;
        }
        return option_error("signal", opt);
    }
    if (argc - optind != 2) {
        fprintf(stderr, "Usage: processgroup signal PGID SIGNAL\n");
        return PG_USAGE;
    }
    pid_t pgid;
    int sig;
    if (parse_pgid(argv[optind], &pgid) == -1) {
        return PG_USAGE;
    }
    if (parse_signal(argv[optind + 1], &sig) == -1) {
        fprintf(stderr, "processgroup: unknown signal '%s'\n", argv[optind + 1]);
        return PG_USAGE;
    }
    if (check_signal_target(pgid) == -1) {
        return PG_FAIL;
    }
    struct snapshot s;
    if (take_snapshot(pgid, &s) == -1) {
        return PG_FAIL;
    }
    size_t n = s.n;
    snapshot_free(&s);
    if (n == 0) {
        fprintf(stderr, "processgroup: no processes found in group %d\n", (int)pgid);
        return PG_FAIL;
    }
    if (killpg(pgid, sig) == -1) {
        fprintf(stderr, "processgroup: cannot signal group %d: %s\n", (int)pgid, strerror(errno));
        return PG_FAIL;
    }
    printf("Sent signal %d to process group %d (%zu process(es)).\n", sig, (int)pgid, n);
    return PG_OK;
}

static int cmd_terminate(int argc, char **argv) {
    double wait_secs = DEFAULT_TERM_WAIT;
    int opt;

    optind = 0;
    opterr = 0;
    while ((opt = getopt(argc, argv, "+:w:h")) != -1) {
        switch (opt) {
        case 'w':
            if (parse_seconds(optarg, 0.0, 3600.0, &wait_secs) == -1) {
                fprintf(stderr, "processgroup terminate: -w expects seconds between 0 and 3600\n");
                return PG_USAGE;
            }
            break;
        case 'h':
            printf("Usage: processgroup terminate [-w SECS] PGID\n"
                   "Send SIGTERM (and SIGCONT, so stopped processes can react) to the group,\n"
                   "wait up to SECS seconds (default %.0f), then SIGKILL any survivors.\n",
                   DEFAULT_TERM_WAIT);
            return PG_OK;
        default:
            return option_error("terminate", opt);
        }
    }
    if (argc - optind != 1) {
        fprintf(stderr, "Usage: processgroup terminate [-w SECS] PGID\n");
        return PG_USAGE;
    }
    pid_t pgid;
    if (parse_pgid(argv[optind], &pgid) == -1) {
        return PG_USAGE;
    }
    if (check_signal_target(pgid) == -1) {
        return PG_FAIL;
    }

    struct snapshot s;
    if (take_snapshot(pgid, &s) == -1) {
        return PG_FAIL;
    }
    size_t initial = live_count(&s);
    snapshot_free(&s);
    if (initial == 0) {
        fprintf(stderr, "processgroup: no running processes found in group %d\n", (int)pgid);
        return PG_FAIL;
    }

    struct saved_handlers sh;
    install_handlers(&sh);

    if (killpg(pgid, SIGTERM) == -1) {
        fprintf(stderr, "processgroup: cannot signal group %d: %s\n", (int)pgid, strerror(errno));
        restore_handlers(&sh);
        return PG_FAIL;
    }
    killpg(pgid, SIGCONT);      /* a stopped process only acts on SIGTERM once continued */

    size_t live = initial;
    double deadline = now_seconds() + wait_secs;
    while (!g_interrupted) {
        if (take_snapshot(pgid, &s) == -1) {
            restore_handlers(&sh);
            return PG_FAIL;
        }
        live = live_count(&s);
        snapshot_free(&s);
        if (live == 0 || now_seconds() >= deadline) {
            break;
        }
        pg_sleep(0.1);
    }

    if (g_interrupted) {
        printf("Interrupted; group %d may still have running processes.\n", (int)pgid);
        restore_handlers(&sh);
        return PG_FAIL;
    }

    size_t exited_on_term = initial - live;
    size_t killed = 0;
    if (live > 0) {
        printf("%zu process(es) still running after %.1f s; sending SIGKILL.\n", live, wait_secs);
        killpg(pgid, SIGKILL);
        double kill_deadline = now_seconds() + 2.0;
        while (!g_interrupted) {
            if (take_snapshot(pgid, &s) == -1) {
                break;
            }
            size_t left = live_count(&s);
            snapshot_free(&s);
            killed = live - left;
            live = left;
            if (live == 0 || now_seconds() >= kill_deadline) {
                break;
            }
            pg_sleep(0.05);
        }
    }
    restore_handlers(&sh);

    if (live > 0) {
        fprintf(stderr, "processgroup: %zu process(es) in group %d could not be terminated\n", live, (int)pgid);
        return PG_FAIL;
    }
    printf("Group %d terminated: %zu exited on SIGTERM, %zu needed SIGKILL.\n",
           (int)pgid, exited_on_term, killed);
    return PG_OK;
}

/* ===================================================================== */
/*  entry point                                                           */
/* ===================================================================== */

static void usage(FILE *out) {
    fprintf(out,
        "Usage: processgroup <command> [options] [arguments]\n"
        "\n"
        "Manage a batch of processes as one process group.\n"
        "\n"
        "Commands:\n"
        "  start     [-q] [-d] [-n N | -f FILE] [--] [COMMAND [ARG...]]\n"
        "                                launch a batch in a new group, print its PGID\n"
        "  status    PGID                list the group's processes once\n"
        "  monitor   [-i SECS] [-c COUNT] PGID\n"
        "                                refresh CPU and memory use until the group ends\n"
        "  signal    PGID SIGNAL         send a signal to every process in the group\n"
        "  terminate [-w SECS] PGID      SIGTERM, then SIGKILL after SECS (default %.0f)\n"
        "  help                          show this message\n"
        "\n"
        "Use 'processgroup <command> -h' for details on one command.\n",
        DEFAULT_TERM_WAIT);
}

/* Called by main() below, or directly by the Custom Shell. argv[0] is the
 * program name, argv[1] the subcommand; argv must be NULL-terminated. */
int processgroup_main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(stderr);
        return PG_USAGE;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return PG_OK;
    }
    if (strcmp(cmd, "start") == 0)     return cmd_start(argc - 1, argv + 1);
    if (strcmp(cmd, "status") == 0)    return cmd_status(argc - 1, argv + 1);
    if (strcmp(cmd, "monitor") == 0)   return cmd_monitor(argc - 1, argv + 1);
    if (strcmp(cmd, "signal") == 0)    return cmd_signal(argc - 1, argv + 1);
    if (strcmp(cmd, "terminate") == 0) return cmd_terminate(argc - 1, argv + 1);

    fprintf(stderr, "processgroup: unknown command '%s'\n\n", cmd);
    usage(stderr);
    return PG_USAGE;
}

#ifndef PROCESSGROUP_NO_MAIN
int main(int argc, char *argv[]) {
    return processgroup_main(argc, argv);
}
#endif
