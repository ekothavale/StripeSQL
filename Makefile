SRC  = src
SQL  = src/SQL_interpreter
STOR = src/storage_engine
CFLAGS = -I$(SRC) -I$(SQL) -I$(STOR) -O3

OBJS = main.o debug.o \
       chunk.o generator.o lexer.o memory.o parser.o schema.o value.o vm.o \
       bplus.o file.o ordering.o page.o tableIO.o wal.o stor_testing.o sql_testing.o

ASAN = -fsanitize=address

main: $(OBJS)
	clang -g $(OBJS) -o main
	rm -f $(OBJS)

clean:
	rm -f $(OBJS) main

# crash-recovery test for the write-ahead log (macOS only; see crashtest/crashtest.py)
crashtest: main
	python3 crashtest/crashtest.py

# --- tests and coverage ---
# the unit tests (src/run_tests.c) create and delete tables and the schema file, so every target
# below builds and runs in a throwaway directory and never touches tables/

MAIN_SRCS = $(filter-out $(SRC)/run_tests.c, $(wildcard $(SRC)/*.c)) $(wildcard $(SQL)/*.c) $(wildcard $(STOR)/*.c)
TEST_SRCS = $(filter-out $(SRC)/main.c, $(wildcard $(SRC)/*.c)) $(wildcard $(SQL)/*.c) $(wildcard $(STOR)/*.c)
COVFLAGS  = -I$(SRC) -I$(SQL) -I$(STOR) -O0 -g -fprofile-instr-generate -fcoverage-mapping
COV_IGNORE = 'testing\.c|run_tests\.c'
LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV ?= xcrun llvm-cov

test:
	@dir=$$(mktemp -d); mkdir $$dir/tables; \
	clang $(CFLAGS) -g $(TEST_SRCS) -o $$dir/run_tests && (cd $$dir && ./run_tests); \
	status=$$?; rm -rf $$dir; exit $$status

# line, function and branch coverage of the unit tests (test code excluded)
coverage:
	@dir=$$(mktemp -d); mkdir $$dir/tables; \
	clang $(COVFLAGS) $(TEST_SRCS) -o $$dir/run_tests && \
	(cd $$dir && LLVM_PROFILE_FILE=$$dir/unit.profraw ./run_tests > /dev/null) && \
	$(LLVM_PROFDATA) merge -sparse $$dir/unit.profraw -o $$dir/all.profdata && \
	$(LLVM_COV) report $$dir/run_tests -instr-profile=$$dir/all.profdata -ignore-filename-regex=$(COV_IGNORE); \
	status=$$?; rm -rf $$dir; exit $$status

# the unit tests plus the crash-recovery test, run against an instrumented binary (macOS only, ~15 min)
# processes the crash test kills can leave unreadable profiles, so merging skips them (--failure-mode=all)
coverage-full:
	@dir=$$(mktemp -d); mkdir $$dir/tables $$dir/profiles; \
	clang $(COVFLAGS) $(TEST_SRCS) -o $$dir/run_tests && \
	clang $(COVFLAGS) $(MAIN_SRCS) -o $$dir/main && \
	(cd $$dir && LLVM_PROFILE_FILE=$$dir/profiles/unit.profraw ./run_tests > /dev/null) && \
	LLVM_PROFILE_FILE=$$dir/profiles/crash-%p.profraw STRIPESQL_BIN=$$dir/main python3 crashtest/crashtest.py && \
	$(LLVM_PROFDATA) merge -sparse --failure-mode=all $$dir/profiles/*.profraw -o $$dir/all.profdata 2>/dev/null && \
	$(LLVM_COV) report $$dir/main -object $$dir/run_tests -instr-profile=$$dir/all.profdata -ignore-filename-regex=$(COV_IGNORE); \
	status=$$?; rm -rf $$dir; exit $$status

.PHONY: clean crashtest test coverage coverage-full

# --- core ---

main.o: $(SRC)/main.c $(SRC)/common.h $(SRC)/debug.h \
        $(SQL)/lexer.h $(SQL)/chunk.h $(SQL)/vm.h \
        $(STOR)/bplus.h $(STOR)/wal.h $(STOR)/testing.h
	clang $(CFLAGS) -c $(SRC)/main.c -o main.o

debug.o: $(SRC)/debug.c $(SRC)/debug.h $(SRC)/common.h \
         $(SQL)/chunk.h $(SRC)/value.h \
         $(STOR)/bplus.h $(STOR)/page.h
	clang $(CFLAGS) -c $(SRC)/debug.c -o debug.o

# --- SQL interpreter ---

chunk.o: $(SQL)/chunk.c $(SQL)/chunk.h $(SRC)/common.h $(SRC)/memory.h $(SRC)/value.h
	clang $(CFLAGS) -c $(SQL)/chunk.c -o chunk.o

generator.o: $(SQL)/generator.c $(SQL)/generator.h $(SQL)/chunk.h \
             $(SQL)/parser.h $(SRC)/common.h
	clang $(CFLAGS) -c $(SQL)/generator.c -o generator.o

lexer.o: $(SQL)/lexer.c $(SQL)/lexer.h $(SRC)/common.h $(SRC)/memory.h $(SRC)/value.h
	clang $(CFLAGS) -c $(SQL)/lexer.c -o lexer.o

memory.o: $(SRC)/memory.c $(SRC)/memory.h
	clang $(CFLAGS) -c $(SRC)/memory.c -o memory.o

parser.o: $(SQL)/parser.c $(SQL)/parser.h $(SQL)/lexer.h $(SQL)/chunk.h $(SRC)/common.h
	clang $(CFLAGS) -c $(SQL)/parser.c -o parser.o

schema.o: $(SQL)/schema.c $(SQL)/schema.h $(SRC)/common.h $(SRC)/const.h $(STOR)/tableIO.h
	clang $(CFLAGS) -c $(SQL)/schema.c -o schema.o

value.o: $(SRC)/value.c $(SRC)/value.h $(SRC)/memory.h
	clang $(CFLAGS) -c $(SRC)/value.c -o value.o

vm.o: $(SQL)/vm.c $(SQL)/vm.h $(SQL)/parser.h $(SQL)/chunk.h \
      $(SRC)/value.h $(SQL)/schema.h $(SRC)/common.h $(SRC)/debug.h
	clang $(CFLAGS) -c $(SQL)/vm.c -o vm.o

# --- storage engine ---

bplus.o: $(STOR)/bplus.c $(STOR)/bplus.h
	clang $(CFLAGS) -c $(STOR)/bplus.c -o bplus.o

file.o: $(STOR)/file.c $(STOR)/file.h $(SRC)/common.h $(SRC)/const.h
	clang $(CFLAGS) -c $(STOR)/file.c -o file.o

ordering.o: $(STOR)/ordering.c $(STOR)/ordering.h $(SRC)/value.h
	clang $(CFLAGS) -c $(STOR)/ordering.c -o ordering.o

page.o: $(STOR)/page.c $(STOR)/page.h $(SRC)/common.h
	clang $(CFLAGS) -c $(STOR)/page.c -o page.o

tableIO.o: $(STOR)/tableIO.c $(STOR)/tableIO.h $(STOR)/file.h $(STOR)/wal.h
	clang $(CFLAGS) -c $(STOR)/tableIO.c -o tableIO.o

wal.o: $(STOR)/wal.c $(STOR)/wal.h $(STOR)/file.h $(SRC)/common.h $(SRC)/const.h
	clang $(CFLAGS) -c $(STOR)/wal.c -o wal.o

stor_testing.o: $(STOR)/testing.c $(STOR)/testing.h $(STOR)/bplus.h $(STOR)/page.h $(STOR)/wal.h
	clang $(CFLAGS) -c $(STOR)/testing.c -o stor_testing.o

sql_testing.o: $(SQL)/testing.c $(SQL)/testing.h $(SQL)/chunk.h $(SQL)/schema.h $(SRC)/value.h $(SQL)/lexer.h $(SQL)/parser.h $(SQL)/generator.h $(SQL)/vm.h $(SRC)/common.h
	clang $(CFLAGS) -c $(SQL)/testing.c -o sql_testing.o
