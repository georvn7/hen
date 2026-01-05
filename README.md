# std::rave

std::rave is a long-horizon coding agent that, given an input project description and test cases, can synthesize complex algorithms using C++ and STL.

The agent uses specialized chain-of-thought to decompose the problem, reason about each step, ensure consistent functional and data flow and generate syntactically correct, working code. Embodied in high-bandwidth baspoke development environment that provides a constant stream of ground-truth data, the agent accumulates knowledge during inference to maintain a reliable long trajectory.

When done, the agent provides a link to download the project - that's it!

For optimal quality, speed, and cost-efficiency, the agent uses a party of three LLMs with distinct roles:

Specialist: A fast model with excellent knowledge of C++ and the STL.
Expert:    A model with comprehensive knowledge of algorithms, data structures, and design patterns.
Director:  A powerful model with domain-specific knowledge in the researched area that guides the other two models through complex software development tasks.

The only inputs to the system are:

Project description in the file (for example: "stdrave/SimpleC/Description.txt")
Test cases (for example: "stdrave/SimpleC/test)

Everything else is autonomously synthesized by the agent

**Installing std::rave (tested only on macOS!!!)**
===============

To get started with std::rave, follow these steps:

1. **Clone the Git repository**

2. **Change working directory to the cloned stdrave folder**

3. **Execute the build.sh script**
```bash
./build.sh
```
If you don't have Homebrew installed, quit the command window and run `build.sh` again.

4. **Open the created Xcode project**
```bash
stdrave/build/std-rave.xcodeproj
```
In Xcode:

* Set the active target to "std-rave"
* Set the target device to "My Mac"
* Press Command+B to build the project

**Setting up command line with different LLM models**
---------------------------

To use std::rave with different Large Language Models (LLMs), for example:

5. **Change working directory to the executable folder**
```bash
cd /full/path/to/stdrave/build/Debug
```

### OpenAI
```bash
./std-rave -client -server -llmpx "localhost:8081" -proj "/full/path/to/stdrave/SimpleC" -env "/full/path/to/stdrave/Environment" -dp 10002 -llmDir openai/gpt-5 -llmExp openai/gpt-5-mini -llmDev openai/gpt-5-nano -key openai=YOUR_OPENAI_KEY
```

### Anthropic
```bash
./std-rave -client -server -llmpx "localhost:8081" -proj "/full/path/to/stdrave/SimpleC" -env "/full/path/to/stdrave/Environment" -dp 10002 -llmDir anthropic/claude-opus-4-5-high -llmExp anthropic/claude-sonnet-4-5-high -llmDev anthropic/claude-haiku-4-5-high -key anthropic=YOUR_CLAUDE_KEY
```

Here 'high' is not that high actually, it means '8K thinking'

### Google
```bash
./std-rave -client -server -llmpx "localhost:8081" -proj "/full/path/to/stdrave/SimpleC" -env "/full/path/to/stdrave/Environment" -dp 10002 -llmDir google/gemini-3-pro-preview -llmExp google/gemini-3-flash-preview -llmDev google/gemini-2.5-flash-lite -key google=YOUR_GEMINI_KEY
```

6. **When the std::rave prompt appear**

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
All-in single file - your_project_name.stdrave.cpp (with embedded test)
CMake project - you should be able to generate all type of project VS, VS Code, Xcode, ...

Notice, due to the nature of LLMs, std::rave is non-deterministic. The output folders and their content, from the very same prompt, will vary with each run
