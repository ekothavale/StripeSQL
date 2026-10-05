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
#include "wal.h"
#include "file.h"
#include <unistd.h>
#include <sys/wait.h>

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
// NOTE: test_page_roundtrip, test_node_roundtrip, test_page_write_drains_all, and
// test_node_write_drains_all verify field values that pass through writePage/writeNode.

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
    // Inline dirty-table initialisation (initDirtyHashmaps is static in tableIO.c)
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
after marking must not change the copy held in the dirty hashmap.
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
writeNextPage on an empty dirty hashmap must be a no-op (no crash, count stays 0).
*/
void test_page_write_empty_hashmap(void) {
    printf("  test_page_write_empty_hashmap ... ");
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
void test_page_write_drains_all(void) {
    printf("  test_page_write_drains_all ... ");
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

void test_node_write_empty_hashmap(void) {
    printf("  test_node_write_empty_hashmap ... ");
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
void test_node_write_drains_all(void) {
    printf("  test_node_write_drains_all ... ");
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
/*
removeAddrTable must keep every remaining key findable (it shifts later entries of
a probe run back into the hole instead of leaving a gap that would end searches
early). 200 keys are inserted, then every third is removed.
*/
void test_addr_table_remove(void) {
    printf("  test_addr_table_remove ... ");
    addr_table at;
    initAddrTable(&at);
    int n = 200;
    for (int i = 1; i <= n; i++) insertAddrTable((address)(i * 64), (void*)(uintptr_t)i, &at);
    for (int i = 1; i <= n; i += 3) {
        void* value = NULL;
        assert(removeAddrTable((address)(i * 64), &at, &value));
        assert((uintptr_t)value == (uintptr_t)i);
    }
    assert(!removeAddrTable(64, &at, NULL));  // already removed
    for (int i = 1; i <= n; i++) {
        void* value = findAddrTable((address)(i * 64), &at);
        if ((i - 1) % 3 == 0) assert(value == NULL);
        else assert((uintptr_t)value == (uintptr_t)i);
    }
    assert(at.count == n - (n + 2) / 3);
    freeAddrTable(&at);
    printf("PASS\n");
}

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
    // dirty hashmap removal
    test_addr_table_remove();
    // allocPage
    test_alloc_page();
    test_alloc_page_stripe();
    test_alloc_page_after_stripe();
    // allocNode
    test_alloc_node();
    test_alloc_node_stripe();
    test_alloc_node_after_stripe();
    // writeNextPage / readPage
    test_page_write_empty_hashmap();
    test_page_roundtrip();
    test_page_write_drains_all();
    // writeNextNode / readNode
    test_node_write_empty_hashmap();
    test_node_roundtrip();
    test_node_write_drains_all();
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
After marking dirty objects, commit() must drain all three dirty hashmaps to zero.
*/
void test_btree_commit_drains_hashmaps(void) {
    printf("  test_btree_commit_drains_hashmaps ... ");
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

/*
commit() on a table with nothing dirty (e.g. after a read-only statement) must
write nothing: a header change made without dirtying any page or node must
not reach disk.
*/
void test_btree_commit_skips_clean(void) {
    printf("  test_btree_commit_skips_clean ... ");
    table* t = createTree("bt_csc", pn(1));
    assert(t != NULL);
    address root = t->root;

    t->root = root + 1;  // header-only change, nothing marked dirty
    assert(commit(t));
    close_table_keep_file(t);

    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_csc", t2));
    assert(t2->root == root);

    deleteTree(t2);
    printf("PASS\n");
}

/*
If writing a table fails after its transaction is committed in the log, the
table may be half-written, so commit() must exit the process; the next
recover() then finishes the commit from the log. The commit runs in a forked
child whose table stream is swapped for a read-only one so its writes fail.
*/
void test_btree_commit_apply_failure_recovers(void) {
    printf("  test_btree_commit_apply_failure_recovers ... ");
    table* t = createTree("bt_cafr", pn(1));
    assert(t != NULL);
    findAndInsert(pn(2), t);  // dirties a new page and the root node

    fflush(NULL);  // so the child doesn't re-flush output buffered before the fork
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char* path = build_tbl_path("bt_cafr");
        t->source = fopen(path, "rb");
        commit(t);  // logs and commits, then fails writing the table and exits
        _exit(0);   // unreachable if commit() behaves
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 74);

    assert(recover());         // finishes the child's commit from the log
    close_table_keep_file(t);  // drop the parent's copy of the (uncommitted) changes
    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_cafr", t2));
    assert(findPage(pn(2), t2) != 0);

    deleteTree(t2);
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

/*
Splitting a node that is not its parent's last child must register the new
sibling in the parent. Inserting in descending order routes every page after
the first split into the leftmost leaf, so each later split takes the
mid-parent path in splitUpdateParent; every page must remain findable.
*/
void test_btree_split_non_last_child(void) {
    printf("  test_btree_split_non_last_child ... ");
    uint32_t total = 3 * M_GLOBAL;
    table* t = createTree("bt_snl", pn(total));
    assert(t != NULL);

    for (uint32_t i = total - 1; i >= 1; i--)
        findAndInsert(pn(i), t);

    for (uint32_t i = 1; i <= total; i++)
        assert(findPage(pn(i), t) != 0);

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

// ── Group 5b: rebalancing anywhere in the tree ────────────────────────────

/*
Asserts the structural invariants of the subtree rooted at addr and returns how many pages it
holds: every node points back at its parent, is at least half full unless it's the root, keeps its
page numbers in order, and sits within the separator keys above it. lo is the largest page number
to the left of this subtree (hasLo is false when there is none); *maxOut receives the largest page
number in it.
*/
static uint32_t check_subtree(table* t, address addr, address parent, bool hasLo, page_num lo, page_num* maxOut) {
    node n = {0};
    assert(readNode(addr, &n, t));
    assert(n.parent == parent);
    assert(n.childCount >= 1 && n.childCount <= M_GLOBAL);
    if (parent) assert(n.childCount >= HALF_M);
    uint32_t pages = 0;
    for (uint32_t i = 0; i < n.childCount; i++) {
        assert(n.children[i] != 0);
        if (n.isLeaf) {
            if (hasLo) assert(comparePageNums(n.keys[i], lo) > 0);
            lo = n.keys[i];
            pages++;
        } else {
            page_num childMax;
            pages += check_subtree(t, n.children[i], addr, hasLo, lo, &childMax);
            // a separator key is an upper bound for everything under the child before it
            if (i < n.childCount - 1) assert(comparePageNums(childMax, n.keys[i]) <= 0);
            lo = (i < n.childCount - 1) ? n.keys[i] : childMax;
        }
        hasLo = true;
    }
    *maxOut = lo;
    return pages;
}

/*
Asserts that the tree holds exactly the pages marked in present[1..maxPage]: each can be found by
key and no other can, the tree's structure is sound, and the leaves' linked list visits the same
pages in order.
*/
static void check_tree(table* t, const bool* present, uint32_t maxPage) {
    uint32_t expected = 0;
    for (uint32_t k = 1; k <= maxPage; k++) expected += present[k];
    node n = {0};
    assert(readNode(t->root, &n, t));
    if (expected == 0) {  // an empty tree is a root leaf with no pages
        assert(n.isLeaf && n.childCount == 0);
        return;
    }
    for (uint32_t k = 1; k <= maxPage; k++)
        assert((findPage(pn(k), t) != 0) == present[k]);
    page_num max;
    assert(check_subtree(t, t->root, 0, false, pn(0), &max) == expected);

    address addr = t->root;
    while (!n.isLeaf) {
        addr = n.children[0];
        assert(readNode(addr, &n, t));
    }
    uint32_t seen = 0;
    uint64_t last = 0;
    address prevAddr = 0;
    for (;;) {
        assert(n.prev == prevAddr);
        for (uint32_t i = 0; i < n.childCount; i++) {
            uint64_t k = n.keys[i].as.u64;
            assert(k > last && k <= maxPage && present[k]);
            last = k;
            seen++;
        }
        if (!n.next) break;
        prevAddr = addr;
        addr = n.next;
        assert(readNode(addr, &n, t));
    }
    assert(seen == expected);
}

/* Returns a tree holding pages 1..count, and marks them in present. */
static table* create_big_tree(char* name, uint32_t count, bool* present) {
    table* t = createTree(name, pn(1));
    assert(t != NULL);
    insert_pages(t, 2, count - 1);
    for (uint32_t k = 1; k <= count; k++) present[k] = true;
    return t;
}

/* Deletes the pages listed in order, checking the whole tree after every checkEvery deletions. */
static void delete_and_check(table* t, bool* present, uint32_t maxPage, const uint32_t* order, uint32_t count, uint32_t checkEvery) {
    for (uint32_t i = 0; i < count; i++) {
        assert(findAndDelete(pn(order[i]), t));
        present[order[i]] = false;
        if ((i + 1) % checkEvery == 0 || i + 1 == count) check_tree(t, present, maxPage);
    }
}

/* A small deterministic generator, so the random tests run the same way everywhere. */
static uint32_t test_rand(uint32_t* state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void shuffle(uint32_t* a, uint32_t count, uint32_t* state) {
    for (uint32_t i = count - 1; i > 0; i--) {
        uint32_t j = test_rand(state) % (i + 1);
        uint32_t tmp = a[i]; a[i] = a[j]; a[j] = tmp;
    }
}

#define REBALANCE_PAGES (6 * M_GLOBAL)

/*
Deleting from the last leaf backwards makes every rebalance borrow from, or merge into, the
previous leaf, which deleting from the front of the tree never does.
*/
void test_btree_delete_from_the_end(void) {
    printf("  test_btree_delete_from_the_end ... ");
    bool present[REBALANCE_PAGES + 1] = {0};
    uint32_t order[REBALANCE_PAGES];
    table* t = create_big_tree("bt_dfe", REBALANCE_PAGES, present);
    check_tree(t, present, REBALANCE_PAGES);
    for (uint32_t i = 0; i < REBALANCE_PAGES; i++) order[i] = REBALANCE_PAGES - i;
    delete_and_check(t, present, REBALANCE_PAGES, order, REBALANCE_PAGES, 1);
    deleteTree(t);
    printf("PASS\n");
}

/*
Deleting outwards from the middle rebalances leaves that have siblings on both sides, so a leaf
that isn't its parent's first child borrows from the next one.
*/
void test_btree_delete_from_the_middle(void) {
    printf("  test_btree_delete_from_the_middle ... ");
    bool present[REBALANCE_PAGES + 1] = {0};
    uint32_t order[REBALANCE_PAGES];
    table* t = create_big_tree("bt_dfm", REBALANCE_PAGES, present);
    uint32_t lo = REBALANCE_PAGES / 2, hi = lo + 1;
    for (uint32_t i = 0; i < REBALANCE_PAGES; i++) order[i] = (i % 2 == 0) ? hi++ : lo--;
    delete_and_check(t, present, REBALANCE_PAGES, order, REBALANCE_PAGES, 1);
    deleteTree(t);
    printf("PASS\n");
}

/* Deleting every other page, then the rest, thins every leaf at once. */
void test_btree_delete_alternating(void) {
    printf("  test_btree_delete_alternating ... ");
    bool present[REBALANCE_PAGES + 1] = {0};
    uint32_t order[REBALANCE_PAGES];
    table* t = create_big_tree("bt_dalt", REBALANCE_PAGES, present);
    uint32_t count = 0;
    for (uint32_t k = 2; k <= REBALANCE_PAGES; k += 2) order[count++] = k;
    for (uint32_t k = 1; k <= REBALANCE_PAGES; k += 2) order[count++] = k;
    delete_and_check(t, present, REBALANCE_PAGES, order, REBALANCE_PAGES, 1);
    deleteTree(t);
    printf("PASS\n");
}

#define DEEP_PAGES (50 * M_GLOBAL)

/*
A tree three levels deep is deleted from in random order, refilled, and emptied again, so internal
nodes borrow through their parent and merge as well as leaves.
*/
void test_btree_delete_random_deep(void) {
    printf("  test_btree_delete_random_deep ... ");
    bool* present = calloc(DEEP_PAGES + 1, sizeof(bool));
    uint32_t* order = malloc(DEEP_PAGES * sizeof(uint32_t));
    table* t = create_big_tree("bt_drd", DEEP_PAGES, present);
    node root = {0}, child = {0};
    readNode(t->root, &root, t);
    readNode(root.children[0], &child, t);
    assert(!root.isLeaf && !child.isLeaf);  // three levels
    check_tree(t, present, DEEP_PAGES);

    uint32_t state = 12345;
    for (uint32_t i = 0; i < DEEP_PAGES; i++) order[i] = i + 1;
    shuffle(order, DEEP_PAGES, &state);
    uint32_t most = DEEP_PAGES * 9 / 10;
    delete_and_check(t, present, DEEP_PAGES, order, most, 16);
    // put them back in a different order
    shuffle(order, most, &state);
    for (uint32_t i = 0; i < most; i++) {
        findAndInsert(pn(order[i]), t);
        present[order[i]] = true;
        if ((i + 1) % 64 == 0 || i + 1 == most) check_tree(t, present, DEEP_PAGES);
    }
    // and empty the tree
    for (uint32_t i = 0; i < DEEP_PAGES; i++) order[i] = i + 1;
    shuffle(order, DEEP_PAGES, &state);
    delete_and_check(t, present, DEEP_PAGES, order, DEEP_PAGES, 16);

    deleteTree(t);
    free(present);
    free(order);
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
    test_btree_commit_drains_hashmaps();
    test_btree_commit_persist();
    test_btree_commit_delete_persist();
    test_btree_commit_skips_clean();
    test_btree_commit_apply_failure_recovers();
    // insert and split
    test_btree_insert_new_page();
    test_btree_insert_existing_page();
    test_btree_split_structure();
    test_btree_split_find_all();
    test_btree_split_linked_list();
    test_btree_split_non_last_child();
    // page deletion and rebalancing
    test_btree_delete_page();
    test_btree_delete_triggers_borrow();
    test_btree_delete_triggers_merge();
    test_btree_delete_from_the_end();
    test_btree_delete_from_the_middle();
    test_btree_delete_alternating();
    test_btree_delete_random_deep();
    // full persistence roundtrip
    test_btree_full_roundtrip();
    // cleanup
    test_btree_delete_tree();
    test_btree_delete_tree_not_reloadable();
    printf("=== All B+ tree tests passed ===\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// FILE HELPER TESTS

/* Whether a forked child process can take the lock on path. */
static bool child_can_lock(const char* path) {
    fflush(NULL);  // so the child doesn't re-flush output buffered before the fork
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) _exit(lockFileExclusive(path) == -1 ? 1 : 0);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
While one process holds the lock, another must be refused; once it's released,
the other can take it. (Record locks belong to a process, so the second taker
has to be a separate process.)
*/
void test_file_lock_excludes_other_processes(void) {
    printf("  test_file_lock_excludes_other_processes ... ");
    const char* path = TABLE_DIRECTORY "_lock_test.lock";
    int fd = lockFileExclusive(path);
    assert(fd != -1);
    assert(!child_can_lock(path));
    close(fd);
    assert(child_can_lock(path));
    remove(path);
    printf("PASS\n");
}

/* Run all file helper tests. */
void test_file(void) {
    printf("=== File Helper Tests ===\n");
    test_file_lock_excludes_other_processes();
    printf("=== All file helper tests passed ===\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// WRITE-AHEAD LOG TESTS

/* Size of the log file in bytes (0 if it doesn't exist). */
static long log_size(void) {
    FILE* f = fopen(WAL_LOG_PATH, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

/* Flip every bit of the log byte at offset. */
static void corrupt_log_byte(long offset) {
    FILE* f = fopen(WAL_LOG_PATH, "rb+");
    assert(f != NULL);
    fseek(f, offset, SEEK_SET);
    int c = fgetc(f);
    fseek(f, offset, SEEK_SET);
    fputc(~c & 0xFF, f);
    fclose(f);
}

/* Read len bytes at addr from tables/<name>.tbl into out (bytes past EOF stay untouched). */
static void read_tbl_bytes(const char* name, address addr, char* out, size_t len) {
    char* path = build_tbl_path(name);
    FILE* f = fopen(path, "rb");
    free(path);
    assert(f != NULL);
    fseek(f, (long)addr, SEEK_SET);
    size_t n = fread(out, 1, len, f);
    (void)n;
    fclose(f);
}

/* Create a table and return an address in its unused space, for logging raw writes to. */
static address make_wal_table(char* name) {
    table* t = createTree(name, pn(1));
    assert(t != NULL);
    address addr = t->pageFree;
    close_table_keep_file(t);
    return addr;
}

static void drop_wal_table(char* name) {
    table* t = calloc(1, sizeof(table));
    assert(loadTable(name, t));
    deleteTree(t);
}

static const char WAL_TEST_PAYLOAD[] = "REDOTEST";

/* The checksum must match the published CRC-32C check value. */
void test_wal_crc32c_check_value(void) {
    printf("  test_wal_crc32c_check_value ... ");
    assert(crc32c_compute(0, (const uint8_t*)"123456789", 9) == 0xE3069283);
    printf("PASS\n");
}

/*
A committed log that was never applied (as if the process crashed right after
the commit point) must be replayed by recover(), which then empties the log.
*/
void test_wal_recover_redoes_committed_log(void) {
    printf("  test_wal_recover_redoes_committed_log ... ");
    address addr = make_wal_table("wal_redo");
    assert(initManager());
    assert(addLogEntry("wal_redo.tbl", WAL_PAGE, addr, (const uint8_t*)WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)));
    assert(markLogCommitted());

    assert(recover());
    char got[sizeof(WAL_TEST_PAYLOAD)] = {0};
    read_tbl_bytes("wal_redo", addr, got, sizeof(got));
    assert(memcmp(got, WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)) == 0);
    assert(log_size() == 0);

    drop_wal_table("wal_redo");
    printf("PASS\n");
}

/* A log with entries but no commit marker must be discarded without touching the table. */
void test_wal_recover_discards_uncommitted_log(void) {
    printf("  test_wal_recover_discards_uncommitted_log ... ");
    address addr = make_wal_table("wal_disc");
    assert(initManager());
    assert(addLogEntry("wal_disc.tbl", WAL_PAGE, addr, (const uint8_t*)WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)));

    assert(recover());  // closing the log flushes the entry first, so it is on disk without a marker
    char got[sizeof(WAL_TEST_PAYLOAD)] = {0};
    read_tbl_bytes("wal_disc", addr, got, sizeof(got));
    assert(memcmp(got, WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)) != 0);
    assert(log_size() == 0);

    drop_wal_table("wal_disc");
    printf("PASS\n");
}

/* A commit marker that fails its checksum (e.g. torn) means the transaction never committed. */
void test_wal_recover_rejects_damaged_marker(void) {
    printf("  test_wal_recover_rejects_damaged_marker ... ");
    address addr = make_wal_table("wal_dmgm");
    assert(initManager());
    assert(addLogEntry("wal_dmgm.tbl", WAL_PAGE, addr, (const uint8_t*)WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)));
    assert(markLogCommitted());
    corrupt_log_byte(log_size() - 1);  // last byte of the marker's checksum

    assert(recover());
    char got[sizeof(WAL_TEST_PAYLOAD)] = {0};
    read_tbl_bytes("wal_dmgm", addr, got, sizeof(got));
    assert(memcmp(got, WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)) != 0);
    assert(log_size() == 0);

    drop_wal_table("wal_dmgm");
    printf("PASS\n");
}

/*
A committed log with a damaged entry can't be finished: recover() must report
failure, write nothing, and leave the log in place for inspection.
*/
void test_wal_recover_reports_damaged_entry(void) {
    printf("  test_wal_recover_reports_damaged_entry ... ");
    address addr = make_wal_table("wal_dmge");
    assert(initManager());
    assert(addLogEntry("wal_dmge.tbl", WAL_PAGE, addr, (const uint8_t*)WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)));
    assert(markLogCommitted());
    long size = log_size();
    corrupt_log_byte(100);  // inside the first entry's table name

    assert(!recover());
    char got[sizeof(WAL_TEST_PAYLOAD)] = {0};
    read_tbl_bytes("wal_dmge", addr, got, sizeof(got));
    assert(memcmp(got, WAL_TEST_PAYLOAD, sizeof(WAL_TEST_PAYLOAD)) != 0);
    assert(log_size() == size);

    assert(resetLog());  // clean up for the tests that follow
    drop_wal_table("wal_dmge");
    printf("PASS\n");
}

/* Run all write-ahead log tests. */
void test_wal(void) {
    printf("=== Write-Ahead Log Tests ===\n");
    test_wal_crc32c_check_value();
    test_wal_recover_redoes_committed_log();
    test_wal_recover_discards_uncommitted_log();
    test_wal_recover_rejects_damaged_marker();
    test_wal_recover_reports_damaged_entry();
    printf("=== All write-ahead log tests passed ===\n");
}
