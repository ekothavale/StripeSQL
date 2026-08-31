SRC  = src
SQL  = src/SQL_interpreter
STOR = src/storage_engine
CFLAGS = -I$(SRC) -I$(SQL) -I$(STOR) -O3

OBJS = main.o debug.o \
       chunk.o generator.o lexer.o memory.o parser.o schema.o value.o vm.o \
       bplus.o ordering.o page.o tableIO.o stor_testing.o sql_testing.o

ASAN = -fsanitize=address

main: $(OBJS)
	clang -g $(OBJS) -o main
	rm -f $(OBJS)

clean:
	rm -f $(OBJS) main

# --- core ---

main.o: $(SRC)/main.c $(SRC)/common.h $(SRC)/debug.h \
        $(SQL)/lexer.h $(SQL)/chunk.h $(SQL)/vm.h \
        $(STOR)/bplus.h $(STOR)/testing.h
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

schema.o: $(SQL)/schema.c $(SQL)/schema.h $(SRC)/common.h
	clang $(CFLAGS) -c $(SQL)/schema.c -o schema.o

value.o: $(SRC)/value.c $(SRC)/value.h $(SRC)/memory.h
	clang $(CFLAGS) -c $(SRC)/value.c -o value.o

vm.o: $(SQL)/vm.c $(SQL)/vm.h $(SQL)/parser.h $(SQL)/chunk.h \
      $(SRC)/value.h $(SQL)/schema.h $(SRC)/common.h $(SRC)/debug.h
	clang $(CFLAGS) -c $(SQL)/vm.c -o vm.o

# --- storage engine ---

bplus.o: $(STOR)/bplus.c $(STOR)/bplus.h
	clang $(CFLAGS) -c $(STOR)/bplus.c -o bplus.o

ordering.o: $(STOR)/ordering.c $(STOR)/ordering.h $(SRC)/value.h
	clang $(CFLAGS) -c $(STOR)/ordering.c -o ordering.o

page.o: $(STOR)/page.c $(STOR)/page.h $(SRC)/common.h
	clang $(CFLAGS) -c $(STOR)/page.c -o page.o

tableIO.o: $(STOR)/tableIO.c $(STOR)/tableIO.h
	clang $(CFLAGS) -c $(STOR)/tableIO.c -o tableIO.o

stor_testing.o: $(STOR)/testing.c $(STOR)/testing.h $(STOR)/bplus.h $(STOR)/page.h
	clang $(CFLAGS) -c $(STOR)/testing.c -o stor_testing.o

sql_testing.o: $(SQL)/testing.c $(SQL)/testing.h $(SQL)/chunk.h $(SQL)/schema.h $(SRC)/value.h $(SQL)/lexer.h $(SQL)/parser.h $(SQL)/generator.h $(SQL)/vm.h $(SRC)/common.h
	clang $(CFLAGS) -c $(SQL)/testing.c -o sql_testing.o
