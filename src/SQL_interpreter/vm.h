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

#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "schema.h"
#include "../value.h"
#include "../storage_engine/bplus.h"
#include "../storage_engine/tableIO.h"
#include "../storage_engine/scanner.h"

#define STACK_MAX 256

typedef enum {
	INTERPRET_OK, // successful interpret but query did not request data
	INTERPRET_LOAD_ERROR,
	INTERPRET_COMPILE_ERROR,
	INTERPRET_RUNTIME_ERROR,
} interpret_result;

typedef struct result_buffer {
	value** rows;
	char* types; // the SQL type of each column in the result
	int count;
	int capacity;
	int cols;
	interpret_result ir;
	bool print;
} result_buffer;

// a virtual computer to manage a local database
typedef struct VM {
	chunk* chunk;
	uint8_t* ip; 					// instruction pointer
	value* stackTop;
	hashtable* schema; 				// contains the schema for each table
	result_buffer results;
	scanner scanners[MAX_SCANNERS]; // concurrent database processes
	int numScanners; 				// number of scanners currently open
	value stack[STACK_MAX]; 		// where values are stored
} VM;

// Public API — callable from outside this translation unit
// (resetStack, runtimeError, openScanner, closeScanner, equal, lessThan,
//  greaterThan, loadFirstValidPage, advanceScanner, valueToEntry, likeMatch,
//  and run are file-scoped static helpers)
void initVM(hashtable* schema);
void freeVM();
result_buffer interpret(const char* source);
void push(value value);
value pop();

#endif // VM_H