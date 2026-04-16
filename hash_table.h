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

#define MAX_KEY_LEN    (1 << 12)  
#define MAX_VALUE_SIZE (1 << 20) 
#define HT_DEFAULT_CAPACITY 101

typedef unsigned long (*hash_func)(const void *key, size_t keySize, unsigned long seed);

/**
 * A single key-value pair stored in the hash table.
 * Entries with the same bucket index are linked through next pointer,
 * forming a linked list (chaining).
 */
typedef struct Entry {
    void          *key;    
    size_t keySize;
    void          *value;  
    size_t         size;   
    unsigned long  hash;  
    struct Entry  *next;   
} Entry;

/**
 * The hash table container. Holds the bucket array, where each slot points
 * to the head of a chained linked list, along with the current size and capacity.
 * A per-instance seed protects against hash-flooding attacks, while a pluggable
 * hash function pointer allows the user to supply any compatible hashing algorithm.
 * All fields are protected by an internal readers-writer lock.
 */
typedef struct {
    Entry        **pool; 
    size_t         size;      
    size_t         capacity;   
    unsigned long  seed;       
    hash_func      hashFunction;
    pthread_rwlock_t lock;      
} Hash_Table;


/* PROTOTYPES */


/**
 * Allocates and initialises a new hash table.
 * The actual bucket count starts from initialCapacity.
 * The provided hash function will be used for
 * all key operations and must not be NULL. 
 * Returns the new table pointer or NULL on allocation failure.
 */
Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction);

/**
 * Frees all resources held by the table: entries, rwlock, and the table struct itself.
 * If persistenceFilePath is available, it calls ht_snapshot to save data before clean up.
 * When persistenceFilePath is NULL the persistence is not provided.
 */
void ht_destroy(Hash_Table *table, const char *persistenceFilePath);

/**
 * creates a point-in-time binary dump of the hash table.
 * Iterates through all buckets and linked chains to serialize every active 
 * key-value pair into a compact binary file. This provides a "clean state" 
 * recovery point, discarding historical operation logs. Uses a readers-writer 
 * lock to ensure data consistency during the dump process.
 */
void ht_snapshot(Hash_Table *table, const char *path);

/**
 * Inserts a new key-value pair or updates (when the key already exists) an existing one.
 * A resize is triggered when size + 1 >= capacity.
 * Both table and key must not be NULL.
 * Returns 1 on success, 0 on memory allocation or resize failure.
 */
int ht_set(Hash_Table *table, void *key, size_t keySize, void *value, size_t valueSize);

/**
 * Get a value by key, copying it into destBuffer. 
 * Acquires only a read lock, so multiple concurrent calls
 * are safe. Returns 1 if the key was found and data was copied, 0 otherwise.
 */
int ht_get(Hash_Table *table, void *key, size_t keySize, void *destBuffer, size_t destSize);

/**
 * Removes a key-value pair from the table, unlinking the entry from its bucket
 * chain and freeing all associated memory. Returns 1 if the key was found and
 * deleted, 0 if not found or if at least one param is NULL.
 */
int ht_delete(Hash_Table *table, void *key, size_t keySize);

/**
 * Populates the table from a binary file,
 * reading entries sequentially and inserting them using ht_set(). Existing entries
 * are not cleared before loading.
 * The binary format per record is: [ key_len ][ key[key_len] ][ val_size ][ value[val_size] ].
 * Returns 1 on success, 0 if persistenceFilePath is NULL or the file cannot be opened.
 */
int ht_load(Hash_Table *table, const char *persistenceFilePath);

#endif /* HASH_TABLE_H */