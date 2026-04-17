#include "report.h"
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int report_setup_table() {
    const char *sql = "CREATE TABLE IF NOT EXISTS archived_reports ("
                      "id INTEGER PRIMARY KEY,"
                      "author_id INTEGER,"
                      "lat REAL, lon REAL,"
                      "city TEXT, category TEXT, description TEXT,"
                      "status INTEGER, created_at INTEGER);";
    return db_execute(sql, 0);
}

ActiveReport* report_create(uint64_t id, uint64_t author, double lat, double lon, const char* city, const char* cat, const char* desc) {
    ActiveReport *r = malloc(sizeof(ActiveReport));
    if (!r) return NULL;
    r->reportId = id;
    r->authorId = author;
    r->lat = lat;
    r->lon = lon;
    r->status = STATUS_ACTIVE;
    r->createdAt = time(NULL);
    strncpy(r->city, city, CITY_LEN - 1);
    strncpy(r->category, cat, CAT_LEN - 1);
    strncpy(r->description, desc, DESC_LEN - 1);
    return r;
}

// --- LOGICA DATABASE (ARCHIVIATI) ---

static int report_list_callback(sqlite3_stmt *stmt, void *userdata) {
    char **json_ptr = (char **)userdata;
    // Qui andrebbe costruita la stringa JSON riga per riga concatenando i risultati
    // Per brevità in questo esempio, supponiamo di stampare o accumulare in un buffer
    return 1; 
}

char* report_get_archived_filtered(uint64_t userId, const char* city, bool isOperator) {
    char sql[512];
    if (isOperator) {
        // Operatore: vede tutti i risolti della sua città
        sprintf(sql, "SELECT * FROM archived_reports WHERE city = '%s'", city);
    } else {
        // Cittadino: vede solo i suoi report risolti
        sprintf(sql, "SELECT * FROM archived_reports WHERE author_id = %lu", userId);
    }
    
    // Esecuzione db_query... (richiede gestione dinamica della stringa JSON)
    return NULL; // Ritornerà il puntatore alla stringa JSON allocata
}

// --- LOGICA HASH TABLE (ATTIVI) ---

char* report_get_active_filtered(Hash_Table *ht, uint64_t userId, const char* city, bool isOperator) {
    // 1. Alloca un buffer per il JSON
    char *result = malloc(8192); 
    strcpy(result, "[");

    // 2. Supponendo che la tua hash_table.h permetta l'iterazione:
    // Bisogna scorrere i report e verificare:
    // if (isOperator && strcmp(r->city, city) == 0) -> aggiungi
    // if (!isOperator && r->authorId == userId) -> aggiungi

    // NOTA: Se la tua HT non ha un iteratore, dovrai implementare una funzione 
    // 'ht_get_all' che restituisce tutti i nodi.

    strcat(result, "]");
    return result;
}

int report_archive_to_db(ActiveReport *r) {
    const char *sql = "INSERT INTO archived_reports (id, author_id, lat, lon, city, category, description, status, created_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    return db_execute(sql, 2, (sqlite3_int64)r->reportId, 2, (sqlite3_int64)r->authorId, 
                      3, r->lat, 3, r->lon, 4, r->city, 4, r->category, 4, r->description, 
                      1, (int)STATUS_RESOLVED, 2, (sqlite3_int64)r->createdAt, 0);
}

void report_destroy(ActiveReport* r) {
    free(r);
}