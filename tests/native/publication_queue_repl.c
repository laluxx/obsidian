/// publication_queue_repl.c — Queue/REPL admission and completion integration
#include "publication_queue.h"
#include "repl_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/// Only a successful checkpoint consumes a pending REPL source
static void submit_text(ObsReplModel *m, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        assert(obs_repl_model_insert(m, *p));
    assert(obs_repl_model_submit(m));
}

int main(void) {
    ObsPublishQueue queue;
    obs_publish_queue_reset(&queue);
    ObsReplModel repl;
    obs_repl_model_init(&repl);
    submit_text(&repl, "(define (step) 1)");
    /* A live foreign-object callback prevents entering the checkpoint:
     * requests remain queued, and the REPL source is untouched. */
    assert(obs_publish_queue_request(&queue, OBS_PUBLISH_REPL));
    assert(obs_publish_queue_request(&queue, OBS_PUBLISH_BOOT));
    assert(repl.has_pending && !repl.executing);
    assert(strcmp(repl.pending, "(define (step) 1)") == 0);

    ObsPublishTicket t;
    assert(obs_publish_queue_begin(&queue, &t) && t.kind == OBS_PUBLISH_REPL);
    assert(obs_repl_model_begin(&repl));
    /* A source rejection completes this request, not the queued boot load. */
    obs_repl_model_finish(&repl, false);
    assert(obs_publish_queue_finish(&queue, t, false));
    assert(repl.completed == 1 && repl.rejected == 1);
    assert(obs_publish_queue_pending(&queue) == 1);
    assert(obs_publish_queue_begin(&queue, &t) && t.kind == OBS_PUBLISH_BOOT);
    assert(!obs_publish_queue_cancel(&queue, OBS_PUBLISH_BOOT));
    assert(obs_publish_queue_request(&queue, OBS_PUBLISH_BOOT));
    assert(obs_publish_queue_finish(&queue, t, true));
    assert(obs_publish_queue_pending(&queue) == 1); /* watcher changed mid-load */

    submit_text(&repl, "(define (step) 2)");
    assert(obs_publish_queue_request(&queue, OBS_PUBLISH_REPL));
    assert(obs_publish_queue_begin(&queue, &t) && t.kind == OBS_PUBLISH_REPL);
    assert(obs_repl_model_begin(&repl));
    obs_repl_model_finish(&repl, true);
    assert(obs_publish_queue_finish(&queue, t, true));
    assert(obs_publish_queue_begin(&queue, &t) && t.kind == OBS_PUBLISH_BOOT);
    assert(obs_publish_queue_finish(&queue, t, true));
    assert(repl.completed == 2 && repl.rejected == 1);
    assert(queue.completed == 4 && queue.failed == 1 && queue.succeeded == 3);
    assert(!obs_publish_queue_pending(&queue));
    puts("PASS: REPL source retained, fair checkpoints, failed and superseded reloads");
    return 0;
}
