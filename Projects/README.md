# Hen v3 Project Curriculum

This directory contains project seeds for the planned `super-debug-v3` dataset.
Each subdirectory is an input project for Hen, not a generated solution.

The curriculum intentionally spans different debugging domains and scales from a small calculator to a compiler:

- `clcalc`: expression parsing and numeric evaluation.
- `ocr8`: 8x8 ASCII glyph parsing, features, and template classification.
- `math3d`: vectors, matrices, quaternions, transforms, and numeric formatting.
- `tinyvm`: assembler, bytecode VM, jumps, stack, calls, and memory.
- `poseblend`: skeleton hierarchy, keyframe sampling, pose blending, and fades.
- `mini2d_tilegame`: ASCII rendering, layers, tiles, sprites, and deterministic tile simulation.
- `sgps`: deterministic small constraint solving and grid-search diagnostics.
- `simplec`: the maximum-complexity C17 subset compiler target, including parsing, semantic checks, and ARM64 assembly generation.

`simplec` mirrors the SimpleC benchmark shape as a self-contained project seed under `Projects/`. The working/generated SimpleC state still lives separately in `/Users/georvn/projects/hen/SimpleC` and should not be confused with this seed curriculum entry.

## Estimated Solution Size

The complexity estimates below are rough final-solution SLOC bands, not hard limits. They estimate the C++ implementation Hen is expected to generate after all public ramp steps pass.

SLOC means implementation source lines only:

- count non-empty, non-comment C++ lines in the generated project sources
- exclude tests, `common.h`, instrumentation, debug logs, cached state, and generated trajectory artifacts
- include ordinary helper functions, data types, parsing/formatting code, and command dispatch
- treat the band as a complexity signal, not a grading rule

The estimate is based on feature surface, number of subsystems, data-model size, parsing/dispatch complexity, algorithmic branching, and how much intermediate state must be printable for debugging.

| Project | Estimated SLOC | Complexity Drivers |
| --- | ---: | --- |
| `clcalc` | 600-1,000 | expression lexer/parser, numeric evaluation, precedence, functions, CLI errors |
| `ocr8` | 700-1,100 | 8x8 glyph parsing, feature extraction, template scoring, diagnostics |
| `math3d` | 900-1,400 | vector math, affine transforms, quaternions, projection, script mode, fixed formatting |
| `tinyvm` | 1,100-1,800 | assembler, bytecode representation, VM execution, jumps, stack, calls, memory |
| `poseblend` | 1,100-1,700 | skeleton hierarchy, keyframe sampling, interpolation, pose blending, fade windows |
| `mini2d_tilegame` | 1,200-1,900 | tile maps, sprites, layers, deterministic simulation, ASCII rendering |
| `sgps` | 1,500-2,400 | constraint model, search, pruning, diagnostics, stable solution formatting |
| `simplec` | 3,500-6,000 | lexer, parser, AST, semantic checks, symbol/layout logic, ARM64 code generation |

Each project follows the same test-ramp principle:

1. Start with CLI and validation behavior.
2. Add one feature surface per step.
3. Preserve earlier semantics in later steps.
4. Keep public tests deterministic and private tests adversarial but grounded.
5. Require debug-observable intermediate state through `--dump` and/or `PRINT_TEST` where useful.
6. End with a smoother integrated workflow rather than a disconnected stress test.

The tests follow `/Users/georvn/projects/hen/Environment/Prompts/TestFramework.txt`:

- `test.command` contains exactly one main invocation.
- phase input/output files declare the file flow explicitly.
- public `io_hint` is `none`.
- private `io_hint` describes anti-hardcoding or boundary behavior.
- private tests are canaries for reward-hacking review, not implementation hints for public behavior.
