#pragma once

#include "Reflection.h"
#include "File.h"

#include <set>

namespace stdrave {

class TestStep : public Reflection<TestStep>
{
public:
    DECLARE_TYPE(TestStep, "Description")
    DECLARE_ARRAY_FIELD(std::string, commands, "List with commands to executed on this step")
    DECLARE_ARRAY_FIELD(std::string, input_files, "Read-only, input files used as command line arguments for this step")
    DECLARE_ARRAY_FIELD(std::string, output_files, "Files produced by the commands in this step")
    
    void clear();
};

class TestCommand : public Reflection<TestCommand>
{
public:
    DECLARE_TYPE(TestCommand, "Description")
    DECLARE_FIELD(std::string, command, "Command to execute, the name of the executable must be 'main'.")
    DECLARE_ARRAY_FIELD(std::string, input_files, "Read-only, input files used as command line arguments for this command")
    DECLARE_ARRAY_FIELD(std::string, output_files, "Files produced by the command")
    
    void clear();
};

class CommandRegex : public Reflection<CommandRegex>
{
public:
    DECLARE_TYPE(CommandRegex, "Regex contract for a single command from the test")
    DECLARE_ENUM_FIELD(test_step, "\"pretest\",\"test\",\"posttest\"","For which test step is this regex pattern")
    DECLARE_FIELD(uint32_t, command_index, "For which command int the test step is this regex pattern.")
    DECLARE_FIELD(std::string, regex_pattern, "The ECMAScript regex pattern that will be fully matched with the command stdout")
    DECLARE_FIELD(std::string, example, "Verification example that matches with the regex pattern")
};

class TestRegexContract : public Reflection<TestRegexContract>
{
public:
    DECLARE_TYPE(TestRegexContract, "Regex contract all commands in the test")
    DECLARE_FIELD(std::string, note, "Concise one or two sentences note about kind of gotchas related to the regex pattern the agent need to be careful when implementing and debugging the test")
    DECLARE_ARRAY_FIELD(CommandRegex, regex_patterns, "For all commands with regex match check.")
    
    std::string verify();
};

class TestDef : public Reflection<TestDef>
{
private:
    template <typename Fn>
    void forAllSteps(Fn&& fn)
    {
        fn(pretest);
    
        //TODO: Is there a better way to do that?
        TestStep testStep;
        testStep.input_files = test.input_files;
        testStep.output_files = test.output_files;
        testStep.commands = { std::make_shared<std::string>(test.command) };
        fn(testStep);
        
        fn(posttest);
    }

    template <typename Fn>
    void forAllSteps(Fn&& fn) const
    {
        fn(pretest);
        
        //TODO: Is there a better way to do that?
        TestStep testStep;
        testStep.input_files = test.input_files;
        testStep.output_files = test.output_files;
        testStep.commands = { std::make_shared<std::string>(test.command) };
        fn(testStep);
        
        fn(posttest);
    }
    
    std::string validateIOFiles();
    std::string validateCommands();
    
public:
    DECLARE_TYPE(TestDef, "Description")
    DECLARE_FIELD(std::string, name, "The name of the test")
    DECLARE_FIELD(std::string, description, "Detailed description of the unit test. "\
                                "This description will be used as implementation guidelines")
    DECLARE_FIELD(TestStep, pretest, "Setp with commands that will be executed before running the test")
    DECLARE_FIELD(TestCommand, test, "Command line to run the executable being debugged. This step is the actual test")
    DECLARE_FIELD(TestStep, posttest, "Setp with commands that will be executed after running the test")
    DECLARE_FIELD(std::string, io_hint, "IO hint, must be 'none' if no hint, not empty or missing")
    
    std::set<std::string> getInputFiles() const;
    std::set<std::string> getRegressionFiles() const;
    std::set<std::string> getRewardHackingTestFiles(const std::string& workingDirectory) const;
    std::pair<std::set<std::string>, std::set<std::string>> getIOFiles() const;
    std::set<std::string> getCommandLineFiles() const;
    std::string getDescription(const std::string& workingDir) const;
    
    std::string validate(bool isPrivate);
    void clear();
    std::string checksStdout() const;
    std::string m_lastResult;
    
    bool load(const std::string& jsonPath);
    bool hasRegexChecks() const;
};

class TestConfig : public Reflection<TestConfig>
{
public:
    DECLARE_TYPE(TestConfig, "Description")
    DECLARE_ARRAY_FIELD(std::string, ramp, "Tests ramp")
    DECLARE_FIELD(std::string, current, "Current test to start from")
    DECLARE_ARRAY_FIELD(std::string, ramp_unit_tests, "Tests ramp")
    DECLARE_FIELD(std::string, current_unit_test, "Current unit test to start from")
};

class UnitTest : public Reflection<UnitTest>
{
public:
    DECLARE_TYPE(UnitTest, "Description")
    
    DECLARE_FIELD(TestDef, definition, "Fully describes the unit test, its behavior, whether it needs command line arguments and input files");
    DECLARE_FIELD(std::string, implementation, "Description")
    DECLARE_ARRAY_FIELD(File, input_files, "Description")
    DECLARE_FIELD(TestRegexContract, regex_contract, "Regex contract for this unit test")
    
    void clear();
    std::string getDescription();
};

}
