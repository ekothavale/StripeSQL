TODO:
 - implement hashtables - DONE
	- mapping table names to column names and schema files - DONE
 - implement separate hashing function to map pks to internal
   keys (pagenum | offset) - DONE
 - implement scanner opcodes - DONE
 - implement logical opcodes - DONE
 - implement control flow opcodes - DONE
 - implement database manipulation opcodes - DONE
 - implement database definition opcodes - DONE
 - write compiler second pass to generate code from AST - DONE
 - test every component of the front end - DONE
 - test the whole project - DONE
 - add ability to process multiple queries in one file - DONE
 - fix memory leaks - DONE
 - merge schema and hash table
 - implement primary keys - DONE
 - implement ability to use column reorderings in queries
 - implement transactions - DONE
 - update terminal output

CONSIDERATIONS:
 - readNode and readPage need to propagate failure
 - Changing query column order is not supported