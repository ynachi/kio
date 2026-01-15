# live
#!/bin/bash
# /usr/local/bin/setup-benchmark.sh

echo "=== Setting up system for high-performance benchmarking ==="

# Set temporary limits for current session
ulimit -n 1048576
ulimit -u 1048576

# Increase socket buffers
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 134217728"
sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728"

# Enable TCP window scaling
sudo sysctl -w net.ipv4.tcp_window_scaling=1

# Disable swap for benchmarking (temporary)
sudo swapoff -a

echo "System ready for benchmarking!"




# permanent

#!/bin/bash

echo "=== Permanent benchmark optimization for Ubuntu ==="

# Backup original files
sudo cp /etc/security/limits.conf /etc/security/limits.conf.backup
sudo cp /etc/sysctl.conf /etc/sysctl.conf.backup

# Update limits.conf
sudo tee -a /etc/security/limits.conf > /dev/null << EOF
*               soft    nofile          1048576
*               hard    nofile          1048576
root            soft    nofile          1048576
root            hard    nofile          1048576
*               soft    nproc           1048576
*               hard    nproc           1048576
*               soft    memlock         unlimited
*               hard    memlock         unlimited
EOF

# Update systemd config
sudo sed -i 's/^#DefaultLimitNOFILE=/DefaultLimitNOFILE=1048576/' /etc/systemd/system.conf
sudo sed -i 's/^#DefaultLimitNPROC=/DefaultLimitNPROC=1048576/' /etc/systemd/system.conf
sudo sed -i 's/^#DefaultLimitNOFILE=/DefaultLimitNOFILE=1048576/' /etc/systemd/user.conf
sudo sed -i 's/^#DefaultLimitNPROC=/DefaultLimitNPROC=1048576/' /etc/systemd/user.conf

# Update sysctl.conf
sudo tee -a /etc/sysctl.conf > /dev/null << EOF
fs.file-max = 2097152
fs.nr_open = 2097152
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_mem = 8388608 12582912 16777216
net.ipv4.tcp_rmem = 4096 87380 6291456
net.ipv4.tcp_wmem = 4096 16384 4194304
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_keepalive_intvl = 60
net.ipv4.tcp_keepalive_probes = 10
net.ipv4.tcp_fastopen = 3
net.netfilter.nf_conntrack_max = 1048576
net.nf_conntrack_max = 1048576
EOF

# Apply changes
sudo systemctl daemon-reload
sudo sysctl -p

echo "=== Setup complete! Please logout and login again for changes to take effect ==="