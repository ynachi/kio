#!/usr/bin/env bash
# Remote build/test gate for kio on srv401 (192.168.1.155).
# Mac docker (Rancher Desktop) is down; the remote is the Linux toolchain.
#
# Modes:
#   dev   -- build + run the full test suite (no sanitizer)
#   check -- same through ASan+UBSan, then TSan (what CI's `make check` does)
#
# The working tree is rsynced (without .git/build dirs) to bench/kio-ring on
# the remote; build trees live there and survive thanks to rsync's
# exclude-protection, so dependency fetches (abseil, gtest) happen once.
set -uo pipefail

REMOTE=192.168.1.155
RDIR=bench/kio-ring
MODE="${1:-dev}"

rsync -az --delete \
    --exclude .git --exclude build --exclude build-docker --exclude .zcode \
    --exclude 'cmake-build-*' --exclude docs --exclude benchmarks \
    ./ "$REMOTE:$RDIR/" || exit 1

remote_preset() {
    local san="$1" bdir="$2"
    ssh -o BatchMode=yes "$REMOTE" "set -e; cd $RDIR && \
        cmake -S . -B $bdir -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
            -DKIO_BUILD_TESTS=ON -DKIO_BUILD_DEMOS=OFF -DKIO_BUILD_BENCH=OFF \
            -DKIO_FETCH_LIBURING=OFF -DKIO_SANITIZER=$san && \
        cmake --build $bdir -j16 && \
        ctest --test-dir $bdir --output-on-failure"
}

case "$MODE" in
    dev)   remote_preset none    build-dev ;;
    check) remote_preset address build-asan && remote_preset thread build-tsan ;;
    *) echo "unknown mode: $MODE (use dev|check)" >&2; exit 2 ;;
esac
