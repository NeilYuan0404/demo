#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

TEST_FILE="${TEST_FILE:-test_1gb.bin}"
DEFAULT_SIZE_MB="${TEST_SIZE_MB:-64}"

cleanup_compare_local_receiver() {
    local receiver_pid="$1"

    if [[ -n "$receiver_pid" ]] && kill -0 "$receiver_pid" >/dev/null 2>&1; then
        kill "$receiver_pid" >/dev/null 2>&1 || true
        wait "$receiver_pid" >/dev/null 2>&1 || true
    fi
}

is_loopback_ip() {
    [[ "$1" == "127.0.0.1" || "$1" == "localhost" ]]
}

list_local_ipv4s() {
    if command -v ip >/dev/null 2>&1; then
        ip -4 -o addr show up scope global | awk '{print $4}' | cut -d/ -f1
        return 0
    fi

    if command -v hostname >/dev/null 2>&1; then
        hostname -I 2>/dev/null | tr ' ' '\n' | sed '/^$/d'
    fi
}

is_local_ipv4() {
    local candidate_ip="$1"
    local local_ip

    while IFS= read -r local_ip; do
        if [[ "$local_ip" == "$candidate_ip" ]]; then
            return 0
        fi
    done < <(list_local_ipv4s)

    return 1
}

require_rdma_support() {
    if [[ ! -d /sys/class/infiniband ]] || [[ -z "$(ls -A /sys/class/infiniband 2>/dev/null)" ]]; then
        echo "No RDMA devices detected under /sys/class/infiniband." >&2
        echo "compare-local requires rdma-core plus an active RDMA device/driver on this host." >&2
        return 1
    fi

    if command -v ibv_devinfo >/dev/null 2>&1 && ! ibv_devinfo >/dev/null 2>&1; then
        echo "ibv_devinfo could not enumerate a usable RDMA device." >&2
        return 1
    fi
}

detect_local_rdma_ip() {
    local detected_ip=""

    if [[ -n "${RDMA_IP:-}" ]]; then
        if is_loopback_ip "$RDMA_IP"; then
            echo "RDMA_IP must be a non-loopback address, got: $RDMA_IP" >&2
            return 1
        fi
        printf '%s\n' "$RDMA_IP"
        return 0
    fi

    if command -v ip >/dev/null 2>&1; then
        detected_ip=$(ip -4 -o addr show up scope global | awk '{print $4}' | cut -d/ -f1 | head -n1)
    fi

    if [[ -z "$detected_ip" ]] && command -v hostname >/dev/null 2>&1; then
        detected_ip=$(hostname -I 2>/dev/null | awk '{print $1}')
    fi

    if [[ -z "$detected_ip" ]]; then
        echo "Unable to detect a usable non-loopback IPv4 address. Set RDMA_IP explicitly." >&2
        return 1
    fi

    if is_loopback_ip "$detected_ip"; then
        echo "Detected loopback address $detected_ip, which cannot be used for RDMA. Set RDMA_IP explicitly." >&2
        return 1
    fi

    printf '%s\n' "$detected_ip"
}

build_sendfile_binary() {
    echo "Compiling test_sendfile for local debugging..."
    gcc -o test_sendfile test_sendfile.c -O0 -g -Wall -Wextra -lpthread
}

build_rdma_binaries() {
    echo "Compiling RDMA binaries for local debugging..."
    gcc -o simulate_rdma simulate_rdma.c rdma_transfer.c -O0 -g -Wall -Wextra -lrdmacm -libverbs -lpthread
    gcc -o compare_transfer compare_transfer.c rdma_transfer.c -O0 -g -Wall -Wextra -lrdmacm -libverbs -lpthread
}

create_test_file() {
    local size_mb="$1"

    echo "Creating local debug file ${TEST_FILE} (${size_mb} MiB)..."
    rm -f "$TEST_FILE"
    dd if=/dev/zero of="$TEST_FILE" bs=1M count="$size_mb" status=none
}

run_sendfile_debug() {
    local size_mb="${1:-$DEFAULT_SIZE_MB}"

    build_sendfile_binary
    create_test_file "$size_mb"

    echo "Running local sendfile test..."
    ./test_sendfile
}

run_simulate_debug() {
    local size_mb="${1:-$DEFAULT_SIZE_MB}"

    build_rdma_binaries
    create_test_file "$size_mb"

    echo "Running local RDMA benchmark..."
    ./simulate_rdma
}

run_compare_local() {
    local bind_ip="${1:-}"
    local size_mb="${2:-$DEFAULT_SIZE_MB}"
    local receiver_pid=""
    local receiver_status

    if [[ -n "$bind_ip" && "$bind_ip" =~ ^[0-9]+$ && -z "${2:-}" ]]; then
        size_mb="$bind_ip"
        bind_ip=""
    fi

    if [[ -z "$bind_ip" ]]; then
        bind_ip=$(detect_local_rdma_ip)
    fi

    if is_loopback_ip "$bind_ip"; then
        echo "compare-local requires a non-loopback IPv4 address; 127.0.0.1 cannot be used for RDMA." >&2
        echo "Set RDMA_IP or pass an explicit local RDMA IP, for example: ./local_debug.sh compare-local 192.168.1.20 64" >&2
        return 1
    fi

    if ! is_local_ipv4 "$bind_ip"; then
        echo "compare-local requires a local IPv4 assigned on this host; got: $bind_ip" >&2
        echo "Available local IPv4 addresses:" >&2
        list_local_ipv4s | sed 's/^/  - /' >&2
        return 1
    fi

    require_rdma_support

    build_rdma_binaries
    create_test_file "$size_mb"

    echo "Starting local receiver on ${bind_ip}..."
    ./compare_transfer receive "$bind_ip" &
    receiver_pid=$!

    trap "cleanup_compare_local_receiver '$receiver_pid'" EXIT
    sleep 1

    if ! kill -0 "$receiver_pid" >/dev/null 2>&1; then
        wait "$receiver_pid"
        receiver_status=$?
        echo "Local receiver exited before sender started (status ${receiver_status})." >&2
        return "$receiver_status"
    fi

    echo "Running local sender against ${bind_ip}..."
    ./compare_transfer send "$bind_ip"
    wait "$receiver_pid"
    trap - EXIT
}

print_usage() {
    cat <<'EOF'
Usage:
  ./local_debug.sh sendfile [size_mb]
  ./local_debug.sh simulate [size_mb]
  ./local_debug.sh compare-local [bind_ip] [size_mb]

Examples:
  ./local_debug.sh sendfile 32
  ./local_debug.sh simulate 64
    RDMA_IP=192.168.1.20 ./local_debug.sh compare-local 64
    ./local_debug.sh compare-local 192.168.1.20 64

Notes:
  sendfile: pure local loopback test, does not require RDMA hardware
  simulate/compare-local: require rdma_cm/libibverbs and usable RDMA networking
    compare-local: does not accept 127.0.0.1 or localhost for the RDMA path
    compare-local: bind_ip must be assigned on the current host
EOF
}

case "${1:-sendfile}" in
    sendfile)
        shift || true
        run_sendfile_debug "$@"
        ;;
    simulate)
        shift || true
        run_simulate_debug "$@"
        ;;
    compare-local)
        shift || true
        run_compare_local "$@"
        ;;
    -h|--help|help)
        print_usage
        ;;
    *)
        print_usage
        exit 1
        ;;
esac