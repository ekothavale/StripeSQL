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
A cache of the nodes at the top of one table's b+ tree: the root and the root's children.

Every lookup, insert and delete walks down from the root, so these few nodes are read far more often than
any others, and there are at most M_GLOBAL + 1 of them. Keeping them in memory saves a seek, a read and a
decode for each of the first two nodes on every walk.

What it holds
 - A node is cached under its address in the table's file.
 - The cache holds each node as it was last committed, the same contents the file has. Changes a statement
   or transaction has made but not committed live in the table's dirty hashmap, which readNode() checks
   before it checks here. So nothing has to be undone in the cache when changes are discarded or a
   statement is rolled back.

What its owner has to tell it (tableIO.c and bplus.c do)
 - nodeCacheUpdate() when a node is written to the file, so the cache never holds contents the file
   doesn't have any more
 - nodeCacheRemove() when a node is deleted from the file
 - nodeCacheClear() when the tree gets a different root: every node is then a level higher or lower than
   it was, and the cache starts again from the new root

It fills as nodes are read: nodeCacheOffer() is given every node that is read from the file and keeps the
ones that belong at the top of the tree.

A zeroed node_cache is a valid cache that holds nothing and never will, so a table struct that was never
given one (in a test, say) works without it.
*/

#ifndef CACHE_H
#define CACHE_H

#include "../common.h"
#include "node.h"

// one place in the cache's table of nodes
typedef struct node_cache_slot {
	address addr; // the address of the node held here, or 0 if the slot is empty
	node* cached; // the cache's own copy of the node
}node_cache_slot;

typedef struct node_cache {
	node_cache_slot* slots; // open-addressed by node address, with linear probing
	uint32_t slotCount; // number of slots: a power of two, at least twice maxNodes
	uint32_t count; // number of nodes held
	uint32_t maxNodes; // the most nodes it will hold
	uint64_t hits; // nodeCacheGet() calls that found their node
	uint64_t misses; // nodeCacheGet() calls that didn't
}node_cache;

void nodeCacheInit(node_cache* c, uint32_t maxNodes);
void nodeCacheFree(node_cache* c);

bool nodeCacheGet(node_cache* c, address addr, node* out);
bool nodeCacheOffer(node_cache* c, address addr, const node* n, address root);
bool nodeCacheUpdate(node_cache* c, address addr, const node* n);
bool nodeCacheRemove(node_cache* c, address addr);
void nodeCacheClear(node_cache* c);

#endif // CACHE_H
