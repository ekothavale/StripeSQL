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

// NOTE: function scanNumber assumes max length of number literal is 255 characters
// NOTE: function parse assumes max length of query is 2048 tokens

#include <ctype.h>
#include <strings.h>
#include "lexer.h"
#include "../common.h"
#include "../value.h"

typedef struct lexer {
    const char* start;
    const char* current;
    int line;
} lexer;

lexer lex;

void initLexer(const char* source) {
    lex.start = source;
    lex.current = source;
    lex.line = 1;
}

/*
mallocs t->tokens
*/
void initTokenized(tokenized* t) {
    t->tokens = (token*) malloc(sizeof(token) * INITIAL_TOKEN_COUNT);
    t->capacity = INITIAL_TOKEN_COUNT;
    t->count = 0;
}

/*
t is stack allocated
frees t->tokens
*/
void freeTokenized(tokenized* t) {
    free(t->tokens);
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isDigit(char c) {
    return c >= '0' && c <= '9';
} 


static bool isAtEnd() {
    return *lex.current == '\0';
}

static token makeToken(token_type type) {
    token token;
    token.type = type;
    token.start = lex.start;
    token.length = (int)(lex.current - lex.start);
    token.line = lex.line;
    return token;
}

static token errorToken(const char* message) {
    token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lex.line;
    return token;
}

static char advance() {
    lex.current++;
    return lex.current[-1];
}

static char peek() {
    return *lex.current;
}

static char peekNext() {
    if (isAtEnd()) return '\0';
    return lex.current[1];
}

static bool match(char expected) {
  if (isAtEnd()) return false;
  if (*lex.current != expected) return false;
  lex.current++;
  return true;
}

static void skipWhitespace() {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                lex.line++;
                advance();
                break;
            case '-': // single line comments are '--' in SQL
                if (peekNext() == '-') {
                    while (peek() != '\n' && !isAtEnd()) advance();
                } else {
                    return;
                }
                break;
            case '/': // multi-line comments are '/* ... */' and don't nest
                if (peekNext() == '*') {
                    advance();
                    advance();
                    while (!isAtEnd() && !(peek() == '*' && peekNext() == '/')) {
                        if (peek() == '\n') lex.line++;
                        advance();
                    }
                    // step past the closing '*/'; a comment that is never closed runs to the end of the input
                    if (!isAtEnd()) {
                        advance();
                        advance();
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static token_type checkKeyword(int start, int length, const char* rest, token_type type) {
    if (lex.current - lex.start == start + length &&
        strncasecmp(lex.start + start, rest, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

/*
scans the next sequence of letters in the SQL query to determine
if the characters represent a keyword or an identifier
keywords are matched case-insensitively (CREATE == create)
INSTEAD OF PATTERN MATCHING, DFA COULD BE REPRESENTED WITH A TABLE INSTEAD
*/
static token_type identifierType() {
    switch (tolower((unsigned char)lex.start[0])) {
        case 'a':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'd': return checkKeyword(1, 2, "dd", TOKEN_ADD);
                case 'l':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'l': return checkKeyword(1, 2, "ll", TOKEN_ALL);
                        case 't': return checkKeyword(1, 4, "lter", TOKEN_ALTER);
                    }
                    break;
                case 'n':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'd': return checkKeyword(1, 2, "nd", TOKEN_AND);
                        case 'y': return checkKeyword(1, 2, "ny", TOKEN_ANY);
                    }
                    break;
                case 's':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'c': return checkKeyword(1, 2, "sc", TOKEN_ASC);
                    }
                    return checkKeyword(1, 1, "s", TOKEN_AS);
            }
            break;
        case 'b':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'a': return checkKeyword(1, 5, "ackup", TOKEN_BACKUP);
                case 'e': {
                    switch(tolower((unsigned char)lex.start[2])) {
                        case 'g': return checkKeyword(1, 4, "egin", TOKEN_BEGIN);
                        case 't': return checkKeyword(1, 6, "etween", TOKEN_BETWEEN);
                    }
                }
                case 'y': return checkKeyword(1, 1, "y", TOKEN_BY);
            }
            break;
        case 'c':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'a': return checkKeyword(1, 3, "ase", TOKEN_CASE);
                case 'h': return checkKeyword(1, 4, "heck", TOKEN_CHECK);
                case 'o':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'l': {
                            switch(tolower((unsigned char)lex.start[3])) {
                                case 'l': return checkKeyword(1, 6, "ollect", TOKEN_COLLECT);
                                case 'u': return checkKeyword(1, 5, "olumn", TOKEN_COLUMN);
                            }
                        }
                        case 'm': return checkKeyword(1, 5, "ommit", TOKEN_COMMIT);
                        case 'n': return checkKeyword(1, 9, "onstraint", TOKEN_CONSTRAINT);
                    }
                    break;
                case 'r': return checkKeyword(1, 5, "reate", TOKEN_CREATE);
            }
            break;
        case 'd':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'a': return checkKeyword(1, 7, "atabase", TOKEN_DATABASE);
                case 'e':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'f': return checkKeyword(1, 6, "efault", TOKEN_DEFAULT);
                        case 'l': return checkKeyword(1, 5, "elete", TOKEN_DELETE);
                        case 's': return checkKeyword(1, 3, "esc", TOKEN_DESC);
                    }
                    break;
                case 'i':
                    switch (tolower((unsigned char)lex.start[3])) {
                        case 'c': return checkKeyword(1, 6, "iscard", TOKEN_DISCARD);
                        case 't': return checkKeyword(1, 7, "istinct", TOKEN_DISTINCT);
                    }
                    break;
                case 'r': return checkKeyword(1, 3, "rop", TOKEN_DROP);
            }
            break;
        case 'e':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'x':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'e': return checkKeyword(1, 3, "xec", TOKEN_EXEC);
                        case 'i': return checkKeyword(1, 5, "xists", TOKEN_EXISTS);
                    }
                    break;
            }
            break;
        case 'f':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'o': return checkKeyword(1, 6, "oreign", TOKEN_FOREIGN);
                case 'r': return checkKeyword(1, 3, "rom", TOKEN_FROM);
                case 'u': return checkKeyword(1, 3, "ull", TOKEN_FULL);
            }
            break;
        case 'g': return checkKeyword(1, 4, "roup", TOKEN_GROUP);
        case 'h': return checkKeyword(1, 5, "aving", TOKEN_HAVING);
        case 'i':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'n':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'd': return checkKeyword(1, 4, "ndex", TOKEN_INDEX);
                        case 'n': return checkKeyword(1, 4, "nner", TOKEN_INNER);
                        case 's': return checkKeyword(1, 5, "nsert", TOKEN_INSERT);
                        case 't': return checkKeyword(1, 3, "nto", TOKEN_INTO);
                    }
                    return checkKeyword(1, 1, "n", TOKEN_IN);
                case 's': return checkKeyword(1, 1, "s", TOKEN_IS);
            }
            break;
        case 'j': return checkKeyword(1, 3, "oin", TOKEN_JOIN);
        case 'k': return checkKeyword(1, 2, "ey", TOKEN_KEY);
        case 'l':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'e': return checkKeyword(1, 3, "eft", TOKEN_LEFT);
                case 'i':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'k': return checkKeyword(1, 3, "ike", TOKEN_LIKE);
                        case 'm': return checkKeyword(1, 4, "imit", TOKEN_LIMIT);
                    }
                    break;
            }
            break;
        case 'n':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'o': return checkKeyword(1, 2, "ot", TOKEN_NOT);
                case 'u': return checkKeyword(1, 3, "ull", TOKEN_NULL);
            }
            break;
        case 'o':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'n': return checkKeyword(1, 1, "n", TOKEN_ON);
                case 'r':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'd': return checkKeyword(1, 4, "rder", TOKEN_ORDER);
                    }
                    return checkKeyword(1, 1, "r", TOKEN_OR);
                case 'u': return checkKeyword(1, 4, "uter", TOKEN_OUTER);
            }
            break;
        case 'p':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'r':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'i': return checkKeyword(1, 6, "rimary", TOKEN_PRIMARY);
                        case 'o': return checkKeyword(1, 8, "rocedure", TOKEN_PROCEDURE);
                    }
                    break;
            }
            break;
        case 'r':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'e': return checkKeyword(1, 6, "eplace", TOKEN_REPLACE);
                case 'i': return checkKeyword(1, 4, "ight", TOKEN_RIGHT);
                case 'o': return checkKeyword(1, 5, "ownum", TOKEN_ROWNUM);
            }
            break;
        case 's':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'e':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'l': return checkKeyword(1, 5, "elect", TOKEN_SELECT);
                        case 't': return checkKeyword(1, 2, "et", TOKEN_SET);
                    }
                    break;
            }
            break;
        case 't':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'a': return checkKeyword(1, 4, "able", TOKEN_TABLE);
                case 'o': return checkKeyword(1, 2, "op", TOKEN_TOP);
                case 'r':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'a': return checkKeyword(1, 10, "ransaction", TOKEN_TRANSACTION);
                        case 'u': return checkKeyword(1, 7, "runcate", TOKEN_TRUNCATE);
                    }
                    break;
            }
            break;
        case 'u':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'n':
                    switch (tolower((unsigned char)lex.start[2])) {
                        case 'i':
                            switch (tolower((unsigned char)lex.start[3])) {
                                case 'o': return checkKeyword(1, 4, "nion", TOKEN_UNION);
                                case 'q': return checkKeyword(1, 5, "nique", TOKEN_UNIQUE);
                            }
                            break;
                    }
                    break;
                case 'p': return checkKeyword(1, 5, "pdate", TOKEN_UPDATE);
            }
            break;
        case 'v':
            switch (tolower((unsigned char)lex.start[1])) {
                case 'a': return checkKeyword(1, 5, "alues", TOKEN_VALUES);
                case 'i': return checkKeyword(1, 3, "iew", TOKEN_VIEW);
            }
            break;
        case 'w': return checkKeyword(1, 4, "here", TOKEN_WHERE);
    }
    return TOKEN_IDENTIFIER;
}

static token identifier() {
    while (isAlpha(peek()) || isDigit(peek())) advance();
    return makeToken(identifierType());
}

static token number() {
    while (isDigit(peek())) advance();

    // Look for a fractional part
    if (peek() == '.' && isDigit(peekNext())) {
        // consume the .
        advance();

        while (isDigit(peek())) advance();
    }

    return makeToken(TOKEN_NUMBER);
}

/*
consumes the tokens of a character in a string literal
the actual contents of the string get processed in the parsing phase
*/
static token string() {
    while (peek() != '\'' && !isAtEnd()) {
        if (peek() == '\n') lex.line++;
        advance();
    }

    if (isAtEnd()) return errorToken("Unterminated string.");

    // the closing quote
    advance();
    return makeToken(TOKEN_STRING);
}

token scanToken() {
    skipWhitespace();
    lex.start = lex.current;
    if (isAtEnd()) return makeToken(TOKEN_EOF);

    char c = advance();
    if (isAlpha(c)) return identifier();
    if (isDigit(c)) return number();
    
    switch (c) {
        case '(': return makeToken(TOKEN_LEFT_PAREN);
        case ')': return makeToken(TOKEN_RIGHT_PAREN);
        case '{': return makeToken(TOKEN_LEFT_BRACE);
        case '}': return makeToken(TOKEN_RIGHT_BRACE);
        case ';': return makeToken(TOKEN_SEMICOLON);
        case ',': return makeToken(TOKEN_COMMA);
        case '.': return makeToken(TOKEN_DOT);
        case '-': return makeToken(TOKEN_MINUS);
        case '+': return makeToken(TOKEN_PLUS);
        case '/': return makeToken(TOKEN_SLASH);
        case '*': return makeToken(TOKEN_STAR);
        case '!':
            return makeToken(
            match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=':
            return makeToken(
            match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '<':
            return makeToken(
            match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            return makeToken(
            match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        case '\'': return string(); // strings use single quotes

    }

    printf("Error: unexpected symbol '%c' (line %d)\n", c, lex.line);

    return errorToken("");
}

void addToken(tokenized* t, token tok) {
    if (t->capacity < t->count + 1) {
        // Resize the token array if necessary
        printf("Growing tokenized array (for debugging purposes)\n");
        int oldCapacity = t->capacity;
        t->capacity = GROW_CAPACITY(oldCapacity);
        t->tokens = GROW_ARRAY(token, t->tokens, oldCapacity, t->capacity);
    }
    t->tokens[t->count] = tok;
    t->count++;
}

/*
lexes a query
*/
tokenized lexQuery(const char* source) {
    initLexer(source);
    tokenized t;
    initTokenized(&t);
    token tok;
    tok.type = TOKEN_EMPTY;
    while(tok.type != TOKEN_EOF) {
        tok = scanToken();
        if (tok.type == TOKEN_ERROR) {
            printf("%s", tok.start);
            // can do other cleanup
        } else {
            addToken(&t, tok);
        }
    }
    return t;
}
