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

#ifndef ORDERING_H
#define ORDERING_H

#include "../common.h"
#include "value.h"

#define TEXT_KEY_MAX_LEN 24 // maximum size in bytes of a string ordering key

// On-disk size of an ordering key, the same for every key type: 1 byte type + TEXT_KEY_MAX_LEN bytes of data
#define ORDERING_KEY_DISK_SIZE (1 + TEXT_KEY_MAX_LEN)

typedef enum {
	ORDERING_ULONG,
	ORDERING_STRING,
	ORDERING_DOUBLE
} ordering_type;

typedef struct {
	ordering_type type;
	union {
		uint64_t u64;
		char string[TEXT_KEY_MAX_LEN + 1];
	} as;
} ordering_key;

ordering_key pkToOk(value pk);
int compareOrderingKeys(ordering_key a, ordering_key b);

#endif // ORDERING_H
