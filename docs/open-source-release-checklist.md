# Open Source Release Checklist

## Purpose

This document captures the main release-readiness concerns for publishing `hen` as open source software.

The key judgment is:

- A somewhat messy internal architecture is not, by itself, a reason to delay release.
- The main things to fix before OSS are trust, portability, and repository hygiene issues.

## Not A Release Blocker

These are worth improving over time, but they are not inherently embarrassing to ship:

- large orchestration functions
- hidden state that should eventually be scoped more cleanly
- debugger-centered control flow that is more practical than elegant
- uneven architectural polish across subsystems

For a project like `hen`, people will generally accept rough internals if the system is interesting, useful, and honest about its constraints.

## Must Fix Before OSS

### 1. Build And Install Must Not Mutate The User's Machine Silently

Current concern:

- [`build.sh`](../build.sh) appends to `~/.zprofile`
- [`build.sh`](../build.sh) overwrites `/opt/homebrew/include/cpprest/streams.h`

Why it matters:

- This violates user trust.
- It makes install behavior feel unsafe and machine-specific.

Preferred fix:

- `build.sh` should only check dependencies, configure, and build.
- If `cpprestsdk` needs a patch, keep that patch inside the repo or build tree.
- If the user must update shell configuration, print the command and ask them to run it manually.

### 2. Remove Hard-Coded Local Toolchain Assumptions

Current concern:

- [`CMakeLists.txt`](../CMakeLists.txt) hard-codes `/opt/homebrew`
- [`src/Utils.cpp`](../src/Utils.cpp) hard-codes Homebrew paths into `PATH`

Why it matters:

- It makes the project read as "works only on the author's machine layout".
- It creates friction even for other macOS users.

Preferred fix:

- Use normal CMake package discovery with documented overrides such as `CMAKE_PREFIX_PATH`, `LLVM_DIR`, and environment variables.
- Avoid rewriting runtime `PATH` to a fixed Homebrew layout unless explicitly configured.

### 3. Make Prompt / Response Logging Explicit And Safe

Current concern:

- [`src/Client.cpp`](../src/Client.cpp) logs requests and responses to disk by default

Why it matters:

- `hen` may process private code, prompts, traces, and other sensitive project material.
- Default logging without very clear disclosure will make some users distrust the project.

Preferred fix:

- Gate full request / response logging behind an explicit debug or tracing option.
- Document where logs are written and what they contain.
- Ensure API keys, auth headers, and similar secrets are never persisted.

### 4. Clean Up Repository Hygiene

Current concern:

- [`.gitignore`](../.gitignore) only ignores `.DS_Store`

Why it matters:

- The repo naturally produces build outputs, logs, and generated state.
- Contributors will otherwise get noisy dirty worktrees and accidental junk commits.

Preferred fix:

- Ignore build products, Xcode/CMake artifacts, logs, generated runtime state, and any repo-local temporary outputs that should not be versioned.

### 5. Remove Personal Absolute Paths And Local Examples

Current concern:

- [`Environment/Debugger/Scripts/upload_hf_jsonl.sh`](../Environment/Debugger/Scripts/upload_hf_jsonl.sh) includes `/Users/georvn/...` example paths
- Similar machine-local strings should be removed repo-wide

Why it matters:

- This makes the project look unprepared for outside users.
- It signals that the docs have not been cleaned for publication.

Preferred fix:

- Replace personal paths with repo-relative paths or placeholders.
- Sweep the repo for `/Users/...`, `/opt/homebrew`, and similar local assumptions.

### 6. Set Expectations Clearly In The README

Current concern:

- The current [`README.md`](../README.md) describes usage, but it should state the operational constraints more explicitly

Why it matters:

- Users are much more forgiving of rough edges when the project is honest about what it is.

Preferred fix:

- State clearly that `hen` is macOS-first.
- State clearly that it is experimental and stateful.
- State clearly whether it writes logs, debug artifacts, and generated working state to disk.

## Can Wait Until After Release

These can be improved later without blocking an OSS launch:

- splitting oversized source files
- refactoring the debugger into smaller action classes
- reducing hidden mutable state with scoped guards
- making the architecture more uniform and aesthetically clean
- cross-platform support beyond the currently tested environment

## Practical Release Standard

The right bar for open-sourcing `hen` is not "beautiful architecture".

The right bar is:

- no surprising machine mutation
- no unsafe default handling of private user material
- no obviously personal machine assumptions in public docs
- a build and README that another engineer can follow without reverse-engineering the author's setup

If those are handled, `hen` can be released as an honest, interesting, debugger-centered experimental system even if the internals are still rough.
