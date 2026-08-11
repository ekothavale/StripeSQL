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

#include "schema.h"


#include "../memory.h"
#include "../value.h"

// ##########################################################################################################################################
// ##########################################################################################################################################
// hash table functions

static void freeHTValue(schema* val) {
	for (int i = 0; i < val->count; i++) free(val->colNames[i]);
	free(val->colNames);
	free(val->colTypes);
	free(val->tablename);
}

void initHashTable(hashtable* table) {
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

/*
can use hash as check since FNV1-A algorithm never naturally returns 0
does not free hash table pointer itself
*/
void freeHashTable(hashtable* table) {

	for (int i = 0; i < table->capacity; i++) {
		if (table->entries[i].hash) {
			freeHTValue(table->entries + i);
		}
	}
	FREE_ARRAY(schema, table->entries, table->capacity);
	initHashTable(table);
}

/*
FNV-1a hash function
*/
static uint32_t FNV1_A(const char* key, int length) {
	uint32_t hash = 2166136261u;
	for (int i = 0; i < length; i++) {
		hash ^= (uint8_t)key[i];
		hash *= 16777619;
	}
	return hash;
}

/*
induction wrapper around hash function
hash function must never naturally return 0 (see readHT() and freeHashTable())
*/
uint32_t hashString(const char* key, int length) {
	return FNV1_A(key, length);
}

/*
finds an entry in the given table
resolves collisions via quadratic probing
*/
static schema* findEntry(uint32_t hash, schema* entries, int capacity) {
	int velocity = 0;
	for (;;) {
		uint32_t index = (hash + velocity * velocity) % capacity;
		schema* found = &entries[index];
		if (found->hash == hash || found->hash == 0) {
			return found;
		}
		velocity++;
	}
}

/*
resize the given hash table by copying the data to a new table by rehashing each entry
frees the original table entries array, callocs a new one
*/
static void adjustCapacity(int capacity, hashtable* table) {
	schema* entries = calloc(sizeof(schema), capacity);
	for (int i = 0; i < table->capacity; i++) {
		schema* e = &table->entries[i];
		if (e->hash == 0) continue;

		schema* dest = findEntry(e->hash, entries, capacity);
		dest->hash = e->hash;
		dest->colNames = e->colNames;
		dest->colTypes = e->colTypes;
		dest->count = e->count;
		dest->tablename = e->tablename;
	}
	free(table->entries);
	table->entries = entries;
	table->capacity = capacity;
}

/*
inserts e into the given table if it is not already present
overwrites the exisiting value if e is already in the table
*/
void insertHT(schema* e, hashtable* table) {
	if (table->count + 1 > table->capacity * MAX_LOAD_FACTOR) {
		int capacity = GROW_CAPACITY(table->capacity);
		adjustCapacity(capacity, table);
	}
	schema* found = findEntry(e->hash, table->entries, table->capacity);
	found->hash = e->hash;
	found->colNames = e->colNames;
	found->colTypes = e->colTypes;
	found->count = e->count;
	found->tablename = e->tablename;
	table->count++;
}

/*
read a value from a hash table given a key
return NULL if value is not found
can use found->hash as a check because FNV1-A never returns 0 naturally
*/
schema* readHT(uint32_t hash, hashtable* table) {
	if (table->capacity == 0) return NULL;
	schema* found = findEntry(hash, table->entries, table->capacity);
	return found->hash ? found : NULL;
}

/*
deletes a key-value pair from a hash table
*/
void deleteHT(uint32_t hash, hashtable* table) {
	schema* target = findEntry(hash, table->entries, table->capacity);
	freeHTValue(target);
	target->colNames = NULL;
	target->colTypes = NULL;
	target->count = 0;
	target->hash = 0;
	target->tablename = NULL;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Schema functions

/*
creates a new, empty schema file at SCHEMA_DIR/schema.scma.
writes the magic number and a zero entry count.
overwrites any existing file at that path.
*/
void initSchema() {
	const char* path = SCHEMA_PATH;

	FILE* tfile = fopen(path, "wb");
	if (!tfile) {
		printf("Error: failed to create schema file\n");
		return;
	}
	uint32_t magic = SCHEMA_MAGIC;
	fwrite(&magic, sizeof(uint32_t), 1, tfile);
	uint32_t zero = 0;
	fwrite(&zero, sizeof(uint32_t), 1, tfile);
	fclose(tfile);
}

/*
reads the entries in the schema file and loads them into the given hash table
on-disk format per entry: [hash: u32] [name len: u32] [name bytes] [col count: u32] ([col N type: u8] [col N len: u32] [col N bytes])*
*/
static void readEntries(hashtable* ht, FILE* file) {
	uint32_t entryCount;
	fread(&entryCount, sizeof(uint32_t), 1, file);

	for (uint32_t i = 0; i < entryCount; i++) {
		uint32_t hash;
		fread(&hash, sizeof(uint32_t), 1, file);

		uint32_t nameLen;
		fread(&nameLen, sizeof(uint32_t), 1, file);
		char* tablename = malloc(nameLen + 1);
		fread(tablename, 1, nameLen, file);
		tablename[nameLen] = '\0';

		uint32_t colCount;
		fread(&colCount, sizeof(uint32_t), 1, file);

		char** cols = malloc(colCount * sizeof(char*));
		char* types = malloc(colCount);
		for (uint32_t j = 0; j < colCount; j++) {
			fread(&types[j], 1, 1, file);
			uint32_t len;
			fread(&len, sizeof(uint32_t), 1, file);
			char* name = malloc(len + 1);
			fread(name, 1, len, file);
			name[len] = '\0';
			cols[j] = name;
		}

		schema e = { .hash = hash, .tablename = tablename, .colNames = cols, .colTypes = types, .count = (int)colCount };
		insertHT(&e, ht);
	}
}

/*
writes all of the entries in the schema hash table to the schema file
on-disk format per entry: [hash: u32] [name len: u32] [name bytes] [col count: u32] ([col N type: u8] [col N len: u32] [col N bytes])*
*/
static void writeEntries(hashtable* ht, FILE* file) {
	uint32_t count = 0;
	for (int i = 0; i < ht->capacity; i++) {
		if (ht->entries[i].hash != 0) count++;
	}
	fwrite(&count, sizeof(uint32_t), 1, file);

	for (int i = 0; i < ht->capacity; i++) {
		schema* e = &ht->entries[i];
		if (e->hash == 0) continue;

		fwrite(&e->hash, sizeof(uint32_t), 1, file);
		uint32_t nameLen = (uint32_t)strlen(e->tablename);
		fwrite(&nameLen, sizeof(uint32_t), 1, file);
		fwrite(e->tablename, 1, nameLen, file);

		uint32_t colCount = (uint32_t)e->count;
		fwrite(&colCount, sizeof(uint32_t), 1, file);

		for (int j = 0; j < e->count; j++) {
			fwrite(&e->colTypes[j], 1, 1, file);
			uint32_t len = (uint32_t)strlen(e->colNames[j]);
			fwrite(&len, sizeof(uint32_t), 1, file);
			fwrite(e->colNames[j], 1, len, file);
		}
	}
}


/*
load schema data from tables/schema.scma
@return: hashtable pointer wiht schema data or NULL on failed read
mallocs (1 hashtable)
*/
hashtable* loadSchema() {
	// ../../tables/schema.scma
	const char* path = SCHEMA_PATH;

	FILE* tfile;
	tfile = fopen(path, "rb");
	// if tfile isn't found, try creating it
	if (!tfile) {
		initSchema();
		tfile = fopen(path, "rb");
	}
	// if this still fails, report an error
	if (!tfile) {
		printf("Error: failed to open database schema\n");
		return NULL;
	}
	uint32_t magic;
	fread(&magic, 4, 1, tfile);
	if (magic != SCHEMA_MAGIC) {
		printf("Read: %u\n", magic);
		printf("Error: schema file is corrupted\n");
		fclose(tfile);
		return NULL;
	}
	hashtable* ht = malloc(sizeof(hashtable));
	initHashTable(ht);
	readEntries(ht, tfile);
	fclose(tfile);
	return ht;
}

/*
assumes the hashtable contains accurate data
*/
void saveSchema(hashtable* schema) {
	const char* path = SCHEMA_PATH;

	FILE* tfile = fopen(path, "wb");
	if (!tfile) {
		printf("Error: failed to open schema file for writing\n");
		return;
	}
	uint32_t magic = SCHEMA_MAGIC;
	fwrite(&magic, sizeof(uint32_t), 1, tfile);
	writeEntries(schema, tfile);
	fclose(tfile);
}
