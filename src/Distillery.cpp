#include "Distillery.h"
#include "Client.h"
#include <algorithm>

namespace hen {

    DEFINE_TYPE(TrajectoryAnalysis)
    DEFINE_ARRAY_FIELD(TrajectoryAnalysis, blockers)
    DEFINE_ARRAY_FIELD(TrajectoryAnalysis, regressions)
    DEFINE_ARRAY_FIELD(TrajectoryAnalysis, contributors)
    DEFINE_ARRAY_FIELD(TrajectoryAnalysis, unnecessary)
    DEFINE_FIELD(TrajectoryAnalysis, analysis)

    DEFINE_TYPE(DistilledStep)
    DEFINE_FIELD(DistilledStep, debug_step)
    DEFINE_FIELD(DistilledStep, motivation_summary)
    DEFINE_FIELD(DistilledStep, analysis)

    DEFINE_TYPE(OptimizedStep)
    DEFINE_FIELD(OptimizedStep, action_type)
    DEFINE_FIELD(OptimizedStep, action_subject)
    DEFINE_FIELD(OptimizedStep, line_number)
    DEFINE_FIELD(OptimizedStep, invocation)
    DEFINE_FIELD(OptimizedStep, original_step)

    DEFINE_TYPE(DistilledAanalysis)
    DEFINE_FIELD(DistilledAanalysis, system_analysis)
    DEFINE_FIELD(DistilledAanalysis, thinking_analysis)

    DEFINE_TYPE(EditSourceSequence)
    DEFINE_ARRAY_FIELD(EditSourceSequence, steps)
    DEFINE_FIELD(EditSourceSequence, analysis)

    static void addMentionedFunctionsFromText(const std::string& text,
                                              const std::set<std::string>& candidates,
                                              std::set<std::string>& out);

    static bool tryParseJsonText(const std::string& text, web::json::value& out)
    {
        try
        {
            out = web::json::value::parse(utility::conversions::to_string_t(text));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static bool normalizeActionPayload(web::json::value& actionObj, std::string& reason)
    {
        reason.clear();
        if (!actionObj.is_object())
        {
            reason = "action payload is not a JSON object";
            return false;
        }

        if (!actionObj.has_field(U("action_type")) || !actionObj.at(U("action_type")).is_string())
        {
            reason = "action payload missing string field 'action_type'";
            return false;
        }

        if (!actionObj.has_field(U("action_subject")) || !actionObj.at(U("action_subject")).is_string())
        {
            reason = "action payload missing string field 'action_subject'";
            return false;
        }

        if (!actionObj.has_field(U("motivation")) || !actionObj.at(U("motivation")).is_string())
        {
            actionObj[U("motivation")] = web::json::value::string(U(""));
        }

        if (!actionObj.has_field(U("breakpoints")) || !actionObj.at(U("breakpoints")).is_array())
        {
            actionObj[U("breakpoints")] = web::json::value::array();
        }

        int invocation = 1;
        if (actionObj.has_field(U("invocation")))
        {
            const auto& inv = actionObj.at(U("invocation"));
            if (!inv.is_number())
            {
                reason = "action payload field 'invocation' is not numeric";
                return false;
            }

            try
            {
                invocation = inv.as_integer();
            }
            catch (...)
            {
                reason = "action payload field 'invocation' cannot be converted to integer";
                return false;
            }
        }

        if (invocation <= 0)
        {
            invocation = 1;
        }
        actionObj[U("invocation")] = web::json::value::number(invocation);

        int lineNumber = 0;
        if (actionObj.has_field(U("line_number")))
        {
            const auto& ln = actionObj.at(U("line_number"));
            if (!ln.is_number())
            {
                reason = "action payload field 'line_number' is not numeric";
                return false;
            }

            try
            {
                lineNumber = ln.as_integer();
            }
            catch (...)
            {
                // Handle very large numbers (e.g. UINT_MAX rendered in JSON)
                double lnDouble = ln.as_double();
                if (lnDouble > 2147483647.0 || lnDouble < -2147483648.0)
                {
                    lineNumber = 0;
                }
                else
                {
                    lineNumber = static_cast<int>(lnDouble);
                }
            }
        }

        if (lineNumber < 0)
        {
            lineNumber = 0;
        }
        actionObj[U("line_number")] = web::json::value::number(lineNumber);

        return true;
    }

    static bool sanitizeTrainingSampleByName(const std::string& sampleName,
                                             web::json::value& chatSample,
                                             std::string& reason)
    {
        reason.clear();

        if (!chatSample.is_object() ||
            !chatSample.has_field(U("messages")) ||
            !chatSample.at(U("messages")).is_array())
        {
            reason = "sample has no valid messages array";
            return false;
        }

        auto& messages = chatSample[U("messages")].as_array();
        if (messages.size() == 0)
        {
            reason = "messages array is empty";
            return false;
        }

        // Basic role/content shape validation for all turns.
        for (size_t i = 0; i < messages.size(); ++i)
        {
            if (!messages[i].is_object())
            {
                reason = "message[" + std::to_string(i) + "] is not an object";
                return false;
            }

            auto& msg = messages[i];
            if (!msg.has_field(U("role")) || !msg.at(U("role")).is_string())
            {
                reason = "message[" + std::to_string(i) + "] missing string role";
                return false;
            }

            if (!msg.has_field(U("content")) || !msg.at(U("content")).is_string())
            {
                reason = "message[" + std::to_string(i) + "] missing string content";
                return false;
            }

            // Normalize any assistant action payload in interleaved history.
            const std::string role = utility::conversions::to_utf8string(msg.at(U("role")).as_string());
            if (role == "assistant")
            {
                const std::string content =
                utility::conversions::to_utf8string(msg.at(U("content")).as_string());
                web::json::value parsed;
                if (tryParseJsonText(content, parsed) && parsed.is_object() &&
                    parsed.has_field(U("action_type")))
                {
                    std::string localReason;
                    if (!normalizeActionPayload(parsed, localReason))
                    {
                        reason = "assistant action payload in history is invalid: " + localReason;
                        return false;
                    }

                    msg[U("content")] = web::json::value::string(parsed.serialize());
                }
            }
        }

        auto& last = messages[messages.size() - 1];
        if (!last.is_object() || !last.has_field(U("content")) || !last.at(U("content")).is_string())
        {
            reason = "last message has no string content";
            return false;
        }

        std::string lastContent = utility::conversions::to_utf8string(last.at(U("content")).as_string());

        if (startsWith(sampleName, "step_"))
        {
            web::json::value parsed;
            if (!tryParseJsonText(lastContent, parsed))
            {
                reason = "step sample assistant content is not valid JSON";
                return false;
            }

            std::string localReason;
            if (!normalizeActionPayload(parsed, localReason))
            {
                reason = "step sample assistant content invalid: " + localReason;
                return false;
            }

            last[U("content")] = web::json::value::string(parsed.serialize());
            return true;
        }

        if (startsWith(sampleName, "system_") || startsWith(sampleName, "debug_"))
        {
            web::json::value parsed;
            if (tryParseJsonText(lastContent, parsed) && parsed.is_object() &&
                parsed.has_field(U("debug_notes")) && parsed.has_field(U("log_summary")) &&
                parsed.at(U("debug_notes")).is_string() && parsed.at(U("log_summary")).is_string())
            {
                return true;
            }

            // Enforce schema shape even when model returned free text.
            RunAnalysis normalized;
            normalized.debug_notes = trim(lastContent);
            normalized.log_summary = std::string();
            last[U("content")] = web::json::value::string(normalized.to_json().serialize());
            return true;
        }

        return true;
    }

    static std::string summarizeFeedbackText(const std::string& s)
    {
        const std::size_t kMax = 600;
        if (s.size() <= kMax)
        {
            return s;
        }

        return s.substr(0, kMax) + "...";
    }

    static std::string buildDisclosureContractText(const std::map<int, StepDisclosureMapEntry>& disclosure,
                                                   int maxVisiblePerStep = 40)
    {
        if (disclosure.empty())
        {
            return "No per-step disclosure data available.";
        }

        std::string out;
        out += "DISCLOSURE CONTRACT (per-step visible functions in distilled trajectory context)\n";
        out += "Use this as hard grounding constraint when selecting action_subject/search_source.\n\n";

        for (const auto& perStep : disclosure)
        {
            const int stepId = perStep.first;
            const auto& entry = perStep.second;

            out += "STEP " + std::to_string(stepId) + ":\n";
            out += "  visible_functions: " + getAsCsv(entry.visible_functions, maxVisiblePerStep) + "\n";
            out += "  fixable_functions (prior function_info): " + getAsCsv(entry.fixable_functions, maxVisiblePerStep) + "\n\n";
        }

        return out;
    }

    static std::string buildAllowedCandidatesLine(const std::set<std::string>& visibleFunctions,
                                                  const std::string& disallowedSubject,
                                                  int maxCandidates = 30)
    {
        if (visibleFunctions.empty())
        {
            return "<none>";
        }

        std::vector<std::pair<int, std::string>> ranked;
        ranked.reserve(visibleFunctions.size());

        auto commonPrefixLen = [&](const std::string& a, const std::string& b) {
            int n = 0;
            while (n < (int)a.size() && n < (int)b.size() && a[n] == b[n]) ++n;
            return n;
        };

        for (const auto& fn : visibleFunctions)
        {
            int score = commonPrefixLen(fn, disallowedSubject);
            if (!disallowedSubject.empty() && fn.find(disallowedSubject) != std::string::npos)
            {
                score += 4;
            }
            if (!disallowedSubject.empty() && disallowedSubject.find(fn) != std::string::npos)
            {
                score += 2;
            }

            ranked.push_back({score, fn});
        }

        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<int, std::string>& a,
                     const std::pair<int, std::string>& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

        std::string out;
        int emitted = 0;
        for (const auto& item : ranked)
        {
            if (emitted >= maxCandidates) break;
            if (!out.empty()) out += ", ";
            out += item.second;
            ++emitted;
        }

        const int remaining = static_cast<int>(ranked.size()) - emitted;
        if (remaining > 0)
        {
            out += ", ... (+" + std::to_string(remaining) + " more)";
        }

        return out;
    }
    
    Distillery::Distillery()
    {
        m_fromStep = 0;
        m_currentFixIndex = -1;
        m_project = nullptr;
    }

    std::string Distillery::checkoutExact(const std::string& folder,
                                          const std::string& commitish,
                                          bool autoRestore /*=false*/,
                                          bool updateSubmodules /*=false*/,
                                          bool checkoutParent /*=false*/)
    {
        const boost_fs::path repo = boost_fs::absolute(folder);
        if (!boost_fs::exists(repo) || !boost_fs::is_directory(repo)) {
            throw std::runtime_error("checkoutExact(): folder does not exist or is not a directory: " + repo.string());
        }
        if (!boost_fs::exists(repo / ".git")) {
            throw std::runtime_error("checkoutExact(): not a git repo: " + repo.string());
        }

        const std::string repoQ   = shQuote(repo.string());
        const std::string targetQ = shQuote(commitish);

        // Validate that `commitish` resolves to a commit
        (void)exec("git -C " + repoQ + " cat-file -e " + targetQ + "^{commit}",
                   repo.string(), "gitVerifyCommit", true);

        // Resolve effective target to an exact hash (and optionally its first parent)
        std::string effectiveTarget;
        if (checkoutParent) {
            try {
                effectiveTarget =
                    trim(exec("git -C " + repoQ + " rev-parse " + targetQ + "^",
                              repo.string(), "gitRevParseParent", true));
            } catch (...) {
                throw std::runtime_error(
                    "checkoutExact(): requested before-commit snapshot, but '" +
                    commitish + "' has no parent (root commit or invalid rev)."
                );
            }
        } else {
            effectiveTarget =
                trim(exec("git -C " + repoQ + " rev-parse " + targetQ,
                          repo.string(), "gitRevParseTarget", true));
        }
        const std::string effectiveTargetQ = shQuote(effectiveTarget);

        // Check cleanliness INCLUDING untracked (so checkout can't be blocked)
        const std::string porcelain =
            trim(exec("git -C " + repoQ + " status --porcelain --untracked-files=all",
                      repo.string(), "gitStatusPorcelain", true));

        if (!porcelain.empty()) {
            if (!autoRestore) {
                throw std::runtime_error(
                    "checkoutExact(): working tree not clean; pass autoRestore=true to auto-restore clean state."
                );
            }

            // Exploration semantics: discard all local modifications + untracked files
            (void)exec("git -C " + repoQ + " reset --hard",
                       repo.string(), "gitResetHard", true);
            (void)exec("git -C " + repoQ + " clean -fd",
                       repo.string(), "gitCleanFd", true);
            // If you also want to remove ignored build artifacts, use -fdx instead of -fd.
            // (void)exec("git -C " + repoQ + " clean -fdx", repo.string(), "gitCleanFdx", true);
        }

        // Prefer modern switch; fall back to checkout
        try {
            (void)exec("git -C " + repoQ + " switch --detach " + effectiveTargetQ,
                       repo.string(), "gitSwitchDetach", true);
        } catch (...) {
            (void)exec("git -C " + repoQ + " checkout --detach " + effectiveTargetQ,
                       repo.string(), "gitCheckoutDetach", true);
        }

        if (updateSubmodules) {
            (void)exec("git -C " + repoQ + " submodule sync --recursive",
                       repo.string(), "gitSubmoduleSync", true);
            (void)exec("git -C " + repoQ + " submodule update --init --recursive",
                       repo.string(), "gitSubmoduleUpdate", true);
        }

        // Report where we landed + verify (so aborted checkout can't silently pass)
        const std::string headHash =
            trim(exec("git -C " + repoQ + " rev-parse HEAD",
                      repo.string(), "gitRevParseHead", true));

        if (headHash != effectiveTarget) {
            throw std::runtime_error(
                "checkoutExact(): expected HEAD " + effectiveTarget +
                " but got " + headHash + " (checkout may have been blocked)."
            );
        }

        return headHash;
    }

    int Distillery::getLastIndexBefore(int step, const std::string& action)
    {
        if(step <= m_fromStep)
        {
            //error
            return -1;
        }
        
        int stepIndex = step - m_fromStep - 1;
        
        for(int i=stepIndex; i >= 0; --i)
        {
            if(m_trajectory[i].m_action == action)
            {
                return i;
            }
        }
        
        return -1;
    }

    int Distillery::getFirstIndexAfter(int step, const std::string& action)
    {
        if(step <= m_fromStep)
        {
            //error
            return -1;
        }
        
        if(step - m_fromStep >= m_trajectory.size())
        {
            return -1;
        }
        
        int stepIndex = step - m_fromStep - 1;
        
        for(int i=stepIndex; i < m_trajectory.size(); ++i)
        {
            if(m_trajectory[i].m_action == action)
            {
                return i;
            }
        }
        
        return -1;
    }

    std::string Distillery::functionBrief(CCodeProject* project, int toStep, const std::string& functionName)
    {
        return "";
    }

    std::string Distillery::functionInfo(CCodeProject* project, int toStep, const std::string& functionName, int invocation, int lineNumber)
    {
        goTo(project, toStep);
        
        DebugStep stepInfo;
        std::string info = m_debugContext.stepFunctionInfo(m_project, functionName, "", invocation, lineNumber, stepInfo);
        return info;
    }

    std::pair<int, int> Distillery::getFixTrackRange(CCodeProject* project, int fixStep)
    {
        int fixStepIndex = stepToTrajectoryIndex(fixStep);
        
        int startFixIndex;
        int endFixIndex;
        for(auto fix : m_mergedFixes)
        {
            if(fix.first <= fixStepIndex && fixStepIndex <= fix.second)
            {
                startFixIndex = fix.first;
                endFixIndex = fix.second;
                break;
            }
        }
        
        int startFixStep = trajectoryIndexToStep(startFixIndex);
        int firstRunIndex = getLastIndexBefore(startFixStep, "run_test");
        //int nextRunStep = fixStep + 1;

        int lastRunIndex = endFixIndex + 1; //stepToTrajectoryIndex(nextRunStep);
        
        return std::make_pair(firstRunIndex, lastRunIndex);
    }

    std::string Distillery::trackForFix(CCodeProject* project, int fixStep)
    {
        auto range = getFixTrackRange(project, fixStep);
        
        m_debugContext.clear();
        
        std::string contextInfo;
        std::string lastFunctionName;
        for(int i=range.first; i<range.second; ++i)
        {
            int s = trajectoryIndexToStep(i);
            
            contextInfo += "\nSTEP: " + std::to_string(s) + "\n\n";
            contextInfo += m_trajectory[i].fullInfo() + "\n\n";
            
            if(m_trajectory[i].m_action == "fix_function")
            {
                lastFunctionName = m_trajectory[i].m_subject;
                
                std::string functionSourceBefore =  m_debugContext.getRequestedInfo(m_project, 0, 0, {},
                                                    {std::make_shared<std::string>(m_trajectory[i].m_subject)},
                                                    {},{});
                
                contextInfo += "Source of the function '" + m_trajectory[i].m_subject + "' before the fix\n\n";
                if(functionSourceBefore.empty())
                {
                    contextInfo += "The source is already available in the context\n\n";
                }
                else
                {
                    contextInfo += functionSourceBefore;
                }
            }
            else if(m_trajectory[i].m_action == "run_test")
            {
                goTo(project, s);
                
                if(!lastFunctionName.empty())
                {
                    //TODO: Should we check if the function exists in the m_project??
                    
                    contextInfo += "Source of the function '" + lastFunctionName + "' after the fix\n\n";
                    
                    std::string functionSourceAfter =  m_debugContext.getRequestedInfo(m_project, 0, 0, {},
                                                        {std::make_shared<std::string>(lastFunctionName)},
                                                        {},{});
                    
                    if(functionSourceAfter.empty())
                    {
                        contextInfo += "The source is already available in the context\n\n";
                    }
                    else
                    {
                        contextInfo += functionSourceAfter;
                    }
                }
                
                contextInfo += m_debugContext.loadTestLogFromStep(project, m_test, s);
                
                lastFunctionName.clear();
            }
            else if(m_trajectory[i].m_action == "debug_function")
            {
                goTo(project, s);
                
                NextDebugStep prevStep;
                int stepId = trajectoryIndexToStep(s)-1;
                std::string stepDir = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + std::to_string(s-1);
                web::json::value stepJson;
                if(loadJson(stepJson, stepDir + "/nextStep.json"))
                prevStep.from_json(stepJson);
                
                if(!prevStep.breakpoints.empty())
                {
                    contextInfo += "\nbreakpoints:\n";
                    contextInfo += m_debugContext.getBreakpointsInfo( false, m_trajectory[i].m_subject, prevStep.breakpoints);
                }
            }
            else
            {
                goTo(project, s);
                
                DebugStep dummyDebugStep;
                contextInfo += m_debugContext.stepInfo(m_project, m_test,
                                                       m_trajectory[i].m_action,
                                                       m_trajectory[i].m_subject,
                                                       m_trajectory[i].m_motivation,
                                                       m_trajectory[i].m_invocation,
                                                       m_trajectory[i].m_lineNumber,
                                                       dummyDebugStep);
                
                lastFunctionName.clear();
            }
        }
        
        return contextInfo;
    }

    Distillery::CommitInfo Distillery::findCommit(CCodeProject* project, int step)
    {
        CommitInfo info;
        info.hash.clear();
        
        info.fixIndex = getLastIndexBefore(step, "fix_function");
        info.checkoutParent = false;
        if(info.fixIndex < 0)
        {
            std::cout << "Unable to get the preceeding fix_function step for step " << step << std::endl;
            //Well, this is not ideal we need something, presumably expected to have a valid m_project after this call
            info.fixIndex = 0;
        }
        else
        {
            //fixIndex+1 as the commit now is stored in the subsequent run_test step
            info.hash = m_trajectory[info.fixIndex+1].m_commitHash;
        }
        
        while(info.hash.empty() && info.fixIndex >= 0 && info.fixIndex < m_trajectory.size())
        {
            int fixStep = trajectoryIndexToStep(info.fixIndex);
            
            //Scan froward, so we need to get the parent commit of the found one
            info.checkoutParent = true;
            info.fixIndex = getFirstIndexAfter(fixStep + 1, "fix_function");
            
            //fixIndex+1 as the commit now is stored in the subsequent run_test step
            info.hash = m_trajectory[info.fixIndex+1].m_commitHash;
        }
        
        return info;
    }

    void Distillery::goTo(CCodeProject* project, int step)
    {
        auto commitInfo = findCommit(project, step);
        
        if(commitInfo.hash.empty())
        {
            //TODO: do something here!
            std::cout << "Error: the commit hash is empty!!!\n";
        }
        
        if(m_currentFixIndex == commitInfo.fixIndex)
        {
            return;
        }
        
        m_currentFixIndex = commitInfo.fixIndex;
        
        std::string originalDagDir = project->getProjDir() + "/dag";
        std::string distillProjDir = project->getProjDir() + "/temp/DistillProject";
        std::string tempDagDir = distillProjDir + "/dag";
        
        if(!boost_fs::exists(tempDagDir))
        {
            boost_fs::create_directories(distillProjDir);
            
            boost::system::error_code ec;
            boost_fs::copy(originalDagDir, tempDagDir,
                           boost_fs::copy_options::recursive |
                           boost_fs::copy_options::overwrite_existing, ec);
            
            if (ec) {
                std::cout << "Copy dag Git repo failed! from: " << originalDagDir << " to: " << tempDagDir << "\n";
                std::cout << "Error: " << ec.message() << "\n";
            }
        }
        
        checkoutExact(tempDagDir, commitInfo.hash, true, false, commitInfo.checkoutParent);

        if(m_project)
        {
            delete m_project;
            m_project = nullptr;
        }
        
        
        {
            //We create a temporary project but as a principle:
            //WE DO THE ONLY VERY NECESSARY with this project!
            m_project = new CCodeProject;
            m_project->m_description = project->m_description;
            m_project->setup(distillProjDir);
            
            std::string promptsDir = Client::getInstance().getEnvironmentDir();
            std::string promptsDirDistillery = promptsDir + "/Distillery/Prompts";
            
            Prompt::clearSearchPaths();
            Prompt::addSearchPath(promptsDirDistillery);
            
            //m_project->generateCommonFiles("build");
            //m_project->compileCommonHeader(CCodeNode::BUILD_PRINT_TEST | CCodeNode::BUILD_DEBUG);
            
            Client::getInstance().setHackyProject(m_project);
            
            m_project->load();
            m_project->generateSources();
            
            Client::getInstance().setHackyProject(project);
        }
        
        int lastRun = getLastIndexBefore(step, "run_test");
        if(lastRun < 0)
        {
            std::cout << "Corrupted trajectory at step " << step << std::endl;
            return;
        }
        
        int lastDebug = getLastIndexBefore(step, "debug_function");
        if(lastDebug >= 0 && lastDebug > lastRun)
        {
            //We have to load the infor from the debug step.
            lastRun = lastDebug;
        }
        
        int stepRun = m_fromStep + lastRun + 1;
        
        std::string stepDir = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_";
        stepDir += std::to_string(stepRun);
        stepDir += "/wd";
        
        m_debugContext.setup(stepDir, m_system);
    }

    std::string Distillery::printTrajectory()
    {
        int s = 1;
        std::map<std::string, int> usedActions;
        std::stringstream ssout;
        for(auto& step : m_trajectory)
        {
            std::string stepHint;
            
            usedActions[step.m_action]++;
            
            if(step.m_action == "fix_function" || step.m_action == "run_test" || step.m_action == "stop_unit_test")
            {
                stepHint += "\nDEBUG STEP: " + std::to_string(s++) + "\n\n";
                stepHint += step.fullInfo() + "\n\n";
            }
            else
            {
                stepHint += "DEBUG STEP: " + std::to_string(s++) + " " + step.m_action + " for '" + step.m_subject + "' ";
                stepHint += " invc: " + std::to_string(step.m_invocation);
                stepHint += " line: " + std::to_string(step.m_lineNumber);
                stepHint += " - " + step.m_motivation.substr(0,96) + "...";
            }
            
            ssout << stepHint << std::endl;
        }
        
        ssout << std::endl << "USED ACTIONS" << std::endl;
        
        for(auto& action : usedActions)
        {
            ssout << action.first << " : " << action.second << std::endl;
        }
        
        return ssout.str();
    }

    bool Distillery::loadTrajectory(CCodeProject* project, const TestDef& test, int fromStep, int toStep)
    {
        std::string trajectoryDir = Client::getInstance().getProjectDirectory() + "/debug/" + test.name + "/trajectory";
        
        if(!boost_fs::exists(trajectoryDir))
        {
            return false;
        }
        
        if(fromStep < 0) {
            fromStep = 0;
        }
        
        if(toStep < 0) {
            toStep = 50000; //We must not have more than that.
        }
        
        m_fromStep = fromStep;
        
        for(int step = fromStep; step < toStep; ++step)
        {
            std::string stepIndexStr = std::to_string(step+1);
            
            std::string stepDir = "/step_" + stepIndexStr;
            
            std::string dbgStepPath = trajectoryDir + stepDir + "/dbgStep.json";
            if(!boost_fs::exists(dbgStepPath))
            {
                std::cout << "Attempted to load debug step that doesn't exist: " << dbgStepPath << std::endl;
                std::cout << "Stop trajectory loading" << std::endl;
                return true;
            }
            
            DebugStep dbgStep;
            dbgStep.load(dbgStepPath);
            
            //Load line number and invocation from the previous nextStep.json
            std::string prevStepIndexStr = std::to_string(step);
            
            std::string prevNextStepPath = trajectoryDir + "/step_" + prevStepIndexStr + "/nextStep.json";
            
            web::json::value jsonPrevNextStep;
            loadJson(jsonPrevNextStep, prevNextStepPath);
            
            if(jsonPrevNextStep.has_field(U("line_number")))
            {
                dbgStep.m_lineNumber = jsonPrevNextStep[U("line_number")].as_integer();
            }
            
            if(jsonPrevNextStep.has_field(U("invocation")))
            {
                dbgStep.m_invocation = jsonPrevNextStep[U("invocation")].as_integer();
            }
            
            m_trajectory.push_back(dbgStep);
            
            if(dbgStep.m_action == "fix_function")
            {
                m_newFunctionsPerStep[step+1] = getFunctionsDefinedInStep(project, test, step);
            }
        }
        
        return true;
    }

    std::pair<bool, uint32_t> Distillery::findRequestIdForPattern(const boost_fs::path& dir, const std::string& prefix, const std::string& suffix)
    {
        if (!boost_fs::exists(dir) || !boost_fs::is_directory(dir)) {
            return {false, 0};
        }
        
        // Small helper as a lambda
        const auto regex_escape = [](const std::string& s) -> std::string {
            static const std::regex re{ R"([.^$|()\\[*+?{}])" };
            return std::regex_replace(s, re, R"(\$&)");
        };

        const std::string pattern = std::string("^")
            + regex_escape(prefix)
            + R"((\d+))"
            + regex_escape(suffix)
            + "$";

        const std::regex rx(pattern, std::regex::ECMAScript);

        uint32_t best = 0;
        bool found = false;

        for (boost_fs::directory_iterator it(dir), end; it != end; ++it) {
            if (!boost_fs::is_regular_file(*it)) continue;

            const std::string name = it->path().filename().string();
            std::smatch m;
            if (std::regex_match(name, m, rx)) {
                try {
                    unsigned long v = std::stoul(m[1].str());
                    if (v <= std::numeric_limits<uint32_t>::max()) {
                        uint32_t id = static_cast<uint32_t>(v);
                        if (!found || id > best) { best = id; found = true; }
                    }
                } catch (...) { /* ignore */ }
            }
        }
        return {found, best};
    }

    std::pair<int,int> Distillery::findRequestsIdRange(const std::string& folderPath)
    {
        std::pair<int,int> result{-1, -1};

        const std::regex rx(R"(^(?:request|response)_(\d+)_.*\.json$)",
                            std::regex::ECMAScript);

        try {
            boost_fs::path dir(folderPath);
            if (!boost_fs::exists(dir) || !boost_fs::is_directory(dir)) {
                return result;
            }

            int minId = std::numeric_limits<int>::max();
            int maxId = std::numeric_limits<int>::min();
            bool found = false;

            for (boost_fs::directory_iterator it(dir), end; it != end; ++it) {
                const auto& e = *it;
                boost::system::error_code ec;
                if (!boost_fs::is_regular_file(e.status(ec)) || ec) continue;

                const std::string name = e.path().filename().string();
                std::smatch m;
                if (!std::regex_match(name, m, rx)) continue;

                long long ll = 0;
                try {
                    ll = std::stoll(m[1].str());
                } catch (...) {
                    continue;
                }
                if (ll < std::numeric_limits<int>::min() || ll > std::numeric_limits<int>::max())
                    continue;

                const int id = static_cast<int>(ll);
                if (id < minId) minId = id;
                if (id > maxId) maxId = id;
                found = true;
            }

            if (found) {
                result.first  = minId;
                result.second = maxId;
            }
        } catch (...) {
            // keep {-1, -1}
        }

        return result;
    }

    std::vector<std::string> Distillery::collectReqResInRange(const std::string& folderPath,
                                                         int id_lo, int id_hi,
                                                         const std::string& kind,// = "request",
                                                         const std::string& filter)// = ""
    {
        std::vector<std::pair<int, std::string>> hits; // (id, path)

        if (id_hi < id_lo) std::swap(id_lo, id_hi);

        // Build a regex that optionally fixes the kind (request/response) or allows both.
        // Captures:
        //   1: kind (request|response)
        //   2: id
        //   3: middle piece (e.g., "SystemAnalysis", "NextStep", etc.)
        const std::string kindAlt = (kind == "request" || kind == "response")
                                    ? kind
                                    : "(?:request|response)";
        const std::regex rx(("^" + kindAlt + R"(_(\d+)_([^.]+)\.json$)"),
                            std::regex::ECMAScript);

        try {
            boost_fs::path dir(folderPath);
            if (!boost_fs::exists(dir) || !boost_fs::is_directory(dir)) {
                return {}; // no matches
            }

            for (boost_fs::directory_iterator it(dir), end; it != end; ++it) {
                const auto& e = *it;
                boost::system::error_code ec;
                if (!boost_fs::is_regular_file(e.status(ec)) || ec) continue;

                const std::string name = e.path().filename().string();
                std::smatch m;
                if (!std::regex_match(name, m, rx)) continue;

                // Parse ID
                long long ll = 0;
                try {
                    ll = std::stoll(m[1].str());
                } catch (...) {
                    continue;
                }
                if (ll < std::numeric_limits<int>::min() || ll > std::numeric_limits<int>::max())
                    continue;

                const int id = static_cast<int>(ll);
                if (id < id_lo || id > id_hi) continue;

                // Optional middle filter
                if (!filter.empty()) {
                    const std::string middle = m[2].str();
                    if (middle.find(filter) == std::string::npos) continue;
                }

                hits.emplace_back(id, e.path().string());
            }
        } catch (...) {
            // ignore and return what we have
        }

        // Sort by id, then by path to keep deterministic order among same IDs
        std::sort(hits.begin(), hits.end(),
                  [](const std::pair<int,std::string>& a, const std::pair<int,std::string>& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                  });

        // Project to just the paths
        std::vector<std::string> out;
        out.reserve(hits.size());
        for (auto& p : hits) out.emplace_back(std::move(p.second));
        return out;
    }

    std::set<std::string> Distillery::getFunctionsDefinedInStep(CCodeProject* project, const TestDef& test, int step)
    {
        int stepInTrajectoryIdx = step - m_fromStep;
        std::set<std::string> definedFunctions;
        if(m_trajectory.size() <= stepInTrajectoryIdx)
        {
            std::cout << "The step " << step << " doesn't exist in the trajectory being distilled (";
            std::cout << m_fromStep + m_trajectory.size() << " steps long)" << std::endl;
            return definedFunctions;
        }
        
        if(m_trajectory[stepInTrajectoryIdx].m_action != "fix_function")
        {
            std::cout << "The step " << step + 1 << " is not a 'fix_function' step and can't define functions" << std::endl;
            return definedFunctions;
        }
        
        std::string projDir = project->getProjDir();
        std::string stepLog = projDir + "/logs/debug/" + test.name + "/step_" + std::to_string(step + 1);
        auto startIdRange = findRequestsIdRange(stepLog);
        
        if(startIdRange.first < 0)
        {
            //'fix_function' actions don't generate request for the next step
            //since the 'run_test' action is automaticaly selected without request
            //Get the range from the preveious step
            if(step > 0)
            {
                std::string prevStepLog = projDir + "/logs/debug/" + test.name + "/step_" + std::to_string(step);
                startIdRange = findRequestsIdRange(prevStepLog);
                if(startIdRange.second > startIdRange.first)
                {
                    startIdRange.first = startIdRange.second;
                    startIdRange.second = -1;
                }
            }
            
            //no excuses this time though
            if(startIdRange.first < 0)
            {
                std::cout << "Couldn't find requests for step " << step + 1 << std::endl;
                return definedFunctions;
            }
        }
        
        auto endIdRange = startIdRange;
        
        int nextStep = step + 1;
        
        //If the range is open, try to find the end of the range in the next steps
        while(startIdRange.second <= startIdRange.first)
        {
            std::string nextStepLog = projDir + "/logs/debug/" + test.name + "/step_" + std::to_string(nextStep + 1);
            if(!boost_fs::exists(nextStepLog))
            {
                break;
            }
            
            endIdRange = findRequestsIdRange(nextStepLog);
            
            if(endIdRange.first < 0)
            {
                std::cout << "Couldn't find requests for step " << step << std::endl;
            }
            
            startIdRange.second = endIdRange.first;
            
            nextStep++;
        }
        
        std::string logNodes = projDir + "/logs/nodes";
        if (boost_fs::exists(logNodes) && boost_fs::is_directory(logNodes))
        {
            for (boost_fs::directory_iterator it(logNodes), end; it != end; ++it)
            {
                if (boost_fs::is_directory(*it))
                {
                    std::vector<std::string> newFunctionRequests = collectReqResInRange(it->path().string(), startIdRange.first, startIdRange.second, "request", "DefineFunction");
                    for(auto req : newFunctionRequests)
                    {
                        const boost_fs::path p(req);
                        const std::string& marker = "nodes";
                        
                        for (auto it = p.begin(), end = p.end(); it != end; ++it) {
                            if (it->string() == marker) {
                                auto next = it; ++next;
                                if (next != end) {
                                    definedFunctions.insert(next->string());
                                }
                                break; // marker is last segment -> error below
                            }
                        }
                    }
                }
            }
        }
        
        return definedFunctions;
    }

    void Distillery::compileDataset(const std::string& datasetDir)
    {
        const boost_fs::path root(datasetDir);

        if (!boost_fs::exists(root) || !boost_fs::is_directory(root)) {
            throw std::runtime_error("compileDataset(): datasetDir is not a directory: " + datasetDir);
        }

        std::vector<boost_fs::path> files;
        for (boost_fs::directory_iterator it(root), end; it != end; ++it)
        {
            if (boost_fs::is_regular_file(*it))
            {
                files.push_back(it->path());
            }
        }

        std::sort(files.begin(), files.end(),
                  [](const boost_fs::path& a, const boost_fs::path& b) {
            return a.filename().string() < b.filename().string();
        });

        boost_fs::remove(datasetDir + "/train_dbg_sft.jsonl");

        for (const auto& p : files)
        {
            if (p.extension() == ".json" && startsWith(p.filename().string(), "step_"))
            {
                std::string jsonSample = getFileContent(p.string());
                web::json::value chatSample;
                if (!tryParseJsonText(jsonSample, chatSample))
                {
                    std::cout << "Skipping malformed training sample during compileDataset: ";
                    std::cout << p.string() << std::endl;
                    continue;
                }

                std::string sanitizeReason;
                const std::string sampleName = p.stem().string();
                if (!sanitizeTrainingSampleByName(sampleName, chatSample, sanitizeReason))
                {
                    std::cout << "Skipping invalid training sample during compileDataset: ";
                    std::cout << p.string() << "\nReason: " << sanitizeReason << std::endl;
                    continue;
                }

                jsonSample = utility::conversions::to_utf8string(chatSample.serialize());
                std::ofstream trainFile(datasetDir + "/train_dbg_sft.jsonl", std::ios::app);
                if(trainFile.good())
                {
                    trainFile << jsonSample << std::endl;
                }
            }
        }

        boost_fs::remove(datasetDir + "/train_run_sft.jsonl");

        for (const auto& p : files)
        {
            if (p.extension() == ".json" &&
                (startsWith(p.filename().string(), "system_") || startsWith(p.filename().string(), "debug_")))
            {
                std::string jsonSample = getFileContent(p.string());
                web::json::value chatSample;
                if (!tryParseJsonText(jsonSample, chatSample))
                {
                    std::cout << "Skipping malformed training sample during compileDataset: ";
                    std::cout << p.string() << std::endl;
                    continue;
                }

                std::string sanitizeReason;
                const std::string sampleName = p.stem().string();
                if (!sanitizeTrainingSampleByName(sampleName, chatSample, sanitizeReason))
                {
                    std::cout << "Skipping invalid training sample during compileDataset: ";
                    std::cout << p.string() << "\nReason: " << sanitizeReason << std::endl;
                    continue;
                }

                jsonSample = utility::conversions::to_utf8string(chatSample.serialize());
                std::ofstream trainFile(datasetDir + "/train_run_sft.jsonl", std::ios::app);
                if(trainFile.good())
                {
                    trainFile << jsonSample << std::endl;
                }
            }
        }
    }

    void Distillery::scoreTrajectory(CCodeProject* project, const std::string& testDirectory, const std::string& mergedTrajectory)
    {
        std::string testFiles = "//File: test.json\n```json\n";
        testFiles += getFileContent(testDirectory + "/test.json");
        testFiles += "\n```\n\n";
        
        std::string datasetDir = project->getProjDir() + "/dataset/" + m_test.name;
        
        saveToFile(mergedTrajectory, datasetDir + "/merged_trajectory.txt");
        
        //TODO: There is a serious chance testWD to be wrong!
        std::string testWD = project->getProjDir() + "/debug/wd_pub";
        for(auto file : m_test.getRewardHackingTestFiles(testWD))
        {
            boost_fs::path filePath(file);
            testFiles += "//File: " + filePath.filename().string() + "\n```" + filePath.extension().string() + "\n";
            testFiles += getFileContent(testDirectory + "/" + file);
            testFiles += "\n```\n\n";
        }
        
        Prompt promptScoreTrajectory("AnalyzeTrajectory.txt",{
                            {"trajectory", mergedTrajectory},
                            {"test_files", testFiles}
        });
        
        web::json::value object;
        web::json::value schema;
        setupSchema<TrajectoryAnalysis>(schema);
        
        //Cache _cache(dir, cache);
        Cache cache(datasetDir, "/trajecoty_analysis.json");
        project->captureContext(std::string());
        
        project->inference(cache, promptScoreTrajectory, schema, object);
        
        TrajectoryAnalysis trackAnalysis;
        trackAnalysis.from_json(object);
        
        project->popContext();
        
        saveJson(object, datasetDir + "/trajecoty_analysis.json");
        
        for(auto b : trackAnalysis.blockers)
        {
            uint32_t fixStep = *b;
            
            //TODO: Only for test
            //if(fixStep != 34) continue;
            
            int fixIndex = stepToTrajectoryIndex(fixStep);
            if(fixIndex < 0 || fixIndex > m_trajectory.size())
            {
                //Somthing is not cool here
                continue;
            }
            
            if(m_trajectory[fixIndex].m_action != "fix_function")
            {
                fixStep = trajectoryIndexToStep(fixIndex);
                fixIndex = getFirstIndexAfter(fixStep, "fix_function");
                fixStep = trajectoryIndexToStep(fixIndex);
            }
            
            distillFixTrack(project, trackAnalysis.analysis, fixStep);
        }
        
        //Compile train.jsonl
        compileDataset(datasetDir);
    }

    void Distillery::clear()
    {
        m_test.clear();
        m_trajectory.clear();
        m_newFunctionsPerStep.clear();
        m_fromStep = 0;
        
        m_distilleryContext.reset();
        
        m_currentFixIndex = -1;
        if(m_project)
        {
            delete m_project;
            m_project = nullptr;
        }
        
        m_debugContext.clear();
        m_dataset.clear();
        
        m_dbgSystemPrompt.clear();
        m_projDesc.clear();
        
        m_nextStepPrompt.clear();
        m_debugAnalysisPrompt.clear();
        
        //The original trajectory
        m_fromStep = 0;
        m_trajectory.clear();
        //New functions for each step
        m_newFunctionsPerStep.clear();
        m_newFunctions.clear();
        
        //From first:fix_function to the last secodn:fix_function actions
        m_mergedFixes.clear();
        m_prologue.clear();
    }

    uint32_t Distillery::loadTrajectory(CCodeProject* project,
                                        const std::string& testJsonPath,
                                        int fromStep, int toStep)
    {
        std::ifstream file(testJsonPath + "/test.json");
        
        std::string jsonStr((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto uJsonStr = utility::conversions::to_string_t(jsonStr);
        auto json = web::json::value::parse(uJsonStr);
        if(json.has_field(U("function")))
        {
            m_system = utility::conversions::to_utf8string(json[U("function")].as_string());
        }
        else
        {
            std::cout << "The test file doesn't have 'function' field: ";
            std::cout << testJsonPath << "/test.json" << std::endl;
            m_system = "main";
        }
        
        m_test.from_json(json);
        
        loadTrajectory(project, m_test, fromStep, toStep);
        
        return (uint32_t)m_trajectory.size();
    }

    void Distillery::distillTrajectory(CCodeProject* project, const std::string& testJsonPath, int& fromStep, int toStep)
    {
        {
            web::json::value jsonTest;
            loadJson(jsonTest, testJsonPath + "/test.json");
            TestDef tempTest;
            tempTest.from_json(jsonTest);
            std::string trajectoryDir = Client::getInstance().getProjectDirectory() + "/debug/" + tempTest.name + "/trajectory";
            
            uint32_t nextStepIndex = (uint32_t)nextIndex(trajectoryDir, "step_");
            if(nextStepIndex > 300)
            {
                fromStep = nextStepIndex - 300;
            }
        }
        
        if(loadTrajectory(project, testJsonPath, fromStep, toStep) <= 0)
        {
            return;
        }
        
        std::cout << printTrajectory();
        
        for(auto perS : m_newFunctionsPerStep)
        {
            m_newFunctions.insert(perS.second.begin(), perS.second.end());
        }
        
        if(!m_newFunctions.empty())
        {
            std::cout << "ALL NEW FUNCTIONS: " << getAsCsv(m_newFunctions) << std::endl;
        }
        
        std::string promptsDir = Client::getInstance().getEnvironmentDir();
        std::string promptsDirDebugger = promptsDir + "/Debugger/Prompts";
        std::string promptsDirDistillery = promptsDir + "/Distillery/Prompts";
        
        m_distilleryContext.reset();
        project->setActiveContext(&m_distilleryContext);
        
        Prompt::clearSearchPaths();
        Prompt::addSearchPath(promptsDirDistillery);
        
        Prompt role("DistilleryRole.txt",{});
        project->pushMessage(role, "system", true);
        
        Prompt distillationWorkflow("DistillationWorkflow.txt",{});
        project->pushMessage(distillationWorkflow, "user", true);
        
        Prompt::clearSearchPaths();
        Prompt::addSearchPath(promptsDirDebugger);
        
        m_projDesc = "PROJECT DESCRIPTION: " + project->getProjectBrief();
        project->pushMessage(m_projDesc, "user", true);
        
        std::string hitCount = std::to_string(MAX_BREKPOINT_HITCOUNT);
        Prompt workflow("Workflow.txt",{{"project", project->getProjectName()},
                                        {"hit_count", hitCount}});
        
        project->pushMessage(workflow, "user", true);
        
        m_dbgSystemPrompt = "\nHey, you are a Large Language Model specialized in complex debugging workflows for applications written on C++ and STL.\n\n";//dbgRole.str();
        
        m_nextStepPrompt = "Analyze the provided information and current progress debugging the application. ";
        m_nextStepPrompt += "What should be the next action in order to debug the application?\n\n";
        
        m_debugAnalysisPrompt = getFileContent(promptsDirDebugger + "/SystemDebugAnalysis.txt");
        

        Prompt::clearSearchPaths();
        Prompt::addSearchPath(promptsDirDistillery);
        
        std::string logDir = project->getProjDir() + "/logs/distill/" + m_test.name;
        boost_fs::create_directories(logDir);
        Client::getInstance().setContextLogDir(logDir);
        
        Client::getInstance().selectLLM(InferenceIntent::DEBUG_ANALYSIS);
        
        std::string mergedTrajectory = mergeFixes();
        
        scoreTrajectory(project, testJsonPath, mergedTrajectory);
        
        
        Prompt::clearSearchPaths();
        std::string promptsDirEnv = Client::getInstance().getEnvironmentDir() + "/Prompts";
        Prompt::addSearchPath(promptsDirEnv);
        
        Client::getInstance().unlockLLM();
        project->switchToCompileContext();
        
        clear();
    }

    //TODO: Consider moving the to the DebugContextProvider
    int Distillery::getSummaryStepForStep(CCodeProject* project, int step, std::string& summary)
    {
        web::json::value trajectoryCfg;
        if(!m_debugContext.getStepTrajecotyCfg(project, m_test, step, trajectoryCfg))
        {
            return -1;
        }
        
        //These properties are necessary. They must be in the json object
        if(!trajectoryCfg.has_field(U("previousSteps")))
        {
            return -1;
        }
        
        int summaryStep = trajectoryCfg[U("previousSteps")].as_number().to_int32();
        if(summaryStep < 0)
        {
            return summaryStep;
        }
        
        std::string summaryFile = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + std::to_string(summaryStep) + "/summary.txt";
        
        if(!boost_fs::exists(summaryFile))
        {
            std::cout << "\nUnable to find summary file for step " << summaryStep << "!\n";
            return -1;
        }
        
        summary = getFileContent(summaryFile);
        return summaryStep;
    }

    std::string Distillery::getOriginalTrajectory(CCodeProject* project, int stepFromIdx, int stepToIdx)
    {
        std::string trajectory;
        for(int s=stepFromIdx; s<stepToIdx; ++s)
        {
            const DebugStep& step = m_trajectory[s];
            uint32_t stepIndex = m_fromStep + s + 1;
            
            trajectory += "\nSTEP " + std::to_string(stepIndex) + ": ";
            trajectory += step.m_action;
            if(!step.m_subject.empty())
            {
                trajectory += " for '" + step.m_subject + "'";
            }
            trajectory += "\n";
            if(!step.m_motivation.empty())
            {
                trajectory += "Motivation for this setp:\n";
                trajectory += step.m_motivation;
            }
            trajectory += "\n";
            if(!step.m_debugNotes.empty())
            {
                trajectory += "Debug notes for this setp:\n";
                trajectory += step.m_debugNotes;
            }
            trajectory += "\n";
            if(!step.m_logSummary.empty())
            {
                trajectory += "Log summary for this setp:\n";
                trajectory += step.m_logSummary;
            }
            trajectory += "\n";
        }
        
        return trajectory;
    }

    std::string Distillery::distillSummaryBefore(CCodeProject* project, int runStep, int fixStep)
    {
        std::string oldSummary;
        int summaryStep = getSummaryStepForStep(project, runStep, oldSummary);
        if(summaryStep <= m_fromStep)
        {
            oldSummary = "\nNo summarized steps yet\n\n";
            summaryStep = m_fromStep + 1;
        }
        
        int summaryStepIdx = stepToTrajectoryIndex(summaryStep);
        int runStepIdx = stepToTrajectoryIndex(runStep);
        
        if(summaryStepIdx <= 0 && summaryStepIdx == runStepIdx)
        {
            //return "Summary is not available for the fix sequence.";
            return std::string();
        }
        
        std::string summarizeTrajectory = getOriginalTrajectory(project, summaryStepIdx, runStepIdx);
        
        std::string sequenceToDistill = "(from run_test step " + std::to_string(runStep) + " to fix_function step " + std::to_string(fixStep) + ")";
        std::string sequenceToSummarize = "(from run_test step " + std::to_string(summaryStep) + " to fix_function step " + std::to_string(runStep-1) + ")";
        Prompt promptDistillSummary("DistillSummary.txt",{
                            {"old_summary", oldSummary},
                            {"summarize_trajecotry", summarizeTrajectory},
                            {"sequence_to_distill", sequenceToDistill},
                            {"sequence_to_summarize", sequenceToSummarize}
        });
        
        std::string message = promptDistillSummary.str();
        project->captureContext(std::string());
        
        Cache cache;
        bool truncated = false;
        std::string summary = "review";
        project->inference(cache, message, summary, &truncated);
        int attempts = 0;
        while(summary.length() > 2048 && attempts < 3)
        {
            std::string feedback = "\n\nThe summary is too long. It should be less than 2000 characters. 15-20 concise sentences!\n\n";
            
            summary = "review";
            truncated = false;
            project->inference(cache, feedback, summary, &truncated);
            
            //TODO: Investigate in the future, we want something substantial
            if(summary.length() <= 512)
            {
                //break;
            }
            
            attempts++;
        }
        
        project->popContext();
        
        return summary;
    }

    void Distillery::saveTrainingData(const std::string& datasetDir, const std::string& sampleName, web::json::value& chatSample, bool jsonReponse)
    {
        //TODO: Consider to enforce alternated messages user->assistant
        
        //alternateRoles(chatSample);

        std::string sanitizeReason;
        if(!sanitizeTrainingSampleByName(sampleName, chatSample, sanitizeReason))
        {
            std::cout << "Skipping training sample '" << sampleName << "' due to invalid shape. Reason: ";
            std::cout << sanitizeReason << std::endl;
            return;
        }
        
        if(!boost_fs::exists(datasetDir)) {
            boost_fs::create_directories(datasetDir);
        }
        
        std::string renderedChatName = datasetDir + "/" + sampleName;
        std::string renderedChatJson = renderedChatName + ".json";
        
        std::string jsonContent = utility::conversions::to_utf8string(chatSample.serialize());
        saveToFile(jsonContent, renderedChatJson);
        
        std::string renderedChat;
        
        uint32_t size = (uint32_t)chatSample[U("messages")].as_array().size();
        for(uint32_t i=0; i<size; ++i)
        {
            auto& message = chatSample[U("messages")].at(i);
            std::string role = utility::conversions::to_utf8string(message[U("role")].as_string());
            
            bool formatAsJson = false;
            if(message.has_field(U("thinking")))
            {
                std::string thinking = utility::conversions::to_utf8string(message[U("thinking")].as_string());
                if(!thinking.empty())
                {
                    renderedChat += ">> thinking\n\n\n";
                    renderedChat += thinking + "\n\n\n";
                    
                    if(jsonReponse)
                    {
                        formatAsJson = true;//After the thinking we have a json reponse!
                    }
                }
            }
            
            std::string content = utility::conversions::to_utf8string(message[U("content")].as_string());
            if(formatAsJson)
            {
                content = formatJson(content, "  ");
            }
            renderedChat += ">> " + role + "\n\n\n";
            renderedChat += content + "\n\n\n";
        }
        
        std::string renderedChatFile = renderedChatName + ".txt";
        saveToFile(renderedChat, renderedChatFile);
    }

    std::pair<bool, web::json::value> Distillery::distillResponse(CCodeProject* project, const std::string& sufix, int step,
                                     web::json::value& schema, const std::string& promptFile,
                                     const std::function<Prompt(const std::string&, const std::string&)> &buildPrompt)
    {
        std::string stepStr = std::to_string(step);
        std::string stepLogsDir = project->getProjDir() + "/logs/debug/" + m_test.name + "/step_" + stepStr;
        std::pair<bool, uint32_t> requestId = findRequestIdForPattern(stepLogsDir, "request_", sufix);
        if(!requestId.first)
        {
            return std::make_pair<>(false, web::json::value());
        }
        
        std::string requestFileName = "request_" + std::to_string(requestId.second) + sufix;
        
        web::json::value request;
        loadJson(request, stepLogsDir + "/" + requestFileName);
        std::string jsonRequest;
        jsonRequest += utility::conversions::to_utf8string(request.serialize());
        
        std::string responseFileName = "response_" + std::to_string(requestId.second) + sufix;
        
        web::json::value response;
        loadJson(response, stepLogsDir + "/" + responseFileName);
        std::string jsonResponse;
        jsonResponse += utility::conversions::to_utf8string(response.serialize());
        
        // Let the caller configure the Prompt via lambda
        Prompt distillPrompt = buildPrompt
        ? buildPrompt(jsonRequest, jsonResponse)
        : Prompt(promptFile, {
            { "json_request",  jsonRequest  },
            { "json_response", jsonResponse }
        });
        
        Cache cache;
        web::json::value object;
        project->inference(cache, distillPrompt, schema, object);
        
        return std::make_pair<>(true, object);
    }


    std::pair<bool, std::string> Distillery::thinkingForResponse(CCodeProject* project, const std::string& sufix, int step, web::json::value& request, web::json::value& response)
    {
        std::string stepStr = std::to_string(step);
        std::string stepLogsDir = project->getProjDir() + "/logs/debug/" + m_test.name + "/step_" + stepStr;
        std::pair<bool, uint32_t> requestId = findRequestIdForPattern(stepLogsDir, "request_", sufix);
        if(!requestId.first)
        {
            return std::make_pair<>(false, std::string());
        }
        
        std::string requestFileName = "request_" + std::to_string(requestId.second) + sufix;
        
        loadJson(request, stepLogsDir + "/" + requestFileName);
        std::string jsonRequest = "```json\n";
        jsonRequest += utility::conversions::to_utf8string(request.serialize());
        jsonRequest += "\n```\n";
        
        std::string responseFileName = "response_" + std::to_string(requestId.second) + sufix;
        
        loadJson(response, stepLogsDir + "/" + responseFileName);
        std::string jsonResponse = "```json\n";
        jsonResponse += utility::conversions::to_utf8string(response.serialize());
        jsonResponse += "\n```\n";
        
        // Let the caller configure the Prompt via lambda
        Prompt prompt("Thinking.txt", {
            { "json_request",  jsonRequest  },
            { "json_response", jsonResponse }
        });
        
        Cache cache;
        
        std::string thinking = "review";
        bool truncated = false;
        
        project->captureContext(std::string());
        
        project->inference(cache, prompt.str(), thinking, &truncated);
        
        project->popContext();
        
        return std::make_pair<>(true, thinking);
    }

    void Distillery::saveSystemAnalysis(const std::string& datasetDir, const std::string& fileName, DistilledAanalysis& systemAnalysis, const std::string& sysAnalysisRequest)
    {
        web::json::value jsonSysAnalysisRequest = web::json::value::parse(utility::conversions::to_string_t(sysAnalysisRequest));
        web::json::value jsonSample;
        jsonSample[U("messages")] = jsonSysAnalysisRequest[U("messages")];
        
        json::value responseMessage;
        responseMessage[U("role")] = json::value::string(U("assistant"));
        responseMessage[U("thinking")] = json::value::string(utility::conversions::to_string_t(systemAnalysis.thinking_analysis));
                                                     
        auto responseContent = systemAnalysis.system_analysis.to_json().serialize();
        responseMessage[U("content")] = json::value::string(responseContent);

        auto& messagesArray = jsonSample[U("messages")].as_array();
        messagesArray[messagesArray.size()] = responseMessage;
        
        saveTrainingData(datasetDir, fileName, jsonSample, true);
    }

    void Distillery::saveResponseForTraining(const std::string& datasetDir,
                                 const std::string& fileName,
                                 const std::string& request,
                                 const std::string& thinking,
                                 const std::string& response,
                                 bool responseAsJson)
    {
        web::json::value jsonSysAnalysisRequest = web::json::value::parse(utility::conversions::to_string_t(request));
        web::json::value jsonSample;
        jsonSample[U("messages")] = jsonSysAnalysisRequest[U("messages")];
        
        json::value responseMessage;
        responseMessage[U("role")] = json::value::string(U("assistant"));
        
        if(!thinking.empty())
        {
            responseMessage[U("thinking")] = json::value::string(utility::conversions::to_string_t(thinking));
        }
                                                     
        responseMessage[U("content")] = json::value::string(utility::conversions::to_string_t(response));

        auto& messagesArray = jsonSample[U("messages")].as_array();
        messagesArray[messagesArray.size()] = responseMessage;
        
        saveTrainingData(datasetDir, fileName, jsonSample, responseAsJson);
    }

    bool Distillery::distillRunStep(CCodeProject* project, const std::string& summary, const std::string& fixedFunction, int testStep, int fixStep, std::string& debugNotes)
    {
        m_debugContext.clear();
        
        web::json::value schemaAnalysis;
        setupSchema<DistilledAanalysis>(schemaAnalysis);
        
        project->captureContext(std::string());
        
        std::string currentTrajectory = getTrajectoryPrologue(project, testStep, testStep, summary);
        
        auto hint = m_debugContext.runAnalysis(m_project);
        
        if(!hint.second.empty())
        {
            currentTrajectory += "//Here is a hint from my analysis:\n";
            currentTrajectory += hint.second;
            currentTrajectory += "//The analysis hint ends here\n\n";
        }
        
        std::set<std::string> subSystems;
        std::string systemData = m_debugContext.getSubSystemsData(m_project, subSystems);
        std::string subSystemsStr = getAsCsv(subSystems);
        
        const int subSystemsDepth = PRINT_MAX_FUNCTIONS_DEPTH-1;
        currentTrajectory += "The following functions are classified as sub-systems as they are with call stack depth <= ";
        currentTrajectory += std::to_string(subSystemsDepth) + " and are called just once:\n";
        currentTrajectory += subSystemsStr + "\n\n";
        currentTrajectory += systemData;
        
        if(!fixedFunction.empty() && fixedFunction != "none")
        {
            std::string recentFix = m_debugContext.getRecentFixInfo(m_project, fixedFunction);
            currentTrajectory += recentFix;
        }
        
        //TODO: Inference the response
        DistilledAanalysis distilledSystemAnalysis;
        {
            std::string testStepStr = std::to_string(testStep);
            std::string fixStepStr = std::to_string(fixStep);
            std::string optimizedSequence = "(from step " + testStepStr + " to step " + fixStepStr + ")";
            
            web::json::value schema;
            setupSchema<DistilledAanalysis>(schema);
            
            project->captureContext(std::string());
            
            auto chat = getChat(project, "_SystemAnalysis.json", testStep);
            
            if(chat.first.empty() && chat.second.empty())
            {
                //TODO: Very likely reward hacking step
                std::string debugStepPath = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + testStepStr + "/dbgStep.json";
                DebugStep dbgStep;
                dbgStep.load(debugStepPath);
                
                debugNotes = dbgStep.m_debugNotes;
                m_debugContext.clear();
                return true;
            }
            
            Prompt promptDistillStep("DistillSystemAnalysis.txt",{
                { "json_request",  chat.first },
                { "json_response", chat.second },
                { "application", project->getProjectName()},
                { "analysis_step", testStepStr},
                { "optimized_sequence", optimizedSequence},
                { "current_trajectory", currentTrajectory},
            });
            
            Cache cache;
            
            // Build visibility from EXACT text that will be in the distilled sample.
            const std::set<std::string> functionCandidates = (m_project ? m_project :
                                                              project)->getNodeNames();
            std::set<std::string> visibleFunctions;
            addMentionedFunctionsFromText(currentTrajectory, functionCandidates,
                                          visibleFunctions);
            
            auto validateSystemGrounding = [&](const DistilledAanalysis& candidate,
                                               std::string& outFeedback) -> bool
            {
                outFeedback.clear();
                
                std::string narrative;
                narrative += candidate.system_analysis.debug_notes;
                narrative += "\n";
                narrative += candidate.system_analysis.log_summary;
                narrative += "\n";
                narrative += candidate.thinking_analysis;
                
                std::set<std::string> mentionedFunctions;
                addMentionedFunctionsFromText(narrative, functionCandidates,
                                              mentionedFunctions);
                
                std::set<std::string> disallowed;
                for (const auto& fn : mentionedFunctions)
                {
                    if (!fixedFunction.empty() && fixedFunction != "none" && fn ==
                        fixedFunction) continue;
                    if (!visibleFunctions.count(fn))
                    {
                        disallowed.insert(fn);
                    }
                }
                
                if (disallowed.empty())
                {
                    return true;
                }
                
                outFeedback += "Grounding violation in system analysis narrative fields ";
                outFeedback += "(debug_notes/log_summary/thinking_analysis).\n";
                outFeedback += "Disallowed function names: " + getAsCsv(disallowed, 30) + "\n";
                outFeedback += "Currently visible functions in CURRENT TRAJECTORY: ";
                outFeedback += getAsCsv(visibleFunctions, 30) + "\n";
                outFeedback += "Rule: mention only functions visible in CURRENT TRAJECTORY for this distilled sample.\n";
                outFeedback += "Do not use function names known only from OLD SYSTEM ANALYSIS REQUEST/RESPONSE or OPTIMIZED SEQUENCE.\n";
                outFeedback += "Regenerate using only grounded names.\n\n";
                
                return false;
            };
            
            web::json::value object;
            project->inference(cache, promptDistillStep, schema, object);
            distilledSystemAnalysis.from_json(object);
            
            std::string groundingFeedback;
            uint32_t attempts = 0;
            const uint32_t kMaxAttempts = 8;
            
            while (attempts < kMaxAttempts &&
                   !validateSystemGrounding(distilledSystemAnalysis, groundingFeedback))
            {
                project->inference(cache, groundingFeedback, schema, object);
                distilledSystemAnalysis = DistilledAanalysis();
                distilledSystemAnalysis.from_json(object);
                ++attempts;
            }
            
            // Final check after loop (important).
            if (!validateSystemGrounding(distilledSystemAnalysis, groundingFeedback))
            {
                std::cout << "System-analysis grounding didn't converge after "
                << kMaxAttempts << " retries at step " << testStep
                << ". Keeping last response." << std::endl;
            }
            
            project->popContext();
        }
        
        project->popContext();
        
        std::string datasetDir = project->getProjDir() + "/dataset/" + m_test.name;
        
        std::string testStepStr = std::to_string(testStep);
        std::string fixStepStr = std::to_string(fixStep);
        
        std::string sysAnalysisSample = "system_" + testStepStr + "_" + fixStepStr;
        
        {
            std::string content = utility::conversions::to_utf8string(distilledSystemAnalysis.system_analysis.to_json().serialize());
            json::value jsonSample = buildTrainingData(project, currentTrajectory, content, distilledSystemAnalysis.thinking_analysis);
            
            saveTrainingData(datasetDir, sysAnalysisSample, jsonSample, true);
        }
        
        {
            debugNotes += "\n\nSYSTEM ANALYSIS:\n\n";
            if(!distilledSystemAnalysis.system_analysis.debug_notes.empty())
            {
                debugNotes += distilledSystemAnalysis.system_analysis.debug_notes;
                debugNotes += "\n\nLog Summary:\n";
                debugNotes += distilledSystemAnalysis.system_analysis.log_summary;
                
            }
        }
        
        m_debugContext.clear();
        
        return false;
    }

    void Distillery::saveDebugAnalysis(CCodeProject* project,
                                       const std::string& datasetDir,
                                       const std::string& fileName,
                                       DistilledAanalysis& debugAnalysis,
                                       const std::string& debugInfo)
    {
        std::string datasetFile = datasetDir + "/train.jsonl";
        
        //web::json::value jsonSysAnalysisRequest = web::json::value::parse(utility::conversions::to_string_t(sysAnalysisRequest));
        json::value messagesArray = json::value::array();
        
        //*** system prompt
        json::value systemMessage;
        systemMessage[U("role")] = json::value::string(U("system"));
        std::string systemPrompt = m_dbgSystemPrompt;
        //systemPrompt += m_debugWorkflow;
        systemMessage[U("content")] = json::value::string(utility::conversions::to_string_t(systemPrompt));
        messagesArray[messagesArray.size()] = systemMessage;
        
        //*** trajecotry
        
        std::string trajecotryContent = debugInfo;
        trajecotryContent += "\n\n" + m_nextStepPrompt;
        
        json::value trajecotryMessage;
        trajecotryMessage[U("role")] = json::value::string(U("user"));
        std::string mergedUserMessage = m_projDesc + "\n\n";
        //mergedUserMessage += m_testPrompt + "\n\n";
        mergedUserMessage += trajecotryContent;
        trajecotryMessage[U("content")] = json::value::string(utility::conversions::to_string_t(mergedUserMessage));
        messagesArray[messagesArray.size()] = trajecotryMessage;
        
        //*** response message
        json::value responseMessage;
        responseMessage[U("role")] = json::value::string(U("assistant"));
        responseMessage[U("thinking")] = json::value::string(utility::conversions::to_string_t(debugAnalysis.thinking_analysis));
                                                     
        auto responseContent = debugAnalysis.system_analysis.to_json().serialize();
        
        responseMessage[U("content")] = json::value::string(responseContent);

        //auto& messagesArray = jsonSample[U("messages")].as_array();
        messagesArray[messagesArray.size()] = responseMessage;
        
        json::value jsonTrajecotry;
        jsonTrajecotry[U("messages")] = messagesArray;
        
        saveTrainingData(datasetDir, fileName, jsonTrajecotry, true);
    }

    std::pair<std::string, std::string> Distillery::getChat(CCodeProject* project, const std::string& sufix, int step)
    {
        std::string stepStr = std::to_string(step);
        std::string stepLogsDir = project->getProjDir() + "/logs/debug/" + m_test.name + "/step_" + stepStr;
        std::pair<bool, uint32_t> requestId = findRequestIdForPattern(stepLogsDir, "request_", sufix);
        if(!requestId.first)
        {
            return std::make_pair(std::string(), std::string());
        }
        
        std::string requestFileName = "request_" + std::to_string(requestId.second) + sufix;
        
        web::json::value request;
        loadJson(request, stepLogsDir + "/" + requestFileName);
        std::string jsonRequest;// = "```json\n";
        jsonRequest += utility::conversions::to_utf8string(request.serialize());
        //jsonRequest += "\n```\n";
        
        std::string responseFileName = "response_" + std::to_string(requestId.second) + sufix;
        
        web::json::value response;
        loadJson(response, stepLogsDir + "/" + responseFileName);
        std::string jsonResponse;// = "```json\n";
        jsonResponse += utility::conversions::to_utf8string(response.serialize());
        //jsonResponse += "\n```\n";
        
        return std::make_pair(jsonRequest, jsonResponse);
    }

    std::string Distillery::distillDebugStep(CCodeProject* project,
                                             const std::string& summary,
                                             std::string& prevSteps,
                                             std::string& newInfo,
                                             DistilledStep& distilledStep,
                                             int originalStep,
                                             int testStep, int debugStep)
    {
        std::string debugInfo = getTrajectoryPrologue(project, originalStep, debugStep, summary);
        debugInfo += prevSteps;
        
        std::string currentTrajectory = debugInfo;
        
        std::string stepStr = std::to_string(originalStep);
        //TODO: I don't like extraction from logs, maybe replace with parsing traces/logs for that step, but for now it is OK-ish
        {
            std::string stepLogsDir = project->getProjDir() + "/logs/debug/" + m_test.name + "/step_" + stepStr;
            
            std::pair<bool, uint32_t> requestId = findRequestIdForPattern(stepLogsDir, "request_", "_SystemDebugAnalysis.json");
            if(!requestId.first)
            {
                //return std::make_pair<>(false, web::json::value());
            }
            
            std::string requestFileName = "request_" + std::to_string(requestId.second) + "_SystemDebugAnalysis.json";
            
            web::json::value request;
            
            loadJson(request, stepLogsDir + "/" + requestFileName);
            
            if(!request.has_field(U("messages")))
            {
                //TODO: signal error
            }
            
            auto& messages = request[U("messages")].as_array();
            auto umessage = messages.at(messages.size()-1).as_object()[U("content")].as_string();
            std::string message = utility::conversions::to_utf8string(umessage);
            
            //TODO: Only temporary
            auto pos = message.find("DEBUG ANALYSIS REQUEST");
            //auto pos = message.find("TRACE ANALYSIS");
            
            std::string debugSection = (pos == std::string::npos) ? "" : message.substr(pos);
            if(debugSection.empty())
            {
                //TODO: signal error
            }
            
            debugInfo += "\n\n";
            debugInfo += debugSection;
            
            std::string jsonRequest;// = "```json\n";
            jsonRequest += utility::conversions::to_utf8string(request.serialize());
            jsonRequest = formatJson(jsonRequest, std::string("  "));
            
            //TODO: find the DEBUG ANALYSIS text
        }
        
        auto chat = getChat(project, "_SystemDebugAnalysis.json", originalStep);
        
        web::json::value schema;
        setupSchema<DistilledAanalysis>(schema);
        
        project->captureContext(std::string());
        
        std::string debugStepStr = std::to_string(debugStep);
        Prompt promptDistillStep("DistillSystemDebugAnalysis.txt",{
                            {"original_prompt", m_debugAnalysisPrompt},
                            {"original_analysis", chat.second},
                            {"current_trajectory", debugInfo},
                            {"original_step_id", stepStr},
                            {"step_id", debugStepStr}
        });
        
        Cache cache;
        
        web::json::value object;
        project->inference(cache, promptDistillStep, schema, object);

        DistilledAanalysis distilledDebugAnalysis;
        distilledDebugAnalysis.from_json(object);

        const std::set<std::string> functionCandidates = (m_project ? m_project :
                                                          project)->getNodeNames();
        std::set<std::string> visibleFunctions;
        addMentionedFunctionsFromText(debugInfo, functionCandidates, visibleFunctions);

        const std::string lockedDebugSubject = distilledStep.debug_step.action_subject;

        auto validateDebugGrounding = [&](const DistilledAanalysis& candidate,
                                          std::string& outFeedback) -> bool
        {
            outFeedback.clear();

            std::string narrative;
            narrative += candidate.system_analysis.debug_notes;
            narrative += "\n";
            narrative += candidate.system_analysis.log_summary;
            narrative += "\n";
            narrative += candidate.thinking_analysis;

            std::set<std::string> mentionedFunctions;
            addMentionedFunctionsFromText(narrative, functionCandidates, mentionedFunctions);

            std::set<std::string> disallowed;
            for (const auto& fn : mentionedFunctions)
            {
                if (!lockedDebugSubject.empty() && lockedDebugSubject != "none" && fn == lockedDebugSubject)
                {
                    continue;
                }

                if (!visibleFunctions.count(fn))
                {
                    disallowed.insert(fn);
                }
            }

            if (disallowed.empty())
            {
                return true;
            }

            outFeedback += "Grounding violation in debug analysis narrative fields ";
            outFeedback += "(debug_notes/log_summary/thinking_analysis).\n";
            outFeedback += "Disallowed function names: " + getAsCsv(disallowed, 30) + "\n";
            outFeedback += "Currently visible functions in CURRENT TRAJECTORY: ";
            outFeedback += getAsCsv(visibleFunctions, 30) + "\n";
            outFeedback += "Rule: mention only functions visible in CURRENT TRAJECTORY for this distilled sample.\n";
            outFeedback += "Regenerate using only grounded names.\n\n";

            return false;
        };

        std::string debugGroundingFeedback;
        uint32_t attempts = 0;
        const uint32_t kMaxAttempts = 8;
        while (attempts < kMaxAttempts &&
               !validateDebugGrounding(distilledDebugAnalysis, debugGroundingFeedback))
        {
            project->inference(cache, debugGroundingFeedback, schema, object);
            distilledDebugAnalysis = DistilledAanalysis();
            distilledDebugAnalysis.from_json(object);
            ++attempts;
        }

        if (!validateDebugGrounding(distilledDebugAnalysis, debugGroundingFeedback))
        {
            std::cout << "Debug-analysis grounding didn't converge after "
            << kMaxAttempts << " retries at step " << debugStep
            << ". Keeping last response." << std::endl;
        }

        project->popContext();
        
        DebugStep distilledDebugStep;
        
        //DebugStep stepInfo;
        distilledDebugStep.m_logSummary = distilledDebugAnalysis.system_analysis.log_summary;
        distilledDebugStep.m_debugNotes = distilledDebugAnalysis.system_analysis.debug_notes;
        distilledDebugStep.m_action = "debug_function";
        distilledDebugStep.m_subject = distilledStep.debug_step.action_subject;
        distilledDebugStep.m_motivation = distilledStep.debug_step.motivation;
        
        distilledStep.m_debugNotes = distilledDebugAnalysis.system_analysis.debug_notes;
        distilledStep.m_logSummary = distilledDebugAnalysis.system_analysis.log_summary;
        
        if(!distilledStep.debug_step.breakpoints.empty())
        {
            distilledDebugStep.m_motivation += "\n\nEvaluated breakpoints: \n";
            distilledDebugStep.m_motivation += m_debugContext.getBreakpointsInfo(false, distilledStep.debug_step.action_subject,
                                                                                 distilledStep.debug_step.breakpoints);
        }
        
        //Reuse prevSteps to return step info to be added
        prevSteps = "\nSTEP " + debugStepStr + ":\n";
        
        newInfo = distilledDebugStep.fullInfo();
        prevSteps += newInfo;
        
        
        std::string datasetDir = project->getProjDir() + "/dataset/" + m_test.name;
        std::string testStepStr = std::to_string(testStep);
        saveDebugAnalysis(project, datasetDir, "debug_" + testStepStr + "_" + debugStepStr, distilledDebugAnalysis, debugInfo);
        
        return currentTrajectory;
    }

    void Distillery::addStepToMessages(CCodeProject*,
                                       const DistilledStep& step,
                                       const std::string& newInfo,
                                       web::json::value& messages)
    {
        // Prevent empty/whitespace-only user turns in interleaved history.
        if (newInfo.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
            return;
        }
        
        auto& arr = messages.as_array();
        
        NextDebugStep req = step.debug_step;
        if (req.invocation <= 0) {
            req.invocation = 1;
        }
        if (req.line_number == (uint32_t)-1) {
            req.line_number = 0;
        }
        if (!step.motivation_summary.empty()) {
            req.motivation = step.motivation_summary;
        }
        
        web::json::value a;
        a[U("role")] = web::json::value::string(U("assistant"));
        a[U("content")] = web::json::value::string(req.to_json().serialize());
        arr[arr.size()] = a;
        
        web::json::value u;
        u[U("role")] = web::json::value::string(U("user"));
        u[U("content")] = web::json::value::string(utility::conversions::to_string_t(newInfo));
        arr[arr.size()] = u;
    }

    std::string Distillery::rebuildRequestedInfo(CCodeProject* project, const std::vector<DistilledStep>& trajectory, int newDebugStep)
    {
        m_debugContext.clear();
        
        std::string requestedInfo;
        for(int i=0; i<newDebugStep; ++i)
        {
            if(NextDebugStep::isInformationRequest(trajectory[i].debug_step.action_type))
            {
                DebugStep debugStep;
                requestedInfo += m_debugContext.stepInfo(m_project, m_test,
                                                         trajectory[i].debug_step.action_type,
                                                         trajectory[i].debug_step.action_subject,
                                                         trajectory[i].debug_step.motivation,
                                                         trajectory[i].debug_step.invocation,
                                                         trajectory[i].debug_step.line_number,
                                                         debugStep);
            }
        }

        if(!requestedInfo.empty())
        {
            requestedInfo = "\n//Information requested previous steps starts here\n\n" + requestedInfo;
            requestedInfo += "\n//Information requested previous steps ends here\n\n";
        }
    
        return requestedInfo;
    }

    std::string Distillery::getTrajectoryPrologue(CCodeProject* project, int originalRunStep, int distilledRunStep, const std::string& summary)
    {
        std::string prologue;
        
        //Ensure we have the correct info incorporating the modification from the last debug step
        prologue += m_debugContext.getHighLevelAppInfo(m_project, {}, 0, PRINT_MAX_FUNCTIONS_DEPTH);
        prologue += "\n\n";
        
        prologue += "\nSUMMARY OF PREVIOUS STEPS:\n\n";
        prologue += summary;
        
        std::string lastRunTestLog = m_debugContext.loadTestLogFromStep(project, m_test, originalRunStep);
        
        prologue += "\n\nINFORMATION FOR THE LAST RUN STEP: " + std::to_string(distilledRunStep) + " STARTS HERE\n";
       
        prologue += lastRunTestLog;
        prologue += "INFORMATION FOR THE LAST RUN STEP ENDS HERE\n\n\n";
        
        m_prologue = prologue;
        return prologue;
    }

    static bool isIdentChar(char c)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_';
    }

    static bool containsToken(const std::string& text, const std::string& token)
    {
        if (token.empty()) return false;
        
        std::size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos)
        {
            const bool leftOk = (pos == 0) || !isIdentChar(text[pos - 1]);
            const std::size_t endPos = pos + token.size();
            const bool rightOk = (endPos >= text.size()) || !isIdentChar(text[endPos]);
            if (leftOk && rightOk) return true;
            pos = endPos;
        }
        return false;
    }

    static void addMentionedFunctionsFromText(const std::string& text,
                                              const std::set<std::string>& candidates,
                                              std::set<std::string>& out)
    {
        for (const auto& fn : candidates)
        {
            if (containsToken(text, fn))
            {
                out.insert(fn);
            }
        }
    }

    static void extractIdentifierTokensFromRegex(const std::string& pattern,
                                                 std::set<std::string>& out)
    {
        std::string token;
        bool escaped = false;
        
        auto flush = [&]() {
            if (!token.empty()) {
                out.insert(token);
                token.clear();
            }
        };
        
        for (char c : pattern)
        {
            if (escaped) {
                escaped = false;
                flush();
                continue;
            }
            
            if (c == '\\') {
                escaped = true;
                flush();
                continue;
            }
            
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) || c == '_') {
                token.push_back(c);
            } else {
                flush();
            }
        }
        
        flush();
    }

    std::map<int, StepDisclosureMapEntry>
    Distillery::buildDisclosureMap(CCodeProject* project,
                                   const EditSourceSequence& optimalSequence,
                                   int runStep,
                                   const std::string& summary)
    {
        std::map<int, StepDisclosureMapEntry> out;
        if (!project || optimalSequence.steps.empty()) return out;
        
        // Reset to exactly the same starting state as distillation.
        goTo(project, runStep - 1);
        m_debugContext.clear();
        
        const std::set<std::string> functionCandidates = project->getNodeNames();
        
        std::string prevSteps;
        std::string requestedInfo;
        std::set<std::string> fixableByFunctionInfo;
        
        int stepId = runStep;
        
        for (std::size_t i = 0; i < optimalSequence.steps.size(); ++i, ++stepId)
        {
            const auto& step = optimalSequence.steps[i];
            
            // Build the same textual view used by distillStep.
            std::string currentTrajectory = getTrajectoryPrologue(project, runStep, runStep, summary);
            currentTrajectory += prevSteps;
            
            if (!requestedInfo.empty())
            {
                currentTrajectory += "\n//Information requested previous steps starts here\n\n";
                currentTrajectory += requestedInfo;
                currentTrajectory += "\n//Information requested previous steps ends here\n\n";
            }
            
            // Distilled step files start from runStep+1 (run_test itself is not a step_ sample).
            if (i > 0)
            {
                StepDisclosureMapEntry entry;
                const DebugVisibility& vis = m_debugContext.visibility();
                
                entry.visible_functions = vis.m_functions;
                entry.visible_data_types = vis.m_dataTypes;
                entry.fixable_functions = fixableByFunctionInfo;
                
                // Text parity fallback: include names explicitly visible in the distilled prompt text.
                addMentionedFunctionsFromText(currentTrajectory, functionCandidates, entry.visible_functions);
                
                out[stepId] = std::move(entry);
            }
            
            // Advance simulated visibility using the same context provider path.
            if (step->action_type == "function_info")
            {
                fixableByFunctionInfo.insert(step->action_subject);
            }
            
            if (NextDebugStep::isInformationRequest(step->action_type))
            {
                DebugStep dbgStep;
                std::string info = m_debugContext.stepInfo(m_project ? m_project : project,
                                                           m_test,
                                                           step->action_type,
                                                           step->action_subject,
                                                           std::string(),
                                                           step->invocation,
                                                           step->line_number,
                                                           dbgStep);
                
                if (!info.empty())
                {
                    if (!requestedInfo.empty()) requestedInfo += "\n\n";
                    requestedInfo += info;
                }
                
                prevSteps += "\nSTEP " + std::to_string(stepId) + " ";
                prevSteps += dbgStep.summary() + "\n\n";
            }
            else
            {
                // Keep step trace shape consistent even for non-info actions.
                DebugStep dbgStep;
                dbgStep.m_action = step->action_type;
                dbgStep.m_subject = step->action_subject;
                dbgStep.m_invocation = step->invocation;
                dbgStep.m_lineNumber = step->line_number;
                
                prevSteps += "\nSTEP " + std::to_string(stepId) + " ";
                prevSteps += dbgStep.summary() + "\n\n";
            }
        }
        
        return out;
    }

    std::pair<std::string, std::string> Distillery::validateSequence(CCodeProject* project, const EditSourceSequence& optimalSequence, int originalSize, int startStep)
    {
        std::string feedback;
        std::string recommendation;
        
        if(optimalSequence.steps.size() < 2)
        {
            feedback += "The optimal sequence must have atleast two steps run_test and fix_function.\n\n";
            return std::pair<std::string, std::string>(); //We should not continue
        }
        
        std::string summary;
        getSummaryStepForStep(project, startStep, summary);
        auto disclosure = buildDisclosureMap(project, optimalSequence, startStep, summary);
        
        if((int)optimalSequence.steps.size() > originalSize + 2)
        {
            std::string originalSizeStr = std::to_string(originalSize);
            std::string optimalSizeStr = std::to_string(optimalSequence.steps.size());
            feedback += "The optimal sequence has too many steps (" + optimalSizeStr + ") compared to the original (";
            feedback += originalSizeStr + "). Maximum allowed expansion is +2 steps.\n\n";
        }
        
        std::string firstAction = optimalSequence.steps.front()->action_type;
        if(firstAction != "run_test")
        {
            feedback += "The first step in the optimal sequecne is from type '" + firstAction;
            feedback += "' it must be run_test.\n\n";
        }
        
        std::string lastAction = optimalSequence.steps.back()->action_type;
        if(//optimalSequence.steps.back()->action_type != "run_test" &&
           optimalSequence.steps.back()->action_type != "fix_function")
        {
            feedback += "The last step in the optimal sequecne is from type '" + lastAction;
            feedback += "' it must be fix_function.\n\n";
        }
        
        const std::set<std::string> functionCandidates = (m_project ? m_project : project)->getNodeNames();
        
        int stepIndex = 1;
        for(const auto& step : optimalSequence.steps)
        {
            // stepIndex is 1-based; stepId aligns with distilled step ids.
            const int stepId = startStep + stepIndex - 1;
            if (step->action_type.empty())
            {
                feedback += "Step " + std::to_string(stepId) + " has empty action_type.\n";
                feedback += "Every optimized step must have a valid non-empty action_type.\n\n";
            }

            if (step->invocation == 0)
            {
                feedback += "Step " + std::to_string(stepId) + " has invalid invocation=0.\n";
                feedback += "Use invocation=1 when not needed.\n\n";
            }
            
            if (step->original_step == 0 || step->original_step < -1)
            {
                feedback += "Step " + std::to_string(stepId) + " has invalid original_step=";
                feedback += std::to_string(step->original_step) + ".\n";
                feedback += "original_step must be > 0 (mapped to original trajectory) or -1 (synthetic inserted step).\n\n";
            }
            
            if (step->action_type == "debug_function" && step->original_step <= 0)
            {
                feedback += "Step " + std::to_string(stepId) + " debug_function has invalid original_step=";
                feedback += std::to_string(step->original_step) + ".\n";
                feedback += "debug_function must map to a real original step (> 0).\n\n";
            }
            
            if (step->action_type == "debug_function" && step->original_step > 0)
            {
                // Keep mapping semantics identical to distillStep(): originalStep -> step_(originalStep-1)/nextStep.json
                const int mappedStepId = step->original_step - 1;
                if (mappedStepId <= 0 || mappedStepId > m_fromStep + (int)m_trajectory.size())
                {
                    feedback += "Step " + std::to_string(stepId) + " debug_function maps to invalid original step id ";
                    feedback += std::to_string(mappedStepId) + ".\n\n";
                }
                else
                {
                    std::string stepDir = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + std::to_string(mappedStepId);
                    web::json::value stepJson;
                    if (!loadJson(stepJson, stepDir + "/nextStep.json"))
                    {
                        feedback += "Step " + std::to_string(stepId) + " debug_function mapping failed: missing original nextStep.json at ";
                        feedback += stepDir + "/nextStep.json\n\n";
                    }
                    else
                    {
                        NextDebugStep originalStep;
                        originalStep.from_json(stepJson);
                        
                        if (originalStep.action_type != "debug_function")
                        {
                            feedback += "Step " + std::to_string(stepId) + " debug_function mapping mismatch: ";
                            feedback += "mapped original step is action_type='" + originalStep.action_type + "' (expected debug_function).\n\n";
                        }
                        
                        if (originalStep.action_type == "debug_function" &&
                            (originalStep.action_subject != step->action_subject ||
                             originalStep.invocation != step->invocation ||
                             originalStep.line_number != step->line_number))
                        {
                            feedback += "Step " + std::to_string(stepId) + " debug_function mapping mismatch in locked fields.\n";
                            feedback += "Expected from original: subject='" + originalStep.action_subject + "', ";
                            feedback += "invocation=" + std::to_string(originalStep.invocation) + ", ";
                            feedback += "line_number=" + std::to_string(originalStep.line_number) + ".\n";
                            feedback += "Got optimized: subject='" + step->action_subject + "', ";
                            feedback += "invocation=" + std::to_string(step->invocation) + ", ";
                            feedback += "line_number=" + std::to_string(step->line_number) + ".\n\n";
                        }
                    }
                }
            }
            
            if (step->line_number == (uint32_t)-1)
            {
                feedback += "Step " + std::to_string(stepId) + " has invalid line_number=UINT_MAX";
                feedback += " (likely wrapped from a negative value).\n";
                feedback += "Use line_number=0 when not needed.\n\n";
            }
            
            if (stepIndex > 1) // only distilled step_* entries
            {
                auto it = disclosure.find(stepId);
                if (it != disclosure.end())
                {
                    const auto& d = it->second;
                    
                    std::string visiblePreview;
                    {
                        int shown = 0;
                        const int kMaxShown = 30;
                        for (const auto& fn : d.visible_functions)
                        {
                            if (!visiblePreview.empty()) visiblePreview += ", ";
                            visiblePreview += fn;
                            ++shown;
                            if (shown >= kMaxShown) break;
                        }
                        if (visiblePreview.empty()) visiblePreview = "<none>";
                        else if ((int)d.visible_functions.size() > shown) visiblePreview += ", ...";
                    }
                    
                    if (
                        (step->action_type == "function_info" ||
                         step->action_type == "fix_function" ||
                         step->action_type == "call_graph" ||
                         step->action_type == "debug_function") &&
                        
                        !step->action_subject.empty() &&
                        step->action_subject != "none" &&
                        d.visible_functions.find(step->action_subject) == d.visible_functions.end()
                        )
                    {
                        feedback += "Step " + std::to_string(stepId) + " action '" + step->action_type +
                        "' uses subject '" + step->action_subject +
                        "' that is not visible in CURRENT TRAJECTORY at this step.\n";
                        feedback += "Visibility rule: a function is visible only if it appears in information available up to this step "
                        "(prologue + last run_test info + prior distilled steps + prior info responses), "
                        "not from skipped/original trajectory steps.\n";
                        feedback += "Recovery: regenerate the optimized sequence so this step uses a currently visible subject, "
                        "or add earlier info steps (e.g., function_info on a visible caller/dependency) that disclose '" +
                        step->action_subject + "' before this step.\n\n";

                        feedback += "FAILING_STEP_INDEX: " + std::to_string(stepId) + "\n";
                        feedback += "DISALLOWED_SUBJECT: " + step->action_subject + "\n";
                        feedback += "ALLOWED_VISIBLE_CANDIDATES_FOR_THIS_STEP: ";
                        feedback += buildAllowedCandidatesLine(d.visible_functions, step->action_subject, 35) + "\n";
                        feedback += "Currently visible functions at step " + std::to_string(stepId) + ": " + visiblePreview + "\n";
                    }
                    
                    if (step->action_type == "fix_function" &&
                        d.fixable_functions.find(step->action_subject) == d.fixable_functions.end())
                    {
                        feedback += "Step " + std::to_string(stepId) + " fix_function target '" + step->action_subject +
                        "' lacks prior function_info disclosure in CURRENT TRAJECTORY.\n";
                        feedback += "Fix precondition: before fix_function(X), the optimized sequence must contain an earlier function_info(X).\n";
                        feedback += "Recovery: insert function_info('" + step->action_subject + "') before this fix, "
                        "then keep fix_function('" + step->action_subject + "') as the final step.\n\n";
                        
                        std::string fixablePreview;
                        {
                            int shown = 0;
                            const int kMaxShown = 10;
                            for (const auto& fn : d.fixable_functions)
                            {
                                if (!fixablePreview.empty()) fixablePreview += ", ";
                                fixablePreview += fn;
                                ++shown;
                                if (shown >= kMaxShown) break;
                            }
                            if (fixablePreview.empty()) fixablePreview = "<none>";
                            else if ((int)d.fixable_functions.size() > shown) fixablePreview += ", ...";
                        }
                        feedback += "FAILING_STEP_INDEX: " + std::to_string(stepId) + "\n";
                        feedback += "DISALLOWED_SUBJECT: " + step->action_subject + "\n";
                        feedback += "ALLOWED_FIXABLE_CANDIDATES_FOR_THIS_STEP: " + fixablePreview + "\n";
                        feedback += "Functions already eligible for fix (prior function_info) at step ";
                        feedback += std::to_string(stepId) + ": " + fixablePreview + "\n";
                    }
                    
                    if (step->action_type == "search_source")
                    {
                        std::set<std::string> regexTokens;
                        extractIdentifierTokensFromRegex(step->action_subject, regexTokens);
                        
                        std::set<std::string> referencedFunctions;
                        for (const auto& tok : regexTokens)
                        {
                            if (functionCandidates.count(tok))
                            {
                                referencedFunctions.insert(tok); // exact-name only
                            }
                        }
                        
                        std::set<std::string> notVisible;
                        for (const auto& fn : referencedFunctions)
                        {
                            if (!d.visible_functions.count(fn))
                            {
                                notVisible.insert(fn);
                            }
                        }
                        
                        if (!notVisible.empty())
                        {
                            feedback += "Step " + std::to_string(stepId) + " search_source regex references function names ";
                            feedback += "not visible in CURRENT TRAJECTORY at this step.\n";
                            feedback += "Disallowed function names in regex: ";
                            feedback += getAsCsv(notVisible, 30) + "\n";
                            feedback += "FAILING_STEP_INDEX: " + std::to_string(stepId) + "\n";
                            feedback += "DISALLOWED_SUBJECT(regex): " + step->action_subject + "\n";
                            feedback += "ALLOWED_VISIBLE_FUNCTION_TOKENS_FOR_REGEX: ";
                            feedback += getAsCsv(d.visible_functions, 40) + "\n";
                            feedback += "Currently visible functions at step ";
                            feedback += std::to_string(stepId) + ": " + visiblePreview + "\n";
                            feedback += "Recovery: use only currently visible function names in search_source regex, ";
                            feedback += "or add earlier disclosure steps.\n\n";
                        }
                    }
                }
            }
            
            if(step->action_type == "run_test")
            {
                goTo(project, startStep);
            }
            else if(step->action_type == "search_source")
            {
                std::string motivation;
                DebugStep dummy;
                
                std::string searchResult = m_debugContext.stepSearchSource(m_project, step->action_subject, motivation, dummy);
                
                if(searchResult.length() > SEARCH_TRACE_SIZE)
                {
                    recommendation += "The source search for regex:\n";
                    recommendation += step->action_subject + "\n\n";
                    recommendation += "Returned too many matches, which inflates context and training data size. ";
                    recommendation += "Suggest a more specific pattern, grounded only in information available up to this point in the trajectory.\n\n";
                    recommendation += searchResult + "\n\n";
                }
            }
            else if(step->action_type == "fix_function")
            {
                if(stepIndex != optimalSequence.steps.size())
                {
                    feedback += "We can't have fix_function step in the middle of the sequence being optimized, only at the end.\n\n";
                }
            }
            
            stepIndex++;
        }
        
        return std::make_pair(feedback, recommendation);
    }

    void Distillery::pushOptimizedFixTrack(CCodeProject* project, const std::string& message, EditSourceSequence& optimalSequence)
    {
        project->pushMessage(message, "user", true);
        std::string sequenceMsg = utility::conversions::to_utf8string(optimalSequence.to_json().serialize());
        project->pushMessage(sequenceMsg, "assistant", true);
    }

    std::string Distillery::optimizeFixTrack(CCodeProject* project,
                                             Cache& cache,
                                             const std::string& trajectoryAnalysis,
                                             const std::string& fixTrack,
                                             uint32_t fixStep,
                                             uint32_t idealMaxCount,
                                             EditSourceSequence& optimalSequence)
    {
        auto range = getFixTrackRange(project, fixStep);
        int runStepIndex = range.first;
        int runStep = trajectoryIndexToStep(runStepIndex);
        
        
        std::string trajecotry;
        std::string summary;
        int summaryStep = getSummaryStepForStep(project, runStep, summary);
        if(summaryStep > m_fromStep)
        {
            trajecotry += summary;
        }
        else
        {
            summaryStep = runStep;
        }
        
        int summaryStepIdx = stepToTrajectoryIndex(summaryStep);
        
        int originalSize = fixStep - runStep + 1;
        
        std::string prevSteps;
        for(int s=summaryStepIdx;s<runStepIndex;++s)
        {
            int step = trajectoryIndexToStep(s);
            std::string stepIdStr = std::to_string(step);
            
            prevSteps += "\nSTEP " + stepIdStr + ":\n";
            prevSteps += m_trajectory[s].fullInfo() + "\n";
        }
        
        trajecotry += prevSteps;
        
        if(trajecotry.empty())
        {
            trajecotry = "Information for previous steps is not available";
        }

        EditSourceSequence originalSequence;
        {
            const int fixStepIndex = stepToTrajectoryIndex(fixStep);
            for (int s = runStepIndex; s <= fixStepIndex; ++s)
            {
                const DebugStep& srcStep = m_trajectory[s];
                const int sourceStepId = trajectoryIndexToStep(s);

                OptimizedStep opt;
                opt.action_type = srcStep.m_action;
                opt.action_subject = srcStep.m_subject.empty() ? "none" : srcStep.m_subject;
                opt.line_number = srcStep.m_lineNumber < 0 ? 0 : (uint32_t)srcStep.m_lineNumber;
                opt.invocation = srcStep.m_invocation <= 0 ? 1 : (uint32_t)srcStep.m_invocation;
                opt.original_step = sourceStepId;

                originalSequence.steps.push_back(std::make_shared<OptimizedStep>(opt));
            }
        }

        auto disclosureForOptimize = buildDisclosureMap(project, originalSequence, runStep, summary);
        std::string disclosureContract = buildDisclosureContractText(disclosureForOptimize, 45);
        
        goTo(project, fixStep-1);
        std::string appInfo = m_debugContext.getHighLevelAppInfo(m_project, "", PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH);
        
        Prompt promptOptimizeFixTrack("OptimizeFixTrack.txt",{
                            {"app_info", appInfo},
                            {"trajectory", trajecotry},
                            {"fix_track", fixTrack},
                            {"disclosure_contract", disclosureContract}
        });
        
        web::json::value object;
        web::json::value schema;
        setupSchema<EditSourceSequence>(schema);
        
        project->captureContext(std::string());
        
        InfoRequest infoRequest;
    
        std::string message = promptOptimizeFixTrack.str();
        
        std::string maxInfoRequestsStr = std::to_string(OPTIMIZE_TRACK_MAX_INFO_REQUESTS);
        Prompt promptOptimizeFixTrackInfo("OptimizeFixTrackInfo.txt",{
            {"max_requests", maxInfoRequestsStr}
        });
        
        message += promptOptimizeFixTrackInfo.str();
        
        std::string responseInfo = project->provideInfoLoop(message, OPTIMIZE_TRACK_MAX_INFO_REQUESTS);
        
        Prompt promptOptimizeFixTrackEpilog("OptimizeFixTrackEpilog.txt",{
        });
        responseInfo += promptOptimizeFixTrackEpilog.str();
        
        project->inference(cache, responseInfo, schema, object);
        
        optimalSequence.from_json(object);
        
        auto feedback = validateSequence(project, optimalSequence, originalSize, runStep);
        bool firstFb = true;
        uint32_t attempts = 0;
        while(attempts < 8 &&
                (
                    !feedback.first.empty() ||
                    (firstFb && !feedback.second.empty())
                )
             )
        {
            std::string feedbackPrompt = feedback.first + feedback.second;
            project->inference(cache, feedbackPrompt, schema, object);
        
            optimalSequence.clear();
            optimalSequence.from_json(object);
            feedback = validateSequence(project, optimalSequence, originalSize, runStep);
            
            firstFb = false;
            
            attempts++;
        }
        
        project->popContext();

        if (!feedback.first.empty())
        {
            std::cout << "ERROR: Rejecting optimized sequence due to unresolved validator feedback after retries.\n";
            std::cout << summarizeFeedbackText(feedback.first) << std::endl;
            optimalSequence.clear();
            return std::string();
        }
        
        // Hard cap on expansion: allow at most +2 steps over the original run->fix span.
        if ((int)optimalSequence.steps.size() > originalSize + 2)
        {
            std::cout << "ERROR: Rejecting optimized sequence due to expansion > +2. ";
            std::cout << "optimized=" << optimalSequence.steps.size();
            std::cout << ", original=" << originalSize << std::endl;
            optimalSequence.clear();
            return std::string();
        }
        
        std::string ctxMessage = promptOptimizeFixTrack.str();
        ctxMessage += promptOptimizeFixTrackEpilog;
        return ctxMessage;
    }

    json::value Distillery::buildTrainingData(CCodeProject* project,
                                              const std::string& trajectory,
                                              const std::string& content,
                                              const std::string& thinking,
                                              web::json::value* schema,
                                              web::json::value* messages)
    {
        json::value messagesArray = json::value::array();
        
        // system
        json::value systemMessage;
        systemMessage[U("role")] = json::value::string(U("system"));
        std::string systemPrompt = m_dbgSystemPrompt;
        systemMessage[U("content")] = json::value::string(utility::conversions::to_string_t(systemPrompt));
        messagesArray[messagesArray.size()] = systemMessage;
        
        // schema/instrumentation tail
        web::json::value localSchema;
        if (!schema) {
            setupSchema<NextDebugStep>(localSchema);
            schema = &localSchema;
        }
        
        std::string nextStepTail = "\n\n" + m_nextStepPrompt;
        nextStepTail += project->getInstrumentationMessage(*schema);
        
        if (messages && messages->is_array()) {
            // interleaved mode: user prologue + prior interleaved msgs
            json::value prologueMessage;
            prologueMessage[U("role")] = json::value::string(U("user"));
            
            std::string base = m_projDesc + "\n\n";
            if (!m_prologue.empty()) {
                base += m_prologue;
            } else {
                // fallback if prologue was not set yet
                base += trajectory;
            }
            
            prologueMessage[U("content")] = json::value::string(utility::conversions::to_string_t(base));
            messagesArray[messagesArray.size()] = prologueMessage;
            
            auto& src = messages->as_array();
            for (size_t i = 0; i < src.size(); ++i) {
                messagesArray[messagesArray.size()] = src[i];
            }
            
            // append instruction/schema to the last user message
            auto& arr = messagesArray.as_array();
            int lastUserIdx = -1;
            for (int i = static_cast<int>(arr.size()) - 1; i >= 0; --i) {
                if (!arr[i].is_object() || !arr[i].has_field(U("role")) || !arr[i].at(U("role")).is_string()) {
                    continue;
                }
                
                const std::string role =
                utility::conversions::to_utf8string(arr[i].at(U("role")).as_string());
                if (role == "user") {
                    lastUserIdx = i;
                    break;
                }
            }
            
            if (lastUserIdx >= 0) {
                std::string lastContent;
                if (arr[lastUserIdx].has_field(U("content")) && arr[lastUserIdx].at(U("content")).is_string()) {
                    lastContent = utility::conversions::to_utf8string(arr[lastUserIdx].at(U("content")).as_string());
                }
                lastContent += nextStepTail;
                arr[lastUserIdx][U("content")] = json::value::string(utility::conversions::to_string_t(lastContent));
            } else {
                json::value userMessage;
                userMessage[U("role")] = json::value::string(U("user"));
                userMessage[U("content")] = json::value::string(utility::conversions::to_string_t(nextStepTail));
                arr[arr.size()] = userMessage;
            }
        } else {
            // original single-turn mode
            std::string trajectoryContent = trajectory;
            trajectoryContent += nextStepTail;
            
            json::value trajectoryMessage;
            trajectoryMessage[U("role")] = json::value::string(U("user"));
            
            std::string mergedUserMessage = m_projDesc + "\n\n";
            mergedUserMessage += trajectoryContent;
            trajectoryMessage[U("content")] = json::value::string(utility::conversions::to_string_t(mergedUserMessage));
            
            messagesArray[messagesArray.size()] = trajectoryMessage;
        }
        
        // assistant target
        json::value responseMessage;
        responseMessage[U("role")] = json::value::string(U("assistant"));
        responseMessage[U("thinking")] = json::value::string(utility::conversions::to_string_t(thinking));
        responseMessage[U("content")] = json::value::string(utility::conversions::to_string_t(content));
        messagesArray[messagesArray.size()] = responseMessage;
        
        json::value jsonTrajectory;
        jsonTrajectory[U("messages")] = messagesArray;
        
        return jsonTrajectory;
    }

    std::string Distillery::distillStep(CCodeProject* project,
                                        int originalStep,
                                        int lastOriginalRunStep,
                                        int fixStep, int step, const std::string& summary,
                                        std::string& prevSteps,
                                        const std::string& requestedInfo,
                                        std::string& newInfo,
                                        DistilledStep& nextStep,
                                        const StepDisclosureMapEntry* disclosureEntry)
    {
        web::json::value object;
        
        web::json::value schema;
        setupSchema<DistilledStep>(schema);
        
        Cache cache;
        project->captureContext(std::string());
        
        std::string currentTrajectory = getTrajectoryPrologue(project, lastOriginalRunStep, lastOriginalRunStep, summary);
        
        currentTrajectory += prevSteps;
        
        if(!requestedInfo.empty())
        {
            currentTrajectory += "\n//Information requested previous steps starts here\n\n";
            currentTrajectory += requestedInfo;
            currentTrajectory += "\n//Information requested previous steps ends here\n\n";
        }
        
        std::string stepIdStr = std::to_string(step);
        
        std::set<std::string> allowedNewHelpers;
        
        std::string newFunctionsHint;
        if(nextStep.debug_step.action_type == "fix_function")
        {
            CCodeNode* ccFixed =
            project->getNodeByName(nextStep.debug_step.action_subject);
            
            //Also collect helper names allowed in narrative
            auto collectHelpers = [&](int keyStep) {
                auto it = m_newFunctionsPerStep.find(keyStep);
                if(it == m_newFunctionsPerStep.end() || it->second.empty())
                    return;
                
                for(const auto& func : it->second)
                {
                    CCodeNode* ccNode = project->getNodeByName(func);
                    if(!ccNode) continue;
                    if(ccFixed && !ccFixed->calledInTheSource(func)) continue;
                    if(!ccNode->hasPathToMain()) continue;
                    
                    allowedNewHelpers.insert(func);
                    
                    if(newFunctionsHint.empty())
                    {
                        newFunctionsHint += "\n\nThe original fix_function step you're distilling from maybe ";
                        newFunctionsHint += "introduced the following helper functions that were used in the source.";
                        newFunctionsHint += " Consider mentioning their addition in the motivation and analysis:\n";
                    }
                    
                    newFunctionsHint += "//" + ccNode->m_prototype.brief;
                    newFunctionsHint += "\n";
                    newFunctionsHint += ccNode->m_prototype.declaration;
                    newFunctionsHint += "\n\n";
                }
            };
            
            collectHelpers(nextStep.m_originalStep);
            collectHelpers(nextStep.m_originalStep + 1); // robust against step-index offset
        }
        
        const std::string lockedActionType    = nextStep.debug_step.action_type;
        const std::string lockedActionSubject = nextStep.debug_step.action_subject;
        const uint32_t    lockedInvocation    = nextStep.debug_step.invocation;
        const uint32_t    lockedLineNumber    = nextStep.debug_step.line_number;
        
        const std::set<std::string> functionCandidates = (m_project ? m_project : project)->getNodeNames();
        
        Prompt promptDistillStep("DistillStep.txt",{
                            {"current_trajectory", currentTrajectory},
                            {"new_functions", newFunctionsHint},
                            {"step_id", stepIdStr}
        });
        
        project->inference(cache, promptDistillStep, schema, object);
        
        nextStep.debug_step.clear();
        nextStep.from_json(object);
        
        DebugStep debugStep;
        
        std::string feedback;
        uint32_t attempts = 0;
        const uint32_t kMaxAttempts = 8;
        bool converged = false;
        
        auto summarizeFeedback = [](const std::string& s) -> std::string {
            const std::size_t kMax = 600;
            if (s.size() <= kMax) return s;
            return s.substr(0, kMax) + "...";
        };
        
        while(true)
        {
            feedback.clear();
            
            if (nextStep.debug_step.action_type != lockedActionType ||
                nextStep.debug_step.action_subject != lockedActionSubject ||
                nextStep.debug_step.invocation != lockedInvocation ||
                nextStep.debug_step.line_number != lockedLineNumber)
            {
                feedback += "ACTION LOCK violation.\n";
                feedback += "Preserve exactly these fields from OPTIMIZED TRAJECTORY:\n";
                feedback += "  action_type=" + lockedActionType + "\n";
                feedback += "  action_subject=" + lockedActionSubject + "\n";
                feedback += "  invocation=" + std::to_string(lockedInvocation) + "\n";
                feedback += "  line_number=" + std::to_string(lockedLineNumber) + "\n";
                feedback += "Regenerate. Change only motivation/motivation_summary/analysis.\n\n";
            }
            
            if(nextStep.debug_step.action_type == "debug_function")
            {
                int stepId = originalStep-1;
                std::string stepIdStr = std::to_string(stepId);
                if(stepId > 0 && stepId <= m_fromStep + m_trajectory.size())
                {
                    std::string stepDir = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + stepIdStr;
                    web::json::value stepJson;
                    loadJson(stepJson, stepDir + "/nextStep.json");
                    
                    NextDebugStep prevStep;
                    prevStep.from_json(stepJson);
                    
                    if(prevStep.action_type != "debug_function")
                    {
                        feedback += "Breakpoints and their conditions must exactly match the one from the original step\n\n";
                    }
                    else
                        if(nextStep.debug_step.breakpoints.size() != prevStep.breakpoints.size())
                        {
                            feedback += "Breakpoints and their conditions must exactly match the one from the original step\n\n";
                        }
                        else
                        {
                            int bpCount = (int)nextStep.debug_step.breakpoints.size();
                            
                            for(int i=0; i<bpCount; ++i)
                            {
                                int j=0;
                                for(; j<bpCount; ++j)
                                {
                                    if(nextStep.debug_step.breakpoints[i]->isTheSame(*prevStep.breakpoints[j]))
                                    {
                                        break;
                                    }
                                }
                                
                                if(j==bpCount)
                                {
                                    feedback += "Breakpoints and their conditions must exactly match the one from the original step\n\n";
                                    break;
                                }
                            }
                        }
                }
                else
                {
                    feedback += "The suggested mapping to the original step " + stepIdStr + " doesn't exist in the trajectory\n\n";
                }
            }
            
            if (disclosureEntry)
            {
                std::string narrative;
                narrative += nextStep.debug_step.motivation;
                narrative += "\n";
                narrative += nextStep.motivation_summary;
                narrative += "\n";
                narrative += nextStep.analysis; // becomes assistant thinking in training
                
                std::set<std::string> mentionedFunctions;
                addMentionedFunctionsFromText(narrative, functionCandidates,
                                              mentionedFunctions);
                
                std::set<std::string> disallowed;
                for (const auto& fn : mentionedFunctions)
                {
                    if (fn == lockedActionSubject) continue; // allow locked subject mention
                    
                    const bool isVisible = disclosureEntry->visible_functions.count(fn) > 0;
                    const bool isAllowedHelper = allowedNewHelpers.count(fn) > 0;
                    
                    if (!isVisible && !isAllowedHelper)
                    {
                        disallowed.insert(fn);
                    }
                }
                
                if (!disallowed.empty())
                {
                    feedback += "Grounding violation in narrative fields (motivation/motivation_summary/analysis).\n";
                    feedback += "Disallowed function names: " + getAsCsv(disallowed, 30) + "\n";
                    feedback += "Currently visible functions at this step: " + getAsCsv(disclosureEntry->visible_functions, 30) + "\n";
                    feedback += "Regenerate using only functions disclosed in CURRENT TRAJECTORY.\n\n";
                    
                    if (!allowedNewHelpers.empty())
                    {
                        feedback += "Allowed newly introduced helper functions for this fix step: ";
                        feedback += getAsCsv(allowedNewHelpers, 30) + "\n";
                    }
                }
                
                const bool lockedNeedsMention =
                (lockedActionType == "function_info" || lockedActionType ==
                 "fix_function" || lockedActionType == "debug_function") &&
                !lockedActionSubject.empty() && lockedActionSubject != "none" &&
                functionCandidates.count(lockedActionSubject);
                
                if (lockedNeedsMention && !containsToken(narrative, lockedActionSubject))
                {
                    feedback += "Grounding violation: narrative must explicitly mention locked action_subject '";
                    feedback += lockedActionSubject + "'.\n\n";
                }
            }
            
            if (feedback.empty())
            {
                converged = true;
                break;
            }
            
            if (attempts >= kMaxAttempts)
            {
                break; // keep last response, but report explicit non-convergence
            }
            
            ++attempts;
            project->inference(cache, feedback, schema, object);
            nextStep.debug_step.clear();
            nextStep.from_json(object);
            
        }
        
        if (!converged)
        {
            std::cout << "Step distillation didn't converge after ";
            std::cout << kMaxAttempts << " retries at distilled step " << step;
            std::cout << " (original step " << originalStep << "). Keeping last response.\n";
            std::cout << "Last validation feedback:\n" << summarizeFeedback(feedback) << std::endl;
        }
        
        if(nextStep.debug_step.isInformationRequest())
        {
            newInfo = m_debugContext.stepInfo(m_project, m_test,
                                              nextStep.debug_step.action_type,
                                              nextStep.debug_step.action_subject,
                                              nextStep.debug_step.motivation,
                                              nextStep.debug_step.invocation,
                                              nextStep.debug_step.line_number,
                                              debugStep);
            
            prevSteps += "\nSTEP " + stepIdStr + " ";
            prevSteps += debugStep.summary() + "\n";
            prevSteps += nextStep.motivation_summary + "\n\n";
        }
        
        project->popContext();
        
        return currentTrajectory;
    }

    bool Distillery::checkFixTrackData(CCodeProject* project, uint32_t startStep, uint32_t fixStep)
    {
        std::string testStepStr = std::to_string(startStep);
        std::string fixStepStr = std::to_string(fixStep);
        
        std::string datasetDir = project->getProjDir() + "/dataset/" + m_test.name;
        if(!boost_fs::exists(datasetDir))
        {
            std::cout << "The dataset directory doesn't exists: " << datasetDir << std::endl;
            return false;
        }
        
        std::string sysAnalysisSample = "system_" + testStepStr + "_" + fixStepStr;
        
        std::string systemFile = datasetDir + "/" + sysAnalysisSample + ".json";
        if(!boost_fs::exists(systemFile))
        {
            std::cout << "The system analysis training sample doesn't exists: " << systemFile << std::endl;
            return false;
        }
        
        for(uint32_t s=startStep+1; s<=fixStep; ++s)
        {
            std::string currentStepStr = std::to_string(s);
            std::string stepFile = datasetDir + "/step_" + testStepStr + "_" + currentStepStr + ".json";
            if(!boost_fs::exists(stepFile))
            {
                std::cout << "The step training samepl doesn't exists: " << stepFile << std::endl;
                return false;
            }
        }
        
        return true;
    }

    void Distillery::removeFixTrackData(CCodeProject* project, uint32_t startStep, uint32_t fixStep)
    {
        std::string datasetDir = project->getProjDir() + "/dataset/" + m_test.name;
        std::string startStepStr = std::to_string(startStep);
        std::string fixStepStr = std::to_string(fixStep);
        
        std::string systemAnalysisFile = datasetDir + "/system_" + startStepStr + "_" + fixStepStr;
        boost_fs::remove(systemAnalysisFile + ".json");
        boost_fs::remove(systemAnalysisFile + ".txt");
        
        std::string prefix = "step_" + startStepStr + "_";
        for (boost_fs::directory_iterator it(datasetDir), end; it != end; ++it)
        {
            if (!boost_fs::is_regular_file(*it))
                continue;

            const boost_fs::path& p = it->path();

            if ((p.extension() == ".json" || p.extension() == ".txt") &&
                startsWith(p.filename().string(), prefix))
            {
                boost_fs::remove(p);
            }
        }
    }

    std::string Distillery::prevStepsSummary(const std::vector<DistilledStep>& distilledTrajectory, int startStep)
    {
        std::string summary;
        
        int stepId = startStep;
        for(const auto& step : distilledTrajectory)
        {
            std::string stepIdStr = std::to_string(stepId);
            
            summary += "\n\nSTEP " + stepIdStr + " ";
            summary += step.debug_step.action_type + " " + step.debug_step.action_subject;
            
            if(step.debug_step.action_type == "function_info")
            {
                summary += " invocation " + std::to_string(step.debug_step.invocation);
            }
            else if(step.debug_step.action_type == "log_info")
            {
                summary += " line " + std::to_string(step.debug_step.line_number);
                summary += " invocation " + std::to_string(step.debug_step.invocation);
            }
            
            if(!step.motivation_summary.empty())
            {
                summary += "\n" + step.motivation_summary + "\n\n";
            }
            
            if(step.debug_step.action_type == "debug_function")
            {
                summary += "\nBreakpoints:\n";
                summary += m_debugContext.getBreakpointsInfo(false, step.debug_step.action_subject, step.debug_step.breakpoints);
                
                //summary += "\n\nDebug Notes\n";
                summary += step.m_debugNotes;
            }
            else if(step.debug_step.action_type == "run_test")
            {
                //summary += "\nDebug Notes\n";
                summary += step.m_debugNotes;
            }
            
            stepId++;
        }
        
        return summary;
    }

    void Distillery::distillFixTrack(CCodeProject* project, const std::string& trajectoryAnalysis, uint32_t fixStep)
    {
        auto fixRange = getFixTrackRange(project, fixStep);
        bool needsOptimization = fixRange.second - fixRange.first > 3;
        std::string fixTrack = trackForFix(project, fixStep);
        
        int startStep = trajectoryIndexToStep(fixRange.first);
        std::string testStepStr = std::to_string(startStep);
        std::string fixStepStr = std::to_string(fixStep);
        
        std::string datasetDir = project->getProjDir() + "/dataset/" + m_test.name;
        std::string sysAnalysisSample = "system_" + testStepStr + "_" + fixStepStr;
        
        if(checkFixTrackData(project, startStep, fixStep))
        {
            return ;
        }
        
        removeFixTrackData(project, startStep, fixStep);
        
        project->captureContext(std::string());
        
        std::string optimizedJson = "optimized_fix_" + testStepStr + "_" + fixStepStr + ".json";
        EditSourceSequence optimalSequence;
        std::string optimalSequenceMsg;
        if(needsOptimization)
        {
            Cache cache(datasetDir, optimizedJson);
            for(uint32_t i = 0; i<MAX_OPTIMAL_FIX_TRACK_ROLLOUTS; ++i)
            {
                EditSourceSequence sequence;
                std::string sequenceMsg = optimizeFixTrack(project, cache,
                                                           trajectoryAnalysis,
                                                           fixTrack, fixStep,
                                                           (uint32_t)optimalSequence.steps.size(),
                                                           sequence);
                
                if(sequence.steps.size() > 0 &&
                   (i==0 || sequence.steps.size() < optimalSequence.steps.size()))
                {
                    optimalSequenceMsg = sequenceMsg;
                    optimalSequence = sequence;
                }
                
                if(optimalSequence.steps.size() < 4)
                {
                    break;
                }
            }
            
            if(optimalSequence.steps.size() > 0)
            {
                pushOptimizedFixTrack(project, optimalSequenceMsg, optimalSequence);
            }
        }
        else
        {
            for(int s = fixRange.first; s<fixRange.second; ++s)
            {
                NextDebugStep step;
                int stepId = trajectoryIndexToStep(s);
                std::string stepDir = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + std::to_string(stepId);
                web::json::value stepJson;
                if(!loadJson(stepJson, stepDir + "/nextStep.json"))
                {
                    std::cout << "ERROR: Unable to load fallback optimized step from: ";
                    std::cout << stepDir + "/nextStep.json" << std::endl;
                    project->popContext();
                    return;
                }
                step.from_json(stepJson);
                
                if(step.action_type.empty())
                {
                    std::cout << "ERROR: Fallback optimized step has empty action_type at step ";
                    std::cout << stepId << std::endl;
                    project->popContext();
                    return;
                }
                
                OptimizedStep optimizedStep;
                
                optimizedStep.action_type = step.action_type;
                optimizedStep.action_subject = step.action_subject;
                optimizedStep.line_number = (step.line_number == (uint32_t)-1) ? 0 : step.line_number;
                optimizedStep.invocation = (step.invocation == 0) ? 1 : step.invocation;
                optimizedStep.original_step = stepId;
                
                optimalSequence.steps.push_back(std::make_shared<OptimizedStep>(optimizedStep));
            }
            
            std::string firstRunStr = std::to_string(trajectoryIndexToStep(fixRange.first));
            std::string lastRunStr = std::to_string(trajectoryIndexToStep(fixRange.second));
            optimalSequence.analysis = "The optimal sequence for this fix is the original one from run_test step ";
            optimalSequence.analysis += firstRunStr + " to fix_function step " + lastRunStr;
            
            std::string originalFixSequence = "OPTIMIZED TRAJECTORY READY FOR DISTILLATION:\n\n\n";
            originalFixSequence += fixTrack + "\n";
            
            project->pushMessage(originalFixSequence, "user", true);
        }
        
        if(optimalSequence.steps.empty())
        {
            std::cout << "Skipping fix track due to invalid/empty optimized sequence for ";
            std::cout << optimizedJson << std::endl;
            project->popContext();
            return;
        }

        {
            const int originalSize = fixStep - startStep + 1;
            auto finalValidation = validateSequence(project, optimalSequence, originalSize, startStep);
            if (!finalValidation.first.empty())
            {
                std::cout << "Skipping fix track due to invalid optimized sequence after validation.\n";
                std::cout << summarizeFeedbackText(finalValidation.first) << std::endl;
                project->popContext();
                return;
            }
        }
        
        //Save optimized sequence
        saveJson(optimalSequence.to_json(), datasetDir + "/" + optimizedJson);
        
        std::string summary = distillSummaryBefore(project, startStep, fixStep);
        
        auto disclosure = buildDisclosureMap(project, optimalSequence, startStep, summary);
        
        std::string prevSteps;
        std::string requestedInfo;
        std::vector<std::string> infoRequests;
        m_debugContext.clear();
        
        
        if(needsOptimization &&
           optimalSequence.steps.size() > 2 &&
           optimalSequence.steps.back()->action_type == "run_test")
        {
            //Do not distill the last run_test step
            optimalSequence.steps.pop_back();
        }
        
        for(const auto& step : optimalSequence.steps)
        {
            const bool badOriginalStep =
                (step->original_step == 0 || step->original_step < -1) ||
                (step->action_type == "debug_function" && step->original_step <= 0);
            
            if(step->action_type.empty() || badOriginalStep)
            {
                std::cout << "ERROR: Rejecting optimized sequence with invalid step fields: ";
                std::cout << "action_type='" << step->action_type << "', original_step=" << step->original_step << std::endl;
                project->popContext();
                return;
            }
            
            if(step->line_number == (uint32_t)-1)
            {
                step->line_number = 0;
            }

            if(step->invocation == 0)
            {
                step->invocation = 1;
            }
        }
        
        std::string systemAnalysis;
        
        std::vector<DistilledStep> distilledTrajectory;
        
        std::string startStepStr = std::to_string(startStep);
        int currentStep = startStep;
        web::json::value messages = web::json::value::array();
#if 0
        for(auto step : optimalSequence.steps)
        {
            std::string currentStepStr = std::to_string(currentStep);
            
            prevSteps = prevStepsSummary(distilledTrajectory, startStep);
            
            if(step->action_type == "run_test")
            {
                goTo(project, currentStep);
                
                std::string prevStepJson = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + std::to_string(currentStep-1) + "/dbgStep.json";
                
                DebugStep prevStep;
                bool prevLoaded = prevStep.load(prevStepJson);
                
                DebugStep stepInfo;
                stepInfo.m_logSummary.clear(); //Everything for the system analysis is in the debug notes
                
                if(distillRunStep(project, summary, prevStep.m_subject, currentStep, fixStep, systemAnalysis))
                {
                    //Need to check if there is a reward-hacking review
                    web::json::value rewardHackingRequest;
                    web::json::value rewardHackingResponse;
                    std::pair<bool, std::string> rewardHacking = thinkingForResponse(project, "_RewardHacking.json", currentStep,
                                                                                     rewardHackingRequest, rewardHackingResponse);
                    
                    if(rewardHacking.first && !rewardHacking.second.empty()) //Yes, this is reward-hacking review
                    {
                        //Let's save the system analysis
                        DistilledAanalysis rewardAnalysis;
                        rewardAnalysis.thinking_analysis = rewardHacking.second;
                        rewardAnalysis.system_analysis.debug_notes = systemAnalysis;
                        if(systemAnalysis == "PASS")
                        {
                            rewardAnalysis.system_analysis.log_summary = "Results for all commands match the expected outcomes.\n";
                        }
                        else
                        {
                            rewardAnalysis.system_analysis.log_summary = "Reward-hacking prcatices have been identified.\n";
                        }
                        
                        stepInfo.m_logSummary = rewardAnalysis.system_analysis.log_summary;
                        
                        std::string rewardHackingRequestStr = utility::conversions::to_utf8string(rewardHackingRequest.serialize());

                        saveSystemAnalysis(datasetDir,
                                           "system_" + testStepStr + "_" + fixStepStr,
                                           rewardAnalysis,
                                           rewardHackingRequestStr);
                    }
                }
                
                stepInfo.m_debugNotes = systemAnalysis;
                stepInfo.m_action = "run_test";
                stepInfo.m_lineNumber = 0;
                stepInfo.m_invocation = 1;
                
                //DebugStep prevFixStep;
                if(prevLoaded &&
                   !prevStep.m_subject.empty() &&
                   prevStep.m_subject != "none")
                {
                    stepInfo.m_motivation = "Run the test to verify the fix of '" + prevStep.m_subject;
                    stepInfo.m_motivation += "' and find what else needs fixes to successfully pass the test.";
                    stepInfo.m_subject = prevStep.m_subject;
                    
                    std::string newInfo;
                    
                    //We need debug notes for the system anaysis added to the info
                    //for the multiple turns training set
                    newInfo = stepInfo.m_debugNotes + newInfo;
                    
                    infoRequests.push_back(newInfo);
                }
                else
                {
                    stepInfo.m_motivation = "Run the test to verify the recent fix and find what else needs fixes to successfully pass the test.";
                    stepInfo.m_subject = "none";
                }
                
                prevSteps += "\nSTEP: " + currentStepStr + "\n\n";
                prevSteps += stepInfo.fullInfo() + "\n\n";
                
                std::cout << systemAnalysis;
                
                DistilledStep distilledStep;
                distilledStep.debug_step.action_type    = stepInfo.m_action;
                distilledStep.debug_step.action_subject = stepInfo.m_subject;
                distilledStep.debug_step.invocation     = stepInfo.m_invocation;
                distilledStep.debug_step.line_number    = stepInfo.m_lineNumber;
                //distilledStep.analysis                  = systemAnalysis.thinking_analysis;
                distilledStep.debug_step.motivation     = stepInfo.m_motivation;
                distilledStep.m_debugNotes = systemAnalysis;
                
                distilledTrajectory.push_back(distilledStep);
                addStepToMessages(project, distilledStep, stepInfo.notes(), messages);
            }
            else if(step->action_type == "debug_function")
            {
                //TODO: We must very precisely map debugging steps from the original trajectory,
                //so we can restore the Git to this commit
                goTo(project, step->original_step);
                
                //TODO: We need to recompile the requestedInfo as we have a new run
                
                //TODO: remap the startStep to the original step corresponding on the new debug_function step
                
                DistilledStep nextStep;
                std::string newInfo;
                nextStep.debug_step.action_type = step->action_type;
                nextStep.debug_step.action_subject = step->action_subject;
                nextStep.debug_step.invocation = (step->invocation == 0) ? 1 : step->invocation;
                nextStep.debug_step.line_number = step->line_number;
                nextStep.m_originalStep = step->original_step;
                
                const StepDisclosureMapEntry* disclosureEntry = nullptr;
                auto dIt = disclosure.find(currentStep);
                if (dIt != disclosure.end()) disclosureEntry = &dIt->second;
                
                std::string currentTrajectory = distillStep(project, step->original_step, startStep, fixStep, currentStep,
                                                            summary, prevSteps, requestedInfo, newInfo, nextStep, disclosureEntry);
                
                std::string content = utility::conversions::to_utf8string(nextStep.debug_step.to_json().serialize());
                web::json::value schema;
                setupSchema<NextDebugStep>(schema);
                json::value jsonSample = buildTrainingData(project, currentTrajectory, content, nextStep.analysis, &schema, &messages);
                saveTrainingData(datasetDir, "step_" + startStepStr + "_" + currentStepStr, jsonSample, true);
                
                distilledTrajectory.push_back(nextStep);
                
                DebugStep debugStep;
                {
                    debugStep.m_motivation = nextStep.debug_step.motivation;
                    
                    debugStep.m_action = nextStep.debug_step.action_type;
                    debugStep.m_subject = nextStep.debug_step.action_subject;
                    
                    debugStep.m_lineNumber = nextStep.debug_step.line_number;
                    debugStep.m_invocation = nextStep.debug_step.invocation;
                }
                
                std::string tempPrevSteps = prevSteps;
                tempPrevSteps += "\nSTEP " + currentStepStr + " ";
                tempPrevSteps += debugStep.summary() + "\n";
                tempPrevSteps += nextStep.motivation_summary + "\n\n";
                
                std::string trajectory = distillDebugStep(project, summary,
                                                           tempPrevSteps,
                                                          newInfo,
                                                          nextStep,
                                                          step->original_step,
                                                          startStep, currentStep);
                
                //Now tempPrevSteps is updated in the distillDebugStep to contain only the debug_function step full info
                //so add it to the steps trajectory
                prevSteps += tempPrevSteps;
                
                int newDebugStep = currentStep - startStep;
                requestedInfo = rebuildRequestedInfo(project, distilledTrajectory, newDebugStep);
                addStepToMessages(project, nextStep, newInfo, messages);
            }
            else
            {
                DistilledStep nextStep;
                nextStep.debug_step.action_type = step->action_type;
                nextStep.debug_step.action_subject = step->action_subject;
                nextStep.debug_step.invocation = (step->invocation == 0) ? 1 : step->invocation;
                nextStep.debug_step.line_number = step->line_number;
                nextStep.m_originalStep = step->original_step;
                
                const StepDisclosureMapEntry* disclosureEntry = nullptr;
                auto dIt = disclosure.find(currentStep);
                if (dIt != disclosure.end()) disclosureEntry = &dIt->second;
                
                std::string newInfo;
                std::string currentTrajectory = distillStep(project, step->original_step, startStep, fixStep, currentStep,
                                                            summary, prevSteps, requestedInfo, newInfo, nextStep, disclosureEntry);
                
                if(!newInfo.empty())
                {
                    requestedInfo += "\n\n";
                    requestedInfo += newInfo;
                }
                
                infoRequests.push_back(newInfo);
                
                distilledTrajectory.push_back(nextStep);
                
                //TODO: Find the proper way to save the data!!!
                
                std::string content = utility::conversions::to_utf8string(nextStep.debug_step.to_json().serialize());
                web::json::value schema;
                setupSchema<NextDebugStep>(schema);
                json::value jsonSample = buildTrainingData(project, currentTrajectory, content, nextStep.analysis, &schema, &messages);
                
                saveTrainingData(datasetDir, "step_" + startStepStr + "_" + currentStepStr, jsonSample, true);
                
                if(NextDebugStep::isInformationRequest(step->action_type))
                {
                    addStepToMessages(project, nextStep, newInfo, messages);
                }
            }
            
            currentStep++;
        }
#endif
        
        project->popContext();
    }

    void Distillery::printTrajectoryInfo()
    {
        int stepIdx = m_fromStep;
        for(auto step : m_trajectory)
        {
            std::cout << "\nSTEP: " << stepIdx + 1 << std::endl << std::endl;
            std::cout << step.fullInfo() << std::endl << std::endl;
            
            if(step.m_action == "fix_function")
            {
                auto it = m_newFunctionsPerStep.find(stepIdx);
                if(it != m_newFunctionsPerStep.end() && it->second.size() > 0)
                {
                    std::cout << "New functions added this step:" << std::endl;
                    for(auto func : it->second)
                    {
                        std::cout << func << " ";
                    }
                    
                    std::cout << std::endl;
                }
            }
            
            stepIdx++;
        }
    }

    void Distillery::printFixesAndTests()
    {
        int stepIdx = m_fromStep;
        
        for(auto step : m_trajectory)
        {
            if(step.m_action == "fix_function" || step.m_action == "run_test")
            {
                std::cout << "\nSTEP: " << stepIdx + 1 << std::endl << std::endl;
                std::cout << step.concise() << std::endl << std::endl;
            }
            
            stepIdx++;
        }
    }

    void Distillery::printMergedFixesAndTests()
    {
        //I want this function refactored to
        //std::string Distillery::mergeFixes()
        //The mergeFixes doesn't print the info to std::cout but returns a string with that info
        
        //I also want to keep track of the merged fixes.
        //The separate fixes the pair entries are both the 0-based step indices
        //std::vector<std::pair<int,int>> m_mergedFixes;
        //I plan to traverse merged fixes like this: for(int step = fix.first; step < fix.second; ++step) {}
        
        using std::size_t;
        const size_t N = m_trajectory.size();

        auto stepNo = [&](size_t idx) -> int {
            return static_cast<int>(m_fromStep + static_cast<int>(idx) + 1);
        };

        size_t i = 0;
        while (i < N) {
            const auto& cur = m_trajectory[i];

            // We only print fix/test; other actions are skipped here (but do not break blocks).
            if (cur.m_action != "fix_function" && cur.m_action != "run_test") {
                ++i;
                continue;
            }

            // Standalone tests (not inside a block start):
            if (cur.m_action == "run_test") {
                std::cout << "\nSTEP: " << stepNo(i) << "\n\n";
                std::cout << cur.concise() << "\n\n";
                ++i;
                continue;
            }

            // ---------- Start a contiguous fix block for this subject ----------
            const std::string subject = cur.m_subject;
            size_t runStart = i;
            size_t anchor   = i;
            size_t j        = i + 1;

            // Scan forward: only break on fix_function of a different subject.
            // Keep going for run_test and ANY other action types.
            while (j < N) {
                const auto& e = m_trajectory[j];

                if (e.m_action == "fix_function") {
                    if (e.m_subject == subject) {
                        anchor = j;       // extend block to this later fix
                        ++j;
                        continue;
                    }
                    // Different function's fix => end of this block
                    break;
                }

                // run_test or any other action: do NOT break the block; just advance
                ++j;
            }

            // We now have a block [runStart .. j-1], anchored at 'anchor'.
            // 1) Print the anchor header.
            std::cout << "\nSTEP: " << stepNo(anchor) << "\n\n";

            // 2) Merge earlier fixes for this subject and all tests BETWEEN first and last fix.
            for (size_t k = runStart; k < anchor; ++k) {
                const auto& t = m_trajectory[k];

                if (t.m_action == "fix_function" && t.m_subject == subject) {
                    std::cout << "NOTE: Merge step " << stepNo(k)
                              << " to step " << stepNo(anchor)
                              << " as it fixes the same function '" << subject << "'\n\n";
                    std::cout << t.concise() << "\n\n";
                } else if (t.m_action == "run_test") {
                    std::cout << "NOTE: Merge step " << stepNo(k)
                              << " to step " << stepNo(anchor)
                              << " as it tests the same function '" << subject << "'\n\n";
                    std::cout << t.concise() << "\n\n";
                }
                // Other actions inside the block are ignored entirely (by design).
            }

            // 3) Anchor’s own concise() (last fix in the block).
            //std::cout << "NOTE: Anchor’s own step " << stepNo(i) << "\n\n";
            std::cout << m_trajectory[anchor].concise() << "\n\n";

            // 4) Post‑anchor tests (after last fix but before next different-function fix) as standalone steps.
            for (size_t k = anchor + 1; k < j; ++k) {
                const auto& t = m_trajectory[k];
                if (t.m_action == "run_test") {
                    std::cout << "\nSTEP: " << stepNo(k) << "\n\n";
                    std::cout << t.concise() << "\n\n";
                }
            }

            // Continue scanning from the first index after this block.
            i = j;
        }
    }

    std::string Distillery::mergeFixes()
    {
        using std::size_t;

        // Reset the bookkeeping for this run
        m_mergedFixes.clear();

        const size_t N = m_trajectory.size();
        std::ostringstream out;

        auto stepNo = [&](size_t idx) -> int {
            // Human-friendly numbering with external offset.
            return static_cast<int>(m_fromStep + static_cast<int>(idx) + 1);
        };

        size_t i = 0;
        while (i < N) {
            const auto& cur = m_trajectory[i];

            // We only output fix/test; other actions are skipped (but do not break blocks).
            if (cur.m_action != "fix_function" && cur.m_action != "run_test") {
                ++i;
                continue;
            }

            // Standalone tests (not inside a fix block start)
            if (cur.m_action == "run_test") {
                out << "\nSTEP: " << stepNo(i) << "\n\n";
                out << cur.fullInfo() << "\n\n";
                ++i;
                continue;
            }

            // ---------- Start a contiguous fix block for this subject ----------
            const std::string subject = cur.m_subject;
            const size_t runStart = i;   // first fix for this block
            size_t anchor   = i;         // will become last fix for this block
            size_t j        = i + 1;

            // Scan forward: advance anchor on same-subject fixes; stop on different-subject fix.
            // Tests or any other actions do NOT break the block.
            while (j < N) {
                const auto& e = m_trajectory[j];

                if (e.m_action == "fix_function") {
                    if (e.m_subject == subject) {
                        anchor = j;       // extend block to this later fix
                        ++j;
                        continue;
                    }
                    // Different function's fix => end of this block
                    break;
                }

                // run_test or any other action: do NOT break the block; just advance
                ++j;
            }

            // We now have a block [runStart .. j-1], anchored at 'anchor'.
            // 1) Anchor header.
            out << "\nSTEP: " << stepNo(anchor) << "\n\n";

            // 2) Merge earlier fixes for this subject and all tests BETWEEN first and last fix.
            for (size_t k = runStart; k < anchor; ++k) {
                const auto& t = m_trajectory[k];

                if (t.m_action == "fix_function" && t.m_subject == subject) {
                    out << "NOTE: Merge step " << stepNo(k)
                        << " into step " << stepNo(anchor)
                        << " as it fixes the same function '" << subject << "'\n\n";
                    out << t.fullInfo() << "\n\n";
                } else if (t.m_action == "run_test") {
                    out << "NOTE: Merge step " << stepNo(k)
                        << " into step " << stepNo(anchor)
                        << " as it tests the same function '" << subject << "'\n\n";
                    out << t.fullInfo() << "\n\n";
                }
                // Other actions inside the block are ignored entirely (by design).
            }

            // 3) Anchor’s own fullInfo() (last fix in the block).
            if (runStart < anchor) {
                out << "NOTE: Anchor’s own step " << stepNo(anchor) << "\n\n";
            }
            out << m_trajectory[anchor].fullInfo() << "\n\n";

            // 4) Post‑anchor tests (after last fix but before next different-function fix) as standalone steps.
            for (size_t k = anchor + 1; k < j; ++k) {
                const auto& t = m_trajectory[k];
                if (t.m_action == "run_test") {
                    out << "\nSTEP: " << stepNo(k) << "\n\n";
                    out << t.fullInfo() << "\n\n";
                }
            }

            // 5) Record exactly one entry per block in m_mergedFixes:
            //    - merged block: (firstFixIdx, anchorIdx)
            //    - singleton block: (anchorIdx, anchorIdx)
            if (runStart < anchor) {
                m_mergedFixes.emplace_back(static_cast<int>(runStart), static_cast<int>(anchor));
            } else {
                m_mergedFixes.emplace_back(static_cast<int>(anchor), static_cast<int>(anchor));
            }

            // Continue scanning from the first index after this block.
            i = j;
        }

        return out.str();
    }

    std::string Distillery::printFixInfo(int step)
    {
        int stepIndex = (step-1) - m_fromStep;
        std::ostringstream out;
        
        if(stepIndex >= m_trajectory.size())
        {
            //this is not cool
            out << "Invalid step " << step << " Maximum steps in the trajectory " << m_fromStep + (int)m_trajectory.size() << "\n\n";
            return out.str();
        }
        
        int startFix = stepIndex;
        int endFix = stepIndex;
        
        for(auto fix : m_mergedFixes)
        {
            if(fix.first <= stepIndex && stepIndex <= fix.second)
            {
                startFix = fix.first;
                endFix = fix.second;
                break;
            }
        }
        
        int testIndex=startFix;
        for(; testIndex >= 0; --testIndex)
        {
            if(m_trajectory[testIndex].m_action == "run_test")
            {
                break;
            }
        }
        
        for(int i=testIndex; i <= endFix; i++)
        {
            out << "\nSTEP: " << (m_fromStep+i+1) << "\n\n";
            out << m_trajectory[i].fullInfo() << "\n\n";
        }
        
        return out.str();
    }

}
