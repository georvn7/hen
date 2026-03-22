YouTube video: https://youtu.be/u4Sl35QuvAo

# hen

hen is a long-horizon coding agent that, given an input project description and test cases, can synthesize complex algorithms using C++ and STL.

The agent uses specialized chain-of-thought to decompose the problem, reason about each step, ensure consistent functional and data flow and generate syntactically correct, working code. Embodied in high-bandwidth baspoke development environment that provides a constant stream of ground-truth data, the agent accumulates knowledge during inference to maintain a reliable long trajectory.

When done, the agent provides a link to download the project - that's it!

For optimal quality, speed, and cost-efficiency, the agent uses a party of three LLMs with distinct roles:

Specialist: A fast model with excellent knowledge of C++ and the STL.
Expert:    A model with comprehensive knowledge of algorithms, data structures, and design patterns.
Director:  A powerful model with domain-specific knowledge in the researched area that guides the other two models through complex software development tasks.

The only inputs to the system are:

Project description in the file (for example: "hen/SimpleC/Description.txt")
Test cases (for example: "hen/SimpleC/test)

Everything else is autonomously synthesized by the agent

The repository includes `SimpleC/` as a bundled example target and working project directory. It is a sample use case used to exercise `hen`'s generation, build, and debugger flows, not a special built-in project type required by the architecture.

## Current status

- `hen` is currently macOS-first. The build and runtime flow in this repository is tested on macOS.
- `hen` is experimental and stateful. It persists debugger trajectories, generated source, intermediate working state, and other run artifacts between steps and across resumed runs.
- `hen` writes logs and debug artifacts to disk under the project tree during normal operation. This includes generated build state and directories such as `build/`, `SimpleC/debug/`, `SimpleC/logs/`, `SimpleC/dag/`, and related runtime outputs.
- During debugger and distillation runs, `hen` persists request/response logs and chat transcripts under the project tree to support debugger resume, trajectory inspection, and dataset distillation. Do not treat the working project directory as ephemeral or private-by-default.

**Installing hen (tested only on macOS!!!)**
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

### OpenAI
```bash
./hen -client -server -llmpx "localhost:8081" -proj "/full/path/to/hen/SimpleC" -env "/full/path/to/hen/Environment" -dp 10002 \
-llmDir openai/gpt-5 \
-llmExp openai/gpt-5-mini \
-llmDev openai/gpt-5-nano \
-llmDbg openai/gpt-5 -key openai=YOUR_OPENAI_KEY
```

6. **When the hen prompt appear**

To start the project
```bash
start -c 1 -s 1
```

To continue to the next step
```bash
step
```

To run autonomously to the full project generation/compilation/debugging
```bash
continue
```

At the end of the run two artifacts will be recorded:
All-in single file - your_project_name.hen.cpp (with embedded test)
CMake project - you should be able to generate all type of project VS, VS Code, Xcode, ...

Notice, due to the nature of LLMs, hen is non-deterministic. The output folders and their content, from the very same prompt, will vary with each run
