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
Recursive descent parser
*/

/*
query      → select_stmt | insert_stmt | update_stmt | delete_stmt
           | create_stmt | drop_stmt | alter_stmt

select_stmt  → SELECT [DISTINCT] select_list FROM identifier
               [where_clause] [group_clause] [having_clause]
               [order_clause] [limit_clause]
select_list  → * | select_item (, select_item)*
select_item  → expr [AS identifier]

where_clause  → WHERE expr
group_clause  → GROUP BY expr_list
having_clause → HAVING expr
order_clause  → ORDER BY expr [ASC | DESC]
limit_clause  → LIMIT NUMBER

Expressions:
expr           → or_expr
or_expr        → and_expr  (OR  and_expr)*
and_expr       → not_expr  (AND not_expr)*
not_expr       → NOT not_expr | comparison
comparison     → additive ((= | != | < | <= | > | >= | LIKE | IS [NOT] NULL) additive
                          | BETWEEN additive AND additive
                          | [NOT] IN ( expr_list ))*
additive       → multiplicative ((+ | -) multiplicative)*
multiplicative → unary ((* | /) unary)*
unary          → - unary | primary
primary        → NUMBER | STRING | IDENTIFIER | IDENTIFIER ( expr_list )
               | ( expr ) | *

insert_stmt → INSERT INTO identifier ( col_list ) VALUES ( val_list )
            | INSERT INTO identifier VALUES ( val_list )
col_list    → identifier (, identifier)*
val_list    → expr (, expr)*

update_stmt → UPDATE identifier SET assignment_list [where_clause]
assignment_list → assignment (, assignment)*
assignment  → identifier = expr

delete_stmt → DELETE FROM identifier [where_clause]

create_stmt → CREATE TABLE identifier ( col_def_list )
            | CREATE [UNIQUE] INDEX identifier ON identifier ( col_list )
            | CREATE VIEW identifier AS select_stmt
col_def_list → col_def (, col_def)*
col_def     → identifier identifier [NOT NULL]

drop_stmt   → DROP (TABLE | INDEX | VIEW | DATABASE) identifier

alter_stmt  → ALTER TABLE identifier ADD [COLUMN] col_def
            | ALTER TABLE identifier DROP COLUMN identifier
            | ALTER TABLE identifier ALTER COLUMN identifier identifier
*/

#include "../common.h"
#include "chunk.h"
#include "parser.h"

typedef struct parser {
    token* start;
    token* current;
	int numCurrent;
	int total;
} parser;

parser p;

static void initParser(tokenized t) {
	p.current = t.tokens;
	p.start = t.tokens;
	p.numCurrent = 0;
	p.total = t.count;
}

/*
returns the type of the current token without consuming it
*/
static token_type peek() {
	return (*p.current).type;
}

/*
returns the type of the current token and advances to the next one
*/
static token advance() {
	p.current++;
	p.numCurrent++;
	return p.current[-1];
}

/*
prints the current token as erroneous
*/
static void reportUnexpected() {
	char* errTok = malloc((*p.current).length + 1);
	strncpy(errTok, p.current->start, p.current->length);
	errTok[p.current->length] = '\0';
	printf("Error: unexpected token '%s' encountered (line %d)\n", errTok, p.current->line);
	free(errTok);
}

/*
checks to see if the current token is of a certain type.
if so, consumes it, else returns an error
*/
static token expect(token_type type) {
	if (p.total - p.numCurrent > 0 && type == (*p.current).type) {
		return advance();
	} else {
		reportUnexpected();
		exit(74);
	}
}

/*
frees the nodes of the given AST including the root pointer
*/
void freeAST(ast_node* node) {
    if (!node) return;
    for (int i = 0; i < 7; i++) freeAST(node->children[i]);
    free(node);
}

/*
allocates and zero-initializes an ast_node of the given type
*/
static ast_node* makeNode(ast_type type) {
	ast_node* node = calloc(1, sizeof(ast_node));
	node->type = type;
	return node;
}

// ##########################################################################################################################################
// ##########################################################################################################################################
// AST Building Functions

// forward declarations for expressions
static ast_node* valList();
static ast_node* expr();
static ast_node* listNode(ast_node* item, ast_node* next);

/*
primary → NUMBER | STRING | IDENTIFIER [( val_list )] | ( expr ) | *
tok: the NUMBER, STRING, IDENTIFIER, or * token
flag: true if this primary is a function call (IDENTIFIER followed by ( val_list ))
*/
static ast_node* primary() {
	token_type t = peek();
	if (t == TOKEN_NUMBER || t == TOKEN_STRING || t == TOKEN_NULL) {
		ast_node* node = makeNode(TYPE_PRIMARY);
		node->tok = advance();
		return node;
	}
	if (t == TOKEN_IDENTIFIER) {
		ast_node* node = makeNode(TYPE_PRIMARY);
		node->tok = advance();
		if (peek() == TOKEN_LEFT_PAREN) {
			advance();
			node->flag = FLAG_FUNCTION_CALL;  // function call
			if (peek() != TOKEN_RIGHT_PAREN) node->children[0] = valList();
			expect(TOKEN_RIGHT_PAREN);
		}
		return node;
	}
	if (t == TOKEN_LEFT_PAREN) {
		advance();
		ast_node* node = expr();
		expect(TOKEN_RIGHT_PAREN);
		return node;  // parentheses are grouping only — no wrapper node
	}
	if (t == TOKEN_STAR) {
		ast_node* node = makeNode(TYPE_PRIMARY);
		node->tok = advance();
		return node;
	}
	printf("Error: expected a primary expression\n");
	exit(74);
}

/*
unary → - unary | primary
tok: the - token
*/
static ast_node* unary() {
	if (peek() == TOKEN_MINUS) {
		ast_node* node = makeNode(TYPE_UNARY);
		node->tok = advance();
		node->children[0] = unary();
		return node;
	}
	return primary();
}

/*
multiplicative → unary ((* | /) unary)*
tok on TYPE_MULTIPLICATIVE node: the * or / token
*/
static ast_node* multiplicative() {
	ast_node* left = unary();
	while (peek() == TOKEN_STAR || peek() == TOKEN_SLASH) {
		ast_node* node = makeNode(TYPE_MULTIPLICATIVE);
		node->tok = advance();
		node->children[0] = left;
		node->children[1] = unary();
		left = node;
	}
	return left;
}

/*
additive → multiplicative ((+ | -) multiplicative)*
tok on TYPE_ADDITIVE node: the + or - token
*/
static ast_node* additive() {
	ast_node* left = multiplicative();
	while (peek() == TOKEN_PLUS || peek() == TOKEN_MINUS) {
		ast_node* node = makeNode(TYPE_ADDITIVE);
		node->tok = advance();
		node->children[0] = left;
		node->children[1] = multiplicative();
		left = node;
	}
	return left;
}

/*
comparison → additive ((= | != | < | <= | > | >= | LIKE) additive
                      | IS [NOT] NULL
                      | BETWEEN additive AND additive
                      | [NOT] IN ( val_list ))*
tok: the comparison operator token (=, !=, <, etc., IS, BETWEEN, or IN)
flag on IS node: true = IS NOT NULL, false = IS NULL
flag on IN node: true = NOT IN, false = IN
*/
static ast_node* comparison() {
	ast_node* left = additive();
	for (;;) {
		token_type t = peek();
		if (t == TOKEN_EQUAL     || t == TOKEN_BANG_EQUAL ||
		    t == TOKEN_LESS      || t == TOKEN_LESS_EQUAL  ||
		    t == TOKEN_GREATER   || t == TOKEN_GREATER_EQUAL ||
		    t == TOKEN_LIKE) {
			ast_node* node = makeNode(TYPE_COMPARISON);
			node->tok = advance();
			node->children[0] = left;
			node->children[1] = additive();
			left = node;
		} else if (t == TOKEN_IS) {
			ast_node* node = makeNode(TYPE_COMPARISON);
			node->tok = advance();
			node->children[0] = left;
			if (peek() == TOKEN_NOT) { advance(); node->flag = FLAG_IS_NOT_NULL; }
			expect(TOKEN_NULL);
			left = node;
		} else if (t == TOKEN_BETWEEN) {
			ast_node* node = makeNode(TYPE_COMPARISON);
			node->tok = advance();
			node->children[0] = left;
			node->children[1] = additive();
			expect(TOKEN_AND);
			node->children[2] = additive();
			left = node;
		} else if (t == TOKEN_IN) {
			ast_node* node = makeNode(TYPE_COMPARISON);
			node->tok = advance();
			node->children[0] = left;
			expect(TOKEN_LEFT_PAREN);
			node->children[1] = valList();
			expect(TOKEN_RIGHT_PAREN);
			left = node;
		} else if (t == TOKEN_NOT) {
			ast_node* node = makeNode(TYPE_COMPARISON);
			advance();
			node->tok = expect(TOKEN_IN);
			node->flag = FLAG_NOT_IN;  // NOT IN
			node->children[0] = left;
			expect(TOKEN_LEFT_PAREN);
			node->children[1] = valList();
			expect(TOKEN_RIGHT_PAREN);
			left = node;
		} else {
			break;
		}
	}
	return left;
}

/*
not_expr → NOT not_expr | comparison
tok: the NOT token
*/
static ast_node* notExpr() {
	if (peek() == TOKEN_NOT) {
		ast_node* node = makeNode(TYPE_NOT_EXPR);
		node->tok = advance();
		node->children[0] = notExpr();
		return node;
	}
	return comparison();
}

/*
and_expr → not_expr (AND not_expr)*
tok on TYPE_AND_EXPR node: the AND token
*/
static ast_node* andExpr() {
	ast_node* left = notExpr();
	while (peek() == TOKEN_AND) {
		ast_node* node = makeNode(TYPE_AND_EXPR);
		node->tok = advance();
		node->children[0] = left;
		node->children[1] = notExpr();
		left = node;
	}
	return left;
}

/*
or_expr → and_expr (OR and_expr)*
tok on TYPE_OR_EXPR node: the OR token
*/
static ast_node* orExpr() {
	ast_node* left = andExpr();
	while (peek() == TOKEN_OR) {
		ast_node* node = makeNode(TYPE_OR_EXPR);
		node->tok = advance();
		node->children[0] = left;
		node->children[1] = andExpr();
		left = node;
	}
	return left;
}

/*
expr → or_expr
*/
static ast_node* expr() {
	return orExpr();
}

/*
identifier → IDENTIFIER
tok: the consumed IDENTIFIER token
*/
static ast_node* identifier() {
	ast_node* node = makeNode(TYPE_IDENTIFIER);
	node->tok = expect(TOKEN_IDENTIFIER);
	return node;
}

/*
limit_clause → [LIMIT] NUMBER
consumed by caller: LIMIT
tok: the NUMBER token
*/
static ast_node* limitClause() {
	ast_node* node = makeNode(TYPE_LIMIT_CLAUSE);
	node->tok = expect(TOKEN_NUMBER);
	return node;
}

/*
order_clause → [ORDER] [BY] expr [ASC|DESC] (, expr [ASC|DESC])*
consumed by caller: ORDER, BY
flag on each list node: false = ASC (default), true = DESC
*/
static ast_node* orderClause() {
	ast_node* node = makeNode(TYPE_ORDER_CLAUSE);
	ast_node* item = makeNode(TYPE_LIST_NODE);
	item->children[0] = expr();
	if (peek() == TOKEN_DESC) { advance(); item->flag = FLAG_DESC; }
	else if (peek() == TOKEN_ASC) { advance(); }
	ast_node* head = item;
	ast_node* tail = item;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = makeNode(TYPE_LIST_NODE);
		next->children[0] = expr();
		if (peek() == TOKEN_DESC) { advance(); next->flag = FLAG_DESC; }
		else if (peek() == TOKEN_ASC) { advance(); }
		tail->children[1] = next;
		tail = next;
	}
	node->children[0] = head;
	return node;
}

/*
having_clause → [HAVING] expr
consumed by caller: HAVING
*/
static ast_node* havingClause() {
	ast_node* node = makeNode(TYPE_HAVING_CLAUSE);
	node->children[0] = expr();
	return node;
}

/*
group_clause → [GROUP] [BY] expr (, expr)*
consumed by caller: GROUP, BY
*/
static ast_node* groupClause() {
	ast_node* node = makeNode(TYPE_GROUP_CLAUSE);
	ast_node* head = listNode(expr(), NULL);
	ast_node* tail = head;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = listNode(expr(), NULL);
		tail->children[1] = next;
		tail = next;
	}
	node->children[0] = head;
	return node;
}

/*
where_clause → [WHERE] expr
consumed by caller: WHERE
*/
static ast_node* whereClause() {
	ast_node* node = makeNode(TYPE_WHERE_CLAUSE);
	node->children[0] = expr();
	return node;
}

static ast_node* selectStmt();  // forward declaration for CREATE VIEW

/*
creates a TYPE_LIST_NODE with item as children[0] and next as children[1]
*/
static ast_node* listNode(ast_node* item, ast_node* next) {
	ast_node* node = makeNode(TYPE_LIST_NODE);
	node->children[0] = item;
	node->children[1] = next;
	return node;
}

/*
col_list → identifier (, identifier)*
consumed by caller: surrounding ( )
*/
static ast_node* colList() {
	ast_node* head = listNode(identifier(), NULL);
	ast_node* tail = head;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = listNode(identifier(), NULL);
		tail->children[1] = next;
		tail = next;
	}
	return head;
}

/*
val_list → expr (, expr)*
consumed by caller: surrounding ( )
*/
static ast_node* valList() {
	ast_node* head = listNode(expr(), NULL);
	ast_node* tail = head;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = listNode(expr(), NULL);
		tail->children[1] = next;
		tail = next;
	}
	return head;
}

/*
col_def → identifier identifier [NOT NULL]
flag: true if NOT NULL constraint is present
*/
static ast_node* colDef() {
	ast_node* node = makeNode(TYPE_COL_DEF);
	node->children[0] = identifier();  // column name
	node->children[1] = identifier();  // type name
	if (peek() == TOKEN_NOT) {
		advance();
		expect(TOKEN_NULL);
		node->flag = FLAG_NOT_NULL_CONSTRAINT;
	} else if (peek() == TOKEN_PRIMARY) {
		advance();
		expect(TOKEN_KEY);
		node->flag = FLAG_PRIMARY_KEY_CONSTRAINT;
	}
	return node;
}

/*
col_def_list → col_def (, col_def)*
consumed by caller: surrounding ( )
*/
static ast_node* colDefList() {
	ast_node* head = listNode(colDef(), NULL);
	ast_node* tail = head;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = listNode(colDef(), NULL);
		tail->children[1] = next;
		tail = next;
	}
	return head;
}

/*
assignment → identifier = expr
*/
static ast_node* assignment() {
	ast_node* node = makeNode(TYPE_ASSIGNMENT);
	node->children[0] = identifier();
	expect(TOKEN_EQUAL);
	node->children[1] = expr();
	return node;
}

/*
assignment_list → assignment (, assignment)*
consumed by caller: SET
*/
static ast_node* assignmentList() {
	ast_node* head = listNode(assignment(), NULL);
	ast_node* tail = head;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = listNode(assignment(), NULL);
		tail->children[1] = next;
		tail = next;
	}
	return head;
}

/*
alter_stmt → [ALTER] TABLE identifier ADD [COLUMN] col_def
           | [ALTER] TABLE identifier DROP COLUMN identifier
           | [ALTER] TABLE identifier ALTER COLUMN identifier identifier
consumed by caller: ALTER
tok: the action keyword (ADD, DROP, or ALTER) identifying the alter variant
*/
static ast_node* alterStmt() {
	ast_node* node = makeNode(TYPE_ALTER_STMT);
	expect(TOKEN_TABLE);
	node->children[0] = identifier();
	if (peek() == TOKEN_ADD) {
		node->tok = advance();
		if (peek() == TOKEN_COLUMN) advance();
		node->children[1] = colDef();
	} else if (peek() == TOKEN_DROP) {
		node->tok = advance();
		expect(TOKEN_COLUMN);
		node->children[1] = identifier();
	} else if (peek() == TOKEN_ALTER) {
		node->tok = advance();
		expect(TOKEN_COLUMN);
		node->children[1] = identifier();  // column name
		node->children[2] = identifier();  // new type
	} else {
		reportUnexpected();
		printf("Expected ADD, DROP, or ALTER after ALTER TABLE <name>\n");
		exit(74);
	}
	return node;
}

/*
drop_stmt → [DROP] (TABLE | INDEX | VIEW | DATABASE) identifier
consumed by caller: DROP
tok: the object type keyword (TABLE, INDEX, VIEW, or DATABASE)
*/
static ast_node* dropStmt() {
	ast_node* node = makeNode(TYPE_DROP_STMT);
	token_type t = peek();
	if (t == TOKEN_TABLE || t == TOKEN_INDEX || t == TOKEN_VIEW || t == TOKEN_DATABASE) {
		node->tok = advance();  // store TABLE/INDEX/VIEW/DATABASE token to identify what is dropped
	} else {
		reportUnexpected();
		printf("Expected TABLE, INDEX, VIEW, or DATABASE after DROP\n");
		exit(74);
	}
	node->children[0] = identifier();
	return node;
}

/*
create_stmt → [CREATE] TABLE identifier ( col_def_list )
            | [CREATE] [UNIQUE] INDEX identifier ON identifier ( col_list )
            | [CREATE] VIEW identifier AS select_stmt
consumed by caller: CREATE
tok: the object type keyword (TABLE, INDEX, or VIEW)
flag: true if CREATE UNIQUE INDEX
*/
static ast_node* createStmt() {
	ast_node* node = makeNode(TYPE_CREATE_STMT);
	if (peek() == TOKEN_TABLE) {
		node->tok = advance();
		node->children[0] = identifier();
		expect(TOKEN_LEFT_PAREN);
		node->children[1] = colDefList();
		expect(TOKEN_RIGHT_PAREN);
	} else if (peek() == TOKEN_UNIQUE || peek() == TOKEN_INDEX) {
		if (peek() == TOKEN_UNIQUE) {
			node->flag = FLAG_UNIQUE_INDEX;
			advance();
		}
		node->tok = expect(TOKEN_INDEX);
		node->children[0] = identifier();  // index name
		expect(TOKEN_ON);
		node->children[1] = identifier();  // table name
		expect(TOKEN_LEFT_PAREN);
		node->children[2] = colList();
		expect(TOKEN_RIGHT_PAREN);
	} else if (peek() == TOKEN_VIEW) {
		node->tok = advance();
		node->children[0] = identifier();  // view name
		expect(TOKEN_AS);
		node->children[1] = selectStmt();
	} else {
		reportUnexpected();
		printf("Expected TABLE, INDEX, or VIEW after CREATE\n");
		exit(74);
	}
	return node;
}

/*
delete_stmt → [DELETE] FROM identifier [where_clause]
consumed by caller: DELETE
*/
static ast_node* deleteStmt() {
	ast_node* node = makeNode(TYPE_DELETE_STMT);
	expect(TOKEN_FROM);
	node->children[0] = identifier();
	if (peek() == TOKEN_WHERE) {
		advance();
		node->children[1] = whereClause();
	}
	return node;
}

/*
update_stmt → [UPDATE] identifier SET assignment_list [where_clause]
consumed by caller: UPDATE
*/
static ast_node* updateStmt() {
	ast_node* node = makeNode(TYPE_UPDATE_STMT);
	node->children[0] = identifier();
	expect(TOKEN_SET);
	node->children[1] = assignmentList();
	if (peek() == TOKEN_WHERE) {
		advance();
		node->children[2] = whereClause();
	}
	return node;
}

/*
insert_stmt → [INSERT] INTO identifier ( col_list ) VALUES ( val_list )
            | [INSERT] INTO identifier VALUES ( val_list )
consumed by caller: INSERT
flag: true if an explicit column list is present
*/
static ast_node* insertStmt() {
	ast_node* node = makeNode(TYPE_INSERT_STMT);
	expect(TOKEN_INTO);
	node->children[0] = identifier();
	if (peek() == TOKEN_LEFT_PAREN) {
		advance();
		node->children[1] = colList();
		expect(TOKEN_RIGHT_PAREN);
		node->flag = FLAG_COLUMN_LIST;  // explicit column list present
	}
	expect(TOKEN_VALUES);
	expect(TOKEN_LEFT_PAREN);
	node->children[2] = valList();
	expect(TOKEN_RIGHT_PAREN);
	return node;
}

/*
select_item → expr [AS identifier]
*/
static ast_node* selectItem() {
	ast_node* node = makeNode(TYPE_SELECT_ITEM);
	node->children[0] = expr();
	if (peek() == TOKEN_AS) {
		advance();
		node->children[1] = identifier();
	}
	return node;
}

/*
select_list → * | select_item (, select_item)*
flag on the star node: true signals SELECT * (wildcard — no expression children)
*/
static ast_node* selectList() {
	if (peek() == TOKEN_STAR) {
		advance();
		ast_node* star = makeNode(TYPE_SELECT_ITEM);
		star->flag = FLAG_SELECT_STAR;  // flag signals SELECT *
		return listNode(star, NULL);
	}
	ast_node* head = listNode(selectItem(), NULL);
	ast_node* tail = head;
	while (peek() == TOKEN_COMMA) {
		advance();
		ast_node* next = listNode(selectItem(), NULL);
		tail->children[1] = next;
		tail = next;
	}
	return head;
}

/*
select_stmt → [SELECT] [DISTINCT] select_list FROM identifier
              [where_clause] [group_clause] [having_clause]
              [order_clause] [limit_clause]
consumed by caller: SELECT
flag: true if DISTINCT is present
*/
static ast_node* selectStmt() {
	ast_node* out = makeNode(TYPE_SELECT_STMT);
	token_type type = peek();
	if (type == TOKEN_DISTINCT) {
		advance();
		out->flag = FLAG_SELECT_DISTINCT;
	}
	out->children[0] = selectList();
	expect(TOKEN_FROM);
	out->children[1] = identifier();
	while(true) {
		type = peek();
			switch(type) {
				case TOKEN_WHERE: {
					advance();
					out->children[2] = whereClause();
					break;
				}
				case TOKEN_GROUP: {
					advance();
					expect(TOKEN_BY);
					out->children[3] = groupClause();
					break;
				}
				case TOKEN_HAVING: {
					advance();
					out->children[4] = havingClause();
					break;
				}
				case TOKEN_ORDER: {
					advance();
					expect(TOKEN_BY);
					out->children[5] = orderClause();
					break;
				}
				case TOKEN_LIMIT: {
					advance();
					out->children[6] = limitClause();
					break;
				}
				default: {
					return out;
				}
			}
		}
		return out;
}

/*
begin_transaction_stmt → [BEGIN] TRANSACTION
consumed by caller: BEGIN
*/
static ast_node* beginTransactionStmt() {
	ast_node* node = makeNode(TYPE_BEGIN_TRANSACTION_STMT);
	expect(TOKEN_TRANSACTION);
	return node;
}

/*
commit_stmt → [COMMIT]
consumed by caller: COMMIT
*/
static ast_node* commitStmt() {
	return makeNode(TYPE_COMMIT_STMT);
}

/*
discard_stmt → [DISCARD]
consumed by caller: DISCARD
*/
static ast_node* discardStmt() {
	return makeNode(TYPE_DISCARD_STMT);
}

/*
query → select_stmt | insert_stmt | update_stmt | delete_stmt
      | create_stmt | drop_stmt | alter_stmt
      | begin_transaction_stmt | commit_stmt | discard_stmt
consumes the leading statement keyword and dispatches to the appropriate stmt function
*/
static ast_node* query() {
	ast_node* out = makeNode(TYPE_QUERY);
	token_type tok = peek();
	switch(tok) {
		case TOKEN_SELECT: {
			advance();
			out->children[0] = selectStmt();
			break;
		}
		case TOKEN_INSERT: {
			advance();
			out->children[0] = insertStmt();
			break;
		}
		case TOKEN_UPDATE: {
			advance();
			out->children[0] = updateStmt();
			break;
		}
		case TOKEN_DELETE: {
			advance();
			out->children[0] = deleteStmt();
			break;
		}
		case TOKEN_CREATE: {
			advance();
			out->children[0] = createStmt();
			break;
		}
		case TOKEN_DROP: {
			advance();
			out->children[0] = dropStmt();
			break;
		}
		case TOKEN_ALTER: {
			advance();
			out->children[0] = alterStmt();
			break;
		}
		case TOKEN_BEGIN: {
			advance();
			out->children[0] = beginTransactionStmt();
			break;
		}
		case TOKEN_COMMIT: {
			advance();
			out->children[0] = commitStmt();
			break;
		}
		case TOKEN_DISCARD: {
			advance();
			out->children[0] = discardStmt();
			break;
		}
		default: {
			reportUnexpected();
			printf("Expected SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, ALTER, BEGIN, COMMIT, or DISCARD\n");
			return NULL;
		}
	}
	return out;

}


/*
converts a stream of tokens into an abstract syntax tree
*/
ast_node* compile(tokenized t) {
	initParser(t);
	// probably some more cleanup that has to be done here
	return query();
}

