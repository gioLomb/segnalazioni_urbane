#ifndef REPORT_H
#define REPORT_H

#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include "hash_table.h"

#define CITY_LEN 32
#define CAT_LEN 16
#define DESC_LEN 128

typedef enum {
    STATUS_ACTIVE = 0,
    STATUS_IN_PROGRESS = 1,
    STATUS_RESOLVED = 2      
} ReportStatus;

typedef struct {
    uint64_t reportId;
    uint64_t authorId;
    double lat;
    double lon;
    char city[CITY_LEN];
    char category[CAT_LEN];
    char description[DESC_LEN];
    ReportStatus status;
    time_t createdAt;
} ActiveReport;

// Setup Tabella DB
int report_setup_table();

// Costruttore/Distruttore
ActiveReport* report_create(uint64_t id, uint64_t author, double lat, double lon, const char* city, const char* cat, const char* desc);
void report_destroy(ActiveReport* r);

// RAM: Funzioni per Hash Table (Report Attivi)
// Ritorna una stringa JSON contenente l'array di report filtrati
char* report_get_active_filtered(Hash_Table *ht, uint64_t userId, const char* city, bool isOperator);

// DB: Funzioni per SQLite (Report Inattivi/Archiviati)
int report_archive_to_db(ActiveReport *r);
char* report_get_archived_filtered(uint64_t userId, const char* city, bool isOperator);

// Utility
bool report_to_json(const ActiveReport *r, char *dest, size_t destSize);

#endif