/// audio_api — Monad PCM playback and native typing-note dispatch
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "runtime.h"
bool obs_monad_register_audio_api(ObsMonadRuntime *host, monad_error_t *error);
void obs_monad_audio_typed_character(uint32_t codepoint);
