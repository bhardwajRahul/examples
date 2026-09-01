/* pocketqueue.h - public C API for the PocketQueue in-process queue service.
 *
 * Stages are added incrementally. Stage 1 only exposes configuration;
 * later stages add the queue service interface (spec §43).
 */
#ifndef POCKETQUEUE_H
#define POCKETQUEUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status codes (spec §42) ------------------------------------------- */
typedef enum pq_status {
    PQ_OK = 0,
    PQ_INVALID_ARGUMENT,
    PQ_NOT_FOUND,
    PQ_CONFLICT,
    PQ_DATABASE_BUSY,
    PQ_DATABASE_ERROR,
    PQ_OUT_OF_MEMORY,
    PQ_INTERNAL_ERROR,
    PQ_SHUTTING_DOWN
} pq_status;

/* ---- Bounded error object --------------------------------------------- */
#define PQ_ERROR_CODE_MAX     32
#define PQ_ERROR_MESSAGE_MAX  256
#define PQ_ERROR_DETAILS_MAX  512

typedef struct pq_error {
    char code[PQ_ERROR_CODE_MAX];
    char message[PQ_ERROR_MESSAGE_MAX];
    char details_json[PQ_ERROR_DETAILS_MAX];
} pq_error;

void pq_error_clear(pq_error *err);
void pq_error_set(pq_error *err, const char *code, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* POCKETQUEUE_H */