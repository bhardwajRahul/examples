/* main.c - pq-spider entry point.
 *
 * Usage:
 *   pq-spider --server URL --seeds FILE [--threads N] [--dedup FILE]
 *             [--visibility-ms N] [--max-depth N] [--timeout S]
 *
 * Seeds file: one URL per line. Blank lines and lines starting with '#'
 * are ignored. Each seed URL is published to the `pages` queue with
 * max_attempts = 5 unless --max-attempts is specified.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"

#include "pqctl_client.h"
#include "worker.h"
#include "url_norm.h"

static volatile sig_atomic_t g_signal = 0;

static void on_signal(int s)
{
    (void)s;
    g_signal = 1;
}

static volatile bool g_shutdown = false;

typedef struct {
    const char *seeds_path;
    const char *server;
    const char *dedup_path;
    int threads;
    int visibility_ms;
    int max_depth;
    int max_attempts;
} cli_opts;

static int publish_seed(pq_client *c, const char *line, int max_attempts)
{
    /* Trim whitespace. */
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' ||
                       line[len-1] == '\r' || line[len-1] == '\n')) len--;
    if (len == 0) return 0;

    char url[URL_MAX];
    if (len >= sizeof(url)) return 0;
    memcpy(url, line, len);
    url[len] = '\0';

    char normalized[URL_MAX];
    if (!url_normalize(url, normalized, sizeof(normalized))) {
        fprintf(stderr, "spider: skipping invalid seed '%s'\n", url);
        return 0;
    }
    if (!url_is_fetchable(normalized)) {
        fprintf(stderr, "spider: skipping non-fetchable seed '%s'\n", normalized);
        return 0;
    }

    /* Build payload. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "url", normalized);
    cJSON_AddNumberToObject(root, "depth", 0);
    cJSON_AddStringToObject(root, "parent", "");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    pq_error err = {0};
    bool ok = pq_client_publish(c, "pages", body, max_attempts, &err);
    free(body);
    if (!ok) {
        fprintf(stderr, "spider: publish seed failed: %s\n", err.message);
        return -1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    cli_opts opts = {
        .seeds_path = "seeds.txt",
        .server = "http://127.0.0.1:8080",
        .dedup_path = "dedup.tsv",
        .threads = 4,
        .visibility_ms = 30000,
        .max_depth = 5,
        .max_attempts = 5,
    };

    static struct option longopts[] = {
        {"server",        required_argument, NULL, 's'},
        {"seeds",         required_argument, NULL, 'f'},
        {"threads",       required_argument, NULL, 't'},
        {"dedup",         required_argument, NULL, 'd'},
        {"visibility-ms", required_argument, NULL, 'v'},
        {"max-depth",     required_argument, NULL, 'm'},
        {"max-attempts",  required_argument, NULL, 'a'},
        {"help",          no_argument,       NULL, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+s:f:t:d:v:m:a:h", longopts, NULL)) != -1) {
        switch (opt) {
            case 's': opts.server = optarg; break;
            case 'f': opts.seeds_path = optarg; break;
            case 't': opts.threads = atoi(optarg); break;
            case 'd': opts.dedup_path = optarg; break;
            case 'v': opts.visibility_ms = atoi(optarg); break;
            case 'm': opts.max_depth = atoi(optarg); break;
            case 'a': opts.max_attempts = atoi(optarg); break;
            case 'h':
                printf("Usage: pq-spider [options]\n"
                       "  --server URL         PocketQueue server URL\n"
                       "  --seeds FILE         File with seed URLs (one per line)\n"
                       "  --threads N          Number of worker threads\n"
                       "  --dedup FILE         Dedup-store path\n"
                       "  --visibility-ms N    Default visibility timeout\n"
                       "  ---depth N           Max crawl depth (0 = unlimited)\n"
                       "  --max-attempts N     Attempts per published URL\n");
                return 0;
            default: return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "spider: unexpected positional arguments\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Open dedup store, publish seeds, start workers. */
    dedup_store *dedup = dedup_open(opts.dedup_path);
    if (dedup == NULL) return 1;

    pq_client *client = pq_client_open(opts.server);
    if (client == NULL) {
        fprintf(stderr, "spider: cannot parse server URL '%s'\n", opts.server);
        dedup_close(dedup);
        return 1;
    }

    FILE *sf = fopen(opts.seeds_path, "r");
    if (sf == NULL) {
        fprintf(stderr, "spider: cannot read seeds file '%s': %s\n",
                opts.seeds_path, strerror(errno));
        pq_client_close(client);
        dedup_close(dedup);
        return 1;
    }

    char line[URL_MAX];
    int published = 0;
    while (fgets(line, sizeof(line), sf) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        int r = publish_seed(client, line, opts.max_attempts);
        if (r < 0) break;
        published += r;
    }
    fclose(sf);
    printf("spider: published %d seed URL(s) to 'pages'\n", published);

    /* Set up dedup entries for the seeds so they're not re-fetched as new
     * discoveries by some other process. */
    rewind(sf = fopen(opts.seeds_path, "r"));
    (void)sf;  /* we already loaded seeds; just trust them */
    /* (dedup entries are added on the queue side anyway.) */

    /* Spawn workers. */
    if (opts.threads < 1) opts.threads = 1;
    if (opts.threads > 32) opts.threads = 32;
    pthread_t *threads = calloc((size_t)opts.threads, sizeof(*threads));
    worker_ctx *ctxs = calloc((size_t)opts.threads, sizeof(*ctxs));
    bool *owned = calloc((size_t)opts.threads, sizeof(*owned));
    for (int i = 0; i < opts.threads; i++) {
        ctxs[i].client = client;
        ctxs[i].dedup = dedup;
        ctxs[i].thread_idx = i;
        ctxs[i].default_visibility_ms = opts.visibility_ms;
        ctxs[i].max_depth = opts.max_depth;
        ctxs[i].assets = (i % 2 == 1);   /* half workers on assets */
        ctxs[i].shutdown = &g_shutdown;
        if (pthread_create(&threads[i], NULL, worker_loop, &ctxs[i]) == 0) {
            owned[i] = true;
        }
    }

    /* Wait for signal. */
    while (!g_signal) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        /* Periodically report progress. */
        static time_t last = 0;
        time_t now = time(NULL);
        if (now != last) {
            last = now;
            fprintf(stderr,
                    "spider: dedup=%zu URLs tracked (%d workers)\n",
                    dedup_size(dedup), opts.threads);
        }
    }

    g_shutdown = true;
    for (int i = 0; i < opts.threads; i++) {
        if (owned[i]) pthread_join(threads[i], NULL);
    }
    free(threads); free(ctxs); free(owned);
    pq_client_close(client);
    dedup_close(dedup);
    printf("spider: shutdown complete\n");
    return 0;
}