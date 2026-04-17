#ifndef DB_H
#define DB_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdarg.h>

// Inizializza il database (crea/ apre il file)
int db_init(const char *path);

// Esegue una query senza risultati (CREATE, INSERT, UPDATE, DELETE)
// sql: query con placeholder '?'
// ...: valori da bindare (supporta: int, int64, double, string, NULL)
// Esempio: db_execute("INSERT INTO users VALUES (?,?)", 1, "pippo");
int db_execute(const char *sql, ...);

// Esegue una query che restituisce righe, chiama row_callback per ogni riga.
// row_callback: funzione utente che riceve sqlite3_stmt* e il userdata.
// Ritorna il numero di righe processate o -1 in caso di errore.
typedef int (*db_row_callback)(sqlite3_stmt *stmt, void *userdata);
int db_query(const char *sql, db_row_callback callback, void *userdata, ...);

// Prepara una statement, la esegue e chiama il callback per ogni riga.
// Versione con va_list già inizializzata (per uso interno).
int db_query_va(const char *sql, db_row_callback callback, void *userdata, va_list args);

// Chiude il database
void db_close(void);

// Ultimo errore (per logging)
const char *db_errmsg(void);

#endif