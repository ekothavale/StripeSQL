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

#ifndef WAL_H
#define WAL_H

#include <stdint.h>
#include "../common.h"
#include "../const.h"


#define WAL_MAGIC 0x10549800 // the last byte is empty so that it can combined with the object type
#define WAL_MAGIC_MASK 0xFFFFFF00 // selects the magic out of an entry's combined magic | type field
#define WAL_COMMIT_TAG 0x434F4D54 // "COMT": first word of the commit marker
#define WAL_LOG_PATH TABLE_DIRECTORY "stripe.log"
#define WAL_TABLE_NAME_LEN 256 // room for any identifier (MAX_IDENT_LEN in generator.c)
#define TRANS_ID_SEED 0x09410293
#define TRANS_ID_MULTIPLIER 1007683
#define TRANS_ID_INCREMENT 54321
#define TRANS_ID_MOD 0x7FFFFFFF

/*
kind of object an entry's payload holds, stored in the low byte of the entry's magic
recovery writes every kind the same way (payload bytes at addr); the type is for inspecting the log
*/
typedef enum {
	WAL_PAGE   = 1,
	WAL_NODE   = 2,
	WAL_META   = 3, // table header, always at address 0
	WAL_DELETE = 4, // 1-byte garbage marker (see deleteObject() in tableIO.c)
} wal_object_type;

/*
one logged write: payloadLen bytes to be written at addr in tables/<tableName>.tbl
on disk every entry takes exactly WAL_ENTRY_DISK_SIZE bytes, big-endian:
 magic | type (4B) | transID (4B) | tableName (256B, NUL-padded) | addr (8B) | payloadLen (2B) |
 payload (PAGE_SIZE B, zero-padded) | checksum (4B, CRC-32C of every byte before it)
*/
typedef struct wal_entry {
	uint32_t magic;
	uint32_t transID;
	char tableName[WAL_TABLE_NAME_LEN];
	address addr;
	uint16_t payloadLen;
	uint8_t payload[PAGE_SIZE];
	uint32_t checksum;
}wal_entry;

#define WAL_ENTRY_DISK_SIZE (4 + 4 + WAL_TABLE_NAME_LEN + 8 + 2 + PAGE_SIZE + 4)

/*
the commit marker appended after a transaction's entries; once it is synced, the transaction is committed
 commit tag (4B) | entry count (4B) | transID (4B) | checksum (4B, CRC-32C of the first 12 bytes)
*/
#define WAL_COMMIT_DISK_SIZE 16

typedef struct wal_manager {
	wal_entry entry;
	uint32_t numEntries;
	uint32_t transID;
	bool seeded; // whether transID has been set by this process yet
	FILE* log;
}wal_manager;

// Public API
bool initManager(void); // call at the start of every logged transaction
bool addLogEntry(const char* tableName, wal_object_type type, address addr, const uint8_t* bytes, uint16_t len);
bool markLogCommitted(void);
bool resetLog(void);
bool recover(void);
uint32_t crc32c_compute(uint32_t crc, const uint8_t* data, size_t length);

#endif
