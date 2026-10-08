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

#include "ordering.h"

static uint64_t doubleToBits(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

// flipping the sign bit of a signed 64bit integer maps the integers onto the unsigned ones in order:
// the most negative becomes 0, -1 becomes 2^63 - 1, 0 becomes 2^63 and the most positive becomes 2^64 - 1
#define SIGN_BIT 0x8000000000000000ULL

/*
converts a primary key value into an internal ordering key
ordering keys of one type compare the way their primary keys do (see compareOrderingKeys), so a table's
records are stored, and scanned, in primary key order
floats -> type punned to 64bit integers, sign-bit adjusted for correct unsigned ordering
ints -> casted to unsigned 64bit integers with the sign bit flipped, so that negative keys sort below positive ones
strings -> the string's own bytes, up to TEXT_KEY_MAX_LEN of them, zero-padded
uints -> casted to unsigned 64bit integers
*/
ordering_key pkToOk(value pk) {
	ordering_key out = {0};
	switch (pk.type) {
		// in this SQL implementation it is a compile time error to have
		// a primary key be NULL or boolean typing
		case VAL_FLOAT: {
			uint64_t whole = doubleToBits(pk.as.floating);
			if (whole >> 63) whole = ~whole;
			else whole ^= SIGN_BIT;
			out.as.u64 = whole;
			out.type = ORDERING_DOUBLE;
			break;
		}
		case VAL_INT: {
			uint64_t whole = (uint64_t) pk.as.integer ^ SIGN_BIT;
			out.as.u64 = whole;
			out.type = ORDERING_ULONG;
			break;
		}
		case VAL_TEXT: {
			int len = strlen(pk.as.text);
			if (len > TEXT_KEY_MAX_LEN) {
				printf("Dev Error: string primary key exceeds maximum length of %d\n", TEXT_KEY_MAX_LEN);
				out = (ordering_key){0};
				break;
			}
			out.type = ORDERING_STRING;
			memcpy(out.as.string, pk.as.text, len); // the rest of the array is already zeroed
			break;
		}
		case VAL_U32: {
			uint64_t whole = (uint64_t) pk.as.u32;
			out.as.u64 = whole;
			out.type = ORDERING_ULONG;
			break;
		}
		default: {
			out.as.u64 = (uint64_t) pk.as.integer;
			out.type = ORDERING_ULONG;
			printf("Dev Error: unknown or illegal primary key type given to storage engine\n");
			break;
		}
	}
	return out;
}

/*
the number of bytes a key of this type takes on disk: room for the longest string key, or a number's eight
bytes. Every key in a table has the table's key type, so this is the same for all of a table's keys
*/
uint32_t orderingKeyDiskSize(ordering_type type) {
	return type == ORDERING_STRING ? TEXT_KEY_MAX_LEN : NUMERIC_KEY_DISK_SIZE;
}

/*
compares two string keys byte by byte, as unsigned bytes, so that a string sorts after its own prefixes
and text in UTF-8 sorts by code point
*/
static int txtKeyCmp(char* a, char* b) {
	int cmp = strncmp(a, b, TEXT_KEY_MAX_LEN);
	return cmp < 0 ? -1 : (cmp == 0 ? 0 : 1);
}

/*
return -1 if a < b, 0 if a == b, and 1 if a > b
*/
int compareOrderingKeys(ordering_key a, ordering_key b) {
	if (a.type != b.type) {
		printf("Dev Error: comparing offsets of primary keys of mismatching types\n");
		return 0;
	}
	switch (a.type) {
		case ORDERING_DOUBLE:
		case ORDERING_ULONG:
			return a.as.u64 < b.as.u64 ? -1 : (a.as.u64 == b.as.u64 ? 0 : 1);
		case ORDERING_STRING:
			return txtKeyCmp(a.as.string, b.as.string);
	}
	return 0;
}
