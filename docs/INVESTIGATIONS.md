# Bounded investigations

## Server question

Does changing client issuance from serial GET to batches of 16 change loopback throughput and observed round-trip latency?

Baseline: release binary, same key/value and number of correct requests. One candidate: batch the client workload (no server code alteration). Hypothesis: batching may amortize syscall/scheduling cost but changes within-batch waiting. Run:

```sh
python3 scripts/investigate_server.py --requests 256 --repetitions 5
ctest --preset release --output-on-failure
```

`evidence/server-investigation.json` retains every request's intended issue, batch issue and completion, repetition order, p50/p95/p99/count, elapsed/rate, checksums, binary hash, revision and compiler command lines. Setup and 32 warm-up GETs are excluded; send/receive/client checksum are included. Closed-loop issuance is not an offered-load benchmark. No server-internal service-time instrument exists. No causal syscall/cache conclusion follows without tracing; perf/strace are unavailable on this host. Accept an observational/no-change outcome rather than optimizing the server to fit a story.

## CPU/concurrency question

Compare the inspector's equivalent unsigned aggregation variants using its release workload driver and independent checksum fixtures. Hypothesis: shared/adjacent/spaced/local aggregation can have different costs from contention and layout, but timing alone cannot identify false sharing. Keep raw seed, work size, repetition times, checksums and flags. Use at most two plausible alternatives, not a tuning search. Details and actual conclusion are recorded alongside `evidence/cpu-investigation.json` once measured. A workload comparison is not a fabricated source optimization; no mandatory code change or speedup.

```sh
python3 scripts/investigate_cpu.py --size 200000 --threads 4 --seed 42 --repetitions 7
ctest --preset release --output-on-failure
```

The shared atomic is the baseline; spaced and local are two existing workload alternatives, adjacent a diagnostic control. Independent Python arbitrary-precision arithmetic verifies every checksum outside the timed C++ work. `evidence/cpu-investigation.json` retains raw workload output and release compile commands. Correctness/sanitizers run separately. The measured binary hash and source revision identify each dataset; later documentation commits do not retroactively alter those identities.
