/* pqctl_client.h - tiny HTTP client for the pocketqueue-server.
 *
 * Only the operations pq-spider needs: publish / consume+ack / nack /
 * stats. Built directly on sockets + cJSON so we don't add another
 * dependency.
 */
#ifndef SPIDER_PQCTL_CLIENT_H
#define SPIDER_PQCTL_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "pocketqueue/service.h"

typedef struct pq_client pq_client;

pq_client *pq_client_open(const char *base_url);
/* Returns false on connection failure or non-2xx HTTP status. */
bool pq_client_publish(pq_client *c, const char *queue,
                        const char *payload_json,
                        int max_attempts, pq_error *err);
bool pq_client_reserve(pq_client *c, const char *queue,
                       int64_t visibility_timeout_ms,
                       int64_t wait_ms, pq_message *out, pq_error *err);
bool pq_client_ack(pq_client *c, const char *queue,
                   const char *message_id, const char *receipt,
                   pq_error *err);
bool pq_client_nack(pq_client *c, const char *queue,
                    const char *message_id, const char *receipt,
                    const char *reason, pq_error *err);
void pq_client_close(pq_client *c);

#endif /* SPIDER_PQCTL_CLIENT_H */
