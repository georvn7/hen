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
