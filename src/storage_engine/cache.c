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

#include "cache.h"

// ##########################################################################################################################################
// ##########################################################################################################################################
// THE TABLE OF NODES

/*
the slot a node's address hashes to. Addresses are file offsets a node's size apart, so they're multiplied
by a large odd constant to spread them over the slots
*/
static uint32_t homeSlot(node_cache* c, address addr) {
	return (uint32_t)((addr * 0x9E3779B97F4A7C15ULL) >> 32) & (c->slotCount - 1);
}

/*
finds the slot holding the node at addr
@return the slot's index, or the index of the empty slot the node would go in if it isn't cached
the table is never more than half full, so there is always an empty slot to stop at
*/
static uint32_t findSlot(node_cache* c, address addr) {
	uint32_t i = homeSlot(c, addr);
	while (c->slots[i].addr != 0 && c->slots[i].addr != addr) {
		i = (i + 1) & (c->slotCount - 1);
	}
	return i;
}

/*
empties slot i and closes the gap it leaves, so that every node after it in the same run of full slots can
still be found by probing forward from its home slot
*/
static void emptySlot(node_cache* c, uint32_t i) {
	uint32_t mask = c->slotCount - 1;
	free(c->slots[i].cached);
	uint32_t hole = i;
	for (uint32_t j = (i + 1) & mask; c->slots[j].addr != 0; j = (j + 1) & mask) {
		// the node at j can move back into the hole unless its home slot lies after the hole, on the way to j:
		// probing for it would then never pass the hole
		uint32_t home = homeSlot(c, c->slots[j].addr);
		if (((j - home) & mask) >= ((j - hole) & mask)) {
			c->slots[hole] = c->slots[j];
			hole = j;
		}
	}
	c->slots[hole] = (node_cache_slot){0};
	c->count--;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// NODE CACHE API

/*
sets up an empty cache
@param maxNodes - the most nodes it will hold: for the top two levels of a tree, the root and as many
                  children as a node can have
callocs the table of slots (see nodeCacheFree())
*/
void nodeCacheInit(node_cache* c, uint32_t maxNodes) {
	*c = (node_cache){0};
	if (maxNodes == 0) return;
	c->maxNodes = maxNodes;
	c->slotCount = 8;
	while (c->slotCount < 2 * maxNodes) c->slotCount *= 2;
	c->slots = calloc(c->slotCount, sizeof(node_cache_slot));
}

/*
frees everything the cache holds and leaves it zeroed: still a valid cache, one that holds nothing
*/
void nodeCacheFree(node_cache* c) {
	nodeCacheClear(c);
	free(c->slots);
	*c = (node_cache){0};
}

/*
copies the cached node at addr into out
the caller gets a copy of its own, as it does from readNode(), so changing it doesn't change the cache
@return false if the node isn't cached, and out is left as it was
*/
bool nodeCacheGet(node_cache* c, address addr, node* out) {
	if (c->slotCount == 0) return false;
	node_cache_slot* slot = &c->slots[findSlot(c, addr)];
	if (slot->addr == 0) {
		c->misses++;
		return false;
	}
	*out = *slot->cached;
	c->hits++;
	return true;
}

/*
offers the cache a node that has just been read from the file. It keeps a copy if the node is at the top
of the tree, which is to say the root or one of the root's children, and it has room
@param n - the node's committed contents: never a pending change (see cache.h)
@param root - the address of the tree's root (0 if the table has no tree yet, and then nothing is kept)
@return whether the node is cached now
*/
bool nodeCacheOffer(node_cache* c, address addr, const node* n, address root) {
	if (c->slotCount == 0 || addr == 0 || root == 0) return false;
	if (addr != root && n->parent != root) return false;
	node_cache_slot* slot = &c->slots[findSlot(c, addr)];
	if (slot->addr == 0) {
		if (c->count >= c->maxNodes) return false;
		slot->addr = addr;
		slot->cached = malloc(sizeof(node));
		c->count++;
	}
	*slot->cached = *n;
	return true;
}

/*
replaces the cached copy of the node at addr, if there is one, with n: for when the node has been written to
the file, so that the cache goes on matching the file
a node that isn't cached is left out. It's offered when it is next read
@return whether the node was cached
*/
bool nodeCacheUpdate(node_cache* c, address addr, const node* n) {
	if (c->slotCount == 0) return false;
	node_cache_slot* slot = &c->slots[findSlot(c, addr)];
	if (slot->addr == 0) return false;
	*slot->cached = *n;
	return true;
}

/*
drops the node at addr from the cache: for when it has been deleted from the file
@return whether the node was cached
*/
bool nodeCacheRemove(node_cache* c, address addr) {
	if (c->slotCount == 0) return false;
	uint32_t i = findSlot(c, addr);
	if (c->slots[i].addr == 0) return false;
	emptySlot(c, i);
	return true;
}

/*
drops every node from the cache: for when the tree has a different root, and with it a different top two
levels. The cache fills again as nodes are read
*/
void nodeCacheClear(node_cache* c) {
	for (uint32_t i = 0; i < c->slotCount; i++) {
		free(c->slots[i].cached);
		c->slots[i] = (node_cache_slot){0};
	}
	c->count = 0;
}
