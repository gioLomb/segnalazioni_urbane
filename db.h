/**
 * @file db.h
 * @brief High-level wrapper for SQLite3 database operations.
 *
 * This module provides a simplified interface for executing SQL queries
 * and iterating through result sets using a cursor-based approach.
 * It supports automated parameter binding using format strings.
 *
 * Format string (fmt) conventions:
 * - 's' : const char * (string)
 * - 'i' : int
 * - 'l' : int64_t
 * - 'f' : double
 * - 'n' : NULL
 */

#ifndef DB_H
#define DB_H

#include <stdint.h>
#include <stdbool.h>
#include <sqlite3.h>
#include "config.h"

/* ── Cursor types ────────────────────────────────────────────────────── */

/**
 * @brief Opaque structure representing a database result set cursor.
 */
typedef struct DbCursor DbCursor;

const char *db_cursor_text  (DbCursor *c, int col);
int64_t     db_cursor_int64 (DbCursor *c, int col);
double      db_cursor_double(DbCursor *c, int col);

/* ── Generic cursor getter ───────────────────────────────────────────── */

/**
 * @brief Type-generic macro to read a column from the current cursor row.
 *
 * Dispatches to db_cursor_int64 / db_cursor_double / db_cursor_text
 * based on the declared type of out_var.
 *
 * For enum fields, read into a plain int then cast:
 *   int tmp; db_cursor_get(c, col, tmp); field = (MyEnum)tmp;
 *
 * Supported types:
 *   int, long, long long, unsigned int, unsigned long, unsigned long long,
 *   int64_t (= long / long long depending on platform), double,
 *   const char *, char *.
 *
 * Example:
 *   int64_t id;    db_cursor_get(c, 0, id);
 *   double  lat;   db_cursor_get(c, 1, lat);
 *   const char *s; db_cursor_get(c, 2, s);
 */
#define db_cursor_get(cursor, col, out_var) _Generic((out_var),         \
    int:                (out_var) = (int)               db_cursor_int64 (cursor, col), \
    long:               (out_var) = (long)              db_cursor_int64 (cursor, col), \
    long long:          (out_var) = (long long)         db_cursor_int64 (cursor, col), \
    unsigned int:       (out_var) = (unsigned int)      db_cursor_int64 (cursor, col), \
    unsigned long:      (out_var) = (unsigned long)     db_cursor_int64 (cursor, col), \
    unsigned long long: (out_var) = (unsigned long long)db_cursor_int64 (cursor, col), \
    double:             (out_var) =                     db_cursor_double(cursor, col), \
    const char *:       (out_var) =                     db_cursor_text  (cursor, col), \
    char *:             (out_var) = (char *)            db_cursor_text  (cursor, col), \
    default:            (void)0                                                         \
)

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Opens or creates a database file at the specified path.
 * @pre path is a valid file system path.
 * @post If successful, a global database connection is established.
 * @param path Path to the .db file.
 * @return 0 on success, -1 if the database cannot be opened.
 */
int db_init(const char *path);

/**
 * @brief Closes the global database connection.
 * @post The database handle is safely closed and set to NULL.
 */
void db_close(void);

/**
 * @brief Retrieves the last error message from the database engine.
 * @return A string containing the last SQLite error.
 */
const char *db_errmsg(void);

/**
 * @brief Returns the ID of the last row inserted via an INSERT statement.
 * @return The 64-bit row ID.
 */
int64_t db_last_insert_id(void);

/**
 * @brief Returns the number of rows modified by the last executed statement.
 * @return Number of modified rows.
 */
int db_changes(void);

/* ── Cursor API (SELECT) ─────────────────────────────────────────────── */

/**
 * @brief Executes a SELECT query and returns a cursor to iterate rows.
 * @pre Database is initialized. sql contains placeholders '?'.
 * @pre fmt matches the number of placeholders and types of trailing arguments.
 * @param sql The SQL statement string.
 * @param fmt Format string for parameters (e.g., "si" for string, int).
 * @param ... Arguments matching the format string.
 * @return A pointer to a DbCursor, or NULL on error.
 */
DbCursor *db_cursor_open(const char *sql, const char *fmt, ...);

/**
 * @brief Advances the cursor to the next row in the result set.
 * @param c Pointer to the cursor.
 * @return true if a new row is available, false if EOF or error.
 */
bool db_cursor_next(DbCursor *c);

/**
 * @brief Retrieves a string value from a specific column of the current row.
 * @param c Pointer to the cursor.
 * @param col Zero-based column index.
 * @return The string content, or an empty string "" if the value is NULL.
 */
const char *db_cursor_text(DbCursor *c, int col);

/**
 * @brief Retrieves a 64-bit integer value from a column.
 * @param c Pointer to the cursor.
 * @param col Zero-based column index.
 * @return The integer value, or 0 if the value is NULL.
 */
int64_t db_cursor_int64(DbCursor *c, int col);

/**
 * @brief Retrieves a double-precision float value from a column.
 * @param c Pointer to the cursor.
 * @param col Zero-based column index.
 * @return The double value, or 0.0 if the value is NULL.
 */
double db_cursor_double(DbCursor *c, int col);

/**
 * @brief Closes the cursor and releases associated statement memory.
 * @param c Pointer to the cursor to close.
 */
void db_cursor_close(DbCursor *c);

/* ── Execution API (INSERT/UPDATE/DELETE) ────────────────────────────── */

/**
 * @brief Executes a non-query SQL statement (INSERT, UPDATE, DELETE).
 * @pre Database is initialized.
 * @param sql The SQL statement string.
 * @param fmt Format string for parameters.
 * @param ... Arguments matching the format string.
 * @return 0 on success (SQLITE_DONE), -1 on error.
 */
int db_exec(const char *sql, const char *fmt, ...);

#endif /* DB_H */