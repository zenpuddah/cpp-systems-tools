# A4–A5 implementation plan

1. `specimen/{main,operations}.cpp` and `specimen/specimen.hpp`: two-TU executable, static/shared examples, runtime object-layout and address observations. `report.py` compiles fresh preprocess/assembly/object/archive/shared artifacts and captures bounded tool output into one report directory; deliberate link failure must fail.
2. `include/runtime/{workloads,bounded_queue}.hpp`, `src/{workloads,main}.cpp`: runtime-input AoS/SoA and four aggregation variants, unsigned arithmetic, checksums, setup-free timing boundaries and raw JSON samples.
3. `tests/{tests.cpp,check_workloads.py,check_report.py}`: always-on checks, independent Python arithmetic oracle, empty/full/uneven queue exact-once/closure/wakeup/join checks, report artifact/platform checks. CTest deadlines and concurrency labels.
4. `README.md`, `architecture.md`, `progress.md`: explain observed ABI/layout versus guarantees and models, native portability limits, reproducible performance question and exact verification outcomes. Verify release, supported sanitizers and fresh report; no claims about unrun Linux/IDE/counters.

Public interfaces: targets `runtime-specimen`, `runtime-workloads`, `runtime-tests`; `python3 projects/runtime-inspector/report.py --output <directory>`; workload CLI `--size N --threads N --seed N --repetitions N` emits JSON.
