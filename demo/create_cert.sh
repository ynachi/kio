# 1. Create a CA (Certificate Authority)
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 365 -key ca.key -out ca.crt \
    -subj "/CN=Test CA"

# 2. Create server certificate request
openssl genrsa -out server.key 4096
openssl req -new -key server.key -out server.csr \
    -subj "/CN=localhost"

# 3. Sign with CA
openssl x509 -req -days 365 -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt

# 4. Test with verification
openssl s_client -connect localhost:8080 -CAfile ca.crt -tls1_3
# Now you'll see: "Verify return code: 0 (ok)"