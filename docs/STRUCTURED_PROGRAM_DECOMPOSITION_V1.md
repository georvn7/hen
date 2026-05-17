# Hen with SPD: Structured Program Decomposition Pitch v1

Last updated: May 17, 2026
Status: Proof-of-idea pitch for the next version of `hen`

## Pitch

**Hen with SPD** is a proposed next version of `hen` that generates ordinary restricted C++ which is also structured enough to be decomposed into GPU-lowerable compute shapes.

The goal is not to make a GPU magically parallelize arbitrary C++. The goal is to make `hen` generate programs in a disciplined shape from the start:

```text
ordinary restricted C++ for debugging
structured program decomposition for validation
MLIR/GPU-lowerable compute regions for execution
```

Structured Program Decomposition, or **SPD**, is the source and compiler discipline behind this idea.

SPD follows the fundamental idea of the [structured program theorem](https://en.wikipedia.org/wiki/Structured_program_theorem): general computation can be expressed through composition of sequence, selection, and iteration. SPD uses that as the control-flow foundation, then adds the restrictions needed for GPU-oriented lowering: batchable sequence regions, recoverable effects, generated materialization, and explicit continuations.

The theorem gives SPD an important conceptual guarantee: arbitrary programs can be decomposed into structured control. It does not by itself guarantee GPU efficiency or automatic lowering. Hen with SPD must make each decomposed region analyzable, bounded, effect-explicit, and repairable by validation feedback.

SPD says that every generated function must either:

```text
be a batchable lowerable shape
```

or:

```text
decompose into batchable lowerable shapes,
compiler-generated materialization,
and explicit continuations
```

If this works, `hen` becomes more than a C++ coding agent. It becomes an agent that writes debuggable C++ while also producing a program structure that can be validated, batched, scheduled, and lowered toward GPU execution.

V1 is deliberately scoped to retire the generation/validation bet first. Runtime and GPU scheduling work should be gated on whether Hen can generate, validate, repair, and differentially test SPD-shaped code.

## Motivation

Not every program is a perfect fit for parallel execution, especially on SIMD/SIMT architectures such as GPUs. Some computations have real dependencies, irregular data access, or control flow that limits useful parallelism.

But most current codebases also hide much of the parallelism that is actually available. They often serialize work because ordinary software development rewards local clarity, object ownership, call-stack convenience, and incremental mutation more than global dependency transparency.

Exposing the available parallelism requires unusual discipline:

```text
make dependencies explicit
avoid hidden state
separate compute from effects
bound local work
represent data in analyzable layouts
preserve enough structure for compiler lowering
avoid accidental sequencing
```

That discipline is difficult for human teams to maintain across large codebases. It is easy to break accidentally, expensive to review manually, and painful to retrofit after the architecture is already object-heavy and side-effect-heavy.

Hen changes the premise. An agentic system can be asked to follow unnatural coding rules, receive static validation feedback, repair its own code, run tests, debug failures, and accumulate trajectory data from those repairs. What is exhausting as a manual team convention can become part of the generation loop.

SPD is motivated by that possibility:

```text
Use Hen's existing generation, validation, debugging, and repair machinery
to maintain a program shape that humans would struggle to preserve manually.
```

The bet is not that all software becomes perfectly parallel, or that SPD can manufacture parallelism that true dependencies do not allow. The bet is that AI-generated software can expose more of the parallelism that is already present because the agent can continuously enforce the structural discipline needed for decomposition and lowering.

## What Hen With SPD Is

Hen with SPD is:

```text
a constrained code-generation system
a debugger-centered C++ agent
a structured-program decomposition experiment
a validation-and-repair loop for lowerable program shapes
a path from ordinary debuggable C++ toward MLIR/GPU execution
```

The intended innovation is not a single backend trick. It is the combination of:

```text
agent-generated restricted C++
static source validation
function decomposition
effect extraction
compiler-generated materialization
CPU reference execution
eventual MLIR/GPU lowering
synthetic training data from failed and repaired lowerability attempts
```

Hen with SPD should be judged by whether it can make generated programs correct, debuggable, and structurally lowerable at the same time.

## What Hen With SPD Is Not

Hen with SPD is not:

```text
an automatic GPU compiler for arbitrary C++
a promise that arbitrary programs run optimally on the GPU
a replacement for MLIR, LLVM, Metal, CUDA, or other backend systems
a claim that pointer-heavy STL/object code can be lowered without changing its shape
a hand-written job system disguised as code generation
a bytecode interpreter that the agent is expected to author manually
a guarantee that every generated function is GPU-lowerable in the first prototype
```

The honest claim is narrower and stronger:

```text
Hen can learn to generate restricted, debuggable C++ that is already shaped for decomposition into GPU-lowerable regions.
```

## Problem

Modern GPUs want large batches of similar work. Ordinary general-purpose C++ tends to hide that work behind:

- pointer-heavy object graphs
- implicit call-stack sequencing
- container mutation
- hidden aliasing
- dynamic control flow
- side effects mixed into compute

LLVM, MLIR, and GPU backends are powerful once the program is already structured. They do not magically recover high-level parallelism from arbitrary pointer-heavy C++.

That creates the central problem:

```text
If hen generates ordinary C++ first and asks the compiler to discover parallelism later,
the project probably collapses into CPU orchestration plus occasional GPU helper kernels.
```

SPD flips the strategy:

```text
Generate code that is already shaped for decomposition.
Use the linter/compiler to verify that shape.
Only then lower the verified structure toward MLIR/GPU execution.
```

## Why Hen Is The Right Basis

Hen already has several pieces that most code-generation systems would need to invent before SPD could even be attempted.

Existing Hen strengths:

```text
prompt contracts:
  generated code is already guided into a constrained C++ style

source checklist:
  no application classes, no globals, no exceptions, limited STL, explicit arguments/returns

linter:
  generated code is statically inspected and sent back for repair

function decomposition:
  large behavior is already split into project functions and helper functions

compile repair:
  generated code is not only produced; it is compiled, checked, and revised

unit tests:
  behavior is grounded by executable tests rather than prompt intent alone

debugger:
  Hen has a first-class loop for gathering runtime evidence and repairing specific functions

project graph:
  functions, data definitions, calls, context, and generated artifacts are already tracked as project state

trajectory/distillation workflow:
  failures and repairs can become training data for better future generation
```

This matters because SPD is not just a backend. SPD is a generation discipline plus a validation loop. Hen already has the shape of that loop.

The proposed evolution is:

```text
current Hen:
  generate restricted C++
  compile it
  lint it
  test/debug it
  repair it

Hen with SPD:
  generate restricted C++ in SPD style
  compile it
  lint it for normal Hen rules
  validate it for lowerable sequence/materialize/continuation structure
  extract effect descriptors
  generate materialization
  test ordinary C++ behavior against SPD artifacts
  lower verified shapes toward MLIR/GPU execution
```

In other words, SPD should extend Hen's existing strengths rather than replace them.

The subsystem evolution is roughly:

```text
SourceChecklist / prompts:
  add SPD mode conventions for lowerable regions, recoverable writes, and explicit continuations

CCodeAnalysis / linter:
  add effect extraction, alias restrictions, bounded-loop checks, and lowerability diagnostics

CCodeNode / decomposition:
  split functions at sequence, materialization, and continuation boundaries

CCodeProject / project graph:
  store SPD metadata beside ordinary function/data metadata

Debugger:
  keep ordinary C++ as the semantic reference, then compare it against CPU-SPD and later GPU-SPD behavior

Training/trajectory data:
  collect examples of code that is lowerable vs. code that must be repaired or decomposed
```

This is the main reason the idea is plausible inside Hen specifically. Hen already treats generated code as something that can be constrained, inspected, repaired, and debugged over time. SPD gives that process a stronger target: code that is not only correct C++, but also decomposable into GPU-lowerable shapes.

## Lowerability Feedback Loop

The most important Hen-specific advantage is that failed lowerability attempts can become training data.

A traditional compiler can reject a program, emit diagnostics, and wait for a human to rewrite the code. Hen can do something different:

```text
agent writes function
 -> SPD validator rejects or partially lowers it
 -> validator explains the non-lowerable construct
 -> agent repairs the original C++
 -> tests/debugger confirm behavior still matches
 -> before/after pair becomes trajectory data
```

This creates a compounding loop:

```text
unlowerable implementation
 -> validator diagnosis
 -> lowerable repair
 -> SFT/DPO/distillation example
 -> future agent is more likely to write SPD-shaped code on the first attempt
```

This feedback loop is one of the main reasons SPD belongs inside Hen rather than only inside a standalone compiler pass. The project is not just asking a compiler to understand more programs. It is training the generator to produce programs that are easier for the compiler to understand.

## Proposal

The proposed pipeline is:

```text
project description + tests
 -> hen-generated restricted C++ in SPD style
 -> static SPD validation and effect extraction
 -> sequence/product/materialize/continuation representation
 -> exact CPU reference path for debugging
 -> MLIR / GPU scheduler/runtime executing compatible batches
```

The core invariant is:

```text
Every generated function must either be a batchable shape or must decompose into batchable shapes, generated materialization, and explicit continuations.
```

A batchable shape is:

```text
a fixed lowerable compute/product/effect recipe whose invocations differ only by explicit values, handles, and product buffers, not by the program they execute
```

The function-level decomposition is:

```text
bounded inputs + explicit state
 -> lowerable sequence products
 -> declared materialization effects
 -> explicit continuation decisions
```

The important split is:

```text
sequence:
  the lowerable compute region; in structured iteration, this is the bounded loop body that produces next loop/control state

materialize:
  compiler-generated commit of sequence products into application-visible state

continuation:
  explicit selection, iteration, and child-work scheduling
```

The agent should write and debug normal-looking restricted C++. The SPD compiler should recover the sequence products and app-visible effects, then generate `materialize` mechanically.

For example, source may contain:

```cpp
frame.color[pixel] = color;
```

The SPD pass should recover:

```text
effect kind: indexed_write
target: frame.color
index: pixel
value product: color
conflict policy: invocation-owned
```

Then the GPU-lowered `sequence` produces product buffers, and generated `materialize` commits those products under explicit conflict rules.

## Why This Is Plausible

SPD is ambitious, but it avoids the impossible version of the problem.

The impossible version is:

```text
take arbitrary C++
recover all useful high-level parallelism
make it run well on the GPU
```

SPD chooses a narrower and more plausible contract:

```text
generate restricted C++ from the beginning
lint and repair it while it is generated
extract lowerable compute regions and recoverable effects
derive materialization mechanically
lower structured regions to MLIR/GPU kernels
keep an exact CPU path for debugging
```

What seems solid enough now:

- `hen` already uses prompt contracts, linter feedback, compile repair, tests, and debugger loops to keep generated code inside unnatural constraints.
- The existing no-classes/no-globals/no-lambdas/no-exceptions/no-`auto` direction reduces aliasing and hidden control flow.
- `sequence`/`materialize` gives a credible split between GPU-friendly compute and application-visible mutation.
- Materialization can be generated from structured effect descriptors rather than hand-written by the agent.
- MLIR is a good final lowering target once SPD has exposed structured operations.
- The first proof does not need a complete GPU backend. It can start with CPU validation, product buffers, generated materialization, and differential tests.

## Non-Goals For The First Proof

The broader "what this is not" boundary is stated above. For the first proof specifically, this pitch does not claim:

```text
arbitrary C++ can be automatically parallelized
all generated code will be GPU-lowerable immediately
STL containers can be used directly inside GPU-lowerable regions
materialization conflicts are solved without explicit policies
the first prototype needs full GPU-resident scheduling
```

The first claim is smaller:

```text
hen can generate ordinary restricted C++ that is structured enough for an SPD validator to decompose into lowerable sequences, effect descriptors, generated materialization, and explicit continuations.
```

## Relation To Prior Work

SPD is not a claim that prior compiler work missed an easy trick. Systems such as Halide, JAX, Triton, polyhedral compilers, and Mojo all point toward the same lesson: high-performance parallel execution usually requires code to be written in a constrained, analyzable shape.

SPD accepts that lesson. The difference is where the discipline lives.

```text
traditional model:
  human writes inside a constrained DSL/subset
  compiler lowers the constrained program

Hen with SPD:
  agent writes restricted C++
  linter/validator/debugger force the constrained shape
  failed lowerability attempts become repair and training data
  compiler lowers the validated SPD artifact
```

For example, Triton asks a human to write directly inside a kernel-oriented DSL. SPD keeps ordinary restricted C++ as the source/debug surface, then asks Hen's linter, validator, debugger, and repair loop to force the agent toward a similarly disciplined lowerable shape.

The goal is not to replace MLIR, LLVM, Halide-like scheduling ideas, Triton-style kernels, or Mojo-like lowerable language work. The goal is to use Hen's agentic generation and repair loop to keep ordinary debuggable C++ close enough to a lowerable shape that those backend ideas can become applicable.

## First Proof

The first useful proof should be deliberately small:

```text
Projects/clcalc_shape
 -> ordinary restricted C++ evaluator
 -> SPD validator classifies functions as lowerable / decomposable / unlowerable
 -> identify pure arithmetic sequence regions
 -> extract simple products/effects
 -> generate materialize when needed
 -> build a CPU SPD artifact/executor for the classified subset
 -> compare ordinary C++ behavior against SPD artifact behavior
```

This keeps the first experiment focused on the essential question:

```text
Can hen generate code that remains normal C++ for debugging,
but is constrained enough that SPD can verify and lower its compute shape?
```

The first proof should retire the generation/validation bet before the runtime bet.

```text
Bet A:
  Hen can generate and repair code until the SPD validator classifies it as lowerable, decomposable, or intentionally CPU-bound.

Bet B:
  A GPU runtime can execute validated SPD artifacts efficiently enough to matter.
```

V1 should focus on Bet A. No Metal-shaped runtime work should be required until the validator can cleanly classify Hen-generated functions and the CPU SPD artifact can pass differential tests against ordinary C++ behavior.

## Terms

```text
SPD:
  the source/program decomposition discipline

batchable shape:
  a fixed lowerable compute/product/effect recipe whose invocations differ only by explicit values, handles, and product buffers

SPD shape:
  a compiler-visible unit made from sequence products, declared materialization effects, and explicit continuation decisions; a batchable shape is the subset of SPD shapes whose invocations can be grouped together

sequence:
  the lowerable compute region

materialize:
  the generated commit of sequence products into application-visible state

continuation:
  explicit selected follow-up work, including selection, loop re-entry, child calls, joins, or fallback
```

Runtime representations are deliberately left open in this pitch. SPD should first define what is decomposed, validated, materialized, and lowered. A later runtime may choose dispatch records, persistent kernels, indirect commands, or another execution strategy.

## Load-Bearing Risk: Effect Grammar

The effect grammar is not just one open detail. It is load-bearing.

SPD only works if app-visible writes can be recovered as explicit materialization effects often enough to be useful, and rejected precisely enough to stay safe.

```text
too narrow:
  almost nothing useful is lowerable

too broad:
  the validator cannot decide ownership, aliasing, ordering, or conflicts

right-sized:
  common generated mutations become field writes, indexed writes, appends, reductions, and declared update policies
```

A first grammar might accept only owned field writes and invocation-owned indexed writes. That would be safe, but probably too narrow for useful generated programs. A more useful early grammar likely adds structured appends and declared associative reductions, while still rejecting unknown pointer aliasing, opaque container mutation, and writes without a recoverable conflict policy.

The first proof should therefore treat effect grammar as a measured boundary, not a solved design. For each generated function, the validator should classify the result as:

```text
lowerable:
  all sequence products and effects are recoverable

decomposable:
  some regions are lowerable, but the function must be split at control/effect boundaries

unlowerable:
  the source uses mutation, aliasing, containers, or control flow outside the current grammar
```

The useful question for V1 is not "can every function lower?" It is:

```text
Can validator feedback reliably move unlowerable code toward decomposable or lowerable code without breaking CPU behavior?
```

## Open Risks

- the exact slot/product ABI
- the first arena/container vocabulary
- materialization conflict policies
- scheduler granularity and batching strategy
- how much metadata is recovered from AST analysis versus generated alongside the code
- how efficiently the runtime can execute SPD work on a real GPU

The project is speculative, but the proof obligation is clear and testable.

The remaining sections expand the pitch into technical architecture notes. They describe the current direction, not final API commitments.

## Parallelism Target

The intent of SPD is to preserve the three major routes toward the system's maximum available parallelism:

```text
data parallelism
task parallelism
pipelining
```

Not every program has all three in equal amounts, and real dependencies still define the limit. But if the program structure hides any of these opportunities, later MLIR/GPU lowering cannot recover them reliably.

SPD gives every decomposed function a structured opportunity to expose these forms of parallelism without asking the agent to hand-write a scheduler.

At the function level, SPD separates:

```text
data parallelism:
  sequence is the lowerable compute region. Many invocations of the same sequence shape can be batched, and some sequence bodies may later lower to SIMD/SIMT-friendly MLIR/GPU operations.

task parallelism:
  selection and continuation expose independent follow-up work. When multiple selected continuations have compatible effect summaries, they can run in parallel instead of being serialized by local call order.

pipelining:
  once sequence, materialize, and continuation boundaries are explicit, a smarter MLIR/GPU/runtime lowering may overlap independent stages or batches as a later optimization.
```

The first priority is to expose true dependencies. The system should then be able to exploit whichever parallelism is actually available: data parallelism inside or across sequence batches, task parallelism across compatible continuations, and later pipelining across lowered stages.

The primary target is not only "parallelism inside one function." The primary target is to make the program expose as many independent SPD shape invocations as possible.

The scheduler should see a wide ready frontier:

```text
ready work = all shape invocations whose true dependencies are satisfied
parallelism = width of the ready frontier
minimum runtime = critical path + scheduling/memory overhead
```

The core generation rule should therefore be:

```text
Do not encode sequence unless there is a real dependency.
```

Ordinary C++ tends to hide possible parallelism in sequential control flow:

```cpp
a();
b();
c();
```

In SPD mode, if `a`, `b`, and `c` do not depend on each other, the representation should expose them as sibling dispatches:

```text
emit a
emit b
emit c
join only when a later continuation needs their results
```

This makes the scheduler's job concrete. It does not have to infer arbitrary program parallelism from opaque code; it receives a dependency-transparent SPD graph and schedules the largest currently-ready batch it can.

Intra-shape parallelism is still valuable, but it should not be the initial assumption. Parallelism inside a shape can be added later through:

- compiler lowering of recognized loops
- special primitives such as reduce, scan, map, partition, and sort
- hand-written or generated SPD intrinsics
- backend-specific kernels for hot shape classes

The first SPD architecture should succeed even if each individual shape is mostly scalar and bounded, because the main source of parallelism is the number of ready shape invocations. Later versions can refine hot shapes to expose internal SIMD/SIMT parallelism too.

This creates a three-level parallelism model:

```text
level 1, required:
  task parallelism from explicit dependencies, selections, and compatible continuations

level 2, required where available:
  data parallelism from batchable sequence shapes and lowerable sequence bodies

level 3, later optimization:
  pipelining across sequence/materialize/continuation stages or across batches
```

The first two levels are representation contracts. The third level is an optimization strategy for later lowering.

## Existing Hen Stress Test

A useful technical stress test is the current generated SimpleC compiler. It is not naturally GPU-friendly, but it demonstrates that `hen` can keep many functions inside a constrained style while building compiler-like software.

A recent SimpleC generated-source sample is a hard use case:

- 182 generated `.cpp` functions.
- Most functions are pointer/tree/string heavy.
- Many use `std::shared_ptr`, `std::string`, AST `children`, `parent`, and `type_spec`.
- This is compiler-style software: parsing, AST transforms, and assembly-string emission.

This workload is important because it is hard in the right way, but it is not expected to become massively data-parallel by wishful thinking. Recursive-descent parsing, AST rewriting, symbol-table updates, and string-heavy emission contain real dependencies. SPD should expose the parallelism that exists and identify the regions that are genuinely sequential or CPU-bound.

That means unconstrained generated C++ is not enough. The project must start from the principle that every generated function has an SPD shape or decomposes into SPD shapes. Otherwise the architecture will collapse back into CPU orchestration with occasional GPU helpers.

## Lowered SPD Artifact

`hen` should not need to ask the agent to write a job system, bytecode interpreter, or explicit GPU runtime code. The source/debug surface should remain ordinary restricted C++.

Internally, the SPD compiler may lower a function or function fragment to an artifact that has explicit inputs, outputs, products, effects, and continuations. The exact runtime API is intentionally not specified here.

The semantic contract should not change:

1. All persistent data is arena-indexed.
2. Inputs and outputs are fixed bounded slots.
3. The shape is deterministic.
4. The shape has no hidden side effects.
5. The shape performs bounded local work.
6. The shape does not call arbitrary shapes directly.
7. Control flow is explicit through predicates, selections, continuations, and joins.
8. Memory effects are declared and checkable.
9. The same SPD graph has an exact CPU execution path for debugging.
10. Independent child work is emitted as sibling dispatches, not serialized through local call order.
11. Continuations are emitted only for true data/control dependencies.

The rule can be summarized as:

```text
slots + arena snapshot -> slots + declared arena effects + explicit continuation/dispatch metadata
```

For source code, the corresponding rule is:

```text
ordinary restricted C++ function
 -> decomposed into lowerable sequence regions
 -> recoverable materialization effects
 -> explicit continuation decisions
```

## Sequence And Materialize Shape

The first lowered compute-shape target should be simpler than a full runtime, but it should still preserve the place where data-parallel work naturally lives.

The useful split is:

```text
sequence:
  GPU-lowerable data-parallel compute region, including the bounded body of a structured loop when the function is iterative

materialize:
  explicit commit from sequence products into application-visible memory/state
```

### Sequence

`sequence` is the region `hen` should try hardest to make GPU-lowerable.

It takes explicit slots, handles, and loop/control state as inputs. It produces explicit scalar outputs, predicate outputs, next-state values, and compiler-declared temporary products.

In SPD, `sequence` is compatible with the iteration part of the structured program theorem. A sequence may be run once, or it may be re-entered as the body of a structured loop. In the loop case, the sequence must produce explicit next loop/control state, and the continuation layer decides whether to re-enter the sequence or exit the loop.

The sequence contract:

- no hidden calls to arbitrary generated functions
- no implicit heap/object state
- no arbitrary application-memory mutation
- deterministic for the same inputs
- control flow is static, bounded, or compiler-visible
- loops are statically bounded or have a declared bound suitable for lowering
- branches are static, uniform, predicated, or small enough to lower into GPU-friendly select/predicate operations
- outputs are declared products of the lowered shape, not untracked side effects

The phrase "static branches and loops" means the linter/lowering pass can see and bound the control structure before GPU execution. A sequence may contain structured control, but that control must lower to GPU-friendly MLIR operations such as arithmetic/math, vector/tensor/linalg-style maps, affine/scf loops with known bounds, predication/selects, or other approved structured ops.

In compiler terms, `sequence` starts close to a lowerable basic-block/dataflow region, then grows only through structured, bounded control that the linter can prove lowerable.

The output of lowering a sequence is a product shape:

```text
scalar output slots
predicate/select outputs
next loop/control state
temporary buffers or memrefs with known element types and static/bounded sizes
declared effect descriptors for any product that must later become app-visible state
```

The important point is that buffer size and layout should be discovered from the lowered MLIR/shape products when possible. The application should not hand-write a job system or a recipe interpreter. The recipe/MLIR shape is a compiler product.

### Materialize

`materialize` is the explicit phase that maps sequence products into application-visible state.

It is the place where memory effects become concrete and checkable:

```text
owned_write
indexed_write
range_write
append
reduce
atomic/update-with-policy
ordered_update
serialize-if-unknown
```

If a sequence produces a color/depth buffer for a renderer, `materialize` is where that product becomes framebuffer/depth-buffer state. If ownership is obvious, materialization may later be fused or optimized away, but the semantic contract remains explicit.

This avoids two bad extremes:

- forbidding data-parallel sequences from producing useful memory-shaped output
- allowing arbitrary hidden writes inside the compute region

### Derived Materialize

The agent should not usually be asked to hand-write `materialize`.

The intended workflow is:

```text
hen writes/debugs restricted ordinary C++
 -> linter/lowering pass extracts app-visible write sites as effect descriptors
 -> GPU-lowered sequence produces product buffers for those effects
 -> materialize is generated mechanically from the effect descriptors
```

In other words, the source code may look like normal state-updating C++:

```cpp
frame.color[pixel] = color;
```

but the linter/lowering pass must be able to recover a structured effect:

```text
effect kind: indexed_write
target: frame.color
index: pixel
value product: color
predicate: true
conflict policy: invocation-owned
```

The GPU sequence does not directly mutate `frame.color`. It computes products such as:

```text
product.pixel[i]
product.color[i]
product.valid[i]
```

Then generated `materialize` commits those products:

```text
for each product item:
  if valid:
    frame.color[pixel] = color
```

This keeps the `hen` debugging surface ordinary and CPU-visible while still giving the GPU lowering path a clean split between compute products and app-visible effects.

The required invariant is:

```text
Every app-visible update written by the agent must be statically recoverable as a declared materialization effect.
```

Allowed effect patterns should start small and explicit:

```text
field write with owned object/handle
indexed write with provably invocation-owned index
bounded range write
append through a known arena API
declared associative reduction
declared atomic/update-with-policy
```

Rejected or serialized patterns include:

```text
unknown pointer aliasing
unstructured raw pointer writes
hidden writes through opaque calls
virtual/dynamic dispatch with effects
unknown container mutation
writes whose target/index/conflict policy cannot be recovered
```

If the effect cannot be extracted, the function is not lowerable in this mode. It must either be rewritten into recoverable effect syntax, decomposed so the effect is isolated, or remain on the CPU path.

### Effect Extraction And The Linter

SPD depends on static validation. Anything fuzzy must either be made syntactically recoverable or rejected by the linter.

The existing `hen` linter already has useful foundations:

```text
no application classes
no application member functions
no lambdas
no globals/statics/externals for application state
no exceptions
no function pointers in application data
no auto in SPD-lowerable code
limited STL/container vocabulary
explicit function arguments and returns
```

The SPD extension should add an effect extraction pass. Instead of only asking "is this C++ allowed?", the linter must also ask "can this write be recovered as a materialization effect?"

Useful first-class linter facts:

```text
effect kind:
  field_write
  indexed_write
  range_write
  append
  reduce
  atomic/update-with-policy
  unknown_mutation

target root:
  function argument
  return/product object
  arena handle
  local-only value

access path:
  object.field
  buffer[index]
  arena.slice[offset]
  container operation

context:
  predicate/branch condition
  loop bounds
  helper function effect summary
  conflict policy
```

Examples:

```cpp
node->value = x;              // field_write: node.value
items[index] = value;         // indexed_write: items[index]
results.push_back(value);     // append effect: results
sum += value;                 // reduce or unknown mutation, depending on declaration
```

Mutable aliases to application-visible state should be forbidden or isolated because they hide the write target:

```cpp
int& value = node->value;      // reject in SPD-lowerable regions
value = x;                    // target is no longer syntactically obvious
```

The same applies to explicit references even if `auto` is already forbidden. No-`auto` removes one common aliasing hazard, but SPD still needs a rule against local mutable references/pointers to application-visible state inside lowerable regions.

### GPU Materialize

Generated `materialize` should itself be considered a lowerable and schedulable program unit.

For many structured effects, materialization is just another data-parallel kernel:

```text
for each product item:
  if valid:
    target[index] = value
```

Examples:

```text
owned_write:
  each invocation writes its own known index or range
  usually lowers to a simple coalesced store kernel

range_write:
  lowers well when ranges are provably disjoint, tiled, or otherwise conflict-free

append:
  lowers when output offsets are reserved or computed through scan/prefix-sum

reduce:
  lowers when the operation is declared associative, such as add/min/max/and/or

atomic/update-with-policy:
  lowers through atomics or a generated multi-pass resolve strategy

ordered_update:
  requires an explicit ordering policy, sorting/grouping, serialization, or CPU fallback
```

The scheduler can treat materialization as a dependent shape:

```text
sequence kernel produces products
 -> materialize kernel commits products under the declared effect policy
 -> dependent continuations become ready
```

This means `materialize` is not necessarily a slow host-side cleanup step. When the effect descriptor is structured enough, it can be generated as an efficient GPU commit kernel or as a small sequence of GPU kernels. The hard requirement is explicit conflict semantics. Unknown conflicts must be rejected, serialized, or routed through a conservative fallback.

A batchable shape is therefore:

```text
a fixed sequence/product shape whose invocations differ only by explicit slot values, arena handles, or declared product buffers, not by the program they execute
```

## Structured Function Shape

The current candidate shape for a generated function has three zones:

```text
1. sequence
   lowerable data-parallel compute region

2. materialize
   compiler-generated commit of sequence products into app-visible state

3. continuation prep and dispatch
   scalar orchestration that prepares child-call arguments and chooses scheduling
```

This shape is meant to expose both:

```text
data parallelism:
  sequence computes lowerable products for many compatible invocations

task parallelism:
  selection chooses one or more continuations, and compatible continuations may be scheduled together
```

Conceptually:

```cpp
function(args...)
{
    SequenceData seqData;

    do {
        sequence(seqData, args...);

        if (seqData.needs_materialize) {
            materialize(seqData, args...);
        }
    } while (seqData.run);

    materialize_remaining_products(seqData, args...);

    // scalar continuation prep may read materialized data/products
    // prepare args1 / args2 / ...
    // choose parallel, serial, one-branch-only, or fallback scheduling

    if (seqData.select1) {
        function1(args1...);
    }

    if (seqData.select2) {
        function2(args2...);
    }
}
```

`SequenceData` is not just a bag of booleans. It is the explicit bridge between zones:

```text
loop/run state
selection predicates
next-state values
scalar sequence products
handles/counts for lowered product buffers
effect descriptors recovered from app-visible updates
continuation scheduling hints or summaries
```

The continuation-prep zone is intentionally separate from `sequence`. It may contain scalar code that is not efficient or appropriate for the data-parallel sequence region, such as reading materialized data, preparing arguments for selected child functions, or choosing whether selected continuations can run in parallel.

Selected continuations may run simultaneously only when their effect summaries are compatible. The compiler/scheduler should use recovered effect descriptors to decide:

```text
parallel:
  effects are provably compatible or disjoint

serial:
  both continuations are selected but their effects may conflict

one-branch-only:
  selection state chooses a single continuation

fallback:
  effect compatibility cannot be recovered safely
```

This keeps the source code close to ordinary C++ while making the control and effect boundaries explicit enough for lowering, scheduling, and debugging.

Materialization is not necessarily a single phase at the end of the function. The compiler/lowering pass owns materialize placement.

Generated materialization may be needed:

```text
after each sequence iteration:
  when the next iteration reads app-visible effects from the current iteration

after the loop:
  when products can be accumulated privately and committed once

before continuation prep:
  when scalar continuation code reads materialized app state

before child dispatch:
  when selected continuations depend on committed effects
```

If a dynamic loop depends on materialized effects, the sequence/materialize boundary is also a synchronization boundary. If the next iteration can consume explicit sequence products or loop state directly, materialization may be delayed or avoided. Placement should be dependency-driven, not fixed by syntax.

General-purpose computation comes from composition, not from making each shape internally general. This is the key structured-programming mapping:

```text
sequence:
  compose lowerable sequence regions through explicit continuation/state slots

selection:
  compute predicate/branch data in sequence, then route to the selected continuation

iteration:
  re-enter sequence with updated loop/control state until the exit predicate selects the exit continuation

materialization:
  commit declared sequence products into app-visible memory/state under an explicit effect policy
```

In compiler terms, the first lowered compute shape should target basic-block-like dataflow regions and then expand only to structured bounded regions that can be lowered to GPU-friendly MLIR ops. Arbitrary functions may need to be split at control-flow and effect boundaries into multiple lowerable sequences plus materialization phases. If a generated function cannot be represented as bounded lowerable sequence products plus explicit materialization/effects, it must either be decomposed further or remain outside the initial GPU-lowerable subset.

## Bounded Slot ABI

The runtime should expose a small fixed ABI, inspired by the earlier 64 input / 16 output idea.

Example defaults:

```text
max input slots: 64
max output slots: 16
```

Slots should be typed or tagged, not unstructured floats only. A slot can hold scalar values or handles into typed arenas.

Examples:

```cpp
struct NodeId { uint32_t index; };
struct TokenId { uint32_t index; };
struct StringId { uint32_t offset; uint32_t size; };
struct SliceId { uint32_t arena; uint32_t offset; uint32_t size; };
```

The slot ABI gives the scheduler a stable execution interface. It also gives `hen` a concrete target: every function must declare exactly what it consumes and produces.

## Arena-Indexed State

Raw object pointers are not part of the final GPU-visible SPD program state.

This does not mean the first Hen decomposition pass must stop using the existing restricted STL surface. The intended source/debug path can still use normal Hen types such as `std::shared_ptr`, `std::vector`, and `std::string` while the code is generated, compiled, tested, and debugged on the CPU.

Arena handles are introduced by a later SPD translation/lowering pass, after the linter has recovered enough structure and effects to know what the GPU-visible representation must be.

Instead of:

```cpp
std::shared_ptr<Node> node;
std::vector<std::shared_ptr<Node>> children;
std::string token;
```

the lowered SPD representation should move toward arena handles:

```cpp
NodeId node;
SliceId children;
StringId token;
```

In the lowered representation, arenas are GPU-visible storage regions. Allocation is explicit and goes through arena APIs.

Examples:

```cpp
NodeId arena_alloc_node(ArenaCtx& arena);
SliceId arena_append_tokens(ArenaCtx& arena, const TokenWrite* items, uint32_t count);
StringId arena_append_string(ArenaCtx& arena, const char* data, uint32_t size);
```

This makes pointer-heavy software representable on the GPU without requiring the agent to hand-write GPU containers in the initial source. AST work can become work over node indices and arena slices. String work can become append/slice operations over arena buffers.

Representation does not guarantee efficiency, but it makes GPU execution possible and analyzable.

## STL Containers And Lowering Passes

Hen decomposition should continue to use the current restricted STL surface where it helps produce debuggable ordinary C++.

The agent should not be asked to manually replace every `std::shared_ptr`, `std::vector`, or `std::string` with GPU-facing containers during the first source-generation pass. That would move too much backend responsibility into the generated application code and would make ordinary CPU debugging harder.

Instead, container replacement should be introduced by an additional SPD translation/lowering pass:

```text
Hen-generated restricted C++ with approved STL types
 -> linter/effect extraction recovers access patterns
 -> SPD lowering maps recoverable containers to handles, slices, arenas, or product buffers
 -> GPU-visible representation uses explicit layouts
```

STL containers with custom allocators are not the right answer for the final GPU-visible subset.

A custom allocator only controls where memory is placed. It does not remove:

- iterator-heavy control flow
- hidden dynamic reallocation
- nontrivial destructors
- opaque aliasing
- layout instability
- implementation-specific GPU support problems
- CPU/GPU semantic mismatch risk

Recommended staged policy:

```text
v0:
  Hen source/decomposition may use the existing restricted STL surface

v1:
  SPD translator recognizes a small set of read/write/access patterns over std::shared_ptr, std::vector, and std::string

v2:
  translator emits explicit handles, slices, product buffers, and materialization effects for recoverable patterns

v3:
  lower GPU-visible artifacts to arena-backed representations

v4:
  optionally introduce CPU-visible facade types or aliases when they improve generation, validation, or debugging
```

The lowered semantic core should use explicit arena/container representations:

```cpp
gpu::Span<T>
gpu::Slice<T>
gpu::StringView
gpu::ArenaVec<T>
```

These should be compiler/runtime artifacts or facades over handles and offsets, not ordinary STL state authored by the agent in the first pass.

This means Hen must own an explicit `std::` to `gpu::` translation contract. It is not enough to say that STL appears in the source and GPU containers appear later. The SPD translator must know which source-level types and operations correspond to which lowered representations.

The first translation table can be small:

```text
std::shared_ptr<T>
  -> gpu::Ref<T> / TId / arena object handle
  allowed source operations: null check, dereference for field read/write, identity comparison
  required metadata: arena kind, object type, ownership/alias policy

std::vector<T>
  -> gpu::Slice<T> for read-only ranges
  -> gpu::ArenaVec<T> or product buffer for append/write patterns
  allowed source operations: size, empty, indexed read, indexed write, push_back when effect policy is known
  required metadata: element type, bounds, append policy, conflict policy

std::string
  -> gpu::String / StringId / byte slice
  allowed source operations: size, empty, indexed read, compare, append/copy through known effect patterns
  required metadata: encoding, byte range, ownership, allocation/append policy

std::map<K, V>
  -> gpu::Map<K, V> / sorted table / hash table / CPU fallback
  allowed source operations: lookup and structured insert/update only when key and conflict policy are recoverable
  required metadata: key type, value type, lookup policy, update policy
```

Each entry in the table should define:

```text
source type pattern
allowed source operations
rejected operations
lowered gpu:: or handle representation
effect descriptor emitted by writes/appends/updates
aliasing and ownership assumptions
CPU fallback rule
```

If the translator cannot match a source operation to this table, the function is not GPU-lowerable in that form. It must be repaired, decomposed, serialized, or left on the CPU path.

## Bridge From Current hen STL Usage

This transition is more feasible than it first appears because `hen` already exposes a small allowed STL surface for generated application code.

Current `CCodeProject::getStdContainers()` allows:

```text
vector
map
stack
list
set
queue
```

Current `CCodeProject::getStdUtilityTypes()` allows:

```text
string
shared_ptr
```

So SPD does not need to replace the entire C++ standard library at source-generation time. It needs a lowering story for a small number of concepts.

Do not implement replacements inside namespace `std`. That creates undefined-behavior and portability problems. Instead, the lowering/runtime layer should introduce an explicit runtime namespace or project-level aliases when those types become useful:

```cpp
gpu::Vec<T>
gpu::Map<K, V>
gpu::Stack<T>
gpu::Queue<T>
gpu::Set<T>
gpu::List<T>
gpu::String
gpu::Ref<T>
gpu::Slice<T>
```

or shorter project aliases:

```cpp
Vec<T>
Map<K, V>
Stack<T>
Queue<T>
Set<T>
Str
Ref<T>
Slice<T>
```

The intended mapping is:

```text
std::shared_ptr<Node>              -> Ref<Node> / NodeId
std::vector<std::shared_ptr<Node>> -> Vec<NodeId> or Slice<NodeId>
std::string                        -> Str / StringId
std::map<K, V>                     -> Map<K, V> or table arena
std::stack<T>                      -> Stack<T>
std::queue<T>                      -> Queue<T>
std::set<T>                        -> Set<T>
std::list<T>                       -> avoid when possible; otherwise arena linked-list handle
```

This keeps generated code valid C++ and debuggable with normal CPU tools, while allowing the SPD translator to move recoverable program state toward GPU-addressable arena handles.

The key policy should be mode-dependent:

```text
Hen source/decomposition mode:
  restricted std types are allowed

SPD translation/lowering mode:
  approved std usage is analyzed and mapped to handles, slices, arenas, products, and materialization effects
  unrecoverable container usage is rejected, decomposed, serialized, or left on the CPU path

GPU-visible artifact mode:
  explicit arena-backed representations are required
  host/debug tooling may still use std
```

This can reuse the existing `hen` machinery that already reviews function signatures and rejects restricted STL patterns. SPD should first make those checks more semantic: which container reads, writes, aliases, appends, and indexed accesses can be recovered as lowerable effects?

Recommended migration path:

```text
v0:
  keep normal hen as-is

v1:
  add SPD analysis over the existing restricted STL surface

v2:
  extract container access/effect metadata from std::shared_ptr, std::vector, and std::string patterns

v3:
  generate handles, slices, product buffers, and materialize code for recoverable patterns

v4:
  implement deterministic CPU arenas and exact replay

v5:
  implement GPU-visible arena ABI with the same handles
```

This avoids a hard cliff. `hen` can keep compiling and debugging ordinary restricted C++ while the SPD translator gradually learns how to map approved STL usage into GPU-visible representations.

## Initial SPD Shape Vocabulary

Do not start with too many categories. The minimal useful set is:

```text
ComputeShape
RouterShape
ExpandShape
JoinShape
ReduceShape
ArenaAppendShape
```

These six are enough to encode general-purpose control and useful parallelism.

### ComputeShape

Performs bounded local computation.

Can call whitelisted scalar intrinsics. Does not emit unbounded child work.

### RouterShape

Encodes selection and loop decisions.

Consumes predicate/selection values and emits one of several continuations.

### ExpandShape

Turns one input work item into many child work items.

Useful for traversals, tokenization chunks, fanout over ranges, and decomposing large work.

### JoinShape

Fanin point for work items that must complete before a continuation runs.

Usually implemented with dependency counters or completion records in arena state.

### ReduceShape

Combines many values using a declared associative operation.

This should have special lowering rather than being encoded as arbitrary loops.

### ArenaAppendShape

Allocates or appends structured data to typed arenas.

This is required for general-purpose software because dynamic data structures cannot be ignored.

## General-Purpose Control Encoding

The structured program theorem says sequence, selection, and iteration are enough for general computation. In SPD, those constructs are represented by lowerable sequence regions plus explicit continuation and materialization boundaries.

```text
sequence:
  lowerable compute region produces explicit products and next-state values; when used as loop body, it also produces the next loop/control state

selection:
  RouterShape or continuation prep emits selected branch; if multiple selected continuations are effect-compatible, they can expose task parallelism

iteration:
  updated loop/control state re-enters sequence or selects exit continuation

function call:
  emit callee shape with continuation metadata

return:
  write output slots and signal continuation or join

dynamic data:
  use ArenaAppendShape / arena APIs

parallel fanout:
  ExpandShape emits child work

parallel fanin:
  JoinShape or ReduceShape emits continuation after completion
```

A function is therefore not mainly a stack-call unit. It is an SPD transformer: compute products, materialize declared effects, and expose continuations.

The quality of generated SPD code should be judged by how much false sequencing it avoids. If two work items only share a parent context and do not depend on each other's outputs, they should become separate ready dispatches. If a later step needs both results, the dependency should be represented as a join rather than as arbitrary local order.

## Later GPU Scheduling Direction

This section is a later runtime direction, not part of the initial SPD proof. The first priority is to prove the source/decomposition/lowering shape. Once that exists, the long-term target is to avoid CPU-side kernel submission for every small step.

A dispatch record can look like:

```cpp
struct DispatchRecord {
    uint32_t shape_id;
    uint32_t input_base;
    uint32_t output_base;
    uint32_t continuation_id;
    uint32_t predicate_slot;
    uint32_t selection_slot;
};
```

Runtime loop, conceptually:

```text
1. ready dispatch records exist in device queues
2. scheduler groups records by shape_id / resource class
3. GPU executes grouped dispatches
4. executing shapes write outputs and append new dispatch records
5. joins/reductions release continuations when dependencies complete
6. repeat until queues drain or host synchronization is required
```

Primary backend strategy for development should be Metal on macOS.

Metal-specific mechanisms to investigate and prefer:

- indirect command buffers
- indirect dispatch buffers
- argument buffers
- GPU-visible buffers for dispatch queues
- persistent scheduler/worker kernels when practical
- command-buffer driven fallback when GPU-resident scheduling is too early

Other APIs such as D3D ExecuteIndirect, Vulkan indirect dispatch, CUDA, and HIP are useful conceptual comparisons, but they are not the initial implementation target. The first serious backend should stay inside what Metal can expose on the local macOS development stack.

The CPU should supervise, debug, synchronize, and provide fallback. It should not be required to schedule every small unit of work.

## CPU Reference Executor

Every SPD graph must run under an exact CPU executor.

This is essential because `hen` debugs ordinary CPU-visible behavior. The CPU executor is also the semantic oracle for GPU execution.

Requirements:

- same slots
- same arenas
- same dispatch records
- same scheduling semantics, modulo legal parallel ordering
- deterministic replay support
- traceability for `hen` debugger actions

If GPU and CPU diverge, the CPU path should be treated as the reference until proven otherwise.

## Validation Is Mandatory

Prompting alone is not enough. `hen` can generate the shape, but validators must enforce it.

SPD validator should reject:

- raw host pointers in persistent state
- unrecoverable STL container usage inside lowerable sequence regions
- hidden global state
- blocking I/O
- exceptions
- recursion
- unbounded loops
- unsupported dynamic dispatch
- unmanaged allocation
- undeclared writes
- undeclared arena effects
- ordinary calls to non-whitelisted/generated functions from lowerable regions

Loop validator should prove one of:

- statically bounded local loop
- input-bounded loop with declared maximum
- decomposition into emitted child work
- special primitive such as reduce/scan/partition

Memory/effect validator should track:

- arena reads
- arena writes
- arena appends
- slot reads
- slot writes
- aliasing assumptions
- output ownership

Scheduler validator should track:

- emitted shape IDs
- continuation validity
- join counters
- work explosion limits
- dependency cycles
- resource limits
- unnecessary sequentialization when emitted sibling dispatches would preserve semantics

## Translation-First Lowering Strategy

The source/debug language should remain ordinary valid C++ wherever possible. The agent does not need to generate runtime APIs directly. A more practical path is to make `hen` generate disciplined ordinary C++ that is translatable into SPD shapes.

Recommended pipeline:

```text
hen prompt contract
 -> disciplined ordinary C++ function
 -> normal C++ compile/debug path
 -> SPD translator attempts lowering
 -> SPD validator checks constraints
 -> CPU SPD artifact
 -> Metal runtime/scheduler artifact
 -> differential CPU/SPD/GPU tests
```

This avoids the trap of translating arbitrary C++. The source subset still has to be strict, but it lets `hen` keep using its existing C++ generation, compilation, unit-test, and debugger machinery.

The translator should run while each function is generated, not only after the whole project is finished:

```text
agent writes function
 -> regular compiler checks C++
 -> SPD translator checks lowerability
 -> translator/validator feedback is returned to agent
 -> agent repairs the original C++ function
```

This makes SPD lowerability another first-class validation loop, similar to compile repair and debugger validation.

Example feedback:

```text
SPD lowering failed for parse_global_decl.

Reason:
- loop at line 42 has unbounded lexer-state condition
- function mutates lexer->current_token through shared pointer state
- std::string value escapes into dynamic allocation

Required repair:
- express lexer state as explicit input/output data
- use translatable Str/Vec/Ref types
- split token advancement into a bounded step function
```

This loop can also generate training data:

```text
train_spd_translation_sft.jsonl
train_spd_validation_sft.jsonl
SPD DPO pairs: translatable implementation vs non-translatable implementation
```

The backend should not depend on LLVM magically offloading arbitrary C++.

The backend should consume explicit metadata:

```json
{
  "shape_id": "parse_token_shape",
  "shape_kind": "ComputeShape",
  "inputs": 2,
  "outputs": 2,
  "reads": ["TokenStreamArena[source]"],
  "writes": ["TokenArena append"],
  "continuations": ["parse_token_shape", "finish_parse_shape"],
  "bounded_work": true,
  "gpu_lowerable": true
}
```

## Example SPD Shape Sketch

Conceptual example, intentionally without committing to a runtime API:

```text
shape_id:
  parse_token_shape

inputs:
  TokenStreamId stream
  uint32_t pos

sequence:
  token = scan_one_token(stream, pos)
  next = token_next_pos(token)
  is_done = token_is_eof(token)

products:
  TokenId token
  uint32_t next
  bool is_done

effects:
  TokenArena append token

continuation:
  if is_done:
    finish_parse_shape(token, next)
  else:
    parse_token_shape(stream, next)
```

This is still ordinary enough to debug through the CPU source path, but the SPD artifact exposes products, effects, and continuations rather than hiding them behind a call stack.

## Relationship To Existing hen Checklist

The current source checklist already pushes toward analyzability:

- no globals for application state
- explicit function arguments/returns
- no classes/inheritance for application-defined types
- no exceptions
- no function pointers in application structs
- no arbitrary external libraries
- decomposition into helper functions

The SPD checklist would be stricter:

- no unrecoverable native object graph in lowered artifacts
- no host pointers in GPU-visible program state
- no unrecoverable STL container usage in lowerable sequence regions
- no unbounded local loops
- no ordinary function calls for decomposable work
- explicit slots, arenas, effects, continuations, and emitted dispatch/work records

This is a much stronger project mode. It should probably be a separate generation mode, not a casual extension of the existing checklist.

## Feasibility Summary

The idea is plausible if the invariant stays strict:

```text
Every generated function is either an SPD shape or decomposes into SPD shapes.
```

It is not plausible if the system accepts unconstrained ordinary C++ and later hopes to recover GPU parallelism from it.

Compiler-like software is a hard case, but not a disproof. Pointers can become arena indices. AST children can become slices. Strings can become arena ranges. Allocation can become arena append. Control flow can become explicit continuation dispatch. Application-visible mutation can become generated materialization.

The real hard problems are:

- efficiency under control divergence
- GPU-side queue performance
- deterministic arena allocation
- bounded work explosion
- debugging and replay
- exact CPU/GPU semantic matching
- choosing shape granularity large enough to amortize overhead but small enough to expose parallelism

The core position remains:

```text
C++ remains the generated/debuggable surface.
The semantic model is SPD: sequence, materialize, continuation.
hen enforces the shape.
Validators reject violations.
The scheduler exploits whatever parallelism the disciplined representation exposes.
```

## Practical Development Order

1. Keep normal `hen` working as the CPU/debuggable source path.
2. Define a minimal SPD source subset for one small project.
3. Add linter checks for lowerable `sequence` code: no hidden mutation, no unbounded loops, no unsupported calls, no mutable app-state aliases.
4. Add effect extraction for simple writes: field write, indexed write, append, and declared reduction.
5. Generate a simple `SequenceData`/product description from the extracted shape.
6. Generate `materialize` mechanically from the extracted effects.
7. Run ordinary C++ behavior and SPD-generated materialization through differential tests.
8. Use `Projects/clcalc_shape` as the first tiny evaluator experiment.
9. Add a CPU SPD executor only after the sequence/product/materialize split is concrete.
10. Lower the first pure arithmetic sequence shape toward MLIR or a simple Metal kernel.
11. Add explicit continuation/selection once pure sequence lowering works.
12. Add arena-backed `Ref`, `Vec`, `Slice`, and `Str` only in the translation/runtime layer, and only when the first scalar/product experiment proves the loop.
13. Only after CPU semantics are stable, implement GPU-side queues, indirect dispatch, or persistent scheduler experiments.
14. Measure batching, shape size, queue overhead, materialization cost, and divergence.

## Out-Of-Scope Future Direction: Differentiable SPD

The first motivation for SPD is heterogeneous-friendly execution. Differentiable code is a later research direction, not part of the V1 proof.

The same structure may eventually be useful because differentiable programming also benefits from explicit compute regions, declared effects, and visible control boundaries. A future lowering pass might identify differentiable sequence regions, generate adjoints or derivative forms, and handle non-differentiable materialization through policies, custom gradients, serialization, or fallback.

This should stay out of the first proof. It is included here only to note that SPD may become a broader compiler-visible structure for Hen, not only a GPU-lowering strategy.

## Open Questions

- What is the right default slot count: fixed 64/16, configurable per project, or fixed per shape class?
- How rich should slot typing be in v0?
- What are the first allowed arena data structures?
- What is the smallest useful effect grammar for generated materialization?
- Which writes are rejected, which are serialized, and which are accepted as GPU-lowerable?
- How should joins be represented: explicit counters, continuation records, or both?
- What is the maximum local work budget per shape?
- What work explosion limits are needed to keep GPU scheduling stable?
- What syntax should `hen` generate: raw C++ API calls, a DSL-like C++ subset, or metadata plus C++?
- How much of the validator should be AST-based versus generated metadata-based?
