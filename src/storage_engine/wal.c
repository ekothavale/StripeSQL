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
Implements a redo write ahead log

Commit protocol (driven by commitTables() in tableIO.c):
 1. initManager(), then addLogEntry() for every dirty page, node, delete marker and table header
 2. markLogCommitted(): sync the entries, append the commit marker, sync again  <- commit point
 3. make the same writes to the table files and sync them
 4. resetLog(): truncate the log and sync it
Each entry holds the exact bytes of one write, so redoing a committed log is idempotent: recovery can
rewrite every entry without knowing which writes reached disk before the crash.
Nothing is logged before commit because uncommitted changes never leave the dirty hashmaps, so there is
never anything on disk to undo.
*/

#include <stddef.h>
#include "wal.h"
#include "file.h"

// global manager (and the entry buffer it reuses for every entry) for cache friendliness
static wal_manager manager;
static char entryBuf[WAL_ENTRY_DISK_SIZE]; // one serialized entry

// initialization functions

#define ENTRY_CHECKSUM_OFFSET (WAL_ENTRY_DISK_SIZE - 4)
// The Castagnoli polynomial reflected for little-endian representation
#define POLY_CRC32C 0x82F63B78U

static uint32_t crc32c_table[256];
static int table_initialized = 0;

// Generates the lookup table for CRC-32C
static void crc32c_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ POLY_CRC32C;
            } else {
                crc >>= 1;
            }
        }
        crc32c_table[i] = crc;
    }
    table_initialized = 1;
}

// Computes or updates a CRC-32C checksum over a buffer
uint32_t crc32c_compute(uint32_t crc, const uint8_t *data, size_t length) {
    if (!table_initialized) {
        crc32c_init_table();
    }

    // CRC-32 is usually initialized with 0xFFFFFFFF, which flips the bits
    crc = ~crc;

    for (size_t i = 0; i < length; i++) {
        uint8_t table_index = (uint8_t)(crc ^ data[i]);
        crc = (crc >> 8) ^ crc32c_table[table_index];
    }

    // Final XOR inversion
    return ~crc;
}

/*
Linear Congruential Generator PRNG algorithm
the multiply is done in 64 bits so it can't overflow before the modulus is applied
*/
static uint32_t getTransID(uint32_t xn) {
	return (uint32_t)(((uint64_t)xn * TRANS_ID_MULTIPLIER + TRANS_ID_INCREMENT) % TRANS_ID_MOD);
}

/*
open the log file in the appropriate mode and assign to the wal_manager's file pointer
*/
static bool openLog(const char* mode) {
	FILE* out = fopen(WAL_LOG_PATH, mode);
	if (!out) return false;
	manager.log = out;
	return true;
}

/*
opens the log for appending, creating it if it doesn't exist yet
a new log also needs its directory synced: syncing a file doesn't make the directory entry naming it durable,
so a crash could otherwise lose the whole log during the first commit
*/
static bool ensureLogOpen(void) {
	if (manager.log) return true;
	FILE* probe = fopen(WAL_LOG_PATH, "rb");
	bool exists = probe != NULL;
	if (probe) fclose(probe);
	if (!openLog("ab")) return false;
	if (!exists && !(syncFile(manager.log) && syncDirectory(TABLE_DIRECTORY))) return false;
	return true;
}

// sets the metadata of the entry (called once per transaction)
static void initEntry(wal_entry* entry) {
	entry->magic = WAL_MAGIC;
	entry->transID = manager.transID;
	entry->payloadLen = 0;
}

/*
prepares the manager for a new transaction: opens the log and picks a new transaction ID
the log must be empty, since every commit resets it and recovery empties it at startup
*/
bool initManager(void) {
	if (!ensureLogOpen()) return false;
	if (fseek(manager.log, 0, SEEK_END) != 0 || ftell(manager.log) != 0) {
		printf("Error: the log still holds a transaction; recovery must run before new commits\n");
		return false;
	}
	if (manager.seeded) {
		manager.transID = getTransID(manager.transID);
	}
	else {
		manager.transID = TRANS_ID_SEED;
		manager.seeded = true;
	}
	manager.numEntries = 0;
	initEntry(&manager.entry);
	return true;
}

// write functions

/*
fills the manager's entry with one write
*/
static bool buildEntry(const char* tableName, wal_object_type type, address addr, const uint8_t* bytes, uint16_t len) {
	if (len > PAGE_SIZE || strlen(tableName) >= WAL_TABLE_NAME_LEN) return false;
	manager.entry.magic = WAL_MAGIC | type;
	memset(manager.entry.tableName, 0, WAL_TABLE_NAME_LEN);
	strcpy(manager.entry.tableName, tableName);
	manager.entry.addr = addr;
	manager.entry.payloadLen = len;
	memcpy(manager.entry.payload, bytes, len);
	return true;
}

/*
serializes an entry into exactly WAL_ENTRY_DISK_SIZE bytes (layout in wal.h) and fills in its checksum
checksumming the serialized bytes rather than the struct keeps compiler padding out of the checksum
*/
static void serializeEntry(wal_entry* e, char* buf) {
	memset(buf, 0, WAL_ENTRY_DISK_SIZE);
	char* p = buf;
	writeUIntBytewise(p, e->magic);              p += 4;
	writeUIntBytewise(p, e->transID);            p += 4;
	memcpy(p, e->tableName, WAL_TABLE_NAME_LEN); p += WAL_TABLE_NAME_LEN;
	writeULongBytewise(p, e->addr);              p += 8;
	writeUShortBytewise(p, e->payloadLen);       p += 2;
	memcpy(p, e->payload, e->payloadLen);        // the rest of the payload stays zeroed
	e->checksum = crc32c_compute(0, (const uint8_t*)buf, ENTRY_CHECKSUM_OFFSET);
	writeUIntBytewise(buf + ENTRY_CHECKSUM_OFFSET, e->checksum);
}

/*
parses a serialized entry
returns false if its magic, type, table name, length or checksum is invalid
*/
static bool parseEntry(const char* buf, wal_entry* e) {
	const char* p = buf;
	e->magic = readUIntBytewise(p);              p += 4;
	e->transID = readUIntBytewise(p);            p += 4;
	memcpy(e->tableName, p, WAL_TABLE_NAME_LEN); p += WAL_TABLE_NAME_LEN;
	e->addr = readULongBytewise(p);              p += 8;
	e->payloadLen = readUShortBytewise(p);       p += 2;
	memcpy(e->payload, p, PAGE_SIZE);
	e->checksum = readUIntBytewise(buf + ENTRY_CHECKSUM_OFFSET);

	uint32_t type = e->magic & ~WAL_MAGIC_MASK;
	if ((e->magic & WAL_MAGIC_MASK) != WAL_MAGIC || type < WAL_PAGE || type > WAL_DELETE) return false;
	if (e->payloadLen > PAGE_SIZE || e->tableName[WAL_TABLE_NAME_LEN - 1] != '\0') return false;
	return e->checksum == crc32c_compute(0, (const uint8_t*)buf, ENTRY_CHECKSUM_OFFSET);
}

/*
append a log entry to the managers log file
the entry isn't durable until markLogCommitted() syncs the log
*/
bool addLogEntry(const char* tableName, wal_object_type type, address addr, const uint8_t* bytes, uint16_t len) {
	if (!manager.log || !buildEntry(tableName, type, addr, bytes, len)) return false;
	serializeEntry(&manager.entry, entryBuf);
	if (fwrite(entryBuf, 1, WAL_ENTRY_DISK_SIZE, manager.log) != WAL_ENTRY_DISK_SIZE) return false;
	manager.numEntries++;
	return true;
}

/*
write a commit marker to the end of the log
the commit marker is structured as such:
 4B commit tag | 4B unsigned entry count | 4B unsigned transaction ID | 4B checksum of the first 12 bytes
the entries are synced before the marker is written, so the marker can never reach disk ahead of them
once the second sync returns, the transaction is committed: recovery will finish it after a crash
*/
bool markLogCommitted(void) {
	if (!manager.log || !syncFile(manager.log)) return false;
	char marker[WAL_COMMIT_DISK_SIZE];
	writeUIntBytewise(marker, WAL_COMMIT_TAG);
	writeUIntBytewise(marker + 4, manager.numEntries);
	writeUIntBytewise(marker + 8, manager.transID);
	writeUIntBytewise(marker + 12, crc32c_compute(0, (const uint8_t*)marker, 12));
	if (fwrite(marker, 1, WAL_COMMIT_DISK_SIZE, manager.log) != WAL_COMMIT_DISK_SIZE) return false;
	return syncFile(manager.log);
}

/*
reads the entry at the given index of the log into e
*/
static bool readEntry(uint32_t index, wal_entry* e) {
	if (fseek(manager.log, (long)index * WAL_ENTRY_DISK_SIZE, SEEK_SET) != 0) return false;
	if (fread(entryBuf, 1, WAL_ENTRY_DISK_SIZE, manager.log) != WAL_ENTRY_DISK_SIZE) return false;
	return parseEntry(entryBuf, e);
}

/*
writes one entry's bytes into its table file and syncs it
recovery can't use the B+ tree code (the table may be half-written), so this is a raw write
naive: opens, syncs and closes the table file for every entry
*/
static bool applyEntry(const wal_entry* e) {
	char path[sizeof(TABLE_DIRECTORY) + WAL_TABLE_NAME_LEN + sizeof(TABLE_EXTENSION)];
	snprintf(path, sizeof(path), "%s%s%s", TABLE_DIRECTORY, e->tableName, TABLE_EXTENSION);
	FILE* f = fopen(path, "rb+");
	if (!f) return false;
	bool written = fseek(f, (long)e->addr, SEEK_SET) == 0
	            && fwrite(e->payload, 1, e->payloadLen, f) == e->payloadLen
	            && syncFile(f);
	fclose(f);
	return written;
}

/*
write all the entries in the log file to disk and fsync
only needs to be used for crash recovery since otherwise the dirty hashmaps are live and contain the relevant data
every entry is verified before anything is written, so a damaged log is reported rather than half-applied
*/
static bool executeCommit(uint32_t numEntries, uint32_t transID) {
	for (uint32_t i = 0; i < numEntries; i++) {
		if (!readEntry(i, &manager.entry) || manager.entry.transID != transID) {
			printf("Error: log entry %u is damaged; the interrupted commit can't be finished\n", i);
			return false;
		}
	}
	for (uint32_t i = 0; i < numEntries; i++) {
		readEntry(i, &manager.entry);
		if (!applyEntry(&manager.entry)) {
			printf("Error: failed to write log entry %u to table '%s'\n", i, manager.entry.tableName);
			return false;
		}
	}
	return true;
}

/*
truncate and fsync log
the log is left open (and empty) for the next transaction
*/
bool resetLog(void) {
	if (manager.log) fclose(manager.log);
	manager.log = NULL;
	if (!openLog("wb")) return false;
	return syncFile(manager.log);
}

/*
check if log file is empty
*/
static bool checkSynced(void) {
	if (fseek(manager.log, 0, SEEK_END) != 0) return false;
	return ftell(manager.log) == 0;
}

/*
the last bytes of the file will be:
	4 byte committed tag | 4 byte # of entries | 4 byte transaction ID | 4 byte checksum
this function checks to see if a valid marker is there and that the rest of the log is exactly that many entries
if so, it fills the given pointers with the number of entries and transaction ID
*/
static bool checkCommitted(uint32_t* numEntries, uint32_t* transID) {
	if (fseek(manager.log, 0, SEEK_END) != 0) return false;
	long size = ftell(manager.log);
	if (size < WAL_COMMIT_DISK_SIZE || (size - WAL_COMMIT_DISK_SIZE) % WAL_ENTRY_DISK_SIZE != 0) return false;

	char marker[WAL_COMMIT_DISK_SIZE];
	if (fseek(manager.log, -WAL_COMMIT_DISK_SIZE, SEEK_END) != 0) return false;
	if (fread(marker, 1, WAL_COMMIT_DISK_SIZE, manager.log) != WAL_COMMIT_DISK_SIZE) return false;
	if (readUIntBytewise(marker) != WAL_COMMIT_TAG) return false;
	if (readUIntBytewise(marker + 12) != crc32c_compute(0, (const uint8_t*)marker, 12)) return false;

	uint32_t count = readUIntBytewise(marker + 4);
	if (count != (uint32_t)((size - WAL_COMMIT_DISK_SIZE) / WAL_ENTRY_DISK_SIZE)) return false;
	*numEntries = count;
	*transID = readUIntBytewise(marker + 8);
	return true;
}

/*
recovers the log file; must run before anything reads a table
if execution paused mid commit, the commit is retried
else, the transaction is discarded
returns false (leaving the log in place) if a committed transaction couldn't be finished
*/
bool recover(void) {
	if (manager.log) fclose(manager.log);
	manager.log = NULL;
	if (!openLog("rb")) return true; // no log yet, so nothing to recover
	bool synced = checkSynced(); // the last commit was synced if ther is nothing in the log
	if (synced) {
		fclose(manager.log);
		manager.log = NULL;
		return true;
	}
	uint32_t numEntries;
	uint32_t transID;
	bool committed = checkCommitted(&numEntries, &transID); // means a commit was started and never finished
	if (committed) {
		// if commit in progress: redo every op (all ops are idempotent)
		if (!executeCommit(numEntries, transID)) return false;
		printf("Recovery: finished an interrupted commit (%u log entries)\n", numEntries);
	}
	// otherwise, crash occurred mid-transaction before commit
	else {
		// in this case, the transaction is abandoned completely
		printf("Recovery: discarded an uncommitted transaction\n");
	}
	if (!resetLog()) return false;
	fclose(manager.log);
	manager.log = NULL;
	return true;
}
