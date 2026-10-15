/// publication_queue.c — Owner-thread admission, fairness, cancellation, fuzz
#include "publication_queue.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/// Explicit contracts
static void contracts(void) {
    ObsPublishQueue q;
    obs_publish_queue_reset(&q);
    ObsPublishTicket t = {0};
    assert(!obs_publish_queue_begin(&q, &t));
    assert(!obs_publish_queue_request(NULL, OBS_PUBLISH_BOOT));
    assert(!obs_publish_queue_request(&q, OBS_PUBLISH_KIND_COUNT));
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_BOOT));
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_BOOT));
    assert(q.coalesced == 1 && obs_publish_queue_pending(&q) == 1);
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_REPL));
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_REPL));
    assert(q.enqueued == 2 && q.coalesced == 1);
    assert(obs_publish_queue_begin(&q, &t));
    assert(t.kind == OBS_PUBLISH_REPL);
    assert(!obs_publish_queue_begin(&q, &t));
    assert(!obs_publish_queue_finish(&q, (ObsPublishTicket){t.id + 1, t.kind}, true));
    assert(!obs_publish_queue_cancel(&q, OBS_PUBLISH_REPL));
    assert(obs_publish_queue_finish(&q, t, true));
    assert(!obs_publish_queue_finish(&q, t, true));
    assert(obs_publish_queue_begin(&q, &t) && t.kind == OBS_PUBLISH_BOOT);
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_BOOT)); /* update during compile */
    assert(obs_publish_queue_finish(&q, t, false));
    assert(obs_publish_queue_pending(&q) == 1);
    assert(obs_publish_queue_cancel(&q, OBS_PUBLISH_BOOT));
    assert(!obs_publish_queue_cancel(&q, OBS_PUBLISH_BOOT));
    assert(q.completed == 2 && q.failed == 1 && q.succeeded == 1);
    q.next_id = UINT64_MAX;
    assert(!obs_publish_queue_request(&q, OBS_PUBLISH_REPL));
    assert(obs_publish_queue_pending(&q) == 0);
}

/// Deterministic model-based stress without GPU or Monad SDK
static uint64_t state = UINT64_C(0xc6a4a7935bd1e995);
static uint32_t random32(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (uint32_t)(state >> 16);
}
static void stress(void) {
    ObsPublishQueue q;
    obs_publish_queue_reset(&q);
    ObsPublishTicket running = {0};
    for (unsigned i = 0; i < 100000; ++i) {
        unsigned op = random32() % 9;
        bool result;
        switch (op) {
            case 0: case 1:
                result = obs_publish_queue_request(&q, OBS_PUBLISH_BOOT);
                assert(result); break;
            case 2:
                result = obs_publish_queue_request(&q, OBS_PUBLISH_REPL);
                assert(result); break;
            case 3:
                (void)obs_publish_queue_cancel(&q, OBS_PUBLISH_BOOT); break;
            case 4:
                (void)obs_publish_queue_cancel(&q, OBS_PUBLISH_REPL); break;
            case 5: case 6:
                if (!q.busy) {
                    ObsPublishTicket t;
                    if (obs_publish_queue_begin(&q, &t)) running = t;
                }
                break;
            default:
                if (q.busy) {
                    assert(obs_publish_queue_finish(&q, running, op == 7));
                    running = (ObsPublishTicket){0};
                }
                break;
        }
        assert(obs_publish_queue_pending(&q) <= OBS_PUBLISH_KIND_COUNT);
        assert(q.started == q.completed + (q.busy ? 1u : 0u));
        assert(q.completed == q.succeeded + q.failed);
        assert(q.busy == (q.active.id != 0));
        for (int k = 0; k < OBS_PUBLISH_KIND_COUNT; ++k)
            assert(!q.pending_id[k] || q.pending_id[k] <= q.next_id);
    }
    /* Prove fairness under simultaneous recurring submissions. */
    obs_publish_queue_reset(&q);
    ObsPublishKind previous = OBS_PUBLISH_BOOT;
    for (unsigned i = 0; i < 2000; ++i) {
        assert(obs_publish_queue_request(&q, OBS_PUBLISH_BOOT));
        assert(obs_publish_queue_request(&q, OBS_PUBLISH_REPL));
        ObsPublishTicket ticket;
        assert(obs_publish_queue_begin(&q, &ticket));
        assert(ticket.kind != previous);
        previous = ticket.kind;
        assert(obs_publish_queue_finish(&q, ticket, true));
    }
}

int main(void) {
    contracts();
    stress();
    puts("PASS: publication queue coalescing/fairness/cancellation/100K steps");
    return 0;
}
