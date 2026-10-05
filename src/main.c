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

#include "time.h"
#include <sys/stat.h>

#include "common.h"
#include "debug.h"
#include "const.h"
#include "SQL_interpreter/lexer.h"
#include "SQL_interpreter/chunk.h"
#include "SQL_interpreter/vm.h"
#include "storage_engine/bplus.h"
#include "storage_engine/wal.h"
#include "storage_engine/file.h"
#include "storage_engine/testing.h"
#include "SQL_interpreter/testing.h"


/*
pretty prints the rows in the results of a query
each value is printed according to its own runtime type, which stays correct
for projected, reordered, and computed columns
*/
static void printResult(result_buffer result) {
    for (int r = 0; r < result.count; r++) {
        for (int c = 0; c < result.cols; c++) {
            if (c > 0) printf(" | ");
            value v = result.rows[r][c];
            switch (v.type) {
                case VAL_NULL:  printf("NULL");              break;
                case VAL_BOOL:  printf("%s", v.as.boolean ? "true" : "false"); break;
                case VAL_INT:   printf("%lld", v.as.integer); break;
                case VAL_FLOAT: printf("%g",   v.as.floating); break;
                case VAL_TEXT:  printf("%s",   v.as.text);    break;
                default:        printf("N/A");               break;
            }
        }
        printf("\n");
    }
    printf("(%d row%s)\n", result.count, result.count == 1 ? "" : "s");
}

static void printTime(struct timespec* start, struct timespec* end) {
    double time_taken = ((end->tv_sec - start->tv_sec) + 
                        (end->tv_nsec - start->tv_nsec) / 1e9) * 1e3;
    printf(" %.3f ms\n", time_taken);
}

static void repl() {
    struct timespec start, end; // timer

    char line[MAX_REPL_INPUT_LEN];
    for (;;) {
        printf("> ");

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        if (strncasecmp(line, "quit\n", 5) == 0) {
            break;
        }

        // continue if empty line
        bool blank = true;
        for (int i = 0; line[i] != '\0'; i++) {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\n' && line[i] != '\r') {
                blank = false;
                break;
            }
        }
        if (blank) continue;

        clock_gettime(CLOCK_MONOTONIC, &start); // start timer
        result_buffer result = interpret(line);
        clock_gettime(CLOCK_MONOTONIC, &end); // end timer

        if (result.print) printResult(result);
        printTime(&start, &end);
    }
}

/*
pulls a query out of a source that could contain multiple queries
returns null if string ends without '\0' or any other failure
mallocs query (caller responsible for buffer lifetime)
*/
static char* isolateQuery(int* start, int len, const char* source) {
    #define PEEK() (i < len-1 ? source[i+1] : '\0')
    bool singleQuote = false;
    bool lineComment = false;
    bool multiLineComment = false;
    for (int i = *start; i <= len; i++) {
        // a comment ends at its terminator or at the end of the file, where the '\0' case below
        // returns what's left
        if (lineComment && source[i] != '\0') {
            if (source[i] == '\n') lineComment = false;
            continue;
        }
        if (multiLineComment && source[i] != '\0') {
            if (source[i] == '*' && PEEK() == '/') {
                i++; // the closing '/' is part of the comment, so it can't open another one
                multiLineComment = false;
            }
            continue;
        }
        switch (source[i]) {
            case '\'': {
                singleQuote = !singleQuote;
                break;
            }
            case '\\': {
                if (PEEK() == '\'') i++;
                break;
            }
            // comments don't start inside a string
            case '-':
                if (!singleQuote && PEEK() == '-') {
                    i++;
                    lineComment = true;
                }
                break;
            case '/': {
                if (!singleQuote && PEEK() == '*') {
                    i++;
                    multiLineComment = true;
                }
                break; // a lone '/' is division; without this it fell through and ended the statement
            }
            case ';': {
                if (singleQuote) continue;
                char* out = malloc(i - *start + 2);
                strncpy(out, source + *start, i - *start + 1);
                out[i - *start + 1] = '\0';
                if (PEEK() == '\0') *start = len; // terminate if this is the last query
                else {
                    int j = i + 1;
                    while (j < len && (source[j] == '\n' || source[j] == '\t' || source[j] == ' ' || source[j] == '\r')) {
                        j++;
                    }
                    *start = j; // skips trailing whitespace; lands on next stmt, or on `len` if it was all trailing whitespace
                }
                return out;
            }
            case '\0': {
                char* out = malloc(i - *start + 1);
                strncpy(out, source + *start, i - *start + 1);
                *start = (i+1);
                return out;
            }
        }
    }
    return NULL;
}

/*
reads a file into a buffer
mallocs file buffer (caller responsible for buffer lifetime)
*/
static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    // get file size to allocate correct buffer
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    // allocate buffer
    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    // read file
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }
    buffer[bytesRead] = '\0';

    // clean up
    fclose(file);
    return buffer;
}

static void runFile(const char* path) {
    struct timespec start, end; // timer

    char* source = readFile(path);
    int exCode = 0;
    int pos = 0;
    int len = strlen(source);
    if (len == 0) {
        printf("Empty input\n");
        return;
    }
    char* query;
    int queriesProcessed = 0;
    clock_gettime(CLOCK_MONOTONIC, &start); // start timer
    while (pos < len) {

        // obtain and interpret query
        query = isolateQuery(&pos, len, source);
        if (query == NULL) {
            exCode = 65;
            break;
        }
        result_buffer result = interpret(query);
        free(query);

        // handle result
        if (result.ir == INTERPRET_LOAD_ERROR) {
            exCode = 60;
            break;
        }
        if (result.ir == INTERPRET_COMPILE_ERROR) {
            exCode = 65;
            break;
        }
        if (result.ir == INTERPRET_RUNTIME_ERROR) {
            exCode = 70;
            break;
        }
        if (result.print) printResult(result);
        queriesProcessed++;
    }
    clock_gettime(CLOCK_MONOTONIC, &end); // end timer
    if (queriesProcessed > 0) printTime(&start, &end);
    // cleanup
    free(source);
    if (exCode) exit(exCode);
}

int main(int argc, char** argv) {
    // the unit tests have their own entry point: src/run_tests.c, run with `make test`

    // only one process may use the database at a time; the lock is held until this process exits,
    // and the OS releases it even after a crash. It's taken before recovery so recovery can never
    // run while another process is partway through a commit
    mkdir(TABLE_DIRECTORY, 0755); // no-op if it already exists
    if (lockFileExclusive(LOCK_PATH) == -1) {
        printf("Error: couldn't lock %s; is another StripeSQL process using this database?\n", LOCK_PATH);
        return 75;
    }

    // finish or discard any commit a crash interrupted before anything reads a table
    if (!recover()) {
        printf("Error: crash recovery failed; the log (%s) was left in place\n", WAL_LOG_PATH);
        return 74;
    }

    if (argc == 1) {
        repl();
    } else if (argc == 2) {
        if (strncmp(argv[1], "-d", 2) == 0) {
            #define DEBUG_TRACE_EXECUTION
            repl();
        } else {
            runFile(argv[1]);
        }
    } else if (argc == 3) {
        if (strncmp(argv[2], "-d", 2) == 0) {
            #define DEBUG_TRACE_EXECUTION
        } else {
            printf("Usage: ./main [SQL file] [id]\n");
            return 0;
        }
        runFile(argv[1]);

    } else {
        printf("Usage: ./main [SQL file] [-d]\n");
    }
    return 0;
}