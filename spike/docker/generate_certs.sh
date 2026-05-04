#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M1 spike — self-signed cert generation for the local OpenTelemetry
# Collector. Run once before `docker compose up`. Certs are gitignored;
# each spike author regenerates locally.
#
# THROWAWAY. Deleted at the end of M1.

set -euo pipefail

CERT_DIR="$(cd "$(dirname "$0")" && pwd)/certs"
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

# Root CA (10 years, plenty for spike use).
openssl req -x509 -newkey rsa:2048 -keyout ca.key -out ca.crt \
    -days 3650 -nodes -subj "/CN=microtel-spike-ca"

# Server cert with SAN for localhost + collector hostname.
cat > server.cnf <<EOF
[req]
distinguished_name = req_distinguished_name
req_extensions = v3_req
prompt = no
[req_distinguished_name]
CN = localhost
[v3_req]
subjectAltName = @alt_names
[alt_names]
DNS.1 = localhost
DNS.2 = collector
IP.1 = 127.0.0.1
EOF

openssl req -newkey rsa:2048 -keyout server.key -out server.csr \
    -nodes -config server.cnf -subj "/CN=localhost"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out server.crt -days 3650 \
    -extensions v3_req -extfile server.cnf

rm server.csr server.cnf ca.srl

# Spike-only: keys are world-readable so the otel-collector container can
# read them regardless of the uid it runs as. Self-signed, gitignored,
# regenerated per author, never leave the local disk. Do not copy this
# permission posture to anything that handles real keys.
chmod 644 ./*.key ./*.crt

echo
echo "Generated certs in $CERT_DIR:"
ls -la "$CERT_DIR"
echo
echo "Trust anchor for spike clients: $CERT_DIR/ca.crt"
