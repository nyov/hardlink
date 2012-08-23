/* hardlink.c - Link multiple identical files together
 *
 * Copyright (C) 2008 - 2012 Julian Andres Klode <jak@jak-linux.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE             /* GNU extensions (optional) */
#define _POSIX_C_SOURCE 200112L /* POSIX functions */
#define _XOPEN_SOURCE      600  /* nftw() */

#define _FILE_OFFSET_BITS   64  /* Large file support */
#define _LARGEFILE_SOURCE       /* Large file support */
#define _LARGE_FILES            /* AIX apparently */

#include <sys/types.h>          /* stat */
#include <sys/stat.h>           /* stat */
#include <sys/time.h>           /* getrlimit, getrusage */
#include <sys/resource.h>       /* getrlimit, getrusage */
#include <unistd.h>             /* stat */
#include <fcntl.h>              /* posix_fadvise */
#include <ftw.h>                /* ftw */
#include <search.h>             /* tsearch() and friends */

#include <errno.h>              /* strerror, errno */
#include <locale.h>             /* setlocale */
#include <signal.h>             /* SIG*, sigaction */
#include <stdio.h>              /* stderr, fprint */
#include <stdarg.h>             /* va_arg */
#include <stdlib.h>             /* free(), realloc() */
#include <string.h>             /* strcmp() and friends */
#include <assert.h>             /* assert() */

/* Some boolean names for clarity */
typedef enum hl_bool {
    FALSE,
    TRUE
} hl_bool;

/* The makefile sets this for us and creates config.h */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* We don't have getopt_long(). Define no-op alternatives */
#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#else
#define getopt_long(argc, argv, shrt, lng, index) getopt((argc), (argv), (shrt))
#endif

/* For systems without posix_fadvise */
#ifndef HAVE_POSIX_FADVISE
#define posix_fadvise(fd, offset, len, advise) (void) 0
#endif

/* __attribute__ is fairly GNU-specific, define a no-op alternative elsewhere */
#ifndef __GNUC__
#define __attribute__(attributes)
#endif

/* Use libpcreposix if it's available, it's cooler */
#ifdef HAVE_libpcreposix
#include <pcreposix.h>
#undef REG_NOSUB
#define REG_NOSUB 0             /* we do want backreferences in PCRE mode */
#else
#include <regex.h>              /* regcomp(), regsearch() */
#endif

/**
 * struct file - Information about a file
 * @st:       The stat buffer associated with the file
 * @next:     Next file with the same size
 * @basename: The offset off the basename in the filename
 * @slave:    Whether the file has been linked to another one
 * @path:     The path of the file
 *
 * This contains all information we need about a file.
 */
struct file {
    struct stat st;
    struct file *next;
    hl_bool slave;
    struct link {
        struct link *next;
        int basename;
#if __STDC_VERSION__ >= 199901L
        char path[];
#elif __GNUC__
        char path[0];
#else
        char path[1];
#endif
    } *links;
};

/**
 * enum log_level - Logging levels
 * @JLOG_SYSFAT:  Fatal error message with errno, will be printed to stderr
 * @JLOG_FATAL:   Fatal error message with errno, will be printed to stderr
 * @JLOG_SYSERR:  Error message with errno, will be printed to stderr
 * @JLOG_ERROR:   Error message, will be printed to stderr
 * @JLOG_SUMMARY: Default log level
 * @JLOG_INFO:    Verbose logging (verbose == 1)
 * @JLOG_DEBUG1:  Verbosity 2
 * @JLOG_DEBUG2:  Verbosity 3
 */
enum log_level {
    JLOG_SYSFAT = -4,
    JLOG_FATAL = -3,
    JLOG_SYSERR = -2,
    JLOG_ERROR = -1,
    JLOG_SUMMARY,
    JLOG_INFO,
    JLOG_DEBUG1,
    JLOG_DEBUG2
};

/**
 * struct statistic - Statistics about the file
 * @started: Whether we are post command-line processing
 * @files: The number of files worked on
 * @linked: The number of files replaced by a hardlink to a master
 * @comparisons: The number of comparisons
 * @saved: The (exaggerated) amount of space saved
 * @start_time: The time we started at, in seconds since some unspecified point
 */
static struct statistics {
    hl_bool started;
    size_t files;
    size_t linked;
    size_t comparisons;
    double saved;
    double start_time;
} stats;

/**
 * struct options - Processed command-line options
 * @include: A linked list of regular expressions for the --include option
 * @exclude: A linked list of regular expressions for the --exclude option
 * @verbosity: The verbosity. Should be one of #enum log_level
 * @respect_mode: Whether to respect file modes (default = TRUE)
 * @respect_owner: Whether to respect file owners (uid, gid; default = TRUE)
 * @respect_name: Whether to respect file names (default = FALSE)
 * @respect_time: Whether to respect file modification times (default = TRUE)
 * @maximise: Chose the file with the highest link count as master
 * @minimise: Chose the file with the lowest link count as master
 * @dry_run: Specifies whether hardlink should not link files (default = FALSE)
 */
static struct options {
    struct regex_link {
        regex_t preg;
        struct regex_link *next;
    } *include, *exclude;
    signed int verbosity;
    unsigned int respect_mode:1;
    unsigned int respect_owner:1;
    unsigned int respect_name:1;
    unsigned int respect_time:1;
    unsigned int maximise:1;
    unsigned int minimise:1;
    unsigned int dry_run:1;
} opts;

/*
 * files
 *
 * A binary tree of files, managed using tsearch(). To see which nodes
 * are considered equal, see compare_nodes()
 */
static void *files;

/*
 * last_signal
 *
 * The last signal we received. We store the signal here in order to be able
 * to break out of loops gracefully and to return from our nftw() handler.
 */
static int last_signal;

__attribute__ ((format(printf, 2, 3)))
/**
 * jlog - Logging for hardlink
 * @level: The log level
 * @format: A format string for printf()
 */
static void jlog(enum log_level level, const char *format, ...)
{
    FILE *stream = (level >= 0) ? stdout : stderr;
    int errno_ = errno;
    va_list args;

    if (level <= opts.verbosity) {
        if (level <= JLOG_FATAL)
            fprintf(stream, "ERROR: ");
        else if (level < 0)
            fprintf(stream, "WARNING: ");
        va_start(args, format);
        vfprintf(stream, format, args);
        va_end(args);
        if (level == JLOG_SYSERR || level == JLOG_SYSFAT)
            fprintf(stream, ": %s\n", strerror(errno_));
        else
            fputc('\n', stream);
    }
}

/**
 * CMP - Compare two numerical values, return 1, 0, or -1
 * @a: First value
 * @b: Second value
 *
 * Used to compare two integers of any size while avoiding overflow.
 */
#define CMP(a, b) ((a) > (b) ? 1 : ((a) < (b) ? -1 : 0))

/**
 * format - Print a human-readable name for the given size
 * @bytes: A number specifying an amount of bytes
 *
 * Uses a double. The result with infinity and NaN is most likely
 * not pleasant.
 */
static const char *format(double bytes)
{
    static char buf[256];

    if (bytes >= 1024 * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f GiB", (bytes / 1024 / 1024 / 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f MiB", (bytes / 1024 / 1024));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%.2f KiB", (bytes / 1024));
    else
        snprintf(buf, sizeof(buf), "%.0f bytes", bytes);

    return buf;
}

/**
 * gettime() - Get the current time from the system
 */
static double gettime(void)
{
    struct timeval tv = { 0, 0 };

    if (gettimeofday(&tv, NULL) != 0)
        jlog(JLOG_SYSERR, "Cannot read current time");

    return (double) tv.tv_sec + (double) tv.tv_usec / 1000000;
}

/**
 * regexec_any - Match against multiple regular expressions
 * @pregs: A linked list of regular expressions
 * @what:  The string to match against
 *
 * Checks whether any of the regular expressions in the list matches the
 * string.
 */
static hl_bool regexec_any(struct regex_link *pregs, const char *what)
{
    for (; pregs != NULL; pregs = pregs->next)
        if (regexec(&pregs->preg, what, 0, NULL, 0) == 0)
            return TRUE;
    return FALSE;
}

/**
 * compare_nodes - Node comparison function
 * @_a: The first node (a #struct file)
 * @_b: The second node (a #struct file)
 *
 * Compare the two nodes for the binary tree.
 */
static int compare_nodes(const void *_a, const void *_b)
{
    const struct file *a = _a;
    const struct file *b = _b;
    int diff = 0;

    if (diff == 0)
        diff = CMP(a->st.st_dev, b->st.st_dev);
    if (diff == 0)
        diff = CMP(a->st.st_size, b->st.st_size);

    return diff;
}

/**
 * print_stats - Print statistics to stdout
 */
static void print_stats(void)
{
    jlog(JLOG_SUMMARY, "Mode:     %s", opts.dry_run ? "dry-run" : "real");
    jlog(JLOG_SUMMARY, "Files:    %zu", stats.files);
    jlog(JLOG_SUMMARY, "Linked:   %zu files", stats.linked);
    jlog(JLOG_SUMMARY, "Compared: %zu files", stats.comparisons);
    jlog(JLOG_SUMMARY, "Saved:    %s", format(stats.saved));
    jlog(JLOG_SUMMARY, "Duration: %.2f seconds", gettime() - stats.start_time);
}

/**
 * handle_interrupt - Handle a signal
 *
 * Returns: %TRUE on SIGINT, SIGTERM; %FALSE on all other signals.
 */
static hl_bool handle_interrupt(void)
{
    switch (last_signal) {
    case SIGINT:
    case SIGTERM:
        return TRUE;
    case SIGUSR1:
        print_stats();
        putchar('\n');
        break;
    }

    last_signal = 0;
    return FALSE;
}

/**
 * file_contents_equal - Compare contents of two files for equality
 * @a: The first file
 * @b: The second file
 *
 * Compare the contents of the files for equality
 */
static hl_bool file_contents_equal(const struct file *a, const struct file *b)
{
    FILE *fa = NULL;
    FILE *fb = NULL;
    char buf_a[8192];
    char buf_b[8192];
    int cmp = 0;                /* zero => equal */
    off_t off = 0;              /* current offset */

    assert(a->links != NULL);
    assert(b->links != NULL);

    jlog(JLOG_DEBUG1, "Comparing %s to %s", a->links->path, b->links->path);

    stats.comparisons++;

    if ((fa = fopen(a->links->path, "rb")) == NULL)
        goto err;
    if ((fb = fopen(b->links->path, "rb")) == NULL)
        goto err;

    posix_fadvise(fileno(fa), 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(fileno(fb), 0, 0, POSIX_FADV_SEQUENTIAL);

    while (!handle_interrupt() && cmp == 0) {
        size_t ca;
        size_t cb;

        ca = fread(buf_a, 1, sizeof(buf_a), fa);
        if (ca < sizeof(buf_a) && ferror(fa))
            goto err;

        cb = fread(buf_b, 1, sizeof(buf_b), fb);
        if (cb < sizeof(buf_b) && ferror(fb))
            goto err;

        off += ca;

        if ((ca != cb || ca == 0)) {
            cmp = CMP(ca, cb);
            break;
        }
        cmp = memcmp(buf_a, buf_b, ca);
    }
  out:
    if (fa != NULL)
        fclose(fa);
    if (fb != NULL)
        fclose(fb);
    return cmp == 0;
  err:
    if (fa == NULL || fb == NULL)
        jlog(JLOG_SYSERR, "Cannot open %s",
             fa ? b->links->path : a->links->path);
    else
        jlog(JLOG_SYSERR, "Cannot read %s",
             ferror(fa) ? a->links->path : b->links->path);
    cmp = 1;
    goto out;
}

/**
 * file_may_link_to - Check whether a file may replace another one
 * @a: The first file
 * @b: The second file
 *
 * Check whether the two fies are considered equal and can be linked
 * together. If the two files are identical, the result will be FALSE,
 * as replacing a link with an identical one is stupid.
 */
static hl_bool file_may_link_to(const struct file *a, const struct file *b)
{
    return (a->st.st_size != 0 &&
            a->st.st_size == b->st.st_size &&
            a->links != NULL && b->links != NULL &&
            a->st.st_dev == b->st.st_dev &&
            a->st.st_ino != b->st.st_ino &&
            (!opts.respect_mode || a->st.st_mode == b->st.st_mode) &&
            (!opts.respect_owner || a->st.st_uid == b->st.st_uid) &&
            (!opts.respect_owner || a->st.st_gid == b->st.st_gid) &&
            (!opts.respect_time || a->st.st_mtime == b->st.st_mtime) &&
            (!opts.respect_name
             || strcmp(a->links->path + a->links->basename,
                       b->links->path + b->links->basename) == 0)
            && file_contents_equal(a, b));
}

/**
 * file_compare - Compare two files to decide which should be master
 * @a: The first file
 * @b: The second file
 *
 * Check which of the files should be considered greater and thus serve
 * as the master when linking (the master is the file that all equal files
 * will be replaced with).
 */
static int file_compare(const struct file *a, const struct file *b)
{
    int res = 0;
    if (a->st.st_dev == b->st.st_dev && a->st.st_ino == b->st.st_ino)
        return 0;

    if (res == 0 && opts.maximise)
        res = CMP(a->st.st_nlink, b->st.st_nlink);
    if (res == 0 && opts.minimise)
        res = CMP(b->st.st_nlink, a->st.st_nlink);
    if (res == 0)
        res = CMP(a->st.st_mtime, b->st.st_mtime);
    if (res == 0)
        res = CMP(b->st.st_ino, a->st.st_ino);

    return res;
}

/**
 * file_link - Replace b with a link to a
 * @a: The first file
 * @b: The second file
 *
 * Link the file, replacing @b with the current one. The file is first
 * linked to a temporary name, and then renamed to the name of @b, making
 * the replace atomic (@b will always exist).
 */
static hl_bool file_link(struct file *a, struct file *b)
{
  file_link:
    assert(a->links != NULL);
    assert(b->links != NULL);

    jlog(JLOG_INFO, "%sLinking %s to %s (-%s)",
         opts.dry_run ? "[DryRun] " : "", a->links->path, b->links->path,
         format(a->st.st_size));

    if (a->st.st_dev == b->st.st_dev && a->st.st_ino == b->st.st_ino) {
        b->slave = TRUE;
        return TRUE;
    }

    if (!opts.dry_run) {
        size_t len = strlen(b->links->path) + strlen(".hardlink-temporary") + 1;
        char *new_path = malloc(len);

        if (new_path == NULL) {
            jlog(JLOG_SYSFAT, "Cannot allocate memory");
            exit(1);
        }

        snprintf(new_path, len, "%s.hardlink-temporary", b->links->path);

        if (link(a->links->path, new_path) != 0) {
            jlog(JLOG_SYSERR, "Cannot link %s to %s", a->links->path, new_path);
            free(new_path);
            return FALSE;
        } else if (rename(new_path, b->links->path) != 0) {
            jlog(JLOG_SYSERR, "Cannot rename %s to %s", a->links->path,
                 new_path);
            unlink(new_path);   /* cleanup failed rename */
            free(new_path);
            return FALSE;
        }
        free(new_path);
    }

    /* Update statistics */
    stats.linked++;

    /* Increase the link count of this file, and set stat() of other file */
    a->st.st_nlink++;
    b->st.st_nlink--;

    if (b->st.st_nlink == 0)
        stats.saved += a->st.st_size;

    /* Move the link from file b to a */
    {
        struct link *new_link = b->links;

        b->links = b->links->next;
        new_link->next = a->links->next;
        a->links->next = new_link;
    }

    // Do it again
    if (b->links)
        goto file_link;

    b->slave = TRUE;

    return TRUE;
}

/**
 * file_free - Free a linked list of files
 * @node: A #struct file
 *
 * Free the file pointed to by @node and then follow the files @next
 * pointer.
 */
static void file_free_chain(void *node)
{
    struct file *file;
    struct link *link;

    while (node != NULL) {
        file = node;
        node = file->next;

        while (file->links != NULL) {
            link = file->links;
            file->links = link->next;

            free(link);
        }

        free(file);
    }
}

/**
 * inserter - Callback function for nftw()
 * @fpath: The path of the file being visited
 * @sb:    The stat information of the file
 * @typeflag: The type flag
 * @ftwbuf:   Contains current level of nesting and offset of basename
 *
 * Called by nftw() for the files. See the manual page for nftw() for
 * further information.
 */
static int inserter(const char *fpath, const struct stat *sb, int typeflag,
                    struct FTW *ftwbuf)
{
    struct file *fil;
    struct file **node;
    size_t pathlen;
    hl_bool included;
    hl_bool excluded;

    if (handle_interrupt())
        return 1;
    if (typeflag == FTW_DNR || typeflag == FTW_NS)
        jlog(JLOG_SYSERR, "Cannot read %s", fpath);
    if (typeflag != FTW_F || !S_ISREG(sb->st_mode))
        return 0;

    included = regexec_any(opts.include, fpath);
    excluded = regexec_any(opts.exclude, fpath);

    if ((opts.exclude && excluded && !included) ||
        (!opts.exclude && opts.include && !included))
        return 0;

    stats.files++;

    if (sb->st_size == 0)
        return 0;

    jlog(JLOG_DEBUG2, "Visiting %s (file %zu)", fpath, stats.files);

    pathlen = strlen(fpath) + 1;

    fil = calloc(1, sizeof(*fil));

    if (fil == NULL)
        return jlog(JLOG_SYSFAT, "Cannot continue"), 1;

    fil->links = calloc(1, sizeof(struct link) + pathlen);

    if (fil->links == NULL)
        return jlog(JLOG_SYSFAT, "Cannot continue"), 1;

    fil->st = *sb;
    fil->links->basename = ftwbuf->base;
    fil->links->next = NULL;

    memcpy(fil->links->path, fpath, pathlen);

    node = tsearch(fil, &files, compare_nodes);

    if (node == NULL)
        return jlog(JLOG_SYSFAT, "Cannot continue"), 1;

    if (*node != fil) {
        assert((*node)->st.st_size == sb->st_size);
        fil->next = *node;
        *node = fil;
    }

    return 0;
}

/**
 * hardlinker - Link common files together
 * @master: The first file in a linked list of candidates for linking
 *
 * Traverse the list pointed to by @master, find the greatest file using
 * @file_compare and link all equal files to it. This function will be
 * called once for each file in the link pointed to by @master.
 */
static int hardlinker(struct file *master)
{
    struct file *other = NULL;
    struct file **others = NULL;
    size_t i = 8, n = 0;

    if (handle_interrupt())
        return 1;
    if (master->slave)
        return 0;

    for (other = master->next; other != NULL; other = other->next) {
        if (handle_interrupt())
            return 1;

        assert(other != other->next);
        assert(other->st.st_size == master->st.st_size);

        if (!file_may_link_to(master, other))
            continue;
        if (i <= n || others == NULL)
            others = realloc(others, (i *= 2) * sizeof(*others));
        if (others == NULL) {
            jlog(JLOG_SYSFAT, "Unable to continue");
            return -1;
        }
        if (file_compare(master, other) < 0) {
            others[n++] = master;
            master = other;
        } else {
            others[n++] = other;
        }
    }

    for (i = 0; !handle_interrupt() && i < n; i++)
        file_link(master, others[i]);

    free(others);

    return handle_interrupt();
}

/**
 * visitor - Callback for twalk()
 * @nodep: Pointer to a pointer to a #struct file
 * @which: At which point this visit is (preorder, postorder, endorder)
 * @depth: The depth of the node in the tree
 *
 * Visit the nodes in the binary tree. For each node, call hardlinker()
 * on each #struct file in the linked list of #struct file instances located
 * at that node.
 */
static void visitor(const void *nodep, const VISIT which, const int depth)
{
    struct file *file = *(struct file **) nodep;

    (void) depth;

    if (which != leaf && which != endorder)
        return;

    for (; file != NULL; file = file->next)
        if (hardlinker(file) != 0)
            exit(1);
}

/**
 * version - Print the program version and exit
 */
static int version(void)
{
    printf("hardlink 0.2\n");
    printf("Compiled %s at %s\n", __DATE__, __TIME__);
    exit(0);
}

/**
 * help - Print the program help and exit
 * @name: The name of the program executable (argv[0])
 */
static int help(const char *name)
{
    printf("Usage: %s [options] directory|file ...\n", name);
    puts("Options:");
    puts("  -V, --version         show program's version number and exit");
    puts("  -h, --help            show this help message and exit");
    puts("  -v, --verbose         Increase verbosity (repeat for more verbosity)");
    puts("  -n, --dry-run         Modify nothing, just print what would happen");
    puts("  -f, --respect-name    Filenames have to be identical");
    puts("  -p, --ignore-mode     Ignore changes of file mode");
    puts("  -o, --ignore-owner    Ignore owner changes");
    puts("  -t, --ignore-time     Ignore timestamps. Will retain the newer timestamp,");
    puts("                        unless -m or -M is given");
    puts("  -m, --maximize        Maximize the hardlink count, remove the file with");
    puts("                        lowest hardlink cout");
    puts("  -M, --minimize        Reverse the meaning of -m");
    puts("  -x REGEXP, --exclude=REGEXP");
    puts("                        Regular expression to exclude files");
    puts("  -i REGEXP, --include=REGEXP");
    puts("                        Regular expression to include files/dirs");
    puts("");
    puts("Compatibility options to Jakub Jelinek's hardlink:");
    puts("  -c                    Compare only file contents, same as -pot");

#ifndef HAVE_GETOPT_LONG
    puts("");
    puts("Your system only supports the short option names given above.");
#endif
    exit(0);
}

/**
 * register_regex - Compile and insert a regular expression into list
 * @pregs: Pointer to a linked list of regular expressions
 * @regex: String containing the regular expression to be compiled
 */
static int register_regex(struct regex_link **pregs, const char *regex)
{
    struct regex_link *link;
    int err;

    link = malloc(sizeof(*link));

    if (link == NULL) {
        jlog(JLOG_SYSFAT, "Cannot allocate memory");
        exit(1);
    }

    if ((err = regcomp(&link->preg, regex, REG_NOSUB | REG_EXTENDED)) != 0) {
        size_t size = regerror(err, &link->preg, NULL, 0);
        char *buf = malloc(size + 1);

        if (buf == NULL) {
            jlog(JLOG_SYSFAT, "Cannot allocate memory");
            exit(1);
        }

        regerror(err, &link->preg, buf, size);

        jlog(JLOG_FATAL, "Could not compile regular expression %s: %s",
             regex, buf);
        free(buf);
        free(link);
        return 1;
    }

    link->next = *pregs;
    *pregs = link;
    return 0;
}

/**
 * parse_options - Parse the command line options
 * @argc: Number of options
 * @argv: Array of options
 */
static int parse_options(int argc, char *argv[])
{
    static const char optstr[] = "VhvnfpotcmMx:i:";
#ifdef HAVE_GETOPT_LONG
    static const struct option long_options[] = {
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {"verbose", no_argument, NULL, 'v'},
        {"dry-run", no_argument, NULL, 'n'},
        {"respect-name", no_argument, NULL, 'f'},
        {"ignore-mode", no_argument, NULL, 'p'},
        {"ignore-owner", no_argument, NULL, 'o'},
        {"ignore-time", no_argument, NULL, 't'},
        {"maximize", no_argument, NULL, 'm'},
        {"minimize", no_argument, NULL, 'M'},
        {"exclude", required_argument, NULL, 'x'},
        {"include", required_argument, NULL, 'i'},
        {NULL, 0, NULL, 0}
    };
#endif

    int opt;

    opts.respect_mode = TRUE;
    opts.respect_owner = TRUE;
    opts.respect_time = TRUE;

    while ((opt = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            opts.respect_mode = FALSE;
            break;
        case 'o':
            opts.respect_owner = FALSE;
            break;
        case 't':
            opts.respect_time = FALSE;
            break;
        case 'm':
            opts.maximise = TRUE;
            break;
        case 'M':
            opts.minimise = TRUE;
            break;
        case 'f':
            opts.respect_name = TRUE;
            break;
        case 'v':
            opts.verbosity++;
            break;
        case 'c':
            opts.respect_mode = FALSE;
            opts.respect_name = FALSE;
            opts.respect_owner = FALSE;
            opts.respect_time = FALSE;
            break;
        case 'n':
            opts.dry_run = 1;
            break;
        case 'h':
            return help(argv[0]);
        case 'V':
            return version();
        case 'x':
            if (register_regex(&opts.exclude, optarg) != 0)
                return 1;
            break;
        case 'i':
            if (register_regex(&opts.include, optarg) != 0)
                return 1;
            break;
        case '?':
            return 1;
        default:
            jlog(JLOG_ERROR, "Unexpected invalid option: -%c\n", opt);
            return 1;
        }
    }
    return 0;
}

/**
 * to_be_called_atexit - Cleanup handler, also prints statistics.
 */
static void to_be_called_atexit(void)
{
    if (stats.started)
        print_stats();

#if defined(__GNU__) || defined(__GLIBC__)
    tdestroy(files, file_free_chain);
#endif

    while (opts.include) {
        struct regex_link *next = opts.include->next;
        regfree(&opts.include->preg);
        free(opts.include);
        opts.include = next;
    }
    while (opts.exclude) {
        struct regex_link *next = opts.exclude->next;
        regfree(&opts.exclude->preg);
        free(opts.exclude);
        opts.exclude = next;
    }
}

/**
 * sighandler - Signal hander, sets the global last_signal variable
 * @i: The signal number
 */
static void sighandler(int i)
{
    if (last_signal != SIGINT)
        last_signal = i;
    if (i == SIGINT)
        putchar('\n');
}

int main(int argc, char *argv[])
{
    struct sigaction sa;

    sa.sa_handler = sighandler;
    sa.sa_flags = SA_RESTART;
    sigfillset(&sa.sa_mask);

    /* If we receive a SIGINT, end the processing */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* Pretty print numeric output */
    setlocale(LC_NUMERIC, "");
    stats.start_time = gettime();

    if (atexit(to_be_called_atexit) != 0) {
        jlog(JLOG_SYSFAT, "Cannot register exit handler");
        return 1;
    }

    if (parse_options(argc, argv) != 0)
        return 1;

    if (optind == argc) {
        jlog(JLOG_FATAL, "Expected file or directory names");
        return 1;
    }

    stats.started = TRUE;

    for (; optind < argc; optind++)
        if (nftw(argv[optind], inserter, 20, FTW_PHYS) == -1)
            jlog(JLOG_SYSERR, "Cannot process %s", argv[optind]);

    twalk(files, visitor);

    return 0;
}
