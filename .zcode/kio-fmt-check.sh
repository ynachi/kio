#!/usr/bin/env bash
# Formatting gate for the ready-ring change. Repo .clang-format needs
# >= clang-format 16 (system one is 14); the PRE-EXISTING tree (user WIP in
# context.h etc.) is not clang-format-clean, so this gate only covers the NEW
# files this change owns -- do not "fix" the user's WIP formatting.
set -u
cd "$(dirname "$0")/.."
CLANG_FORMAT="${CLANG_FORMAT:-/opt/homebrew/opt/llvm@21/bin/clang-format}"
status=0
for f in include/kio/detail/ready_ring.h \
         include/kio/baton.h \
         include/kio/semaphore.h \
         tests/kio/ready_ring_test.cpp \
         tests/kio/run_loop_test.cpp \
         tests/kio/buffer_pool_test.cpp \
         tests/kio/baton_semaphore_test.cpp \
         tests/kio/timing_cancel_test.cpp; do
    [ -f "$f" ] || continue
    "$CLANG_FORMAT" --dry-run -Werror "$f" || status=1
done
exit $status
