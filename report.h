#ifndef REPORT_H
#define REPORT

#include <time.h>
#include <stdint.h>
#include "config.h"

#define ID_LEN 100000
#define CAT_LEN 16
#define DESC_LEN 32

typedef enum {
    STATUS_ACTIVE = 0,      // Appena creata
    STATUS_IN_PROGRESS = 1, // Presa in carico dal comune
    STATUS_ARCHIVED = 2     // Risolta (andrà su SQLite)
} ReportStatus;

typedef struct {
    char reportId[ID_LEN];   // Chiave primaria (usata come chiave nella HT)
    char authorId[ID_LEN];   // "Chiave esterna" che punta a User.user_id
    double lat;               // Coordinate per Leaflet
    double lon; 
    char category[CAT_LEN];   // Es: "Rifiuti", "Buca stradale"
    char description[DESC_LEN];
    ReportStatus status;
    time_t createdAt;        // Timestamp di creazione
} ActiveReport;

// Funzione "Costruttore"
ActiveReport* report_create(uint64_t id, uint64_t author, double lat, double lon, 
                            const char* cat, const char* desc);

// Funzione "Distruttore"
void report_destroy(ActiveReport* r);

// Utility
int report_to_json(const ActiveReport* r, char* dest, size_t size);

#endif