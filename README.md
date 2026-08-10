# WhyRun

WhyRun is a semantic execution recorder for Linux. It captures what a command or
interactive shell session actually does and saves the result as a self-contained
`.wrun` capsule that can be inspected or compared later.

Instead of presenting raw system calls, WhyRun organizes observed activity into
useful concepts such as processes, file reads and writes, network connections,
local IPC, exit status, and errors.

WhyRun currently supports Linux x86_64.

## Features

- **Record one command or a complete shell session.** Follow the full process
  tree, including child and background processes.
- **Understand filesystem activity.** See which paths were read or written,
  along with call counts and observed byte totals.
- **Inspect connections and local IPC.** Capture IPv4/IPv6 connection attempts
  and Unix-domain socket activity.
- **Find failures quickly.** Associate failed operations with Linux error names
  such as `ENOENT` and `ECONNREFUSED`.
- **View a semantic timeline.** Inspect ordered process, file, network, and error
  events without reading a syscall trace.
- **Attribute session activity to commands.** Select one command from an
  interactive session and view only its process tree and effects.
- **Compare two executions.** Highlight changes in exit status, commands,
  executables, files, network endpoints, and local IPC.
- **Keep portable run artifacts.** Each capsule is a single SQLite-backed file
  that can be archived, shared, or attached to a bug report.
- **Run locally.** Recording and inspection do not require an external service.

## Use Cases

WhyRun is useful when you need to answer questions such as:

- Why did a command fail on one machine but succeed on another?
- Which configuration files, inputs, or executables did a build or test use?
- What changed between a successful run and a failed run?
- Which process created or modified an unexpected file?
- Which endpoint did a tool try to connect to?
- What did each command in a troubleshooting session change or access?

It works well for debugging command-line tools, investigating CI failures,
understanding build and installation scripts, comparing environments, and
capturing reproducible evidence for bug reports.

WhyRun observes a process; it does not sandbox it or prevent side effects.

## Installation

### Prebuilt release

Download the Linux x86_64 archive and its checksum from
[GitHub Releases](https://github.com/Hermit-Avon/Whyrun/releases), then verify and
install it:

```bash
sha256sum -c whyrun-vX.Y.Z-linux-x86_64.tar.gz.sha256
tar -xzf whyrun-vX.Y.Z-linux-x86_64.tar.gz
sudo install -m 0755 whyrun-vX.Y.Z-linux-x86_64/whyrun /usr/local/bin/whyrun
whyrun --version
```

Replace `vX.Y.Z` with the downloaded release version. The release binary targets
Ubuntu 22.04-compatible Linux x86_64 systems.

### Build from source

On Ubuntu 22.04 or newer, install a C++20 compiler, CMake, and SQLite development
headers:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake libsqlite3-dev
```

Build and install WhyRun:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
whyrun --version
```

Root access is normally needed only for installation. Recording usually works
as an unprivileged user because WhyRun traces processes that it starts itself.
Hardened ptrace policies, seccomp profiles, or container runtimes may require
additional permissions.

## Quick Start

### Record a command

Use `--` to separate WhyRun arguments from the command being recorded:

```bash
whyrun record -- sh -c 'cat /etc/hosts > /tmp/hosts-copy'
```

WhyRun writes a timestamped capsule such as
`run-20260809-185501.wrun` in the current directory.

### Inspect a capsule

Show a semantic summary:

```bash
whyrun show run-20260809-185501.wrun
```

Include the ordered event timeline:

```bash
whyrun show run-20260809-185501.wrun --events
```

A summary includes the command and exit status, process count, file reads and
writes, network endpoints, local IPC, and failed operations.

### Record an interactive session

Start a clean Bash session under WhyRun:

```bash
whyrun record
```

Run commands normally, then use `exit` or `Ctrl-D` to finish and publish the
capsule. The session summary assigns a number to each command. Use that number
to inspect only one command and its descendants:

```bash
whyrun show session.wrun --command 2
whyrun show session.wrun --command 2 --events
```

Background processes remain attributed to the command that launched them.

### Compare two runs

```bash
whyrun diff successful.wrun failed.wrun
```

The diff focuses on behavioral changes rather than raw event-by-event noise.

### Get help

```bash
whyrun help
whyrun help record
whyrun help show
whyrun help diff
```

The conventional `--help`, `-h`, and `--version` flags are also supported.

## Example Output

```text
$ whyrun show run-20260809-185501.wrun
Command
  sh -c 'cat /etc/hosts > /tmp/hosts-copy'

Exit
  code 0

Processes
  2

Files
  READ
    /etc/hosts (2 calls, 187 bytes)

  WRITE
    /tmp/hosts-copy (1 calls, 187 bytes)
```

## Current Limitations

- Linux x86_64 is the only supported platform and syscall ABI.
- Recording uses `ptrace`, which adds runtime overhead and may be restricted by
  hardened systems or containers.
- Interactive mode launches `/bin/bash --noprofile --norc`; other shells and
  user startup files are not currently supported.
- One interactive input line is treated as one command unit. Pipelines and
  compound commands on that line are grouped together.
- File access through `mmap`, `sendfile`, io_uring, and other uncovered paths is
  not recorded.
- Network recording covers connection metadata, not DNS correlation, packet
  capture, or encrypted payload inspection.
- Container, mount namespace, and unusual directory-FD path translation may be
  incomplete.

## License

WhyRun is licensed under the terms in [LICENSE](LICENSE).
