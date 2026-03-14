# Future Debugger Notes

## Purpose

This document is the current engineering assessment of the debugger-centered `hen` architecture.

Use it when touching:

- [`src/Debugger.cpp`](../src/Debugger.cpp)
- [`src/CCodeNode.cpp`](../src/CCodeNode.cpp)
- [`src/CCodeProject.cpp`](../src/CCodeProject.cpp)
- [`src/Project.cpp`](../src/Project.cpp)
- [`src/Inferencing.cpp`](../src/Inferencing.cpp)

## What To Preserve

- The persisted debugger action loop in [`src/Debugger.cpp`](../src/Debugger.cpp): `run_test` -> gather evidence -> choose constrained next action -> `fix_function` -> forced `run_test`.
- Trajectory persistence under `debug/<test>/trajectory/step_N/`, including resumability and compact summarization.
- The separation of responsibilities between decomposition, compile repair, and runtime debugging:
  - `CCodeNode::decompose()`
  - `CCodeNode::reviewCompilation()`
  - `Debugger::debug()`
  - `CCodeProject::debugTests()`
- Prompt specialization by phase. Debugger prompts and codegen prompts should stay distinct.
- DAG-backed persistence and the ability to archive, resume, and inspect prior runs.

## Current Weaknesses

The primary weakness is not the overall architecture. It is hidden mutable state at system boundaries plus oversized orchestration functions.

### Hidden Mutable State

These are the main ambient state channels today:

- Active context selection in [`src/Project.cpp`](../src/Project.cpp) via `m_activeContext`.
- Static prompt search paths and prompt cache in [`src/Inferencing.cpp`](../src/Inferencing.cpp) via `Prompt::m_searchPaths` and `Prompt::m_cache`.
- Client singleton runtime state in [`src/Client.cpp`](../src/Client.cpp): selected LLM, prompt label, log directory, request ID, and project ID.
- Build-mode switching in [`src/Debugger.cpp`](../src/Debugger.cpp): swapping `build`, `build_backup`, and `build_instrumented`.
- Persisted debug and test state on disk: trajectories, logs, traces, cached summaries, and test working directories.
- DAG repo mutation in [`src/CCodeProject.cpp`](../src/CCodeProject.cpp): commits, branch creation, revert, hard reset, and reload.

The practical consequence is that many failures can be caused by stale or mis-restored state rather than by model behavior.

### Oversized Control Functions

These functions carry too much responsibility and are high-risk edit points:

- `CCodeNode::decompose()` in [`src/CCodeNode.cpp`](../src/CCodeNode.cpp)
  It defines the function contract, plans child calls, may implement speculatively, describes the function, implements it, defines data, verifies it, saves it, and commits it.
- `CCodeNode::reviewCompilation()` plus `CCodeNode::compile()` in [`src/CCodeNode.cpp`](../src/CCodeNode.cpp)
  This is not just local compile repair. It can pull in more API, spawn new helper functions, refactor the graph, and retry under multiple repair modes.
- `Debugger::debug()` and `Debugger::executeNextStep()` in [`src/Debugger.cpp`](../src/Debugger.cpp)
  They combine prompt setup, context switching, trajectory resume/save, build validation, action execution, step validation, and next-step inference.
- `CCodeProject::debugTests()` in [`src/CCodeProject.cpp`](../src/CCodeProject.cpp)
  It mixes test ramp orchestration, unit-test generation, debugger invocation, trajectory archiving, DAG branch management, and destructive rollback.

## Refactor Direction

The right direction is additive hardening, not flattening `hen` into a generic coding agent.

### Prefer Explicit State Ownership

- Pass explicit contexts or session objects where practical instead of relying on ambient active-context switching.
- Resolve prompts through explicit phase-local resolvers rather than process-global search-path mutation.
- Prefer explicit build roots or build-mode objects over directory renaming conventions.

### For Unavoidable Mutation, Make Restoration Automatic

Use scoped guards / RAII for:

- active context switching
- prompt search-path switching
- temporary LLM overrides
- build-mode transitions

If a code path can return early, restoration should still happen automatically.

### Split Oversized Flows By Boundary

Good splits would look like:

- `decompose()`:
  - define signature and brief
  - request architectural subcalls
  - resolve conflicts with existing graph
  - implement and verify
  - persist and report
- `reviewCompilation()`:
  - analyze compiler output
  - choose repair strategy
  - gather extra API
  - update source
  - compile and record progress
- `debugTests()`:
  - test-ramp iterator
  - unit-test ramp executor
  - full-test executor
  - rollback/archive policy

Keep the behavior the same while reducing the amount of state any single function has to manage.

## Specific Risks To Watch

### Prompt Cache Behavior

`Prompt::m_cache` is keyed by the prompt file name argument. If two prompt trees use the same relative name, stale content can leak across phases unless prompt resolution is handled carefully.

### Context Symmetry

Manual `captureContext()` / `popContext()` pairing is fragile. Early returns in decomposition, compile repair, or debugger step selection can leave the wrong context active or leave stale messages in scope.

### Build Switching

Debugger instrumentation currently depends on moving directory names around. If a failure happens between rename steps, subsequent compile or test flows can run against the wrong tree.

### Test Workflow Rollback

`CCodeProject::debugTests()` intentionally uses destructive DAG reset when it decides a unit test is broken. Keep that behavior narrowly scoped and evidence-driven.

## Recommended First Refactors

1. Introduce scoped guard helpers for context switching, prompt search paths, and temporary LLM overrides.
2. Make prompt resolution/cache keys path-aware instead of relying on prompt basename alone.
3. Extract smaller orchestration helpers out of `CCodeNode::decompose()`, `CCodeNode::compile()`, and `CCodeProject::debugTests()`.
4. Replace build-directory swapping with explicit build roots or a build-mode abstraction when feasible.
5. Keep trajectory compatibility and debugger action semantics stable while refactoring internals.

## Validation Expectations

Minimum validation should match the boundary changed:

- Decomposition change: exercise at least one real decompose/build path.
- Compile-repair change: exercise at least one real compile-fix loop.
- Debugger change: exercise at least one real step sequence and a `run_test` or `debug_function` path.
- Prompt/context change: verify prompt resolution and context restoration across at least one codegen flow and one debugger flow.

## Current Assessment

Short version:

- `hen` has a strong debugger-centered architecture.
- The architecture is worth preserving.
- Most current risk comes from hidden mutable state and oversized control functions.
- The right long-term move is to harden boundaries so cleanup and restoration become automatic or unnecessary by construction.
