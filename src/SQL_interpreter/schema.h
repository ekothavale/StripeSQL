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
manages .scma files which map table names to column names
each entry is stored as such:
 table name | # columns | column name 0 | ...

table names are stored as hashes and all column names are length prefixed
*/

#ifndef SCHEMA_H
#define SCHEMA_H

#include "../common.h"
#include "hashtable.h"

#define SCHEMA_PATH "tables/schema.scma"
#define SCHEMA_MAGIC 0xFFBB8844


// Public API — callable from outside this translation unit
// (readEntries and writeEntries are file-scoped static helpers)
void initSchema();
hashtable* loadSchema();
void saveSchema(hashtable* schema);

#endif