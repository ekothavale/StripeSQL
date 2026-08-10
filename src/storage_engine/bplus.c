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
The backend for this DBMS uses a B+ tree, a cousin of the B tree.
The B+ tree functions the same as the B tree, except data is only stored in leaf nodes.
Internal nodes store keys to guide the logarithmic search for nodes.
Furthermore, all leaf nodes are connected to form a sorted (doubly) linked list.
This speeds up operations such as surveying the entire database.
This file implements the B+ tree. The data in each node will be stored as a slotted page
which is implemented in another file.
*/

// Optimize 
//     first for algorithmic time complexity
//     second for smallest possible nodes

/* TODO: 
 - Finish implementing b tree algorithms in this file
 - Build file IO API
 - Reimplement the b tree algorithms on disk
*/ 

/* Keys:
 - Each key is a 32bit unsigned int
 - Last n bits represent the slot number within a page
 - Remaining bits represent the page number
 - n = number of slots per page
 - How many slots should be in a page?
 - How big are pages?
*/ 

// eventually clamp all the Ms in the shiftArray calls to their proper values

#include "bplus.h"
#include "../value.h"

static node* balanceTreeAdd(node* n, address addr, address* newAddrOut, table* t);
static address balanceTreeDelete(node* n, address addr, table* t);
static void propagateMaxKeyUp(address nodeAddr, page_num newMaxKey, table* t);

// ##########################################################################################################################################
// ##########################################################################################################################################
// TREE CREATION FUNCTIONS

/*
creates a new root node
*/
static node* newRoot(node* child, address childAddr, table* t) {
	node* new = calloc(1, sizeof(node));
	new->childCount = 1;
	new->children[0] = childAddr;
	new->parent = 0;
	new->prev = 0;
	new->next = 0;
	new->isLeaf = false;
	new->maxKey = child->maxKey;
	address rootAdd = allocNode(t);
	child->parent = rootAdd;
	markNode(childAddr, child, t);
	markNode(rootAdd, new, t);
	t->root = rootAdd;
	return new;
}

/*
creates a blank leaf or interior node
*/
static node* newNode(bool isLeaf, address parent) {
	node* n = calloc(1, sizeof(node));
	n->isLeaf = isLeaf;
	n->parent = parent;
	n->childCount = 0;
	n->maxKey = (page_num){0};
	return n;
}

/*
creates and initializes a new table file and fills it with a new b+ tree, with one empty node and one empty page
@param pageNum - the page number of the starting page
@return - table struct containing the necessary data to use the table
mallocs new memory (table)
*/
table* createTree(char* tablename, page_num firstKey) {
	// create structs
	table* t = createTable(tablename);
	node* root = calloc(1, sizeof(node));
	address rootAddr = allocNode(t);
	slotted_page* page = makeSPage(firstKey, PAGE_NUM_SLOTS, PAGE_NUM_ENTRIES, PAGE_ARR_CAP);
	address pageAddr = allocPage(t);

	// initialize struct members (root and page already 0ed out)
	root->childCount = 1;
	root->children[0] = pageAddr;
	root->keys[0] = firstKey;
	root->isLeaf = true;
	root->maxKey = firstKey;

	t->root = rootAddr;

	// write structs and flush metadata (root pointer) to disk
	writeNewTree(page, pageAddr, root, rootAddr, t);
	free(root);
	free(page);
	return t;
}

void deleteTree(table* t) {
	deleteTable(t);
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// GENERAL HELPER FUNCTIONS

// this should eventually be moved to a file containing general helper functions for the entire project
static int max(int a, int b) {
	return a >= b ? a : b;
}

/* shifts the elements of an array right by 1 starting at the given index
assumes there is a free space in the array
len is the total length of the array
start is the free space to be created
*/
static int shiftPageNumArrayR(page_num* array, int start, int len) {
	if (start > len-1) {
		printf("Start index %d beyond length %d of array in shiftPageNumArrayR\n", start, len);
		return -1;
	}
	for (int i = len-1; i > start; i--) {
		array[i] = array[i-1];
	}
	array[start] = (page_num){0};
	return 0;
}

/*
shifts the elements of an array left, overwriting target
@input target - the first spot to be overwritten
@input len - the total length of the array (or at least the values you care about)
*/
static int shiftPageNumArrayL(page_num* array, int target, int len) {
	if (target > len-1) {
		printf("Start index %d beyond length %d of array in shiftPageNumArrayL\n", target, len);
		return -1;
	}
	for (int i = target; i < len-1; i++) {
		array[i] = array[i+1];
	}
	array[len-1] = (page_num){0};
	return 0;
}

static int shiftAddressArrayR(address* array, int start, int len) {
	if (start > len-1) {
		printf("Start index %d beyond length %d of array in shiftAddressArrayR\n", start, len);
		return -1;
	}
	for (int i = len-1; i > start; i--) {
		array[i] = array[i-1];
	}
	array[start] = 0;
	return 0;
}

static int shiftAddressArrayL(address* array, int target, int len) {
	if (target > len-1) {
		printf("Start index %d beyond length %d of array in shiftPageArrayL\n", target, len);
		return -1;
	}
	for (int i = target; i < len-1; i++) {
		array[i] = array[i+1];
	}
	array[len-1] = 0;
	return 0;
}

static bool isNodeFull(node* n) {
	return n->childCount >= M_GLOBAL;
}

static bool nodeAtMinimum(node* n) {
	return n->childCount <= HALF_M;
}

static bool isRoot(node* n) {
	return n->parent == 0 && n->childCount > 0;
}

/*
returns the address of a node's next sibling (not cousin)
*/
static address getNextInternal(node* n, address nAddr, table* t) {
	if (!n->parent) return 0;
	node parent;
	loadParent(n, &parent, t);
	for (int i = 0; i < parent.childCount - 1; i++) {
		if (parent.children[i] == nAddr) return parent.children[i+1];
	}
	return 0;
}

/*
returns the address of a node's previous sibling (not cousin)
*/
static address getPrevInternal(node* n, address nAddr, table* t) {
	if (!n->parent) return 0;
	node parent;
	loadParent(n, &parent, t);
	for (int i = 1; i < parent.childCount; i++) {
		if (parent.children[i] == nAddr) return parent.children[i-1];
	}
	return 0;
}


// ##########################################################################################################################################
// ##########################################################################################################################################
// INSERTION FUNCTIONS

/*
puts a page into a parent node's children and keys arrays
assumes node is not full
*/
static void insertPageIntoChildren(node* n, address nodeAddr, slotted_page* p, address pageAddr, table* t) {
	// check if page should be inserted into the middle of the children
	for (int i = 0; i < n->childCount; i++) {
		if (comparePageNums(n->keys[i], p->header.pageNum) > 0) {
			shiftPageNumArrayR(n->keys, i, M_GLOBAL);
			n->keys[i] = p->header.pageNum;
			shiftAddressArrayR(n->children, i, M_GLOBAL);
			n->children[i] = pageAddr;
			n->childCount++;
			markNode(nodeAddr, n, t);
			return;
		}
	}
	// otherwise, the correct spot must be at the end
	n->keys[n->childCount] = p->header.pageNum;
	n->children[n->childCount] = pageAddr;
	n->childCount++;
	n->maxKey = p->header.pageNum;
	markNode(nodeAddr, n, t);
	// n's own max just grew; if n is itself its parent's last child, that
	// growth must propagate up through ancestor separator keys too, or a
	// future top-down search can miss it entirely (see propagateMaxKeyUp)
	propagateMaxKeyUp(nodeAddr, n->maxKey, t);
	return;
}

/*
puts a node into a parent node's children and keys arrays
assumes node is not full
*/
static void splitUpdateParent(node* parent, node* child, address childAddr, page_num newKey, table* t) {
	if (isNodeFull(parent)) {
		printf("Balancing parent from splitUpdateParent()\n");
		address newSiblingAddr;
		node* newSibling = balanceTreeAdd(parent, child->parent, &newSiblingAddr, t);
		// balanceTreeAdd() just split `parent` into two nodes: `parent` itself
		// (now the lower half) and `newSibling` (the upper half). child must
		// go into whichever half actually covers its key range — blindly
		// continuing to use `parent` below would silently misplace child
		// under the wrong half whenever newKey belongs in the upper half.
		if (comparePageNums(newKey, parent->maxKey) > 0) {
			child->parent = newSiblingAddr;
			splitUpdateParent(newSibling, child, childAddr, newKey, t);
			free(newSibling);
			return;
		}
		free(newSibling);
	}
	// look for correct spot in parent's keys
	for (int i = 0; i < parent->childCount-1; i++) {
		if (comparePageNums(parent->keys[i], newKey) > 0) {
			shiftPageNumArrayR(parent->keys, i, M_GLOBAL);
			parent->keys[i] = newKey;
			shiftAddressArrayR(parent->children, i+1, M_GLOBAL);
			/*parent->children[i+1] = parent->children[i+2];
			parent->children[i+2] = child;*/
			parent->children[i+1] = childAddr;
			parent->childCount++;
			return;
		}
	}
	// otherwise, the correct spot must be at the end
	parent->keys[parent->childCount-1] = newKey; // no previous key to replace
	parent->children[parent->childCount] = childAddr;
	parent->childCount++;
	// child just became parent's new last child, so parent's own max grew
	// to match child's own true max — NOT newKey, which is only the
	// separator between the previous last child and child (roughly the
	// previous node's own shrunk max after splitting), and would silently
	// undercount whenever child's true max exceeds that separator.
	// Propagate the correct value up too, in case parent is itself its own
	// parent's last child (see propagateMaxKeyUp).
	parent->maxKey = child->maxKey;
	markNode(child->parent, parent, t);
	propagateMaxKeyUp(child->parent, child->maxKey, t);
	return;
}

// splits a node, making sure the new node is properly connected to the b+tree
// assumes the parent node is not full
static node* splitNode(node* n, address addr, address* newAddrOut, table* t) {
	//if (isNodeFull(n->parent)) printf("Error: tried to split a node with a full parent");
	node* new = newNode(n->isLeaf, n->parent);
	address newAddr = allocNode(t);
	*newAddrOut = newAddr;
	int middleKid = n->childCount / 2;

	// save separator key before modifying keys (internal nodes only)
	page_num separatorKey = n->keys[middleKid - 1];

	// copy children
	for (int i = 0; i < middleKid; i++) {
		address childAddr = n->children[i + middleKid];
		new->children[i] = childAddr;
		n->children[i + middleKid] = 0;
		if (!n->isLeaf) {
			node cn = {0};
			if (readNode(childAddr, &cn, t)) {
				cn.parent = newAddr;
				markNode(childAddr, &cn, t);
			}
		}
	}

	// copy keys: leaf gets middleKid keys; internal promotes middleKid-1 (separator goes to parent)
	if (n->isLeaf) {
		for (int i = 0; i < middleKid; i++) {
			new->keys[i] = n->keys[i + middleKid];
			n->keys[i + middleKid] = (page_num){0};
		}
	} else {
		for (int i = 0; i < middleKid - 1; i++) {
			new->keys[i] = n->keys[i + middleKid];
			n->keys[i + middleKid] = (page_num){0};
		}
		n->keys[middleKid - 1] = (page_num){0}; // clear promoted separator from n
	}

	new->childCount = middleKid;
	n->childCount -= middleKid;

	// insert new node into the leaf linked list
	if (n->isLeaf) {
		if (n->next) {
			address oldNext = n->next;
			n->next = newAddr;
			new->prev = addr;
			new->next = oldNext;
			node tmp;
			readNode(oldNext, &tmp, t);
			tmp.prev = newAddr;
			markNode(oldNext, &tmp, t);
		} else {
			n->next = newAddr;
			new->prev = addr;
		}
	}

	// update maxKey for both halves
	new->maxKey = n->maxKey;
	if (n->isLeaf) {
		n->maxKey = n->keys[n->childCount - 1];
	} else {
		// keys[i] holds children[i]'s own max (see findPageAndLeaf's
		// pageNum <= keys[i] -> descend children[i]), so keys[childCount-2]
		// gives the SECOND-to-last child's max, not the new last child's
		// (children[childCount-1]) — reading it here was always wrong,
		// leaving n->maxKey one child too low after every internal split.
		node lastChild = {0};
		readNode(n->children[n->childCount - 1], &lastChild, t);
		n->maxKey = lastChild.maxKey;
	}

	// insert new node into parent
	page_num splitKey = n->isLeaf ? n->maxKey : separatorKey;
	node parent;
	readNode(n->parent, &parent, t);
	splitUpdateParent(&parent, new, newAddr, splitKey, t);
	markNode(addr, n, t);
	markNode(newAddr, new, t);
	return new;
}

/*
@param n = the node to be split
*/
static node* balanceTreeAdd(node* n, address addr, address* newAddrOut, table* t) {
	if (!isNodeFull(n)) {
		printf("Error: called balanceTreeAdd() on a node that wasn't full\n");
		return NULL;
	}
	if (isRoot(n)) {
		newRoot(n, addr, t);
	} else {
		node parent;
		readNode(n->parent, &parent, t);
		if (isNodeFull(&parent)) {
			address dummy;
			balanceTreeAdd(&parent, n->parent, &dummy, t);
			// The grandparent's own split may have moved n into the new
			// sibling half, changing n's true parent. n's in-memory
			// .parent field was cached before that recursion ran, so it
			// can now be stale — re-read n so splitNode below inserts n's
			// split-off half into n's actual current parent, not a stale one.
			readNode(addr, n, t);
		}
	}
	return splitNode(n, addr, newAddrOut, t);
}

// adds a page to a node and balances the tree recursively
static void addPage(node* n, address nodeAddr, slotted_page* p, address pageAddr, table* t) {
	if (isNodeFull(n)) {
		address newNodeAddr;
		node* new = balanceTreeAdd(n, nodeAddr, &newNodeAddr, t);
		if (comparePageNums(p->header.pageNum, n->maxKey) > 0) {
			insertPageIntoChildren(new, newNodeAddr, p, pageAddr, t);
			// Propagate the new max up to the root so findPage stays accurate
			node root;
			readNode(t->root, &root, t);
			if (comparePageNums(p->header.pageNum, root.maxKey) > 0) {
				root.maxKey = p->header.pageNum;
				markNode(t->root, &root, t);
			}
			return;
		}
	}
	insertPageIntoChildren(n, nodeAddr, p, pageAddr, t);
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// DELETION FUNCTIONS

/*
Propagates a node's growth (its new, larger maxKey — typically after absorbing
a sibling via mergeNode) up through its ancestors' separator keys.

An internal node's keys[] array only bounds children other than the last one
(children[i] is bounded above by keys[i] for i < childCount-1; the last child
has no explicit upper bound, matched via the "not found" fallback in the
top-down search functions). So:
 - if nodeAddr is NOT its parent's last child, only the parent's separator
   key immediately governing it needs updating — nothing above that changes.
 - if nodeAddr IS its parent's last child (including the degenerate
   single-child case), the parent's own maxKey grew by the same amount, so
   the same propagation must continue to the grandparent, and so on.
Without this, an ancestor's separator key can stay stuck at an old, too-low
value indefinitely — silently hiding everything under the grown node from
any future top-down key search (though it stays fully reachable via the leaf
linked list, since that's unaffected by any of this).
*/
static void propagateMaxKeyUp(address nodeAddr, page_num newMaxKey, table* t) {
	node n = {0};
	if (!readNode(nodeAddr, &n, t) || !n.parent) return;
	node parent = {0};
	if (!readNode(n.parent, &parent, t)) return;
	for (int i = 0; i < parent.childCount; i++) {
		if (parent.children[i] != nodeAddr) continue;
		if (i < parent.childCount - 1) {
			parent.keys[i] = newMaxKey;
			markNode(n.parent, &parent, t);
		} else {
			parent.maxKey = newMaxKey;
			markNode(n.parent, &parent, t);
			propagateMaxKeyUp(n.parent, newMaxKey, t);
		}
		return;
	}
}

/*
Assumes n's siblings are empty enough to merge with n since merging should only be done if borrowing fails
returns the address of the surviving node
*/
static address mergeNode(node* n, address addr, table* t) {
	// setup node pointers
	node* survivor = n;
	address survAddr = addr;
	node* source = calloc(1, sizeof(node));
	address sourceAddr;
	node* prev = calloc(1, sizeof(node));
	// some operations differ whether n is a leaf node or not
	if (n->isLeaf) {
		// get previous node
		address prevAddr = getPrevInternal(n, addr, t);
		if (prevAddr) readNode(prevAddr, prev, t);
		// determine source and survivor
		if (prevAddr && prev->parent == n->parent) {
			survivor = prev;
			survAddr = prevAddr;
			source = n;
			sourceAddr = addr;
		} else {
			sourceAddr = getNextInternal(n, addr, t);
			readNode(sourceAddr, source, t);
		}
		// copy keys and children
		for (int i = 0; i < source->childCount; i++) {
			survivor->keys[survivor->childCount] = source->keys[i];
			survivor->children[survivor->childCount++] = source->children[i];
		}

		// update linked list — must run even when source->next is 0 
		survivor->next = source->next;
		if (source->next) {
			node tmp;
			readNode(source->next, &tmp, t);
			tmp.prev = survAddr;
			markNode(survivor->next, &tmp, t);
		}
	// n is an internal node
	} else {
		// determine source and survivor
		address prevAddr = getPrevInternal(n, addr, t);
		if (prevAddr) readNode(prevAddr, prev, t);
		if (prevAddr && prev->parent == n->parent) {
			survivor = prev;
			survAddr = prevAddr;
			source = n;
			sourceAddr = addr;
		} else {
			sourceAddr = getNextInternal(n, addr, t);
			readNode(sourceAddr, source, t);
		}

		// load parent
		node mergeParent = {0};
		loadParent(n, &mergeParent, t);
		// get the key between source and survivor
		page_num boundaryKey = (page_num){0};
		for (int i = 0; i < mergeParent.childCount - 1; i++) {
			if (mergeParent.children[i] == survAddr && mergeParent.children[i+1] == sourceAddr) {
				boundaryKey = mergeParent.keys[i];
				break;
			}
		}
		// copy keys and children
		for (int i = 0; i < source->childCount; i++) {
			survivor->keys[survivor->childCount-1] = (i == 0) ? boundaryKey : source->keys[i-1];
			address childAddr = source->children[i];
			survivor->children[survivor->childCount++] = childAddr;
			// the moved child's parent pointer must follow it to the survivor
			node cn = {0};
			if (readNode(childAddr, &cn, t)) {
				cn.parent = survAddr;
				markNode(childAddr, &cn, t);
			}
		}
	}
	// update source
	source->childCount = 0;
	markNode(sourceAddr, source, t);
	// update maxKey
	survivor->maxKey = source->maxKey;
	markNode(survAddr, survivor, t);
	// propagate max key
	propagateMaxKeyUp(survAddr, survivor->maxKey, t);
	// update parent
	node* parent = calloc(1, sizeof(node));
	loadParent(n, parent, t);
	for (int i = 0; i < parent->childCount; i++) {
		if (parent->children[i] == sourceAddr) {
			shiftAddressArrayL(parent->children, i, M_GLOBAL);
			shiftPageNumArrayL(parent->keys, i == 0 ? 0 : i - 1, M_GLOBAL);
			break;
		}
	}
	parent->childCount--;
	// delete source
	markDelete(sourceAddr, t);
	// rebalance parent if necessary (rebalancing persists state to disk)
	if (parent->childCount < HALF_M) {
		balanceTreeDelete(parent, n->parent, t);
	} else {
		markNode(n->parent, parent, t);
	}
	// clean up and return
	free(source);
	free(prev);
	free(parent);
	return survAddr;
}

/*
determines whether a node can borrow a child from a target
valid borrow targets:
 - exist
 - have more than M/2 children
 - have the same parent as n
*/
static bool isValidBorrow(node* n, node* target) {
	return (target && target->childCount > M_GLOBAL/2 && target->parent == n->parent);
}

/*
assumes that next is a valid target for a borrow
*/
static void borrowNext(node* n, address nAddr, node* next, address nextAddr, table* t) {
	address borrowedAddr = next->children[0];
	n->children[n->childCount] = borrowedAddr;
	n->keys[n->childCount++] = next->keys[0];
	// n gained a new last child, so its own maxKey must grow to match —
	// otherwise, if n is later chosen as a merge source, mergeNode reads this
	// stale (too-low) maxKey via survivor->maxKey = source->maxKey and
	// silently loses track of everything n has grown to contain since
	n->maxKey = n->keys[n->childCount-1];
	next->childCount--;
	shiftAddressArrayL(next->children, 0, M_GLOBAL);
	shiftPageNumArrayL(next->keys, 0, M_GLOBAL);
	// the borrowed page doesn't track its own parent (found via tree traversal
	// instead), so nothing further to update about it here
	markNode(nAddr, n, t);
	markNode(nextAddr, next, t);
	// update parent
	node parent = {0};
	loadParent(n, &parent, t);
	page_num borrowed = n->keys[n->childCount-1];
	for (int i = 0; i < parent.childCount; i++) {
		if (comparePageNums(borrowed, parent.keys[i]) > 0) {
			parent.keys[i] = borrowed;
			markNode(n->parent, &parent, t);
			return;
		}
	}
	printf("Error: Function borrowNext() borrowed a key from next that was less than a key from n\n");
	return;

}

/*
assumes that prev is a valid target for a borrow
*/
static void borrowPrev(node* n, address nAddr, node* prev, address prevAddr, table* t) {
	shiftPageNumArrayR(n->keys, 0, M_GLOBAL);
	shiftAddressArrayR(n->children, 0, M_GLOBAL);
	prev->childCount--;
	n->keys[0] = prev->keys[prev->childCount];
	address borrowedAddr = prev->children[prev->childCount];
	n->children[0] = borrowedAddr;
	prev->keys[prev->childCount] = (page_num){0};
	prev->children[prev->childCount] = 0;
	// prev just lost its highest (last) child, so its own maxKey must shrink
	// to match its new true last child
	prev->maxKey = prev->keys[prev->childCount-1];
	n->childCount++;
	// the borrowed page doesn't track its own parent (found via tree traversal
	// instead), so nothing further to update about it here
	markNode(nAddr, n, t);
	markNode(prevAddr, prev, t);
	// update parent
	node parent = {0};
	loadParent(n, &parent, t);
	page_num borrowed = n->keys[0];
	for (int i = 0; i < parent.childCount; i++) {
		if (comparePageNums(borrowed, parent.keys[i]) == 0) {
			parent.keys[i] = prev->keys[prev->childCount-1];
			markNode(n->parent, &parent, t);
			return;
		}
	}
}

/*
implements b+ tree borrowing for internal nodes
assumes n, next and their parent are valid and internal nodes
*/
static void borrowNextThroughParent(node* n, address nAddr, node* next, address nextAddr, table* t) {
	node parent = {0};
	loadParent(n, &parent, t);
	for (int i = 0; i < parent.childCount; i++) {
		if (parent.children[i] == nAddr) {
			n->keys[n->childCount-1] = parent.keys[i];
			parent.keys[i] = next->keys[0];
			address borrowedAddr = next->children[0];
			n->children[n->childCount++] = borrowedAddr;
			node borrowed = {0};
			readNode(borrowedAddr, &borrowed, t);
			n->maxKey = borrowed.maxKey;
			// the borrowed child's parent pointer must be updated to n
			borrowed.parent = nAddr;
			markNode(borrowedAddr, &borrowed, t);
			shiftPageNumArrayL(next->keys, 0, M_GLOBAL-1);
			shiftAddressArrayL(next->children, 0, M_GLOBAL);
			next->childCount--;
			next->children[next->childCount] = 0;
			next->keys[next->childCount-1] = (page_num){0};
			markNode(nAddr, n, t);
			markNode(nextAddr, next, t);
			markNode(n->parent, &parent, t);
			return;
		}
	}
}

static void borrowPrevThroughParent(node* n, address nAddr, node* prev, address prevAddr, table* t) {
	node parent = {0};
	loadParent(n, &parent, t);
	for (int i = 1; i < parent.childCount; i++) {
		if (parent.children[i] == nAddr) {
			shiftPageNumArrayR(n->keys, 0, M_GLOBAL);
			shiftAddressArrayR(n->children, 0, M_GLOBAL);
			n->keys[0] = parent.keys[i-1];
			prev->childCount--;
			address borrowedAddr = prev->children[prev->childCount];
			n->children[0] = borrowedAddr;
			parent.keys[i-1] = prev->keys[prev->childCount-1];
			// prev just lost its highest (last) child, so its own maxKey
			// must shrink to match its new true last child
			prev->maxKey = prev->keys[prev->childCount-1];
			prev->children[prev->childCount] = 0;
			prev->keys[prev->childCount-1] = (page_num){0};
			// the borrowed child's parent pointer must be updated to n
			node borrowed = {0};
			if (readNode(borrowedAddr, &borrowed, t)) {
				borrowed.parent = nAddr;
				markNode(borrowedAddr, &borrowed, t);
			}
			markNode(nAddr, n, t);
			markNode(prevAddr, prev, t);
			markNode(n->parent, &parent, t);
			return;
		}
	}
}

static address balanceTreeDelete(node* n, address addr, table* t) {
	// if n is a leaf node
	if (n->isLeaf) { // needs to come before root case since a node that is both a root and a leaf can have one page child
		// get next
		address nextAddr = getNextInternal(n, addr, t);
		node next = {0};
		// get parent
		if (nextAddr) readNode(nextAddr, &next, t);
		node* parent = malloc(sizeof(node));
		loadParent(n, parent, t);
		// check if next is a valid borrow target
		if (nextAddr && isValidBorrow(n, &next)) {
			borrowNext(n, addr, &next, nextAddr, t);
			if (parent->childCount < HALF_M) balanceTreeDelete(parent, n->parent, t);
			free(parent);
			return addr;
		}
		// load prev
		address prevAddr = getPrevInternal(n, addr, t);
		node prev = {0};
		if (prevAddr) readNode(prevAddr, &prev, t);
		// check if prev is a valid borrow target
		if (prevAddr && isValidBorrow(n, &prev)) {
			borrowPrev(n, addr, &prev, prevAddr, t);
			if (parent->childCount < HALF_M) balanceTreeDelete(parent, n->parent, t);
			free(parent);
			return addr;
		}
		free(parent);
		return mergeNode(n, addr, t);
	// if n is a root node
	} else if (addr == t->root && n->childCount == 1) {
		node* r = malloc(sizeof(node));
		address rAddr = n->children[0];
		readNode(rAddr, r, t);
		t->root = n->children[0];
		r->parent = 0;
		markNode(n->children[0], r, t);
		free(r);
		return rAddr;
	// if n is an internal node
	} else {
		address nextAddr = getNextInternal(n, addr, t);
		node nextNode = {0};
		if (nextAddr) readNode(nextAddr, &nextNode, t);
		if (nextAddr && isValidBorrow(n, &nextNode)) {
			borrowNextThroughParent(n, addr, &nextNode, nextAddr, t);
			return addr;
		}
		address prevAddr = getPrevInternal(n, addr, t);
		node prevNode = {0};
		if (prevAddr) readNode(prevAddr, &prevNode, t);
		if (prevAddr && isValidBorrow(n, &prevNode)) {
			borrowPrevThroughParent(n, addr, &prevNode, prevAddr, t);
			return addr;
		}
		return mergeNode(n, addr, t);
	}
}


/*
Deletes a page from a node
@return - whether the page was successfully deleted or not
*/
static bool deletePage(node* n, address nAddr, page_num pageNum, table* t)  {
	if (!n->isLeaf) {
		printf("Error: Tried to delete page in inner node\n");
		return false;
	}
	if (n->childCount == HALF_M && !isRoot(n)) {
		// balanceTreeDelete can return a different address than nAddr (e.g. if
		// n gets merged into a "prev" sibling, the survivor lives at that
		// sibling's address, not nAddr) — nAddr must track that or the
		// deletion below gets applied to n's refreshed content but then
		// written back to the wrong (stale) address via markNode(nAddr, ...).
		nAddr = balanceTreeDelete(n, nAddr, t);
		readNode(nAddr, n, t);
	}
	// search for page in node's children
	for (int i = 0; i < n->childCount; i++) {
		if (comparePageNums(n->keys[i], pageNum) == 0) {
			markDelete(n->children[i], t);
			shiftPageNumArrayL(n->keys, i, M_GLOBAL);
			shiftAddressArrayL(n->children, i, M_GLOBAL);
			n->childCount--;
			if (i == n->childCount) n->maxKey = n->keys[n->childCount-1];
			markNode(nAddr, n, t);
			return true;
		}
	}
	// otherwise, page doesn't exist in the node
	printf("Error: Tried to delete page from node at %p, but page was not found\n", n);
	return false;
}


// ##########################################################################################################################################
// ##########################################################################################################################################
// B+TREE API

// finds a page in a tree by page number and returns its address
// returns null if page is not in tree
address findPage(page_num pageNum, table* t) {
	node cur = {0};
	readNode(t->root, &cur, t);
    if (cur.childCount == 0) {
        printf("Attempted to find page in invalid tree\n");
        return 0;
    }
    while (!cur.isLeaf) {
        int found = 0;
        for (int i = 0; i < cur.childCount - 1; i++) {
            if (comparePageNums(pageNum, cur.keys[i]) <= 0) {
                readNode(cur.children[i], &cur, t);
                found = 1;
                break;
            }
        }
        if (!found) {
            readNode(cur.children[cur.childCount - 1], &cur, t);
        }
    }
    for (int i = 0; i < cur.childCount; i++) {
        if (comparePageNums(cur.keys[i], pageNum) == 0) {
            return cur.children[i];
        }
    }
	return 0;
}

/*
finds a page in a tree by page number and returns its address, along with the
address of the leaf node whose children array contains it (via leafOut).
returns 0 (leafOut left untouched) if the page is not in the tree
*/
address findPageAndLeaf(page_num pageNum, table* t, address* leafOut) {
	node cur = {0};
	address nAddr = t->root;
	readNode(nAddr, &cur, t);
    if (cur.childCount == 0) {
        printf("Attempted to find page in invalid tree\n");
        return 0;
    }
    while (!cur.isLeaf) {
        int found = 0;
        for (int i = 0; i < cur.childCount - 1; i++) {
            if (comparePageNums(pageNum, cur.keys[i]) <= 0) {
                nAddr = cur.children[i];
                readNode(nAddr, &cur, t);
                found = 1;
                break;
            }
        }
        if (!found) {
            nAddr = cur.children[cur.childCount - 1];
            readNode(nAddr, &cur, t);
        }
    }
    for (int i = 0; i < cur.childCount; i++) {
        if (comparePageNums(cur.keys[i], pageNum) == 0) {
            *leafOut = nAddr;
            return cur.children[i];
        }
    }
	return 0;
}

/*
finds a page in a tree by page number and returns its address
if the page does not exist, creates a page in the right spot and returns it
*/
address findAndInsert(page_num pageNum, table* t) {
	node cur = {0};
	address nAddr = t->root;
	readNode(nAddr, &cur, t);
    if (cur.childCount == 0) {
        printf("Attempted to find page in invalid tree\n");
        return 0;
    }
    while (!cur.isLeaf) {
        int found = 0;
        for (int i = 0; i < cur.childCount - 1; i++) {
            if (comparePageNums(pageNum, cur.keys[i]) <= 0) {
                nAddr = cur.children[i];
                readNode(nAddr, &cur, t);
                found = 1;
                break;
            }
        }
        if (!found) {
            nAddr = cur.children[cur.childCount - 1];
            readNode(nAddr, &cur, t);
        }
    }
    for (int i = 0; i < cur.childCount; i++) {
        page_num key = cur.keys[i];
        if (comparePageNums(key, pageNum) == 0) {
            return cur.children[i];
        } else if (comparePageNums(key, pageNum) > 0) {
			slotted_page* p = makeSPage(pageNum, PAGE_NUM_SLOTS, PAGE_NUM_ENTRIES, PAGE_ARR_CAP);
			address pageAddr = allocPage(t);
			addPage(&cur, nAddr, p, pageAddr, t);
			markPage(pageAddr, p, t);
			freeSPage(p); free(p);
			return pageAddr;
		}
    }
	slotted_page* p = makeSPage(pageNum, PAGE_NUM_SLOTS, PAGE_NUM_ENTRIES, PAGE_ARR_CAP);
	address pageAddr = allocPage(t);
    addPage(&cur, nAddr, p, pageAddr, t);
	markPage(pageAddr, p, t);
	freeSPage(p); free(p);
	return pageAddr;
}

/*
Searches for a page by number in the tree and deletes it
@return true - page is found and deleted
@return false - page deletion was unsuccessful
*/
bool findAndDelete(page_num pageNum, table* t) {
	node cur = {0};
	address nAddr = t->root;
	readNode(nAddr, &cur, t);
    if (cur.childCount == 0) {
        printf("Attempted to find page in invalid tree\n");
        return false;
    }
    while (!cur.isLeaf) {
        int found = 0;
        for (int i = 0; i < cur.childCount - 1; i++) {
            if (comparePageNums(pageNum, cur.keys[i]) <= 0) {
                nAddr = cur.children[i];
                readNode(nAddr, &cur, t);
                found = 1;
                break;
            }
        }
        if (!found) {
            nAddr = cur.children[cur.childCount - 1];
            readNode(nAddr, &cur, t);
        }
    }
    for (int i = 0; i < cur.childCount; i++) {
        page_num key = cur.keys[i];
        if (comparePageNums(key, pageNum) == 0) {
            return deletePage(&cur, nAddr, pageNum, t);
		}
    }
	return false;
}

bool insertRecord(sp_record* record, ordering_key key, table* t) {
	address addr = findAndInsert(key.pageNum, t);
	slotted_page p = {0};
	if (!readPage(addr, &p, t)) return false;
	bool out = SPInsert(&p, key.offset, *record);
	if (out) markPage(addr, &p, t);
	freeSPage(&p);
	return out;
}

bool searchRecord(ordering_key key, table* t) {
	address addr = findPage(key.pageNum, t);
	if (!addr) return false;
	slotted_page p = {0};
	readPage(addr, &p, t);
	int idx = SPSearch(&p, key.offset);
	freeSPage(&p);
	return idx >= 0;
}

/*
Reads a record by key. The caller provides a page buffer that backs the returned sp_record;
the caller must call freeSPage(page) when done with the record data.
Returns {0} if the record does not exist (page is left uninitialized in that case).
*/
sp_record readRecord(ordering_key key, table* t, slotted_page* page) {
	address addr = findPage(key.pageNum, t);
	if (!addr) return (sp_record){0};
	readPage(addr, page, t);
	return SPRead(page, key.offset);
}

bool updateRecord(sp_record* record, ordering_key key, table* t) {
	address addr = findAndInsert(key.pageNum, t);
	slotted_page p = {0};
	if (!readPage(addr, &p, t)) return false;
	bool out = SPUpdate(&p, key.offset, *record);
	if (out) markPage(addr, &p, t);
	freeSPage(&p);
	return out;
}

/*
Deletes a record from the B+ tree.
page is a caller-provided buffer; on return it holds the post-deletion state of the page.
@return true if the record was deleted or did not exist; false on failure.
*/
bool deleteRecord(ordering_key key, table* t, slotted_page* page) {
	address leafAddr = 0;
	address addr = findPageAndLeaf(key.pageNum, t, &leafAddr);
	if (!addr) return true;
	if (!readPage(addr, page, t)) return false;
	bool out = SPDelete(page, key.offset);
	if (!out) return false;
	if (page->header.numRecords == 0) {
		node leaf = {0};
		readNode(leafAddr, &leaf, t);
		deletePage(&leaf, leafAddr, key.pageNum, t);
	} else {
		markPage(addr, page, t);
	}
	return out;
}
