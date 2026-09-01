/* test_queue_service.c - unit tests for pq_service (stage 2 in-memory). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "pocketqueue/service.h"

#include "clock.h"
#include "queue_service.h"
#include "str_util.h"

static pq_service *svc;

/* Each test gets a fresh service whose clock starts at a fixed epoch. */
static int setup(void **state)
{
    (void)state;
    pq_service_config cfg = {
        .default_visibility_ms = 1000,
        .min_visibility_ms = 100,
        .max_visibility_ms = 60000,
        .default_max_attempts = 3,
        .max_wait_ms = 30000,
        .clock = pq_clock_fake(),
    };
    pq_error err = {0};
    pq_clock_fake_set(1000000000000LL);
    svc = pq_service_create(&cfg, &err);
    if (svc == NULL) {
        fprintf(stderr, "service create failed: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    pq_service_destroy(svc);
    svc = NULL;
    return 0;
}

static void publish(const char *q, const char *payload, int max_attempts,
                    pq_message *out)
{
    /* Zero-initialise before disposing so an uninitialised payload_json
     * pointer (UBSan undefined behaviour) never reaches free(). */
    memset(out, 0, sizeof(*out));
    pq_message_dispose(out);  /* idempotent on the just-cleared struct */
    pq_publish_request req = {
        .queue_name = q,
        .payload_json = payload,
        .payload_len = strlen(payload),
        .max_attempts = max_attempts,
    };
    pq_error err = {0};
    pq_status s = pq_service_publish(svc, &req, out, &err);
    assert_int_equal(s, PQ_OK);
}

static void test_publish_and_reserve_happy_path(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"task\":\"a\"}", 0, &pub);
    assert_int_equal(pub.state, PQ_MSG_READY);
    assert_int_equal(pub.attempts, 0);
    assert_int_equal(pub.max_attempts, 3);
    assert_string_equal(pub.queue, "jobs");
    assert_string_equal(pub.payload_json, "{\"task\":\"a\"}");
    pq_message_dispose(&pub);

    pq_message reserved;
    pq_error err = {0};
    pq_status s = pq_service_reserve(svc, "jobs", 1000, 0, &reserved, &err);
    assert_int_equal(s, PQ_OK);
    assert_int_equal(reserved.attempts, 1);
    assert_int_equal(reserved.state, PQ_MSG_RESERVED);
    assert_int_equal(reserved.max_attempts, 3);
    assert_true(reserved.receipt[0] != '\0');
    assert_int_equal(reserved.reserved_until_ms, 1000000001000LL);
    pq_message_dispose(&reserved);

    s = pq_service_ack(svc, "jobs", reserved.id, reserved.receipt, &err);
    assert_int_equal(s, PQ_OK);

    /* Queue should now be empty. */
    pq_message again;
    s = pq_service_reserve(svc, "jobs", 1000, 0, &again, &err);
    assert_int_equal(s, PQ_NOT_FOUND);
}

static void test_fifo_ordering_across_publishes(void **state)
{
    (void)state;
    pq_message a, b, c;
    publish("jobs", "{\"n\":1}", 0, &a);
    pq_clock_fake_advance_ms(10);
    publish("jobs", "{\"n\":2}", 0, &b);
    pq_clock_fake_advance_ms(10);
    publish("jobs", "{\"n\":3}", 0, &c);
    pq_message_dispose(&a); pq_message_dispose(&b); pq_message_dispose(&c);

    pq_message r1, r2, r3;
    pq_error err = {0};
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r1, &err), PQ_OK);
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r2, &err), PQ_OK);
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r3, &err), PQ_OK);
    /* All three payloads are equal in this test — the addresses differ. */
    (void)r1.payload_json; (void)r2.payload_json; (void)r3.payload_json;
    /* Ack all three to clean up. */
    pq_service_ack(svc, "jobs", r1.id, r1.receipt, &err);
    pq_service_ack(svc, "jobs", r2.id, r2.receipt, &err);
    pq_service_ack(svc, "jobs", r3.id, r3.receipt, &err);
    pq_message_dispose(&r1); pq_message_dispose(&r2); pq_message_dispose(&r3);
}

static void test_nack_requeues(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"x\":1}", 0, &pub);
    pq_message_dispose(&pub);

    pq_message r;
    pq_error err = {0};
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r, &err), PQ_OK);
    char id[64], receipt[33];
    pq_str_copy(id, sizeof(id), r.id);
    pq_str_copy(receipt, sizeof(receipt), r.receipt);
    pq_message_dispose(&r);

    /* Nack — should requeue. */
    assert_int_equal(pq_service_nack(svc, "jobs", id, receipt, "boom", &err),
                     PQ_OK);

    /* Reserve again — should get the same id back with attempts=2. */
    pq_message r2;
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r2, &err), PQ_OK);
    assert_string_equal(r2.id, id);
    assert_int_equal(r2.attempts, 2);
    pq_message_dispose(&r2);
}

static void test_nack_dead_letters_at_max_attempts(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"x\":1}", 2 /* max_attempts */, &pub);
    pq_message_dispose(&pub);

    pq_error err = {0};
    /* First reservation + nack: attempts becomes 1; < 2 → requeue. */
    pq_message r1;
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r1, &err), PQ_OK);
    assert_int_equal(r1.attempts, 1);
    pq_service_nack(svc, "jobs", r1.id, r1.receipt, "retry-me", &err);
    pq_message_dispose(&r1);

    /* Second reservation + nack: attempts == max → moves to jobs.dead. */
    pq_message r2;
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r2, &err), PQ_OK);
    assert_int_equal(r2.attempts, 2);
    assert_int_equal(pq_service_nack(svc, "jobs", r2.id, r2.receipt,
                                     "final", &err), PQ_OK);
    char id[64];
    pq_str_copy(id, sizeof(id), r2.id);
    pq_message_dispose(&r2);

    /* Live queue is now empty. */
    pq_message empty;
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &empty, &err),
                     PQ_NOT_FOUND);

    /* Dead-letter queue should hold it. */
    pq_message dead;
    assert_int_equal(pq_service_reserve(svc, "jobs.dead", 1000, 0, &dead, &err),
                     PQ_OK);
    assert_string_equal(dead.id, id);
    /* Reserve from .dead still increments attempts (spec §9.3); original
     * value was 2, after this reserve it's 3. */
    assert_int_equal(dead.attempts, 3);
    assert_int_equal(dead.state, PQ_MSG_DEAD_RESERVED);
    assert_true(dead.last_error[0] != '\0');
    pq_message_dispose(&dead);

    /* Ack the dead message — cleans up. */
    assert_int_equal(pq_service_ack(svc, "jobs.dead", id, dead.receipt, &err),
                     PQ_OK);
}

/* Spec §17.3: nack from a DEAD_RESERVED message requeues it to
 * DEAD_READY on the same .dead queue, NOT back to the live queue and
 * NOT to a further dead-letter queue. */
static void test_dead_nack_requeues_no_recursion(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"x\":1}", 1 /* max_attempts=1 */, &pub);
    pq_message_dispose(&pub);

    /* First nack moves it to jobs.dead. */
    pq_message r1;
    pq_error err = {0};
    assert_int_equal(pq_service_reserve(svc, "jobs", 100, 0, &r1, &err), PQ_OK);
    assert_int_equal(pq_service_nack(svc, "jobs", r1.id, r1.receipt,
                                   "first", &err), PQ_OK);
    pq_message_dispose(&r1);

    /* Reserve from jobs.dead. */
    pq_message dead;
    assert_int_equal(pq_service_reserve(svc, "jobs.dead", 100, 0, &dead, &err),
                     PQ_OK);
    assert_int_equal(dead.state, PQ_MSG_DEAD_RESERVED);
    /* Spec §9.3: every successful reserve increments attempts,
     * even from .dead. */
    assert_int_equal(dead.attempts, 2);

    /* Nack from DEAD_RESERVED — must requeue to DEAD_READY, not back to
     * the live queue, not deeper into another dead queue. */
    char id[64], receipt[33];
    pq_str_copy(id, sizeof(id), dead.id);
    pq_str_copy(receipt, sizeof(receipt), dead.receipt);
    pq_message_dispose(&dead);
    assert_int_equal(pq_service_nack(svc, "jobs.dead", id, receipt,
                                   "from dead", &err), PQ_OK);

    /* Live queue stays empty. */
    pq_message live;
    assert_int_equal(pq_service_reserve(svc, "jobs", 100, 0, &live, &err),
                     PQ_NOT_FOUND);
    /* Dead queue still holds the message; attempts bumped again. */
    assert_int_equal(pq_service_reserve(svc, "jobs.dead", 100, 0, &live, &err),
                     PQ_OK);
    assert_int_equal(live.state, PQ_MSG_DEAD_RESERVED);
    assert_int_equal(live.attempts, 3);
    pq_message_dispose(&live);
}

static void test_stale_receipt_rejected(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"x\":1}", 0, &pub);
    pq_message_dispose(&pub);

    pq_message r1;
    pq_error err = {0};
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r1, &err), PQ_OK);
    pq_message_dispose(&r1);

    /* Wrong receipt. */
    assert_int_equal(pq_service_ack(svc, "jobs", r1.id, "deadbeef", &err),
                     PQ_CONFLICT);

    /* Correct receipt. */
    assert_int_equal(pq_service_ack(svc, "jobs", r1.id, r1.receipt, &err),
                     PQ_OK);
}

static void test_visibility_expires_and_requeues(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"x\":1}", 5 /* max_attempts */, &pub);
    pq_message_dispose(&pub);

    /* Short visibility window. */
    pq_message r;
    pq_error err = {0};
    assert_int_equal(pq_service_reserve(svc, "jobs", 100, 0, &r, &err), PQ_OK);
    assert_int_equal(r.attempts, 1);
    char id[64]; pq_str_copy(id, sizeof(id), r.id);
    pq_message_dispose(&r);

    /* Step time past the deadline, then reserve — must succeed
     * (expired reservation returned to READY). */
    pq_clock_fake_advance_ms(200);

    pq_message r2;
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &r2, &err), PQ_OK);
    assert_string_equal(r2.id, id);
    assert_int_equal(r2.attempts, 2);
    pq_message_dispose(&r2);
}

static void test_visibility_expires_dead_letters(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"x\":1}", 1 /* max_attempts=1 → first expiry is fatal */,
            &pub);
    pq_message_dispose(&pub);

    pq_message r;
    pq_error err = {0};
    assert_int_equal(pq_service_reserve(svc, "jobs", 100, 0, &r, &err), PQ_OK);
    char id[64]; pq_str_copy(id, sizeof(id), r.id);
    pq_message_dispose(&r);

    pq_clock_fake_advance_ms(200);

    /* Reserve from jobs → no message. */
    pq_message empty;
    assert_int_equal(pq_service_reserve(svc, "jobs", 1000, 0, &empty, &err),
                     PQ_NOT_FOUND);

    /* The dead-letter queue holds it. */
    pq_message dead;
    assert_int_equal(pq_service_reserve(svc, "jobs.dead", 1000, 0, &dead, &err),
                     PQ_OK);
    assert_string_equal(dead.id, id);
    pq_message_dispose(&dead);
}

static void test_invalid_inputs(void **state)
{
    (void)state;
    pq_message pub;
    pq_error err = {0};
    pq_publish_request bad = {
        .queue_name = "../bad", .payload_json = "{}", .payload_len = 2,
        .max_attempts = 0,
    };
    assert_int_equal(pq_service_publish(svc, &bad, &pub, &err),
                     PQ_INVALID_ARGUMENT);

    bad.queue_name = "ok";
    bad.payload_json = "";
    bad.payload_len = 0;
    assert_int_equal(pq_service_publish(svc, &bad, &pub, &err),
                     PQ_INVALID_ARGUMENT);

    bad.payload_json = "not json";
    bad.payload_len = 8;
    assert_int_equal(pq_service_publish(svc, &bad, &pub, &err),
                     PQ_INVALID_ARGUMENT);

    bad.payload_json = "{\"ok\":1}";
    bad.payload_len = 8;
    assert_int_equal(pq_service_publish(svc, &bad, &pub, &err), PQ_OK);
    pq_message_dispose(&pub);
}

static void test_stats_counts(void **state)
{
    (void)state;
    pq_message pub;
    publish("jobs", "{\"a\":1}", 0, &pub);
    publish("jobs", "{\"b\":2}", 0, &pub);
    publish("jobs", "{\"c\":3}", 1 /* max_attempts */, &pub);
    pq_message_dispose(&pub);

    pq_error err = {0};
    pq_message r1, r2;
    assert_int_equal(pq_service_reserve(svc, "jobs", 10000, 0, &r1, &err), PQ_OK);
    assert_int_equal(pq_service_reserve(svc, "jobs", 10000, 0, &r2, &err), PQ_OK);
    pq_message_dispose(&r1); pq_message_dispose(&r2);

    pq_queue_stats stats;
    assert_int_equal(pq_service_stats(svc, "jobs", &stats, &err), PQ_OK);
    assert_int_equal(stats.ready, 1);
    assert_int_equal(stats.reserved, 2);
    assert_int_equal(stats.dead_lettered, 0);
    assert_int_equal(stats.total_active, 3);

    /* Nack the third message; with max_attempts=1 it goes to jobs.dead. */
    pq_message r3;
    assert_int_equal(pq_service_reserve(svc, "jobs", 10000, 0, &r3, &err), PQ_OK);
    assert_int_equal(pq_service_nack(svc, "jobs", r3.id, r3.receipt,
                                     "fail", &err), PQ_OK);
    pq_message_dispose(&r3);

    assert_int_equal(pq_service_stats(svc, "jobs", &stats, &err), PQ_OK);
    assert_int_equal(stats.ready, 0);
    assert_int_equal(stats.reserved, 2);
    /* Spec §18: dead_lettered counts messages in the corresponding dead-letter
     * queue, so jobs.dead_lettered counts the c message. */
    assert_int_equal(stats.dead_lettered, 1);
    assert_int_equal(stats.total_active, 2);

    assert_int_equal(pq_service_stats(svc, "jobs.dead", &stats, &err), PQ_OK);
    assert_int_equal(stats.ready, 1);
    assert_int_equal(stats.reserved, 0);
    /* dead_lettered is omitted (zero) for .dead queues (spec §18). */
    assert_int_equal(stats.dead_lettered, 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_publish_and_reserve_happy_path, setup, teardown),
        cmocka_unit_test_setup_teardown(test_fifo_ordering_across_publishes, setup, teardown),
        cmocka_unit_test_setup_teardown(test_nack_requeues, setup, teardown),
        cmocka_unit_test_setup_teardown(test_nack_dead_letters_at_max_attempts, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stale_receipt_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_visibility_expires_and_requeues, setup, teardown),
        cmocka_unit_test_setup_teardown(test_visibility_expires_dead_letters, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_inputs, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stats_counts, setup, teardown),
        cmocka_unit_test_setup_teardown(test_dead_nack_requeues_no_recursion, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}