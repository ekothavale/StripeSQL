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
This file includes constants that can be tweaked to alter the behavior and performance of this DBMS
Some combinations of constant values may break the DBMS
*/

#ifndef CONST_H
#define CONST_H

#define M_GLOBAL 45		// order (number of children a node can have) of the tree
#define PAGE_SIZE 4096 		// size in bytes of each page
#define FULL_FSYNC 0		// 1: sync commits with F_FULLFSYNC, which also flushes the drive's cache on macOS; 0: plain fsync (SQLite's default)

// NEED TO PROGRAMMATICALLY CALCULATE THESE BASED ON PAGE SIZE
// THE VALUES BELOW ARE PLACEHOLDERS
#define PAGE_NUM_SLOTS 64 		// Size of slot array within each page (each page can hold 72 tuples)
#define PAGE_NUM_ENTRIES 700 		// in reality this will be the size of the page minus the slot array and the header
#define PAGE_ARR_CAP 4000  		// page slot array size

#define MAX_REPL_INPUT_LEN 1024

#define TABLE_DIRECTORY "tables/" // directory in which table files (and the write-ahead log) are placed
#define TABLE_EXTENSION ".tbl" // file extension for table files
#define LOCK_PATH TABLE_DIRECTORY "stripe.lock" // held by the one process allowed to use the database

#endif