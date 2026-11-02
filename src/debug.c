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

#include "debug.h"
#include "chunk.h"
#include "bplus.h"
#include "value.h"

// ##########################################################################################################################################
// ##########################################################################################################################################
// SQL INTERPRETER PRETTY PRINTERS

void disassembleChunk(chunk* chunk, const char* name) {
    printf("== %s ==\n", name);

    for (int offset = 0; offset < chunk->count;) {
        offset = disassembleInstruction(chunk, offset);
    }
}

/*
prints a constant instruction
@return - new offset after value (values are 64 bits)
*/
static int constantInstruction(const char* name, chunk* chunk, int offset) {
    uint8_t constant = chunk->code[offset + 1];
    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant]);
    printf("'\n");
    return offset + 2;
}

static int simpleInstruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

int disassembleInstruction(chunk* chunk, int offset) {
    printf("%04d ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("    | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }
    uint8_t instruction = chunk->code[offset];
    switch(instruction) {
        case OP_CONSTANT:
            return constantInstruction("OP_CONSTANT", chunk, offset);
        case OP_ADD:
            return simpleInstruction("OP_ADD", offset);
        case OP_SUBTRACT:
            return simpleInstruction("OP_SUBTRACT", offset);
        case OP_MULTIPLY:
            return simpleInstruction("OP_MULTIPLY", offset);
        case OP_DIVIDE:
            return simpleInstruction("OP_DIVIDE", offset);
        case OP_NEGATE:
            return simpleInstruction("OP_NEGATE", offset);
        default: {
            return -1;
        }
    }
}

/*
Creates a small test B+ tree on disk and sets t->root.

Structure (with M=4, page numbers incremented by 50):
                    root [internal, key=51]
                   /                      \
      left [leaf, keys=1,51]       right [leaf, keys=101,151]
        |           |                  |              |
     Page#1      Page#51           Page#101        Page#151

All nodes and pages are written to disk via the table's dirty queues.
Requires the table to have a valid source file and configured
pageSize/nodeSize/pageFree/nodeFree/metalen fields.
*/
static ordering_key keyFromU64(uint64_t v) {
    return (ordering_key){ .type = ORDERING_ULONG, .as.u64 = v };
}

static void printKey(ordering_key k) {
    if (k.type == ORDERING_STRING) printf("%s", k.as.string);
    else printf("%llu", (unsigned long long)k.as.u64);
}

void generateTestBPlusTree(table* t) {
    // Allocate disk addresses for the two leaf nodes and the root
    address leftAddr  = allocNode(t);
    address rightAddr = allocNode(t);
    address rootAddr  = allocNode(t);

    // Allocate disk addresses for the four pages
    address pageAddrs[4];
    for (int i = 0; i < 4; i++) pageAddrs[i] = allocPage(t);

    // Write pages to disk
    uint32_t pageNums[4] = {1, 51, 101, 151};
    for (int i = 0; i < 4; i++) {
        slotted_page p;
        memset(&p, 0, sizeof(p));
        p.header.minKey     = keyFromU64(pageNums[i]);
        p.header.maxKey     = keyFromU64(pageNums[i]);
        p.header.numRecords = 0;
        p.header.numEntries = 0;
        markPage(pageAddrs[i], &p, t);
    }

    // Left leaf node: pages #1 and #51
    node left;
    memset(&left, 0, sizeof(left));
    left.isLeaf        = true;
    left.parent        = rootAddr;
    left.prev          = 0;
    left.next          = rightAddr;
    left.childCount    = 2;
    left.children[0]   = pageAddrs[0];
    left.children[1]   = pageAddrs[1];
    left.keys[0]       = keyFromU64(1);
    left.keys[1]       = keyFromU64(51);
    left.maxKey        = keyFromU64(51);
    markNode(leftAddr, &left, t);

    // Right leaf node: pages #101 and #151
    node right;
    memset(&right, 0, sizeof(right));
    right.isLeaf        = true;
    right.parent        = rootAddr;
    right.prev          = leftAddr;
    right.next          = 0;
    right.childCount    = 2;
    right.children[0]   = pageAddrs[2];
    right.children[1]   = pageAddrs[3];
    right.keys[0]       = keyFromU64(101);
    right.keys[1]       = keyFromU64(151);
    right.maxKey        = keyFromU64(151);
    markNode(rightAddr, &right, t);

    // Root node: two children, one separator key
    node root;
    memset(&root, 0, sizeof(root));
    root.isLeaf        = false;
    root.parent        = 0;
    root.prev          = 0;
    root.next          = 0;
    root.childCount    = 2;
    root.children[0]   = leftAddr;
    root.children[1]   = rightAddr;
    root.keys[0]       = keyFromU64(51);   // max key of left subtree
    root.maxKey        = keyFromU64(151);
    markNode(rootAddr, &root, t);

    // Flush all dirty entries to disk
    while (t->pageDirty.count > 0) writeNextPage(t);
    while (t->nodeDirty.count > 0) writeNextNode(t);

    t->root = rootAddr;
}

void printIntArray(int* arr, int length) {
    if(arr == NULL) {
        printf("Array is NULL\n");
        return;
    }
    printf("[%d", arr[0]);
    if (length == 1) return;
    for (int i = 1; i < length; i++) {
        printf(", %d", arr[i]);
    }
    printf("]\n");
}

/*
Pretty-prints a single in-memory node struct. Leaf nodes show one key per
child; internal nodes show childCount-1 separator keys. All parent/prev/next
and child values are disk addresses printed as decimal.
*/
void printNode(node* n) {
    if (n == NULL) {
        printf("[NULL node]\n");
        return;
    }
    printf("%s node\n", n->isLeaf ? "Leaf" : "Internal");
    int keyCount = n->isLeaf ? (int)n->childCount : (int)n->childCount - 1;
    printf("  keys (%d):     ", keyCount);
    for (int i = 0; i < keyCount; i++) { printKey(n->keys[i]); printf(" "); }
    printf("\n");
    printf("  childCount:    %u\n",  n->childCount);
    printf("  maxKey:        "); printKey(n->maxKey); printf("\n");
    printf("  children:\n");
    for (int i = 0; i < (int)n->childCount; i++)
        printf("    [%d] @%llu\n", i, (unsigned long long)n->children[i]);
    printf("  parent:        @%llu\n", (unsigned long long)n->parent);
    printf("  prev:          @%llu\n", (unsigned long long)n->prev);
    printf("  next:          @%llu\n\n", (unsigned long long)n->next);
}

/*
Recursively reads nodes from disk and pretty-prints the tree indented by level.
For leaf nodes also reads and prints the page number of each child page.
*/
static void printTreeHelper(address addr, int level, table* t) {
    if (addr == 0) return;

    node n;
    memset(&n, 0, sizeof(n));
    if (!readNode(addr, &n, t)) {
        for (int i = 0; i < level; i++) printf("    ");
        printf("<unreadable node @%llu>\n", (unsigned long long)addr);
        return;
    }

    for (int i = 0; i < level; i++) printf("    ");
    int keyCount = n.isLeaf ? (int)n.childCount : (int)n.childCount - 1;
    printf("[%s @%llu] keys:", n.isLeaf ? "Leaf" : "Internal",
           (unsigned long long)addr);
    for (int i = 0; i < keyCount; i++) { printf(" "); printKey(n.keys[i]); }
    printf("  maxKey="); printKey(n.maxKey); printf("\n");

    for (int i = 0; i < (int)n.childCount; i++) {
        if (n.isLeaf) {
            for (int j = 0; j < level + 1; j++) printf("    ");
            slotted_page p;
            memset(&p, 0, sizeof(p));
            if (readPage(n.children[i], &p, t)) {
                printf("[Page @%llu] #", (unsigned long long)n.children[i]);
                printKey(p.header.maxKey); printf("\n");
                free(p.slots);
                free(p.entries);
            } else {
                printf("<unreadable page @%llu>\n",
                       (unsigned long long)n.children[i]);
            }
        } else {
            printTreeHelper(n.children[i], level + 1, t);
        }
    }
}

void printTree(table* t) {
    printf("=== B+ Tree (root @%llu) ===\n", (unsigned long long)t->root);
    if (t->root == 0) {
        printf("  (empty tree)\n");
    } else {
        printTreeHelper(t->root, 0, t);
    }
    printf("===========================\n");
}

/*
Recursively reads nodes and pages from disk, checking that every node's
parent field matches the address of the node that pointed to it, and that
every leaf's children are all readable pages. Pages don't track their own
parent (found via tree traversal instead), so there's nothing to cross-check
on the page side beyond readability.
*/
static bool checkTreePointersHelper(address addr, address expectedParent,
                                    table* t) {
    if (addr == 0) return true;

    node n;
    memset(&n, 0, sizeof(n));
    if (!readNode(addr, &n, t)) {
        printf("Error: failed to read node @%llu\n", (unsigned long long)addr);
        return false;
    }

    if (n.parent != expectedParent) {
        printf("Error: node @%llu has parent @%llu, expected @%llu\n",
               (unsigned long long)addr,
               (unsigned long long)n.parent,
               (unsigned long long)expectedParent);
        return false;
    }

    for (int i = 0; i < (int)n.childCount; i++) {
        if (n.isLeaf) {
            slotted_page p;
            memset(&p, 0, sizeof(p));
            if (!readPage(n.children[i], &p, t)) {
                printf("Error: failed to read page @%llu\n",
                       (unsigned long long)n.children[i]);
                return false;
            }
            free(p.slots);
            free(p.entries);
        } else {
            if (!checkTreePointersHelper(n.children[i], addr, t)) return false;
        }
    }
    return true;
}

bool checkTreePointers(table* t) {
    return checkTreePointersHelper(t->root, 0, t);
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// SLOTTED PAGE PRETTY-PRINTERS

static const char* datatypeStr(datatype t) {
    switch (t) {
        case T_INT:    return "INT";
        case T_STRING: return "STR";
        case T_DATE:   return "DATE";
        case T_TIME:   return "TIME";
        default:       return "???";
    }
}

/*
Prints a single entry in the form TYPE(value), e.g. STR(Alice) or INT(42).
*/
void printEntry(entry* e) {
    if (e == NULL || e->data == NULL) {
        printf("<NULL>");
        return;
    }
    printf("%s(%s)", datatypeStr(e->type), e->data);
}

/*
Prints a single slot's metadata.
*/
void printSPSlot(sp_slot* s) {
    if (s == NULL) {
        printf("<NULL slot>\n");
        return;
    }
    printf("{ ID="); printKey(s->ID); printf("  ptr=%-4u  len=%-4u }", s->ptr, s->len);
}

/*
Pretty-prints a slotted_page showing its header, the slot directory, and
the contents of every record stored in the page.

Layout printed:
  === Slotted Page #N ===
  header fields ...
  --- Slot Directory ---
  idx   ID     ptr    len
  ...
  --- Records ---
  [ID=N]  entry0, entry1, ...
*/
void printSlottedPage(slotted_page* p) {
    if (p == NULL) {
        printf("[NULL slotted_page]\n");
        return;
    }

    printf("=== Slotted Page #"); printKey(p->header.maxKey); printf(" ===\n");
    printf("  numRecords: %u\n",   p->header.numRecords);
    printf("  numEntries: %u\n",   p->header.numEntries);
    printf("  usedData  : %u bytes\n\n", p->header.usedData);

    printf("  --- Slot Directory ---\n");
    printf("  %4s  %6s  %6s  %6s\n", "idx", "ID", "ptr", "len");
    printf("  %4s  %6s  %6s  %6s\n", "----", "------", "------", "------");
    for (uint32_t i = 0; i < p->header.numRecords; i++) {
        sp_slot* s = &p->slots[i];
        printf("  %4u  ", i); printKey(s->ID); printf("  %6u  %6u\n", s->ptr, s->len);
    }

    printf("\n  --- Records ---\n");
    for (uint32_t i = 0; i < p->header.numRecords; i++) {
        sp_slot* s = &p->slots[i];
        printf("  [ID="); printKey(s->ID); printf("]  ");
        for (uint32_t j = 0; j < s->len; j++) {
            if (j > 0) printf(", ");
            printEntry(&p->entries[s->ptr + j]);
        }
        printf("\n");
    }
    printf("======================\n");
}