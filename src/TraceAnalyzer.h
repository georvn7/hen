#pragma once

#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <map>
#include <vector>
#include <utility>

#define FULL_TRACE_MAX_EVENTS_HIT_COUNT 1

class TraceAnalyzer
{
public:
    struct Section {
        std::string              m_invocation;
        std::string              m_type;
        std::vector<std::string> m_lines;
    };

    struct Frame {
        //function/hitCount
        std::pair<std::string, int32_t>         m_invocation;
        std::string                             m_stack;
        int                                     m_depth;
        std::vector<std::shared_ptr<Section>>   m_sections;
    };
private:
    
    std::vector<std::shared_ptr<Section>>         m_sectionEvents;
    
    std::string                                   m_loadingIssus;
    std::vector<std::shared_ptr<Frame>>           m_frames;
    
    //The key would be the name of the function and its instance call: func_name_1
    std::map<std::string, std::shared_ptr<Frame>> m_functionToFrameMap;
    
    //Tracks the execution stack
    std::stack<std::shared_ptr<Frame>>            m_framesStack;
    
    static std::pair<std::string, int32_t> makeFramePair(const std::string& stack) {
        auto gt = stack.find_last_of('>');
        std::string tail = (gt == std::string::npos) ? stack : stack.substr(gt + 1);
        auto colon = tail.find_last_of(':');
        return (colon == std::string::npos)
                 ? std::make_pair(tail, uint32_t(1))
                 : std::make_pair(tail.substr(0, colon), uint32_t(std::atoi(tail.substr(colon + 1).c_str())));
    }
    
    static std::string makeFrameKey(const std::string& stack) {
        
        auto framePair = makeFramePair(stack);
        return framePair.first + '_' + std::to_string(framePair.second);
    }
    
    int getDepth(const std::string& stack) {
        return static_cast<int>(std::count(stack.begin(), stack.end(), ':'));
    }

public:
    void loadFromFile(const std::string& filePath)
    {
        std::ifstream in(filePath);
        if (!in)
        {
            std::cout << "Unable to open trace file: " << filePath << std::endl;
        }
        
        loadTrace(in);
    }
    
    void loadFromString(const std::string& trace)
    {
        std::stringstream in(trace);
        loadTrace(in);
    }
    
    void loadTrace(std::istream& in)
    {
        static const std::regex pushRe(
            R"(<\[\s*PUSH\s*\(\s*([^)]+?)\s*\):\s*([^\]]+?)\s*\]>)"); // 1:type 2:stack
        static const std::regex popRe(R"(<\[\s*POP\s*\]>)");
        
        clear();

        std::stack<std::shared_ptr<Section>>                sectionStack;
        std::stack<std::shared_ptr<Frame>> frameStack;

        std::string line;
        std::smatch m;
        while (std::getline(in, line))
        {
            if (std::regex_match(line, m, pushRe))
            {
                const std::string type  = m[1];
                const std::string stack = m[2];

                const auto invocation = makeFramePair(stack);
                const std::string key = invocation.first + ':' + std::to_string(invocation.second);
                auto it = m_functionToFrameMap.find(key);
                std::shared_ptr<Frame> frame;

                if (it == m_functionToFrameMap.end()) {
                    frame = std::make_shared<Frame>();
                    frame->m_invocation = invocation;
                    frame->m_stack    = stack;
                    frame->m_depth    = getDepth(stack);
                    m_frames.push_back(frame);
                    m_functionToFrameMap.emplace(key, frame);
                } else {
                    frame = it->second;
                }

                auto sec = std::make_shared<Section>();
                sec->m_type = type;
                sectionStack.push(sec);
                frameStack.push(frame);
            }
            else if (std::regex_match(line, popRe))
            {
                auto frame = frameStack.top();
                frameStack.pop();
                
                if (sectionStack.empty())
                {
                    m_loadingIssus += "Unmatched 'section end' marker in log. This requires further investigation";
                }

                //Section sec = std::move(sectionStack.top());
                auto sec = sectionStack.top();
                sectionStack.pop();
                
                //Update the execution frames stack based on section type
                if (sec->m_type == "arguments" || sec->m_type == "entry")
                {
                    // Function entry - push the frame onto execution stack
                    if(m_framesStack.empty() || m_framesStack.top()->m_stack != frame->m_stack)
                    {
                        m_framesStack.push(frame);
                    }
                }
                else
                {
                    if ((sec->m_type == "exit" || sec->m_type == "return") && frame->m_invocation.first != "main")
                    {
                        //Pop the frames on exit or return
                        //but let the exit/return for the 'main' to stay on top of the stack
                        //for further trace analysis
                        
                        // Function exit - pop from execution stack
                        if (!m_framesStack.empty())
                        {
                            m_framesStack.pop();
                        }
                        else
                        {
                            m_loadingIssus += "Attempting to pop from empty execution stack. ";
                        }
                    }
                }
                
                frame->m_sections.push_back(sec);
                
                //Also add the section in the chronological events list
                auto invocation = makeFramePair(frame->m_stack);
                const std::string key = invocation.first + ':' + std::to_string(invocation.second);
                sec->m_invocation = key;
                m_sectionEvents.push_back(sec);
            }
            else
            {
                if (!sectionStack.empty())
                {
                    sectionStack.top()->m_lines.push_back(line);
                }
            }
        }
        if (!sectionStack.empty())
        {
            m_loadingIssus += "End of file reached with unterminated section. ";
            auto lastFrame = getLastFrame(false);
            if(lastFrame)
            {
                m_loadingIssus += "Often this indicates crash or hang during the captured frame. ";
                m_loadingIssus += "Maybe it is possible to understand that by looking at the content ";
                m_loadingIssus += "of the last detailed frame in the trace:\n\n";
                std::stringstream ssFrame;
                printFrame(ssFrame, lastFrame);
                m_loadingIssus += ssFrame.str();
            }
        }
    }
    
    static std::vector<std::pair<std::string, uint32_t>> parseStack(const std::string& stack)
    {
        std::vector<std::pair<std::string, uint32_t>> result;
        
        size_t start = 0;
        size_t end = 0;
        
        // Process each function entry separated by "->"
        while ((end = stack.find("->", start)) != std::string::npos) {
            std::string entry = stack.substr(start, end - start);
            
            // Split function name and invocation count by ":"
            size_t colonPos = entry.find(':');
            if (colonPos != std::string::npos) {
                std::string functionName = entry.substr(0, colonPos);
                std::string invocationStr = entry.substr(colonPos + 1);
                uint32_t invocation = (uint32_t)std::stoul(invocationStr);
                
                result.emplace_back(functionName, invocation);
            }
            
            start = end + 2; // Skip past "->"
        }
        
        // Handle the last entry (no "->" after it)
        if (start < stack.length()) {
            std::string entry = stack.substr(start);
            
            size_t colonPos = entry.find(':');
            if (colonPos != std::string::npos) {
                std::string functionName = entry.substr(0, colonPos);
                std::string invocationStr = entry.substr(colonPos + 1);
                uint32_t invocation = (uint32_t)std::stoul(invocationStr);
                
                result.emplace_back(functionName, invocation);
            }
        }
        
        return result;
    }
    
    std::vector<std::pair<std::string, int32_t>> loadStack(const std::string& stackString)
    {
        std::vector<std::pair<std::string, int32_t>> stackFrames;
        
        //It is not required to clear the tracer
        //TODO: What do we do with frames that already exist in the full trace
        
        if (stackString.empty())
            return stackFrames;
        
        // 2) Split on "->" to get each "function:invocation" element
        std::vector<std::string> elements;
        {
            size_t start = 0;
            while (start < stackString.size()) {
                size_t delim = stackString.find("->", start);
                if (delim == std::string::npos)
                    delim = stackString.size();
                elements.push_back(stackString.substr(start, delim - start));
                start = delim + 2; // skip past "->"
            }
        }
        
        // 3) Build each Frame by accumulating a "cumulative" stack string with '>' separators
        std::string cumulative;
        for (size_t i = 0; i < elements.size(); ++i) {
            
            const std::string& elem = elements[i];
            // elem should be in the form "functionName:hitCount"
            auto colonPos = elem.find(':');
            if (colonPos == std::string::npos) {
                // malformed element—skip it
                continue;
            }
            
            // 4a) Parse function name and invocation count
            std::string funcName = elem.substr(0, colonPos);
            int32_t    hitCount = static_cast<int32_t>(std::stoul(elem.substr(colonPos + 1)));
            
            // 4b) Build the "cumulative" stack string in the same format
            //     that loadTrace expects (using '>' between frames):
            if (i == 0) {
                cumulative = elem;                 // first frame is just "func:count"
            } else {
                cumulative += "->" + elem;          // append ">func:count"
            }
            
            //Check if the frame for this invocation alreayd exists
            //Lookup into m_functionToFrameMap
            
            auto keyPair = std::make_pair(funcName, hitCount);
            std::string key = funcName + ":" + std::to_string(hitCount);
            
            auto itFrame = m_functionToFrameMap.find(key);
            if(itFrame == m_functionToFrameMap.end())
            {
                // 4c) Create a new Frame and populate its fields
                auto frame = std::make_shared<Frame>();
                frame->m_invocation = keyPair;
                frame->m_stack      = cumulative;
                frame->m_depth      = getDepth(cumulative);
                
                // 5a) Add to m_frames
                m_frames.push_back(frame);
                
                // 5b) Insert into the lookup map under "function:invocation"
                m_functionToFrameMap.emplace(key, frame);
            }
            
            stackFrames.push_back(keyPair);
        }
        
        // Note: We do not push anything onto m_framesStack or m_sectionEvents here,
        // because loadStack is only meant to populate m_frames (and m_functionToFrameMap).
        // 1) Clear any previous trace‐state
        
        return stackFrames;
    }
    
    //https://chatgpt.com/c/684115db-cf8c-8013-953b-b007c7e0a51d
    bool loadStackFrame(const std::string& fullFrame,
                        const std::string& function,
                        int invocation)
    {
        // 1) Lookup (or create) the Frame object for "function:invocation"
        std::string frameKey = function + ":" + std::to_string(invocation);
        std::shared_ptr<Frame> frame;
        auto itF = m_functionToFrameMap.find(frameKey);

        if (itF == m_functionToFrameMap.end()) {
            // If not found, make a new placeholder Frame
            frame = std::make_shared<Frame>();
            frame->m_invocation = std::make_pair(function, static_cast<uint32_t>(invocation));
            frame->m_stack      = "";  // will set on first PUSH
            frame->m_depth      = 0;   // will set then
            m_frames.push_back(frame);
            m_functionToFrameMap.emplace(frameKey, frame);
        } else {
            frame = itF->second;
        }

        // 2) We will rebuild frame->m_sections by scanning fullFrame in order:
        frame->m_sections.clear();

        // 3) Prepare to parse lines. We will keep a local stack of Section pointers.
        static const std::regex pushRe(
            R"(<\[\s*PUSH\s*\(\s*([^)]+?)\s*\)\s*:\s*([^\]]+?)\s*\]>)");
        static const std::regex popRe(R"(<\[\s*POP\s*\]>)");

        std::stack<std::shared_ptr<Section>> sectionStack;
        std::istringstream                    in(fullFrame);
        std::string                           line;
        std::smatch                           m;

        bool firstPushSeen = false;
        while (std::getline(in, line)) {
            if (std::regex_match(line, m, pushRe)) {
                // A PUSH line matched:
                //   m[1].str() is the "type"  (e.g. "arguments", "entry", "breakpoint", etc.)
                //   m[2].str() is the full stack (e.g. "main:1->foo:4->bar:2")
                std::string typeStr  = m[1].str();
                std::string stackStr = m[2].str();

                // Extract the last "function:hitCount" from stackStr
                std::pair<std::string, int32_t> framePair = makeFramePair(stackStr);
                std::string                     fname     = framePair.first;
                int32_t                         hitCount  = framePair.second;
                std::string                     thisKey   = fname + ":" + std::to_string(hitCount);

                // On the very first PUSH, fix frame->m_stack + frame->m_depth
                if (!firstPushSeen) {
                    firstPushSeen = true;
                    if (fname != function || static_cast<int>(hitCount) != invocation) {
                        m_loadingIssus +=
                            "Warning: loadStackFrame saw a PUSH for \"" + thisKey +
                            "\" but expected \"" + frameKey + "\".\n";
                    }
                    frame->m_stack = stackStr;
                    frame->m_depth = getDepth(stackStr);
                }

                // Determine whether an existing Section object already lives in m_sectionEvents.
                // If hitCount ≤ cap, we expect it to be in m_sectionEvents. Otherwise, we make a new one.
                // (Assume FULL_TRACE_MAX_EVENTS_HIT_COUNT is the “cap”.)
                std::shared_ptr<Section> secPtr;

                // (A) Instead of “if (hitCount <= FULL_TRACE_MAX_EVENTS_HIT_COUNT) { … }”
                //     do a direct existence check over m_sectionEvents:
                for (std::size_t i = 0; i < m_sectionEvents.size(); ++i) {
                    auto &candidate = m_sectionEvents[i];
                    if (candidate->m_invocation == thisKey
                        && candidate->m_type       == typeStr)
                    {
                        secPtr = candidate;
                        break;
                    }
                }
                
                if (!secPtr) {
                    // Either invocation > cap, or no existing Section matched → create new one
                    secPtr = std::make_shared<Section>();
                    secPtr->m_type       = typeStr;
                    secPtr->m_invocation = thisKey;
                }
                
                //Clear the old content for this section.
                //If the section was discovered in m_sectionEvents we expect this frame to override everyting
                
                secPtr->m_lines.clear();
                
                // Push onto our local stack
                sectionStack.push(secPtr);
            }
            else if (std::regex_match(line, popRe)) {
                // A POP line: close the top Section
                if (sectionStack.empty()) {
                    m_loadingIssus +=
                        "Unmatched POP in loadStackFrame(\"" + frameKey + "\").\n";
                    continue;
                }
                auto secPtr = sectionStack.top();
                sectionStack.pop();

                // Attach to this Frame’s section list (in the exact pop‐order)
                frame->m_sections.push_back(secPtr);
            }
            else {
                //TODO: What happens if this is not a new section but one discovered in m_sectionEvents
                //Are we going to duplicate content?!?
                
                // Normal text line → add to current Section’s lines
                if (!sectionStack.empty()) {
                    sectionStack.top()->m_lines.push_back(line);
                }
            }
        }

        // If any PUSH didn’t see a matching POP, warn
        if (!sectionStack.empty()) {
            m_loadingIssus +=
                "End of fullFrame text reached with " +
                std::to_string(sectionStack.size()) +
                " unterminated PUSH(s) for \"" + frameKey + "\".\n";
        }
        
        //Do we have at lease one frame?
        return firstPushSeen;
    }
    
    std::string getStackFromFile(const std::string& stackFile)
    {
        auto line = stdrave::getFirstLine(stackFile);
        if(!line)
        {
            m_loadingIssus += "Unable to read stack string from file: " + stackFile + "\n";
            return std::string();
        }
        
        return *line;
    }
    
    std::vector<std::pair<std::string, int32_t>> loadStackTrace(const std::string& stackFile, const std::string& stackDir)
    {
        std::vector<std::pair<std::string, int32_t>> stackFrames;
        
        std::string stackString = getStackFromFile(stackFile);
        
        if (stackString.empty()) {
            //m_loadingIssus += "Empty stack string in memo file: " + stackFile + "\n";
            return stackFrames;
        }
        
        // 2. Load the stack to get function/invocation pairs and create frame structures
        stackFrames = loadStack(stackString);
        
        bool hasFrames = false;
        // 3. For each function in the stack, load its detailed frame content
        for (const auto& framePair : stackFrames) {
            const std::string& function = framePair.first;
            uint32_t invocation = framePair.second;
            
            // Construct the frame file path: {stackDir}/{function}.{invocation}.txt
            std::string frameFilePath = stackDir;
            if (!frameFilePath.empty() && frameFilePath.back() != '/' && frameFilePath.back() != '\\') {
                frameFilePath += "/";
            }
            frameFilePath += function + "." + std::to_string(invocation) + ".txt";
            
            // Read the frame content
            std::ifstream frameFile(frameFilePath);
            if (!frameFile) {
                // Don't treat this as a critical error, just warn
                m_loadingIssus += "Warning: Unable to open frame file: " + frameFilePath + "\n";
                continue;
            }
            
            // Read the entire file content
            std::string frameContent;
            std::string line;
            while (std::getline(frameFile, line)) {
                if (!frameContent.empty()) {
                    frameContent += "\n";
                }
                frameContent += line;
            }
            frameFile.close();
            
            if (frameContent.empty()) {
                m_loadingIssus += "Warning: Empty frame file: " + frameFilePath + "\n";
                continue;
            }
            
            // Load the frame using loadStackFrame
            bool frameLoaded = loadStackFrame(frameContent, function, static_cast<int>(invocation));
            hasFrames = hasFrames || frameLoaded;
        }
        
        return stackFrames;
    }
    
    void loadBreakpointTraces(const std::string& breakpointsDir, const std::string& functionName)
    {
        boost::system::error_code ec;
        
        if (!boost_fs::exists(breakpointsDir, ec) || ec) {
            m_loadingIssus += "Error checking breakpoints directory: " + breakpointsDir + " - " + ec.message() + "\n";
            return;
        }
        
        if (!boost_fs::is_directory(breakpointsDir, ec) || ec) {
            m_loadingIssus += "Breakpoints path is not a directory: " + breakpointsDir + "\n";
            return;
        }
        
        // Iterate through all subdirectories in breakpointsDir
        boost_fs::directory_iterator it(breakpointsDir, ec);
        if (ec) {
            m_loadingIssus += "Error opening breakpoints directory: " + breakpointsDir + " - " + ec.message() + "\n";
            return;
        }
        
        for (; it != boost_fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                m_loadingIssus += "Error iterating through breakpoints directory: " + ec.message() + "\n";
                break;
            }
            
            if (boost_fs::is_directory(it->status(ec)) && !ec) {
                std::string breakpointDir = it->path().string();
                
                // Look for stack file and frames directory in this breakpoint directory
                std::string stackFile;
                std::string framesDir;
                
                // Search for stack file - try common names
                //std::vector<std::string> stackFileNames = {"stack.txt", "memo.txt", "callstack.txt", "trace.txt"};
                std::vector<std::string> stackFileNames = {"stack.txt", "memo.txt"};
                for (const auto& name : stackFileNames) {
                    boost_fs::path candidate = boost_fs::path(breakpointDir) / name;
                    if (boost_fs::exists(candidate, ec) && !ec && boost_fs::is_regular_file(candidate, ec) && !ec) {
                        stackFile = candidate.string();
                        break;
                    }
                    ec.clear(); // Clear any error from the check
                }
                
                // Search for frames directory - try common names
                std::vector<std::string> framesDirNames = {"frames", "stack_frames", "traces", "data"};
                for (const auto& name : framesDirNames) {
                    boost_fs::path candidate = boost_fs::path(breakpointDir) / name;
                    if (boost_fs::exists(candidate, ec) && !ec && boost_fs::is_directory(candidate, ec) && !ec) {
                        framesDir = candidate.string();
                        break;
                    }
                    ec.clear(); // Clear any error from the check
                }
                
                if (stackFile.empty()) {
                    m_loadingIssus += "Warning: No stack file found in breakpoint directory: " + breakpointDir + "\n";
                    continue;
                }
                
                if (framesDir.empty()) {
                    m_loadingIssus += "Warning: No frames directory found in breakpoint directory: " + breakpointDir + "\n";
                    continue;
                }
                
                // If functionName is specified, check if the stack contains that function
                bool shouldLoad = true;
                if (!functionName.empty()) {
                    std::ifstream file(stackFile);
                    if (file) {
                        std::string stackString;
                        if (std::getline(file, stackString)) {
                            // Check if the function appears in the stack
                            if (stackString.find(functionName + ":") == std::string::npos) {
                                shouldLoad = false;
                            }
                        } else {
                            m_loadingIssus += "Warning: Could not read stack from file: " + stackFile + "\n";
                            shouldLoad = false;
                        }
                        file.close();
                    } else {
                        m_loadingIssus += "Warning: Could not open stack file: " + stackFile + "\n";
                        shouldLoad = false;
                    }
                }
                
                if (shouldLoad) {
                    // Load this breakpoint using loadMemo
                    loadStackTrace(stackFile, framesDir);
                }
            }
        }
    }
    
    void print(std::ostream& os, bool consise, int maxDepth, bool withIssus, const std::string& function) const
    {
        for(const auto s : m_sectionEvents)
        {
            auto f = m_functionToFrameMap.find(s->m_invocation)->second;
            
            bool conciseFrame = consise;
            
            //DEEP_RECURSION:-1
            if(f->m_invocation.first == "DEEP_RECURSION" && f->m_invocation.second == -1)
            {
                os << "\n[[DEEP RECURSION DETECTED]]\n";
                return;
            }
            
            if(conciseFrame && f->m_invocation.second > FULL_TRACE_MAX_EVENTS_HIT_COUNT)
            {
                continue;
            }
            
            if(maxDepth > 0 && f->m_depth >= maxDepth)
            {
                continue;
            }
            
            bool showSection = (!conciseFrame && s->m_type != "arguments") ||
                               (conciseFrame && (s->m_type == "arguments" ||
                                                 stdrave::startsWith(s->m_type, "breakpoint")));
            if(showSection)
            {
                os << "\ncall stack: " << f->m_stack << "\n";
                os << "[[" << s->m_type << " start]]\n";
                
                for (const auto& l : s->m_lines)
                    os << "  " << l << '\n';
                
                os << "[[" << s->m_type << " end]]\n";
            }
            else if(conciseFrame && (s->m_type == "exit" || s->m_type == "return"))
            {
                os << "[[" << s->m_type << " from " << s->m_invocation << "]]\n";
                
                if(f->m_invocation.second == FULL_TRACE_MAX_EVENTS_HIT_COUNT)
                {
                    os << "Function '" << f->m_invocation.first << "' reached the maximum hit count of " << f->m_invocation.second;
                    os << ". Future invocatoins will not be traced" << std::endl;
                }
            }
        }
        
        if(withIssus && !m_loadingIssus.empty())
        {
            os << m_loadingIssus;
        }
    }
    
    void printFrame(std::ostream& os, std::shared_ptr<const Frame> frame)
    {
        os << "\ncall stack: " << frame->m_stack << "\n";
        
        for (const auto s : frame->m_sections)
        {
            bool showSection = s->m_type != "arguments";
            if(showSection)
            {
                os << "[[" << s->m_type << " start]]\n";
                
                for (const auto& l : s->m_lines)
                    os << "  " << l << '\n';
                
                os << "[[" << s->m_type << " end]]\n";
            }
        }
    }
    
    void printFrame(std::ostream& os, const std::string function, int invocation)
    {
        //TODO: if invocation is -1 print a list with all recorded invocations (if possible) and info for the last or first one ?!
        
        std::string frameKey = function + ":" + std::to_string(invocation);
        
        auto frame = m_functionToFrameMap.find(frameKey);
        if(frame != m_functionToFrameMap.end())
        {
            printFrame(os, frame->second);
        }
        else
        {
            bool functinFound = false;
            std::string allInvocations;
            for(auto frame : m_frames)
            {
                if(frame->m_invocation.first == function)
                {
                    functinFound = true;
                    allInvocations += frame->m_stack + "\n\n";
                }
            }
            
            if(functinFound)
            {
                os << "Unable to find trace events recorded for the requested invocation '" << invocation << "'. ";
                os << "Here is a list with all invocations recorded in the trace for function '" << function << "'\n\n";
                os << allInvocations;
            }
            else
            {
                os << "Unable to find any events recorded in the trace for the requested function '" << function << "'.\n";
            }
        }
    }
    
    bool hasEvents(const std::string function, int invocation)
    {
        std::string frameKey = function + ":" + std::to_string(invocation);
        
        auto frame = m_functionToFrameMap.find(frameKey);
        if(frame != m_functionToFrameMap.end())
        {
            return frame->second->m_sections.size() > 0;
        }
        
        return false;
    }
    
    std::shared_ptr<const Frame> getLastFrame(bool fallbackToLastFound)
    {
        if (!m_framesStack.empty())
        {
            return m_framesStack.top();
        }
        
        // Fallback to the last frame in the vector if stack is empty (last found frame)
        if(fallbackToLastFound)
        {
            if (!m_frames.empty())
            {
                return m_frames.back();
            }
        }
        
        return nullptr;
    }
    
    std::shared_ptr<const Frame> getFrame(const std::string& function, int32_t invocation)
    {
        if(invocation == 0)
        {
            invocation = 1;
        }
        
        std::string frameKey = function + ":" + std::to_string(invocation);
        
        auto it = m_functionToFrameMap.find(frameKey);
        if(it != m_functionToFrameMap.end())
        {
            return it->second;
        }
        
        return nullptr;
    }
    
    std::shared_ptr<const Frame> getLastInvocation(const std::string& function)
    {
        std::shared_ptr<Frame> last = nullptr;
        
        for(auto frame : m_frames)
        {
            if(frame->m_invocation.first == function)
            {
                last = frame;
            }
        }
        
        return last;
    }
    
    std::pair<std::string, uint32_t> getBreakpointFunctionAndLine(const std::string& breakpointLogLine)
    {
        //NOTE: this functin assumes the following code in trace::hitBP in common_debug.h
        //log << "<[PUSH (breakpoint): " << getStack() << "]>" << std::endl;
        //log << "Hit breakpoint at function: " << atFunction << " line: " << atLine << std::endl;
        
        
        // regex breakdown:
        //  (.+?)                             capture function name (non-greedy)
        //  \s*line:\s*                       literal “line:” with optional spaces
        //  (\d+)                             capture one or more digits
        static const std::regex re{
            R"(Hit breakpoint at function:\s*(.+?)\s*line:\s*(\d+))"
        };

        std::smatch match;
        if (std::regex_search(breakpointLogLine, match, re) && match.size() == 3) {
            const std::string funcName = match[1].str();
            const uint32_t  lineNum  = static_cast<uint32_t>(std::stoul(match[2].str()));
            return { funcName, lineNum };
        }

        // Fallback on failure
        return { "", 0 };
    }
    
    std::vector<std::shared_ptr<Frame>> findBreadkpoints(const std::string& function, uint32_t line)
    {
        std::vector<std::shared_ptr<Frame>> frames;
        
        for (const auto& f : m_frames)
        {
            for (const auto s : f->m_sections)
            {
               if(stdrave::startsWith(s->m_type, "breakpoint"))
                {
                    if(s->m_lines.size())
                    {
                        auto bpLocation = getBreakpointFunctionAndLine(s->m_lines.front());
                        if(bpLocation.first == function && bpLocation.second == line)
                        {
                            //return true;
                            frames.push_back(f);
                        }
                    }
                }
            }
        }
        
        return frames;
    }
    
    uint32_t getFramesCount()
    {
        return (uint32_t)m_frames.size();
    }
    
    void clear()
    {
        m_frames.clear();
        m_functionToFrameMap.clear();
        m_sectionEvents.clear();
        m_loadingIssus.clear();
        
        // Clear the new stack as well
        while (!m_framesStack.empty())
        {
            m_framesStack.pop();
        }
    }
};
