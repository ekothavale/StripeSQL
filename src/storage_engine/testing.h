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

#ifndef TESTING_STORAGE_H
#define TESTING_STORAGE_H

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bplus.h"
#include "page.h"

/* B+ tree integration tests */
void test_btree();
void test_btree_find_initial(void);
void test_btree_find_nonexistent(void);
void test_btree_record_add(void);
void test_btree_record_update(void);
void test_btree_record_delete(void);
void test_btree_commit_drains_stacks(void);
void test_btree_commit_persist(void);
void test_btree_commit_delete_persist(void);
void test_btree_insert_new_page(void);
void test_btree_insert_existing_page(void);
void test_btree_split_structure(void);
void test_btree_split_find_all(void);
void test_btree_split_linked_list(void);
void test_btree_split_non_last_child(void);
void test_btree_delete_page(void);
void test_btree_delete_triggers_borrow(void);
void test_btree_delete_triggers_merge(void);
void test_btree_full_roundtrip(void);
void test_btree_delete_tree(void);
void test_btree_delete_tree_not_reloadable(void);

/* Slotted-page CRUD tests */
void test_page_add();
void test_page_read();
void test_page_delete();
void test_page_update();
void test_page_multiple_records();
void test_page();   /* runs all of the above */

/* TableIO tests */
void test_tableio(); /* runs all tableIO tests */

/* Table and tree management tests */
void test_table_mgmt(); /* runs all table/tree management tests */

#endif