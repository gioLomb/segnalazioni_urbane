#ifndef REPORT_H
#define REPORT_H

#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define CITY_LEN 32
#define CAT_LEN 16
#define DESC_LEN 32
#define REPORT_ID_LEN 16   // UUID binario a 16 byte (opzionale)

typedef enum {
    STATUS_ACTIVE = 0,
    STATUS_IN_PROGRESS = 1,
    STATUS_RESOLVED = 2      // quando viene archiviata
} ReportStatus;

typedef struct {
    uint64_t reportId;               // chiave primaria (incrementale o random)
    uint64_t authorId;               // userId del cittadino
    double lat;
    double lon;
    char city[CITY_LEN];
    char category[CAT_LEN];
    char description[DESC_LEN];
    ReportStatus status;
    time_t createdAt;
} ActiveReport;

// Costruttore/distruttore
ActiveReport* report_create(uint64_t id, uint64_t author, double lat, double lon,
                            const char* cat, const char* desc);
void report_destroy(ActiveReport* r);

// Utility per JSON (usato negli endpoint)
bool report_to_json(const ActiveReport *r, char *dest, size_t destSize);

#endif