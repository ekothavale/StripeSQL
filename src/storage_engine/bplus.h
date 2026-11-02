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

#ifndef BPLUS_H
#define BPLUS_H

#include "../common.h"
#include "ordering.h"
#include "page.h"
#include "node.h"
#include "tableIO.h"

// UNTESTED
#define MAX_KEY(n) \
	((n)->keys[n->childCount-1])

// UNTESTED
#define MIN_KEY(n) \
	((n)->keys[0])

#define HALF_M (M_GLOBAL / 2)

table* newTree(char* tablename, ordering_type keyType); // in memory only, until committed
table* createTree(char* tablename, ordering_type keyType); // newTree() plus a commit that creates its file
void deleteTree(table* t);


address findPage(ordering_key key, table* t);
address findPageAndLeaf(ordering_key key, table* t, address* leafOut);

int insertRecord(sp_record* record, ordering_key key, table* t);
bool updateRecord(sp_record* record, ordering_key key, table* t);
bool deleteRecord(ordering_key key, table* t, slotted_page* page, address* leafOut);
bool searchRecord(ordering_key key, table* t);
sp_record readRecord(ordering_key key, table* t, slotted_page* page);
bool storePage(slotted_page* p, address pageAddr, table* t, bool* splitOut);

#endif // BPLUS_H