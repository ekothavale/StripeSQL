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
General-purpose file and byte-encoding helpers that aren't tied to any one file format
*/

#include "file.h"
#include "../const.h"
#include <fcntl.h>
#include <unistd.h>

// ##########################################################################################################################################
// ##########################################################################################################################################
// DURABILITY

/*
flushes a stream's buffered writes and forces them to stable storage
fails if the flush or sync fails, or if any write since the stream's error indicator was last cleared failed
FULL_FSYNC (see const.h) selects F_FULLFSYNC on platforms that have it
*/
bool syncFile(FILE* file) {
	if (fflush(file) != 0 || ferror(file)) return false;
#if FULL_FSYNC && defined(F_FULLFSYNC)
	return fcntl(fileno(file), F_FULLFSYNC) != -1;
#else
	return fsync(fileno(file)) == 0;
#endif
}

/*
syncs a directory so that files created in or removed from it survive a crash
syncing a file only makes its contents durable, not the directory entry that names it
*/
bool syncDirectory(const char* path) {
	int fd = open(path, O_RDONLY);
	if (fd == -1) return false;
#if FULL_FSYNC && defined(F_FULLFSYNC)
	bool synced = fcntl(fd, F_FULLFSYNC) != -1;
#else
	bool synced = fsync(fd) == 0;
#endif
	close(fd);
	return synced;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// BYTE ENCODING

/*
UNSAFE FUNCTION - assumes there's enough space in the array for the long
big endian
*/
void writeULongBytewise(char* arr, uint64_t lui) {
	for (int i = 7; i >= 0; i--) {
		*(arr+i) = lui & 0xFF;
		lui >>= 8;
	}
}

void writeUIntBytewise(char* arr, uint32_t ui) {
	for (int i = 3; i >= 0; i--) {
		*(arr+i) = ui & 0xFF;
		ui >>= 8;
	}
}

void writeUShortBytewise(char* arr, uint16_t us) {
	arr[0] = (us >> 8) & 0xFF;
	arr[1] = us & 0xFF;
}

/*
inverses of the write*Bytewise functions: decode big-endian integers from a buffer
*/
uint64_t readULongBytewise(const char* arr) {
	const unsigned char* a = (const unsigned char*)arr;
	uint64_t out = 0;
	for (int i = 0; i < 8; i++) out = (out << 8) | a[i];
	return out;
}

uint32_t readUIntBytewise(const char* arr) {
	const unsigned char* a = (const unsigned char*)arr;
	return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | (uint32_t)a[3];
}

uint16_t readUShortBytewise(const char* arr) {
	const unsigned char* a = (const unsigned char*)arr;
	return (uint16_t)(a[0] << 8 | a[1]);
}
