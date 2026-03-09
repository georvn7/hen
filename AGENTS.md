# AGENTS.md

## Purpose

This repository hosts `hen`, a long-horizon C++ coding agent with a first-class debugger and trajectory/distillation workflow.

Future agents working in this repo should treat it as a stateful agent system, not as a simple C++ application. Changes in transport, context handling, build switching, or debugger state can have second-order effects that are not obvious from a local edit.

## Working Assumptions

- The most important subsystem is the debugger pipeline in [`src/Debugger.cpp`](src/Debugger.cpp).
- The inference and provider-normalization boundary is mainly in [`src/Project.cpp`](src/Project.cpp) and [`src/Client.cpp`](src/Client.cpp).
- The concrete project pipeline is implemented in [`src/CCodeProject.cpp`](src/CCodeProject.cpp).
- Prompt files live under [`Environment/`](Environment), especially [`Environment/Debugger/Prompts`](Environment/Debugger/Prompts).
- Repo-generated state under [`SimpleC/`](SimpleC) can be large, noisy, and intentionally persistent.

## High-Level Architecture

`hen` is organized around several interacting layers:

1. `Project` and `CCodeProject`
   These manage the project graph, context, inference calls, build flow, and persistence.

2. `Debugger`
   This runs a constrained step loop over actions like `run_test`, `debug_function`, `fix_function`, and information-request actions.

3. `Client` and `Server`
   These handle local orchestration, LLM routing by intent, proxy/server behavior, and provider communication.

4. `SimpleC`
   This is both a sample target project and a working state area containing generated code, tests, logs, debug artifacts, and trajectories.

## Files To Read First

When starting work, prioritize these files:

- [`README.md`](README.md)
- [`src/Project.cpp`](src/Project.cpp)
- [`src/Client.cpp`](src/Client.cpp)
- [`src/CCodeProject.cpp`](src/CCodeProject.cpp)
- [`src/Debugger.cpp`](src/Debugger.cpp)
- [`docs/future-debugger-notes.md`](docs/future-debugger-notes.md)

## Session Start Checklist

At the start of a session:

1. Run `git status --short` and classify the dirty state before editing anything.
2. Read [`docs/future-debugger-notes.md`](docs/future-debugger-notes.md) if the task touches debugging, inference, context handling, or build switching.
3. Determine which boundary the task is really about:
   - provider/response parsing
   - prompt/context handling
   - debugger state machine
   - instrumentation/build mode
   - code generation/project graph
4. Prefer the smallest reproduction path available.
5. Avoid mixing architectural cleanup with bug fixing unless the task explicitly requires both.

## Debugger Mental Model

The debugger is a persisted action loop.

Typical flow:

1. `CCodeProject::debugTests()` invokes `Debugger::debug()`.
2. `Debugger::debug()` prepares prompt context, test working directories, and cached debug info.
3. `executeNextStep()` runs the current action.
4. The next action is validated and either forced or requested from the LLM.
5. Trajectory state is saved under `debug/<test>/trajectory/step_N/`.

Key action types:

- `run_test`
- `debug_function`
- `fix_function`
- `function_info`
- `data_info`
- `file_info`
- `functions_summary`
- `call_graph`
- `log_info`
- `step_info`
- `search_source`

Important constraint:

- After `fix_function`, the debugger forces `run_test`.

## File Responsibility Map

Use this as the default ownership map when narrowing a problem.

- [`src/Project.cpp`](src/Project.cpp)
  Inference request plumbing, schema instrumentation, response parsing, context operations.

- [`src/Client.cpp`](src/Client.cpp)
  LLM selection by intent, proxy/client behavior, request logging, session-level runtime settings.

- [`src/CCodeProject.cpp`](src/CCodeProject.cpp)
  Concrete C++ project workflow, build flow, test/debug entrypoints, DAG repo commit/revert helpers.

- [`src/Debugger.cpp`](src/Debugger.cpp)
  Debug action loop, LLDB/test execution, trace/log analysis, instrumentation, trajectory persistence.

- [`src/Inferencing.cpp`](src/Inferencing.cpp)
  `Prompt` and `Context` infrastructure, including global prompt search-path behavior.

- [`Environment/Debugger/Prompts`](Environment/Debugger/Prompts)
  Debugger-specific prompting behavior.

- [`Environment/Prompts`](Environment/Prompts)
  Main codegen, planning, review, and test-generation prompting behavior.

## Risk Areas

These areas are fragile and require extra care:

### 1. Provider response parsing

Files:

- [`src/Project.cpp`](src/Project.cpp)
- [`src/Client.cpp`](src/Client.cpp)

Notes:

- Different providers return different shapes for `content`, `function_call`, and `tool_calls`.
- Bugs here can look like debugger failures even when the model output is correct.

### 2. Context management

Files:

- [`src/Project.cpp`](src/Project.cpp)
- [`src/CCodeProject.cpp`](src/CCodeProject.cpp)
- [`src/Debugger.cpp`](src/Debugger.cpp)

Notes:

- `captureContext()` / `popContext()` symmetry matters.
- Active context switching is mutable global-like state inside the project.

### 3. Prompt search-path switching

Files:

- [`src/Inferencing.cpp`](src/Inferencing.cpp)
- [`src/Debugger.cpp`](src/Debugger.cpp)
- [`src/CCodeProject.cpp`](src/CCodeProject.cpp)

Notes:

- Prompt search paths are static global state.
- Always restore prompt search paths after specialized flows.

### 4. Instrumented build switching

Files:

- [`src/Debugger.cpp`](src/Debugger.cpp)

Notes:

- The debugger swaps between `build` and `build_instrumented`.
- Failures or interruptions can leave the working tree in an unexpected state.

### 5. Repo mutation and rollback

Files:

- [`src/CCodeProject.cpp`](src/CCodeProject.cpp)

Notes:

- Some debugger recovery paths revert the DAG repo to a prior commit.
- Be careful with any change that broadens or automates destructive reset behavior.

## Incident Workflow

When the system is failing and the cause is unclear, use this order.

1. Preserve evidence before patching.
   Capture the failing request/response shape, the selected next step, and any `lldb.log`, `summary.txt`, `nextStep.json`, `dbgStep.json`, or test working directory artifacts if they exist.

2. Classify the failure by boundary.
   Most failures belong primarily to one of these:
   - provider response normalization
   - schema/prompt contract
   - context leakage or wrong active context
   - debugger action validation/execution
   - instrumentation/build-mode swap
   - test fixture or generated state drift

3. Reproduce with the smallest path.
   Prefer a single inference call, a single debugger step, or a single unit test over a full end-to-end run.

4. Patch one boundary at a time.
   If the parser is broken, do not also rewrite prompt logic in the same change unless the prompt contract is the root cause.

5. Validate at the same boundary first, then one layer above it.
   Example:
   - parser fix -> validate response extraction directly, then run one real debugger inference
   - debugger fix -> validate one step sequence, then run a short test-debug loop

## Safe Refactor Checklist

Before making a structural change, answer these questions:

1. Does this change alter provider response assumptions?
2. Does this change alter prompt search paths or prompt cache behavior?
3. Does this change require balanced `captureContext()` / `popContext()` pairs?
4. Does this change alter when `build`, `build_instrumented`, or build cache directories are active?
5. Does this change broaden git reset, revert, or cleanup behavior?
6. Does this change affect trajectory save/load compatibility?

If the answer is yes to more than one of these, split the work if possible.

## Working Rules For Future Agents

1. Do not assume a debugger failure is a model failure.
   Check provider response shape, parser behavior, context state, and prompt state first.

2. Avoid cross-boundary edits unless necessary.
   If a change touches both provider parsing and debugger control flow, risk increases sharply.

3. Prefer local fixes over architectural rewrites unless requested.
   This repo has a lot of implicit contracts. Small, well-scoped fixes are usually safer.

4. Do not casually clean generated state.
   `SimpleC/build`, `SimpleC/debug`, `SimpleC/dag`, and `SimpleC/logs` often contain useful evidence.

5. Treat repo resets as high risk.
   Never introduce broader reset behavior without a clear reason.

6. Preserve the debugger’s core strengths.
   The action model, runtime evidence gathering, and trajectory persistence are central to the project.

7. Prefer additive hardening over speculative redesign.
   `hen` already has a strong underlying idea. Stabilize boundaries before introducing new abstractions.

8. Keep changes boundary-local where possible.
   A parser fix should live near parsing. A prompt-selection fix should live near prompt selection. Avoid "while here" edits in adjacent subsystems.

9. When touching debugger behavior, inspect both code and prompt files.
   Debugger behavior is jointly defined by C++ control flow and prompt contracts.

## Documentation For Future Refactors

Use [`docs/future-debugger-notes.md`](docs/future-debugger-notes.md) as the current engineering assessment of:

- what should be preserved
- what is brittle
- what should be refactored first

Update that document when making significant debugger or inference-stack changes.

## Prompt Map

These prompt files are especially important for debugger behavior:

- [`Environment/Debugger/Prompts/DebuggerRole.txt`](Environment/Debugger/Prompts/DebuggerRole.txt)
  High-level debugger role framing.

- [`Environment/Debugger/Prompts/Workflow.txt`](Environment/Debugger/Prompts/Workflow.txt)
  Main debugging workflow contract.

- [`Environment/Debugger/Prompts/NextStep.txt`](Environment/Debugger/Prompts/NextStep.txt)
  Next-action selection prompt.

- [`Environment/Debugger/Prompts/NextStepInstructions.txt`](Environment/Debugger/Prompts/NextStepInstructions.txt)
  Action-format and schema-following instructions.

- [`Environment/Debugger/Prompts/SystemDebugAnalysis.txt`](Environment/Debugger/Prompts/SystemDebugAnalysis.txt)
  Focused function-level debug analysis.

- [`Environment/Debugger/Prompts/RunAnalysis.txt`](Environment/Debugger/Prompts/RunAnalysis.txt)
  Test-run analysis path.

- [`Environment/Debugger/Prompts/RewardHacking.txt`](Environment/Debugger/Prompts/RewardHacking.txt)
  Post-pass review against reward-hacking behavior.

- [`Environment/Debugger/Prompts/FixFunction.txt`](Environment/Debugger/Prompts/FixFunction.txt)
  Function repair behavior.

- [`Environment/Debugger/Prompts/SummarizeTrajectory.txt`](Environment/Debugger/Prompts/SummarizeTrajectory.txt)
  Trajectory summarization/compression.

Main non-debugger prompts live in [`Environment/Prompts`](Environment/Prompts). If a change affects generation, planning, or compile-fix behavior, inspect the relevant prompt there before assuming the issue is only in C++.

## Validation Guidance

When changing code in this repo, validation should be proportional to the area touched.

Preferred checks:

1. Rebuild `hen`.
2. If touching inference parsing, run at least one known-good inference path.
3. If touching the debugger, run at least one debugger step sequence or a small unit-test debugging flow.
4. If touching prompt or context plumbing, verify that prompts still resolve and trajectories still save/load.

## Validation Matrix

Use this matrix to choose the minimum acceptable validation for a change.

- `src/Project.cpp` or `src/Client.cpp`
  Rebuild, then validate at least one real inference response shape that exercises the changed parser or transport path.

- `src/Debugger.cpp`
  Rebuild, then run at least one debugger step sequence. If the change affects execution or instrumentation, include a real `run_test` or `debug_function` path.

- `src/CCodeProject.cpp`
  Rebuild, then validate project build flow or test/debug entry flow, depending on the touched area.

- `src/Inferencing.cpp`
  Rebuild, then verify prompt resolution and context restoration in at least one debugger or codegen flow.

- Prompt files under `Environment/Debugger/Prompts`
  Exercise the corresponding debugger action or next-step path, not just a compile.

- Prompt files under `Environment/Prompts`
  Exercise the corresponding generation/review/fix path, not just a compile.

## Commands

Common commands:

```bash
./build.sh
cmake --build build --config Debug -j4
```

Typical runtime shape:

```bash
./hen -client -server -proj "/full/path/to/hen/SimpleC" -env "/full/path/to/hen/Environment" ...
```

## Repo State Hygiene

This repository is often dirty because `hen` generates artifacts during normal use.

Before editing:

- check `git status --short`
- identify whether changes are user work, generated state, or previous agent changes

Do not revert unrelated user changes.

Generated-state directories are often diagnostically useful:

- [`SimpleC/build`](SimpleC/build)
- [`SimpleC/build_instrumented`](SimpleC/build_instrumented)
- [`SimpleC/debug`](SimpleC/debug)
- [`SimpleC/dag`](SimpleC/dag)
- [`SimpleC/logs`](SimpleC/logs)

Do not delete or clean these casually during investigation.

## Current Assessment

Short version:

- `hen` has a strong debugger-centered architecture.
- Its main weakness is hidden mutable state at system boundaries.
- The right long-term direction is to harden boundaries, not to flatten the debugger into a generic code agent.
