#include "report.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

ActiveReport* report_create(uint64_t id, uint64_t author, double lat, double lon,
                            const char* cat, const char* desc) {
    ActiveReport *r = malloc(sizeof(ActiveReport));
    if (!r) return NULL;

    r->reportId = id;
    r->authorId = author;
    r->lat = lat;
    r->lon = lon;
    r->status = STATUS_ACTIVE;
    r->createdAt = time(NULL);

    strncpy(r->category, cat, CAT_LEN - 1);
    r->category[CAT_LEN - 1] = '\0';

    strncpy(r->description, desc, DESC_LEN - 1);
    r->description[DESC_LEN - 1] = '\0';

    return r;
}

void report_destroy(ActiveReport* r) {
    free(r);
}

bool report_to_json(const ActiveReport *r, char *dest, size_t destSize) {
    if (!r || !dest || destSize == 0) return false;

    snprintf(dest, destSize,
        "{"
        "\"id\":%lu,"
        "\"author\":%lu,"
        "\"lat\":%f,"
        "\"lon\":%f,"
        "\"category\":\"%s\","
        "\"description\":\"%s\","
        "\"status\":%d,"
        "\"created_at\":%ld"
        "}",
        r->reportId, r->authorId, r->lat, r->lon,
        r->category, r->description, r->status, r->createdAt);
    return true;
}
