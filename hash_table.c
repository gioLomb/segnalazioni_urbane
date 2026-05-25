#include "hash_table.h"

/* ── Forward declarations ───────────────────────────────────────────────── */

static Entry *create_entry(void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash);

static int ht_resize(Hash_Table *table);

static int is_prime(size_t n);

static size_t next_prime(size_t n);

static inline void save_entry(Entry *e, FILE *f);

static unsigned long generate_seed(void);

static inline int keys_equal(const void *a, size_t aSize,const void *b, size_t bSize);


/* ── API implementation ──────────────────────────────────────────────── */

Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction) {
    // Allocate memory for the main structure
    Hash_Table *table = malloc(sizeof(Hash_Table));
    if (!table) return NULL;

    table->size = 0;
    // Ensure capacity is at least the default value
    table->capacity = initialCapacity > 0 ? initialCapacity : HT_DEFAULT_CAPACITY;

    // Allocate bucket array and initialize with NULL pointers
    table->pool = calloc(table->capacity, sizeof(Entry *));
    if (!table->pool) { 
        free(table); return NULL; 
    }

    // Initialize security seed and synchronization primitives
    table->seed = generate_seed();
    table->hashFunction = hashFunction;
    pthread_rwlock_init(&table->lock, NULL);
    return table;
}

void ht_snapshot(Hash_Table *table, const char *path) {
    if (!table || !path) return;

    FILE *f = fopen(path, "wb");
    if (!f) { perror("ht_snapshot: fopen"); return; }

    // Use a read lock to allow concurrent reads but prevent writes during IO
    pthread_rwlock_rdlock(&table->lock);
    for (size_t i = 0; i < table->capacity; i++) {
        // Traverse each bucket's linked list
        for (Entry *e = table->pool[i]; e; e = e->next)
            save_entry(e, f);
    }
    pthread_rwlock_unlock(&table->lock);
    fclose(f);
}

int ht_set(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict value, size_t valueSize) {
    if (!table || !key) return 0;
    
    // Writers-only lock: modification of the structure starts here
    pthread_rwlock_wrlock(&table->lock);

    unsigned long h = table->hashFunction(key, keySize, table->seed);
    unsigned int index = h % table->capacity;

    // Update existing entry if key is already present
    for (Entry *e = table->pool[index]; e; e = e->next) {
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;

        // Allocate new value buffer before freeing old one to maintain consistency on failure
        void *newValue = malloc(valueSize);
        if (!newValue) goto error;
        free(e->value);
        e->value = newValue;
        memcpy(e->value, value, valueSize);
        e->size = valueSize;

        pthread_rwlock_unlock(&table->lock);
        return 1;
    }

 
    //We resize before inserting to keep load factor <= 1.0
    if (table->size + 1 >= table->capacity) {
        if (!ht_resize(table)) goto error;
        // Recalculate index as capacity has changed
        index = h % table->capacity;
    }

    Entry *newEntry = create_entry(key, keySize, value, valueSize, h);
    if (!newEntry) goto error;

    // Standard linked list insertion at head of the bucket
    newEntry->next = table->pool[index];
    table->pool[index] = newEntry;
    table->size++;

    pthread_rwlock_unlock(&table->lock);
    return 1;

error:
    pthread_rwlock_unlock(&table->lock);
    return 0;
}

int ht_get(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict destBuffer, size_t destSize) {
    if (!table || !key || !destBuffer) return 0;
    
    // Concurrent access allowed here
    pthread_rwlock_rdlock(&table->lock);

    unsigned int index = table->hashFunction(key, keySize, table->seed) % table->capacity;
    for (Entry *e = table->pool[index]; e; e = e->next) {
        // Compare binary keys exactly
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;

        // Prevent buffer overflow in destination
        size_t n = e->size < destSize ? e->size : destSize;
        memcpy(destBuffer, e->value, n);
        pthread_rwlock_unlock(&table->lock);
        return 1;
    }

    pthread_rwlock_unlock(&table->lock);
    return 0;
}

int ht_delete(Hash_Table * restrict table, void * restrict key, size_t keySize) {
    if (!table || !key) return 0;
    pthread_rwlock_wrlock(&table->lock);

    unsigned int index = table->hashFunction(key, keySize, table->seed) % table->capacity;
    Entry *prev = NULL;
    Entry *e = table->pool[index];

    // Standard linked list node removal search
    while (e && !keys_equal(e->key, e->keySize, key, keySize)) {
        prev = e;
        e = e->next;
    }

    if (!e) {
        pthread_rwlock_unlock(&table->lock);
        return 0;
    }

    // Unlinking logic
    if (prev) prev->next = e->next;
    else table->pool[index] = e->next;

    // Explicitly free deep-copied fields
    free(e->key);
    free(e->value);
    free(e);
    table->size--;
    
    pthread_rwlock_unlock(&table->lock);
    return 1;
}

static int ht_resize(Hash_Table *table) {
    // Determine new capacity using primality for better distribution
    size_t  newCap  = next_prime(table->capacity * 2);
    Entry **newPool = calloc(newCap, sizeof(Entry *));
    if (!newPool) return 0;

    // Rehash all existing entries
    for (size_t i = 0; i < table->capacity; i++) {
        Entry *e = table->pool[i];
        while (e) {
            Entry *next = e->next;
            // Use cached hash to avoid expensive recomputations
            unsigned int newIndex = e->hash % newCap;

            // Move entry to new bucket
            e->next = newPool[newIndex];
            newPool[newIndex] = e;
            e = next;
        }
    }
    
    // Free old bucket array (entries themselves were moved, not freed)
    free(table->pool);
    table->pool = newPool;
    table->capacity = newCap;
    return 1;
}

static unsigned long generate_seed(void) {
    unsigned long seed;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) goto fallback;

    if (fread(&seed, sizeof(seed), 1, f) != 1) {
        fclose(f);
        goto fallback;
    }

    fclose(f);
    return seed;

fallback:
    return (unsigned long)time(NULL);
}

static inline int keys_equal(const void *a, size_t aSize,
                              const void *b, size_t bSize) {
    // Short-circuit: if lengths differ, keys cannot be equal
    return aSize == bSize && memcmp(a, b, aSize) == 0;
}

void ht_destroy(Hash_Table *table, const char *persistenceFilePath) {
    if (!table) return;

    // Optional: save the current state to a file before wiping memory
    if (persistenceFilePath) ht_snapshot(table, persistenceFilePath);

    // Deep-clean the table: iterate through every bucket
    for (size_t i = 0; i < table->capacity; i++) {
        Entry *e = table->pool[i];
        while (e) {
            Entry *next = e->next;
            // Free all dynamically allocated members of the entry
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
    }
    
    // Release the bucket array and the synchronization primitive
    free(table->pool);
    pthread_rwlock_destroy(&table->lock);
    free(table);
}

int ht_load(Hash_Table *table, const char *path) {
    if (!table || !path) return 0;
    
    // Safety check: loading into a non-empty table could cause logic conflicts
    if (table->size > 0) {
        fprintf(stderr, "ht_load: table is not empty, aborting\n");
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    size_t keyLen, valLen;
    char  *key = NULL;

    // Read the binary file sequence: [keySize][keyData][valSize][valData]
    while (fread(&keyLen, sizeof(size_t), 1, f) == 1) {
        // Validate key length against defined limits to prevent heap corruption
        if (keyLen == 0 || keyLen > MAX_KEY_LEN) {
            fprintf(stderr, "ht_load: invalid key length %zu, aborting\n", keyLen);
            break;
        }
        
        key = malloc(keyLen);
        if (!key) break;

        // Ensure we can read exactly keyLen bytes
        if (fread(key, 1, keyLen, f) != keyLen) goto clean;
        
        if (fread(&valLen, sizeof(size_t), 1, f) != 1) goto clean;
        if (valLen == 0 || valLen > MAX_VALUE_SIZE) {
            fprintf(stderr, "ht_load: invalid value length %zu, aborting\n", valLen);
            goto clean;
        }

        char *val = malloc(valLen);
        if (!val) goto clean;

        if (fread(val, 1, valLen, f) == valLen) {
            // Re-insert into the table (this handles rehashing internally)
            ht_set(table, key, keyLen, val, valLen);
        }

        free(val);
        free(key);
        key = NULL;
    }

clean:
    if (key) free(key); // Cleanup in case of jump from 'goto'
    fclose(f);
    return 1;
}

void ht_foreach(Hash_Table *table, ht_foreach_cb callback, void *userdata) {
    if (!table || !callback) return;

    // Use a read lock to ensure the table structure is stable during traversal
    pthread_rwlock_rdlock(&table->lock);
    for (size_t i = 0; i < table->capacity; i++) {
        for (Entry *e = table->pool[i]; e; e = e->next) {
            // Trigger the user callback for each valid key-value pair
            callback(e->key, e->keySize, e->value, e->size, userdata);
        }
    }
    pthread_rwlock_unlock(&table->lock);
}


/* ── Static helpers ──────────────────────────────────────────────────── */

/**
 * Internal helper to serialize a single entry to disk.
 */
static inline void save_entry(Entry *e, FILE *f) {
    // Write size then raw binary data for both key and value
    fwrite(&e->keySize, sizeof(size_t), 1, f);
    fwrite(e->key,       e->keySize,    1, f);
    fwrite(&e->size,    sizeof(size_t), 1, f);
    fwrite(e->value,     e->size,       1, f);
}

static Entry *create_entry(void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash) {
    Entry *e = malloc(sizeof(Entry));
    if (!e) return NULL;
    e->key = e->value = NULL;

    // Allocate and copy key
    e->key = malloc(keySize);
    if (!e->key) goto error;
    memcpy(e->key, key, keySize);
    e->keySize = keySize;

    // Allocate and copy value
    e->value = malloc(valueSize);
    if (!e->value) goto error;
    memcpy(e->value, value, valueSize);
    
    e->size = valueSize;
    e->hash = hash; // Cache hash for fast resizing
    e->next = NULL;
    return e;

error:
    // Rollback allocations if any step fails
    free(e->key);
    free(e->value);
    free(e);
    return NULL;
}

/**
 * Checks if a number is prime using the 6k +/- 1 optimization.
 * This is used to maintain good distribution in the hash table.
 */
static int is_prime(size_t n) {
    if (n < 2) return 0;
    if (n == 2 || n == 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    
    // Check divisors up to sqrt(n)
    for (size_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

static size_t next_prime(size_t n) {
    if (n < 2) return 2;
    if (n % 2 == 0) n++; // Start from odd number
    while (!is_prime(n)) n += 2;
    return n;
}