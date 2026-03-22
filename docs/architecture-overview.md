# hen Architecture Overview

This document is a high-level map of `hen` for new contributors and AI agents.
It explains the main workflows and the main components without trying to fully specify every internal detail.

## Framing

`hen` is not a single-shot code generator. It is a stateful coding agent built around:

- long-horizon decomposition and code generation
- compile-repair loops
- grounded debugger-driven fixing
- persistent trajectories and logs
- synthetic-data distillation from successful debug sessions

The border between some components is still blurry. That is historical rather than accidental: several pieces started as direct workflow support and only later became recognizable subsystems. This document describes the conceptual architecture as it exists today, even where the implementation is still spread across multiple files.

## Main Workflows

### Planning, decomposition, and code generation

The main logic lives in [`src/CCodeNode.cpp`](../src/CCodeNode.cpp), especially `CCodeNode::decompose()`.

- `defineFunction()` establishes the function contract: name, signature, role, and how it fits into the current project graph.
- `tryToImplement()` is the optimistic path. It is only attempted under the right conditions, mainly when the function is deep enough in the graph and its required application-defined types already exist, before the system commits to a deeper breakdown.
- If that optimistic path is not enough, `decompose()` expands the function into smaller responsibilities, searches for reusable library or project functionality, and plans child calls.
- During this phase the agent also resolves name conflicts, API mismatches, and graph-level consistency issues before finalizing the implementation.
- Implementation is not just "write code once". It may refactor truncated or incomplete sources, regenerate parts of the function, and update surrounding interfaces.
- Data types and supporting definitions are handled alongside function generation, not as a fully separate later phase.
- After generation, the node goes through syntax checks, source reviews, and escalation loops until the function is usable enough to move into compilation and integration.

The important idea is that decomposition is the real construction pipeline for a function. It defines the contract, builds the subgraph, writes source, repairs local issues, and persists the result into the project structure.

### Compilation and compile repair

Compilation is the stage that turns plausible generated code into code that actually builds.

- The central logic is in `CCodeNode::reviewCompilation()`.
- Compile-repair context is synthesized from the current function source, compiler output, referenced APIs, data definitions, neighboring graph state, and previous repair attempts.
- This is why compile repair is stronger than a normal "fix compiler errors" pass. The agent is not only reading an error string; it is rebuilding the local implementation context around the failing function.
- The compilation path has different repair tiers:
  - optimistic: a lightweight first pass that tries to repair the current function with minimal disruption. It is used for the earliest attempts when the system believes a local fix may be enough.
  - regular: the normal repair flow that allows broader context, richer review, and more substantial code updates. This is the default tier once the narrow optimistic fix path is no longer sufficient.
  - panic: the final repair tier used near the end of the retry budget. It assumes the local view may be too weak and allows a wider, more forceful recovery attempt.
- Repeated failures within a tier escalate to a more powerful LLM before the system gives up or moves to a harsher repair mode.
- Escalation can also widen the repair scope, pull in more context, or allow broader changes such as new helpers or refactors when a narrow patch is not enough.

The practical goal of this phase is not just local syntax correctness. It is end-to-end successful compilation and linkage of the project so `hen` can produce the final main executable.

### Debugging

Debugging is the most distinctive and architecturally strongest part of `hen`: a compact action vocabulary wrapped around a grounded runtime workflow.

- The test framework provides structured tests with `pretest`, main `test`, and `posttest` phases, along with expected exit codes, regex checks, input files, and output files. The main test-framework manual/prompt lives in [`Environment/Prompts/TestFramework.txt`](../Environment/Prompts/TestFramework.txt).
- [`src/CCodeProject.cpp`](../src/CCodeProject.cpp) drives the concrete workflow through `CCodeProject::debugTests()`, which prepares test runs, archives old trajectories, manages unit-test and full-test ramps, and invokes the debugger.
- [`src/Debugger.cpp`](../src/Debugger.cpp) implements the main debugger loop. This is not free-form chat. It is a constrained action system with persisted steps.
- Typical flow is:
  - run a test
  - gather grounded evidence
  - choose and validate the next action
  - inspect or fix a function
  - force another test run
- Grounded information can include:
  - test command output
  - LLDB output
  - source snapshots
  - function and data summaries
  - file and call-graph information
  - previous trajectory steps
  - focused log slices
- System analysis tries to explain the failure before editing.
- Information-gathering actions are deliberately narrow. They are used to collect missing evidence before a fix is attempted.
- Fixing usually happens function-by-function, but debugging can still trigger refactors or helper creation when the failure is structural.
- Next-step validation constrains the loop so the debugger cannot wander arbitrarily after a run.
- After visible tests pass, reward-hacking analysis checks whether the implementation is only satisfying the surfaced tests while violating the real intent. This can include executing private tests and feeding back only a limited hint from those failures rather than exposing the full hidden test.
- A test is only considered truly successful when the visible behavior passes and the run also survives the reward-hacking checks.

The core idea is that debugging in `hen` is evidence-driven. The model is not asked to "guess a fix"; it is asked to act inside a loop that keeps collecting runtime ground truth.

### Synthetic data distillation

Distillation turns successful debugger trajectories into training data.

- The main idea is not to train on raw transcripts. The goal is to extract and optimize the most informative parts of a full debug session so the distilled data teaches the model to identify the next important blocker and the next action most likely to move quickly toward a passing result.
- The pipeline analyzes the full trajectory, the test context, the logs, and the source history.
- Repeated `fix_function` sequences for the same function are merged so the distilled result reflects the real blocker-and-fix story rather than every intermediate false start.
- The distillation pipeline nominates blockers, filters out low-value noise, and focuses on the steps that actually changed the outcome.
- `Distillery::goTo()` is the "time travel" mechanism. It reconstructs earlier project state inside a temporary workspace so a specific step can be re-examined in context.
- `DebugContextProvider` rebuilds grounded context for distillation, including step history, test commands, outputs, expected results, and regex expectations.
- `Distillery::distillFixTrack()` distills individual fix sequences one at a time.
- Trajectory compression happens in two places: `Debugger::optimizeTrajectory()` keeps the saved debug trajectory manageable during the run, and distillation later optimizes individual fix tracks so the final training sample is shorter, cleaner, and more instructionally useful than the raw run.
- The current outputs include:
  - system analysis training data (SFT)
  - next debug action training data (SFT)
  - direct preference optimization data (DPO)

The overall point of distillation is to preserve the value of `hen`'s long grounded trajectories after the run is over.

## Components

### Client

The client is the agent-facing runtime entrypoint.

- It parses user commands.
- It routes inference requests by intent.
- It selects which LLM role should handle a given phase.
- It maintains session-level state such as request metadata, logging paths, and the active project.

In practice, the client is the orchestration layer that turns interactive use into concrete project actions.

### Server

The server is the provider-facing side of the system.

- It normalizes requests to different vendors.
- It holds API keys and provider-specific transport behavior.
- It converts provider responses back into the internal shape expected by the rest of `hen`.
- It also performs usage accounting and related request bookkeeping.

This boundary matters because parser or provider-shape bugs can look like debugger failures even when the model output was fine.

### LLM Registry

The LLM registry is the configured model catalog, mainly in [`Environment/LLRegistry.json`](../Environment/LLRegistry.json).

- It records providers, model names, token limits, reasoning settings, and cost metadata.
- It defines which models are available for which roles.
- It is the configuration layer that lets the same architecture run with different model lineups.

### LLM Party: Director, Expert, Developer, Debugger

`hen` does not treat "the model" as one undifferentiated role.

- Director: the strongest high-level planner and chooser for hard decisions.
- Expert: the broad algorithmic and architecture model.
- Developer: the fast specialist for concrete implementation work.
- Debugger: the model used mainly for grounded debug analysis and debugger step selection.

This role split is part of the architecture, not just a prompt detail. Different phases deliberately use different model capabilities, although the split is not absolute. For example, debugger step analysis and source-fix synthesis can be routed through different roles.

### DAG

The project is represented as a function/data graph rather than just a folder of files.

- The DAG captures dependencies between generated nodes.
- It provides stable traversal and persistence structure.
- It is also tied to the on-disk project state under `dag/`, including git-backed snapshots and rollback points.

This is how `hen` keeps long-horizon generation incremental instead of rewriting everything from scratch at every step.

### Build system

`hen` has its own agent-oriented build layer on top of the generated source tree.

- It does not rely only on an external project generator to decide what to rebuild during inference-time iteration.
- Functions are generated as separate source units, compiled into object files, and then linked into the main executable.
- The build layer tracks per-node source and referenced-data hashes, stores cached object files, and restores them when a node has not materially changed.
- In practice this lets `hen` rebuild only the sources affected by new inference output or upstream dependency changes instead of recompiling the whole project every time.

This custom build behavior is important for fast iteration because `hen` repeatedly generates, repairs, tests, and debugs the project inside the same long-running workflow.

### Linter

There is no single isolated "Linter" subsystem class. Conceptually, the linting role is spread across:

- syntax and compile checks
- code review prompts
- static source inspection
- compiler diagnostics

So "linter" is best understood as a cross-cutting validation layer rather than one standalone component.

### Debugger

The debugger is the grounded action engine of `hen`.

- It runs tests.
- It gathers runtime evidence.
- It validates the next action.
- It fixes issues in a constrained loop.
- It persists the full trajectory for later resume and distillation.

This is the most important subsystem in the repository.

### Test framework

The test framework defines what "correct behavior" means for the generated project.

- Tests are structured, not ad hoc.
- They support `pretest`, `test`, and `posttest` phases.
- They can validate command results, stdout regexes, input files, output files, and reward-hacking-sensitive behavior.

The debugger depends on this structure to turn failures into grounded evidence.

### Execution environment

The execution environment is the concrete runtime surface where `hen` verifies its work.

- It includes build directories, generated source trees, test working directories, instrumented builds, shell commands, and LLDB sessions.
- It is where code stops being a text artifact and becomes something the system can actually compile, run, inspect, and debug.

### Tracer

"Tracer" is currently more a role than a single named class.

- Runtime traces come from instrumented runs, LLDB sessions, command outputs, saved step artifacts, and related debug evidence.
- These traces are what let the debugger and the distillery reason about actual execution instead of only about source text.

### Logger

"Logger" is also cross-cutting rather than one isolated class.

- `hen` persists request/response logs, debugger artifacts, test outputs, summaries, and trajectory step data.
- Those logs are operationally important: they support resume, inspection, debugging, and synthetic-data distillation.

### Distillery

The distillery is the post-debug data-extraction subsystem.

- It loads saved trajectories.
- It reconstructs project state at chosen steps.
- It analyzes which parts of the run mattered.
- It emits higher-quality training examples from successful debug sessions.

This is the subsystem that turns `hen` from only an agent into an agent that can also generate its own grounded training data.

## Key Source Anchors

For readers who want to jump from this overview into the implementation, start here:

- [`src/Client.cpp`](../src/Client.cpp)
- [`src/Project.cpp`](../src/Project.cpp)
- [`src/CCodeNode.cpp`](../src/CCodeNode.cpp)
- [`src/CCodeProject.cpp`](../src/CCodeProject.cpp)
- [`src/Debugger.cpp`](../src/Debugger.cpp)
- [`src/Distillery.cpp`](../src/Distillery.cpp)
- [`src/DebugContextProvider.h`](../src/DebugContextProvider.h)
- [`Environment/Debugger/Prompts`](../Environment/Debugger/Prompts)

## Short Summary

At a high level, `hen` works like this:

1. It decomposes a project into a DAG of functions and data.
2. It generates and repairs source until the code builds.
3. It debugs the result through a grounded persisted action loop.
4. It saves the full trajectory and logs.
5. It distills the valuable parts of that trajectory into training data.

That combination of generation, grounded debugging, persistence, and distillation is the core architectural idea of `hen`.
