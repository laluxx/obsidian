/// repl_model.h — Bounded REPL interaction state; no compiler/GPU dependencies
#ifndef OBSIDIAN_MONAD_REPL_MODEL_H
#define OBSIDIAN_MONAD_REPL_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OBS_REPL_SOURCE_CAP 4096u
#define OBS_REPL_LINE_CAP 256u
#define OBS_REPL_SCROLLBACK 96u
#define OBS_REPL_HISTORY 32u

typedef enum ObsReplTone {
    OBS_REPL_COMMAND, OBS_REPL_SUCCESS, OBS_REPL_INFO, OBS_REPL_ERROR
} ObsReplTone;
typedef struct ObsReplLine {
    char text[OBS_REPL_LINE_CAP];
    ObsReplTone tone;
} ObsReplLine;
typedef struct ObsReplModel {
    char input[OBS_REPL_SOURCE_CAP];
    size_t input_bytes;
    char pending[OBS_REPL_SOURCE_CAP];
    bool has_pending;
    bool executing;
    ObsReplLine lines[OBS_REPL_SCROLLBACK];
    size_t head, count;
    char history[OBS_REPL_HISTORY][OBS_REPL_SOURCE_CAP];
    size_t history_head, history_count, history_offset;
    char history_draft[OBS_REPL_SOURCE_CAP]; /* unsent input before history walk */
    uint64_t submissions, completed, rejected;
} ObsReplModel;

void obs_repl_model_init(ObsReplModel *model);
bool obs_repl_model_insert(ObsReplModel *model, uint32_t codepoint);
bool obs_repl_model_backspace(ObsReplModel *model);
void obs_repl_model_line(ObsReplModel *model, ObsReplTone tone, const char *message);
bool obs_repl_model_submit(ObsReplModel *model);
bool obs_repl_model_begin(ObsReplModel *model);
void obs_repl_model_finish(ObsReplModel *model, bool success);
const ObsReplLine *obs_repl_model_recent(const ObsReplModel *model, size_t newest_offset);
bool obs_repl_model_history(ObsReplModel *model, bool older);
#endif
