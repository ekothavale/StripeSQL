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

#include <stdarg.h>
#include <sys/stat.h>
#include "../common.h"
#include "../debug.h"
#include "parser.h"
#include "generator.h"
#include "schema.h"
#include "vm.h"

VM vm;
static hashtable* schemaCache = NULL;

/*
Session-scoped transaction state. Deliberately kept outside the VM struct:
initVM()/freeVM() run once per statement (every interpret() call), but a
transaction spans multiple statements/interpret() calls, so its state must
survive across them. Tables touched during an active transaction stay open
here (dirty writes accumulating in their dirty hashmaps, see tableIO.c)
instead of being committed and closed at the end of each statement; COMMIT
or DISCARD is what finally closes them out.
*/
#define MAX_TXN_TABLES MAX_SCANNERS

typedef struct txn_table_entry {
	uint32_t tableHash;
	table* tbl;
} txn_table_entry;

static struct {
	bool active;
	int count;
	txn_table_entry tables[MAX_TXN_TABLES];
} transaction = {0};

/*
returns the transaction's already-open handle for a table, or NULL if no
transaction is active or it hasn't touched this table yet
*/
static table* findTxnTable(uint32_t tableHash) {
	if (!transaction.active) return NULL;
	for (int i = 0; i < transaction.count; i++) {
		if (transaction.tables[i].tableHash == tableHash) return transaction.tables[i].tbl;
	}
	return NULL;
}

static void statementError(const char* format, ...);

/*
registers a freshly opened table handle with the active transaction so later
statements in the same transaction reuse it instead of reloading from disk
*/
static bool registerTxnTable(uint32_t tableHash, table* t) {
	if (transaction.count >= MAX_TXN_TABLES) {
		statementError("Error: transaction has touched too many tables\n");
		return false;
	}
	transaction.tables[transaction.count].tableHash = tableHash;
	transaction.tables[transaction.count].tbl = t;
	transaction.count++;
	return true;
}

static void resetStack(){
	vm.stackTop = vm.stack;
}

static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  size_t instruction = vm.ip - vm.chunk->code - 1;
  int line = vm.chunk->lines[instruction];
  fprintf(stderr, "[line %d] in script\n", line);
  resetStack();
  vm.failed = true;
}

/*
reports an error that halts the current statement: run() stops before its next instruction, and
interpret() rolls back whatever the statement changed
*/
static void statementError(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	vm.failed = true;
}

void initVM(hashtable* schema) {
	resetStack();
	for (int i = 0; i < MAX_SCANNERS; i++) {
		vm.scanners[i].open = false;
		vm.scanners[i].tbl = NULL;
	}
	vm.results.rows     = NULL;
	vm.results.count    = 0;
	vm.results.capacity = 0;
	vm.results.cols     = 0;
	vm.schema           = schema;
	vm.results.print    = false;
	vm.failed           = false;
}

void freeVM() {
	for (int i = 0; i < MAX_SCANNERS; i++) {
		if (vm.scanners[i].open) {
			commit(vm.scanners[i].tbl);
			fclose(vm.scanners[i].tbl->source);
			freeTable(vm.scanners[i].tbl);
			vm.scanners[i].tbl = NULL;
			vm.scanners[i].open = false;
		}
	}
	if (vm.results.rows) {
		for (int i = 0; i < vm.results.count; i++) free(vm.results.rows[i]);
		free(vm.results.rows);
		vm.results.rows  = NULL;
		vm.results.count = 0;
	}
}

/*
reset the schema cache
*/
static void invalidateSchema() {
	if (!schemaCache) return;
	freeHashTable(schemaCache);
	free(schemaCache);
	schemaCache = NULL;
}

static bool tableAlreadyExists(const char* tablename) {
	char* dir = TABLE_DIRECTORY;
	char* ext = TABLE_EXTENSION;
	int pathLen = strlen(dir) + strlen(tablename) + strlen(ext);
	char* path = malloc(pathLen + 1);
	snprintf(path, pathLen + 1, "%s%s%s", dir, tablename, ext);
	struct stat st;
	bool exists = stat(path, &st) == 0;
	free(path);
	return exists;
}

/*
maps a SQL column type to the storage engine's key-ordering type, mirroring
the type mapping pkToOk() applies to real values (ordering.c)
*/
static ordering_type sqlTypeToOrdering(SQL_type t) {
	switch (t) {
		case SQL_TEXT: return ORDERING_STRING;
		case SQL_FLOAT:
		case SQL_DOUBLE: return ORDERING_DOUBLE;
		default: return ORDERING_ULONG; // SQL_INT and other numeric-ish types
	}
}

/*
finds the ordering type of a table's primary key column, so the placeholder
page created at CREATE TABLE time can be tagged with the correct type instead
of defaulting to ORDERING_ULONG (see createTree() below)
*/
static ordering_type getPkOrderingType(schema* s) {
	for (int i = 0; i < s->count; i++) {
		if (((s->colTypes[i] & 0b11100000) >> 5) == CONSTRAINT_PRIMARY_KEY) {
			return sqlTypeToOrdering((SQL_type)(s->colTypes[i] & 0b00011111));
		}
	}
	statementError("Error: table '%s' has no primary key column\n", s->tablename);
	return ORDERING_ULONG;
}

// could potentially be abstracted into a general print value type
/*
prints a primary key based on its type. prints illegal pk types as hex
*/
static void printPK(value pk) {
	switch (pk.type) {
		case VAL_FLOAT: printf("%lf", pk.as.floating); break;
		case VAL_INT: printf("%lli", pk.as.integer); break;
		case VAL_TEXT: printf("%s", pk.as.text); break;
		case VAL_U32: printf("%u", pk.as.u32); break;
		default: printf("%llx", pk.as.integer); break;
	}
}

static bool loadFirstValidPage(scanner* s) {
	table* t = s->tbl;
	while (true) {
		while (s->childIdx < s->leafNode.childCount) {
			s->pageAddr = s->leafNode.children[s->childIdx];
			readPage(s->pageAddr, &s->page, t);
			s->slotIdx = 0;
			if (s->page.header.numRecords > 0) return true;
			s->childIdx++;
		}
		if (s->leafNode.next == 0) {
			s->atEnd = true;
			return false;
		}
		s->leafAddr = s->leafNode.next;
		readNode(s->leafAddr, &s->leafNode, t);
		s->childIdx = 0;
	}
}

/*
opens a new scanner
*/
void openScanner(uint32_t tableNameHash, uint8_t pkIdx) {
	const char* tablename = readHT(tableNameHash, vm.schema)->tablename;
	if (vm.numScanners >= MAX_SCANNERS) {
		statementError("Error: no free scanner slots available\n");
		return;
	}
	table* t = findTxnTable(tableNameHash);
	if (!t) {
		t = malloc(sizeof(table));
		if (!loadTable((char*)tablename, t)) { // loadTable() reports why
			free(t);
			vm.failed = true;
			return;
		}
		if (transaction.active && !registerTxnTable(tableNameHash, t)) {
			fclose(t->source);
			freeTable(t);
			return;
		}
	}
	beginStatement(t); // so the table can be rolled back if this statement fails
	int idx = vm.numScanners++;
	vm.scanners[idx].tbl      = t;
	vm.scanners[idx].tblHash  = tableNameHash;
	vm.scanners[idx].open     = true;
	vm.scanners[idx].started  = false;
	vm.scanners[idx].atEnd    = false;
	vm.scanners[idx].leafNode = (node){0};
	vm.scanners[idx].leafAddr = 0;
	vm.scanners[idx].childIdx = 0;
	vm.scanners[idx].page     = (slotted_page){0};
	vm.scanners[idx].pageAddr = 0;
	vm.scanners[idx].slotIdx  = 0;
	vm.scanners[idx].pkIdx    = pkIdx;
}

/*
closes a scanner, committing its table unless a transaction owns it
returns false if that commit failed to reach disk
*/
bool closeScanner(scanner* s) {
	if (!s->open) return true;
	bool committed = true;
	endStatement(s->tbl); // the statement is done with this table, so its changes are kept
	// tables owned by an active transaction stay open until COMMIT/DISCARD
	if (!findTxnTable(s->tblHash)) {
		committed = commit(s->tbl);
		fclose(s->tbl->source);
		freeTable(s->tbl);
	}
	freeSPage(&s->page);
	s->page = (slotted_page){0};
	s->leafNode = (node){0};
	s->tbl     = NULL;
	s->tblHash = 0;
	s->open    = false;
	s->started = false;
	s->atEnd   = false;
	vm.numScanners--;
	return committed;
}

/*
Advances the scanner to the next row.
On the first call (started == false), walks from root to the first row.
Returns true if a valid row is now current, false if the table is exhausted.
*/
bool advanceScanner(scanner* s) {
	if (s->atEnd) return false;
	table* t = s->tbl;

	if (!s->started) {
		// walk down to leftmost leaf
		address nAddr = t->root;
		if (!readNode(nAddr, &s->leafNode, t)) {
			statementError("Error: scanner could not read root node at address %llu; tree may be corrupt\n", nAddr);
			s->atEnd = true;
			return false;
		}
		while (!s->leafNode.isLeaf) {
			nAddr = s->leafNode.children[0];
			if (!readNode(nAddr, &s->leafNode, t)) {
				statementError("Error: scanner could not read node at address %llu; tree may be corrupt\n", nAddr);
				s->atEnd = true;
				return false;
			}
		}
		s->leafAddr = nAddr;
		s->childIdx = 0;
		s->started  = true;
		return loadFirstValidPage(s);
	}

	// advance within current page
	s->slotIdx++;
	if (s->slotIdx < s->page.header.numRecords) return true;

	// move to next page
	s->childIdx++;
	return loadFirstValidPage(s);
}

/*
searches for and loads a record into the given scanner by primary key lookup
*/
bool scannerKeySearch(scanner* s, ordering_key key) {
	address addr = findPage(key.pageNum, s->tbl);
	if (!addr) return false;
	s->pageAddr = addr;
	readPage(addr, &s->page, s->tbl);
	int slotIdx = SPSearch(&s->page, key.offset);
	if (slotIdx < 0) return false;
	s->slotIdx = slotIdx;
	return true;
}

/*
determines if two values are equal
*/
static bool equal(value a, value b) {
	if (a.type != b.type) return false;
	switch (a.type) {
		case VAL_BOOL: return a.as.boolean == b.as.boolean;
		case VAL_FLOAT: return a.as.floating == b.as.floating;
		case VAL_INT: return a.as.integer == b.as.integer;
		case VAL_NULL: return true;
		case VAL_TEXT: return strcmp(a.as.text, b.as.text) ? 0 : 1;
		case VAL_U32: return a.as.u32 == b.as.u32;
		default: {
			runtimeError("Equality not supported for type %i", a.type);
			break;
		}
	}
}

static bool lessThan(value a, value b) {
	if (a.type != b.type) {
		runtimeError("Operands of less than comparison must have the same type");
		return false;
	}
	switch (a.type) {
		case VAL_BOOL: return a.as.boolean < b.as.boolean;
		case VAL_FLOAT: return a.as.floating < b.as.floating;
		case VAL_INT: return a.as.integer < b.as.integer;
		case VAL_NULL: return false;
		//case VAL_TEXT: return strcmp(a.as.text, b.as.text) ? 1 : 0;
		case VAL_U32: return a.as.u32 < b.as.u32;
		default: {
			runtimeError("Less than not supported for type %i", a.type);
			return false;
		}
	}
}

static bool greaterThan(value a, value b) {
	if (a.type != b.type) {
		runtimeError("Operands of greater than comparison must have the same type");
		return false;
	}
	switch (a.type) {
		case VAL_BOOL: return a.as.boolean > b.as.boolean;
		case VAL_FLOAT: return a.as.floating > b.as.floating;
		case VAL_INT: return a.as.integer > b.as.integer;
		case VAL_NULL: return false;
		//case VAL_TEXT: return strcmp(a.as.text, b.as.text) ? 1 : 0;
		case VAL_U32: return a.as.u32 > b.as.u32;
		default: {
			runtimeError("Greater than not supported for type %i", a.type);
			return false;
			break;
		}
	}
}

/*
Push value to VM's stack
*/
void push(value value) {
	if (vm.stackTop - vm.stack >= STACK_MAX) {
		runtimeError("Stack overflow");
		return;
	}
	*vm.stackTop = value;
	vm.stackTop++;
}

/*
Pop value from top of VM's stack
*/
value pop() {
	if (vm.stackTop <= vm.stack) {
		runtimeError("Stack underflow");
		return NULL_VAL(0);
	}
	vm.stackTop--;
	return *vm.stackTop;
}

/*
Converts a VM value to a heap-allocated storage engine entry.
Caller must eventually free e.data (or transfer ownership to a slotted_page).
VAL_FLOAT is stored as raw IEEE 754 bytes under T_STRING — no T_FLOAT type exists yet.
*/
static entry valueToEntry(value v) {
	entry e;
	switch (v.type) {
		case VAL_INT: {
			int32_t raw = (int32_t)v.as.integer;
			e.type = T_INT;
			e.size = sizeof(int32_t);
			e.data = malloc(e.size);
			memcpy(e.data, &raw, e.size);
			break;
		}
		case VAL_TEXT: {
			e.type = T_STRING;
			e.size = (uint32_t)strlen(v.as.text);
			e.data = malloc(e.size);
			memcpy(e.data, v.as.text, e.size);
			break;
		}
		case VAL_BOOL: {
			int32_t raw = v.as.boolean ? 1 : 0;
			e.type = T_INT;
			e.size = sizeof(int32_t);
			e.data = malloc(e.size);
			memcpy(e.data, &raw, e.size);
			break;
		}
		case VAL_FLOAT: {
			e.type = T_STRING;
			e.size = sizeof(double);
			e.data = malloc(e.size);
			memcpy(e.data, &v.as.floating, e.size);
			break;
		}
		default: { // VAL_NULL
			e.type = T_INT;
			e.size = 0;
			e.data = NULL;
			break;
		}
	}
	return e;
}

/*
SQL LIKE pattern matching: % matches any sequence of characters, _ matches exactly one.
Returns true if str matches pattern.
*/
static bool likeMatch(const char* str, const char* pattern) {
	if (*pattern == '\0') return *str == '\0';
	if (*pattern == '%') {
		do {
			if (likeMatch(str, pattern + 1)) return true;
		} while (*str++ != '\0');
		return false;
	}
	if (*str == '\0') return false;
	if (*pattern == '_' || *pattern == *str) return likeMatch(str + 1, pattern + 1);
	return false;
}

static interpret_result run() {
	#define READ_BYTE() (*vm.ip++)
	#define READ_TWO_BYTES() (((uint16_t) *vm.ip++ << 8) | (uint16_t) *vm.ip++)
	#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
	// somewhat dubious macro that takes operators as arguments
	#define BINARY_OP(op) \
		do { \
			value b = pop(); \
			value a = pop(); \
			if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) push(FLOAT_VAL(a.as.floating op b.as.floating)); \
			else if (a.type == VAL_FLOAT) push(FLOAT_VAL(a.as.floating op b.as.integer)); \
			else if (b.type == VAL_FLOAT) push(FLOAT_VAL(a.as.integer op b.as.floating)); \
			else push(INTEGER_VAL(a.as.integer op b.as.integer)); \
		} while (false)

	for (;;) {
		if (vm.failed) return INTERPRET_RUNTIME_ERROR; // an error halts the statement
		#ifdef DEBUG_TRACE_EXECUTION
			printf("        ");
			for (value* slot = vm.stack; slot < vm.stackTop; slot++) {
				printf("[ ");
				printValue(*slot);
				printf(" ]");
			}
			printf("\n");
			disassembleInstruction(vm.chunk, (int)(vm.ip - vm.chunk->code));
		#endif
		uint8_t instruction;
		switch (instruction = READ_BYTE()) {
			case OP_CONSTANT: {
				value constant = READ_CONSTANT();
				push(constant);
				break;
			}
			case OP_TRUE: {
				value v = BOOL_VAL(true);
				push(v);
				break;
			}
			case OP_FALSE: {
				value v = BOOL_VAL(false);
				push(v);
				break;
			}
			case OP_NULL: {
				value v = NULL_VAL();
				push(v);
				break;
			}
			case OP_POP: pop(); break;
			case OP_ADD: BINARY_OP(+); break;
			case OP_SUBTRACT: BINARY_OP(-); break;
			case OP_MULTIPLY: BINARY_OP(*); break;
			case OP_DIVIDE: {
				// integer division by zero is undefined behavior in C, so it's an error rather than whatever the CPU does
				value divisor = vm.stackTop[-1];
				if ((divisor.type == VAL_INT && divisor.as.integer == 0) ||
				    (divisor.type == VAL_FLOAT && divisor.as.floating == 0)) {
					runtimeError("Division by zero");
					break;
				}
				BINARY_OP(/);
				break;
			}
			case OP_NEGATE: {
				value v = pop();
				if (v.type != VAL_INT && v.type != VAL_FLOAT) {
					runtimeError("Operand must be a number");
				}
				if (v.type == VAL_INT) {
					push(INTEGER_VAL(-v.as.integer));
				} else if (v.type == VAL_FLOAT) {
					push(FLOAT_VAL(-v.as.floating));
				}
				break;
			}
			case OP_EQUAL: {
				push(BOOL_VAL(equal(pop(), pop())));
				break;
			}
			case OP_NOT_EQUAL: {
				push(BOOL_VAL(!equal(pop(), pop())));
				break;
			}
			case OP_LESS: {
				push(BOOL_VAL(lessThan(pop(), pop())));
				break;
			}
			case OP_LESS_EQUAL: {
				push(BOOL_VAL(!greaterThan(pop(), pop())));
				break;
			}
			case OP_GREATER: {
				push(BOOL_VAL(greaterThan(pop(), pop())));
				break;
			}
			case OP_GREATER_EQUAL: {
				push(BOOL_VAL(!lessThan(pop(), pop())));
				break;
			}
			case OP_LIKE: {
				value pattern = pop();
				value str = pop();
				if (str.type == VAL_NULL || pattern.type == VAL_NULL) {
					push(NULL_VAL(0));
				} else {
					push(BOOL_VAL(likeMatch(str.as.text, pattern.as.text)));
				}
				break;
			}
			case OP_IS_NULL: {
				push(BOOL_VAL(pop().type == VAL_NULL));
				break;
			}
			case OP_NOT_NULL: {
				push(BOOL_VAL(!(pop().type == VAL_NULL)));
				break;
			}
			case OP_NOT: {
				push(BOOL_VAL(!pop().as.boolean));
				break;
			}
			case OP_JUMP: {
				uint16_t offset = READ_TWO_BYTES();
				vm.ip += (int16_t)offset;
				break;
			}
			case OP_JUMP_FALSE: {
				uint16_t offset = READ_TWO_BYTES();
				value v = pop();
				if ((v.type == VAL_BOOL && !v.as.boolean) || v.type == VAL_NULL) {
					vm.ip += (int16_t)offset;
				}
				break;
			}
			case OP_JUMP_TRUE: {
				uint16_t offset = READ_TWO_BYTES();
				value v = pop();
				if (v.type == VAL_BOOL && v.as.boolean) {
					vm.ip += (int16_t)offset;
				}
				break;
			}
			case OP_OPEN_SCAN: {
				value v = pop();
				uint8_t pkIdx = READ_BYTE();
				openScanner(v.as.u32, pkIdx);
				break;
			}
			case OP_CLOSE_SCAN: {
				if (!closeScanner(&vm.scanners[vm.numScanners-1])) return INTERPRET_RUNTIME_ERROR;
				break;
			}
			// advance scanner to next record. if at the end of the tree, jump to target
			case OP_NEXT: {
				uint16_t offset = READ_TWO_BYTES();
				if (!advanceScanner(&vm.scanners[vm.numScanners-1])) {
					vm.ip += (int16_t)offset;
				}
				break;
			}
			// search for a record by primary key. if found, load it into scanner. if not found, jump to target
			case OP_KEY_SEARCH: {
				value pk = pop();
				ordering_key ik = pkToOk(pk);
				uint16_t offset = READ_TWO_BYTES();
				scanner* s = &vm.scanners[vm.numScanners-1];
				if (!scannerKeySearch(s, ik)) {
					vm.ip += (int16_t)offset;
				}
				break;
			}
			case OP_REWIND: {
				scanner* s = &vm.scanners[vm.numScanners-1];
				s->started  = false;
				s->atEnd    = false;
				s->childIdx = 0;
				s->slotIdx  = 0;
				break;
			}
			case OP_COLUMN: {
				uint8_t col_idx = READ_BYTE();
				scanner* s = &vm.scanners[vm.numScanners-1];
				sp_slot slot = s->page.slots[s->slotIdx];
				entry e = s->page.entries[slot.ptr + col_idx];
				value v;
				switch (e.type) {
					case T_INT: {
						int32_t raw;
						memcpy(&raw, e.data, sizeof(int32_t));
						v.type = VAL_INT;
						v.as.integer = (int64_t)raw;
						break;
					}
					case T_STRING:
					case T_DATE:
					case T_TIME: {
						char* str = malloc(e.size + 1);
						memcpy(str, e.data, e.size);
						str[e.size] = '\0';
						v.type = VAL_TEXT;
						v.as.text = str;
						break;
					}
				}
				push(v);
				break;
			}
			case OP_EMIT_ROW: {
				uint8_t count = READ_BYTE();
				// pop in reverse so row[0] is the leftmost column
				value* row = malloc(count * sizeof(value));
				for (int i = count - 1; i >= 0; i--) row[i] = pop();
				#ifdef DEBUG_TRACE_EXECUTION
				for (int i = 0; i < count; i++) {
					if (i > 0) printf(" | ");
					printValue(row[i]);
				}
				printf("\n");
				#endif
				// grow result buffer
				if (vm.results.capacity == 0) {
					vm.results.capacity = 8;
					vm.results.rows = malloc(vm.results.capacity * sizeof(value*));
					vm.results.cols = count;
				} else if (vm.results.count == vm.results.capacity) {
					vm.results.capacity *= 2;
					vm.results.rows = realloc(vm.results.rows, vm.results.capacity * sizeof(value*));
				}
				// transfer ownership of the row array (including any VAL_TEXT pointers) to vm.results
				vm.results.rows[vm.results.count++] = row;
				break;
			}
			case OP_INSERT_ROW: {
				// get context
				uint8_t count = READ_BYTE();
				// find scanner
				scanner* s = &vm.scanners[vm.numScanners-1];
				table* t = s->tbl;
				// stack top is the last column, so pop in reverse to preserve column order
				entry* entries = malloc(count * sizeof(entry));
				uint32_t totalSize = 0;
				value pk;
				for (int i = count - 1; i >= 0; i--) {
					value v = pop();
					entries[i] = valueToEntry(v);
					totalSize += entries[i].size;
					if (i == s->pkIdx) pk = v; // pkIdx is stored ahead of time in scanner since it's a loop invariant
				}
				ordering_key ik = pkToOk(pk);
				sp_record r = { .entries = entries, .len = count, .size = totalSize };
				int result = insertRecord(&r, ik, t);
				if (result == 1) {
					printf("Error: a row with primary key ");
					printPK(pk);
					printf(" already exists\n");
					vm.failed = true;
				} else if (result == 2) {
					printf("Error: failed to insert the row with primary key ");
					printPK(pk);
					printf(" (page full)\n");
					vm.failed = true;
				}
				// when insertRecord fails, the data is never freed
				if (result != 0) {
					for (int i = 0; i < count; i++) free(entries[i].data);
				}
				free(entries);  // page owns the data pointers; release only the metadata array
				break;
			}
			case OP_UPDATE_COL: {
				uint8_t col_idx = READ_BYTE();
				scanner* s = &vm.scanners[vm.numScanners-1];
				table* t = s->tbl;
				sp_slot slot = s->page.slots[s->slotIdx];
				entry* target = &s->page.entries[slot.ptr + col_idx];
				free(target->data);
				*target = valueToEntry(pop());
				markPage(s->pageAddr, &s->page, t);
				break;
			}
			case OP_DELETE_ROW: {
				scanner* s = &vm.scanners[vm.numScanners-1];
				table* t = s->tbl;
				ordering_key ik = { .pageNum = s->page.header.pageNum, .offset = s->page.slots[s->slotIdx].ID };
				freeSPage(&s->page);
				s->page = (slotted_page){0};
				if (!deleteRecord(ik, t, &s->page)) {
					statementError("Error: failed to delete a row\n");
					break;
				}
				if (s->page.header.numRecords == 0) {
					s->childIdx = (s->childIdx > 0) ? s->childIdx - 1 : (uint32_t)(-1);
					// deleteRecord()'s rebalancing (borrow/merge) can modify nodes
					// elsewhere in the tree, including our own current leaf out from
					// under us. Re-sync from our own address rather than assume our
					// cached leafNode is still accurate.
					readNode(s->leafAddr, &s->leafNode, t);
				} else {
					s->slotIdx = (s->slotIdx > 0) ? s->slotIdx - 1 : (uint32_t)(-1);
				}
				break;
			}
			case OP_CREATE_TABLE: {
				// schema entry is pre-populated in vm.schema by the compiler before execution
				uint8_t schemaIdx = READ_BYTE();
				uint32_t hash = vm.chunk->constants.values[schemaIdx].as.u32;
				// CREATE TABLE commits on its own, so a transaction couldn't DISCARD it
				if (transaction.active) {
					statementError("Error: CREATE TABLE can't run inside a transaction\n");
					break;
				}
				schema* s = readHT(hash, vm.schema);
				if (!s) {
					statementError("Error: schema not found for CREATE TABLE\n");
					break;
				}
				if (tableAlreadyExists(s->tablename)) {
					statementError("Error: table %s already exists\n", s->tablename);
					break;
				}
				page_num firstKey = { .type = getPkOrderingType(s) };
				if (vm.failed) {
					break;
				}
				// the new table's file and its schema entry are committed together, so a crash can't
				// leave one without the other
				table* t = newTree(s->tablename, firstKey);
				size_t len;
				char* bytes = serializeSchema(vm.schema, &len);
				bool committed = false;
				if (bytes) {
					file_change schemaChange = { SCHEMA_FILE, bytes, len };
					committed = commitTables(&t, 1, &schemaChange, 1);
					free(bytes);
				} else {
					printf("Error: failed to serialize the schema\n");
					discard(t);
				}
				if (t->source) fclose(t->source);
				freeTable(t);
				if (!committed) return INTERPRET_RUNTIME_ERROR;
				break;
			}
			case OP_DROP_TABLE: {
				uint8_t nameIdx = READ_BYTE();
				const char* name = vm.chunk->constants.values[nameIdx].as.text;
				uint32_t hash = hashString(name, (int)strlen(name));
				// DROP TABLE commits on its own, so a transaction couldn't DISCARD it
				if (transaction.active) {
					statementError("Error: DROP TABLE can't run inside a transaction\n");
					break;
				}
				if (!readHT(hash, vm.schema) && !tableAlreadyExists(name)) {
					statementError("Error: table '%s' not found\n", name);
					break;
				}
				// the schema entry and the table's file are removed together
				deleteHT(hash, vm.schema);
				size_t len;
				char* bytes = serializeSchema(vm.schema, &len);
				if (!bytes) {
					printf("Error: failed to serialize the schema\n");
					return INTERPRET_RUNTIME_ERROR;
				}
				size_t fileLen = strlen(name) + strlen(TABLE_EXTENSION) + 1;
				char* file = malloc(fileLen);
				snprintf(file, fileLen, "%s%s", name, TABLE_EXTENSION);
				file_change changes[] = { { SCHEMA_FILE, bytes, len }, { file, NULL, 0 } };
				bool committed = commitTables(NULL, 0, changes, 2);
				free(bytes);
				free(file);
				if (!committed) return INTERPRET_RUNTIME_ERROR;
				break;
			}
			case OP_BEGIN_TRANSACTION: {
				if (transaction.active) {
					statementError("Error: a transaction is already in progress\n");
					break;
				}
				transaction.active = true;
				transaction.count = 0;
				break;
			}
			case OP_COMMIT: {
				if (!transaction.active) {
					statementError("Error: no transaction in progress to commit\n");
					break;
				}
				// every table in the transaction commits under one commit marker in the log, so
				// either all of their changes survive a crash or none do
				table* tables[MAX_TXN_TABLES];
				for (int i = 0; i < transaction.count; i++) tables[i] = transaction.tables[i].tbl;
				bool committed = commitTables(tables, transaction.count, NULL, 0);
				for (int i = 0; i < transaction.count; i++) {
					fclose(tables[i]->source);
					freeTable(tables[i]);
				}
				transaction.active = false;
				transaction.count = 0;
				if (!committed) return INTERPRET_RUNTIME_ERROR;
				break;
			}
			case OP_DISCARD: {
				if (!transaction.active) {
					statementError("Error: no transaction in progress to discard\n");
					break;
				}
				for (int i = 0; i < transaction.count; i++) {
					table* t = transaction.tables[i].tbl;
					discard(t);
					fclose(t->source);
					freeTable(t);
				}
				transaction.active = false;
				transaction.count = 0;
				break;
			}
			case OP_SET_RESULT: {
				vm.results.print = true;
				break;
			}
			case OP_HALT: {
				return INTERPRET_OK;
			}
		}
	}
	#undef READ_BYTE
	#undef READ_CONSTANT
	#undef BINARY_OP
}

/*
undoes a statement that failed partway through: each table it was changing gets back the pending changes
it had before the statement, so a transaction carries on as if the statement never ran. A table outside a
transaction also drops them (none of the statement was committed) and is closed
*/
static void abortStatement(void) {
	for (int i = vm.numScanners - 1; i >= 0; i--) {
		scanner* s = &vm.scanners[i];
		if (!s->open) continue;
		rollbackStatement(s->tbl);
		if (!findTxnTable(s->tblHash)) {
			discard(s->tbl);
			fclose(s->tbl->source);
			freeTable(s->tbl);
		}
		freeSPage(&s->page);
		s->page = (slotted_page){0};
		s->tbl  = NULL;
		s->open = false;
	}
	vm.numScanners = 0;
}

result_buffer interpret(const char* source) {
	// load up database schema and initialize bytecode chunk
	chunk c;
	if (!schemaCache) {
		schemaCache = loadSchema();
	}
	if (!schemaCache) {
		vm.results.ir = INTERPRET_LOAD_ERROR;
		return vm.results;
	}
	initVM(schemaCache);
	initChunk(&c);
	// lex the query
	tokenized t = lexQuery(source);
	if (t.count == 1) { // return if there is no meaningful input TOKEN_EOF is automatic
		vm.results.ir = INTERPRET_OK;
		return vm.results;
	} else if (t.count == 0) { // should always at least have TOKEN_EOF
		vm.results.ir = INTERPRET_COMPILE_ERROR;
		return vm.results;
	}
	// parse the query into an AST
	ast_node* root = compile(t);
	freeTokenized(&t);
	if (!root) {
		vm.results.ir = INTERPRET_COMPILE_ERROR;
		return vm.results;
	}
	// determine whether the query is DDL so we can update the schema
	ast_node* stmt = root->children[0];
	bool ddl = stmt->type == TYPE_CREATE_STMT || stmt->type == TYPE_DROP_STMT;
	// generate bytecode from the AST
	if (!generate(root, &c, vm.schema)) {
		freeAST(root);
		if (ddl) invalidateSchema();
		vm.results.ir = INTERPRET_COMPILE_ERROR;
		return vm.results;
	}
	freeAST(root);

	// run bytecode 
	vm.chunk = &c;
	vm.ip = vm.chunk->code;

	vm.results.ir = run();
	if (vm.results.ir == INTERPRET_RUNTIME_ERROR) abortStatement();
	if (ddl) invalidateSchema();

	freeChunk(&c);
	return vm.results;
}