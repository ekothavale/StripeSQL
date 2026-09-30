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
runs every unit test suite (see testing.c in storage_engine/ and SQL_interpreter/)
the tests create and delete tables (and the schema file) in tables/ under the working directory,
so run them with `make test`, which builds and runs them in a throwaway directory
a failed assertion aborts with a non-zero exit status
*/

#include "common.h"
#include "storage_engine/testing.h"
#include "SQL_interpreter/testing.h"

int main(void) {
    // storage engine
    test_page();
    test_tableio();
    test_table_mgmt();
    test_btree();
    test_file();
    test_wal();
    // SQL interpreter
    test_chunk();
    test_value();
    test_lexer();
    test_parser();
    test_hashtable();
    test_schema();
    test_generator();
    test_vm();
    printf("All test suites passed.\n");
    return 0;
}
