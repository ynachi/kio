#!/usr/bin/env bash
#
# Build Seastar and the kio-vs-Seastar benchmark, from scratch, on a Linux box.
#
# This exists because the compile/link incantation for a Seastar app is not
# guessable: seastar::listen() returns server_socket by value, app_template eats
# argv (so the port has to come from an env var), output_stream buffers, and the
# link line needs ~17 libraries in a specific order. All of it is recorded here
# so a rerun is one command instead of archaeology.
#
# Takes ~15-25 min on 14 cores, most of it compiling libseastar itself (~300 TUs).
#
# USAGE
#   ./build_seastar.sh [install_dir]     # default: $HOME/bench/seastar
#
# SYSTEM PACKAGES NEEDED (Debian/Ubuntu names; adapt for other distros):
#   sudo apt-get install -y --no-install-recommends \
#     build-essential cmake ninja-build git ragel doxygen stow \
#     libssl-dev libaio-dev libhwloc-dev libzstd-dev liblz4-dev libpcre2-dev \
#     libncurses-dev liburing-dev libdaxctl-dev libc-ares-dev libsctp-dev \
#     libyaml-cpp-dev libfmt-dev libprotobuf-dev protobuf-compiler \
#     libbpf-dev libxdp-dev rapidjson-dev xfslibs-dev libnuma-dev libgnutls28-dev

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:-$HOME/bench/seastar}"
SEASTAR_TAG="${SEASTAR_TAG:-seastar-25.05.0}"
JOBS="${JOBS:-$(nproc)}"

echo "==> Seastar source: $PREFIX ($SEASTAR_TAG)"

if [[ ! -d "$PREFIX/.git" ]]; then
  mkdir -p "$(dirname "$PREFIX")"
  git clone --depth 1 --branch "$SEASTAR_TAG" \
    https://github.com/scylladb/seastar.git "$PREFIX"
fi

# The flags that are easy to get wrong:
#   --mode release          -O3 + NDEBUG. Anything else and you are benchmarking
#                            the compiler's patience, not the framework.
#   --enable-io_uring       kio is io_uring-only. Without this, Seastar would be
#                            on epoll by default and the comparison would really
#                            be "io_uring vs epoll" dressed up as a framework
#                            comparison. Both backends are still benchmarked.
#   --without-apps/demos    skips ~40 targets we do not link against.
#
# NOTE: configure.py writes the ninja file into build/release, NOT build.
# `ninja -C build` fails with a confusing "no such file" -- that cost an
# afternoon; use `ninja -C build/release`.
echo "==> configuring"
cd "$PREFIX"
[[ -f build/release/build.ninja ]] || ./configure.py \
  --mode release \
  --compiler=g++ \
  --c++-standard 20 \
  --enable-io_uring \
  --without-apps \
  --without-demos

echo "==> building libseastar (this is the long one)"
ninja -C build/release -j "$JOBS"

echo "==> building benchmark"
# The DEFINES must match how libseastar itself was compiled or the ABI differs
# (Seastar_API_LEVEL and SCHEDULING_GROUPS_COUNT are baked into the library).
# They are read straight out of the generated ninja file rather than hardcoded.
DEFSTR="$(grep -m1 -E '^  DEFINES = ' build/release/build.ninja | sed -E 's/^ *DEFINES = //' | tr ' ' '\n' | grep -E '^-D' | tr '\n' ' ')"

g++ -std=c++20 -O3 -o "$PREFIX/seastar_http_bench" \
    "$HERE/cpp/seastar_http_bench.cc" \
    $DEFSTR \
    -I "$PREFIX/include" \
    -I "$PREFIX/build/release/generated" \
    -L "$PREFIX/build/release" \
    -Wl,-rpath,"$PREFIX/build/release" \
    -lseastar \
    -luring -lboost_program_options -lboost_thread -lgnutls -lcares -llz4 -lfmt \
    -lyaml-cpp -lprotobuf -lzstd -lpcre2-8 -lsctp -laio -lhwloc -lnuma \
    -latomic -lrt -ldl -lpthread

echo
echo "==> done: $PREFIX/seastar_http_bench"
echo "    smoke test:"
echo "      BENCH_PORT=8081 $PREFIX/seastar_http_bench --overprovisioned -c 4 -m 8G --reactor-backend=io_uring"
echo
echo "    Then run the comparison:"
echo "      SEASTAR_BIN=$PREFIX/seastar_http_bench $HERE/run_kio_vs_seastar.sh"