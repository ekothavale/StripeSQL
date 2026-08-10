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

#ifndef TABLEIO_H
#define TABLEIO_H

#include <stdio.h>
#include "common.h"
#include "page.h"
#include "node.h"

#define MAGIC 0xFACE3419
#define METALEN 16 // number of 4-byte words needed to represent a table's metadata
#define TABLE_DIRECTORY "tables/" // directory in which table files are placed
#define TABLE_EXTENSION ".tbl" // file extension for table files

typedef struct addr_entry {
	address key;   // 0 = empty slot (address 0 is never a valid page/node address)
	void* value;
}addr_entry;

typedef struct addr_table {
	int count;
	int capacity;
	addr_entry* entries;
}addr_table;

typedef struct table {
	addr_table pageDirty; // address -> slotted_page* of dirty pages
	addr_table nodeDirty; // address -> node* of dirty nodes
	addr_table delete; // address -> NULL; presence marks an object for deletion
	FILE* source; // physical file
	char* name; // name of table (corresponding file path is tables/[name].tbl)
	address cursor; // current file position (low-level; callers own their page/node state)
	address pageFree; // address of next free page space
	address nodeFree; // address of next free node space
	address root; // pointer to the root of the tree
	int metalen; // size of the metadata in the table file in bytes
	int pageStripes; // number of page stripes in file
	int pageStripeLen; // number of pages per page stripe
	int nodeStripes; // number of node stripes in file
	int nodeStripeLen; // number of nodes per stripe
	int pageNodeRatio; // how many page stripes there are per node stripe
	int pageSize; // size of page in bytes
	int nodeSize; // size of node in bytes
	int M; // maximum number of children each node can have
}table;

// generic address-keyed hash table (backs pageDirty / nodeDirty / delete)
void initAddrTable(addr_table* at);
void freeAddrTable(addr_table* at); // frees only the entries array; caller owns/frees the values
void* findAddrTable(address key, addr_table* at);
void insertAddrTable(address key, void* value, addr_table* at);

// manage table struct
void freeTable(table* t);
// manage database tables
table* createTable(char* tablename);
bool loadTable(char* tablename, table* t);
bool deleteTable(table* t);
// loading pages and nodes into caller-provided structs
bool readPage(address address, slotted_page* p, table* t);
bool readNode(address address, node* n, table* t);
void loadParent(node* n, node* parent, table* t);
void loadPrev(node* n, node* prev, table* t);
void loadNext(node* n, node* next, table* t);
// writing
void writeNextPage(table* t);
void writeNextNode(table* t);
void writeNewTree(slotted_page* p, address pageAddr, node* n, address nodeAddr, table* t);
// marking dirty objects
void markPage(address address, slotted_page* p, table* t);
void markNode(address address, node* n, table* t);
void markDelete(address address, table* t); // can be used for any object type
void commit(table* t);
void discard(table* t);
// allocate new addresses
void newStripe(table* t);
address allocNode(table* t);
address allocPage(table* t);
// file-level garbage collection (not yet implemented)
void condenseStripe(table* t);
void condenseAll(table* t);

// for testing purposes
address currentPageStripeStart(table* t);
address currentNodeStripeStart(table* t);

#endif