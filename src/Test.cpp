#include "Test.h"
#include "Utils.h"

namespace stdrave {


DEFINE_TYPE(TestStep)
DEFINE_ARRAY_FIELD(TestStep, commands)
DEFINE_ARRAY_FIELD(TestStep, input_files)
DEFINE_ARRAY_FIELD(TestStep, output_files)

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

void TestStep::clear()
{
    commands.clear();
    input_files.clear();
    output_files.clear();
}

std::pair<std::set<std::string>, std::set<std::string>> TestDef::getIOFiles() const
{
    std::set<std::string> inputFiles;
    std::set<std::string> outputFiles;

    auto baseName = [](const std::string& p) {
        return boost_fs::path(p).filename().string();
    };

    forAllSteps([&](const TestStep& step) {
        for (const auto& file : step.input_files) {
            inputFiles.insert(baseName(*file));
        }
        for (const auto& file : step.output_files) {
            outputFiles.insert(baseName(*file));
        }
    });

    return { std::move(inputFiles), std::move(outputFiles) };
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
std::set<std::string> TestDef::getRewardHackingTestFiles() const
{
    std::set<std::string> files;
    
    auto baseName = [](const std::string& p) {
        return boost_fs::path(p).filename().string();
    };
    
    for (const auto& file : test.output_files) {
        
        if(isTextFileAsciiOrUtf8(*file))
        {
            files.insert(baseName(*file));
        }
    }
    
    for (const auto& file : posttest.output_files) {
        
        if(isTextFileAsciiOrUtf8(*file))
        {
            files.insert(baseName(*file));
        }
    }
    
    return files;
}

std::set<std::string> TestDef::getCommandLineFiles() const
{
    std::set<std::string> files;

    forAllSteps([&](const TestStep& step) {
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

std::string TestDef::validate(bool isPrivate)
{
    std::string feedback;
    
    if(name.empty())
    {
        feedback += "The 'name' must not be empty\n";
    }
    
    if(test.commands.size() > 1)
    {
        feedback += "More than one commands in the 'test.commands' section\n";
    }
    
    if(test.commands.size() < 1)
    {
        feedback += "Requires command in 'test.commands'\n";
    }
    else if(startsWithIgnoreCase(*test.commands[0], "./"))
    {
        boost_fs::path arg1AsPath(*test.commands[0]);
        if(!arg1AsPath.has_extension())
        {
            feedback += "'test.commands[0]' starts with executable. Executable for the main test will be implicitly provided\n";
        }
    }
    
    if(test.commands.size() >= 1)
    {
        std::string rawCmd = *test.commands[0];
        std::string cmd = rawCmd;
        std::string expectedResult;
        std::string stdoutRegex;
        bool debug = false;
        bool finalResult = false;
        parsePrefixFlags(rawCmd, debug, finalResult, expectedResult, stdoutRegex, cmd);
        
        if(startsWithIgnoreCase(cmd, "./"))
        {
            feedback += "'test.commands[0]' appears to contain an executable name as the first argument. Reminder: the executable for the main 'test' step is provided implicitly.\n";
        }
        
        if(stdoutRegex.empty() && expectedResult.empty() && posttest.commands.empty())
        {
            feedback += "'test.commands[0]' does not check stdout or the exit code, and posttest.commands is empty. ";
            feedback += "Ensure a mechanism is in place to verify the tested behavior.\n";
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
    
    return feedback;
}

std::string TestDef::checksStdout() const
{
    std::string result;

    forAllSteps([&](const TestStep& step) {
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
    desc += utility::conversions::to_utf8string( definition.to_json().serialize() );
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
