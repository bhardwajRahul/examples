/* migrations.c - sequential numeric migrations (D5).
 *
 * Layout: <dir>/Vnnn__name.sql, nnn zero-padded to 3 digits. Files are
 * sorted by version. Each script runs in its own transaction; on failure
 * the database is left unchanged.
 */
#include "migrations.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sqlite_repository.h"
#include "str_util.h"

typedef struct {
    int64_t version;
    char name[256];
} migration_entry;

static int compare_migrations(const void *a, const void *b)
{
    int64_t va = ((const migration_entry *)a)->version;
    int64_t vb = ((const migration_entry *)b)->version;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static bool parse_filename(const char *fname, int64_t *out_version)
{
    /* Expected: Vnnn__<name>.sql */
    if (fname[0] != 'V') return false;
    int n = 0;
    for (int i = 1; i < 4; i++) {
        if (fname[i] < '0' || fname[i] > '9') return false;
        n = n * 10 + (fname[i] - '0');
    }
    if (fname[4] != '_' || fname[5] != '_') return false;
    if (strcmp(fname + strlen(fname) - 4, ".sql") != 0) return false;
    *out_version = n;
    return true;
}

static char *slurp(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

int64_t pq_migrations_current_version_locked(pq_repository *repo)
{
    return pq_repository_schema_version_locked(repo);
}

pq_status pq_migrations_apply(pq_repository *repo, const char *dir,
                              int64_t supported_version, pq_error *err)
{
    if (repo == NULL || dir == NULL) {
        pq_error_set(err, "internal_error", "migrations: null argument");
        return PQ_INTERNAL_ERROR;
    }

    DIR *d = opendir(dir);
    if (d == NULL) {
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "cannot open migrations directory '%s': %s",
                 dir, strerror(errno));
        pq_error_set(err, "internal_error", msg);
        return PQ_INTERNAL_ERROR;
    }

    migration_entry entries[64];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < 64) {
        int64_t v;
        if (parse_filename(de->d_name, &v)) {
            entries[count].version = v;
            pq_str_copy(entries[count].name, sizeof(entries[count].name),
                        de->d_name);
            count++;
        }
    }
    closedir(d);

    qsort(entries, (size_t)count, sizeof(migration_entry), compare_migrations);

    pq_repository_lock(repo);
    int64_t current = pq_repository_schema_version_locked(repo);
    pq_repository_unlock(repo);

    if (current > supported_version) {
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg),
                 "database schema version %lld is newer than supported %lld",
                 (long long)current, (long long)supported_version);
        pq_error_set(err, "database_error", msg);
        return PQ_DATABASE_ERROR;
    }

    /* Apply pending migrations one at a time, each in its own
     * transaction so partial failures leave earlier migrations applied. */
    for (int i = 0; i < count; i++) {
        if (entries[i].version <= current) continue;
        if (entries[i].version > supported_version) {
            char msg[PQ_ERROR_MESSAGE_MAX];
            snprintf(msg, sizeof(msg),
                     "migration file %s exceeds supported version %lld",
                     entries[i].name, (long long)supported_version);
            pq_error_set(err, "database_error", msg);
            return PQ_DATABASE_ERROR;
        }
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir, entries[i].name);
        size_t script_len = 0;
        char *script = slurp(path, &script_len);
        if (script == NULL) {
            char msg[PQ_ERROR_MESSAGE_MAX];
            snprintf(msg, sizeof(msg), "cannot read %s: %s",
                     path, strerror(errno));
            pq_error_set(err, "internal_error", msg);
            return PQ_INTERNAL_ERROR;
        }
        pq_repository_lock(repo);
        char begin[] = "BEGIN;";
        char commit[] = "COMMIT;";
        pq_status s = pq_repository_exec_script_locked(repo, begin, err);
        if (s == PQ_OK) s = pq_repository_exec_script_locked(repo, script, err);
        if (s == PQ_OK) s = pq_repository_exec_script_locked(repo, commit, err);
        if (s != PQ_OK) {
            char rb[] = "ROLLBACK;";
            (void)pq_repository_exec_script_locked(repo, rb, NULL);
            pq_repository_unlock(repo);
            free(script);
            return s;
        }
        pq_repository_unlock(repo);
        free(script);
    }

    return PQ_OK;
}