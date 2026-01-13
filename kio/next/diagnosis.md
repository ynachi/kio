# 1. Check CPU breakdown
pidstat -t -p $(pgrep tcp_demo_v1) 1 | head -20

# 2. Profile for 30 seconds
sudo perf record -F 999 -p $(pgrep tcp_demo_v1) -g -- sleep 30
sudo perf report --stdio | head -100 > perf_output.txt

# 3. Check syscall rate
sudo strace -c -f -p $(pgrep tcp_demo_v1) 2>&1 &
# Wait 10 seconds, then Ctrl+C