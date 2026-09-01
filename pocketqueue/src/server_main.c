/* server_main.c - pocketqueue-server entry point.
 *
 * Wires config + logger + clock + pq_service + sqlite_repository + HTTP
 * dispatcher + shutdown. The HTTP handler dispatches spec §14-§22 onto
 * pq_service (which holds in-memory state in stage 2/3, swaps to the
 * SQLite-backed implementation in stage 4).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pocketqueue/pocketqueue.h"
#include "pocketqueue/service.h"

#include "clock.h"
#include "config.h"
#include "http_routes.h"
#include "http_server.h"
#include "logger.h"
#include "migrations.h"
#include "sqlite_repository.h"
#include "str_util.h"

#define PQ_SUPPORTED_SCHEMA_VERSION 2

typedef struct server_ctx {
    pq_repository *repo;
    pq_service *service;
    pq_http_routes_ctx routes_ctx;
} server_ctx;

/* Per-thread cache of the request id, used by access logs. */
static volatile sig_atomic_t g_signalled = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_signalled = 1;
}

static pq_http_response handle_request(void *ctx, const pq_http_request *req)
{
    server_ctx *sc = (server_ctx *)ctx;

    /* Health endpoints live here; the queue API is dispatched via
     * pq_http_routes_dispatch. Use heap-allocated responses so the
     * generic dispose path can safely free them. */
    if (strcmp(req->path, "/healthz") == 0 &&
        strcmp(req->method, "GET") == 0) {
        return pq_text_response(200, "application/json",
                                "{\"status\":\"ok\"}");
    }
    if (strcmp(req->path, "/readyz") == 0 &&
        strcmp(req->method, "GET") == 0) {
        pq_repository_lock(sc->repo);
        pq_status s = pq_repository_ping_locked(sc->repo);
        pq_repository_unlock(sc->repo);
        if (s == PQ_OK) {
            return pq_text_response(200, "application/json",
                                    "{\"status\":\"ready\"}");
        }
        return pq_text_response(503, "application/json",
                                "{\"status\":\"not_ready\"}");
    }
    return pq_http_routes_dispatch(&sc->routes_ctx, req);
}

static bool ensure_parent_dir(const char *path, pq_error *err)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) return true;
    size_t dir_len = (size_t)(slash - path);
    if (dir_len == 0) return true;
    char dir[1024];
    if (dir_len >= sizeof(dir)) {
        pq_error_set(err, "invalid_config", "database path too long");
        return false;
    }
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "cannot create directory '%s': %s",
                 dir, strerror(errno));
        pq_error_set(err, "invalid_config", msg);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    pq_config cfg;
    pq_error err = {0};
    if (pq_config_parse(&cfg, &err, argc, argv) != PQ_OK) {
        fprintf(stderr, "pocketqueue-server: configuration error: %s\n",
                err.message);
        return 2;
    }
    pq_logger_init(cfg.log_level, cfg.log_format);
    PQ_LOG_INFO("pocketqueue-server %s starting", "1.0.0");
    PQ_LOG_INFO("bind=%s port=%u database=%s",
                cfg.bind_address, cfg.port, cfg.database_path);

    if (!ensure_parent_dir(cfg.database_path, &err)) {
        PQ_LOG_ERROR("config: %s", err.message);
        return 2;
    }

    pq_repository *repo = pq_repository_open(cfg.database_path, &err);
    if (repo == NULL) {
        PQ_LOG_ERROR("open database: %s", err.message);
        return 1;
    }
    if (pq_migrations_apply(repo, cfg.migrations_dir,
                            PQ_SUPPORTED_SCHEMA_VERSION, &err) != PQ_OK) {
        PQ_LOG_ERROR("migrations: %s", err.message);
        pq_repository_close(repo);
        return 1;
    }
    PQ_LOG_INFO("schema version: %lld",
                (long long)pq_migrations_current_version_locked(repo));

    pq_service_config scfg = {
        .default_visibility_ms = cfg.default_visibility_ms,
        .min_visibility_ms     = cfg.min_visibility_ms,
        .max_visibility_ms     = cfg.max_visibility_ms,
        .default_max_attempts  = cfg.default_max_attempts,
        .max_wait_ms           = cfg.max_wait_ms,
        .clock                 = pq_clock_system(),
        .repository            = repo,
    };
    pq_service *service = pq_service_create(&scfg, &err);
    if (service == NULL) {
        PQ_LOG_ERROR("service create: %s", err.message);
        pq_repository_close(repo);
        return 1;
    }

    server_ctx sc = {
        .repo = repo,
        .service = service,
        .routes_ctx = { .service = service,
                        .default_visibility_ms = cfg.default_visibility_ms },
    };

    pq_http_server *srv = pq_http_server_start(cfg.bind_address, cfg.port,
                                               cfg.worker_threads,
                                               cfg.max_body_bytes,
                                               handle_request, &sc);
    if (srv == NULL) {
        PQ_LOG_ERROR("failed to start HTTP server on %s:%u",
                     cfg.bind_address, cfg.port);
        pq_service_destroy(service);
        pq_repository_close(repo);
        return 1;
    }
    PQ_LOG_INFO("listening on %s:%u", cfg.bind_address,
                pq_http_server_port(srv));

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    while (g_signalled == 0) {
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);
    }
    PQ_LOG_INFO("shutdown signal received; draining");

    int64_t grace = cfg.default_visibility_ms;
    if (grace < 1000) grace = 1000;
    if (grace > 30000) grace = 30000;
    pq_http_server_stop(srv, grace);

    pq_service_destroy(service);
    pq_repository_close(repo);
    PQ_LOG_INFO("bye");
    return 0;
}