# WhyRun

WhyRun records what a Linux process actually did, normalizes low-level kernel
activity into semantic execution events, and stores a command or interactive
session as a portable capsule that can be inspected or compared with another run.

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

Record an interactive Bash session:

```bash
./build/whyrun record
```

Run commands normally, then use `exit` or `Ctrl-D` to finish and publish the
capsule. WhyRun records each shell command with its working directory, exit
status, process lineage, and semantic OS activity. A background process keeps
the identity of the command that launched it, even after Bash displays the next
prompt.

Record one command instead:

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

For an interactive session, select one command by the number shown in the
summary. This filters files, network endpoints, local IPC, errors, processes,
and the optional timeline to that command's process tree:

```bash
./build/whyrun show run-20260809-185501.wrun --command 2
./build/whyrun show run-20260809-185501.wrun --command 2 --events
```

One-shot recordings also have an implicit command `1`, so the same query works
for both recording modes.

Compare two executions:

```bash
./build/whyrun diff before.wrun after.wrun
```

`record` returns the traced command or session shell's exit code after
successfully finalizing the capsule. Collector failures return a WhyRun error
instead.

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

BashSession
  -> in-band command arm/start/end markers
  -> PtraceCollector control-FD correlation
  -> ExecutionState command/process lineage

semantic Event + command_id
  -> CapsuleWriter commands/events tables

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

Each newly recorded `.wrun` file is a self-contained SQLite database with schema
version `3`; `show` and `diff` also accept version `1` and `2` capsules. It
contains run and process records, command boundaries, semantic events with an
optional command ID, and aggregate file and network activity. Negative Linux
syscall results are stored alongside their positive errno value. Per-command
views require a version `3` capsule.

## Current Limitations

- Linux x86_64 syscall ABI only.
- Interactive sessions currently use a clean `/bin/bash` launched with
  `--noprofile --norc`; user startup files and other shells are not yet supported.
- One interactive input line is one command unit. Pipelines, compound commands,
  and commands separated on the same line are intentionally grouped together.
- Session command boundaries are reported by Bash over an internal descriptor.
  Ptrace captures those markers in syscall order and remains the source of truth
  for observed processes, files, network activity, and errors. Shell startup
  activity before the first prompt is intentionally not assigned to a command.
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
