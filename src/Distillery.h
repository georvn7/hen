#pragma once

#include <cpprest/json.h>

#include "Singleton.h"
#include "Test.h"
#include "CCodeProject.h"
#include "DebugContextProvider.h"

#include "TraceAnalyzer.h"
#include "LogAnalyzer.h"

namespace stdrave {

    class TrajectoryAnalysis : public Reflection<TrajectoryAnalysis>
    {
        public:
        DECLARE_TYPE(TrajectoryAnalysis, "Contains information about scoring important test-fix sequences in a trajectory. "\
                    "Each sequence spans from a run_test step through all information-gathering actions to the final fix_function action")
        DECLARE_ARRAY_FIELD(uint32_t, blockers, "Sequences that fix critical blockers that must be fixed in order to pass the test")
        DECLARE_ARRAY_FIELD(uint32_t, regressions, "Sequences that introduced regression while attempting to fix critical blockers")
        DECLARE_ARRAY_FIELD(uint32_t, contributors, "Sequences that somehow contributed passing the test but weren't strictly necessary "\
                                                              "(e.g., optimizations, edge case handling, enabling conditions).")
        DECLARE_ARRAY_FIELD(uint32_t, unnecessary, "Sequences that had no impact on test success or introduced regressions while addressing non-critical "\
                                                             "issues. Changes that better to be filtered from the solution path.")
        DECLARE_FIELD(std::string, analysis, "Detailed analysis about the reasons to score each step from the sequences")
    };

    class DistilledStep : public Reflection<DistilledStep>
    {
        public:
        DECLARE_TYPE(DistilledStep, "Synthetically distilled debug step")
        DECLARE_FIELD(NextDebugStep, debug_step, "Debug step as it would appear in a debug trajectory")
        DECLARE_FIELD(std::string, motivation_summary, "Single line concise summary of the motivation. "\
                      "What you learned from the provided information so far and why the next step is selcted")
        DECLARE_FIELD(std::string, analysis, "Analysis for training reasoning LLMs")
        
        std::string m_debugNotes;
        std::string m_logSummary;
        int32_t m_originalStep;
    };

    class OptimizedStep : public Reflection<OptimizedStep>
    {
        public:
        DECLARE_TYPE(OptimizedStep, "Synthetically distilled debug step")
        DECLARE_ENUM_FIELD(action_type, "\"log_info\",\"function_info\",\"data_info\",\"file_info\",\"functions_summary\",\"call_graph\",\"step_info\",\"search_source\",\"fix_function\",\"refactor_data\",\"new_data\",\"debug_function\",\"run_test\"",\
                           "Type of the action performed as a next step")
        DECLARE_FIELD(std::string, action_subject, "Subject of the specified action. Could be a function name, a data type name or a file name. If no subject required for the action, then this field must be set to 'none'")
        DECLARE_FIELD(uint32_t, line_number, "Line number to the relevant actions like 'log_info', 'file_info'. If not needed must be 0")
        DECLARE_FIELD(uint32_t, invocation, "Function invocation to the relevant actions like 'log_info' and 'function_info'. Or step index for the 'step_info' action. If not needed must be 1")
        DECLARE_FIELD(int32_t, original_step, "The id (index) of the step from the original trajectory " \
                      "that matches this step or -1 if this step doesn't exist in the original trajectory. " \
                      "For debug_function steps this must be correspond to a step from the original trajectory")
    };

    class DistilledAanalysis : public Reflection<DistilledAanalysis>
    {
        public:
        DECLARE_TYPE(DistilledAanalysis, "Synthetically distilled debug step")
        DECLARE_FIELD(RunAnalysis, system_analysis, "Debug step as it would appear in a debug trajectory")
        DECLARE_FIELD(std::string, thinking_analysis, "Chain-of-thought to be uset to train reasoning LLMs")
    };

    class EditSourceSequence : public Reflection<EditSourceSequence>
    {
        public:
        DECLARE_TYPE(EditSourceSequence, "A sequence spans from a run_test step through all information-gathering actions to the final fix_function action")
        DECLARE_ARRAY_FIELD(OptimizedStep, steps, "Optimal sequence of debugging steps")
        DECLARE_FIELD(std::string, analysis, "Detailed analysis why this sequence is optimal. Why the selected actions are appropriate "\
                      "and what is the logic to select those actions based on the information available from the previos actions")
        
        void clear()
        {
            steps.clear();
            analysis.clear();
        }
    };

    class Distillery : public Singleton<Distillery>
    {
        Context                 m_distilleryContext;
        
        TestDef                 m_test;
        
        int                     m_currentFixIndex;
        CCodeProject*           m_project;
        
        DebugContextProvider    m_debugContext;
        
        std::vector<web::json::value> m_dataset;
        std::string m_system;
        
        std::string m_dbgSystemPrompt;
        std::string m_projDesc;
        std::string m_nextStepPrompt;
        std::string m_debugAnalysisPrompt;
        
        int m_fromStep;
        std::vector<DebugStep> m_trajectory;
        
        std::map<int, std::set<std::string>> m_newFunctionsPerStep;
        std::set<std::string> m_newFunctions;
        
        std::vector<std::pair<int,int>> m_mergedFixes;
        
        bool loadTrajectory(CCodeProject* project, const TestDef& test, int fromStep, int toStep);
        
        std::pair<bool, uint32_t> findRequestIdForPattern(const boost_fs::path& dir, const std::string& prefix, const std::string& suffix);
        std::pair<int,int> findRequestsIdRange(const std::string& folderPath);
        
        std::vector<std::string> collectReqResInRange(const std::string& folderPath,
                                                             int id_lo, int id_hi,
                                                             const std::string& kind = "request",
                                                             const std::string& filter = "");
        
        std::set<std::string> getFunctionsDefinedInStep(CCodeProject* project, const TestDef& test, int step);
        
        std::string checkoutExact(const std::string& folder,
                                  const std::string& commitish,
                                  bool autoRestore /*=false*/,
                                  bool updateSubmodules /*=false*/,
                                  bool checkoutParent /*=false*/);
        
        struct CommitInfo
        {
            std::string hash;
            int         fixIndex;
            bool        checkoutParent;
        };
        
        struct StepInfo
        {
            std::string request;
            std::string response;
        };
        
        CommitInfo findCommit(CCodeProject* project, int step);
        void goTo(CCodeProject* project, int step);
        
        int getLastIndexBefore(int step, const std::string& action);
        int getFirstIndexAfter(int step, const std::string& action);
        
        std::string functionBrief(CCodeProject* project, int toStep, const std::string& functionName);
        
        inline int stepToTrajectoryIndex(int step)
        {
            assert(step >= 1);
            
            int index = (step-1) - m_fromStep;
            
            assert(index >= 0 && index < m_trajectory.size());
            
            return index;
        }
        
        inline int trajectoryIndexToStep(int index)
        {
            if(index < 0) return index;
            
            return m_fromStep + index + 1;
        }
        
        int getSummaryStepForStep(CCodeProject* project, int step, std::string& summary);
        
        std::string distillSummaryBefore(CCodeProject* project, int runStep, int fixStep);
        std::string getTrajectoryPrologue(CCodeProject* project, int originalRunStep, int distilledRunStep, const std::string& summary);
        std::string getOriginalTrajectory(CCodeProject* project, int stepFromIdx, int stepToIdx);
        std::pair<std::string, std::string> validateSequence(CCodeProject* project, const EditSourceSequence& optimalSequence, int originalSize, int startStep);
        void optimizeFixTrack(CCodeProject* project, Cache& cache, const std::string& trajectoryAnalysis, const std::string& fixTrack, uint32_t fixStep, EditSourceSequence& optimalSequence);
        
        bool checkFixTrackData(CCodeProject* project, uint32_t startStep, uint32_t fixStep);
        void removeFixTrackData(CCodeProject* project, uint32_t startStep, uint32_t fixStep);
        
        void distillFixTrack(CCodeProject* project, const std::string& trajectoryAnalysis, uint32_t fixStep);
        std::string distillStep(CCodeProject* project,
                                int originalStep,
                                int lastOriginalRunStep,
                                int fixStep, int step, const std::string& summary,
                                std::string& prevSteps,
                                const std::string& requestedInfo,
                                std::string& newInfo,
                                DistilledStep& nextStep);
        
        web::json::value buildTrainingData(CCodeProject* project, const std::string& trajectory, const std::string& content, const std::string& thinking);
        
        std::string rebuildRequestedInfo(CCodeProject* project, const std::vector<DistilledStep>& trajectory, int newDebugStep);
        
        void scoreTrajectory(CCodeProject* project, const std::string& testDirectory, const std::string& mergedTrajectory);
        
        //From run_test to run_test
        std::pair<int, int> getFixTrackRange(CCodeProject* project, int fixStep);
        
        std::pair<bool, web::json::value> distillResponse(CCodeProject* project, const std::string& sufix, int step,
                             web::json::value& schema, const std::string& promptFile,
                             const std::function<Prompt(const std::string&, const std::string&)> &buildPrompt);
        
        
        std::pair<bool, std::string> thinkingForResponse(CCodeProject* project, const std::string& sufix, int step,
                                                         web::json::value& request, web::json::value& response);
        
        void saveSystemAnalysis(const std::string& datasetDir,
                                const std::string& fileName,
                                DistilledAanalysis& systemAnalysis,
                                const std::string& sysAnalysisRequest);
        
        void saveDebugAnalysis(CCodeProject* project,
                               const std::string& datasetDir,
                                const std::string& fileName,
                                DistilledAanalysis& systemAnalysis,
                                const std::string& sysAnalysisRequest);
        
        void saveResponseForTraining(const std::string& datasetDir,
                                     const std::string& fileName,
                                     const std::string& request,
                                     const std::string& thinking,
                                     const std::string& response,
                                     bool responseAsJson);
        
        bool distillRunStep(CCodeProject* project, const std::string& summary, const std::string& fixedFunction, int testStep, int fixStep, std::string& debugNotes);
        
        std::string distillDebugStep(CCodeProject* project,
                                     const std::string& summary,
                                     std::string& prevSteps,
                                     DistilledStep& distilledStep,
                                     int originalStep,
                                     int testStep, int debugStep);
        
        void saveTrainingData(const std::string& datasetDir, const std::string& sampleName, web::json::value& chatSample, bool jsonReponse);
        void compileDataset(const std::string& datasetDir);
        
        std::pair<std::string, std::string> getChat(CCodeProject* project, const std::string& sufix, int step);
        
        std::string prevStepsSummary(const std::vector<DistilledStep>& distilledTrajectory, int startStep);
        
    public:
        
        void distillDag();
        
        uint32_t loadTrajectory(CCodeProject* project, const std::string& testJsonPath, int fromStep, int toStep);
        std::string printTrajectory();
        void distillTrajectory(CCodeProject* project, const std::string& testJsonPath, int fromStep, int toStep);
        
        void printTrajectoryInfo();
        void printFixesAndTests();
        void printMergedFixesAndTests();
        std::string mergeFixes();
        std::string printFixInfo(int step);
        
        std::string functionInfo(CCodeProject* project, int toStep, const std::string& functionName, int invocation, int lineNumber);
        
        std::string trackForFix(CCodeProject* project, int fixStep);
        
        void clear();
        
        Distillery();
        
    };
}
