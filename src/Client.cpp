#include <fstream>
#include <iostream>
#include <string>
#include <iomanip>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h> // For Linux/Unix
#endif

#include "Client.h"
#include "CCodeProject.h"
#include "Utils.h"
#include "Server.h"
#include "Artifacts.h"
#include "Distillery.h"

#include <cpprest/json.h>

using namespace web; // For convenience
using namespace utility; // For conversions

namespace stdrave {

std::atomic<bool> stop_requested(false);

struct NullBuf : std::streambuf { int overflow(int c) override { return c; } };

static NullBuf g_null;
static std::streambuf* g_old = nullptr;
static bool g_muted = false;

void set_cout_enabled(bool enabled) {
#if 0
    if (enabled) {
        if (g_muted) {
            std::cout.rdbuf(g_old);
            g_muted = false;
        }
    } else {
        if (!g_muted) {
            g_old = std::cout.rdbuf(&g_null);
            g_muted = true;
        }
    }
#endif
}

int Client::init(int argc, char* argv[])
{
    m_supportsFunctionCalls = false;
    m_auto = false;
    m_llmLock = false;
    m_localUser = true;
    //m_agentPort = -1;
    m_projectId = "";
    m_disableUserCommands = false;
    m_logEnabled = true;
    
    std::cout << m_commands.size() << std::endl;
    
#if 1
    registerCommand("step", []() { boost_opt::options_description desc; desc.add_options()
        ("help,h", "Continues execution to the next response from the LLM")
        ("decompose,d", boost_opt::value<bool>(), "Sets decompose pass, by default keeps the current value")
        ("integrate,i", boost_opt::value<bool>(), "Toggle integrate pass, by default keeps the current value")
        ("cache,c", boost_opt::value<bool>(), "Toggle cache schemas, by default keeps the current value")
        ("save,s", boost_opt::value<bool>(), "Toggle save nodes after being fully defined, by default keeps the current value");
        return desc;}());
    
    registerCommand("continue", []() { boost_opt::options_description desc; desc.add_options()
        ("help,h", "Continues execution to the next breackpoint or the end of the build");
        return desc;}());
    
    registerCommand("save", []() { boost_opt::options_description desc; desc.add_options()
        ("help,h", "Saves the graph")
        ("overwrite,o", "Overwrite existing directory")
        ("directory,d", boost_opt::value<std::string>(), "Output directory");
    return desc; }());
    
    registerCommand("start", []() { boost_opt::options_description desc; desc.add_options()
        ("help,h", "Starts the depth-first decompose/integrate traversal")
        ("decompose,d", boost_opt::value<bool>(), "Sets decompose pass, by default keeps the current value")
        ("integrate,i", boost_opt::value<bool>(), "Toggle integrate pass, by default keeps the current value")
        ("cache,c", boost_opt::value<bool>(), "Toggle cache schemas, by default keeps the current value")
        ("generate,g", boost_opt::value<bool>(), "Generates project description from the chat history")
        ("save,s", boost_opt::value<bool>(), "Toggle save nodes after being fully defined, by default keeps the current value")
        ("reorder,r", boost_opt::value<bool>(), "Reorder dependent data type definitions, by default keeps the current value")
        ("brainstorm,b", boost_opt::value<bool>(), "Slef brainstorming the responses, by default keeps the current value");
    return desc; }());
    
    registerCommand("quit", []() { boost_opt::options_description desc; desc.add_options()
        ("help,h", "Exit the application");
    return desc; }());
    
    registerCommand("skip", []() { boost_opt::options_description desc; desc.add_options()
        ("help,h", "Skips human-in-a-loop interaction triggered by malfunctioned response");
    return desc; }());
    
    registerCommand("help", []() { boost_opt::options_description desc; desc.add_options()
        ;//("help,h", "Exit the application");
    return desc; }());
#endif

    Peer::init(argc, argv);

    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "-clver")
        {
            std::cout << "0.1\n";
        }
        else if (std::string(argv[i]) == "-sip")
        {
            m_serverIP = argv[i+1];
        }
        else if (std::string(argv[i]) == "-proj")
        {
            m_projectDirectory = argv[i+1];
            std::cout << "Project directory " << m_projectDirectory << std::endl;
        }
        else if (std::string(argv[i]) == "-pid")
        {
            try {
                m_projectId = argv[i+1];
            } catch (const std::exception& e) {
                std::cerr << "Unable to parse projectId. Could be fatal: " << e.what() << std::endl;
            }
            
            std::cout << "ProjectId: " << m_projectId << std::endl;
        }
        else if (std::string(argv[i]) == "-dp")
        {
            m_debugPort = (uint16_t)atoi(argv[i+1]);
            //TODO: Do a check for valid debug port
        }
    }

    m_requestId = 0;
    
    setupEnv();
    initClient(m_llmProxyIP);
    //if(m_agentPort != -1)
    if(!m_serverIP.empty())
    {
        m_localUser = false;
        connectToServer();
    }
    
    m_project = nullptr;

    bool projectFound = initProject() == 0;
    
    if (!projectFound)
    {
        std::cerr << "ERROR: Couldnt find project folder! ";
        std::cerr << "Make sure to set it from the command line with -proj flag" << std::endl;
    }

    return 0;
}

void Client::shutdown()
{
}

void Client::log(const json::value& jsonObject, const std::string& fileName)
{
    utility::string_t jsonStringU = jsonObject.serialize();
    std::string jsonString = utility::conversions::to_utf8string(jsonStringU);
   
    boost_fs::path p(fileName);
    
    if(boost_fs::exists(p.parent_path()))
    {
        std::ofstream outFile(fileName, std::ios::out | std::ios::trunc);
        
        if (outFile.is_open())
        {
            // Write the serialized JSON string to the file
            outFile << jsonString;
            
            //We can't afford to save a log without saving the sats with request_id
            if(m_project)
            {
                m_project->saveStats();
            }
        }
        else
        {
            std::cerr << "Failed to open file for writing: " << fileName << std::endl;
        }
    }
    else //In case the node is being created at the moment
    {
        // Serialize the JSON object to a string
        m_log[fileName] = jsonString;
    }
}

void Client::logRequest(const json::value& jsonObject, const std::string& fileName)
{
    json::value jsonWithMetadata = jsonObject;
    
    jsonWithMetadata[U("node_dir")] = json::value::string(conversions::to_string_t(m_ctxLogDir));
    jsonWithMetadata[U("prompt")] = json::value::string(conversions::to_string_t(m_ctxPrompt));
    jsonWithMetadata[U("requested_llm")] = json::value::string(conversions::to_string_t(m_ctxLLMRequested));
    jsonWithMetadata[U("time")] = json::value::string(getCurrentDateTime());
    
    log(jsonWithMetadata, fileName);
}

void Client::logResponse(const json::value& jsonObject, const std::string& fileName)
{
    log(jsonObject, fileName);
}

void Client::flushLog()
{
    std::map<std::string, std::string> newLog;
    for(auto entry : m_log)
    {
        // Open a file in write mode.
        boost_fs::path p(entry.first);
        boost_fs::create_directories(p.parent_path());
        //if(boost::filesystem::exists(p.parent_path()))
        {
            std::ofstream outFile(entry.first, std::ios::out | std::ios::trunc);
            if (!outFile.is_open()) {
                std::cerr << "Failed to open file for writing: " << entry.first << std::endl;
                continue;
            }
            
            // Write the serialized JSON string to the file
            outFile << entry.second;
        }
    }
    
    m_log = newLog;
}

std::shared_ptr<LLMConfig> Client::currentLLMConfig()
{
    if(m_currentLLM == LLMRole::DEVELOPER)
    {
        return findLLM(m_llmDeveloper.first, m_llmDeveloper.second);
    }
    else if(m_currentLLM == LLMRole::EXPERT)
    {
        return findLLM(m_llmExpert.first, m_llmExpert.second);
    }
    else if(m_currentLLM == LLMRole::DIRECTOR)
    {
        return findLLM(m_llmDirector.first, m_llmDirector.second);
    }
    
    return nullptr;
}

std::shared_ptr<LLMConfig> Client::getLLMConfig(LLMRole forRole)
{
    if(forRole == LLMRole::DEVELOPER)
    {
        return findLLM(m_llmDeveloper.first, m_llmDeveloper.second);
    }
    else if(forRole == LLMRole::EXPERT)
    {
        return findLLM(m_llmExpert.first, m_llmExpert.second);
    }
    else if(forRole == LLMRole::DIRECTOR)
    {
        return findLLM(m_llmDirector.first, m_llmDirector.second);
    }
    
    return nullptr;
}

void Client::checkLLMContextSize(const json::value& messages, json::value& request)
{
    //Handle context size escalation
    uint32_t estimatedTokens = (uint32_t)messages.serialize().length() / CHARACTERS_PER_TOKEN;
    LLMRole currentLLM = m_currentLLM;
    utility::string_t uLLM;
    
    std::string selectedLLM = m_llmDeveloper.first + "/" + m_llmDeveloper.second;
    if(m_currentLLM == LLMRole::EXPERT) {
        selectedLLM = m_llmExpert.first + "/" + m_llmExpert.second;
    }
    else if(m_currentLLM == LLMRole::DIRECTOR) {
        selectedLLM = m_llmDirector.first + "/" + m_llmDirector.second;
    }
    
    if(currentLLM == LLMRole::DEVELOPER)
    {
        m_ctxLLMRequested = m_llmDeveloper.first + "/" + m_llmDeveloper.second;
        
        std::shared_ptr<LLMConfig> llm = findLLM(m_llmDeveloper.first, m_llmDeveloper.second);
        if((estimatedTokens + MIN_LLM_OUTPUT_SIZE) > llm->context_size*1024) {
            currentLLM = LLMRole::EXPERT;
        } else {
            uLLM = U(m_llmDeveloper.first + "/" + m_llmDeveloper.second);
        }
    }
    if(currentLLM == LLMRole::EXPERT)
    {
        m_ctxLLMRequested = m_llmExpert.first + "/" + m_llmExpert.second;
        
        std::shared_ptr<LLMConfig> llm = findLLM(m_llmExpert.first, m_llmExpert.second);
        if((estimatedTokens + MIN_LLM_OUTPUT_SIZE) > llm->context_size*1024) {
            currentLLM = LLMRole::DIRECTOR;
        } else {
            uLLM = U(m_llmExpert.first + "/" + m_llmExpert.second);
        }
    }
    if(currentLLM == LLMRole::DIRECTOR)
    {
        m_ctxLLMRequested = m_llmDirector.first + "/" + m_llmDirector.second;
        
        uLLM = U(m_llmDirector.first + "/" + m_llmDirector.second);
    }
    
    if(m_currentLLM != currentLLM)
    {
        std::cout << "***** LLM Escaltaion due to estimated tokens count: " << estimatedTokens;
        std::cout << " Selected LLM: " << selectedLLM << " escalated to: " << m_ctxLLMRequested << std::endl;
    }
    
    m_ctxLLMUsed = utility::conversions::to_utf8string(uLLM);
    request[U("llm")] = json::value(uLLM);
}

bool Client::sendRequest(const json::value& messages, json::value& response, const json::value* schemas)
{
    json::value request;
    
    request[U("messages")] = messages;

    if (schemas && m_supportsFunctionCalls)
    {
        request[U("functions")] = *schemas;
        request[U("function_call")] = web::json::value(U("auto"));
    }
    
    if(!m_projectId.empty())
    {
        request[U("projectId")] = json::value::string(utility::conversions::to_string_t(m_projectId));
    }
    
    checkLLMContextSize(messages, request);

    std::string logDir = m_ctxLogDir;
    
    std::string requestLog = logDir + "/request_";
    requestLog += std::to_string(m_requestId);
    requestLog += "_" + m_ctxPrompt;
    requestLog += ".json";
    logRequest(request, requestLog);
    
    auto& messagesArray = request[U("messages")].as_array();
    auto responseMessageIndex = messagesArray.size();
    auto errorMessageIndex = responseMessageIndex + 1;

    bool finished = false;
    for(uint32_t i=0; i<10; ++i)
    {
        //This will be deleted on the proxy, before sending it to the LLMs,
        //we need to set it every iteration
        request[U("request_id")] = json::value::number(m_requestId);
        
        Client::sendToServer(request, response);
        
        if(!response.has_field(U("request_id")))
        {
            std::cout << "Skipping response without request_id" << std::endl;
            continue;
        }
        
        uint32_t requestId = (uint32_t)response[U("request_id")].as_integer();
        if (requestId != m_requestId)
        {
            std::cout << "Skipping response with invalid request_id: " << requestId << std::endl;
            continue;
        }
        
        //Strip the thinking tokens
        if (response.has_field(U("message")))
        {
            if (response[U("message")].has_field(U("content")))
            {
                auto& contentVal = response[U("message")][U("content")];
                if(!contentVal.is_string())
                {
                    //The content must be a string, we can't continue otherwise
                    
                    std::cout << "sendRequest content in the response is not a string" << std::endl;
                    continue;
                }
                
                auto ucontent = response[U("message")][U("content")].as_string();
                ucontent = removeThinkPart(ucontent);
                response[U("message")][U("content")] = json::value::string(ucontent);
            }
        }
            
        if (!schemas)
        {
            finished = true;
            break;
        }
        else
        {
            const web::json::value& schema = schemas->as_array().at(0);
            web::json::value object;
            bool hasObject = m_project->handleResponse(response, &object, false);
            
            //std::cout << std::endl << "RESPONSE:" << std::endl;
            //std::cout << utility::conversions::to_utf8string(response.serialize()) << std::endl;

            if(hasObject)
            {
                const web::json::value& parameters = schemas->as_array().at(0);
                assert(parameters.has_object_field(U("parameters")));
                if (parameters.has_object_field(U("parameters")))
                {
                    const web::json::value& schema = parameters.at(U("parameters"));
                    std::string jsonLog;
                    bool validObject = validateJson(object, schema, jsonLog);
                    if (validObject)
                    {
                        finished = true;
                        break;
                    }
                    else
                    {
                        if (response.has_field(U("message")))
                        {
                            if (response[U("message")].has_field(U("content")))
                            {
                                auto ucontent = response[U("message")][U("content")].as_string();
                                
                                messagesArray[responseMessageIndex][U("content")] = json::value(ucontent);
                                messagesArray[responseMessageIndex][U("role")] = json::value(U("assistant"));
                                
                                utility::string_t failMessage = conversions::to_string_t(jsonLog);
                                
                                failMessage += U("\nIn the response, provide only a single top-level JSON formatted object as specified in the provided JSON Schema. In the JSON object return only the fields specified in the JSON Schema. ALL fields must be presented, including those not explicitly marked as required in the JSON Schema. If a field is not needed, you can leave the value empty\n");
                                
                                messagesArray[errorMessageIndex][U("content")] = json::value(failMessage);
                                messagesArray[errorMessageIndex][U("role")] = json::value(U("user"));
                                
                                std::cout << failMessage;
                            }
                        }
                    }
                }
            }
            else
            {
                if (response.has_field(U("message")))
                {
                    if (response[U("message")].has_field(U("content")))
                    {
                        auto ucontent = response[U("message")][U("content")].as_string();
                        
                        auto markdownSection = hasMarkdownSection(ucontent);
                        if(markdownSection && *markdownSection != U("json"))
                        {
                            
                            messagesArray[responseMessageIndex][U("content")] = json::value(ucontent);
                            messagesArray[responseMessageIndex][U("role")] = json::value(U("assistant"));
                            
                            utility::string_t failMessage;
                            failMessage += U("Expected JSON formatted answer but found another markdown section: ");
                            failMessage += *markdownSection + U("\n");
                            //failMessage += U("Provide JSON formatted answer as specified in the JSON Schema. All fields are required!!!\n");
                            failMessage += U("\nProvide JSON formatted answer as specified in the JSON Schema. In the JSON object return only the fields specified in the JSON Schema. ALL fields must be presented, including those not explicitly marked as required in the JSON Schema. If a field is not needed, you can leave the value empty\n");
                            
                            messagesArray[errorMessageIndex][U("content")] = json::value(failMessage);
                            messagesArray[errorMessageIndex][U("role")] = json::value(U("user"));
                            std::cout << failMessage;
                        }
                        else
                        {
                            utility::string_t ujson;
                            try
                            {
                                ujson = findJson(ucontent, false);
                                auto object = web::json::value::parse(ujson);
                            }
                            catch (const web::json::json_exception& e)
                            {
                                utility::string_t failMessage;
                                
                                messagesArray[responseMessageIndex][U("content")] = json::value(ucontent);
                                messagesArray[responseMessageIndex][U("role")] = json::value(U("assistant"));
                                
                                if(!ujson.empty())
                                {
                                    ucout << U("********** Invalid json object start **********") << std::endl;
                                    ucout << ujson << std::endl;
                                    ucout << U("********** Invalid json object end **********") << std::endl;
                                }
                                
                                {
                                    ucout << U("Unable to extract json object from the content section") << std::endl;
                                    ucout << U("********** Message content start **********") << std::endl;
                                    ucout << ucontent << std::endl;
                                    ucout << U("********** Message content end **********") << std::endl;
                                }
                                
                                failMessage += U("*** Info about response with invalid JSON start here ***\n");
                                
                                failMessage += U("I've tried to parse the JSON from your response with CPPRESTSDK like this:\n");
                                failMessage += U("auto object = web::json::value::parse(content);\n");
                                failMessage += U("But this has generated the following exception:\n");
                                failMessage += U(e.what());
                                failMessage += U("\n");
                                failMessage += U("Here are a few common pitfalls to avoid:\n");
                                failMessage += U("  - Unescaped quotation marks\n");
                                failMessage += U("  - Unescaped backslashes\n");
                                failMessage += U("  - Invisible control characters\n");
                                failMessage += U("  - Incorrect line break handling\n");
                                failMessage += U("  - Missing commas or brackets\n");
                                
                                failMessage += U("  - Non-standard JSON extensions (trailing commas, comments)\n");
                                failMessage += U("  - Over-escaping characters that don't need escaping\n");
                                failMessage += U("  - Wrong Unicode escaping format (e.g., using \\u instead of \\U)\n");
                                failMessage += U("  - Using scientific notation inconsistently\n");
                                failMessage += U("  - Not validating UTF-8 encoding\n");
                                failMessage += U("  - Missing quotes around numeric strings\n");
                                failMessage += U("  - Inconsistent number formatting across locales\n");
                                
                                failMessage += U("  - Mixing boolean types (true/false vs 1/0)\n");
                                failMessage += U("  - Null vs empty string confusion\n");
                                failMessage += U("  - Incorrect handling of floating-point precision\n");
                                failMessage += U("  - Using raw tab characters\n");
                                failMessage += U("  - Not handling BOM (Byte Order Mark) correctly\n");
                                failMessage += U("  - Assuming object property order is guaranteed\n");
                                
                                failMessage += U("*** Info about response with invalid JSON end here ***\n");
                                failMessage += U("\nProvide fixed JSON!\n");
                                
                                messagesArray[errorMessageIndex][U("content")] = json::value(failMessage);
                                messagesArray[errorMessageIndex][U("role")] = json::value(U("user"));
                                
                                ucout << failMessage;
                            }
                        }
                    }
                    else
                    {
                        ucout << "Unknown message in the response:\n" << response[U("message")].serialize() << std::endl;
                    }
                }
            }
        }

        std::cout << "sendRequest attempt " << i+1 << ": the response from the server has been stoped..." << std::endl;
    }

    if (!finished)
    {
        std::cout << "sendRequest couldn't finish successfully" << std::endl;
        return false;
    }
    
    if (
        response.has_field(U("usage")) &&
        response[U("usage")].has_field(U("prompt_tokens")) &&
        response[U("usage")].has_field(U("completion_tokens")) &&
        response[U("usage")].has_field(U("step_credits")) &&
        response[U("usage")].has_field(U("consumed_credits")) &&
        response[U("usage")].has_field(U("limit_credits"))
        )
    {
        uint32_t promptTokens = (uint32_t)response[U("usage")][U("prompt_tokens")].as_number().to_uint64();
        uint32_t completionTokens = (uint32_t)response[U("usage")][U("completion_tokens")].as_number().to_uint64();
        
        float lastStep = (float)response[U("usage")][U("step_credits")].as_number().to_double();
        float consumed = (float)response[U("usage")][U("consumed_credits")].as_number().to_double();
        uint32_t limit = (uint32_t)response[U("usage")][U("limit_credits")].as_number().to_uint64();
        
        std::string llmCfgStr = utility::conversions::to_utf8string(request[U("llm")].as_string());
        auto llmCfg = splitByFirstOccurence(llmCfgStr, '/');
        auto usedLLM = findLLM(llmCfg.first, llmCfg.second);
        
        setStepCost(usedLLM, lastStep, consumed, limit);
        sendStepLog();
    }

    std::string responseLog = logDir + "/response_";
    responseLog += std::to_string(m_requestId);
    responseLog += "_" + m_ctxPrompt;
    responseLog += ".json";
    logResponse(response, responseLog);
    
    m_requestId++;

    return true;
}

std::string Client::getUserInput()
{
    if(m_localUser)
    {
        std::string cmdLn;
        std::getline(std::cin, cmdLn);
        return cmdLn;
    }
    else
    {
        while(!m_clientEP || !m_clientEP->session() || !m_clientEP->session()->isConnected())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        
        std::string command = m_clientEP->session()->receive()->c_str();
        std::cout << "Server command: " << command << std::endl;
        
        if(!command.empty())
        {
            m_auto = true;
        }
        
        return command;
    }
}

bool Client::processUserInput()
{
    if(m_disableUserCommands)
    {
        return true;
    }
    
    if(m_auto)
    {
        return 1;
    }

	//Wait for the next user command
	while (1)
	{
        set_cout_enabled(true);
        std::string prompt = "(" + std::string(MERCH_PRODUCT_NAME) + "):";
        std::cout << prompt;
        
        std::string cmdLn = getUserInput();
        set_cout_enabled(false);

        std::vector<std::string> tokens = boost_opt::split_unix(cmdLn);
        if(!tokens.size())
        {
            std::cout << "Empty command line" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        
        if (tokens[0] == ">")
        {
            //TODO: Convert a command from native English language to Unix command-line
            continue; 
        }
        std::string command = tokens[0];

		auto it = m_commands.find(command);
		if (it == m_commands.end())
		{
            cmdLn = "chat" + cmdLn;
            command = "chat";
		}
        
        //=======================================
        // Parse command line arguments
        
        // Setup for handling the "input" argument as a positional option
        boost_opt::positional_options_description pos_opt;
        pos_opt.add("input", -1); // All positional arguments are mapped to input
        
        boost_opt::variables_map args;
        if (command == "continue")
        {
            std::cout << "Auto traversal, Ctrl+C to stop...\n";
            m_auto = true;
            break;
        }
        else if(command == "help")
        {
            for(auto cmd : m_commands)
            {
                std::cout << std::endl;
                std::cout << "Command: " << cmd.first << std::endl;
                std::cout << cmd.second << std::endl;
            }
            std::cout << std::endl;
            
            continue;
        }
        else if(command == "quit")
        {
            exit(0);
        }
        else if(command == "skip")
        {
            return false;
        }
        else if(command != "chat")
        {
            try {
                std::vector<std::string> argsOnly(++tokens.begin(),tokens.end());
                boost_opt::store(boost_opt::command_line_parser(argsOnly).options(it->second).positional(pos_opt).run(), args);
                boost_opt::notify(args);
            }
            catch (const boost_opt::unknown_option& e) {
                // Handle unknown options
                std::cerr << "Unknown option: " << e.what() << std::endl;
                // Possibly show help message here
                continue;
            }
            catch (const boost_opt::invalid_command_line_syntax& e) {
                // Handle syntax errors
                std::cerr << "Invalid command line syntax: " << e.what() << std::endl;
                // Possibly show help message here
                continue;
            }
            catch (const boost_opt::error& e) {
                // Handle all other Boost.Program_options errors
                std::cerr << "Error: " << e.what() << std::endl;
                // Possibly show help message here
                continue;
            }
            catch (const std::exception& e) {
                // Handle standard exceptions
                std::cerr << "Unhandled Standard Exception: " << e.what() << std::endl;
                continue;
            }
            catch (...) {
                // Catch-all for any other exceptions
                std::cerr << "Unknown error occurred" << std::endl;
                continue;
            }
        }
        //=======================================
		
        if (m_project->executeCommand(command, cmdLn.substr(command.size()), args) > 0)
        {
            m_auto = false;
            break;
        }
	}
    
    return true;
}

int Client::initProject()
{
    std::wcout << "Init project..." << std::endl;

    std::string projectPath = m_environmentDirectory + "/Project.json";

    std::ifstream file(projectPath);
    std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto ustr = conversions::to_string_t(str);
    json::value projectJson = json::value::parse(ustr);
    utility::string_t projectType = projectJson[U("type")].as_string();
    //TODO: We need abstraction here. The Client should not have knowedge of the specic project (CCodeProject in this case)
    if (projectType == U("CCodeProject"))
    {
        m_project = new CCodeProject;
        
        m_project->m_description.from_json(projectJson);
        
        m_project->setup(m_projectDirectory);
    }
    
    return 0;
}

void Client::useMentorLLM()
{
    m_currentLLM = LLMRole::DIRECTOR;
}

void Client::useArchitectLLM()
{
    m_currentLLM = LLMRole::EXPERT;
}

void Client::useDeveloperLLM()
{
    m_currentLLM = LLMRole::DEVELOPER;
}

bool Client::escalateLLM()
{
    if(m_currentLLM == LLMRole::DEVELOPER)
    {
        setLLM(LLMRole::EXPERT);
        return true;
    }
    else if(m_currentLLM == LLMRole::EXPERT)
    {
        setLLM(LLMRole::DIRECTOR);
        return true;
    }
    
    return false;
}

void Client::setLLM(LLMRole role)
{
    if(m_llmLock) return;
    
    switch(role)
    {
        case LLMRole::DIRECTOR:
            useMentorLLM();
            break;
        case LLMRole::EXPERT:
            useArchitectLLM();
            break;
        case LLMRole::DEVELOPER:
        default:
            useDeveloperLLM();
            break;
    }
}

LLMRole Client::getLLM() const
{
    return m_currentLLM;
}

LLMRole Client::getLLMIntent(InferenceIntent intent)
{
    static LLMRole s_intents[] =
    {
        EXPERT,//DEFINE = 0,
        DIRECTOR,//BREAKDOWN_HI,
        DIRECTOR,//BREAKDOWN_LO,
        DEVELOPER,//SEARCH_LIB,
        EXPERT,//RESOLVE,
        DEVELOPER,//DATA,
        EXPERT,//IMPLEMENT_OPTIMISTIC ,
        DEVELOPER,//IMPLEMENT,
        DEVELOPER,//FIX_OPTIMISTIC,
        EXPERT,//FIX,
        EXPERT,//FIX_PANIC,
        EXPERT,//REASON_DEFINE,
        DIRECTOR,//REASON_BREAKDOWN,
        DEVELOPER,//REASON_RESOLVE,
        DEVELOPER,//REASON_IMPLEMENT,
        EXPERT,//REASON_DATA,
        DEVELOPER,//REASON_FIX,
        DEVELOPER,//REASON,
        DIRECTOR,//DEBUG_ANALYSIS
        EXPERT//DEBUG_ASSISTANT
        //COUNT
    };
    
    return s_intents[intent];
}

void Client::selectLLM(InferenceIntent intent)
{
    if(m_llmLock) return;
    setLLM(getLLMIntent(intent));
}

void Client::setStepHint(const std::string& stepHint)
{
    m_stepHint = stepHint;
}

void Client::setStepCost(std::shared_ptr<LLMConfig> usedLLM, float lastStep, float consumed, uint32_t limit)
{
    m_creditsState.m_lastStep = lastStep;
    if(limit == 0) {
        m_creditsState.m_limit = 100;
    }
    else {
        m_creditsState.m_limit = limit;
    }
    
    if(consumed < 0.0000001f) {
        m_creditsState.m_consumed += lastStep;
    }
    else {
        m_creditsState.m_consumed = consumed;
    }
    
    std::stringstream ss;
    ss << std::fixed << std::setprecision(4) << m_creditsState.m_lastStep << "/";
    ss << std::fixed << std::setprecision(2) << m_creditsState.m_consumed << "/";
    ss << m_creditsState.m_limit;
    
    m_creditsHint = ss.str();
}

void Client::agentToServer(const std::string& message)
{
    if(!m_localUser && m_clientEP && m_clientEP->session() && m_clientEP->session()->isConnected())
    {
        m_clientEP->session()->send((void*)message.c_str(), (uint32_t)message.size()+1);
    }
}

void Client::sendStepLog()
{
    std::string stepMessage;
    if(m_stepHint == "[[end]]")
    {
        stepMessage = m_stepHint;
    }
    else
    {
        stepMessage = "Step " + std::to_string(m_requestId) + " (credit: " + m_creditsHint + "): " + m_stepHint + "\n";
    }
    
    set_cout_enabled(true);
    std::cout << stepMessage << std::flush;
    set_cout_enabled(false);
    
    agentToServer(stepMessage);
}

bool Client::update()
{
    processUserInput();
    return true;
}

}
