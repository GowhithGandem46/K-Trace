#!/bin/bash
#
# benchmark_perftop.sh - Benchmark perftop overhead
#
# Runs stress-ng or sysbench workloads to measure profiling overhead.
# Compares results with and without perftop module loaded.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="${RESULTS_DIR}/benchmark_${TIMESTAMP}.txt"

# Configuration
DURATION=30  # seconds
ITERATIONS=3

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root${NC}"
    exit 1
fi

# Create results directory
mkdir -p "${RESULTS_DIR}"

# Logging function
log() {
    echo -e "$1" | tee -a "${RESULTS_FILE}"
}

log_header() {
    log "\n${GREEN}=== $1 ===${NC}\n"
}

log_info() {
    log "${YELLOW}$1${NC}"
}

log_result() {
    log "$1"
}

# Check if module is loaded
is_module_loaded() {
    lsmod | grep -q "^perftop"
}

# Load module
load_module() {
    if is_module_loaded; then
        log_info "Module already loaded"
        return 0
    fi

    log_info "Loading perftop module..."
    if insmod "${SCRIPT_DIR}/perftop.ko" 2>&1 | tee -a "${RESULTS_FILE}"; then
        log_info "Module loaded successfully"
        sleep 2  # Allow module to initialize
        return 0
    else
        log "Failed to load module"
        return 1
    fi
}

# Unload module
unload_module() {
    if ! is_module_loaded; then
        log_info "Module not loaded"
        return 0
    fi

    log_info "Unloading perftop module..."
    if rmmod perftop 2>&1 | tee -a "${RESULTS_FILE}"; then
        log_info "Module unloaded successfully"
        sleep 1
        return 0
    else
        log "Failed to unload module"
        return 1
    fi
}

# Get memory footprint
get_memory_footprint() {
    if ! is_module_loaded; then
        echo "0"
        return
    fi

    # Get module memory usage from /proc/modules or /sys/module
    if [ -f "/sys/module/perftop/sections/.data" ]; then
        # This is a rough estimate
        size=$(cat /sys/module/perftop/sections/.data 2>/dev/null | awk '{print $1}' || echo "0")
        echo "$size"
    else
        # Fallback: use lsmod
        lsmod | grep "^perftop" | awk '{print $2}' || echo "0"
    fi
}

# Run benchmark with a given workload
run_benchmark() {
    local workload_name="$1"
    local workload_cmd="$2"
    local with_module="$3"

    log_header "Benchmark: ${workload_name} (Module: ${with_module})"

    if [ "$with_module" = "yes" ]; then
        load_module || return 1
    else
        unload_module || return 1
    fi

    # Reset stats if module is loaded
    if [ "$with_module" = "yes" ] && [ -f "/sys/kernel/debug/perftop/config" ]; then
        echo "reset=yes" > /sys/kernel/debug/perftop/config 2>/dev/null || true
    fi

    # Run workload in background
    log_info "Starting workload: ${workload_cmd}"
    eval "${workload_cmd}" > /dev/null 2>&1 &
    local workload_pid=$!

    # Measure CPU usage
    local start_time=$(date +%s.%N)
    local start_utime=$(cat /proc/${workload_pid}/stat 2>/dev/null | awk '{print $14}' || echo "0")
    local start_stime=$(cat /proc/${workload_pid}/stat 2>/dev/null | awk '{print $15}' || echo "0")

    # Wait for duration
    sleep "${DURATION}"

    local end_time=$(date +%s.%N)
    local end_utime=$(cat /proc/${workload_pid}/stat 2>/dev/null | awk '{print $14}' || echo "0")
    local end_stime=$(cat /proc/${workload_pid}/stat 2>/dev/null | awk '{print $15}' || echo "0")

    # Kill workload
    kill $workload_pid 2>/dev/null || true
    wait $workload_pid 2>/dev/null || true

    # Calculate metrics
    local elapsed=$(echo "$end_time - $start_time" | bc)
    local utime_diff=$(echo "$end_utime - $start_utime" | bc)
    local stime_diff=$(echo "$end_stime - $start_stime" | bc)
    local total_cpu=$(echo "$utime_diff + $stime_diff" | bc)
    local cpu_percent=$(echo "scale=2; ($total_cpu / $elapsed) * 100" | bc)

    log_result "Elapsed time: ${elapsed}s"
    log_result "CPU time: ${total_cpu} jiffies"
    log_result "CPU usage: ${cpu_percent}%"

    if [ "$with_module" = "yes" ]; then
        # Get perftop stats
        if [ -f "/proc/perftop" ]; then
            local schedule_count=$(grep -c "PID:" /proc/perftop 2>/dev/null || echo "0")
            log_result "Tasks profiled: ${schedule_count}"
        fi
    fi

    echo "$cpu_percent"
}

# Main benchmark function
main_benchmark() {
    log_header "Perftop Benchmark Suite - $(date)"
    log "Duration per test: ${DURATION} seconds"
    log "Iterations: ${ITERATIONS}"
    log "Results file: ${RESULTS_FILE}"

    # System information
    log_header "System Information"
    log_result "Kernel: $(uname -r)"
    log_result "CPU: $(nproc) cores"
    log_result "Memory: $(free -h | grep Mem | awk '{print $2}')"

    # Check for stress-ng
    if command -v stress-ng &> /dev/null; then
        WORKLOAD_CMD="stress-ng --cpu $(nproc) --timeout ${DURATION}s"
        WORKLOAD_NAME="stress-ng CPU"
    elif command -v sysbench &> /dev/null; then
        WORKLOAD_CMD="sysbench cpu --threads=$(nproc) --time=${DURATION} run"
        WORKLOAD_NAME="sysbench CPU"
    else
        log_info "Neither stress-ng nor sysbench found. Using simple CPU burn."
        WORKLOAD_CMD="timeout ${DURATION} bash -c 'while true; do :; done'"
        WORKLOAD_NAME="CPU burn"
    fi

    # Run benchmarks
    log_header "Baseline (without perftop)"
    baseline_results=()
    for i in $(seq 1 ${ITERATIONS}); do
        log_info "Iteration $i/${ITERATIONS}"
        result=$(run_benchmark "${WORKLOAD_NAME}" "${WORKLOAD_CMD}" "no")
        baseline_results+=("$result")
        sleep 2
    done

    log_header "With perftop module"
    perftop_results=()
    for i in $(seq 1 ${ITERATIONS}); do
        log_info "Iteration $i/${ITERATIONS}"
        result=$(run_benchmark "${WORKLOAD_NAME}" "${WORKLOAD_CMD}" "yes")
        perftop_results+=("$result")
        sleep 2
    fi

    # Calculate averages
    baseline_avg=$(printf '%s\n' "${baseline_results[@]}" | awk '{sum+=$1; count++} END {print sum/count}')
    perftop_avg=$(printf '%s\n' "${perftop_results[@]}" | awk '{sum+=$1; count++} END {print sum/count}')

    # Calculate overhead
    if [ -n "$baseline_avg" ] && [ -n "$perftop_avg" ]; then
        overhead=$(echo "scale=2; (($perftop_avg - $baseline_avg) / $baseline_avg) * 100" | bc)
    else
        overhead="N/A"
    fi

    # Memory footprint
    load_module
    memory_footprint=$(get_memory_footprint)

    # Summary
    log_header "Summary"
    log_result "Baseline CPU usage (avg): ${baseline_avg}%"
    log_result "With perftop CPU usage (avg): ${perftop_avg}%"
    log_result "Overhead: ${overhead}%"
    log_result "Memory footprint: ${memory_footprint} KB"

    # Accuracy check (if possible)
    if [ -f "/proc/perftop" ] && command -v perf &> /dev/null; then
        log_header "Accuracy Check (comparing with perf top)"
        log_info "Note: Manual comparison recommended"
        log_info "Run: sudo perf top & sudo python3 perftop_cli.py"
    fi

    # Cleanup
    unload_module

    log_header "Benchmark Complete"
    log "Results saved to: ${RESULTS_FILE}"
}

# Check dependencies
check_dependencies() {
    local missing=0

    if ! command -v bc &> /dev/null; then
        echo "Error: 'bc' command not found. Install with: apt-get install bc"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        exit 1
    fi
}

# Main
main() {
    check_dependencies
    main_benchmark
}

main "$@"

