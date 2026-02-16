#include "Distillery.h"
#include "Client.h"

namespace stdrave {

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
        
        boost_fs::remove(datasetDir + "/train_dbg_sft.jsonl");

        
        for (boost_fs::directory_iterator it(root), end; it != end; ++it)
        {
            if (!boost_fs::is_regular_file(*it))
                continue;

            const boost_fs::path& p = it->path();

            if (p.extension() == ".json" && startsWith(p.filename().string(), "step_"))
            {
                std::string jsonSample = getFileContent(p.string());
                
                std::ofstream trainFile(datasetDir + "/train_dbg_sft.jsonl", std::ios::app);
                if(trainFile.good())
                {
                    trainFile << jsonSample << std::endl;
                }
            }
        }
        
        boost_fs::remove(datasetDir + "/train_run_sft.jsonl");

        for (boost_fs::directory_iterator it(root), end; it != end; ++it)
        {
            if (!boost_fs::is_regular_file(*it))
                continue;

            const boost_fs::path& p = it->path();

            if (p.extension() == ".json" &&
                (startsWith(p.filename().string(), "system_") || startsWith(p.filename().string(), "debug_")))
            {
                std::string jsonSample = getFileContent(p.string());
                
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
        m_system = utility::conversions::to_utf8string(json[U("function")].as_string());
        
        m_test.from_json(json);
        
        loadTrajectory(project, m_test, fromStep, toStep);
        
        return (uint32_t)m_trajectory.size();
    }

    void Distillery::distillTrajectory(CCodeProject* project, const std::string& testJsonPath, int fromStep, int toStep)
    {
        loadTrajectory(project, testJsonPath, fromStep, toStep);
        
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
            
            web::json::value object;
            project->inference(cache, promptDistillStep, schema, object);
            
            project->popContext();
            
            distilledSystemAnalysis.from_json(object);
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
        
        project->popContext();
        
        DistilledAanalysis distilledDebugAnalysis;
        distilledDebugAnalysis.from_json(object);
        
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
        prologue += m_debugContext.getHighLevelAppInfo(m_project, {}, 0, PRINT_MAX_FUNCTIONS_DEPTH-1);
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

    std::pair<std::string, std::string> Distillery::validateSequence(CCodeProject* project, const EditSourceSequence& optimalSequence, int originalSize, int startStep)
    {
        std::string feedback;
        std::string recommendation;
        
        if(optimalSequence.steps.size() < 2)
        {
            feedback += "The optimal sequence must have atleast two steps run_test and fix_function.\n\n";
            return std::pair<std::string, std::string>(); //We should not continue
        }
        
        if(optimalSequence.steps.size() > originalSize)
        {
            std::string originalSizeStr = std::to_string(originalSize);
            std::string optimalSizeStr = std::to_string(optimalSequence.steps.size());
            feedback += "The optimal sequence has more steps (" + optimalSizeStr + ") than the original (";
            feedback += optimalSizeStr + ").\n\n";
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
        
        int stepIndex = 1;
        for(const auto& step : optimalSequence.steps)
        {
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

    void Distillery::optimizeFixTrack(CCodeProject* project, Cache& cache, const std::string& trajectoryAnalysis, const std::string& fixTrack, uint32_t fixStep, EditSourceSequence& optimalSequence)
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
        
        goTo(project, fixStep-1);
        std::string appInfo = m_debugContext.getHighLevelAppInfo(m_project, "", PRINT_MAX_FUNCTIONS_DEPTH, PRINT_MAX_FUNCTIONS_DEPTH);
        
        Prompt promptOptimizeFixTrack("OptimizeFixTrack.txt",{
                            {"app_info", appInfo},
                            {"trajectory", trajecotry},
                            {"fix_track", fixTrack}
        });
        
        web::json::value object;
        web::json::value schema;
        setupSchema<EditSourceSequence>(schema);
        
        std::string message = promptOptimizeFixTrack.str();
        project->inference(cache, message, schema, object);
        
        optimalSequence.from_json(object);
        
        auto feedback = validateSequence(project, optimalSequence, originalSize, runStep);
        bool firstFb = true;
        while(!feedback.first.empty() ||
              (firstFb && !feedback.second.empty()))
        {
            std::string feedbackPrompt = feedback.first + feedback.second;
            project->inference(cache, feedbackPrompt, schema, object);
        
            optimalSequence.clear();
            optimalSequence.from_json(object);
            feedback = validateSequence(project, optimalSequence, originalSize, runStep);
            
            firstFb = false;
        }
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
                                        DistilledStep& nextStep)
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
        
        std::string newFunctionsHint;
        if(nextStep.debug_step.action_type == "fix_function")
        {
            CCodeNode* ccFixed = project->getNodeByName(nextStep.debug_step.action_subject);
            
            auto it = m_newFunctionsPerStep.find(nextStep.m_originalStep);
            if(it != m_newFunctionsPerStep.end() && it->second.size() > 0)
            {
                for(auto func : it->second)
                {
                    CCodeNode* ccNode = project->getNodeByName(func);
                    if(!ccNode) continue;

                    if(ccFixed && !ccFixed->calledInTheSource(func)) continue;

                    if(ccNode->hasPathToMain())
                    {
                        bool addSource = false;
                        if(newFunctionsHint.empty())
                        {
                            newFunctionsHint += "\n\nThe original fix_function step you're distilling from maybe introduced the following helper functions that were used in the source.";
                            newFunctionsHint += " Consider mentioning their addition in the motivation and analysis:\n";
                            addSource = true;
                        }
                        
                        newFunctionsHint += "//" + ccNode->m_prototype.brief;
                        newFunctionsHint += "\n";
                        newFunctionsHint += ccNode->m_prototype.declaration;
                        newFunctionsHint += "\n\n";
                    }
                }
            }
        }
        
        Prompt promptDistillStep("DistillStep.txt",{
                            {"current_trajectory", currentTrajectory},
                            {"new_functions", newFunctionsHint},
                            {"step_id", stepIdStr}
        });
        
        project->inference(cache, promptDistillStep, schema, object);
        
        nextStep.debug_step.clear();
        nextStep.from_json(object);
        
        DebugStep debugStep;
        
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
        
        std::string feedback;
        do
        {
            feedback.clear();
            
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
            
            if(!feedback.empty())
            {
                project->inference(cache, feedback, schema, object);
                nextStep.debug_step.clear();
                nextStep.from_json(object);
            }
            
        } while(!feedback.empty());
        
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
        bool needsOptimization = fixRange.second - fixRange.first > 2;
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
        if(needsOptimization)
        {
            Cache cache(datasetDir, optimizedJson);
            optimizeFixTrack(project, cache, trajectoryAnalysis, fixTrack, fixStep, optimalSequence);
        }
        else
        {
            for(int s = fixRange.first; s<fixRange.second; ++s)
            {
                NextDebugStep step;
                int stepId = trajectoryIndexToStep(s)-1;
                std::string stepDir = project->getProjDir() + "/debug/" + m_test.name + "/trajectory/step_" + std::to_string(stepId);
                web::json::value stepJson;
                loadJson(stepJson, stepDir + "/nextStep.json");
                step.from_json(stepJson);
                
                OptimizedStep optimizedStep;
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
        
        //Save optimized sequence
        saveJson(optimalSequence.to_json(), datasetDir + "/" + optimizedJson);
        
        std::string summary = distillSummaryBefore(project, startStep, fixStep);
        
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
        
        std::string systemAnalysis;
        
        std::vector<DistilledStep> distilledTrajectory;
        
        std::string startStepStr = std::to_string(startStep);
        int currentStep = startStep;
        web::json::value messages = web::json::value::array();
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
                        
                        auto contentU = rewardHackingResponse[U("message")][U("content")].as_string();
                        std::string rewardHackingResponseStr = utility::conversions::to_utf8string(contentU);
                        saveResponseForTraining(datasetDir,
                                                "system_" + testStepStr + "_" + fixStepStr,
                                                rewardHackingRequestStr,
                                                rewardHacking.second,
                                                rewardHackingResponseStr, false);
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
                nextStep.debug_step.invocation = step->invocation;
                nextStep.debug_step.line_number = step->line_number;
                nextStep.m_originalStep = step->original_step;
                
                std::string currentTrajectory = distillStep(project, step->original_step, startStep, fixStep, currentStep, summary, prevSteps, requestedInfo, newInfo, nextStep);
                
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
                nextStep.debug_step.invocation = step->invocation;
                nextStep.debug_step.line_number = step->line_number;
                nextStep.m_originalStep = step->original_step;
                
                std::string newInfo;
                std::string currentTrajectory = distillStep(project, step->original_step, startStep, fixStep, currentStep, summary, prevSteps, requestedInfo, newInfo, nextStep);
                
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
