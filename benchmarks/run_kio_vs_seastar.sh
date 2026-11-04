#!/usr/bin/env bash
#
# kio vs Seastar -- fixed-rate latency comparison.
#
# WHY NOT JUST "MAX THROUGHPUT"
#   The first version of this harness ran unpinned-to-capacity wrk and both
#   frameworks landed on ~290k rps. That number was the LOAD GENERATOR, not the
#   servers: wrk sat at 800% CPU while kio used 254%, and throughput stayed flat
#   from 100 to 1000 connections. One box cannot both saturate a server here and
#   generate the load to do it -- driving this workload costs ~3x more CPU on the
#   client than on the server, so the client always wins that race.
#
#   So this harness measures LATENCY AT A FIXED RATE instead. oha's --rate
#   emits requests on a schedule regardless of whether the server has answered
#   the previous one, which means:
#     * the offered load is identical for every configuration
#     * the server is never asked to do more than it can, so what we compare is
#       the frameworks rather than the load generator
#     * it is coordinated-omission-free in the way that matters: a server that
#       falls behind shows up as rising p99 at a fixed rate, not as a flattering
#       "max rps" achieved by quietly slowing the client down
#
#   (-q is oha's rate limiter: emit on a schedule regardless of whether the
#   previous request was answered. --latency-correction additionally corrects
#   for requests that were never issued because the previous one was slow, which
#   is what keeps a stalling server from looking fast.)
#
#   Saturation throughput is still reported at the end as a ceiling, explicitly
#   labelled load-generator-bound, because it is the honest upper bound we can
#   observe on a single box -- not a property of either server.
#
# CORE MAP (verify on your own box before trusting it)
#   cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list
#   physical core 0 -> cpus 0,1   ...   physical core 7 -> cpus 14,15
#
#   SERVER_CPUS and LOAD_CPUS must each cover whole physical cores (both
#   hyperthreads of each) or the two sides share silicon and the premise breaks.
#
# USAGE
#   ./run_kio_vs_seastar.sh [--rates 50000,100000,150000] [--duration 20] [--conns 128]

set -uo pipefail

SERVER_CPUS="0-7"      # physical cores 0-3
LOAD_CPUS="8-15"      # physical cores 4-7
PORT=8080
CONNS=128
DURATION=20
RATES="50000,100000,150000,200000"
LOADGEN="oha"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIO_BIN="${KIO_BIN:-$HERE/kio-vs-seastar/build/release/benchmarks/cpp/kio_http_bench}"
SEASTAR_BIN="${SEASTAR_BIN:-$HOME/bench/seastar_http_bench}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rates) RATES="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --conns) CONNS="$2"; shift 2 ;;
    --loadgen) LOADGEN="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

for b in "$KIO_BIN" "$SEASTAR_BIN"; do
  [[ -x "$b" ]] || { echo "missing executable: $b" >&2; exit 1; }
done
command -v "$LOADGEN" >/dev/null || { echo "$LOADGEN not installed" >&2; exit 1; }

OUT="$(mktemp)"
trap 'rm -f "$OUT" "${OUT}.json"' EXIT

# --- cpu accounting -------------------------------------------------------
# /proc/PID/stat fields 14,15 are utime,stime in clock ticks. Sampling them
# around the run gives per-config server CPU, which IS comparable between the
# two: they get identical cores. (Total-CPU-per-request is NOT comparable --
# Seastar busy-polls its shards, so it burns a core per shard while idle. See
# FAIRNESS NOTES in seastar_http_bench.cc.)
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
HZ=$(getconf CLK_TCK)

start_server() {  # <label> -> echoes pid
  local kind="$1" pid
  case "$kind" in
    kio-dispatch)
      taskset -c "$SERVER_CPUS" "$KIO_BIN" --port="$PORT" --workers=3 >/tmp/bench_srv.log 2>&1 &
      pid=$! ;;
    kio-reuseport)
      taskset -c "$SERVER_CPUS" "$KIO_BIN" --port="$PORT" --workers=4 --no-dispatch >/tmp/bench_srv.log 2>&1 &
      pid=$! ;;
    seastar-io_uring)
      BENCH_PORT="$PORT" taskset -c "$SERVER_CPUS" "$SEASTAR_BIN" \
        --overprovisioned -c 4 -m 8G --reactor-backend=io_uring >/tmp/bench_srv.log 2>&1 &
      pid=$! ;;
    seastar-epoll)
      BENCH_PORT="$PORT" taskset -c "$SERVER_CPUS" "$SEASTAR_BIN" \
        --overprovisioned -c 4 -m 8G --reactor-backend=epoll >/tmp/bench_srv.log 2>&1 &
      pid=$! ;;
    *) echo "unknown server kind: $kind" >&2; exit 1 ;;
  esac
  sleep 4
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "FAILED to start $kind:"; cat /tmp/bench_srv.log; exit 1
  fi
  echo "$pid"
}

run_rate() {  # <label> <pid> <rate>
  local label="$1" pid="$2" rate="$3"
  local before after elapsed
  before=$(cpu_ticks "$pid")

  taskset -c "$LOAD_CPUS" "$LOADGEN" -c "$CONNS" -z "${DURATION}s" -q "$rate" --no-tui --latency-correction --output-format json \
    "http://127.0.0.1:$PORT/" >"${OUT}.json" 2>/dev/null &
  local lp=$!
  sleep 2
  local t0=$SECONDS
  wait $lp
  elapsed=$((SECONDS - t0))
  after=$(cpu_ticks "$pid")

  # (ticks / HZ) is CPU-SECONDS; dividing by wall seconds and scaling by 100
  # gives percent of ONE core. Do NOT skip the /elapsed -- without it this
  # prints CPU-seconds*100 and reports nonsense like "1691%" on an 8-CPU set.
  local cpu_pct=0
  [[ "$elapsed" -gt 0 ]] && cpu_pct=$(( (after - before) * 100 / (HZ * elapsed) ))

  python3 - "$label" "$rate" "${OUT}.json" "$cpu_pct" "$elapsed" <<'PY' | tee -a "$OUT"
import json, sys
label, rate, path, cpu_pct, elapsed = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
try:
    d = json.load(open(path))
except Exception as e:
    print(f"{label:<24} rate={rate:<8} PARSE ERROR {e}"); raise SystemExit

s = d.get("summary", {})
lp = d.get("latencyPercentiles", {})

def ms(k):
    # oha reports these in SECONDS; we print milliseconds.
    v = lp.get(k)
    return v * 1000.0 if v is not None else float("nan")

succ = s.get("successRate", 0) * 100
rps = d.get("rps", {}).get("mean", 0)
print(f"{label:<22} offered={float(rate):>8.0f}  achieved={rps:>9.0f}  success={succ:7.3f}%  "
      f"p50={ms('p50'):7.3f}ms  p90={ms('p90'):7.3f}ms  p99={ms('p99'):8.3f}ms  "
      f"p99.9={ms('p99.9'):9.3f}ms  srv_cpu={cpu_pct:>4}%")
PY
}

echo "=============================================================================="
echo " kio vs Seastar -- FIXED RATE latency comparison"
echo " server cpus: $SERVER_CPUS (physical 0-3)   loadgen cpus: $LOAD_CPUS (physical 4-7)"
echo " conns=$CONNS  duration=${DURATION}s  loadgen=$LOADGEN"
echo "=============================================================================="

for kind in kio-dispatch kio-reuseport seastar-io_uring seastar-epoll; do
  echo
  echo "--- $kind ---"
  pid=$(start_server "$kind")
  # warmup at a rate below anything we will measure, to let the JIT-free but
  # page-cache-cold server settle and to avoid measuring first-touch faults.
  "$LOADGEN" -c "$CONNS" -z 8s -q 20000 --no-tui "http://127.0.0.1:$PORT/" >/dev/null 2>&1
  IFS=',' read -ra rate_list <<<"$RATES"
  for r in "${rate_list[@]}"; do
    run_rate "$kind" "$pid" "$r"
  done
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  sleep 2
done

echo
echo "=============================================================================="
echo " saturation ceiling (LOAD-GENERATOR-BOUND -- not a server property)"
echo "=============================================================================="
pid=$(start_server kio-dispatch)
taskset -c "$LOAD_CPUS" wrk -t8 -c400 -d10s --latency "http://127.0.0.1:$PORT/" 2>&1 \
  | grep -E "Requests/sec| 50%| 99%" | sed 's/^/  kio        /'
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; sleep 2
pid=$(start_server seastar-epoll)
taskset -c "$LOAD_CPUS" wrk -t8 -c400 -d10s --latency "http://127.0.0.1:$PORT/" 2>&1 \
  | grep -E "Requests/sec| 50%| 99%" | sed 's/^/  seastar    /'
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

echo
echo "full table above; raw json per run was written to ${OUT}.json"