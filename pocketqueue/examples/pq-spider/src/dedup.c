/* dedup.c - persistent dedup store.
 *
 * Strategy: hash table over fixed-size buckets, with each URL stored as
 * a node. Loads the whole file into memory on open. Appends new URLs
 * to a side file so a crash doesn't lose progress. Marking status is
 * an in-memory update + rewrite on close.
 */
#include "dedup.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "url_norm.h"

#define BUCKET_COUNT 16384
#define MAX_LINE    (URL_MAX + 8)

typedef struct entry {
    char *url;
    char status;                 /* 'v' / 'f' / 'p' or 0 if unknown */
    struct entry *next;          /* hash bucket chain */
} entry;

struct dedup_store {
    pthread_mutex_t mu;
    char *path;                  /* backing file path */
    FILE *append_fp;             /* append-only writer */
    entry *buckets[BUCKET_COUNT];
    size_t total;
};

static unsigned long hash_str(const char *s)
{
    unsigned long h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

dedup_store *dedup_open(const char *path)
{
    dedup_store *s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    pthread_mutex_init(&s->mu, NULL);
    s->path = strdup(path);

    FILE *fp = fopen(path, "a+");
    if (fp == NULL) {
        fprintf(stderr, "dedup: cannot open %s: %s\n", path, strerror(errno));
        free(s->path);
        free(s);
        return NULL;
    }
    /* Re-read from the start. */
    rewind(fp);
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        char *tab = strchr(line, '\t');
        char status = 'p';
        char *url = line;
        if (tab != NULL) {
            *tab = '\0';
            status = tab[1];
            url = line;
        }
        /* Insert into hash table. */
        entry *e = calloc(1, sizeof(*e));
        if (e == NULL) break;
        e->url = strdup(url);
        if (e->url == NULL) { free(e); break; }
        e->status = status;
        unsigned long h = hash_str(url) % BUCKET_COUNT;
        e->next = s->buckets[h];
        s->buckets[h] = e;
        s->total++;
    }
    /* Re-open in append mode for new entries. */
    fclose(fp);
    s->append_fp = fopen(path, "a");
    if (s->append_fp == NULL) {
        fprintf(stderr, "dedup: cannot reopen for append: %s\n",
                strerror(errno));
    }
    return s;
}

void dedup_close(dedup_store *s)
{
    if (s == NULL) return;
    if (s->append_fp) fclose(s->append_fp);

    /* Rewrite the file atomically so all status updates are persisted. */
    char tmp[URL_MAX + 256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);
    FILE *out = fopen(tmp, "w");
    if (out != NULL) {
        for (int i = 0; i < BUCKET_COUNT; i++) {
            for (entry *e = s->buckets[i]; e != NULL; e = e->next) {
                fprintf(out, "%s\t%c\n", e->url, e->status ? e->status : 'p');
            }
        }
        fclose(out);
        rename(tmp, s->path);
    }

    for (int i = 0; i < BUCKET_COUNT; i++) {
        entry *e = s->buckets[i];
        while (e != NULL) {
            entry *next = e->next;
            free(e->url);
            free(e);
            e = next;
        }
    }
    pthread_mutex_destroy(&s->mu);
    free(s->path);
    free(s);
}

bool dedup_contains(const dedup_store *s, const char *url)
{
    if (s == NULL || url == NULL) return false;
    /* dedup_contains doesn't lock — safe because entries are immutable
     * after insertion (status changes happen under the mutex but we
     * tolerate a stale status during reads). */
    unsigned long h = hash_str(url) % BUCKET_COUNT;
    for (entry *e = s->buckets[h]; e != NULL; e = e->next) {
        if (strcmp(e->url, url) == 0) return true;
    }
    return false;
}

bool dedup_add(dedup_store *s, const char *url)
{
    if (s == NULL || url == NULL) return false;
    pthread_mutex_lock(&s->mu);
    if (dedup_contains(s, url)) {
        pthread_mutex_unlock(&s->mu);
        return false;
    }
    entry *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        pthread_mutex_unlock(&s->mu);
        return false;
    }
    e->url = strdup(url);
    if (e->url == NULL) {
        free(e);
        pthread_mutex_unlock(&s->mu);
        return false;
    }
    e->status = 'p';
    unsigned long h = hash_str(url) % BUCKET_COUNT;
    e->next = s->buckets[h];
    s->buckets[h] = e;
    s->total++;
    if (s->append_fp != NULL) {
        fprintf(s->append_fp, "%s\tp\n", url);
        fflush(s->append_fp);
    }
    pthread_mutex_unlock(&s->mu);
    return true;
}

void dedup_mark(dedup_store *s, const char *url, char status)
{
    if (s == NULL || url == NULL) return;
    pthread_mutex_lock(&s->mu);
    unsigned long h = hash_str(url) % BUCKET_COUNT;
    for (entry *e = s->buckets[h]; e != NULL; e = e->next) {
        if (strcmp(e->url, url) == 0) {
            e->status = status;
            break;
        }
    }
    pthread_mutex_unlock(&s->mu);
}

size_t dedup_size(const dedup_store *s)
{
    return s ? s->total : 0;
}
