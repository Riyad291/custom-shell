/*
 * loganalyzer.c - parse large log files with mmap(2) and report statistics
 *
 * Operating System I, Group Project (Custom Shell), Group 6
 *
 * The file is mapped into the address space with mmap() instead of being
 * copied into a user-space buffer with read(). The scan then walks the
 * mapping in place, locating line endings with memchr(), so no line is ever
 * copied anywhere.
 *
 * Build standalone:  gcc -DSTANDALONE -Wall -Wextra -O2 -o loganalyzer loganalyzer.c
 * Build for the shell: gcc -Wall -Wextra -O2 -c loganalyzer.c
 */

#define _GNU_SOURCE             /* memmem(3) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "loganalyzer.h"

#define PROG            "loganalyzer"
#define TABLE_SIZE      16384       /* slots in the ranking table           */
#define MAX_DISTINCT    (TABLE_SIZE / 2)  /* keep the table half empty so
                                           * probe chains stay short        */
#define MAX_PROBE       64          /* bound the work done per line         */
#define KEY_MAX         64          /* longest key stored, longer is cut    */
#define READ_BUF        (64 * 1024) /* buffer for the read() comparison     */
#define SIGCHECK_EVERY  100000UL    /* lines between stop-flag checks       */

/* ---------------------------------------------------------------- levels */

enum { LVL_INFO, LVL_WARN, LVL_ERROR, LVL_DEBUG, LVL_COUNT };

static const char *const level_name[LVL_COUNT] = {
    "INFO", "WARN", "ERROR", "DEBUG"
};

/* --------------------------------------------------------------- options */

enum { KEY_IP, KEY_PATH };

typedef struct {
    const char *path;       /* -f */
    const char *pattern;    /* -p, NULL if unused            */
    int         level;      /* -l, -1 if unused              */
    long        topn;       /* -t                            */
    int         keyfield;   /* -k                            */
    int         compare;    /* -c                            */
    int         verbose;    /* -v                            */
} opts_t;

/* ----------------------------------------------------------------- stats */

typedef struct {
    char            key[KEY_MAX];
    unsigned long   count;
    unsigned char   klen;       /* 0 means the slot is free */
} keyent_t;

typedef struct {
    unsigned long   lines;
    unsigned long   bytes;
    unsigned long   matched;
    unsigned long   level[LVL_COUNT];
    keyent_t       *table;
    unsigned long   distinct;
    unsigned long   dropped;    /* keys not ranked, table was full */
} stats_t;

/*
 * The signal handler can only touch a file-scope object, so this one flag is
 * unavoidably global. It is reset at the top of cmd_loganalyzer() so that a
 * Ctrl+C during one run inside the shell cannot affect the next run.
 */
static volatile sig_atomic_t stop_flag = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* ----------------------------------------------------------------- utils */

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: " PROG " -f FILE [-p PATTERN] [-l LEVEL] [-t N]\n"
        "                   [-k ip|path] [-c] [-v] [-h]\n"
        "\n"
        "  -f FILE      log file to analyze (required)\n"
        "  -p PATTERN   count only lines containing PATTERN\n"
        "  -l LEVEL     count only lines of LEVEL (ERROR, WARN, INFO, DEBUG)\n"
        "  -t N         report the N most frequent keys (default 5)\n"
        "  -k FIELD     field to rank: ip or path (default ip)\n"
        "  -c           also scan with read() and compare elapsed times\n"
        "  -v           verbose: file size, page size, pages mapped\n"
        "  -h           show this help\n");
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Does the line contain `tok` as a whole word? */
static int has_token(const char *p, size_t len, const char *tok)
{
    size_t tlen = strlen(tok);
    const char *hit = p;
    size_t left = len;

    while (left >= tlen) {
        hit = memmem(hit, left, tok, tlen);
        if (hit == NULL)
            return 0;
        {
            int left_ok  = (hit == p)            || hit[-1] == ' ' || hit[-1] == '[';
            int right_ok = (hit + tlen == p + len) || hit[tlen] == ' ' || hit[tlen] == ']';
            if (left_ok && right_ok)
                return 1;
        }
        hit += tlen;
        left = (size_t)((p + len) - hit);
    }
    return 0;
}

static int line_level(const char *p, size_t len)
{
    int i;
    for (i = 0; i < LVL_COUNT; i++)
        if (has_token(p, len, level_name[i]))
            return i;
    return -1;
}

/*
 * Pull one field out of a common-log-format line, without copying and
 * without modifying the mapping (it is PROT_READ, so strtok would crash).
 *
 *   192.168.1.5 - - [12/Nov/2026:09:14:03] "GET /api/users HTTP/1.1" 200 12 INFO
 *   ^ip                                         ^path
 */
static int extract_field(const char *p, size_t len, int which,
                         const char **out, size_t *outlen)
{
    if (which == KEY_IP) {
        const char *sp = memchr(p, ' ', len);
        size_t n = sp ? (size_t)(sp - p) : len;
        if (n == 0)
            return -1;
        *out = p;
        *outlen = n;
        return 0;
    } else {
        const char *q = memchr(p, '"', len);          /* opening quote     */
        const char *end = p + len;
        const char *sp, *sp2;
        if (q == NULL)
            return -1;
        q++;
        sp = memchr(q, ' ', (size_t)(end - q));       /* end of the method */
        if (sp == NULL)
            return -1;
        sp++;
        sp2 = memchr(sp, ' ', (size_t)(end - sp));    /* end of the path   */
        if (sp2 == NULL)
            sp2 = end;
        if (sp2 == sp)
            return -1;
        *out = sp;
        *outlen = (size_t)(sp2 - sp);
        return 0;
    }
}

/* FNV-1a, 32 bit */
static unsigned long fnv1a(const char *s, size_t n)
{
    unsigned long h = 2166136261UL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619UL;
    }
    return h;
}

/*
 * Open addressing with linear probing. The probe run is bounded, and the
 * table is never filled past half, so the cost per line stays constant
 * instead of degrading to a full sweep once the table is busy.
 */
static void table_add(stats_t *st, const char *key, size_t klen)
{
    size_t slot, probe;

    if (klen >= KEY_MAX)
        klen = KEY_MAX - 1;

    slot = fnv1a(key, klen) % TABLE_SIZE;

    for (probe = 0; probe < MAX_PROBE; probe++) {
        keyent_t *e = &st->table[(slot + probe) % TABLE_SIZE];

        if (e->klen == 0) {                 /* free slot: a new key */
            if (st->distinct >= MAX_DISTINCT) {
                st->dropped++;              /* at capacity, not ranked */
                return;
            }
            memcpy(e->key, key, klen);
            e->key[klen] = '\0';
            e->klen = (unsigned char)klen;
            e->count = 1;
            st->distinct++;
            return;
        }
        if (e->klen == klen && memcmp(e->key, key, klen) == 0) {
            e->count++;
            return;
        }
    }
    st->dropped++;              /* probe run exhausted, still counted above */
}

static int cmp_desc(const void *a, const void *b)
{
    const keyent_t *x = a, *y = b;
    if (y->count > x->count) return 1;
    if (y->count < x->count) return -1;
    return strcmp(x->key, y->key);          /* stable, readable tie-break */
}

/* ------------------------------------------------------------- line pass */

static void process_line(const char *p, size_t len, const opts_t *o, stats_t *st)
{
    int lvl;
    const char *key;
    size_t klen;

    st->bytes += len + 1;

    lvl = line_level(p, len);
    if (lvl >= 0)
        st->level[lvl]++;

    if (o->level >= 0 && lvl != o->level)
        return;
    if (o->pattern != NULL &&
        memmem(p, len, o->pattern, strlen(o->pattern)) == NULL)
        return;

    st->matched++;

    if (extract_field(p, len, o->keyfield, &key, &klen) == 0)
        table_add(st, key, klen);
}

/* ------------------------------------------------- the two scan variants */

/* Full analysis over the mapping. Returns 0, or 130 if interrupted. */
static int scan_mapping(const char *map, size_t size, const opts_t *o, stats_t *st)
{
    const char *p = map;
    const char *end = map + size;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        process_line(p, len, o, st);
        st->lines++;

        if ((st->lines % SIGCHECK_EVERY) == 0 && stop_flag)
            return 130;

        if (nl == NULL)
            break;                  /* last line has no trailing newline */
        p = nl + 1;
    }
    return 0;
}

/* Line counting only, over the mapping. Used by -c so both sides do the
 * same amount of work and the timing comparison is honest. */
static unsigned long count_lines_mapping(const char *map, size_t size)
{
    const char *p = map;
    const char *end = map + size;
    unsigned long lines = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        lines++;
        if (nl == NULL)
            break;
        p = nl + 1;
    }
    return lines;
}

/* Line counting only, through ordinary read() calls. */
static long count_lines_read(const char *path, unsigned long *lines_out)
{
    char *buf;
    int fd;
    ssize_t n;
    unsigned long lines = 0;
    int saw_data = 0, last_was_nl = 1;

    buf = malloc(READ_BUF);
    if (buf == NULL) {
        fprintf(stderr, PROG ": out of memory\n");
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, PROG ": %s: %s\n", path, strerror(errno));
        free(buf);
        return -1;
    }

    while ((n = read(fd, buf, READ_BUF)) > 0) {
        const char *p = buf, *end = buf + n;
        saw_data = 1;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            if (nl == NULL)
                break;
            lines++;
            p = nl + 1;
        }
        last_was_nl = (buf[n - 1] == '\n');
    }

    if (n == -1) {
        fprintf(stderr, PROG ": %s: %s\n", path, strerror(errno));
        close(fd);
        free(buf);
        return -1;
    }

    if (saw_data && !last_was_nl)
        lines++;                    /* trailing line with no newline */

    close(fd);
    free(buf);
    *lines_out = lines;
    return 0;
}

/* Line counting the way most people write it: stdio, one line at a time. */
static long count_lines_getline(const char *path, unsigned long *lines_out)
{
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    unsigned long lines = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, PROG ": %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (getline(&line, &cap, fp) != -1)
        lines++;

    free(line);
    fclose(fp);
    *lines_out = lines;
    return 0;
}

/* --------------------------------------------------------------- reports */

static void report(const opts_t *o, const stats_t *st, double secs, int partial)
{
    int i;
    double mb = (double)st->bytes / (1024.0 * 1024.0);

    if (partial)
        printf("\n" PROG ": interrupted, partial results below\n");

    printf("\nFile:      %s\n", o->path);
    printf("Lines:     %lu\n", st->lines);
    printf("Bytes:     %lu (%.1f MiB)\n", st->bytes, mb);
    printf("Scan time: %.3f s", secs);
    if (secs > 0.0)
        printf("  (%.1f MiB/s)", mb / secs);
    printf("\n");

    printf("\nLevels:\n");
    for (i = 0; i < LVL_COUNT; i++)
        printf("  %-6s %10lu\n", level_name[i], st->level[i]);

    if (o->pattern != NULL || o->level >= 0) {
        printf("\nFilter:    ");
        if (o->level >= 0)
            printf("level=%s ", level_name[o->level]);
        if (o->pattern != NULL)
            printf("pattern=\"%s\" ", o->pattern);
        printf("\nMatched:   %lu line(s)\n", st->matched);
    }

    if (st->distinct > 0) {
        keyent_t *sorted;
        unsigned long shown = 0, i2;

        sorted = malloc(sizeof(keyent_t) * TABLE_SIZE);
        if (sorted == NULL)
            return;
        memcpy(sorted, st->table, sizeof(keyent_t) * TABLE_SIZE);
        qsort(sorted, TABLE_SIZE, sizeof(keyent_t), cmp_desc);

        printf("\nTop %ld by %s (%lu distinct):\n",
               o->topn, o->keyfield == KEY_IP ? "ip" : "path", st->distinct);

        for (i2 = 0; i2 < TABLE_SIZE && shown < (unsigned long)o->topn; i2++) {
            if (sorted[i2].klen == 0)
                break;
            printf("  %10lu  %s\n", sorted[i2].count, sorted[i2].key);
            shown++;
        }
        if (st->dropped > 0)
            printf("  (%lu line(s) past the %d-key limit were not ranked)\n",
                   st->dropped, MAX_DISTINCT);
        free(sorted);
    }
}

/* ------------------------------------------------------------------ main */

int cmd_loganalyzer(int argc, char **argv)
{
    opts_t o;
    stats_t st;
    struct sigaction sa, old_sa;
    struct stat sb;
    int fd = -1, c, rc = 0, interrupted = 0, have_old = 0;
    char *map = MAP_FAILED;
    size_t size = 0;
    double t0, t1;

    /* getopt and the stop flag both keep state between calls; the shell
     * runs this function again in the same process, so reset both. */
    optind = 1;
    stop_flag = 0;

    memset(&o, 0, sizeof o);
    o.level = -1;
    o.topn = 5;
    o.keyfield = KEY_IP;

    while ((c = getopt(argc, argv, "f:p:l:t:k:cvh")) != -1) {
        switch (c) {
        case 'f':
            o.path = optarg;
            break;
        case 'p':
            o.pattern = optarg;
            break;
        case 'l': {
            int i, found = -1;
            for (i = 0; i < LVL_COUNT; i++)
                if (strcasecmp(optarg, level_name[i]) == 0)
                    found = i;
            if (found < 0) {
                fprintf(stderr, PROG ": unknown level \"%s\"\n", optarg);
                return 1;
            }
            o.level = found;
            break;
        }
        case 't': {
            char *endp;
            long v;
            errno = 0;
            v = strtol(optarg, &endp, 10);
            if (errno != 0 || *endp != '\0' || endp == optarg || v <= 0) {
                fprintf(stderr, PROG ": -t needs a positive number, got \"%s\"\n",
                        optarg);
                return 1;
            }
            o.topn = v;
            break;
        }
        case 'k':
            if (strcmp(optarg, "ip") == 0)
                o.keyfield = KEY_IP;
            else if (strcmp(optarg, "path") == 0)
                o.keyfield = KEY_PATH;
            else {
                fprintf(stderr, PROG ": -k must be ip or path, got \"%s\"\n",
                        optarg);
                return 1;
            }
            break;
        case 'c':
            o.compare = 1;
            break;
        case 'v':
            o.verbose = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    if (o.path == NULL) {
        fprintf(stderr, PROG ": no input file, use -f FILE\n");
        usage(stderr);
        return 1;
    }

    fd = open(o.path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, PROG ": %s: %s\n", o.path, strerror(errno));
        return 1;
    }

    if (fstat(fd, &sb) == -1) {
        fprintf(stderr, PROG ": %s: %s\n", o.path, strerror(errno));
        close(fd);
        return 1;
    }

    if (!S_ISREG(sb.st_mode)) {
        fprintf(stderr, PROG ": %s: not a regular file\n", o.path);
        close(fd);
        return 1;
    }

    size = (size_t)sb.st_size;

    /* mmap() of length 0 fails with EINVAL, so deal with it first. */
    if (size == 0) {
        close(fd);
        printf("\nFile:      %s\nLines:     0\nBytes:     0\n", o.path);
        return 0;
    }

    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, PROG ": %s: mmap: %s\n", o.path, strerror(errno));
        close(fd);
        return 1;
    }

    /* We read front to back exactly once: let the kernel read ahead harder
     * and drop the pages behind us. */
    if (madvise(map, size, MADV_SEQUENTIAL) == -1 && o.verbose)
        fprintf(stderr, PROG ": madvise: %s\n", strerror(errno));

    /* The mapping stays valid once the descriptor is closed. */
    close(fd);
    fd = -1;

    if (o.verbose) {
        long pagesz = sysconf(_SC_PAGESIZE);
        printf(PROG ": mapped %zu bytes, page size %ld, %zu pages\n",
               size, pagesz, (size + (size_t)pagesz - 1) / (size_t)pagesz);
    }

    memset(&st, 0, sizeof st);
    st.table = calloc(TABLE_SIZE, sizeof(keyent_t));
    if (st.table == NULL) {
        fprintf(stderr, PROG ": out of memory\n");
        munmap(map, size);
        return 1;
    }

    /* Catch Ctrl+C during the scan, and hand the terminal back afterwards. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                    /* no SA_RESTART: we want the break */
    if (sigaction(SIGINT, &sa, &old_sa) == 0)
        have_old = 1;

    t0 = now_seconds();
    rc = scan_mapping(map, size, &o, &st);
    t1 = now_seconds();
    interrupted = (rc == 130);

    report(&o, &st, t1 - t0, interrupted);

    if (o.compare && !interrupted) {
        unsigned long l_map = 0, l_read = 0, l_gl = 0;
        double m0, m1, r0 = 0, r1 = 0, g0 = 0, g1 = 0;
        int ok = 1;

        /* All three count lines and nothing else, so the comparison is fair. */
        m0 = now_seconds();
        l_map = count_lines_mapping(map, size);
        m1 = now_seconds();

        r0 = now_seconds();
        if (count_lines_read(o.path, &l_read) == -1)
            ok = 0;
        r1 = now_seconds();

        if (ok) {
            g0 = now_seconds();
            if (count_lines_getline(o.path, &l_gl) == -1)
                ok = 0;
            g1 = now_seconds();
        }

        if (!ok) {
            rc = 1;
        } else {
            double tm = m1 - m0, tr = r1 - r0, tg = g1 - g0;

            printf("\nLine counting, identical work three ways:\n");
            printf("  mmap + memchr     %.6f s  (%lu lines)\n", tm, l_map);
            printf("  read() + memchr   %.6f s  (%lu lines)\n", tr, l_read);
            printf("  stdio getline()   %.6f s  (%lu lines)\n", tg, l_gl);

            /* A small file can scan faster than the clock can resolve. */
            if (tm > 1e-9) {
                printf("\n  mmap vs read():    %.2fx\n", tr / tm);
                printf("  mmap vs getline(): %.2fx\n", tg / tm);
            } else {
                printf("\n  mmap vs read():    file too small to time\n");
                printf("  mmap vs getline(): file too small to time\n");
            }
            if (l_map != l_read || l_map != l_gl)
                printf("  WARNING: the scans disagree on the line count\n");
        }
    }

    if (have_old)
        sigaction(SIGINT, &old_sa, NULL);

    free(st.table);

    if (munmap(map, size) == -1) {
        fprintf(stderr, PROG ": munmap: %s\n", strerror(errno));
        return 1;
    }

    return rc;
}

#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_loganalyzer(argc, argv);
}
#endif
