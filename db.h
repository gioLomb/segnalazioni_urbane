#ifndef DB_H
#define DB_H

#include <stdint.h>
#include <stdbool.h>
#include <sqlite3.h>

/* ── Cursore ─────────────────────────────────────────────────────────
 *
 * Itera le righe di una SELECT una alla volta, senza materializzare
 * il result set in memoria.  Uso tipico:
 *
 *   DbCursor *c = db_cursor_open(
 *       "SELECT id, name FROM users WHERE city = ?", "s", city);
 *   while (db_cursor_next(c)) {
 *       int64_t     id   = db_cursor_int64(c, 0);
 *       const char *name = db_cursor_text (c, 1);
 *   }
 *   db_cursor_close(c);
 *
 * fmt descrive i parametri '?' nell'ordine:
 *   's' = const char *     'i' = int
 *   'l' = int64_t          'f' = double     'n' = NULL
 * Passa NULL (o "") se la query non ha parametri.
 * ──────────────────────────────────────────────────────────────────── */

typedef struct DbCursor DbCursor;

DbCursor   *db_cursor_open  (const char *sql, const char *fmt, ...);
bool        db_cursor_next  (DbCursor *c);
const char *db_cursor_text  (DbCursor *c, int col);   /* "" se NULL  */
int64_t     db_cursor_int64 (DbCursor *c, int col);   /*  0 se NULL  */
double      db_cursor_double(DbCursor *c, int col);   /* 0.0 se NULL */
void        db_cursor_close (DbCursor *c);

/* ── Execute ──────────────────────────────────────────────────────────
 *
 * INSERT / UPDATE / DELETE / CREATE — non restituisce righe.
 * Stessa convenzione fmt del cursore.
 * Restituisce 0 su successo, -1 su errore.
 * ──────────────────────────────────────────────────────────────────── */

int db_exec(const char *sql, const char *fmt, ...);

/* ── Lifecycle & utilità ─────────────────────────────────────────────*/

int         db_init          (const char *path);
void        db_close         (void);
const char *db_errmsg        (void);
int64_t     db_last_insert_id(void);
int         db_changes       (void);

#endif /* DB_H */