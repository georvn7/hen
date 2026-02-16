#pragma once

#include "Test.h"
#include "CCodeProject.h"
#include "LogAnalyzer.h"
#include "TraceAnalyzer.h"

#define LOG_SECTION_SIZE (3*MAX_CHARACTERS_IN_CONTENT_CHUNK)
#define MAX_BREKPOINT_HITCOUNT 3
#define PRINT_DATA_REFS_FOR_CALLS_MAX_DEPTH 4
#define PRINT_MAX_FUNCTIONS_DEPTH 4

//The thinking here is 20 linse of the log each up to 160 characters. Should be enough
#define LOG_TRACE_SIZE (20*160)
#define SEARCH_TRACE_SIZE (20*160)

namespace stdrave

{

class Breakpoint : public Reflection<Breakpoint>
{
    public:
    DECLARE_TYPE(Breakpoint, "Information about a breakpoint to be set with the LLDB debugger in batch mode. Information about in which function to set the breakpoin is provided separately in the context")
    DECLARE_FIELD(int32_t, source_line, "On which line the breakpoint should be set. If this is -1 no breakpoint will be set")
    DECLARE_FIELD(std::string, condition, "Condition evaluated at each hit whether to break. Set this to 'none' if the breakpoint has to break each time")
    DECLARE_FIELD(std::string, expression, "Expression evaluated in the current thread. The result will be recorded for analysis. If you don't want additional information you can set this to 'none'")
    
    bool hasCondition() const;
    bool hasExpression() const;
    std::string getConditionCode() const;
    std::string getExpressionCode() const;
    std::string getCodeSnippet() const;
    const std::string& getInstrumentedCodeSnippet() const;
    
    std::string m_instrumentedSnippet;
    std::string m_instrumentedCondition;
    std::string m_instrumentedExpression;
    std::map<std::string, std::string> m_stdCalls;
    
    std::string instrumentFunction(const std::string& snippet, const std::string& functionName, const std::string& returnType,
                                   const std::string& functionNameReplacement, const std::string& returnTypeReplacement) const;
    void instrumentCalls(const std::string& functionName, const std::map<std::string, std::string>& stdCalls);
    std::string instrumentCalls(const std::string& snippet, const std::string& functionName, const std::map<std::string, std::string>& stdCalls);
    bool containsFunction(const std::string &snippet, const std::string &functionName) const;
    
    const std::string& getInstrumentedConditionCode() const { return m_instrumentedCondition; }
    const std::string& getInstrumentedExpressionCode() const { return m_instrumentedExpression; }
    
    bool isTheSame(const Breakpoint& other);
    bool operator==(const Breakpoint& other);
    
    bool isValid() const;
};

class NextDebugStep : public Reflection<NextDebugStep>
{
    public:
    DECLARE_TYPE(NextDebugStep, "Next step debugging the application")
    DECLARE_ENUM_FIELD(action_type, "\"log_info\",\"function_info\",\"data_info\",\"file_info\",\"functions_summary\",\"call_graph\",\"search_source\",\"fix_function\",\"refactor_data\",\"new_data\",\"debug_function\",\"run_test\"",\
                       "Type of the action performed as a next step")
    DECLARE_FIELD(std::string, action_subject, "Could be a function name, a data type name, regex, or a file name. If no subject required for the action must be set to 'none'")
    DECLARE_FIELD(uint32_t, line_number, "Line number to the relevant actions like 'log_info', 'file_info'. If not needed must be 0")
    DECLARE_FIELD(uint32_t, invocation, "Function invocation to the relevant actions like 'log_info' and 'function_info'. If not needed must be 1")
    DECLARE_ARRAY_FIELD(Breakpoint, breakpoints, "Breakpoints to be set only if the 'action_type' is 'debug_function'. Required field, must be empty array if no need for breakpoints")
    DECLARE_FIELD(std::string, motivation, "Concise explanation of why this action_type and action_subject are selected for the next debug step.")
    uint32_t m_stepId;
    void clear();
    
    static bool isInformationRequest(const std::string& actionType);
    bool isInformationRequest();
};

class RunAnalysis : public Reflection<RunAnalysis>
{
public:
    DECLARE_TYPE(RunAnalysis, "Analysis of the result from test execution")
    DECLARE_FIELD(std::string, debug_notes, "Detailed debug notes updated with the current analysis. If the test is successful this field must have only one word - PASS. Keep to 3 paragraphs or fewer, under 2000 characters total.")
    DECLARE_FIELD(std::string, log_summary, "Summary of the log, updated with the current log section. Contains key quetes from the log relevant to the analysis in the Debug notes. Keep to 3 paragraphs or fewer, under 2000 characters total.")
    
    
    std::string m_function;
    
    bool m_testResult;
    std::string m_testLog;
    
    void clear()
    {
        debug_notes.clear();
        log_summary.clear();
        m_function.clear();
    }
};

struct DebugStep
{
    std::string m_motivation;
    std::string m_logSummary;
    std::string m_debugNotes;
    std::string m_action;
    std::string m_subject;
    std::string m_commitHash;
    
    uint32_t    m_lineNumber;
    uint32_t    m_invocation;
    
    bool save(const std::string& filePath)
    {
        web::json::value json;
        
        json[U("motivation")] = web::json::value::string(utility::conversions::to_string_t(m_motivation));
        json[U("logSummary")] = web::json::value::string(utility::conversions::to_string_t(m_logSummary));
        json[U("debugNotes")] = web::json::value::string(utility::conversions::to_string_t(m_debugNotes));
        json[U("action")] = web::json::value::string(utility::conversions::to_string_t(m_action));
        json[U("subject")] = web::json::value::string(utility::conversions::to_string_t(m_subject));
        json[U("commitHash")] = web::json::value::string(utility::conversions::to_string_t(m_commitHash));
        
        return stdrave::saveJson(json, filePath);
    }

    bool load(const std::string& filePath)
    {
        web::json::value json;
        if(!stdrave::loadJson(json, filePath))
        {
            return false;
        }
        
        if(!json.has_field(U("motivation"))) { return false; }
        m_motivation = utility::conversions::to_utf8string(json[U("motivation")].as_string());
        
        if(!json.has_field(U("logSummary"))) { return false; }
        m_logSummary = utility::conversions::to_utf8string(json[U("logSummary")].as_string());
        
        if(!json.has_field(U("debugNotes"))) { return false; }
        m_debugNotes = utility::conversions::to_utf8string(json[U("debugNotes")].as_string());
        
        if(!json.has_field(U("action"))) { return false; }
        m_action = utility::conversions::to_utf8string(json[U("action")].as_string());
        
        if(!json.has_field(U("subject"))) { return false; }
        m_subject = utility::conversions::to_utf8string(json[U("subject")].as_string());
        
        if(json.has_field(U("commitHash"))) //Do not exists in old trajectories
        {
            m_commitHash = utility::conversions::to_utf8string(json[U("commitHash")].as_string());
        }
        
        return true;
    }

    std::string summary() const
    {
        std::string summary;
        summary += "\naction: " + m_action + ", ";
        if(!m_subject.empty())
        {
            summary += "\nsubject: " + m_subject + "\n";
        }
        else
        {
            summary += "\nsubject: none\n";
        }
        
        return summary;
    }

    std::string concise() const
    {
        std::string info;
        info += summary();
        if(!m_motivation.empty())
        {
            info += "\nmotivation: " + m_motivation + "\n";
        }
        
        return info;
    }
    
    std::string notes() const
    {
        std::string info;
        if(!m_debugNotes.empty())
        {
            info += "\ndebug notes: " + m_debugNotes + "\n";
        }
        if(!m_logSummary.empty())
        {
            info += "\nlog summary: " + m_logSummary + "\n";
        }
        return info;
    }
    
    std::string fullInfo() const
    {
        std::string info;
        info += concise();
        info += notes();
        return info;
    }
};

struct DebugVisibility
{
    std::set<std::string>   m_functions;
    std::set<std::string>   m_dataTypes;
    std::set<std::string>   m_files;
    
    std::set<std::string>   m_functionInfo;
    std::set<std::string>   m_log;
    std::set<std::string>   m_callGraph;
    std::set<std::string>   m_functionsSummary;
    std::set<int>           m_history;
    std::set<std::string>   m_search;
    
    //the key is function:invocation
    std::set<std::string>   m_frames;
    std::set<std::string>   m_functionLog;
    
    void clear()
    {
        m_functions.clear();
        m_dataTypes.clear();
        m_files.clear();
        
        m_functionInfo.clear();
        m_log.clear();
        m_callGraph.clear();
        m_functionsSummary.clear();
        m_history.clear();
        m_search.clear();
        m_frames.clear();
        m_functionLog.clear();
    }
    
    bool visibleFunction(const std::string& function)
    {
        if(m_functions.find(function) == m_functions.end())
        {
            m_functions.insert(function);
            return true;
        }
        
        return false;
    }

    bool isFunctionVisible(const std::string& function)
    {
        return m_functions.find(function) != m_functions.end();
    }
    
    bool isTraceFrameVisible(const std::string& key)
    {
        return m_frames.find(key) != m_frames.end();
    }
    
    bool isTraceFrameVisible(const std::string& function, int invocation)
    {
        std::string key = function + ":" + std::to_string(invocation);
        return isTraceFrameVisible(key);
    }
    
    bool visibleTraceFrame(const std::string& function, int invocation)
    {
        std::string key = function + ":" + std::to_string(invocation);
        
        if(!isTraceFrameVisible(key))
        {
            m_frames.insert(key);
            return true;
        }
        
        return false;
    }
    
    bool isFunctionLogVisible(const std::string& key)
    {
        return m_functionLog.find(key) != m_functionLog.end();
    }
    
    bool visibleFunctionLog(const std::string& function, int invocation)
    {
        std::string key = function + ":" + std::to_string(invocation);
        
        if(!isFunctionLogVisible(key))
        {
            m_functionLog.insert(key);
            return true;
        }
        
        return false;
    }

    bool visibleData(const std::string& data)
    {
        if(m_dataTypes.find(data) == m_dataTypes.end())
        {
            m_dataTypes.insert(data);
            return true;
        }
        
        return false;
    }

    bool isDataVisible(const std::string& data)
    {
        return m_dataTypes.find(data) != m_dataTypes.end();
    }

    bool visibleFile(const std::string& filename, int lineNumber)
    {
        if(lineNumber < 1) lineNumber = 1;
        
        std::string key = filename + ":" + std::to_string(lineNumber);
        if(m_files.find(key) == m_files.end())
        {
            m_files.insert(key);
            return true;
        }
        
        return false;
    }

    bool visibleFunctionInfo(const std::string& function, int invocation, int lineNumber)
    {
        if(invocation < 1) invocation = 1;
        if(lineNumber < 1) lineNumber = 1;
        
        std::string key = function + ":" + std::to_string(invocation) + ":" + std::to_string(lineNumber);
        if(m_functionInfo.find(key) == m_functionInfo.end())
        {
            m_functionInfo.insert(key);
            return true;
        }
        
        return false;
    }

    bool visibleLogInfo(const std::string& function, int invocation, int lineNumber)
    {
        if(invocation < 1) invocation = 1;
        if(lineNumber < 1) lineNumber = 1;
        
        std::string functionKey = function;
        if(functionKey.empty() || functionKey == "none")
        {
            functionKey.clear();
        }
        
        std::string key = functionKey + ":" + std::to_string(invocation) + ":" + std::to_string(lineNumber);
        if(m_log.find(key) == m_log.end())
        {
            m_log.insert(key);
            return true;
        }
        
        return false;
    }

    bool visibleFunctionsSummary(const std::string& function)
    {
        if(m_functionsSummary.find(function) == m_functionsSummary.end())
        {
            m_functionsSummary.insert(function);
            return true;
        }
        
        return false;
    }

    bool visibleCallGraph(const std::string& function)
    {
        if(m_callGraph.find(function) == m_callGraph.end())
        {
            m_callGraph.insert(function);
            return true;
        }
        
        return false;
    }

    bool visibleHistory(int step)
    {
        if(m_history.find(step) == m_history.end())
        {
            m_history.insert(step);
            return true;
        }
        
        return false;
    }

    bool visibleSearchSource(const std::string& regexPattern)
    {
        if(m_search.find(regexPattern) == m_search.end())
        {
            m_search.insert(regexPattern);
            return true;
        }
        
        return false;
    }

    bool isVisible(const std::string& action, const std::string& subject, int invocation, int lineNumber)
    {
        if(action == "function_info")
        {
            if(invocation < 1) invocation = 1;
            if(lineNumber < 1) lineNumber = 1;
            std::string key = subject + ":" + std::to_string(invocation) + ":" + std::to_string(lineNumber);
            return m_functionInfo.find(key) != m_functionInfo.end();
        }
        else if(action == "data_info")
        {
            return m_dataTypes.find(subject) != m_dataTypes.end();
        }
        else if(action == "file_info")
        {
            if(lineNumber < 1) lineNumber = 1;
            
            std::string key = subject + ":" + std::to_string(lineNumber);
            return m_files.find(key) != m_files.end();
        }
        else if(action == "functions_summary")
        {
            return m_functionsSummary.find(subject) != m_functionsSummary.end();
        }
        else if(action == "call_graph")
        {
            return m_callGraph.find(subject) != m_callGraph.end();
        }
        else if(action == "log_info")
        {
            if(invocation < 1) invocation = 1;
            if(lineNumber < 1) lineNumber = 1;
            
            std::string key = subject + ":" + std::to_string(invocation) + ":" + std::to_string(lineNumber);
            return m_log.find(key) != m_log.end();
        }
        else if(action == "step_info")
        {
            return m_history.find(invocation) != m_history.end();
        }
        else if(action == "search_source")
        {
            return m_search.find(subject) != m_search.end();
        }
        
        return false;
    }
    
};

inline std::pair<int, int> getConsecutiveSteps(const boost::filesystem::path& dir_path)
{
    if (!boost::filesystem::exists(dir_path) ||
        !boost::filesystem::is_directory(dir_path))
    {
        // Invalid directory
        return std::make_pair(-1, -1);
    }

    const std::string prefix = "step_";
    std::set<int> indices;

    // 1) Scan and collect all numeric suffixes
    for (boost::filesystem::directory_iterator it(dir_path), end; it != end; ++it)
    {
        if (!boost::filesystem::is_directory(it->status()))
            continue;

        std::string name = it->path().filename().string();
        if (name.size() <= prefix.size())
            continue;

        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;

        std::string num_str = name.substr(prefix.size());
        try
        {
            int n = std::stoi(num_str);
            if (n > 0)
                indices.insert(n);
        }
        catch (const std::exception&)
        {
            // non-numeric or overflow → skip
        }
    }

    // 2) Find the first consecutive sequence starting from 1
    if (indices.empty() || indices.find(1) == indices.end())
    {
        // No step_1 found, so no consecutive sequence
        return std::make_pair(0, 0);
    }

    // We have step_1, so first is 1
    int first = 1;
    int last = 1;
    
    // Walk from 2 upwards until a missing index is found
    for (int expect = 2; ; ++expect)
    {
        if (indices.find(expect) != indices.end())
            last = expect;
        else
            break;
    }

    return std::make_pair(first, last);
}

inline bool parseLastExitCode(const std::string& log, int& out_code) {
    // Match: "... status = 17 (0x00000011)" or "... code = 17 (...)".
    // Captures either the decimal (grp 1) or the hex (grp 2).
    static const std::regex rx_status(
        R"((?:status|code)\s*=\s*(?:(-?\d+)|\(\s*(0x[0-9A-Fa-f]+)\s*\)))",
        std::regex::icase);

    bool found = false;
    long last = 0;

    std::smatch m;
    auto it = log.cbegin();
    const auto end = log.cend();

    // Walk all matches, keep the last one seen.
    while (std::regex_search(it, end, m, rx_status)) {
        if (m[1].matched) {
            last = std::stol(m[1].str(), nullptr, 10);
        } else if (m[2].matched) {
            last = std::stol(m[2].str(), nullptr, 16); // "0x..." parsed as hex
        }
        found = true;
        it = m.suffix().first;
    }

    // Fallback: if nothing matched, accept any "(0x...)" anywhere in the log.
    if (!found) {
        static const std::regex rx_any_hex(R"(\(\s*(0x[0-9A-Fa-f]+)\s*\))");
        it = log.cbegin();
        while (std::regex_search(it, end, m, rx_any_hex)) {
            last = std::stol(m[1].str(), nullptr, 16);
            found = true;
            it = m.suffix().first;
        }
    }

    if (found) out_code = (int)last;
    return found;
}

// --- helper ---------------------------------------------------------------
inline std::string cleanLldbPrompts(const std::string& raw)
{
    std::stringstream in(raw);
    std::string line, out;

    while (std::getline(in, line))
    {
        // rfind == 0  ➜  line *starts* with "(lldb)"
        if (line.rfind("(lldb)", 0) != 0)
            out += line + '\n';
    }
    return out;
}

inline std::string getTestResult(const std::string& result)
{
    const std::string filtered = cleanLldbPrompts(result);

    const std::regex pattern(R"(_DEBUG_COMMAND_RESULT=([^\r\n]*))");
    std::string resultStr;

    for (std::sregex_iterator it(filtered.begin(), filtered.end(), pattern),
                              end;
         it != end; ++it)
    {
        resultStr = (*it)[1].str();          // overwrites → keeps the *last* match
    }

    return resultStr;
}

class DebugContextProvider
{
    DebugVisibility m_contextVisibility;
    std::string m_workingDirectory;
    
    LogAnalyzer m_logger;
    TraceAnalyzer m_tracer;
    std::string m_system;

public:
    
    void setup(const std::string& workingDirectory, const std::string& system)
    {
        m_contextVisibility.clear();
        
        m_system = system;
        
        m_workingDirectory = workingDirectory;
        
        std::string logPath = workingDirectory + "/stdout.log";
        std::string logContent = getFileContent(logPath);
        m_logger.parse(logContent);
        std::string tracePath = workingDirectory + "/trace.txt";
        m_tracer.loadFromFile(tracePath);
        
        DebugStep dbgStep;
        std::string dbgStepPath = workingDirectory + "/dbgStep.json";
        dbgStep.load(dbgStepPath);
        
        if(dbgStep.m_action == "debug_function")
        {
            m_tracer.loadBreakpointTraces(workingDirectory + "/breakpoints", dbgStep.m_subject);
        }
        
        //If we have a memo that means crash/hang that must be fixed first.
        //Generate the analysis and return.
        std::string memoFile = workingDirectory + "/memo.txt";
        //m_memo.loadFromFile(memoFile);
        //auto memoFrame = m_memo.getLastFrame(true);
        auto memoFrames = m_tracer.loadStackTrace(memoFile, workingDirectory + "/stack");
    }
    
    void clear()
    {
        m_contextVisibility.clear();
    }
    
    bool checkFunctionExists(CCodeProject* project, const std::string& functionName, std::string& debugNode)
    {
        if(project->nodeMap().find(functionName) == project->nodeMap().end())
        {
            debugNode = "The requested function '" + functionName + "' doesn't exist in the project.\n";
            return false;
        }
        
        return true;
    }
    
    std::string getFileInfo(CCodeProject* project, const std::string& file, int lineToStartFrom, int maxCharacters)
    {
        std::string info;
        
        std::string fileName = boost_fs::path(file).filename().string();
        std::string fileExt = boost_fs::path(file).extension().string();
        std::string fullPath = m_workingDirectory + "/" + fileName;
        
        if(fileName == "stdout.log")
        {
            info += "\n//File '" + fileName + "' is not accessible. To obtain log informatin from the most recent test run use 'log_info' and 'function_info' actions.\n\n";
            return info;
        }
        
        if(fileName == "trace.txt")
        {
            info += "\n//File '" + fileName + "' is not accessible. To obtain trace informatin from the most recent test run use trace_info and function_info actions.\n\n";
            return info;
        }
        
        if(fileName == ".DS_Store" || fileName == "trace.txt")
        {
            info += "\n//File '" + fileName + "' is not accessible.\n\n";
            return info;
        }
        
        if(fileName == "main") {
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
        
        //lineToStartFrom must be 1-based
        
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
    
    std::string getHighLevelAppInfo(CCodeProject* project, const std::string& functionName,
                                              int functionsMaxDepth, int callGraphMaxDepth)
    {
        std::string info;
        
        if(callGraphMaxDepth)
        {
            info += project->printGraph(functionName, callGraphMaxDepth, false);
        }
        
        if(functionsMaxDepth)
        {
            if(functionsMaxDepth < 0) {
                info += "List of all functions defined by this application:\n\n";
            }
            else {
                info += "List of all functions defined by this application from the call graph with maximum depth of up to ";
                info += std::to_string(functionsMaxDepth) + ":\n\n";
            }
            
            info += project->listAllFunctions(functionName, functionsMaxDepth, true, true, false, {});
        }
        
        return info;
    }
    
    int mapLine(CCodeProject* project, const std::string& filePath, const std::string& functionName, const std::string& functionSource, int lineNumber) const
    {
        if(!boost_fs::exists(filePath))
        {
            std::cout << "Couldn't map line: " << lineNumber << ". File doesn't exists: " << filePath << std::endl;
            return -1;
        }
        
        std::string sourceFile = getFileContent(filePath);
        
        //TODO: This could be optimized with mapLine to avoid normalization for each breakpoint
        return normalizeAndMapLine(functionSource, sourceFile, lineNumber);
    }

    int mapLine(CCodeProject* project, const std::string& filePath, const std::string& functionName, int lineNumber) const
    {
        std::string implementation = project->getFunctionImplementation(functionName);
        return mapLine(project, filePath, functionName, implementation, lineNumber);
    }
    
    std::string printFunctionSource(CCodeProject* project, const std::string& functionName, const std::string source) const
    {
        std::string sourceFilePath = project->getProjDir() + "/build/source/" + functionName + "/" + functionName + ".cpp";
        
        int lineOffset = mapLine(project, sourceFilePath, functionName, source, 1);
        return printLineNumbers(source, lineOffset);
    }
    
    std::string getRequestedInfo(CCodeProject* project, int allFunctions, int callGraph,
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
    
    std::string stepLogInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& stepInfo)
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
            
            std::string startLine = std::to_string(lineNumber);
            std::string linesCount = std::to_string(m_logger.linesCount());
            debugNotes += "Requesting log section start at line: " + startLine;
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

    std::string stepTraceInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
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
    
    std::string analysisFrameTrace(CCodeProject* project, const std::string& function, uint32_t invocation)
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

    std::string stepFunctionInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& stepInfo)
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
            
            if(m_contextVisibility.visibleTraceFrame(functionName, invocation))
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
                    std::string startLine = std::to_string(lineNumber);
                    std::string linesCount = std::to_string(m_logger.linesCount());
                    debugNotes += "Log section start line: " + startLine;
                    
                    debugNotes += ", total application log lines count: " + linesCount;
                    debugNotes += ", log entries for function invocation: " + std::to_string(logForInvocation);
                    
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
            
                //Trace information
                std::stringstream ssTrace;
                logForInvocation =  invocation;
                m_tracer.printFrame(ssTrace, functionName, invocation);
                std::string functionTrace = ssTrace.str();
                if(!functionTrace.empty())
                {
                    std::string traceEventsMsg = "\nTrace events for function '" + functionName + "' invocation: " + std::to_string(logForInvocation) + "\n";
                    debugNotes += traceEventsMsg;
                    
                    infoForCurrentStep += traceEventsMsg;
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
                debugNotes += "trace and log events for function '" + functionName + "' invocation: " + std::to_string(invocation) + "\n";
                debugNotes += "[[provided in the context]]\n";
                
                infoForCurrentStep += debugNotes;
            }
            
        }
        else
        {
            infoForCurrentStep += "Consult with the list of available functions defined in the project:\n\n";
            infoForCurrentStep += getRequestedInfo(project, -1, 0, {}, {}, {}, {});
        }
        
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
        stepInfo.m_debugNotes.clear();
        return "";
    }

    std::string stepDataInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
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
            infoForCurrentStep += "The requested data type " + subject + " doesn't exist in the project\n\n";
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

    std::string stepFileInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int lineNumber, DebugStep& stepInfo)
    {
        boost_fs::path filePath(subject);
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
    
    std::string stepFunctionsSummary(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
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

    std::string stepCallGraph(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
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
    
    void checkTestStepInput(std::ostream& log,
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
    
    void checkTestStepInput(std::ostream& log, const TestStep& step, const std::string& stepName, bool deleteOutput)
    {
        checkTestStepInput(log, step.input_files, step.output_files, stepName, deleteOutput);
    }

    void checkTestStepOutput(std::ostream& log, const std::vector<std::shared_ptr<std::string>>& output_files, const std::string& stepName)
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
    
    void checkTestStepOutput(std::ostream& log, const TestStep& step, const std::string& stepName)
    {
        checkTestStepOutput(log, step.output_files, stepName);
    }
    
    std::string loadTestLogFromStep(CCodeProject* project, const TestDef& test, uint32_t debugStepId)
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
            
            log += "Test command:\n\n";
            log += "main " + cmd + "\n\n";
            
            std::string consoleLog = getFileContent(m_workingDirectory + "/console.log");
            
            log += "Test command stdout:\n";
            if(!consoleLog.empty())
            {
                std::string consoleLogLimited = consoleLog.length() > 2048 ? consoleLog.substr(0, 2048) + "...[[truncated]]": consoleLog;
                log += consoleLogLimited + "\n\n";
            }
            else
            {
                log += "Empty stdout string\n\n";
            }
            
            std::stringstream ssTestInput;
            checkTestStepInput(ssTestInput, test.test.input_files, test.test.output_files, "test", false);
            
            std::string lldbLog = getFileContent(directoryForThisStep + "/lldb.log");
            std::string stdoutLog = getFileContent(directoryForThisStep + "/stdout.log");
            
            int returnCode = 65535;
            
            auto line = getFirstLine(directoryForThisStep + "/memo.txt");
            if(line && line->length() > 0)
            {
                if(!lldbLog.empty()) {
                    log += "lldb log from the test command:\n\n";
                    log += lldbLog + "\n\n";
                    
                    if(!parseLastExitCode(lldbLog, returnCode))
                    {
                        //TODO: Do we want to do something here?
                    }
                }
            }
            
            std::stringstream ssTestOutput;
            checkTestStepOutput(ssTestOutput, test.test.output_files, "test");
            
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
                        log += "stdout doesn't match the expected regex pattern: " + stdoutRegex + "\n";
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
        
        m_workingDirectory = oldWd;
        
        log = replaceAll(log, project->getProjDir(), ".");
        return log;
    }

    std::string loadTestLogForStep(CCodeProject* project, const TestDef& test, const TestStep& step, const std::string& testStepName, uint32_t debugStepId)
    {
        bool stepResults = true;
        std::string log;
        
        std::string directoryForThisStep = project->getProjDir() + "/debug/" + test.name + "/trajectory/step_" + std::to_string(debugStepId) + "/wd";
        
        std::stringstream ssTestInput;
        checkTestStepInput(ssTestInput, step, testStepName, false);
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
                    std::string consoleLogLimited = testCommandResult.length() > 2048 ? testCommandResult.substr(0, 2048) + "...[[truncated]]": testCommandResult;
                    log += consoleLogLimited + "\n\n";
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
                            log += "stdout doesn't match the expected regex pattern: " + stdoutRegex + "\n";
                            stepResults = false;
                        }
                    }
                }
                
                i++;
            }
        }
        
        std::stringstream ssTestOutput;
        checkTestStepOutput(ssTestOutput, step, testStepName);
        log += ssTestOutput.str();
        
        log = replaceAll(log, project->getProjDir(), ".");
        return log;
    }
    
    bool getStepTrajecotyCfg(CCodeProject* project, const TestDef& test, int stepId, web::json::value& cfg)
    {
        std::string info;
        
        std::string trajectoryDir = project->getProjDir() + "/debug/" + test.name + "/trajectory";
        
        if(!boost_fs::exists(trajectoryDir))
        {
            std::cout << "\nUnable to find any recorded steps!\n";
            return false;
        }
        
        std::string stepIdStr = std::to_string(stepId);
        
        auto stepsRange = getConsecutiveSteps(trajectoryDir);
        
        if( stepsRange.first > stepId || stepsRange.second < stepId)
        {
            std::cout << "\nInformation for the requested step " + stepIdStr + " doesn't exist! ";
            std::cout << " The available steps range is from " + std::to_string(stepsRange.first);
            std::cout << " to " + std::to_string(stepsRange.second) + "\n";
            return false;
        }
        
        std::string stepIdDir = "/step_" + stepIdStr;
        
        std::string tracjectoryFile = trajectoryDir + stepIdDir + "/tracjectory.json";
        if(!boost_fs::exists(tracjectoryFile))
        {
            std::cout << "\nUnable to load information for the requested step " + stepIdStr + ". The data might be corrupted!\n";
            return false;
        }
        
        //Load the trajectory configuration json file
        stdrave::loadJson(cfg, tracjectoryFile);
        return true;
    }
    
    std::string getStepInfo(CCodeProject* project, const TestDef& test, int stepId)
    {
        std::string info;
        std::string stepIdStr = std::to_string(stepId);
        std::string trajectoryDir = project->getProjDir() + "/debug/" + test.name + "/trajectory";
        std::string stepIdDir = "/step_" + stepIdStr;

        web::json::value trajectoryCfg;
        if(getStepTrajecotyCfg(project, test, stepId, trajectoryCfg))
        {
            info += "\nUnable to load information for the requested step " + stepIdStr + ". The data might be corrupted!\n";
            return info;
        }
        
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
        std::string summary = stdrave::loadFile(requestedStepDir + "summary.txt");
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
        
        info += stdrave::loadFile(requestedStepDir + "info.txt");
        info += "\n********** SUMMARIZED TRAJECTORY AT DEBUGGING STEP " + stepIdStr + " END! **********\n";
        
        return info;
    }

    std::string stepHistory(CCodeProject* project, const std::string& motivation, const TestDef& test, int invocation, DebugStep& stepInfo)
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

    std::string stepSearchSource(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo)
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
    
    std::string stepInfo(CCodeProject* project, const TestDef& test, const std::string& action, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& debugStep)
    {
        //if(!NextDebugStep::isInformationRequest(nextStep.action_type))
        if(action == "run_test" || action == "fix_function" || action == "debug_function")
        {
            return "";
        }
        
        std::string info;
        
        if(action == "log_info")
        {
            info += stepLogInfo(project, subject, motivation, invocation, lineNumber, debugStep);
        }
        else if(action == "function_info")
        {
            info += stepFunctionInfo(project, subject, motivation, invocation, lineNumber, debugStep);
        }
        else if(action == "data_info")
        {
            info += stepDataInfo(project, subject, motivation, debugStep);
        }
        else if(action == "file_info")
        {
            info += stepFileInfo(project, subject, motivation, lineNumber, debugStep);
        }
        else if(action == "functions_summary")
        {
            info += stepFunctionsSummary(project, subject, motivation, debugStep);
        }
        else if(action == "call_graph")
        {
            info += stepCallGraph(project, subject, motivation, debugStep);
        }
        else if(action == "step_info")
        {
            info += stepHistory(project, subject, test, invocation, debugStep);
        }
        else if(action == "search_source")
        {
            info += stepSearchSource(project, subject, motivation, debugStep);
        }
        
        return info;
    }
    
    std::string getTestDescription(CCodeProject* project, const TestDef& test)
    {
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
        std::string execPath = projDir + "/build/" + getPlatform() + "_test/main";
        
        std::string testRawCmd = test.test.command;
        std::string testCmd = testRawCmd;
        std::string testResultStr;
        bool testDebug = false;
        bool testResult = false;
        std::string stdoutRegex;
        parsePrefixFlags(testRawCmd, testDebug, testResult, testResultStr, stdoutRegex, testCmd);
        
        std::string commandLine = "main " + testCmd;
        
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
        commands << "Note the '" << application << "' executable is 'main' in the 'Test command'" << std::endl;
        
        commands << "We are going to debug the '" << application << "' until it successfully pass the test!" << std::endl;
        commands << "Reward-hacking is stricly prohibited! ";
        commands << "Ensure functionality is implemente according to the project description. ";
        commands << "The application will be tested with private tests to identify reward-hacking practices." << std::endl << std::endl;
        
        return commands.str();
    }
    
    std::string getTestFunctionalityDelta(CCodeProject* project, const TestDef& test, const std::string& regressionTestPath)
    {
        //Build the prompt for functionality delta with the most recently passed test
        std::stringstream testFuncDelta;
        
        testFuncDelta << "PASSED TEST START" << std::endl << std::endl;
        testFuncDelta << "The follwing test is a subset of the current test and has passed both public and private tests. ";
        testFuncDelta << "Consider this when investigating issues with the current test and the added new features. ";
        testFuncDelta << "It doesn't mean that we can't have issues with the previously passing tests but the risk should be lower";
        testFuncDelta << std::endl << std::endl;
        
        TestDef regressionTest;
        web::json::value regressionTestJson;
        loadJson(regressionTestJson, regressionTestPath + "/public/test.json");
        
        std::string jsonFile = "```json\n";
        jsonFile += getFileContent(regressionTestPath + "/public/test.json");
        jsonFile += "```\n\n";
        
        testFuncDelta << jsonFile;
        
        regressionTest.from_json(regressionTestJson);
        for(auto file : regressionTest.getRegressionFiles())
        {
            std::string fileContent = getFileContent(regressionTestPath + "/public/" + file);
            testFuncDelta << "File: " << file;
            testFuncDelta << "```" << boost_fs::path(file).extension() << std::endl;
            testFuncDelta << fileContent;
            testFuncDelta << "```\n\n";
        }
        
        testFuncDelta << "PASSED TEST END" << std::endl << std::endl;
        
        testFuncDelta << "Files from the current test, useful to analyze the delta in the functionality between the passed and the current test:\n\n";
        for(auto file : test.getRegressionFiles())
        {
            std::string fileContent = getFileContent(m_workingDirectory + "/" + file);
            testFuncDelta << "File: " << file;
            testFuncDelta << "```" << boost_fs::path(file).extension() << std::endl;
            testFuncDelta << fileContent;
            testFuncDelta << "```\n\n";
        }
        
        testFuncDelta << "Consider to review the delta in the functionality between the current test and the already 'PASSED TEST' (if presented) it might help when investigating issues.\n\n";
        
        return testFuncDelta.str();
    }
    
    bool visibleTraceAndLog(std::ostream& log, const std::pair<std::string, uint32_t>& frame)
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
    
    std::string getSubSystemsData(CCodeProject* project, std::set<std::string>& subSystems)
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
        
        return ss.str();
    }
    
    std::string getBreakpointsInfo(bool instrumented, const std::string& function,
                                             const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const
    {
        std::string breakpointInfo;
        
        int bpId = 1;
        for(auto bp : customBreakpoints) {
            
            breakpointInfo += "Breakpoint " + std::to_string(bpId) + ":\n";
            
            breakpointInfo += "Source line: " + std::to_string(bp->source_line) + "\n";
            breakpointInfo += "Condition: " + bp->getConditionCode() + "\n";
            breakpointInfo += "Expression: " + bp->getExpressionCode() + "\n";
            
            if(instrumented)
            {
                breakpointInfo += "Instrumented condition: " + bp->getInstrumentedConditionCode() + "\n";
                breakpointInfo += "Instrumented expression: " + bp->getInstrumentedExpressionCode() + "\n\n";
            }
            
            bpId++;
        }
        
        return breakpointInfo;
    }
    
    std::string getStackTrace(CCodeProject* project, const std::string& stack, bool log, uint32_t maxTailToPrint)
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
    
    std::pair<bool, std::string> runAnalysis(CCodeProject* project)
    {
        std::string analysisHint;
        bool requireSeparateStep = false;
        
        //If we have a memo that means crash/hang that must be fixed first.
        //Generate the analysis and return.
        std::string memoFile = m_workingDirectory + "/memo.txt";
        
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
                
                analysisHint += "Here are traces for some of the functions in the call stack before the recursion:\n\n";
                analysisHint += memoTrace;
                
                auto stack = TraceAnalyzer::parseStack(frameTrace->m_stack);
                analysisHint += "Investigate the stack trace : " + frameTrace->m_stack + "\n";
                analysisHint += "Start from the last recursive call and up to the '" + stack.front().first + "' function. ";
            }
            else
            {
                analysisHint += "NOTE: The application terminated unexpectedly in function '";
                analysisHint += memoFrame.first + "' invocation: " + std::to_string(memoFrame.second) + "\n\n";
                
                analysisHint += "call stack before the crash: " + frameTrace->m_stack + "\n";
                analysisHint += "Here are full traces for each of the functions in the call stack before the crash:\n\n";
                analysisHint += memoTrace;
                
                auto stack = TraceAnalyzer::parseStack(frameTrace->m_stack);
                analysisHint += "Investigate the stack trace : " + frameTrace->m_stack + "\n";
                analysisHint += "Start from the last call '" + stack.back().first + "' and for each function call up to '" + stack.front().first;
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
            if(lastFrame->m_invocation.first != "main")
            {
                requireSeparateStep = true;
                
                analysisHint += "The last recorded frame in the trace is for function '" + lastFrame->m_invocation.first;
                lastFrameIsNotMain = true;
                
                std::stringstream ssFrame;
                m_tracer.printFrame(ssFrame, lastFrame->m_invocation.first, lastFrame->m_invocation.second);
                
                analysisHint += "Trace for '" + lastFrame->m_invocation.first + "' :\n";
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
                analysisHint += "This suggest the application crashes or hangs in the function '";
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
                //if(analysis.m_testResult != test.expected_result)
                
                //TODO: We need this check
                /*if(!analysis.m_testResult)
                {
                    //analysisHint += "However, the returned result for _DEBUG_COMMAND_RESULT=" + analysis.m_testResult;
                    //analysisHint += " doesn't match the expected result: " + test.expected_result;
                    analysisHint += "However, results for some of the commands don't match expected outcomes. ";
                    analysisHint += " Further investigation and debugging are required!\n";
                }
                else*/
                {
                    analysisHint += "Verify that the test results match expected outcomes. ";
                }
            }
        }
        else if(m_tracer.getFramesCount() == 0)
        {
            analysisHint += "No frames recorded in the trace.";
            analysisHint += " This suggest problems in the build system to successfully build instrumented binary.";
        }
        else if(m_system != "main")
        {
            analysisHint += "The unit test executed without crashes or hangs! ";
            /*if(!analysis.m_testResult)
            {
                analysisHint += "However, results for some of the commands don't match expected outcomes. ";
                analysisHint += " Further investigation and debugging are required!\n";
            }
            else*/
            {
                analysisHint += "Verify that the test results match expected outcomes. ";
                //analysisHint += "If they don't, investigate and debug.";
            }
        }
        
        //Now check the log
        auto lastInvocation = m_logger.logGetLastInvocation();
        
        if(lastInvocation.second != 0)
        {
            if(lastInvocation.first != "main")
            {
                analysisHint += "\nFrom the log, the last logged function is '" + lastInvocation.first + "\n";
            }
        }
        else
        {
            analysisHint += "\nCouldn't find logged messages from application functions in the log. This is not expected.\n";
        }
        
        return std::make_pair(requireSeparateStep,analysisHint);
    }
    
    
    //Get information for the recently fixed function
    std::string getRecentFixInfo(CCodeProject* project, std::string fixedFunction)
    {
        if(fixedFunction.empty() || fixedFunction == "none")
        {
            return std::string();
        }
        
        std::string fixedFunctionInfo = "Here is information for the recently fixed function.\n\n";
        
        fixedFunctionInfo += getRequestedInfo(project, 0, 0, {},
                                          {std::make_shared<std::string>(fixedFunction)},
                                          {},{});
        
        fixedFunctionInfo += "Trace and log messages for the last recorded invocations of '" + fixedFunction + "'\n\n";
        fixedFunctionInfo += getRequestedInfo(project, 0, 0, fixedFunction, {}, {}, {});
        
        auto lastFrame = m_tracer.getLastInvocation(fixedFunction);
        if(lastFrame)
        {
            if(lastFrame->m_invocation.second != 1)
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
            if( lastLog.second != 1)
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
        
        return fixedFunctionInfo;
    }
};

}
