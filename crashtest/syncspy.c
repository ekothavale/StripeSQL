/*
Copyright (c) 2026 Ethan Kothavale

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
syncspy: a library injected into StripeSQL with DYLD_INSERT_LIBRARIES (macOS only) that counts
the process's file writes and syncs, and can kill it just before one of them.

Environment variables:
 SYNCSPY_CRASH_AT=N  the process dies (_exit(137)) just before its Nth file write or sync
 SYNCSPY_FAIL=1      every fsync fails with EIO (to exercise error handling)
At a normal exit it prints "[syncspy] fsync=.. F_FULLFSYNC=.. writes=.." to stderr.

Only writes to regular files count, so output to a terminal or pipe isn't a crash point.
stdio writes go through write$NOCANCEL rather than write, so both are intercepted.
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int nFsync, nFull, nWrite, nEvents;
static long crashAt = -1;

// called before every counted write or sync; dies if this is the one to crash at
static void event(void) {
    if (crashAt < 0) {
        const char* s = getenv("SYNCSPY_CRASH_AT");
        crashAt = s ? atol(s) : 0;
    }
    if (crashAt > 0 && ++nEvents == crashAt) _exit(137);
}

static int isRegular(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

static int spy_fsync(int fd) {
    event();
    nFsync++;
    if (getenv("SYNCSPY_FAIL")) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int spy_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (cmd == F_FULLFSYNC) {
        event();
        nFull++;
    }
    return fcntl(fd, cmd, arg);
}

static ssize_t spy_write(int fd, const void* buf, size_t n) {
    if (isRegular(fd)) {
        event();
        nWrite++;
    }
    return write(fd, buf, n);
}

extern ssize_t write_nocancel(int, const void*, size_t) __asm("_write$NOCANCEL");
static ssize_t spy_write_nocancel(int fd, const void* buf, size_t n) {
    if (isRegular(fd)) {
        event();
        nWrite++;
    }
    return write_nocancel(fd, buf, n);
}

__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "[syncspy] fsync=%d F_FULLFSYNC=%d writes=%d\n", nFsync, nFull, nWrite);
}

typedef struct { const void* replacement; const void* replacee; } interpose_t;
__attribute__((used)) static const interpose_t interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void*)spy_fsync, (const void*)fsync },
    { (const void*)spy_fcntl, (const void*)fcntl },
    { (const void*)spy_write, (const void*)write },
    { (const void*)spy_write_nocancel, (const void*)write_nocancel },
};
