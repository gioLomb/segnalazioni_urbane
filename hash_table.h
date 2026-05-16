/**
 * @file hash_table.h
 * @brief Thread-safe hash table with separate chaining and binary key-value support.
 * * This module provides a generic hash table implementation. It is designed to be 
 * thread-safe using a readers-writer lock
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
 * @brief Callback for table traversal.
 * @param key Pointer to the entry key.
 * @param keySize Size of the key in bytes.
 * @param value Pointer to the entry value.
 * @param valueSize Size of the value in bytes.
 * @param userdata User-provided pointer passed through ht_foreach.
 */
typedef void (*ht_foreach_cb)(void *key, size_t keySize,
                               void *value, size_t valueSize,
                               void *userdata);

typedef struct Entry {
    void          *key;
    size_t         keySize;
    void          *value;
    size_t         size;
    unsigned long  hash;   
    struct Entry  *next;
} Entry;

typedef struct {
    Entry **pool;
    size_t size;
    size_t capacity;
    unsigned long seed;
    hash_func hashFunction;
    pthread_rwlock_t lock;
} Hash_Table;

/* ── API ─────────────────────────────────────────────────────────────── */

/**
 * @brief Creates and initializes a new hash table.
 * @pre initialCapacity > 0 (recommended), hashFunction != NULL
 * @post A new Hash_Table instance is allocated and returned
 * @param initialCapacity Suggestion for the initial number of buckets.
 * @param hashFunction Function pointer to the hashing algorithm.
 * @return Pointer to the new Hash_Table, or NULL on failure.
 */
Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction);

/**
 * @brief Saves a snapshot of the table to a binary file.
 * @pre table != NULL, path != NULL
 * @post All current entries are serialized to the file at path
 * @param table Pointer to the hash table.
 * @param path Filesystem path where the snapshot will be saved.
 */
void ht_snapshot(Hash_Table *table, const char *path);

/**
 * @brief Inserts or updates a key-value pair in the table.
 * @pre table != NULL, key != NULL, keySize <= MAX_KEY_LEN, valueSize <= MAX_VALUE_SIZE
 * @post The value is stored under the specified key; table size increments if new
 * @param table Pointer to the hash table.
 * @param key Pointer to the key buffer.
 * @param keySize Size of the key.
 * @param value Pointer to the value buffer.
 * @param valueSize Size of the value.
 * @return 1 on success, 0 on failure
 */
int ht_set(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict value, size_t valueSize);

/**
 * @brief Retrieves a copy of the value associated with a key.
 * @pre table != NULL, key != NULL, destBuffer != NULL
 * @post Up to destSize bytes are copied into destBuffer if the key is found
 * @param table Pointer to the hash table.
 * @param key Pointer to the search key.
 * @param keySize Size of the key.
 * @param destBuffer Buffer where the value will be copied.
 * @param destSize Maximum capacity of destBuffer.
 * @return 1 if found, 0 otherwise.
 */
int ht_get(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict destBuffer, size_t destSize);

/**
 * @brief Removes an entry from the table.
 * @pre table != NULL, key != NULL
 * @post Entry is freed and removed; table size decrements if it existed
 * @param table Pointer to the hash table.
 * @param key Pointer to the key to delete.
 * @param keySize Size of the key.
 * @return 1 if found and deleted, 0 otherwise.
 */
int ht_delete(Hash_Table * restrict table, void * restrict key, size_t keySize);

/**
 * @brief Destroys the table and frees all allocated resources.
 * @pre table != NULL
 * @post Memory for all entries, the pool, and the table struct is released
 * @param table Pointer to the table to destroy.
 * @param persistenceFilePath Optional path to save a final snapshot before destruction.
 */
void ht_destroy(Hash_Table *table, const char *persistenceFilePath);

/**
 * @brief Loads table content from a binary snapshot.
 * @pre table != NULL (must be empty), path != NULL
 * @post The table is populated with entries from the file
 * @param table Pointer to an empty hash table.
 * @param path Path to the snapshot file.
 * @return 1 on success, 0 on failure.
 */
int ht_load(Hash_Table *table, const char *path);

/**
 * @brief Iterates over every entry in the table.
 * @pre table != NULL, cb != NULL
 * @post cb is invoked for every entry. Table is read-locked during traversal
 * @param table Pointer to the hash table.
 * @param cb Callback function.
 * @param userdata Arbitrary data passed to the callback.
 */
void ht_foreach(Hash_Table *table, ht_foreach_cb cb, void *userdata);

#endif