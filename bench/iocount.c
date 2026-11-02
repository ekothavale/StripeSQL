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
iocount: a library injected into StripeSQL with DYLD_INSERT_LIBRARIES (macOS only) that counts the
file calls StripeSQL makes through the C library, and the bytes it reads and writes, and prints the
totals to stderr when the process exits (see bench/profile.py --io)
*/

#include <stdio.h>
#include <unistd.h>

static long nSeek, nRead, nWrite, nOpen, nClose, nSync, bytesRead, bytesWritten;

static int spy_fseek(FILE* f, long offset, int whence) {
    nSeek++;
    return fseek(f, offset, whence);
}

static size_t spy_fread(void* p, size_t size, size_t n, FILE* f) {
    nRead++;
    size_t got = fread(p, size, n, f);
    bytesRead += got * size;
    return got;
}

static size_t spy_fwrite(const void* p, size_t size, size_t n, FILE* f) {
    nWrite++;
    size_t put = fwrite(p, size, n, f);
    bytesWritten += put * size;
    return put;
}

static FILE* spy_fopen(const char* path, const char* mode) {
    nOpen++;
    return fopen(path, mode);
}

static int spy_fclose(FILE* f) {
    nClose++;
    return fclose(f);
}

static int spy_fsync(int fd) {
    nSync++;
    return fsync(fd);
}

__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "[iocount] fseek=%ld fread=%ld fwrite=%ld fopen=%ld fclose=%ld fsync=%ld bytesRead=%ld bytesWritten=%ld\n",
            nSeek, nRead, nWrite, nOpen, nClose, nSync, bytesRead, bytesWritten);
}

typedef struct { const void* replacement; const void* replacee; } interpose_t;
__attribute__((used)) static const interpose_t interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void*)spy_fseek, (const void*)fseek },
    { (const void*)spy_fread, (const void*)fread },
    { (const void*)spy_fwrite, (const void*)fwrite },
    { (const void*)spy_fopen, (const void*)fopen },
    { (const void*)spy_fclose, (const void*)fclose },
    { (const void*)spy_fsync, (const void*)fsync },
};
