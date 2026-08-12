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

#include "testing.h"

// ##########################################################################################################################################
// ##########################################################################################################################################
// SHARED TEST HELPERS

/* Convenience constructors so tests can write pn(42) instead of a compound literal. */
static page_num pn(uint64_t v) {
    return (page_num){ .type = ORDERING_ULONG, .as.u64 = v };
}
static page_offset po(uint64_t v) {
    return (page_offset){ .type = ORDERING_ULONG, .as.u64 = v };
}

/* Build the full path "tables/<name>.tbl" into a heap-allocated string. */
static char* build_tbl_path(const char* name) {
    size_t len = strlen(TABLE_DIRECTORY) + strlen(name) + strlen(TABLE_EXTENSION) + 1;
    char* path = malloc(len);
    snprintf(path, len, "%s%s%s", TABLE_DIRECTORY, name, TABLE_EXTENSION);
    return path;
}

/* Return true if the table file for <name> currently exists on disk. */
static bool tbl_file_exists(const char* name) {
    char* path = build_tbl_path(name);
    FILE* f = fopen(path, "rb");
    free(path);
    if (!f) return false;
    fclose(f);
    return true;
}

/* Close a table's file handle and free its memory without deleting the file. */
static void close_table_keep_file(table* t) {
    fclose(t->source);
    freeTable(t);
}

/* Compute the file offset at which the current page/node stripe ends — the
   boundary at which allocPage/allocNode triggers a new stripe. Page and node
   addressing are independent (see currentPageStripeStart/
   currentNodeStripeStart in tableIO.c), so these are two separate
   computations, not a single shared boundary. */
static uint64_t page_stripe_boundary(table* t) {
    return (uint64_t)currentPageStripeStart(t) + (uint64_t)t->pageStripeLen * t->pageSize;
}
static uint64_t node_stripe_boundary(table* t) {
    return (uint64_t)currentNodeStripeStart(t) + (uint64_t)t->nodeStripeLen * t->nodeSize;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// SLOTTED PAGE TESTS


/* Allocate a fresh slotted_page with room for up to 64 slots and 256 entries. */
static slotted_page* make_test_page(void) {
    slotted_page* p = calloc(1, sizeof(slotted_page));
    p->header.pageNum    = pn(1);
    p->header.usedData   = 0;
    p->header.numRecords = 0;
    p->header.numEntries = 0;
    p->header.arrCap     = 8192;
    p->header.maxSlots   = 64;
    p->header.maxEntries = 256;
    p->slots   = calloc(64,  sizeof(sp_slot));
    p->entries = calloc(256, sizeof(entry));
    return p;
}

/*
Free a test page.  SPDelete already frees data strings for deleted
entries, so we only iterate up to the current numEntries count.
*/
static void free_test_page(slotted_page* p) {
    for (uint32_t i = 0; i < p->header.numEntries; i++) {
        free(p->entries[i].data);
    }
    free(p->slots);
    free(p->entries);
    free(p);
}

/* Build an entry whose data is a heap-allocated copy of str. */
static entry make_entry(const char* str, datatype t) {
    entry e;
    e.type = t;
    e.size = (uint32_t)(strlen(str) + 1);
    e.data = malloc(e.size);
    strcpy(e.data, str);
    return e;
}

/* Wrap an array of entries into an sp_record (does not copy). */
static sp_record make_sp_record(entry* entries, uint32_t len) {
    sp_record r = { entries, len, 0 };
    return r;
}

/*
test_page_add: add a single record and verify the slot directory and
entry array are updated correctly.
*/
void test_page_add(void) {
    printf("  test_page_add ... ");
    slotted_page* p = make_test_page();

    entry es[] = { make_entry("Alice", T_STRING), make_entry("30", T_INT) };
    bool ok = SPInsert(p, po(10), make_sp_record(es, 2));

    assert(ok);
    assert(p->header.numRecords == 1);
    assert(p->header.numEntries == 2);
    assert(compareOffsets(p->slots[0].ID, po(10)) == 0);
    assert(p->slots[0].ptr == 0);
    assert(p->slots[0].len == 2);

    free_test_page(p);
    printf("PASS\n");
}

/*
test_page_read: add a record then read it back; also verify that reading
a non-existent ID returns the sentinel {NULL, 0}.
*/
void test_page_read(void) {
    printf("  test_page_read ... ");
    slotted_page* p = make_test_page();

    entry es[] = { make_entry("Bob", T_STRING), make_entry("25", T_INT) };
    SPInsert(p, po(5), make_sp_record(es, 2));

    sp_record result = SPRead(p, po(5));
    assert(result.len == 2);
    assert(result.entries != NULL);
    assert(strcmp(result.entries[0].data, "Bob") == 0);
    assert(strcmp(result.entries[1].data, "25")  == 0);

    sp_record missing = SPRead(p, po(99));
    assert(missing.entries == NULL);
    assert(missing.len == 0);

    free_test_page(p);
    printf("PASS\n");
}

/*
test_page_delete: add two records, delete one, and confirm:
  - numRecords decremented
  - the deleted record is no longer readable
  - the remaining record is still intact
*/
void test_page_delete(void) {
    printf("  test_page_delete ... ");
    slotted_page* p = make_test_page();

    entry es1[] = { make_entry("Carol", T_STRING) };
    entry es2[] = { make_entry("Dave",  T_STRING) };
    SPInsert(p, po(1), make_sp_record(es1, 1));
    SPInsert(p, po(2), make_sp_record(es2, 1));
    assert(p->header.numRecords == 2);

    bool ok = SPDelete(p, po(1));
    assert(ok);
    assert(p->header.numRecords == 1);

    sp_record gone = SPRead(p, po(1));
    assert(gone.entries == NULL);

    sp_record kept = SPRead(p, po(2));
    assert(kept.len == 1);
    assert(strcmp(kept.entries[0].data, "Dave") == 0);

    /* SPDelete already freed Carol's data; only Dave's remains. */
    free_test_page(p);
    printf("PASS\n");
}

/*
test_page_update: add a record, update one field, then read back and
confirm the new value is stored.
*/
void test_page_update(void) {
    printf("  test_page_update ... ");
    slotted_page* p = make_test_page();

    entry es[] = { make_entry("Eve", T_STRING), make_entry("20", T_INT) };
    SPInsert(p, po(7), make_sp_record(es, 2));

    /* Replace both entries with fresh heap strings. */
    entry new_es[] = { make_entry("Eve", T_STRING), make_entry("21", T_INT) };
    bool ok = SPUpdate(p, po(7), make_sp_record(new_es, 2));
    assert(ok);

    sp_record result = SPRead(p, po(7));
    assert(strcmp(result.entries[0].data, "Eve") == 0);
    assert(strcmp(result.entries[1].data, "21")  == 0);

    free_test_page(p);
    printf("PASS\n");
}

/*
test_page_multiple_records: insert records out of ID order and verify:
  - the slot directory is kept sorted by ID
  - every record is readable by its original ID
*/
void test_page_multiple_records(void) {
    printf("  test_page_multiple_records ... ");
    slotted_page* p = make_test_page();

    entry e30[] = { make_entry("Charlie", T_STRING) };
    entry e10[] = { make_entry("Alice",   T_STRING) };
    entry e20[] = { make_entry("Bob",     T_STRING) };

    SPInsert(p, po(30), make_sp_record(e30, 1));
    SPInsert(p, po(10), make_sp_record(e10, 1));
    SPInsert(p, po(20), make_sp_record(e20, 1));

    assert(p->header.numRecords == 3);

    /* Slot array must be sorted by ID after each insertion. */
    assert(compareOffsets(p->slots[0].ID, po(10)) == 0);
    assert(compareOffsets(p->slots[1].ID, po(20)) == 0);
    assert(compareOffsets(p->slots[2].ID, po(30)) == 0);

    sp_record r = SPRead(p, po(20));
    assert(r.len == 1);
    assert(strcmp(r.entries[0].data, "Bob") == 0);

    /* Delete the middle record and verify neighbours are still accessible. */
    SPDelete(p, po(20));
    assert(p->header.numRecords == 2);
    assert(SPRead(p, po(20)).entries == NULL);
    assert(strcmp(SPRead(p, po(10)).entries[0].data, "Alice")   == 0);
    assert(strcmp(SPRead(p, po(30)).entries[0].data, "Charlie") == 0);

    free_test_page(p);
    printf("PASS\n");
}

/* Run all slotted-page tests. */
void test_page(void) {
    printf("=== Slotted Page Tests ===\n");
    test_page_add();
    test_page_read();
    test_page_delete();
    test_page_update();
    test_page_multiple_records();
    printf("=== All slotted page tests passed ===\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// TABLEIO TESTS
//
// NOTE: test_page_roundtrip, test_node_roundtrip, test_page_write_lifo, and
// test_node_write_lifo verify field values that pass through writePage/writeNode.

// writeMeta is non-static but not in tableIO.h; forward-declare it here
bool writeMeta(FILE* file, table* t);

#define TEST_PAGE_SIZE 512
#define TEST_NODE_SIZE (49 + M_GLOBAL * (8 + PAGE_NUM_DISK_SIZE))

/* Create an in-memory table backed by a fresh tmpfile. */
static table make_test_table(void) {
    table t;
    memset(&t, 0, sizeof(t));
    t.source = tmpfile();
    t.cursor = 0;
    t.metalen = METALEN * 4;
    t.pageStripes = 1;
    t.nodeStripes = 1;
    t.pageStripeLen = 8;
    t.nodeStripeLen = 4;
    t.pageNodeRatio = 2;
    t.pageSize = TEST_PAGE_SIZE;
    t.nodeSize = TEST_NODE_SIZE;
    // layout is [node stripe][pageNodeRatio page stripes] per unit — node
    // stripe 1 starts immediately after the header, page stripe 1 right
    // after that (matches createTable(); see currentPageStripeStart/
    // currentNodeStripeStart in tableIO.c)
    t.nodeFree = t.metalen;
    t.pageFree = (uint64_t)t.metalen + (uint64_t)t.nodeStripeLen * t.nodeSize;
    t.root = 0;
    t.M = M_GLOBAL;
    // Inline dirty-table initialisation (setStacks is static in tableIO.c)
    initAddrTable(&t.pageDirty);
    initAddrTable(&t.nodeDirty);
    initAddrTable(&t.delete);
    writeMeta(t.source, &t);
    return t;
}

/* Free dirty-table heap copies and close the backing file. */
static void free_test_table(table* t) {
    for (int i = 0; i < t->pageDirty.capacity; i++)
        if (t->pageDirty.entries[i].key && t->pageDirty.entries[i].value) free(t->pageDirty.entries[i].value);
    freeAddrTable(&t->pageDirty);
    for (int i = 0; i < t->nodeDirty.capacity; i++)
        if (t->nodeDirty.entries[i].key && t->nodeDirty.entries[i].value) free(t->nodeDirty.entries[i].value);
    freeAddrTable(&t->nodeDirty);
    freeAddrTable(&t->delete);
    fclose(t->source);
}

/* Build a page with the given pageNum, zeroed slot/entry arrays. */
static slotted_page* make_io_page(page_num pageNum) {
    slotted_page* p = calloc(1, sizeof(slotted_page));
    p->header.pageNum    = pageNum;
    p->header.usedData   = 128;
    p->header.numRecords = 3;
    p->header.numEntries = 0;
    p->header.arrCap     = 200;
    p->header.maxEntries = 10;
    p->header.maxSlots   = 5;
    p->slots   = calloc(10, sizeof(sp_slot));
    p->entries = calloc(10, sizeof(entry));
    return p;
}

static void free_io_page(slotted_page* p) {
    free(p->slots);
    free(p->entries);
    free(p);
}

// --- writeMeta / loadMeta ---

/*
Write modified metadata fields to a table file and reload it via loadTable,
verifying every field survives the round-trip through writeMeta / loadMeta.
*/
void test_meta_roundtrip(void) {
    printf("  test_meta_roundtrip ... ");
    table* t = createTable("_mt_rt");
    assert(t != NULL);
    t->pageStripes    = 3;
    t->nodeStripes    = 2;
    t->pageStripeLen  = 6;
    t->nodeStripeLen  = 4;
    t->pageNodeRatio  = 3;
    t->pageFree       = 0x00002000;
    t->nodeFree       = 0x00006000;
    t->root           = 0x0000A000;
    writeMeta(t->source, t);
    fclose(t->source);
    freeTable(t);

    table* t2 = calloc(1, sizeof(table));
    bool ok = loadTable("_mt_rt", t2);
    assert(ok);
    assert(t2->pageStripes   == 3);
    assert(t2->nodeStripes   == 2);
    assert(t2->pageStripeLen == 6);
    assert(t2->nodeStripeLen == 4);
    assert(t2->pageNodeRatio == 3);
    assert(t2->pageFree      == 0x00002000);
    assert(t2->nodeFree      == 0x00006000);
    assert(t2->root          == 0x0000A000);
    assert(t2->M             == M_GLOBAL);
    deleteTable(t2);
    printf("PASS\n");
}

/*
64-bit addresses with non-zero upper 32 bits must survive the
writeMeta / loadMeta round-trip (tests the high/low 32-bit split logic).
*/
void test_meta_large_addr(void) {
    printf("  test_meta_large_addr ... ");
    table* t = createTable("_mt_la");
    assert(t != NULL);
    t->pageFree = 0x0000000100000000ULL;
    t->nodeFree = 0x00000001ABCDEF12ULL;
    t->root     = 0x00000002FEEDBEEFULL;
    writeMeta(t->source, t);
    fclose(t->source);
    freeTable(t);

    table* t2 = calloc(1, sizeof(table));
    bool ok = loadTable("_mt_la", t2);
    assert(ok);
    assert(t2->pageFree == 0x0000000100000000ULL);
    assert(t2->nodeFree == 0x00000001ABCDEF12ULL);
    assert(t2->root     == 0x00000002FEEDBEEFULL);
    deleteTable(t2);
    printf("PASS\n");
}

/*
loadTable must reject any .tbl file whose first four bytes are not MAGIC.
(Tests the magic-number check in validateTableFile via the public loadTable path.)
*/
void test_meta_bad_magic(void) {
    printf("  test_meta_bad_magic ... ");
    // Use createTable to guarantee the tables/ directory exists, then remove it
    table* dir = createTable("_mt_dir");
    if (dir) deleteTable(dir);

    FILE* f = fopen("tables/_mt_badmag.tbl", "wb");
    assert(f != NULL);
    uint32_t buf[METALEN];
    memset(buf, 0, sizeof(buf));  // magic = 0 != MAGIC
    fwrite(buf, 4, METALEN, f);
    fclose(f);

    table* t2 = calloc(1, sizeof(table));
    bool ok = loadTable("_mt_badmag", t2);
    assert(!ok);
    free(t2);

    remove("tables/_mt_badmag.tbl");
    printf("PASS\n");
}

// --- markPage ---

/*
Marking the same address twice must not increase the dirty count.
*/
void test_mark_page_dedup(void) {
    printf("  test_mark_page_dedup ... ");
    table t = make_test_table();
    slotted_page* p = make_io_page(pn(1));

    markPage(100, p, &t);
    assert(t.pageDirty.count == 1);

    markPage(100, p, &t);                // same address — should be a no-op
    assert(t.pageDirty.count == 1);

    markPage(200, p, &t);                // different address — should be added
    assert(t.pageDirty.count == 2);

    free_io_page(p);
    free_test_table(&t);
    printf("PASS\n");
}

/*
markPage stores a private heap copy of the page; mutating the original
after marking must not change the copy held in the dirty stack.
*/
void test_mark_page_snapshot(void) {
    printf("  test_mark_page_snapshot ... ");
    table t = make_test_table();
    slotted_page* p = make_io_page(pn(7));

    markPage(300, p, &t);
    p->header.pageNum = pn(99);          // mutate original after mark

    // The snapshot in the dirty table should still have pageNum == 7
    slotted_page* snap = (slotted_page*)findAddrTable(300, &t.pageDirty);
    assert(comparePageNums(snap->header.pageNum, pn(7)) == 0);

    free_io_page(p);
    free_test_table(&t);
    printf("PASS\n");
}

/*
Pushing more pages than the initial dirty-table capacity must cause the
table to grow without losing any entries.
*/
void test_mark_page_growth(void) {
    printf("  test_mark_page_growth ... ");
    table t = make_test_table();

    // Shrink the capacity to 4 to trigger growth after just a few marks
    freeAddrTable(&t.pageDirty);
    t.pageDirty.capacity = 4;
    t.pageDirty.count    = 0;
    t.pageDirty.entries  = calloc(4, sizeof(addr_entry));

    slotted_page* p = make_io_page(pn(1));
    for (int i = 0; i < 6; i++)         // 6 > initial capacity of 4
        markPage((address)(1000 + i), p, &t);

    assert(t.pageDirty.count    == 6);
    assert(t.pageDirty.capacity >  4);  // table was reallocated

    free_io_page(p);
    free_test_table(&t);
    printf("PASS\n");
}

// --- markNode ---

void test_mark_node_dedup(void) {
    printf("  test_mark_node_dedup ... ");
    table t = make_test_table();
    node n = {0};
    n.childCount = 1;

    markNode(500, &n, &t);
    assert(t.nodeDirty.count == 1);

    markNode(500, &n, &t);
    assert(t.nodeDirty.count == 1);

    markNode(600, &n, &t);
    assert(t.nodeDirty.count == 2);

    free_test_table(&t);
    printf("PASS\n");
}

void test_mark_node_snapshot(void) {
    printf("  test_mark_node_snapshot ... ");
    table t = make_test_table();
    node n = {0};
    n.childCount = 3;

    markNode(700, &n, &t);
    n.childCount = 99;                   // mutate after mark

    node* snap = (node*)findAddrTable(700, &t.nodeDirty);
    assert(snap->childCount == 3);

    free_test_table(&t);
    printf("PASS\n");
}

void test_mark_node_growth(void) {
    printf("  test_mark_node_growth ... ");
    table t = make_test_table();

    freeAddrTable(&t.nodeDirty);
    t.nodeDirty.capacity = 3;
    t.nodeDirty.count    = 0;
    t.nodeDirty.entries  = calloc(3, sizeof(addr_entry));

    node n = {0};
    for (int i = 0; i < 5; i++)
        markNode((address)(2000 + i), &n, &t);

    assert(t.nodeDirty.count    == 5);
    assert(t.nodeDirty.capacity >  3);

    free_test_table(&t);
    printf("PASS\n");
}

// --- allocPage / allocNode ---

/*
Each call to allocPage must return the previous pageFree and advance it
by exactly pageSize.
*/
void test_alloc_page(void) {
    printf("  test_alloc_page ... ");
    table t = make_test_table();
    address base = t.pageFree;

    address a1 = allocPage(&t);
    address a2 = allocPage(&t);
    address a3 = allocPage(&t);

    assert(a1 == base);
    assert(a2 == base + TEST_PAGE_SIZE);
    assert(a3 == base + 2 * TEST_PAGE_SIZE);

    free_test_table(&t);
    printf("PASS\n");
}

/*
When pageFree reaches the end-of-file boundary, allocPage must trigger a
new page stripe and return the boundary address as the first slot of that
stripe.
*/
void test_alloc_page_stripe(void) {
    printf("  test_alloc_page_stripe ... ");
    table t = make_test_table();

    uint64_t boundary = page_stripe_boundary(&t);
    t.pageFree = boundary;
    int old_stripes = t.pageStripes;

    address addr = allocPage(&t);

    assert(addr           == boundary);
    assert(t.pageStripes  == old_stripes + 1);

    free_test_table(&t);
    printf("PASS\n");
}

/*
After a stripe boundary is crossed, the next allocPage should advance
pageFree by one more pageSize.
*/
void test_alloc_page_after_stripe(void) {
    printf("  test_alloc_page_after_stripe ... ");
    table t = make_test_table();

    uint64_t boundary = page_stripe_boundary(&t);
    t.pageFree = boundary;

    allocPage(&t);                       // crosses boundary, pageStripes++
    address next = allocPage(&t);       // should be boundary + pageSize

    assert(next == boundary + TEST_PAGE_SIZE);

    free_test_table(&t);
    printf("PASS\n");
}

void test_alloc_node(void) {
    printf("  test_alloc_node ... ");
    table t = make_test_table();
    address base = t.nodeFree;

    address a1 = allocNode(&t);
    address a2 = allocNode(&t);
    address a3 = allocNode(&t);

    assert(a1 == base);
    assert(a2 == base + TEST_NODE_SIZE);
    assert(a3 == base + 2 * TEST_NODE_SIZE);

    free_test_table(&t);
    printf("PASS\n");
}

void test_alloc_node_stripe(void) {
    printf("  test_alloc_node_stripe ... ");
    table t = make_test_table();

    uint64_t boundary = node_stripe_boundary(&t);
    t.nodeFree = boundary;
    int old_stripes = t.nodeStripes;

    address addr = allocNode(&t);

    // Unlike pages (pageNodeRatio stripes per unit), a node stripe is always
    // exactly 1 per unit, so crossing a node-stripe boundary always starts a
    // brand new unit — the new stripe does NOT start at the old stripe's
    // end (boundary), it starts after that whole unit's page-stripe
    // allotment too. Check against the freshly-recomputed current node
    // stripe start rather than the pre-rollover boundary value.
    assert(t.nodeStripes == old_stripes + 1);
    assert(addr          == currentNodeStripeStart(&t));

    free_test_table(&t);
    printf("PASS\n");
}

void test_alloc_node_after_stripe(void) {
    printf("  test_alloc_node_after_stripe ... ");
    table t = make_test_table();

    uint64_t boundary = node_stripe_boundary(&t);
    t.nodeFree = boundary;

    address first = allocNode(&t);      // crosses boundary, nodeStripes++
    address next  = allocNode(&t);      // should be first + nodeSize

    assert(next == first + TEST_NODE_SIZE);

    free_test_table(&t);
    printf("PASS\n");
}

// --- writeNextPage / readPage ---

/*
writeNextPage on an empty queue must be a no-op (no crash, count stays 0).
*/
void test_page_write_empty_queue(void) {
    printf("  test_page_write_empty_queue ... ");
    table t = make_test_table();

    assert(t.pageDirty.count == 0);
    writeNextPage(&t);
    assert(t.pageDirty.count == 0);

    free_test_table(&t);
    printf("PASS\n");
}

/*
Mark a page, write it, read it back, and verify every header field
survives the round-trip.
*/
void test_page_roundtrip(void) {
    printf("  test_page_roundtrip ... ");
    table t = make_test_table();
    slotted_page* p = make_io_page(pn(42));
    p->header.usedData   = 128;
    p->header.numRecords = 3;
    p->header.arrCap     = 200;
    p->header.maxEntries = 10;
    p->header.maxSlots   = 5;

    address addr = allocPage(&t);
    markPage(addr, p, &t);
    writeNextPage(&t);
    assert(t.pageDirty.count == 0);

    slotted_page r;
    memset(&r, 0, sizeof(r));
    bool ok = readPage(addr, &r, &t);
    assert(ok);
    assert(comparePageNums(r.header.pageNum, pn(42)) == 0);
    assert(r.header.usedData   == 128);
    assert(r.header.numRecords == 3);
    assert(r.header.arrCap     == 200);
    assert(r.header.maxEntries == 10);
    assert(r.header.maxSlots   == 5);

    free(r.slots);
    free(r.entries);
    free_io_page(p);
    free_test_table(&t);
    printf("PASS\n");
}

/*
Mark 3 pages, then call writeNextPage 3 times. The dirty table has no
inherent order, so which page drains on which call is unspecified — this
verifies all 3 end up correctly written regardless of drain order.
*/
void test_page_write_lifo(void) {
    printf("  test_page_write_lifo ... ");
    table t = make_test_table();

    slotted_page* p1 = make_io_page(pn(1));
    slotted_page* p2 = make_io_page(pn(2));
    slotted_page* p3 = make_io_page(pn(3));
    address a1 = allocPage(&t);
    address a2 = allocPage(&t);
    address a3 = allocPage(&t);

    markPage(a1, p1, &t);
    markPage(a2, p2, &t);
    markPage(a3, p3, &t);
    assert(t.pageDirty.count == 3);

    writeNextPage(&t);
    assert(t.pageDirty.count == 2);
    writeNextPage(&t);
    assert(t.pageDirty.count == 1);
    writeNextPage(&t);
    assert(t.pageDirty.count == 0);

    // Each address should now contain its corresponding page
    slotted_page r1 = {0}, r2 = {0}, r3 = {0};
    assert(readPage(a1, &r1, &t) && comparePageNums(r1.header.pageNum, pn(1)) == 0);
    assert(readPage(a2, &r2, &t) && comparePageNums(r2.header.pageNum, pn(2)) == 0);
    assert(readPage(a3, &r3, &t) && comparePageNums(r3.header.pageNum, pn(3)) == 0);

    free(r1.slots); free(r1.entries);
    free(r2.slots); free(r2.entries);
    free(r3.slots); free(r3.entries);
    free_io_page(p1); free_io_page(p2); free_io_page(p3);
    free_test_table(&t);
    printf("PASS\n");
}

// --- writeNextNode / readNode ---

void test_node_write_empty_queue(void) {
    printf("  test_node_write_empty_queue ... ");
    table t = make_test_table();

    assert(t.nodeDirty.count == 0);
    writeNextNode(&t);
    assert(t.nodeDirty.count == 0);

    free_test_table(&t);
    printf("PASS\n");
}

/*
Mark a node, write it, read it back, and verify every field.
*/
void test_node_roundtrip(void) {
    printf("  test_node_roundtrip ... ");
    table t = make_test_table();

    node n = {0};
    n.childCount  = 3;
    n.maxKey      = pn(77);
    n.isLeaf      = true;
    n.parent      = 1000;
    n.prev        = 2000;
    n.next        = 3000;
    n.children[0] = 100;
    n.children[1] = 200;
    n.children[2] = 300;
    n.keys[0]     = pn(10);
    n.keys[1]     = pn(20);
    n.keys[2]     = pn(30);

    address addr = allocNode(&t);
    markNode(addr, &n, &t);
    writeNextNode(&t);
    assert(t.nodeDirty.count == 0);

    node r = {0};
    bool ok = readNode(addr, &r, &t);
    assert(ok);
    assert(r.childCount == 3);
    assert(comparePageNums(r.maxKey, pn(77)) == 0);
    assert(r.isLeaf     == true);
    assert(r.parent     == 1000);
    assert(r.prev       == 2000);
    assert(r.next       == 3000);
    assert(r.children[0] == 100);
    assert(r.children[1] == 200);
    assert(r.children[2] == 300);
    assert(comparePageNums(r.keys[0], pn(10)) == 0);
    assert(comparePageNums(r.keys[1], pn(20)) == 0);

    free_test_table(&t);
    printf("PASS\n");
}

/*
Mark 3 nodes, drain them all via writeNextNode, and verify each address
holds its node regardless of drain order.
*/
void test_node_write_lifo(void) {
    printf("  test_node_write_lifo ... ");
    table t = make_test_table();

    node n1 = {0}; n1.childCount = 1; n1.maxKey = pn(10);
    node n2 = {0}; n2.childCount = 2; n2.maxKey = pn(20);
    node n3 = {0}; n3.childCount = 3; n3.maxKey = pn(30);
    address a1 = allocNode(&t);
    address a2 = allocNode(&t);
    address a3 = allocNode(&t);

    markNode(a1, &n1, &t);
    markNode(a2, &n2, &t);
    markNode(a3, &n3, &t);
    assert(t.nodeDirty.count == 3);

    writeNextNode(&t); assert(t.nodeDirty.count == 2);
    writeNextNode(&t); assert(t.nodeDirty.count == 1);
    writeNextNode(&t); assert(t.nodeDirty.count == 0);

    node r1 = {0}, r2 = {0}, r3 = {0};
    assert(readNode(a1, &r1, &t) && comparePageNums(r1.maxKey, pn(10)) == 0);
    assert(readNode(a2, &r2, &t) && comparePageNums(r2.maxKey, pn(20)) == 0);
    assert(readNode(a3, &r3, &t) && comparePageNums(r3.maxKey, pn(30)) == 0);

    free_test_table(&t);
    printf("PASS\n");
}

/* Run all tableIO tests. */
void test_tableio(void) {
    printf("=== TableIO Tests ===\n");
    // writeMeta / loadMeta
    test_meta_roundtrip();
    test_meta_large_addr();
    test_meta_bad_magic();
    // markPage
    test_mark_page_dedup();
    test_mark_page_snapshot();
    test_mark_page_growth();
    // markNode
    test_mark_node_dedup();
    test_mark_node_snapshot();
    test_mark_node_growth();
    // allocPage
    test_alloc_page();
    test_alloc_page_stripe();
    test_alloc_page_after_stripe();
    // allocNode
    test_alloc_node();
    test_alloc_node_stripe();
    test_alloc_node_after_stripe();
    // writeNextPage / readPage
    test_page_write_empty_queue();
    test_page_roundtrip();
    test_page_write_lifo();
    // writeNextNode / readNode
    test_node_write_empty_queue();
    test_node_roundtrip();
    test_node_write_lifo();
    printf("=== All tableIO tests passed ===\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// TABLE AND TREE MANAGEMENT TESTS

// --- createTable ---

/*
createTable must create the file at tables/<name>.tbl.
*/
void test_create_table_file_exists(void) {
    printf("  test_create_table_file_exists ... ");
    table* t = createTable("mgmt_c1");
    assert(t != NULL);
    assert(tbl_file_exists("mgmt_c1"));
    deleteTable(t);
    printf("PASS\n");
}

/*
Every field in the returned table struct must be correctly initialised.
*/
void test_create_table_fields(void) {
    printf("  test_create_table_fields ... ");
    table* t = createTable("mgmt_c2");
    assert(t != NULL);
    assert(strcmp(t->name, "mgmt_c2") == 0);
    assert(t->source    != NULL);
    assert(t->pageSize  == PAGE_SIZE);
    assert(t->nodeSize  == 49 + M_GLOBAL * (8 + PAGE_NUM_DISK_SIZE));
    assert(t->M         == M_GLOBAL);
    assert(t->root      == 0);
    assert(t->metalen   == METALEN * 4);
    // layout is [node stripe][pageNodeRatio page stripes] per unit — node
    // stripe 1 starts immediately after the header, page stripe 1 right
    // after that
    assert(t->nodeFree  == (address)(METALEN * 4));
    assert(t->pageFree  == (address)(METALEN * 4)
                         + (address)t->nodeStripeLen * t->nodeSize);
    deleteTable(t);
    printf("PASS\n");
}

/*
The file written by createTable must start with the correct magic number.
*/
void test_create_table_valid_magic(void) {
    printf("  test_create_table_valid_magic ... ");
    table* t = createTable("mgmt_c3");
    assert(t != NULL);
    rewind(t->source);
    uint32_t magic = 0;
    fread(&magic, 4, 1, t->source);
    assert(magic == MAGIC);
    deleteTable(t);
    printf("PASS\n");
}

// --- loadTable ---

/*
A file produced by createTable must be loadable by loadTable with matching
metadata fields.
*/
void test_load_table_roundtrip(void) {
    printf("  test_load_table_roundtrip ... ");
    table* t = createTable("mgmt_l1");
    assert(t != NULL);
    close_table_keep_file(t);  // close without deleting

    table* t2 = calloc(1, sizeof(table));
    bool ok = loadTable("mgmt_l1", t2);
    assert(ok);
    assert(strcmp(t2->name, "mgmt_l1") == 0);
    assert(t2->pageSize == PAGE_SIZE);
    assert(t2->M        == M_GLOBAL);
    assert(t2->metalen  == METALEN * 4);
    deleteTable(t2);
    printf("PASS\n");
}

/*
loadTable must return false when the named file does not exist.
*/
void test_load_table_nonexistent(void) {
    printf("  test_load_table_nonexistent ... ");
    table t;
    bool ok = loadTable("mgmt_no_such_table_xyz", &t);
    assert(!ok);
    printf("PASS\n");
}

/*
loadTable must reject a .tbl file whose first four bytes are not MAGIC.
*/
void test_load_table_bad_magic(void) {
    printf("  test_load_table_bad_magic ... ");
    // Use createTable to ensure the tables/ directory exists, then remove it
    table* tmp = createTable("mgmt_tmpdir");
    if (tmp) deleteTable(tmp);

    char* path = build_tbl_path("mgmt_badmag");
    FILE* f = fopen(path, "wb");
    assert(f != NULL);
    uint32_t wrong = 0xDEADBEEF;
    fwrite(&wrong, 4, 1, f);
    fclose(f);
    free(path);

    table t;
    bool ok = loadTable("mgmt_badmag", &t);
    assert(!ok);

    char* cleanup = build_tbl_path("mgmt_badmag");
    remove(cleanup);
    free(cleanup);
    printf("PASS\n");
}

// --- deleteTable ---

/*
deleteTable must remove the file from disk.
*/
void test_delete_table_removes_file(void) {
    printf("  test_delete_table_removes_file ... ");
    table* t = createTable("mgmt_d1");
    assert(t != NULL);
    assert(tbl_file_exists("mgmt_d1"));
    bool ok = deleteTable(t);
    assert(ok);
    assert(!tbl_file_exists("mgmt_d1"));
    printf("PASS\n");
}

/*
deleteTable(NULL) must return false without crashing.
*/
void test_delete_table_null(void) {
    printf("  test_delete_table_null ... ");
    bool ok = deleteTable(NULL);
    assert(!ok);
    printf("PASS\n");
}

/*
After deleteTable, the same table name must no longer be loadable.
*/
void test_delete_table_not_reloadable(void) {
    printf("  test_delete_table_not_reloadable ... ");
    table* t = createTable("mgmt_d2");
    assert(t != NULL);
    char* saved = strdup(t->name);
    deleteTable(t);

    table t2;
    bool ok = loadTable(saved, &t2);
    assert(!ok);
    free(saved);
    printf("PASS\n");
}

// --- createTree ---

/*
createTree must return a non-NULL, properly initialised table.
*/
void test_create_tree_not_null(void) {
    printf("  test_create_tree_not_null ... ");
    table* t = createTree("mgmt_t1", pn(1));
    assert(t != NULL);
    assert(t->root != 0);
    deleteTree(t);
    printf("PASS\n");
}

/*
The root node written by createTree must be a leaf with one child and the
correct key and maxKey.
*/
void test_create_tree_root_node(void) {
    printf("  test_create_tree_root_node ... ");
    table* t = createTree("mgmt_t2", pn(42));
    assert(t != NULL);

    node n = {0};
    bool ok = readNode(t->root, &n, t);
    assert(ok);
    assert(n.isLeaf     == true);
    assert(n.childCount == 1);
    assert(comparePageNums(n.keys[0], pn(42)) == 0);
    assert(comparePageNums(n.maxKey,  pn(42)) == 0);
    assert(n.parent     == 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
The initial page pointed to by the root must be readable and have the
pageNum passed to createTree.
*/
void test_create_tree_initial_page(void) {
    printf("  test_create_tree_initial_page ... ");
    table* t = createTree("mgmt_t3", pn(99));
    assert(t != NULL);

    node n = {0};
    readNode(t->root, &n, t);

    slotted_page p = {0};
    bool ok = readPage(n.children[0], &p, t);
    assert(ok);
    assert(comparePageNums(p.header.pageNum, pn(99)) == 0);

    free(p.slots);
    free(p.entries);
    deleteTree(t);
    printf("PASS\n");
}

/* Run all table and tree management tests. */
void test_table_mgmt(void) {
    printf("=== Table and Tree Management Tests ===\n");
    // createTable
    test_create_table_file_exists();
    test_create_table_fields();
    test_create_table_valid_magic();
    // loadTable
    test_load_table_roundtrip();
    test_load_table_nonexistent();
    test_load_table_bad_magic();
    // deleteTable
    test_delete_table_removes_file();
    test_delete_table_null();
    test_delete_table_not_reloadable();
    // createTree
    test_create_tree_not_null();
    test_create_tree_root_node();
    test_create_tree_initial_page();
    printf("=== All table and tree management tests passed ===\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// B+ TREE INTEGRATION TESTS

/* Build a single-entry string record.  entry.size is set so writePage
   serialises the data correctly through the disk roundtrip. */
static entry make_btree_entry(const char* str) {
    entry e;
    e.type = T_STRING;
    e.size = (uint32_t)(strlen(str) + 1);
    e.data = malloc(e.size);
    strcpy(e.data, str);
    return e;
}

/* Wrap an array of entries into a record and compute r.size. */
static sp_record make_btree_record(entry* entries, uint32_t count) {
    sp_record r;
    r.entries = entries;
    r.len = count;
    r.size = 0;
    for (uint32_t i = 0; i < count; i++) r.size += entries[i].size;
    return r;
}

/* Call findAndInsert for page numbers [first, first+count). */
static void insert_pages(table* t, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        findAndInsert(pn(first + i), t);
}

/* Free every entry.data still live in p, then the slot/entry arrays. */
static void free_page_contents(slotted_page* p) {
    for (uint32_t i = 0; i < p->header.numEntries; i++)
        free(p->entries[i].data);
    free(p->slots);
    free(p->entries);
}

/* Return a tree pre-loaded with pages 1..(M_GLOBAL+1) and one split already performed. */
static table* create_split_tree(char* name) {
    table* t = createTree(name, pn(1));
    assert(t != NULL);
    insert_pages(t, 2, M_GLOBAL);  // pages 2..(M_GLOBAL+1); M_GLOBAL+1 total forces the root to split
    return t;
}

/* Find page 1, assert it exists, read it into p, and return its address. */
static address load_initial_page(table* t, slotted_page* p) {
    address addr = findPage(pn(1), t);
    assert(addr != 0);
    readPage(addr, p, t);
    return addr;
}

/* Read the root node and both of its immediate leaf children. */
static void read_root_leaves(table* t, node* root, node* left, node* right) {
    readNode(t->root, root, t);
    readNode(root->children[0], left, t);
    readNode(root->children[1], right, t);
}

// ── Group 1: find ──────────────────────────────────────────────────────────

/* findPage must return a non-zero address for the page created by createTree. */
void test_btree_find_initial(void) {
    printf("  test_btree_find_initial ... ");
    table* t = createTree("bt_fi", pn(1));
    assert(t != NULL);
    assert(findPage(pn(1), t) != 0);
    deleteTree(t);
    printf("PASS\n");
}

/* findPage must return 0 for a page number not present in the tree. */
void test_btree_find_nonexistent(void) {
    printf("  test_btree_find_nonexistent ... ");
    table* t = createTree("bt_fn", pn(1));
    assert(t != NULL);
    assert(findPage(pn(99), t) == 0);
    deleteTree(t);
    printf("PASS\n");
}

// ── Group 2: in-memory record operations ───────────────────────────────────

/*
Load the initial page from disk, add a single-entry record, and read it
back in memory without committing.  Uses single-entry records so that the
writePage/readPage entry-count convention (numRecords == numEntries) holds.
*/
void test_btree_record_add(void) {
    printf("  test_btree_record_add ... ");
    table* t = createTree("bt_ra", pn(1));
    assert(t != NULL);

    slotted_page p = {0};
    address addr = load_initial_page(t, &p);

    entry e = make_btree_entry("Alice");
    assert(SPInsert(&p, po(10), make_btree_record(&e, 1)));
    assert(p.header.numRecords == 1);

    sp_record r = SPRead(&p, po(10));
    assert(r.len == 1);
    assert(strcmp(r.entries[0].data, "Alice") == 0);

    // p.entries[0].data == e.data; free once through the entries array
    free_page_contents(&p);
    deleteTree(t);
    printf("PASS\n");
}

/* SPUpdate must replace the stored entry; old data must not leak. */
void test_btree_record_update(void) {
    printf("  test_btree_record_update ... ");
    table* t = createTree("bt_ru", pn(1));
    assert(t != NULL);

    slotted_page p = {0};
    address addr = load_initial_page(t, &p);

    entry e1 = make_btree_entry("old");
    SPInsert(&p, po(5), make_btree_record(&e1, 1));

    // SPUpdate frees the old entry data internally
    entry e2 = make_btree_entry("new");
    assert(SPUpdate(&p, po(5), make_btree_record(&e2, 1)));

    sp_record r = SPRead(&p, po(5));
    assert(strcmp(r.entries[0].data, "new") == 0);

    // e1.data freed by SPUpdate; p.entries[0].data == e2.data
    free_page_contents(&p);
    deleteTree(t);
    printf("PASS\n");
}

/* SPDelete must remove one record while leaving others intact. */
void test_btree_record_delete(void) {
    printf("  test_btree_record_delete ... ");
    table* t = createTree("bt_rd", pn(1));
    assert(t != NULL);

    slotted_page p = {0};
    address addr = load_initial_page(t, &p);

    entry e1 = make_btree_entry("Alice");
    entry e2 = make_btree_entry("Bob");
    SPInsert(&p, po(1), make_btree_record(&e1, 1));
    SPInsert(&p, po(2), make_btree_record(&e2, 1));
    assert(p.header.numRecords == 2);

    // SPDelete frees e1.data internally
    assert(SPDelete(&p, po(1)));
    assert(p.header.numRecords == 1);
    assert(SPRead(&p, po(1)).entries == NULL);
    assert(strcmp(SPRead(&p, po(2)).entries[0].data, "Bob") == 0);

    // e1.data freed by SPDelete; remaining data lives in p.entries
    free_page_contents(&p);
    deleteTree(t);
    printf("PASS\n");
}

// ── Group 3: commit ────────────────────────────────────────────────────────

/*
After marking dirty objects, commit() must drain all three stacks to zero.
*/
void test_btree_commit_drains_stacks(void) {
    printf("  test_btree_commit_drains_stacks ... ");
    table* t = createTree("bt_cds", pn(1));
    assert(t != NULL);

    // findAndInsert dirtifies the root node via markNode
    findAndInsert(pn(2), t);
    assert(t->nodeDirty.count > 0);

    commit(t);
    assert(t->pageDirty.count == 0);
    assert(t->nodeDirty.count == 0);
    assert(t->delete.count    == 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
A page modified in memory, committed, then reloaded from disk must preserve
numRecords.  Uses single-entry records so writePage/readPage counts match.
*/
void test_btree_commit_persist(void) {
    printf("  test_btree_commit_persist ... ");
    table* t = createTree("bt_cp", pn(1));
    assert(t != NULL);

    slotted_page p = {0};
    address addr = load_initial_page(t, &p);
    entry e = make_btree_entry("persist");
    SPInsert(&p, po(7), make_btree_record(&e, 1));
    markPage(addr, &p, t);

    // commit before freeing: writePage will dereference the slot/entry arrays
    commit(t);
    free_page_contents(&p);
    close_table_keep_file(t);

    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_cp", t2));
    address addr2 = findPage(pn(1), t2);
    assert(addr2 != 0);

    slotted_page p2 = {0};
    assert(readPage(addr2, &p2, t2));
    assert(p2.header.numRecords == 1);

    free_page_contents(&p2);
    deleteTree(t2);
    printf("PASS\n");
}

/*
After commit, an object staged via markDelete must have its first byte set
to 2 on disk, making a subsequent readPage on that address return false.
*/
void test_btree_commit_delete_persist(void) {
    printf("  test_btree_commit_delete_persist ... ");
    table* t = createTree("bt_cdp", pn(1));
    assert(t != NULL);

    address pageAddr = findPage(pn(1), t);
    assert(pageAddr != 0);

    markDelete(pageAddr, t);
    commit(t);

    // The first byte at pageAddr on disk must now be 2 (garbage marker)
    fseek(t->source, (long)pageAddr, SEEK_SET);
    unsigned char firstByte = 0;
    fread(&firstByte, 1, 1, t->source);
    assert(firstByte == 2);

    // readPage must reject the deleted address
    slotted_page p = {0};
    bool ok = readPage(pageAddr, &p, t);
    assert(!ok);

    deleteTree(t);
    printf("PASS\n");
}

// ── Group 4: insert and split ──────────────────────────────────────────────

/*
findAndInsert on a new page number must add a child to the root node.
*/
void test_btree_insert_new_page(void) {
    printf("  test_btree_insert_new_page ... ");
    table* t = createTree("bt_inp", pn(1));
    assert(t != NULL);

    node root = {0};
    readNode(t->root, &root, t);
    assert(root.childCount == 1);

    findAndInsert(pn(2), t);

    readNode(t->root, &root, t);
    assert(root.childCount == 2);
    assert(comparePageNums(root.keys[1], pn(2)) == 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
findAndInsert on a page number that already exists must not increase
childCount.
*/
void test_btree_insert_existing_page(void) {
    printf("  test_btree_insert_existing_page ... ");
    table* t = createTree("bt_iep", pn(1));
    assert(t != NULL);

    findAndInsert(pn(2), t);
    node root = {0};
    readNode(t->root, &root, t);
    uint32_t before = root.childCount;

    findAndInsert(pn(2), t);  // duplicate

    readNode(t->root, &root, t);
    assert(root.childCount == before);

    deleteTree(t);
    printf("PASS\n");
}

/*
Inserting M+1 pages must split the root leaf into an internal node with two
leaf children. The leaf pre-emptively splits once it reaches M_GLOBAL
children (isNodeFull); splitNode hands the first M_GLOBAL-HALF_M pages to the
left half and the rest to the right half, and the (M_GLOBAL+1)-th page (the
one that triggered the split) is then routed to whichever half covers it —
always the right half here, since pages are inserted in increasing order.
*/
void test_btree_split_structure(void) {
    printf("  test_btree_split_structure ... ");
    table* t = create_split_tree("bt_ss");

    node root = {0}, left = {0}, right = {0};
    read_root_leaves(t, &root, &left, &right);
    assert(!root.isLeaf);
    assert(root.childCount == 2);
    assert(left.isLeaf  && right.isLeaf);
    assert(left.childCount  == M_GLOBAL - HALF_M);  // pages 1..(M_GLOBAL-HALF_M)
    assert(right.childCount == HALF_M + 1);         // remaining pages, plus the one that triggered the split

    deleteTree(t);
    printf("PASS\n");
}

/* After a split, every inserted page must still be locatable by findPage. */
void test_btree_split_find_all(void) {
    printf("  test_btree_split_find_all ... ");
    table* t = create_split_tree("bt_sfa");

    for (uint32_t i = 1; i <= M_GLOBAL + 1; i++)
        assert(findPage(pn(i), t) != 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
After a split the two leaf nodes must be correctly linked: left.next →
right, right.prev → left, and the outer terminator fields must be 0.
*/
void test_btree_split_linked_list(void) {
    printf("  test_btree_split_linked_list ... ");
    table* t = create_split_tree("bt_sll");

    node root = {0}, left = {0}, right = {0};
    read_root_leaves(t, &root, &left, &right);
    address leftAddr  = root.children[0];
    address rightAddr = root.children[1];

    assert(left.next   == rightAddr);
    assert(right.prev  == leftAddr);
    assert(left.prev   == 0);
    assert(right.next  == 0);

    deleteTree(t);
    printf("PASS\n");
}

// ── Group 5: page deletion and tree rebalancing ────────────────────────────

/*
findAndDelete must remove a page from the tree; findPage for that number
must then return 0 while all other pages remain accessible.
*/
void test_btree_delete_page(void) {
    printf("  test_btree_delete_page ... ");
    table* t = createTree("bt_dp", pn(1));
    assert(t != NULL);

    findAndInsert(pn(2), t);
    findAndInsert(pn(3), t);

    assert(findAndDelete(pn(2), t));
    assert(findPage(pn(2), t) == 0);
    assert(findPage(pn(1), t) != 0);
    assert(findPage(pn(3), t) != 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
When a leaf node sits at exactly HALF_M children and its next sibling has
more than HALF_M, deleting a page must trigger a borrow rather than a merge.

Setup: create_split_tree gives left=[1..leftCount] right=[leftCount+1..total],
where leftCount = M_GLOBAL - HALF_M and total = M_GLOBAL + 1. leftCount
equals HALF_M for even M_GLOBAL, but HALF_M+1 for odd M_GLOBAL (M+1 splits
evenly into two HALF_M+1 halves when M is odd) — that extra "slack" child
means the first delete from left merely lands it at HALF_M (still valid, no
rebalance) rather than pushing it below HALF_M, so for odd M_GLOBAL a priming
delete is needed first to actually reach the HALF_M boundary that triggers
deletePage's rebalance check. Once left is at exactly HALF_M going into a
delete, and right still has HALF_M+1 (> HALF_M, a valid lender per
isValidBorrow), left borrows right's smallest page, yielding
left=[slack+2..leftCount+1], right=[leftCount+2..total], and the root
separator updates to leftCount+1 (left's new max).
*/
void test_btree_delete_triggers_borrow(void) {
    printf("  test_btree_delete_triggers_borrow ... ");
    table* t = create_split_tree("bt_dtb");
    uint32_t total     = M_GLOBAL + 1;
    uint32_t leftCount = M_GLOBAL - HALF_M;
    uint32_t slack     = leftCount - HALF_M;  // 0 for even M_GLOBAL, 1 for odd

    // priming deletes: absorbed by left's post-split slack above HALF_M
    // without triggering a rebalance (only needed for odd M_GLOBAL)
    for (uint32_t i = 1; i <= slack; i++)
        assert(findAndDelete(pn(i), t));

    // left is now at exactly HALF_M; this delete triggers the borrow
    assert(findAndDelete(pn(slack + 1), t));

    // Root separator key must be updated to leftCount+1 (new max of left leaf)
    node root = {0};
    readNode(t->root, &root, t);
    node left = {0};
    readNode(root.children[0], &left, t);
    node right = {0};
    readNode(root.children[1], &right, t);
    assert(comparePageNums(root.keys[0], pn(leftCount + 1)) == 0);

    // Left leaf must contain pages (slack+2)..(leftCount+1) (leftCount+1 was borrowed from right)
    assert(left.childCount == HALF_M);
    for (uint32_t i = 0; i < HALF_M; i++)
        assert(comparePageNums(left.keys[i], pn(slack + 2 + i)) == 0);

    // Right leaf must now hold only pages (leftCount+2)..total
    assert(right.childCount == HALF_M);
    for (uint32_t i = 0; i < right.childCount; i++)
        assert(comparePageNums(right.keys[i], pn(leftCount + 2 + i)) == 0);

    // Both deleted and surviving pages accessible by findPage
    for (uint32_t i = 1; i <= slack + 1; i++)
        assert(findPage(pn(i), t) == 0);
    for (uint32_t i = slack + 2; i <= total; i++)
        assert(findPage(pn(i), t) != 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
When no sibling can lend a child, deleting a page must trigger a merge and
collapse the tree to a single leaf root.

Setup: borrow test's end state (left=[slack+2..leftCount+1],
right=[leftCount+2..total], both now at exactly HALF_M children). Deleting
left's new smallest (slack+2) drops left below HALF_M with no valid borrow
target — right is at exactly HALF_M, not more than HALF_M, so isValidBorrow
fails — so left and right merge and the now-single-child internal root
collapses into the merged leaf, holding pages (slack+3)..total in order.
*/
void test_btree_delete_triggers_merge(void) {
    printf("  test_btree_delete_triggers_merge ... ");
    table* t = create_split_tree("bt_dtm");
    uint32_t total     = M_GLOBAL + 1;
    uint32_t leftCount = M_GLOBAL - HALF_M;
    uint32_t slack     = leftCount - HALF_M;  // 0 for even M_GLOBAL, 1 for odd

    for (uint32_t i = 1; i <= slack; i++)
        findAndDelete(pn(i), t);            // priming (odd M_GLOBAL only)
    findAndDelete(pn(slack + 1), t);        // triggers a borrow, per test_btree_delete_triggers_borrow
    assert(findAndDelete(pn(slack + 2), t)); // left's new smallest after borrow; triggers the merge

    uint32_t deletedCount = slack + 2;

    // After merge + root collapse the root must be a leaf holding the surviving pages
    node root = {0};
    readNode(t->root, &root, t);
    assert(root.isLeaf);
    assert(root.childCount == total - deletedCount);
    for (uint32_t i = 0; i < root.childCount; i++)
        assert(comparePageNums(root.keys[i], pn(deletedCount + 1 + i)) == 0);

    for (uint32_t i = 1; i <= deletedCount; i++)
        assert(findPage(pn(i), t) == 0);
    for (uint32_t i = deletedCount + 1; i <= total; i++)
        assert(findPage(pn(i), t) != 0);

    deleteTree(t);
    printf("PASS\n");
}

// ── Group 6: full persistence roundtrip ───────────────────────────────────

/*
Create a tree, insert pages, add a record to the initial page, commit, close,
reopen, and verify: all pages still findable; the page modification
(numRecords) survived the disk roundtrip.
*/
void test_btree_full_roundtrip(void) {
    printf("  test_btree_full_roundtrip ... ");
    table* t = create_split_tree("bt_fr");

    slotted_page p = {0};
    address addr = load_initial_page(t, &p);
    entry e = make_btree_entry("hello");
    SPInsert(&p, po(42), make_btree_record(&e, 1));
    markPage(addr, &p, t);

    commit(t);
    free_page_contents(&p);
    close_table_keep_file(t);

    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_fr", t2));

    for (uint32_t i = 1; i <= M_GLOBAL + 1; i++)
        assert(findPage(pn(i), t2) != 0);

    address addr2 = findPage(pn(1), t2);
    slotted_page p2 = {0};
    assert(readPage(addr2, &p2, t2));
    assert(p2.header.numRecords == 1);

    free_page_contents(&p2);
    deleteTree(t2);
    printf("PASS\n");
}

// ── Group 7: tree cleanup ──────────────────────────────────────────────────

/* deleteTree must remove the backing file from disk. */
void test_btree_delete_tree(void) {
    printf("  test_btree_delete_tree ... ");
    table* t = createTree("bt_dt", pn(1));
    assert(t != NULL);
    assert(tbl_file_exists("bt_dt"));
    deleteTree(t);
    assert(!tbl_file_exists("bt_dt"));
    printf("PASS\n");
}

/* After deleteTree, loadTable must fail for the same name. */
void test_btree_delete_tree_not_reloadable(void) {
    printf("  test_btree_delete_tree_not_reloadable ... ");
    table* t = createTree("bt_dtnr", pn(1));
    assert(t != NULL);
    char* name = strdup(t->name);
    deleteTree(t);
    table t2;
    assert(!loadTable(name, &t2));
    free(name);
    printf("PASS\n");
}

/* Run all B+ tree integration tests. */
void test_btree(void) {
    printf("=== B+ Tree Integration Tests ===\n");
    // find
    test_btree_find_initial();
    test_btree_find_nonexistent();
    // in-memory record operations
    test_btree_record_add();
    test_btree_record_update();
    test_btree_record_delete();
    // commit
    test_btree_commit_drains_stacks();
    test_btree_commit_persist();
    test_btree_commit_delete_persist();
    // insert and split
    test_btree_insert_new_page();
    test_btree_insert_existing_page();
    test_btree_split_structure();
    test_btree_split_find_all();
    test_btree_split_linked_list();
    // page deletion and rebalancing
    test_btree_delete_page();
    test_btree_delete_triggers_borrow();
    test_btree_delete_triggers_merge();
    // full persistence roundtrip
    test_btree_full_roundtrip();
    // cleanup
    test_btree_delete_tree();
    test_btree_delete_tree_not_reloadable();
    printf("=== All B+ tree tests passed ===\n");
}
