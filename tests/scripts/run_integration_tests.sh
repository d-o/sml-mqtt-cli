#!/usr/bin/env bash
# Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
# SPDX-License-Identifier: MIT
#
# run_integration_tests.sh
#
# The Zephyr native_sim binary creates its own zeth0 TAP interface using
# eth_native_tap.  It needs CAP_NET_ADMIN to do that.  Rather than running
# the binary as root, this script uses 'sudo setcap' to grant ONLY that
# capability to the binary, then runs it as the normal user.
#
# Privileged operations performed here (via inline sudo):
#   - setcap on the binary          (once per build)
#   - ip addr + ip link on zeth0    (after binary creates it)
#   - sysctl ip_forward             (only with --enable-routing)
#   - iptables MASQUERADE           (only with --enable-routing)
#
# Everything else - including the test binary itself - runs as YOU.
#
# Three use cases:
#
#   1. Broker on this build machine (default):
#
#        west build -p always -s sml-mqtt-cli/tests -b native_sim \
#          -- -DZEPHYR_MODULES="$PWD/boost-sml;$PWD/sml-mqtt-cli"
#        tests/scripts/run_integration_tests.sh
#
#   2. Broker on a LAN host (e.g. 172.20.5.156):
#
#        west build ... -DSML_MQTT_TEST_BROKER_HOST="172.20.5.156"
#        tests/scripts/run_integration_tests.sh \
#          --broker-host 172.20.5.156 --enable-routing --no-local-broker
#
#   3. Remote broker:
#
#        west build ... -DSML_MQTT_TEST_BROKER_HOST="mqtt.example.com"
#        tests/scripts/run_integration_tests.sh \
#          --broker-host mqtt.example.com --enable-routing --no-local-broker
#
# Network layout (after startup):
#   zeth0 (host side)  : 192.0.2.1/24
#   zeth0 (Zephyr side): 192.0.2.2/24  gateway 192.0.2.1
#   Mosquitto (case 1) : 0.0.0.0:1883
#   External broker    : routed via host uplink (cases 2 and 3)
#
# Requirements:
#   - sudo (for setcap, ip, iptables, sysctl)
#   - libcap2-bin (setcap command): sudo apt install libcap2-bin
#   - mosquitto (case 1 only)

set -euo pipefail

# -------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------

TAP_IFACE="zeth"
HOST_TAP_IP="192.0.2.1"
HOST_TAP_CIDR="${HOST_TAP_IP}/24"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MOSQUITTO_CONF="${SCRIPT_DIR}/mosquitto_integration.conf"
# scripts/ -> tests/ -> sml-mqtt-cli/ -> workspace root -> build/
BUILD_BINARY="${SCRIPT_DIR}/../../../build/zephyr/zephyr.exe"

BROKER_HOST="192.0.2.1"
BROKER_PORT="1883"

ENABLE_ROUTING=false
LOCAL_BROKER=true
SETUP_ONLY=false
UPLINK_IFACE=""

BINARY_PID=""
MOSQUITTO_PID=""
IPTABLES_RULE_ADDED=false
IP_FWD_PREV=""

# -------------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------------

log() { echo "[integration] $*"; }

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Run as your NORMAL USER.  The binary is not run as root.
sudo is used only for: setcap, ip, iptables, sysctl.

Options:
  --broker-host HOST      IP/hostname the binary connects to.
                          Default: 192.0.2.1 (local Mosquitto via TAP).
                          Must match -DSML_MQTT_TEST_BROKER_HOST at build time.

  --broker-port PORT      Broker TCP port.  Default: 1883.

  --enable-routing        Enable IP forwarding + MASQUERADE so the Zephyr app
                          (192.0.2.2) can reach hosts beyond the TAP subnet.
                          Required for LAN/remote brokers.

  --no-local-broker       Skip starting Mosquitto.

  --setup-only            Set up networking and broker, then wait.
                          Run the binary manually in another terminal.

  --uplink-iface IFACE    Host interface for MASQUERADE (auto-detected).

  --binary PATH           Path to the zephyr.exe binary.
                          Default: build/zephyr/zephyr.exe

  --help, -h              Show this help.

Examples:
  # Case 1 - local Mosquitto:
  $0

  # Case 2 - LAN broker (rebuild first with matching IP):
  west build ... -DSML_MQTT_TEST_BROKER_HOST=172.20.5.156
  $0 --broker-host 172.20.5.156 --enable-routing --no-local-broker

  # Interactive: keep setup alive, run binary manually:
  $0 --setup-only
  ./build/zephyr/zephyr.exe
EOF
}

detect_uplink() {
    ip route show default | awk '/^default/ { print $5; exit }'
}

cleanup() {
    log "Cleaning up..."

    # Stop binary if still running (--setup-only path)
    if [[ -n "$BINARY_PID" ]] && kill -0 "$BINARY_PID" 2>/dev/null; then
        kill "$BINARY_PID" 2>/dev/null || true
        wait "$BINARY_PID" 2>/dev/null || true
    fi

    if [[ -n "$MOSQUITTO_PID" ]] && kill -0 "$MOSQUITTO_PID" 2>/dev/null; then
        log "Stopping Mosquitto"
        kill "$MOSQUITTO_PID" || true
        wait "$MOSQUITTO_PID" 2>/dev/null || true
    fi

    if [[ "$IPTABLES_RULE_ADDED" == "true" ]] && [[ -n "$UPLINK_IFACE" ]]; then
        log "Removing iptables MASQUERADE on $UPLINK_IFACE"
        sudo iptables -t nat -D POSTROUTING \
            -s "$HOST_TAP_CIDR" -o "$UPLINK_IFACE" -j MASQUERADE 2>/dev/null || true
    fi

    if [[ -n "$IP_FWD_PREV" ]]; then
        log "Restoring ip_forward to $IP_FWD_PREV"
        sudo sysctl -qw "net.ipv4.ip_forward=${IP_FWD_PREV}" || true
    fi

    # The TAP is non-persistent (binary created it without TUNSETPERSIST).
    # It disappears automatically when the binary exits.
    # Force-remove if somehow still present (e.g. binary crash).
    if ip link show "$TAP_IFACE" &>/dev/null; then
        log "Removing lingering TAP $TAP_IFACE"
        sudo ip link delete "$TAP_IFACE" 2>/dev/null || true
    fi

    log "Done."
}

trap cleanup EXIT INT TERM

# -------------------------------------------------------------------------
# Parse arguments
# -------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --broker-host)     BROKER_HOST="$2";    shift 2 ;;
        --broker-port)     BROKER_PORT="$2";    shift 2 ;;
        --enable-routing)  ENABLE_ROUTING=true; shift ;;
        --no-local-broker) LOCAL_BROKER=false;  shift ;;
        --setup-only)      SETUP_ONLY=true;     shift ;;
        --uplink-iface)    UPLINK_IFACE="$2";   shift 2 ;;
        --binary)          BUILD_BINARY="$2";   shift 2 ;;
        --help|-h)         usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

# -------------------------------------------------------------------------
# Sanity checks
# -------------------------------------------------------------------------

if [[ $EUID -eq 0 ]]; then
    echo "ERROR: Do not run as root.  Run as your normal user." >&2
    echo "       sudo is used automatically for the specific commands that need it." >&2
    exit 1
fi

if ! command -v sudo &>/dev/null;     then echo "ERROR: sudo not found."    >&2; exit 1; fi
if ! command -v ip &>/dev/null;       then echo "ERROR: ip not found (install iproute2)." >&2; exit 1; fi
if ! command -v setcap &>/dev/null;   then
    echo "ERROR: setcap not found.  Install: sudo apt install libcap2-bin" >&2; exit 1
fi
if [[ "$ENABLE_ROUTING" == "true" ]] && ! command -v iptables &>/dev/null; then
    echo "ERROR: iptables not found.  Install: sudo apt install iptables" >&2; exit 1
fi
if [[ "$LOCAL_BROKER" == "true" ]] && ! command -v mosquitto &>/dev/null; then
    echo "ERROR: mosquitto not found.  Install: sudo apt install mosquitto" >&2
    echo "       Or use: --no-local-broker" >&2; exit 1
fi
if [[ ! -x "$BUILD_BINARY" ]]; then
    echo "ERROR: Binary not found: $BUILD_BINARY" >&2
    echo "       Build first:" >&2
    echo "         west build -p always -s sml-mqtt-cli/tests -b native_sim \\" >&2
    echo "           -- -DZEPHYR_MODULES=\"\$PWD/boost-sml;\$PWD/sml-mqtt-cli\"" >&2
    [[ "$BROKER_HOST" != "192.0.2.1" ]] && \
        echo "          -DSML_MQTT_TEST_BROKER_HOST=\"${BROKER_HOST}\"" >&2
    exit 1
fi

# -------------------------------------------------------------------------
# Step 1: Grant CAP_NET_ADMIN to the binary (allows it to create zeth0)
#
# setcap is permanent on the file.  It survives until the file is replaced
# (e.g. by the next west build).  Re-applying it here is safe and fast.
# The capability is ONLY on this specific binary - it is NOT inherited by
# child processes and grants nothing to your shell.
# -------------------------------------------------------------------------

log "Granting cap_net_admin to binary (setcap)"
sudo setcap cap_net_admin+ep "$BUILD_BINARY"

# -------------------------------------------------------------------------
# Step 2: Start local Mosquitto (case 1 only, runs as current user)
# -------------------------------------------------------------------------

if [[ "$LOCAL_BROKER" == "true" ]]; then
    if ss -ltn 2>/dev/null | grep -q ":${BROKER_PORT} "; then
        log "WARNING: Port ${BROKER_PORT} already in use."
    fi
    log "Starting Mosquitto ($MOSQUITTO_CONF)"
    mosquitto -c "$MOSQUITTO_CONF" &
    MOSQUITTO_PID=$!
    MOSQUITTO_WAIT=0
    while ! ss -ltn 2>/dev/null | grep -q ":${BROKER_PORT} "; do
        sleep 0.1
        MOSQUITTO_WAIT=$((MOSQUITTO_WAIT + 1))
        if [[ $MOSQUITTO_WAIT -gt 50 ]]; then
            echo "ERROR: Mosquitto not listening on port ${BROKER_PORT} after 5 s." >&2
            exit 1
        fi
        if ! kill -0 "$MOSQUITTO_PID" 2>/dev/null; then
            echo "ERROR: Mosquitto exited unexpectedly." >&2
            exit 1
        fi
    done
    log "Mosquitto PID=$MOSQUITTO_PID at ${HOST_TAP_IP}:${BROKER_PORT}"
else
    log "No local broker - using ${BROKER_HOST}:${BROKER_PORT}"
fi

# -------------------------------------------------------------------------
# Step 3: Launch binary in background (creates zeth0 itself)
# -------------------------------------------------------------------------

log "Launching binary as $USER: $BUILD_BINARY"
"$BUILD_BINARY" &
BINARY_PID=$!

# -------------------------------------------------------------------------
# Step 4: Wait for zeth0 to appear, then configure the host side
#
# Zephyr's net_config_init waits up to CONFIG_NET_CONFIG_INIT_TIMEOUT (10 s)
# for the interface to come up.  We have that window to configure the host.
# -------------------------------------------------------------------------

log "Waiting for $TAP_IFACE to appear..."
TAP_WAIT=0
while ! ip link show "$TAP_IFACE" &>/dev/null; do
    sleep 0.1
    TAP_WAIT=$((TAP_WAIT + 1))
    if [[ $TAP_WAIT -gt 50 ]]; then
        echo "ERROR: $TAP_IFACE did not appear within 5 s - binary may have crashed." >&2
        exit 1
    fi
done

log "$TAP_IFACE appeared - configuring host side"
sudo ip addr add "$HOST_TAP_CIDR" dev "$TAP_IFACE"
sudo ip link set "$TAP_IFACE" up

# -------------------------------------------------------------------------
# Step 5: IP forwarding + NAT (cases 2 and 3)
# -------------------------------------------------------------------------

if [[ "$ENABLE_ROUTING" == "true" ]]; then
    if [[ -z "$UPLINK_IFACE" ]]; then
        UPLINK_IFACE="$(detect_uplink)"
        [[ -z "$UPLINK_IFACE" ]] && { echo "ERROR: Cannot detect uplink.  Use --uplink-iface." >&2; exit 1; }
    fi

    log "IP forwarding + MASQUERADE: $HOST_TAP_CIDR -> $UPLINK_IFACE -> $BROKER_HOST"
    IP_FWD_PREV="$(sudo sysctl -n net.ipv4.ip_forward)"
    sudo sysctl -qw net.ipv4.ip_forward=1
    sudo iptables -t nat -A POSTROUTING \
        -s "$HOST_TAP_CIDR" -o "$UPLINK_IFACE" -j MASQUERADE
    IPTABLES_RULE_ADDED=true
fi

# -------------------------------------------------------------------------
# Step 6: Summary
# -------------------------------------------------------------------------

log ""
log "--- Running ---"
log "  Binary PID     : $BINARY_PID (running as $USER)"
log "  Broker target  : ${BROKER_HOST}:${BROKER_PORT}"
log "  Routing        : $([[ $ENABLE_ROUTING == true ]] && echo "NAT via $UPLINK_IFACE" || echo "direct (TAP only)")"
log ""

# -------------------------------------------------------------------------
# Step 7: Wait for binary (or hold for --setup-only)
# -------------------------------------------------------------------------

if [[ "$SETUP_ONLY" == "true" ]]; then
    log "Setup complete.  Binary is running (PID $BINARY_PID)."
    log "Watch output above.  Press Ctrl+C to stop and tear down."
    wait "$BINARY_PID" || true
    exit 0
fi

wait "$BINARY_PID"
EXIT_CODE=$?
log "Binary exited with code $EXIT_CODE"
exit "$EXIT_CODE"
