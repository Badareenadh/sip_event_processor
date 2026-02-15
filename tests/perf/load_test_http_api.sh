
// =============================================================================
// FILE: tests/perf/load_test_http_api.sh
//
// Shell-based HTTP API load test using 'ab' (Apache Bench) or 'wrk'
// =============================================================================
/*
#!/bin/bash
# HTTP API Load Test
# Requires: wrk (https://github.com/wg/wrk) or ab (apache2-utils)

HOST="${1:-localhost}"
PORT="${2:-8080}"
DURATION="${3:-30}"
THREADS="${4:-4}"
CONNECTIONS="${5:-100}"

echo "=== HTTP API Load Test ==="
echo "Target: http://${HOST}:${PORT}"
echo "Duration: ${DURATION}s, Threads: ${THREADS}, Connections: ${CONNECTIONS}"
echo ""

if command -v wrk &> /dev/null; then
    echo "--- /health endpoint ---"
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://${HOST}:${PORT}/health
    echo ""

    echo "--- /stats endpoint ---"
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://${HOST}:${PORT}/stats
    echo ""

    echo "--- /stats/workers endpoint ---"
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://${HOST}:${PORT}/stats/workers
    echo ""

    echo "--- /subscriptions endpoint ---"
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://${HOST}:${PORT}/subscriptions
    echo ""

elif command -v ab &> /dev/null; then
    REQUESTS=10000

    echo "--- /health endpoint ---"
    ab -n ${REQUESTS} -c ${CONNECTIONS} http://${HOST}:${PORT}/health
    echo ""

    echo "--- /stats endpoint ---"
    ab -n ${REQUESTS} -c ${CONNECTIONS} http://${HOST}:${PORT}/stats
    echo ""

else
    echo "Install 'wrk' or 'ab' (apache2-utils) for HTTP load testing"
    echo "  Ubuntu: sudo apt install apache2-utils"
    echo "  wrk:    https://github.com/wg/wrk"
fi
*/
