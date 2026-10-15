/// repl_model.c — Deterministic, bounded, UTF-8 REPL input and transcript
#include "repl_model.h"
#include <string.h>

/// UTF-8 input
static size_t encode_utf8(uint32_t cp, char out[4]) {
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    if (cp < 0x20 && cp != '\n' && cp != '\t') return 0;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 63)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63));
    out[3] = (char)(0x80 | (cp & 63)); return 4;
}

void obs_repl_model_init(ObsReplModel *m) {
    if (m) memset(m, 0, sizeof(*m));
}

bool obs_repl_model_insert(ObsReplModel *m, uint32_t codepoint) {
    if (!m || m->has_pending || m->executing) return false;
    char utf8[4];
    size_t n = encode_utf8(codepoint, utf8);
    if (!n || n > OBS_REPL_SOURCE_CAP - 1 - m->input_bytes) return false;
    memcpy(m->input + m->input_bytes, utf8, n);
    m->input_bytes += n;
    m->input[m->input_bytes] = '\0';
    /* An edited recalled entry becomes the new draft, not a mutation of the
     * immutable history ring. */
    if (m->history_offset) {
        m->history_offset = 0;
        memcpy(m->history_draft, m->input, m->input_bytes + 1);
    }
    return true;
}

bool obs_repl_model_backspace(ObsReplModel *m) {
    if (!m || !m->input_bytes || m->has_pending || m->executing) return false;
    size_t pos = m->input_bytes - 1;
    while (pos > 0 && ((unsigned char)m->input[pos] & 0xc0u) == 0x80u) --pos;
    m->input[pos] = '\0';
    m->input_bytes = pos;
    if (m->history_offset) {
        m->history_offset = 0;
        memcpy(m->history_draft, m->input, m->input_bytes + 1);
    }
    return true;
}

/// Fixed-memory transcript
void obs_repl_model_line(ObsReplModel *m, ObsReplTone tone, const char *message) {
    if (!m || !message) return;
    ObsReplLine *line = &m->lines[m->head];
    size_t len = strlen(message);
    if (len >= sizeof(line->text)) len = sizeof(line->text) - 1;
    /* Never truncate a UTF-8 scalar in the transcript. */
    while (len && ((unsigned char)message[len] & 0xc0u) == 0x80u) --len;
    memcpy(line->text, message, len);
    line->text[len] = '\0';
    line->tone = tone;
    m->head = (m->head + 1u) % OBS_REPL_SCROLLBACK;
    if (m->count < OBS_REPL_SCROLLBACK) ++m->count;
}

const ObsReplLine *obs_repl_model_recent(const ObsReplModel *m, size_t offset) {
    if (!m || offset >= m->count) return NULL;
    size_t index = (m->head + OBS_REPL_SCROLLBACK - 1u - offset) % OBS_REPL_SCROLLBACK;
    return &m->lines[index];
}

/// Command submission and history
bool obs_repl_model_submit(ObsReplModel *m) {
    if (!m || m->has_pending || m->executing || !m->input_bytes) return false;
    memcpy(m->pending, m->input, m->input_bytes + 1);
    m->has_pending = true;
    ++m->submissions;
    obs_repl_model_line(m, OBS_REPL_COMMAND, m->input);
    memcpy(m->history[m->history_head], m->input, m->input_bytes + 1);
    m->history_head = (m->history_head + 1u) % OBS_REPL_HISTORY;
    if (m->history_count < OBS_REPL_HISTORY) ++m->history_count;
    m->history_offset = 0;
    m->history_draft[0] = '\0';
    m->input_bytes = 0;
    m->input[0] = '\0';
    return true;
}

bool obs_repl_model_begin(ObsReplModel *m) {
    if (!m || !m->has_pending || m->executing) return false;
    m->executing = true;
    return true;
}

void obs_repl_model_finish(ObsReplModel *m, bool success) {
    if (!m || !m->executing) return;
    ++m->completed;
    if (!success) ++m->rejected;
    m->pending[0] = '\0';
    m->has_pending = false;
    m->executing = false;
}

bool obs_repl_model_history(ObsReplModel *m, bool older) {
    if (!m || !m->history_count || m->has_pending || m->executing) return false;
    if (older) {
        if (m->history_offset == m->history_count) return false;
        /* Save unsent source exactly once before entering history. */
        if (!m->history_offset)
            memcpy(m->history_draft, m->input, m->input_bytes + 1);
        ++m->history_offset;
    } else if (m->history_offset) {
        --m->history_offset;
    } else return false;
    if (m->history_offset == 0) {
        m->input_bytes = strlen(m->history_draft);
        memcpy(m->input, m->history_draft, m->input_bytes + 1);
        return true;
    }
    const size_t i = (m->history_head + OBS_REPL_HISTORY - m->history_offset)
                   % OBS_REPL_HISTORY;
    m->input_bytes = strlen(m->history[i]);
    memcpy(m->input, m->history[i], m->input_bytes + 1);
    return true;
}
