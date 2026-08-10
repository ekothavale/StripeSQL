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

/*
reverses byte order of the first len bytes of buf in place.
*/
static void reverseNBytes(char* buf, int len) {
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        char tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
    }
}

static uint64_t reverseBits64(uint64_t x) {
    x = (x >> 32) | (x << 32);
    x = ((x & 0xFFFF0000FFFF0000ULL) >> 16) | ((x & 0x0000FFFF0000FFFFULL) << 16);
    x = ((x & 0xFF00FF00FF00FF00ULL) >>  8) | ((x & 0x00FF00FF00FF00FFULL) <<  8);
    x = ((x & 0xF0F0F0F0F0F0F0F0ULL) >>  4) | ((x & 0x0F0F0F0F0F0F0F0FULL) <<  4);
    x = ((x & 0xCCCCCCCCCCCCCCCCULL) >>  2) | ((x & 0x3333333333333333ULL) <<  2);
    x = ((x & 0xAAAAAAAAAAAAAAAAULL) >>  1) | ((x & 0x5555555555555555ULL) <<  1);
    return x;
}

static page_offset numericalOffset(uint64_t key, ordering_type type) {
	return (page_offset){ .type = type, .as.u64 = key & ((1ULL << OFFSET_BITS) - 1) };
}

static page_num numericalPageNum(uint64_t key, ordering_type type) {
	return (page_num){ .type = type, .as.u64 = key >> OFFSET_BITS };
}

/*
converts a primary key value into an internal ordering key
floats -> type punned to 64bit integers, sign-bit adjusted for correct unsigned ordering
ints -> casted to unsigned 64bit integers (-1 -> 2^64 - 1) and bit inverted for dispersion
strings -> extended if necessary to TEXT_KEY_LENGTH_MINIMUM bytes using STX padding, then byte-reversed for dispersion (see reverseKeyBytes)
uints -> casted to unsigned 64bit integers
callocs pageNum and offset for text primary keys
*/
ordering_key pkToOk(value pk) {
	ordering_key out;
	switch (pk.type) {
		// in this SQL implementation it is a compile time error to have
		// a primary key be NULL or boolean typing
		case VAL_FLOAT: {
			uint64_t whole = doubleToBits(pk.as.floating);
			if (whole >> 63) whole = ~whole;
			else whole ^= 0x8000000000000000ULL;
			out.offset  = numericalOffset(whole, ORDERING_DOUBLE);
			out.pageNum = numericalPageNum(whole, ORDERING_DOUBLE);
			break;
		}
		case VAL_INT: {
			uint64_t whole = reverseBits64((uint64_t) pk.as.integer);
			out.offset  = numericalOffset(whole, ORDERING_ULONG);
			out.pageNum = numericalPageNum(whole, ORDERING_ULONG);
			break;
		}
		case VAL_TEXT: {
			int len = strlen(pk.as.text);
			if (len > TEXT_KEY_MAX_LEN) {
				printf("Dev Error: string primary key exceeds maximum length of %d\n", TEXT_KEY_MAX_LEN);
				out = (ordering_key){0};
				break;
			}
			char key[TEXT_KEY_MAX_LEN + 1] = {0};
			int paddedLen = (len >= TEXT_KEY_LENGTH_MINIMUM) ? len : TEXT_KEY_LENGTH_MINIMUM;
			if (len < TEXT_KEY_LENGTH_MINIMUM) {
				for (int i = 0; i < TEXT_KEY_LENGTH_MINIMUM - len; i++) {
					key[i] = 2; // STX padding
				}
				strncpy(key + TEXT_KEY_LENGTH_MINIMUM - len, pk.as.text, len);
			} else {
				strncpy(key, pk.as.text, len);
			}
			reverseKeyBytes(key, paddedLen);
			out.pageNum.type = ORDERING_STRING;
			memset(out.pageNum.as.string, 0, sizeof(out.pageNum.as.string));
			strncpy(out.pageNum.as.string, key, paddedLen - OFFSET_BITS);
			out.offset.type = ORDERING_STRING;
			memset(out.offset.as.string, 0, sizeof(out.offset.as.string));
			strncpy(out.offset.as.string, key + paddedLen - OFFSET_BITS, OFFSET_BITS);
			break;
		}
		case VAL_U32: {
			uint64_t whole = (uint64_t) pk.as.u32;
			out.offset  = numericalOffset(whole, ORDERING_ULONG);
			out.pageNum = numericalPageNum(whole, ORDERING_ULONG);
			break;
		}
		default: {
			out.offset  = (page_offset){ .type = ORDERING_ULONG, .as.u64 = 0 };
			out.pageNum = (page_num){    .type = ORDERING_ULONG, .as.u64 = 0 };
			printf("Dev Error: unknown or illegal primary key type given to storage engine\n");
			break;
		}
	}
	return out;
}

static int txtKeyCmp(char* a, char* b) {
	int i = -1;
	while (true) {
		i++;
		char charA = a[i];
		char charB = b[i];
		if (charA == 0) {
			if (charB == 0) return 0;
			return -1;
		}
		if (charB == 0) return 1;
		if (charA == charB) continue;
		if (charA < charB || charA == 2) return -1;
		return 1;
	}
}

/*
return -1 if a < b, 0 if a == b, and 1 if a > b
*/
int comparePageNums(page_num a, page_num b) {
	if (a.type != b.type) {
		printf("Dev Error: comparing page nums of primary keys of mismatching types\n");
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

int compareOffsets(page_offset a, page_offset b) {
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
