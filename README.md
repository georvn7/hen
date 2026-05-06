<p align="center">
  <img src="assets/hen.png" alt="Cute hen" width="220">
</p>

YouTube video: https://youtu.be/u4Sl35QuvAo

# hen

hen is a stateful long-horizon coding agent for C++ projects. Given a project description and tests, it decomposes the problem into functions and data, generates source, repairs compilation failures, debugs against runtime evidence, and can distill successful debug trajectories into training data.

`hen` is built around a debugger-centered workflow rather than a minimal-latency chat loop. The system keeps dense runtime evidence, persists trajectories and logs to disk, and uses those artifacts both for reliability during a run and for later synthetic-data generation.

In practice, `hen` routes work across four LLM roles:

- Developer: the fast implementation specialist
- Expert: the broader algorithmic and architecture model
- Director: the highest-level planner and escalation target
- Debugger: the model used for grounded debug analysis and next-step selection

The main inputs are:

- a project description file, for example `hen/SimpleC/Description.txt`
- test cases, for example `hen/SimpleC/tests/...`

The repository includes `SimpleC/` as a bundled example target and working project directory. It is a sample use case used to exercise `hen`'s generation, build, and debugger flows, not a special built-in project type required by the architecture.

## Who this is for

- People interested in coding agents with grounded debugger loops rather than only chat-style tool use
- People interested in trajectory persistence, runtime evidence, and synthetic-data distillation
- Contributors who are comfortable with an experimental, stateful, macOS-first C++ codebase

## Who this is not for

- People looking for a polished cross-platform end-user coding assistant
- People expecting the fastest possible agent loop with minimal review and testing overhead
- People who want the working project tree to stay ephemeral or private-by-default during debugger and distillation runs

## Current status

- `hen` is currently macOS-first. The build and runtime flow in this repository is tested on macOS.
- `hen` is experimental and stateful. It persists debugger trajectories, generated source, intermediate working state, and other run artifacts between steps and across resumed runs.
- `hen` writes logs and debug artifacts to disk under the project tree during normal operation. This includes generated build state and directories such as `build/`, `SimpleC/debug/`, `SimpleC/logs/`, `SimpleC/dag/`, and related runtime outputs.
- During debugger and distillation runs, `hen` persists request/response logs and chat transcripts under the project tree to support debugger resume, trajectory inspection, and dataset distillation. Do not treat the working project directory as ephemeral or private-by-default.
- `hen` is currently tuned more for dense debugging signal and synthetic-data generation than for the shortest possible agent loop. It intentionally performs extensive testing, keeps extra review passes such as git-history-aware review around some `fix_function` flows, and usually schedules another test run after each `fix_function` so the trajectory stays grounded in fresh runtime evidence.
- This is not the final efficiency envelope. Future optimization opportunities include bundling multiple information requests into one step, allowing coordinated fixes across multiple functions when safe, and adding a leaner fast-agent mode with fewer review passes when data generation is not the priority.

## Further reading

- [Architecture overview](docs/architecture-overview.md)
- [Future debugger notes](docs/future-debugger-notes.md)

**Installation**
===============

To get started with hen, follow these steps:

1. **Clone the Git repository**

2. **Change working directory to the cloned hen folder**

3. **Execute the build.sh script**
```bash
./build.sh
```
If you don't have Homebrew installed, quit the command window and run `build.sh` again.

4. **Open the created Xcode project**
```bash
hen/build/hen.xcodeproj
```
In Xcode:

* Set the active target to "hen"
* Set the target device to "My Mac"
* Press Command+B to build the project

**Setting up command line with different LLM models**
---------------------------

To use hen with different Large Language Models (LLMs), for example:

5. **Change working directory to the executable folder**
```bash
cd /full/path/to/hen/build/Debug
```

### Recommended: Anthropic + Groq
```bash
./hen -client -server -llmpx "localhost:8081" -proj "/full/path/to/hen/SimpleC" -env "/full/path/to/hen/Environment" -dp 10002 \
-llmDir anthropic/claude-sonnet-4-6 \
-llmExp groq/openai/gpt-oss-120b \
-llmDev groq/openai/gpt-oss-20b \
-llmDbg anthropic/claude-sonnet-4-6 \
-key anthropic=YOUR_ANTHROPIC_KEY \
-key groq=YOUR_GROQ_KEY
```

For other available providers and model IDs, check [Environment/LLRegistry.json](Environment/LLRegistry.json) and [src/LLMConfig.h](src/LLMConfig.h). The registry is the source of truth for provider/model pairs and the metadata `hen` uses for role suitability, context size, reasoning settings, rate limits, and token pricing.

6. **When the `hen` prompt appears**

To start the project
```bash
/start
```

To continue to the next step
```bash
/step
```

To run autonomously to the full project generation/compilation/debugging
```bash
/continue
```

Bare text entered at the prompt is treated as chat input to the active project session.

At the end of the run two main artifacts are produced:

- an all-in-one source file: `your_project_name.hen.cpp` (with embedded tests)
- a generated CMake project, which you can then open or generate for environments such as Xcode, VS Code, or Visual Studio

When the project has been fully debugged and the trajectory logs are preserved, you can synthesize training data from those successful debug runs by enabling synthetic-data generation on `start`:

```bash
/start --synthetic-data=true
```

This loads the already generated project and preserved trajectories, runs the normal post-build debugger flow, and then distills data from the successful debug runs and logs.

## Synthetic data artifacts

`hen`-generated debugging and distillation artifacts are also published on Hugging Face:

- [georvn7/super-debug-v1](https://huggingface.co/datasets/georvn7/super-debug-v1)
- [georvn7/super-debug-v2](https://huggingface.co/datasets/georvn7/super-debug-v2)

These datasets contain debugger-oriented training artifacts distilled from successful `hen` trajectories. They are meant as concrete examples of the synthetic-data pipeline described in this repository, not as separate products from `hen` itself.

Notice, due to the nature of LLMs, hen is non-deterministic. The output folders and their content, from the very same prompt, will vary with each run
