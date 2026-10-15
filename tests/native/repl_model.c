/// repl_model.c — Headless Unicode, history, boundedness, and publication queue
#include "repl_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void enter(ObsReplModel *m, const char *s) {
    while (*s) { assert(obs_repl_model_insert(m, (unsigned char)*s)); ++s; }
}
int main(void) {
    ObsReplModel m;
    obs_repl_model_init(&m);
    assert(!obs_repl_model_submit(&m));
    assert(obs_repl_model_insert(&m, 0x1f680));
    assert(m.input_bytes == 4);
    assert(obs_repl_model_backspace(&m));
    assert(m.input_bytes == 0);
    assert(!obs_repl_model_insert(&m, 0xd800));
    enter(&m, "(define x 1)");
    assert(obs_repl_model_submit(&m));
    assert(m.has_pending && !strcmp(m.pending, "(define x 1)"));
    assert(!obs_repl_model_submit(&m));
    assert(!obs_repl_model_insert(&m, 'x'));
    assert(obs_repl_model_begin(&m));
    assert(!obs_repl_model_begin(&m));
    obs_repl_model_finish(&m, true);
    assert(m.completed == 1 && m.rejected == 0);
    assert(obs_repl_model_history(&m, true));
    assert(!strcmp(m.input, "(define x 1)"));
    assert(obs_repl_model_history(&m, false));
    assert(!m.input_bytes);
    /* Unsent text survives a history excursion, including UTF-8 content. */
    assert(obs_repl_model_insert(&m, 0x03bb));
    enter(&m, " draft");
    assert(obs_repl_model_history(&m, true));
    assert(!strcmp(m.input, "(define x 1)"));
    assert(obs_repl_model_history(&m, false));
    assert(!strcmp(m.input, "\xce\xbb draft"));
    /* Editing a recalled command must not alter the saved history entry. */
    assert(obs_repl_model_history(&m, true));
    assert(obs_repl_model_insert(&m, '!'));
    assert(!strcmp(m.input, "(define x 1)!"));
    assert(obs_repl_model_history(&m, true));
    assert(!strcmp(m.input, "(define x 1)"));
    assert(obs_repl_model_history(&m, false));
    assert(!strcmp(m.input, "(define x 1)!"));
    assert(obs_repl_model_history(&m, true));
    assert(obs_repl_model_backspace(&m));
    assert(!strcmp(m.input, "(define x 1"));
    assert(obs_repl_model_history(&m, true));
    assert(obs_repl_model_history(&m, false));
    assert(!strcmp(m.input, "(define x 1"));
    /* Transcript truncation never cuts the inside of a UTF-8 scalar. */
    char long_line[400];
    memset(long_line, 'a', sizeof(long_line));
    long_line[sizeof(long_line)-1] = 0;
    long_line[OBS_REPL_LINE_CAP - 2] = (char)0xe2;
    long_line[OBS_REPL_LINE_CAP - 1] = (char)0x82;
    long_line[OBS_REPL_LINE_CAP] = (char)0xac;
    obs_repl_model_line(&m, OBS_REPL_INFO, long_line);
    assert(strlen(obs_repl_model_recent(&m, 0)->text) == OBS_REPL_LINE_CAP - 2);
    for (size_t i = 0; i < 200; i++) {
        char line[32];
        snprintf(line, sizeof(line), "entry %zu", i);
        obs_repl_model_line(&m, OBS_REPL_INFO, line);
    }
    assert(m.count == OBS_REPL_SCROLLBACK);
    assert(!strcmp(obs_repl_model_recent(&m, 0)->text, "entry 199"));
    assert(!strcmp(obs_repl_model_recent(&m, OBS_REPL_SCROLLBACK - 1)->text, "entry 104"));
    assert(obs_repl_model_recent(&m, OBS_REPL_SCROLLBACK) == NULL);
    obs_repl_model_init(&m);
    for (size_t i = 0; i < OBS_REPL_SOURCE_CAP-1; i++)
        assert(obs_repl_model_insert(&m, 'a'));
    assert(!obs_repl_model_insert(&m, 'b'));
    assert(!obs_repl_model_insert(&m, 0x1f680));
    assert(obs_repl_model_submit(&m));
    assert(obs_repl_model_begin(&m));
    obs_repl_model_finish(&m, false);
    assert(m.rejected == 1);
    obs_repl_model_init(&m);
    uint32_t random = 0x243f6a88u;
    for (unsigned i = 0; i < 20000; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        switch (random % 5u) {
        case 0: (void)obs_repl_model_insert(&m, 'a'); break;
        case 1: (void)obs_repl_model_insert(&m, 0x20ac); break;
        case 2: (void)obs_repl_model_backspace(&m); break;
        case 3:
            if (obs_repl_model_submit(&m)) {
                assert(obs_repl_model_begin(&m));
                obs_repl_model_finish(&m, (random & 8u) != 0);
            }
            break;
        case 4: (void)obs_repl_model_history(&m, (random & 8u) != 0); break;
        }
        assert(m.input_bytes < OBS_REPL_SOURCE_CAP);
        assert(m.input[m.input_bytes] == '\0');
        assert(m.count <= OBS_REPL_SCROLLBACK);
        assert(m.history_count <= OBS_REPL_HISTORY);
        assert(m.completed <= m.submissions);
    }
    puts("PASS: REPL model bounds, Unicode, history, queue, transcript");
    return 0;
}
