/**
 * @file server.h
 * @brief Server lifecycle
 */

#ifndef SERVER_H
#define SERVER_H

#include "config.h"
#include "hash_table.h"

// Maximum size of a serialised HTTP response header.
#define MAX_HEADER_STR_LEN 512
// Buffer size for numeric strings (content-length, port numbers, etc.).
#define MAX_NUMBER_LEN     24
// MurmurHash2 mix multiplier — chosen for its avalanche properties.
#define MURMUR_MUL   0x5bd1e995UL
// MurmurHash3 finalizer multiplier — breaks up residual bit patterns.
#define MURMUR_FIN   0x85ebca6bUL

/**
 * @brief MurmurHash-inspired hash function used by every Hash_Table instance.
 *
 * Accepts arbitrary binary keys; suitable for strings, integers and structs.
 * The seed (set once at table creation) prevents hash-flooding attacks.
 *
 * @param key     Pointer to the key bytes.
 * @param keySize Number of bytes to hash.
 * @param seed    Per-table seed value.
 * @return Unsigned long hash value.
 */
unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);


#endif /* SERVER_H */