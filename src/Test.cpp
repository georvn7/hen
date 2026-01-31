#include "Test.h"
#include "Utils.h"

namespace stdrave {


DEFINE_TYPE(TestStep)
DEFINE_ARRAY_FIELD(TestStep, commands)
DEFINE_ARRAY_FIELD(TestStep, input_files)
DEFINE_ARRAY_FIELD(TestStep, output_files)

DEFINE_TYPE(TestCommand)
DEFINE_FIELD(TestCommand, command)
DEFINE_ARRAY_FIELD(TestCommand, input_files)
DEFINE_ARRAY_FIELD(TestCommand, output_files)

DEFINE_TYPE(CommandRegex)
DEFINE_FIELD(CommandRegex, test_step)
DEFINE_FIELD(CommandRegex, command_index)
DEFINE_FIELD(CommandRegex, regex_pattern)
DEFINE_FIELD(CommandRegex, example)

DEFINE_TYPE(TestRegexContract)
DEFINE_FIELD(TestRegexContract, note)
DEFINE_ARRAY_FIELD(TestRegexContract, regex_patterns)

DEFINE_TYPE(TestConfig)
DEFINE_ARRAY_FIELD(TestConfig, ramp)
DEFINE_FIELD(TestConfig, current)
DEFINE_ARRAY_FIELD(TestConfig, ramp_unit_tests)
DEFINE_FIELD(TestConfig, current_unit_test)

DEFINE_TYPE(TestDef)
DEFINE_FIELD(TestDef, name)
DEFINE_FIELD(TestDef, description)
DEFINE_FIELD(TestDef, pretest)
DEFINE_FIELD(TestDef, test)
DEFINE_FIELD(TestDef, posttest)
DEFINE_FIELD(TestDef, io_hint)

DEFINE_TYPE(UnitTest)
DEFINE_FIELD(UnitTest, definition)
DEFINE_FIELD(UnitTest, implementation)
DEFINE_ARRAY_FIELD(UnitTest, input_files)
DEFINE_FIELD(UnitTest, regex_contract)

void TestStep::clear()
{
    commands.clear();
    input_files.clear();
    output_files.clear();
}

void TestCommand::clear()
{
    command.clear();
    input_files.clear();
    output_files.clear();
}

std::string TestRegexContract::verify()
{
    std::string feedback;
    
    for(const auto pattern : regex_patterns)
    {
        std::string regexErr;
        if(!fullRegexMatch(pattern->example, pattern->regex_pattern, regexErr))
        {
            if (!regexErr.empty()) {
                //We must not be here
                feedback += "ERROR: invalid stdout regex: " + regexErr + "\n";
            }
            else
            {
                feedback += "Unable to fully match regex pattern:\n" + pattern->regex_pattern;
                feedback += "\nwith example:\n" + pattern->example + "\n\n";
            }
        }
    }
    
    return feedback;
}

std::pair<std::set<std::string>, std::set<std::string>> TestDef::getIOFiles() const
{
    std::set<std::string> inputFiles;
    std::set<std::string> outputFiles;

    auto baseName = [](const std::string& p) {
        return boost_fs::path(p).filename().string();
    };

    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        for (const auto& file : step.input_files) {
            inputFiles.insert(baseName(*file));
        }
        for (const auto& file : step.output_files) {
            outputFiles.insert(baseName(*file));
        }
    });

    return { std::move(inputFiles), std::move(outputFiles) };
}

uint32_t TestDef::hasRegexChecks() const
{
    uint32_t result = false;
    
    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        for (const auto& cmd : step.commands) {
            if (!cmd || cmd->empty())
                continue;

            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *cmd;
            std::string cmdLn = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdLn);
            if(!stdoutRegex.empty())
            {
                result++;
            }
        }
    });
    
    return result;
}

std::set<std::string> TestDef::getInputFiles() const
{
    const auto& ioFiles = getIOFiles();
    
    std::set<std::string> testInput;
    std::set_difference(ioFiles.first.begin(), ioFiles.first.end(),
                        ioFiles.second.begin(), ioFiles.second.end(),
                        std::inserter(testInput, testInput.begin()));
    
    return testInput;
}

std::set<std::string> TestDef::getRegressionFiles() const
{
    return getInputFiles();
}

//TODO: One day we can add images
std::set<std::string> TestDef::getRewardHackingTestFiles(const std::string& workingDirectory) const
{
    std::set<std::string> files;
    
    auto baseName = [](const std::string& p) {
        return boost_fs::path(p).filename().string();
    };
    
    const auto& testFiles = getInputFiles();
    
    for (const auto& file : testFiles) {
        
        std::string filePaht = workingDirectory + "/" + baseName(file);
        if(isTextFileAsciiOrUtf8(filePaht))
        {
            files.insert(baseName(file));
        }
    }
    
    for (const auto& file : test.output_files) {
        
        std::string filePaht = workingDirectory + "/" + baseName(*file);
        if(isTextFileAsciiOrUtf8(filePaht))
        {
            files.insert(baseName(*file));
        }
    }
    
    for (const auto& file : posttest.output_files) {
        
        std::string filePaht = workingDirectory + "/" + baseName(*file);
        if(isTextFileAsciiOrUtf8(filePaht))
        {
            files.insert(baseName(*file));
        }
    }
    
    return files;
}

std::set<std::string> TestDef::getCommandLineFiles() const
{
    std::set<std::string> files;

    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        for (const auto& cmd : step.commands) {
            if (!cmd || cmd->empty())
                continue;

            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *cmd;
            std::string cmdLn = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdLn);
            
            const auto extracted = extractFilesFromCommandLine(cmdLn);
            files.insert(extracted.begin(), extracted.end());
        }
    });

    return files;
}

std::string TestDef::validateCommands()
{
    bool hasLongCommands = false;

    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        
        for (const auto& command: step.commands) {
            if(command->size() > 512)
            {
                hasLongCommands = true;
            }
        }
        
    });
    
    if(hasLongCommands)
    {
        std::string feedback = "Some commands exceed 512 characters. ";
        feedback += "Avoid embedding test content directly in commands (e.g., via cat or echo). ";
        feedback += "Instead, declare input files in the input_files list, reference them in your commands, ";
        feedback += "and concisely describe their purpose in the test description. ";
        feedback += "File contents will be generated in a separate step.";
        return feedback;
    }
    
    return std::string();
}

std::string TestDef::validateIOFiles()
{
    std::string feedback;
    
    static std::set<std::string> disabledNames = {"common","main", "data_defs", "data_printers",
        "trace_printers", "common_debug", "common_eval", "black_box_api"
    };
    
    auto baseName = [](const std::string& p) {
        return boost_fs::path(p).stem().string();
    };
    
    bool mainAsIO = false;

    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        
        for (const auto& file : step.input_files) {
            
            if(disabledNames.find(baseName(*file)) != disabledNames.end())
            {
                feedback += *file + " ";
            }
            
            if(baseName(*file) == "main")
            {
                mainAsIO = true;
            }
        }
        for (const auto& file : step.output_files) {
            
            if(disabledNames.find(baseName(*file)) != disabledNames.end())
            {
                feedback += *file + " ";
            }
            
            if(baseName(*file) == "main")
            {
                mainAsIO = true;
            }
        }
    });
    
    if(!feedback.empty())
    {
        std::string message = "\nThe following file names are reserved for files managed by the build system ";
        message += "and must not be used to name files produced or consumed by the test:\n";
        feedback = message + feedback + "\n";
        if(mainAsIO)
        {
            feedback += "Note that the test driver (main.cpp) will be implemented in a spearate step later ";
            feedback += "and must not be listed in the input/otput files.\n";
        }
    }
    
    return feedback;
}

std::string TestDef::validate(bool isPrivate)
{
    std::string feedback;
    
    if(name.empty())
    {
        feedback += "The 'name' must not be empty\n";
    }
    
    if(test.command.empty())
    {
        feedback += "Requires command in 'test.command'\n";
    }
    else if(startsWithIgnoreCase(test.command, "./"))
    {
        boost_fs::path arg1AsPath(test.command);
        if(!arg1AsPath.has_extension())
        {
            feedback += "'test.command' starts with executable. Executable for the main test will be implicitly provided\n";
        }
    }
    
    bool emptyCommands = false;
    for(auto cmd : pretest.commands)
    {
        std::string rawCmd = *cmd;
        
        std::string cmdOnly = rawCmd;
        std::string expectedResult;
        std::string stdoutRegex;
        bool debug = false;
        bool finalResult = false;
        parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdOnly);
        
        auto args = stdrave::parseCommandLine(cmdOnly);
        
        if(args.empty())
        {
            feedback += "Command in the 'pretest' step has empty command line: " + rawCmd + "\n";
            emptyCommands = true;
        }
        
        //TODO: Signal here if pretest commands check the result
        if(finalResult)
        {
            feedback += "Command in a 'pretest' step uses 'result' or 'stdout' attributes. ";
            feedback += "The idea of the 'pretest' step is only to prepare files consumed by the 'test' step. ";
            feedback += "The commands in the 'pretest' step must not check the result with 'result' or 'stdout' attributes. ";
            feedback += "However, each command line is expected to exit successfuly when executed in the shell.\n";
        }
    }
    
    int postIndex = 0;
    for(auto cmd : posttest.commands)
    {
        std::string rawCmd = *cmd;
        
        std::string cmdOnly = rawCmd;
        std::string expectedResult;
        std::string stdoutRegex;
        bool debug = false;
        bool finalResult = false;
        parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdOnly);
        
        auto args = stdrave::parseCommandLine(cmdOnly);
        
        if(args.empty())
        {
            feedback += "Command in the 'posttest' step has empty command line: " + rawCmd + "\n";
            emptyCommands = true;
        }
        
        postIndex++;
        
        if(postIndex == posttest.commands.size())
        {
            if(!finalResult)
            {
                feedback += "The last command in the 'posttest' step doesn't check the result via the 'result' or 'stdout' attributes. ";
                feedback += "The idea of the 'posttest' step is to post process and check the outcome from the 'test' step. ";
                feedback += "If 'posttest' step lists any commands, only the last command has to use either 'result' or 'stdout' attributes to check the test success\n";
            }
        }
        else if(finalResult)
        {
            feedback += "Command that is not the last in the 'posttest' step uses 'result' or 'stdout' attributes. ";
            feedback += "The idea of the 'posttest' step is to post process and check the outcome from the 'test' step. ";
            feedback += "If 'posttest' step has any commands, only the last command has to use either 'result' or 'stdout' attributes to check the test success\n";
        }
    }
    
    if(emptyCommands)
    {
        feedback += "\n\nCommands in the pretest and posttest steps must specify executable\n\n";
    }
    
    if(!test.command.empty())
    {
        std::string rawCmd = test.command;
        std::string cmd = rawCmd;
        std::string expectedResult;
        std::string stdoutRegex;
        bool debug = false;
        bool finalResult = false;
        parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmd);
        
        if(!startsWithIgnoreCase(cmd, "main"))
        {
            feedback += "For test.command the name of the executable must be 'main'.\n";
        }
        
        if(stdoutRegex.empty() && expectedResult.empty() && posttest.commands.empty())
        {
            feedback += "'test.command' does not check stdout or the exit code, and posttest.commands is empty. ";
            feedback += "Ensure a mechanism is in place to verify the tested behavior.\n";
        }
    }
    else
    {
        feedback += "test.command can't be an emtpy string, at least the name of the executable must be 'main'.\n";
    }
    
    for(auto outFile : pretest.output_files)
    {
        std::string outFilename = boost_fs::path(*outFile).filename().string();
        bool consumed = false;
    
        for(auto inFile : test.input_files)
        {
            if(outFilename == boost_fs::path(*inFile).filename().string())
            {
                consumed = true;
                break;
            }
        }
        
        if(!consumed)
        {
            feedback += "File '" + outFilename + "' listed as output from the pretest step. ";
            feedback += "is not consumed by the test step. ";
            feedback += "All listed output files in the pretest step must be consumed by the test step\n";
        }
    }
    
    for(auto outFile : test.output_files)
    {
        std::string outFilename = boost_fs::path(*outFile).filename().string();
        bool consumed = false;
    
        for(auto inFile : posttest.input_files)
        {
            if(outFilename == boost_fs::path(*inFile).filename().string())
            {
                consumed = true;
                break;
            }
        }
        
        if(!consumed)
        {
            feedback += "File '" + outFilename + "' listed as output from the test step. ";
            feedback += "is not consumed by the posttest step. ";
            feedback += "All listed output files in the test step must be consumed by the posttest step\n";
        }
    }
    
    const auto ioFiles = getIOFiles();
    const auto cmdFiles = getCommandLineFiles();
    
    std::set<std::string> ioAll;
    ioAll.insert(ioFiles.first.begin(),  ioFiles.first.end());
    ioAll.insert(ioFiles.second.begin(), ioFiles.second.end());

    std::set<std::string> cmdOnly;

    std::set_difference(
        cmdFiles.begin(), cmdFiles.end(),
        ioAll.begin(),    ioAll.end(),
        std::inserter(cmdOnly, cmdOnly.end())
    );
    
    if(!cmdOnly.empty())
    {
        feedback += "The follwoing are presumable file names that appear in the command lines ";
        feedback += "but aren't specified as 'input_files' or 'output_files' for any of the steps: " + getAsCsv(cmdOnly);
        feedback += "\n";
    }
    
    feedback += validateIOFiles();
    feedback += validateCommands();
    
    return feedback;
}

std::string TestDef::validate(const TestRegexContract& contract)
{
    std::string feedback;
    
    uint32_t commandsWithRegex = hasRegexChecks();
    uint32_t contractRegexPatterns = (uint32_t)contract.regex_patterns.size();
    
    if(commandsWithRegex != contractRegexPatterns)
    {
        feedback += "The number of commands in the test (";
        feedback += std::to_string(commandsWithRegex);
        feedback += ") that check a regex to fully match stdout ";
        feedback += "doesn't match the number ot entries in the regex contract (";
        feedback += std::to_string(contractRegexPatterns);
        feedback += ")\n\n";
    }
    
    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        
        uint32_t commandIndex = 0;
        for (const auto& cmd : step.commands) {
            
            if (!cmd || cmd->empty())
                continue;

            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *cmd;
            std::string cmdLn = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdLn);
        
            bool foundInTheContract = false;
            if(!stdoutRegex.empty())
            {
                bool foundInContract = false;
                for(auto pattern : contract.regex_patterns)
                {
                    if(pattern->test_step == stepName &&
                       pattern->command_index == commandIndex)
                    {
                        foundInTheContract = true;
                        break;
                    }
                }
            }
            else
            {
                foundInTheContract = true;
            }
            
            if(!foundInTheContract)
            {
                feedback += "Unable to find regext contract entry for the '" + stepName + "' command ";
                feedback += stepName == "test" ? "" : ("with index " + std::to_string(commandIndex) + " ");
                feedback += "from the test script\n";
            }
            
            commandIndex++;
        }
    });
    
    if(!feedback.empty())
    {
        feedback += "All commands from the test script that have regex expression to fully match stdout must also have an entry in the test contract\n\n";
    }
    
    return feedback;
}

void TestDef::swapInvalid(const TestRegexContract& contract)
{
    forAllSteps([&](TestStep& step, const std::string& stepName) {
        
        uint32_t commandIndex = 0;
        for (const auto& cmd : step.commands) {
            
            if (!cmd || cmd->empty())
            {
                commandIndex++;
                continue;
            }

            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *cmd;
            std::string cmdLn = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdLn);
        
            if(!stdoutRegex.empty())
            {
                std::shared_ptr<CommandRegex> fromContract = nullptr;
                
                for(auto pattern : contract.regex_patterns)
                {
                    //Command vs contract match must have been validated at this point
                    if(pattern->test_step == stepName &&
                       pattern->command_index == commandIndex)
                    {
                        fromContract = pattern;
                        break;
                    }
                }
                
                if(fromContract)
                {
                    std::string err;
                    if(!fullRegexMatch(fromContract->example, stdoutRegex, err))
                    {
                        std::string newCommand = makeTestCommand(cmdLn, debug, finalResult, expectedResult, fromContract->regex_pattern);
                        step.commands[commandIndex] = std::make_shared<std::string>(newCommand);
                    }
                }
            }
            
            commandIndex++;
        }
    });
}

std::string TestDef::checksStdout() const
{
    std::string result;

    forAllSteps([&](const TestStep& step, const std::string& stepName) {
        for (const auto& cmd : step.commands) {
            
            if (!cmd || cmd->empty())
                continue;

            bool debug = false;
            bool finalResult = false;
            
            std::string rawCmd = *cmd;
            std::string cmdLn = rawCmd;
            std::string expectedResult;
            std::string stdoutRegex;
            parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmdLn);
            
            if(!stdoutRegex.empty())
            {
                result += rawCmd + "\n";
            }
        }
    });

    return result;
}

void TestDef::clear()
{
    name.clear();
    description.clear();
    pretest.clear();
    test.clear();
    posttest.clear();
    io_hint.clear();
    m_lastResult.clear();
}

std::string TestDef::getDescription(const std::string& workingDir) const
{
    std::string desc;
    desc += "\n```json\n";
    desc += utility::conversions::to_utf8string( to_json().serialize() );
    desc += "\n```\n\n";
    
    const auto& inputFiles = getInputFiles();
    
    for(const auto& file : inputFiles)
    {
        std::string filePath = workingDir + "/" + file;
        
        if(!boost_fs::exists(filePath))
        {
            std::cout << "Missing input file: " << filePath << " for test: " << name;
            continue;
        }
        
        desc += "Input file: " + file;
        desc += "\n```" + boost_fs::path(file).extension().string() + "/";
        desc += getFileContent(workingDir + "/" + file);
        desc += "```\n\n";
    }
    
    return desc;
}

bool TestDef::load(const std::string& jsonPath)
{
    std::ifstream file(jsonPath);
    if(!file.good())
    {
        std::cout << "ERROR: Unable to load test definition: " << jsonPath << std::endl;
        return false;
    }
    
    std::string jsonStr((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto uJsonStr = utility::conversions::to_string_t(jsonStr);
    auto json = web::json::value::parse(uJsonStr);
    from_json(json);
    
    return true;
}

void UnitTest::clear()
{
    definition.clear();
    implementation.clear();
    input_files.clear();
}

std::string UnitTest::getDescription()
{
    std::string desc;
    desc += "\n```json\n";
    std::string testDef = formatJson(utility::conversions::to_utf8string( definition.to_json().serialize() ), "  ");
    desc += testDef;
    desc += "\n```\n\n";
    
    desc += "Unit test source:\n";
    desc += "\n```cpp\n";
    desc += implementation;
    desc += "```\n\n";
    
    for(const auto& file : input_files)
    {
        desc += "Input file: " + file->file_name;
        desc += "\n```" + boost_fs::path(file->file_name).extension().string() + "\n";
        desc += file->content;
        desc += "```\n\n";
        
    }
    
    return desc;
}

}
