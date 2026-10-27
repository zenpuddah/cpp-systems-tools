# TCP Key-Value Server

A small bounded, in-memory TCP service for inspecting real POSIX stream handling:
one owning thread, nonblocking sockets, `poll`, move-only descriptors, and an
`unordered_map`. No external service, persistence, worker pool, or dependencies
beyond C++20, POSIX, CMake 3.25+, and Python 3.10+ for tests.

## Quick start

From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev --target tcp-kv tcp-kv-unit
ctest --preset dev -R '^tcp-kv-' --output-on-failure
./build/dev/projects/tcp-kv/tcp-kv --port 5555
```

The server binds **127.0.0.1 only**, defaults to 5555, and writes exactly one
startup line, `READY <actual-port>`, to stdout. `--port 0` selects an ephemeral
port. Invalid arguments and startup failures exit nonzero with stderr diagnostics.
SIGINT/SIGTERM stop acceptance and input, drain already queued replies, and exit
within a two-second monotonic drain window (subject to OS scheduling).

Example using `nc 127.0.0.1 5555` in another terminal:

```text
SET language C++20
OK
GET language
VALUE C++20
DEL language
DELETED
GET language
NOT_FOUND
```

## Exact wire protocol

Requests and replies end in a single LF byte; connections are persistent. Every
request byte other than the terminal LF must be printable ASCII (`0x20..0x7e`).
CRLF, tabs, NUL, DEL, and non-ASCII bytes are rejected, not normalized.

| Request | Reply |
| --- | --- |
| `SET key value` | `OK` |
| `GET key` | `VALUE value` or `NOT_FOUND` |
| `DEL key` | `DELETED` or `NOT_FOUND` |

Commands are case-sensitive uppercase. Exactly one ASCII space separates verb
and key. SET requires another space separating key from value; **all bytes after
that second separator are the value**, including leading/trailing/repeated spaces.
Thus `SET k  x ` stores ` x ` and `SET k  ` stores a single space. Keys are 1–64
non-space printable bytes, values are 1–1,024 printable bytes, and the complete
line including LF is at most 4,096 bytes. GET/DEL allow no extra arguments or
spaces. Empty keys/values are syntax errors.

### Deterministic errors and recovery

For a complete frame, the **first matching condition** wins:

1. More than 4,096 bytes including LF: `ERROR line_too_long`.
2. Any non-printable/non-ASCII byte before LF: `ERROR invalid_byte`.
3. Token before the first space is not SET/GET/DEL: `ERROR unknown_command`.
4. Missing separator/field, empty key/value, or extra GET/DEL arguments/spaces:
   `ERROR syntax` (a known verb with no argument is syntax, not unknown).
5. Key exceeds 64 bytes: `ERROR key_too_long`.
6. SET value exceeds 1,024 bytes: `ERROR value_too_long`.
7. Valid SET inserts a new key into the full store: `ERROR store_full`.

An invalid complete frame consumes only that frame; the next frame is processed
normally. If a frame has accumulated 4,096 bytes **without LF**, it cannot be
valid: emit one `ERROR line_too_long`, discard up to and including the next LF,
and resume. Invalid bytes in such a frame do not override length precedence.
This early response does not wait for LF and does not retain an unbounded line.

On normal read EOF/half-close, process all complete buffered requests (over as
many fair turns as needed), then emit `ERROR incomplete_line` for a nonempty
unterminated tail, regardless of the tail's byte/grammar errors. An already
reported overlong tail emits no second error. Empty EOF emits no error. Drain
replies until empty or two seconds after EOF, whichever comes first. A reset or
fatal socket error can close immediately and discard buffered bytes. Global
signal shutdown discards unprocessed input and drains only already queued replies.

## Bounds and scheduling

| Resource | Bound |
| --- | --- |
| Active clients | 32; accepted excess sockets immediately close |
| Keys | 1,024; replacement remains legal at capacity |
| Unconsumed input / connection | 8 KiB |
| Unsent output / connection | 64 KiB |
| Commands / client turn | 16 |
| Bytes received / sent per client turn | 16 KiB each |
| Receive + send syscall attempts / client turn | 32, including EINTR/EAGAIN |
| Accept syscall attempts / loop turn | 8, including failures |
| EOF/global shutdown drain | Two absolute steady-clock seconds; never extended |

Consumed prefixes do not count toward either buffer limit. Input/output overflow
closes that connection without guaranteeing a final reply; clients must bound
their pipelines or read replies concurrently. Slow readers therefore cannot grow
application buffers without limit. Idle live clients have no timeout and can use
all 32 slots: this is a local developer tool, not an authenticated production
service. Kernel socket buffers and allocator capacity are separate from the
application's unconsumed/unsent byte limits.

Every existing client receives at most one turn per reactor pass before another
bounded accept turn. Buffered complete requests force a zero-timeout next poll,
so budget exhaustion never waits for another network arrival. POLLOUT is requested
only when unsent output exists. Each accepted socket explicitly gets O_NONBLOCK
and FD_CLOEXEC. Short writes retain offsets; EINTR retries consume budget; EAGAIN
yields; SIGPIPE is suppressed. See the [state diagram](docs/architecture.md).

## Tests and results

`tcp-kv-unit` has checks that remain enabled under `-DNDEBUG`: pure parser/store
boundaries, full-store replacement, every split point of representative requests,
descriptor ownership, injected exact short-write offsets, EINTR/EAGAIN/fatal
errors, all byte/command/syscall/buffer limits, EOF recovery and injected-clock
deadlines. Actual short writes are **not** assumed from localhost behavior.

`tcp-kv-integration` runs real ephemeral-port processes: validated READY handshake,
fragmentation/coalescing, >16 buffered requests with no new arrivals, key/line/client
limits, fair progress against pipelining and a non-reading peer, half-close/tail,
reset/reconnect, idle clients, and signal shutdown. Every process/socket wait has
a deadline and unconditional cleanup. CTest also has outer test timeouts.

Repeat the commands above with `release`, `asan`, or `ubsan`. Recorded actual
results and limitations are in [progress.md](progress.md); no test result implies
independent learner competence.

## Limitations

Linux is the reference POSIX target; native macOS support is included. Only actual
native checks are recorded, not inferred Linux passes. No TLS, authentication,
persistence, replication, IPv6, hard-real-time guarantees, or internet-facing
hardening. Timing/profiling investigations belong to the repository release
evidence phase; unit/integration timings are not throughput benchmarks.
