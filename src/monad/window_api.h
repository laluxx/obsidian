#ifndef OBSIDIAN_MONAD_WINDOW_API_H
#define OBSIDIAN_MONAD_WINDOW_API_H

#include "runtime.h"
#include "editor_frames.h"
#include <stdbool.h>

typedef struct MonadLiveEvaluator MonadLiveEvaluator;
typedef struct RuntimeValue RuntimeValue;

bool obs_monad_register_window_api(ObsMonadRuntime *host,
                                   monad_error_t *error);
bool obs_monad_window_register_live_api(MonadLiveEvaluator *evaluator);
void obs_monad_window_discard_live_callbacks(void);
void obs_monad_window_live_runtime_closed(void);
RuntimeValue *obs_monad_live_window_open_value(void *context, int argc,
                                               RuntimeValue **argv);
RuntimeValue *obs_monad_live_window_close_value(void *context, int argc,
                                                RuntimeValue **argv);
RuntimeValue *obs_monad_live_window_background_value(
    void *context, int argc, RuntimeValue **argv);
RuntimeValue *obs_monad_live_window_rect_value(void *context, int argc,
                                               RuntimeValue **argv);
RuntimeValue *obs_monad_live_window_circle_value(void *context, int argc,
                                                 RuntimeValue **argv);
RuntimeValue *obs_monad_live_window_line_value(void *context, int argc,
                                               RuntimeValue **argv);
RuntimeValue *obs_monad_live_window_text_value(void *context, int argc,
                                               RuntimeValue **argv);
bool obs_monad_window_is_surface(ObsEditorFrame *frame);
void obs_monad_window_render(ObsEditorFrame *frame);
void obs_monad_window_frame_destroyed(ObsEditorFrame *frame);
bool obs_monad_window_key(ObsEditorFrame *frame, int key, int action,
                          int mods);
bool obs_monad_window_char(ObsEditorFrame *frame, unsigned int codepoint);
bool obs_monad_window_mouse_button(ObsEditorFrame *frame, int button,
                                   int action, int mods, double x, double y);
bool obs_monad_window_mouse_move(ObsEditorFrame *frame, double x, double y);
bool obs_monad_window_scroll(ObsEditorFrame *frame, double x_offset,
                             double y_offset);
void obs_monad_window_discard_callbacks(void);

#endif
