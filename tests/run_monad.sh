#!/bin/sh
# Pass OBSIDIAN_MONAD_TEST_RUNNER=/path/to/engine-host-test-runner.
# The runner must own a real embedding host and accept a .mon path as its
# sole argument, returning nonzero for type, evaluation or assertion errors.
set -eu
: "${OBSIDIAN_MONAD_TEST_RUNNER:?Set OBSIDIAN_MONAD_TEST_RUNNER to an embedded Monad test runner}"
for file in tests/monad/*.mon; do
    printf 'monad: %s\n' "$file"
    "$OBSIDIAN_MONAD_TEST_RUNNER" "$file"
done
