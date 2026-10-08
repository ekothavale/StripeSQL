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

#include "test_heap.h" // must come first; see the header
#include "testing.h"
#include "wal.h"
#include "file.h"
#include <unistd.h>
#include <sys/wait.h>

// ##########################################################################################################################################
// ##########################################################################################################################################
// SHARED TEST HELPERS

/* A numeric ordering key, so tests can write okey(42) instead of a compound literal. */
static ordering_key okey(uint64_t v) {
    return (ordering_key){ .type = ORDERING_ULONG, .as.u64 = v };
}
/* A string ordering key holding str, up to the longest a key can be. */
static ordering_key okey_text(const char* str) {
    ordering_key k = { .type = ORDERING_STRING };
    strncpy(k.as.string, str, TEXT_KEY_MAX_LEN);
    return k;
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
// ORDERING KEY TESTS

/* Asserts that the ordering keys of these primary keys, listed in ascending order, compare in that order. */
static void assert_keys_ascend(value* pks, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < count; j++) {
            int expected = i < j ? -1 : (i == j ? 0 : 1);
            assert(compareOrderingKeys(pkToOk(pks[i]), pkToOk(pks[j])) == expected);
        }
    }
}

/*
Integer keys sort as signed integers, negative below positive, and consecutive integers get consecutive
keys, which is what lets rows inserted in key order fill one page after another. The most negative
integer gets the smallest key there is.
*/
void test_ordering_int_keys(void) {
    printf("  test_ordering_int_keys ... ");
    value pks[] = {
        INTEGER_VAL(INT64_MIN), INTEGER_VAL(-1000000), INTEGER_VAL(-65), INTEGER_VAL(-64), INTEGER_VAL(-2),
        INTEGER_VAL(-1), INTEGER_VAL(0), INTEGER_VAL(1), INTEGER_VAL(2), INTEGER_VAL(63), INTEGER_VAL(64),
        INTEGER_VAL(65), INTEGER_VAL(1000000), INTEGER_VAL(INT64_MAX),
    };
    assert_keys_ascend(pks, sizeof(pks) / sizeof(pks[0]));
    for (int64_t k = -70; k < 70; k++) {
        assert(pkToOk(INTEGER_VAL(k)).type == ORDERING_ULONG);
        assert(pkToOk(INTEGER_VAL(k + 1)).as.u64 == pkToOk(INTEGER_VAL(k)).as.u64 + 1);
    }
    assert(pkToOk(INTEGER_VAL(INT64_MIN)).as.u64 == 0);
    assert(pkToOk(INTEGER_VAL(INT64_MAX)).as.u64 == UINT64_MAX);
    printf("PASS\n");
}

/*
Text keys are the text itself and sort byte by byte: a string after its own prefixes, upper case before
lower, and bytes above ASCII (UTF-8) after all of it. Every byte up to the maximum length counts, and the
empty string gets the smallest key there is.
*/
void test_ordering_text_keys(void) {
    printf("  test_ordering_text_keys ... ");
    char longest[TEXT_KEY_MAX_LEN + 1], longestButLast[TEXT_KEY_MAX_LEN + 1];
    memset(longest, 'z', TEXT_KEY_MAX_LEN);
    longest[TEXT_KEY_MAX_LEN] = '\0';
    strcpy(longestButLast, longest);
    longestButLast[TEXT_KEY_MAX_LEN - 1] = 'y';
    value pks[] = {
        TEXT_VAL(""), TEXT_VAL("A"), TEXT_VAL("Z"), TEXT_VAL("a"), TEXT_VAL("a "), TEXT_VAL("aa"), TEXT_VAL("ab"),
        TEXT_VAL("abc"), TEXT_VAL("b"), TEXT_VAL("key00009"), TEXT_VAL("key00010"), TEXT_VAL("key1"),
        TEXT_VAL(longestButLast), TEXT_VAL(longest), TEXT_VAL("\xC3\xA9"),
    };
    assert_keys_ascend(pks, sizeof(pks) / sizeof(pks[0]));

    ordering_key pear = pkToOk(TEXT_VAL("pear"));
    assert(pear.type == ORDERING_STRING && strcmp(pear.as.string, "pear") == 0);
    assert(strcmp(pkToOk(TEXT_VAL(longest)).as.string, longest) == 0);
    ordering_key empty = pkToOk(TEXT_VAL(""));
    ordering_key smallest = { .type = ORDERING_STRING };
    assert(memcmp(empty.as.string, smallest.as.string, sizeof(empty.as.string)) == 0);
    printf("PASS\n");
}

/* Float keys sort as numbers, negative below positive. */
void test_ordering_float_keys(void) {
    printf("  test_ordering_float_keys ... ");
    value pks[] = {
        FLOAT_VAL(-1e300), FLOAT_VAL(-2.5), FLOAT_VAL(-1.0), FLOAT_VAL(-1e-300), FLOAT_VAL(0.0),
        FLOAT_VAL(1e-300), FLOAT_VAL(1.0), FLOAT_VAL(2.5), FLOAT_VAL(1e300),
    };
    assert_keys_ascend(pks, sizeof(pks) / sizeof(pks[0]));
    assert(pkToOk(FLOAT_VAL(1.0)).type == ORDERING_DOUBLE);
    printf("PASS\n");
}

void test_ordering(void) {
    printf("=== Ordering Key Tests ===\n");
    test_ordering_int_keys();
    test_ordering_text_keys();
    test_ordering_float_keys();
    printf("=== All ordering key tests passed ===\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// SLOTTED PAGE TESTS


/* Allocate a fresh slotted_page with room for up to 64 slots and 256 entries. */
static slotted_page* make_test_page(void) {
    slotted_page* p = calloc(1, sizeof(slotted_page));
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
    bool ok = SPInsert(p, okey(10), make_sp_record(es, 2));

    assert(ok);
    assert(p->header.numRecords == 1);
    assert(p->header.numEntries == 2);
    assert(compareOrderingKeys(p->slots[0].ID, okey(10)) == 0);
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
    SPInsert(p, okey(5), make_sp_record(es, 2));

    sp_record result = SPRead(p, okey(5));
    assert(result.len == 2);
    assert(result.entries != NULL);
    assert(strcmp(result.entries[0].data, "Bob") == 0);
    assert(strcmp(result.entries[1].data, "25")  == 0);

    sp_record missing = SPRead(p, okey(99));
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
    SPInsert(p, okey(1), make_sp_record(es1, 1));
    SPInsert(p, okey(2), make_sp_record(es2, 1));
    assert(p->header.numRecords == 2);

    bool ok = SPDelete(p, okey(1));
    assert(ok);
    assert(p->header.numRecords == 1);

    sp_record gone = SPRead(p, okey(1));
    assert(gone.entries == NULL);

    sp_record kept = SPRead(p, okey(2));
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
    SPInsert(p, okey(7), make_sp_record(es, 2));

    /* Replace both entries with fresh heap strings. */
    entry new_es[] = { make_entry("Eve", T_STRING), make_entry("21", T_INT) };
    bool ok = SPUpdate(p, okey(7), make_sp_record(new_es, 2));
    assert(ok);

    sp_record result = SPRead(p, okey(7));
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

    SPInsert(p, okey(30), make_sp_record(e30, 1));
    SPInsert(p, okey(10), make_sp_record(e10, 1));
    SPInsert(p, okey(20), make_sp_record(e20, 1));

    assert(p->header.numRecords == 3);

    /* Slot array must be sorted by ID after each insertion. */
    assert(compareOrderingKeys(p->slots[0].ID, okey(10)) == 0);
    assert(compareOrderingKeys(p->slots[1].ID, okey(20)) == 0);
    assert(compareOrderingKeys(p->slots[2].ID, okey(30)) == 0);

    sp_record r = SPRead(p, okey(20));
    assert(r.len == 1);
    assert(strcmp(r.entries[0].data, "Bob") == 0);

    /* Delete the middle record and verify neighbours are still accessible. */
    SPDelete(p, okey(20));
    assert(p->header.numRecords == 2);
    assert(SPRead(p, okey(20)).entries == NULL);
    assert(strcmp(SPRead(p, okey(10)).entries[0].data, "Alice")   == 0);
    assert(strcmp(SPRead(p, okey(30)).entries[0].data, "Charlie") == 0);

    free_test_page(p);
    printf("PASS\n");
}

/*
test_page_full_keys: a slot is identified by its whole key. Keys that differ only in their high bits, the
largest key there is, and string keys all live in one page and are found by exactly their own key.
*/
void test_page_full_keys(void) {
    printf("  test_page_full_keys ... ");
    slotted_page* p = make_test_page();

    // these all share their low 6 bits, which is all a slot's ID used to hold
    uint64_t keys[] = { 5, 5 + 64, 5 + 128, 5 + (1ULL << 40), UINT64_MAX - 58 };
    int count = (int)(sizeof(keys) / sizeof(keys[0]));
    char buf[32];
    for (int i = count - 1; i >= 0; i--) {  // inserted largest first
        snprintf(buf, sizeof(buf), "row %d", i);
        entry e[] = { make_entry(buf, T_STRING) };
        assert(SPInsert(p, okey(keys[i]), make_sp_record(e, 1)));
    }
    assert(p->header.numRecords == (uint32_t)count);
    for (int i = 0; i < count; i++) {
        assert(compareOrderingKeys(p->slots[i].ID, okey(keys[i])) == 0);  // sorted by the whole key
        assert(SPSearch(p, okey(keys[i])) == i);
        snprintf(buf, sizeof(buf), "row %d", i);
        assert(strcmp(SPRead(p, okey(keys[i])).entries[0].data, buf) == 0);
    }
    assert(SPSearch(p, okey(5 + 192)) == -1);  // same low bits as the others, but not in the page
    assert(SPRead(p, okey(6)).entries == NULL);
    assert(!SPDelete(p, okey(5 + 192)));
    assert(SPDelete(p, okey(5 + 64)));
    assert(SPSearch(p, okey(5 + 64)) == -1 && SPSearch(p, okey(5)) == 0 && SPSearch(p, okey(5 + 128)) == 1);
    free_test_page(p);

    // string keys, in the order compareOrderingKeys() gives them
    p = make_test_page();
    const char* names[] = { "pear", "apple", "fig", "apples" };
    for (int i = 0; i < 4; i++) {
        entry e[] = { make_entry(names[i], T_STRING) };
        assert(SPInsert(p, okey_text(names[i]), make_sp_record(e, 1)));
    }
    for (uint32_t i = 1; i < p->header.numRecords; i++)
        assert(compareOrderingKeys(p->slots[i - 1].ID, p->slots[i].ID) < 0);
    for (int i = 0; i < 4; i++)
        assert(strcmp(SPRead(p, okey_text(names[i])).entries[0].data, names[i]) == 0);
    assert(SPSearch(p, okey_text("appl")) == -1);
    assert(compareOrderingKeys(p->header.minKey, okey_text("apple")) == 0);
    assert(compareOrderingKeys(p->header.maxKey, okey_text("pear")) == 0);
    free_test_page(p);
    printf("PASS\n");
}

/* Asserts that p holds count records whose smallest and largest keys are min and max. */
static void check_bounds(slotted_page* p, uint32_t count, uint64_t min, uint64_t max) {
    assert(p->header.numRecords == count);
    assert(compareOrderingKeys(p->header.minKey, okey(min)) == 0);
    assert(compareOrderingKeys(p->header.maxKey, okey(max)) == 0);
}

/* Asserts that p holds no records, and that its key bounds are zeroed. */
static void check_empty(slotted_page* p) {
    ordering_key none = {0};
    assert(p->header.numRecords == 0);
    assert(memcmp(&p->header.minKey, &none, sizeof(none)) == 0);
    assert(memcmp(&p->header.maxKey, &none, sizeof(none)) == 0);
}

/*
test_page_key_bounds: the header's minKey and maxKey are the smallest and largest keys in the page after
every insert and delete, whichever end the change is at, and are zeroed whenever the page is empty.
*/
void test_page_key_bounds(void) {
    printf("  test_page_key_bounds ... ");
    slotted_page* made = makeSPage(PAGE_NUM_SLOTS, PAGE_NUM_ENTRIES, PAGE_ARR_CAP);
    check_empty(made);  // a new page has no keys
    freeSPage(made);
    free(made);

    slotted_page* p = make_test_page();
    check_empty(p);
    uint64_t order[] = { 50, 20, 80, 60, 10, 90 };  // each one a new minimum, a new maximum, or neither
    uint64_t mins[]  = { 50, 20, 20, 20, 10, 10 };
    uint64_t maxs[]  = { 50, 50, 80, 80, 80, 90 };
    for (uint32_t i = 0; i < 6; i++) {
        entry e[] = { make_entry("v", T_STRING) };
        assert(SPInsert(p, okey(order[i]), make_sp_record(e, 1)));
        check_bounds(p, i + 1, mins[i], maxs[i]);
    }

    // updating a record leaves the bounds alone
    entry updated[] = { make_entry("w", T_STRING) };
    assert(SPUpdate(p, okey(10), make_sp_record(updated, 1)));
    check_bounds(p, 6, 10, 90);

    assert(SPDelete(p, okey(60)));   // from the middle
    check_bounds(p, 5, 10, 90);
    assert(SPDelete(p, okey(10)));   // the minimum
    check_bounds(p, 4, 20, 90);
    assert(SPDelete(p, okey(90)));   // the maximum
    check_bounds(p, 3, 20, 80);
    assert(!SPDelete(p, okey(90)));  // deleting what isn't there changes nothing
    check_bounds(p, 3, 20, 80);
    assert(SPDelete(p, okey(20)));
    assert(SPDelete(p, okey(80)));
    check_bounds(p, 1, 50, 50);      // one record is both bounds
    assert(SPDelete(p, okey(50)));
    check_empty(p);

    // and the page is usable again afterwards
    entry again[] = { make_entry("x", T_STRING) };
    assert(SPInsert(p, okey(7), make_sp_record(again, 1)));
    check_bounds(p, 1, 7, 7);

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
    test_page_full_keys();
    test_page_key_bounds();
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
#define TEST_NODE_SIZE NODE_DISK_SIZE

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

/* Build a page with the given key as both of its key bounds, and zeroed slot/entry arrays. */
static slotted_page* make_io_page(ordering_key key) {
    slotted_page* p = calloc(1, sizeof(slotted_page));
    p->header.minKey     = key;
    p->header.maxKey     = key;
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

/*
A header whose key type isn't one of the ordering types is rejected: nothing in the file's keys says what
type they are, so they couldn't be read.
*/
void test_meta_bad_key_type(void) {
    printf("  test_meta_bad_key_type ... ");
    table* t = createTable("_mt_badkt");
    assert(t != NULL);
    close_table_keep_file(t);
    table* ok = calloc(1, sizeof(table));
    assert(loadTable("_mt_badkt", ok));  // as written, it loads
    close_table_keep_file(ok);

    // the key type is the header's last word (see fillMeta() in tableIO.c)
    FILE* f = fopen("tables/_mt_badkt.tbl", "rb+");
    assert(f != NULL);
    uint32_t unknown = 7;
    fseek(f, (METALEN - 1) * 4, SEEK_SET);
    assert(fwrite(&unknown, 4, 1, f) == 1);
    fclose(f);

    table* t2 = calloc(1, sizeof(table));
    assert(!loadTable("_mt_badkt", t2));
    free(t2);
    remove("tables/_mt_badkt.tbl");
    printf("PASS\n");
}

// --- markPage ---

/*
Marking the same address twice must not increase the dirty count.
*/
void test_mark_page_dedup(void) {
    printf("  test_mark_page_dedup ... ");
    table t = make_test_table();
    slotted_page* p = make_io_page(okey(1));

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
    slotted_page* p = make_io_page(okey(7));

    markPage(300, p, &t);
    p->header.maxKey = okey(99);         // mutate original after mark

    // The snapshot in the dirty table should still have maxKey == 7
    slotted_page* snap = (slotted_page*)findAddrTable(300, &t.pageDirty);
    assert(compareOrderingKeys(snap->header.maxKey, okey(7)) == 0);

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

    slotted_page* p = make_io_page(okey(1));
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
    slotted_page* p = make_io_page(okey(42));
    p->header.minKey     = okey(17);  // the two bounds are separate fields
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
    assert(compareOrderingKeys(r.header.minKey, okey(17)) == 0);
    assert(compareOrderingKeys(r.header.maxKey, okey(42)) == 0);
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
Marking a page that is already pending replaces its pending version, and reading a pending page into a
struct that already holds a page replaces that page. Neither may leave the old one's arrays allocated.
Both once did, which lost about 15 KB for every row inserted and every pending page a scan read.
*/
void test_page_remark_and_reread_free_old(void) {
    printf("  test_page_remark_and_reread_free_old ... ");
    table t = make_test_table();
    slotted_page* p = makeSPage(PAGE_NUM_SLOTS, PAGE_NUM_ENTRIES, PAGE_ARR_CAP);
    entry e[2] = { make_entry("hello", T_STRING), make_entry("world", T_STRING) };
    sp_record rec = make_sp_record(e, 2);
    rec.size = e[0].size + e[1].size;
    assert(SPInsert(p, okey(7), rec));
    address addr = allocPage(&t);
    markPage(addr, p, &t);
    slotted_page r = {0};
    assert(readPage(addr, &r, &t));

    size_t before = heap_in_use();
    for (int i = 0; i < 1000; i++) {
        markPage(addr, p, &t);
        assert(readPage(addr, &r, &t));
    }
    size_t after = heap_in_use();
    assert(after <= before + 256 * 1024);  // a thousand lost versions of each would be about 30 MB

    // and the page read back is still the one that was marked
    assert(t.pageDirty.count == 1);
    assert(compareOrderingKeys(r.header.minKey, okey(7)) == 0 && compareOrderingKeys(r.header.maxKey, okey(7)) == 0);
    assert(r.header.numRecords == 1 && r.header.numEntries == 2);
    assert(strcmp((char*)r.entries[0].data, "hello") == 0 && strcmp((char*)r.entries[1].data, "world") == 0);

    freeSPage(&r);
    freeSPage(p);
    free(p);
    discard(&t);
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

    slotted_page* p1 = make_io_page(okey(1));
    slotted_page* p2 = make_io_page(okey(2));
    slotted_page* p3 = make_io_page(okey(3));
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
    assert(readPage(a1, &r1, &t) && compareOrderingKeys(r1.header.maxKey, okey(1)) == 0);
    assert(readPage(a2, &r2, &t) && compareOrderingKeys(r2.header.maxKey, okey(2)) == 0);
    assert(readPage(a3, &r3, &t) && compareOrderingKeys(r3.header.maxKey, okey(3)) == 0);

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
    n.maxKey      = okey(77);
    n.isLeaf      = true;
    n.parent      = 1000;
    n.prev        = 2000;
    n.next        = 3000;
    n.children[0] = 100;
    n.children[1] = 200;
    n.children[2] = 300;
    n.keys[0]     = okey(10);
    n.keys[1]     = okey(20);
    n.keys[2]     = okey(30);

    address addr = allocNode(&t);
    markNode(addr, &n, &t);
    writeNextNode(&t);
    assert(t.nodeDirty.count == 0);

    node r = {0};
    bool ok = readNode(addr, &r, &t);
    assert(ok);
    assert(r.childCount == 3);
    assert(compareOrderingKeys(r.maxKey, okey(77)) == 0);
    assert(r.isLeaf     == true);
    assert(r.parent     == 1000);
    assert(r.prev       == 2000);
    assert(r.next       == 3000);
    assert(r.children[0] == 100);
    assert(r.children[1] == 200);
    assert(r.children[2] == 300);
    assert(compareOrderingKeys(r.keys[0], okey(10)) == 0);
    assert(compareOrderingKeys(r.keys[1], okey(20)) == 0);

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

    node n1 = {0}; n1.childCount = 1; n1.maxKey = okey(10);
    node n2 = {0}; n2.childCount = 2; n2.maxKey = okey(20);
    node n3 = {0}; n3.childCount = 3; n3.maxKey = okey(30);
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
    assert(readNode(a1, &r1, &t) && compareOrderingKeys(r1.maxKey, okey(10)) == 0);
    assert(readNode(a2, &r2, &t) && compareOrderingKeys(r2.maxKey, okey(20)) == 0);
    assert(readNode(a3, &r3, &t) && compareOrderingKeys(r3.maxKey, okey(30)) == 0);

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

/* A page that fits in a test table's small pages, with room for numSlots slots. */
static slotted_page* make_small_page(uint32_t numSlots) {
    return makeSPage(numSlots, 16, TEST_PAGE_SIZE - PAGE_HEADER_DISK_SIZE);
}

/* Inserts a one-entry record holding text into p under key. */
static void insert_text(slotted_page* p, ordering_key key, const char* text) {
    entry e[] = { make_entry(text, T_STRING) };
    sp_record rec = make_sp_record(e, 1);
    rec.size = e[0].size;
    assert(SPInsert(p, key, rec));
}

/* Writes p to a newly allocated address in t and returns the address. */
static address write_page(slotted_page* p, table* t) {
    address addr = allocPage(t);
    markPage(addr, p, t);
    writeNextPage(t);
    assert(t->pageDirty.count == 0);
    return addr;
}

/*
A page with several records survives the disk: both key bounds, every slot's whole key, and every
record's data. Run once with numeric keys, including the largest there is and keys that share their low
bits, and once with string keys, including one of the maximum length.
*/
void test_page_roundtrip_records(void) {
    printf("  test_page_roundtrip_records ... ");
    char longest[TEXT_KEY_MAX_LEN + 1];
    memset(longest, 'z', TEXT_KEY_MAX_LEN);
    longest[TEXT_KEY_MAX_LEN] = '\0';
    ordering_key numeric[] = { okey(5), okey(5 + 64), okey(5 + (1ULL << 40)), okey(UINT64_MAX) };
    ordering_key text[]    = { okey_text("apple"), okey_text("apples"), okey_text("pear"), okey_text(longest) };
    ordering_key* sets[]   = { numeric, text };

    for (int which = 0; which < 2; which++) {
        ordering_key* keys = sets[which];
        table t = make_test_table();
        t.keyType = keys[0].type;  // a key is read back with the type its table has
        slotted_page* p = make_small_page(4);
        char buf[16];
        for (int i = 3; i >= 0; i--) {  // inserted largest first
            snprintf(buf, sizeof(buf), "row %d", i);
            insert_text(p, keys[i], buf);
        }
        address addr = write_page(p, &t);

        slotted_page r = {0};
        assert(readPage(addr, &r, &t));
        assert(r.header.numRecords == 4 && r.header.numEntries == 4);
        assert(r.header.usedData == p->header.usedData && r.header.arrCap == p->header.arrCap);
        assert(compareOrderingKeys(r.header.minKey, keys[0]) == 0);
        assert(compareOrderingKeys(r.header.maxKey, keys[3]) == 0);
        for (int i = 0; i < 4; i++) {
            assert(r.slots[i].ID.type == keys[i].type);
            assert(compareOrderingKeys(r.slots[i].ID, keys[i]) == 0);
            snprintf(buf, sizeof(buf), "row %d", i);
            sp_record got = SPRead(&r, keys[i]);
            assert(got.len == 1 && strcmp(got.entries[0].data, buf) == 0);
        }
        // the page read back works like any other
        assert(SPDelete(&r, keys[0]));
        assert(compareOrderingKeys(r.header.minKey, keys[1]) == 0);

        freeSPage(&r);
        freeSPage(p);
        free(p);
        free_test_table(&t);
    }
    printf("PASS\n");
}

/*
Pages differ in how many slots and entries they have room for, so reading one into a struct that still
holds another must give the struct arrays of the new page's size. (The arrays were once kept from whichever
page was read first, which a larger page then overran.)
*/
void test_page_reread_other_capacity(void) {
    printf("  test_page_reread_other_capacity ... ");
    table t = make_test_table();
    slotted_page* small = make_small_page(2);
    slotted_page* large = make_small_page(2);  // grows as it fills
    char buf[16];
    for (uint64_t k = 1; k <= 2; k++) insert_text(small, okey(k), "s");
    for (uint64_t k = 101; k <= 107; k++) {
        snprintf(buf, sizeof(buf), "l%llu", (unsigned long long)k);
        insert_text(large, okey(k), buf);
    }
    assert(large->header.maxSlots > small->header.maxSlots);
    address a = write_page(small, &t);
    address b = write_page(large, &t);

    slotted_page r = {0};
    for (int round = 0; round < 3; round++) {  // small, large, small, large, ...
        assert(readPage(a, &r, &t));
        assert(r.header.numRecords == 2 && r.header.maxSlots == small->header.maxSlots);
        assert(compareOrderingKeys(r.header.maxKey, okey(2)) == 0);
        assert(strcmp(SPRead(&r, okey(2)).entries[0].data, "s") == 0);

        assert(readPage(b, &r, &t));
        assert(r.header.numRecords == 7 && r.header.maxSlots == large->header.maxSlots);
        assert(compareOrderingKeys(r.header.minKey, okey(101)) == 0);
        assert(compareOrderingKeys(r.header.maxKey, okey(107)) == 0);
        for (uint64_t k = 101; k <= 107; k++) {
            snprintf(buf, sizeof(buf), "l%llu", (unsigned long long)k);
            assert(strcmp(SPRead(&r, okey(k)).entries[0].data, buf) == 0);
        }
    }
    freeSPage(&r);
    freeSPage(small); free(small);
    freeSPage(large); free(large);
    free_test_table(&t);
    printf("PASS\n");
}

/*
A page whose header is damaged is refused, and the struct it was being read into keeps the page it held.
*/
void test_page_read_damaged_header(void) {
    printf("  test_page_read_damaged_header ... ");
    table t = make_test_table();
    slotted_page* good = make_small_page(2);
    insert_text(good, okey(1), "kept");
    slotted_page* bad = make_small_page(2);
    insert_text(bad, okey(9), "lost");
    address a = write_page(good, &t);
    address b = write_page(bad, &t);

    // overwrite the damaged page's maxEntries (the fifth 4-byte field after the two keys) with a huge value
    unsigned char huge[4] = { 0xFF, 0xFF, 0xFF, 0xF0 };
    fseek(t.source, (long)(b + 1 + 2 * ORDERING_KEY_DISK_SIZE + 16), SEEK_SET);
    assert(fwrite(huge, 1, 4, t.source) == 4);
    fflush(t.source);

    slotted_page r = {0};
    assert(readPage(a, &r, &t));
    assert(!readPage(b, &r, &t));
    assert(r.header.numRecords == 1 && compareOrderingKeys(r.header.maxKey, okey(1)) == 0);
    assert(strcmp(SPRead(&r, okey(1)).entries[0].data, "kept") == 0);

    freeSPage(&r);
    freeSPage(good); free(good);
    freeSPage(bad); free(bad);
    free_test_table(&t);
    printf("PASS\n");
}

/* Reads the page-sized or node-sized object at addr straight from the table's file. The caller frees it. */
static unsigned char* raw_object(table* t, address addr, int size) {
    unsigned char* raw = malloc(size);
    fseek(t->source, (long)addr, SEEK_SET);
    assert(fread(raw, 1, size, t->source) == (size_t)size);
    return raw;
}

/*
A key on disk is its value and nothing else, ORDERING_KEY_DISK_SIZE bytes of it: the bytes of a string key,
or a number most significant byte first, with zeros after. Its type isn't there. It's the table's keyType,
so the same bytes read through a table with another key type are a key of that type.
*/
void test_key_type_not_stored_with_keys(void) {
    printf("  test_key_type_not_stored_with_keys ... ");
    assert(ORDERING_KEY_DISK_SIZE == TEXT_KEY_MAX_LEN);
    unsigned char apple[ORDERING_KEY_DISK_SIZE] = "apple", pear[ORDERING_KEY_DISK_SIZE] = "pear";
    unsigned char number[ORDERING_KEY_DISK_SIZE] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };

    // a page of string keys: the two bounds, then each slot's key
    table t = make_test_table();
    t.keyType = ORDERING_STRING;
    slotted_page* p = make_small_page(4);
    insert_text(p, okey_text("pear"), "row 1");
    insert_text(p, okey_text("apple"), "row 0");
    address addr = write_page(p, &t);
    unsigned char* raw = raw_object(&t, addr, TEST_PAGE_SIZE);
    assert(raw[0] == 0);
    assert(memcmp(raw + 1, apple, ORDERING_KEY_DISK_SIZE) == 0);
    assert(memcmp(raw + 1 + ORDERING_KEY_DISK_SIZE, pear, ORDERING_KEY_DISK_SIZE) == 0);
    assert(memcmp(raw + PAGE_HEADER_DISK_SIZE, apple, ORDERING_KEY_DISK_SIZE) == 0);
    assert(memcmp(raw + PAGE_HEADER_DISK_SIZE + SP_SLOT_DISK_SIZE, pear, ORDERING_KEY_DISK_SIZE) == 0);
    free(raw);
    freeSPage(p);
    free(p);

    // a node of numeric keys: maxKey in the header, then the keys after the children
    t.keyType = ORDERING_ULONG;
    node n = {0};
    n.isLeaf = true;
    n.childCount = 2;
    n.children[0] = 1000; n.children[1] = 1001;
    n.keys[0] = okey(5);
    n.keys[1] = n.maxKey = okey(0x0102030405060708ULL);
    address nodeAddr = allocNode(&t);
    markNode(nodeAddr, &n, &t);
    writeNextNode(&t);
    raw = raw_object(&t, nodeAddr, TEST_NODE_SIZE);
    assert(raw[0] == 1);
    assert(memcmp(raw + 29, number, ORDERING_KEY_DISK_SIZE) == 0);
    assert(memcmp(raw + NODE_HEADER_DISK_SIZE + 2 * 8 + ORDERING_KEY_DISK_SIZE, number, ORDERING_KEY_DISK_SIZE) == 0);
    free(raw);

    // read as numbers, and then, through a table of string keys, as strings
    node r = {0};
    assert(readNode(nodeAddr, &r, &t));
    assert(r.maxKey.type == ORDERING_ULONG && r.keys[1].as.u64 == 0x0102030405060708ULL && r.keys[0].as.u64 == 5);
    t.keyType = ORDERING_STRING;
    assert(readNode(nodeAddr, &r, &t));
    assert(r.maxKey.type == ORDERING_STRING && r.keys[1].type == ORDERING_STRING);
    assert(strcmp(r.keys[1].as.string, "\x01\x02\x03\x04\x05\x06\x07\x08") == 0);
    slotted_page asText = {0};
    assert(readPage(addr, &asText, &t));
    assert(asText.slots[0].ID.type == ORDERING_STRING && strcmp(asText.slots[0].ID.as.string, "apple") == 0);
    freeSPage(&asText);

    free_test_table(&t);
    printf("PASS\n");
}

/*
A node's keys and maxKey survive the disk as whole keys: numeric keys beyond what the old 19-byte page
number held, and string keys up to the maximum length.
*/
void test_node_roundtrip_full_keys(void) {
    printf("  test_node_roundtrip_full_keys ... ");
    char longest[TEXT_KEY_MAX_LEN + 1];
    memset(longest, 'z', TEXT_KEY_MAX_LEN);
    longest[TEXT_KEY_MAX_LEN] = '\0';
    ordering_key numeric[] = { okey(63), okey(64), okey(UINT64_MAX) };
    ordering_key text[]    = { okey_text("fig"), okey_text("figs"), okey_text(longest) };
    ordering_key* sets[]   = { numeric, text };

    for (int which = 0; which < 2; which++) {
        ordering_key* keys = sets[which];
        table t = make_test_table();
        t.keyType = keys[0].type;
        node n = {0};
        n.isLeaf = true;
        n.childCount = 3;
        n.parent = 11; n.prev = 22; n.next = 33;
        for (int i = 0; i < 3; i++) { n.children[i] = 1000 + i; n.keys[i] = keys[i]; }
        n.maxKey = keys[2];
        address addr = allocNode(&t);
        markNode(addr, &n, &t);
        writeNextNode(&t);

        node r = {0};
        assert(readNode(addr, &r, &t));
        assert(r.isLeaf && r.childCount == 3 && r.parent == 11 && r.prev == 22 && r.next == 33);
        assert(r.maxKey.type == keys[2].type && compareOrderingKeys(r.maxKey, keys[2]) == 0);
        for (int i = 0; i < 3; i++) {
            assert(r.children[i] == (address)(1000 + i));
            assert(r.keys[i].type == keys[i].type && compareOrderingKeys(r.keys[i], keys[i]) == 0);
        }
        free_test_table(&t);
    }

    // a full node fits in the size tables are created with
    table t = make_test_table();
    node full = {0};
    full.isLeaf = true;
    full.childCount = M_GLOBAL;
    for (int i = 0; i < M_GLOBAL; i++) { full.children[i] = 5000 + i; full.keys[i] = okey((uint64_t)i << 32); }
    full.maxKey = full.keys[M_GLOBAL - 1];
    address addr = allocNode(&t);
    markNode(addr, &full, &t);
    writeNextNode(&t);
    node r = {0};
    assert(readNode(addr, &r, &t));
    assert(r.childCount == M_GLOBAL);
    for (int i = 0; i < M_GLOBAL; i++)
        assert(r.children[i] == (address)(5000 + i) && compareOrderingKeys(r.keys[i], okey((uint64_t)i << 32)) == 0);
    free_test_table(&t);
    printf("PASS\n");
}

void test_tableio(void) {
    printf("=== TableIO Tests ===\n");
    // writeMeta / loadMeta
    test_meta_roundtrip();
    test_meta_large_addr();
    test_meta_bad_magic();
    test_meta_bad_key_type();
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
    test_page_remark_and_reread_free_old();
    test_page_roundtrip_records();
    test_page_reread_other_capacity();
    test_page_read_damaged_header();
    // writeNextNode / readNode
    test_node_write_empty_hashmap();
    test_node_roundtrip();
    test_node_roundtrip_full_keys();
    test_key_type_not_stored_with_keys();
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
    assert(t->nodeSize  == NODE_DISK_SIZE);
    assert(t->M         == M_GLOBAL);
    assert(t->root      == 0);
    assert(t->metalen   == METALEN * 4);
    assert(t->keyType   == ORDERING_ULONG);  // until newTree() gives it the type of the table's primary key
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
    table* t = createTree("mgmt_t1", ORDERING_ULONG);
    assert(t != NULL);
    assert(t->root != 0);
    deleteTree(t);
    printf("PASS\n");
}

/*
The root node written by createTree must be a leaf with one page, filed under the smallest key of the key
type createTree was given, which is also the node's maxKey. The table keeps that type, and so does its
file: the table loaded back has it, and gives it to the keys it reads.
*/
void test_create_tree_root_node(void) {
    printf("  test_create_tree_root_node ... ");
    ordering_key first[] = { okey(0), okey_text(""), { .type = ORDERING_DOUBLE } };
    for (int i = 0; i < 3; i++) {
        table* t = createTree("mgmt_t2", first[i].type);
        assert(t != NULL);
        assert(t->keyType == first[i].type);
        close_table_keep_file(t);

        t = calloc(1, sizeof(table));
        assert(loadTable("mgmt_t2", t));
        assert(t->keyType == first[i].type);

        node n = {0};
        bool ok = readNode(t->root, &n, t);
        assert(ok);
        assert(n.isLeaf     == true);
        assert(n.childCount == 1);
        assert(n.keys[0].type == first[i].type && compareOrderingKeys(n.keys[0], first[i]) == 0);
        assert(n.maxKey.type  == first[i].type && compareOrderingKeys(n.maxKey,  first[i]) == 0);
        assert(n.parent     == 0);

        deleteTree(t);
    }
    printf("PASS\n");
}

/*
The initial page pointed to by the root must be readable and empty, with no keys of its own yet.
*/
void test_create_tree_initial_page(void) {
    printf("  test_create_tree_initial_page ... ");
    table* t = createTree("mgmt_t3", ORDERING_ULONG);
    assert(t != NULL);

    node n = {0};
    readNode(t->root, &n, t);

    slotted_page p = {0};
    bool ok = readPage(n.children[0], &p, t);
    assert(ok);
    ordering_key none = {0};
    assert(p.header.numRecords == 0 && p.header.numEntries == 0);
    assert(memcmp(&p.header.minKey, &none, sizeof(none)) == 0 && memcmp(&p.header.maxKey, &none, sizeof(none)) == 0);
    assert(p.header.arrCap == PAGE_ARR_CAP);

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

/* Free every entry.data still live in p, then the slot/entry arrays. */
static void free_page_contents(slotted_page* p) {
    for (uint32_t i = 0; i < p->header.numEntries; i++)
        free(p->entries[i].data);
    free(p->slots);
    free(p->entries);
}

/* An empty tree whose keys are unsigned integers. */
static table* create_test_tree(char* name) {
    table* t = createTree(name, ORDERING_ULONG);
    assert(t != NULL);
    return t;
}

// rows of these two sizes fill a page very differently: dozens of small ones, or two large ones
#define SMALL_ROW 12
#define LARGE_ROW 1900
// how many rows of a size fit in a page
#define ROWS_PER_PAGE(size) (PAGE_ARR_CAP / (SP_SLOT_DISK_SIZE + 6 + (size)))

/* A one-entry row for key k: `size` bytes holding the key as text, then zeros. */
static entry make_row(uint64_t k, uint32_t size) {
    entry e;
    e.type = T_STRING;
    e.size = size;
    e.data = calloc(size, 1);
    snprintf(e.data, size, "%llu", (unsigned long long)k);
    return e;
}

/* Inserts a row of the given size under key k, and returns insertRecord()'s result. */
static int insert_row(table* t, uint64_t k, uint32_t size) {
    entry e = make_row(k, size);
    sp_record r = make_btree_record(&e, 1);
    int result = insertRecord(&r, okey(k), t);
    if (result != 0) free(e.data);
    return result;
}

/* Whether a row's data is what make_row() gave it for key k. */
static bool is_row_for(entry e, uint64_t k) {
    char want[24];
    snprintf(want, sizeof(want), "%llu", (unsigned long long)k);
    return e.size > strlen(want) && strcmp(e.data, want) == 0;
}

/* Whether the tree holds a row under key k. A row that is found must hold what was inserted for k. */
static bool has_row(table* t, uint64_t k) {
    slotted_page p = {0};
    sp_record r = readRecord(okey(k), t, &p);
    bool found = r.entries != NULL;
    if (found) assert(r.len == 1 && is_row_for(r.entries[0], k));
    freeSPage(&p);
    assert(found == searchRecord(okey(k), t));
    return found;
}

/* Deletes the row under key k, which must be in the tree. */
static void delete_row(table* t, uint64_t k) {
    slotted_page p = {0};
    address leaf = 0;
    assert(deleteRecord(okey(k), t, &p, &leaf));
    assert(leaf != 0);
    freeSPage(&p);
}

/* Reads the root, which must be a leaf, and returns how many pages it has. */
static uint32_t root_leaf(table* t, node* root) {
    assert(readNode(t->root, root, t));
    assert(root->isLeaf);
    return root->childCount;
}

/* How many pages the tree has, counted along the leaves' linked list. */
static uint32_t count_pages(table* t) {
    node n = {0};
    assert(readNode(t->root, &n, t));
    while (!n.isLeaf) assert(readNode(n.children[0], &n, t));
    uint32_t pages = n.childCount;
    while (n.next) {
        assert(readNode(n.next, &n, t));
        pages += n.childCount;
    }
    return pages;
}

/* Reads the page at index i of a leaf into p (which must be zeroed or hold an earlier page). */
static void read_leaf_page(table* t, node* leaf, uint32_t i, slotted_page* p) {
    assert(i < leaf->childCount);
    assert(readPage(leaf->children[i], p, t));
}

// ── Group 1: find ──────────────────────────────────────────────────────────

/*
An empty tree has one empty page, filed under the smallest key. That page covers the smallest key and
nothing above it, and no record is found anywhere.
*/
void test_btree_find_empty(void) {
    printf("  test_btree_find_empty ... ");
    table* t = create_test_tree("bt_fe");
    assert(findPage(okey(0), t) != 0);
    assert(findPage(okey(1), t) == 0);
    assert(!searchRecord(okey(0), t) && !searchRecord(okey(1), t));
    deleteTree(t);
    printf("PASS\n");
}

/*
findPage returns the one page that could hold a key: the page is filed under an upper bound for its keys,
so every key up to that bound leads to it whether or not the key is there, and a key above it leads nowhere.
*/
void test_btree_find_covering_page(void) {
    printf("  test_btree_find_covering_page ... ");
    table* t = create_test_tree("bt_fcp");
    assert(insert_row(t, 10, SMALL_ROW) == 0);
    assert(insert_row(t, 30, SMALL_ROW) == 0);
    assert(insert_row(t, 20, SMALL_ROW) == 0);

    node root = {0};
    assert(root_leaf(t, &root) == 1);
    address page = root.children[0];
    address leafAddr = 0;
    assert(findPage(okey(10), t) == page && findPage(okey(30), t) == page);
    assert(findPage(okey(1), t) == page && findPage(okey(25), t) == page);  // not in the tree, but this is where they'd be
    assert(findPageAndLeaf(okey(20), t, &leafAddr) == page && leafAddr == t->root);
    assert(findPage(okey(31), t) == 0);  // above every key

    assert(has_row(t, 10) && has_row(t, 20) && has_row(t, 30));
    assert(!has_row(t, 1) && !has_row(t, 25) && !has_row(t, 31));
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
    table* t = create_test_tree("bt_ra");
    node root = {0};
    root_leaf(t, &root);
    slotted_page p = {0};
    read_leaf_page(t, &root, 0, &p);

    entry e = make_btree_entry("Alice");
    assert(SPInsert(&p, okey(10), make_btree_record(&e, 1)));
    assert(p.header.numRecords == 1);

    sp_record r = SPRead(&p, okey(10));
    assert(r.len == 1);
    assert(strcmp(r.entries[0].data, "Alice") == 0);

    // p.entries[0].data == e.data; free once through the entries array
    free_page_contents(&p);
    deleteTree(t);
    printf("PASS\n");
}

/* SPUpdate must replace the stored entry and keep the page's sizes in step; old data must not leak. */
void test_btree_record_update(void) {
    printf("  test_btree_record_update ... ");
    table* t = create_test_tree("bt_ru");
    node root = {0};
    root_leaf(t, &root);
    slotted_page p = {0};
    read_leaf_page(t, &root, 0, &p);

    entry e1 = make_btree_entry("old");
    SPInsert(&p, okey(5), make_btree_record(&e1, 1));
    uint32_t before = SPUsedBytes(&p);

    // SPUpdate frees the old entry data internally
    entry e2 = make_btree_entry("a longer new value");
    assert(SPUpdate(&p, okey(5), make_btree_record(&e2, 1)));

    sp_record r = SPRead(&p, okey(5));
    assert(strcmp(r.entries[0].data, "a longer new value") == 0);
    assert(SPUsedBytes(&p) == before + e2.size - 4);  // "old" took 4 bytes
    assert(p.slots[0].size == e2.size && p.header.usedData == e2.size);

    // e1.data freed by SPUpdate; p.entries[0].data == e2.data
    free_page_contents(&p);
    deleteTree(t);
    printf("PASS\n");
}

/* SPDelete must remove one record while leaving others intact. */
void test_btree_record_delete(void) {
    printf("  test_btree_record_delete ... ");
    table* t = create_test_tree("bt_rd");
    node root = {0};
    root_leaf(t, &root);
    slotted_page p = {0};
    read_leaf_page(t, &root, 0, &p);

    entry e1 = make_btree_entry("Alice");
    entry e2 = make_btree_entry("Bob");
    SPInsert(&p, okey(1), make_btree_record(&e1, 1));
    SPInsert(&p, okey(2), make_btree_record(&e2, 1));
    assert(p.header.numRecords == 2);

    // SPDelete frees e1.data internally
    assert(SPDelete(&p, okey(1)));
    assert(p.header.numRecords == 1);
    assert(SPRead(&p, okey(1)).entries == NULL);
    assert(strcmp(SPRead(&p, okey(2)).entries[0].data, "Bob") == 0);

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
    table* t = create_test_tree("bt_cds");

    // inserting past the end dirties the page, and the root node its new key goes in
    assert(insert_row(t, 2, SMALL_ROW) == 0);
    assert(t->pageDirty.count > 0 && t->nodeDirty.count > 0);

    commit(t);
    assert(t->pageDirty.count == 0);
    assert(t->nodeDirty.count == 0);
    assert(t->delete.count    == 0);

    deleteTree(t);
    printf("PASS\n");
}

/*
A row inserted, committed, then reloaded from disk must still be there, in a page with one record.
*/
void test_btree_commit_persist(void) {
    printf("  test_btree_commit_persist ... ");
    table* t = create_test_tree("bt_cp");
    assert(insert_row(t, 7, SMALL_ROW) == 0);
    commit(t);
    close_table_keep_file(t);

    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_cp", t2));
    assert(has_row(t2, 7) && !has_row(t2, 8));
    address addr2 = findPage(okey(7), t2);
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
    table* t = create_test_tree("bt_cdp");

    address pageAddr = findPage(okey(0), t);
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
    table* t = create_test_tree("bt_csc");
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
    table* t = create_test_tree("bt_cafr");
    assert(insert_row(t, 2, SMALL_ROW) == 0);  // dirties the page and the root node

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
    assert(has_row(t2, 2));

    deleteTree(t2);
    printf("PASS\n");
}

// ── Group 4: insert, and pages splitting ───────────────────────────────────

/*
Rows go into the tree's one page until it is full. The page is filed under the largest key it has held,
which the root's maxKey follows, and a key that is already there is refused.
*/
void test_btree_insert_fills_one_page(void) {
    printf("  test_btree_insert_fills_one_page ... ");
    table* t = create_test_tree("bt_ifp");
    node root = {0};
    uint64_t keys[] = { 50, 20, 80, 10, 60 };
    uint64_t largest[] = { 50, 50, 80, 80, 80 };
    for (int i = 0; i < 5; i++) {
        assert(insert_row(t, keys[i], SMALL_ROW) == 0);
        assert(root_leaf(t, &root) == 1);
        assert(compareOrderingKeys(root.keys[0], okey(largest[i])) == 0);
        assert(compareOrderingKeys(root.maxKey,  okey(largest[i])) == 0);
    }
    assert(insert_row(t, 20, SMALL_ROW) == 1);  // already there
    slotted_page p = {0};
    read_leaf_page(t, &root, 0, &p);
    assert(p.header.numRecords == 5);
    assert(compareOrderingKeys(p.header.minKey, okey(10)) == 0 && compareOrderingKeys(p.header.maxKey, okey(80)) == 0);
    freeSPage(&p);
    for (int i = 0; i < 5; i++) assert(has_row(t, keys[i]));
    deleteTree(t);
    printf("PASS\n");
}

/* A row that couldn't fit in a page even alone is refused, and the tree is left as it was. */
void test_btree_insert_too_large(void) {
    printf("  test_btree_insert_too_large ... ");
    table* t = create_test_tree("bt_itl");
    assert(insert_row(t, 1, SMALL_ROW) == 0);
    assert(insert_row(t, 2, PAGE_ARR_CAP) == 3);
    assert(insert_row(t, 3, PAGE_ARR_CAP - SP_SLOT_DISK_SIZE - 6) == 0);  // the largest row there is room for
    node root = {0};
    assert(root_leaf(t, &root) == 2);
    assert(has_row(t, 1) && !has_row(t, 2) && has_row(t, 3));
    deleteTree(t);
    printf("PASS\n");
}

/*
Keys inserted in order fill each page completely: when a page has no room for the next key, that key starts
a new page and the full one is left alone. The full page is then filed under exactly its largest key.
*/
void test_btree_page_split_in_order(void) {
    printf("  test_btree_page_split_in_order ... ");
    table* t = create_test_tree("bt_psio");
    uint32_t perPage = ROWS_PER_PAGE(SMALL_ROW);
    node root = {0};
    for (uint64_t k = 1; k <= perPage; k++) assert(insert_row(t, k, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 1);
    assert(insert_row(t, perPage + 1, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 2);

    slotted_page p = {0};
    read_leaf_page(t, &root, 0, &p);
    assert(p.header.numRecords == perPage);
    assert(compareOrderingKeys(root.keys[0], okey(perPage)) == 0);
    read_leaf_page(t, &root, 1, &p);
    assert(p.header.numRecords == 1);
    assert(compareOrderingKeys(root.keys[1], okey(perPage + 1)) == 0);
    assert(compareOrderingKeys(root.maxKey,  okey(perPage + 1)) == 0);
    freeSPage(&p);

    // three more pages' worth, and every page but the last is full
    for (uint64_t k = perPage + 2; k <= 4 * perPage; k++) assert(insert_row(t, k, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 4);
    for (uint64_t k = 1; k <= 4 * perPage; k++) assert(has_row(t, k));
    deleteTree(t);
    printf("PASS\n");
}

/*
A key that belongs in the middle of a full page splits it into two pages about equally full. The lower one
is filed under its own largest key and the upper one under the key the page had.
*/
void test_btree_page_split_in_the_middle(void) {
    printf("  test_btree_page_split_in_the_middle ... ");
    table* t = create_test_tree("bt_psm");
    uint32_t perPage = ROWS_PER_PAGE(SMALL_ROW);
    node root = {0};
    for (uint64_t i = 1; i <= perPage; i++) assert(insert_row(t, 2 * i, SMALL_ROW) == 0);  // even keys
    assert(root_leaf(t, &root) == 1);
    assert(insert_row(t, 7, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 2);

    slotted_page lower = {0}, upper = {0};
    read_leaf_page(t, &root, 0, &lower);
    read_leaf_page(t, &root, 1, &upper);
    assert(lower.header.numRecords + upper.header.numRecords == perPage + 1);
    uint32_t a = lower.header.numRecords, b = upper.header.numRecords;
    assert((a > b ? a - b : b - a) <= 1);
    assert(compareOrderingKeys(root.keys[0], lower.header.maxKey) == 0);
    assert(compareOrderingKeys(root.keys[1], okey(2 * perPage)) == 0);
    assert(compareOrderingKeys(lower.header.maxKey, upper.header.minKey) < 0);
    assert(SPSearch(&lower, okey(7)) >= 0);
    freeSPage(&lower);
    freeSPage(&upper);

    assert(has_row(t, 7));
    for (uint64_t i = 1; i <= perPage; i++) assert(has_row(t, 2 * i) && (i == 3 || !has_row(t, 2 * i + 1)));
    deleteTree(t);
    printf("PASS\n");
}

/*
When a full page's rows can't be divided between two pages along with the new one, because they are too
large, the new row gets a page to itself between the rows below it and the rows above it.
*/
void test_btree_page_split_three_ways(void) {
    printf("  test_btree_page_split_three_ways ... ");
    table* t = create_test_tree("bt_pst");
    assert(insert_row(t, 10, 2300) == 0);
    assert(insert_row(t, 30, 1500) == 0);
    node root = {0};
    assert(root_leaf(t, &root) == 1);
    assert(insert_row(t, 20, 3000) == 0);  // fits beside neither of them
    assert(root_leaf(t, &root) == 3);

    slotted_page p = {0};
    uint64_t expect[] = { 10, 20, 30 };
    for (uint32_t i = 0; i < 3; i++) {
        read_leaf_page(t, &root, i, &p);
        assert(p.header.numRecords == 1 && SPSearch(&p, okey(expect[i])) == 0);
        assert(compareOrderingKeys(root.keys[i], okey(expect[i])) == 0);
    }
    freeSPage(&p);
    assert(has_row(t, 10) && has_row(t, 20) && has_row(t, 30));
    deleteTree(t);

    // the page is cut where the new row belongs, wherever that is: here, after the first of its four rows
    t = create_test_tree("bt_pst2");
    assert(insert_row(t, 10, 1800) == 0 && insert_row(t, 20, 1800) == 0);
    assert(insert_row(t, 30, SMALL_ROW) == 0 && insert_row(t, 40, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 1);
    assert(insert_row(t, 15, 3000) == 0);
    assert(root_leaf(t, &root) == 3);
    uint64_t largest[] = { 10, 15, 40 };
    uint32_t rows[] = { 1, 1, 3 };
    slotted_page q = {0};
    for (uint32_t i = 0; i < 3; i++) {
        read_leaf_page(t, &root, i, &q);
        assert(q.header.numRecords == rows[i]);
        assert(compareOrderingKeys(q.header.maxKey, okey(largest[i])) == 0);
        assert(compareOrderingKeys(root.keys[i], okey(largest[i])) == 0);
    }
    freeSPage(&q);
    assert(has_row(t, 10) && has_row(t, 15) && has_row(t, 20) && has_row(t, 30) && has_row(t, 40));
    deleteTree(t);
    printf("PASS\n");
}

/*
Making a row larger than its page has room for splits the page, and every row is still found with the right
contents. A row made too large for any page is refused, and nothing changes.
*/
void test_btree_update_grows_row(void) {
    printf("  test_btree_update_grows_row ... ");
    table* t = create_test_tree("bt_ugr");
    uint32_t perPage = ROWS_PER_PAGE(SMALL_ROW);
    node root = {0};
    for (uint64_t k = 1; k <= perPage; k++) assert(insert_row(t, k, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 1);

    uint64_t grown = perPage / 2;
    entry big = make_row(grown, 3000);
    sp_record rec = make_btree_record(&big, 1);
    assert(updateRecord(&rec, okey(grown), t));
    assert(root_leaf(t, &root) >= 2);
    for (uint32_t i = 0; i < root.childCount; i++) {
        slotted_page p = {0};
        read_leaf_page(t, &root, i, &p);
        assert(SPFits(&p) && p.header.numRecords > 0);
        assert(compareOrderingKeys(p.header.maxKey, root.keys[i]) <= 0);
        freeSPage(&p);
    }
    for (uint64_t k = 1; k <= perPage; k++) assert(has_row(t, k));
    slotted_page p = {0};
    sp_record r = readRecord(okey(grown), t, &p);
    assert(r.entries != NULL && r.entries[0].size == 3000);
    freeSPage(&p);

    // too large for any page
    uint32_t pagesBefore = root.childCount;
    entry huge = make_row(1, PAGE_ARR_CAP);
    sp_record hugeRec = make_btree_record(&huge, 1);
    assert(!updateRecord(&hugeRec, okey(1), t));
    free(huge.data);  // a refused update leaves the entries with the caller
    assert(root_leaf(t, &root) == pagesBefore);
    for (uint64_t k = 1; k <= perPage; k++) assert(has_row(t, k));
    slotted_page first = {0};
    r = readRecord(okey(1), t, &first);
    assert(r.entries != NULL && r.entries[0].size == SMALL_ROW);
    freeSPage(&first);

    entry other = make_row(perPage + 50, SMALL_ROW);
    sp_record otherRec = make_btree_record(&other, 1);
    assert(!updateRecord(&otherRec, okey(perPage + 50), t));  // above every key
    assert(insert_row(t, perPage + 60, SMALL_ROW) == 0);
    assert(!updateRecord(&otherRec, okey(perPage + 50), t));  // covered by a page, but not in it
    free(other.data);
    deleteTree(t);
    printf("PASS\n");
}

/*
The smallest key there is, which an empty tree's page is filed under as a placeholder, is a key like any
other: its row can be inserted first or last, found, and deleted.
*/
void test_btree_smallest_key(void) {
    printf("  test_btree_smallest_key ... ");
    table* t = create_test_tree("bt_sk");
    assert(insert_row(t, 0, SMALL_ROW) == 0);
    assert(insert_row(t, 0, SMALL_ROW) == 1);
    assert(has_row(t, 0) && !has_row(t, 1));
    for (uint64_t k = 1; k <= 3 * ROWS_PER_PAGE(SMALL_ROW); k++) assert(insert_row(t, k, SMALL_ROW) == 0);
    assert(count_pages(t) == 4);
    assert(has_row(t, 0));
    delete_row(t, 0);
    assert(!has_row(t, 0) && has_row(t, 1));
    assert(insert_row(t, 0, SMALL_ROW) == 0);  // below every key in the tree
    assert(has_row(t, 0));
    for (uint64_t k = 1; k <= 3 * ROWS_PER_PAGE(SMALL_ROW); k++) assert(has_row(t, k));
    deleteTree(t);
    printf("PASS\n");
}

/*
Keys of different types can't be compared, so a tree takes only keys of its own type. A key of another
type is refused, found nowhere, and leaves the tree able to take the keys it should, whether the tree was
empty or not.
*/
void test_btree_key_of_another_type(void) {
    printf("  test_btree_key_of_another_type ... ");
    for (int rows = 0; rows <= 4 * (int)ROWS_PER_PAGE(SMALL_ROW) * M_GLOBAL / 3; rows += 4 * ROWS_PER_PAGE(SMALL_ROW) * M_GLOBAL / 3) {
        table* t = create_test_tree("bt_koat");  // numeric keys; empty, then with enough rows for two levels
        for (int k = 1; k <= rows; k++) assert(insert_row(t, k, SMALL_ROW) == 0);
        uint32_t pages = count_pages(t);

        entry e = make_btree_entry("stray");
        sp_record r = make_btree_record(&e, 1);
        assert(insertRecord(&r, okey_text("stray"), t) == 2);
        assert(insertRecord(&r, okey_text(""), t) == 2);
        assert(findPage(okey_text("stray"), t) == 0 && !searchRecord(okey_text("stray"), t));
        assert(!updateRecord(&r, okey_text("stray"), t));
        free(e.data);
        assert(count_pages(t) == pages);

        assert(insert_row(t, rows + 1, SMALL_ROW) == 0);  // the tree still takes its own keys
        for (int k = 1; k <= rows + 1; k++) assert(has_row(t, k));
        deleteTree(t);
    }

    // and the other way round: a numeric key in a tree of string keys
    table* t = createTree("bt_koat2", ORDERING_STRING);
    entry e = make_btree_entry("stray");
    sp_record r = make_btree_record(&e, 1);
    assert(insertRecord(&r, okey(7), t) == 2);
    assert(findPage(okey(7), t) == 0);
    assert(insertRecord(&r, okey_text("pear"), t) == 0);  // the page owns e's data now
    assert(insertRecord(&r, okey(7), t) == 2);
    assert(searchRecord(okey_text("pear"), t) && !searchRecord(okey(7), t));
    deleteTree(t);
    printf("PASS\n");
}

/*
A new tree's first page is filed under a key of the table's type from the moment newTree() builds it, while
the table is still only in memory, so the first keys inserted are compared with a key of their own type.
Those rows are found before the table's first commit and after it, from the file.
*/
void test_btree_new_tree_before_commit(void) {
    printf("  test_btree_new_tree_before_commit ... ");
    table* t = newTree("bt_ntbc", ORDERING_STRING);
    assert(t != NULL && t->keyType == ORDERING_STRING && !tbl_file_exists("bt_ntbc"));
    node root = {0};
    assert(readNode(t->root, &root, t));  // from the pending changes: there is no file yet
    assert(root.keys[0].type == ORDERING_STRING && root.maxKey.type == ORDERING_STRING);

    const char* names[] = { "pear", "apple", "fig" };
    for (int i = 0; i < 3; i++) {
        entry e = make_btree_entry(names[i]);
        sp_record r = make_btree_record(&e, 1);
        assert(insertRecord(&r, okey_text(names[i]), t) == 0);
    }
    for (int i = 0; i < 3; i++) assert(searchRecord(okey_text(names[i]), t));
    assert(readNode(t->root, &root, t));
    assert(compareOrderingKeys(root.keys[0], okey_text("pear")) == 0);  // the page's bound followed its largest key

    assert(commit(t));
    close_table_keep_file(t);
    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_ntbc", t2));
    for (int i = 0; i < 3; i++) assert(searchRecord(okey_text(names[i]), t2));
    assert(!searchRecord(okey_text("plum"), t2));
    deleteTree(t2);
    printf("PASS\n");
}

// ── Group 5: the whole tree, as rows come and go ───────────────────────────

/* What check_tree() has found on its way through a tree. */
typedef struct tree_check {
    table* t;
    const bool* present;  // present[k] says whether key k should be in the tree
    uint64_t maxKey;      // the largest key that might be
    uint64_t rows;        // rows found so far
    uint32_t pages;       // pages found so far
    bool hasLast;         // whether a page or node key has been passed yet
    ordering_key last;    // the last one passed: every key from here on must be above it
} tree_check;

/*
Asserts that a page is sound and that its rows belong in it: sorted, above every key passed so far, at
most the key the page is filed under, each one a row that should exist, and within the page's capacity.
An empty page is only allowed to be the tree's only page.
*/
static void check_page(tree_check* c, address pageAddr, ordering_key filedUnder, bool onlyPage) {
    slotted_page p = {0};
    assert(readPage(pageAddr, &p, c->t));
    assert(SPFits(&p));
    assert(p.header.numRecords > 0 || onlyPage);
    uint32_t data = 0;
    for (uint32_t i = 0; i < p.header.numRecords; i++) {
        ordering_key k = p.slots[i].ID;
        if (i > 0) assert(compareOrderingKeys(p.slots[i - 1].ID, k) < 0);
        if (c->hasLast) assert(compareOrderingKeys(k, c->last) > 0);
        assert(compareOrderingKeys(k, filedUnder) <= 0);
        assert(k.as.u64 >= 1 && k.as.u64 <= c->maxKey && c->present[k.as.u64]);
        assert(p.slots[i].len == 1 && is_row_for(p.entries[p.slots[i].ptr], k.as.u64));
        data += p.slots[i].size;
        c->rows++;
    }
    assert(p.header.usedData == data && p.header.numEntries == p.header.numRecords);
    if (p.header.numRecords > 0) {
        assert(compareOrderingKeys(p.header.minKey, p.slots[0].ID) == 0);
        assert(compareOrderingKeys(p.header.maxKey, p.slots[p.header.numRecords - 1].ID) == 0);
    }
    c->pages++;
    freeSPage(&p);
}

/*
Asserts the structure of the subtree at addr: every node points back at its parent and is at least half
full unless it's the root, every key is above the ones before it, and each child's key is an upper bound
for everything under that child.
*/
static void check_node(tree_check* c, address addr, address parent) {
    node n = {0};
    assert(readNode(addr, &n, c->t));
    assert(n.parent == parent);
    assert(n.childCount >= 1 && n.childCount <= M_GLOBAL);
    if (parent) assert(n.childCount >= HALF_M);
    for (uint32_t i = 0; i < n.childCount; i++) {
        assert(n.children[i] != 0);
        if (n.isLeaf) {
            if (c->hasLast) assert(compareOrderingKeys(n.keys[i], c->last) > 0);
            check_page(c, n.children[i], n.keys[i], !parent && n.childCount == 1);
            c->last = n.keys[i];
        } else {
            check_node(c, n.children[i], addr);
            if (i < n.childCount - 1) {
                assert(compareOrderingKeys(c->last, n.keys[i]) <= 0);
                c->last = n.keys[i];
            }
        }
        c->hasLast = true;
    }
}

/*
Asserts that the tree holds exactly the rows marked in present[1..maxKey] and that its whole structure is
sound, that the leaves' linked list runs through the same pages in key order, and that a sample of keys,
there or not, is found or not by searching from the root.
*/
static void check_tree(table* t, const bool* present, uint64_t maxKey) {
    uint64_t expected = 0;
    for (uint64_t k = 1; k <= maxKey; k++) expected += present[k];
    tree_check c = { .t = t, .present = present, .maxKey = maxKey };
    check_node(&c, t->root, 0);
    assert(c.rows == expected);

    node n = {0};
    address addr = t->root;
    assert(readNode(addr, &n, t));
    while (!n.isLeaf) {
        addr = n.children[0];
        assert(readNode(addr, &n, t));
    }
    uint32_t pages = 0;
    address prevAddr = 0;
    bool hasLast = false;
    ordering_key last = {0};
    for (;;) {
        assert(n.prev == prevAddr);
        for (uint32_t i = 0; i < n.childCount; i++) {
            if (hasLast) assert(compareOrderingKeys(n.keys[i], last) > 0);
            last = n.keys[i];
            hasLast = true;
            pages++;
        }
        if (!n.next) break;
        prevAddr = addr;
        addr = n.next;
        assert(readNode(addr, &n, t));
    }
    assert(pages == c.pages);

    for (uint64_t k = 1; k <= maxKey; k += 1 + maxKey / 97) assert(has_row(t, k) == present[k]);
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

/* Inserts rows of one size under the keys listed, checking the whole tree after every checkEvery of them. */
static void insert_and_check(table* t, bool* present, uint64_t maxKey, const uint32_t* order, uint32_t count, uint32_t rowSize, uint32_t checkEvery) {
    for (uint32_t i = 0; i < count; i++) {
        assert(insert_row(t, order[i], rowSize) == 0);
        present[order[i]] = true;
        if ((i + 1) % checkEvery == 0 || i + 1 == count) check_tree(t, present, maxKey);
    }
}

/* Deletes the rows under the keys listed, checking the whole tree after every checkEvery of them. */
static void delete_and_check(table* t, bool* present, uint64_t maxKey, const uint32_t* order, uint32_t count, uint32_t checkEvery) {
    for (uint32_t i = 0; i < count; i++) {
        delete_row(t, order[i]);
        present[order[i]] = false;
        if ((i + 1) % checkEvery == 0 || i + 1 == count) check_tree(t, present, maxKey);
    }
}

/* Fills order with first, first+1, ... */
static void in_order(uint32_t* order, uint32_t count, uint32_t first) {
    for (uint32_t i = 0; i < count; i++) order[i] = first + i;
}

#define SMALL_ROWS 3000

/*
Small rows inserted in random order land in the middle of pages, which split all over the tree; deleted in
random order, pages empty one by one and leave it. The tree ends as it began, with one empty page, and takes
rows again.
*/
void test_btree_small_rows_in_random_order(void) {
    printf("  test_btree_small_rows_in_random_order ... ");
    bool* present = calloc(SMALL_ROWS + 1, sizeof(bool));
    uint32_t* order = malloc(SMALL_ROWS * sizeof(uint32_t));
    table* t = create_test_tree("bt_srro");
    uint32_t state = 2024;
    in_order(order, SMALL_ROWS, 1);
    shuffle(order, SMALL_ROWS, &state);
    insert_and_check(t, present, SMALL_ROWS, order, SMALL_ROWS, SMALL_ROW, 50);
    // pages that split in the middle are left about half full, so there are more than rows in order would need
    uint32_t packed = SMALL_ROWS / ROWS_PER_PAGE(SMALL_ROW) + 1;
    assert(count_pages(t) > packed && count_pages(t) <= 2 * packed);

    shuffle(order, SMALL_ROWS, &state);
    delete_and_check(t, present, SMALL_ROWS, order, SMALL_ROWS, 50);
    node root = {0};
    assert(root_leaf(t, &root) == 1);
    slotted_page p = {0};
    read_leaf_page(t, &root, 0, &p);
    assert(p.header.numRecords == 0);
    freeSPage(&p);

    shuffle(order, SMALL_ROWS, &state);
    insert_and_check(t, present, SMALL_ROWS, order, 500, SMALL_ROW, 100);
    deleteTree(t);
    free(present);
    free(order);
    printf("PASS\n");
}

// with two rows to a page, this many rows makes about six leaf nodes' worth of pages
#define LARGE_ROWS (6 * M_GLOBAL * 2)

/* Returns a tree of large rows under keys 1..LARGE_ROWS, inserted in order, and marks them in present. */
static table* create_large_row_tree(char* name, bool* present) {
    table* t = create_test_tree(name);
    uint32_t* order = malloc(LARGE_ROWS * sizeof(uint32_t));
    in_order(order, LARGE_ROWS, 1);
    insert_and_check(t, present, LARGE_ROWS, order, LARGE_ROWS, LARGE_ROW, 40);
    free(order);
    node root = {0};
    assert(readNode(t->root, &root, t) && !root.isLeaf && root.childCount >= 4);  // the leaf nodes have split
    return t;
}

/*
As pages fill a leaf node it splits, the root becomes an internal node over the leaves, and the leaves stay
linked in key order.
*/
void test_btree_node_split(void) {
    printf("  test_btree_node_split ... ");
    table* t = create_test_tree("bt_ns");
    bool present[2 * (M_GLOBAL + 2) + 1] = {0};
    uint32_t rows = 2 * (M_GLOBAL + 1);  // two to a page: one page more than a node holds
    node root = {0};
    for (uint32_t k = 1; k <= rows; k++) {
        assert(insert_row(t, k, LARGE_ROW) == 0);
        present[k] = true;
        if (k == 2 * M_GLOBAL) assert(root_leaf(t, &root) == M_GLOBAL);  // the root is full, and still a leaf
    }
    assert(readNode(t->root, &root, t));
    assert(!root.isLeaf && root.childCount == 2);
    node left = {0}, right = {0};
    assert(readNode(root.children[0], &left, t) && readNode(root.children[1], &right, t));
    assert(left.isLeaf && right.isLeaf);
    assert(left.childCount + right.childCount == M_GLOBAL + 1);
    assert(left.next == root.children[1] && right.prev == root.children[0] && left.prev == 0 && right.next == 0);
    assert(compareOrderingKeys(root.keys[0], left.maxKey) == 0);
    assert(compareOrderingKeys(root.maxKey, okey(rows)) == 0);
    check_tree(t, present, rows + 2);
    deleteTree(t);
    printf("PASS\n");
}

/*
Deleting from the last leaf backwards makes every rebalance borrow from, or merge into, the
previous leaf, which deleting from the front of the tree never does.
*/
void test_btree_delete_from_the_end(void) {
    printf("  test_btree_delete_from_the_end ... ");
    bool present[LARGE_ROWS + 1] = {0};
    uint32_t order[LARGE_ROWS];
    table* t = create_large_row_tree("bt_dfe", present);
    for (uint32_t i = 0; i < LARGE_ROWS; i++) order[i] = LARGE_ROWS - i;
    delete_and_check(t, present, LARGE_ROWS, order, LARGE_ROWS, 3);
    deleteTree(t);
    printf("PASS\n");
}

/*
Deleting outwards from the middle rebalances leaves that have siblings on both sides, so a leaf
that isn't its parent's first child borrows from the next one.
*/
void test_btree_delete_from_the_middle(void) {
    printf("  test_btree_delete_from_the_middle ... ");
    bool present[LARGE_ROWS + 1] = {0};
    uint32_t order[LARGE_ROWS];
    table* t = create_large_row_tree("bt_dfm", present);
    uint32_t lo = LARGE_ROWS / 2, hi = lo + 1;
    for (uint32_t i = 0; i < LARGE_ROWS; i++) order[i] = (i % 2 == 0) ? hi++ : lo--;
    delete_and_check(t, present, LARGE_ROWS, order, LARGE_ROWS, 3);
    deleteTree(t);
    printf("PASS\n");
}

/* Deleting every other row, then the rest, half-empties every page before any of them goes. */
void test_btree_delete_alternating(void) {
    printf("  test_btree_delete_alternating ... ");
    bool present[LARGE_ROWS + 1] = {0};
    uint32_t order[LARGE_ROWS];
    table* t = create_large_row_tree("bt_dalt", present);
    uint32_t count = 0;
    for (uint32_t k = 2; k <= LARGE_ROWS; k += 2) order[count++] = k;
    for (uint32_t k = 1; k <= LARGE_ROWS; k += 2) order[count++] = k;
    delete_and_check(t, present, LARGE_ROWS, order, LARGE_ROWS, 3);
    deleteTree(t);
    printf("PASS\n");
}

// two to a page and at least HALF_M pages to a leaf node: enough leaves that the root can't hold them all
#define DEEP_ROWS (2 * (HALF_M + 1) * (M_GLOBAL + 20))

/*
A tree three levels deep loses rows in random order, is refilled, and is emptied again, so internal
nodes borrow through their parent and merge as well as leaves.
*/
void test_btree_delete_random_deep(void) {
    printf("  test_btree_delete_random_deep ... ");
    bool* present = calloc(DEEP_ROWS + 1, sizeof(bool));
    uint32_t* order = malloc(DEEP_ROWS * sizeof(uint32_t));
    table* t = create_test_tree("bt_drd");
    in_order(order, DEEP_ROWS, 1);
    insert_and_check(t, present, DEEP_ROWS, order, DEEP_ROWS, LARGE_ROW, 500);
    node root = {0}, child = {0};
    readNode(t->root, &root, t);
    readNode(root.children[0], &child, t);
    assert(!root.isLeaf && !child.isLeaf);  // three levels

    uint32_t state = 12345;
    shuffle(order, DEEP_ROWS, &state);
    uint32_t most = DEEP_ROWS * 9 / 10;
    delete_and_check(t, present, DEEP_ROWS, order, most, 24);
    // put them back in a different order
    shuffle(order, most, &state);
    insert_and_check(t, present, DEEP_ROWS, order, most, LARGE_ROW, 100);
    // and empty the tree
    in_order(order, DEEP_ROWS, 1);
    shuffle(order, DEEP_ROWS, &state);
    delete_and_check(t, present, DEEP_ROWS, order, DEEP_ROWS, 24);

    deleteTree(t);
    free(present);
    free(order);
    printf("PASS\n");
}

/*
A table that has been emptied keeps one page, so it can take rows again, including ones below any key it
ever held.
*/
void test_btree_emptied_tree_takes_rows(void) {
    printf("  test_btree_emptied_tree_takes_rows ... ");
    table* t = create_test_tree("bt_ettr");
    bool present[201] = {0};
    for (uint64_t k = 101; k <= 200; k++) assert(insert_row(t, k, LARGE_ROW) == 0);
    for (uint64_t k = 101; k <= 200; k++) delete_row(t, k);
    check_tree(t, present, 200);
    node root = {0};
    assert(root_leaf(t, &root) == 1);
    for (uint64_t k = 1; k <= 200; k += 7) {
        assert(insert_row(t, k, SMALL_ROW) == 0);
        present[k] = true;
    }
    check_tree(t, present, 200);
    deleteTree(t);
    printf("PASS\n");
}

/* Replaces the row under key k in a page held in memory with one of another size. */
static void resize_row_in_page(slotted_page* p, uint64_t k, uint32_t size) {
    entry e = make_row(k, size);
    assert(SPUpdate(p, okey(k), make_btree_record(&e, 1)));
}

/*
storePage() stores a page whose rows were changed in memory. One that no longer fits is split into as many
pages as its rows need, and the caller is told; one with a row too large for any page is refused before
anything in the tree changes.
*/
void test_btree_store_page(void) {
    printf("  test_btree_store_page ... ");
    table* t = create_test_tree("bt_sp");
    uint32_t perPage = ROWS_PER_PAGE(SMALL_ROW);
    bool present[ROWS_PER_PAGE(SMALL_ROW) + 1] = {0};
    for (uint64_t k = 1; k <= perPage; k++) {
        assert(insert_row(t, k, SMALL_ROW) == 0);
        present[k] = true;
    }
    node root = {0};
    assert(root_leaf(t, &root) == 1);
    address addr = root.children[0];
    slotted_page p = {0};
    assert(readPage(addr, &p, t));

    // nothing changed, and a row made smaller: the page is stored as it is
    bool split = true;
    assert(storePage(&p, addr, t, &split) && !split);
    resize_row_in_page(&p, 5, SMALL_ROW - 4);
    assert(storePage(&p, addr, t, &split) && !split);
    assert(count_pages(t) == 1);

    // three rows that each take half a page: the rows can't be divided between two pages
    resize_row_in_page(&p, 10, LARGE_ROW);
    resize_row_in_page(&p, perPage / 2, LARGE_ROW);
    resize_row_in_page(&p, perPage - 10, LARGE_ROW);
    assert(SPUsedBytes(&p) > 2 * PAGE_ARR_CAP);
    assert(storePage(&p, addr, t, &split) && split);
    assert(count_pages(t) >= 3);
    assert(SPFits(&p) && SPSearch(&p, okey(1)) == 0);  // p is left with the rows that stayed where it was
    check_tree(t, present, perPage);

    // a row too large for any page
    uint32_t pages = count_pages(t);
    assert(readPage(addr, &p, t));
    resize_row_in_page(&p, 1, PAGE_ARR_CAP);
    split = true;
    assert(!storePage(&p, addr, t, &split) && !split);
    assert(count_pages(t) == pages);
    check_tree(t, present, perPage);
    slotted_page stored = {0};
    sp_record r = readRecord(okey(1), t, &stored);
    assert(r.entries != NULL && r.entries[0].size == SMALL_ROW);
    freeSPage(&stored);
    freeSPage(&p);
    deleteTree(t);

    // one row grows to nearly a page: it keeps the page, and all the others move to a new one together,
    // more of them than a new page's slot array starts with room for
    t = create_test_tree("bt_sp2");
    for (uint64_t k = 1; k <= perPage; k++) assert(insert_row(t, k, SMALL_ROW) == 0);
    assert(root_leaf(t, &root) == 1);
    addr = root.children[0];
    slotted_page q = {0};
    assert(readPage(addr, &q, t));
    resize_row_in_page(&q, 1, PAGE_ARR_CAP - 100);
    assert(storePage(&q, addr, t, &split) && split);
    assert(q.header.numRecords == 1);
    assert(root_leaf(t, &root) == 2);
    read_leaf_page(t, &root, 1, &q);
    assert(q.header.numRecords == perPage - 1);
    freeSPage(&q);
    check_tree(t, present, perPage);
    deleteTree(t);
    printf("PASS\n");
}

// ── Group 6: full persistence roundtrip ───────────────────────────────────

/*
Create a tree with rows across many pages and several nodes, change it again, commit, close and reopen:
the tree read back from disk holds exactly the same rows, and its structure is sound.
*/
void test_btree_full_roundtrip(void) {
    printf("  test_btree_full_roundtrip ... ");
    bool present[LARGE_ROWS + 1] = {0};
    table* t = create_large_row_tree("bt_fr", present);
    assert(commit(t));
    for (uint32_t k = 3; k <= LARGE_ROWS; k += 3) {  // a second commit, over the first
        delete_row(t, k);
        present[k] = false;
    }
    assert(commit(t));
    close_table_keep_file(t);

    table* t2 = calloc(1, sizeof(table));
    assert(loadTable("bt_fr", t2));
    assert(t2->pageDirty.count == 0 && t2->nodeDirty.count == 0);
    check_tree(t2, present, LARGE_ROWS);
    for (uint32_t k = 1; k <= LARGE_ROWS; k++) assert(has_row(t2, k) == present[k]);

    deleteTree(t2);
    printf("PASS\n");
}

// ── Group 7: tree cleanup ──────────────────────────────────────────────────

/* deleteTree must remove the backing file from disk. */
void test_btree_delete_tree(void) {
    printf("  test_btree_delete_tree ... ");
    table* t = create_test_tree("bt_dt");
    assert(tbl_file_exists("bt_dt"));
    deleteTree(t);
    assert(!tbl_file_exists("bt_dt"));
    printf("PASS\n");
}

/* After deleteTree, loadTable must fail for the same name. */
void test_btree_delete_tree_not_reloadable(void) {
    printf("  test_btree_delete_tree_not_reloadable ... ");
    table* t = create_test_tree("bt_dtnr");
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
    test_btree_find_empty();
    test_btree_find_covering_page();
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
    // insert, and pages splitting
    test_btree_insert_fills_one_page();
    test_btree_insert_too_large();
    test_btree_page_split_in_order();
    test_btree_page_split_in_the_middle();
    test_btree_page_split_three_ways();
    test_btree_update_grows_row();
    test_btree_smallest_key();
    test_btree_key_of_another_type();
    test_btree_new_tree_before_commit();
    // the whole tree, as rows come and go
    test_btree_small_rows_in_random_order();
    test_btree_node_split();
    test_btree_delete_from_the_end();
    test_btree_delete_from_the_middle();
    test_btree_delete_alternating();
    test_btree_delete_random_deep();
    test_btree_emptied_tree_takes_rows();
    test_btree_store_page();
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
    table* t = createTree(name, ORDERING_ULONG);
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
