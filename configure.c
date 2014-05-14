/* check.c - Build-time checks
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

#if TEST_GETOPT_LONG          /* Check for getopt_long() */

#include <getopt.h>

static struct option options[] = {
    {0, 0, 0, 0}
};

int main(int argc, char *argv[]) {
    return getopt_long(argc, argv, "", options, 0);
}

#elif TEST_POSIX_FADVISE

#include <fcntl.h>

int main(void)
{
    posix_fadvise(-1, 0, 0, POSIX_FADV_DONTNEED);
    return posix_fadvise(-1, 0, 0, POSIX_FADV_SEQUENTIAL);
}

#elif TEST_libpcreposix

#include <pcreposix.h>

int main(void)
{
    regex_t preg;
    regcomp(&preg, "regex", 0);
}

#elif TEST_XATTR

#include <sys/xattr.h>

int main(void)
{
    llistxattr(0, 0, 0);
}

#else

#error "Invalid feature"

#endif
