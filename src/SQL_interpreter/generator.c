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

#include "generator.h"
#include "../common.h"
#include "schema.h"
#include <strings.h>

#define MAX_IDENT_LEN 256

static void munchExpr(ast_node* node, chunk* c, hashtable* ht, schema* s);

static uint32_t getConstraint(uint8_t typeCode) {
	return (typeCode & 0b11100000) >> 5;
}

/*
finds the index of the column with the primary key restriction in a table
returns 255 if there is no primary key (maximum 250 columns per table)
*/
static uint8_t getPkIdx(schema* s) {
	uint8_t pkIdx = 255;
	for (int i = 0; i < s->count; i++) {
		if (getConstraint(s->colTypes[i]) == CONSTRAINT_PRIMARY_KEY) {
			pkIdx = i;
			break;
		}
	}
	return pkIdx;
}

/*
copies tok's source text into buf as a null-terminated string
*/
static void tokenToStr(token tok, char* buf) {
    int len = tok.length < MAX_IDENT_LEN - 1 ? tok.length : MAX_IDENT_LEN - 1;
    memcpy(buf, tok.start, len);
    buf[len] = '\0';
}

/*
looks up a table's schema entry by name; returns NULL if not found
*/
static schema* lookupSchema(const char* tname, hashtable* ht) {
    uint32_t hash = hashString(tname, (int)strlen(tname));
    return readHT(hash, ht);
}

/*
returns the 0-based index of colname within s->colNames, or -1 if not found
*/
static int lookupColIdx(const char* colname, schema* s) {
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->colNames[i], colname) == 0) return i;
    }
    return -1;
}

/*
checks a where AST to determine if the where condition references the primary key
if so, the vm can search for nodes by primary key instead of doing a linear search
*/
static bool checkWhereForPK(ast_node* where, schema* s, uint8_t pkIdx, chunk* c, hashtable* ht) {
    // unwrap grammar layers: WHERE_CLAUSE → EXPR → OR → AND → NOT → COMPARISON
    ast_node* n = where->children[0]; // EXPR
	// unwrap the precedence layers to get a primary
    if (n->type == TYPE_OR_EXPR)  n = n->children[0];
    if (n->type == TYPE_AND_EXPR) n = n->children[0];
    if (n->type == TYPE_NOT_EXPR) n = n->children[0];
    if (n->type != TYPE_COMPARISON || n->tok.type != TOKEN_EQUAL) return false;

    ast_node* lhs = n->children[0];
    ast_node* rhs = n->children[1];
    // get identifier on left and literal on right
    if (rhs->type == TYPE_PRIMARY && rhs->tok.type == TOKEN_IDENTIFIER) {
        ast_node* tmp = lhs; lhs = rhs; rhs = tmp;
    }
    if (lhs->type != TYPE_PRIMARY || lhs->tok.type != TOKEN_IDENTIFIER) return false;
    if (rhs->type != TYPE_PRIMARY) return false;

	// read identifier into buffer
    char colname[MAX_IDENT_LEN];
    tokenToStr(lhs->tok, colname);
    if (pkIdx >= s->count) return false;
    // get pk column name from schema and compare
    // (depends on how schema stores column names — adjust accordingly)
    if (strcmp(colname, s->colNames[pkIdx]) != 0) return false;

	// rhs must be a literal, not another column reference
	if (rhs->tok.type == TOKEN_IDENTIFIER) return false;
	munchExpr(rhs, c, ht, s);
    return true;
}

/*
adds v to the chunk's constant pool and emits OP_CONSTANT + its index
writeDynamicConst should be used for variable size constants
*/
static void writeStaticConst(chunk* c, value v) {
    uint8_t idx = (uint8_t)addConstant(c, v);
    writeChunk(c, OP_CONSTANT, 0);
    writeChunk(c, idx, 0);
}

/*
adds v to the chunk's const pool and emits OP_CONSTANT + its index
adds v to the chunk's dynamic constant pool so it can later be freed
writeStaticConst should be used for fixed size constants
*/
static void writeDynamicConst(chunk* c, value v) {
	writeStaticConst(c, v);
	addDynamic(c, v);
}

/*
emits op followed by two 0xFF placeholder bytes for the jump offset.
returns the position of the placeholder so it can be patched later with patchJump()
*/
static int emitJump(chunk* c, opcode op) {
    writeChunk(c, op, 0);
    writeChunk(c, 0xFF, 0);
    writeChunk(c, 0xFF, 0);
    return c->count - 2;
}

/*
back-patches the 2-byte forward offset at pos to reach the current code position.
assumes OP_JUMP fix is applied (vm.ip += (int16_t)offset)
*/
static void patchJump(chunk* c, int pos) {
    int16_t jump = (int16_t)(c->count - (pos + 2));
    c->code[pos]     = (uint8_t)((uint16_t)jump >> 8);
    c->code[pos + 1] = (uint8_t)((uint16_t)jump & 0xFF);
}

/*
emits op with a signed 2-byte offset that reaches target, which may be behind current position.
assumes OP_JUMP fix is applied (vm.ip += (int16_t)offset)
*/
static void emitBackJump(chunk* c, opcode op, int target) {
    writeChunk(c, op, 0);
    int16_t jump = (int16_t)(target - (c->count + 2));
    writeChunk(c, (uint8_t)((uint16_t)jump >> 8), 0);
    writeChunk(c, (uint8_t)((uint16_t)jump & 0xFF), 0);
}

/*
s is the schema for the table currently being scanned; pass NULL when no column
references are expected (e.g., inside INSERT VALUES).
*/
static void munchExpr(ast_node* node, chunk* c, hashtable* ht, schema* s) {
	switch(node->type) {
		case TYPE_EXPR: {
			/* TYPE_EXPR is a passthrough — parser's expr() returns orExpr() directly
			   so TYPE_EXPR nodes are never instantiated; this case is unreachable */
			break;
		}

		case TYPE_OR_EXPR: {
			/*
			Short-circuit OR:  left OR right
			  [left]
			  JUMP_TRUE <true>   left is true: skip right
			  [right]            right's result is the OR result if left was false
			  JUMP <done>
			<true:>
			  OP_TRUE
			<done:>
			*/
			munchExpr(node->children[0], c, ht, s);
			int truePatch = emitJump(c, OP_JUMP_TRUE);
			munchExpr(node->children[1], c, ht, s);
			int donePatch = emitJump(c, OP_JUMP);
			patchJump(c, truePatch);
			writeChunk(c, OP_TRUE, 0);
			patchJump(c, donePatch);
			break;
		}

		case TYPE_AND_EXPR: {
			/*
			Short-circuit AND:  left AND right
			  [left]
			  JUMP_FALSE <false>   left is false: skip right
			  [right]              right's result is the AND result if left was true
			  JUMP <done>
			<false:>
			  OP_FALSE
			<done:>
			*/
			munchExpr(node->children[0], c, ht, s);
			int falsePatch = emitJump(c, OP_JUMP_FALSE);
			munchExpr(node->children[1], c, ht, s);
			int donePatch = emitJump(c, OP_JUMP);
			patchJump(c, falsePatch);
			writeChunk(c, OP_FALSE, 0);
			patchJump(c, donePatch);
			break;
		}

		case TYPE_NOT_EXPR: {
			munchExpr(node->children[0], c, ht, s);
			writeChunk(c, OP_NOT, 0);
			break;
		}

		case TYPE_COMPARISON: {
			token_type op = node->tok.type;
			switch(op) {
				case TOKEN_EQUAL: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_EQUAL, 0);
					break;
				}
				case TOKEN_BANG_EQUAL: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_NOT_EQUAL, 0);
					break;
				}
				case TOKEN_LESS: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_LESS, 0);
					break;
				}
				case TOKEN_LESS_EQUAL: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_LESS_EQUAL, 0);
					break;
				}
				case TOKEN_GREATER: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_GREATER, 0);
					break;
				}
				case TOKEN_GREATER_EQUAL: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_GREATER_EQUAL, 0);
					break;
				}
				case TOKEN_LIKE: {
					munchExpr(node->children[0], c, ht, s);
					munchExpr(node->children[1], c, ht, s);
					writeChunk(c, OP_LIKE, 0);
					break;
				}
				case TOKEN_IS: {
					/*
					IS NULL    (flag=false) → OP_IS_NULL
					IS NOT NULL (flag=true) → OP_NOT_NULL
					*/
					munchExpr(node->children[0], c, ht, s);
					writeChunk(c, node->flag ? OP_NOT_NULL : OP_IS_NULL, 0);
					break;
				}
				case TOKEN_BETWEEN: {
					/*
					A BETWEEN B AND C  →  (A >= B) AND (A <= C)
					A is emitted twice; valid because column reads have no side effects.
					Uses short-circuit AND pattern:
					  [A] [B] OP_GREATER_EQUAL
					  JUMP_FALSE <false>
					  [A] [C] OP_LESS_EQUAL
					  JUMP <done>
					<false:>
					  OP_FALSE
					<done:>
					*/
					munchExpr(node->children[0], c, ht, s);  // A
					munchExpr(node->children[1], c, ht, s);  // B
					writeChunk(c, OP_GREATER_EQUAL, 0);
					int falsePatch = emitJump(c, OP_JUMP_FALSE);
					munchExpr(node->children[0], c, ht, s);  // A (emitted again)
					munchExpr(node->children[2], c, ht, s);  // C
					writeChunk(c, OP_LESS_EQUAL, 0);
					int donePatch = emitJump(c, OP_JUMP);
					patchJump(c, falsePatch);
					writeChunk(c, OP_FALSE, 0);
					patchJump(c, donePatch);
					break;
				}
				case TOKEN_IN: {
					/*
					A IN (v1,...,vN):  (A==v1) OR ... OR (A==vN)
					A NOT IN (...):    same with OP_NOT appended (node->flag == true)
					A is emitted for each comparison; no DUP opcode available.

					Each non-last element:
					  [A] [vN] OP_EQUAL
					  JUMP_TRUE <found>
					Last element:
					  [A] [vN] OP_EQUAL   (result stays on stack)
					  JUMP <done>
					<found:>
					  OP_TRUE
					<done:>
					  [OP_NOT if NOT IN]
					*/
					int foundPatches[64];
					int patchCount = 0;

					ast_node* valIt = node->children[1];
					while (valIt) {
						bool isLast = (valIt->children[1] == NULL);
						munchExpr(node->children[0], c, ht, s);   // A
						munchExpr(valIt->children[0], c, ht, s);  // vN
						writeChunk(c, OP_EQUAL, 0);
						if (!isLast) {
							foundPatches[patchCount++] = emitJump(c, OP_JUMP_TRUE);
						}
						valIt = valIt->children[1];
					}
					int donePatch = emitJump(c, OP_JUMP);
					for (int i = 0; i < patchCount; i++) patchJump(c, foundPatches[i]);
					writeChunk(c, OP_TRUE, 0);
					patchJump(c, donePatch);
					if (node->flag) writeChunk(c, OP_NOT, 0);
					break;
				}
				default: {
					break;
				}
			}
			break;
		}

		case TYPE_ADDITIVE: {
			munchExpr(node->children[0], c, ht, s);
			munchExpr(node->children[1], c, ht, s);
			writeChunk(c, node->tok.type == TOKEN_PLUS ? OP_ADD : OP_SUBTRACT, 0);
			break;
		}

		case TYPE_MULTIPLICATIVE: {
			munchExpr(node->children[0], c, ht, s);
			munchExpr(node->children[1], c, ht, s);
			writeChunk(c, node->tok.type == TOKEN_STAR ? OP_MULTIPLY : OP_DIVIDE, 0);
			break;
		}

		case TYPE_UNARY: {
			// only operator is unary minus (tok.type == TOKEN_MINUS)
			munchExpr(node->children[0], c, ht, s);
			writeChunk(c, OP_NEGATE, 0);
			break;
		}

		case TYPE_PRIMARY: {
			token_type t = node->tok.type;
			if (t == TOKEN_NUMBER) {
				char numStr[MAX_IDENT_LEN];
				tokenToStr(node->tok, numStr);
				value v = strchr(numStr, '.')
				    ? FLOAT_VAL(atof(numStr))
				    : INTEGER_VAL(atoll(numStr));
				writeStaticConst(c, v);
			} else if (t == TOKEN_STRING) {
				// strip surrounding quote characters
				int slen = node->tok.length - 2;
				char* text = malloc(slen + 1);
				memcpy(text, node->tok.start + 1, slen);
				text[slen] = '\0';
				writeDynamicConst(c, TEXT_VAL(text));
			} else if (t == TOKEN_IDENTIFIER) {
				if (node->flag) {
					// function call: no OP_CALL opcode exists yet
					printf("Error: function calls are not yet supported in expressions\n");
				} else if (s) {
					// column reference: resolve name to schema index
					char colname[MAX_IDENT_LEN];
					tokenToStr(node->tok, colname);
					int idx = lookupColIdx(colname, s);
					if (idx >= 0) {
						writeChunk(c, OP_COLUMN, 0);
						writeChunk(c, (uint8_t)idx, 0);
					} else {
						printf("Error: unknown column '%s'\n", colname);
					}
				} else {
					// no schema context (e.g. an INSERT VALUES list) — a bare
					// identifier here isn't a valid value. Likely a string
					// literal that wasn't quoted with single quotes. Report
					// it instead of silently emitting nothing, since the
					// caller still counts this as one value on the stack.
					char text[MAX_IDENT_LEN];
					tokenToStr(node->tok, text);
					printf("Error: unexpected identifier '%s' where a value was expected (string literals must use single quotes)\n", text);
				}
			} else if (t == TOKEN_NULL) {
				writeChunk(c, OP_NULL, 0);
			}
			break;
		}
		default: {
			break;
		}
	}
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// MUNCH STATEMENT

// returns false if a statement references a table with no known schema
static bool munchStmt(ast_node* node, chunk* c, hashtable* ht) {
	switch(node->type) {
		case TYPE_QUERY: {
			if (!munchStmt(node->children[0], c, ht)) return false;
			writeChunk(c, OP_HALT, 0);
			break;
		}

		case TYPE_SELECT_STMT: {
			/*
			Bytecode layout:
			  CONSTANT <tname>         push table name string
			  OPEN_SCAN <pkIdx>        open scanner on the table
			  REWIND                   reset scanner to start
			(if scanning via pk)		   
			  [WHERE expr] 			   evaluate filter expression
			  KEY_SEARCH <exit_offset> search for record directly by pk
			  [OP_COLUMN ...]		   push each selected column (or matchExpr for expressions)
			  EMIT_ROW <col_count> 	   emit the result row
			(else)
			[loop_top:]
			  NEXT <exit_offset>       advance; if exhausted jump forward to exit
			  [WHERE expr]             evaluate filter condition (if present)
			  [JUMP_FALSE loop_top]    skip row if filter is false (backward)
			  [OP_COLUMN ...]          push each selected column (or matchExpr for expressions)
			  EMIT_ROW <col_count>     emit the result row
			  JUMP <loop_top>          loop back (backward)
			[exit:]
			  CLOSE_SCAN
			*/
			// setup
			char tname[MAX_IDENT_LEN];
			tokenToStr(node->children[1]->tok, tname);
			schema* s = lookupSchema(tname, ht);
			if (!s) {
				printf("Error: table '%s' does not exist\n", tname);
				return false;
			}

			// get primary key index
			uint8_t pkIdx = getPkIdx(s);

			writeStaticConst(c, UINT_VAL(s->hash));
			writeChunk(c, OP_OPEN_SCAN, 0);
			writeChunk(c, pkIdx, 0); // give scanner the primary key column index
			writeChunk(c, OP_REWIND, 0);

			// setup variables
			int nextPatch;
			int loopTop = c->count;
			bool pkScan = node->children[2] && checkWhereForPK(node->children[2], s, pkIdx, c, ht);

			// write bytecode for either a pk search or a linear scan
			if (pkScan) {
				// don't need to evaluate the where in this case since checkWhereForPK() already evaluates it
				// munchExpr(node->children[2]->children[0], c, ht, s);
				nextPatch = emitJump(c, OP_KEY_SEARCH);
			} else {
				nextPatch = emitJump(c, OP_NEXT);

				// WHERE: emit condition; jump back to loopTop (next row) if false
				if (node->children[2]) {
					munchExpr(node->children[2]->children[0], c, ht, s);
					emitBackJump(c, OP_JUMP_FALSE, loopTop);
				}
			}

			// Emit selected columns
			int colCount = 0;
			ast_node* listIt = node->children[0];
			ast_node* firstItem = listIt ? listIt->children[0] : NULL;
			if (firstItem && firstItem->flag) { // SELECT *: emit OP_COLUMN for every schema column in order
				if (s) {
					for (int i = 0; i < s->count; i++) {
						writeChunk(c, OP_COLUMN, 0);
						writeChunk(c, (uint8_t)i, 0);
						colCount++;
					}
				}
			} else { // SELECT column0 column1 ...
				// Named items: resolve simple column identifiers to OP_COLUMN;
				// fall back to munchExpr for computed expressions
				while (listIt) {
					ast_node* item = listIt->children[0];     // TYPE_SELECT_ITEM
					ast_node* itemExpr = item->children[0];   // the expression
					if (s && itemExpr && itemExpr->type == TYPE_PRIMARY &&
					    itemExpr->tok.type == TOKEN_IDENTIFIER) {
						char colname[MAX_IDENT_LEN];
						tokenToStr(itemExpr->tok, colname);
						int idx = lookupColIdx(colname, s);
						if (idx >= 0) {
							writeChunk(c, OP_COLUMN, 0);
							writeChunk(c, (uint8_t)idx, 0);
						}
					} else if (itemExpr) {
						munchExpr(itemExpr, c, ht, s);
					}
					colCount++;
					listIt = listIt->children[1];
				}
			}

			writeChunk(c, OP_EMIT_ROW, 0);
			writeChunk(c, (uint8_t)colCount, 0);

			// loop if doing a linear scan
			if (!pkScan) emitBackJump(c, OP_JUMP, loopTop);

			// cleanup
			patchJump(c, nextPatch);
			writeChunk(c, OP_CLOSE_SCAN, 0);
			writeStaticConst(c, UINT_VAL(s->hash));
			writeChunk(c, OP_SET_RESULT, 0);
			break;
		}

		case TYPE_INSERT_STMT: {
			/*
			Bytecode layout:
			  CONSTANT <hash>  		   push table's hash
			  OPEN_SCAN <pkIdx>        open scanner
			  [val_0 ... val_n-1]      evaluate each value expression in order
			  INSERT_ROW <val_count>   pop val_count values and insert as new row
			  CLOSE_SCAN

			Note: if an explicit column list is present (node->flag), values
			are pushed in the order written and assumed to be in schema column order.
			Reordering to match an arbitrary column list is not yet supported.
			*/
			char tname[MAX_IDENT_LEN];
			tokenToStr(node->children[0]->tok, tname);
			uint32_t hash = hashString(tname, strlen(tname));
			schema* s = readHT(hash, ht);
			if (!s) {
				printf("Error: table '%s' does not exist\n", tname);
				return false;
			}

			// obtain primary key index
			uint8_t pkIdx = getPkIdx(s);

			writeStaticConst(c, UINT_VAL(hash));
			writeChunk(c, OP_OPEN_SCAN, 0);
			writeChunk(c, pkIdx, 0);

			int valCount = 0;
			ast_node* valIt = node->children[2];
			while (valIt) {
				munchExpr(valIt->children[0], c, ht, NULL);
				valCount++;
				valIt = valIt->children[1];
			}
			writeChunk(c, OP_INSERT_ROW, 0);
			writeChunk(c, (uint8_t)valCount, 0);
			writeChunk(c, OP_CLOSE_SCAN, 0);
			break;
		}

		case TYPE_UPDATE_STMT: {
			/*
			Bytecode layout:
			  CONSTANT <tname>         push table name string
			  OPEN_SCAN <pkIdx>        open scanner
			  REWIND
			(if scanning via pk)
			  [WHERE expr]             evaluate pk literal
			  KEY_SEARCH <exit_offset> seek directly to row
			  [for each assignment:]
			    [new_value expr]       evaluate right-hand side
			    UPDATE_COL <idx>       replace column at idx in current slot
			(else)
			[loop_top:]
			  NEXT <exit_offset>       advance; jump to exit if done
			  [WHERE expr]             evaluate filter (if present)
			  [JUMP_FALSE loop_top]    skip row if filter is false
			  [for each assignment:]
			    [new_value expr]       evaluate right-hand side
			    UPDATE_COL <idx>       replace column at idx in current slot
			  JUMP <loop_top>          loop back
			[exit:]
			  CLOSE_SCAN
			*/
			char tname[MAX_IDENT_LEN];
			tokenToStr(node->children[0]->tok, tname);
			schema* s = lookupSchema(tname, ht);
			if (!s) {
				printf("Error: table '%s' does not exist\n", tname);
				return false;
			}

			uint8_t pkIdx = getPkIdx(s);

			writeStaticConst(c, UINT_VAL(s->hash));
			writeChunk(c, OP_OPEN_SCAN, 0);
			writeChunk(c, pkIdx, 0);
			writeChunk(c, OP_REWIND, 0);

			int nextPatch;
			int loopTop = c->count;
			bool pkScan = node->children[2] && checkWhereForPK(node->children[2], s, pkIdx, c, ht);

			if (pkScan) {
				nextPatch = emitJump(c, OP_KEY_SEARCH);
			} else {
				nextPatch = emitJump(c, OP_NEXT);
				if (node->children[2]) {
					munchExpr(node->children[2]->children[0], c, ht, s);
					emitBackJump(c, OP_JUMP_FALSE, loopTop);
				}
			}

			ast_node* asgmtIt = node->children[1];
			while (asgmtIt) {
				ast_node* asgmt = asgmtIt->children[0];  // TYPE_ASSIGNMENT
				munchExpr(asgmt->children[1], c, ht, s); // new value expression
				char colname[MAX_IDENT_LEN];
				tokenToStr(asgmt->children[0]->tok, colname);
				int idx = s ? lookupColIdx(colname, s) : 0;
				writeChunk(c, OP_UPDATE_COL, 0);
				writeChunk(c, (uint8_t)idx, 0);
				asgmtIt = asgmtIt->children[1];
			}

			if (!pkScan) emitBackJump(c, OP_JUMP, loopTop);
			patchJump(c, nextPatch);
			writeChunk(c, OP_CLOSE_SCAN, 0);
			break;
		}

		case TYPE_DELETE_STMT: {
			/*
			Bytecode layout:
			  CONSTANT <tname>         push table name string
			  OPEN_SCAN <pkIdx>        open scanner
			  REWIND
			(if scanning via pk)
			  [WHERE expr]             evaluate pk literal
			  KEY_SEARCH <exit_offset> seek directly to row
			  DELETE_ROW               delete current slot
			(else)
			[loop_top:]
			  NEXT <exit_offset>       advance; jump to exit if done
			  [WHERE expr]             evaluate filter (if present)
			  [JUMP_FALSE loop_top]    skip row if filter is false
			  DELETE_ROW               delete current slot (vm steps slotIdx back)
			  JUMP <loop_top>          loop back
			[exit:]
			  CLOSE_SCAN
			*/
			char tname[MAX_IDENT_LEN];
			tokenToStr(node->children[0]->tok, tname);
			schema* s = lookupSchema(tname, ht);
			if (!s) {
				printf("Error: table '%s' does not exist\n", tname);
				return false;
			}

			uint8_t pkIdx = getPkIdx(s);

			writeStaticConst(c, UINT_VAL(s->hash));
			writeChunk(c, OP_OPEN_SCAN, 0);
			writeChunk(c, pkIdx, 0);
			writeChunk(c, OP_REWIND, 0);

			int nextPatch;
			int loopTop = c->count;
			bool pkScan = node->children[1] && checkWhereForPK(node->children[1], s, pkIdx, c, ht);

			if (pkScan) {
				nextPatch = emitJump(c, OP_KEY_SEARCH);
			} else {
				nextPatch = emitJump(c, OP_NEXT);
				if (node->children[1]) {
					munchExpr(node->children[1]->children[0], c, ht, s);
					emitBackJump(c, OP_JUMP_FALSE, loopTop);
				}
			}

			writeChunk(c, OP_DELETE_ROW, 0);
			if (!pkScan) emitBackJump(c, OP_JUMP, loopTop);

			patchJump(c, nextPatch);
			writeChunk(c, OP_CLOSE_SCAN, 0);
			break;
		}

		case TYPE_CREATE_STMT: {
			if (node->tok.type == TOKEN_TABLE) {
				/*
				Bytecode layout:
				  CREATE_TABLE <schemaIdx>   look up schema in vm.schema by hash, then createTable

				The schema entry is built here and inserted into ht (which must alias vm.schema)
				so it is available to the VM at runtime when OP_CREATE_TABLE executes.
				*/
				char tname[MAX_IDENT_LEN];
				tokenToStr(node->children[0]->tok, tname);

				// Count columns in the col_def list
				int colCount = 0;
				ast_node* cdList = node->children[1];
				while (cdList) { colCount++; cdList = cdList->children[1]; }

				// Build schema entry and register it for the VM to find at runtime
				schema* s = malloc(sizeof(schema));
				s->tablename = strdup(tname);
				s->hash = hashString(tname, (int)strlen(tname));
				s->count = colCount;
				s->colNames = malloc(colCount * sizeof(char*));
				s->colTypes = malloc(colCount);
				cdList = node->children[1];
				for (int i = 0; i < colCount; i++) {
					ast_node* def = cdList->children[0];  // TYPE_COL_DEF
					// column name
					char colname[MAX_IDENT_LEN];
					tokenToStr(def->children[0]->tok, colname);
					s->colNames[i] = strdup(colname);
					// column type
					char typename[MAX_IDENT_LEN];
					tokenToStr(def->children[1]->tok, typename);
					if      (strcasecmp(typename, "TEXT")     == 0) s->colTypes[i] = SQL_TEXT;
					else if (strcasecmp(typename, "BOOL")     == 0) s->colTypes[i] = SQL_BOOL;
					else if (strcasecmp(typename, "BOOLEAN")  == 0) s->colTypes[i] = SQL_BOOL;
					else if (strcasecmp(typename, "INT")      == 0) s->colTypes[i] = SQL_INT;
					else if (strcasecmp(typename, "INTEGER")  == 0) s->colTypes[i] = SQL_INT;
					else if (strcasecmp(typename, "FLOAT")    == 0) s->colTypes[i] = SQL_FLOAT;
					else if (strcasecmp(typename, "DOUBLE")   == 0) s->colTypes[i] = SQL_DOUBLE;
					else if (strcasecmp(typename, "DATETIME") == 0) s->colTypes[i] = SQL_DATETIME;
					else if (strcasecmp(typename, "DATE")     == 0) s->colTypes[i] = SQL_DATE;
					else if (strcasecmp(typename, "TIME")     == 0) s->colTypes[i] = SQL_TIME;
					else                                             s->colTypes[i] = SQL_TEXT; // replace this for dynamic typing
					// column constraint
					uint8_t constraint = 0;
					if (def->flag == FLAG_NOT_NULL_CONSTRAINT) constraint = CONSTRAINT_NOT_NULL;
					else if (def->flag == FLAG_PRIMARY_KEY_CONSTRAINT) constraint = CONSTRAINT_PRIMARY_KEY;
					constraint <<= 5;
					s->colTypes[i] |= constraint;
					// continue through linked list
					cdList = cdList->children[1];
				}
				insertHT(s, ht);

				uint8_t schemaIdx = (uint8_t)addConstant(c, UINT_VAL(s->hash));
				writeChunk(c, OP_CREATE_TABLE, 0);
				writeChunk(c, schemaIdx, 0);
			} else {
				printf("Error: CREATE INDEX and CREATE VIEW are not yet supported\n");
			}
			break;
		}

		case TYPE_DROP_STMT: {
			if (node->tok.type == TOKEN_TABLE) {
				/*
				Bytecode layout:
				  DROP_TABLE <nameIdx>   reads name string from constants,
				                         removes schema from vm.schema, deletes B+ tree file

				Note: deleteHT is called by OP_DROP_TABLE in vm.c at runtime; do not
				call it here or the VM's lookup will fail.
				*/
				char tname[MAX_IDENT_LEN];
				tokenToStr(node->children[0]->tok, tname);

				uint8_t nameIdx = (uint8_t)addConstant(c, TEXT_VAL(strdup(tname)));
				writeChunk(c, OP_DROP_TABLE, 0);
				writeChunk(c, nameIdx, 0);
			} else {
				// DROP INDEX, VIEW, DATABASE have no corresponding opcodes yet
				printf("Error: DROP INDEX, DROP VIEW, and DROP DATABASE are not yet supported\n");
			}
			break;
		}

		case TYPE_ALTER_STMT: {
			printf("Error: ALTER TABLE is not yet supported\n");
			break;
		}

		case TYPE_BEGIN_TRANSACTION_STMT: {
			writeChunk(c, OP_BEGIN_TRANSACTION, 0);
			break;
		}

		case TYPE_COMMIT_STMT: {
			writeChunk(c, OP_COMMIT, 0);
			break;
		}

		case TYPE_DISCARD_STMT: {
			writeChunk(c, OP_DISCARD, 0);
			break;
		}

		case TYPE_WHERE_CLAUSE:
		case TYPE_HAVING_CLAUSE: {
			// Emit the filter expression; leaves a bool on the stack
			munchExpr(node->children[0], c, ht, NULL);
			break;
		}

		case TYPE_GROUP_CLAUSE:
		case TYPE_ORDER_CLAUSE:
		case TYPE_LIMIT_CLAUSE: {
			// Not yet implemented, require OP_LIMIT, OP_SORT
			break;
		}

		default:
			break;
	}
	return true;
}

/*
takes the given AST and converts it to bytecode, filling a chunk.
returns false if a statement references a table with no known schema.
*/
bool generate(ast_node* root, chunk* c, hashtable* schema) {
	return munchStmt(root, c, schema);
}