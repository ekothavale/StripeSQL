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
This file defines a bunch of constructs used to represent a slotted page in memory.
This representation is not the same used to store pages on disk; the translation algorithms are in tableIO.
*/

#ifndef PAGE_H
#define PAGE_H

#include "../common.h"
#include "ordering.h"

#define SLOTTED_PAGE_SLOT_GROWTH_RATE 1.5
// On-disk size of one sp_slot: ID (ORDERING_KEY_DISK_SIZE) + ptr(4) + len(4) + size(4)
#define SP_SLOT_DISK_SIZE (ORDERING_KEY_DISK_SIZE + 12)

typedef enum datatype {
	T_INT,
	T_STRING,
	T_DATE,
	T_TIME
}datatype;

/*
each record is a row of data in a table
each element of the record is proceeded by an escape character representing its c type
\i - int
\s - string
\d - date
\t = time
*/ 

static const char DATATYPE_CODES[] = {'i', 's', 'd', 't'};
#define NUM_DATATYPES 4

typedef struct header {
	// the smallest and largest keys among the page's records, kept up to date by SPInsert() and SPDelete()
	// the largest is also the key the b+ tree files the page under
	// both are zeroed while the page holds no records, so check numRecords before relying on them
	ordering_key minKey;
	ordering_key maxKey;
	uint32_t usedData; // total amount of data used by records
	uint32_t numRecords; // number of records in array at a time
	uint32_t numEntries; // number of entries in the the page, this should be a constant multiple of numRecords since row size is constant across a tbale
	uint32_t arrCap; // total size in bytes of slot array including both slots and records
	uint32_t maxEntries; // max number of entries in the page
	uint32_t maxSlots; // max number of slots in the page
}header;

typedef struct entry {
	char* data;
	uint32_t size; // size of entry data in bytes (NEEDS TO BE ADDED TO TESTING FUNCTIONS)
	datatype type;
}entry;

/* sp_record / sp_slot / slotted_page use the "sp_" prefix to avoid
   colliding with the identically-named types in types.h. */
typedef struct sp_record {
	entry* entries;
	uint32_t len; // number of entries in record (should be fixed for each table)
	uint32_t size; // size in bytes of record
}sp_record;

typedef struct sp_slot {
	ordering_key ID; // the record's full ordering key; the slot array is kept sorted by it
	uint32_t ptr; // index into the entries array where this record begins
	uint32_t len; // number of entries belonging to this record
	uint32_t size; // size in bytes of the corresponding record
}sp_slot;

/*
arr is the slot array and data
from 0 -> are the slot indexes that store the offset from the end of the corresponding record
from  <- len(arr) are the records
*/
typedef struct slotted_page {
	header header;
	entry* entries;
	sp_slot* slots;
}slotted_page;

slotted_page* makeSPage(uint32_t numSlots, uint32_t numEntries, uint32_t capacity);
void freeSPage(slotted_page* p);

bool SPInsert(slotted_page* p, ordering_key key, sp_record r);
bool SPDelete(slotted_page* p, ordering_key key);
bool SPUpdate(slotted_page* p, ordering_key key, sp_record r);
int SPSearch(slotted_page* p, ordering_key key);
sp_record SPRead(slotted_page* p, ordering_key key);

// how full a page is, measured in the bytes its records take up on disk (it is full at header.arrCap)
uint32_t SPRecordBytes(uint32_t size, uint32_t numEntries);
uint32_t SPUsedBytes(slotted_page* p);
bool SPFits(slotted_page* p);
bool SPHasRoom(slotted_page* p, uint32_t size, uint32_t numEntries);

// splitting a page that has filled up
uint32_t SPPosition(slotted_page* p, ordering_key key);
void SPSplit(slotted_page* p, slotted_page* upper, uint32_t index);

#endif