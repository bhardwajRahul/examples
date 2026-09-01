/* sqlite_repository.h - SQLite lifecycle + helpers.
 *
 * Stage 1 owns only the open/close path and a "ping" for /readyz.
 * The full repository layer arrives in stage 4.
 */
#ifndef PQ_SQLITE_REPOSITORY_H
#define PQ_SQLITE_REPOSITORY_H

#include <stdbool.h>

#include "pocketqueue/pocketqueue.h"

typedef struct pq_repository pq_repository;

/* Open (or create) the database at path with the spec §10.1 pragmas.
 * single_connection_mode forces the mutex-protected single-connection
 * implementation (D4). */
pq_repository *pq_repository_open(const char *path, pq_error *err);
void pq_repository_close(pq_repository *repo);

/* Lock the global mutex. Long-poll waits MUST release this. */
void pq_repository_lock(pq_repository *repo);
void pq_repository_unlock(pq_repository *repo);

/* Lightweight ping used by /readyz. Caller must hold the lock. */
pq_status pq_repository_ping_locked(pq_repository *repo);

/* Returns the path the repository was opened with. */
const char *pq_repository_path(const pq_repository *repo);

/* Schema version helpers used by the migrations module. Both require the
 * lock to be held. Returns 0 if the schema_version table does not yet
 * exist (i.e. no migrations have run). */
int64_t pq_repository_schema_version_locked(pq_repository *repo);

/* Execute a SQL script. Used by the migrations module. Each script runs
 * inside a single transaction. Returns PQ_OK on success. */
pq_status pq_repository_exec_script_locked(pq_repository *repo,
                                           const char *script,
                                           pq_error *err);

/* INTERNAL: returns the underlying sqlite3* handle. The lock must be held.
 * Exposed so the migrations module can prepare versioned statements. */
void *pq_repository_handle_locked(pq_repository *repo);

#endif /* PQ_SQLITE_REPOSITORY_H */