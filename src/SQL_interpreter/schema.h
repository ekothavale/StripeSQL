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
manages .scma files which map table names to column names
each entry is stored as such:
 table name | # columns | column name 0 | ...

table names are stored as hashes and all column names are length prefixed
*/

#ifndef SCHEMA_H
#define SCHEMA_H

#include "../common.h"

#define SCHEMA_PATH "tables/schema.scma"
#define SCHEMA_MAGIC 0xFFBB8844

#define MAX_LOAD_FACTOR 0.8 // load factor at which the hash table is resized

typedef enum column_constraint{
	CONSTRAINT_UNCONSTRAINED,
	CONSTRAINT_NOT_NULL,
	CONSTRAINT_PRIMARY_KEY,
	CONSTRAINT_UNQUE,
	CONSTRAINT_FOREIGN_KEY,
	CONSTRAINT_CHECK,
	CONSTRAINT_DEFAULT
} column_constraints;

typedef struct schema {
	char** colNames; // names of columns
	char* colTypes;// types of columns - first three bits are constraints - last five bits are sql types
	char* tablename; // corresponding name of table (key)
	uint32_t hash; // hash of key to id the entry
	int count; // number of columns in the entry
} schema;

typedef struct hashtable {
	int count;
	int capacity;
	schema* entries;
} hashtable;

// Public API — callable from outside this translation unit
// (FNV1_A, findEntry, and adjustCapacity are file-scoped static helpers)
void initHashTable(hashtable* table);
void freeHashTable(hashtable* table);
uint32_t hashString(const char* key, int len);
void insertHT(schema* e, hashtable* table);
schema* readHT(uint32_t, hashtable* table);
void deleteHT(uint32_t, hashtable* table);

// Public API — callable from outside this translation unit
void initSchema();
hashtable* loadSchema();
void saveSchema(hashtable* schema);

#endif