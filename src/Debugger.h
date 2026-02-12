#pragma once

#include "Singleton.h"
#include "Test.h"
#include "CCodeProject.h"

#include "Inferencing.h"

#include "DebugContextProvider.h"

#define MAX_EVENTS_HIT_COUNT 3

#define ENABLE_TRACE_ON_FUNCTION_EXIT_STR "true"
#define ENABLE_TRACE_ON_FUNCTION_ENTER_STR "true"

#define MAX_TRAJECTORY_FOR_SUMMARIZATION 30
#define MIN_STEPS_TO_SUMMARIZE 15

//#define LLDB_COMPATIBLE_BREAKPOINTS

#define DELETE_INFO_REQUESTS_ON_COMPACTION 3

#define DISCLOSE_STOP_STEPS_AFTER_FIX 20

#define ANALYSIS_HINT_MAX_SIZE (200*1024)
#define LIMIT_DEBUG_NOTES_SIZE
#define LIMIT_DESCRIPTION_SIZE

#include "TraceAnalyzer.h"
#include "LogAnalyzer.h"

namespace stdrave {

    struct SourceLocation
    {
        std::string m_filePath;
        int m_offset;
        int m_lineNumber;
        int m_column;
        
        SourceLocation(const std::string& filePath, int offset, int lineNumber, int column):
        m_offset(offset),
        m_filePath(filePath),
        m_lineNumber(lineNumber),
        m_column(column)
        {
            
        }
        
        SourceLocation():
        m_offset(-1),
        m_lineNumber(-1),
        m_column(-1)
        {
            
        }
        
        //TODO: Do this with m_offset (has been added later)
        // Overload operator<= to compare two SourceLocation objects
        bool operator<=(const SourceLocation &other) const {
            if (m_filePath != other.m_filePath)
                return false; // or consider comparing files lexicographically
            return (m_lineNumber < other.m_lineNumber) || (m_lineNumber == other.m_lineNumber && m_column <= other.m_column);
        }

        // Overload operator>= to compare two SourceLocation objects
        bool operator>=(const SourceLocation &other) const {
            if (m_filePath != other.m_filePath)
                return false;
            return (m_lineNumber > other.m_lineNumber) || (m_lineNumber == other.m_lineNumber && m_column >= other.m_column);
        }
        
        // Overload operator>= to compare two SourceLocation objects
        bool operator==(const SourceLocation &other) const {
            return (m_filePath == other.m_filePath && m_lineNumber == other.m_lineNumber && m_column >= other.m_column);
        }
        
        bool isValid() const { return !m_filePath.empty() && m_lineNumber != -1 && m_column != -1; }
    };

    struct SourceVariable
    {
        SourceLocation m_declared;
        SourceLocation m_live;
        std::string m_name;
        std::string m_type;
        int m_arraySize;//0 it not an array, -1 unspecified []
        
        SourceVariable():m_arraySize(0) {}
    };

    struct SourceScope
    {
        enum Type
        {
            FUNCTION,
            LAMBDA,
            CALL,
            FOR,
            WHILE,
            DO,
            IF,
            SWITCH,
            COMPOUND
        } m_type;
        std::string m_srcLine;
        SourceLocation m_start;
        SourceLocation m_end;
        
        //In case the scope is for a lambda function these are explicitly visible variables
        std::set<std::string> m_capturedVariables;
        bool m_captureAll;//By reference or value, it shouldn't matter for printLiveVariables
        
        //name:type
        std::map<std::string, SourceVariable> m_localVariables;
        
        int m_parentIndex; // Use -1 to indicate no parent (i.e. the root)
        std::vector<int> m_childrenIndices;
        
        SourceScope():m_parentIndex(-1) {} //Invalid scope
        
        SourceScope(SourceScope::Type type, const std::string& srcLine,
                    const SourceLocation& start, const SourceLocation& end):
        m_type(type), m_srcLine(srcLine), m_start(start), m_end(end), m_parentIndex(-1) {}
        
        // Member function to check if this scope contains another
        bool contains(const SourceScope &other) const {
            return m_start <= other.m_start && m_end >= other.m_end;
        }
        
        bool contains(const SourceLocation& location) const {
            return m_start <= location && m_end >= location;
        }
        
        // Helper: Check whether the scope contains a given line number.
        bool containsLine(uint32_t lineNumber) const {
            return m_start.m_lineNumber <= lineNumber && m_end.m_lineNumber >= lineNumber;
        }
        
        //Sorted by the order of declaration: SourceLocation m_declared;
        std::vector<std::string> getLocalVariables() const;
        std::vector<std::string> getLocalVariables(const SourceLocation& before) const;
        
        bool isValid() const { return m_start.isValid() && m_end.isValid() && m_start <= m_end; }
        
        bool operator==(const SourceScope &other) const {
            return m_start == other.m_start && m_end == other.m_end;
        }
    };

    struct SourceFunctionCall
    {
        std::string m_functionName;
        SourceLocation m_before;
        SourceLocation m_after;
        std::string m_expression;
    };

    struct SourceReturn
    {
        std::string m_expression;
        SourceLocation m_start;
        SourceLocation m_end;
    };

    struct FunctionDebugInfo
    {
        std::size_t m_hash;
        std::string m_name;
        std::string m_fileName;
        std::vector<SourceScope> m_scopes;
        std::vector<size_t> m_scopeStack;  // indices into m_scopes
        
        std::map<std::string, SourceVariable> m_arguments;
        std::map<std::string, SourceFunctionCall> m_calls;
        
        std::string m_returnType;
        std::vector<SourceReturn> m_returns;
        
        bool enterScope(CXCursor cursor);
        void exitScope();
        
        SourceScope& getRootScope();
        
        SourceScope& getParentScope(const SourceScope& scope);
        SourceScope& getParentScope(const SourceLocation& location);
        void buildScopesHierarchy();
        
        int getScopeIndex(const SourceScope& scope) const;
        
        //The pair is scope_index:variable_name
        std::vector<std::pair<int,std::string>> getLiveVariables(const SourceLocation& before) const;
        
        std::string getFormatedDebugInfo() const;
    };

	class Debugger : public Singleton<Debugger>
	{
        Context m_debugContext;
        std::string m_system;
        std::string m_workingDirectory;
        std::string m_privateWorkingDirectory;
        std::string m_scriptsDirectory;
        std::string m_appInfo;
        
        std::vector<DebugStep> m_runAnalysisSteps;
        //the format is function_name:invocation
        
        uint32_t               m_previousSteps;
        std::string            m_summary;
        std::vector<DebugStep> m_trajectory;
        std::string            m_lldbLog;
        
        uint32_t m_step;
        uint32_t m_runAnalysisStep;
        uint32_t m_attemptsToFixUnitTestMain;
        
        NextDebugStep m_nextStep;
        
        DebugVisibility m_contextVisibility;
        
        std::string             m_compiledInfo;
        
        mutable std::mutex m_dbgInfoMutex;
        mutable std::map<std::string, std::shared_ptr<FunctionDebugInfo>> m_dbgInfoCache;
        
        std::map<std::string, size_t> m_functionHashes;
        std::map<std::string, size_t> m_prevFunctionHashes;
        std::string                   m_functionBeingDebugged;
        
        LogAnalyzer             m_logger;
        TraceAnalyzer           m_tracer;
        
        bool                    m_hasValidBuild;
        
        std::string             m_actionFeedback;
        int                     m_infoStepsStart;
        uint32_t                m_lastRunStep;
        uint32_t                m_lastFixStep;
        std::string             m_lastRunInfo;
        std::string             m_rewardHackingReview;
        std::string             m_lastRunTestLog;
        std::string             m_testFunctionalityDelta;
        std::string             m_unitTestSource;
        std::string             m_commitMessage;
        
        std::string             m_sdkPath;
        
        mutable boost::asio::thread_pool m_threadPool;
        uint16_t m_debugPort;
        
        enum class TraceOptions : uint32_t
        {
            TRACE_NONE          = 0x00000000,
            TRACE_CALL          = 0x00000001,
            TRACE_SCOPE_ENTER   = 0x00000002,
            TRACE_SCOPE_EXIT    = 0x00000004,
            TRACE_RETURN        = 0x00000008,
            TRACE_EXIT          = 0x00000010,
            TRACE_BREAKPOINT    = 0x00000020
        };
        
        std::vector<std::string> parseCommandLine(const std::string &cmdLine) const;
        
        bool execTestScript(CCodeProject* project,
                            const TestDef& test,
                            RunAnalysis& analysis,
                            std::string& traceOnlyLog,
                            std::string& debugTestLog,
                            std::string& debugAppLog,
                            bool instrument);
        
        std::pair<std::string, std::string>
        runTest(std::string& lldbLog,
                            CCodeProject* project,
                            const std::string& executableFilePath,
                            const std::string& testCommandLine,
                            const std::string& workingDir,
                            int timeoutInSeconds, bool instrument,
                            int& returnCode);
        
        std::pair<std::string, std::string>
        runLLDB(CCodeProject* project, std::string& traceLog,
                            const std::string& traceExecutable,
                            const std::string& traceCommandLine,
                            const std::string& workingDir,
                            int timeoutInSeconds, bool instrument,
                            int& returnCode);
        
        std::string runTrace(CCodeProject* project, std::string& traceLog,
                             const std::string& traceExecutable,
                             const std::string& traceCommandLine,
                             const std::string& workingDir,
                             int timeoutInSeconds, bool instrument,
                             int& returnCode);
        
        std::string getRunAnalysisProgress();
        
        void inferenceRunAnalysis(CCodeProject* project, const std::string& prompt, RunAnalysis& analysis, const std::string& debugTitle=std::string());
        void analysisLogSection(CCodeProject* project,
                                  const std::string& logSection,
                                  RunAnalysis& analysis);
        void analysisTrace(CCodeProject* project,
                           const std::string& dbgTestLog,
                           const std::string& traceLog,
                           RunAnalysis& analysis, const TestDef& test);
        
        void logAnalysis(CCodeProject* project,
                         const std::string& debugLogTestStr,
                         RunAnalysis& analysis);
        
        std::string getSubSystemsData(CCodeProject* project, std::set<std::string>& subSystems);
        void systemAnalysis(CCodeProject* project, const std::string& hint, RunAnalysis& analysis);
        
        bool rewardHackingAnalysis(CCodeProject* project,
                                   const TestDef& test,
                                   std::string& review,
                                   std::string& hint);
        
        std::set<std::string> getTestTextFiles(CCodeProject* project, const TestDef& test, const std::string& workingDirectory);
        
        std::string getStackTrace(CCodeProject* project, const std::string& stack, bool log, uint32_t maxTailToPrint);
        
        std::pair<bool, std::string> analysisFullTrace(CCodeProject* project, RunAnalysis& analysis, const TestDef& test);
        std::string analysisFrameTrace(CCodeProject* project, const std::string& function, uint32_t invocation);
        
        std::string getTrajectory(int fromStep, int toStep, bool addCurrent, bool addSummary, bool includeRunInfo = false) const;
        void debugAnalysis(CCodeProject* project,
                           const std::string& function,
                           RunAnalysis& analysis,
                           const TestDef& test);
        
        void checkTestStepInput(std::ostream& log,
                                CCodeProject* project,
                                const std::vector<std::shared_ptr<std::string>>& input_files,
                                const std::vector<std::shared_ptr<std::string>>& output_files,
                                const std::string& stepName,
                                bool deleteOutput);
        
        void checkTestStepInput(std::ostream& log, CCodeProject* project, const TestStep& step, const std::string& stepName, bool deleteOutput);
        
        void checkTestStepOutput(std::ostream& log,
                                 CCodeProject* project,
                                 const std::vector<std::shared_ptr<std::string>>& output_files,
                                 const std::string& stepName);
        
        void checkTestStepOutput(std::ostream& log, CCodeProject* project, const TestStep& step, const std::string& stepName);
        
        bool executeTestStep(std::ostream& log, CCodeProject* project, const TestStep& step, const std::string& stepName, bool enforceResult0);
        
        void runAnalysis(CCodeProject* project, const TestDef& test, RunAnalysis& analysis, bool analyzeLog);

        std::string getTestDescription(CCodeProject* project, const TestDef& test, const std::string& regressionTestPath);
        
        std::string getHighLevelAppInfo(CCodeProject* project, const std::string& functionName,
                                        int functionsMaxDepth, int callGraphMaxDepth);
        std::string getRequestedInfo(CCodeProject* project, int allFunctions, int callGraph,
                                     const std::string& functionName,
                                     const std::vector<std::shared_ptr<std::string>>& functions,
                                     const std::vector<std::shared_ptr<std::string>>& data_types,
                                     const std::vector<std::shared_ptr<std::string>>& test_files);
        
        std::string getFileInfo(CCodeProject* project, const std::string& file, int lineToStartFrom, int maxCharacters);
        std::string getStepInfo(CCodeProject* project, const TestDef& test, int stepId);
        
        void optimizeTrajectory(CCodeProject* project, const TestDef& test);
        bool saveTrajectory(CCodeProject* project, const TestDef& test);
        bool loadTrajectory(CCodeProject* project, const TestDef& test);
        
        std::string loadTestLogFromStep(CCodeProject* project, const TestDef& test, uint32_t debugStepId);
        std::string loadTestLogForStep(CCodeProject* project, const TestDef& test, const TestStep& step, const std::string& testStepName, uint32_t debugStepId);
        
        std::string stepLogInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& stepInfo);
        std::string stepTraceInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo);
        std::string stepFunctionInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int invocation, int lineNumber, DebugStep& stepInfo);
        std::string stepDataInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo);
        std::string stepFileInfo(CCodeProject* project, const std::string& subject, const std::string& motivation, int lineNumber, DebugStep& stepInfo);
        std::string stepFunctionsSummary(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo);
        std::string stepCallGraph(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo);
        std::string stepHistory(CCodeProject* project, const std::string& motivation, const TestDef& test, int invocation, DebugStep& stepInfo);
        std::string stepSearchSource(CCodeProject* project, const std::string& subject, const std::string& motivation, DebugStep& stepInfo);
        
        bool executeNextStep(CCodeProject* project, const TestDef& test);
        std::string compileContext(CCodeProject* project, const TestDef& test, uint32_t maxSize);
        std::string validateStep(CCodeProject* project, const TestDef& test, int attempt);
        void setStepHint(const TestDef& test);
        
        bool isOnTrack(const std::string& queryTrajectory);
        
        
        std::shared_ptr<FunctionDebugInfo> getFunctionDebugInfo(CCodeProject* project, const std::string& functionName) const;
        
        bool checkFunctionExists(CCodeProject* project, const std::string& functionName, std::string& debugNode) const;
        
        std::vector<SourceScope> getScopes(CCodeProject* project, const std::string& functionName);
        
        bool debugFunctionScopes(const std::shared_ptr<FunctionDebugInfo>& debugInfo, std::vector<std::string>& lldbArgs, bool infoForRoot) const;
        bool debugFunctionReturns(const std::shared_ptr<FunctionDebugInfo>& debugInfo, bool extended, std::vector<std::string>& lldbArgs, int assignedBpId) const;
        
        int debugScope(const SourceScope& scope, bool info, bool extendedFrame, bool printInOut, bool debugEnd, std::vector<std::string>& lldbArgs) const;
        bool debugLocation(const SourceLocation& location, std::vector<std::string>& lldbArgs, bool stepIn, int stepsCount=10) const;
        
        int extractProcessId(const std::string& text) const;
        
        int mapLine(CCodeProject* project, const std::string& filePath, const std::string& functionName, int lineNumber) const;
        int mapLine(CCodeProject* project, const std::string& filePath, const std::string& functionName, const std::string& functionSource, int lineNumber) const;
        
        std::string printFunctionSource(CCodeProject* project, const std::string& functionName, const std::string source) const;
        
        std::string getFunctionDetailedInfo(CCodeProject* project, const std::string& functionName) const;
        std::string getVisibleDataTypes(CCodeProject* project, CCodeNode* ccNode, bool checkContext, std::set<std::string>& referencedNodes);
        std::string ensureFunctionIsVisible(CCodeProject* project, const std::string& functionName, bool checkContext);
        
        std::string breakpoint(std::vector<std::string>& lldbArgs,
                               const std::string& filePath,
                               int line,
                               const std::string& condition,
                               const std::string& expression) const;
        
        void setBreakpoints(std::vector<std::string>& lldbArgs, CCodeProject* project, const std::string& functionName,
                            const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints);
        
        std::string getBreakpointsInfo(bool instrumented, bool command, const std::string& function,
                                       const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const;
        
        std::string evaluateBreakpoints(CCodeProject* project, const std::string& functionName, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const;
        
        std::pair<std::string, std::string> debugFunction2(CCodeProject* project, const TestDef& test, const std::string& functionName,
                                                          const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints);
        
        std::pair<std::string, std::string> lldbLogSections(const std::string& lldbLog);
        
        std::string fixFunction(CCodeProject* project, const TestDef& test, const std::string& functionName, std::string& before, std::string& debugNotes);
        
        void criticalError(const std::string& debugNotes);
        
        void startCodeInstrumentation(CCodeProject* project,
                                      const std::string& prevDebuggedFunction);
        
        void switchToInstrumentedBuild(CCodeProject* project) const;
        void switchToDefaultBuild(CCodeProject* project) const;
        void restoreSource(CCodeProject* project);
        
        bool compileFunction(CCodeProject* project, const std::string& functionName) const;
        bool compile(CCodeProject* project) const;
        void generateTraceSources(CCodeProject* project, const std::string& debugFunctionName, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const;
        std::string generateTraceCode(CCodeProject* project, const std::string& functionName, uint32_t traceOptions, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints) const;
        std::string getFrameSnippet(CCodeProject* project, const std::string& functionName, int line, int column, int addedLinesCount) const;
        
        std::pair<std::string, std::string> getLiveVariables(CCodeProject* project,
                                                             std::shared_ptr<FunctionDebugInfo> dbgInfo,
                                                             int line, int column,
                                                             std::vector<std::string>& paramList,
                                                             std::vector<std::string>& argList) const;
        
        std::string generateTracePoint(CCodeProject* project,
                                       std::shared_ptr<FunctionDebugInfo> dbgInfo,
                                       int line, int column,
                                       std::string& liveVarParams,
                                       std::string& liveVarArgs,
                                       bool attachToFunction) const;
        std::pair<std::string, std::string> generateTracePointDeclCall(CCodeProject* project,
                                                                       std::shared_ptr<FunctionDebugInfo> dbgInfo,
                                                                       int line, int column) const;
        
        std::string getOriginalSource(CCodeProject* project, const std::string& filePath) const;
        std::string assembleBreakpointCode(CCodeProject* project, const std::string& functionName, std::shared_ptr<Breakpoint> bp) const;
        std::string getInstrumentedPath(CCodeProject* project, const std::string& path) const;
        bool instrumentFunction(CCodeProject* project, const std::string& functionName, uint32_t traceOptions, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints = {}) const;
        void instrumentSource(CCodeProject* project, const std::string& debugFunctionName = {}, const std::vector<std::shared_ptr<Breakpoint>>& customBreakpoints = {});
        
        void debugLoadTrace(const std::string& root, CCodeProject* project, const std::string& function, const TestDef& test);
        
        bool isFunctionChanged(CCodeProject* project,
                                 const std::string& function,
                                 const std::string& debuggedFunction,
                                 const std::string& prevDebuggedFunction);
        
        std::pair<int, std::string> extractFirstTruncationTag(const std::string& log);
        
        void prebuildDebugInfo(CCodeProject* project);
        
        void resetTest();
        bool visibleTraceAndLog(std::ostream& log, const std::pair<std::string, uint32_t>& frame);
        std::string generateConfig();
        
        bool deployToWorkingDirectory(CCodeProject* project, const std::string& testJsonDir, bool isPublic, TestDef& test);
        bool saveTestToDirectory(CCodeProject* project, const std::string& testJsonDir, const std::string& testDirectory, TestDef& test);
        
        void reviewGiHistoryForFix(CCodeProject* project);
        
	public:
        std::pair<bool, std::string> debug(CCodeProject* project,
                   int stepsCount,
                   const std::string& system,
                   const std::string& testJsonPath,
                   const std::string& privateTestJsonPath,
                   const std::string& regressionTestJsonPath,
                   uint16_t debugPort);
        
        bool debug(CCodeProject* project, const std::string& executableFilePath,
                                          const std::string& testCommandLine,
                                          const std::string& workingDir,
                                          const std::string& functionToDebug,
                                          const std::set<std::string>& calledFunctions);
        
        bool debugPretest(CCodeProject* project,
                          const std::string& system,
                          const std::string& testJsonPath,
                          std::string& log);
        
        void feedback(const std::string& message);
        Debugger();
	};

}
