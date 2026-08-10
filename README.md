# WhyRun

WhyRun records what a Linux process actually did, normalizes low-level kernel
activity into semantic execution events, and stores one execution as a portable
capsule that can be inspected or compared with another run.

WhyRun v0.1 is a working x86_64 Linux prototype. It uses `ptrace` as its
collector backend and SQLite as its capsule format. Future versions may add an
eBPF collector without changing the CLI, event sink, storage, or diff layers.

## Build

Ubuntu 22.04 or newer needs a C++20 compiler, CMake, and SQLite3 development
headers:

```bash
sudo apt-get install build-essential cmake libsqlite3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The executable is `build/whyrun`. Tests can be run with:

```bash
ctest --test-dir build --output-on-failure
```

The kernel must permit a process to trace its own children. This normally works
without root. Hardened ptrace policies, seccomp profiles, and some container
runtimes may require adjusted permissions or `sudo`.

## Release

Pushing a version tag matching `v*` runs the release workflow. It builds and
tests WhyRun on Ubuntu 22.04, verifies that the tag matches `whyrun --version`,
and publishes a GitHub Release with a Linux x86_64 tarball and SHA-256 checksum.

The workflow must be committed before creating the tag:

```bash
git tag -a v0.1.0 -m "WhyRun v0.1.0"
git push origin main
git push origin v0.1.0
```

## Usage

List the available commands, show command-specific help, or print the version:

```bash
./build/whyrun help
./build/whyrun help record
./build/whyrun version
```

The conventional `--help`, `-h`, and `--version` flags are also supported.
Each operational command accepts `--help`, for example
`./build/whyrun show --help`.

Record a command:

```bash
./build/whyrun record -- cat /etc/hosts
```

The command creates a timestamped SQLite capsule such as
`run-20260809-185501.wrun` in the current directory.

Show its semantic summary or timeline:

```bash
./build/whyrun show run-20260809-185501.wrun
./build/whyrun show run-20260809-185501.wrun --events
```

Compare two executions:

```bash
./build/whyrun diff before.wrun after.wrun
```

`record` returns the traced command's exit code after successfully finalizing
the capsule. Collector failures return a WhyRun error instead.

## Example

```text
$ ./build/whyrun record -- sh -c 'cat /etc/hosts >/tmp/out'

Capsule
  run-20260809-185501.wrun

$ ./build/whyrun show run-20260809-185501.wrun
Command
  sh -c 'cat /etc/hosts >/tmp/out'

Exit
  code 0

Processes
  2

Files
  READ
    /etc/hosts (2 calls, 187 bytes)

  WRITE
    /tmp/out (1 calls, 187 bytes)
```

## Architecture

```text
PtraceCollector
  -> per-TID raw syscall entry/exit state
  -> ExecutionState normalization and FD/process correlation
  -> semantic Event / EventSink
  -> CapsuleWriter
  -> SQLite .wrun capsule

SQLite capsule -> show summary / event timeline
two capsules   -> aggregate diff
```

The SQLite database is built in the system temporary directory and published
under its final `.wrun` name only after collection finishes. This prevents the
recorded command from observing WhyRun's capsule or rollback journal in its
working directory.

`Collector` is an abstract interface. The CLI constructs the current
`PtraceCollector`, but storage and comparison code never depend on ptrace.

The collector follows fork, vfork, clone, exec, and exit ptrace events across
the process tree. Every traced TID has independent syscall entry/exit state.
`ExecutionState` correlates successful open and socket results with later
read, write, close, dup, and connect calls. Child processes inherit a snapshot
of the parent's FD table and cwd.

Each `.wrun` file is a self-contained SQLite database with schema version `1`.
It contains run and process records, semantic events, aggregated file activity,
and aggregated network activity. Negative Linux syscall results are stored
alongside their positive errno value.

## Current Limitations

- Linux x86_64 syscall ABI only.
- The v0.1 collector uses ptrace and has its runtime overhead and permission
  constraints.
- FD tables are copied at fork/clone time; `CLONE_FILES` sharing is not modeled.
- File access through `mmap`, memory-mapped I/O, `sendfile`, and io_uring is not
  tracked.
- Relative `openat` paths need a known directory FD opened with `O_DIRECTORY`;
  otherwise the capsule keeps an explicit `<dirfd:N>/...` unresolved path.
- Network collection covers `socket` plus IPv4/IPv6 `connect`. Unix-domain
  connects are decoded separately as local IPC. WhyRun does not do DNS
  correlation, packet capture, TLS inspection, or socket namespace mapping.
- Namespace and container path translation is not complete.
- A process that changes working directory through covered `chdir` or `fchdir`
  calls is tracked, but mount namespace changes are not interpreted.

## Roadmap

- Add more semantic syscall coverage while preserving the event model.
- Model shared file descriptor tables and richer thread/process identity.
- Add an optional eBPF collector behind the existing `Collector` interface.
- Extend capsule compatibility and diff policies without requiring external
  databases or services.
