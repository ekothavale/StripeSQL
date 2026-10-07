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

#ifndef TEST_HEAP_H
#define TEST_HEAP_H

// heap_in_use(), for tests that check an operation doesn't leave memory allocated.
//
// Include this before any other project header. On macOS it needs the allocator's statistics, and the
// header for those defines a PAGE_SIZE of its own, so the name is given back here for const.h to define.

#include <stddef.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#undef PAGE_SIZE
#endif

// AddressSanitizer holds freed memory back instead of reusing it, so under it the heap grows without a leak
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HEAP_HELD_BACK
#endif
#endif

/*
Bytes the program has allocated right now. Only macOS can report it, and not under AddressSanitizer;
otherwise this returns 0, so a test that compares two readings still runs but can't fail on them.
*/
static inline size_t heap_in_use(void) {
#if defined(__APPLE__) && !defined(HEAP_HELD_BACK)
    malloc_statistics_t stats;
    malloc_zone_statistics(NULL, &stats);
    return stats.size_in_use;
#else
    return 0;
#endif
}

#endif // TEST_HEAP_H
