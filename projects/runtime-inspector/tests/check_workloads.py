"""Independent arbitrary-precision reference; no implementation imports."""
import json
import math
import subprocess
import sys

exe = sys.argv[1]
mask = (1 << 64) - 1
for size, seed, threads in [(0, 0, 1), (1, 1, 3), (17, 42, 4), (1003, mask, 7), (300003, 42, 7)]:
    completed = subprocess.run([exe, "--size", str(size), "--seed", str(seed),
                                "--threads", str(threads), "--repetitions", "2"],
                               capture_output=True, text=True, timeout=20, check=True)
    data = json.loads(completed.stdout)
    expected = sum(((((i ^ seed) * 0x9e3779b97f4a7c15 + seed) & mask) >> 17)
                   for i in range(size)) & mask
    assert data["expected"] == expected, (data, expected)
    assert not data["errors"]
    assert len(data["samples"]) == 12
    assert {s["variant"] for s in data["samples"]} == {"aos", "soa", "shared", "adjacent", "spaced", "local"}
    for sample in data["samples"]:
        assert sample["checksum"] == expected
        assert math.isfinite(sample["seconds"]) and sample["seconds"] >= 0
        if sample["seconds"] == 0:
            assert sample["items_per_second"] is None and sample["timing_status"] == "below_clock_resolution"
        else:
            assert math.isfinite(sample["items_per_second"]) and sample["items_per_second"] >= 0
for args in [["--threads", "0"], ["--size", "-1"], ["--seed", str(1 << 64)],
             ["--repetitions", "31"], ["--size", "10000001"], ["--bogus", "2"], ["--size"]]:
    result = subprocess.run([exe, *args], capture_output=True, text=True, timeout=5)
    assert result.returncode != 0 and "runtime-workloads:" in result.stderr, args
print("passed: independent unsigned reference, variants, boundaries and invalid inputs")
