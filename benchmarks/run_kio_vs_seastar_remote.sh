#!/usr/bin/env bash
#
# kio vs Seastar -- CROSS-MACHINE comparison.
#
# Run this ON THE SERVER. It drives the load generator on a second machine so
# the two never compete for CPU.
#
# WHY A SECOND MACHINE EXISTS
#   The first version of this benchmark ran the load generator on the same box
#   as the server and reported ~290k rps for both frameworks. That number was
#   wrk, not either server: wrk sat at 800% CPU while kio used 254%, and
#   throughput did not move at all between 100 and 1000 connections. Driving
#   this workload costs roughly 3x more CPU on the client than on the server, so
#   a single box can never measure the server -- it measures the client.
#
# WHAT A SECOND MACHINE DOES *NOT* FIX
#   The client NIC here is 1 GbE. A request/response exchange is ~150 bytes up
#   and ~104 down, so the wire ceiling is roughly 300k exchanges/sec -- close to
#   what the single-box run produced. So this still cannot give you a true
#   max-throughput number for either framework; it gives you a CLEAN latency
#   comparison at a known offered rate, which is the thing worth having.
#
#   To measure real saturation you would need both sides on 10 GbE, or the load
#   generator on DPDK. Neither is true here.
#
# METHOD
#   Fixed rate via oha -q. The offered load is identical for every configuration
#   and is emitted on a schedule regardless of whether the previous request was
#   answered, so a server that falls behind shows up as rising p99 rather than
#   as a flattering "max rps". --latency-correction additionally accounts for
#   requests that were never issued because an earlier one was slow.
#
# USAGE (on the server)
#   ./run_kio_vs_seastar_remote.sh --client 192.168.1.177 \
#       [--rates 50000,100000,150000,200000] [--duration 20] [--conns 128]

set -uo pipefail

SERVER_CPUS="0-7"          # physical cores 0-3 on the server
CLIENT_CPUS="0-7"          # physical cores 0-3 on the client
CLIENT=""
PORT=8080
CONNS=128
DURATION=20
RATES="50000,100000,150000,200000"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIO_BIN="${KIO_BIN:-$HERE/kio-vs-seastar/build/release/benchmarks/cpp/kio_http_bench}"
SEASTAR_BIN="${SEASTAR_BIN:-$HOME/bench/seastar_http_bench}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --client) CLIENT="$2"; shift 2 ;;
    --rates) RATES="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --conns) CONNS="$2"; shift 2 ;;
    --server-cpus) SERVER_CPUS="$2"; shift 2 ;;
    --client-cpus) CLIENT_CPUS="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

[[ -n "$CLIENT" ]] || { echo "--client is required" >&2; exit 1; }
for b in "$KIO_BIN" "$SEASTAR_BIN"; do
  [[ -x "$b" ]] || { echo "missing executable: $b" >&2; exit 1; }
done

OUT="$(mktemp)"
trap 'rm -f "$OUT" /tmp/remote_oha.json' EXIT

cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
HZ=$(getconf CLK_TCK)

start_server() {  # <kind> -> pid
  local kind="$1" pid
  case "$kind" in
    kio-dispatch)
      taskset -c "$SERVER_CPUS" "$KIO_BIN" --port="$PORT" --workers=3 >/tmp/remote_srv.log 2>&1 &
      pid=$! ;;
    kio-reuseport)
      taskset -c "$SERVER_CPUS" "$KIO_BIN" --port="$PORT" --workers=4 --no-dispatch >/tmp/remote_srv.log 2>&1 &
      pid=$! ;;
    seastar-io_uring)
      BENCH_PORT="$PORT" taskset -c "$SERVER_CPUS" "$SEASTAR_BIN" \
        --overprovisioned -c 4 -m 8G --reactor-backend=io_uring >/tmp/remote_srv.log 2>&1 &
      pid=$! ;;
    seastar-epoll)
      BENCH_PORT="$PORT" taskset -c "$SERVER_CPUS" "$SEASTAR_BIN" \
        --overprovisioned -c 4 -m 8G --reactor-backend=epoll >/tmp/remote_srv.log 2>&1 &
      pid=$! ;;
    *) echo "unknown server kind: $kind" >&2; exit 1 ;;
  esac
  sleep 4
  kill -0 "$pid" 2>/dev/null || { echo "FAILED to start $kind:"; cat /tmp/remote_srv.log; exit 1; }
  echo "$pid"
}

# Verify the client can actually reach us before blaming a framework for 0 rps.
check_reachable() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$CLIENT" \
    "python3 -c \"
import socket,sys
try:
    s=socket.create_connection(('$SERVER_IP','$PORT'),timeout=5)
    s.close()
    print('  client can reach $SERVER_IP:$PORT')
except Exception as e:
    print('  CLIENT CANNOT REACH SERVER:', e); sys.exit(1)
\""
}

SERVER_IP="$(hostname -I | awk '{print $1}')"

echo "=============================================================================="
echo " kio vs Seastar -- CROSS-MACHINE fixed-rate comparison"
echo " server : $SERVER_IP:$PORT  cpus=$SERVER_CPUS"
echo " client : $CLIENT          cpus=$CLIENT_CPUS"
echo " conns=$CONNS duration=${DURATION}s"
echo " NOTE: client NIC is 1GbE -> ~300k exchanges/sec wire ceiling. These are"
echo "       latency numbers at a fixed offered rate, NOT max-throughput numbers."
echo "=============================================================================="

for kind in kio-dispatch kio-reuseport seastar-io_uring seastar-epoll; do
  echo
  echo "--- $kind ---"
  pid=$(start_server "$kind")
  check_reachable || { kill "$pid" 2>/dev/null; exit 1; }

  # Warm up from the client so page cache / JIT-free code paths settle.
  ssh "$CLIENT" "taskset -c $CLIENT_CPUS oha -c $CONNS -z 8s -q 20000 --no-tui http://$SERVER_IP:$PORT/" >/dev/null 2>&1

  IFS=',' read -ra rate_list <<<"$RATES"
  for r in "${rate_list[@]}"; do
    before=$(cpu_ticks "$pid")
    t0=$SECONDS
    ssh "$CLIENT" "taskset -c $CLIENT_CPUS oha -c $CONNS -z ${DURATION}s -q $r \
        --no-tui --latency-correction --output-format json \
        http://$SERVER_IP:$PORT/" >/tmp/remote_oha.json 2>/dev/null
    elapsed=$((SECONDS - t0))
    after=$(cpu_ticks "$pid")

    cpu_pct=0
    [[ "$elapsed" -gt 0 ]] && cpu_pct=$(( (after - before) * 100 / (HZ * elapsed) ))

    python3 - "$kind" "$r" /tmp/remote_oha.json "$cpu_pct" <<'PY' | tee -a "$OUT"
import json, sys
label, rate, path, cpu_pct = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
try:
    d = json.load(open(path))
except Exception as e:
    print(f"{label:<22} rate={rate:<8} PARSE ERROR {e}"); raise SystemExit
s, lp = d.get("summary", {}), d.get("latencyPercentiles", {})
def ms(k):
    v = lp.get(k)
    return v * 1000.0 if v is not None else float("nan")
print(f"{label:<22} offered={float(rate):>8.0f}  achieved={d.get('rps',{}).get('mean',0):>9.0f}  "
      f"success={s.get('successRate',0)*100:7.3f}%  p50={ms('p50'):7.3f}ms  p90={ms('p90'):7.3f}ms  "
      f"p99={ms('p99'):8.3f}ms  p99.9={ms('p99.9'):9.3f}ms  srv_cpu={cpu_pct:>4}%")
PY
  done

  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  sleep 2
done

echo
echo "done."