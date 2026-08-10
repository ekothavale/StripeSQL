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
API for reading and writing with database tables

Layout of a table file:

 FACE3419 | Metadata Length | Num page rows | Num node rows | Page rows per node row |
 Pages per row | Nodes per row | Page size | Node size | Pointer to free space | Root pointer |
 M |

Addressing:

 All nodes and pages are referenced by the distance of their first byte from the start of the file
 Represented by an unsigned 64-bit integer
*/

/*
TODO:
 - MarkDelete
 - Garbage collection
 - Change array shifting to address
*/

#include "tableIO.h"
#include "../memory.h"
#include <stdbool.h>
#include <sys/stat.h>

#define ADDR_TABLE_MAX_LOAD_FACTOR 0.8

// ##########################################################################################################################################
// ##########################################################################################################################################
// STATIC HELPER FUNCTIONS


/*
jumps to a specific address in the table's source
*/
static bool jump(address address, table* t) {
	if (fseek(t->source, address, SEEK_SET) == 0) {
		t->cursor = address;
		return true;
	} else {
		printf("Error: failed to navigate to address: %llu when reading a binary file\n", address);
		return false;
	}
}

/*
jumps to an offset relative to the cursor's current position in the table's source
*/
static bool jumpRel(long offset, table* t) {
	if (fseek(t->source, offset, SEEK_CUR) == 0) {
		t->cursor += offset;
		return true;
	} else {
		printf("Error: failed to make relative jump to address: %llu when reading a binary file\n", t->cursor);
		return false;
	}
}


/*
UNSAFE FUNCTION - assumes there's enough space in the array for the long
big endian
*/
static void writeULongBytewise(char* arr, uint64_t lui) {
	for (int i = 7; i >= 0; i--) {
		*(arr+i) = lui & 0xFF;
		lui >>= 8;
	}
}

static void writeUIntBytewise(char* arr, uint32_t ui) {
	for (int i = 3; i >= 0; i--) {
		*(arr+i) = ui & 0xFF;
		ui >>= 8;
	}
}

/*
Serializes a page_num into exactly PAGE_NUM_DISK_SIZE bytes at buf.
Layout: [type(1B)][data(18B)] where data is big-endian u64 zero-padded, or raw string bytes.
*/
static void writePageNumBytewise(char* buf, page_num k) {
	buf[0] = (char)k.type;
	if (k.type == ORDERING_STRING) {
		memset(buf + 1, 0, PAGE_NUM_DISK_SIZE - 1);
		memcpy(buf + 1, k.as.string, TEXT_PAGE_NUM_LEN);
	} else {
		writeULongBytewise(buf + 1, k.as.u64);
		memset(buf + 9, 0, PAGE_NUM_DISK_SIZE - 9);
	}
}

/*
Serializes a page_offset into exactly PAGE_OFFSET_DISK_SIZE bytes at buf.
Layout: [type(1B)][data(8B)] where data is big-endian u64, or raw string bytes zero-padded.
*/
static void writePageOffsetBytewise(char* buf, page_offset k) {
	buf[0] = (char)k.type;
	if (k.type == ORDERING_STRING) {
		memset(buf + 1, 0, PAGE_OFFSET_DISK_SIZE - 1);
		memcpy(buf + 1, k.as.string, OFFSET_BITS);
	} else {
		writeULongBytewise(buf + 1, k.as.u64);
	}
}

/*
Reads a page_num from (cursor + offset), restores cursor.
*/
static page_num readPageNum(long offset, table* t) {
	unsigned char buf[PAGE_NUM_DISK_SIZE];
	jumpRel(offset, t);
	fread(buf, 1, PAGE_NUM_DISK_SIZE, t->source);
	jumpRel(-(long)offset - PAGE_NUM_DISK_SIZE, t);
	page_num out;
	out.type = (ordering_type)buf[0];
	if (out.type == ORDERING_STRING) {
		memset(out.as.string, 0, sizeof(out.as.string));
		memcpy(out.as.string, buf + 1, TEXT_PAGE_NUM_LEN);
	} else {
		out.as.u64 = ((uint64_t)buf[1] << 56) | ((uint64_t)buf[2] << 48) |
		             ((uint64_t)buf[3] << 40) | ((uint64_t)buf[4] << 32) |
		             ((uint64_t)buf[5] << 24) | ((uint64_t)buf[6] << 16) |
		             ((uint64_t)buf[7] <<  8) | (uint64_t)buf[8];
	}
	return out;
}

/*
Reads a page_offset from (cursor + offset), restores cursor.
*/
static page_offset readPageOffset(long offset, table* t) {
	unsigned char buf[PAGE_OFFSET_DISK_SIZE];
	jumpRel(offset, t);
	fread(buf, 1, PAGE_OFFSET_DISK_SIZE, t->source);
	jumpRel(-(long)offset - PAGE_OFFSET_DISK_SIZE, t);
	page_offset out;
	out.type = (ordering_type)buf[0];
	if (out.type == ORDERING_STRING) {
		memset(out.as.string, 0, sizeof(out.as.string));
		memcpy(out.as.string, buf + 1, OFFSET_BITS);
	} else {
		out.as.u64 = ((uint64_t)buf[1] << 56) | ((uint64_t)buf[2] << 48) |
		             ((uint64_t)buf[3] << 40) | ((uint64_t)buf[4] << 32) |
		             ((uint64_t)buf[5] << 24) | ((uint64_t)buf[6] << 16) |
		             ((uint64_t)buf[7] <<  8) | (uint64_t)buf[8];
	}
	return out;
}

/*
reads one byte at the table's cursor + an offset
returns the cursor to the original position
*/
static char readByte(long offset, table* t) {
	char a;
	jumpRel(offset, t);
	fread(&a, 1, 1, t->source);
	jumpRel(-offset - 1, t);
	return a;
}

/*
reads an unsigned integer at the table's cursor + an offset
returns the cursor to the original position
*/
static uint32_t readUInt(long offset, table* t) {
	unsigned char a[4];
	jumpRel(offset, t);
	fread(a, 4, 1, t->source);
	jumpRel(-offset - 4, t);
	uint32_t out = (uint32_t) a[0] << 24 | (uint32_t) a[1] << 16 | (uint32_t) a[2] << 8 | (uint32_t) a[3];
	return out;
}

/*
reads an unsigned long at the table's cursor + an offset
returns the cursor to the original position
*/
static uint64_t readULong(long offset, table* t) {
	unsigned char a[8];
	jumpRel(offset, t);
	fread(a, 8, 1, t->source);
	jumpRel(-offset - 8, t);
	uint64_t out = (uint64_t) a[0] << 56 | (uint64_t) a[1] << 48 | (uint64_t) a[2] << 40 | (uint64_t) a[3] << 32
					| (uint64_t) a[4] << 24 | (uint64_t) a[5] << 16 | (uint64_t) a[6] << 8 | (uint64_t) a[7];
	return out;
}

/*
reads an arbitrary number of bytes from the source file into the buffer
returns the cursor to the original position
*/
static void readArbitrary(char* buffer, uint32_t len, long offset, table* t) {
	jumpRel(offset, t);
	fread(buffer, 1, len, t->source);
	jumpRel(-offset-len, t);
}

/*
reads one byte at the table's cursor + an offset
does not return the cursor to the original position
*/
static char consumeByte(long offset, table* t) {
	char a;
	jumpRel(offset, t);
	fread(&a, 1, 1, t->source);
	return a;
}

/*
reads an unsigned integer at the table's cursor + an offset
does not return the cursor to the original position
*/
static uint32_t consumeUInt(long offset, table* t) {
	uint32_t a;
	jumpRel(offset, t);
	fread(&a, 4, 1, t->source);
	return a;
}

/*
reads an unsigned long at the table's cursor + an offset
does not return the cursor to the original position
*/
static uint64_t consumeULong(long offset, table* t) {
	uint64_t a;
	jumpRel(offset, t);
	fread(&a, 8, 1, t->source);
	return a;
}

/*
reads an arbitrary number of bytes from the source file into the buffer
does not return the cursor to the original position
*/
static void consumeArbitrary(char* buffer, uint32_t len, long offset, table* t) {
	jumpRel(offset, t);
	fread(buffer, 1, len, t->source);
}

/*
deep copies the contents of the source page into the target page
*/
static void copyPage(slotted_page* source, slotted_page* target) {
	target->header = source->header;
	target->entries = calloc(source->header.maxEntries, sizeof(entry));
	for (int i = 0; i < source->header.numEntries; i++) {
		entry* tgt = target->entries + i;
		entry* src = source->entries + i;
		*tgt = *src;
		tgt->data = malloc(src->size);
		memcpy(tgt->data, src->data, src->size);
	}
	target->slots = calloc(source->header.maxSlots, sizeof(sp_slot));
	memcpy(target->slots, source->slots, source->header.numRecords * sizeof(sp_slot));
}

/*
deep copies the contents of the source node into the target node
*/
static void copyNode(node* source, node* target) {
	*target = *source;
}

static bool validateTableFile(FILE* file, char* fname) {
	// Check file extension
	char* ext = strrchr(fname, '.');
	if (!ext || strcmp(ext, TABLE_EXTENSION) != 0) {
		printf("Error: '%s' does not have the %s extension\n", fname, TABLE_EXTENSION);
		return false;
	}

	// Check magic number (writeMeta writes MAGIC as a native-endian uint32_t)
	uint32_t magic;
	rewind(file);
	if (fread(&magic, 4, 1, file) != 1) {
		printf("Error: could not read magic number from '%s'\n", fname);
		rewind(file);
		return false;
	}
	rewind(file);

	if (magic != MAGIC) {
		printf("Error: '%s' has magic number 0x%X, expected 0x%X\n", fname, magic, MAGIC);
		return false;
	}

	return true;
}

/*
Loads the metadata from a file into a table
Assumes the file has been validated as a correct table file
*/
static bool loadMeta(FILE* file, char* fname, table* table) {
	uint32_t buf[METALEN];
	fread(&buf, 4, METALEN, file);
	table->source = file;
	table->cursor = 0;
	table->metalen = buf[1];
	table->pageStripes = buf[2];
	table->nodeStripes = buf[3];
	table->pageNodeRatio = buf[4];
	table->pageStripeLen = buf[5];
	table->nodeStripeLen = buf[6];
	table->pageSize = buf[7];
	table->nodeSize = buf[8];
	table->pageFree = ((uint64_t) (uint32_t) buf[9] << 32) | (uint32_t) buf[10];
	table->nodeFree = ((uint64_t) (uint32_t) buf[11] << 32) | (uint32_t) buf[12];
	table->root    = ((uint64_t) (uint32_t) buf[13] << 32) | (uint32_t) buf[14];
	table->M = buf[15];
	return true;
}

bool writeMeta(FILE* file, table* t) {
	uint32_t buf[] = {
		MAGIC,
		t->metalen,
		t->pageStripes,
		t->nodeStripes,
		t->pageNodeRatio,
		t->pageStripeLen,
		t->nodeStripeLen,
		t->pageSize,
		t->nodeSize,
		(uint32_t) (t->pageFree >> 32),
		(uint32_t) (t->pageFree & 0xFFFFFFFF),
		(uint32_t) (t->nodeFree >> 32),
		(uint32_t) (t->nodeFree & 0xFFFFFFFF),
		(uint32_t) (t->root >> 32),
		(uint32_t) (t->root & 0xFFFFFFFF),
		t->M
	};
	jump(0, t);
	fwrite(buf, 4, METALEN, file);
	return true;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Dirty Hash Table Functions

/*
initializes a table's dirty-write hash tables
*/
static void setStacks(table* t) {
	initAddrTable(&t->pageDirty);
	initAddrTable(&t->nodeDirty);
	initAddrTable(&t->delete);
}

static void freeStacks(table* t) {
	freeAddrTable(&t->pageDirty);
	freeAddrTable(&t->nodeDirty);
	freeAddrTable(&t->delete);
}

/*
FNV-1a hash function over the 8 bytes of an address
*/
static uint64_t hashAddress(address key) {
	uint64_t hash = 14695981039346656037ULL;
	for (int i = 0; i < 8; i++) {
		hash ^= (key >> (i * 8)) & 0xFF;
		hash *= 1099511628211ULL;
	}
	return hash;
}

/*
finds an entry in the given entries array for the given key
resolves collisions via linear probing
compares the actual key (not just its hash) so hash collisions can't misidentify a match
returns the matching entry if key is present, else the first empty slot found
*/
static addr_entry* findAddrEntry(address key, addr_entry* entries, int capacity) {
	uint64_t hash = hashAddress(key);
	for (uint64_t i = 0; i < (uint64_t)capacity; i++) {
		uint64_t index = (hash + i) % capacity;
		addr_entry* found = &entries[index];
		if (found->key == key || found->key == 0) return found;
	}
	return NULL; // unreachable: capacity always exceeds count, so an empty slot always exists
}

/*
resize the given hash table by copying the data to a new table by rehashing each entry
frees the original entries array, callocs a new one
*/
static void adjustAddrCapacity(addr_table* at, int capacity) {
	addr_entry* entries = calloc(capacity, sizeof(addr_entry));
	for (int i = 0; i < at->capacity; i++) {
		addr_entry* e = &at->entries[i];
		if (e->key == 0) continue;
		addr_entry* dest = findAddrEntry(e->key, entries, capacity);
		dest->key = e->key;
		dest->value = e->value;
	}
	free(at->entries);
	at->entries = entries;
	at->capacity = capacity;
}

void initAddrTable(addr_table* at) {
	at->count = 0;
	at->capacity = 0;
	at->entries = NULL;
}

/*
does not free the values stored in the table — caller must drain/free those first
*/
void freeAddrTable(addr_table* at) {
	free(at->entries);
	initAddrTable(at);
}

void* findAddrTable(address key, addr_table* at) {
	if (at->capacity == 0) return NULL;
	addr_entry* found = findAddrEntry(key, at->entries, at->capacity);
	return found->key ? found->value : NULL;
}

/*
inserts key/value into the table, or overwrites the value if key is already present
*/
void insertAddrTable(address key, void* value, addr_table* at) {
	if (at->count + 1 > at->capacity * ADDR_TABLE_MAX_LOAD_FACTOR) {
		int capacity = GROW_CAPACITY(at->capacity);
		adjustAddrCapacity(at, capacity);
	}
	addr_entry* found = findAddrEntry(key, at->entries, at->capacity);
	bool isNew = found->key == 0;
	found->key = key;
	found->value = value;
	if (isNew) at->count++;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// PUBLIC API FUNCTIONS

/*
frees a table struct, including the struct itself and all of its memory allocated members
frees allocated memory
*/
void freeTable(table* t) {
	freeStacks(t);
	free(t->name);
	free(t);
}

/*
Creates a new file for a database table and returns the matching table struct
Used to create a new table
mallocs new memory (table)
*/
table* createTable(char* tablename) {
	// Build path: "tables/<tablename>.tbl"
	const char* dir = TABLE_DIRECTORY;
	const char* ext = TABLE_EXTENSION;
	size_t pathlen = strlen(dir) + strlen(tablename) + strlen(ext) + 1;
	char* path = malloc(pathlen);
	snprintf(path, pathlen, "%s%s%s", dir, tablename, ext);

	mkdir(dir, 0755); // no-op if directory already exists

	FILE* f = fopen(path, "wb+");
	free(path);
	if (!f) {
		printf("Error: failed to create table file for '%s'\n", tablename);
		return NULL;
	}

	table* t = malloc(sizeof(table));
	t->source        = f;
	t->cursor        = 0;
	t->metalen       = METALEN * 4;
	t->pageStripes   = 1;
	t->nodeStripes   = 1;
	t->pageStripeLen = 8;
	t->nodeStripeLen = 8;
	t->pageNodeRatio = 1;
	t->pageSize      = PAGE_SIZE;
	t->nodeSize      = 49 + M_GLOBAL * (8 + PAGE_NUM_DISK_SIZE); // 49B fixed header + M children (8B) + M keys (PAGE_NUM_DISK_SIZE)
	t->M             = M_GLOBAL;
	// layout is [node stripe][pageNodeRatio page stripes] per unit — node
	// stripe 1 starts immediately after the header, page stripe 1 right
	// after that (see unitStart/currentPageStripeStart/currentNodeStripeStart)
	t->nodeFree      = t->metalen;
	t->pageFree      = (uint64_t)t->metalen
	                 + (uint64_t)t->nodeStripeLen * t->nodeSize;
	t->root          = 0;
	t->name          = strdup(tablename);

	setStacks(t);
	writeMeta(f, t);
	return t;
}

/*
initializes a table struct from a table file
used to open an existing table
*/
bool loadTable(char* tablename, table* t) {
	if (!tablename || !t) {
		printf("Error: loadTable() recieved a null input\n");
		return false;
	}
	// fname = tables/[tablename].tbl\0
	// size = 7 + strlen(tablename) + 4 + 1
	char* dir = TABLE_DIRECTORY;
	char* ext = TABLE_EXTENSION;
	size_t lenFName = 7 + strlen(tablename) + 5;
	char* fname = malloc(lenFName);
	snprintf(fname, lenFName, "%s%s%s", dir, tablename, ext);

	FILE* tfile = fopen(fname, "rb+");
	if (!tfile) {
		printf("Error: failed to open table %s\n", tablename);
		free(fname);
		return false;
	}
	if (!validateTableFile(tfile, fname)) {
		printf("Error: tried to load a table from %s but file was invalid\n", fname);
		free(fname);
		return false;
	}
	t->source = tfile;
	t->cursor = 0;
	t->name   = strdup(tablename);
	loadMeta(tfile, fname, t);
	setStacks(t);
	free(fname);
	return true;
}

/*
Deletes and frees a table and its table file
Frees allocated memory (table)
*/
bool deleteTable(table* t) {
	if (!t) {
		printf("Error: deleteTable was called on a NULL table struct\n");
		return false;
	}
	char* dir = TABLE_DIRECTORY;
	char* ext = TABLE_EXTENSION;
	size_t lenFName = 7 + strlen(t->name) + 5;
	char* fname = malloc(lenFName);
	snprintf(fname, lenFName, "%s%s%s", dir, t->name, ext);
	int r = remove(fname);
	if (r != 0) {
		printf("Error: failed to delete file for table %s\n", t->name);
		return false;
	}
	r = fclose(t->source);
	if (r != 0) {
		printf("Error: failed to close file connection for table %s\n", t->name);
		return false;
	}
	freeTable(t);
	return true;
}

/*
reads a page from an address into a chunk of memory
@param: p - a slotted page to load the data from disk into
mallocs page entries, page slots, and page entry data
*/
bool readPage(address addr, slotted_page* p, table* t) {
	// checking dirty table
	slotted_page* dirty = (slotted_page*)findAddrTable(addr, &t->pageDirty);
	if (dirty) {
		copyPage(dirty, p);
		return true;
	}

	// otherwise read from disk
	address prev = t->cursor;
	jump(addr, t);
	if (readByte(0, t) != 0) {
		printf("Error: attempted to read page at address %llu but page was invalid\n", addr);
		return false;
	}
	if (p == NULL) {
		printf("Error: tried to read a page into a chunk of memory but the pointer given was null\n");
		return false;
	}
	// header
	// page header layout: 0(1B) | pageNum(19B) | usedData(4B) | numRecords(4B) |
	//                     numEntries(4B) | arrCap(4B) | maxEntries(4B) | maxSlots(4B)  = 44B
	p->header.pageNum    = readPageNum(1, t);
	p->header.usedData   = readUInt(20, t);
	p->header.numRecords = readUInt(24, t);
	p->header.numEntries = readUInt(28, t);
	p->header.arrCap     = readUInt(32, t);
	p->header.maxEntries = readUInt(36, t);
	p->header.maxSlots   = readUInt(40, t);
	// slots (each slot on disk: ID(9B) | len(4B) | size(4B) | ptr(4B) = 21B)
	if (!p->slots) {
		p->slots = calloc(p->header.maxSlots, sizeof(sp_slot));
	}
	int offset = 44;
	for (int i = 0; i < p->header.numRecords; i++) {
		p->slots[i].ID   = readPageOffset(offset,    t);
		p->slots[i].len  = readUInt(offset + 9,  t);
		p->slots[i].size = readUInt(offset + 13, t);
		p->slots[i].ptr  = readUInt(offset + 17, t);
		offset += SP_SLOT_DISK_SIZE;
	}
	// records
	int entryOffset = 0;
	jump(addr + t->pageSize, t); // navigating to 1 byte after the end of the page
	if(!p->entries) {
		p->entries = calloc(p->header.maxEntries, sizeof(entry));
	}
	for (int i = 0; i < p->header.numEntries; i++) {
		// entry: <--  data | size (4B) | type (2B)  <--
		char code = readByte(-1, t);
		jumpRel(-2, t);
		for (int j = 0; j < NUM_DATATYPES; j++) {
			if (DATATYPE_CODES[j] == code) p->entries[i].type = j;
		}

		uint32_t size = readUInt(-4, t);
		jumpRel(-4, t);
		p->entries[i].size = size;
		if (p->entries[i].data) free(p->entries[i].data);
		p->entries[i].data = malloc(size);
		readArbitrary(p->entries[i].data, size, -(long)size, t);
		jumpRel(-(long)size, t);
	}
	jump(prev, t);
	return true;
}

/*
reads a page from an address into a chunk of memory
@param: p - a slotted page to load the data from disk into
*/
bool readNode(address addr, node* n, table* t) {
	// checking dirty table
	node* dirty = (node*)findAddrTable(addr, &t->nodeDirty);
	if (dirty) {
		copyNode(dirty, n);
		return true;
	}

	// otherwise search disk
	address prev = t->cursor;
	jump(addr, t);

	if (readByte(0, t) != 1) {
		printf("Error: attempted to read page at address %llu but page was invalid\n", addr);
		return false;
	}
	if (n == NULL) {
		printf("Error: tried to read a node into a chunk of memory but the pointer given was null\n");
		return false;
	}

	// read metadata
	// node layout: 0(1B) | parent(8B) | prev(8B) | next(8B) | childCount(4B) | maxKey(19B) | isLeaf(1B) |
	//              children(8B each) | keys(19B each)
	n->parent = readULong(1, t);
	n->prev = readULong(9, t);
	n->next = readULong(17, t);
	n->childCount = readUInt(25, t);
	n->maxKey = readPageNum(29, t);
	n->isLeaf = readByte(48, t);

	// read children
	int offset = 49;
	for (int i = 0; i < n->childCount; i++) {
		n->children[i] = readULong(offset, t);
		offset += 8;
	}
	// read keys
	int keylim = n->childCount;
	if (!n->isLeaf) {
		keylim--;
	}
	for (int i = 0; i < keylim; i++) {
		n->keys[i] = readPageNum(offset, t);
		offset += PAGE_NUM_DISK_SIZE;
	}

	// return to original cursor position
	jump(prev, t);
	return true;
}
// write page
/*
writes the given page to the given address
page layout:
 | 0 | pageNum | usedData | numRecords | numEntries | arrCap |
 | maxEntries | maxSlots | slots | ... | records |

the code in page.c -> hasSpace() relies on the number of bytes used to represent entry type and length
if any changes are made to that encoding, hasSpace() must be updated as well
please feel free to change the design of this system because I feel unclean using code like this
*/
static void writePage(slotted_page* p, address address, table* t) {
	// setup
	jump(address, t);
	char* buffer = calloc(t->pageSize, 1);
	// write header
	// page header layout: 0(1B) | pageNum(19B) | usedData(4B) | numRecords(4B) |
	//                     numEntries(4B) | arrCap(4B) | maxEntries(4B) | maxSlots(4B)  = 44B
	header h = p->header;
	writePageNumBytewise(buffer+1, h.pageNum);
	writeUIntBytewise(buffer+20, h.usedData);
	writeUIntBytewise(buffer+24, h.numRecords);
	writeUIntBytewise(buffer+28, h.numEntries);
	writeUIntBytewise(buffer+32, h.arrCap);
	writeUIntBytewise(buffer+36, h.maxEntries);
	writeUIntBytewise(buffer+40, h.maxSlots);
	// write slots (each slot on disk: ID(9B) | len(4B) | size(4B) | ptr(4B) = 21B)
	int offset = 44;
	for (int i = 0; i < h.numRecords; i++) {
		writePageOffsetBytewise(buffer+offset,    p->slots[i].ID);
		writeUIntBytewise(buffer+offset+9,  p->slots[i].len);
		writeUIntBytewise(buffer+offset+13, p->slots[i].size);
		writeUIntBytewise(buffer+offset+17, p->slots[i].ptr);
		offset += SP_SLOT_DISK_SIZE;
	}
	// write records
	int entryOffset = 0;
	for (int i = 0; i < h.numEntries; i++) {
		// entry: <--  data | size (4B) | type (2B)  <--
		entry e = p->entries[i];
		int addition = e.size + 6;
		if (entryOffset + addition + offset > t->pageSize) { // records with overwrite slots (without malicious inputs this should never occur)
			printf("Error: page stores more data than the specified page capacity.\n");
			// once error handling is implemented should kill program here
		}
		char* entryStart = buffer + t->pageSize - entryOffset - 1;
		*entryStart = DATATYPE_CODES[e.type];
		*(entryStart-1) = '\\';
		writeUIntBytewise(entryStart-5, e.size);
		memcpy(entryStart - 5 - e.size, e.data, e.size); // potentially dangerous
		entryOffset += addition;
	}
	// copy buffer to disk and clean up
	fwrite(buffer, 1, t->pageSize, t->source);
	free(buffer);
}

/*
writes one dirty page to its address and removes it from the dirty table
which entry gets written is unspecified (the dirty table has no inherent order)
*/
void writeNextPage(table* t) {
	if (t->pageDirty.count <= 0) return;
	for (int i = 0; i < t->pageDirty.capacity; i++) {
		addr_entry* e = &t->pageDirty.entries[i];
		if (e->key == 0) continue;
		address addr = e->key;
		slotted_page* page = (slotted_page*)e->value;
		e->key = 0;
		e->value = NULL;
		t->pageDirty.count--;
		writePage(page, addr, t);
		freeSPage(page); // frees page members
		free(page); // frees page pointer itself
		return;
	}
}

// write node

/*
writes the given node to the given address
*/
static void writeNode(node* n, address address, table* t) {
	// setup
	jump(address, t);
	char* buffer = calloc(t->nodeSize, 1);
	// write metadata
	// node layout: 0(1B) | parent(8B) | prev(8B) | next(8B) | childCount(4B) | maxKey(19B) | isLeaf(1B) |
	//              children(8B each) | keys(19B each)
	buffer[0] = 1;
	writeULongBytewise(buffer+1,  n->parent);
	writeULongBytewise(buffer+9,  n->prev);
	writeULongBytewise(buffer+17, n->next);
	writeUIntBytewise(buffer+25,  n->childCount);
	writePageNumBytewise(buffer+29, n->maxKey);
	buffer[48] = n->isLeaf;
	// write children
	int offset = 49;
	for (int i = 0; i < n->childCount; i++) {
		writeULongBytewise(buffer+offset, n->children[i]);
		offset += 8;
	}
	// write keys (leaf: one key per child; internal: one fewer key than children)
	int keylim = n->childCount;
	if (!n->isLeaf) keylim--;
	for (int i = 0; i < keylim; i++) {
		writePageNumBytewise(buffer+offset, n->keys[i]);
		offset += PAGE_NUM_DISK_SIZE;
	}
	// copy buffer to disk and clean up
	fwrite(buffer, 1, t->nodeSize, t->source);
	free(buffer);
}

/*
writes one dirty node to its address and removes it from the dirty table
which entry gets written is unspecified (the dirty table has no inherent order)
*/
void writeNextNode(table* t) {
	if (t->nodeDirty.count <= 0) return;
	for (int i = 0; i < t->nodeDirty.capacity; i++) {
		addr_entry* e = &t->nodeDirty.entries[i];
		if (e->key == 0) continue;
		address addr = e->key;
		node* n = (node*)e->value;
		e->key = 0;
		e->value = NULL;
		t->nodeDirty.count--;
		jump(addr, t);
		writeNode(n, addr, t);
		free(n);
		return;
	}
}

/*
writes a new tree consisting of one empty page and one empty node directly to a table file
should be used only on a new file
*/
void writeNewTree(slotted_page* p, address pageAddr, node* n, address nodeAddr, table* t) {
	writePage(p, pageAddr, t);
	writeNode(n, nodeAddr, t);
	writeMeta(t->source, t);
}

/*MIGHT NEED THESE TO RETURN TRUE OR FALSE*/
void loadParent(node* n, node* parent, table* t) {
	readNode(n->parent, parent, t);
}

void loadPrev(node* n, node* prev, table* t) {
	readNode(n->prev, prev, t);
}

void loadNext(node* n, node* next, table* t) {
	readNode(n->next, next, t);
}

// mark page dirty
void markPage(address address, slotted_page* p, table* t) {
	slotted_page* existing = (slotted_page*)findAddrTable(address, &t->pageDirty);
	if (existing) {
		copyPage(p, existing);
		return;
	}
	slotted_page* copy = malloc(sizeof(slotted_page));
	copyPage(p, copy);
	insertAddrTable(address, copy, &t->pageDirty);
}

// mark node dirty
void markNode(address address, node* n, table* t) {
	node* existing = (node*)findAddrTable(address, &t->nodeDirty);
	if (existing) {
		copyNode(n, existing);
		return;
	}
	node* copy = malloc(sizeof(node));
	copyNode(n, copy);
	insertAddrTable(address, copy, &t->nodeDirty);
}

// mark object for deletion
void markDelete(address address, table* t) {
	if (findAddrTable(address, &t->delete)) return; // skip if already marked
	insertAddrTable(address, NULL, &t->delete);
}

// delete object
void deleteObject(address address, table* t) {
	char code = 2; // a two in the first byte of an obejct means it's garbage
	jump(address, t);
	fwrite(&code, 1, 1, t->source);
}

/*
empties a table's write tables and makes the changes to the file on disk
*/
void commit(table* t) {
	for (int i = 0; i < t->pageDirty.capacity; i++) {
		addr_entry* e = &t->pageDirty.entries[i];
		if (e->key == 0) continue;
		slotted_page* page = (slotted_page*)e->value;
		writePage(page, e->key, t);
		freeSPage(page);
		free(page);
		e->key = 0;
		e->value = NULL;
	}
	t->pageDirty.count = 0;

	for (int i = 0; i < t->nodeDirty.capacity; i++) {
		addr_entry* e = &t->nodeDirty.entries[i];
		if (e->key == 0) continue;
		node* n = (node*)e->value;
		jump(e->key, t);
		writeNode(n, e->key, t);
		free(n);
		e->key = 0;
		e->value = NULL;
	}
	t->nodeDirty.count = 0;

	for (int i = 0; i < t->delete.capacity; i++) {
		addr_entry* e = &t->delete.entries[i];
		if (e->key == 0) continue;
		deleteObject(e->key, t);
		e->key = 0;
		e->value = NULL;
	}
	t->delete.count = 0;

	writeMeta(t->source, t);
}

/*
Discards a table's pending (uncommitted) changes instead of writing them.
Empties the dirty-write tables without touching the file, so the on-disk state
is left exactly as it was before the transaction started. Since nothing is
written here (not even writeMeta), the caller must not keep using this table
struct afterward — reload it fresh if further access is needed.
*/
void discard(table* t) {
	for (int i = 0; i < t->pageDirty.capacity; i++) {
		addr_entry* e = &t->pageDirty.entries[i];
		if (e->key == 0) continue;
		freeSPage((slotted_page*)e->value);
		free(e->value);
		e->key = 0;
		e->value = NULL;
	}
	t->pageDirty.count = 0;

	for (int i = 0; i < t->nodeDirty.capacity; i++) {
		addr_entry* e = &t->nodeDirty.entries[i];
		if (e->key == 0) continue;
		free(e->value);
		e->key = 0;
		e->value = NULL;
	}
	t->nodeDirty.count = 0;

	for (int i = 0; i < t->delete.capacity; i++) {
		t->delete.entries[i].key = 0;
		t->delete.entries[i].value = NULL;
	}
	t->delete.count = 0;
}

/*
header
node | node | ... | node
page | page | ... | page
page | page | ... | page
...
page | page | ... | page
node | node | ... | node
page | page | ... | page
...

header at beginning of file
a node stripe will always be first
then pageNodeRatio page stripes will follow

this group is a "unit". Page addressing and node
addressing are each a pure function of their own stripe count and
pageNodeRatio so that pages and nodes can be allocated at completely
different rates without their addressing ever computing an overlapping
address for either type.
*/

/*
total bytes occupied by one full unit: 1 node stripe + pageNodeRatio page stripes
*/
static uint64_t unitByteSize(table* t) {
	return (uint64_t)t->nodeStripeLen * t->nodeSize
	     + (uint64_t)t->pageNodeRatio * t->pageStripeLen * t->pageSize;
}

/*
byte offset where the given (0-indexed) unit begins
*/
static address unitStart(table* t, int unitIndex) {
	return t->metalen + (uint64_t)unitIndex * unitByteSize(t);
}

/*
start address of the current (t->pageStripes-th, 1-indexed) page stripe
exposed (non-static) for test use
*/
address currentPageStripeStart(table* t) {
	int stripeIdx = t->pageStripes - 1; // 0-indexed
	int unitIndex = stripeIdx / t->pageNodeRatio;
	int posInUnit = stripeIdx % t->pageNodeRatio;
	return unitStart(t, unitIndex)
	     + (uint64_t)t->nodeStripeLen * t->nodeSize
	     + (uint64_t)posInUnit * t->pageStripeLen * t->pageSize;
}

/*
start address of the current (t->nodeStripes-th, 1-indexed) node stripe
exposed (non-static) for test use
*/
address currentNodeStripeStart(table* t) {
	int unitIndex = t->nodeStripes - 1; // 0-indexed
	return unitStart(t, unitIndex);
}

// allocate new stripe
/*
allocates a new stripe and sets table.pageFree to that address
*/
static void newPageStripe(table* t) {
	t->pageStripes++;
	t->pageFree = currentPageStripeStart(t);
}

static void newNodeStripe(table* t) {
	t->nodeStripes++;
	t->nodeFree = currentNodeStripeStart(t);
}

address allocPage(table* t) {
	if (t->pageFree >= currentPageStripeStart(t) + (uint64_t)t->pageStripeLen * t->pageSize) {
		newPageStripe(t);
	}
	address out = t->pageFree;
	t->pageFree += t->pageSize;
	return out;
}

address allocNode(table* t) {
	if (t->nodeFree >= currentNodeStripeStart(t) + (uint64_t)t->nodeStripeLen * t->nodeSize) {
		newNodeStripe(t);
	}
	address out = t->nodeFree;
	t->nodeFree += t->nodeSize;
	return out;
}

// GARBAGE COLLECTION

/*
moves a node from the source disk address to the dest disk address
writes directly to disk without using the write queue
trusts that both addresses given are correct
*/
static void moveNode(address source, address dest, table* t) {
	address addr = t->cursor;
	node n;
	readNode(source, &n, t);
	writeNode(&n, dest, t);
	jump(addr, t);
}

/*
moves a page from the source disk address to the dest disk address
writes directly to disk without using the write queue
trusts that both addresses given are correct
*/
static void movePage(address source, address dest, table* t) {
	address addr = t->cursor;
	slotted_page p = {0};
	readPage(source, &p, t);
	writePage(&p, dest, t);
	freeSPage(&p);
	jump(addr, t);
}

/*
// condense stripe

@param address - address of the first byte of the stripe

void condensePageStripe(uint64_t address, table* t) {
	jump(address, t);
	int garbage = 0;
	for (int i = 0; i + garbage < t->pageStripeLen; i++) {
		if (readByte(0, t) == 2) { // first byte of an object being 2 means its garbage
			if (garbage > 0) {
				//
			}
		} else if (readByte(0, t) != 0) {
			printf("Error: tried to condense memory stripe at %d but address was invalid\n", address);
		}
	}
}

// condense all stripes
*/