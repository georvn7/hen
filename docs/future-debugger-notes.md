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
- The small explicit action vocabulary. Keep the debugger constrained around actions such as `run_test`, `debug_function`, `fix_function`, `function_info`, `data_info`, `call_graph`, and `log_info` rather than flattening it into freeform inspection and editing.
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

### Provider Normalization Boundary

Provider transport and response normalization are still too entangled with agent logic.

The critical boundary is centered around:

- [`src/Project.cpp`](../src/Project.cpp): `inference()`, `sendRequest()`, and `handleResponse()`
- [`src/Client.cpp`](../src/Client.cpp): request routing and session-level request metadata

Problems here can present as debugger failures even when the model produced a valid answer. Different providers can vary in:

- `message.content` shape
- tool / function call fields
- JSON embedded in strings
- structured content arrays

This should remain a first-class refactor target separate from debugger policy changes.

### Token Accounting And Vendor Pricing

`hen`'s request-cost display is normalized in [`src/Server.cpp`](../src/Server.cpp) and rendered in [`src/Client.cpp`](../src/Client.cpp). That normalization is currently too weak to reflect vendor pricing accurately.

Current limitations:

- [`LLMConfig`](../src/LLMConfig.h) only stores `input_tokens_price` and `output_tokens_price`.
- [`Server::updateUsage()`](../src/Server.cpp) hardcodes a few provider/model cache multipliers instead of using explicit per-model cache prices.
- [`Client::setStepCost()`](../src/Client.cpp) may keep the `cached` display bucket combined, but the internal accounting still needs to distinguish cache reads from cache writes.
- Some vendors bill reasoning / thinking tokens as output. Those tokens must be included in billed output even if the visible response text is shorter.

What needs to be fixed to correctly reflect vendor pricing for the currently important providers:

- OpenAI
  - Support both response shapes:
    - Chat Completions style: `prompt_tokens`, `completion_tokens`, `prompt_tokens_details.cached_tokens`
    - Responses style: `input_tokens`, `output_tokens`, `input_tokens_details.cached_tokens`, `output_tokens_details.reasoning_tokens`
  - Do not rely on provider-wide heuristics for cached pricing. Cached input price is model-specific.
  - Treat reasoning tokens as billed output. For OpenAI this is already part of billed output usage even when broken out separately in `output_tokens_details.reasoning_tokens`.

- Anthropic
  - Continue reading `input_tokens`, `cache_creation_input_tokens`, `cache_read_input_tokens`, `output_tokens`.
  - Also parse the cache-creation breakdown object so 5-minute and 1-hour cache writes can be billed correctly. A single `cache_write_tokens` bucket is not enough for Anthropic pricing.
  - Treat extended-thinking usage as billed output even when the visible response contains only a summary of the thinking content.

- Google
  - Parse `cachedContentTokenCount` as cached prompt usage.
  - Parse `thoughtsTokenCount` and add it to billed output. `candidatesTokenCount` alone undercounts request cost on thinking models.
  - Distinguish explicit context-caching request charges from cache-storage charges. Google explicit caching has both reduced-price cached tokens and a separate storage fee, so `step_credits` is incomplete if it only uses per-request tokens.
  - Keep in mind that implicit caching and explicit caching are different products and should not be normalized as if they were the same billing mechanism.

- Groq
  - Keep using the OpenAI-compatible usage shape when available (`prompt_tokens`, `completion_tokens`, `prompt_tokens_details.cached_tokens`).
  - Apply cached-input discounts when cache hits are actually reported. Groq prompt caching is not a write-priced system like Anthropic or Google explicit caching.
  - Do not assume prompt caching exists uniformly across all Groq models; apply the discount based on actual cached-token usage rather than provider name alone.

Recommended data-model change:

- Add explicit cached-pricing fields to [`LLMConfig`](../src/LLMConfig.h) and [`Environment/LLRegistry.json`](../Environment/LLRegistry.json) instead of inferring cached prices from `input_tokens_price`.
- At minimum:
  - `cache_read_tokens_price`
  - `cache_write_tokens_price`
- If needed for Anthropic and Google explicit caching:
  - `cache_write_5m_tokens_price`
  - `cache_write_1h_tokens_price`
  - `cache_storage_price_per_mtok_hour`

Implementation note:

- The user-visible step hint can still show `in / cached / out`.
- The pricing path in [`src/Server.cpp`](../src/Server.cpp) should nevertheless charge:
  - uncached input
  - cache writes
  - cache reads
  - output, including reasoning / thinking tokens where the vendor bills them as output

Verification note:

- Pricing references for these vendors change over time. When updating token accounting, re-check the official pricing and usage-field docs for OpenAI, Anthropic, Google, and Groq before changing [`Environment/LLRegistry.json`](../Environment/LLRegistry.json).

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

### Do Not Flatten Away The Debugger

Do not refactor `hen` into a generic single-shot coding agent. The debugger is not incidental glue. The core value is:

- runtime-evidence-driven debugging
- constrained step selection
- forced verification after edits
- persisted step-by-step state

The goal is to harden the existing debugger-centered architecture, not to replace it with a looser chat-style loop.

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

### Make Debugger Actions First-Class Units

The debugger already behaves like a set of action handlers. The code should move in that direction explicitly.

Useful extraction targets include:

- `RunTestAction`
- `DebugFunctionAction`
- `FixFunctionAction`
- `FunctionInfoAction`
- `DataInfoAction`

The initial goal is not a framework rewrite. It is to reduce the amount of unrelated mutable state any one action implementation has to coordinate.

## Specific Risks To Watch

### Prompt Cache Behavior

`Prompt::m_cache` is keyed by the prompt file name argument. If two prompt trees use the same relative name, stale content can leak across phases unless prompt resolution is handled carefully.

### Context Symmetry

Manual `captureContext()` / `popContext()` pairing is fragile. Early returns in decomposition, compile repair, or debugger step selection can leave the wrong context active or leave stale messages in scope.

### Build Switching

Debugger instrumentation currently depends on moving directory names around. If a failure happens between rename steps, subsequent compile or test flows can run against the wrong tree.

### Repo Recovery And Rollback

`CCodeProject::debugTests()` intentionally uses destructive DAG reset when it decides a unit test is broken. More broadly, repo recovery after failed fixes is a high-risk boundary.

Keep destructive rollback:

- narrowly scoped
- evidence-driven
- separated from ordinary hot-path repair when possible

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
