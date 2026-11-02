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

/*
This file defines a bunch of constructs used to represent a slotted page in memory.
This representation is not the same used to store pages on disk; the translation algorithms are in tableIO.
*/

#include "tableIO.h"
#include "page.h"
#include "node.h"

#define MAX_SCANNERS 4 // maximum amount of concurrent scanners

// state for scanning over a btree
typedef struct scanner {
	table* tbl;          // open table
	uint32_t tblHash;	 // hash of table's name (the table itself stores the string)
	bool open;           // whether scanner is connected to a table
	bool started;        // whether OP_NEXT has been called at least once
	bool atEnd;          // whether all rows have been exhausted

	node leafNode;       // currently loaded leaf node (scanner-owned)
	address leafAddr;    // disk address of leafNode
	uint32_t childIdx;   // which child of leafNode is the current page

	slotted_page page;   // currently loaded page (scanner-owned)
	address pageAddr;    // disk address of page
	uint32_t slotIdx;    // which slot (row) within page is current

	// CHANGE TO UINT8_T for consistency
	int pkIdx;			 // the column containing the primary key in the input query (-1 = no pk)

	// strings copied out of the current row for the VM's stack (scanner-owned). They are freed when the
	// scanner moves to another row or closes, so nothing that pops one has to free it
	char** scratch;
	int scratchCount;
	int scratchCap;
} scanner;
