#include <iomanip>
#include <atomic>
#include <future>
#include <mutex>

#include "Debugger.h"
#include "Utils.h"
#include "Client.h"
#include "BlackBox.h"

#include <clang-c/Index.h>

//#define TRACE_WITH_LLDB

#define MAX_LOG_SECTIONS_PER_LOCATION 2
#define MAX_DEBUGGING_STEPS 400
#define MAX_REPEATED_INVALID_NEXT_STEP_ATTEMPTS 2

#define TRACE_MAX_MEMBERS 16
#define TRACE_MIN_MEMBERS 4
#define TRACE_MAX_ELEMENTS 4
#define TRACE_MIN_ELEMENTS 2
#define TRACE_MAX_DEPTH 2
#define TRACE_MIN_DEPTH 2


#define TRACE_SYS_MEMBERS 32
#define TRACE_SYS_ELEMENTS 8
#define TRACE_SYS_DEPTH 6

//#define EVALUATE_BREAKPOINTS_WITH_MIN_CPP_VER

//In seconds
#define MIN_LLDB_TIMEOUT 90
#define RUN_TEST_LLDB_TIMEOUT 10

//This MUST be above the  HIGH_WATER_MARK (currently 25 MB) of PRINT_TEST in Environment/Prompts/CommonSource.h
#define MAX_LLDB_STDOUT_SIZE (30*1024*1024)

//#define LLDB_PRINT_BREAKPOINT_HITS
//#define LLDB_VERBOSE_BATCH_MODE

namespace hen {

DEFINE_TYPE(Breakpoint)
DEFINE_FIELD(Breakpoint, source_line)
DEFINE_FIELD(Breakpoint, condition)
DEFINE_FIELD(Breakpoint, expression)

DEFINE_TYPE(NextDebugStep)
DEFINE_FIELD(NextDebugStep, action_type)
DEFINE_FIELD(NextDebugStep, action_subject)
DEFINE_FIELD(NextDebugStep, line_number)
DEFINE_FIELD(NextDebugStep, invocation)
DEFINE_ARRAY_FIELD(NextDebugStep, breakpoints)
DEFINE_FIELD(NextDebugStep, motivation)

DEFINE_TYPE(RunAnalysis)
DEFINE_FIELD(RunAnalysis, debug_notes)
DEFINE_FIELD(RunAnalysis, log_summary)

void NextDebugStep::clear()
{
    action_type.clear();
    action_subject.clear();
    motivation.clear();
    m_stepId = 0;
}

bool NextDebugStep::isInformationRequest(const std::string& actionType)
{
    if(actionType == "function_info"       ||
       actionType == "data_info"           ||
       actionType == "file_info"           ||
       actionType == "functions_summary"   ||
       actionType == "call_graph"          ||
       actionType == "log_info"            ||
       actionType == "search_source"       ||
       actionType == "new_function"        ||
       actionType == "refactor_data"       ||
       actionType == "new_data"            ||
       actionType == "step_info")
    {
        return true;
    }
                       
    return false;
}

bool NextDebugStep::isInformationRequest()
{
    return isInformationRequest(action_type);
}

bool Breakpoint::isTheSame(const Breakpoint& other)
{
    return source_line == other.source_line &&
    hasCondition() == other.hasCondition() && (!hasCondition() || getConditionCode() == other.getConditionCode()) &&
    hasExpression() == other.hasExpression() && (!hasExpression() || getExpressionCode() == other.getExpressionCode());
}

bool Breakpoint::operator==(const Breakpoint& other)
{
    return isTheSame(other);
}

bool Breakpoint::hasCondition() const
{
    std::string s = boost::algorithm::trim_copy(condition);
    return (!s.empty() && s != "none" && s != "true");
}

bool Breakpoint::hasExpression() const
{
    return (!expression.empty() && expression != "none");
}

std::string Breakpoint::getConditionCode() const
{
    return hasCondition() ? condition : "true";
}

std::string Breakpoint::getExpressionCode() const
{
    return hasExpression() ? expression : "printf(\"User breakpoint\\n\")";
}

std::string Breakpoint::getCodeSnippet() const
{
    std::string bpSnippet = "{if(";
    bpSnippet += getConditionCode();
    bpSnippet += "){";
    bpSnippet += getExpressionCode();
    bpSnippet += ";}}";
    
    return bpSnippet;
}

bool Breakpoint::isValid() const
{
    return source_line > 0 && (hasCondition() || hasExpression());
}

// Helper function to escape regex metacharacters in a given string.
std::string escapeRegexFromCode(const std::string& input) {
    static const std::string specials = R"(\^$.|?*+()[{)";
    std::string output;
    for (char c : input) {
        if (specials.find(c) != std::string::npos) {
            output.push_back('\\');
        }
        output.push_back(c);
    }
    return output;
}

//This function takes a snippet of a C++ source code. It checks the snippet for all occurence of a functionName.
//The matched fragment could be:
// 1/ "functionName("
// or
// 2/ "(returnType)functionName("
// All C++ allowed formatting of the 1/ and 2/ should be considered, for example "( returnType)\nfunctionName  ("
// The matched fragments are replaced with:
// 3/ "(returnTypeReplacement)functionNameReplacement"
std::string Breakpoint::instrumentFunction(const std::string& snippet,
                                           const std::string& functionName, const std::string& returnType,
                                           const std::string& functionNameReplacement, const std::string& returnTypeReplacement) const
{
    // Escape the inputs to safely insert them into a regex.
    std::string escapedReturnType = escapeRegexFromCode(returnType);
    std::string escapedFunctionName = escapeRegexFromCode(functionName);
    
    // Build the regex pattern.
    // The pattern breakdown:
    //   (?:\(\s*<returnType>\s*\)\s*)?  --> Optionally matches a parenthesized return type
    //   <functionName>\s*\(             --> Then matches the function name followed by an opening parenthesis,
    //                                     allowing whitespace before the parenthesis.
    
    //std::string patternStr = "(?:\\(\\s*" + escapedReturnType + "\\s*\\)\\s*)?" +
    //                         escapedFunctionName + "\\s*\\(";
    std::string patternStr = "(?:\\(\\s*" + escapedReturnType + "\\s*\\)\\s*)?" +
                             "\\b" + escapedFunctionName + "\\b\\s*\\(";
    
    std::regex pattern(patternStr);
    
    // Prepare the replacement string.
    // Regardless of whether the return type was present, we output a constant format:
    // (returnTypeReplacement)functionNameReplacement
    std::string typeCast;
    if(!returnTypeReplacement.empty())
    {
        typeCast = "(" + returnTypeReplacement + ")";
    }
    
    std::string replacement = typeCast + functionNameReplacement;
    
    // Use regex_replace to replace all occurrences in the snippet.
    std::string result = std::regex_replace(snippet, pattern, replacement);
    return result;
}

void Breakpoint::instrumentCalls(const std::string& functionName, const std::map<std::string, std::string>& stdCalls)
{
    m_instrumentedCondition = instrumentCalls(getConditionCode(), functionName, stdCalls);
    
    //Just in case if the expression is missing ;
    m_instrumentedExpression = instrumentCalls(getExpressionCode() + ";", functionName, stdCalls);
    
    m_instrumentedSnippet = "{if(";
    m_instrumentedSnippet += m_instrumentedCondition;
    m_instrumentedSnippet += "){";
    m_instrumentedSnippet += m_instrumentedExpression;
    m_instrumentedSnippet += "}}";
    
    m_stdCalls = stdCalls;
}

bool Breakpoint::containsFunction(const std::string &snippet, const std::string &functionName) const
{
    // Escape the function name to safely use it in a regex.
    std::string escapedFunctionName = escapeRegex(functionName);
    // Build the regex pattern to match:
    // functionName followed by any whitespace (including newlines) and then an opening parenthesis.
    std::string patternStr = escapedFunctionName + "\\s*\\(";
    std::regex pattern(patternStr);
    
    // Return true if the snippet contains a match, false otherwise.
    return std::regex_search(snippet, pattern);
}

std::string Breakpoint::instrumentCalls(const std::string& snippet, const std::string& functionName, const std::map<std::string, std::string>& stdCalls)
{
    std::string instrumentedCode = snippet;
    std::string prefixCode;
    std::string postfixCode;
    
    bool hasPrintf = containsFunction(snippet, "printf");
    for(auto call : stdCalls)
    {
        if(hasPrintf && call.first == "printf")
        {
            if(prefixCode.empty())
            {
                //TODO: Do I need to zero-fill the buffer?
                prefixCode = "char _b_[512];char* _p_=_b_;";
            }
            
            std::string functionNameReplacement = "_p_+=(int)snprintf(_p_,512-(_p_-_b_),";
            instrumentedCode = instrumentFunction(instrumentedCode,
            call.first, call.second, functionNameReplacement, std::string());
            
            if(postfixCode.empty())
            {
#ifdef TRACE_WITH_LLDB
                postfixCode = "; _b_";
#else
                postfixCode = "; if(strlen(_b_)>0){trace::log << \"[[Breakpoint]] \" << _b_ << std::endl;}";
#endif
            }
        }
        else
        {
            instrumentedCode = instrumentFunction(instrumentedCode,
            call.first, call.second, call.first + "(", call.second);
        }
    }
    
    std::string combinedCode = (prefixCode + instrumentedCode + postfixCode);
    
    return combinedCode;
}

const std::string& Breakpoint::getInstrumentedCodeSnippet() const
{
    return m_instrumentedSnippet;
}

std::vector<std::string> Debugger::parseCommandLine(const std::string &cmdLine) const
{
    return hen::parseCommandLine(cmdLine);
}

//Grab PID from launched/stopped/exited lines.
//That way we still get the PID even if the watchdog kills LLDB before any -k commands execute.
int Debugger::extractProcessId(const std::string& text) const {
    std::smatch m;

    // 1) Prefer the earliest signal: on launch
    {
        static const std::regex rx_launched(R"(Process\s+(\d+)\s+launched:)");
        if (std::regex_search(text, m, rx_launched))
            return std::stoi(m[1]);
    }

    // 2) If we missed launch, try when it stopped (crash/breakpoint)
    {
        static const std::regex rx_stopped(R"(Process\s+(\d+)\s+stopped)");
        if (std::regex_search(text, m, rx_stopped))
            return std::stoi(m[1]);
    }

    // 3) Or after it exited
    {
        static const std::regex rx_exited(R"(Process\s+(\d+)\s+exited)");
        if (std::regex_search(text, m, rx_exited))
            return std::stoi(m[1]);
    }

    return -1;
}

std::pair<std::string, std::string> Debugger::runLLDB(CCodeProject* project, std::string& traceLog,
                              const std::string& traceExecutable,
                              const std::string& traceCommandLine,
                              const std::string& workingDir,
                              int timeoutInSeconds, bool instrument, int& returnCode)
{
    namespace bp   = boost::process::v2;
    namespace asio = boost::asio;
    
    // Build minimal pre-run - we let the script do the heavy lifting
    std::vector<std::string> args;
    args.push_back("--batch");
    args.push_back("-o"); args.push_back("settings set auto-confirm true");
    args.push_back("-o"); args.push_back("settings set target.max-string-summary-length 128");
    args.push_back("-o"); args.push_back("settings set target.max-children-count 8");
    
    std::string consoleLogCmd = "settings set target.output-path " + m_workingDirectory + "/console.log";
    //args.push_back("-o"); args.push_back("settings set target.output-path /dev/null");
    args.push_back("-o"); args.push_back(consoleLogCmd);
    args.push_back("-o"); args.push_back("settings set target.error-path /dev/null");
    
    std::string asanSettings = "settings set -- target.env-vars ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:";
    asanSettings += "detect_stack_use_after_return=1:alloc_dealloc_mismatch=1:strict_string_checks=1 ";
    asanSettings += "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1";
    args.push_back("-o"); args.push_back(asanSettings);
    
    const std::string scriptPath = workingDir + "/lldb_cmds_san.txt";
    // Always (re)write a script that reads LLDB_TIMEOUT_SEC
    boost_fs::remove(scriptPath);
    
    {
        std::string lldbCommandsFile = Client::getInstance().getEnvironmentDir() + "/Debugger/Scripts/lldb_cmds_san.txt";
        std::string lldbCommandsStr = getFileContent(lldbCommandsFile);
        std::string lldbCommandsScript = buildPrompt(lldbCommandsStr, {{"timeout", std::to_string(MIN_LLDB_TIMEOUT)}});
        
        saveToFile(lldbCommandsScript, scriptPath);
    }
    
    args.push_back("-s"); args.push_back(scriptPath);

    args.push_back("--");
    args.push_back(traceExecutable);

    // append program args
    auto testArgs = parseCommandLine(traceCommandLine);
    args.insert(args.end(), testArgs.begin(), testArgs.end());
    
    std::string stdoutLogPath = workingDir + "/stdout.log";
    std::string tracePath = workingDir + "/trace.txt";
    
    std::string functionToDebug;
    if(m_nextStep.action_type == "debug_function")
    {
        functionToDebug = m_nextStep.action_subject;
        //TODO: Should we check the function actually exists?
    }
    
    if(instrument)
    {
        instrumentSource(project, functionToDebug, m_nextStep.breakpoints);
    }
    
#if defined(__APPLE__)
    //Codesign with debug entitlements
    //codesign -s - --entitlements ./debug.entitlements ./feature_test
    std::string entitlementPath = Client::getInstance().getEnvironmentDir();
    entitlementPath += "/debug.entitlements";
    std::string codesignCmd = "codesign -s - --entitlements " + entitlementPath + " " + traceExecutable;
    hen::exec(codesignCmd, m_workingDirectory, "Codesign", true);
#endif
    
    namespace bp = boost::process::v2;  // Changed to v2
    namespace asio = boost::asio;
    
    // Create io_context for v2
    auto ctx = hen::getAsioContext();
    
    std::cout << "Executing lldb with the following command line:" << std::endl;
    std::cout << "/usr/bin/lldb";
    
    bool prevIsOption = false;
    for(const auto& arg : args)
    {
        if(arg == "-o" || arg == "-k") {
            std::cout << " " << arg;
            prevIsOption = true;
        }
        else {
            
            if(prevIsOption)
            {
                std::cout << " '" << arg << "'";
            }
            else
            {
                std::cout << " " << arg;
            }
            
            prevIsOption = false;
        }
    }
    
    std::cout << " " << traceCommandLine;
    std::cout << std::endl << std::endl;

    // Build a command result string
    std::string stdLog;
    std::ostringstream lldbLog;


    // Retrieve the LLDB process ID
    pid_t lldbPID = -1;
    // Get the process group ID for the LLDB process
    pid_t lldbPGID = -1;
    
    std::string lldbDebugLog;
    lldbDebugLog.reserve(64 * 1024); // optional pre-reserve
    int exitCode = -100000;
    
    //Delete old logs
    boost_fs::remove(stdoutLogPath);
    boost_fs::remove(tracePath);
    boost_fs::remove(workingDir + "/memo.txt");
    boost_fs::remove(workingDir + "/lldb.log");
    boost_fs::remove(workingDir + "/console.log");
    boost_fs::remove_all(workingDir + "/stack");
    boost_fs::remove_all(workingDir + "/breakpoints");
    m_lldbLog.clear();
    
    // --- start the black box thread (before LLDB) ---
    const std::string sockPath = "/tmp/simplec.logger.sock"; // keep consistent with your client
    
    BlackBox blackBox(m_debugPort, workingDir);

    if (!blackBox.start()) {
        std::cerr << "[black_box] failed to start logger thread on " << sockPath << "\n";
        // Proceeding is still possible, but the target won't be able to stream logs.
    }
    
    try
    {
        // Create pipes for capturing output
        asio::readable_pipe stdout_pipe(*ctx);
        
        auto set_nonblocking = [](auto& pipe){
            int fd = pipe.native_handle();
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags != -1)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        };
        
        // Launch LLDB with the arguments using v2 API
        auto lldbChild = std::make_shared<bp::process>(
            *ctx,
            "/usr/bin/lldb",
            args,
            bp::process_stdio{
                nullptr,
                stdout_pipe,
                stdout_pipe
            },
            bp::process_start_dir(workingDir)
        );
        
        // Retrieve the LLDB process ID
        lldbPID = lldbChild->id();
        // Get the process group ID for the LLDB process
        lldbPGID = ::getpgid(lldbPID);

        // Print the IDs
        std::cout << "LLDB PID: " << lldbPID << std::endl;
        std::cout << "LLDB PGID: " << lldbPGID << std::endl;

        std::atomic<size_t> bytes_accumulated{0};
        
        auto drain_pipe_blocking = [&](boost::asio::readable_pipe& pipe) {
            try {
                std::array<char, 4096> buf;
                for (;;) {
                    boost::system::error_code ec;
                    std::size_t n = pipe.read_some(boost::asio::buffer(buf), ec);
                    if (n) lldbDebugLog.append(buf.data(), n);
                    if (ec == boost::asio::error::eof) break;
                    if (ec) break;                    // includes bad_descriptor after close()
                }
            } catch (...) { /* ignore */ }
        };
        
        std::thread t_out([&]{ drain_pipe_blocking(stdout_pipe); });

        // --- the one and only wait/reap ---

        lldbChild->wait();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // let kernel flush
        try { stdout_pipe.close(); } catch (...) {}

        // join threads (no hanging now)
        t_out.join();
        exitCode = lldbChild->exit_code();
    }
    catch (const std::exception &ex)
    {
        lldbLog << "[EXCEPTION] " << ex.what() << "\n";
    }
    
    // --- stop the black box thread (after LLDB process finishes) ---
    // If the instrumented target didn’t send MSG_FINISH_STOP, request_stop()
    // will break any blocking reads, flush, and exit cleanly.
    blackBox.request_stop();
    blackBox.join();
    
    stdLog = readTextFileWithLimit(stdoutLogPath, MAX_LLDB_STDOUT_SIZE);
    
    if (exitCode != 0)
    {
        if(exitCode == -100000)
        {
            //TODO: Is this lldb or application exit code ?!?!
            lldbLog << "[ERROR] unable to obtain lldb exit code \n";
        }
        else
        {
            lldbLog << "[ERROR] lldb exited with code " << exitCode << "\n";
        }
    }
    else
    {
        lldbLog << "[INFO] lldb exited normally (code 0).\n";
    }

    lldbDebugLog += "\n\n";
    lldbDebugLog += lldbLog.str();
    
    std::cout << lldbDebugLog;
    m_lldbLog = lldbDebugLog;
    saveToFile(lldbDebugLog, workingDir + "/lldb.log");
    
    //Parese process return code, from something like this:
    //Process 45043 exited with status = 0 (0x00000000)
    if(!parseLastExitCode(m_lldbLog, returnCode))
    {
        //TODO: Do we want to do something here?
    }
    
    int pid = extractProcessId(lldbDebugLog);
    std::cout << "PROCESS ID: " << pid << std::endl;
    if(pid > 0)
    {
        killProcess(pid);
    }
    
    if(lldbPID > 0)
    {
        killProcess(lldbPID);
    }
    
    auto truncation = extractFirstTruncationTag(stdLog);
    if(truncation.first > 0)
    {
        //Add this note to the end as we expect the log analysis always to look at the end of the log!
        stdLog += "\n\nNOTE: the log has been truncated at line: " + std::to_string(truncation.first);
        stdLog += " due to size limit\n";
        stdLog += truncation.second;
        stdLog += "\n\n";
    }
    
    // Read trace file
    if(boost_fs::exists(tracePath))
    {
        std::ifstream traceFile(tracePath);
        traceLog = std::string((std::istreambuf_iterator<char>(traceFile)), std::istreambuf_iterator<char>());
    }
    
    std::string consoleLog = readTextFileWithLimit(workingDir + "/console.log", MAX_LLDB_STDOUT_SIZE);
    
    //Must be the very last thing we do here as the unit test working dir is in the instrumented folder
    //And switching will make paths that pont to 'build' invalid
    if(instrument)
    {
        switchToDefaultBuild(project);
    }

    return std::make_pair(stdLog, consoleLog);
}

std::string Debugger::runTrace(CCodeProject* project, std::string& traceLog,
                               const std::string& traceExecutable,
                               const std::string& traceCommandLine,
                               const std::string& workingDir,
                               int timeoutInSeconds, bool instrument, int& returnCode)
{
    std::string functionToDebug;
    if(m_nextStep.action_type == "debug_function")
    {
        functionToDebug = m_nextStep.action_subject;
        //TODO: Should we check the function actually exists?
    }
    
    if(instrument)
    {
        instrumentSource(project, functionToDebug, m_nextStep.breakpoints);
    }
    
    namespace bp = boost::process::v2;  // Changed to v2
    namespace asio = boost::asio;

    // Let's parse the testCommandLine into individual arguments
    auto testArgs = parseCommandLine(traceCommandLine);

    std::string stdoutLogPath = workingDir + "/stdout.log";
    std::string traceLogPath = workingDir + "/trace.txt";
    
    //Remove old trace, if any
    boost_fs::remove(traceLogPath);
    boost_fs::remove(workingDir + "/memo.txt");
    boost_fs::remove(workingDir + "/lldb.log");
    boost_fs::remove_all(workingDir + "/stack");
    boost_fs::remove_all(workingDir + "/breakpoints");
    m_lldbLog.clear();
    
    std::cout << "Tracing the application with command line:" << std::endl;
    std::cout << traceExecutable << " " << traceCommandLine << std::endl;
    std::cout << std::endl << std::endl;

    // Create io_context for v2
    auto ctx = hen::getAsioContext();
    
    // Create pipe for capturing output
    //asio::readable_pipe lldb_stdout(*ctx);

    // Build a command result string
    std::string stdLog;

    // Retrieve the LLDB process ID
    pid_t pid = -1;
    // Get the process group ID for the LLDB process
    pid_t pgid = -1;
    
    // Print the IDs
    std::cout << "PID: " << pid << std::endl;
    std::cout << "PGID: " << pgid << std::endl;
    
    // We'll use an atomic flag to note if the timeout fired.
    std::atomic<bool> timed_out{false};
    std::atomic<bool> size_exceeded{false};
    
    if(!boost_fs::exists(traceExecutable))
    {
        traceLog += "[ERROR] the executable doesn't exist: " + traceExecutable + "\n";
        stdLog += "[ERROR] the executable doesn't exist: " + traceExecutable + "\n";
        switchToDefaultBuild(project);
        return stdLog;
    }
    
    try
    {
        //Delete old logs
        boost_fs::remove(stdoutLogPath);
        
        // Create pipes for capturing output
        asio::readable_pipe stdout_pipe(*ctx);
        asio::readable_pipe stderr_pipe(*ctx);
        
        auto set_nonblocking = [](auto& pipe){
            int fd = pipe.native_handle();
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags != -1)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        };

        set_nonblocking(stdout_pipe);
        set_nonblocking(stderr_pipe);
        
        // Launch LLDB with the arguments using v2 API
        auto traceChild = std::make_shared<bp::process>(
            *ctx,
            traceExecutable,
            testArgs,
            bp::process_stdio{
                nullptr,
                stdout_pipe,
                stderr_pipe
            },
            bp::process_start_dir(workingDir)
        );
        
        // Get process info immediately after creation
        pid = traceChild->id();
        ::setpgid(pid, 0);
        pgid = ::getpgid(pid);
        
        // Shared flag to signal threads to stop
        std::atomic<bool> should_stop{false};
        
        // Start async reading from pipes to file
        std::ofstream outFile(stdoutLogPath);
        std::mutex out_mtx;
        
        std::atomic<bool> reader_done{false};
        
        auto drain_pipe = [&](asio::readable_pipe& pipe)
        {
            std::array<char, 4096> buf;
            boost::system::error_code ec;

            while (!should_stop)
            {
                if (!traceChild->running()) break;

                std::size_t n = pipe.read_some(asio::buffer(buf), ec);

                if (ec == asio::error::eof)      break;   // pipe closed
                if (ec)                         continue; // temporary error: EAGAIN, etc.

                if (n)
                {
                    std::lock_guard<std::mutex> lk(out_mtx);
                    outFile.write(buf.data(), n);         // no flush in the hot loop
                }
            }
        };
        
        std::thread stdoutReader(drain_pipe, std::ref(stdout_pipe));
        std::thread stderrReader(drain_pipe, std::ref(stderr_pipe));
        
        // Timeout watchdog with proper cleanup
        std::unique_ptr<std::thread> watchdog;
        std::mutex watchdog_mtx;
        std::condition_variable watchdog_cv;
        bool watchdog_cancelled = false;
        if (timeoutInSeconds > 0) {
            watchdog = std::make_unique<std::thread>([&, traceChild, timeoutInSeconds]() {
                std::unique_lock<std::mutex> lk(watchdog_mtx);
                // Wait until either deadline passes _or_ cancel is signaled
                if (!watchdog_cv.wait_for(lk,
                      std::chrono::seconds(MIN_LLDB_TIMEOUT + timeoutInSeconds),
                      [&]{ return watchdog_cancelled; }))
                {
                    // timeout expired without cancellation
                    if (traceChild->running()) {
                        traceChild->terminate();
                        timed_out = true;
                    }
                }
            });
        }
        
        // Size watchdog with proper cleanup
        std::thread sizeWatchdog([&, traceChild, stdoutLogPath]() {
            while (!should_stop)
            {
                try {
                    // Check if process is still running
                    if (!traceChild->running()) {
                        break;
                    }
                    
                    boost::system::error_code ec;
                    auto fileSize = boost_fs::file_size(stdoutLogPath, ec);
                    if (!ec && fileSize > MAX_LLDB_STDOUT_SIZE)
                    {
                        size_exceeded = true;
                        traceChild->terminate();
                        break;
                    }
                } catch (...) {
                    // Process is gone, exit watchdog
                    break;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

        // Wait for process completion
        boost::system::error_code wait_ec;
        int exitCode = traceChild->wait(wait_ec);
        
        // Signal all threads to stop
        should_stop = true;
        reader_done = true;
        
        if (stdoutReader.joinable()) stdoutReader.join();
        if (stderrReader.joinable()) stderrReader.join();
        outFile.flush();
        
        {
            std::lock_guard<std::mutex> lk(watchdog_mtx);
            watchdog_cancelled = true;
        }
        
        watchdog_cv.notify_one();
        if (watchdog && watchdog->joinable()) {
            watchdog->join();
        }
        if (sizeWatchdog.joinable()) {
            sizeWatchdog.join();
        }
        
        // Close pipes
        stdout_pipe.close();
        stderr_pipe.close();
        
        // Read trace file
        std::ifstream traceFile(traceLogPath);
        traceLog = std::string((std::istreambuf_iterator<char>(traceFile)), std::istreambuf_iterator<char>());
        
        // Check for errors
        if (wait_ec) {
            exitCode = -1;
        }

        if (timed_out)
        {
            traceLog += "[ERROR] terminated due to timeout\n";
        }
        
        if (size_exceeded)
        {
            traceLog += "[ERROR] terminated because the output log exceeded " + std::to_string(MAX_LLDB_STDOUT_SIZE/1024/1024) + " MB.\n";
        }
        
        if (exitCode != 0)
        {
            traceLog += "[ERROR] exited with code " + std::to_string(exitCode) + "\n";
        }
        else
        {
            traceLog += "[INFO] exited normally (code 0).\n";
        }
        
        stdLog = readTextFileWithLimit(stdoutLogPath, MAX_LLDB_STDOUT_SIZE);
    }
    catch (const std::exception &ex)
    {
        std::ifstream traceFile(traceLogPath);
        traceLog = std::string((std::istreambuf_iterator<char>(traceFile)), std::istreambuf_iterator<char>());
        
        if (timed_out)
        {
            traceLog += "[ERROR] terminated due to timeout\n";
        }
        
        if (size_exceeded)
        {
            traceLog += "[ERROR] terminated because the output log exceeded " + std::to_string(MAX_LLDB_STDOUT_SIZE/1024/1024) + " MB.\n";
        }
        
        traceLog += "[EXCEPTION] " + std::string(ex.what()) + "\n";
        
        stdLog = readTextFileWithLimit(stdoutLogPath, MAX_LLDB_STDOUT_SIZE);
    }
    
    auto truncation = extractFirstTruncationTag(stdLog);
    if(truncation.first > 0)
    {
        stdLog += "\n\nNOTE: the log has been truncated at line: " + std::to_string(truncation.first);
        stdLog += " due to size limit\n";
        stdLog += truncation.second;
        stdLog += "\n\n";
    }
    
    std::cout << "PROCESS ID: " << pid << std::endl;
    if(pid > 0)
    {
        killProcess(pid);
    }
    
    if(instrument)
    {
        switchToDefaultBuild(project);
    }
    
    // Read trace file
    if(boost_fs::exists(traceLogPath))
    {
        std::ifstream traceFile(traceLogPath);
        traceLog = std::string((std::istreambuf_iterator<char>(traceFile)), std::istreambuf_iterator<char>());
    }

    return stdLog;
}

//The purpose of this function is to run the test and collect all type of useful information.
//In case of execution without crashes and hangs - that would be the std::cout, std::cerr
//In case of crashes and hangs -  whatever debug information that will help LLM assisted analysis:
//stack, symbols, source, registers, memory
std::pair<std::string, std::string> Debugger::runTest(std::string& lldbLog, CCodeProject* project, const std::string& executableFilePath, //Full path to the executable, including executable name.
                       const std::string& testCommandLine, //This is the command line to execute the test. Could be multiple shell commands
                       const std::string& workingDir, //Working direcotry, the testCommandLine might assume specific working directory
                       int timeoutInSeconds, bool instrument, int& returnCode)
{
#if 0
    return runTrace(project, lldbLog, executableFilePath, testCommandLine, workingDir, timeoutInSeconds, instrument, returnCode);
#else
    return runLLDB(project, lldbLog, executableFilePath, testCommandLine, workingDir, timeoutInSeconds, instrument, returnCode);
#endif


#if 0
    std::vector<std::string> lldbBeforeArgs;
    
    //Set breakpoint on enter and exit for each function
    setBreakpoints(lldbBeforeArgs, project, "", {});
 
    std::vector<std::string> lldbAfterArgs;
    
    /*lldbAfterArgs.push_back("-o");
    lldbAfterArgs.push_back("thread backtrace all");
    
    lldbAfterArgs.push_back("-o");
    lldbAfterArgs.push_back("register read");
    
    lldbAfterArgs.push_back("-o");
    lldbAfterArgs.push_back("frame variable");*/
    
    lldbAfterArgs.push_back("-o");
    lldbAfterArgs.push_back("command script import " + m_scriptsDirectory + "/crash_handler.py");
    lldbAfterArgs.push_back("-o");
    lldbAfterArgs.push_back("check_crash");
    
    return runLLDB(lldbLog, testCommandLine, workingDir, lldbBeforeArgs, lldbAfterArgs, timeoutInSeconds);
#endif // TRACE_WITH_LLDB
}

std::string Debugger::getRunAnalysisProgress()
{
    std::string progress;
    int i=0;
    for(auto step : m_runAnalysisSteps) {
        progress += "\n" + step.m_debugNotes + "\n\n";
        progress += "Log summary: " + step.m_logSummary + "\n\n";
    }
    
    if(!progress.empty())
    {
        progress += "\n\n";
    }
    
    if(progress.empty()) {
        progress = "No previous steps\n";
    } else {
        progress = "Analysis with summary and debug notes\n\n" + progress;
    }
    
    return progress;
}

void Debugger::inferenceRunAnalysis(CCodeProject* project, const std::string& prompt, RunAnalysis& analysis, const std::string& debugTitle)
{
    web::json::value schema;
    setupSchema<RunAnalysis>(schema);
    
    web::json::value object;
    Cache cache;
    
    project->captureContext(std::string());
    project->inference(cache, prompt, schema, object);
    
    analysis.from_json(object);
    
#ifdef LIMIT_DEBUG_NOTES_SIZE
    //Let's check if the response is too verbose and unoptimal for context window management
    std::string feedback;
    bool provideFeedback = false;
    
    if(analysis.debug_notes.length() > 4000)
    {
        provideFeedback = true;
        feedback += "\nMore than 2048 characters in the 'debug_notes' field. Ideally keep to 3 paragraphs or fewer, under 2000 characters total!\n";
    }
    
    if(analysis.log_summary.length() > 4000)
    {
        provideFeedback = true;
        feedback += "\nMore than 2048 characters in the 'log_summary' field. Ideally keep to 3 paragraphs or fewer, under 2000 characters total!\n";
    }
    
    if(provideFeedback && !feedback.empty())
    {
        object = web::json::value();
        
        feedback += "\nThese are not a hard limits as we don't want to miss something important. However, if more concise response makes sense you can consider this recommendation.\n";
        
        project->inference(cache, feedback, schema, object);
        
        analysis.clear();
        analysis.from_json(object);
    }
#endif
    
    project->popContext();
    
    if(!debugTitle.empty() && !startsWithIgnoreCase(analysis.debug_notes, "PASS"))
    {
        analysis.debug_notes = debugTitle + analysis.debug_notes;
    }
    else if(startsWithIgnoreCase(analysis.debug_notes, "PASS"))
    {
        //We won't be here if the test passes
        analysis.debug_notes += "\n\n(Feedback from the agent's algorithm: You suggested that all tests pass, but unfortunately ";
        analysis.debug_notes += "outcomes from the test commands in the last run do not support this. ";
        analysis.debug_notes += "For more info, have a look at the section 'INFORMATION FROM THE LAST RUN STEP')\n";
    }
    
    DebugStep thisStep;
    thisStep.m_logSummary = analysis.log_summary;
    thisStep.m_debugNotes = analysis.debug_notes;
    m_runAnalysisSteps.push_back(thisStep);
    
    m_runAnalysisStep++;
}

void Debugger::analysisLogSection(CCodeProject* project,
                          const std::string& logSection,
                          RunAnalysis& analysis)
{
    std::string application = project->getProjectName();
    std::string progress = getRunAnalysisProgress();
    
    Prompt promptAnalysis("RunAnalysis.txt",{
                        {"app_info", m_appInfo},
                        {"application", application},
                        {"log", logSection},
                        {"progress", progress}
    });
    
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ASSISTANT);
    
    Client::getInstance().setStepHint("Analyzing log section...");
    inferenceRunAnalysis(project, promptAnalysis.str(), analysis);
    
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
}

bool Debugger::visibleTraceAndLog(std::ostream& log, const std::pair<std::string, uint32_t>& frame)
{
    std::string key = frame.first + ":" + std::to_string(frame.second);
    
    if(!m_contextVisibility.visibleTraceFrame(frame.first, frame.second))
    {
        log << "\nTrace for function invocation: " << key << "\n";
        log << "[[provided in the context]]\n\n";
    }
    else
    {
        m_tracer.printFrame(log, frame.first, frame.second);
    }
    
    if(!m_contextVisibility.visibleFunctionLog(frame.first, frame.second))
    {
        log << "\nLog for function invocation: " << key << "\n";
        log << "[[provided in the context]]\n\n";
    }
    else
    {
        auto logSection = m_logger.logMessagesForFunction(frame.first, 0,
                                                          frame.second, LOG_TRACE_SIZE);
        
        if(logSection.second > 0)
        {
            log << std::endl << "Logged events for: " << frame.first;
            log << " invocation: " << frame.second << std::endl;
            log << logSection.first << std::endl << std::endl;
        }
    }
    
    log << std::endl;
    return true;
}

std::string Debugger::getStackTrace(CCodeProject* project, const std::string& stack, bool log, uint32_t maxTailToPrint)
{
    std::string stackInfo;

    auto frames = TraceAnalyzer::parseStack(stack);
    
    stackInfo += "\nCall stack: " + stack + "\n";

    const std::size_t total = frames.size();
    if (total == 0) {
        return stackInfo;
    }

    // 0 => no limit, otherwise cap by maxTailToPrint
    const std::size_t tailLimit = (maxTailToPrint == 0) ? total : std::min<std::size_t>(total, maxTailToPrint);
    std::string tailLimitStr = std::to_string(tailLimit);
    stackInfo += "Prints the last " + tailLimitStr + " fames\n\n";

    // Start index so we get only the last `tailLimit` frames
    const std::size_t start = total - tailLimit;

    for (std::size_t i = total; i-- > start; )
    {
        const auto& stackFrame = frames[i];

        std::stringstream ssBp;
        
        if(log)
        {
            visibleTraceAndLog(ssBp, stackFrame);
        }
        else
        {
            std::string key = stackFrame.first + ":" + std::to_string(stackFrame.second);
            if(!m_contextVisibility.visibleTraceFrame(stackFrame.first, stackFrame.second))
            {
                
                ssBp << "\nTrace for function invocation: " << key << "\n";
                ssBp << "[[provided in the context]]\n";
                ssBp << std::endl;
            }
            else
            {
                m_tracer.printFrame(ssBp, stackFrame.first, stackFrame.second);
            }
        }
        
        stackInfo += ssBp.str();
        stackInfo += "\n";
    }

    return stackInfo;
}

std::pair<bool, std::string> Debugger::analysisFullTrace(CCodeProject* project, RunAnalysis& analysis, const TestDef& test)
{
    std::string analysisHint;
    bool requireSeparateStep = false;
    
    if(m_nextStep.action_type == "debug_function")
    {
        std::string debuggedFunction = m_nextStep.action_subject;
        
        std::string debugNotes;
        if(!checkFunctionExists(project, debuggedFunction, debugNotes))
        {
            analysisHint += "The debugged function '" + debuggedFunction + " doesn't exist in the project. ";
            analysisHint += "The trace will not contain any events recorded for this function.\n\n";
        }
        else
        {
            //First ensure we have the top frame for each breakpoint printed
            //We can't have that many breakpoints to worry about the context size, right?
            for(auto bp : m_nextStep.breakpoints)
            {
                const auto& foundBp = m_tracer.findBreadkpoints(debuggedFunction, bp->source_line);
                std::stringstream ssBreakpoints;
                for(auto bpFrame : foundBp)
                {
                    auto stack = TraceAnalyzer::parseStack(bpFrame->m_stack);
                    visibleTraceAndLog(ssBreakpoints, stack.back());
                    
                    //No need for that, function source and info will be provided in the prompt
                    //DebugStep dummy;
                    //ssBreakpoints << stepFunctionInfo(project, debuggedFunction, m_nextStep.motivation, m_nextStep.invocation, m_nextStep.line_number, dummy);
                }
                analysisHint += ssBreakpoints.str();
            }
            
            bool hasBPHits = false;
            bool hitTheLimit = false;
            for(auto bp : m_nextStep.breakpoints)
            {
                const auto& foundBp = m_tracer.findBreadkpoints(debuggedFunction, bp->source_line);
                if(foundBp.empty())
                {
                    analysisHint += "Information for the breakpoint at function: '" + debuggedFunction + "' line: ";
                    analysisHint += std::to_string(bp->source_line);
                    analysisHint += " couldn't be found in the detailed trace.";
                    analysisHint += " A breakpoint may not be hit because either the application never executed that line of code, or the breakpoint's condition wasn't satisfied.\n\n";
                }
                else
                {
                    for(auto bpFrame : foundBp)
                    {
                        analysisHint += "Stack trace for the breakpoint at function: '" + debuggedFunction + "' line: " + std::to_string(bp->source_line);
                        analysisHint += getStackTrace(project, bpFrame->m_stack, false, 3);
                        analysisHint += "\n\n\n";
                        
                        if(hitTheLimit) break;
                    }
                    
                    if(hitTheLimit) break;
                }
            }
            
            if(hasBPHits)
            {
                requireSeparateStep = true;
                
                analysisHint += "Investigate the above stack traces for each of the breakpoints\n";
                analysisHint += "Format of each entry in the stack: function_name:invocatoin_id\n";
                analysisHint += "For each stack trace, start from the last call and for each function up to the firs call";
                analysisHint += " verify the input arguments for unexpected values and corrupted memory. ";
                analysisHint += "Inspect the live variable at the available trace points. ";
                analysisHint += "Try to understand more about STL containers and check if they are corrupted. ";
                analysisHint += "If you identify any issues, try to understand from where they start ";
                analysisHint += "and if they are in the functions executed before the functions from the stack, ";
                analysisHint += "consider using 'function_info' and other actions to trace the root cause of the problem. ";
                analysisHint += "Traces and logs for all invocations from the breakpoints call stacks are available on request with 'function_info' action until the next run.";
            }
        }
    }
    
    std::string stdoutChecks = test.checksStdout();
    if(!stdoutChecks.empty())
    {
        analysisHint += "Some commands in this test use ECMAScript regex to FULLY match stdout:\n\n";
        analysisHint += stdoutChecks;
        analysisHint += "\n\nUse std::cout with the << operator (avoid other methods) for output that must match these patterns. ";
        analysisHint += "Use PRINT_TEST for diagnostic output during debugging—it does not appear in stdout.\n\n";
    }
    
    //If we have a memo that means crash/hang that must be fixed first.
    //Generate the analysis and return.
    std::string memoFile = m_workingDirectory + "/memo.txt";
    //m_memo.loadFromFile(memoFile);
    //auto memoFrame = m_memo.getLastFrame(true);
    auto memoFrames = m_tracer.loadStackTrace(memoFile, m_workingDirectory + "/stack");
    if(!memoFrames.empty())
    {
        requireSeparateStep = true;
        std::string memoTrace;
        
        auto memoFrame = memoFrames.back();
        bool deepRecursion = false;
        if(memoFrame.first == "DEEP_RECURSION" && memoFrame.second < 0)
        {
            deepRecursion = true;
        }
        
        auto frameTrace = m_tracer.getFrame(memoFrame.first, memoFrame.second);
        memoTrace += getStackTrace(project, frameTrace->m_stack, true, 5);
        
        if(deepRecursion)
        {
            analysisHint += "NOTE: The application entered deep recursion with the top of the stack:\n\n";
            analysisHint += frameTrace->m_stack + "\n\n";
            
            //analysisHint += "Here are the callstack and events recorded before the crash:\n\n";
            analysisHint += "Here are full traces for some of the functions in the call stack before the recursion:\n\n";
            analysisHint += memoTrace;
            
            auto stack = TraceAnalyzer::parseStack(frameTrace->m_stack);
            analysisHint += "Investigate the stack trace : " + frameTrace->m_stack + "\n";
            analysisHint += "Format of each entry in the stack the format is: function_name:invocatoin_id\n";
            analysisHint += "Note that the printed stack represents only the top of the stack ";
            analysisHint += "with frames skipped after the deep recursion has been detected\n";
            analysisHint += "Start from the last recursive call and up to the '" + stack.front().first + "' function ";
            analysisHint += " verify the input arguments for unexpected values and corrupted memory. ";
            analysisHint += "Inspect the live variable at the available trace points. ";
            analysisHint += "If you identify any issues, try to understand from where they start. ";
            analysisHint += "Try to understand more about the STL containers and confirm they aren't corrupted. ";
            analysisHint += "Try to understand if this recursion is expected at all and if so ";
            analysisHint += "why the application doesn't exit from it on time or crashed? ";
            analysisHint += "If the issues are in the functions executed before the functions from the stack, ";
            analysisHint += "consider using 'function_info' and other actions to trace the root cause of the problem.";
        }
        else
        {
            analysisHint += "NOTE: The application terminated unexpectedly in function '";
            analysisHint += memoFrame.first + "' invocation: " + std::to_string(memoFrame.second) + "\n\n";
            
            analysisHint += "call stack before the crash: " + frameTrace->m_stack + "\n";
            //analysisHint += "Here are the callstack and events recorded before the crash:\n\n";
            analysisHint += "Here are full traces for each of the functions in the call stack before the crash:\n\n";
            analysisHint += memoTrace;
            
            auto stack = TraceAnalyzer::parseStack(frameTrace->m_stack);
            analysisHint += "Investigate the stack trace : " + frameTrace->m_stack + "\n";
            analysisHint += "Format of each entry in the stack is: function_name:invocatoin_id\n";
            analysisHint += "Start from the last call '" + stack.back().first + "' and for each function call up to '" + stack.front().first;
            analysisHint += "' verify the input arguments for unexpected values and corrupted memory. ";
            analysisHint += "Inspect the live variable at the available trace points. ";
            analysisHint += "If you identify any issues, try to understand from where they start. ";
            analysisHint += "Try to understand more about the STL containers and confirm they aren't corrupted. ";
            analysisHint += "Try to understand what is the last function, if any, called by " + stack.back().first;
            analysisHint += " Was that call successful? ";
            analysisHint += "If the issues are in the functions executed before the functions from the stack, ";
            analysisHint += "consider using 'function_info' and other actions to trace the root cause of the problem.";
            
            //For now let the LLM to request function_info by itself
            //analysis.m_function = memoFrame.first;
        }
        
        return std::make_pair(requireSeparateStep,analysisHint);
    }
    
    
    auto lastFrame = m_tracer.getLastFrame(false);
    
    if(lastFrame)
    {
        std::string lastSectionType;
        if(lastFrame->m_sections.size() > 0)
        {
            lastSectionType = lastFrame->m_sections.back()->m_type;
        }
        
        bool lastFrameIsNotMain = false;
        if(lastFrame->m_invocation.first != m_system)
        {
            requireSeparateStep = true;
            
            analysisHint += "The last recorded frame in the detailed trace is for function '" + lastFrame->m_invocation.first;
            analysisHint += "'. Usually the last frame should be for the function '" + m_system + "'. ";
            //analysisHint += "This suggest crash or hang in the function '" + lastFrame->m_invocation.first + "'\n\n";
            analysisHint += "This suggest crash or hang in the application after the call to '" + lastFrame->m_invocation.first + "'\n\n";
            lastFrameIsNotMain = true;
            
            std::stringstream ssFrame;
            m_tracer.printFrame(ssFrame, lastFrame->m_invocation.first, lastFrame->m_invocation.second);
            
            analysisHint += "Detailed trace for '" + lastFrame->m_invocation.first + "' :\n";
            analysisHint += ssFrame.str() + "\n";
        }
        
        if(lastSectionType.empty() ||
           (lastSectionType != "exit" && lastSectionType != "return"))
        {
            if(lastSectionType.empty())
            {
                lastSectionType = "empty";
            }
            
            analysisHint += "The last recorded section for the function '" + lastFrame->m_invocation.first;
            analysisHint += "' is of type: '" + lastSectionType + "'. ";
            analysisHint += "Usually the last recorded section for a given function should be 'exit' or 'return'. ";
            analysisHint += "This suggest that the application crashes or hangs in the function '";
            analysisHint += lastFrame->m_invocation.first + "' after section '" + lastSectionType + "'\n\n";
        }
        else if(lastFrameIsNotMain)
        {
            analysisHint += "The last event for '" + lastFrame->m_invocation.first + "' is exit/return, so potentially the issue is after this function\n\n";
            
            //TODO: Try to understand the call stack upstream and to which function to have a look
        }
        else if(!lastFrameIsNotMain &&
                (lastSectionType == "exit" || lastSectionType == "return"))
        {
            analysisHint += "The application executed without crashes or hangs! ";
            
            if(!analysis.m_testResult)
            {
                analysisHint += "However, results for some of the commands don't match expected outcomes. ";
                analysisHint += " Further investigation and debugging are required!\n";
            }
            else
            {
                analysisHint += "Verify that the test results match expected outcomes. ";
                analysisHint += "If they don't, investigate and debug.";
            }
        }
    }
    else if(m_tracer.getFramesCount() == 0)
    {
        analysisHint += "No frames recorded in the detailed trace (the provided full trace is less verbose).";
        analysisHint += " This suggest problems in the build system to successfully build instrumented binary.";
        analysisHint += " Anothrer 'run_test' action might fix the build\n";
    }
    else if(m_system != "main")
    {
        analysisHint += "The application executed without crashes or hangs! ";
        if(!analysis.m_testResult)
        {
            analysisHint += "However, results for some of the commands don't match expected outcomes. ";
            analysisHint += " Further investigation and debugging are required!\n";
        }
        else
        {
            analysisHint += "Verify that the test results match expected outcomes. ";
            analysisHint += "If they don't, investigate and debug.";
        }
    }
    
    //Now check the log
    auto lastInvocation = m_logger.logGetLastInvocation();
    
    if(lastInvocation.second != 0)
    {
        if(lastInvocation.first != m_system)
        {
            analysisHint += "From the log, the last logged function is '" + lastInvocation.first;
            analysisHint += "'. This might be normal, but it is worth checkin if the '" + m_system + "' function";
            analysisHint += " is expected to output with PRINT_TEST something after this function.";
            analysisHint += " If so, this might suggest crash or hang after the call to function '" + lastInvocation.first + "'\n\n";
        }
    }
    else
    {
        analysisHint += "Couldn't find logged messages from application functions in the log. This is not expected.";
        //analysisHint += " Consider anothrer 'run_test' action \n";
        analysisHint += " Anothrer 'run_test' action might fix the build\n";
    }
    
    return std::make_pair(requireSeparateStep,analysisHint);
}

std::string Debugger::analysisFrameTrace(CCodeProject* project, const std::string& function, uint32_t invocation)
{
    std::string analysisHint;
    
    std::string debugNotes;
    if(!checkFunctionExists(project, function, debugNotes))
    {
        analysisHint += "The function '" + function + " doesn't exist in the project. ";
        analysisHint += "The trace will not contain any events recorded for this function.\n\n";
        return analysisHint;
    }
    
    std::shared_ptr<const TraceAnalyzer::Frame> foundFrame = nullptr;
    
    if(invocation > 0)
    {
        foundFrame = m_tracer.getFrame(function, invocation);
        if(!foundFrame)
        {
            foundFrame = m_tracer.getLastInvocation(function);
        }
    }
    else
    {
        foundFrame = m_tracer.getLastInvocation(function);
    }
    
    if(foundFrame)
    {
        std::string lastSectionType;
        if(foundFrame->m_sections.size() > 0)
        {
            lastSectionType = foundFrame->m_sections.back()->m_type;
        }
        
        if(lastSectionType.empty() ||
           (lastSectionType != "exit" && lastSectionType != "return"))
        {
            if(lastSectionType.empty())
            {
                lastSectionType = "empty";
            }
            
            analysisHint += "The last recorded section for the function '" + foundFrame->m_invocation.first;
            analysisHint += "' is of type: '" + lastSectionType + "'. ";
            analysisHint += "Usually the last recorded section for a given function should be 'exit' or 'return'. ";
            analysisHint += "This suggest that the application crashes or hangs in the function '";
            analysisHint += foundFrame->m_invocation.first + "' after section '" + lastSectionType + "'\n\n";
        }
    }
    else
    {
        analysisHint += "There are no recorded trace events for the function '" + function;
        analysisHint += "' this suggest the function has not been executed.\n\n";
    }
    
    return analysisHint;
}

void Debugger::analysisTrace(CCodeProject* project,
                             const std::string& dbgTestLog,
                             const std::string& traceLog,
                             RunAnalysis& analysis, const TestDef& test)
{
    m_tracer.loadFromString(traceLog);
    
    std::stringstream ss;
    m_tracer.print(ss, true, -1, true, std::string());
    std::string conciseLog = ss.str();
    
    std::string application = project->getProjectName();
    std::string trajectory = getTrajectory(0, -1, true, true);
    
    //std::string progress = getRunAnalysisProgress();
    
    std::string traceDescStr = "\n\nFormats of the trace and log are specified in 'TRACE DESCRIPTION' and 'LOG DESCRIPTION' sections\n\n";
    
    auto analysisHintIt = analysisFullTrace(project, analysis, test);
    std::string analysisHint = analysisHintIt.second;
    std::string analysisHint1;
    std::string dbg_test_log;
    bool requiresAnalysis = analysisHintIt.first;
    if(!analysisHint.empty())
    {
        analysisHint = "//Here is a hint from my analysis:\n" + analysisHint + "\n//The analysis hint ends here\n\n";
    }
    
    RunAnalysis fullTraceAnalysis;
    std::string hitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);

    systemAnalysis(project, analysisHint, fullTraceAnalysis);
    analysis = fullTraceAnalysis;
}

std::set<std::string> Debugger::getTestTextFiles(CCodeProject* project, const TestDef& test, const std::string& workingDirectory)
{
    return test.getRewardHackingTestFiles(workingDirectory);
}

bool Debugger::rewardHackingAnalysis(CCodeProject* project,
                                     const TestDef& test,
                                     std::string& review,
                                     std::string& hint)
{
    std::set<std::string> testFiles = getTestTextFiles(project, test, m_workingDirectory);
    
    std::string outputFilesContent;
    
    auto llmConfig = Client::getInstance().currentLLMConfig();
    const uint32_t maxInfoSize = (llmConfig->context_size * 1024) * CHARACTERS_PER_TOKEN * 0.7f;
    
    bool fitInContext = true;
    std::string fitInContextIssues;
    for(auto file : testFiles)
    {
        std::string fileName = boost_fs::path(file).filename().string();
        
        if(!boost_fs::exists(m_workingDirectory + "/" + file))
        {
            continue;
        }
        
        auto fileSize = boost_fs::file_size(m_workingDirectory + "/" + file);
        if(fileSize > maxInfoSize)
        {
            fitInContext = false;
            fitInContextIssues += "Size of the file: " + file + " it too big: " + std::to_string(fileSize) + "\n\n";
            continue;
        }
        
        const int maxCharacters = -1; //No limit here!!!
        std::string content = getFileInfo(project, file, 1, maxCharacters);

        outputFilesContent += "\n//File " + fileName + " starts here\n\n";
        outputFilesContent += content;
        outputFilesContent += "\n//File " + fileName + " ends here\n";
    }
    
    if(outputFilesContent.length() > maxInfoSize)
    {
        fitInContextIssues += "Combined size of the files, used or generated by the test,";
        fitInContextIssues += " required for reward hacking evaluation is more than " + std::to_string(maxInfoSize);
        fitInContextIssues += " bytes. This is not permited by design!\n\n";
    }
    
    if(!fitInContextIssues.empty())
    {
        review = "The test has passed with expected results (see 'INFORMATION FOR THE LAST RUN')";
        review += " but there are indications for reward-hacking practices or other issues:\n\n";
        review += fitInContextIssues;
       // review += "\n\nAll the issues mentioned MUST BE FIXED in order to successfully pass the reward hacking evaluation for this test!\n\n";
        return;
    }
    
    std::string privateTestInfo;
    std::string privateTestHint;
    bool privateTestsPass = true;
    if(boost_fs::exists(m_privateWorkingDirectory))
    {
        project->buildBinary(true);
        std::string workingDirectory = m_workingDirectory;
        for (const auto& entry : boost_fs::directory_iterator(m_privateWorkingDirectory))
        {
            boost_fs::path filePath = entry.path();
            if(!entry.is_directory()) {
                continue;
            }
            
            TestDef privateTest;
            deployToWorkingDirectory(project, filePath.string(), false, privateTest);
            
            std::string lldbOnlyLog, debugLogTest, debugLogApp;
            
            RunAnalysis analysis;
            bool testResult = execTestScript(project, privateTest,
                                             analysis, lldbOnlyLog,
                                             debugLogTest, debugLogApp, false);
            privateTestsPass = privateTestsPass && testResult;
            if(!privateTestsPass)
            {
                privateTestHint = privateTest.io_hint;
                break;
            }
        }
        
        if(!privateTestsPass)
        {
            privateTestInfo += "\nI've executed private tests on the same feature but with different configuration and the test did not pass!\n";
            privateTestInfo += "This is a strong indication for presense of reward-hacking practices or critical bugs in the code generated for this project. Please review it!\n";
            
            if(!privateTestHint.empty())
            {
                privateTestInfo += "Here is a direct hint from the failed private test:\n";
                privateTestInfo += privateTestHint;
            }
            privateTestInfo += "\n\n";
        }
        
        m_workingDirectory = workingDirectory;
    }
    
    std::string unitTestHint;
    if(m_system != "main")
    {
        unitTestHint += "\nWe are currently debugging a unit test for the '" + m_system + "' function. ";
        unitTestHint += "Note that the unit test may focus only on certain features and may not have full coverage. ";
        unitTestHint += "Do not flag this as reward-hacking. Instead, focus only on the behavior of '" + m_system + "' ";
        unitTestHint += "and functions called directly or indirectly by it, and how they satisfy the test cases ";
        unitTestHint += "without reward-hacking.\n\n";
    }
    
    Context rewardHackingCtx;
    rewardHackingCtx.reset();
    Context* prevContext = project->setActiveContext(&rewardHackingCtx);
    
    Prompt role("DebuggerRole.txt",{});
    project->pushMessage(role, "system", true);
    project->pushMessage(project->getProjectDescription(), "user", true);
    
    std::string testDescription = getTestDescription(project, test, "");
    project->pushMessage(testDescription, "user", true);
    
    std::string appInfo = getHighLevelAppInfo(project, m_system, PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH);
    appInfo += "\n\n";
    appInfo += m_lastRunTestLog;
    
    Prompt rewardHacking("RewardHacking.txt",{
                        {"app_info", appInfo},
                        {"private_test", privateTestInfo},
                        {"unit_test", unitTestHint},
                        {"output_files", outputFilesContent}
    });
    
    Cache cache;
    bool truncated = false;

    std::string rewardHackingReview = "review";
    project->inference(cache, rewardHacking, rewardHackingReview, &truncated);
    bool reviewPass = startsWithIgnoreCase(rewardHackingReview, "NO");
    bool selfReviewFail = startsWithIgnoreCase(rewardHackingReview, "YES");
    while(!reviewPass && !selfReviewFail)
    {
        rewardHackingReview = "review";
        truncated = false;
        project->inference(cache, "You must start your response with YES or NO", rewardHackingReview, &truncated);
        
        reviewPass = startsWithIgnoreCase(rewardHackingReview, "NO");
        selfReviewFail = startsWithIgnoreCase(rewardHackingReview, "YES");
    }
    
    project->setActiveContext(prevContext);
    
    review = "The test has passed with expected results (see 'INFORMATION FOR THE LAST RUN')";
    review += " but there are indications for reward-hacking practices or other issues:\n\n";
    
    review += rewardHackingReview;
    
    hint = privateTestHint;
    return privateTestsPass && reviewPass;
}

std::string Debugger::getHighLevelAppInfo(CCodeProject* project, const std::string& functionName,
                                          int functionsMaxDepth, int callGraphMaxDepth)
{
    return project->getHighLevelAppInfo(functionName, functionsMaxDepth, callGraphMaxDepth);
}

std::string Debugger::getRequestedInfo(CCodeProject* project, int allFunctions, int callGraph,
                             const std::string& functionName,
                             const std::vector<std::shared_ptr<std::string>>& functions,
                             const std::vector<std::shared_ptr<std::string>>& data_types,
                             const std::vector<std::shared_ptr<std::string>>& test_files)
{
    std::string info;
    std::string hitTheLimitMsg = "Some requested information may not be provided due to size limits! ";
    hitTheLimitMsg += "If you need more information, summarize the important details in the 'Debug notes' ";
    hitTheLimitMsg += "and request more information in the next step";
    
    #define CHECK_INFORMATIO_REQUEST_SIZE if(info.length() > MAX_INFORMATIO_REQUEST_SIZE){ info += "\n"; info += hitTheLimitMsg + "\n"; return info; }
    
    if(allFunctions)
    {
        info += getHighLevelAppInfo(project, functionName, allFunctions, 0);
    }
    
    CHECK_INFORMATIO_REQUEST_SIZE
    
    if(callGraph)
    {
        info += getHighLevelAppInfo(project, functionName, 0, callGraph);
    }
    
    CHECK_INFORMATIO_REQUEST_SIZE
    
    std::vector<std::shared_ptr<std::string>> _functions = functions;
    for(auto file : test_files)
    {
        std::string fileName = boost_fs::path(*file).filename().string();
        
        if(!m_contextVisibility.visibleFile(fileName, 1))
        {
            continue;
        }

        int maxCharacters = LOG_SECTION_SIZE;
        std::string content = getFileInfo(project, *file, 1, maxCharacters);

        info += "\n//File " + fileName + " starts here\n\n";
        info += content;
        info += "\n//File " + fileName + " ends here\n";
        
        CHECK_INFORMATIO_REQUEST_SIZE
    }
    
    for(auto fun : _functions)
    {
        //The function is already in the context
        auto funKey = makeStringNumberPair(*fun);
        
        if(!m_contextVisibility.visibleFunction(funKey.first))
        {
            continue;
        }
        
        std::string functionInfo;
        auto it = project->nodeMap().find(funKey.first);
        if(it != project->nodeMap().end())
        {
            auto ccNode = (const CCodeNode*)it->second;
            if(!ccNode) continue;
            
            if(!ccNode->m_prototype.brief.empty())
            {
                std::string briefStr = ccNode->m_prototype.brief;
                if(briefStr.length() > BRIEF_MAX_CHARACTERS)
                {
                    briefStr = truncateWithNoteUtf8(briefStr, BRIEF_MAX_CHARACTERS, " [[...truncated]]");
                }
                
                info += ccNode->m_brief.func_name + ": " + briefStr + "\n\n";
            }
            
            if(!ccNode->m_implementation.m_source.empty())
            {
                info += printFunctionSource(project, ccNode->m_brief.func_name, ccNode->m_implementation.m_source) + "\n\n";
            }
            
            CHECK_INFORMATIO_REQUEST_SIZE
        }
    }
    
    for(auto data : data_types)
    {
        //The data type is already in the context
        if(!m_contextVisibility.visibleData(*data))
        {
            continue;
        }
        
        std::string owningPath;
        auto dataDef = project->findData(*data, owningPath);
        if(!dataDef) continue;
        
        info += dataDef->m_typeDef.m_definition + ";\n\n";
        
        CHECK_INFORMATIO_REQUEST_SIZE
    }
    
    return info;
}

std::string Debugger::getFileInfo(CCodeProject* project, const std::string& file, int lineToStartFrom, int maxCharacters)
{
    std::string info;
    
    std::string fileName = boost_fs::path(file).filename().string();
    std::string fileExt = boost_fs::path(file).extension().string();
    std::string fullPath = m_workingDirectory + "/" + fileName;
    
    if(fileName == "stdout.log")
    {
        info += "\n//File '" + fileName + "' is not accessible. ";
        info += "To obtain log information from the most recent test run use 'log_info' and 'function_info' actions. ";
        info += "The stdout is available in the TEST SCRIPT EXECUTION LOG\n\n";
        
        return info;
    }
    
    if(fileName == "console.log")
    {
        info += "\n//File '" + fileName + "' is not accessible. ";
        info += "To obtain log information from the most recent test run use 'log_info' and 'function_info' actions. ";
        info += "The stdout is available in the TEST SCRIPT EXECUTION LOG\n\n";
        return info;
    }
    
    if(fileName == "trace.txt")
    {
        info += "\n//File '" + fileName + "' is not accessible. To obtain trace information from the most recent test run use trace_info and function_info actions.\n\n";
        return info;
    }
    
    if(fileName == ".DS_Store" || fileName == "trace.txt")
    {
        info += "\n//File '" + fileName + "' is not accessible.\n\n";
        return info;
    }
    
    if(fileName == "main" || fileName == "main.cpp") {
        info += "\n//File '" + fileName + "' Is the main executable file for the application being debugged. Can't show the content\n\n";
        return info;
    }
    
    if(fileExt == ".cpp" || fileExt == ".h")
    {
        std::string functionName = boost_fs::path(file).stem().string();
        auto fIt = project->nodeMap().find(functionName);
        if(fIt != project->nodeMap().end())
        {
            info += "\n//The file '" + fileName + "' is a C++ source for the function with the same name. Use 'function_info' action instead\n\n";
            return info;
        }
    }
    
    std::ifstream fileStream(fullPath);
    if(!fileStream)
    {
        info += "\n//Unable to open file '" + fileName + "'\n\n";
        return info;
    }
    
    std::string content((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
    
    if(isBinaryFile(fullPath)) {
        info += "\n//File '" + fileName + "' Is a binary file. Can't show the content\n\n";
        return info;
    }
    
    uint32_t fileLinesCount = (uint32_t)countLines(content);
    
    if(lineToStartFrom <= 0)
    {
        lineToStartFrom = 1;
    }
    
    std::string lineToStartFromStr = std::to_string(lineToStartFrom);
    info += "\n//File " + fileName + " content starts here from line: " + lineToStartFromStr + "\n\n";
    
    // Extract content from lineToStartFrom with maxCharacters limit
    std::istringstream contentStream(content);
    std::string line;
    std::string fileFragment;
    int currentLine = 1;
    int endLine = lineToStartFrom - 1;
    int charactersCount = 0;
    
    // Skip lines before lineToStartFrom
    while (currentLine < lineToStartFrom && std::getline(contentStream, line)) {
        currentLine++;
    }
    
    // Collect lines starting from lineToStartFrom until we reach maxCharacters
    while (std::getline(contentStream, line)) {
        // Add newline if not the first line in fragment
        if (!fileFragment.empty()) {
            fileFragment += "\n";
            charactersCount += 1;
        }
        
        // Check if adding this line would exceed maxCharacters
        if (maxCharacters > 0 && charactersCount + line.length() > maxCharacters) {
            // Add partial line if there's room
            int remainingChars = maxCharacters - charactersCount;
            if (remainingChars > 0) {
                fileFragment += line.substr(0, remainingChars);
                endLine = currentLine;
            }
            break;
        }
        
        fileFragment += line;
        charactersCount += line.length();
        endLine = currentLine;
        currentLine++;
    }
    
    info += printLineNumbers(fileFragment, (lineToStartFrom-1));
    
    info += "\n//File " + fileName + " ends here. ";
    info += "Printed from line: " + lineToStartFromStr + " to line: " + std::to_string(endLine) + ". ";
    info += "Total lines in the file: " + std::to_string(fileLinesCount);
    
    return info;
}

std::string Debugger::getStepInfo(CCodeProject* project, const TestDef& test, int stepId)
{
    std::string info;
    
    std::string trajectoryDir = project->getProjDir() + "/debug/" + test.name + "/trajectory";
    
    if(!boost_fs::exists(trajectoryDir))
    {
        info += "\nUnable to find any recorded steps!\n";
        return info;
    }
    
    std::string stepIdStr = std::to_string(stepId);
    
    auto stepsRange = getConsecutiveSteps(trajectoryDir);
    
    if( stepsRange.first > stepId || stepsRange.second < stepId)
    {
        info += "\nInformation for the requested step " + stepIdStr + " doesn't exist! ";
        info += " The available steps range is from " + std::to_string(stepsRange.first);
        info += " to " + std::to_string(stepsRange.second) + "\n";
        return info;
    }
    
    std::string stepIdDir = "/step_" + stepIdStr;
    
    std::string tracjectoryFile = trajectoryDir + stepIdDir + "/tracjectory.json";
    if(!boost_fs::exists(tracjectoryFile))
    {
        info += "\nUnable to load information for the requested step " + stepIdStr + ". The data might be corrupted!\n";
        return info;
    }
    
    //Load the trajectory configuration json file
    web::json::value trajectoryCfg;
    hen::loadJson(trajectoryCfg, tracjectoryFile);
    
    //These properties are necessary. They must be in the json object
    if(!trajectoryCfg.has_field(U("previousSteps")))
    {
        info += "\nUnable to load information for the requested step " + stepIdStr + ". The data might be corrupted!\n";
        return info;
    }
    
    uint32_t previousSteps = trajectoryCfg[U("previousSteps")].as_number().to_uint32();
    
    //Load trajectory
    uint32_t startStep = previousSteps;
    
    std::string requestedStepDir = trajectoryDir + stepIdDir + "/";
    info += "\n********** SUMMARIZED TRAJECTORY AT DEBUGGING STEP " + stepIdStr + " START! **********\n";
    std::string summary = hen::loadFile(requestedStepDir + "summary.txt");
    info += summary + "\n";
    
    std::string runInfo = loadTestLogFromStep(project, test, stepId);
    info += runInfo;
    
    for(uint32_t s = startStep; s < stepId; ++s)
    {
        uint32_t stepIndex =  s + 1;
        std::string stepIndexStr = std::to_string(stepIndex);
        std::string stepDir = trajectoryDir + "/step_" + stepIndexStr + "/";
        
        if(!boost_fs::exists(stepDir))
        {
            info += "\nInformation for step " + stepIndexStr + " doesn't exist! Skipping this step.\n";
            continue;
        }
        
        DebugStep dbgStep;
        
        std::string dbgStepPath = stepDir + "dbgStep.json";
        dbgStep.load(dbgStepPath);
        
        info += "STEP " + stepIndexStr + " ";
        if(stepIndex == stepId)
        {
            info += dbgStep.fullInfo() + "\n";
        }
        else
        {
            info += dbgStep.summary() + "\n";
        }
    }
    
    info += hen::loadFile(requestedStepDir + "info.txt");
    info += "\n********** SUMMARIZED TRAJECTORY AT DEBUGGING STEP " + stepIdStr + " END! **********\n";
    
    return info;
}

// A helper function to check if a given value is within any interval.
static bool isWithinIntervals(uint32_t value, const std::vector<std::pair<uint32_t, uint32_t>>& intervals) {
    return std::any_of(intervals.begin(), intervals.end(),
                       [value](const std::pair<uint32_t, uint32_t>& interval) {
                           return (value >= interval.first && value < interval.second);
                       });
}

std::string Debugger::getTestDescription(CCodeProject* project, const TestDef& test, const std::string& regressionTestPath)
{
    if(!regressionTestPath.empty())
    {
        //Build the prompt for functionality delta with the most recently passed test
        
        std::stringstream testFuncDelta;
        
        testFuncDelta << "PASSED TEST START" << std::endl << std::endl;
        testFuncDelta << "The follwing test is a subset of the current test and has passed both public and private tests. ";
        testFuncDelta << "Consider this when investigating issues with the current test and the added new features. ";
        testFuncDelta << "It doesn't mean that we can't have issues with features previously passing tests but the risk should be lower";
        testFuncDelta << std::endl << std::endl;
        
        TestDef regressionTest;
        web::json::value regressionTestJson;
        loadJson(regressionTestJson, regressionTestPath + "/public/test.json");
        
        std::string jsonFile = "```json\n";
        jsonFile += getFileContent(regressionTestPath + "/public/test.json");
        jsonFile += "```\n\n";
        
        testFuncDelta << jsonFile;
        
        regressionTest.from_json(regressionTestJson);
        
        const auto& regressionInputFiles = regressionTest.getInputFiles();
        for(auto file : regressionInputFiles)
        {
            std::string fileContent = getFileContent(regressionTestPath + "/public/" + file);
            testFuncDelta << "File: " << file;
            testFuncDelta << "\n```" << boost_fs::path(file).extension() << std::endl;
            testFuncDelta << fileContent;
            testFuncDelta << "```\n\n";
        }
        
        testFuncDelta << "PASSED TEST END" << std::endl << std::endl;
        
        testFuncDelta << "Files from the current test, useful to analyze the delta in the functionality between the passed and the current test:\n\n";
        const auto& inputFiles = test.getInputFiles();
        for(auto file : inputFiles)
        {
            std::string fileContent = getFileContent(m_workingDirectory + "/" + file);
            testFuncDelta << "File: " << file;
            testFuncDelta << "\n```" << boost_fs::path(file).extension() << std::endl;
            testFuncDelta << fileContent;
            testFuncDelta << "```\n\n";
        }
        
        testFuncDelta << "Consider to review the delta in the functionality between the current test and the already 'PASSED TEST' (if presented) it might help when investigating issues.\n\n";
        
        m_testFunctionalityDelta = testFuncDelta.str();
    }
    else
    {
        m_testFunctionalityDelta.clear();
    }
    
    if(m_system != "main")
    {
        m_unitTestSource.clear();
        
        //Debugging unit test, lets get the source
        std::string unitTestFile = m_workingDirectory + "/main.cpp";
        m_unitTestSource += "\n```cpp\n";
        m_unitTestSource += printLineNumbers(getFileContent(unitTestFile), 0);
        m_unitTestSource += "\n```\n\n";
        
        if(test.hasRegexChecks())
        {
            CCodeNode* testedNode = project->getNodeByName(m_system);
            std::string regexContract = utility::conversions::to_utf8string(testedNode->m_unitTest.regex_contract.to_json().serialize());
            m_unitTestSource += "Some of the commands in the test use regex patterns to FULLY match the stdout. ";
            m_unitTestSource += "Here is the regex contract with tested examples\n\n";
            m_unitTestSource += "\n```json\n";
            m_unitTestSource += regexContract;
            m_unitTestSource += "\n```\n";
            m_unitTestSource += "Sometimes the regex patterns from the contract contract could be different from the patterns from the test. ";
            m_unitTestSource += "If so, use the patterns from the contract as they have been tested to fully match the provided examples in the contract\n\n";
        }
    }
    else
    {
        m_unitTestSource.clear();
    }
    
    std::stringstream commands;
    commands << "TEST DESCRIPTION" << std::endl << std::endl << test.description << std::endl << std::endl;
    
    //TODO: Do I need to report the expected result for pretest?
    if(!test.pretest.commands.empty())
    {
        commands << "Pre test commands:" << std::endl;
        for(auto c : test.pretest.commands)
        {
            commands << *c << std::endl << std::endl;
        }
    }

    std::string projDir = project->getProjDir();
    //std::string execPath = projDir + "/build/" + getPlatform() + "_test/main";
    
    std::string testRawCmd = test.test.command;
    std::string testCmd = testRawCmd;
    std::string testResultStr;
    bool testDebug = false;
    bool testResult = false;
    std::string stdoutRegex;
    parsePrefixFlags(testRawCmd, testDebug, testResult, testResultStr, stdoutRegex, testCmd);
    
    std::string commandLine = testCmd;
    
    commands << "Test command:" << std::endl;
    commands << commandLine << std::endl;
    
    if(testResult && !testResultStr.empty()) {
        commands << "Expected result: " << testResultStr << std::endl;
    }
    
    if(testResult && !stdoutRegex.empty()) {
        commands << "stdout expected to fully match regex pattern: " << stdoutRegex << std::endl;
    }
    
    commands << std::endl;
    
    if(!test.posttest.commands.empty())
    {
        commands << "Post test commands:" << std::endl;
        for(auto c : test.posttest.commands)
        {
            std::string rawCmd = *c;
            std::string cmd = rawCmd;
            std::string expectedResult;
            bool debug = false;
            bool finalResult = false;
            stdoutRegex.clear();
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmd);
            
            commands << cmd << std::endl;
            if(finalResult && !expectedResult.empty()) {
                commands << "Expected result: " << expectedResult << std::endl;
            }
            
            if(finalResult && !stdoutRegex.empty()) {
                commands << "stdout expected to fully match regex pattern: " << stdoutRegex << std::endl;
            }
            
            commands << std::endl;
        }
    }
    
    std::string application = project->getProjectName();
    
    if(m_system == "main")
    {
        commands << "Note the '" << application << "' executable is 'main' in the 'Test command'" << std::endl;
        commands << "We are going to debug the '" << application << "' until it successfully pass the test!" << std::endl;
    }
    else
    {
        commands << "Note currently we are debugging unit test for system function '" << m_system << "'" << std::endl;
        commands << "Here is the unit test source" << std::endl << std::endl;
        commands << m_unitTestSource;
    }
    
    commands << "Reward-hacking is stricly prohibited! ";
    commands << "Ensure functionality is implemente according to the project description. ";
    commands << "The application will be tested with private tests to identify reward-hacking practices." << std::endl << std::endl;
    
    return commands.str();
}

void Debugger::checkTestStepInput(std::ostream& log,
                        CCodeProject* project,
                        const std::vector<std::shared_ptr<std::string>>& input_files,
                        const std::vector<std::shared_ptr<std::string>>& output_files,
                        const std::string& stepName,
                                  bool deleteOutput)
{
    std::string missingInputFiles;
    for(auto file : input_files)
    {
        if(!boost_fs::exists(m_workingDirectory + "/" + *file))
        {
            if(!missingInputFiles.empty()) {
                missingInputFiles += ", ";
            }
            
            missingInputFiles += *file;
        }
    }
    if(!missingInputFiles.empty())
    {
        log << "Missing input files required for " << stepName << " step (should be provided as test inputs or produced by previous steps):" << std::endl;
        log << missingInputFiles << std::endl << std::endl;
    }
    
    //Delete all files that this step has to generate
    if(deleteOutput)
    {
        for(auto file : output_files)
        {
            std::string filePath = m_workingDirectory + "/" + *file;
            if(boost_fs::exists(filePath))
            {
                boost_fs::remove(filePath);
            }
        }
    }
}

void Debugger::checkTestStepInput(std::ostream& log, CCodeProject* project, const TestStep& step, const std::string& stepName, bool deleteOutput)
{
    checkTestStepInput(log, project, step.input_files, step.output_files, stepName, deleteOutput);
}

void Debugger::checkTestStepOutput(std::ostream& log,
                         CCodeProject* project,
                         const std::vector<std::shared_ptr<std::string>>& output_files,
                         const std::string& stepName)
{
    std::string missingOutputFiles;
    for(auto file : output_files)
    {
        if(!boost_fs::exists(m_workingDirectory + "/" + *file))
        {
            if(!missingOutputFiles.empty()) {
                missingOutputFiles += ", ";
            }
            
            missingOutputFiles += *file;
        }
    }
    if(!missingOutputFiles.empty())
    {
        log << "Missing files that should have been produced by the " << stepName << " step: " << std::endl;
        log << missingOutputFiles << std::endl << std::endl;
    }
}

void Debugger::checkTestStepOutput(std::ostream& log, CCodeProject* project, const TestStep& step, const std::string& stepName)
{
    checkTestStepOutput(log, project, step.output_files, stepName);
}

bool Debugger::executeTestStep(std::ostream& log, CCodeProject* project, const TestStep& step, const std::string& stepName, bool enforceResult0)
{
    bool stepResults = true;
    if(!step.commands.empty())
    {
        checkTestStepInput(log, project, step, stepName, true);
        
        log << stepName << " commands:" << std::endl;
        uint32_t i=0;
        for(auto c : step.commands)
        {
            //[[debug,result]]
            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *c;
            std::string cmd = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmd);
            if(enforceResult0)
            {
                finalResult = true;
                expectedResult = "0";
            }
            
            std::string instrumentedCmd;
            if(debug)
            {
#if defined(__APPLE__)
                //We are going to debug this executable with lldb (/lldb_cmds.txt),
                //we need to codesign with entitlement:
                //codesign -s - --entitlements ./debug.entitlements ./feature_test
                std::string entitlementPath = Client::getInstance().getEnvironmentDir();
                entitlementPath += "/debug.entitlements";
                std::string execPath = m_workingDirectory + "/" + extractExecutablePath(cmd);
                std::string codesignCmd = "codesign -s - --entitlements " + entitlementPath + " " + execPath;
                hen::exec(codesignCmd, m_workingDirectory, "Codesign", true);
#endif
                
                instrumentedCmd += "lldb --batch";
                std::string lldbCommandsFile = Client::getInstance().getEnvironmentDir() + "/Debugger/Scripts/lldb_cmds.txt";
                std::string lldbCommandsStr = getFileContent(lldbCommandsFile);
                std::string lldbCommandsScript = buildPrompt(lldbCommandsStr, {{"timeout", std::to_string(RUN_TEST_LLDB_TIMEOUT)}});
                
                std::string lldb_commands = m_workingDirectory + "/lldb_cmds.txt";
                saveToFile(lldbCommandsScript, lldb_commands);
                
                instrumentedCmd += " -s '" + lldb_commands + "'";
                instrumentedCmd += " -- ";
                instrumentedCmd += cmd;
            }
            else if(finalResult)
            {
                instrumentedCmd = cmd;
                instrumentedCmd += R"( >/dev/null 2>&1; printf '_DEBUG_COMMAND_RESULT=%d\n' $?)";
            }
            else
            {
                instrumentedCmd = cmd;
            }
        
            
            log << "command " << ++i << ": ";
            log << cmd << std::endl << std::endl;
            
            std::string commandName = stepName + "_cmd" + std::to_string(i);
            
            
            std::string result;
            if(debug)
            {
                result = exec(instrumentedCmd, m_workingDirectory, commandName, false);
            }
            else
            {
                //When debugging we set timeout in the lldb scrip, but here we, without debugging we need to execute with timeout
                auto timeoutMillisecs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::duration<double>(RUN_TEST_LLDB_TIMEOUT));
                
                ExecResult execResult = exec_with_timeout(instrumentedCmd,
                                             m_workingDirectory,
                                             commandName,
                                             false,timeoutMillisecs);
                
                result = execResult.output;
            }
            
            std::string resultStr = getTestResult(result);
            
            log << "stdout " << i << ":" << std::endl;
            if(!result.empty())
            {
                std::string resultLimited = result.length() > 2048 ? result.substr(0, 2048) + "...[[truncated]]" : result;
                
                log << "```stdout\n";
                log << resultLimited << std::endl << "```" << std::endl;
            }
            else
            {
                log << "Empty stdout string" << std::endl << std::endl;
            }
            
            if(finalResult && !expectedResult.empty() && resultStr != expectedResult)
            {
                stepResults = false;
                log << "Returned result '" << resultStr << "' is not expected! Expected result is: '" << expectedResult << "'" << std::endl << std::endl;
            }
            
            if(finalResult && !stdoutRegex.empty())
            {
                std::string consoleLog = result;//getFileContent(m_workingDirectory + "/console.log");
                
                std::string regexErr;
                if (!fullRegexMatch(consoleLog, stdoutRegex, regexErr)) {
                   
                    if (!regexErr.empty()) {
                        //We must not be here
                        std::cout << "ERROR: invalid stdout regex: " << regexErr << "\n";
                    } else {
                        log << "stdout doesn't fully match the expected regex pattern: " << stdoutRegex << "\n";
                        log << "Note that PRINT_TEST doesn't print to the stdout, only std::cout does.\n";
                        stepResults = false;
                    }
                }
            }
        }

        checkTestStepOutput(log, project, step, stepName);
    }
    
    return stepResults;
}

// --------------------------------------------------------------------------
bool Debugger::execTestScript(CCodeProject* project,
                              const TestDef& test,
                              RunAnalysis& analysis,
                              std::string& traceOnlyLog,
                              std::string& debugTestLog,
                              std::string& debugAppLog,
                              bool instrument)
{
    std::stringstream debugLogTest;
    if(test.test.command.empty())
    {
        std::cout << "ERROR: Empty test command!" << std::endl;
        debugLogTest << "ERROR: Empty test command!" << std::endl;
        return false;
    }
    
    executeTestStep(debugLogTest, project, test.pretest, "pretest", false);

    std::string projDir = project->getProjDir();
    
    std::string execPath = projDir + "/build/" + getPlatform() + "_test/main";
    if(m_system != "main")
    {
        //We are debugging unit test
        execPath = projDir + "/build/source/" + m_system + "/test/main";
    }
    
    bool testDebug = false;
    bool testResult = false;
    
    std::string rawCmd = test.test.command;
    std::string cmd = rawCmd;
    std::string expectedResult;
    std::string stdoutRegex;
    parsePrefixFlags(rawCmd, testDebug, testResult, expectedResult, stdoutRegex, cmd);
    
    debugLogTest << "Test command:" << std::endl << std::endl;
    debugLogTest << cmd << std::endl << std::endl;
    
    checkTestStepInput(debugLogTest, project, test.test.input_files, test.test.output_files, "test", true);
    
    int returnCode;
    
    std::string cmdArgsOnly = removeFirstWord(cmd, "main");
    
    auto logs = runTest(traceOnlyLog, project, execPath, cmdArgsOnly, m_workingDirectory, 10, instrument, returnCode);
    debugAppLog = logs.first;
    
    std::string consoleLog = logs.second;//getFileContent(m_workingDirectory + "/console.log");
    
    debugLogTest << "Test command stdout:\n";
    if(!consoleLog.empty())
    {
        std::string consoleLogLimited = consoleLog.length() > 2048 ? consoleLog.substr(0, 2048) + "...[[truncated]]" : consoleLog;
        debugLogTest << "```stdout\n";
        debugLogTest << consoleLogLimited + "\n```\n";
    }
    else
    {
        debugLogTest << "Empty stdout string\n\n";
    }
    
    auto line = getFirstLine(m_workingDirectory + "/memo.txt");
    if(line && line->length() > 0)
    {
        if(!m_lldbLog.empty()) {
            debugLogTest << "lldb log from the test command:" << std::endl << std::endl;
            debugLogTest << m_lldbLog << std::endl << std::endl;
        }
    }
    
    std::string returnCodeStr = std::to_string(returnCode);
    bool mainTestPass = true;
    if(testResult && !expectedResult.empty() && returnCodeStr != expectedResult)
    {
        debugLogTest << "Returned result '" << returnCodeStr << "' is not expected! Expected result is: '" << expectedResult << "'" << std::endl << std::endl;
        mainTestPass = false;
    }
    
    if(testResult && !stdoutRegex.empty())
    {
        std::string regexErr;
        if (!fullRegexMatch(consoleLog, stdoutRegex, regexErr)) {
           
            if (!regexErr.empty()) {
                //We must not be here
                std::cout << "ERROR: invalid stdout regex: " << regexErr << "\n";
            } else {
                debugLogTest << "stdout doesn't match the expected regex pattern: " << stdoutRegex << "\n";
                debugLogTest << "Note that PRINT_TEST doesn't print to the stdout, only std::cout does.\n";
                mainTestPass = false;
            }
        }
    }
    
    checkTestStepOutput(debugLogTest, project, test.test.output_files, "test");
    
    if(!debugAppLog.empty())
    {
        debugLogTest << "Application log captured for analysis." << std::endl;
    }
    else
    {
        debugLogTest << "Application log is empty." << std::endl;
    }

    bool finalResult = executeTestStep(debugLogTest, project,
                                             test.posttest, "posttest", false);
    
    std::string debugLogTestStr = "\n\n*************** TEST SCRIPT EXECUTION LOG START ***************\n\n";
    debugLogTestStr += debugLogTest.str();
    debugLogTestStr += "\n\n*************** TEST SCRIPT EXECUTION LOG END ***************\n\n";
    
    debugTestLog = debugLogTestStr;

    analysis.m_testResult = mainTestPass && finalResult;

    return mainTestPass && finalResult;
}

void Debugger::logAnalysis(CCodeProject* project,
                           const std::string& debugLogTestStr,
                           RunAnalysis& analysis)
{
    bool dgbTestLogShowOnStart = false;
    std::string application = project->getProjectName();
    
    const uint32_t logSectionSize = LOG_SECTION_SIZE;
    const uint32_t maxSections = 3*MAX_LOG_SECTIONS_PER_LOCATION;
    
    std::vector<std::pair<uint32_t, uint32_t>> intervalsForAnalysis;
    
    const uint32_t L = MAX_LOG_SECTIONS_PER_LOCATION * logSectionSize; // 2*logSectionSize
    const uint32_t N = (uint32_t)m_logger.size();
    const uint32_t mid = N / 2;
    const uint32_t mid_lo = (mid > L/2 ? mid - L/2 : 0);
    const uint32_t mid_hi = std::min(N, mid_lo + L);

    intervalsForAnalysis = {
        {0, L},
        {mid_lo, mid_hi},
        {N > L ? N - L : 0, N}
    };
    
    uint32_t parsedSize = 0;
    
    auto formatLogSection = [](uint32_t startLine,
                               uint32_t endLine,
                               size_t totalLines,
                               const std::string& section) -> std::string
    {
        std::ostringstream oss;
        oss << "Log section start line: " << (startLine)
            << " end line: "         << (endLine)
            << " total log lines count: "<< totalLines
            << "\n\n"
            << printLineNumbers(section, startLine-1); //The second arg of printLineNumbers is offset. We need to subtract 1 from the 1-based startLine
        return oss.str();
    };
    
    uint32_t logSectionLineStart = 1;
    uint32_t logSectionLineEnd = 1;
    bool skipLines = false;
    
    // Ported usage:
    m_logger.forEachSectionByByteIntervals(
        intervalsForAnalysis,
        logSectionSize,
        [&](const LogAnalyzer::SectionInfo& info, const std::string& payload) {
            // Format like before
            std::string msg = formatLogSection(
                info.line_start, info.line_end, m_logger.linesCount(),
                info.skipped ? std::string() : payload
            );
            if (info.skipped) {
                msg += "[[skip log lines]]";
            }
            if (!dgbTestLogShowOnStart) {
                msg = debugLogTestStr + msg;
                dgbTestLogShowOnStart = true;
            }
            analysisLogSection(project, msg, analysis);
        }, false
    );
}

void Debugger::runAnalysis(CCodeProject* project, const TestDef& test, RunAnalysis& analysis, bool analyzeLog)
{
    std::string lldbOnlyLog, debugLogTestStr, debugLog;
    bool testPasses = execTestScript(project, test, analysis, lldbOnlyLog, debugLogTestStr, debugLog, true);
    debugLogTestStr = replaceAll(debugLogTestStr,project->getProjDir(),".");
    m_lastRunTestLog = debugLogTestStr;
    analysis.m_testLog = debugLogTestStr;
    
    m_logger.parse(debugLog);
    
    if(!lldbOnlyLog.empty())
    {
        m_tracer.loadFromString(lldbOnlyLog);
    }
    
    //We have to set this as early as possible since getTrajectory prints information for the last run
    m_lastRunStep = m_previousSteps + uint32_t( m_trajectory.size() ) + 1;
    
    //Do reward hacking test only when we run the test (in other word when this function will analyze logs)
    if(analyzeLog && testPasses)
    {
        m_rewardHackingReview.clear();
        
        std::string rewardHackingReview, privateTestHint;
        bool rewardHackingPass = rewardHackingAnalysis(project, test, rewardHackingReview, privateTestHint);
        if(!rewardHackingPass &&  rewardHackingReview.length() > 0)
        {
            if(!privateTestHint.empty())
            {
                rewardHackingReview += "\n\nHere is a direct hint from the failed private test:\n";
                rewardHackingReview += privateTestHint;
                rewardHackingReview += "\n\nFocus on the issues mentoined in the direct hint. ";
                rewardHackingReview += "They must be fixed in order to pass the reward-hacking evaluation for this test!\n\n";
            }
            
            rewardHackingReview += "\n\nReview and the fix the issues highlighted in the review!\n\n";
            
            analysis.debug_notes = rewardHackingReview;
            m_rewardHackingReview = rewardHackingReview;
            
            //TODO: this EXACT string is in use to tag the step in the trajectory for reward-hacking review
            //TODO: It is crucial to match the changes on this text with the check in loadTrajectory()
            analysis.log_summary =  "Reward-hacking prcatices have been identified.\n";
            
            return;
        }
        else
        {
            analysis.debug_notes = "PASS";
            analysis.log_summary =  "Results for all commands match the expected outcomes.\n";
            
            return;
        }
    }
    else if(analyzeLog)
    {
        m_rewardHackingReview.clear();
    }
    
    if(startsWithIgnoreCase(analysis.debug_notes, "PASS"))
    {
        analysis.debug_notes += "\n\n(Feedback from the agent's algorithm: You suggested that all tests pass, but unfortunately ";
        analysis.debug_notes += "outcomes from the test commands in the last run do not support this. ";
        analysis.debug_notes += "For more info, have a look at the section 'INFORMATION FROM THE LAST RUN STEP')\n";
    }
    
    m_runAnalysisSteps.clear();
    m_runAnalysisStep = 0;
    
    if(!analyzeLog)
    {
        return;
    }
    
    if(!lldbOnlyLog.empty())
    {
        analysisTrace(project, debugLogTestStr, lldbOnlyLog, analysis, test);
    }
    
    if(!m_runAnalysisSteps.empty())
    {
        analysis.debug_notes = m_runAnalysisSteps[0].m_debugNotes;
        analysis.log_summary = m_runAnalysisSteps[0].m_logSummary;
    }
    
    analysis.m_testLog = debugLogTestStr;
}

std::string Debugger::getSubSystemsData(CCodeProject* project, std::set<std::string>& subSystems)
{
    std::vector<std::string> topFunctions = project->listAllFunctions(m_system, PRINT_MAX_FUNCTIONS_DEPTH-1, {});
    subSystems.clear();
 
    std::stringstream ss;
    for(auto func : topFunctions)
    {
        auto firstInvocation = m_tracer.getFrame(func, 1);
        auto nextInvocation = m_tracer.getFrame(func, 2);
        
        if(firstInvocation && !nextInvocation)
        {
            subSystems.insert(func);
         
            const CCodeNode* ccNode = project->getNodeByName(func);
            
            ss << func << ": " << ccNode->m_brief.brief << std::endl << std::endl;
            ss << printFunctionSource(project, func, ccNode->m_implementation.m_source) + "\n\n";
            
            visibleTraceAndLog(ss, firstInvocation->m_invocation);
        }
    }
    
    if(m_system != "main")
    {
        auto logSection = m_logger.logMessagesForUntrackedFunctions(0, LOG_TRACE_SIZE);

        if(logSection.second > 0)
        {
            ss << std::endl << "Logged messages from the unit test source (debug log, not stdout):\n\n";
            ss << logSection.first << std::endl << std::endl;
        }
    }
    
    return ss.str();
}

void Debugger::systemAnalysis(CCodeProject* project, const std::string& hint, RunAnalysis& analysis)
{
    std::set<std::string> subSystems;
    std::string systemData = getSubSystemsData(project, subSystems);
    std::string subSystemsStr = getAsCsv(subSystems);
    if(subSystemsStr.empty())
    {
        subSystemsStr = "Unable to identify functions that appear to be sub-systems";
    }
    
    std::string callTree = project->printGraph(m_system, PRINT_MAX_FUNCTIONS_DEPTH, true);
    std::string application = project->getProjectName();
    
    std::string traceDescStr = "\n\nFormats of the trace and log are specified in 'TRACE DESCRIPTION' and 'LOG DESCRIPTION' sections\n\n";
    
    std::string trajectory = getTrajectory(0, -1, true, true, true);
    
    std::string fixedFunctionInfo;
    std::string debugNote;
    
    std::string fixedFunction = m_nextStep.action_subject;
    
    //Get information for the recently fixed function
    if(!fixedFunction.empty() && checkFunctionExists(project, fixedFunction, debugNote))
    {
        fixedFunctionInfo += "Here is information for the recently fixed function.\n\n";
        
        fixedFunctionInfo += getRequestedInfo(project, 0, 0, {},
                                          {std::make_shared<std::string>(fixedFunction)},
                                          {},{});
        
        fixedFunctionInfo += "Trace and log messages for the last recorded invocations of '" + fixedFunction + "'\n\n";
        
        //Provide the source of the fixed function
        bool isSubSystem = false;
        if(subSystems.find(fixedFunction) == subSystems.end())
        {
            //fixedFunctionInfo += getRequestedInfo(project, 0, 0, fixedFunction, {}, {}, {});
        }
        else
        {
            isSubSystem = true;
        }
        
        auto lastFrame = m_tracer.getLastInvocation(fixedFunction);
        if(lastFrame)
        {
            if(!isSubSystem || lastFrame->m_invocation.second != 1)
            {
                fixedFunctionInfo += "Trace for the last traced invocation of function '";
                fixedFunctionInfo += fixedFunction + "' invocation: " + std::to_string(lastFrame->m_invocation.second) + "\n";
                
                if(m_contextVisibility.visibleTraceFrame(fixedFunction, lastFrame->m_invocation.second))
                {
                    std::stringstream ssLastFrame;
                    m_tracer.printFrame(ssLastFrame, lastFrame);
                    
                    fixedFunctionInfo += ssLastFrame.str() + "\n\n";
                }
                else
                {
                    fixedFunctionInfo += "[[provided in the context]]\n\n";
                }
            }
            else
            {
                fixedFunctionInfo += "Trace for the last traced invocation " + std::to_string(lastFrame->m_invocation.second)  + " of function '";
                fixedFunctionInfo += fixedFunction + "' is already available\n\n";
            }
        }
        else
        {
            fixedFunctionInfo += "Couldn't find recorded trace for function '" + fixedFunction + "'\n\n";
        }
        
        auto lastLog = m_logger.logGetLastInvocation(fixedFunction);
        if(lastLog.first == fixedFunction)
        {
            if(!isSubSystem || lastLog.second != 1)
            {
                fixedFunctionInfo += "Logged events for the last logged invocation of function '";
                fixedFunctionInfo += fixedFunction + "' invocation: " + std::to_string(lastLog.second) + "\n\n";
                
                if(m_contextVisibility.visibleFunctionLog(fixedFunction, lastLog.second))
                {
                    auto logEntries = m_logger.logMessagesForFunction(fixedFunction, 0, lastLog.second, LOG_TRACE_SIZE);
                    fixedFunctionInfo += logEntries.first + "\n\n";
                }
                else
                {
                    fixedFunctionInfo += "[[provided in the context]]\n\n";
                }
            }
            else
            {
                fixedFunctionInfo += "Logged events for the last logged invocation " + std::to_string(lastLog.second) + " of function '";
                fixedFunctionInfo += fixedFunction + "' are already available\n\n";
            }
        }
        else
        {
            fixedFunctionInfo += "Couldn't find logged messages for function '" + fixedFunction + "'\n\n";
        }
    }
    
    std::string maxDepth = std::to_string(PRINT_MAX_FUNCTIONS_DEPTH-1);
    Prompt promptAnalysis("SystemAnalysis.txt",{
                        {"trace_desc", traceDescStr},
                        {"trajectory", trajectory},
                        {"call_graph", callTree},
                        {"application", application},
                        {"max_depth", maxDepth},
                        {"sub_systems", subSystemsStr},
                        {"system_data", systemData},
                        {"fixed_function", fixedFunctionInfo},
                        {"analysis_hint", hint},
                        {"test_delta", m_testFunctionalityDelta}
    });
    
    std::string message = promptAnalysis.str();
    inferenceRunAnalysis(project, message, analysis, "SYSTEM ANALYSIS:\n");
}

bool Debugger::isOnTrack(const std::string& queryTrajectory)
{
    std::string trajectory;
    for(auto step : m_trajectory) {
        if(!trajectory.empty()) {
            trajectory += ">";
        }
        trajectory += step.m_action;
    }
    
    return queryTrajectory.find(trajectory) != std::string::npos;
}

static int g_bpId = 1;

int Debugger::debugScope(const SourceScope& scope, bool info, bool extendedFrame, bool printInOut, bool debugEnd, std::vector<std::string>& lldbArgs) const
{
    std::string bpBeginId = "$bp_" + std::to_string(g_bpId++);
    
    lldbArgs.push_back("-o");
    lldbArgs.push_back("expression int " + bpBeginId + " = 0");

    boost_fs::path startPath = scope.m_start.m_filePath;
    std::string functionName = startPath.stem().string();
    std::string beginCommands;
    
    if(info)
    {
        beginCommands += " --command \"process status\"";
        beginCommands += " --command \"thread info\"";
    }
    
    if(extendedFrame)
    {
        beginCommands += " --command \"thread backtrace\"";
        beginCommands += " --command \"frame select\"";
        beginCommands += " --command \"frame variable\"";
    }
    else if(printInOut)
    {
        beginCommands += " --command \"frame info\"";
    }

    std::string startBpName = startPath.filename().string() + ":" + std::to_string(scope.m_start.m_lineNumber);
#ifdef LLDB_PRINT_BREAKPOINT_HITS
    beginCommands += " --command \"print \\\"Hit breakpoint: " + startBpName + "\\\"\"";
#endif
    
    // 1) Breakpoint at scope begin
    std::string bpBegin =
        "breakpoint set --file " + scope.m_start.m_filePath +
        " --line " + std::to_string(scope.m_start.m_lineNumber)
        + " --column " + std::to_string(scope.m_start.m_column)
        + " --auto-continue true"
        + beginCommands
        //Limit the breakpoints hit cout to 3 for now
        + " --condition \"" + bpBeginId + " < 3 && ++" + bpBeginId + "\"";
    
    lldbArgs.push_back("-o");
    lldbArgs.push_back(bpBegin);
    
    //if(debugEnd)
    {
        boost_fs::path endPath = scope.m_end.m_filePath;
        std::string endBpName = endPath.filename().string() + ":" + std::to_string(scope.m_end.m_lineNumber);
        
        std::string endCommands;
        
        if(debugEnd)
        {
            endCommands += " --command \"thread backtrace\"";
            endCommands += " --command \"frame select\"";
            endCommands += " --command \"frame variable\"";
#ifdef LLDB_PRINT_BREAKPOINT_HITS
            endCommands += " --command \"print \\\"Hit breakpoint: " + endBpName + "\\\"\"";
#endif
        }
        
        if(printInOut) {
            endCommands += " --command \"frame info\"";
        }
        
        if(!endCommands.empty())
        {
            std::string bpEndId = "$bp_" + std::to_string(g_bpId++);
            lldbArgs.push_back("-o");
            lldbArgs.push_back("expression int " + bpEndId + " = 0");
            
            // 2) Breakpoint at scope end
            std::string bpEnd =
            "breakpoint set --file " + scope.m_end.m_filePath +
            " --line " + std::to_string(scope.m_end.m_lineNumber)
            + " --column " + std::to_string(scope.m_end.m_column)
            + " --auto-continue true"
            + endCommands
            //Limit the breakpoints hit cout to 3 for now
            + " --condition \"" + bpEndId + " < 3 && ++" + bpEndId + "\"";
            
            lldbArgs.push_back("-o");
            lldbArgs.push_back(bpEnd);
            
            return g_bpId;
        }
    }
    
    return -1;
}

bool Debugger::debugLocation(const SourceLocation& location, std::vector<std::string>& lldbArgs, bool stepIn, int stepsCount) const
{
    for(int i=0; i<stepsCount; ++i) {
        
        std::string bpId = "$bp_" + std::to_string(g_bpId++);
        
        lldbArgs.push_back("-o");
        lldbArgs.push_back("expression int " + bpId + " = 0");
        
        boost_fs::path path = location.m_filePath;
        std::string bpName = path.filename().string() + ":Step: " + std::to_string(i);
        
        std::string commands;
        commands += " --command \"frame select\"";
        commands += " --command \"frame variable\"";
#ifdef LLDB_PRINT_BREAKPOINT_HITS
        commands += " --command \"print \\\"Hit breakpoint: " + bpName + "\\\"\"";
#endif
        
        // 1) Breakpoint at scope begin
        std::string bp =
            "breakpoint set --file " + location.m_filePath +
            " --line " + std::to_string(location.m_lineNumber + i)
            //+ " --column " + std::to_string(location.m_column)
            + " --auto-continue true"
            + commands
            //Limit the breakpoints hit cout to 1 for now
            + " --condition \"" + bpId + " < 1 && ++" + bpId + "\"";

        lldbArgs.push_back("-o");
        lldbArgs.push_back(bp);
    }
    
    return true;
}

bool Debugger::debugFunctionScopes(const std::shared_ptr<FunctionDebugInfo>& debugInfo, std::vector<std::string>& lldbArgs, bool infoForRoot) const
{
    size_t numScopes = debugInfo->m_scopes.size();
    const SourceScope& rootScope = debugInfo->getRootScope();
    for (size_t i = 0; i < numScopes; ++i)
    {
        const SourceScope& scope = debugInfo->m_scopes[i];
        
        bool isRoot =
        rootScope.m_start.m_lineNumber == scope.m_start.m_lineNumber &&
        rootScope.m_start.m_column     == scope.m_start.m_column &&
        rootScope.m_end.m_lineNumber   == scope.m_end.m_lineNumber &&
        rootScope.m_end.m_column       == scope.m_end.m_column;
        
        //Only for the function body scope if requested
        bool info = infoForRoot && isRoot;
        bool printInOut = isRoot;
        bool extendedFrame = isRoot;
        
        //bool info, bool extendedFrame, bool printInOut, bool debugEnd
        debugScope(scope, info, extendedFrame, printInOut, true, lldbArgs);
    }
    
    return true;
}

bool Debugger::debugFunctionReturns(const std::shared_ptr<FunctionDebugInfo>& debugInfo, bool extended, std::vector<std::string>& lldbArgs, int assignedBpId) const
{
    for(auto ret : debugInfo->m_returns)
    {
        std::string bpId = "$bp_" + (assignedBpId > 0 ? std::to_string(assignedBpId) : std::to_string(g_bpId++));
        
        if(assignedBpId < 0)
        {
            lldbArgs.push_back("-o");
            lldbArgs.push_back("expression int " + bpId + " = 0");
        }
        
        boost_fs::path path = ret.m_start.m_filePath;
        std::string bpName = path.filename().string() + ":" + std::to_string(ret.m_start.m_lineNumber);
        
        std::string commands;
        if(extended)
        {
            commands += " --command \"thread backtrace\"";
            commands += " --command \"frame select\"";
            commands += " --command \"frame variable\"";
#ifdef LLDB_PRINT_BREAKPOINT_HITS
            commands += " --command \"print \\\"Hit breakpoint: " + bpName + "\\\"\"";
#endif
        }
        //if(extended)
        else
        {
            commands += " --command \"frame info\"";
            //commands += " --command \"print \\\"[[lldb exits: " + debugInfo->m_name + "]]\\\"\"";
        }
        
        // 1) Breakpoint at scope begin
        std::string bp =
            "breakpoint set --file " + ret.m_start.m_filePath
            + " --line " + std::to_string(ret.m_start.m_lineNumber)
            + " --column " + std::to_string(ret.m_start.m_column)
            + " --auto-continue true"
            + commands
            //Limit the breakpoints hit cout to 1 for now
            + " --condition \"" + bpId + " < 1 && ++" + bpId + "\"";

        lldbArgs.push_back("-o");
        lldbArgs.push_back(bp);
    }
    
    return true;
}

int Debugger::mapLine(CCodeProject* project, const std::string& filePath, const std::string& functionName, const std::string& functionSource, int lineNumber) const
{
    if(!boost_fs::exists(filePath))
    {
        std::cout << "Couldn't map line: " << lineNumber << ". File doesn't exists: " << filePath << std::endl;
        return -1;
    }
    
    std::ifstream file(filePath);
    std::string sourceFile((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    //TODO: This could be optimized with mapLine to avoid normalization for each breakpoint
    return normalizeAndMapLine(functionSource, sourceFile, lineNumber);
}

int Debugger::mapLine(CCodeProject* project, const std::string& filePath, const std::string& functionName, int lineNumber) const
{
    std::string implementation = project->getFunctionImplementation(functionName);
    return mapLine(project, filePath, functionName, implementation, lineNumber);
}

std::string Debugger::printFunctionSource(CCodeProject* project, const std::string& functionName, const std::string source) const
{
    std::string platform = getPlatform() + "_test";
    uint32_t options = CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG;
    auto info = project->getCompilationInfo(functionName, platform, options);
    
    int lineOffset = mapLine(project, info->m_sourceFilePath, functionName, source, 1);
    return printLineNumbers(source, lineOffset);
}

std::string Debugger::getFunctionDetailedInfo(CCodeProject* project, const std::string& functionName) const
{
    std::string info;
    
    auto it = project->nodeMap().find(functionName);
    if(it != project->nodeMap().end())
    {
        auto ccNode = (const CCodeNode*)it->second;
        if(!ccNode) return info;
        
        if(!ccNode->m_prototype.brief.empty())
        {
            std::string briefStr = ccNode->m_prototype.brief;
            if(briefStr.length() > BRIEF_MAX_CHARACTERS)
            {
                briefStr = truncateWithNoteUtf8(briefStr, BRIEF_MAX_CHARACTERS, " [[...truncated]]");
            }
            
            info += ccNode->m_brief.func_name + ": " + briefStr + "\n\n";
        }
        
        if(!ccNode->m_implementation.m_source.empty())
        {
            info += printFunctionSource(project, ccNode->m_brief.func_name, ccNode->m_implementation.m_source) + "\n\n";
        }
    }
    
    return info;
}

std::string Debugger::getVisibleDataTypes(CCodeProject* project, CCodeNode* ccNode, bool checkContext, std::set<std::string>& referencedNodes)
{
    std::string info;
    
    //Check and add data types visible by the function
    std::set<std::string> owners;
    std::set<std::string> structs;
    std::set<std::string> enums;
    ccNode->getFullVisibility(false, owners, structs, enums);
    
    std::string dataTypeDefs;
    for(auto s : structs)
    {
        bool inContext = m_contextVisibility.isDataVisible(s);
        if(!checkContext || !inContext)
        {
            std::string owningPath;
            auto dataDef = project->findData(s, owningPath);
            if(!dataDef) continue;
            
            referencedNodes.insert(owningPath);
            info += dataDef->m_typeDef.m_definition + ";\n\n";
        }
    }
    
    for(auto e : enums)
    {
        bool inContext = m_contextVisibility.isDataVisible(e);
        if(!checkContext || !inContext)
        {
            std::string owningPath;
            auto dataDef = project->findData(e, owningPath);
            if(!dataDef) continue;
            
            referencedNodes.insert(owningPath);
            info += dataDef->m_typeDef.m_definition + ";\n\n";
        }
    }
    
    return info;
}

std::string Debugger::ensureFunctionIsVisible(CCodeProject* project, const std::string& functionName, bool checkContext)
{
    CCodeNode* ccNode = project->getNodeByName(functionName);
    if(!ccNode)
    {
        return std::string();
    }
    
    //Add data types visible by the function
    std::set<std::string> referencedNodes;
    std::string info = getVisibleDataTypes(project, ccNode, checkContext, referencedNodes);
    
    bool inContext = m_contextVisibility.isFunctionVisible(functionName);
    if(!checkContext || inContext)
    {
        info += getFunctionDetailedInfo(project, functionName);
    }
    
    return info;
}

std::string Debugger::breakpoint(std::vector<std::string>& lldbArgs, const std::string& filePath, int line, const std::string& condition, const std::string& expression) const
{
    std::string bpBeginId = "$bp_" + std::to_string(g_bpId++);
    
    lldbArgs.push_back("-o");
    lldbArgs.push_back("expression int " + bpBeginId + " = 0");
    
    std::string hitCondition = bpBeginId + " < 3 && ++" + bpBeginId;
    std::string fullCondition = "(" + hitCondition + ") && (" + condition + ")";
    
    std::string lldbBPCmdLineArgs = "breakpoint set --file " + filePath + " --line " + std::to_string(line) + " --auto-continue true" +
                                    " --condition '" + fullCondition + "'  --command 'expression " + expression + "'" +
                                    " --command \"thread backtrace\"";
    lldbBPCmdLineArgs += " --command \"frame select\" --command \"frame variable\"";
    lldbBPCmdLineArgs += " --command \"frame select 1\" --command \"frame variable\"";
    lldbBPCmdLineArgs += " --command \"frame select 2\" --command \"frame variable\"";
    
    std::cout << "Set manually configured breakpoint with the following command:" << std::endl;
    std::string lldbCmdLine = replaceAll(lldbBPCmdLineArgs, "$", "\\$");
    std::cout << lldbCmdLine << std::endl << std::endl;
    
    lldbArgs.push_back("-o");
    lldbArgs.push_back(lldbBPCmdLineArgs);
    
    return lldbBPCmdLineArgs;
}

void Debugger::setBreakpoints(std::vector<std::string>& lldbArgs, CCodeProject* project, const std::string& functionName,
                              const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints)
{
    //Let's start from 1
    g_bpId = 1;
    
    lldbArgs.push_back("-o");
    lldbArgs.push_back("settings set target.max-string-summary-length 64");
    lldbArgs.push_back("-o");
    lldbArgs.push_back("settings set target.max-children-count 3");
    
    std::set<std::string> directsInCallGraph;
    if(!functionName.empty())
    {
        directsInCallGraph = project->getDirectsInCallGraph(functionName);
    }
    
    for(auto func : project->nodeMap()) {
        
        //std::cout << "SET BREAKPOINTS FOR FUNCTION: " << func.first << std::endl;
        
        auto funcDebugInfo = getFunctionDebugInfo(project, func.first);
        
        if(functionName == func.first) {
            if(funcDebugInfo->m_scopes.size() > 0)
            {
                debugFunctionScopes(funcDebugInfo, lldbArgs, functionName == "main");
                debugFunctionReturns(funcDebugInfo, true, lldbArgs, -1);
            }
            
#if 1
            if(customBreakpoints.size() > 0 && funcDebugInfo->m_scopes.size() > 0)
            {
                const SourceScope& rootScope = funcDebugInfo->getRootScope();
                for(auto bp : customBreakpoints) {
                    
                    //int sourceLine = mapLine(project, rootScope.m_start.m_filePath, functionName, bp->source_line);
                    int sourceLine = bp->source_line;
                    breakpoint(lldbArgs, rootScope.m_start.m_filePath, sourceLine,
                               bp->getInstrumentedConditionCode(),
                               bp->getInstrumentedExpressionCode());
                }
            }
#endif
        }
        else if(func.first == "main")
        {
            if(funcDebugInfo->m_scopes.size() > 0)
            {
                debugFunctionScopes(funcDebugInfo, lldbArgs, true);
                
                debugFunctionReturns(funcDebugInfo, true, lldbArgs, -1);
            }
        }
        //If we have a direct of the debugged function
        if(!functionName.empty() &&
           directsInCallGraph.find(func.first) != directsInCallGraph.end())
        {
            if(funcDebugInfo->m_scopes.size() > 0)
            {
                debugFunctionScopes(funcDebugInfo, lldbArgs, false);
                debugFunctionReturns(funcDebugInfo, true, lldbArgs, -1);
            }
        }
        else {
            
            if(funcDebugInfo->m_scopes.size() > 0)
            {
                const SourceScope& rootScope = funcDebugInfo->getRootScope();
                //bool info, bool extendedFrame, bool printInOut, bool debugEnd
                debugScope(rootScope, false, false, true, false, lldbArgs);
                debugFunctionReturns(funcDebugInfo, false, lldbArgs, -1);
            }
        }
    }
}

std::string Debugger::getBreakpointsInfo(bool instrumented, bool command, const std::string& function,
                                         const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const
{
    std::string breakpointInfo;
    
    int bpId = 1;
    for(auto bp : m_nextStep.breakpoints) {
        
        breakpointInfo += "Breakpoint " + std::to_string(bpId) + ":\n";
        
        breakpointInfo += "Source line: " + std::to_string(bp->source_line) + "\n";
        breakpointInfo += "Condition: " + bp->getConditionCode() + "\n";
        breakpointInfo += "Expression: " + bp->getExpressionCode() + "\n";
        
        if(instrumented)
        {
            breakpointInfo += "Instrumented condition: " + bp->getInstrumentedConditionCode() + "\n";
            breakpointInfo += "Instrumented expression: " + bp->getInstrumentedExpressionCode() + "\n\n";
        }
        
        if(command)
        {
            std::vector<std::string> lldbArgs;
            std::string lldbCmdLn = breakpoint(lldbArgs, function + ".cpp", bp->source_line,
                                               bp->getInstrumentedConditionCode(),
                                               bp->getInstrumentedExpressionCode());
            
            breakpointInfo += "LLDB command for the breakpoint: " + lldbCmdLn + "\n\n";
        }
        bpId++;
    }
    
    return breakpointInfo;
}

// Input structure with just location information
struct BreakpointLocationInfo {
    unsigned int lineNumber = 0;
    unsigned int snippetOffset = 0;
    unsigned int snippetLength = 0;
    std::string snippetCode;  // Optional: include the actual code for reference
    CXSourceRange range;
    
    BreakpointLocationInfo(unsigned int line = 0, unsigned int offset = 0,
                          unsigned int length = 0, const std::string& code = "")
    : lineNumber(line), snippetOffset(offset), snippetLength(length), snippetCode(code) {
        memset(&range,0, sizeof(range));
    }
    
    std::pair<std::string, std::string> getCondAndExp() {
        // Define a regex pattern with two capturing groups:
        // Group 1: captures condition between "if(" and "){"
        // Group 2: captures expression between "){"
        // and ";}}" (excluding the semicolon)
        std::regex re(R"(\{if\((.*?)\)\{(.*?);\}\})");
        std::smatch match;
        
        if (std::regex_search(snippetCode, match, re)) {
            if (match.size() >= 3) {  // match[0] is the entire match; match[1] is the condition; match[2] is the expression
                return { match[1].str(), match[2].str() };
            }
        }
        
        // Return empty strings if the pattern does not match
        return {"", ""};
    }
    
    std::string getLocationInfo(unsigned int line, unsigned int column)
    {
        // {if(
        // ){
        // ;}}
        
        auto parts = getCondAndExp();
        uint32_t cndStart = snippetOffset + 4;
        uint32_t cndEnd = cndStart + (uint32_t)parts.first.length();
        uint32_t expStart = cndEnd + 2;
        uint32_t expEnd = expStart + (uint32_t)parts.second.length();
        
        if(column > cndStart && column < cndEnd)
            return "condition: " + parts.first;
        else if(column > expStart && column < expEnd)
            return "expression: " + parts.second;
        
        return std::string();
    }
};

// Output structure focused on analysis results
struct BreakpointAnalysis {
    
    CXSourceRange range;
    std::string snippet;
    
    //first: function name, second: return type
    std::map<std::string, std::string> stdCalls;
    
    // Source location reference
    unsigned int lineNumber = 0;
    
    // Analysis flags
    bool hasTemplateExpressions = false;
    bool hasLambdaFunctions = false;
    bool hasOperatorOverloads = false;
    bool hasExceptionHandling = false;
    bool hasComplexSTL = false;
    bool hasRTTI = false;
    bool hasCpp17Features = false;
    bool hasCpp20Features = false;
    bool hasStringLiteralComparison = false;
    
    // Detailed issues
    std::vector<std::string> issues;
    
    // Constructor to initialize with a line number
    BreakpointAnalysis(unsigned int line = 0) : lineNumber(line) {}
};

// Function to check if two ranges overlap
bool rangesOverlap(CXSourceRange range1, CXSourceRange range2) {
    if (clang_Range_isNull(range1) || clang_Range_isNull(range2)) {
        return false;
    }
    
    // Get start and end locations
    CXSourceLocation start1 = clang_getRangeStart(range1);
    CXSourceLocation end1 = clang_getRangeEnd(range1);
    CXSourceLocation start2 = clang_getRangeStart(range2);
    CXSourceLocation end2 = clang_getRangeEnd(range2);
    
    // Get file, line, column for each location
    CXFile file1, file2, file3, file4;
    unsigned line1, line2, line3, line4;
    unsigned column1, column2, column3, column4;
    unsigned offset1, offset2, offset3, offset4;
    
    clang_getSpellingLocation(start1, &file1, &line1, &column1, &offset1);
    clang_getSpellingLocation(end1, &file2, &line2, &column2, &offset2);
    clang_getSpellingLocation(start2, &file3, &line3, &column3, &offset3);
    clang_getSpellingLocation(end2, &file4, &line4, &column4, &offset4);
    
    // Compare files
    if (!clang_File_isEqual(file1, file3)) {
        return false;
    }
    
    // Check for overlap
    return (offset1 <= offset4 && offset2 >= offset3);
}

bool rangesMatch(CXSourceRange range1, CXSourceRange range2)
{
    if (clang_Range_isNull(range1) || clang_Range_isNull(range2)) {
        return false;
    }
    
    // Get start and end locations
    CXSourceLocation start1 = clang_getRangeStart(range1);
    CXSourceLocation end1 = clang_getRangeEnd(range1);
    CXSourceLocation start2 = clang_getRangeStart(range2);
    CXSourceLocation end2 = clang_getRangeEnd(range2);
    
    // Get file, line, column for each location
    CXFile file1, file2, file3, file4;
    unsigned line1, line2, line3, line4;
    unsigned column1, column2, column3, column4;
    unsigned offset1, offset2, offset3, offset4;
    
    clang_getSpellingLocation(start1, &file1, &line1, &column1, &offset1);
    clang_getSpellingLocation(end1, &file2, &line2, &column2, &offset2);
    clang_getSpellingLocation(start2, &file3, &line3, &column3, &offset3);
    clang_getSpellingLocation(end2, &file4, &line4, &column4, &offset4);
    
    // Compare files
    if (!clang_File_isEqual(file1, file3)) {
        return false;
    }
    
    // Check for overlap
    return (offset1 == offset3 && offset2 == offset4);
}

// Define a global structure to share data with the C-style visitor function
struct VisitorData {
    std::vector<BreakpointAnalysis>* results;
    std::vector<BreakpointLocationInfo>* bpLocationInfo;
    CXTranslationUnit unit;
    std::string file;
};

struct BreakpointData {
    CXTranslationUnit unit;
    BreakpointLocationInfo* bpInfo;
    BreakpointAnalysis* bpAnalysis;
};

enum CXChildVisitResult validateBreakpoint(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    
    auto* data = static_cast<BreakpointData*>(client_data);
    
    BreakpointLocationInfo& bpInfo = *data->bpInfo;
    BreakpointAnalysis& bpAnalysis = *data->bpAnalysis;
    
    CXTranslationUnit unit = data->unit;
    
    CXSourceRange cursorRange = clang_getCursorExtent(cursor);
    if (clang_Range_isNull(cursorRange) || clang_Cursor_isNull(cursor))
    {
        return CXChildVisit_Continue;
    }
    
    //TODO: Handle more precisely and more cases here with breakpoints that aren't valid LLDB Debugger expressions
    {
        std::string cursorName = getCursorName(cursor);
        std::string cursorSrc = getCursorSource(cursor);
        std::string cursorKind = getCursorKind(cursor);
        std::string cursorType = getCursorType(cursor);
        std::string cursorLocation = getCursorLocation(cursor);
        
        std::cout << "cursorLocation: " << cursorLocation << std::endl;
        std::cout << "cursorName: " << cursorName << std::endl;
        std::cout << "cursorType: " << cursorType << std::endl;
        std::cout << "cursorKind: " << cursorKind << std::endl;
        std::cout << "cursorSrc: " << cursorSrc << std::endl;
        
        // Now we know this cursor is in the range for breakpoint i
        CXCursorKind kind = clang_getCursorKind(cursor);
        CXString kindName = clang_getCursorKindSpelling(kind);
        CXString cursorSpelling = clang_getCursorSpelling(cursor);
        std::string kindNameStr = clang_getCString(kindName);
        std::string cursorSpellingStr = clang_getCString(cursorSpelling);
        
        // Get cursor location for better error reporting
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        unsigned int line, column;
        clang_getSpellingLocation(loc, nullptr, &line, &column, nullptr);
        
        std::string locationStr = " at line "  + std::to_string(line) + " " + bpInfo.getLocationInfo(line, column);
        
        //----------------------------------------------------------------------
        // 1. Check for template expressions
        //----------------------------------------------------------------------
        if (kind == CXCursor_TemplateRef ||
            kind == CXCursor_TemplateTypeParameter ||
            kind == CXCursor_TemplateTemplateParameter ||
            kind == CXCursor_ClassTemplate ||
            kind == CXCursor_FunctionTemplate ||
            kind == CXCursor_ClassTemplatePartialSpecialization) {
            bpAnalysis.hasTemplateExpressions = true;
            bpAnalysis.issues.push_back("Template expression found: '" + cursorSpellingStr + "'" + locationStr);
        }
        
        //----------------------------------------------------------------------
        // 2. Check for lambda functions
        //----------------------------------------------------------------------
        if (kind == CXCursor_LambdaExpr) {
            bpAnalysis.hasLambdaFunctions = true;
            bpAnalysis.issues.push_back("Lambda function found" + locationStr);
        }
        
        //----------------------------------------------------------------------
        // 3. Check for operator overloads
        //----------------------------------------------------------------------
#ifdef LLDB_COMPATIBLE_BREAKPOINTS
        if (kind == CXCursor_CXXMethod || kind == CXCursor_UnexposedExpr) {
            std::string name = cursorSpellingStr;
            if (name.find("operator") == 0 && name.find("operator[]") != 0) {
                bpAnalysis.hasOperatorOverloads = true;
                bpAnalysis.issues.push_back("Operator overload used: '" + name + "'" + locationStr);
            }
        }
#endif
        
#if 1 //#ifdef LLDB_COMPATIBLE_BREAKPOINTS
        //----------------------------------------------------------------------
        // 4. Check for exception handling
        //----------------------------------------------------------------------
        if (kind == CXCursor_CXXTryStmt ||
            kind == CXCursor_CXXCatchStmt ||
            kind == CXCursor_CXXThrowExpr) {
            bpAnalysis.hasExceptionHandling = true;
            bpAnalysis.issues.push_back("Exception handling construct found" + locationStr);
        }
#endif
        
#if 1 //#ifdef LLDB_COMPATIBLE_BREAKPOINTS
        //----------------------------------------------------------------------
        // 5. Check for RTTI features
        //----------------------------------------------------------------------
        if (kind == CXCursor_CXXDynamicCastExpr ||
            kind == CXCursor_CXXTypeidExpr ||
            (kind == CXCursor_DeclRefExpr && cursorSpellingStr == "typeid")) {
            bpAnalysis.hasRTTI = true;
            bpAnalysis.issues.push_back("RTTI feature found: '" + cursorSpellingStr + "'" + locationStr);
        }
#endif
        
        //----------------------------------------------------------------------
        // 6. Check for complex STL usage
        //----------------------------------------------------------------------
        if ((kind == CXCursor_NamespaceRef && cursorSpellingStr == "std") ||
            (kind == CXCursor_TypeRef && cursorSpellingStr.find("std::") == 0)) {
            
            // List of complex STL containers and algorithms that might be problematic
            const std::vector<std::string> complexStlTypes = {
                "map", "set", "multimap", "multiset", "unordered_map", "unordered_set",
                "list", "forward_list", "deque", "priority_queue",
                "regex", "bitset", "valarray", "tuple", "variant", "optional",
                "any", "function", "bind", "mem_fn",
                "for_each", "transform", "find_if", "remove_if", "sort", "stable_sort",
                "unique", "accumulate", "inner_product", "partial_sum"
            };
            
            // Check for complex STL types
            for (const auto& stlType : complexStlTypes) {
                if (cursorSpellingStr.find(stlType) != std::string::npos) {
                    bpAnalysis.hasComplexSTL = true;
                    bpAnalysis.issues.push_back("Complex STL usage found: '" + cursorSpellingStr + "'" + locationStr);
                    break;
                }
            }
        }
        
        //----------------------------------------------------------------------
        // 7. Check for C++17/20 specific features
        //----------------------------------------------------------------------
        // C++17 features
        if ((kind == CXCursor_StructDecl && cursorSpellingStr.find("optional") != std::string::npos) ||
            (kind == CXCursor_StructDecl && cursorSpellingStr.find("variant") != std::string::npos) ||
            (kind == CXCursor_StructDecl && cursorSpellingStr.find("any") != std::string::npos) ||
            (kind == CXCursor_VarDecl && cursorSpellingStr.find("inline") != std::string::npos) ||
            (kind == CXCursor_ForStmt && kindNameStr.find("RangeBasedFor") != std::string::npos)) {
            bpAnalysis.hasCpp17Features = true;
            bpAnalysis.issues.push_back("C++17 feature detected: '" + cursorSpellingStr + "'" + locationStr);
        }
        
        // C++20 features (less precise without dedicated AST nodes)
        if ((kind == CXCursor_StructDecl && cursorSpellingStr.find("span") != std::string::npos) ||
            (kind == CXCursor_TypeRef && cursorSpellingStr.find("concepts") != std::string::npos) ||
            (kind == CXCursor_TypeRef && cursorSpellingStr.find("coroutine") != std::string::npos) ||
            (kind == CXCursor_FunctionDecl && cursorSpellingStr.find("co_await") != std::string::npos) ||
            (kind == CXCursor_FunctionDecl && cursorSpellingStr.find("co_yield") != std::string::npos)) {
            bpAnalysis.hasCpp20Features = true;
            bpAnalysis.issues.push_back("Possible C++20 feature detected: '" + cursorSpellingStr + "'" + locationStr);
        }
        
        //----------------------------------------------------------------------
        // 8. Check for string literal comparison
        //----------------------------------------------------------------------
#ifdef LLDB_COMPATIBLE_BREAKPOINTS
        if (kind == CXCursor_BinaryOperator) {
            CXToken* tokens;
            unsigned numTokens;
            clang_tokenize(unit, cursorRange, &tokens, &numTokens);
            
            bool hasStringVar = false;
            bool hasStringLiteral = false;
            bool hasEqualityOp = false;
            
            for (unsigned j = 0; j < numTokens; j++) {
                CXString spelling = clang_getTokenSpelling(unit, tokens[j]);
                std::string tokenText = clang_getCString(spelling);
                clang_disposeString(spelling);
                
                if (tokenText == "==" || tokenText == "!=") {
                    hasEqualityOp = true;
                }
                
                // Look for string-related types
                if (tokenText.find("string") != std::string::npos ||
                    tokenText.find("String") != std::string::npos ||
                    tokenText.find("std::") != std::string::npos) {
                    hasStringVar = true;
                }
                
                // Look for string literals
                if ((tokenText.front() == '"' && tokenText.back() == '"') ||
                    (tokenText.front() == '\'' && tokenText.back() == '\'')) {
                    hasStringLiteral = true;
                }
            }
            
            if (hasEqualityOp && hasStringVar && hasStringLiteral) {
                bpAnalysis.hasStringLiteralComparison = true;
                bpAnalysis.issues.push_back("Direct string comparison with literal detected" + locationStr +
                                          " - use strcmp(), compare(), or [0] comparison instead");
            }
            
            clang_disposeTokens(unit, tokens, numTokens);
        }
#endif
        
#ifdef LLDB_COMPATIBLE_BREAKPOINTS
        //----------------------------------------------------------------------
        // 9. Check for auto keyword usage (can sometimes be problematic in LLDB)
        //----------------------------------------------------------------------
        if (kind == CXCursor_VarDecl) {
            CXType type = clang_getCursorType(cursor);
            CXString typeSpelling = clang_getTypeSpelling(type);
            std::string typeStr = clang_getCString(typeSpelling);
            
            if (typeStr.find("auto") != std::string::npos) {
                bpAnalysis.issues.push_back("Auto type deduction used" + locationStr +
                                         " - may not resolve correctly in LLDB");
            }
            
            clang_disposeString(typeSpelling);
        }
#endif
        
#ifdef LLDB_COMPATIBLE_BREAKPOINTS
        //----------------------------------------------------------------------
        // 10. Check for smart pointers (can be problematic for inspection)
        //----------------------------------------------------------------------
        if (kind == CXCursor_TypeRef) {
            if (cursorSpellingStr.find("unique_ptr") != std::string::npos ||
                cursorSpellingStr.find("shared_ptr") != std::string::npos ||
                cursorSpellingStr.find("weak_ptr") != std::string::npos) {
                bpAnalysis.issues.push_back("Smart pointer usage found" + locationStr +
                                         " - use explicit methods like .get() or -> for inspection");
            }
        }
#endif
        
        //----------------------------------------------------------------------
        // 11. Find standard library functions with their return types
        // We can use this information for instrumentation
        //----------------------------------------------------------------------
        if (kind == CXCursor_CallExpr) {
            CXCursor referenced = clang_getCursorReferenced(cursor);
            CXSourceLocation loc = clang_getCursorLocation(referenced);
            
            // Check explicitly if the referenced cursor is a free-standing function
            if (clang_getCursorKind(referenced) != CXCursor_FunctionDecl) {
                // Skip anything that is not a plain C-style function declaration
                return CXChildVisit_Recurse;
            }

            if (clang_Location_isInSystemHeader(loc)) {
                CXString spelling = clang_getCursorSpelling(referenced);

                // Get the return type
                CXType funcType = clang_getCursorType(referenced);
                CXType returnType = clang_getResultType(funcType);
                
                CXString returnTypeSpelling = clang_getTypeSpelling(returnType);

                std::string functionName = getClangString(spelling);
                std::string returnTypeName = getClangString(returnTypeSpelling);
                bpAnalysis.stdCalls[functionName] = returnTypeName;
            }
        }
        
        // Cleanup
        clang_disposeString(kindName);
        clang_disposeString(cursorSpelling);
    }
    
    // Continue traversing
    return CXChildVisit_Recurse;
}

enum CXChildVisitResult findBreakpoints(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    auto* data = static_cast<VisitorData*>(client_data);
    auto& results = *(data->results);
    
    //auto& ranges = *(data->ranges);
    std::vector<BreakpointLocationInfo>& bpLocationInfo = *(data->bpLocationInfo);
    
    CXTranslationUnit unit = data->unit;
    
    CXSourceRange cursorRange = clang_getCursorExtent(cursor);
    if (clang_Range_isNull(cursorRange) || clang_Cursor_isNull(cursor))
    {
        return CXChildVisit_Continue;
    }
    
    std::string cursorFile = getCursorFile(cursor);
    if(cursorFile != data->file)
    {
        // Continue traversing
        return CXChildVisit_Recurse;
    }
    
    // Check each location range
    for (size_t i = 0; i < bpLocationInfo.size(); i++) {
        if (clang_Range_isNull(bpLocationInfo[i].range)) {
            continue;
        }
        
        bool overlaps = rangesOverlap(cursorRange, bpLocationInfo[i].range);
        if (!overlaps) {
            continue;
        }
        
        std::string cursorSrc = getCursorSource(cursor);
        
        // Now we know this cursor is in the range for breakpoint i
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        // Get cursor location for better error reporting
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        unsigned int line, column;
        clang_getSpellingLocation(loc, nullptr, &line, &column, nullptr);
        std::string locationStr = " at line " + std::to_string(line) + ", column " + std::to_string(column);
        
        if (kind == CXCursor_CompoundStmt &&
            rangesMatch(cursorRange, bpLocationInfo[i].range) &&
            bpLocationInfo[i].snippetCode == cursorSrc)
        {
            BreakpointData bpData = { unit, &bpLocationInfo[i], &(results.data()[i]) };
            bpData.bpAnalysis->snippet = cursorSrc;
            
            // Traverse the AST once for all locations
            clang_visitChildren(cursor, validateBreakpoint, &bpData);
            
            return CXChildVisit_Continue;
        }
    }
    
    // Continue traversing
    return CXChildVisit_Recurse;
}

// Function to analyze multiple breakpoint locations
std::vector<BreakpointAnalysis> analyzeBreakpointCode(
    CXTranslationUnit unit,
    const std::string& filename,
    std::vector<BreakpointLocationInfo>& locations) {
    
    // Initialize results with line numbers from input locations
    std::vector<BreakpointAnalysis> results;
    
    // Set up output and source ranges
    CXFile file = clang_getFile(unit, filename.c_str());
    for (auto& loc : locations) {
        results.emplace_back(loc.lineNumber);
        
        CXSourceLocation startLoc = clang_getLocation(unit, file, loc.lineNumber, loc.snippetOffset);
        CXSourceLocation endLoc = clang_getLocation(unit, file, loc.lineNumber, loc.snippetOffset + loc.snippetLength);
        
        loc.range = clang_getRange(startLoc, endLoc);
    }
    
    VisitorData data = { &results, &locations, unit, filename };
    
    // Traverse the AST once for all locations
    clang_visitChildren(clang_getTranslationUnitCursor(unit), findBreakpoints, &data);
    
    return results;
}

std::string Debugger::evaluateBreakpoints(CCodeProject* project,
                                   const std::string& functionName,
                                   const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const
{
    if(customBreakpoints.empty())
    {
        return std::string();
    }
    
    std::string debugNotes;
    if(!checkFunctionExists(project, functionName, debugNotes))
    {
        return debugNotes;
    }
    
    std::set<int32_t> sourceLines;
    std::set<int32_t> reportBpLines;
    for(auto bp : customBreakpoints)
    {
        if(sourceLines.find(bp->source_line) != sourceLines.end())
        {
            //Found breakpoints on the same line!
            reportBpLines.insert(bp->source_line);
        }
        
        sourceLines.insert(bp->source_line);
    }
    
    if(!reportBpLines.empty())
    {
        std::string linesWithManyBPs = "The following souce lines have more than one breakpoints: ";
        for(auto ln : reportBpLines) {
            linesWithManyBPs += std::to_string(ln) + " ";
        }
        
        linesWithManyBPs += "\nOnly one breakpoint per line is allowed\n";
        return linesWithManyBPs;
    }
    
    //We can't have breakpoint on the return statements
    std::shared_ptr<FunctionDebugInfo> dbgInfo = getFunctionDebugInfo(project, functionName);
    std::set<uint32_t> bpOnReturnReported;
    std::string reportBpOnReturn;
    if(dbgInfo)
    {
        for(auto ret : dbgInfo->m_returns)
        {
            auto ret_start = ret.m_start.m_lineNumber;
            auto ret_end   = ret.m_end.m_lineNumber;
            
            for(auto bp : customBreakpoints)
            {
                if(ret_start <= bp->source_line && bp->source_line <= ret_end)
                {
                    if(bpOnReturnReported.find(bp->source_line) == bpOnReturnReported.end())
                    {
                        reportBpOnReturn += "Unable to set breakpoint on the same source line with return statement: " + std::to_string(bp->source_line) + "\n";
                        bpOnReturnReported.insert(bp->source_line);
                    }
                }
            }
        }
    }
    
    if(!reportBpOnReturn.empty())
    {
        return reportBpOnReturn;
    }
    
    std::string platform = getPlatform() + "_test";
    uint32_t options = CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG;
    auto info = project->getCompilationInfo(functionName, platform, options);
    
    // 1. Start building a std::vector<std::string> of flags
    std::vector<std::string> flags;

    // 2. Add each include directory as "-I/dir"
    for (const auto& incDir : info->m_includeDirs) {
        flags.push_back("-I" + incDir);
    }

    // 3. Split the m_options string by whitespace (if it has something like "-std=c++17 -DDEBUG=1")
    //    and push each token into flags.
    if (!info->m_options.empty()) {
        
        std::vector<std::string> optionTokens;
        boost::split(optionTokens, info->m_options, boost::is_any_of(" \t\n\r"), boost::token_compress_on);
        
        for (auto& opt : optionTokens) {
            
            //Skip the C++ revision option for now.
            //The code will be tested for the lowest possible standart revision, for which it still compiles
            if(opt.find("-std=c++") == std::string::npos)
            {
                flags.push_back(opt);
            }
        }
    }
    
    //The idea is, as low the C++ revision is as much the C++ is C-style
    std::vector<std::string> cppStdVersions = {/*"-std=c++03",*/
#ifdef EVALUATE_BREAKPOINTS_WITH_MIN_CPP_VER
                                                "-std=c++11", "-std=c++14",
#endif
                                                "-std=c++17"};

    // 4. Convert flags to an array of const char*
    std::vector<const char*> clangArgs;
    clangArgs.reserve(flags.size());
    for (auto& f : flags) {
        clangArgs.push_back(f.c_str());
    }
    
    std::string sysroot = getSysRoot();
    std::string resourceDir = getClangResourceDir();
    std::string cxxInclude  = getCppInclude();
    std::string cxxIncludeOpt = "-I" + cxxInclude;
    
    clangArgs.push_back("-D_LIBCPP_HAS_NO_WIDE_CHARACTERS");
    clangArgs.push_back("-isysroot");
    clangArgs.push_back(sysroot.c_str());
    
    clangArgs.push_back("-resource-dir");
    clangArgs.push_back(resourceDir.c_str()); // ← critical for stdarg.h, stdint.h, intrinsics, etc.
    clangArgs.push_back(cxxIncludeOpt.c_str()); // ← libc++ headers
    
    CXIndex index = clang_createIndex(0, 0);
    
    std::string errors;
    
    //The code will be tested for the lowest possible standart revision, for which it still compiles
    //Then we will evaluate breakpoint condition/expression in this environment
    for(size_t i=0; i<cppStdVersions.size(); ++i)
    {
        clangArgs.push_back(cppStdVersions[i].c_str());
        
        // 5. Call clang_parseTranslationUnit
        CXTranslationUnit tu = clang_parseTranslationUnit(
                                        index,
                                        info->m_sourceFilePath.c_str(),    // The source file path
                                        clangArgs.data(),                  // array of const char*
                                        static_cast<int>(clangArgs.size()),// number of arguments
                                        nullptr,
                                        0,
                                        CXTranslationUnit_None
                                        );
        
        // Optional: check for errors, handle accordingly
        if (!tu) {
            std::cerr << "Failed to parse Translation Unit: " << info->m_sourceFilePath << std::endl;
        }
        
        errors = printDiagnostics(tu, false);
        clang_disposeTranslationUnit(tu);
        
        if(errors.empty())
        {
            break;
        }
        
        clangArgs.pop_back();
    }
    
    //TODO: Handle here cade that can't compile
    if(!errors.empty())
    {
        std::string coudntCompile = "Couldn't evaluate breakpoints, the code for function: " + functionName + " doesn't compile:\n\n";
        coudntCompile += errors;
        
        clang_disposeIndex(index);
        return coudntCompile;
    }
    
    // 6) Instrument the source with the breakpoint code snippet
    
    std::ifstream srcFile(info->m_sourceFilePath);
    std::string source((std::istreambuf_iterator<char>(srcFile)), std::istreambuf_iterator<char>());
    std::pair<std::string, int> instrumentedSrc;
    instrumentedSrc.first = source;
    
    std::vector<BreakpointLocationInfo> snippetLocations;
    
    std::string wrongSourceLocations;
    for(auto breakpoint : customBreakpoints)
    {
        std::string snippet = breakpoint->getCodeSnippet();
        
        //returns the modified text with inserted snippet and the character position at the line from where the inserted snippet starts
        //If lineNumber is < 1 or bigger that the number of the lines in text the returned string must be empty and the returned integer must be -1
        instrumentedSrc = insertSnippet(instrumentedSrc.first, snippet + " ", breakpoint->source_line, true);
        
        if(instrumentedSrc.second < 0)
        {
            wrongSourceLocations += "Invalid source line (" + std::to_string(breakpoint->source_line);
            wrongSourceLocations += ") for breakpint with:\n";
            wrongSourceLocations += "condition: " + breakpoint->condition + "\n";
            wrongSourceLocations += "expression: " + breakpoint->condition + "\n";
            wrongSourceLocations += "\n";
        }
        
        //std::cout << "INSTRUMENTED SOURCE:" << std::endl << std::endl << printLineNumbers(instrumentedSrc.first, 0) << std::endl;
        
        for(auto location : snippetLocations)
        {
            //We need to shift all breakpoints from this line with the lenght of the new snippet
            if(location.lineNumber == breakpoint->source_line)
            {
                location.lineNumber += snippet.length();
            }
        }
        
        snippetLocations.push_back(BreakpointLocationInfo(breakpoint->source_line, //unsigned int line = 0,
                                                          instrumentedSrc.second, //unsigned int offset = 0,
                                                          (unsigned int)snippet.length(), //unsigned int length = 0,
                                                          snippet));
    }
    
    if(!wrongSourceLocations.empty())
    {
        wrongSourceLocations += "Consult with the source of function '" + functionName;
        wrongSourceLocations += "' and 'HOW TO SET BREAKPOINTS' sections for how to properly set breakpoins on a valid locations\n";
        
        std::cout << wrongSourceLocations;
        
        clang_disposeIndex(index);
        return wrongSourceLocations;
    }
    
    std::string unsavedFileName = "Instrumented_" + functionName + ".cpp";
    CXUnsavedFile unsavedFile = { unsavedFileName.c_str(), instrumentedSrc.first.c_str(), (unsigned long)instrumentedSrc.first.length() };
    
    CXTranslationUnit unit = clang_parseTranslationUnit(
        index,
        unsavedFileName.c_str(),  // Provide a dummy file name
        clangArgs.data(), (int)clangArgs.size(),
        &unsavedFile, 1,  // Pass the unsaved file
        CXTranslationUnit_KeepGoing
        //| CXTranslationUnit_Incomplete
    );
    
    errors = printDiagnostics(unit, false);
    
    //The instrumented code doesn't compile. Probably something is wrong with the breakpoint snippets
    //Most probably invalid variables in the context
    if(!errors.empty())
    {
        std::string message = "INSTRUMENTED SOURCE:\n\n" + printLineNumbers(instrumentedSrc.first, 0) + "\n\n";
        
        message += "Couldn't evaluate breakpoints conditions and/or expressions at the specified locations:\n\n";
        message += errors;
        message += "\nConsult with the function source code and with 'HOW TO SET BREAKPOINTS' section!";
        
        std::cout << message;
        
        clang_disposeTranslationUnit(unit);
        clang_disposeIndex(index);
        return message;
    }
    
    // 7) Here I need an AST traversal that will evaluate the condition and expression snippet
    // as much as possible for the following C++ features we try to avoid:
    
    // - Template expressions
    // - Lambda functions
    // - Operator overloads
    // - Exception handling (try/catch)
    // - Complex STL containers/algorithms
    // - RTTI features (dynamic_cast, typeid)
    // - C++17/20 specific features
    //
    // The traversal should return a string with clear notes on what has been identified
    // so that we can pass this string to the LLM to revise the breakpoint condition/expression
    
    std::vector<BreakpointAnalysis> analysis = analyzeBreakpointCode(unit,
                                                                     unsavedFileName,
                                                                     snippetLocations);
    
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    
    std::stringstream issues;
    for(auto bpAnalysis : analysis) {
        
        if(bpAnalysis.issues.empty())
        {
            continue;
        }
        
        for(auto bp : customBreakpoints) {
            //The sambe breakpoint
            if(bp->getCodeSnippet() == bpAnalysis.snippet &&
               bp->source_line == bpAnalysis.lineNumber)
            {
                issues << "Breakpoin line: " << bp->source_line;
                if(bp->hasCondition())
                {
                    issues << " condition: " << bp->getConditionCode();
                }
                if(bp->hasExpression())
                {
                    issues << " expression: " << bp->getExpressionCode();
                }
                
                for(auto issue : bpAnalysis.issues)
                {
                    issues << std::endl << "   ";
                    issues << issue;
                }
            }
        }
    }
    
    bool hasIssues = issues.str().length();
    std::string message;
    if(hasIssues)
    {
        message += "INSTRUMENTED SOURCE:\n\n" + printLineNumbers(instrumentedSrc.first, 0) + "\n\n";
        
        message += "The following breakpoints reported issues during validation:\n\n";
        message += issues.str();
        
        message += "\nConsult with the function source and 'HOW TO SET BREAKPOINTS' section for how to properly set breakpoints\n";
    }
    else
    {
        //Instrument the breakpoints
        for(auto bpAnalysis : analysis) {
            
            for(auto bp : customBreakpoints) {
                //The sambe breakpoint
                if(bp->getCodeSnippet() == bpAnalysis.snippet &&
                   bp->source_line == bpAnalysis.lineNumber)
                {
                    bp->instrumentCalls(functionName, bpAnalysis.stdCalls);
                }
            }
        }
    }

    return message;
}

std::pair<std::string, std::string> Debugger::lldbLogSections(const std::string& lldbLog)
{
    std::string bpSetup, bpTrace;
    size_t copyFrom = lldbLog.find("(lldb) process launch --stdout ");
    if(copyFrom != std::string::npos)
    {
        bpTrace = lldbLog.substr(copyFrom);
        bpSetup = lldbLog.substr(0, copyFrom);
    }
    else
    {
        bpTrace = lldbLog;
    }

    return std::make_pair(bpSetup, bpTrace);
}

std::pair<std::string, std::string> Debugger::debugFunction2(CCodeProject* project, const TestDef& test, const std::string& functionName,
                                                            //int line, const std::string& condition, const std::string& expression)
                                                            const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints)
{
    //TODO: THIS FUNCTION IS DEPRICATED
    std::vector<std::string> lldbBeforeArgs;
    
    setBreakpoints(lldbBeforeArgs, project, functionName, customBreakpoints);
    
#ifdef LLDB_VERBOSE_BATCH_MODE
    lldbArgs.push_back("-o");
    lldbArgs.push_back("breakpoint list");
#endif
    
    //lldbBeforeArgs.push_back("-o");
    //lldbBeforeArgs.push_back("quit");
    // Build the executable path and run the LLDB batch
    std::string projDir = project->getProjDir();
    std::string execPath = projDir + "/build/" + getPlatform() + "_test/main";
    std::string commandLine = buildPrompt(test.test.command, {{"exec", execPath}});
    
    //std::string printEndCmd = "print \\\"[[lldb setup end: " + functionName + "]]\\\"";
    
    std::string lldbOnlyLog;
    
    //TODO: THIS FUNCTION IS DEPRICATED
    std::string stdOutput;
    
    return lldbLogSections(lldbOnlyLog);
}

std::string Debugger::getTrajectory(int fromStep, int toStep, bool addCurrent, bool addSummary, bool includeRunInfo) const
{
    std::string trajectory;
    if(m_trajectory.empty() && !includeRunInfo)
    {
        return trajectory;
    }
    
    trajectory += "\nHere is the current progress debugging the application:\n\n";
    //int i = 1;
    
    if(toStep < 0)
    {
        toStep = (int)m_trajectory.size();
    }
    
    if(addSummary && !m_summary.empty())
    {
        trajectory += "\nSUMMARY OF PREVIOUS STEPS:\n";
        trajectory += m_summary;
    }
    
    //TODO: Find in reverse order which is the last information step
    
    bool hasRunInTrajectory = false;
    std::string currentTrajectory;
    std::string runStepType;
    for(int s=fromStep; s<toStep; ++s)
    {
        const DebugStep& step = m_trajectory[s];
        uint32_t stepIndex = m_previousSteps + s + 1;
        
        if(step.m_action == "run_test")
        {
            hasRunInTrajectory = true;
            runStepType = "run_test";
        }
        else if(step.m_action == "debug_function")
        {
            hasRunInTrajectory = true;
            runStepType = "debug_function";
        }
        
        currentTrajectory += "\nSTEP " + std::to_string(stepIndex) + ": ";
        currentTrajectory += step.m_action;
        if(!step.m_subject.empty())
        {
            currentTrajectory += " for '" + step.m_subject + "'";
        }
        currentTrajectory += "\n";
        if(!step.m_motivation.empty())
        {
            currentTrajectory += "Motivation for this setp:\n";
            currentTrajectory += step.m_motivation;
        }
        currentTrajectory += "\n";
        if(!step.m_debugNotes.empty())
        {
            currentTrajectory += "Debug notes for this setp:\n";
            currentTrajectory += step.m_debugNotes;
        }
        currentTrajectory += "\n";
        if(!step.m_logSummary.empty())
        {
            currentTrajectory += "Log summary for this setp:\n";
            currentTrajectory += step.m_logSummary;
        }
        currentTrajectory += "\n";
    }
    
    if(addCurrent)
    {
        if(m_nextStep.action_type == "run_test")
        {
            hasRunInTrajectory = true;
            runStepType = "run_test";
        }
    }
    
    //We should be directly printing this in the NextStep prompt
    if(includeRunInfo)
    {
        trajectory += "\n\nINFORMATION FOR THE LAST RUN STEP: " + std::to_string(m_lastRunStep) + " STARTS HERE\n";
        
        if(!hasRunInTrajectory)
        {
            trajectory += m_lastRunInfo;
        }
        
        if(!m_rewardHackingReview.empty())
        {
            trajectory += m_rewardHackingReview;
        }
       
        trajectory += m_lastRunTestLog;
        trajectory += "INFORMATION FOR THE LAST RUN STEP ENDS HERE\n";
    }
    
    trajectory += currentTrajectory;
    
    if(addCurrent)
    {
        //TODO: We need a functin to print step attributes for this and for the loop above
        //std::string currentStepStr = std::to_string(m_previousSteps + m_trajectory.size());
        std::string currentStepStr = std::to_string(m_nextStep.m_stepId);
        trajectory += "\nCURRENT STEP " + currentStepStr + ": ";
        //trajectory += "\nRECENTLY EXECUTED STEP : ";
        trajectory += m_nextStep.action_type;
        if(!m_nextStep.action_subject.empty())
        {
            trajectory += " for '" + m_nextStep.action_subject + "'";
        }
        trajectory += "\n";
        if(!m_nextStep.motivation.empty())
        {
            trajectory += "Motivation for this setp:\n";
            trajectory += m_nextStep.motivation;
        }
        trajectory += "\n";
    }
    
    return trajectory;
}

void Debugger::pushTrajectory(CCodeProject* project)
{
    //Ensure we have the correct info incorporating the modification from the last debug step
    std::string appInfo = getHighLevelAppInfo(project, m_system, PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH);
    
    appInfo += getTrajectory(0, 0, false, true, true);
    
    project->pushMessage(appInfo, "user", true);
    
    //std::vector<std::string> m_rawTrajectory;
    for(auto step : m_rawTrajectory)
    {
        project->pushMessage(step.first, step.second, true);
    }
}

void Debugger::debugAnalysis(CCodeProject* project, const std::string& function,
                             RunAnalysis& analysis, const TestDef& test)
{
    std::string application = project->getProjectName();
    std::string trajectory = getTrajectory(0, -1, true, true);
    
    std::string detailedInfo = getFunctionDetailedInfo(project, function);
    
    std::string breakpointInfo;
    //TODO: Add info for the breakpoint
    if(!m_nextStep.breakpoints.empty())
    {
        if(m_nextStep.breakpoints.size() > 1) {
            breakpointInfo += "\nIn addition, the following custom breakpoints have been requested\n\n";
        } else {
            breakpointInfo += "\nIn addition, the following custom breakpoint has been requested\n\n";
        }
        
        breakpointInfo += getBreakpointsInfo(false, false, function, m_nextStep.breakpoints);
        
        //breakpointInfo += "The condition and expression for each custom breakpoint have instrumented versions that are compatible with the LLDB Debugger\n\n";
        breakpointInfo += "From the trace log, try to analyse fore each custom breakpoint:\n";
        breakpointInfo += "- Has it been hit or not - for what reasons, what conclusions we can draw from this?\n";
        breakpointInfo += "What can we learn for the execution flow and the values of the variables before hitting the breakpoint. Are they correct, are they expected?\n";
        breakpointInfo += "- From the other trace points: ";
        breakpointInfo += "1. Can we trace the debugged function input arguments and results at the time of hitting the custom breakpoints? ";
        breakpointInfo += "2. Can we trace input/output values for functions called by the debugged function before the breakpoint hit? ";
        breakpointInfo += "3. What can we trace for the execution of the debbuged function from the trace points at scopes start/end before hitting the custom breakpoints? ";
        breakpointInfo += "What can we learn from 1, 2 and 3, what is important for our analysis?\n\n";
        
        std::cout << std::endl << "CUSTOM BREAKPOINTS: " << std::endl << std::endl << breakpointInfo << std::endl;
    }
    
    m_tracer.loadBreakpointTraces(m_workingDirectory + "/breakpoints", function);
    
    std::set<std::string> subSystems;
    std::string systemData = getSubSystemsData(project, subSystems);
    std::string subSystemsStr = getAsCsv(subSystems);
    
    std::string traceDescStr = "\n\nFormats of the trace and log are specified in 'TRACE DESCRIPTION' and 'LOG DESCRIPTION' sections\n\n";
    
    auto analysisHintIt = analysisFullTrace(project, analysis, test);
    //std::string analysisHint1;
    std::string analysisHint = analysisHintIt.second;
    bool requiresAnalysis = analysisHintIt.first;
    if(!analysisHint.empty())
    {
        analysisHint = "//Here is a hint from my quick analysis of the full trace:\n" + analysisHint + "\n//The quick analysis ends here\n\n";
    }
    
    std::string analysisFrameHint = analysisFrameTrace(project, function, m_nextStep.invocation);
    if(!analysisFrameHint.empty())
    {
        analysisHint += "//And here is a hint from my quick analysis of the trace for the function '";
        analysisHint += function + "':\n" + analysisFrameHint;
    }
    
    std::string hitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);
    
    std::string maxDepth = std::to_string(PRINT_MAX_FUNCTIONS_DEPTH-1);
    Prompt promptDebugAnalysis("SystemDebugAnalysis.txt",{
        {"app_info", m_appInfo},
        {"trajectory", trajectory},
        {"application", application},
        {"function", function},
        {"function_info", detailedInfo},
        {"breakpoint", breakpointInfo},
        {"hit_count", hitCount},
        {"max_depth", maxDepth},
        {"sub_systems", subSystemsStr},
        {"system_data", systemData},
        
        {"analysis_hint", analysisHint},
        //{"trace_desc", traceDescStr},
        {"test_log", analysis.m_testLog},
        {"test_delta", m_testFunctionalityDelta}
    });
    
    web::json::value schema;
    setupSchema<RunAnalysis>(schema);
    
    web::json::value fullTraceObject;
    Cache cache;
    project->captureContext(std::string());
    project->inference(cache, promptDebugAnalysis, schema, fullTraceObject);
    project->popContext();
    
    analysis.from_json(fullTraceObject);
}

void Debugger::debugLoadTrace(const std::string& root, CCodeProject* project, const std::string& function, const TestDef& test)
{
    std::string oldWorkingDir = m_workingDirectory;
    m_workingDirectory = root;
    std::string oldAction = m_nextStep.action_type;
    std::string oldSubject = m_nextStep.action_subject;
    uint32_t oldInvocation = m_nextStep.invocation;
    
    m_nextStep.action_type = "debug_function";
    m_nextStep.action_subject = "comp_sem_rebuild_decl_symbol_table_stack";
    m_nextStep.invocation = 7;
    
    m_nextStep.breakpoints.clear();
    
    //Add the fake breakpoints here:
    auto customBp = std::make_shared<Breakpoint>();
    customBp->source_line = 36;
    m_nextStep.breakpoints.push_back(customBp);
    
    customBp = std::make_shared<Breakpoint>();
    customBp->source_line = 66;
    m_nextStep.breakpoints.push_back(customBp);
    
    customBp = std::make_shared<Breakpoint>();
    customBp->source_line = 76;
    m_nextStep.breakpoints.push_back(customBp);
    
    customBp = std::make_shared<Breakpoint>();
    customBp->source_line = 78;
    m_nextStep.breakpoints.push_back(customBp);
    
    std::string traceLogPath = root + "/trace.txt";
    std::ifstream traceFile(traceLogPath);
    std::string traceLog = std::string((std::istreambuf_iterator<char>(traceFile)),
                                       std::istreambuf_iterator<char>());
    
    
    
    std::string logPath = root + "/stdout.log";
    std::ifstream logFile(logPath);
    
    m_logger.parse(std::string((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>()));
    
    RunAnalysis analysis;
    std::string traceSetup;
    debugAnalysis(project, function, analysis, test);
    
    m_nextStep.invocation = oldInvocation;
    m_nextStep.action_subject = oldSubject;
    m_nextStep.action_type = oldAction;
    m_workingDirectory = oldWorkingDir;
}

std::string Debugger::fixFunction(CCodeProject* project, const TestDef& test, const std::string& functionName, std::string& before, std::string& debugNotes)
{
    std::string output;
    
    auto it = project->nodeMap().find(functionName);
    if(it == project->nodeMap().end())
    {
        debugNotes += "Unable to find function '" + functionName + "'\n";
        return "";
    }
    
    CCodeNode* ccNode = (CCodeNode*)it->second;
    if(!ccNode)
    {
        debugNotes += "Unable to find function '" + functionName + "'\n";
        return "";
    }
    
    std::set<std::string> referencedNodes;
    std::string visibleDataTypes = getVisibleDataTypes(project, ccNode, false, referencedNodes);
    
    std::stringstream description;
    description << "//" << ccNode->m_brief.brief << std::endl;
    std::string implementation = visibleDataTypes + "\n\n\n";
    implementation += "//Brief description of '" + functionName + "' before the fix. ";
    implementation += "Prioritize suggested solution from the debugging notes if the new implementation requires conceptual changes\n\n";
    implementation += description.str();
    implementation += "\n//Implementation before the fix\n";
    implementation += printFunctionSource(project, functionName, ccNode->m_implementation.m_source);
    before = implementation;
    
    std::string checklist = project->source_checklist.prompt({{"function", ccNode->m_prototype.declaration}});
    
    std::string call_api = "None";
    if(ccNode->m_calls.items.size())
    {
        call_api = ccNode->summarizeCalls(true, false, true);
    }
    
    std::string callers;
    
    CCodeNode* parent = nullptr;
    if(ccNode->m_this->m_parent)
    {
        parent = (CCodeNode*)ccNode->m_this->m_parent->m_data;
        assert(parent);
    }
    
    if(parent)
    {
        callers += "Note: ";
        callers += ccNode->m_prototype.declaration;
        callers += "\nis called in the implementatin of: ";
        //callers += parent->m_implementation.definition;
        callers += parent->m_brief.func_name;
        callers += "\n";
        
        callers += parent->getContexInfo(false, true, true, referencedNodes);
    }
    
    std::string references;
    for(auto ref : ccNode->m_referencedBy)
    {
        CCodeNode* ccNodeRef = (CCodeNode*)ref;
        if(ccNodeRef == parent)
        {
            continue;
        }
        
        references += ccNodeRef->m_prototype.declaration;
        references += "\n";
        references += ccNodeRef->m_prototype.brief;
        references += "\n\n";
    }
    
    if(!references.empty())
    {
        callers += "\nHere are other functions that also call '" + ccNode->m_brief.func_name + "\n";
        callers += references;
    }
    
    {
        std::string nodeLogDir = project->getProjDir() + "/logs/nodes/" + ccNode->m_brief.func_name;
        Client::getInstance().setContextLogDir(nodeLogDir);
    }
    
    std::string trajectory = getTrajectory(0, -1, true, true);
    Prompt promptFixFunction("FixFunction.txt",{
                        {"trajectory", trajectory},
                        {"function", functionName},
                        {"call_api", call_api},
                        {"callers", callers},
                        {"checklist", checklist},
                        {"implementation", implementation},
                        {"struct_members", project->define_struct_members.prompt()}
    });
    
    std::string declBefore = removeWhitespace(ccNode->m_prototype.m_signature.str());
    
    //This model should be able to find the source update!
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ASSISTANT);
    
    std::string degugNotes;
    std::string message = promptFixFunction.str();
    project->captureContext(std::string());
    
    //Prompt for 'PROGRAMMING LANGUAGE' and 'AVAILABLE LIBRARIES'
    {
        Prompt programming("Programming.txt",{});
        project->pushMessage(programming, "user", true);
    }
    
    //The thinking is, we allow refactorin for app->sub-systems->components->modules but not for functions (last level 5)
    bool enableRefactoring = ccNode->getDepth() <= 4;
    bool fixed = project->updateSource(functionName, CCodeNode::FUNC_FIX, message, output, enableRefactoring);
    project->popContext();
    
    //Ensure we are
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
    
    //TODO: at this point, consider full compile traversal to give a chance to review compilation
    //to fix errors in other functions due to data changes or function signature changes
    
    std::string declAfter = removeWhitespace(ccNode->m_prototype.m_signature.str());
    if(declBefore != declAfter)
    {
        //TODO: We must not be here
        degugNotes += "//" + ccNode->m_prototype.brief + "\n";
        degugNotes += "Signature: " + declAfter + "\n";
        degugNotes += "Previous signature: " + declBefore + "\n";
        degugNotes += "Signature of the function '" + functionName + "'  has been updated. This is a critical error. Check the references to this function!!!\n";
        
        return "";
    }
    
    if(fixed)
    {
        degugNotes += "//" + ccNode->m_prototype.brief + "\n";
        degugNotes += ccNode->m_prototype.declaration + "\n";
        degugNotes += "Function '" + functionName + "'  has been fixed\n";
     
        //TODO: Enable this to update the detailed function description after the fix
        //Note that the brief description is most probably already updated in the updateSource() call
#if 0
        //Update the brief and detailed descriptions here
        project->captureContext(std::string());
        project->pushMessage(message, "user", true);
        project->pushMessage(ccNode->m_implementation.definition, "assistant", true);
        
        Client::getInstance().selectLLM(InferenceIntent::DEFINE);
        
        web::json::value schema;
        setupSchema<FunctionDefinition>(schema);
        Cache cache;
        auto object = web::json::value();
        
        //std::string api = summarizeCalls(true, false, true);
        std::string api; //empty for now, should be visible from the updated function source
        std::string parent_info;//also empty
        
        std::string funcDescriptionMessage = project->describe_function.prompt({
            {"abstract", project->abstract_programming.prompt()},
            {"function", ccNode->m_brief.func_name},
            //{"brief", m_prototype.description},
            {"parent", parent_info},
            {"api", api}
        });
        
        funcDescriptionMessage += "\n\nConsider detailed 'description' to 3 paragraphs or fewer, less than 2000 characters.\n";
        funcDescriptionMessage += "Consider 'brief' field to 3 sentences or fewer, less than 300 characters.";
        
        funcDescriptionMessage += "\n\nNow Update the brief and detailed descriptions reflecting the updated function source!";
        
        project->inference(cache, funcDescriptionMessage, schema, object);
        
        ccNode->m_description.from_json(object);
        
#ifdef LIMIT_DESCRIPTION_SIZE
        {
            std::string checkLength;
            if(ccNode->m_description.brief.length() > BRIEF_MAX_CHARACTERS)
            {
                checkLength += " consider 'brief' description to 3 sentences or fewer, less than ";
                checkLength += std::to_string(BRIEF_MAX_CHARACTERS_NOTE) + " characters.\n";
            }
            
            if(ccNode->m_description.description.length() > 2*DESCRIPTION_MAX_CHARACTERS)
            {
                checkLength += " consider detailed 'description' to 3 paragraphs or fewer, less than ";
                checkLength += std::to_string(DESCRIPTION_MAX_CHARACTERS_NOTE) + " characters.\n";
            }
            
            if(!checkLength.empty())
            {
                std::string reviseMessage;
                
                reviseMessage += "\n\nNote: The description is longer than usual, but this is not necessarily a problem. However,";
                reviseMessage += checkLength;
                reviseMessage += "\n\n";
                
                object = web::json::value();
                project->inference(cache, reviseMessage, schema, object);
            }
        }
#endif
        
        ccNode->m_description.from_json(object);
        
        project->popContext();
#endif
        
        ccNode->save(); //Preserve the new
        //TODO: Consider self-review loop
        
        Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
        
        return ccNode->m_implementation.m_source;
    }
    else
    {
        //Very last chance to fix the compilation
        if(project->compile())
        {
            //Get the node again
            auto it = project->nodeMap().find(functionName);
            if(it == project->nodeMap().end())
            {
                debugNotes += "Unable to find function '" + functionName + "'\n";
                return "";
            }
            
            CCodeNode* ccNode = (CCodeNode*)it->second;
            
            return ccNode->m_implementation.m_source;
        }
    }
    
    {
        //This ensures debug logs will go the the right debug step directory.
        uint32_t stepIndex = m_previousSteps + uint32_t( m_trajectory.size() ) + 1;
        std::string logDir = project->getProjDir() + "/logs/debug/" + test.name + "/step_" + std::to_string(stepIndex);
        Client::getInstance().setContextLogDir(logDir);
    }
    
    degugNotes = "Couldn't fix the source for function '" + functionName + "'\n";
    degugNotes += "Compiler output: " + output + "\n";
    
    return "";
}

void Debugger::setStepHint(const TestDef& test)
{
    std::string stepHint;
    
    //TODO: Could this be more verbose and better formatedd
    
    stepHint += m_nextStep.action_type + " for '" + m_nextStep.action_subject + "' ";
    stepHint += " invc: " + std::to_string(m_nextStep.invocation);
    stepHint += " line: " + std::to_string(m_nextStep.line_number);
    //stepHint += " - " + m_nextStep.motivation.substr(0,96) + "...";
    //Let's go with the full motivation for now
    stepHint += " - " + m_nextStep.motivation + "\n";
    
    Client::getInstance().setStepHint(stepHint);
}

std::string Debugger::validateStep(CCodeProject* project, const TestDef& test, int attempt)
{
    //TODO: validate all properties of the m_nextStep - action, subject, ... not just the breakpoints
    
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ASSISTANT);
    
    std::string feedback;
    auto repairInvocation = [&](const std::string& functionName, uint32_t lastInvocation, const char* actionLabel)
    {
        if(functionName.empty() || m_nextStep.invocation <= 0 || lastInvocation == 0)
        {
            if(!functionName.empty() && m_nextStep.invocation > 0 && lastInvocation == 0)
            {
                feedback += "No recorded invocation is available for action '" + std::string(actionLabel);
                feedback += "' and function '" + functionName + "'. ";
                feedback += "Choose a different function/invocation or another valid next step.\n";
            }
            return;
        }
        
        if((uint32_t)m_nextStep.invocation <= lastInvocation)
        {
            return;
        }
        
        m_nextStep.invocation = (int)lastInvocation;
    };
    
    if(m_nextStep.action_type == "debug_function")
    {
        if(!m_nextStep.breakpoints.empty())
        {
            feedback += evaluateBreakpoints(project, m_nextStep.action_subject, m_nextStep.breakpoints);
            if(!feedback.empty())
            {
                feedback = ensureFunctionIsVisible(project, m_nextStep.action_subject, true) + feedback;
            }
        }
    }
    else if(m_nextStep.action_type == "run_test")
    {
        if(m_hasValidBuild)
        {
            feedback += "You've requested a new test run, but I see no changes to the source code since the last run. ";
            //feedback += "If you just need more information use the appropriate 'Actins for requesting more information' explained in the 'DEBUGGING WORKFLOW' section";
            feedback += "Summary of the last run is available in the context. Check the provided 'INFORMATION FOR THE LAST RUN STEP' ";
            if(!m_rewardHackingReview.empty())
            {
                feedback += " Pay attention to the last reward-hacking review. ";
            }
            feedback += "If you just need more information use the appropriate 'Actions for requesting more information' explained in the 'DEBUGGING WORKFLOW' section. Both actions 'run_test' and 'debug_function' capture log and trace available for investigation.";
        }
    }
    else if(m_nextStep.action_type == "fix_function")
    {
        std::string debugNotes;
        if(!checkFunctionExists(project, m_nextStep.action_subject, debugNotes))
        {
            feedback += "The requested function doesn't exist or is not allowed for edit\n";
        }
        else
        {
            //TODO: Do we need this any longer?
            int sameFunctionFixed = 0;
            int functionsFixed = 0;
            
            for (std::size_t i = m_trajectory.size(); i-- > 0; ) {
                
                DebugStep& step = m_trajectory[i];
                if(step.m_action == "run_test"){
                    break;
                } else if(step.m_action == "fix_function") {
                    functionsFixed++;
                    if(step.m_subject == m_nextStep.action_subject) {
                        sameFunctionFixed++;
                    }
                }
            }
            
            if(sameFunctionFixed > 0) {
                feedback += "The function '" + m_nextStep.action_subject + "' has already been fixed. ";
            }
            if(functionsFixed > 2) {
                feedback += "Three or more functions have been fixed since the last test. ";
            }
            if(sameFunctionFixed > 0 || functionsFixed > 2) {
                feedback += "Consider running the test before fixing next function!";
            }
        }
    }
    else if(m_nextStep.action_type == "file_info")
    {
        //TODO: I need to keep an eye on this feedback
        std::string direcoty = boost_fs::path(m_nextStep.action_subject).parent_path().string();
        if(!direcoty.empty() && direcoty != "./")
        {
            feedback += "I see you specify directory part in your file name. ";
            feedback += "Note that file_info command is only for the files in the test working directory";
            feedback += " - text files consumed or produced by the test.\n";
        }
    }
    else if(trim(m_nextStep.action_type).empty())
    {
        feedback += "The 'action_type' field is empty or whitespace-only. ";
        feedback += "Return corrected JSON with one valid next action from the debugging workflow.\n";
    }
    else if(m_nextStep.isInformationRequest())
    {
        if(m_nextStep.action_type == "function_info")
        {
            uint32_t lastTraceInvocation = 0;
            if(auto lastFrame = m_tracer.getLastInvocation(m_nextStep.action_subject))
            {
                lastTraceInvocation = lastFrame->m_invocation.second;
            }
            
            uint32_t lastLogInvocation = m_logger.logGetLastInvocation(m_nextStep.action_subject).second;
            uint32_t lastInvocation = lastTraceInvocation >= lastLogInvocation ? lastTraceInvocation : lastLogInvocation;
            repairInvocation(m_nextStep.action_subject, lastInvocation, "function_info");
        }
        else if(m_nextStep.action_type == "log_info")
        {
            uint32_t lastLogInvocation = m_logger.logGetLastInvocation(m_nextStep.action_subject).second;
            repairInvocation(m_nextStep.action_subject, lastLogInvocation, "log_info");
        }
        
        if(m_contextVisibility.isVisible(m_nextStep.action_type, m_nextStep.action_subject, m_nextStep.invocation, m_nextStep.line_number))
        {
            std::string repeatedRequest = m_nextStep.action_type + "('" + m_nextStep.action_subject + "'";
            if(m_nextStep.invocation > 0)
            {
                repeatedRequest += ", invocation=" + std::to_string(m_nextStep.invocation);
            }
            if(m_nextStep.line_number > 0)
            {
                repeatedRequest += ", line=" + std::to_string(m_nextStep.line_number);
            }
            repeatedRequest += ")";
            
            feedback += "\nThe requested information " + repeatedRequest + " has already been provided in the discussion. ";
            feedback += "Do not repeat the same request. Inspect the currently available information and choose a different next step. ";
            feedback += "\n";
        }
    }
    
#ifdef LIMIT_DEBUG_NOTES_SIZE
    if(m_nextStep.motivation.length() > 4000 && attempt <= 1 /*&& !feedback.empty()*/)
    {
        feedback += "\nDescription in the 'motivation' field is more than 2048 characters. ";
        feedback += "Ideally keep to 3 paragraphs or fewer, under 2000 characters total. ";
        feedback += "This is not a hard limit as we don't want to miss something important. ";
        feedback += "However, if more concise response makes sense you can consider this recommendation.\n";
    }
#endif
    
    if(m_system != "main") //Are we in unit test debugging
    {
        if(m_nextStep.action_type == "fix_function" && (m_nextStep.action_subject == "main" || boost_fs::path(m_nextStep.action_subject).stem().string() == "main"))
        {
            feedback += "We are currently debugging the unit test for function '" + m_system + "'\n";
            feedback += "When debugging unit tests it is not possible to edit the source of the test main function. ";
            feedback += "The source of the test main function (full main.cpp file) should be available in the 'TEST DESCRIPTION' section.\n\n";
            
            if(m_attemptsToFixUnitTestMain >= 5)
            {
                feedback += "If you are sure there are enough evidences that ";
                feedback += "the unit test mian function source is the only reason for the test to fail ";
                feedback += "you can requets stop of the test by requesting next action 'stop_unit_test' ";
                feedback += "and I will try to fix the test sources and resume the debugging. ";
                feedback += "If you decide to do this, provide the justification in the motivation section\n";
            }
            else
            {
                feedback += "Try to investigate a bit more and ensure the reason for the test to fail is not somewhere else\n";
            }
            
            m_attemptsToFixUnitTestMain++;
        }
        else if(m_nextStep.action_type == "function_info" && m_nextStep.action_subject == "main")
        {
            feedback += "We are currently debugging the unit test for function '" + m_system + "'\n";
            feedback += "The source of the test main function (full main.cpp file) should be available in the 'TEST DESCRIPTION' section. ";
            feedback += "Consult with the source and continue the debugging with another action\n";
        }
        else if(m_nextStep.action_type == "file_info" &&
                boost_fs::path(m_nextStep.action_subject).stem().string() == "main")
        {
            feedback += "We are currently debugging the unit test for function '" + m_system + "'\n";
            feedback += "The source of the test main function (full main.cpp file) should be available in the 'TEST DESCRIPTION' section. ";
            feedback += "Consult with the source and continue the debugging with another action\n";
        }
        else if(m_nextStep.action_type == "debug_function" && m_nextStep.action_subject == "main")
        {
            feedback += "We are currently debugging the unit test for function '" + m_system + "'\n";
            feedback += "The source of the test main function (full main.cpp file) should be available in the 'TEST DESCRIPTION' section. ";
            feedback += "When debugging unit tests it is not possible to debug the test main function. ";
            feedback += "Consult with the source and continue the debugging with another action\n";
        }
    }
    
    if((m_nextStep.action_type == "function_info" && m_nextStep.action_subject == "PRINT_TEST") ||
        (m_nextStep.action_type == "file_info" && boost_fs::path(m_nextStep.action_subject).stem().string() == "PRINT_TEST"))
    {
        feedback += "PRINT_TEST is a system-level macro and we can't inspect its source or edit it. ";
        feedback += "It prints verbose information for the purpose of unit tests and debugging. ";
        feedback += "It doesn't print to stdout, only to the debug log - the one that can be explored with the 'log_info' action. ";
        feedback += "For the purpose of regex matching required by the test framwork success checks std::cout has to be used.";
    }
    
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
    
    return feedback;
}

void Debugger::optimizeTrajectory(CCodeProject* project, const TestDef& test)
{
    int trajectorySize = (int)m_trajectory.size();
    int step=0;
    
#ifndef DEBUGGER_INTERLEAVED_TRAJECTORY
    if(m_trajectory.size() <= MAX_TRAJECTORY_FOR_SUMMARIZATION)
        return;
    
    for(; step < trajectorySize; ++step)
    {
        //if(m_trajectory[step].m_action == "run_test")
        {
            if(step >= MIN_STEPS_TO_SUMMARIZE)
            {
                break;
            }
        }
    }
    //couldn't find run_test command to summarize the range from 0 to the first run_test after the MIN_STEPS_TO_SUMMARIZE
    if(step == trajectorySize)
        return;
#else
    // Compact only right after a run has just been executed.
    if(trajectorySize <= 1)
        return;
    
    if(m_trajectory.back().m_action != "run_test")
        return;
    
    // Keep only the latest run_test step, summarize the previous prefix.
    step = trajectorySize - 1;
#endif
    
    std::string stepStr = std::to_string(m_previousSteps + step);
    std::string trajectoryToSummarize = getTrajectory(0, step, false, true);
    
    if(trajectoryToSummarize.empty())
        return;
    
    std::string remainingTrajectory = getTrajectory(step, -1, false, false);
    
    {
        Client::getInstance().setLLM(LLMRole::DIRECTOR);
        
        std::string application = project->getProjectName();
        
        Prompt promptSummarize("SummarizeTrajectory.txt",{
                            {"application", application},
                            {"app_info", m_appInfo},
                            {"remaining_trajectory", remainingTrajectory},
                            {"to_summarize", trajectoryToSummarize},
                            {"step", stepStr}
        });
        
        
        std::string message = promptSummarize.str();
        project->captureContext(std::string());
        
        Cache cache;
        bool truncated = false;
        std::string summary = "review";
        
        project->captureContext(std::string());
        project->inference(cache, promptSummarize, summary, &truncated);
        project->popContext();
        
        project->pushMessage(promptSummarize, "user", true);
        
        int attempts = 0;
        const int maxAttempts = 5;
        while((summary.length() < 100 || summary.length() > 6144) && attempts++ < maxAttempts)
        {
            project->captureContext(std::string());
            project->pushMessage(summary, "assistant", true);
            
            std::string feedback;
            uint32_t lenght = (uint32_t)summary.length();
            if(lenght < 100)
            {
                feedback += "\nUnable to read the summary from the response! Provide it again!\n";
            }
            else if(lenght > 6144)
            {
                feedback += "\nThe summary is too long. It should be less than 6000 characters. Up to 20 concise sentences!\n";
            }
            
            feedback += "\nPlease in your response summarize ONLY the content in the 'STEPS TO SUMMARIZE' section, check the instruction!\n\n";
            
            summary = "review";
            truncated = false;
            project->inference(cache, feedback, summary, &truncated);
            
            project->popContext();
        }
        
        m_summary = summary;
        project->popContext();
        
        Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
    }
    
    m_previousSteps += step;
    
    // Erase from beginning include the element at index 'step'
    m_trajectory.erase(m_trajectory.begin(), m_trajectory.begin() + step);
}

bool Debugger::saveTrajectory(CCodeProject* project, const TestDef& test)
{
    project->saveStats();
    Client::getInstance().flushLog();
    
    uint32_t stepIndex = m_previousSteps + (uint32_t)m_trajectory.size();
    
    std::string trajectoryDir = Client::getInstance().getProjectDirectory() + "/debug/" + test.name + "/trajectory";
    std::string stepDir = trajectoryDir + "/step_" + std::to_string(stepIndex) + "/";
    
    if(!boost_fs::create_directories(stepDir))
    {
        std::cout << "Unable to create step directory: " << stepDir << std::endl;
        return false;
    }
    
    //Save the summary as it looks on this step
    if(!m_summary.empty())
    {
        hen::saveToFile(m_summary, stepDir + "summary.txt");
    }
    
    if(!m_lldbLog.empty())
    {
        hen::saveToFile(m_lldbLog, stepDir + "lldb.log");
    }
    
    //Save the returned json
    web::json::value nextStepJson = m_nextStep.to_json();
    
    if(m_nextStep.action_type == "debug_function")
    {
        uint32_t bpIndex = 0;
        for(auto bp : m_nextStep.breakpoints)
        {
            // Create std_calls object
            web::json::value stdCalls = web::json::value::object();

            for (auto &stdCall : bp->m_stdCalls)
            {
                stdCalls[utility::conversions::to_string_t(stdCall.first)] =
                web::json::value::string(utility::conversions::to_string_t(stdCall.second));
            }
            
            nextStepJson[U("breakpoints")].as_array()[bpIndex][U("std_calls")] = stdCalls;
            
            bpIndex++;
        }
    }
    
    hen::saveJson(nextStepJson, stepDir + "nextStep.json");
    
    //Create/update the trajectory configuration file
    std::string tracjectoryFile = stepDir + "tracjectory.json";
    if(boost_fs::exists(tracjectoryFile))
    {
        boost_fs::remove(tracjectoryFile);
    }
    
    auto trajectoryCfg = json::value::object();
    trajectoryCfg.as_object()[U("previousSteps")] = json::value::number(m_previousSteps);
    
    uint32_t allSteps = m_previousSteps + (uint32_t)m_trajectory.size();
    trajectoryCfg.as_object()[U("allSteps")] = json::value::number(allSteps);
    
    if (m_nextStep.action_type != "run_test" && !m_trajectory.empty() &&
        m_trajectory.back().m_action == "run_test")
    {
        m_infoStepsStart = stepIndex + 1;
    }
    
    //Save only the last step from the debug trajectory
    if(!m_trajectory.empty())
    {
        std::string dbgStepPath = stepDir + "dbgStep.json";
        auto& lastDbgStep = m_trajectory.back();
        
        if(lastDbgStep.m_action == "run_test" ||
           lastDbgStep.m_action == "debug_function")
        {
            m_lastRunStep = stepIndex;
            //Save stdout and trace logs, everything from the working direcotry
            //This can quickly grow in size!!!
            std::string wdCopy = stepDir + "wd";
            boost::system::error_code ec;
            boost_fs::copy(m_workingDirectory, wdCopy,
                           boost_fs::copy_options::recursive |
                           boost_fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cout << "Copy test working directory failed! from: " << m_workingDirectory << " to: " << wdCopy << "\n";
                std::cout << "Error: " << ec.message() << "\n";
            }
        }
        
        lastDbgStep.save(dbgStepPath);
    }
    
    trajectoryCfg.as_object()[U("infoStepsStart")] = json::value::number(m_infoStepsStart);
    trajectoryCfg.as_object()[U("lastRunStep")] = json::value::number(m_lastRunStep);
    
    hen::saveJson(trajectoryCfg, tracjectoryFile);
    
    return true;
}

std::string Debugger::loadTestLogFromStep(CCodeProject* project, const TestDef& test, uint32_t debugStepId)
{
    std::string directoryForThisStep = project->getProjDir() + "/debug/" + test.name + "/trajectory/step_" + std::to_string(debugStepId) + "/wd";
    if(!boost_fs::exists(directoryForThisStep))
    {
        return std::string();
    }
    
    std::string oldWd = m_workingDirectory;
    m_workingDirectory = directoryForThisStep;
    
    std::string log;
    log += "\n\n*************** TEST SCRIPT EXECUTION LOG START ***************\n\n";
    log += loadTestLogForStep(project, test, test.pretest, "pretest", debugStepId);
    
    //This is the test step
    {
        bool debug = false;
        bool testResult = false;
        
        std::string rawCmd = test.test.command;
        std::string cmd = rawCmd;
        std::string expectedResult;
        std::string stdoutRegex;
        parsePrefixFlags(rawCmd, debug, testResult, expectedResult, stdoutRegex, cmd);
        //std::string expectedResultStr = std::to_string(expectedResult);
        
        log += "Test command:\n\n";
        log += cmd + "\n\n";
        
        std::string consoleLog = getFileContent(m_workingDirectory + "/console.log");
        
        log += "Test command stdout:\n";
        if(!consoleLog.empty())
        {
            std::string consoleLogLimited = consoleLog.length() > 2048 ? consoleLog.substr(0, 2048) + "...[[truncated]]": consoleLog;
            log += consoleLogLimited + "\n\n";
        }
        else
        {
            log += "Empty output string\n\n";
        }
        
        
        std::stringstream ssTestInput;
        checkTestStepInput(ssTestInput, project, test.test.input_files, test.test.output_files, "test", false);
        
        int returnCode = 65535;
        
        auto line = getFirstLine(m_workingDirectory + "/memo.txt");
        if(line && line->length() > 0)
        {
            if(!m_lldbLog.empty()) {
                log += "lldb log from the test command:\n\n";
                log += m_lldbLog + "\n\n";
                
                if(!parseLastExitCode(m_lldbLog, returnCode))
                {
                    //TODO: Do we want to do something here?
                }
            }
        }
        
        std::stringstream ssTestOutput;
        checkTestStepOutput(ssTestOutput, project, test.test.output_files, "test");
        
        std::string returnCodeStr = std::to_string(returnCode);
        bool mainTestPass = true;
        
        if(testResult && !expectedResult.empty() && returnCodeStr != expectedResult)
        {
            log += "Returned result '" + returnCodeStr + "' is not expected! Expected result is: '" + expectedResult + "'\n\n";
            mainTestPass = false;
        }
        
        if(testResult && !stdoutRegex.empty())
        {
            std::string regexErr;
            if (!fullRegexMatch(consoleLog, stdoutRegex, regexErr)) {
               
                if (!regexErr.empty()) {
                    //We must not be here
                    std::cout << "ERROR: invalid stdout regex: " << regexErr << "\n";
                } else {
                    log += "stdout doesn't fully match the expected regex pattern: " + stdoutRegex + "\n";
                    log += "Note that PRINT_TEST doesn't print to the stdout, only std::cout does.\n";
                    mainTestPass = false;
                }
            }
        }
        
        if(!m_logger.empty())
        {
            log += "\nApplication log is captured for analysis.\n";
        }
        else
        {
            log += "\nApplication log is empty.\n";
        }
    }
    
    log += loadTestLogForStep(project, test, test.posttest, "posttest", debugStepId);
    log += "\n\n*************** TEST SCRIPT EXECUTION LOG END ***************\n\n";
    
    log = replaceAll(log, project->getProjDir(), ".");
    m_workingDirectory = oldWd;
    
    return log;
}

std::string Debugger::loadTestLogForStep(CCodeProject* project, const TestDef& test, const TestStep& step, const std::string& testStepName, uint32_t debugStepId)
{
    bool stepResults = true;
    std::string log;
    
    std::string directoryForThisStep = project->getProjDir() + "/debug/" + test.name + "/trajectory/step_" + std::to_string(debugStepId) + "/wd";
    
    std::stringstream ssTestInput;
    checkTestStepInput(ssTestInput, project, step, testStepName, false);
    log += ssTestInput.str();
    
    if(!step.commands.empty())
    {
        uint32_t i=1;
        for(auto c : step.commands)
        {
            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *c;
            std::string cmd = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmd);
            
            std::string testCommandIndexStr = std::to_string(i);
            std::string commandName = testStepName + "_cmd" + testCommandIndexStr;
            
            std::string testCommand = directoryForThisStep + "/" + commandName;
            std::string testCommandLine = getFileContent(testCommand + "Command.txt");
            std::string testCommandResult = getFileContent(testCommand + "Output.txt");
            
            
            log += testStepName + " command " + testCommandIndexStr + ": ";
            log += cmd + "\n\n";
            
            log += testStepName + " output " + testCommandIndexStr + ":\n";
            if(!testCommandResult.empty())
            {
                std::string resultLimited = testCommandResult.length() > 2048 ? testCommandResult.substr(0, 2048) + "...[[truncated]]": testCommandResult;
                log += resultLimited + "\n\n";
            }
            else
            {
                log += "Empty output string\n\n";
            }
            
            std::string resultStr = getTestResult(testCommandResult);
            
            if(finalResult && !expectedResult.empty() && resultStr != expectedResult)
            {
                stepResults = false;
                log += "Returned result '" + resultStr + "' is not expected! Expected result is: '" + expectedResult + "'\n\n";
            }
            
            if(finalResult && !stdoutRegex.empty())
            {
                std::string stdoutLog = testCommandResult;//getFileContent(directoryForThisStep + "/console.log");
                
                std::string regexErr;
                if (!fullRegexMatch(stdoutLog, stdoutRegex, regexErr)) {
                   
                    if (!regexErr.empty()) {
                        //We must not be here
                        std::cout << "ERROR: invalid stdout regex: " << regexErr << "\n";
                    } else {
                        log += "stdout doesn't fully match the expected regex pattern: " + stdoutRegex + "\n";
                        log += "Note that PRINT_TEST doesn't print to the stdout, only std::cout does.\n";
                        stepResults = false;
                    }
                }
            }
            
            i++;
        }
    }
    
    std::stringstream ssTestOutput;
    checkTestStepOutput(ssTestOutput, project, step, testStepName);
    log += ssTestOutput.str();
    
    return log;
}

bool Debugger::loadTrajectory(CCodeProject* project, const TestDef& test)
{
    std::string trajectoryDir = Client::getInstance().getProjectDirectory() + "/debug/" + test.name + "/trajectory";
    
    if(!boost_fs::exists(trajectoryDir))
    {
        m_previousSteps = 0;
        return true;
    }
    
    auto stepsRange = getConsecutiveSteps(trajectoryDir);
    int lastStepIndex = stepsRange.second;
    
    if(lastStepIndex <= 0)
    {
        m_previousSteps = 0;
        return true;
    }
    
    std::string lastStepDir = "/step_" + std::to_string(lastStepIndex);
    
    std::string tracjectoryFile = trajectoryDir + lastStepDir + "/tracjectory.json";
    if(!boost_fs::exists(tracjectoryFile))
    {
        std::cout << "The selected last step file doesn't exist: " << tracjectoryFile << std::endl;
        m_previousSteps = 0;
        return true;
    }
    
    //Load the trajectory configuration json file
    web::json::value trajectoryCfg;
    hen::loadJson(trajectoryCfg, tracjectoryFile);
    
    //These properties are necessary. They must be in the json object
    if(!trajectoryCfg.has_field(U("allSteps")) || !trajectoryCfg.has_field(U("previousSteps")))
    {
        std::cout << "Unable to find 'allSteps' or 'previousSteps' fields in the json object" << std::endl;
        return false;
    }
    
    m_previousSteps = trajectoryCfg[U("previousSteps")].as_number().to_uint32();
    uint32_t allSteps = trajectoryCfg[U("allSteps")].as_number().to_uint32();
    m_infoStepsStart = trajectoryCfg[U("infoStepsStart")].as_number().to_int32();
    m_lastRunStep = trajectoryCfg[U("lastRunStep")].as_number().to_uint32();
    
    //Load trajectory
    uint32_t startStep = m_previousSteps;
    
    if(m_infoStepsStart > 0 && startStep > m_infoStepsStart) {
        startStep = uint32_t(m_infoStepsStart - 1);
    }
    
    for(uint32_t s = startStep; s < allSteps; ++s)
    {
        uint32_t stepIndex =  s + 1;
        
        std::string stepDir = trajectoryDir + "/step_" + std::to_string(stepIndex) + "/";
        
        if(!boost_fs::exists(stepDir))
        {
            std::cout << "Directory for the requested step doesn't exist: " << stepDir << std::endl;
            return false;
        }
        
        DebugStep dbgStep;
        
        std::string dbgStepPath = stepDir + "dbgStep.json";
        dbgStep.load(dbgStepPath);
        
        if(stepIndex > m_previousSteps)
        {
            m_trajectory.push_back(dbgStep);
        }
    }
    
    //Try to load the last run step (the one before infoStepsStart)
    //and check if there is a reward hacking hint
    if(m_infoStepsStart > 1)
    {
        std::string stepDir = trajectoryDir + "/step_" + std::to_string(m_infoStepsStart-1) + "/";
        if(boost_fs::exists(stepDir))
        {
            DebugStep dbgStep;
            std::string dbgStepPath = stepDir + "dbgStep.json";
            dbgStep.load(dbgStepPath);
            
            assert(dbgStep.m_action == "run_test"); //something is wrong with this trajectory

            if(startsWith(dbgStep.m_logSummary, "Reward-hacking prcatices have been identified") ||
               startsWith(dbgStep.m_logSummary, "Reward hacking prcatices have been identified") //This for backward compatibility
               )
            {
                m_rewardHackingReview = dbgStep.m_debugNotes;
            }
        }
    }
    
    //Load the last step data
    {
        uint32_t stepIndex = allSteps;
        std::string stepDir = trajectoryDir + "/step_" + std::to_string(stepIndex) + "/";
        
        web::json::value jsonNextStep;
        std::string nextStepPath = stepDir + "nextStep.json";
        hen::loadJson(jsonNextStep, nextStepPath);
        
        m_nextStep.from_json(jsonNextStep);
        
        if(m_nextStep.action_type == "debug_function")
        {
            // Access the breakpoints array from the loaded JSON
            auto& bpArray = jsonNextStep[U("breakpoints")].as_array();

            for (size_t i = 0; i < m_nextStep.breakpoints.size(); ++i)
            {
                auto& bpJson = bpArray[i];

                std::map<std::string, std::string> stdCalls;

                // Check if the "std_calls" field exists and is an object
                if (bpJson.has_field(U("std_calls")) && bpJson[U("std_calls")].is_object())
                {
                    for (auto& kv : bpJson[U("std_calls")].as_object())
                    {
                        std::string key = utility::conversions::to_utf8string(kv.first);
                        std::string val = utility::conversions::to_utf8string(kv.second.as_string());
                        stdCalls.emplace(std::move(key), std::move(val));
                    }
                }

                m_nextStep.breakpoints[i]->instrumentCalls(m_nextStep.action_subject, stdCalls);
            }
        }
        
        m_summary = hen::loadFile(stepDir + "summary.txt");
        
        std::string lldbLog = hen::loadFile(stepDir + "lldb.log");
        lldbLog = replaceAll(lldbLog, project->getProjDir(), ".");
        m_lldbLog = lldbLog;
    }
    
    //Load last run logs and traces
    {
        uint32_t stepIndex = m_lastRunStep;
        std::string lastRunStepStr = std::to_string(stepIndex);
        std::string stepDir = trajectoryDir + "/step_" + lastRunStepStr + "/";
        
        //Load the stdout log
        std::string logPath = stepDir + "wd/stdout.log";
        if(boost_fs::exists(logPath))
        {
            std::ifstream logFile(logPath);
            
            m_logger.parse(std::string((std::istreambuf_iterator<char>(logFile)),
                                       std::istreambuf_iterator<char>()));
        }
        
        
        
        //Log the trace
        std::string tracePath = stepDir + "wd/trace.txt";
        if(boost_fs::exists(tracePath))
        {
            std::ifstream traceFile(tracePath);
            std::string traceLog = std::string((std::istreambuf_iterator<char>(traceFile)),
                                               std::istreambuf_iterator<char>());
            
            //Check and load breakpoints frames if the action is 'debug_function'
            web::json::value runTestObj;
            hen::loadJson(runTestObj, stepDir + "dbgStep.json");
            
            if(!runTestObj.has_field(U("action")) || !runTestObj.has_field(U("subject")))
            {
                std::cout << "Unable to find 'action' or 'subject' fields in the json object" << std::endl;
                //return false;
            }
            else
            {
                std::string actionStr = utility::conversions::to_utf8string(runTestObj[U("action")].as_string());
                if(actionStr == "debug_function" || actionStr == "run_test")
                {
                    std::string function = utility::conversions::to_utf8string(runTestObj[U("subject")].as_string());
                    m_tracer.loadFromString(traceLog);
                    m_tracer.loadBreakpointTraces(stepDir + "wd/breakpoints", function);
                    
                    DebugStep lastRunStep;
                    lastRunStep.load(stepDir + "dbgStep.json");
                    
                    std::string testLog = loadTestLogFromStep(project, test, stepIndex);
                    
                    m_lastRunInfo = lastRunStep.fullInfo();
                }
            }
        }
        
        //Load memo frames
        std::string memoFile = stepDir + "wd/memo.txt";
        auto memoFrames = m_tracer.loadStackTrace(memoFile, m_workingDirectory + "/stack");
    }
    
    auto llmConfig = Client::getInstance().getLLMConfig(LLMRole::DIRECTOR);
    uint32_t maxInfoSize = (llmConfig->context_size * 1024) * CHARACTERS_PER_TOKEN * 0.7f;
    m_compiledInfo = compileContext(project, test, maxInfoSize);
    
    return true;
}

std::string Debugger::stepLogInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& stepInfo)
{
    bool requestLog = true;
    std::string functionName = subject;
    std::string infoForCurrentStep;
    
    std::string debugNotes;
    if(!functionName.empty() && toLower(functionName) != "none")
    {
        if(!checkFunctionExists(project, functionName, debugNotes))
        {
            requestLog = false;
        }
    }
    else
    {
        functionName.clear();
    }
    
    //Log function
    if(requestLog)
    {
        uint32_t logForInvocation = invocation;
        auto logSection = m_logger.logMessagesForFunction(functionName,
                                                        lineNumber,
                                                        logForInvocation,
                                                        LOG_SECTION_SIZE);
        
        //m_logLinesCount
        std::string startLine = std::to_string(lineNumber);
        std::string linesCount = std::to_string(m_logger.linesCount());
        debugNotes += "Requesting log section start at line: " + startLine;
        //requestedInfo += " end line: "         << (endLine + 1)
        debugNotes += ", total application log lines count: " + linesCount;
        if(!functionName.empty())
        {
            debugNotes += ", log entries for function '" + functionName + "'";
            if(logForInvocation > 0)
            {
                debugNotes += " invocation: " + std::to_string(logForInvocation);
            }
        }
        
        debugNotes += "\n";
        
        if(logSection.second > 0)
        {
            infoForCurrentStep += debugNotes;
            infoForCurrentStep += "\n\n";
            infoForCurrentStep += logSection.first;
        }
        else
        {
            debugNotes += "Unable to find logged events\n";
            
            auto lastInvocation = m_logger.logGetLastInvocation(functionName);
            if(lastInvocation.second != 0)
            {
                debugNotes += "The last invocation logged for function '" + functionName + "' is: " + std::to_string(lastInvocation.second);
                debugNotes += "\n\n";
            }
            else
            {
                debugNotes += "Couldn't find any logged invocations for function '" + functionName + "'\n\n";
            }
            
            infoForCurrentStep += debugNotes;
        }
    }
    
    stepInfo.m_debugNotes = debugNotes + "\n";
    stepInfo.m_action = "log_info";
    stepInfo.m_subject = functionName;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = lineNumber >= 1 ? lineNumber : 1;
    stepInfo.m_invocation = invocation;
    
    if(m_contextVisibility.visibleLogInfo(functionName, invocation, lineNumber))
    {
        return infoForCurrentStep;
    }
    
    return "";
}

std::string Debugger::stepTraceInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
{
    //TODO: We must not be here !!!
    
    std::string debugNotes;
    std::string infoForCurrentStep;

    std::stringstream ssFrame;
    m_tracer.print(ssFrame, true, -1, true, "");
    std::string fullTrace = ssFrame.str();
    
    //TODO: This must not go into context. Remind the LLM to check the trace for the last run_test step
    //(must be in the context) and to request followup information requests.
    debugNotes += "Providing requested full trace from the last run of the application.\n";
    
    infoForCurrentStep += "\nProviding requested full trace from the last run of the application. ";
    infoForCurrentStep += "Note that the hit count for all events is limited to " + std::to_string(FULL_TRACE_MAX_EVENTS_HIT_COUNT);
    infoForCurrentStep += " and the hit count for all breakpoints (if any) is limited to " + std::to_string(MAX_BREKPOINT_HITCOUNT) + "\n\n";
    infoForCurrentStep += "//Full trace starts here\n";
    infoForCurrentStep += fullTrace;
    infoForCurrentStep += "//Full trace ends here\n";
    
    stepInfo.m_debugNotes = debugNotes + "\n";
    stepInfo.m_action = "trace_info";
    stepInfo.m_subject = "none";
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = 0;
    stepInfo.m_invocation = 0;
    
    //if(m_contextVisibility.visible(functionName, invocation, lineNumber))
    {
        return infoForCurrentStep;
    }
    
    return "";
}

std::string Debugger::stepFunctionInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& stepInfo)
{
    std::string infoForCurrentStep;
    
    std::string functionName = subject;
    std::string debugNotes;
    std::string funKey = functionName + ":";
    funKey += std::to_string(invocation > 0 ? invocation : 1) + ":";
    funKey += std::to_string(lineNumber > 0 ? lineNumber : 1);
    
    if(checkFunctionExists(project, functionName, debugNotes))
    {
        infoForCurrentStep += "Providing requested information for the function '" + functionName + "':\n\n";
        infoForCurrentStep += getRequestedInfo(project, 0, 0, {},
                                          {std::make_shared<std::string>(functionName)},
                                          {},{});
        
        std::string references;
        auto ccNode = project->getNodeByName(functionName);
        for(auto ref : ccNode->m_referencedBy)
        {
            CCodeNode* ccNodeRef = (CCodeNode*)ref;
            
            //We should find this function in the source of the function that references it
            if(ccNodeRef->m_implementation.m_source.find(functionName) != std::string::npos)
            {
                references += ccNodeRef->m_prototype.brief;
                references += "\n";
                references += ccNodeRef->m_prototype.declaration;
                references += "\n\n";
            }
        }
        
        //This must be the case except for 'main'
        if(!references.empty())
        {
            infoForCurrentStep += "The '"+ functionName + "' is called directly by the following functions:\n\n";
            infoForCurrentStep += references;
        }
        
        if(m_contextVisibility.visibleFunctionLog(functionName, invocation))
        {
            
            debugNotes = "\nParsing the log for logged events for function '" + functionName + "'\n";
            
            //Log function
            uint32_t logForInvocation = invocation;
            auto logSection = m_logger.logMessagesForFunction(functionName,
                                                              lineNumber,
                                                              logForInvocation,
                                                              LOG_TRACE_SIZE);
            if(logSection.second > 0)
            {
                //m_logLinesCount
                std::string startLine = std::to_string(lineNumber);
                std::string linesCount = std::to_string(m_logger.linesCount());
                debugNotes += "Log section start line: " + startLine;
                //requestedInfo += " end line: "         << (endLine + 1)
                debugNotes += " total application log lines count: " + linesCount;
                debugNotes += " log entries for function invocation: " + std::to_string(logForInvocation);
                
                debugNotes += "\n\n";
                
                infoForCurrentStep += debugNotes;
                infoForCurrentStep += logSection.first;
            }
            else
            {
                debugNotes += "\nUnable to find logged events for '" + functionName + "' invocation: " + std::to_string(logForInvocation) + "\n";
                
                auto lastInvocation = m_logger.logGetLastInvocation(functionName);
                if(lastInvocation.second != 0)
                {
                    debugNotes += "The last invocation logged for function '" + functionName + "' is: " + std::to_string(lastInvocation.second);
                    debugNotes += "\n\n";
                }
                else
                {
                    debugNotes += "Couldn't find any logged invocations for function '" + functionName + "'\n\n";
                }
                
                infoForCurrentStep += debugNotes;
            }
        }
        else
        {
            debugNotes += "log events for function '" + functionName + "' invocation: " + std::to_string(invocation) + "\n";
            debugNotes += "[[provided in the context]]\n";
            
            infoForCurrentStep += debugNotes;
        }
        
        if(m_contextVisibility.visibleTraceFrame(functionName, invocation))
        {
            
            //Trace information
            std::stringstream ssTrace;
            m_tracer.printFrame(ssTrace, functionName, invocation);
            std::string functionTrace = ssTrace.str();
            if(!functionTrace.empty())
            {
                //debugNotes += "\n\n";
                
                debugNotes += "trace events for function '" + functionName + "' invocation: " + std::to_string(invocation);
                
                debugNotes += "\n";
                infoForCurrentStep += debugNotes;
                infoForCurrentStep += functionTrace;
            }
            
            std::string analysisFrameHint = analysisFrameTrace(project, functionName, invocation);
            if(!analysisFrameHint.empty())
            {
                infoForCurrentStep += "//Quick analysis of the trace for the function '";
                infoForCurrentStep += functionName + "':\n" + analysisFrameHint;
            }
        }
        else
        {
            debugNotes += "trace events for function '" + functionName + "' invocation: " + std::to_string(invocation) + "\n";
            debugNotes += "[[provided in the context]]\n";
            
            infoForCurrentStep += debugNotes;
        }
        
    }
    else
    {
        infoForCurrentStep += "The function '" + functionName + "' doesn't exists. Consult with the list of available functions defined in the project:\n\n";
        infoForCurrentStep += getRequestedInfo(project, -1, 0, {}, {}, {}, {});
    }
    
    //m_visibleFunctions.insert(funKey);
    
    stepInfo.m_debugNotes = debugNotes;
    stepInfo.m_action = "function_info";
    stepInfo.m_subject = functionName;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = lineNumber > 0 ? lineNumber : 1;
    stepInfo.m_invocation = invocation > 0 ? invocation : 1;
    
    if(m_contextVisibility.visibleFunctionInfo(functionName, invocation, lineNumber))
    {
        return infoForCurrentStep;
    }
    
    //We also need to clear debugNotes
    debugNotes.clear();
    return "";
}

std::string Debugger::stepDataInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
{
    std::string dataTypeName = subject;
    
    std::string infoForCurrentStep;
    std::string owningPath;
    auto dataInfo = project->findData(dataTypeName, owningPath);
    if(dataInfo)
    {
        infoForCurrentStep += "Providing requested information for the data type '" + dataTypeName + "':\n\n";
        std::string dataTypeInfo = getRequestedInfo(project, 0, 0, {}, {},
                                          {std::make_shared<std::string>(dataTypeName)},{});
        
        if(dataTypeInfo.empty())
        {
            //infoForCurrentStep = "\nInformation for data type '" + dataTypeName + "' is already provided!\\n";
            return "";
        }
        
        infoForCurrentStep += dataTypeInfo;
        
        std::string usedByFunctions;
        uint32_t usedByFunctionsCount = 0;
        for(auto ref : dataInfo->m_references)
        {
            std::vector<std::string> pathAndParam;
            boost::split(pathAndParam, ref, boost::is_any_of("/"));
            
            if(pathAndParam.size() <= PRINT_DATA_REFS_FOR_CALLS_MAX_DEPTH)
            {
                std::string nodeName = getLastAfter(ref, "/");
                if(!usedByFunctions.empty())
                {
                    usedByFunctions += ", ";
                }
                usedByFunctions += nodeName;
                usedByFunctionsCount ++;
            }
        }
        
        if(!usedByFunctions.empty())
        {
            if(usedByFunctionsCount == dataInfo->m_references.size())
            {
                infoForCurrentStep += "The '" + dataTypeName + "' data type is used as an argument to the following functions:\n";
            }
            else
            {
                infoForCurrentStep += "Here are all the functions from the application call graph with depth " + std::to_string(PRINT_DATA_REFS_FOR_CALLS_MAX_DEPTH);
                infoForCurrentStep += " That use the data type '" + dataTypeName + "' as an argument:\n";
            }
            
            infoForCurrentStep += usedByFunctions;
            
            if(usedByFunctionsCount < dataInfo->m_references.size())
            {
                infoForCurrentStep += "\n\nIf you need to see functions with more depth that also use '" + dataTypeName + "'";
                infoForCurrentStep += " as an argument use 'functions_summary' action and specify the root function\n\n";
            }
        }
    }
    else
    {
        infoForCurrentStep += "The requested data type " + m_nextStep.action_subject + " doesn't exist in the project\n\n";
        infoForCurrentStep += "Here is a list with the available data types:\n\n";
        infoForCurrentStep += project->listAllDataTypes();
        infoForCurrentStep += "\n";
    }
    
    stepInfo.m_debugNotes = "Provide information for data type '" + dataTypeName + "'\n";
    stepInfo.m_action = "data_info";
    stepInfo.m_subject = dataTypeName;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = 0;
    stepInfo.m_invocation = 0;
    
    return infoForCurrentStep;
}

std::string Debugger::stepFileInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int lineNumber, DebugStep& stepInfo)
{
    boost_fs::path filePath(m_nextStep.action_subject);
    std::string fileName = filePath.filename().string();
    std::string fullPath = m_workingDirectory + "/" + fileName;
    std::string infoForCurrentStep;
    
    if(boost_fs::exists(fullPath))
    {
        infoForCurrentStep += "Providing requested content of the file " + fileName + "\n\n";
        int maxCharacters = LOG_SECTION_SIZE;
        
        uint32_t line_number = lineNumber > 0 ? lineNumber : 1;
        std::string fileKey = fileName + ":" + std::to_string(line_number);
        //if(m_visibleFiles.find(fileKey) == m_visibleFiles.end())
        {
            infoForCurrentStep += getFileInfo(project, fileName, lineNumber, maxCharacters);
            //m_visibleFiles.insert(fileKey);
        }
    }
    else
    {
        std::string functionName = filePath.stem().string();
        auto itFunc = project->nodeMap().find(functionName);
        if(filePath.extension() == ".cpp" && itFunc != project->nodeMap().end())
        {
            infoForCurrentStep += "The requested file " + fileName + " doesn't exist in the working directory\n\n";
            infoForCurrentStep += "I've found function with the same name in the project. Here is the info for this function:\n";
            infoForCurrentStep += getRequestedInfo(project, 0, 0, {},
                                              {std::make_shared<std::string>(functionName)},
                                              {},{});
        }
        else
        {
            std::cout << "The requested file path: " << fullPath << std::endl;
            
            infoForCurrentStep += "The requested file " + fileName + " doesn't exist in the working directory\n\n";
            infoForCurrentStep += "Here is a list with the existing files:\n";
            
            boost_fs::recursive_directory_iterator end_iter; // Default-constructed iterator acts as the end iterator.
            for (boost_fs::recursive_directory_iterator dir_itr(m_workingDirectory); dir_itr != end_iter; ++dir_itr) {
                
                // If this entry is a directory named "stack" or "breakpoints", prune it:
                if (boost_fs::is_directory(dir_itr->status())) {
                    auto name = dir_itr->path().filename().string();
                    if (name == "stack" || name == "breakpoints") {
                        dir_itr.disable_recursion_pending();  // do not recurse into this directory
                    }
                    continue;  // skip directories themselves from file processing
                }
                
                if (!boost_fs::is_directory(dir_itr->status())) { // Check if the entry is a directory.
                    // Get path relative to m_workingDirectory
                    boost_fs::path relativePath = boost_fs::relative(dir_itr->path(), m_workingDirectory);
                    if(relativePath == "stdout.log" ||
                       relativePath == ".DS_Store"  ||
                       relativePath == "trace.txt"  ||
                       relativePath == "memo.txt")
                    {
                        continue;
                    }
                    infoForCurrentStep += relativePath.string() + "\n";
                }
            }
        }
    }
    
    stepInfo.m_debugNotes = "Provide content for file " + fileName + "\n";
    stepInfo.m_action = "file_info";
    stepInfo.m_subject = fileName;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = lineNumber > 0 ? lineNumber : 1;
    stepInfo.m_invocation = 0;
    
    if(m_contextVisibility.visibleFile(fileName, lineNumber))
    {
        return infoForCurrentStep;
    }
    
    return "";
}
std::string Debugger::stepFunctionsSummary(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
{
    std::string functionName;
    std::string application = project->getProjectName();
    
    std::string infoForCurrentStep;
    if(!subject.empty() && subject != "none")
    {
        functionName = subject;
    }
    
    infoForCurrentStep += "Providing requested list of all custom function defined in the application '" + application + "' : \n\n";
    infoForCurrentStep += getRequestedInfo(project, PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH, functionName, {}, {}, {});
    
    stepInfo.m_debugNotes = "Provide list of all custom function defined in the application '" + application + "'\n";
    stepInfo.m_action = "functions_summary";
    stepInfo.m_subject = subject;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = 0;
    stepInfo.m_invocation = 0;
    
    if(m_contextVisibility.visibleFunctionsSummary(functionName))
    {
        return infoForCurrentStep;
    }
    
    return "";
}

std::string Debugger::stepCallGraph(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
{
    std::string functionName;
    std::string application = project->getProjectName();
    
    std::string infoForCurrentStep;
    if(!subject.empty() && subject != "none")
    {
        functionName = subject;
    }
    
    infoForCurrentStep += "Providing requested functions gall graph in ASCI form for application '" + application + "' : \n\n";
    infoForCurrentStep += getRequestedInfo(project, 0, PRINT_MAX_FUNCTIONS_DEPTH, functionName, {}, {}, {});
    
    stepInfo.m_debugNotes = "Provide functions gall graph in ASCI form for application '" + application + "'\n";
    stepInfo.m_action = "call_graph";
    stepInfo.m_subject = subject;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = 0;
    stepInfo.m_invocation = 0;
    
    if(m_contextVisibility.visibleCallGraph(functionName))
    {
        return infoForCurrentStep;
    }
    
    return "";
}

std::string Debugger::stepHistory(CCodeProject* project, const std::string& motivation, const TestDef& test, int invocation, DebugStep& stepInfo)
{
    std::string debugNotes;
    std::string infoForCurrentStep;
    debugNotes += "Providing requested full trace from the last run of the application.\n";
    
    infoForCurrentStep += getStepInfo(project, test, invocation);
    
    stepInfo.m_debugNotes = debugNotes + "\n";
    stepInfo.m_action = "step_info";
    stepInfo.m_subject = std::to_string(invocation);
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = 0;
    stepInfo.m_invocation = 0;
    
    if(m_contextVisibility.visibleHistory(invocation))
    {
        return infoForCurrentStep;
    }
    
    return "";
}

std::string Debugger::stepSearchSource(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
{
    std::string error;
    std::regex pattern;
    std::string infoForCurrentStep;
    
    std::string debugNotes = "\n\nSearch project sources with the following regex:\n";
    debugNotes += subject;
    debugNotes += "\n";
    
    //TODO: Consider moving this to validateStep!
    if(!tryMakeRegex(subject, pattern,std::regex_constants::ECMAScript, &error))
    {
        debugNotes += subject + " is not a valid regex pattern\n";
        debugNotes += error;
        infoForCurrentStep += debugNotes;
    }
    else
    {
        std::string searchResult = project->searchSource(pattern);
        
        //Trim to LOG_SECTION_SIZE
        if(searchResult.length() > SEARCH_TRACE_SIZE)
        {
            searchResult = searchResult.substr(0, SEARCH_TRACE_SIZE);
            searchResult += "\nThe search result has been trimmed due to size limitations\n";
        }
        else if(searchResult.empty())
        {
            searchResult += "\nNo matching strings found in the source for patter: " + subject + "\n";
        }
        else
        {
            searchResult += "\nIf you want to understand more about some of the listed functions and data types ";
            searchResult += "consider function_info and data_info actions as a next step to inspect source/taces/logs\n";
        }
        
        infoForCurrentStep += debugNotes;
        infoForCurrentStep += searchResult;
    }
    
    stepInfo.m_debugNotes = debugNotes + "\n";
    stepInfo.m_action = "search_source";
    stepInfo.m_subject = subject;
    stepInfo.m_motivation = motivation;
    stepInfo.m_lineNumber = 0;
    stepInfo.m_invocation = 0;
    
    if(m_contextVisibility.visibleSearchSource(subject))
    {
        return infoForCurrentStep;
    }
    
    return "";
}

std::string Debugger::compileContext(CCodeProject* project, const TestDef& test, uint32_t maxSize)
{
    m_contextVisibility.clear();
    
    std::string trajectoryDir = Client::getInstance().getProjectDirectory() + "/debug/" + test.name + "/trajectory";
    
    std::string compiledInfo;
     
    uint32_t lastStep = (uint32_t)m_trajectory.size();
    for(uint32_t step = lastStep; step > 1; --step)
    {
        //We need all attributes of the step like invocation, line_number,
        //loading from NextDebugStep (nextStep.json) from the previous step: (step-1)
        uint32_t stepIndex = m_previousSteps + (step-1);
        std::string stepIndexStr = std::to_string(stepIndex);
        
        std::string nextStepPath = trajectoryDir + "/step_" + stepIndexStr + "/nextStep.json";
        
        web::json::value nextStepJson;
        loadJson(nextStepJson, nextStepPath);
        
        NextDebugStep nextStep;
        nextStep.from_json(nextStepJson);
        
        //if(!NextDebugStep::isInformationRequest(nextStep.action_type))
        if(nextStep.action_type == "run_test")
        {
            break;
        }
        
        DebugStep dummyStepInfo;
        if(nextStep.action_type == "log_info")
        {
            compiledInfo += stepLogInfo(project, nextStep.action_subject, nextStep.motivation, nextStep.invocation, nextStep.line_number, dummyStepInfo);
        }
        else if(nextStep.action_type == "function_info")
        {
            compiledInfo += stepFunctionInfo(project,nextStep.action_subject, nextStep.motivation, nextStep.invocation, nextStep.line_number, dummyStepInfo);
        }
        else if(nextStep.action_type == "data_info")
        {
            compiledInfo += stepDataInfo(project, nextStep.action_subject, nextStep.motivation, dummyStepInfo);
        }
        else if(nextStep.action_type == "file_info")
        {
            compiledInfo += stepFileInfo(project, nextStep.action_subject, nextStep.motivation, nextStep.line_number, dummyStepInfo);
        }
        else if(nextStep.action_type == "functions_summary")
        {
            compiledInfo += stepFunctionsSummary(project, nextStep.action_subject, nextStep.motivation, dummyStepInfo);
        }
        else if(nextStep.action_type == "call_graph")
        {
            compiledInfo += stepCallGraph(project,nextStep.action_subject, nextStep.motivation, dummyStepInfo);
        }
        else if(nextStep.action_type == "step_info")
        {
            compiledInfo += stepHistory(project, nextStep.action_subject, test, nextStep.invocation, dummyStepInfo);
        }
        else if(nextStep.action_type == "search_source")
        {
            compiledInfo += stepSearchSource(project, nextStep.action_subject, nextStep.motivation, dummyStepInfo);
        }
        
        if(compiledInfo.length() > maxSize)
        {
            //TODO: Consider adding message in the compiledInfo that it has been cut
            break;
        }
    }
    
    return compiledInfo;
}

//TODO: Move each step action in its own function
bool Debugger::executeNextStep(CCodeProject* project, const TestDef& test)
{
    uint32_t stepIndex = m_previousSteps + uint32_t( m_trajectory.size() ) + 1;
    std::string stepIndexStr = std::to_string(stepIndex);
    std::string logDir = project->getProjDir() + "/logs/debug/" + test.name + "/step_" + stepIndexStr;
    boost_fs::create_directories(logDir);
    Client::getInstance().setContextLogDir(logDir);
    
    std::string application = project->getProjectName();
    
    std::string infoForCurrentStep;
    
    if(m_nextStep.action_type == "stop_unit_test")
    {
        DebugStep stepInfo;
        stepInfo.m_action = m_nextStep.action_type;
        stepInfo.m_subject = m_nextStep.action_subject;
        stepInfo.m_motivation = m_nextStep.motivation;
        m_trajectory.push_back(stepInfo);
        
        return false;
    }
    else if(m_nextStep.action_type == "run_test")
    {
        m_rawTrajectory.clear();
        
        RunAnalysis analysis;
        //Need to rebuild to ensure all recent changes from the trajectory so far are applieds
        bool compiled = project->buildBinary(true);
        if(m_system != "main")
        {
            compiled = compiled && project->buildUnitTest(m_system, true);
        }
        
        if(!compiled)
        {
            //We are unable continue debugging if the binary couldn't be compiled
            //TODO: Add a debug note to the trajectory that explains the problem
            criticalError("Could't build the executable from the latest source code!");
            return false;
        }
        
        runAnalysis(project, test, analysis, true);
        
        std::string stdoutLogPath;
        std::string traceLogPath;
        std::string execPath;
        
        stdoutLogPath = m_workingDirectory + "/stdout.log";
        traceLogPath = m_workingDirectory + "/trace.txt";
        
        if(m_system == "main")
        {
            execPath = project->getProjDir() + "/build/" + getPlatform() + "_test/main";
        }
        else
        {
            //Unit test
            std::string testWD = project->getProjDir() + "/build_instrumented/source/" + m_system + "/test";
            execPath = testWD + "/main";
        }
        
        if(boost_fs::exists(execPath) &&
           boost_fs::exists(stdoutLogPath) &&
           boost_fs::exists(traceLogPath))
        {
            m_hasValidBuild = true;
        }
        
        std::string commitHash;
        if(!m_commitMessage.empty())
        {
            m_commitMessage += "\n\nSTEP: " + stepIndexStr + " run test '" + m_nextStep.action_subject + "'\n\n";
            m_commitMessage += analysis.debug_notes;
            
            commitHash = project->commit(m_commitMessage);
            std::string currentCommit = project->currentCommit();
            
            if(currentCommit.empty() || currentCommit != commitHash)
            {
                criticalError("Invalid commit!. Investigate\n");
                return false;
            }
        }
        
        m_commitMessage.clear();
        
        bool done = analysis.debug_notes == "PASS";
        if(done)
        {
            DebugStep stepInfo;
            stepInfo.m_logSummary = analysis.log_summary;
            stepInfo.m_debugNotes = analysis.debug_notes;
            stepInfo.m_action = m_nextStep.action_type;
            stepInfo.m_subject = m_nextStep.action_subject;
            stepInfo.m_motivation = m_nextStep.motivation;
            stepInfo.m_commitHash = commitHash;
            m_trajectory.push_back(stepInfo);
            
            m_lastRunInfo = stepInfo.fullInfo();
            m_lastRunStep = stepIndex;
            
            project->commit("PASS: " + test.name);
            
            return false;
        }
        
        if(!analysis.m_function.empty())
        {
            infoForCurrentStep += "Providing information for the following function after analyzing application trace/log/sources:\n\n";
            
            infoForCurrentStep += getRequestedInfo(project, 0, 0, {},
                                                   {std::make_shared<std::string>(analysis.m_function)}, {}, {});
        }
        
        DebugStep stepInfo;
        
        stepInfo.m_logSummary = analysis.log_summary;
        stepInfo.m_debugNotes = analysis.debug_notes;
        stepInfo.m_action = "run_test";
        stepInfo.m_motivation = m_nextStep.motivation;
        stepInfo.m_subject = m_nextStep.action_subject;
        stepInfo.m_commitHash = commitHash;
        
        m_lastRunInfo = stepInfo.fullInfo();
        m_lastRunStep = stepIndex;
        
#ifdef DEBUGGER_INTERLEAVED_TRAJECTORY
        infoForCurrentStep = m_lastRunInfo + infoForCurrentStep;
#endif
        
        m_trajectory.push_back(stepInfo);
        
        //In case we have context accumulated during the run_test analysis
        //It isn't expected to keep anything but debugNotes after this step
        m_contextVisibility.clear();
    }
    else if(m_nextStep.action_type == "debug_function")
    {
        bool compiled = project->buildBinary(true);
        if(m_system != "main")
        {
            compiled = compiled && project->buildUnitTest(m_system, true);
        }
        
        if(!compiled)
        {
            //We are unable continue debugging if the binary couldn't be compiled
            //TODO: Add a debug note to the trajectory that explains the problem
            criticalError("Could't build the executable from the lates source code!");
            return false;
        }
        
        std::string functionName = m_nextStep.action_subject;
        std::string debugNotes;
        std::string logSummary;
        RunAnalysis analysis;
        if(checkFunctionExists(project, functionName, debugNotes))
        {
            runAnalysis(project, test, analysis, false);
            
            debugAnalysis(project, functionName, analysis, test);
            
            if(!analysis.m_function.empty())
            {
                infoForCurrentStep += "Providing information for the following function after analyzing trace logs:\n\n";
                
                infoForCurrentStep += getRequestedInfo(project, 0, 0, {},
                                                       {std::make_shared<std::string>(analysis.m_function)}, {}, {});
            }
            
            debugNotes = analysis.debug_notes;
            logSummary = analysis.log_summary;
        }
        else
        {
            infoForCurrentStep += "The function '" + functionName + "' doesn't exists. Consult with the list of available functions defined in the project:\n\n";
            infoForCurrentStep += getRequestedInfo(project, -1, 0, {}, {}, {}, {});
        }
        
        DebugStep stepInfo;
        stepInfo.m_logSummary = logSummary;
        stepInfo.m_debugNotes = debugNotes;
        stepInfo.m_action = "debug_function";
        stepInfo.m_subject = functionName;
        stepInfo.m_motivation = m_nextStep.motivation;
        if(!m_nextStep.breakpoints.empty())
        {
            stepInfo.m_motivation += "\n\nEvaluated breakpoints: \n";
            stepInfo.m_motivation += getBreakpointsInfo(false, false, functionName, m_nextStep.breakpoints);
        }
        
        m_lastRunInfo = stepInfo.fullInfo();
        
        m_trajectory.push_back(stepInfo);
        
        auto llmConfig = Client::getInstance().getLLMConfig(LLMRole::DIRECTOR);
        uint32_t maxInfoSize = (llmConfig->context_size * 1024) * CHARACTERS_PER_TOKEN * 0.7f;
        
        //This will also clear any context accumulated during the debug analysis
        m_compiledInfo = compileContext(project, test, maxInfoSize);
    }
    else if(m_nextStep.action_type == "fix_function")
    {
        std::string functionName = m_nextStep.action_subject;
        std::string debugNotes;
        std::string commitHash;
        if(checkFunctionExists(project, functionName, debugNotes))
        {
            m_hasValidBuild = false;
            
            std::set<std::string> funcSnapshotBefore = project->getNodeNames();
            
            std::string commitBeforeTheFix = project->currentCommit();
            if(commitBeforeTheFix.empty())
            {
                criticalError("Emptu current commit for function: " + functionName + "\n\n");
                return false;
            }
            
            std::string before;
            std::string implementation = fixFunction(project, test, functionName, before, debugNotes);
            
            std::string addedFunctions;
            if(!implementation.empty())
            {
                std::string asciiGraph = project->printNewNodes(functionName, funcSnapshotBefore);
                if(!asciiGraph.empty())
                {
                    addedFunctions += "\n\nHere is a subgraph including the newly implemented functions, ";
                    addedFunctions += "labeled with (New), called directly or indirectly by '" + functionName + "'.";
                    addedFunctions += " Note, the codebase has already been updated with those functions.\n\n";
                    addedFunctions += asciiGraph;
                    
                    infoForCurrentStep += addedFunctions;
                }
            }
            
            //After fixing the function we need to regenerate all sources,
            //compile functions that require update and build the binary
            project->generateSources();
            compile(project);
            
            bool compiled = project->buildBinary(true);
            if(m_system != "main")
            {
                compiled = compiled && project->buildUnitTest(m_system, true);
            }
            
            if(compiled)
            {
                std::string message = "STEP: " + stepIndexStr + " fix function '" + functionName + "'\n\n";
                message += m_nextStep.motivation;
                
                m_commitMessage = message;
                
                //commitHash = project->commit(m_commitMessage);
                
                debugNotes += "The function '" + functionName + "' has been fixed and the fix has been applied to the code base. ";
                //TODO: this needs testing, how much context window space it takes
                debugNotes += addedFunctions;
                
                m_lastFixStep = stepIndex;
            }
            else
            {
                debugNotes += "The function '" + functionName + "' couldn't be fixed due to compilation issues. Reverting changest to the previous stable state";
                project->revertToCommit(commitBeforeTheFix);//how safe is this, we still have the call graph in memomry
                std::string currentCommit = project->currentCommit();
                if(currentCommit.empty() || currentCommit != commitBeforeTheFix)
                {
                    criticalError("Unable to revert to stable commit. Investigate!\n");
                    return false;
                }
                
                project->generateSources();
                compile(project);
                
                compiled = project->buildBinary(true);
                if(m_system != "main")
                {
                    compiled = compiled && project->buildUnitTest(m_system, true);
                }
                
                if(!compiled)
                {
                    //We can't continue debugging if this fix is not successful
                    criticalError(debugNotes);
                    return false;
                }
            }
        }
        else
        {
            infoForCurrentStep += "The function '" + functionName + "' doesn't exists. Consult with the list of available functions defined in the project:\n\n";
            infoForCurrentStep += getRequestedInfo(project, -1, 0, {}, {}, {}, {});
        }
        
        DebugStep stepInfo;
        stepInfo.m_action = "fix_function";
        stepInfo.m_subject = functionName;
        stepInfo.m_debugNotes = debugNotes;
        stepInfo.m_motivation = m_nextStep.motivation;
        stepInfo.m_commitHash = commitHash;
        
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "function_info")
    {
        DebugStep stepInfo;
        infoForCurrentStep += stepFunctionInfo(project, m_nextStep.action_subject, m_nextStep.motivation, m_nextStep.invocation, m_nextStep.line_number, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "data_info")
    {
        DebugStep stepInfo;
        infoForCurrentStep += stepDataInfo(project, m_nextStep.action_subject, m_nextStep.motivation, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "file_info")
    {
        DebugStep stepInfo;
        infoForCurrentStep += stepFileInfo(project, m_nextStep.action_subject, m_nextStep.motivation, m_nextStep.line_number, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "functions_summary")
    {
        DebugStep stepInfo;
        
        std::string function = m_nextStep.action_subject;
        if(m_system != "main" &&
           (function.empty() || function == "main" || function == "none"))
        {
            function = m_system;
        }
        
        infoForCurrentStep += stepFunctionsSummary(project, function, m_nextStep.motivation, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "call_graph")
    {
        DebugStep stepInfo;
        
        std::string function = m_nextStep.action_subject;
        if(m_system != "main" &&
           (function.empty() || function == "main" || function == "none"))
        {
            function = m_system;
        }
        
        infoForCurrentStep += stepCallGraph(project, function, m_nextStep.motivation, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "log_info")
    {
        DebugStep stepInfo;
        infoForCurrentStep += stepLogInfo(project, m_nextStep.action_subject, m_nextStep.motivation, m_nextStep.invocation, m_nextStep.line_number, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "step_info")
    {
        DebugStep stepInfo;
        infoForCurrentStep += stepHistory(project, m_nextStep.motivation, test, m_nextStep.invocation, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "search_source")
    {
        DebugStep stepInfo;
        infoForCurrentStep += stepSearchSource(project, m_nextStep.action_subject, m_nextStep.motivation, stepInfo);
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "new_function")
    {
        std::string debugNotes;
        
        debugNotes = "The action 'new_function' is currently not available. ";
        debugNotes += "If you want a new function just select action 'fix_function' ";
        debugNotes += "for the fucntion that is supposed to call the new '";
        debugNotes += m_nextStep.action_subject + "' function";
        debugNotes += " and directly call the new function in the code of the fucntion ";
        debugNotes += "subject to the 'fix_function' action. We will deduce the arguments and return type of the '";
        debugNotes += m_nextStep.action_subject + "' function later based on it's usage in the code.\n\n";
        
        
        infoForCurrentStep += debugNotes;
        
        DebugStep stepInfo;
        stepInfo.m_debugNotes = debugNotes;
        stepInfo.m_action = m_nextStep.action_type;
        stepInfo.m_subject = m_nextStep.action_subject;
        stepInfo.m_motivation = m_nextStep.motivation;
        
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "refactor_data")
    {
        std::string debugNotes;
        
        debugNotes = "The action 'refactor_data' is currently not available. ";
        debugNotes += "If you want to refactor data type '" + m_nextStep.action_subject;
        debugNotes += "' by adding new members to it - just select action 'fix_function' ";
        debugNotes += "for the function that will access the new data members ";
        debugNotes += "and use the new data members in the function code.";
        debugNotes += " Then we are going to update '" + m_nextStep.action_subject;
        debugNotes += "' based on the usage in the code. ";
        debugNotes += "Note - it is not possible to delete existing members from '" + m_nextStep.action_subject + "'\n\n";
        
        infoForCurrentStep += debugNotes;
        
        DebugStep stepInfo;
        stepInfo.m_debugNotes = debugNotes;
        stepInfo.m_action = m_nextStep.action_type;
        stepInfo.m_subject = m_nextStep.action_subject;
        stepInfo.m_motivation = m_nextStep.motivation;
        
        m_trajectory.push_back(stepInfo);
    }
    else if(m_nextStep.action_type == "new_data")
    {
        std::string debugNotes;
        
        debugNotes = "The action 'new_data' is currently not available. ";
        debugNotes += "If you want a new data teype just select action 'fix_function' ";
        debugNotes += "for the first fucntion that is supposed to declare the new '";
        debugNotes += m_nextStep.action_subject + "' data type";
        debugNotes += " and directly declare the new data type in the implementation of the fucntion ";
        debugNotes += "subject to the 'fix_function' action. We will fully define '";
        debugNotes += m_nextStep.action_subject + "' data type later based on it's usage in the code.\n\n";
        
        infoForCurrentStep += debugNotes;
        
        DebugStep stepInfo;
        stepInfo.m_debugNotes = debugNotes;
        stepInfo.m_action = m_nextStep.action_type;
        stepInfo.m_subject = m_nextStep.action_subject;
        stepInfo.m_motivation = m_nextStep.motivation;
        
        m_trajectory.push_back(stepInfo);
    }
    else
    {
        std::cout << "WE MUST NOT BE HERE" << std::endl;
        infoForCurrentStep += "This action '" + m_nextStep.action_type + "' is currently not available. Select a new one\n\n";
    }
    //TODO: Other types of steps
    
    //Ensure we have the correct info incorporating the modification from the last debug step
    m_appInfo = getHighLevelAppInfo(project, m_system, PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH);
    
    if(infoForCurrentStep.empty())
    {
        m_rawTrajectory.push_back(std::make_pair("No new information was added for this step.", "user"));
    }
    else
    {
        m_rawTrajectory.push_back(std::make_pair(infoForCurrentStep, "user"));
    }
    
    std::string info;
    {
        if(!infoForCurrentStep.empty())
        {
            uint32_t compiledInfoLength = uint32_t(m_compiledInfo.length() + infoForCurrentStep.length());
            auto llmConfig = Client::getInstance().currentLLMConfig();
            uint32_t maxInfoSize = (llmConfig->context_size * 1024) * CHARACTERS_PER_TOKEN * 0.7f;

            if(compiledInfoLength > maxInfoSize)
            {
                //Rebuild the context
                uint32_t maxContextInfoSize = maxInfoSize - (uint32_t)infoForCurrentStep.length();
                m_compiledInfo = compileContext(project, test, maxContextInfoSize);
            }
            else
            {
                //m_requestedInfoChunks.push_back(infoForCurrentStep);
                
                //Accumulate the information for the current step to the context
                m_compiledInfo += infoForCurrentStep;
            }
        }
        
        //TODO: Consider changing the prolog and/or epilog messages here to better reflect what the m_compiledInfo is
        if(!m_compiledInfo.empty())
        {
            info += "\n//Information requested in previous steps starts here\n\n";
            info += m_compiledInfo;
            info += "\n//Information requested in previous steps ends here\n\n";
        }
    }
    
    if(!m_actionFeedback.empty())
    {
        m_trajectory.back().m_debugNotes += "\n[[Feedback on this step]]:\n";
        m_trajectory.back().m_debugNotes += m_actionFeedback;
    }
    m_actionFeedback.clear();

    std::string trajectory = getTrajectory(0, -1, true, true, true);
    
    //Enforces run_test step immediately after fix_function
    if(m_nextStep.action_type == "fix_function")
    {
        m_nextStep.action_type = "run_test";
        m_nextStep.motivation = "Run the test to verify the fix of '" + m_nextStep.action_subject;
        m_nextStep.motivation += "' and find what else needs fixes to successfully pass the test.";
        //Leave the action subject to be the fixed function!
    }
    else
    {
        m_commitMessage.clear();
        
        Cache cache;
        project->captureContext(std::string());
        
#ifdef DEBUGGER_INTERLEAVED_TRAJECTORY
        info.clear();
        m_appInfo.clear();
        trajectory.clear();
        pushTrajectory(project);
#else
        trajectory = "\n\n//Current progress log start" + trajectory;
        trajectory += "\n//Current progress log end\n\n";
#endif
        
        Prompt promptNextStep("NextStep.txt",{
                            {"app_info", m_appInfo},
                            {"application", application},
                            {"info", info},
                            {"trajectory", trajectory}
        });
        
        web::json::value object;
        
        web::json::value schema;
        setupSchema<NextDebugStep>(schema);
        
        std::string promptNextStepMsg = promptNextStep.str();
        project->captureContext(std::string());
        
        uint32_t sinceLastFix = stepIndex - m_lastFixStep;
        if(sinceLastFix >= DISCLOSE_STOP_STEPS_AFTER_FIX && m_system != "main")
        {
            promptNextStepMsg += "\n\nYou haven't edited the source (via fix_function action) for approximately ";
            promptNextStepMsg += std::to_string(sinceLastFix) + " steps. Since we are debugging a unit test (for function '";
            promptNextStepMsg += m_system + "') there is a chance the unit test itself is broken. ";
            promptNextStepMsg += "If you are sure there is enough evidence that ";
            promptNextStepMsg += "the unit test main function source is the only reason for the test to fail, ";
            promptNextStepMsg += "you can request a stop of the test by requesting next action 'stop_unit_test' ";
            promptNextStepMsg += "and I will try to fix the test sources and resume the debugging. ";
            promptNextStepMsg += "If you decide to do this, provide your justification in the motivation section.\n";
        }
        
        project->inference(cache, promptNextStepMsg, schema, object, false);
        
        m_nextStep.clear();
        m_nextStep.from_json(object);
        
#if REVIEW_GIT_COMMITS_BEFORE_FIX
        if(m_nextStep.action_type == "fix_function")
        {
            reviewGiHistoryForFix(project);
        }
#endif
        
        int validationAttempt = 1;
        int maxValidationAttempts = m_rewardHackingReview.empty() ? 8 : 3;
        int repeatedInvalidAttempts = 0;
        auto stepRetryKey = [](const NextDebugStep& step)
        {
            // Retry deduplication intentionally ignores motivation.
            // We only care whether the actionable step proposal repeats.
            std::string key = trim(step.action_type) + "\n";
            key += step.action_subject + "\n";
            key += std::to_string(step.invocation) + "\n";
            key += std::to_string(step.line_number);
            for(const auto& bp : step.breakpoints)
            {
                if(!bp) continue;
                key += "\n" + std::to_string(bp->source_line) + "\n";
                key += bp->condition + "\n";
                key += bp->expression;
            }
            return key;
        };
        
        std::string feedback = validateStep(project, test, validationAttempt);
        std::string invalidStepKey = feedback.empty() ? std::string() : stepRetryKey(m_nextStep);
        while(!feedback.empty() && validationAttempt < maxValidationAttempts)
        {
            // Keep rejected next-step proposals out of the retry prompt history.
            project->popContext();
            project->captureContext(std::string());
            
            object = web::json::value();
            project->inference(cache, feedback, schema, object, false);
            
            NextDebugStep prevStep = m_nextStep;
            
            m_nextStep.clear();
            m_nextStep.from_json(object);
            
            if(prevStep.action_type == "debug_function" &&
               m_nextStep.action_type == prevStep.action_type &&
               m_nextStep.action_subject == prevStep.action_subject)
            {
                //NOTE: Accumulate motivations to not miss something important in the context
                //std::string newMotivation = prevStep.motivation + "\n\n" + m_nextStep.motivation;
                //But since we only collect feedback from breakpoints evaluation, keep the initial motivation
                std::string newMotivation = prevStep.motivation;
                
                m_nextStep.motivation = newMotivation;
            }
            
            validationAttempt++;
            feedback = validateStep(project, test, validationAttempt);
            if(!feedback.empty())
            {
                std::string nextInvalidStepKey = stepRetryKey(m_nextStep);
                if(nextInvalidStepKey == invalidStepKey)
                {
                    repeatedInvalidAttempts++;
                    if(repeatedInvalidAttempts >= MAX_REPEATED_INVALID_NEXT_STEP_ATTEMPTS)
                    {
                        break;
                    }
                }
                else
                {
                    repeatedInvalidAttempts = 0;
                }
                
                invalidStepKey = nextInvalidStepKey;
            }
        }
        
        if(!feedback.empty()) //Probably the LLM insists for something
        {
            //Let's inject ground-truth signal from the test execution
            m_nextStep.clear();
            m_nextStep.action_type = "run_test";
            m_nextStep.motivation = "Run the test to inspect the results and perform post-execution system analysis.";
        }
        
        project->popContext();
        project->popContext();
    }
    
    m_nextStep.m_stepId = stepIndex + 1;
    
    std::string stepMessage = utility::conversions::to_utf8string(m_nextStep.to_json().serialize());
    m_rawTrajectory.push_back(std::make_pair(stepMessage, "assistant"));
     
    setStepHint(test);
    
    if(!m_nextStep.isInformationRequest())
    {
        m_compiledInfo.clear();
        m_contextVisibility.clear();
    }
    
    return true;
}

void Debugger::reviewGiHistoryForFix(CCodeProject* project)
{
    if(m_nextStep.action_type != "fix_function")
    {
        return;
    }
    
    std::string functionName = m_nextStep.action_subject;
    
    CCodeNode* ccNode = project->getNodeByName(functionName);
    
    if(!ccNode)
    {
        //This is serious issue, we must not be here!
        std::cout << "reviewGiHistoryForFix: function '" << functionName << "' desn't exist\n";
        return;
    }
    
    Client::getInstance().setLLM(LLMRole::DIRECTOR);
    
    std::string repoFolder = project->getProjDir() + "/dag";
    
    std::string functionSrcFile = ccNode->getNodeDirectory();
    functionSrcFile += "/" + ccNode->getName() + ".cpp";
    
    std::string gitHistory = project->getGitHistory(repoFolder, functionSrcFile, REVIEW_GIT_COMMITS_BEFORE_FIX);
    
    web::json::value object;
    
    web::json::value schema;
    setupSchema<NextDebugStep>(schema);
    
    Cache cache;
    project->captureContext(std::string());

    Prompt reviewFixStep("ReviewFixStep.txt",{
                        {"function", functionName},
                        {"git_history", gitHistory}
    });
    
    project->inference(cache, reviewFixStep, schema, object, false);
    
    m_nextStep.clear();
    m_nextStep.from_json(object);
    
    project->popContext();
    
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
}

bool Debugger::saveTestToDirectory(CCodeProject* project, const std::string& testJsonDir, const std::string& testDirectory, TestDef& test)
{
    if(boost_fs::exists(testDirectory))
    {
        boost_fs::remove_all(testDirectory);
    }
    
    boost_fs::create_directories(testDirectory);
    
    //We need to instrument the *.json.
    //Inserting the function name, for now the only purpose is during training
    {
        web::json::value json;
        loadJson(json, testJsonDir + "/test.json");
        json[U("function")] = web::json::value::string(utility::conversions::to_string_t(m_system));
        saveJson(json, testJsonDir + "/test.json");
    }
    
    boost_fs::copy(testJsonDir + "/test.json", testDirectory + "/test.json");
    
    const auto& inputFiles = test.getInputFiles();
    for(auto file : inputFiles)
    {
        std::string fileName = boost_fs::path(file).filename().string();
        if(boost_fs::exists(testJsonDir + "/" + fileName))
        {
            boost_fs::copy(testJsonDir + "/" + fileName, testDirectory + "/" + fileName);
        }
    }
    
    if(m_system != "main") //Are we in unit test
    {
        if(!boost_fs::exists(testJsonDir + "/main.cpp"))
        {
            return false;
        }
        
        boost_fs::remove(testDirectory + "/main.cpp");
        
        //Copy the main.cpp for the unit test
        boost_fs::copy(testJsonDir + "/main.cpp", testDirectory + "/main.cpp");
    }
    
    return true;
}

bool Debugger::deployToWorkingDirectory(CCodeProject* project, const std::string& testJsonDir, bool isPublic, TestDef& test)
{
    m_workingDirectory = project->getProjDir();
    m_workingDirectory += isPublic ? "/debug/wd_pub" : "/debug/wd_priv";
    
    if(!boost_fs::exists(testJsonDir + "/test.json"))
    {
        return false;
    }
    
    if(!test.load(testJsonDir + "/test.json"))
    {
        return false;
    }
    
    return saveTestToDirectory(project, testJsonDir, m_workingDirectory, test);
}

std::pair<bool, std::string> Debugger::debug(CCodeProject* project,
                     int stepsCount,
                     const std::string& system,
                     const std::string& testJsonPath,
                     const std::string& privateTestJsonPath,
                     const std::string& regressionTestJsonPath,
                     uint16_t debugPort)
{
    m_system = system;
    
    m_workingDirectory = testJsonPath;
    m_privateWorkingDirectory = privateTestJsonPath;
    m_debugPort = debugPort;
    
    TestDef test;
    if(!deployToWorkingDirectory(project, testJsonPath, true, test))
    {
        criticalError("Couldn't deply the test");
        return std::make_pair(false, std::string());
    }
    
    m_sdkPath = hen::getSysRoot();
    
    loadTrajectory(project, test);
    
    m_hasValidBuild = false;
    
    std::string testDirectory = Client::getInstance().getProjectDirectory() + "/debug/" + test.name + "/trajectory/test";
    if(!boost_fs::exists(testDirectory)) //if(m_trajectory.size() == 0)
    {
        saveTestToDirectory(project, testJsonPath, testDirectory, test);
    }
    
    m_appInfo = getHighLevelAppInfo(project, m_system, PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH);
    
    m_debugContext.reset();
    project->setActiveContext(&m_debugContext);
    
    std::string promptsDir = Client::getInstance().getEnvironmentDir();
    promptsDir += "/Debugger/Prompts";
    Prompt::addSearchPath(promptsDir);
    
    Prompt role("DebuggerRole.txt",{});
    project->pushMessage(role, "system", true);
    project->pushMessage(project->getProjectDescription(), "user", true);
    
    std::string hitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);
    Prompt workflow("Workflow.txt",{{"project", project->getProjectName()},
                                    {"hit_count", hitCount}});
    
    std::string workflowMsg = workflow.str();
    //Log description
    {
        Prompt logDesc("LogDescription.txt", {});
        
        workflowMsg += "\n" + logDesc.str();
    }
    
    //Trace description
    {
        std::string eventsHitCount = std::to_string(MAX_EVENTS_HIT_COUNT);
        std::string fullTraceHitCount = std::to_string(FULL_TRACE_MAX_EVENTS_HIT_COUNT);
        Prompt traceDesc("TraceDescription.txt", {
            {"events_hit_count", eventsHitCount},
            {"full_trace_hit_count", fullTraceHitCount}
        });
        
        workflowMsg += "\n" + traceDesc.str();
    }
    
    //Add source checklist requirements
    {
        workflowMsg += "\nPROJECT SOURCE CODE REQUIREMENTS\n\n";
        workflowMsg += project->source_checklist.prompt({{"function", "<function_placeholder_name>"}});
        workflowMsg += "\n";
    }
    
    {
        std::string hitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);
        //Push instructions for the breakpoints just before the next step
        Prompt breakpoints("Breakpoints.txt",{{"hit_count", hitCount}});
        workflowMsg += "\n";
        workflowMsg += breakpoints.str();
        workflowMsg += "\n";
    }
    
    project->pushMessage(workflowMsg, "user", true);
    
    std::string testDescription = getTestDescription(project, test, regressionTestJsonPath);
    project->pushMessage(testDescription, "user", true);
    
    {
        Prompt nextStepInstruct("NextStepInstructions.txt", {});
        
        std::string nextStepInstructMsg = nextStepInstruct.str();
        
        web::json::value schema;
        setupSchema<NextDebugStep>(schema);
        
        nextStepInstructMsg += project->getInstrumentationMessage(schema);
        
        project->pushMessage(nextStepInstructMsg, "user", true);
    }
    
    m_scriptsDirectory = Client::getInstance().getEnvironmentDir();
    m_scriptsDirectory += "/Debugger/Scripts";
    
    Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
    
    //Lets warm up the debug info cache here
    prebuildDebugInfo(project);
    
    m_step = 0;
    if(m_trajectory.empty())
    {
        m_nextStep.motivation = "first run";
        m_nextStep.action_type = "run_test";
        m_nextStep.m_stepId = 1;
        
        std::string stepMessage = utility::conversions::to_utf8string(m_nextStep.to_json().serialize());
        m_rawTrajectory.push_back(std::make_pair(stepMessage,"assistant"));
    }
    else
    {
        //TODO: Test if this is correct (Verify index in the trajectory on disk)
        m_nextStep.m_stepId = uint32_t(m_trajectory.size() + 1);
    }
    
    bool debugging = true;
    stepsCount = stepsCount > 0 ? stepsCount : MAX_DEBUGGING_STEPS;
    while(debugging && m_step < stepsCount)
    {
        debugging = executeNextStep(project, test);
        if(m_nextStep.action_type == "stop_unit_test")
        {
            break;
        }
        
        optimizeTrajectory(project, test);
        saveTrajectory(project, test);
        m_step++;
    }
    
    bool result = m_trajectory.size() > 0 && m_trajectory.back().m_debugNotes == "PASS";
    
    Prompt::clearSearchPaths();
    std::string promptsDirEnv = Client::getInstance().getEnvironmentDir() + "/Prompts";
    Prompt::addSearchPath(promptsDirEnv);
    
    Client::getInstance().unlockLLM();
    project->switchToCompileContext();
    
    resetTest();
    
    return std::make_pair(result, m_lastRunTestLog);
}

bool Debugger::debugPretest(CCodeProject* project,
                            const std::string& system,
                            const std::string& testJsonPath,
                            std::string& log)
{
    m_workingDirectory = testJsonPath;
    
    TestDef test;
    test.load(testJsonPath + "/test.json");
    
    std::stringstream ss;
    bool result = executeTestStep(ss, project, test.pretest, "pretest", true);
    
    resetTest();
    
    log = ss.str();
    return result;
}

// Returns the names of all local variables sorted by the order of their declaration.
std::vector<std::string> SourceScope::getLocalVariables() const {
    // Create a temporary vector of pairs: (declaration location, variable name)
    std::vector<std::pair<SourceLocation, std::string>> varList;
    for (const auto &entry : m_localVariables) {
        // entry.first is the key (variable name)
        // entry.second.m_declared is the location where the variable was declared.
        varList.push_back(std::make_pair(entry.second.m_declared, entry.first));
    }
    
    // Define a lambda to compare SourceLocation objects.
    auto cmpLocation = [](const std::pair<SourceLocation, std::string>& a,
                          const std::pair<SourceLocation, std::string>& b) -> bool {
        // If variables belong to different files, compare file names lexicographically.
        if (a.first.m_filePath != b.first.m_filePath)
            return a.first.m_filePath < b.first.m_filePath;
        if (a.first.m_lineNumber != b.first.m_lineNumber)
            return a.first.m_lineNumber < b.first.m_lineNumber;
        return a.first.m_column < b.first.m_column;
    };
    
    std::sort(varList.begin(), varList.end(), cmpLocation);
    
    // Extract and return just the variable names.
    std::vector<std::string> result;
    for (const auto& pair : varList) {
        result.push_back(pair.second);
    }
    return result;
}

// Returns the names of local variables that are declared at or before a given location.
std::vector<std::string> SourceScope::getLocalVariables(const SourceLocation& before) const {
    std::vector<std::pair<SourceLocation, std::string>> varList;
    for (const auto &entry : m_localVariables) {
        // Only include variables declared at or before the "before" location.
        if (entry.second.m_declared <= before) {
            varList.push_back(std::make_pair(entry.second.m_declared, entry.first));
        }
    }
    
    // Sort them by their declaration location, as above.
    auto cmpLocation = [](const std::pair<SourceLocation, std::string>& a,
                          const std::pair<SourceLocation, std::string>& b) -> bool {
        if (a.first.m_filePath != b.first.m_filePath)
            return a.first.m_filePath < b.first.m_filePath;
        if (a.first.m_lineNumber != b.first.m_lineNumber)
            return a.first.m_lineNumber < b.first.m_lineNumber;
        return a.first.m_column < b.first.m_column;
    };
    
    std::sort(varList.begin(), varList.end(), cmpLocation);
    
    std::vector<std::string> result;
    for (const auto &pair : varList) {
        result.push_back(pair.second);
    }
    return result;
}


bool FunctionDebugInfo::enterScope(CXCursor cursor)
{
    std::string cursorName = getCursorName(cursor);
    
    boost_fs::path filePath = getCursorFile(cursor);
    std::string fileName = filePath.filename().string();
    
    // build a new SourceScope from this cursor’s extent:
    // This is a '{ ... }' scope. Grab the start/end lines.
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXSourceLocation start = clang_getRangeStart(range);
    CXSourceLocation end   = clang_getRangeEnd(range);

    unsigned startOffset, startLine, startCol, endOffset, endLine, endCol;
    CXFile file;
    clang_getSpellingLocation(start, &file, &startLine, &startCol, &startOffset);
    clang_getSpellingLocation(end,   &file, &endLine,   &endCol,   &endOffset);
    
    CXCursorKind kind = clang_getCursorKind(cursor);
    
    SourceScope::Type scopeType;
    switch(kind)
    {
    case CXCursor_IfStmt:
        scopeType = SourceScope::IF;
        break;
    case CXCursor_ForStmt:
    case CXCursor_CXXForRangeStmt:
        scopeType = SourceScope::FOR;
        break;
    case CXCursor_WhileStmt:
        scopeType = SourceScope::WHILE;
        break;
    case CXCursor_DoStmt:
        scopeType = SourceScope::DO;
        break;
    case CXCursor_CompoundStmt:
        scopeType = SourceScope::COMPOUND;
        break;
    case CXCursor_SwitchStmt:
        scopeType = SourceScope::SWITCH;
        break;
    case CXCursor_LambdaExpr:
        scopeType = SourceScope::LAMBDA;
        break;
    default:
        scopeType = SourceScope::COMPOUND;
        assert(0);//must not be here!
    }
    
    if(startLine < endLine)
    {
        unsigned scopStartCol = scopeType == SourceScope::COMPOUND ? startCol+1 : startCol;
        unsigned scopStartOffset = scopeType == SourceScope::COMPOUND ? startOffset+1 : startOffset;
        
        SourceScope scope(scopeType,
                          cursorName,
                          SourceLocation(fileName, scopStartOffset, startLine, scopStartCol),
                          SourceLocation(fileName, endOffset-1, endLine, endCol-1));
        
        scope.m_captureAll = false;
        scope.m_capturedVariables.clear();

        // 5) if lambda, collect captures
        if (scopeType == SourceScope::LAMBDA) {
            //auto &lambdaScope = m_scopes.back();
            //auto &lambdaScope = scope;

            // → default-capture via tokens
            {
                CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
                CXToken *tokens = nullptr;
                unsigned nTok = 0;
                clang_tokenize(tu, range, &tokens, &nTok);

                bool inBrackets = false;
                for (unsigned i = 0; i < nTok; ++i) {
                    CXString sp = clang_getTokenSpelling(tu, tokens[i]);
                    std::string txt = getClangString(sp);

                    if (!inBrackets) {
                        if (txt == "[") {
                            inBrackets = true;
                        }
                    }
                    else {
                        // End of capture-list?
                        if (txt == "]") {
                            break;
                        }

                        // Capture-all by copy: “[= …]”
                        if (txt == "=") {
                            scope.m_captureAll = true;
                            break;
                        }

                        // Capture-all by ref: “[& …]” ONLY if & is standalone
                        if (txt == "&") {
                            // peek at the next token
                            if (i+1 < nTok) {
                                CXString sp2 = clang_getTokenSpelling(tu, tokens[i+1]);
                                std::string nxt = getClangString(sp2);
                                // only treat as default-capture if next is comma or closing bracket
                                if (nxt == "," || nxt == "]") {
                                    scope.m_captureAll = true;
                                    clang_disposeString(sp2);
                                    break;
                                }
                                //clang_disposeString(sp2);
                            }
                            // otherwise it’s “&identifier” → explicit capture, so skip
                        }
                    }

                    //clang_disposeString(sp);
                }

                clang_disposeTokens(tu, tokens, nTok);
            }

            // → explicit captures via AST
            clang_visitChildren(
                cursor,
                [](CXCursor c, CXCursor /*parent*/, CXClientData data) {
                    auto *ls = static_cast<SourceScope*>(data);
                    CXCursorKind ck = clang_getCursorKind(c);
                    if (ck == CXCursor_VariableRef) {
                        CXCursor var = clang_getCursorReferenced(c);
                        CXString name = clang_getCursorSpelling(var);
                        ls->m_capturedVariables.insert(getClangString(name));
                    }
                    
                    return CXChildVisit_Recurse;
                },
                &scope
            );
        }
        
        m_scopes.push_back(std::move(scope));
        m_scopeStack.push_back(m_scopes.size()-1);
        return true;
    }
    
    return false;
}

void FunctionDebugInfo::exitScope()
{
    m_scopeStack.pop_back();
}

// Returns a reference to the root scope (assumed to be the one with m_parentIndex == -1)
SourceScope& FunctionDebugInfo::getRootScope() {
    for (auto& scope : m_scopes) {
        if (scope.m_parentIndex == -1) {
            return scope;
        }
    }
    
    static SourceScope invalidScope;
    return invalidScope;
}

// Returns the parent scope of a given scope, or nullptr if the scope is the root.
SourceScope& FunctionDebugInfo::getParentScope(const SourceScope& scope) {
    if (scope.m_parentIndex == -1)
    {
        static SourceScope invalidScope;
        return invalidScope;
    }
    return m_scopes[scope.m_parentIndex];
}

SourceScope& FunctionDebugInfo::getParentScope(const SourceLocation& location) {
    int bestCandidateIndex = -1;
    int bestDepth = -1;
    
    // Loop through all scopes to find those that encompass the location.
    for (size_t i = 0; i < m_scopes.size(); ++i) {
        if (m_scopes[i].m_start <= location && m_scopes[i].m_end >= location) {
            // Compute nesting depth by traversing the parent chain.
            int depth = 0;
            int currentIndex = m_scopes[i].m_parentIndex;
            while (currentIndex != -1) {
                ++depth;
                currentIndex = m_scopes[currentIndex].m_parentIndex;
            }
            // Update if this scope is more deeply nested.
            if (depth > bestDepth) {
                bestDepth = depth;
                bestCandidateIndex = static_cast<int>(i);
            }
        }
    }
    
    if (bestCandidateIndex != -1) {
        return m_scopes[bestCandidateIndex];
    }
    
    static SourceScope invalidScope;
    return invalidScope;
}

// Build the hierarchy by sorting scopes by their starting location and determining parent-child relationships.
void FunctionDebugInfo::buildScopesHierarchy() {
    // Sort scopes by start location (line number and then column)
    std::sort(m_scopes.begin(), m_scopes.end(), [](const SourceScope& a, const SourceScope& b) {
        if (a.m_start.m_lineNumber == b.m_start.m_lineNumber)
            return a.m_start.m_column < b.m_start.m_column;
        return a.m_start.m_lineNumber < b.m_start.m_lineNumber;
    });
    
    // Reset all parent and children indices.
    for (auto& scope : m_scopes) {
        scope.m_parentIndex = -1;
        scope.m_childrenIndices.clear();
    }
    
    std::stack<int> scopeStack;
    // Process scopes in sorted order.
    for (size_t i = 0; i < m_scopes.size(); ++i) {
        // Pop from the stack until the scope at the top contains m_scopes[i].
        while (!scopeStack.empty() && !m_scopes[scopeStack.top()].contains(m_scopes[i])) {
            scopeStack.pop();
        }
        if (!scopeStack.empty()) {
            // The current scope's parent is the one on top of the stack.
            m_scopes[i].m_parentIndex = scopeStack.top();
            // Record that this scope is a child of the parent.
            m_scopes[scopeStack.top()].m_childrenIndices.push_back(static_cast<int>(i));
        }
        scopeStack.push(static_cast<int>(i));
    }
}

// A private helper to get the index of a scope pointer within m_scopes.
// Returns -1 if the scope is not found.
int FunctionDebugInfo::getScopeIndex(const SourceScope& scope) const {
    for (size_t i = 0; i < m_scopes.size(); ++i) {
        if (m_scopes[i] == scope)
            return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::pair<int,std::string>>
FunctionDebugInfo::getLiveVariables(const SourceLocation& before) const
{
    // 1) Find innermost scope
    int innermost = -1, bestDepth = -1;
    for (int i = 0, n = int(m_scopes.size()); i < n; ++i) {
        auto const &scp = m_scopes[i];
        if (!scp.isValid() || !scp.contains(before))
            continue;
        int depth = 0, p = scp.m_parentIndex;
        while (p != -1) { ++depth; p = m_scopes[p].m_parentIndex; }
        if (depth > bestDepth) { bestDepth = depth; innermost = i; }
    }
    if (innermost < 0 && !m_scopes.empty())
        innermost = 0;  // fallback

    // 2) Build chain innermost→…→root
    std::vector<int> chain;
    for (int cur = innermost; cur != -1;
         cur = m_scopes[cur].m_parentIndex)
        chain.push_back(cur);

    // 3) Gather variables
    std::unordered_set<std::string> added;
    std::vector<std::tuple<SourceLocation,int,std::string>> raw;

    bool sawLambda        = false;
    bool lambdaCaptureAll = false;
    std::set<std::string> lambdaExplicit;

    for (int idx = 0; idx < (int)chain.size(); ++idx) {
        int scpIdx = chain[idx];
        auto const &scp = m_scopes[scpIdx];

        // If it *is* a lambda, record its capture policy:
        if (scp.m_type == SourceScope::LAMBDA) {
            sawLambda        = true;
            lambdaCaptureAll = scp.m_captureAll;
            lambdaExplicit   = scp.m_capturedVariables;
        }

        // Collect *all* locals from the *first* (innermost) scope:
        // this ensures y (and parameters) show up inside a lambda body.
        bool isInnermost = (idx == 0);

        for (auto const &ent : scp.m_localVariables) {
            auto const &var = ent.second;
            if (!(var.m_live <= before))
                continue;            // declared after the point
            if (added.count(var.m_name))
                continue;            // shadowed

            // if this is not the innermost scope, and we've already
            // seen an *explicit*-only lambda, filter by its list:
            if (!isInnermost && sawLambda && !lambdaCaptureAll) {
                if (!lambdaExplicit.count(var.m_name))
                    continue;
            }

            added.insert(var.m_name);
            raw.emplace_back(var.m_live, scpIdx, var.m_name);
        }
    }

    // 4) Sort by declaration order
    std::sort(raw.begin(), raw.end(),
      [](auto const &A, auto const &B) {
        auto const &a = std::get<0>(A);
        auto const &b = std::get<0>(B);
        if (a.m_lineNumber != b.m_lineNumber)
            return a.m_lineNumber < b.m_lineNumber;
        return a.m_column < b.m_column;
      }
    );

    // 5) Build result pairs
    std::vector<std::pair<int,std::string>> result;
    result.reserve(raw.size());
    for (auto const &t : raw)
        result.emplace_back(std::get<1>(t), std::get<2>(t));
    return result;
}

// Helper: Convert SourceScope::Type enum to a string.
static std::string scopeTypeToString(SourceScope::Type type) {
    switch (type) {
        case SourceScope::FUNCTION: return "FUNCTION";
        case SourceScope::CALL:     return "CALL";
        case SourceScope::FOR:      return "FOR";
        case SourceScope::WHILE:    return "WHILE";
        case SourceScope::DO:       return "DO";
        case SourceScope::IF:       return "IF";
        case SourceScope::SWITCH:   return "SWITCH";
        case SourceScope::COMPOUND: return "COMPOUND";
        default:                    return "UNKNOWN";
    }
}

std::string FunctionDebugInfo::getFormatedDebugInfo() const {
    std::ostringstream oss;
    
    // Print basic function information.
    oss << "---------- Function Debug Info ----------\n";
    oss << "Name: " << m_name << "\n";
    oss << "File: " << m_fileName << "\n";
    oss << "Return Type: " << m_returnType << "\n\n";
    
    // Define a helper lambda that recursively formats a scope.
    std::function<std::string(int, int)> formatScope = [this, &formatScope](int scopeIndex, int indentLevel) -> std::string {
        std::ostringstream soss;
        const SourceScope &scope = m_scopes[scopeIndex];
        
        std::string indent = std::string(indentLevel * 2, ' ');
        
        // Print scope header.
        soss << indent << "Scope [" << scopeIndex << "] " << scopeTypeToString(scope.m_type) << ", ";
        soss << "source range: "
             << scope.m_start.m_lineNumber << ":" << scope.m_start.m_column
             << " -> " << scope.m_end.m_lineNumber << ":" << scope.m_end.m_column << "\n";
        
        // Increase indentation for events.
        indentLevel++;
        indent = std::string(indentLevel * 2, ' ');
        
        // Helper structure for events.
        struct DebugEvent {
            SourceLocation loc;
            std::string text;
            bool isChildScope;
            int childScopeIndex;  // valid only if isChildScope is true.
        };
        std::vector<DebugEvent> events;
        
        // Add all variable declaration events from this scope.
        for (const auto &entry : scope.m_localVariables) {
            const SourceVariable &var = entry.second;
            DebugEvent de;
            de.loc = var.m_declared;
            de.text = "Variable declaration: " + var.m_type + " " + var.m_name + "; source location: " +
                      std::to_string(var.m_declared.m_lineNumber) + ":" +
                      std::to_string(var.m_declared.m_column);
            de.isChildScope = false;
            de.childScopeIndex = -1;
            events.push_back(de);
        }
        
        // Define a helper lambda to check if an event location actually belongs to this scope
        // (i.e. it is not already part of one of its immediate children).
        auto eventBelongsToThisScope = [this, &scope](const SourceLocation &loc) -> bool {
            for (int childIndex : scope.m_childrenIndices) {
                if (m_scopes[childIndex].contains(loc))
                    return false;
            }
            return true;
        };
        
        // Add function call events whose "before" location falls in this scope (and not in a child scope).
        for (const auto &entry : m_calls) {
            const SourceFunctionCall &call = entry.second;
            if (scope.contains(call.m_before) && eventBelongsToThisScope(call.m_before)) {
                DebugEvent de;
                de.loc = call.m_before;
                de.text = "Function call: " + call.m_functionName +
                          " (expression: " + call.m_expression + ") at " +
                          std::to_string(call.m_before.m_lineNumber) + ":" +
                          std::to_string(call.m_before.m_column);
                de.isChildScope = false;
                de.childScopeIndex = -1;
                events.push_back(de);
            }
        }
        
        // Add return statement events that occur within this scope (and not in a child scope).
        for (const auto &ret : m_returns) {
            if (scope.contains(ret.m_start) && eventBelongsToThisScope(ret.m_start)) {
                DebugEvent de;
                de.loc = ret.m_start;
                de.text = "Return statement: " + ret.m_expression + " at " +
                          std::to_string(ret.m_start.m_lineNumber) + ":" +
                          std::to_string(ret.m_start.m_column);
                de.isChildScope = false;
                de.childScopeIndex = -1;
                events.push_back(de);
            }
        }
        
        // Add a sub-scope event for each child scope.
        for (int childIndex : scope.m_childrenIndices) {
            const SourceScope &child = m_scopes[childIndex];
            DebugEvent de;
            de.loc = child.m_start;
            de.text = ""; // label will be printed in the recursive call
            de.isChildScope = true;
            de.childScopeIndex = childIndex;
            events.push_back(de);
        }
        
        // Sort events by source location: first by line then by column.
        std::sort(events.begin(), events.end(), [](const DebugEvent &a, const DebugEvent &b) {
            if (a.loc.m_lineNumber == b.loc.m_lineNumber)
                return a.loc.m_column < b.loc.m_column;
            return a.loc.m_lineNumber < b.loc.m_lineNumber;
        });
        
        // Print the events.
        for (const auto &ev : events) {
            if (ev.isChildScope) {
                // Recursively print the child scope.
                soss << formatScope(ev.childScopeIndex, indentLevel);
            } else {
                soss << indent << ev.text << "\n";
            }
        }
        
        return soss.str();
    };
    
    // Print all root scopes (i.e. scopes with no parent).
    oss << "Scopes:\n";
    for (size_t i = 0; i < m_scopes.size(); ++i) {
        if (m_scopes[i].m_parentIndex == -1) {
            oss << formatScope(static_cast<int>(i), 0);
        }
    }
    
    oss << "-----------------------------------------\n";
    return oss.str();
}

struct ChildExprInfo {
    CXSourceRange exprRange;
    bool foundExpr;
};

CXChildVisitResult findChildExprVisitor(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    ChildExprInfo* info = static_cast<ChildExprInfo*>(client_data);

    if (!info->foundExpr) {
        info->exprRange = clang_getCursorExtent(cursor);
        info->foundExpr = true;  // we take only the first child expression
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

static bool starts_with(const char* s, const char* prefix) {
    return s && prefix && std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

static std::string getReturnExpression(CXCursor retStmt)
{
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(retStmt);
    CXSourceRange sr = clang_getCursorExtent(retStmt);

    CXToken* tok = nullptr;
    unsigned n = 0;
    clang_tokenize(tu, sr, &tok, &n);

    if (n <= 1) { // plain 'return;'
        clang_disposeTokens(tu, tok, n);
        return {};
    }

    // Skip 'return' (index 0). Trim trailing ';' if present.
    unsigned start = 1;
    unsigned end   = n;
    {
        CXString last = clang_getTokenSpelling(tu, tok[n - 1]);
        const char* ls = clang_getCString(last);
        if (ls && std::strcmp(ls, ";") == 0) end = n - 1;
        clang_disposeString(last);
    }

    auto token_text = [&](unsigned idx) -> std::string {
        CXString s = clang_getTokenSpelling(tu, tok[idx]);
        const char* c = clang_getCString(s);
        std::string out = c ? c : "";
        clang_disposeString(s);
        return out;
    };

    auto is_wordlike = [&](unsigned idx) {
        CXTokenKind k = clang_getTokenKind(tok[idx]);
        if (k == CXToken_Identifier || k == CXToken_Keyword || k == CXToken_Literal) return true;
        if (k == CXToken_Punctuation) {
            auto t = token_text(idx);
            return (t == "~");
        }
        // IMPORTANT: comments are not wordlike
        return false;
    };

    std::string expr;
    expr.reserve(128);

    for (unsigned i = start; i < end; ++i) {
        CXTokenKind k = clang_getTokenKind(tok[i]);
        std::string cur = token_text(i);

        if (k == CXToken_Comment) {
            // Ensure line comments can't swallow what follows.
            // Option 1: preserve as line comment but force newline.
            if (!expr.empty() && expr.back() != ' ' && expr.back() != '\n')
                expr.push_back(' ');
            expr += cur;
            if (starts_with(cur.c_str(), "//"))
                expr.push_back('\n'); // CRITICAL
            else
                expr.push_back(' ');  // mild spacing after /*...*/
            continue;
        }

        if (!expr.empty()) {
            // space between two wordlike tokens (e.g., "new" + "NodeLocation")
            // but don't do this if previous token was a comment (handled above with newline/space)
            if (is_wordlike(i - 1) && is_wordlike(i)) expr.push_back(' ');

            // optional nicety: add a space after commas
            std::string prev = token_text(i - 1);
            if (prev == ",") expr.push_back(' ');
        }

        expr += cur;
    }

    clang_disposeTokens(tu, tok, n);
    return expr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. helper: move an offset forward to (and including) the next ‘;’
// ─────────────────────────────────────────────────────────────────────────────
static unsigned advanceToSemicolon(CXTranslationUnit tu,
                                   CXFile file,
                                   unsigned fromOffset)
{
    size_t fileSize = 0;
    const char *buf = clang_getFileContents(tu, file, &fileSize);

    while (fromOffset < fileSize && buf[fromOffset] != ';')
        ++fromOffset;
    if (fromOffset < fileSize) ++fromOffset;          // include the ‘;’
    return fromOffset;
}

static CXChildVisitResult visitorScope(CXCursor cursor,
                                  CXCursor parent,
                                  CXClientData userData)
{
    FunctionDebugInfo& context = *((FunctionDebugInfo*)userData);
    CXCursorKind kind = clang_getCursorKind(cursor);
    
    boost_fs::path filePath = getCursorFile(cursor);
    std::string fileName = filePath.filename().string();
    
    if(context.m_fileName != fileName) {
        //TODO: Shoul I CXChildVisit_Continue instead ?!?
        //return CXChildVisit_Recurse;
        
        //Requires testing!
        return CXChildVisit_Continue;
    }
    
    std::string cursorName = getCursorName(cursor);
    std::string parentCursorName = getCursorName(cursor);
    std::string cursorSource = getCursorSource(cursor);

    switch (kind) {
    case CXCursor_FunctionDecl: {
        // Identify if this is a definition (not just a forward declaration).
        if (clang_isCursorDefinition(cursor)) {
            
            CXType returnType = clang_getCursorResultType(cursor);
            CXString returnTypeSpelling = clang_getTypeSpelling(returnType);
            context.m_returnType = getClangString(returnTypeSpelling);
            
            // Recursively visit children of the function’s body.
            clang_visitChildren(cursor, visitorScope, &context);

            //clang_disposeString(fnName);
            return CXChildVisit_Continue;
        } else
        {
            
        }
        break;
    }
    case CXCursor_ParmDecl: {
        
        if(!cursorName.empty())
        {
            // 1) Grab the semantic parent
            CXCursor semanticParent = clang_getCursorSemanticParent(cursor);
            CXCursorKind parentKind = clang_getCursorKind(semanticParent);
            std::string functionName;
            if (parentKind == CXCursor_FunctionDecl ||
                parentKind == CXCursor_CXXMethod    ||
                parentKind == CXCursor_Constructor  ||
                parentKind == CXCursor_Destructor   ||
                parentKind == CXCursor_FunctionTemplate)
            {
                CXString fnSpelling = clang_getCursorSpelling(semanticParent);
                functionName = getClangString(fnSpelling);
            }
            
            if(context.m_name == functionName)
            {
                CXType type = clang_getCursorType(cursor);
                
                CXString typeSpelling = clang_getTypeSpelling(type);
                std::string typeStr = getClangString(typeSpelling);
                
                SourceVariable variable;
                
                if (type.kind == CXType_IncompleteArray ||
                    type.kind == CXType_VariableArray ||
                    type.kind == CXType_ConstantArray)
                {
                    variable.m_arraySize = -1;//Unrestricted for now
                    
                    // If it's a constant array, retrieve the size
                    if (type.kind == CXType_ConstantArray) {
                        variable.m_arraySize = (int)clang_getArraySize(type);
                    }
                    
                    CXType elemType = clang_getArrayElementType(type);
                    CXString elemSpelling = clang_getTypeSpelling(elemType);
                    std::string elemTypeStr = getClangString(elemSpelling);
                    typeStr = elemTypeStr;
                }
                
                CXSourceRange range = clang_getCursorExtent(cursor);
                CXSourceLocation start = clang_getRangeStart(range);
                unsigned startLine, startCol, startOffset;
                CXFile file;
                clang_getSpellingLocation(start, &file, &startLine, &startCol, &startOffset);
                
                variable.m_declared = SourceLocation(fileName, startOffset, startLine, startCol);
                variable.m_live = variable.m_declared;
                variable.m_name = cursorName;
                variable.m_type = typeStr;
                
                context.m_arguments[cursorName] = variable;
            }
        }
        
        break;
    }
    case CXCursor_VarDecl: {

        if (!context.m_scopes.empty() && !cursorName.empty()) {

            // ── 0. Create the record up front so we can fill it progressively ──
            SourceVariable variable;

            // ── 1. Obtain the nominal type that appears in source ─────────────
            CXType type = clang_getCursorType(cursor);

            // ── 2. Resolve 'auto' / 'decltype(auto)' to its canonical type ────
            CXType canonType  = clang_getCanonicalType(type);   // may differ
            bool   deduced    = (type.kind == CXType_Auto) &&
                                canonType.kind != CXType_Auto &&
                                canonType.kind != CXType_Unexposed;
            
            if(deduced)
            {
                CXString spellingType = clang_getTypeSpelling(type);
                std::string typeStr = getClangString(spellingType);
                
                CXString spellingCanon = clang_getTypeSpelling(canonType);
                std::string canonStr = getClangString(spellingCanon);
                
                std::cout << "type: " << typeStr << " deduced type: " << canonStr << std::endl << std::endl;
                //Let's skip deduced types for now. I can probably generate tracers with template arguments for deduced types later
                //or introduce more restricted checks for 'auto'
                break;
            }

            CXType effectiveType = deduced ? canonType : type;

            // ── 3. Handle array special‑cases and capture element type ────────
            CXType elemType = effectiveType; // may be replaced below
            if (effectiveType.kind == CXType_IncompleteArray ||
                effectiveType.kind == CXType_VariableArray   ||
                effectiveType.kind == CXType_ConstantArray) {

                if (effectiveType.kind == CXType_ConstantArray) {
                    variable.m_arraySize = static_cast<int>(clang_getArraySize(effectiveType));
                }
                elemType = clang_getArrayElementType(effectiveType);
            }

            // ── 4. Turn the final type into readable text ─────────────────────
            CXString spelling = clang_getTypeSpelling(elemType);
            std::string typeStr = getClangString(spelling);

            // ── 5. Source‑location bookkeeping (unchanged) ────────────────────
            CXSourceRange range = clang_getCursorExtent(cursor);
            CXSourceLocation start = clang_getRangeStart(range);
            CXSourceLocation end   = clang_getRangeEnd(range);

            unsigned startLine, startCol, startOffset;
            CXFile startFile;
            clang_getSpellingLocation(start, &startFile,
                                      &startLine, &startCol, &startOffset);

            unsigned endLine, endCol, endOffset;
            CXFile endFile;
            clang_getSpellingLocation(end, &endFile,
                                      &endLine, &endCol, &endOffset);

            // ── 6. Finish populating SourceVariable ───────────────────────────
            variable.m_declared = SourceLocation(fileName, startOffset, startLine, startCol);
            variable.m_live     = SourceLocation(fileName, endOffset,   endLine,   endCol);
            variable.m_name     = cursorName;
            variable.m_type     = typeStr;

            // ── 7. Store in current scope ─────────────────────────────────────
            if (!context.m_scopeStack.empty()) {
                auto idx = context.m_scopeStack.back();
                context.m_scopes[idx].m_localVariables[variable.m_name] = variable;
            }
        }

        break;
    }
    case CXCursor_IfStmt:
    case CXCursor_ForStmt:
    case CXCursor_WhileStmt:
    case CXCursor_DoStmt:
    case CXCursor_SwitchStmt:
    case CXCursor_CompoundStmt:
    case CXCursor_CXXForRangeStmt:
    case CXCursor_LambdaExpr:
    {
        if(context.enterScope(cursor))
        {
            clang_visitChildren(cursor, visitorScope, &context);
            context.exitScope();
        }
        else
        {
            clang_visitChildren(cursor, visitorScope, &context);
        }
        
        return CXChildVisit_Continue;
    }
    case CXCursor_CallExpr: {
        auto& nodeMap = Client::getInstance().project()->nodeMap();
        auto it = nodeMap.find(context.m_name);
        if(it != nodeMap.end())
        {
            // Get the referenced function declaration
            CXCursor referenced = clang_getCursorReferenced(cursor);
            std::string callName = getClangString(clang_getCursorSpelling(referenced));
            
            CCodeNode* ccNode = (CCodeNode*)it->second;
            
            auto itCall = std::find_if(ccNode->m_calls.items.begin(),
                                       ccNode->m_calls.items.end(),
                                   [&](const std::shared_ptr<FunctionItem>& funct_ptr) {
                return funct_ptr->func_name == callName;
            });
            
            if(itCall != ccNode->m_calls.items.end())
            {
                //TODO: Add function call information here
                // Get the full extent (range) of the function call expression
                CXSourceRange range = clang_getCursorExtent(cursor);
                
                CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
                std::string expression = getSourceForRange(tu, range);

                // Get start and end source locations
                CXSourceLocation startLoc = clang_getRangeStart(range);
                CXSourceLocation endLoc = clang_getRangeEnd(range);

                unsigned startOffset, startLine, startCol, endOffset, endLine, endCol;

                // Retrieve exact positions
                clang_getFileLocation(startLoc, nullptr, &startLine, &startCol, &startOffset);
                clang_getFileLocation(endLoc, nullptr, &endLine, &endCol, &endOffset);
                
                //std::map<std::string, SourceFunctionCall> m_calls;
                
                SourceFunctionCall call;
                call.m_functionName = callName;
                call.m_expression = expression;
                call.m_before = SourceLocation(fileName, startOffset, startLine, startCol);
                call.m_after = SourceLocation(fileName, endOffset, startLine, startCol);
                context.m_calls[callName] = call;
                return CXChildVisit_Continue;
            }
        }
        
        break;
    }
    case CXCursor_ReturnStmt: {
        CXTranslationUnit tu   = clang_Cursor_getTranslationUnit(cursor);
        CXSourceRange     full = clang_getCursorExtent(cursor);

        CXSourceLocation  sLoc = clang_getRangeStart(full);
        CXSourceLocation  eLoc = clang_getRangeEnd(full);

        CXFile file;
        unsigned sLine, sCol, sOff;
        unsigned eLine, eCol, eOff;

        clang_getFileLocation(sLoc, &file, &sLine, &sCol, &sOff);
        clang_getFileLocation(eLoc, &file, &eLine, &eCol, &eOff);

        // ---- extend eOff to swallow the terminating semicolon ------------------
        eOff = advanceToSemicolon(tu, file, eOff);

        SourceReturn info;
        info.m_start      = SourceLocation(fileName, sOff, sLine, sCol);
        info.m_end        = SourceLocation(fileName, eOff, eLine, eCol);
        info.m_expression = getReturnExpression(cursor);
        
        if(context.m_returnType != "void" && info.m_expression.empty())
        {
            std::cout << "ERROR: Empty return expression for statement: " << cursorSource << std::endl;
        }

        context.m_returns.push_back(std::move(info));
        return CXChildVisit_Continue;
    }
    default:
        break;
    }

    // By default, keep traversing
    return CXChildVisit_Recurse;
}

std::string Debugger::getOriginalSource(CCodeProject* project, const std::string& filePath) const
{
    //Find the path to the original source file.
    //If we are in the instrumented build we need to check under build_backup
    std::string originalFilePath = filePath;
    std::string buildBackupDir = project->getProjDir() + "/build_backup";
    if(boost_fs::exists(buildBackupDir))
    {
        std::string buildDir = project->getProjDir() + "/build";
        // Replaces all occurrences of substring B in A with substring C.
        //boost::replace_all(A, B, C);
        boost::replace_all(originalFilePath, buildDir, buildBackupDir);
    }
    
    return originalFilePath;
}

std::shared_ptr<FunctionDebugInfo> Debugger::getFunctionDebugInfo(CCodeProject* project, const std::string& functionName) const
{
    auto funcDebugInfo = std::make_shared<FunctionDebugInfo>();
    funcDebugInfo->m_name = functionName;
    
    std::string platform = getPlatform() + "_test";
    uint32_t options = CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG;
    auto info = project->getCompilationInfo(functionName, platform, options);
    
    std::string sourceFilePath = getOriginalSource(project, info->m_sourceFilePath);
    if(!boost_fs::exists(sourceFilePath))
    {
        return nullptr;
    }
    
    std::size_t hash = getFileHash(sourceFilePath);
    
    {
        std::lock_guard<std::mutex> lock(m_dbgInfoMutex);
        
        //Check the cache
        auto it = m_dbgInfoCache.find(functionName);
        if(it != m_dbgInfoCache.end())
        {
            if(it->second->m_hash == hash)
            {
                return it->second;
            }
        }
    }
    
    std::cout << "Building debug info for function: " << functionName << std::endl;
    
    // 1. Start building a std::vector<std::string> of flags
    std::vector<std::string> flags;

    // 2. Add each include directory as "-I/dir"
    for (const auto& incDir : info->m_includeDirs) {
        flags.push_back("-I" + incDir);
    }

    // 3. Split the m_options string by whitespace (if it has something like "-std=c++17 -DDEBUG=1")
    //    and push each token into flags.
    if (!info->m_options.empty()) {
        
        std::vector<std::string> optionTokens;
        // 'boost::is_any_of(" \t\n\r")' splits on spaces, tabs, newlines, etc.
        // 'boost::token_compress_on' merges consecutive delimiters into one.
        boost::split(optionTokens, info->m_options, boost::is_any_of(" \t\n\r"), boost::token_compress_on);
        
        for (auto& opt : optionTokens) {
            flags.push_back(opt);
        }
    }
    
    std::string resourceDir = getClangResourceDir();
    std::string cxxInclude  = getCppInclude();
    std::string cxxIncludeOpt = "-I" + cxxInclude;

    // 4. Convert flags to an array of const char*
    std::vector<const char*> clangArgs;
    clangArgs.reserve(flags.size());
    for (auto& f : flags) {
        clangArgs.push_back(f.c_str());
    }
    
    clangArgs.push_back("-D_LIBCPP_HAS_NO_WIDE_CHARACTERS");
    clangArgs.push_back("-isysroot");
    clangArgs.push_back(m_sdkPath.c_str());
    
    clangArgs.push_back("-resource-dir");
    clangArgs.push_back(resourceDir.c_str()); // ← critical for stdarg.h, stdint.h, intrinsics, etc.
    clangArgs.push_back(cxxIncludeOpt.c_str()); // ← libc++ headers
    
    CXIndex index = clang_createIndex(0, 0);

    unsigned tuOptions =  CXTranslationUnit_DetailedPreprocessingRecord   // <-- NEW
                          | CXTranslationUnit_KeepGoing;
    
    // 5. Call clang_parseTranslationUnit
    CXTranslationUnit tu = clang_parseTranslationUnit(
        index,
        sourceFilePath.c_str(),    // The source file path
        clangArgs.data(),                  // array of const char*
        static_cast<int>(clangArgs.size()),// number of arguments
        nullptr,
        0,
        tuOptions
    );

    // Optional: check for errors, handle accordingly
    if (!tu) {
        std::cerr << "Failed to parse Translation Unit: "
                  << sourceFilePath << std::endl;
    }
    
    //std::cout << printDiagnostics(tu, false);
    
    // 6) Get the cursor for the root of the AST
    CXCursor rootCursor = clang_getTranslationUnitCursor(tu);
    
    //std::vector<SourceScope> scopes;
    //std::vector<SourceLocation> returns;
    boost_fs::path filePath = sourceFilePath;
    funcDebugInfo->m_fileName = filePath.filename().string();//filePath.string();
    
    //ScopeContext context(funcDebugInfo->m_scopes, funcDebugInfo->m_returns, filePath.filename().string());
    // 7) Traverse the AST
    clang_visitChildren(rootCursor, visitorScope, funcDebugInfo.get());
    
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    
    funcDebugInfo->buildScopesHierarchy();
    funcDebugInfo->m_hash = hash;
    //std::cout << funcDebugInfo->getFormatedDebugInfo() << std::endl;
    
    //Update the cache
    {
        std::lock_guard<std::mutex> lock(m_dbgInfoMutex);
        m_dbgInfoCache[functionName] = funcDebugInfo;
    }
    
    return funcDebugInfo;
}

std::vector<SourceScope> Debugger::getScopes(CCodeProject* project, const std::string& functionName)
{
    auto funcDebugInfor = getFunctionDebugInfo(project, functionName);
    return funcDebugInfor->m_scopes;
}

bool Debugger::checkFunctionExists(CCodeProject* project, const std::string& functionName, std::string& debugNode) const
{
    if(project->nodeMap().find(functionName) == project->nodeMap().end())
    {
        debugNode = "The requested function '" + functionName + "' doesn't exist in the project.\n";
        return false;
    }
    
    return true;
}

void Debugger::criticalError(const std::string& debugNotes)
{
    DebugStep stepInfo;
    
    stepInfo.m_debugNotes = "Critical error: " + debugNotes + "\n";
    stepInfo.m_debugNotes += "Unable to continue debugging!\n";
    stepInfo.m_action = m_nextStep.action_type;
    stepInfo.m_subject = m_nextStep.action_subject;
    
    m_trajectory.push_back(stepInfo);
}

std::string Debugger::generateTraceCode(CCodeProject* project, const std::string& functionName, uint32_t traceOptions, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const
{
    auto debugInfo = getFunctionDebugInfo(project, functionName);
    if(!debugInfo)
    {
        return std::string();
    }
        
    const SourceScope& rootScope = debugInfo->getRootScope();
    
    std::string platform = getPlatform() + "_test";
    uint32_t options = CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG;
    auto info = project->getCompilationInfo(functionName, platform, options);
    
    bool debugThis = traceOptions & (uint32_t)TraceOptions::TRACE_BREAKPOINT;
    
    std::string argList;
    std::string argCallList;
    std::string printArgs;
    for(const auto& arg : debugInfo->m_arguments)
    {
        //Build arguments list
        {
            if(!argList.empty()) {
                argList += ", ";
            }
            argList += arg.second.m_type + " " + arg.first;
            //0 it not an array, -1 unspecified []
            if(arg.second.m_arraySize != 0)
            {
                argList += "[";
                if(arg.second.m_arraySize > 0)
                {
                    argList += std::to_string(arg.second.m_arraySize);
                }
                argList += "]";
            }
        }
        
        {
            if(!argCallList.empty()) {
                argCallList += ", ";
            }
            argCallList += arg.first;
        }
        
        //build print commands list
        printArgs += "            trace::log << \"" + arg.first + " = \" << make_printer(" + arg.first + ", trace::cfg) << std::endl;\n";
    }
    
    CCodeNode* ccNode = project->getNodeByName(functionName);
    
    std::string hitCountStr = std::to_string(MAX_EVENTS_HIT_COUNT);
    
    std::string enableEnter = debugThis ? "true" : ENABLE_TRACE_ON_FUNCTION_ENTER_STR;
    std::string enableExit = debugThis ? "true" : ENABLE_TRACE_ON_FUNCTION_EXIT_STR;
    
    //ENTER function definition
    std::string enterFunction;
    {
        std::string hitCount = "inline int _hitcount_" + functionName + "(bool increase) {\n";
        hitCount += "    static int bpId = 0;\n";
        hitCount += "    return (bpId += (increase ? 1 : 0));\n}\n\n";
        enterFunction += hitCount;
        
        enterFunction += "inline void _" + functionName + "_enter(" + argList + ") {\n";
        if(functionName == m_system)
        {
            enterFunction += "    trace::start();\n";
        }
        enterFunction += "    int bpEnterId = _hitcount_" + functionName + "(true);\n";
        enterFunction += "    trace::pushFrame(\"" + functionName + "\", bpEnterId);\n";
        enterFunction += "    {\n";
        enterFunction += "        if(" + enableEnter + "){\n";
        enterFunction += "            trace::cfg.maxMembers = " + std::to_string(TRACE_MIN_MEMBERS) + ";\n";
        enterFunction += "            trace::cfg.maxElements = " + std::to_string(TRACE_MIN_ELEMENTS) + ";\n";
        enterFunction += "            trace::cfg.maxDepth = " + std::to_string(TRACE_SYS_DEPTH) + ";\n";
        enterFunction += "            trace::log << \"<[PUSH (arguments):\" << trace::getStack() << \"]>\" << std::endl;\n";
        enterFunction += printArgs;
        enterFunction += "            trace::log << \"<[POP]>\" << std::endl;\n";
        enterFunction += "            trace::cfg.maxMembers = " + std::to_string(TRACE_MAX_MEMBERS) + ";\n";
        enterFunction += "            trace::cfg.maxElements = " + std::to_string(TRACE_MAX_ELEMENTS) + ";\n";
        enterFunction += "            trace::cfg.maxDepth = " + std::to_string(TRACE_MAX_DEPTH) + ";\n";
        enterFunction += "            trace::log << \"<[PUSH (entry):\" << trace::getStack() << \"]>\" << std::endl;\n";
        enterFunction += printArgs;
        enterFunction += "            trace::log << \"<[POP]>\" << std::endl;\n";
        enterFunction += "        }\n";
        enterFunction += "    }\n";
        enterFunction += "}\n";
    }
    
    //EXIT function definition
    std::string exitFunction;
    {
        exitFunction += "inline void _" + functionName + "_exit(" + argList + ") {\n";
        exitFunction += "    int bpExitId = _hitcount_" + functionName + "(false);\n";
        exitFunction += "    {\n";
        exitFunction += "        if(" + enableExit + ") {\n";
        exitFunction += "            if(trace::getStackDepth() <= " + std::to_string(PRINT_MAX_FUNCTIONS_DEPTH) + " && bpExitId == 1) {\n";
        exitFunction += "                trace::cfg.maxMembers = " + std::to_string(TRACE_SYS_MEMBERS) + ";\n";
        exitFunction += "                trace::cfg.maxElements = " + std::to_string(TRACE_SYS_ELEMENTS) + ";\n";
        exitFunction += "                trace::cfg.maxDepth = " + std::to_string(TRACE_SYS_DEPTH) + ";\n";
        exitFunction += "            }\n";
        exitFunction += "            trace::log << \"<[PUSH (exit):\" << trace::getStack() << \"]>\" << std::endl;\n";
        exitFunction += printArgs;
        exitFunction += "            trace::log << \"<[POP]>\" << std::endl;\n";
        exitFunction += "            trace::cfg.maxMembers = " + std::to_string(TRACE_MAX_MEMBERS) + ";\n";
        exitFunction += "            trace::cfg.maxElements = " + std::to_string(TRACE_MAX_ELEMENTS) + ";\n";
        exitFunction += "            trace::cfg.maxDepth = " + std::to_string(TRACE_MAX_DEPTH) + ";\n";
        exitFunction += "        }\n";
        exitFunction += "    }\n";
        exitFunction += "    trace::popFrame(\"" + functionName + "\", bpExitId);\n";
        exitFunction += "}\n";
    }
    
    std::string returnFunction;
    
    if(!debugInfo->m_returns.empty())
    {
        if(debugInfo->m_returnType != "void")
        {
            if(!argList.empty())
            {
                returnFunction += "inline void _" + functionName + "_return(" + debugInfo->m_returnType + " retval, " + argList + ") {\n";
            }
            else
            {
                returnFunction += "inline void _" + functionName + "_return(" + debugInfo->m_returnType + " retval) {\n";
            }
        }
        else
        {
            returnFunction += "inline void _" + functionName + "_return(" + argList + ") {\n";
        }
        
        returnFunction += "    int bpReturnId = _hitcount_" + functionName + "(false);\n";
        returnFunction += "    {\n";
        returnFunction += "        if(" + enableExit + "){\n";
        returnFunction += "              if(trace::getStackDepth() <= " + std::to_string(PRINT_MAX_FUNCTIONS_DEPTH) + " && bpReturnId == 1) {\n";
        returnFunction += "                  trace::cfg.maxMembers = " + std::to_string(TRACE_SYS_MEMBERS) + ";\n";
        returnFunction += "                  trace::cfg.maxElements = " + std::to_string(TRACE_SYS_ELEMENTS) + ";\n";
        returnFunction += "                  trace::cfg.maxDepth = " + std::to_string(TRACE_SYS_DEPTH) + ";\n";
        returnFunction += "              }\n";
        returnFunction += "            trace::log << \"<[PUSH (return):\" << trace::getStack() << \"]>\" << std::endl;\n";
        returnFunction += printArgs;
        if(debugInfo->m_returnType != "void")
        {
            returnFunction += "            trace::log << \"[[return value]] = \" << make_printer(retval, trace::cfg) << std::endl;\n";
        }
        returnFunction += "            trace::log << \"<[POP]>\" << std::endl;\n";
        returnFunction += "              trace::cfg.maxMembers = " + std::to_string(TRACE_MAX_MEMBERS) + ";\n";
        returnFunction += "              trace::cfg.maxElements = " + std::to_string(TRACE_MAX_ELEMENTS) + ";\n";
        returnFunction += "              trace::cfg.maxDepth = " + std::to_string(TRACE_MAX_DEPTH) + ";\n";
        returnFunction += "        }\n";
        returnFunction += "    }\n";
        returnFunction += "    trace::popFrame(\"" + functionName + "\", bpReturnId);\n";
        returnFunction += "}\n";
    }
    
    std::string printers;
    
    printers += enterFunction;
    printers += "\n\n";
    printers += exitFunction;
    printers += "\n\n";
    printers += returnFunction;
    printers += "\n\n";
    
    //TODO: Or if it calls the function being debugged
    std::set<std::pair<int,int>> tracePoints;
    if(traceOptions & (uint32_t)TraceOptions::TRACE_CALL)
    {
        for(auto call : debugInfo->m_calls)
        {
            std::string liveVarParams, liveVarArgs;
            printers += generateTracePoint(project,
                                           debugInfo,
                                           call.second.m_before.m_lineNumber,
                                           call.second.m_before.m_column,
                                           liveVarParams,
                                           liveVarArgs, true);
            
            tracePoints.insert(std::make_pair(call.second.m_before.m_lineNumber,
                                            call.second.m_before.m_column));
            
            std::string location = "_" + std::to_string(call.second.m_before.m_lineNumber);
            location += "_" + std::to_string(call.second.m_before.m_column);
            printers += "#define _CALL_" + functionName + location + "(a)";
            printers += " (_" + functionName + "_trace" + location;
            printers += "(" + liveVarArgs + "), a)";
            printers += "\n\n";
        }
    }
    
    if(debugThis)
    {
        for(auto bp : customBreakpoints)
        {
            std::string sourceFilePath = getOriginalSource(project, info->m_sourceFilePath);
            
            int bpColumn = getFirstColumn(sourceFilePath, bp->source_line);
            
            //TODO: Since the trace point is attached to the breakpint should I use the BP max hit count for this trace point
            std::string liveVarParams, liveVarArgs;
            
            //Don't define the tracepoint if it is already defined
            auto itLocation = tracePoints.find(std::make_pair(bp->source_line, bpColumn));
            if(itLocation == tracePoints.end())
            {
                printers += generateTracePoint(project,
                                               debugInfo,
                                               bp->source_line,
                                               bpColumn,
                                               liveVarParams,
                                               liveVarArgs, false);
            }
            
            std::string bpCode = assembleBreakpointCode(project, functionName, bp);
            std::string location = "_" + std::to_string(bp->source_line);
            printers += "#define _BREAKPOINT_" + functionName + location + " " + bpCode + "\n\n";
        }
    }
    
    //TODO: Consider trace points for the scopes. For ex each scope up to the bp location?!
    
    printers += "#define _ENTER_" + functionName + " _" + functionName + "_enter(" + argCallList + ")\n\n";
    printers += "#define _EXIT_" + functionName + " _" + functionName + "_exit(" + argCallList + ")\n\n";
    
    if(!debugInfo->m_returns.empty())
    {
        if(debugInfo->m_returnType != "void")
        {
            if(!argCallList.empty())
            {
                printers += "#define _RETURN_" + functionName + " _" + functionName + "_return(__retval_, " + argCallList + ")\n\n";
            }
            else
            {
                printers += "#define _RETURN_" + functionName + " _" + functionName + "_return(__retval_)\n\n";
            }
        }
        else
        {
            printers += "#define _RETURN_" + functionName + " _" + functionName + "_return(" + argCallList + ")\n\n";
        }
    }
    
    return printers;
}

void Debugger::generateTraceSources(CCodeProject* project, const std::string& debugFunctionName, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const
{
    std::string traceSource;
    
    for(auto func : project->nodeMap()) {
        uint32_t traceOptions = 0;
        if(func.first == debugFunctionName)
        {
            traceOptions |= (uint32_t)TraceOptions::TRACE_BREAKPOINT;
            traceOptions |= (uint32_t)TraceOptions::TRACE_CALL;
        }
        traceSource += generateTraceCode(project, func.first, traceOptions, customBreakpoints);
    }
    
    std::string path = project->getProjDir() + "/build_instrumented";
    boost_fs::create_directories(path);
    std::ofstream sourceFile(path + "/trace_printers.h");
    
    sourceFile << "#pragma once" << std::endl;
    sourceFile << traceSource;
    sourceFile.close();
}

std::pair<std::string, std::string>  Debugger::getLiveVariables(CCodeProject* project,
                                                                std::shared_ptr<FunctionDebugInfo> dbgInfo,
                                                                int line, int column,
                                                                std::vector<std::string>& paramList,
                                                                std::vector<std::string>& argList) const
{
    // Create a temporary SourceLocation for the given line and column.
    SourceLocation liveLoc(dbgInfo->m_fileName, 0, line, column);
    // Assume getLiveVariables returns a vector of pairs: {scope_index, variable_name}
    auto liveVars = dbgInfo->getLiveVariables(liveLoc);
    
    std::string paramListCsv;
    std::string argListCsv;
    
    for (const auto &pair : liveVars)
    {
        int scopeIndex = pair.first;
        const std::string &varName = pair.second;
        std::string varType;
        
        // Find the variable in the local variables of the scope.
        const SourceScope &scope = dbgInfo->m_scopes[scopeIndex];
        auto it = scope.m_localVariables.find(varName);
        if (it != scope.m_localVariables.end())
        {
            varType = it->second.m_type;
            
            //If some of the application defined types not visible in the project skip this live variable
            //We might have a local struct/enum definition that is not passed as argument to any function
            //or is not a member of any data type - in other words is not registered in the project
            //In this case the _trace point functions won't be able to compile with this data type
            {
                std::set<std::string> appDefinedTypes = project->getAppTypes(varType);
                for(auto appType : appDefinedTypes)
                {
                    std::string owningPath;
                    if(!project->findData(appType, owningPath))
                    {
                        continue;
                    }
                }
            }
            
            // If it's declared as an array, append the array size if specified.
            if (it->second.m_arraySize != 0)
            {
                varType += "[";
                if (it->second.m_arraySize > 0)
                {
                    varType += std::to_string(it->second.m_arraySize);
                }
                varType += "]";
            }
        }
        // Fallback type if none was determined.
        if (varType.empty())
        {
            varType = "auto";
        }
        
        // Construct the function parameter string, e.g., "int index"
        std::string param = varType + " " + varName;
        
        paramList.push_back(param);
        // And simply store the variable name for later use.
        argList.push_back(varName);
        
        if(!paramListCsv.empty())
        {
            paramListCsv += ", ";
        }
        paramListCsv += param;
        
        if(!argListCsv.empty())
        {
            argListCsv += ", ";
        }
        argListCsv += varName;
    }
    
    return std::make_pair(paramListCsv, argListCsv);
}

std::string Debugger::generateTracePoint(CCodeProject* project,
                                         std::shared_ptr<FunctionDebugInfo> dbgInfo,
                                         int line,
                                         int column,
                                         std::string& liveVarParams,
                                         std::string& liveVarArgs,
                                         bool attachToFunction) const
{
    // Keep these for compatibility with any other generator pieces that consume them.
    std::string functionName = dbgInfo->m_name;
    std::string frameInfo    = getFrameSnippet(project, dbgInfo->m_name, line, column, 2);

    std::vector<std::string> paramList;
    std::vector<std::string> argList;

    auto csvLists = getLiveVariables(project, dbgInfo, line, column, paramList, argList);
    liveVarParams = csvLists.first;   // (not used in the signature anymore, but preserved)
    liveVarArgs   = csvLists.second;  // used by the call-site generator

    // Select the cap for how many times we emit this TP.
    std::string eventsHitCount;
    if (attachToFunction) {
        eventsHitCount = std::to_string(MAX_EVENTS_HIT_COUNT);
    } else {
        // attached to a breakpoint
        eventsHitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);
    }

    // Helper to turn arbitrary variable names into valid template-parameter suffixes.
    // ASCII-only to avoid <cctype> and locale surprises.
    auto sanitize_ident = [](std::string s, std::size_t uniquifier) {
        auto is_alnum = [](char c) {
            return (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9');
        };
        for (char& c : s) {
            if (!is_alnum(c) && c != '_') c = '_';
        }
        if (s.empty() || (s[0] >= '0' && s[0] <= '9')) s.insert(s.begin(), '_');
        if (uniquifier) s += "_" + std::to_string(uniquifier);
        return s;
    };

    // Build unique raw-string delimiter that cannot appear inside frameInfo.
    const std::string raw_delim = "DELIM_" + std::to_string(line) + "_" + std::to_string(column);

    // Precompute sanitized template param names and ensure uniqueness if needed.
    std::vector<std::string> tparams;
    tparams.reserve(argList.size());
    std::unordered_map<std::string, std::size_t> seen; // to avoid collisions
    for (const auto& name : argList) {
        auto base = sanitize_ident(name, 0);
        auto it   = seen.find(base);
        if (it == seen.end()) {
            seen.emplace(base, 1);
            tparams.emplace_back(base);
        } else {
            auto n = it->second++;
            tparams.emplace_back(sanitize_ident(base, n));
        }
    }

    std::ostringstream oss;

    // Emit template header only if we actually have parameters to deduce.
    if (!tparams.empty()) {
        oss << "template<";
        for (std::size_t i = 0; i < tparams.size(); ++i) {
            if (i) oss << ", ";
            oss << "typename T_" << tparams[i];
        }
        oss << ">\n";
    }

    // Function signature. NOTE: keep the leading '_' to stay compatible with existing call sites.
    oss << "inline void _" << functionName << "_trace_"
        << std::to_string(line) << "_" << std::to_string(column) << "(";

    // Parameters as const references (no copies of non-copyables like std::ofstream).
    for (std::size_t i = 0; i < argList.size(); ++i) {
        if (i) oss << ", ";
        oss << "const T_" << tparams[i] << "& " << argList[i];
    }
    oss << ") {\n";

    // Body — unchanged semantics, just with the safer signature above.
    if (attachToFunction) {
        oss << "    int bpFrameId = _hitcount_" << functionName << "(false);\n";
        oss << "    static int tpId = 0;\n";
        oss << "    if (bpFrameId <= " << eventsHitCount << " && tpId++ < " << eventsHitCount << ") {\n";
    } else {
        //oss << "    static int tpId = 0;\n";
        //oss << "    if (tpId++ < " << eventsHitCount << ") {\n";
        
        // Attached to breakpoint - BP condition already gates execution
        oss << "    {\n";
    }

    oss << "        trace::log << \"<[PUSH (trace ln " << line << " col " << column
        << "):\" << trace::getStack() << \"]>\" << std::endl;\n";
    
    if (attachToFunction) {
        oss << "        trace::log << \"function : '" << functionName
            << "' at location " << line << ":" << column
            << " trace point hitCount=\" << tpId << std::endl;\n";
    } else {
        oss << "        trace::log << \"function : '" << functionName
            << "' at location " << line << ":" << column
            << "\" << std::endl;\n";
    }

    // Use a unique raw-string delimiter to avoid accidental terminators inside frameInfo.
    oss << "        trace::log << R\"" << raw_delim << "(" << frameInfo << ")"
        << raw_delim << "\" << std::endl;\n\n";

    if (!argList.empty()) {
        oss << "        trace::log << \"Live variables :\" << std::endl;\n";
        for (const auto& varName : argList) {
            oss << "        trace::log << \"" << varName
                << " = \" << make_printer(" << varName << ", trace::cfg) << std::endl;\n";
        }
    } else {
        oss << "        trace::log << \"No live variables\" << std::endl;\n";
    }

    if(attachToFunction)//Only when attached to a function. BP handles that messaging
    {
        oss << "        if (tpId == " << eventsHitCount << ") {\n";
        oss << "            tpId++;\n"; // ensure the 'max hitCount' message isn't reprinted
        oss << "            trace::log << \"Reached the maximum hitCount for this trace point. Next occurrences won't be traced\";\n";
        oss << "            trace::log << std::endl;\n";
        oss << "        }\n";
    }
    oss << "        trace::log << \"<[POP]>\" << std::endl;\n";
    oss << "    }\n";
    oss << "}\n\n";

    return oss.str();
}

std::pair<std::string, std::string> Debugger::generateTracePointDeclCall(CCodeProject* project,
                                                               std::shared_ptr<FunctionDebugInfo> dbgInfo,
                                                               int line, int column) const
{
    std::vector<std::string> paramList;
    std::vector<std::string> argList;
    
    auto csv = getLiveVariables(project, dbgInfo, line, column, paramList, argList);
    
    std::string location = "_" + std::to_string(line) + "_" + std::to_string(column);
    std::string functionName = "_" + dbgInfo->m_name + "_trace" + location + "(";
    
    std::string decl = "inline void " + functionName + csv.first + ")";
    std::string call = functionName + csv.second + ")";
    return std::make_pair(decl, call);
}

std::string Debugger::getFrameSnippet(CCodeProject* project, const std::string& functionName, int line, int column, int scopeLinesCount) const
{
    CCodeNode* ccNode = project->getNodeByName(functionName);
    if(!ccNode)
    {
        std::cout << "Debugger::getFrameSnippet - " << "couldn't find node '" << functionName << "'" << std::endl;
        //Somethig is wrong!
        return std::string();
    }
    
    std::string filepath;
    
    std::string projDir = project->getProjDir();
    std::string backupDir = projDir + "/build_backup";
    std::string dagPath = ccNode->getDAGPath("/");
    
    if(boost_fs::exists(backupDir))
    {
        filepath = backupDir + "/" + dagPath + "/" + functionName + ".cpp";
    }
    else
    {
        filepath = projDir + "/build/" + dagPath + "/" + functionName + ".cpp";
    }
    
    // Open the file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        // Could not open the source file, return an empty snippet
        return "";
    }
    
    // Read all lines from the file into a vector (lines are 1-indexed logically)
    std::vector<std::string> allLines;
    std::string currentLine;
    while (std::getline(file, currentLine)) {
        allLines.push_back(currentLine);
    }
    file.close();
    
    // If the file is empty, nothing to show.
    if (allLines.empty()) {
        return "";
    }
    
    // Compute the range of lines to include.
    // The source lines are 1-indexed, so convert to proper indices later.
    int totalLines = static_cast<int>(allLines.size());
    int firstLine = std::max(1, line - scopeLinesCount);
    int lastLine  = std::min(totalLines, line + scopeLinesCount);
    
    // Determine the field width for the line numbers.
    // We base the width on the maximum line number in the snippet.
    int width = static_cast<int>(std::to_string(lastLine).length());
    
    std::ostringstream oss;
    
    // Iterate over the lines in the selected range
    for (int currentLineNumber = firstLine; currentLineNumber <= lastLine; ++currentLineNumber) {
        // Choose the marker: target line gets an arrow marker "-> " otherwise "   "
        std::string marker = (currentLineNumber == line) ? "-> " : "   ";
        
        // Output the marker, then the line number right-justified in a field of 'width', then a space, then the actual file line.
        oss << marker
            << std::setw(width) << currentLineNumber
            << allLines[currentLineNumber - 1] << "\n";
        
        // For the target line, add a line that prints a caret '^' underneath the specific column.
        if (currentLineNumber == line) {
            // The printed line begins with the marker ("-> " or "   "), then the padded line number, then a space.
            // The total number of characters printed before the actual text of the file is:
            //int prefixLength = 3 + width + 1;
            int prefixLength = 3 + width;
            // The caret itself should be placed so that its position corresponds to the (column) within the file text
            // (column is 1-based, so we add column-1 spaces to the prefix).
            oss << std::string(prefixLength + (column - 1), ' ') << "^\n";
        }
    }
    
    return oss.str();
}

std::string Debugger::getInstrumentedPath(CCodeProject* project, const std::string& path) const
{
    std::string projDir = project->getProjDir();
    std::string buildDir = projDir + "/build";
    std::string instrumentedDir = projDir + "/build_instrumented";
    
    // Replaces all occurrences of substring B in A with substring C.
    //boost::replace_all(A, B, C);
    std::string result = path;
    boost::replace_all(result, buildDir, instrumentedDir);
    return result;
    
}

//TODO: Implement this multithreaded in the CCodeProject
bool Debugger::compileFunction(CCodeProject* project, const std::string& functionName) const
{
    const CCodeNode* node = nullptr;
    auto it = project->nodeMap().find(functionName);
    if(it != project->nodeMap().end())
    {
        node = (const CCodeNode*)it->second;
    }
    
    if(!node)
    {
        return false;
    }
    
    //Enforce compilation for the tested/debugged/fixed function
    if(m_nextStep.action_subject != functionName && node->objectIsValid())
    {
        node->restoreCachedObject();
        return true;
    }
    
    if(!node->m_defined)
    {
        std::cout << "Attempt to compile node: '" << node->getName() << "' that is not defined yet" << std::endl;
        return false;
    }
    
    std::cout << "Compile function: " << functionName << std::endl;
    
    std::string platform = getPlatform() + "_test";
    std::string compileCL = node->compileCommand(platform, CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG);
    std::string output;
    return node->compileSource(compileCL, output);
}

bool Debugger::compile(CCodeProject* project) const
{
    // 0.  Preparatory work done by the calling thread
    project->compileCommonHeader(CCodeNode::BUILD_PRINT_TEST |
                                 CCodeNode::BUILD_DEBUG);

    //------------------------------------------------------------------
    // 1.  Determine how many jobs we will launch
    //------------------------------------------------------------------
    const auto& nodes     = project->nodeMap();          // e.g. std::map<…>
    const std::size_t jobCount = nodes.size();
    if (jobCount == 0)               // nothing to do
        return project->buildBinary(true);

    //------------------------------------------------------------------
    // 2.  Shared synchronisation objects
    //------------------------------------------------------------------
    auto remaining = std::make_shared<std::atomic<std::size_t>>(jobCount);
    auto done      = std::make_shared<std::promise<void>>();
    auto fut       = done->get_future();

    //------------------------------------------------------------------
    // 3.  Post every compile task
    //------------------------------------------------------------------
    for (auto const& kv : nodes)
    {
        const std::string key = kv.first;      // capture by value
        boost::asio::post(m_threadPool,
            [this, project, key, remaining, done]()
            {
                compileFunction(project, key);

                // last task to finish fulfils the promise
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                    done->set_value();
            });
    }

    //------------------------------------------------------------------
    // 4.  Block until all compile tasks are complete
    //------------------------------------------------------------------
    fut.wait();

    //------------------------------------------------------------------
    // 5.  Now it is safe to link / build the binary
    //------------------------------------------------------------------
    return project->buildBinary(true);
}

class TextEdit {
public:
    // Construct with the original string.
    explicit TextEdit(const std::string& original)
        : original_(original)
    { }

    // Inserts a string at the given absolute offset (0-based).
    // The insertion is queued and will be applied when flush() is called.
    void insertAtOffset(std::size_t offset, const std::string& text) {
        // For an insertion, the "length" to remove is zero.
        edits_.push_back({ EditType::Insert, offset, 0, text });
    }
    
    // Inserts `text` at the start of the given 1-based line,
    // but after any leading whitespace on that line.
    void insertAtLine(int line, const std::string& text) {
        
        std::size_t insertPos = getFirstCharacterOffset(original_, line);
        // 3) Delegate to your existing offset-based insert.
        insertAtOffset(insertPos, text);
    }

    // Replaces text in the range [start, end) with the given string.
    // The replacement is queued and will be applied when flush() is called.
    void replace(std::size_t start, std::size_t end, const std::string& text) {
        if (start > end) {
            // Handle error: you may throw an exception, swap the values, or ignore.
            return;
        }
        // The "length" to remove is the size of the range.
        edits_.push_back({ EditType::Replace, start, end - start, text });
    }

    // Applies all queued edits (insertions and replacements) to the original string
    // and returns the modified string.
    // After flushing, the list of queued edits is cleared.
    std::string flush() {
        std::string result = original_;
        // Sort edits in descending order by offset.
        // This ensures that later edits do not affect the positions of earlier ones.
        std::sort(edits_.begin(), edits_.end(),
            [](const Edit& a, const Edit& b) {
                return a.offset > b.offset;
            });

        for (const auto& edit : edits_) {
            // Ensure the offset is within the current result string.
            if (edit.offset > result.size())
                continue;
                
            if (edit.type == EditType::Insert) {
                result.insert(edit.offset, edit.text);
            } else if (edit.type == EditType::Replace) {
                // For a replacement, check that the deletion range is within bounds.
                result.replace(edit.offset, edit.length, edit.text);
            }
        }
        // Clear the queued edits.
        edits_.clear();
        return result;
    }

private:
    enum class EditType {
        Insert,
        Replace
    };

    // Structure representing an edit operation.
    struct Edit {
        EditType type;
        std::size_t offset;  // For insert: position to insert.
                             // For replace: starting position of replacement.
        std::size_t length;  // For insert: always 0.
                             // For replace: number of characters to remove.
        std::string text;    // Text to insert or replace with.
    };

    std::string original_;
    std::vector<Edit> edits_;
};

std::string Debugger::assembleBreakpointCode(CCodeProject* project, const std::string& functionName, std::shared_ptr<Breakpoint> bp) const
{
    std::string bpCondition = bp->getInstrumentedConditionCode();
    std::string bpExpression = bp->getInstrumentedExpressionCode();
    
    std::string bpHitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);
    
    //The trace point will have its own hitCounter and we can use it for the warning message
    //but we still need to track the hitCount for the overall breakpoint
    std::string hitCountCnd = "[](){static int _i_=0; return _i_++ < " + bpHitCount + ";}()";
    std::string condition = "((" + bpCondition + ") && " + hitCountCnd + ")";
    
    std::string expression = "trace::hitBP(\"" + functionName + "\", " + std::to_string(bp->source_line);
    expression += ", R\"DELIM(" + bp->getConditionCode() + ")DELIM\", R\"DELIM(" + bp->getExpressionCode() + ")DELIM\");";
    expression += bpExpression;
    
    auto dbgInfo = getFunctionDebugInfo(project, functionName);
    
    std::string platform = getPlatform() + "_test";
    uint32_t options = CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG;
    auto info = project->getCompilationInfo(functionName, platform, options);
    
    std::string sourceFilePath = getOriginalSource(project, info->m_sourceFilePath);
    
    int column = getFirstColumn(sourceFilePath, bp->source_line);
    
    auto tpCall = generateTracePointDeclCall(project, dbgInfo, bp->source_line, column);
    expression += " trace::log << std::endl << \"<[POP]>\" << std::endl; " + tpCall.second + ";";
    
    std::string code = "{if" + condition + "{" + expression + "}}";
    return code;
}

bool Debugger::instrumentFunction(CCodeProject* project, const std::string& functionName, uint32_t traceOptions, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const
{
    auto debugInfo = getFunctionDebugInfo(project, functionName);
    if(!debugInfo)
    {
        return false;
    }
    
    const SourceScope& rootScope = debugInfo->getRootScope();
    
    std::string platform = getPlatform() + "_test";
    uint32_t options = CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG;
    auto info = project->getCompilationInfo(functionName, platform, options);
    
    std::ifstream srcFile(info->m_sourceFilePath);
    std::string source((std::istreambuf_iterator<char>(srcFile)), std::istreambuf_iterator<char>());
    
    TextEdit instrumentation(source);
    
    if(!debugInfo->m_scopes.empty())
    {
        const SourceScope& rootScope = debugInfo->getRootScope();
        
        std::string enterMacro = "_ENTER_" + functionName + ";";
        instrumentation.insertAtOffset(rootScope.m_start.m_offset, enterMacro);
        
        std::string exitMacro = "_EXIT_" + functionName + ";";
        instrumentation.insertAtOffset(rootScope.m_end.m_offset, exitMacro);
    }
    
    for(auto ret : debugInfo->m_returns)
    {
        std::string retExpr;
        if(debugInfo->m_returnType != "void")
        {
            if(!ret.m_expression.empty())
            {
                retExpr = "{" + debugInfo->m_returnType + " __temp_ = " + ret.m_expression + ";decltype(auto) __retval_ = __temp_;_RETURN_" + functionName + ";return __retval_;}";
            }
        }
        else
        {
            retExpr = "{_RETURN_" + functionName + ";return;}";
        }
        
        if(!retExpr.empty())
        {
            instrumentation.replace(ret.m_start.m_offset, ret.m_end.m_offset + 1, retExpr);
        }
    }
    
    if(traceOptions & (uint32_t)TraceOptions::TRACE_CALL)
    {
        for(auto call : debugInfo->m_calls)
        {
            //Do not instrument calls in return statements
            bool instrumentCall = true;
            for(auto ret : debugInfo->m_returns)
            {
                auto ret_start = ret.m_start.m_lineNumber;
                auto ret_end   = ret.m_end.m_lineNumber;

                auto call_start = call.second.m_before.m_lineNumber;
                auto call_end   = call.second.m_after.m_lineNumber;
                
                bool intersect = (call_start <= ret_end) && (call_end >= ret_start);

                if (intersect) {
                    // this call lies inside a return-statement → skip instrumentation
                    instrumentCall = false;
                    break;
                }
            }
            
            if(!instrumentCall) continue;
            
            std::string location = "_" + std::to_string(call.second.m_before.m_lineNumber);
            location += "_" + std::to_string(call.second.m_before.m_column);
            
            std::string callExpr = "_CALL_" + functionName + location + "(";
            callExpr += call.second.m_expression + ")";
            
            instrumentation.replace(call.second.m_before.m_offset, call.second.m_after.m_offset, callExpr);
        }
    }
    
    if(traceOptions & (uint32_t)TraceOptions::TRACE_BREAKPOINT)
    {
        for(auto bp : customBreakpoints)
        {
            std::string location = "_" + std::to_string(bp->source_line);
            std::string snippet = "_BREAKPOINT_" + functionName + location + ";";
            instrumentation.insertAtLine(bp->source_line, snippet);
        }
    }
    
    source = instrumentation.flush();
    
    std::string instrumentedPath = getInstrumentedPath(project, info->m_sourceFilePath);
    
    std::ofstream instrumentedFile(instrumentedPath);
    instrumentedFile << source;
    instrumentedFile.close();
    
    //Delete the binary file. We want to enforce rebuild of this function
    std::string projDir = project->getProjDir();
    std::string functionBinary = project->getProjDir() + "/build_instrumented/";
    functionBinary += platform + "/" + functionName + ".o";
    
    boost_fs::remove(functionBinary);
    
    return true;
}

//debugFunctionName could be an empty string
void Debugger::instrumentSource(CCodeProject* project, const std::string& debugFunctionName, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints)
{
    std::string prevDebuggedFunction = m_functionBeingDebugged;
    m_functionBeingDebugged = debugFunctionName;
    
    //TODO: Describe the high level plan here!
    startCodeInstrumentation(project, prevDebuggedFunction);
    
    project->generateDataPrinters();
    generateTraceSources(project, debugFunctionName, customBreakpoints);
    //backupSource(project);
    
    for(auto node : project->nodeMap())
    {
        //This is TEST ONLY to enable only one function
        //if(node.first != debugFunctionName)
        //    continue;
        
        if(node.second == nullptr)
            continue;
        
        uint32_t traceOptions = 0;
        if(node.first == debugFunctionName)
        {
            traceOptions |= (uint32_t)TraceOptions::TRACE_BREAKPOINT;
            traceOptions |= (uint32_t)TraceOptions::TRACE_CALL;
        }
        
        instrumentFunction(project, node.first, traceOptions, customBreakpoints);
    }
    
    switchToInstrumentedBuild(project);
}

std::string Debugger::generateConfig()
{
    std::string config;
    std::string port = std::to_string(Client::getInstance().getDebugPort());
    config += "#define BLACK_BOX_PORT " + port + "\n";
    return config;
}

void Debugger::startCodeInstrumentation(CCodeProject* project,
                                        const std::string& prevDebuggedFunction)
{
    //findFunctionHashes(project);
    
    std::string buildDir = project->getProjDir() + "/build";
    
    //build_instrumented
    std::string instrumentedDir = buildDir + "_instrumented";
    
    boost_fs::remove_all(instrumentedDir);
    
    if(boost_fs::exists(instrumentedDir))
    {
        //TODO: Check and update each file based on the time of the last modification
        //std::vector<std::string> allFunctions = project->listAllFunctions("", -1, {});
        
        //But actually it is better to delete and copy instrumented folder each time
        //and instead to have separate build cache for the binaries
    }
    else
    {
        boost_fs::copy(buildDir, instrumentedDir, boost_fs::copy_options::recursive);
    }
    
    std::string envDir = Client::getInstance().getEnvironmentDir();
    std::string commonDebug = envDir + "/source/common_debug.h";
    boost_fs::copy(commonDebug, instrumentedDir + "/common.h", boost_fs::copy_options::overwrite_existing);
    
    {
        //empty data_defs.h to enable unit test driver compilation
        //data_printers.h will have all necessary data definitions instead
        std::ofstream ofs(instrumentedDir + "/data_defs.h");
    }
    
    std::string projConfigPath = instrumentedDir + "/project_config.h";
    std::string projConfig = generateConfig();
    saveToFile(projConfig, projConfigPath);
    
    std::string blackBoxApi = envDir + "/source/black_box_api.h";
    boost_fs::copy(blackBoxApi, instrumentedDir + "/black_box_api.h", boost_fs::copy_options::overwrite_existing);
    
    std::string dataPrinters = envDir + "/source/data_printers.h";
    boost_fs::copy(dataPrinters, instrumentedDir + "/data_printers.h", boost_fs::copy_options::overwrite_existing);
    
    if(m_system != "main")
    {
        project->buildUnitTest(m_system, true);
    }
}

void Debugger::switchToInstrumentedBuild(CCodeProject* project) const
{
    std::string buildDir = project->getProjDir() + "/build";
    
    std::string backupDir = buildDir + "_backup";
    
    std::string instrumentedDir = buildDir + "_instrumented";
    
    //Delete old backup dir if it exists
    boost_fs::remove(backupDir);
    //Bakup the build directory
    boost_fs::rename(buildDir, backupDir);
    
    //Now the _instrumented dir is the build dir
    boost_fs::rename(instrumentedDir, buildDir);
    
    std::string builCache = "cache/build_instrumented";
    project->setBuildCacheDir(builCache);
    
    compile(project);
    
    if(m_system != "main")
    {
        project->buildUnitTest(m_system, true);
    }
}

void Debugger::switchToDefaultBuild(CCodeProject* project) const
{
    std::string buildDir = project->getProjDir() + "/build";
    
    if(!boost_fs::exists(buildDir))
    {
        std::cout << "switchToDefaultBuild: build directory doesn't exist!" << std::endl;
    }
    
    std::string backupDir = buildDir + "_backup";
    
    std::string instrumentedDir = buildDir + "_instrumented";
    
    if(boost_fs::exists(backupDir))
    {
        boost_fs::remove(instrumentedDir);
        
        //Rename build dir to intrumented dir
        boost_fs::rename(buildDir, instrumentedDir);
        
        //Rename the backup dir to build dir
        boost_fs::rename(backupDir, buildDir);
    }
    
    project->generateDataHeader();
    
    std::string builCache = "cache/build";
    project->setBuildCacheDir(builCache);
    
    //In case we need to rebuild something
    project->buildBinary(true);
}

void Debugger::restoreSource(CCodeProject* project)
{
    std::string buildDir = project->getProjDir() + "/build";
    std::string backupDir = buildDir + "_backup";
    
    boost_fs::rename(buildDir, buildDir + "_instrumented");
    
    project->generateSources();
    if(project->buildBinary(true))
    {
        //TODO: We must not be here
        boost_fs::remove(backupDir);
    }
}

void Debugger::feedback(const std::string& message)
{
    
    //This will be append to debugNotes to the last setp in the trajectory
    //right before m_trajectory.getTrajectory(false), right before the next step prompt
    if (m_actionFeedback.find(message) == std::string::npos)
    {
        m_actionFeedback += message;
    }
}

//This means function requires recompilation!
bool Debugger::isFunctionChanged(CCodeProject* project,
                                   const std::string& function,
                                   const std::string& debuggedFunction,
                                   const std::string& prevDebuggedFunction)
{
    //In this case requires rebuilding. Instrumentation will be different!
    if(function == debuggedFunction || function == prevDebuggedFunction)
    {
        return true;
    }
    
    auto it = m_functionHashes.find(function);
    if(it == m_functionHashes.end())
    {
        return true;
    }
    
    auto prevId = m_prevFunctionHashes.find(function);
    if(prevId == m_prevFunctionHashes.end())
    {
        return true;
    }
    
    return it->second != prevId->second;
}

std::pair<int, std::string> Debugger::extractFirstTruncationTag(const std::string& log)
{
    static const std::string prefix = "[TRUNCATED middle ";
    static const std::string suffix = " bytes]\n";

    // find the start of the tag
    auto start = log.find(prefix);
    if (start == std::string::npos)
        return {-1, ""};

    // find the end of the tag (just after the suffix)
    auto end = log.find(suffix, start + prefix.size());
    if (end == std::string::npos)
        return {-1, ""};
    end += suffix.size();

    // count newlines before 'start' to compute 1-based line number
    int line = static_cast<int>(
        std::count(log.begin(), log.begin() + start, '\n')
    ) + 1;

    // extract the tag substring
    std::string tag = log.substr(start, end - start);
    return { line, std::move(tag) };
}

void Debugger::prebuildDebugInfo(CCodeProject* project)
{
    const std::size_t jobCount = project->nodeMap().size();

    // make them shared so the lambdas can out‑live this scope
    auto remaining = std::make_shared<std::atomic<std::size_t>>(jobCount);
    auto done      = std::make_shared<std::promise<void>>();
    auto fut       = done->get_future();

    for (auto& node : project->nodeMap())
    {
        boost::asio::post(m_threadPool,
            [this, project, node, remaining, done]()
            {
                getFunctionDebugInfo(project, node.first);

                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                    done->set_value();
            
            });
    }

    fut.wait();
}

void Debugger::resetTest()
{
    m_debugContext.reset();
    
    m_system.clear();
    m_workingDirectory.clear();
    m_privateWorkingDirectory.clear();
    m_scriptsDirectory.clear();
    m_appInfo.clear();
    
    m_runAnalysisSteps.clear();
    
    m_previousSteps = 0;
    m_summary.clear();
    m_trajectory.clear();
    m_lldbLog.clear();
    
    m_step = 0;
    m_runAnalysisStep = 0;
    
    m_nextStep.clear();
    
    m_contextVisibility.clear();
    
    m_compiledInfo.clear();
    
    m_functionHashes.clear();
    m_prevFunctionHashes.clear();
    
    m_functionBeingDebugged.clear();
    
    m_logger.clear();
    
    m_tracer.clear();
    
    m_hasValidBuild = false;
    
    m_actionFeedback.clear();
    m_infoStepsStart = -1;
    m_lastRunStep = 0;
    m_lastFixStep = 0;
    m_lastRunInfo.clear();
    m_commitMessage.clear();
    
    m_testFunctionalityDelta.clear();
    m_unitTestSource.clear();
    m_attemptsToFixUnitTestMain = 0;
    m_rawTrajectory.clear();
}

Debugger::Debugger():
m_step(0),
m_runAnalysisStep(0),
m_previousSteps(0),
m_infoStepsStart(-1),
m_hasValidBuild(false),
m_attemptsToFixUnitTestMain(0),
m_threadPool(std::thread::hardware_concurrency() ?: 2)
{
    resetTest();
}

}
