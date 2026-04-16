#include "report.h"

ActiveReport* report_create(uint64_t id, uint64_t author, double lat, double lon, 
                            const char* cat, const char* desc) {
    
    ActiveReport* newReport = (ActiveReport*)malloc(sizeof(ActiveReport));
    if (!newReport) return NULL;

    newReport->reportId = id;
    newReport->authorId = author;
    newReport->lat = lat;
    newReport->lon = lon;
    newReport->status = STATUS_ACTIVE;
    newReport->createdAt = time(NULL);

    // Copia sicura delle stringhe
    strncpy(newReport->category, cat, CAT_LEN - 1);
    newReport->category[CAT_LEN - 1] = '\0';
    
    strncpy(newReport->description, desc, DESC_LEN - 1);
    newReport->description[DESC_LEN - 1] = '\0';

    return newReport;
}

void report_destroy(ActiveReport* report) {
    if (report) {
        free(report);
    }
}