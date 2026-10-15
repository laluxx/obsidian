#!/bin/sh
# Real engine-hosted integration: fixture must execute on an active GPU frame.
# The runner must load source, execute >= 2 begin/end frame pairs, assert
# `frame-callback-calls >= 2`, and verify a stale frame reference is rejected.
set -eu
: "${OBSIDIAN_MONAD_FRAME_RUNNER:?Set OBSIDIAN_MONAD_FRAME_RUNNER to the GPU/Monad frame integration harness}"
for file in tests/monad/frame/*.mon; do
  printf 'frame monad: %s
' "$file"
  "$OBSIDIAN_MONAD_FRAME_RUNNER" "$file"
done
