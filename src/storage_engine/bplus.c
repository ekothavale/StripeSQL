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

/* Keys:
 - Every record is found by one ordering key, built from its primary key (see ordering.c -> pkToOk())
 - Every key in a table has the same type, the table's keyType, which is stored once in the file's header
   and not with each key
 - A page holds the records for one run of consecutive keys, sorted by key, and splits in two when it fills,
   so which keys share a page depends on what has been inserted
 - A leaf node files each of its pages under a key that is an upper bound for that page's keys: every key in
   the page is at most that, and above the key the page before it is filed under. A page that has just been
   filled or split is filed under exactly its largest key; deleting a page's largest record leaves it filed
   under the old one, which is still an upper bound
 - An internal node's keys bound its children in the same way
*/

// eventually clamp all the Ms in the shiftArray calls to their proper values

#include "bplus.h"
#include "../value.h"

static node* balanceTreeAdd(node* n, address addr, address* newAddrOut, table* t);
static address balanceTreeDelete(node* n, address addr, table* t);
static void propagateMaxKeyUp(address nodeAddr, ordering_key newMaxKey, table* t);

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
callocs a node
*/
static node* newNode(bool isLeaf, address parent) {
	node* n = calloc(1, sizeof(node));
	n->isLeaf = isLeaf;
	n->parent = parent;
	n->childCount = 0;
	n->maxKey = (ordering_key){0};
	return n;
}

/*
makes an empty page for a table: as much room for records as the table's pages have, and slots sized for
its keys
Callocs new memory (see makeSPage())
*/
static slotted_page* newPage(table* t) {
	return makeSPage(PAGE_NUM_SLOTS, PAGE_NUM_ENTRIES, pageCapacity(t), keyDiskSize(t));
}

/*
builds a new table's b+ tree in memory, with one empty node and one empty page, both dirty
nothing touches the disk until the table is committed, which also creates its file (see newTable())
@param keyType - the type of the table's keys, which is kept in the table and decides how large its slots
                 and nodes are. The first page is filed under the smallest key of that type until a record
                 is inserted
@return - table struct containing the necessary data to use the table
mallocs new memory (table)
*/
table* newTree(char* tablename, ordering_type keyType) {
	// create structs
	table* t = newTable(tablename, keyType);
	ordering_key firstKey = { .type = keyType };
	node* root = calloc(1, sizeof(node));
	address rootAddr = allocNode(t);
	slotted_page* page = newPage(t);
	address pageAddr = allocPage(t);

	// initialize struct members (root and page already 0ed out)
	root->childCount = 1;
	root->children[0] = pageAddr;
	root->keys[0] = firstKey;
	root->isLeaf = true;
	root->maxKey = firstKey;

	t->root = rootAddr;

	// the dirty hashmaps keep their own copies
	markNode(rootAddr, root, t);
	markPage(pageAddr, page, t);
	free(root);
	freeSPage(page);
	free(page);
	return t;
}

/*
creates a new table with an empty b+ tree and commits it, which creates its file
@param keyType - the type of the table's keys (see newTree())
@return - table struct containing the necessary data to use the table, or NULL if the commit failed
mallocs new memory (table)
*/
table* createTree(char* tablename, ordering_type keyType) {
	table* t = newTree(tablename, keyType);
	if (!commit(t)) {
		freeTable(t);
		return NULL;
	}
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
static int shiftKeyArrayR(ordering_key* array, int start, int len) {
	if (start > len-1) {
		printf("Start index %d beyond length %d of array in shiftKeyArrayR\n", start, len);
		return -1;
	}
	for (int i = len-1; i > start; i--) {
		array[i] = array[i-1];
	}
	array[start] = (ordering_key){0};
	return 0;
}

/*
shifts the elements of an array left, overwriting target
@input target - the first spot to be overwritten
@input len - the total length of the array (or at least the values you care about)
*/
static int shiftKeyArrayL(ordering_key* array, int target, int len) {
	if (target > len-1) {
		printf("Start index %d beyond length %d of array in shiftKeyArrayL\n", target, len);
		return -1;
	}
	for (int i = target; i < len-1; i++) {
		array[i] = array[i+1];
	}
	array[len-1] = (ordering_key){0};
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
puts a page into a parent node's children and keys arrays, filed under key
assumes node is not full
*/
static void insertPageIntoChildren(node* n, address nodeAddr, ordering_key key, address pageAddr, table* t) {
	// check if page should be inserted into the middle of the children
	for (int i = 0; i < n->childCount; i++) {
		if (compareOrderingKeys(n->keys[i], key) > 0) {
			shiftKeyArrayR(n->keys, i, M_GLOBAL);
			n->keys[i] = key;
			shiftAddressArrayR(n->children, i, M_GLOBAL);
			n->children[i] = pageAddr;
			n->childCount++;
			markNode(nodeAddr, n, t);
			return;
		}
	}
	// otherwise, the correct spot must be at the end
	n->keys[n->childCount] = key;
	n->children[n->childCount] = pageAddr;
	n->childCount++;
	n->maxKey = key;
	markNode(nodeAddr, n, t);
	// propagate n's new max up if necessary
	propagateMaxKeyUp(nodeAddr, n->maxKey, t);
	return;
}

/*
puts a node into a parent node's children and keys arrays
assumes node is not full
*/
static void splitUpdateParent(node* parent, node* child, address childAddr, ordering_key newKey, table* t) {
	if (isNodeFull(parent)) {
		address newSiblingAddr;
		node* newSibling = balanceTreeAdd(parent, child->parent, &newSiblingAddr, t);
		// determine whether new sibling is the correct node to insert into
		if (compareOrderingKeys(newKey, parent->maxKey) > 0) {
			child->parent = newSiblingAddr;
			splitUpdateParent(newSibling, child, childAddr, newKey, t);
			free(newSibling);
			return;
		}
		free(newSibling);
	}
	// look for correct spot in parent's keys
	for (int i = 0; i < parent->childCount-1; i++) {
		if (compareOrderingKeys(parent->keys[i], newKey) > 0) {
			shiftKeyArrayR(parent->keys, i, M_GLOBAL);
			parent->keys[i] = newKey;
			shiftAddressArrayR(parent->children, i+1, M_GLOBAL);
			/*parent->children[i+1] = parent->children[i+2];
			parent->children[i+2] = child;*/
			parent->children[i+1] = childAddr;
			parent->childCount++;
			markNode(child->parent, parent, t);
			return;
		}
	}
	// otherwise, the correct spot must be at the end
	parent->keys[parent->childCount-1] = newKey; // no previous key to replace
	parent->children[parent->childCount] = childAddr;
	parent->childCount++;
	parent->maxKey = child->maxKey;
	markNode(child->parent, parent, t);
	propagateMaxKeyUp(child->parent, child->maxKey, t);
	return;
}

/*
splits a node, making sure the new node is properly connected to the b+tree
assumes the parent node is not full
*/
static node* splitNode(node* n, address addr, address* newAddrOut, table* t) {
	//if (isNodeFull(n->parent)) printf("Error: tried to split a node with a full parent");
	node* new = newNode(n->isLeaf, n->parent);
	address newAddr = allocNode(t);
	*newAddrOut = newAddr;
	int middleKid = n->childCount / 2;       // number of children the new (right) node receives
	int keepCount = n->childCount - middleKid; // number of children remaining (keepCount and middleKid differ when M is odd)

	// save separator key before modifying keys (internal nodes only)
	ordering_key separatorKey = n->keys[keepCount - 1];

	// copy children
	for (int i = 0; i < middleKid; i++) {
		address childAddr = n->children[i + keepCount];
		new->children[i] = childAddr;
		n->children[i + keepCount] = 0;
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
			new->keys[i] = n->keys[i + keepCount];
			n->keys[i + keepCount] = (ordering_key){0};
		}
	} else {
		for (int i = 0; i < middleKid - 1; i++) {
			new->keys[i] = n->keys[i + keepCount];
			n->keys[i + keepCount] = (ordering_key){0};
		}
		n->keys[keepCount - 1] = (ordering_key){0}; // clear promoted separator from n
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
		node lastChild = {0};
		readNode(n->children[n->childCount - 1], &lastChild, t);
		n->maxKey = lastChild.maxKey;
	}

	// insert new node into parent
	ordering_key splitKey = n->isLeaf ? n->maxKey : separatorKey;
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
		free(newRoot(n, addr, t)); // markNode() kept its own copy of the new root
	} else {
		node parent;
		readNode(n->parent, &parent, t);
		if (isNodeFull(&parent)) {
			address dummy;
			free(balanceTreeAdd(&parent, n->parent, &dummy, t)); // the parent's new sibling isn't needed here
			readNode(addr, n, t);
		}
	}
	return splitNode(n, addr, newAddrOut, t);
}

// adds a page to a leaf node, filed under key, and balances the tree recursively
static void addPage(node* n, address nodeAddr, ordering_key key, address pageAddr, table* t) {
	if (isNodeFull(n)) {
		address newNodeAddr;
		node* new = balanceTreeAdd(n, nodeAddr, &newNodeAddr, t);
		if (compareOrderingKeys(key, n->maxKey) > 0) {
			insertPageIntoChildren(new, newNodeAddr, key, pageAddr, t);
			// Propagate the new max up to the root so findPage stays accurate
			node root;
			readNode(t->root, &root, t);
			if (compareOrderingKeys(key, root.maxKey) > 0) {
				root.maxKey = key;
				markNode(t->root, &root, t);
			}
			free(new);
			return;
		}
		free(new);
	}
	insertPageIntoChildren(n, nodeAddr, key, pageAddr, t);
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// DELETION FUNCTIONS

/*
Propagates a node's new, larger maxkey up through its ancestors' separator keys.
*/
static void propagateMaxKeyUp(address nodeAddr, ordering_key newMaxKey, table* t) {
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
	// source points at this buffer or at n, which belongs to the caller, so the buffer is freed by its own name
	node* sourceBuf = calloc(1, sizeof(node));
	node* source = sourceBuf;
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
		ordering_key boundaryKey = (ordering_key){0};
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
			shiftKeyArrayL(parent->keys, i == 0 ? 0 : i - 1, M_GLOBAL);
			break;
		}
	}
	parent->childCount--;
	// delete source
	markDelete(sourceAddr, t);
	// rebalance parent if necessary (rebalancing persists state to disk)
	if (isRoot(parent)) {
		if (parent->childCount == 1) balanceTreeDelete(parent, n->parent, t);
		else markNode(n->parent, parent, t);
	} else if (parent->childCount < HALF_M) {
		balanceTreeDelete(parent, n->parent, t);
	} else {
		markNode(n->parent, parent, t);
	}
	// clean up and return
	free(sourceBuf);
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
	n->maxKey = n->keys[n->childCount-1];
	next->childCount--;
	shiftAddressArrayL(next->children, 0, M_GLOBAL);
	shiftKeyArrayL(next->keys, 0, M_GLOBAL);
	markNode(nAddr, n, t);
	markNode(nextAddr, next, t);
	// the borrowed key is n's new largest, so it becomes n's separator in the parent. n is found by
	// address: comparing keys would pick the first child's separator whenever n isn't the first child
	node parent = {0};
	loadParent(n, &parent, t);
	for (int i = 0; i < (int)parent.childCount - 1; i++) {
		if (parent.children[i] == nAddr) {
			parent.keys[i] = n->maxKey;
			markNode(n->parent, &parent, t);
			return;
		}
	}
	printf("Error: Function borrowNext() couldn't find the borrowing node in its parent\n");
	return;

}

/*
assumes that prev is a valid target for a borrow
*/
static void borrowPrev(node* n, address nAddr, node* prev, address prevAddr, table* t) {
	shiftKeyArrayR(n->keys, 0, M_GLOBAL);
	shiftAddressArrayR(n->children, 0, M_GLOBAL);
	prev->childCount--;
	n->keys[0] = prev->keys[prev->childCount];
	address borrowedAddr = prev->children[prev->childCount];
	n->children[0] = borrowedAddr;
	prev->keys[prev->childCount] = (ordering_key){0};
	prev->children[prev->childCount] = 0;
	// prev just lost its highest child, so its own maxKey must shrink
	prev->maxKey = prev->keys[prev->childCount-1];
	n->childCount++;
	markNode(nAddr, n, t);
	markNode(prevAddr, prev, t);
	// prev's separator in the parent shrinks to its new largest key. prev is found by address: its
	// separator can be larger than the key it just gave up, if its largest page was deleted earlier
	node parent = {0};
	loadParent(n, &parent, t);
	for (int i = 0; i < (int)parent.childCount - 1; i++) {
		if (parent.children[i] == prevAddr) {
			parent.keys[i] = prev->maxKey;
			markNode(n->parent, &parent, t);
			return;
		}
	}
	printf("Error: Function borrowPrev() couldn't find the lending node in its parent\n");
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
			shiftKeyArrayL(next->keys, 0, M_GLOBAL-1);
			shiftAddressArrayL(next->children, 0, M_GLOBAL);
			next->childCount--;
			next->children[next->childCount] = 0;
			next->keys[next->childCount-1] = (ordering_key){0};
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
			shiftKeyArrayR(n->keys, 0, M_GLOBAL);
			shiftAddressArrayR(n->children, 0, M_GLOBAL);
			n->keys[0] = parent.keys[i-1];
			prev->childCount--;
			address borrowedAddr = prev->children[prev->childCount];
			n->children[0] = borrowedAddr;
			n->childCount++;
			parent.keys[i-1] = prev->keys[prev->childCount-1];
			// prev just lost its highest child, so its own maxKey must shrink
			prev->maxKey = prev->keys[prev->childCount-1];
			prev->children[prev->childCount] = 0;
			prev->keys[prev->childCount-1] = (ordering_key){0};
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
			if (parent->childCount < HALF_M && !isRoot(parent)) balanceTreeDelete(parent, n->parent, t);
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
			// see the identical guard above in the borrowNext branch
			if (parent->childCount < HALF_M && !isRoot(parent)) balanceTreeDelete(parent, n->parent, t);
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
Deletes a page from a leaf node
the page is found by its address: the key it's filed under is an upper bound, not something to look it up by
@param nAddr - the node's address. Rebalancing can merge the node into a sibling first, so on return
               this holds the address of the leaf the page was removed from, and n holds that leaf
@return - whether the page was successfully deleted or not
*/
static bool deletePage(node* n, address* nAddr, address pageAddr, table* t)  {
	if (!n->isLeaf) {
		printf("Error: Tried to delete page in inner node\n");
		return false;
	}
	if (n->childCount == HALF_M && !isRoot(n)) {
		*nAddr = balanceTreeDelete(n, *nAddr, t);
		readNode(*nAddr, n, t);
	}
	// search for page in node's children
	for (int i = 0; i < n->childCount; i++) {
		if (n->children[i] == pageAddr) {
			markDelete(n->children[i], t);
			shiftKeyArrayL(n->keys, i, M_GLOBAL);
			shiftAddressArrayL(n->children, i, M_GLOBAL);
			n->childCount--;
			if (i == n->childCount && n->childCount > 0) n->maxKey = n->keys[n->childCount-1];
			markNode(*nAddr, n, t);
			return true;
		}
	}
	// otherwise, page doesn't exist in the node
	printf("Error: Tried to delete page from node at %p, but page was not found\n", n);
	return false;
}


// ##########################################################################################################################################
// ##########################################################################################################################################
// FINDING A KEY'S PAGE

/*
walks from the root to the leaf node whose pages cover key, and reads that node into leaf
a key above every key in the tree leads to the last leaf
@return the leaf's address, or 0 if a node on the way couldn't be read, or if key isn't of the type this
        table's keys are: keys of different types can't be compared, so such a key is in no page
*/
static address findLeaf(ordering_key key, table* t, node* leaf) {
	if (key.type != t->keyType) return 0;
	address addr = t->root;
	if (!readNode(addr, leaf, t)) return 0;
	while (!leaf->isLeaf) {
		if (leaf->childCount == 0) return 0;
		// a child's key is an upper bound for every key under it, so key belongs under the first child
		// whose key isn't below it, or under the last child if it's above them all
		uint32_t i = 0;
		while (i < leaf->childCount - 1 && compareOrderingKeys(key, leaf->keys[i]) > 0) i++;
		addr = leaf->children[i];
		if (!readNode(addr, leaf, t)) return 0;
	}
	return addr;
}

/*
finds the page in a leaf node that covers key, by binary search: the first page filed under a key that
isn't below it. The pages are in key order and each is filed under an upper bound for its own keys, so
no other page can hold key
@return the page's index among the leaf's children, or leaf->childCount if key is above every page's key
*/
static uint32_t findPageInLeaf(node* leaf, ordering_key key) {
	uint32_t lo = 0;
	uint32_t hi = leaf->childCount;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (compareOrderingKeys(leaf->keys[mid], key) < 0) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

/*
finds the page that covers key (the only page a record with that key can be in) and returns its address,
along with the address of the leaf node it belongs to (via leafOut)
returns 0 (leafOut left untouched) if key is above every key in the tree, or the tree couldn't be read
*/
address findPageAndLeaf(ordering_key key, table* t, address* leafOut) {
	node leaf = {0};
	address leafAddr = findLeaf(key, t, &leaf);
	if (!leafAddr) return 0;
	uint32_t i = findPageInLeaf(&leaf, key);
	if (i == leaf.childCount) return 0;
	*leafOut = leafAddr;
	return leaf.children[i];
}

// finds the page that covers key and returns its address
// returns null if key is above every key in the tree
address findPage(ordering_key key, table* t) {
	address leafAddr;
	return findPageAndLeaf(key, t, &leafAddr);
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// SPLITTING A PAGE

/*
picks where to split a page so that both halves fit in a page: the first *keepOut records stay, and the
rest move to a new page
if a record is about to be inserted, hasRecord is set, pos is the index it would be inserted at, and needed
is the room it takes (SPRecordBytes()); it then goes to whichever page its key belongs in, and
*recordInUpperOut says which. Of the splits that leave both pages within their capacity and neither one
empty, this picks the one that leaves them most evenly filled
@return false if no such split exists, which takes records larger than half a page
*/
static bool chooseSplit(slotted_page* p, bool hasRecord, uint32_t pos, uint32_t needed, uint32_t* keepOut, bool* recordInUpperOut) {
	uint32_t capacity = p->header.arrCap;
	uint32_t total = SPUsedBytes(p);
	uint32_t below = 0; // the room taken by the first `keep` records
	uint32_t best = UINT32_MAX;
	bool found = false;
	for (uint32_t keep = 0; keep <= p->header.numRecords; keep++) {
		for (int inUpper = 0; inUpper <= 1; inUpper++) {
			if (!hasRecord && inUpper) continue;
			// the new record has to stay on the same side as the records its key sorts beside
			if (hasRecord && (inUpper ? pos < keep : pos > keep)) continue;
			uint32_t extra = hasRecord ? needed : 0;
			uint32_t lower = below + (inUpper ? 0 : extra);
			uint32_t upper = total - below + (inUpper ? extra : 0);
			if (lower == 0 || upper == 0 || lower > capacity || upper > capacity) continue;
			uint32_t difference = lower > upper ? lower - upper : upper - lower;
			if (difference < best) {
				best = difference;
				*keepOut = keep;
				*recordInUpperOut = inUpper;
				found = true;
			}
		}
		if (keep < p->header.numRecords) below += SPRecordBytes(p, p->slots[keep].size, p->slots[keep].len);
	}
	return found;
}

/*
files a newly made page in the tree, directly after the page at pageAddr
the page at pageAddr keeps the lower keys: lowerKey is the largest one left in it, and it is filed under
that from now on. The new page takes over the key the old one was filed under, or its own largest key
(newKey) if that is higher, which happens when a record is added past the end of the tree
*/
static bool addPageAfter(address pageAddr, ordering_key lowerKey, address newAddr, ordering_key newKey, table* t) {
	node leaf = {0};
	address leafAddr = findLeaf(lowerKey, t, &leaf);
	if (!leafAddr) return false;
	uint32_t i = findPageInLeaf(&leaf, lowerKey);
	if (i == leaf.childCount || leaf.children[i] != pageAddr) {
		printf("Error: Tried to split a page that isn't where its keys lead\n");
		return false;
	}
	ordering_key bound = leaf.keys[i];
	leaf.keys[i] = lowerKey;
	markNode(leafAddr, &leaf, t);
	addPage(&leaf, leafAddr, compareOrderingKeys(newKey, bound) > 0 ? newKey : bound, newAddr, t);
	return true;
}

/*
stores a page whose records have been changed in memory
if they still fit in a page, this is markPage(). If a record was made larger and they don't, the page is
split, into two pages as evenly filled as they can be or, if no two pages can hold its records, into as
many as it takes
@param p - the page; if it's split, it is left holding only the records that stay at pageAddr
@param splitOut - set to whether the page was split, which moves records to other pages
@return false if a single record is too large for a page, or the tree couldn't be updated
*/
bool storePage(slotted_page* p, address pageAddr, table* t, bool* splitOut) {
	*splitOut = false;
	// if the records still fit in the page, mark it and return
	if (SPFits(p)) {
		markPage(pageAddr, p, t);
		return true;
	}
	// a record too large for a page even alone fails here, before the tree is changed
	for (uint32_t i = 0; i < p->header.numRecords; i++) {
		if (SPRecordBytes(p, p->slots[i].size, p->slots[i].len) > pageCapacity(t)) return false;
	}
	*splitOut = true;
	slotted_page* current = p;      // the page still being cut down to size
	address currentAddr = pageAddr;
	slotted_page* made = NULL;      // the newest page this made, if current is one
	bool ok = true;
	while (ok && !SPFits(current)) {
		uint32_t keep;
		bool unused;
		if (!chooseSplit(current, false, 0, 0, &keep, &unused)) {
			// no even split fits, so the page keeps as many of its records as it has room for
			uint32_t room = 0;
			for (keep = 0; keep < current->header.numRecords; keep++) {
				room += SPRecordBytes(current, current->slots[keep].size, current->slots[keep].len);
				if (room > current->header.arrCap) break;
			}
			if (keep == 0) { // the first record doesn't fit in a page even alone
				ok = false;
				break;
			}
		}
		slotted_page* upper = newPage(t);
		address upperAddr = allocPage(t);
		SPSplit(current, upper, keep);
		markPage(currentAddr, current, t);
		ok = addPageAfter(currentAddr, current->header.maxKey, upperAddr, upper->header.maxKey, t);
		if (made) {
			freeSPage(made);
			free(made);
		}
		current = made = upper;
		currentAddr = upperAddr;
	}
	if (ok) markPage(currentAddr, current, t);
	if (made) {
		freeSPage(made);
		free(made);
	}
	return ok;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// B+TREE API

/*
finds the page that covers the record's key, checks that the key isn't already in it, and inserts the
record, splitting the page if it's full
0 = inserted, 1 = key already exists, 2 = failed, 3 = the record is too large to fit in a page
*/
int insertRecord(sp_record* record, ordering_key key, table* t) {
	// a page whose records can't be divided into two pages that fit, along with the new one, is first split
	// where the new record belongs. The second time round, the record is at one end of its page
	for (int attempt = 0; attempt < 3; attempt++) {
		node leaf = {0};
		address leafAddr = findLeaf(key, t, &leaf);
		if (!leafAddr || leaf.childCount == 0) return 2;
		// a key above every key in the tree goes at the end of the last page
		uint32_t i = findPageInLeaf(&leaf, key);
		bool pastEnd = i == leaf.childCount;
		if (pastEnd) i = leaf.childCount - 1;
		address pageAddr = leaf.children[i];
		slotted_page p = {0};
		if (!readPage(pageAddr, &p, t)) return 2;
		// the room the record takes in one of this table's pages: too much, if it's more than a whole page has
		uint32_t needed = SPRecordBytes(&p, record->size, record->len);
		if (needed > pageCapacity(t)) { freeSPage(&p); return 3; }
		// if record already exists, reject insertion
		if (SPSearch(&p, key) >= 0) { freeSPage(&p); return 1; }

		if (SPHasRoom(&p, record->size, record->len)) {
			SPInsert(&p, key, *record);
			markPage(pageAddr, &p, t);
			freeSPage(&p);
			if (pastEnd) {
				// the page is now filed under its new largest key, and so is everything above it
				leaf.keys[i] = key;
				leaf.maxKey = key;
				markNode(leafAddr, &leaf, t);
				propagateMaxKeyUp(leafAddr, key, t);
			}
			return 0;
		}
		if (p.header.numRecords == 0) { freeSPage(&p); return 3; } // an empty page that still has no room for it

		// the page is full
		slotted_page* upper = newPage(t);
		address upperAddr = allocPage(t);
		uint32_t pos = SPPosition(&p, key);
		uint32_t keep;
		bool recordInUpper;
		bool inserted = true;
		if (pos == p.header.numRecords) {
			// the record goes after everything in the page, so it starts a new page and the full one is
			// left as it is: keys inserted in order fill every page completely
			SPInsert(upper, key, *record);
		} else if (chooseSplit(&p, true, pos, needed, &keep, &recordInUpper)) {
			SPSplit(&p, upper, keep);
			SPInsert(recordInUpper ? upper : &p, key, *record);
		} else {
			SPSplit(&p, upper, pos);
			inserted = false;
		}
		markPage(pageAddr, &p, t);
		markPage(upperAddr, upper, t);
		bool filed = addPageAfter(pageAddr, p.header.maxKey, upperAddr, upper->header.maxKey, t);
		freeSPage(&p);
		freeSPage(upper);
		free(upper);
		if (!filed) return 2;
		if (inserted) return 0;
	}
	return 2;
}

/*
searches a table for a record
returns whether the record was found in the table
*/
bool searchRecord(ordering_key key, table* t) {
	address addr = findPage(key, t);
	if (!addr) return false;
	slotted_page p = {0};
	if (!readPage(addr, &p, t)) return false;
	int idx = SPSearch(&p, key);
	freeSPage(&p);
	return idx >= 0;
}

/*
Reads a record by key. The caller provides a page buffer that backs the returned sp_record;
the caller must call freeSPage(page) when done with the record data.
Returns {0} if the record does not exist (page is left as it was if no page covers the key).
*/
sp_record readRecord(ordering_key key, table* t, slotted_page* page) {
	address addr = findPage(key, t);
	if (!addr) return (sp_record){0};
	if (!readPage(addr, page, t)) return (sp_record){0};
	return SPRead(page, key);
}

/*
replaces the entries of the record with this key (see SPUpdate()), splitting its page if the record has
grown too large for it
@return false if there is no such record, or the record would be too large to fit in a page; nothing has
        changed, and the new entries are still the caller's. Also false if the page couldn't be stored
*/
bool updateRecord(sp_record* record, ordering_key key, table* t) {
	address addr = findPage(key, t);
	if (!addr) return false;
	slotted_page p = {0};
	if (!readPage(addr, &p, t)) return false;
	bool out = false;
	int index = SPSearch(&p, key);
	if (index >= 0 && p.slots[index].len >= record->len) {
		sp_slot slot = p.slots[index];
		uint32_t size = slot.size;
		for (uint32_t i = 0; i < record->len; i++) size += record->entries[i].size - p.entries[slot.ptr + i].size;
		bool split;
		out = SPRecordBytes(&p, size, slot.len) <= pageCapacity(t) && SPUpdate(&p, key, *record) && storePage(&p, addr, t, &split);
	}
	freeSPage(&p);
	return out;
}

/*
Deletes a record from the B+ tree.
page is a caller-provided buffer; on return it holds the post-deletion state of the page.
A page left with no records is removed from the tree, unless it is the tree's only page.
@param leafOut - set to the address of the leaf that holds the record's page, or held it if the page
                 was emptied and removed. Removing a page can rebalance the tree, so this may not be
                 the leaf the page was in before the call. Set to 0 if no page covers the key
@return true if the record was deleted or no page covers its key; false on failure, which includes the
        page that covers the key not holding it
*/
bool deleteRecord(ordering_key key, table* t, slotted_page* page, address* leafOut) {
	*leafOut = 0;
	node leaf = {0};
	address leafAddr = findLeaf(key, t, &leaf);
	if (!leafAddr) return false;
	uint32_t i = findPageInLeaf(&leaf, key);
	if (i == leaf.childCount) return true;
	*leafOut = leafAddr;
	address addr = leaf.children[i];
	if (!readPage(addr, page, t)) return false;
	bool out = SPDelete(page, key);
	if (!out) return false;
	bool onlyPage = isRoot(&leaf) && leaf.childCount == 1; // kept, so that there's always a page to insert into
	if (page->header.numRecords == 0 && !onlyPage) {
		if (!deletePage(&leaf, &leafAddr, addr, t)) return false;
		*leafOut = leafAddr;
	} else {
		markPage(addr, page, t);
	}
	return out;
}
