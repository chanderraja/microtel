#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Generate the self-signed material the tls example's overlay collector serves
# and the example trusts:
#
#   ca.crt / ca.key          a throwaway CA. The example passes ca.crt as
#                            TlsOptions::ca_bundle; nothing else on the machine
#                            trusts it, which is exactly what makes the
#                            "without ca_bundle" failure in the README real.
#   server.crt / server.key  the collector's certificate, CN=localhost with
#                            subjectAltName DNS:localhost,IP:127.0.0.1. The SAN
#                            is load-bearing: microtel verifies the hostname
#                            (ICP 0022), and a certificate with only a CN is
#                            rejected by modern verification.
#   client.crt / client.key  a client certificate signed by the same CA, for
#                            the mTLS variant (see README.md).
#
# Keys are P-256 (`prime256v1`), matching ci/scripts/conformance.sh, so the
# example exercises the same shape of material the conformance gate does.
#
# Usage:
#   examples/tls/gen-certs.sh [output-dir]
#
# [output-dir] defaults to examples/tls/certs, which is gitignored. Existing
# material is left alone unless MICROTEL_CERT_FORCE=1 is set.
#
# NOTHING HERE IS A SECRET. These certificates authenticate a demo collector on
# localhost. Do not reuse them, do not copy this script into anything that
# matters, and do not be surprised when they expire.

set -euo pipefail

EXAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly EXAMPLE_DIR

CERT_DIR="${1:-${EXAMPLE_DIR}/certs}"
readonly CERT_DIR
readonly CERT_DAYS=365

if ! command -v openssl >/dev/null 2>&1; then
    echo "certs: openssl is required" >&2
    exit 1
fi

if [[ -f "${CERT_DIR}/server.crt" && "${MICROTEL_CERT_FORCE:-0}" != "1" ]]; then
    echo "certs: ${CERT_DIR} already has material; MICROTEL_CERT_FORCE=1 to regenerate"
    exit 0
fi

mkdir -p "$CERT_DIR"
echo "certs: generating into ${CERT_DIR}"

# The CA.
openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/ca.key" 2>/dev/null
openssl req -x509 -new -key "${CERT_DIR}/ca.key" -sha256 -days "$CERT_DAYS" \
    -subj "/CN=microtel-example-ca" -out "${CERT_DIR}/ca.crt" 2>/dev/null

# The collector's certificate. Both names the example may dial are in the SAN:
# `localhost` for the ordinary case, `127.0.0.1` for a run that dials the
# address instead.
openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/server.key" 2>/dev/null
openssl req -new -key "${CERT_DIR}/server.key" \
    -subj "/CN=localhost" -out "${CERT_DIR}/server.csr" 2>/dev/null
openssl x509 -req -in "${CERT_DIR}/server.csr" \
    -CA "${CERT_DIR}/ca.crt" -CAkey "${CERT_DIR}/ca.key" -CAcreateserial \
    -days "$CERT_DAYS" -sha256 \
    -extfile <(printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n') \
    -out "${CERT_DIR}/server.crt" 2>/dev/null

# The client certificate, for the mTLS receiver on :5337.
openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/client.key" 2>/dev/null
openssl req -new -key "${CERT_DIR}/client.key" \
    -subj "/CN=microtel-example-client" -out "${CERT_DIR}/client.csr" 2>/dev/null
openssl x509 -req -in "${CERT_DIR}/client.csr" \
    -CA "${CERT_DIR}/ca.crt" -CAkey "${CERT_DIR}/ca.key" -CAcreateserial \
    -days "$CERT_DAYS" -sha256 \
    -out "${CERT_DIR}/client.crt" 2>/dev/null

rm -f "${CERT_DIR}"/*.csr

# The collector reads these as uid 10001; the example reads them as you. None
# of it is secret, and a key the container cannot open is the single most
# common way this example fails to start.
chmod 0644 "${CERT_DIR}"/*

echo "certs: done — ca.crt, server.crt/key, client.crt/key (valid ${CERT_DAYS} days)"
openssl x509 -in "${CERT_DIR}/server.crt" -noout -subject -ext subjectAltName
