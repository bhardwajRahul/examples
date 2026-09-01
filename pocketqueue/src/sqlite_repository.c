/* sqlite_repository.c - SQLite lifecycle (stage 1) + ping.
 *
 * D4: a single connection guarded by an internal mutex. Stage 4 extends
 * this with the full repository surface. Long-poll waits must release
 * the mutex before sleeping on the condvar (spec §15.4, §29).
 */
#include "sqlite_repository.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "str_util.h"

struct pq_repository {
    sqlite3 *db;
    pthread_mutex_t mu;
    char path[1024];
    bool open;
};

static void apply_pragmas(sqlite3 *db)
{
    /* Spec §10.1: WAL for concurrent readers, foreign keys on, busy
     * timeout 5 s, synchronous=NORMAL. */
    char *err = NULL;
    sqlite3_exec(db, "PRAGMA journal_mode = WAL", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }
    sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }
    sqlite3_exec(db, "PRAGMA busy_timeout = 5000", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }
    sqlite3_exec(db, "PRAGMA synchronous = NORMAL", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }
}

pq_repository *pq_repository_open(const char *path, pq_error *err)
{
    if (path == NULL || *path == '\0') {
        pq_error_set(err, "invalid_config", "database path is empty");
        return NULL;
    }
    pq_repository *repo = calloc(1, sizeof(*repo));
    if (repo == NULL) {
        pq_error_set(err, "out_of_memory", "could not allocate repository");
        return NULL;
    }
    pthread_mutex_init(&repo->mu, NULL);
    pq_str_copy(repo->path, sizeof(repo->path), path);

    int rc = sqlite3_open_v2(path, &repo->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "sqlite3_open_v2 failed: %s",
                 repo->db ? sqlite3_errmsg(repo->db) : "(null handle)");
        sqlite3_close(repo->db);
        pthread_mutex_destroy(&repo->mu);
        free(repo);
        pq_error_set(err, "database_error", msg);
        return NULL;
    }
    apply_pragmas(repo->db);
    repo->open = true;
    return repo;
}

void pq_repository_close(pq_repository *repo)
{
    if (repo == NULL) return;
    pthread_mutex_lock(&repo->mu);
    if (repo->open && repo->db != NULL) {
        sqlite3_close(repo->db);
        repo->db = NULL;
        repo->open = false;
    }
    pthread_mutex_unlock(&repo->mu);
    pthread_mutex_destroy(&repo->mu);
    free(repo);
}

void pq_repository_lock(pq_repository *repo)
{
    pthread_mutex_lock(&repo->mu);
}

void pq_repository_unlock(pq_repository *repo)
{
    pthread_mutex_unlock(&repo->mu);
}

pq_status pq_repository_ping_locked(pq_repository *repo)
{
    if (repo == NULL || repo->db == NULL) return PQ_DATABASE_ERROR;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(repo->db, "SELECT 1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return PQ_DATABASE_ERROR;
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? PQ_OK : PQ_DATABASE_ERROR;
}

const char *pq_repository_path(const pq_repository *repo)
{
    return repo ? repo->path : "";
}

void *pq_repository_handle_locked(pq_repository *repo)
{
    if (repo == NULL) return NULL;
    return repo->open ? repo->db : NULL;
}

int64_t pq_repository_schema_version_locked(pq_repository *repo)
{
    sqlite3 *db = (sqlite3 *)pq_repository_handle_locked(repo);
    if (db == NULL) return -1;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT MAX(version) FROM schema_version",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0; /* table missing → no migrations applied yet */
    }
    rc = sqlite3_step(stmt);
    int64_t v = 0;
    if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        v = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return v;
}

pq_status pq_repository_exec_script_locked(pq_repository *repo,
                                           const char *script,
                                           pq_error *err)
{
    sqlite3 *db = (sqlite3 *)pq_repository_handle_locked(repo);
    if (db == NULL) {
        pq_error_set(err, "database_error", "repository is not open");
        return PQ_DATABASE_ERROR;
    }
    char *msg = NULL;
    int rc = sqlite3_exec(db, script, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        char out[PQ_ERROR_MESSAGE_MAX];
        snprintf(out, sizeof(out), "migration failed: %s",
                 msg ? msg : sqlite3_errmsg(db));
        if (msg) sqlite3_free(msg);
        pq_error_set(err, "database_error", out);
        return PQ_DATABASE_ERROR;
    }
    return PQ_OK;
}