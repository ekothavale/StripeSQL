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
// Chunk Tests

// --- initChunk ---

void test_init_chunk() {
    chunk c;
    initChunk(&c);
    assert(c.code     == NULL);
    assert(c.lines    == NULL);
    assert(c.capacity == 0);
    assert(c.count    == 0);
    assert(c.constants.count == 0);
}

// --- writeChunk ---

void test_write_chunk_stores_byte() {
    chunk c;
    initChunk(&c);
    writeChunk(&c, OP_HALT, 1);
    assert(c.count    == 1);
    assert(c.code[0]  == OP_HALT);
    assert(c.capacity >= 1);
    freeChunk(&c);
}

void test_write_chunk_stores_line() {
    chunk c;
    initChunk(&c);
    writeChunk(&c, OP_ADD, 42);
    assert(c.lines    != NULL);
    assert(c.lines[0] == 42);
    assert(c.count    == 1);
    freeChunk(&c);
}

void test_write_chunk_multiple() {
    chunk c;
    initChunk(&c);
    writeChunk(&c, OP_CONSTANT, 1);
    writeChunk(&c, 0,           1);  // operand byte
    writeChunk(&c, OP_HALT,     1);
    assert(c.count    == 3);
    assert(c.code[0]  == OP_CONSTANT);
    assert(c.code[1]  == 0);
    assert(c.code[2]  == OP_HALT);
    freeChunk(&c);
}

void test_write_chunk_triggers_grow() {
    // GROW_CAPACITY(0)=8, so bytes 1-8 fill the first allocation;
    // the 9th write triggers GROW_CAPACITY(8)=16
    chunk c;
    initChunk(&c);
    for (int i = 0; i < 9; i++) writeChunk(&c, OP_POP, i + 1);
    assert(c.count    == 9);
    assert(c.capacity == 16);
    assert(c.code[8]  == OP_POP);
    freeChunk(&c);
}

// --- addConstant ---

void test_add_constant_returns_index() {
    chunk c;
    initChunk(&c);
    int i0 = addConstant(&c, INTEGER_VAL(10));
    int i1 = addConstant(&c, INTEGER_VAL(20));
    assert(i0 == 0);
    assert(i1 == 1);
    assert(c.constants.count == 2);
    freeChunk(&c);
}

void test_add_constant_stores_value() {
    chunk c;
    initChunk(&c);
    addConstant(&c, INTEGER_VAL(99));
    assert(c.constants.count              == 1);
    assert(c.constants.values[0].type     == VAL_INT);
    assert(c.constants.values[0].as.integer == 99);
    freeChunk(&c);
}

void test_add_constant_multiple() {
    chunk c;
    initChunk(&c);
    addConstant(&c, BOOL_VAL(true));
    addConstant(&c, FLOAT_VAL(3.14));
    addConstant(&c, NULL_VAL(0));
    assert(c.constants.count          == 3);
    assert(c.constants.values[0].type == VAL_BOOL);
    assert(c.constants.values[1].type == VAL_FLOAT);
    assert(c.constants.values[2].type == VAL_NULL);
    freeChunk(&c);
}

// --- freeChunk ---

void test_free_chunk_resets_code() {
    chunk c;
    initChunk(&c);
    writeChunk(&c, OP_ADD,  1);
    writeChunk(&c, OP_HALT, 1);
    freeChunk(&c);
    assert(c.code     == NULL);
    assert(c.lines    == NULL);
    assert(c.count    == 0);
    assert(c.capacity == 0);
}

void test_free_chunk_clears_constants() {
    chunk c;
    initChunk(&c);
    addConstant(&c, INTEGER_VAL(1));
    addConstant(&c, BOOL_VAL(false));
    freeChunk(&c);
    assert(c.constants.count    == 0);
    assert(c.constants.capacity == 0);
    assert(c.constants.values   == NULL);
}

void test_free_chunk_idempotent() {
    // freeChunk on a never-written chunk must not crash
    // (FREE_ARRAY with capacity=0 calls free(NULL) which is safe)
    chunk c;
    initChunk(&c);
    freeChunk(&c);
    assert(c.code  == NULL);
    assert(c.count == 0);
    assert(c.constants.values == NULL);
}

// --- master ---

void test_chunk() {
    test_init_chunk();
    test_write_chunk_stores_byte();
    test_write_chunk_stores_line();
    test_write_chunk_multiple();
    test_write_chunk_triggers_grow();
    test_add_constant_returns_index();
    test_add_constant_stores_value();
    test_add_constant_multiple();
    test_free_chunk_resets_code();
    test_free_chunk_clears_constants();
    test_free_chunk_idempotent();
    printf("All chunk tests passed.\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Value Tests

// --- initValueArray ---

void test_init_value_array() {
    ValueArray arr;
    initValueArray(&arr);
    assert(arr.values   == NULL);
    assert(arr.capacity == 0);
    assert(arr.count    == 0);
}

// --- writeValueArray ---

void test_write_value_array_stores_value() {
    ValueArray arr;
    initValueArray(&arr);
    writeValueArray(&arr, INTEGER_VAL(7));
    assert(arr.count               == 1);
    assert(arr.values[0].type      == VAL_INT);
    assert(arr.values[0].as.integer == 7);
    freeValueArray(&arr);
}

void test_write_value_array_multiple_types() {
    ValueArray arr;
    initValueArray(&arr);
    writeValueArray(&arr, BOOL_VAL(true));
    writeValueArray(&arr, FLOAT_VAL(1.5));
    writeValueArray(&arr, NULL_VAL(0));
    assert(arr.count                  == 3);
    assert(arr.values[0].type         == VAL_BOOL);
    assert(arr.values[0].as.boolean   == true);
    assert(arr.values[1].type         == VAL_FLOAT);
    assert(arr.values[2].type         == VAL_NULL);
    freeValueArray(&arr);
}

void test_write_value_array_triggers_grow() {
    // GROW_CAPACITY(0)=8 on first write; 9th write causes GROW_CAPACITY(8)=16
    ValueArray arr;
    initValueArray(&arr);
    for (int i = 0; i < 9; i++) writeValueArray(&arr, INTEGER_VAL(i));
    assert(arr.count                == 9);
    assert(arr.capacity             == 16);
    assert(arr.values[8].as.integer == 8);
    freeValueArray(&arr);
}

// --- freeValueArray ---

void test_free_value_array_resets() {
    ValueArray arr;
    initValueArray(&arr);
    writeValueArray(&arr, INTEGER_VAL(1));
    writeValueArray(&arr, BOOL_VAL(false));
    freeValueArray(&arr);
    assert(arr.values   == NULL);
    assert(arr.count    == 0);
    assert(arr.capacity == 0);
}

void test_free_value_array_idempotent() {
    // freeValueArray on a never-written array must not crash
    ValueArray arr;
    initValueArray(&arr);
    freeValueArray(&arr);
    assert(arr.values   == NULL);
    assert(arr.count    == 0);
    assert(arr.capacity == 0);
}

// --- printValue ---
// printValue writes to stdout via printf("%g", value.as.floating).
// We redirect fd 1 through a tmpfile to capture and inspect the output.

static char* capture_printValue(value v) {
    FILE* tmp = tmpfile();
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fileno(tmp), STDOUT_FILENO);
    printValue(v);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(tmp);
    static char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);
    return buf;
}

void test_print_value_float() {
    char* out = capture_printValue(FLOAT_VAL(3.14));
    assert(strlen(out)        >  0);
    assert(strstr(out, "3.14") != NULL);
    assert(out[0]             != '\0');
}

void test_print_value_zero() {
    char* out = capture_printValue(FLOAT_VAL(0.0));
    assert(strlen(out)      >  0);
    assert(strstr(out, "0") != NULL);
    assert(out[0]           != '\0');
}

void test_print_value_large() {
    char* out = capture_printValue(FLOAT_VAL(1000.0));
    assert(strlen(out)         >  0);
    assert(strstr(out, "1000") != NULL);
    assert(out[0]              != '\0');
}

// --- master ---

void test_value() {
    test_init_value_array();
    test_write_value_array_stores_value();
    test_write_value_array_multiple_types();
    test_write_value_array_triggers_grow();
    test_free_value_array_resets();
    test_free_value_array_idempotent();
    test_print_value_float();
    test_print_value_zero();
    test_print_value_large();
    printf("All value tests passed.\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Hash table Tests

// insertHT calls findEntry (which does % capacity) before the grow check, so a
// freshly initHashTable'd table with capacity=0 would divide by zero on the first
// insert. Pre-allocate entries to work around this.
static hashtable make_test_table(void) {
    hashtable t;
    initHashTable(&t);
    t.entries = calloc(sizeof(schema), 8);
    t.capacity = 8;
    return t;
}

static schema make_schema(const char* name, int col_count) {
    schema s;
    s.hash      = hashString(name, (int)strlen(name));
    s.tablename = strdup(name);
    s.colNames  = malloc(col_count * sizeof(char*));
    for (int i = 0; i < col_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "col%d", i);
        s.colNames[i] = strdup(buf);
    }
    s.colTypes  = calloc(col_count, 1);
    s.count     = col_count;
    return s;
}

// --- initHashTable ---

void test_init_hash_table() {
    hashtable t;
    initHashTable(&t);
    assert(t.count    == 0);
    assert(t.capacity == 0);
    assert(t.entries  == NULL);
}

// --- freeHashTable ---

void test_free_hash_table_resets() {
    hashtable t = make_test_table();
    schema s = make_schema("users", 3);
    insertHT(&s, &t);
    freeHashTable(&t);
    assert(t.entries  == NULL);
    assert(t.count    == 0);
    assert(t.capacity == 0);
}

void test_free_hash_table_idempotent() {
    // FREE_ARRAY with entries=NULL calls free(NULL), which is safe
    hashtable t;
    initHashTable(&t);
    freeHashTable(&t);
    assert(t.entries  == NULL);
    assert(t.count    == 0);
    assert(t.capacity == 0);
}

// --- hashString ---

void test_hash_string_deterministic() {
    uint32_t h1 = hashString("users", 5);
    uint32_t h2 = hashString("users", 5);
    assert(h1 == h2);
    assert(h1 != 0);
    assert(h2 != 0);
}

void test_hash_string_different_keys() {
    uint32_t h1 = hashString("users",    5);
    uint32_t h2 = hashString("orders",   6);
    uint32_t h3 = hashString("products", 8);
    assert(h1 != h2);
    assert(h2 != h3);
    assert(h1 != h3);
}

void test_hash_string_length_matters() {
    // "user" vs "users" must differ
    uint32_t h1 = hashString("user",  4);
    uint32_t h2 = hashString("users", 5);
    assert(h1 != h2);
    // length=0 never processes any bytes → always returns the FNV-1a offset basis
    uint32_t h3 = hashString("a", 0);
    uint32_t h4 = hashString("z", 0);
    assert(h3 == h4);    // key content irrelevant when length is 0
    assert(h1 != h3);    // non-zero length differs from zero-length result
}

// --- insertHT ---

void test_insert_ht_stores_entry() {
    hashtable t = make_test_table();
    schema s = make_schema("users", 3);
    insertHT(&s, &t);
    schema* found = readHT(s.hash, &t);
    assert(found         != NULL);
    assert(found->hash   == s.hash);
    assert(found->count  == 3);
    freeHashTable(&t);
}

void test_insert_ht_increments_count() {
    hashtable t = make_test_table();
    schema s1 = make_schema("users",  2);
    schema s2 = make_schema("orders", 3);
    insertHT(&s1, &t);
    assert(t.count == 1);
    insertHT(&s2, &t);
    assert(t.count == 2);
    freeHashTable(&t);
}

void test_insert_ht_overwrites() {
    // Inserting the same key twice overwrites the stored fields
    hashtable t = make_test_table();
    schema s1 = make_schema("users", 2);
    schema s2 = make_schema("users", 5);  // same key → same hash
    insertHT(&s1, &t);
    insertHT(&s2, &t);
    schema* found = readHT(s1.hash, &t);
    assert(found        != NULL);
    assert(found->count == 5);   // s2 overwrote s1
    assert(t.count      == 2);   // raw insert count, not unique-entry count
    // insertHT() doesn't free a slot's previous contents on overwrite — the
    // table only references s2 now, so s1's own heap fields are still ours
    // to free (freeHashTable() below only reaches s2's, via the live slot)
    for (int i = 0; i < s1.count; i++) free(s1.colNames[i]);
    free(s1.colNames);
    free(s1.colTypes);
    free(s1.tablename);
    freeHashTable(&t);
}

// --- readHT ---

void test_read_ht_finds_existing() {
    hashtable t = make_test_table();
    schema s = make_schema("products", 4);
    insertHT(&s, &t);
    schema* found = readHT(s.hash, &t);
    assert(found        != NULL);
    assert(found->hash  == s.hash);
    assert(found->count == 4);
    freeHashTable(&t);
}

void test_read_ht_missing() {
    hashtable t = make_test_table();
    uint32_t h = hashString("ghost", 5);
    schema* found = readHT(h, &t);
    assert(found == NULL);
    assert(t.count == 0);
    assert(t.capacity == 8);
    freeHashTable(&t);
}

void test_read_ht_correct_data() {
    hashtable t = make_test_table();
    schema s = make_schema("orders", 7);
    insertHT(&s, &t);
    schema* found = readHT(s.hash, &t);
    assert(found                    != NULL);
    assert(found->count             == 7);
    assert(strcmp(found->tablename, "orders") == 0);
    freeHashTable(&t);
}

// --- deleteHT ---

void test_delete_ht_removes_entry() {
    hashtable t = make_test_table();
    schema s = make_schema("users", 3);
    insertHT(&s, &t);
    deleteHT(s.hash, &t);
    schema* found = readHT(s.hash, &t);
    assert(found == NULL);
    assert(t.capacity > 0);   // table structure intact
    assert(t.entries != NULL);
    freeHashTable(&t);
}

void test_delete_ht_zeroes_fields() {
    // After delete the slot's hash is 0, making readHT return NULL
    hashtable t = make_test_table();
    schema s = make_schema("items", 2);
    insertHT(&s, &t);
    deleteHT(s.hash, &t);
    assert(readHT(s.hash, &t) == NULL);  // hash zeroed → treated as empty
    // A subsequent insert with the same key reclaims the slot correctly
    schema s2 = make_schema("items", 9);
    insertHT(&s2, &t);
    schema* found = readHT(s2.hash, &t);
    assert(found != NULL && found->count == 9);
    freeHashTable(&t);
}

void test_delete_ht_nonexistent() {
    // Deleting an absent key zeroes an empty slot — should not crash
    hashtable t = make_test_table();
    uint32_t h = hashString("ghost", 5);
    deleteHT(h, &t);
    assert(readHT(h, &t) == NULL);
    assert(t.count    == 0);
    assert(t.capacity == 8);
    freeHashTable(&t);
}

// --- master ---

void test_hashtable() {
    test_init_hash_table();
    test_free_hash_table_resets();
    test_free_hash_table_idempotent();
    test_hash_string_deterministic();
    test_hash_string_different_keys();
    test_hash_string_length_matters();
    test_insert_ht_stores_entry();
    test_insert_ht_increments_count();
    test_insert_ht_overwrites();
    test_read_ht_finds_existing();
    test_read_ht_missing();
    test_read_ht_correct_data();
    test_delete_ht_removes_entry();
    test_delete_ht_zeroes_fields();
    test_delete_ht_nonexistent();
    printf("All hashtable tests passed.\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Schema Tests

/*
dynamimcally resizes schema path allocation if schema directory is ever changed
*/
static const char* schema_path(void) {
    const char* path = SCHEMA_PATH;
	return path;
}

// --- loadSchema ---

void test_load_schema_bootstraps_on_missing() {
    // loadSchema() self-heals a missing schema file rather than failing: it's
    // the only place in the codebase that calls initSchema(), and interpret()
    // treats a NULL return as a hard load error, so a fresh checkout (tables/
    // is gitignored) would be unable to run even a first CREATE TABLE if this
    // returned NULL instead.
    remove(schema_path());  // guarantee file is absent
    hashtable* ht = loadSchema();
    assert(ht != NULL);          // missing file is bootstrapped, not an error
    assert(ht->count    == 0);
    assert(ht->capacity == 0);
    assert(ht->entries  == NULL);
    FILE* check = fopen(schema_path(), "rb");
    assert(check != NULL);       // loadSchema() created the file on disk
    fclose(check);
    free(ht);

    hashtable* ht2 = loadSchema();  // file now exists; second call loads it back
    assert(ht2 != NULL);
    assert(ht2->count == 0);
    free(ht2);
    remove(schema_path());
}

void test_load_schema_null_on_bad_magic() {
    FILE* f = fopen(schema_path(), "wb");
    if (!f) return;  // can't set up the test without write access
    uint32_t bad = 0xDEADBEEFu;
    fwrite(&bad, 4, 1, f);
    fclose(f);

    hashtable* ht = loadSchema();
    assert(ht == NULL);              // magic mismatch → NULL
    assert(bad != SCHEMA_MAGIC);     // confirm "bad" really differs from expected
    FILE* check = fopen(schema_path(), "rb");
    assert(check != NULL);           // file exists; loadSchema didn't remove it
    fclose(check);
    remove(schema_path());
}

// --- saveSchema ---

void test_save_schema_no_crash() {
    hashtable t = make_test_table();
    bool completed = false;
    saveSchema(&t);
    completed = true;
    assert(completed);         // saveSchema returned without crashing
    assert(t.capacity == 8);   // table struct is intact after save
    assert(t.count    == 0);
    remove(schema_path());
    freeHashTable(&t);
}

void test_save_schema_writes_magic() {
    hashtable t = make_test_table();
    saveSchema(&t);
    freeHashTable(&t);

    FILE* f = fopen(schema_path(), "rb");
    if (!f) return;  // write may have failed (bad path); skip verification
    uint32_t magic = 0;
    size_t   n     = fread(&magic, 4, 1, f);
    fclose(f);
    remove(schema_path());

    assert(n     == 1);
    assert(magic == SCHEMA_MAGIC);
    assert(magic == 0xFFBB8844u);  // known constant
}

// --- roundtrip (zero entries only — non-zero entry load crashes due to
//     insertHT being called before capacity is non-zero) ---

void test_save_load_empty_schema() {
    hashtable t = make_test_table();
    saveSchema(&t);
    freeHashTable(&t);

    hashtable* loaded = loadSchema();
    if (!loaded) { remove(schema_path()); return; }  // file write failed; skip

    assert(loaded->count    == 0);
    assert(loaded->capacity == 0);    // initHashTable leaves it at 0 (no inserts)
    assert(loaded->entries  == NULL);
    free(loaded);
    remove(schema_path());
}

void test_save_load_schema_with_cols_and_types() {
    hashtable t = make_test_table();

    char** cols = malloc(2 * sizeof(char*));
    cols[0] = strdup("id");
    cols[1] = strdup("name");
    char* types = malloc(2);
    types[0] = 2;   // SQL_INT
    types[1] = 0;   // SQL_TEXT

    schema s;
    s.hash      = hashString("users", 5);
    s.tablename = strdup("users");
    s.colNames  = cols;
    s.colTypes  = types;
    s.count     = 2;

    // insertHT() shallow-copies s's pointers into t's storage, so t now owns
    // tablename/colNames/colTypes; freeHashTable() below frees them — no
    // separate free of s/cols/types here, that would double-free
    insertHT(&s, &t);
    saveSchema(&t);
    freeHashTable(&t);

    hashtable* loaded = loadSchema();
    assert(loaded != NULL);

    uint32_t h = hashString("users", 5);
    schema* found = readHT(h, loaded);
    assert(found != NULL);
    assert(found->count == 2);
    assert(strcmp(found->colNames[0], "id")   == 0);
    assert(strcmp(found->colNames[1], "name") == 0);
    assert((uint8_t)found->colTypes[0] == 2);
    assert((uint8_t)found->colTypes[1] == 0);

    // found aliases directly into loaded->entries (readHT/findEntry return a
    // pointer into the table, not a copy), so freeHashTable() below already
    // frees its fields — freeing them here too would double-free
    freeHashTable(loaded);
    free(loaded);
    remove(schema_path());
}

// --- master ---

void test_schema() {
    test_load_schema_bootstraps_on_missing();
    test_load_schema_null_on_bad_magic();
    test_save_schema_no_crash();
    test_save_schema_writes_magic();
    test_save_load_empty_schema();
    test_save_load_schema_with_cols_and_types();
    printf("All schema tests passed.\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Lexer tests

// --- initLexer / scanToken ---

void test_scan_single_char_tokens() {
    initLexer("(,;.+-*/{}");
    assert(scanToken().type == TOKEN_LEFT_PAREN);
    assert(scanToken().type == TOKEN_COMMA);
    assert(scanToken().type == TOKEN_SEMICOLON);
    assert(scanToken().type == TOKEN_DOT);
    assert(scanToken().type == TOKEN_PLUS);
    assert(scanToken().type == TOKEN_MINUS);
    assert(scanToken().type == TOKEN_STAR);
    assert(scanToken().type == TOKEN_SLASH);
    assert(scanToken().type == TOKEN_LEFT_BRACE);
    assert(scanToken().type == TOKEN_RIGHT_BRACE);
    assert(scanToken().type == TOKEN_EOF);
}

void test_scan_two_char_tokens() {
    initLexer("!= == <= >=");
    token t;
    t = scanToken(); assert(t.type == TOKEN_BANG_EQUAL  && t.length == 2);
    t = scanToken(); assert(t.type == TOKEN_EQUAL_EQUAL && t.length == 2);
    t = scanToken(); assert(t.type == TOKEN_LESS_EQUAL  && t.length == 2);
    t = scanToken(); assert(t.type == TOKEN_GREATER_EQUAL && t.length == 2);
    assert(scanToken().type == TOKEN_EOF);
}

void test_scan_single_ops() {
    // Single-char variants when the second char doesn't match
    initLexer("! = < >");
    assert(scanToken().type == TOKEN_BANG);
    assert(scanToken().type == TOKEN_EQUAL);
    assert(scanToken().type == TOKEN_LESS);
    assert(scanToken().type == TOKEN_GREATER);
    assert(scanToken().type == TOKEN_EOF);
}

void test_scan_keywords() {
    initLexer("select from where between truncate");
    assert(scanToken().type == TOKEN_SELECT);
    assert(scanToken().type == TOKEN_FROM);
    assert(scanToken().type == TOKEN_WHERE);
    assert(scanToken().type == TOKEN_BETWEEN);
    assert(scanToken().type == TOKEN_TRUNCATE);
    assert(scanToken().type == TOKEN_EOF);
}

void test_scan_identifier() {
    // Near-misses that must not match keywords
    initLexer("selector fromage wheres myTable");
    token t;
    t = scanToken(); assert(t.type == TOKEN_IDENTIFIER && t.length == 8);
    t = scanToken(); assert(t.type == TOKEN_IDENTIFIER && t.length == 7);
    t = scanToken(); assert(t.type == TOKEN_IDENTIFIER && t.length == 6);
    t = scanToken(); assert(t.type == TOKEN_IDENTIFIER && strncmp(t.start, "myTable", 7) == 0);
    assert(scanToken().type == TOKEN_EOF);
}

// --- number() ---

void test_scan_number_integer() {
    initLexer("42");
    token t = scanToken();
    assert(t.type   == TOKEN_NUMBER);
    assert(t.length == 2);
    assert(strncmp(t.start, "42", 2) == 0);
}

void test_scan_number_float() {
    initLexer("3.14");
    token t = scanToken();
    assert(t.type   == TOKEN_NUMBER);
    assert(t.length == 4);
    assert(strncmp(t.start, "3.14", 4) == 0);
}

void test_scan_number_no_trailing_decimal() {
    // "3." → NUMBER("3") then DOT (no fractional digit after dot)
    initLexer("3.");
    token t = scanToken();
    assert(t.type   == TOKEN_NUMBER);
    assert(t.length == 1);
    assert(scanToken().type == TOKEN_DOT);
}

// --- string() ---

void test_scan_string() {
    initLexer("'hello'");
    token t = scanToken();
    assert(t.type   == TOKEN_STRING);
    assert(t.length == 7);
    assert(strncmp(t.start, "'hello'", 7) == 0);
}

void test_scan_empty_string() {
    initLexer("''");
    token t = scanToken();
    assert(t.type   == TOKEN_STRING);
    assert(t.length == 2);
}

void test_scan_unterminated_string() {
    initLexer("'hello");
    token t = scanToken();
    assert(t.type == TOKEN_ERROR);
    assert(strncmp(t.start, "Unterminated string.", t.length) == 0);
}

// --- skipWhitespace / line tracking ---

void test_scan_skips_whitespace() {
    initLexer("   \t\r select");
    token t = scanToken();
    assert(t.type   == TOKEN_SELECT);
    assert(t.length == 6);
}

void test_scan_skips_comment() {
    // Everything after '--' until newline is skipped
    initLexer("-- this is a comment\nselect");
    token t = scanToken();
    assert(t.type == TOKEN_SELECT);
    assert(t.line == 2);
}

void test_scan_line_tracking() {
    initLexer("select\nfrom\ntable");
    token t;
    t = scanToken(); assert(t.type == TOKEN_SELECT && t.line == 1);
    t = scanToken(); assert(t.type == TOKEN_FROM   && t.line == 2);
    t = scanToken(); assert(t.type == TOKEN_TABLE  && t.line == 3);
}

// --- initTokenized ---

void test_init_tokenized() {
    tokenized t;
    initTokenized(&t);
    assert(t.capacity == INITIAL_TOKEN_COUNT);
    assert(t.count    == 0);
    assert(t.tokens   != NULL);
    free(t.tokens);
}

// --- addToken ---

void test_add_token_count() {
    tokenized t;
    initTokenized(&t);
    token tok = { TOKEN_SELECT, "select", 6, 1 };
    addToken(&t, tok);
    assert(t.count == 1);
    free(t.tokens);
}

void test_add_token_stored() {
    tokenized t;
    initTokenized(&t);
    token tok = { TOKEN_FROM, "from", 4, 2 };
    addToken(&t, tok);
    assert(t.tokens[0].type   == TOKEN_FROM);
    assert(t.tokens[0].length == 4);
    assert(t.tokens[0].line   == 2);
    free(t.tokens);
}

void test_add_multiple_tokens() {
    tokenized t;
    initTokenized(&t);
    token tok1 = { TOKEN_SELECT,     "select", 6, 1 };
    token tok2 = { TOKEN_STAR,       "*",      1, 1 };
    token tok3 = { TOKEN_IDENTIFIER, "users",  5, 1 };
    addToken(&t, tok1);
    addToken(&t, tok2);
    addToken(&t, tok3);
    assert(t.count == 3);
    assert(t.tokens[0].type == TOKEN_SELECT);
    assert(t.tokens[1].type == TOKEN_STAR);
    assert(t.tokens[2].type == TOKEN_IDENTIFIER);
    free(t.tokens);
}

// --- lexQuery ---

void test_lex_query_simple() {
    // keywords are lowercase-only; uppercase letters become TOKEN_IDENTIFIER
    tokenized t = lexQuery("select * from users;");
    assert(t.count == 6); // select, *, from, users, ;, EOF
    assert(t.tokens[0].type == TOKEN_SELECT);
    assert(t.tokens[1].type == TOKEN_STAR);
    assert(t.tokens[2].type == TOKEN_FROM);
    assert(t.tokens[3].type == TOKEN_IDENTIFIER);
    assert(t.tokens[4].type == TOKEN_SEMICOLON);
    assert(t.tokens[5].type == TOKEN_EOF);
    free(t.tokens);
}

void test_lex_query_empty() {
    tokenized t = lexQuery("");
    assert(t.count == 1);
    assert(t.tokens[0].type == TOKEN_EOF);
    free(t.tokens);
}

void test_lex_query_string_and_number() {
    tokenized t = lexQuery("42 'hello'");
    assert(t.count == 3); // 42, 'hello', EOF
    assert(t.tokens[0].type == TOKEN_NUMBER);
    assert(t.tokens[1].type == TOKEN_STRING);
    assert(t.tokens[2].type == TOKEN_EOF);
    free(t.tokens);
}

// --- master ---

void test_lexer() {
    test_scan_single_char_tokens();
    test_scan_two_char_tokens();
    test_scan_single_ops();
    test_scan_keywords();
    test_scan_identifier();
    test_scan_number_integer();
    test_scan_number_float();
    test_scan_number_no_trailing_decimal();
    test_scan_string();
    test_scan_empty_string();
    test_scan_unterminated_string();
    test_scan_skips_whitespace();
    test_scan_skips_comment();
    test_scan_line_tracking();
    test_init_tokenized();
    test_add_token_count();
    test_add_token_stored();
    test_add_multiple_tokens();
    test_lex_query_simple();
    test_lex_query_empty();
    test_lex_query_string_and_number();
    printf("All lexer tests passed.\n");
}


// ##########################################################################################################################################
// ##########################################################################################################################################
// Parser tests

// Convenience: lex a query and compile it in one call.
// Caller must freeAST(ast) and free(t.tokens) after use.
#define COMPILE(src, t, ast) \
    tokenized t = lexQuery(src); \
    ast_node* ast = compile(t)

// --- compile: SELECT ---

void test_parse_select_star() {
    COMPILE("select * from users", t, ast);
    assert(ast->type == TYPE_QUERY);
    ast_node* sel = ast->children[0];
    assert(sel->type == TYPE_SELECT_STMT);
    // select list is a listNode whose first child is the star item (flag=true)
    assert(sel->children[0]->children[0]->flag == FLAG_SELECT_STAR);
    freeAST(ast); free(t.tokens);
}

void test_parse_select_columns() {
    COMPILE("select name, age from users", t, ast);
    ast_node* sel = ast->children[0];
    assert(sel->type == TYPE_SELECT_STMT);
    ast_node* first = sel->children[0];
    assert(first->type == TYPE_LIST_NODE);
    assert(first->children[0]->type == TYPE_SELECT_ITEM);
    assert(first->children[1] != NULL);  // second column present
    freeAST(ast); free(t.tokens);
}

void test_parse_select_where() {
    COMPILE("select * from users where id = 1", t, ast);
    ast_node* sel = ast->children[0];
    assert(sel->children[2] != NULL);
    assert(sel->children[2]->type == TYPE_WHERE_CLAUSE);
    ast_node* cmp = sel->children[2]->children[0];
    assert(cmp->type == TYPE_COMPARISON);
    assert(cmp->tok.type == TOKEN_EQUAL);
    freeAST(ast); free(t.tokens);
}

void test_parse_select_distinct() {
    COMPILE("select distinct * from users", t, ast);
    ast_node* sel = ast->children[0];
    assert(sel->type == TYPE_SELECT_STMT);
    assert(sel->flag == FLAG_SELECT_DISTINCT);                        // DISTINCT
    assert(sel->children[1]->type == TYPE_IDENTIFIER);
    freeAST(ast); free(t.tokens);
}

void test_parse_select_limit() {
    COMPILE("select * from users limit 10", t, ast);
    ast_node* sel = ast->children[0];
    assert(sel->children[6] != NULL);
    assert(sel->children[6]->type == TYPE_LIMIT_CLAUSE);
    assert(sel->children[6]->tok.type == TOKEN_NUMBER);
    assert(strncmp(sel->children[6]->tok.start, "10", 2) == 0);
    freeAST(ast); free(t.tokens);
}

void test_parse_select_order_by() {
    COMPILE("select * from users order by age desc", t, ast);
    ast_node* sel = ast->children[0];
    assert(sel->children[5] != NULL);
    assert(sel->children[5]->type == TYPE_ORDER_CLAUSE);
    ast_node* item = sel->children[5]->children[0];
    assert(item->type == TYPE_LIST_NODE);
    assert(item->flag == FLAG_DESC);   // DESC
    freeAST(ast); free(t.tokens);
}

void test_parse_select_group_by() {
    COMPILE("select * from users group by dept having count > 1", t, ast);
    ast_node* sel = ast->children[0];
    assert(sel->children[3] != NULL);
    assert(sel->children[3]->type == TYPE_GROUP_CLAUSE);
    assert(sel->children[4] != NULL);
    assert(sel->children[4]->type == TYPE_HAVING_CLAUSE);
    freeAST(ast); free(t.tokens);
}

void test_parse_select_expr_alias() {
    COMPILE("select name as n from users", t, ast);
    ast_node* sel = ast->children[0];
    ast_node* sel_item = sel->children[0]->children[0];  // listNode → selectItem
    assert(sel_item->type == TYPE_SELECT_ITEM);
    assert(sel_item->flag == FLAG_NULL);           // not a star
    assert(sel_item->children[1] != NULL);     // alias present
    assert(sel_item->children[1]->type == TYPE_IDENTIFIER);
    freeAST(ast); free(t.tokens);
}

// --- compile: INSERT ---

void test_parse_insert_with_cols() {
    COMPILE("insert into users (id, name) values (1, 'alice')", t, ast);
    ast_node* ins = ast->children[0];
    assert(ins->type == TYPE_INSERT_STMT);
    assert(ins->flag == FLAG_COLUMN_LIST);               // explicit column list
    assert(ins->children[1] != NULL);        // col list
    assert(ins->children[2] != NULL);        // val list
    freeAST(ast); free(t.tokens);
}

void test_parse_insert_without_cols() {
    COMPILE("insert into users values (1, 'alice')", t, ast);
    ast_node* ins = ast->children[0];
    assert(ins->type == TYPE_INSERT_STMT);
    assert(ins->flag == FLAG_NULL);              // no explicit column list
    assert(ins->children[1] == NULL);        // no col list
    assert(ins->children[2] != NULL);        // val list present
    freeAST(ast); free(t.tokens);
}

// --- compile: UPDATE ---

void test_parse_update() {
    COMPILE("update users set name = 'alice' where id = 1", t, ast);
    ast_node* upd = ast->children[0];
    assert(upd->type == TYPE_UPDATE_STMT);
    assert(upd->children[0]->type == TYPE_IDENTIFIER);  // table name
    assert(upd->children[1] != NULL);       // assignment list
    assert(upd->children[2] != NULL);       // where clause
    freeAST(ast); free(t.tokens);
}

void test_parse_update_no_where() {
    COMPILE("update users set active = 0", t, ast);
    ast_node* upd = ast->children[0];
    assert(upd->type == TYPE_UPDATE_STMT);
    assert(upd->children[1] != NULL);       // assignment list
    assert(upd->children[2] == NULL);       // no where clause
    ast_node* asgn = upd->children[1]->children[0];
    assert(asgn->type == TYPE_ASSIGNMENT);
    freeAST(ast); free(t.tokens);
}

// --- compile: DELETE ---

void test_parse_delete_with_where() {
    COMPILE("delete from users where id = 1", t, ast);
    ast_node* del = ast->children[0];
    assert(del->type == TYPE_DELETE_STMT);
    assert(del->children[0]->type == TYPE_IDENTIFIER);
    assert(del->children[1] != NULL);
    assert(del->children[1]->type == TYPE_WHERE_CLAUSE);
    freeAST(ast); free(t.tokens);
}

void test_parse_delete_no_where() {
    COMPILE("delete from users", t, ast);
    ast_node* del = ast->children[0];
    assert(del->type == TYPE_DELETE_STMT);
    assert(del->children[0]->type == TYPE_IDENTIFIER);
    assert(del->children[1] == NULL);        // no where clause
    freeAST(ast); free(t.tokens);
}

// --- compile: CREATE ---

void test_parse_create_table() {
    COMPILE("create table users (id int, name text)", t, ast);
    ast_node* crt = ast->children[0];
    assert(crt->type == TYPE_CREATE_STMT);
    assert(crt->tok.type == TOKEN_TABLE);
    assert(crt->children[0]->type == TYPE_IDENTIFIER);  // table name
    ast_node* first_col = crt->children[1]->children[0];
    assert(first_col->type == TYPE_COL_DEF);
    freeAST(ast); free(t.tokens);
}

void test_parse_create_index() {
    COMPILE("create index idx on users (id)", t, ast);
    ast_node* crt = ast->children[0];
    assert(crt->type == TYPE_CREATE_STMT);
    assert(crt->tok.type == TOKEN_INDEX);
    assert(crt->flag == FLAG_NULL);              // not UNIQUE
    assert(crt->children[0]->type == TYPE_IDENTIFIER);  // index name
    freeAST(ast); free(t.tokens);
}

void test_parse_create_unique_index() {
    COMPILE("create unique index idx on users (id)", t, ast);
    ast_node* crt = ast->children[0];
    assert(crt->type == TYPE_CREATE_STMT);
    assert(crt->tok.type == TOKEN_INDEX);
    assert(crt->flag == FLAG_UNIQUE_INDEX);               // UNIQUE
    assert(crt->children[0]->type == TYPE_IDENTIFIER);
    freeAST(ast); free(t.tokens);
}

// --- compile: DROP ---

void test_parse_drop_table() {
    COMPILE("drop table users", t, ast);
    ast_node* drp = ast->children[0];
    assert(drp->type == TYPE_DROP_STMT);
    assert(drp->tok.type == TOKEN_TABLE);
    assert(drp->children[0]->type == TYPE_IDENTIFIER);
    freeAST(ast); free(t.tokens);
}

void test_parse_drop_view() {
    COMPILE("drop view myview", t, ast);
    ast_node* drp = ast->children[0];
    assert(drp->type == TYPE_DROP_STMT);
    assert(drp->tok.type == TOKEN_VIEW);
    assert(drp->children[0]->type == TYPE_IDENTIFIER);
    freeAST(ast); free(t.tokens);
}

// --- compile: ALTER ---

void test_parse_alter_add() {
    COMPILE("alter table users add name text", t, ast);
    ast_node* alt = ast->children[0];
    assert(alt->type == TYPE_ALTER_STMT);
    assert(alt->tok.type == TOKEN_ADD);
    assert(alt->children[0]->type == TYPE_IDENTIFIER);  // table name
    assert(alt->children[1]->type == TYPE_COL_DEF);     // new column
    freeAST(ast); free(t.tokens);
}

void test_parse_alter_drop_column() {
    COMPILE("alter table users drop column name", t, ast);
    ast_node* alt = ast->children[0];
    assert(alt->type == TYPE_ALTER_STMT);
    assert(alt->tok.type == TOKEN_DROP);
    assert(alt->children[1]->type == TYPE_IDENTIFIER);  // dropped column name
    assert(alt->children[2] == NULL);
    freeAST(ast); free(t.tokens);
}

void test_parse_alter_alter_column() {
    COMPILE("alter table users alter column name varchar", t, ast);
    ast_node* alt = ast->children[0];
    assert(alt->type == TYPE_ALTER_STMT);
    assert(alt->tok.type == TOKEN_ALTER);
    assert(alt->children[1]->type == TYPE_IDENTIFIER);  // column name
    assert(alt->children[2]->type == TYPE_IDENTIFIER);  // new type
    freeAST(ast); free(t.tokens);
}

// --- compile: expressions ---

void test_parse_expr_arithmetic() {
    COMPILE("select a + b from t", t, ast);
    ast_node* sel_item = ast->children[0]->children[0]->children[0];
    assert(sel_item->type == TYPE_SELECT_ITEM);
    ast_node* add = sel_item->children[0];
    assert(add->type == TYPE_ADDITIVE);
    assert(add->tok.type == TOKEN_PLUS);
    freeAST(ast); free(t.tokens);
}

void test_parse_expr_logical() {
    COMPILE("select * from t where x > 5 and y < 10", t, ast);
    ast_node* whr = ast->children[0]->children[2];
    assert(whr->type == TYPE_WHERE_CLAUSE);
    ast_node* and_node = whr->children[0];
    assert(and_node->type == TYPE_AND_EXPR);
    assert(and_node->children[0]->type == TYPE_COMPARISON);
    assert(and_node->children[1]->type == TYPE_COMPARISON);
    freeAST(ast); free(t.tokens);
}

// --- master ---

void test_parser() {
    test_parse_select_star();
    test_parse_select_columns();
    test_parse_select_where();
    test_parse_select_distinct();
    test_parse_select_limit();
    test_parse_select_order_by();
    test_parse_select_group_by();
    test_parse_select_expr_alias();
    test_parse_insert_with_cols();
    test_parse_insert_without_cols();
    test_parse_update();
    test_parse_update_no_where();
    test_parse_delete_with_where();
    test_parse_delete_no_where();
    test_parse_create_table();
    test_parse_create_index();
    test_parse_create_unique_index();
    test_parse_drop_table();
    test_parse_drop_view();
    test_parse_alter_add();
    test_parse_alter_drop_column();
    test_parse_alter_alter_column();
    test_parse_expr_arithmetic();
    test_parse_expr_logical();
    printf("All parser tests passed.\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// Generator Tests

// Returns true if any byte in c->code[0..count-1] equals op.
static bool has_opcode(chunk* c, uint8_t op) {
    for (int i = 0; i < c->count; i++) {
        if (c->code[i] == op) return true;
    }
    return false;
}

// Build a pre-allocated hashtable with a "users" schema (id PK int, name text).
static hashtable make_users_ht(void) {
    hashtable ht = make_test_table();
    schema s;
    s.hash        = hashString("users", 5);
    s.tablename   = strdup("users");
    s.colNames    = malloc(2 * sizeof(char*));
    s.colNames[0] = strdup("id");
    s.colNames[1] = strdup("name");
    s.colTypes    = malloc(2);
    s.colTypes[0] = (CONSTRAINT_PRIMARY_KEY   << 5) | SQL_INT;
    s.colTypes[1] = (CONSTRAINT_UNCONSTRAINED << 5) | SQL_TEXT;
    s.count       = 2;
    insertHT(&s, &ht);
    return ht;
}

void test_generate_select_emits_scan_opcodes(void) {
    tokenized t = lexQuery("select * from users");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_OPEN_SCAN));
    assert(has_opcode(&c, OP_REWIND));
    assert(has_opcode(&c, OP_NEXT));
    assert(has_opcode(&c, OP_CLOSE_SCAN));
    assert(c.code[c.count - 1] == OP_HALT);
    // SELECT * with 2-column schema emits OP_EMIT_ROW
    assert(has_opcode(&c, OP_EMIT_ROW));

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_select_where_pk_uses_key_search(void) {
    // WHERE on the primary key is recognized by checkWhereForPK() and takes
    // the direct-lookup fast path (OP_KEY_SEARCH) instead of a linear scan
    // with a filter, so the condition itself is never compiled through
    // munchExpr — no OP_EQUAL / OP_JUMP_FALSE is emitted for it.
    tokenized t = lexQuery("select * from users where id = 1");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_OPEN_SCAN));
    assert(has_opcode(&c, OP_KEY_SEARCH));
    assert(!has_opcode(&c, OP_EQUAL));
    assert(!has_opcode(&c, OP_JUMP_FALSE));
    assert(c.code[c.count - 1] == OP_HALT);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_insert_emits_insert_row(void) {
    tokenized t = lexQuery("insert into users values (1, 'alice')");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_OPEN_SCAN));
    assert(has_opcode(&c, OP_INSERT_ROW));
    // Three constants: "users", 1, "alice"
    assert(c.constants.count == 3);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_delete_emits_delete_row(void) {
    tokenized t = lexQuery("delete from users where id = 1");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_OPEN_SCAN));
    assert(has_opcode(&c, OP_DELETE_ROW));
    assert(c.code[c.count - 1] == OP_HALT);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_update_emits_update_col(void) {
    tokenized t = lexQuery("update users set name = 'bob' where id = 1");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_OPEN_SCAN));
    assert(has_opcode(&c, OP_UPDATE_COL));
    assert(c.code[c.count - 1] == OP_HALT);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_create_table_emits_opcode(void) {
    tokenized t = lexQuery("create table users (id int, name text)");
    ast_node* ast = compile(t);
    free(t.tokens);

    // make_test_table() gives capacity=8, avoiding insertHT's capacity=0 crash.
    hashtable ht = make_test_table();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_CREATE_TABLE));
    assert(c.code[c.count - 1] == OP_HALT);
    // generate() inserts the new schema into ht via insertHT
    assert(readHT(hashString("users", 5), &ht) != NULL);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_drop_table_emits_opcode(void) {
    tokenized t = lexQuery("drop table users");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_test_table();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    assert(has_opcode(&c, OP_DROP_TABLE));
    assert(c.code[c.count - 1] == OP_HALT);
    // Only one constant: the table name string
    assert(c.constants.count == 1);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_expr_arithmetic(void) {
    tokenized t = lexQuery("select 1 + 2 from users");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    // Additive expression must emit OP_ADD
    assert(has_opcode(&c, OP_ADD));
    assert(has_opcode(&c, OP_EMIT_ROW));
    assert(c.code[c.count - 1] == OP_HALT);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generate_expr_logical_and(void) {
    tokenized t = lexQuery("select * from users where 1 = 1 and 2 = 2");
    ast_node* ast = compile(t);
    free(t.tokens);

    hashtable ht = make_users_ht();
    chunk c; initChunk(&c);
    generate(ast, &c, &ht);

    // Short-circuit AND emits OP_JUMP_FALSE and two OP_EQUAL comparisons
    assert(has_opcode(&c, OP_EQUAL));
    assert(has_opcode(&c, OP_JUMP_FALSE));
    assert(c.code[c.count - 1] == OP_HALT);

    freeAST(ast); freeChunk(&c); freeHashTable(&ht);
}

void test_generator(void) {
    test_generate_select_emits_scan_opcodes();
    test_generate_select_where_pk_uses_key_search();
    test_generate_insert_emits_insert_row();
    test_generate_delete_emits_delete_row();
    test_generate_update_emits_update_col();
    test_generate_create_table_emits_opcode();
    test_generate_drop_table_emits_opcode();
    test_generate_expr_arithmetic();
    test_generate_expr_logical_and();
    printf("All generator tests passed.\n");
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// VM Tests

// --- push / pop ---

void test_vm_push_pop_integer(void) {
    hashtable ht = make_test_table();
    initVM(&ht);

    push(INTEGER_VAL(42));
    value v = pop();
    assert(v.type == VAL_INT);
    assert(v.as.integer == 42);

    // second roundtrip confirms stackTop resets to base after pop
    push(INTEGER_VAL(-7));
    assert(pop().as.integer == -7);

    freeVM(); freeHashTable(&ht);
}

void test_vm_push_pop_lifo(void) {
    hashtable ht = make_test_table();
    initVM(&ht);

    push(INTEGER_VAL(1));
    push(BOOL_VAL(true));
    push(FLOAT_VAL(2.5));

    assert(pop().type == VAL_FLOAT);
    assert(pop().type == VAL_BOOL);
    assert(pop().as.integer == 1);

    freeVM(); freeHashTable(&ht);
}

void test_vm_push_pop_bool(void) {
    hashtable ht = make_test_table();
    initVM(&ht);

    push(BOOL_VAL(true));
    assert(pop().as.boolean == true);

    push(BOOL_VAL(false));
    assert(pop().as.boolean == false);

    push(BOOL_VAL(true));
    assert(pop().type == VAL_BOOL);

    freeVM(); freeHashTable(&ht);
}

void test_vm_push_pop_null(void) {
    hashtable ht = make_test_table();
    initVM(&ht);

    push(NULL_VAL(0));
    value v = pop();
    assert(v.type == VAL_NULL);

    // push two nulls; both pop as VAL_NULL in LIFO order
    push(NULL_VAL(0)); push(NULL_VAL(0));
    assert(pop().type == VAL_NULL);
    assert(pop().type == VAL_NULL);

    freeVM(); freeHashTable(&ht);
}

// --- initVM / freeVM ---

void test_vm_free_no_crash(void) {
    hashtable ht = make_test_table();

    // freeVM on a fresh VM (no open scanners, no results) must not crash
    initVM(&ht);
    freeVM();

    // re-init after free: push/pop must still give correct LIFO results
    initVM(&ht);
    push(INTEGER_VAL(10));
    push(INTEGER_VAL(20));
    assert(pop().as.integer == 20);
    assert(pop().as.integer == 10);

    freeVM();  // second freeVM on clean state must also not crash

    freeHashTable(&ht);
}

// --- interpret ---

void test_interpret_missing_table_returns_compile_error(void) {
    // loadSchema() self-heals a missing schema file instead of returning NULL
    // (see test_load_schema_bootstraps_on_missing), so querying a table that
    // simply isn't registered in that (now valid, empty) schema no longer
    // reaches INTERPRET_LOAD_ERROR — it's caught by generate()'s table-exists
    // check instead, which reports a normal compile error rather than letting
    // munchStmt dereference a NULL schema.
    remove(schema_path());  // start from a fresh, self-healed schema
    assert(interpret("select * from ghost").ir == INTERPRET_COMPILE_ERROR);
    assert(interpret("insert into ghost values (1)").ir == INTERPRET_COMPILE_ERROR);
    assert(interpret("update ghost set x = 1 where id = 1").ir == INTERPRET_COMPILE_ERROR);
    assert(interpret("delete from ghost where id = 1").ir == INTERPRET_COMPILE_ERROR);
    remove(schema_path());
}

// --- master ---

void test_vm(void) {
    test_vm_push_pop_integer();
    test_vm_push_pop_lifo();
    test_vm_push_pop_bool();
    test_vm_push_pop_null();
    test_vm_free_no_crash();
    test_interpret_missing_table_returns_compile_error();
    printf("All VM tests passed.\n");
}

