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
These pages use the most basic and inefficient method of compression which is to compress garbage cells on every deletion.
One way to upgrade this is to track which cells are garbage, but not delete them. Then on writes, garbage cells can potentially be overwritten.
As of now, on each disk write the entire page is rewritten each time. This is highly inefficient and in upgraded versions, only specific records
would need to be overwritten.
*/

#include "page.h"
#include "../memory.h"

// ##########################################################################################################################################
// ##########################################################################################################################################
// HELPER FUNCTIONS

/*
the bytes a record takes up in a page on disk: its slot, its entries' data (size bytes in all), and the
6 bytes holding the size and type that are written before each entry
see the code in tableIO.c -> serializePage() to adjust if needed
*/
uint32_t SPRecordBytes(uint32_t size, uint32_t numEntries) {
	return SP_SLOT_DISK_SIZE + 6 * numEntries + size;
}

/*
the bytes all of a page's records take up on disk
*/
uint32_t SPUsedBytes(slotted_page* p) {
	return p->header.numRecords * SP_SLOT_DISK_SIZE + 6 * p->header.numEntries + p->header.usedData;
}

/*
whether a page's records fit in the page on disk. A page only stops fitting if a record in it is made
larger; SPInsert() never lets one in that doesn't fit
*/
bool SPFits(slotted_page* p) {
	return SPUsedBytes(p) <= p->header.arrCap;
}

/*
checks if adding a record to a page would exceed the size limit of the page on disk (causing a write overflow)
*/
bool SPHasRoom(slotted_page* p, uint32_t size, uint32_t numEntries) {
	return SPUsedBytes(p) + SPRecordBytes(size, numEntries) <= p->header.arrCap;
}

/*
binary search slot array
@return index of the slot holding key, or of the slot it would be inserted before if the page doesn't hold it
*/
static uint32_t searchSlotArray(slotted_page* p, ordering_key key) {
	uint32_t hi = p->header.numRecords;
	uint32_t lo = 0;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (compareOrderingKeys(p->slots[mid].ID, key) < 0) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

/*
sets the page's minKey and maxKey from its slot array, which is sorted by key
call after any change to which records the page holds
*/
static void updateKeyBounds(slotted_page* p) {
	if (p->header.numRecords == 0) {
		p->header.minKey = (ordering_key){0};
		p->header.maxKey = (ordering_key){0};
		return;
	}
	p->header.minKey = p->slots[0].ID;
	p->header.maxKey = p->slots[p->header.numRecords - 1].ID;
}

/*
Does not zero out start
*/
static int shiftSlotArrayR(sp_slot* array, int start, int len) {
	if (start > len) {
		printf("Start index %d beyond length %d of array in shiftNodeArrayR\n", start, len);
		return -1;
	}
	for (int i = len-1; i > start; i--) {
		array[i] = array[i-1];
	}
	return 0;
}

/*
Does not zero out last element of array
*/
static int shiftSlotArrayL(sp_slot* array, int target, int len) {
	if (target > len-1) {
		printf("Start index %d beyond length %d of array in shiftPageArrayL\n", target, len);
		return -1;
	}
	for (int i = target; i < len-1; i++) {
		array[i] = array[i+1];
	}
	return 0;
}

/*
makes an empty page. It has no keys of its own until a record is inserted (see header.minKey and maxKey)
@param - numSlots - maximum number of slots for the page to hold
@param - numEntries - maximum number of entries for the page to hold
@param - capacity - maximum capacity of the slot array (including both entries and slots)
Callocs new memory
*/
slotted_page* makeSPage(uint32_t numSlots, uint32_t numEntries, uint32_t capacity) {
	slotted_page* out = calloc(1, sizeof(slotted_page));
	out->header.arrCap = capacity;
	out->header.maxSlots = numSlots;
	out->header.maxEntries = numEntries;
	out->slots = calloc(numSlots, sizeof(sp_slot));
	out->entries = calloc(numEntries, sizeof(entry));
	return out;
}

/*
frees the entries and slots of a slotted page since those are always heap allocated
does not free the page pointer itself
*/
void freeSPage(slotted_page* p) {
	for (int i = 0; i < p->header.numEntries; i++) {
		free(p->entries[i].data);
	}
	free(p->entries);
	free(p->slots);
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// CRUD FUNCTIONS

/*
more complicated version:
	// if page is full:
	// 	if the size of the record plus the size of the slot is less than the page's wasted bytes:
	//	  compact the cells and proceed
	//  else:
	// 	  return false
	// find spot in slot array for record
	// shift slots down and put the slot there
	// iterate through deleted slots
	// if there is a deleted slot that is at least twice the size of the record:
	// 	shift all cells down 1
	// 	put record in that cell
	//	decrease size of deleted slot by the size of that record
	//	... | DELETED | ... -> ... | R | DELETED' | ... size(DELETED) = size(R) + size(DELETED)
	//	if the new size of the deleted cell is 4 bytes or less:
	//	  add the new size of the deleted cell to p->wastedBytes
	//	  remove the slot pointing to the cell
	//	  shift any other deleted slots down
	// else:
	// 	add the record to the end of the next open cell
	// return true
*/

/*
insert into a slotted page
If page is full, insertion will fail
does not check whether the page already holds key; callers do that with SPSearch()
*/
bool SPInsert(slotted_page* p, ordering_key key, sp_record r) {
	if (!SPHasRoom(p, r.size, r.len)) {
		printf("Tried to add record to page but it was full\n");
		return false;
	}
	// if capacity is exceeded, dynamically grow record and slot maximums
	if (p->header.numRecords >= p->header.maxSlots) {
		uint32_t newMax = (uint32_t) p->header.maxSlots * SLOTTED_PAGE_SLOT_GROWTH_RATE + 1;
		p->slots = GROW_ARRAY(sp_slot, p->slots, p->header.maxSlots, newMax);
		memset(p->slots + p->header.maxSlots, 0, (newMax - p->header.maxSlots) * sizeof(sp_slot));
		p->header.maxSlots = newMax;
	}
	if (p->header.numEntries + r.len > p->header.maxEntries) {
		uint32_t newMax = (uint32_t) (p->header.numEntries + r.len) * SLOTTED_PAGE_SLOT_GROWTH_RATE + 1;
		p->entries = GROW_ARRAY(entry, p->entries, p->header.numEntries, newMax);
		memset(p->entries + p->header.maxEntries, 0, (newMax - p->header.maxEntries) * sizeof(entry));
		p->header.maxEntries = newMax;
	}
	uint32_t index = searchSlotArray(p, key);
	shiftSlotArrayR(p->slots, index, p->header.numRecords + 1);
	p->slots[index].ID = key;
	p->slots[index].len = r.len;
	p->slots[index].ptr = p->header.numEntries;
	p->slots[index].size = r.size;
	p->header.numRecords++;
	for (int i = 0; i < r.len; i++) {
		p->entries[p->header.numEntries++] = r.entries[i];
	}
	p->header.usedData += r.size;
	updateKeyBounds(p);
	return true;
}

/*
delete a record from a slotted page
@return true if deletion was successful else return false
*/
bool SPDelete(slotted_page* p, ordering_key key) {
	uint32_t index = searchSlotArray(p, key);
	if (index >= p->header.numRecords || compareOrderingKeys(p->slots[index].ID, key) != 0) return false;
	sp_slot deleted = p->slots[index];
	p->header.usedData -= deleted.size;
	for (uint32_t i = 0; i < deleted.len; i++) {
		free((p->entries[deleted.ptr + i]).data);
		p->entries[deleted.ptr + i].data = NULL;
	}
	for (uint32_t i = deleted.ptr; i < p->header.numEntries - deleted.len; i++) {
		p->entries[i] = p->entries[i + deleted.len];
	}

	// Any slot that pointed after the deleted record must be rebased.
	for (uint32_t i = 0; i < p->header.numRecords; i++) {
		if (i == index) continue;
		if (p->slots[i].ptr > deleted.ptr) {
			p->slots[i].ptr -= deleted.len;
		}
	}

	uint32_t oldNumEntries = p->header.numEntries;
	p->header.numEntries -= deleted.len;

	// Clear trailing duplicate cells left behind by entry compaction.
	for (uint32_t i = p->header.numEntries; i < oldNumEntries; i++) {
		p->entries[i].data = NULL;
		p->entries[i].type = T_INT;
		p->entries[i].size = 0;
	}

	shiftSlotArrayL(p->slots, index, p->header.numRecords);
	p->header.numRecords--;
	updateKeyBounds(p);
	return true;
}

/*
update a record in a slotted page
the record keeps its key, so the page's key bounds don't change
does not check that the page still has room for the record: if it was made larger, check SPFits() afterwards
*/
bool SPUpdate(slotted_page* p, ordering_key key, sp_record r) {
	uint32_t index = searchSlotArray(p, key);
	if (index >= p->header.numRecords) return false;
	sp_slot s = p->slots[index];
	if (compareOrderingKeys(s.ID, key) != 0 || s.len < r.len) return false;
	for (uint32_t i = 0; i < r.len; i++) {
		// the record's size, and the page's count of data in use, follow each entry that changes size
		uint32_t change = r.entries[i].size - p->entries[s.ptr + i].size;
		p->slots[index].size += change;
		p->header.usedData += change;
		free(p->entries[s.ptr + i].data);
		p->entries[s.ptr + i] = r.entries[i];
	}
	return true;
}

/*
@return sp_record pointing into the page's entry array
@return {NULL, 0} if record not in page
*/
sp_record SPRead(slotted_page* p, ordering_key key) {
	uint32_t index = searchSlotArray(p, key);
	if (index >= p->header.numRecords) {
		sp_record out = {NULL, 0};
		return out;
	}
	sp_slot s = p->slots[index];
	if (compareOrderingKeys(s.ID, key) != 0) {
		sp_record out = {NULL, 0};
		return out;
	}
	sp_record out = {p->entries + s.ptr, s.len};
	return out;
}

/*
@return slotIndex if record in page
@return -1 if record not in page
*/
int SPSearch(slotted_page* p, ordering_key key) {
	uint32_t index = searchSlotArray(p, key);
	if (index >= p->header.numRecords) return -1;
	sp_slot s = p->slots[index];
	if (compareOrderingKeys(s.ID, key) != 0) return -1;
	return index;
}

/*
@return the index among the page's slots that key's record is at, or would be inserted at
*/
uint32_t SPPosition(slotted_page* p, ordering_key key) {
	return searchSlotArray(p, key);
}

/*
moves the records at index and after it, in key order, out of p and into upper
upper must be an empty page. index can be anything from 0, which moves every record, to p's number of
records, which moves none
the records' entries move with them, so no entry data is copied or freed
*/
void SPSplit(slotted_page* p, slotted_page* upper, uint32_t index) {
	uint32_t total = p->header.numRecords;
	if (index > total) index = total;
	// make room in upper for what it's about to hold
	uint32_t movedSlots = total - index;
	uint32_t movedEntries = 0;
	for (uint32_t i = index; i < total; i++) movedEntries += p->slots[i].len;
	if (movedSlots > upper->header.maxSlots) {
		upper->slots = GROW_ARRAY(sp_slot, upper->slots, upper->header.maxSlots, movedSlots);
		memset(upper->slots + upper->header.maxSlots, 0, (movedSlots - upper->header.maxSlots) * sizeof(sp_slot));
		upper->header.maxSlots = movedSlots;
	}
	if (movedEntries > upper->header.maxEntries) {
		upper->entries = GROW_ARRAY(entry, upper->entries, upper->header.maxEntries, movedEntries);
		memset(upper->entries + upper->header.maxEntries, 0, (movedEntries - upper->header.maxEntries) * sizeof(entry));
		upper->header.maxEntries = movedEntries;
	}
	// a page's entries sit in the order their records were inserted, so the ones that stay are gathered
	// into a new array in key order, and each record that moves has its entries handed to upper
	entry* kept = calloc(p->header.maxEntries, sizeof(entry));
	uint32_t keptEntries = 0;
	uint32_t keptData = 0;
	for (uint32_t i = 0; i < total; i++) {
		sp_slot* s = &p->slots[i];
		if (i < index) {
			memcpy(kept + keptEntries, p->entries + s->ptr, s->len * sizeof(entry));
			s->ptr = keptEntries;
			keptEntries += s->len;
			keptData += s->size;
		} else {
			sp_slot* moved = &upper->slots[upper->header.numRecords++];
			*moved = *s;
			moved->ptr = upper->header.numEntries;
			memcpy(upper->entries + upper->header.numEntries, p->entries + s->ptr, s->len * sizeof(entry));
			upper->header.numEntries += s->len;
			upper->header.usedData += s->size;
			*s = (sp_slot){0};
		}
	}
	free(p->entries);
	p->entries = kept;
	p->header.numRecords = index;
	p->header.numEntries = keptEntries;
	p->header.usedData = keptData;
	updateKeyBounds(p);
	updateKeyBounds(upper);
}
