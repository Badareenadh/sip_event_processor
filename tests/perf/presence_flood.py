
// =============================================================================
// FILE: tests/perf/presence_flood.py
//
// Simulates a presence TCP server flooding call state events.
// Used to test presence feed throughput.
//
// Run: python3 presence_flood.py [port] [rate] [duration_sec]
// =============================================================================
/*
#!/usr/bin/env python3
"""Presence TCP server simulator for load testing."""

import socket
import time
import sys
import random
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
RATE = int(sys.argv[2]) if len(sys.argv) > 2 else 10000  # events/sec
DURATION = int(sys.argv[3]) if len(sys.argv) > 3 else 60

STATES = ["trying", "ringing", "confirmed", "terminated"]
TENANTS = [f"tenant-{i}.com" for i in range(100)]

def make_event(call_id, state):
    tenant = random.choice(TENANTS)
    caller = random.randint(1000, 9999)
    callee = random.randint(1000, 9999)
    return (
        f"<CallStateEvent>"
        f"<CallId>{call_id}</CallId>"
        f"<CallerUri>sip:{caller}@{tenant}</CallerUri>"
        f"<CalleeUri>sip:{callee}@{tenant}</CalleeUri>"
        f"<State>{state}</State>"
        f"<Direction>inbound</Direction>"
        f"<TenantId>{tenant}</TenantId>"
        f"<Timestamp>{time.strftime('%Y-%m-%dT%H:%M:%SZ')}</Timestamp>"
        f"</CallStateEvent>\n"
    )

def handle_client(conn, addr):
    print(f"Client connected: {addr}")
    total_sent = 0
    call_counter = 0
    start = time.time()
    interval = 1.0 / RATE

    try:
        # Send heartbeat first
        conn.sendall(b"<Heartbeat><Timestamp>now</Timestamp></Heartbeat>\n")

        while time.time() - start < DURATION:
            batch = []
            batch_size = min(RATE // 10, 1000)  # Send in batches

            for _ in range(batch_size):
                call_counter += 1
                state = random.choice(STATES)
                batch.append(make_event(f"call-{call_counter}", state))

            data = "".join(batch).encode()
            conn.sendall(data)
            total_sent += batch_size

            # Rate limiting
            elapsed = time.time() - start
            expected = total_sent / RATE
            if expected > elapsed:
                time.sleep(expected - elapsed)

            # Periodic heartbeat
            if total_sent % (RATE * 15) == 0:
                conn.sendall(b"<Heartbeat><Timestamp>now</Timestamp></Heartbeat>\n")

        elapsed = time.time() - start
        print(f"Sent {total_sent} events in {elapsed:.1f}s "
              f"({total_sent/elapsed:.0f} events/sec)")

    except BrokenPipeError:
        print(f"Client {addr} disconnected")
    finally:
        conn.close()

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", PORT))
    server.listen(5)
    print(f"Presence flood server on port {PORT}, rate={RATE}/s, duration={DURATION}s")

    while True:
        conn, addr = server.accept()
        threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    main()
*/