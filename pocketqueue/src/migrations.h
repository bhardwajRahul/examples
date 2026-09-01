/* migrations.h - sequential numeric migrations (D5). */
#ifndef PQ_MIGRATIONS_H
#define PQ_MIGRATIONS_H

#include <stdint.h>

#include "pocketqueue/pocketqueue.h"
#include "sqlite_repository.h"

#define PQ_MIGRATIONS_DIR_DEFAULT "migrations"

/* Apply pending migrations under the repository lock. Each migration file
 * is one SQL script; multiple statements separated by ';' are wrapped in a
 * transaction. The directory layout is Vnnn__name.sql, nnn zero-padded.
 *
 * Returns PQ_OK if the schema is at the latest supported version. */
pq_status pq_migrations_apply(pq_repository *repo, const char *dir,
                               int64_t supported_version, pq_error *err);

/* Returns the version currently recorded in schema_version, or 0 if the
 * table doesn't exist yet. Caller must hold the lock. */
int64_t pq_migrations_current_version_locked(pq_repository *repo);

#endif /* PQ_MIGRATIONS_H */