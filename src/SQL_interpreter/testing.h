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

#ifndef TESTING_INTERPRETER_H
#define TESTING_INTERPRETER_H

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "chunk.h"
#include "schema.h"
#include "lexer.h"
#include "parser.h"
#include "generator.h"
#include "vm.h"

/* Chunk tests */
void test_chunk();
void test_init_chunk();
void test_write_chunk_stores_byte();
void test_write_chunk_stores_line();
void test_write_chunk_multiple();
void test_write_chunk_triggers_grow();
void test_add_constant_returns_index();
void test_add_constant_stores_value();
void test_add_constant_multiple();
void test_free_chunk_resets_code();
void test_free_chunk_clears_constants();
void test_free_chunk_idempotent();

/* Schema tests */
void test_schema();
void test_load_schema_null_on_missing();
void test_load_schema_null_on_bad_magic();
void test_save_schema_no_crash();
void test_save_schema_writes_magic();
void test_save_load_empty_schema();

/* Hash table tests */
void test_hashtable();
void test_init_hash_table();
void test_free_hash_table_resets();
void test_free_hash_table_idempotent();
void test_hash_string_deterministic();
void test_hash_string_different_keys();
void test_hash_string_length_matters();
void test_insert_ht_stores_entry();
void test_insert_ht_increments_count();
void test_insert_ht_overwrites();
void test_read_ht_finds_existing();
void test_read_ht_missing();
void test_read_ht_correct_data();
void test_delete_ht_removes_entry();
void test_delete_ht_zeroes_fields();
void test_delete_ht_nonexistent();

/* Value tests */
void test_value();
void test_init_value_array();
void test_write_value_array_stores_value();
void test_write_value_array_multiple_types();
void test_write_value_array_triggers_grow();
void test_free_value_array_resets();
void test_free_value_array_idempotent();
void test_print_value_float();
void test_print_value_zero();
void test_print_value_large();

/* Lexer tests */
void test_lexer();
void test_scan_single_char_tokens();
void test_scan_two_char_tokens();
void test_scan_single_ops();
void test_scan_keywords();
void test_scan_identifier();
void test_scan_number_integer();
void test_scan_number_float();
void test_scan_number_no_trailing_decimal();
void test_scan_string();
void test_scan_empty_string();
void test_scan_unterminated_string();
void test_scan_skips_whitespace();
void test_scan_skips_comment();
void test_scan_line_tracking();
void test_init_tokenized();
void test_add_token_count();
void test_add_token_stored();
void test_add_multiple_tokens();
void test_lex_query_simple();
void test_lex_query_empty();
void test_lex_query_string_and_number();

/* VM tests */
void test_vm();
void test_vm_push_pop_integer();
void test_vm_push_pop_lifo();
void test_vm_push_pop_bool();
void test_vm_push_pop_null();
void test_vm_free_no_crash();
void test_interpret_no_schema_returns_load_error();

/* Generator tests */
void test_generator();
void test_generate_select_emits_scan_opcodes();
void test_generate_select_where_emits_filter();
void test_generate_insert_emits_insert_row();
void test_generate_delete_emits_delete_row();
void test_generate_update_emits_update_col();
void test_generate_create_table_emits_opcode();
void test_generate_drop_table_emits_opcode();
void test_generate_expr_arithmetic();
void test_generate_expr_logical_and();

/* Parser tests */
void test_parser();
void test_parse_select_star();
void test_parse_select_columns();
void test_parse_select_where();
void test_parse_select_distinct();
void test_parse_select_limit();
void test_parse_select_order_by();
void test_parse_select_group_by();
void test_parse_select_expr_alias();
void test_parse_insert_with_cols();
void test_parse_insert_without_cols();
void test_parse_update();
void test_parse_update_no_where();
void test_parse_delete_with_where();
void test_parse_delete_no_where();
void test_parse_create_table();
void test_parse_create_index();
void test_parse_create_unique_index();
void test_parse_drop_table();
void test_parse_drop_view();
void test_parse_alter_add();
void test_parse_alter_drop_column();
void test_parse_alter_alter_column();
void test_parse_expr_arithmetic();
void test_parse_expr_logical();

#endif