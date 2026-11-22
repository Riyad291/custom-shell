
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include <getopt.h>

#include "timedexec.h"

/* Keeps track of whether the user interrupted the program. */
static volatile sig_atomic_t interrupted = 0;

static void handle_signal(int sig)
{
    interrupted = sig;
}

/* Shows how to use the command and its available options. */
static void usage(void)
{
    printf("Usage: timedexec [-t seconds] [-c seconds] "
           "[-m MB] [-v] [-s] -- command [args...]\n");
    printf("  -t N   Wall-clock timeout in seconds\n");
    printf("  -c N   CPU-time limit in seconds\n");
    printf("  -m N   Memory address-space limit in MB\n");
    printf("  -v     Verbose process and limit details\n");
    printf("  -s     Show CPU time and peak memory summary\n");
    printf("  -h     Display help\n");
}

/* Checks that the given limit is a valid positive number. */
static int parse_positive(const char *s, unsigned long long *value)
{
    char *end;
    unsigned long long n;

    if (!s || !*s || *s == '-')
        return -1;

    errno = 0;
    n = strtoull(s, &end, 10);

    if (errno || *end || n == 0)
        return -1;

    *value = n;
    return 0;
}

/* Calculates how much time has passed since the program started. */
static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *now)
{
    return (double)(now->tv_sec - start->tv_sec) +
           (double)(now->tv_nsec - start->tv_nsec) / 1e9;
}

/* Waits for the child and collects its resource usage. */
static int collect_child(pid_t pid, int *status, struct rusage *stats)
{
    pid_t waited;

    do {
        waited = wait4(pid, status, 0, stats);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        perror("timedexec: wait4");
        return -1;
    }

    return 0;
}

/* Stops the process group and waits for the child to finish. */
static int stop_child(pid_t pid, int *status, struct rusage *stats)
{
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);

    return collect_child(pid, status, stats);
}

int cmd_timedexec(int argc, char **argv)
{
    unsigned long long timeout = 0, cpu = 0, memory = 0;
    int option, status = 0, result = 1;
    int timeout_hit = 0, verbose = 0, summary = 0, have_usage = 0;
    struct rusage usage_stats = {0};
    pid_t pid;
    struct timespec start, now;
    struct sigaction sa, old_int, old_term;
    int have_int = 0, have_term = 0;

    /* Reset these values so the command can run again inside the shell. */
    optind = 1;
    interrupted = 0;

    /* Read the options entered by the user. */
    while ((option = getopt(argc, argv, "+t:c:m:vsh")) != -1) {
        unsigned long long n = 0;

        if (option == 'h') {
            usage();
            return 0;
        }

        if (option == 'v') {
            verbose = 1;
            continue;
        }

        if (option == 's') {
            summary = 1;
            continue;
        }

        if (option == '?') {
            usage();
            return 1;
        }

        if (parse_positive(optarg, &n) != 0) {
            fprintf(stderr, "timedexec: invalid limit: %s\n", optarg);
            return 1;
        }

        if (option == 't')
            timeout = n;
        else if (option == 'c')
            cpu = n;
        else if (option == 'm')
            memory = n;
    }

    /* A command and at least one resource limit are required. */
    if (optind >= argc || (!timeout && !cpu && !memory)) {
        fprintf(stderr, "timedexec: missing command or resource limit\n");
        usage();
        return 1;
    }

    /* Make sure the resource limits are not too large. */
    if ((cpu && cpu >= (unsigned long long)RLIM_INFINITY - 1) ||
        (memory && (memory > ULLONG_MAX / (1024ULL * 1024ULL) ||
         memory > (unsigned long long)RLIM_INFINITY / (1024ULL * 1024ULL) ||
         memory * 1024ULL * 1024ULL >= (unsigned long long)RLIM_INFINITY))) {
        fprintf(stderr, "timedexec: resource limit too large\n");
        return 1;
    }

    /* Record the starting time for the wall-clock timeout. */
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        perror("timedexec: clock_gettime");
        return 1;
    }

    /* Handle Ctrl+C and termination signals. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, &old_int) == -1) {
        perror("timedexec: sigaction");
        return 1;
    }
    have_int = 1;

    if (sigaction(SIGTERM, &sa, &old_term) == -1) {
        perror("timedexec: sigaction");
        goto restore;
    }
    have_term = 1;

    /* Create a child process to run the requested command. */
    pid = fork();

    if (pid < 0) {
        perror("timedexec: fork");
        goto restore;
    }

    if (pid == 0) {
        struct rlimit limit;

        /* Restore the original signal handlers in the child. */
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);

        /* Put the child in its own process group. */
        if (setpgid(0, 0) == -1) {
            perror("timedexec: setpgid");
            _exit(125);
        }

        /* Set the CPU-time limit before running the command. */
        if (cpu) {
            limit.rlim_cur = (rlim_t)cpu;
            limit.rlim_max = (rlim_t)cpu + 1;

            if (setrlimit(RLIMIT_CPU, &limit) == -1) {
                perror("timedexec: CPU limit");
                _exit(125);
            }
        }

        /* Convert MB to bytes and set the memory limit. */
        if (memory) {
            unsigned long long bytes = memory * 1024ULL * 1024ULL;
            limit.rlim_cur = (rlim_t)bytes;
            limit.rlim_max = (rlim_t)bytes;

            if (setrlimit(RLIMIT_AS, &limit) == -1) {
                perror("timedexec: memory limit");
                _exit(125);
            }
        }

        /* Replace the child process with the requested program. */
        execvp(argv[optind], &argv[optind]);
        perror("timedexec: execvp");
        _exit(127);
    }

    /* Make sure the parent and child agree on the process group. */
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH) {
        perror("timedexec: setpgid");
        stop_child(pid, &status, &usage_stats);
        goto restore;
    }

    /* Show process information when verbose mode is enabled. */
    if (verbose) {
        printf("timedexec: PID %ld, wall=%llu s, CPU=%llu s, memory=%llu MB\n",
               (long)pid, timeout, cpu, memory);
        fflush(stdout);
    }

    /* Keep checking the child until it finishes or reaches a limit. */
    for (;;) {
        if (interrupted) {
            /* Stop the child if the user interrupts the command. */
            int sig = interrupted;

            if (stop_child(pid, &status, &usage_stats) == 0)
                have_usage = 1;

            result = 128 + sig;
            goto restore;
        }

        /* Check if the child finished and collect its resource usage. */
        pid_t waited = wait4(pid, &status, WNOHANG, &usage_stats);

        if (waited == pid) {
            have_usage = 1;
            break;
        }

        if (waited == -1) {
            if (errno == EINTR)
                continue;

            perror("timedexec: wait4");
            stop_child(pid, &status, &usage_stats);
            goto restore;
        }

        /* Stop the command if it exceeds the wall-clock timeout. */
        if (timeout) {
            if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
                perror("timedexec: clock_gettime");
                stop_child(pid, &status, &usage_stats);
                goto restore;
            }

            if (elapsed_seconds(&start, &now) >= (double)timeout) {
                timeout_hit = 1;

                if (stop_child(pid, &status, &usage_stats) == 0)
                    have_usage = 1;

                break;
            }
        }

        /* Wait briefly before checking again to avoid wasting CPU. */
        struct timespec delay = {0, 10000000L};
        nanosleep(&delay, NULL);
    }

    /* Report why the command stopped and return the correct status. */
    if (timeout_hit) {
        fprintf(stderr, "timedexec: wall-clock timeout exceeded\n");
        result = 124;
    } else if (WIFEXITED(status)) {
        result = WEXITSTATUS(status);
        printf("timedexec: process exited with status %d\n", result);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr, "timedexec: process terminated by signal %d\n", sig);
        result = 128 + sig;
    }

restore:
    /* Show the total elapsed time when verbose mode is enabled. */
    if (verbose && clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        printf("timedexec: elapsed wall time %.3f s\n",
               elapsed_seconds(&start, &now));
    }

    /* Show actual CPU usage and peak memory when summary is enabled. */
    if (summary && have_usage) {
        double user = usage_stats.ru_utime.tv_sec +
                      usage_stats.ru_utime.tv_usec / 1e6;
        double system = usage_stats.ru_stime.tv_sec +
                        usage_stats.ru_stime.tv_usec / 1e6;

        printf("timedexec: CPU user %.3f s, system %.3f s, total %.3f s\n",
               user, system, user + system);

        /* Linux reports peak resident memory in KiB. */
        printf("timedexec: peak resident memory %ld KiB\n",
               usage_stats.ru_maxrss);
    }

    /* Restore the original signal handlers before returning to the shell. */
    if (have_term)
        sigaction(SIGTERM, &old_term, NULL);

    if (have_int)
        sigaction(SIGINT, &old_int, NULL);

    return result;
}

/* Allows this command to run alone or as part of the Custom Shell. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_timedexec(argc, argv);
}
#endif
