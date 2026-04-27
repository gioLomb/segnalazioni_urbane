/**
 * This module implements a thread-safe hash table using separate chaining
 * to resolve collisions. Keys and values are generic binary buffers.
 * All operations are protected by a readers-writer lock, allowing concurrent reads
 * while serializing writes.
 *
 * The hash function is pluggable via function pointer, and each table instance
 * uses a randomised seed to mitigate hash-flooding attacks. When the load factor
 * reaches 1, the table automatically resizes to the next prime >= double current
 * capacity. Entries can also be saved to and restored from binary file.
 */

#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define MAX_KEY_LEN     (1 << 12)
#define MAX_VALUE_SIZE  (1 << 20)
#define HT_DEFAULT_CAPACITY 101

typedef unsigned long (*hash_func)(const void *key, size_t keySize, unsigned long seed);

/**
 * Callback invoked by ht_foreach for every live entry in the table.
 * The key and value pointers are owned by the table and remain valid only
 * for the duration of the call. The callback must not call any mutating
 * operation on the same table (ht_set, ht_delete) as the table is held
 * under a read lock for the entire traversal.
 */
typedef void (*ht_foreach_cb)(void *key, size_t keySize,
                               void *value, size_t valueSize,
                               void *userdata);

/**
 * A single key-value pair stored in the hash table.
 * Entries with the same bucket index are linked through the next pointer,
 * forming a singly-linked list (separate chaining).
 */
typedef struct Entry {
    void          *key;
    size_t         keySize;
    void          *value;
    size_t         size;
    unsigned long  hash;   /* cached raw hash, avoids recomputation on resize */
    struct Entry  *next;
} Entry;

/**
 * The hash table container. Holds the bucket array (pool), where each slot
 * points to the head of a chain, along with current size and capacity.
 * A per-instance seed protects against hash-flooding attacks; a pluggable
 * hash function pointer allows any compatible algorithm.
 * All public fields are protected by an internal readers-writer lock.
 */
typedef struct {
    Entry          **pool;
    size_t           size;
    size_t           capacity;
    unsigned long    seed;
    hash_func        hashFunction;
    pthread_rwlock_t lock;
} Hash_Table;


/* ── API ─────────────────────────────────────────────────────────────── */

/**
 * Allocates and initialises a new hash table with at least initialCapacity
 * buckets. hashFunction must not be NULL.
 * Returns the new table or NULL on allocation failure.
 */
Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction);

/**
 * Frees all resources held by the table (entries, lock, struct).
 * If persistenceFilePath is non-NULL, calls ht_snapshot before cleanup.
 */
void ht_destroy(Hash_Table *table, const char *persistenceFilePath);

/**
 * Writes a point-in-time binary dump of all entries to path.
 * Format per record: [key_len: size_t][key: key_len bytes]
 *                    [val_len: size_t][value: val_len bytes]
 * Acquires a read lock for the duration of the write.
 */
void ht_snapshot(Hash_Table *table, const char *path);

/**
 * Populates the table from a binary file written by ht_snapshot.
 * Aborts if the table is non-empty. Returns 1 on success, 0 otherwise.
 */
int ht_load(Hash_Table *table, const char *path);

/**
 * Inserts or updates a key-value pair. Triggers a resize when
 * size + 1 >= capacity. Both table and key must not be NULL.
 * Returns 1 on success, 0 on allocation or resize failure.
 */
int ht_set(Hash_Table *table, void *key, size_t keySize,
           void *value, size_t valueSize);

/**
 * Copies the value for key into destBuffer (up to destSize bytes).
 * Acquires only a read lock; concurrent calls are safe.
 * Returns 1 if found and copied, 0 otherwise.
 */
int ht_get(Hash_Table *table, void *key, size_t keySize,
           void *destBuffer, size_t destSize);

/**
 * Removes the entry for key, freeing all associated memory.
 * Returns 1 if found and deleted, 0 if not found or a parameter is NULL.
 */
int ht_delete(Hash_Table *table, void *key, size_t keySize);

/**
 * Iterates over every live entry in unspecified order, invoking callback
 * once per entry under a read lock. The callback must not mutate the table.
 * Both table and callback must not be NULL.
 */
void ht_foreach(Hash_Table *table, ht_foreach_cb callback, void *userdata);

#endif /* HASH_TABLE_H */