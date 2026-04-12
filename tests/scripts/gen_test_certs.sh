#!/usr/bin/env bash
# Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
# SPDX-License-Identifier: MIT
#
# Generate a self-signed test CA plus broker and client certificates for
# sml-mqtt-cli TLS integration tests.
#
# Outputs (relative to workspace root):
#   tests/scripts/certs/ca.crt          - CA certificate (embed in firmware)
#   tests/scripts/certs/broker.crt      - Broker certificate
#   tests/scripts/certs/broker.key      - Broker private key
#   tests/scripts/certs/client.crt      - Client certificate
#   tests/scripts/certs/client.key      - Client private key
#
# Usage (from workspace root):
#   tests/scripts/gen_test_certs.sh
#
# Requirements: openssl >= 1.1.1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERT_DIR="${SCRIPT_DIR}/certs"

mkdir -p "${CERT_DIR}"

# Validity period in days (short — test certs only)
DAYS=3650

echo "Generating test CA..."
openssl genrsa -out "${CERT_DIR}/ca.key" 4096
openssl req -new -x509 -days "${DAYS}" \
    -key "${CERT_DIR}/ca.key" \
    -out "${CERT_DIR}/ca.crt" \
    -subj "/CN=sml-mqtt-cli-test-CA/O=Test/C=AU"

echo "Generating broker certificate..."
openssl genrsa -out "${CERT_DIR}/broker.key" 2048
openssl req -new \
    -key "${CERT_DIR}/broker.key" \
    -out "${CERT_DIR}/broker.csr" \
    -subj "/CN=192.0.2.1/O=Test/C=AU"
openssl x509 -req -days "${DAYS}" \
    -in "${CERT_DIR}/broker.csr" \
    -CA "${CERT_DIR}/ca.crt" \
    -CAkey "${CERT_DIR}/ca.key" \
    -CAcreateserial \
    -out "${CERT_DIR}/broker.crt"

echo "Generating client certificate..."
openssl genrsa -out "${CERT_DIR}/client.key" 2048
openssl req -new \
    -key "${CERT_DIR}/client.key" \
    -out "${CERT_DIR}/client.csr" \
    -subj "/CN=sml-mqtt-cli-test-client/O=Test/C=AU"
openssl x509 -req -days "${DAYS}" \
    -in "${CERT_DIR}/client.csr" \
    -CA "${CERT_DIR}/ca.crt" \
    -CAkey "${CERT_DIR}/ca.key" \
    -CAcreateserial \
    -out "${CERT_DIR}/client.crt"

# Clean up CSR files — not needed after signing
rm -f "${CERT_DIR}/broker.csr" "${CERT_DIR}/client.csr"

echo ""
echo "Test certificates written to: ${CERT_DIR}"
echo "  ca.crt     - embed in firmware as TLS_CREDENTIAL_CA_CERTIFICATE"
echo "  broker.crt - install on Mosquitto broker"
echo "  broker.key - install on Mosquitto broker"
echo "  client.crt - embed in firmware as TLS_CREDENTIAL_SERVER_CERTIFICATE"
echo "  client.key - embed in firmware as TLS_CREDENTIAL_PRIVATE_KEY"
