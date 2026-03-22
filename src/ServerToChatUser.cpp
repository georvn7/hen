#include <iomanip>

#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <cpprest/json.h>  // For JSON functionality
#include <cpprest/http_listener.h>
#include <cpprest/producerconsumerstream.h>

#include "Server.h"
#include "Utils.h"

using namespace utility;                    // Common utilities like string conversions
using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features
using namespace concurrency::streams;       // Asynchronous streams
using namespace http::experimental::listener;

// Loads a server certificate into an ssl::context
void load_server_certificate(boost::asio::ssl::context& ctx, const std::string& directory)
{
    ctx.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::single_dh_use);

    // Example filenames:
    const std::string certFile = directory + "/fullchain.pem";
    const std::string keyFile  = directory + "/privkey.pem";

    boost::system::error_code ec;
    ctx.use_certificate_chain_file(certFile, ec);
    if(ec)
    {
        throw std::runtime_error("Error loading certificate chain file: " + ec.message());
    }

    ctx.use_private_key_file(keyFile, boost::asio::ssl::context::pem, ec);
    if(ec)
    {
        throw std::runtime_error("Error loading private key file: " + ec.message());
    }
}

namespace hen {

bool Server::initChatListener()
{
    if(m_serverPort == 0)
    {
        return false;
    }
    
    if(m_chatProxyIP.empty())
    {
        std::cout << "Empty string for chat proxy IP" << std::endl;
        return false;
    }
    
    try
    {
        // The IP address/port to listen on
        // Binding to port 443 typically requires root privileges
        auto hostInfo = getHostAndPort(m_chatProxyIP);
        boost::asio::ip::address address = boost::asio::ip::make_address(hostInfo.first);
        unsigned short port = std::stoul(hostInfo.second);

        // I/O context
        auto ioc = hen::getAsioContext();

        // Create an SSL context
        m_sslContext = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);

        // Load your certificate
        load_server_certificate(*m_sslContext, m_environmentDirectory + "/Certificates");

        // Create and launch a listener
        m_chatListener = std::make_shared<SSLListener>(*ioc, *m_sslContext, boost::asio::ip::tcp::endpoint{address, port});
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return false;
    }
    
    m_serverEP = std::make_unique<AgentServerEP>(m_serverPort);
    
    return true;
}

static std::string extractProjectUUID(const std::string& input) {
    // Regex pattern to match a UUID
    std::regex pattern(R"(Initializing agent with projectId:([a-fA-F0-9-]{36}))");
    std::smatch match;

    if (std::regex_search(input, match, pattern) && match.size() > 1) {
        return match[1].str(); // Extract the UUID
    }
    return ""; // Return an empty string if no match is found
}

std::string Server::getProjectId(const std::string& chat, json::value& requestFromClientBody)
{
    if(requestFromClientBody.has_field(U("sessionId")))
    {
        string_t sessionIdStrT = requestFromClientBody[U("sessionId")].as_string();
        std::string sessionId = utility::conversions::to_utf8string(sessionIdStrT);
        return sessionId;
    }
    else
    {
        return extractProjectUUID(chat);
    }
}

std::string Server::printLLMSpecs()
{
    std::string llmSpecs;
    llmSpecs = "Available Large Language Models:\n\n";

    for (auto llm : m_llms.llms)
    {
        // Format the prices with two decimal places
        std::ostringstream inputPriceStream, outputPriceStream;
        inputPriceStream << std::fixed << std::setprecision(2) << llm->input_tokens_price;
        outputPriceStream << std::fixed << std::setprecision(2) << llm->output_tokens_price;

        // Build the LLM specs string
        llmSpecs += llm->provider + "/" + llm->model + "\n";
        llmSpecs += "Credits per 1 million input tokens: " + inputPriceStream.str() + "\n";
        llmSpecs += "Credits per 1 million output tokens: " + outputPriceStream.str() + "\n";
        llmSpecs += "Roles: " + llm->roles + "\n\n";
    }

    return llmSpecs;
}

std::pair<std::string, std::string> Server::getLLMForTheRole(json::value& requestFromClientBody, LLMRole role, const std::string& llmRole)
{
    if(requestFromClientBody.has_field(U(llmRole)))
    {
        auto llmU = requestFromClientBody[U(llmRole)].as_string();
        auto llmStr = utility::conversions::to_utf8string(llmU);
        
        auto llmCfg = splitByFirstOccurence(llmStr, '/');
        std::shared_ptr<LLMConfig> llm = findLLM(llmCfg.first, llmCfg.second);
        if(llm && llm->takesOnRole(role))
        {
            return llmCfg;
        }
    }
    
    //Default llm for the role
    switch(role)
    {
        case LLMRole::DEVELOPER:
            return m_llmDeveloper;
        case LLMRole::EXPERT:
            return m_llmExpert;
        case LLMRole::DIRECTOR:
            return m_llmDirector;
        case LLMRole::DEBUGGER:
            return m_llmDebugger;
        default:
            //TODO: We must not be here!!!
            //assert(role < LLMRole::LLM_ROLE_COUNT);
            return m_llmExpert;
    };
}

std::map<std::string, std::string> Server::getLLMParty(json::value& requestFromClientBody)
{
        
    std::map<std::string, std::string> llmParty;
    auto llmDir = getLLMForTheRole(requestFromClientBody, LLMRole::DIRECTOR, "llmDir");
    llmParty["llmDir"] = llmDir.first + "/" + llmDir.second;
    
    auto llmExp = getLLMForTheRole(requestFromClientBody, LLMRole::EXPERT, "llmExp");
    llmParty["llmExp"] = llmExp.first + "/" + llmExp.second;
    
    auto llmDev = getLLMForTheRole(requestFromClientBody, LLMRole::DEVELOPER, "llmDev");
    llmParty["llmDev"] = llmDev.first + "/" + llmDev.second;
    
    return llmParty;
}

std::string Server::setupContextForAgent(const std::string& context)
{
    auto foundProjectId = extractProjectUUID(context);

    std::string projectId = generateUUID();
    
    if(!boost_fs::exists(m_rootDirectory))
    {
        boost_fs::create_directory(m_rootDirectory);
    }
    
    std::string projectPath = m_rootDirectory;
    projectPath += "/" + projectId;
    if(!boost_fs::exists(projectPath))
    {
        if(foundProjectId.empty())
        {
            return 0;
        }
        
        boost_fs::create_directory(projectPath);
    }
    
    //Save the context
    std::string contextFilePath = projectPath + "/Context.txt";
    if(!boost_fs::exists(contextFilePath))
    {
        if(foundProjectId.empty())
        {
            std::ofstream contextFile(contextFilePath);
            if (!contextFile.is_open()) {
                std::cerr << "Failed to open " << contextFilePath << " for writing." << std::endl;
                return;
            }
            contextFile << context;
            contextFile.close();
        }
        else
        {
            std::string projectStarterStr = "Initializing agent with projectId:";
            size_t pos = context.find(foundProjectId);
            if(pos != std::string::npos)
            {
                
            }
        }
    }
    else
    {
        //TODO: compare projectId or check the new context contain the old one
    }
    
    return projectId;
}

std::string Server::setupDescriptionForAgent(const std::string& description)
{
    std::string projectId = generateUUID();
    
    if(!boost_fs::exists(m_rootDirectory))
    {
        boost_fs::create_directory(m_rootDirectory);
    }
    
    std::string projectPath = m_rootDirectory;
    projectPath += "/" + projectId;
    if(!boost_fs::exists(projectPath))
    {
        boost_fs::create_directory(projectPath);
    }
    
    //Save the context
    std::string descFilePath = projectPath + "/Description.txt";
    if(!boost_fs::exists(descFilePath))
    {
        std::ofstream descFile(descFilePath);
        if (!descFile.is_open()) {
            std::cerr << "Failed to open " << descFilePath << " for writing." << std::endl;
            return;
        }
        descFile << description;
        descFile.close();
    }
    
    return projectId;
}

void Server::setupAgentEnvironment(const std::string& apiKey, const std::string& projectId, std::map<std::string, std::string>& llmParty)
{
    std::string projectPath = m_rootDirectory;
    projectPath += "/" + projectId;
    
    boost_fs::path agentPath = getExePath();
    std::string workingDir = agentPath.parent_path().string();
    
    std::string llmDir = "-llmDir " + llmParty["llmDir"];
    std::string llmExp = "-llmExp " + llmParty["llmExp"];
    std::string llmDev = "-llmDev " + llmParty["llmDev"];
    
    //TODO: The addres of the LLM proxy should come from environment
    std::string llmpx = "-llmpx \"" + m_llmProxyIP + "\"";
    //std::string llmpx = "-llmpx \"localhost:8081\"";
    
    std::string sip = "-sip ";
    sip += getLANIP();
    sip += ":";
    sip += std::to_string(m_serverPort);
    
    //Project ID
    std::string pid = "-pid ";
    pid += projectId;
    
    std::string cmd = agentPath.string() + " -client -proj " + projectPath + " " + pid + " -env " + m_environmentDirectory;
    cmd += " " + sip + " " + llmpx + " " + llmDir + " " + llmExp + " " + llmDev;
    
    std::string logDirectory = projectPath + "/Logs";
    if(!boost_fs::exists(logDirectory))
    {
        boost_fs::create_directory(logDirectory);
    }
    
    {
        std::lock_guard<std::mutex> lock(m_agentsMutex);
        auto agentIt = m_activeAgents.find(projectId);
        if(agentIt == m_activeAgents.end())
        {
            std::cout << "Starting new agent: " << cmd << std::endl;
            
            auto process = spawn(cmd, workingDir, logDirectory, "Agent", false);
            
            m_activeAgents[projectId] = std::make_shared<AgentAtService>(projectId, std::move(process), apiKey);
        }
    }
}

std::shared_ptr<RemoteEP> agentConnectionProgress(std::shared_ptr<HTTPSession> session, const std::string& message, std::string sessionId,
                                                  //std::shared_ptr<Concurrency::streams::producer_consumer_buffer<uint8_t>> streambuf,
                                                  std::unique_ptr<AgentServerEP>& server, float& waitStepMultiplier)
{
    auto timeStamp = time(nullptr);
    std::cout << "CONNECTION PROGRESS " << timeStamp << ": " << message << std::flush;
    
    std::shared_ptr<RemoteEP> agentSession =  server->getConnection(sessionId);
    if(!agentSession) {
        waitStepMultiplier = std::max(1.f, waitStepMultiplier*1.2f);
    }
    else {
        waitStepMultiplier = std::min(0.2f, waitStepMultiplier*0.8f);
    }
    
    uint32_t ms = 1000*waitStepMultiplier;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    
    session->sendResponse(true, message);
    
    return agentSession;
}

std::string Server::printUserManual()
{
    std::string manPath = m_environmentDirectory + "/UserManual.txt";
    
    std::ifstream manFile(manPath);
    std::string manStr((std::istreambuf_iterator<char>(manFile)), std::istreambuf_iterator<char>());
    
    std::string llmSpecs = printLLMSpecs();
    std::string userManual = buildPrompt(manStr, {{"llms", llmSpecs}});

    return userManual;
}

void Server::startAgentRequest(std::shared_ptr<HTTPSession> session, const std::string& apiKey, json::value& requestFromClientBody)
{
    auto startTime = time(nullptr);
    std::cout << "START STREAMING AT: " << startTime << std::endl;
    
    auto llmParty = getLLMParty(requestFromClientBody);
    
    std::string projectId;
    if(requestFromClientBody.has_field("projectId"))
    {
        auto projectIdU = requestFromClientBody["projectId"].as_string();
        projectId = utility::conversions::to_utf8string(projectIdU);
    }
    else if(requestFromClientBody.has_field(U("description")))
    {
        string_t descriptionU = requestFromClientBody[U("description")].serialize();
        std::string description = utility::conversions::to_utf8string(descriptionU);
        projectId = setupDescriptionForAgent(description);
    }
    else if(requestFromClientBody.has_field(U("conversationHistory")))
    {
        string_t chatU = requestFromClientBody[U("conversationHistory")].serialize();
        std::string chat = utility::conversions::to_utf8string(chatU);
        projectId = setupContextForAgent(chat);
    }
    
    setupAgentEnvironment(apiKey, projectId, llmParty);
    
    bool streamResponse = false;
    if(requestFromClientBody.has_field(U("stream")))
    {
        streamResponse = requestFromClientBody[U("stream")].as_bool();
    }
            
    startAgentUdate(session, llmParty, projectId, streamResponse);
}

void Server::updateAgentRequest(http_request& requestFromClient, json::value& requestFromClientBody)
{
    std::string chat;
    if(requestFromClientBody.has_field(U("conversationHistory")))
    {
        string_t uchat = requestFromClientBody[U("conversationHistory")].serialize();
        chat = utility::conversions::to_utf8string(uchat);
    }
    
    std::cout << chat;
    std::string projectId = getProjectId(chat, requestFromClientBody);
    
    http_response response(status_codes::OK);
    response.headers().add(U("Content-Type"), U("application/json"));

    if(!projectId.empty())
    {
        std::lock_guard<std::mutex> lock(m_agentsMutex);
        std::string fullResponse = "{\"message\":\"" + m_activeAgents[projectId]->m_log + "\"}";
        m_activeAgents[projectId]->m_history += m_activeAgents[projectId]->m_log;
        response.set_body(fullResponse);
    }
    else
    {
        std::string fullResponse = "{\"message\":\"Couldn't find projectId in the conversation history\"}";
        response.set_body(fullResponse);
    }
    
    requestFromClient.reply(response);
}

void Server::startAgentUdate(std::shared_ptr<HTTPSession> session, std::map<std::string, std::string> llmParty, const std::string projectId, bool stream)
{
    std::shared_ptr<AgentAtService> agent = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_agentsMutex);
        agent = m_activeAgents[projectId];
    }
    
    if(!agent)
    {
        //TODO: Something is wrong here! Signal error
        return;
    }
    
    agent->m_commsThread = std::make_unique<std::thread>( [this, agent, projectId, llmParty, session, stream](){
        
        float waitStepMultiplier = 0.5f;
        
        std::string projecMsg = "\n\n" + Peer::getHeader();
        projecMsg += "\n" + Peer::getRuntimeNotice();
        
        projecMsg += "\n\n******************************************************************************\n";
        projecMsg += "*** Project Unique Identifier (PUID): " + projectId + "\n";
        projecMsg += "*** Copy this PUID to reference the project later !!!\n";
        projecMsg += "******************************************************************************\n\n";
        
        auto agentSession = agentConnectionProgress(session, projecMsg,
                                                    projectId, m_serverEP, waitStepMultiplier);
        
        std::string llmPartyMsg = "Assembling LLM party";
        llmPartyMsg += "\nDirector:   ";
        llmPartyMsg += llmParty.at("llmDir");
        llmPartyMsg += "\nExpert:     ";
        llmPartyMsg += llmParty.at("llmExp");
        llmPartyMsg += "\nDeveloper:  " + llmParty.at("llmDev") + "\n\n";
        
        agentSession = agentConnectionProgress(session, llmPartyMsg,
                                                    projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Inferencing engine: Initialized!\n",
                                                    projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Reasoning core: Initialized!\n",
                                               projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Logic decomposers: Initialized!\n",
                                               projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Trajectory analyzers: Initialized!\n",
                                               projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Logic implementers: Initialized!\n",
                                               projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Develompent environment embodiment: Initialized!\n",
                                               projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "Agentic workflow: Initialized!\n",
                                               projectId, m_serverEP, waitStepMultiplier);
        
        agentSession = agentConnectionProgress(session, "The agent is ready! Prepare for take off...\n\n",
                                               projectId, m_serverEP, waitStepMultiplier);
    
        if(agentSession)
        {
            std::cout << "Agent session for projectId: " << projectId << " is live" << std::endl;
            
            std::string agentCommand = "start";
            agentSession->send((void*)agentCommand.c_str(), (uint32_t)agentCommand.length()+1);
            
            std::string messageStr;
            while(messageStr != "[[end]]")
            {
                std::shared_ptr<RemoteEP> agentSession =  m_serverEP->getConnection(projectId);
                if(agentSession)
                {
                    if(agentSession->hasDataToRead())
                    {
                        std::shared_ptr<Message> message = agentSession->receive();
                        messageStr = message->c_str();
                        agent->m_log += messageStr;
                    }
                    
                    //TODO: Fetch message from the log
                    messageStr = agent->popNextMessage();
                    
                    //if(message && message->c_str() != nullptr)
                    if(messageStr.length() > 0)
                    {
                        std::cout << "LOGGING: " << messageStr;
                        
                        if(stream)
                        {
                            if(messageStr == "[[end]]")
                            {
                                //End streaming and chunk
                                session->sendResponse(false, "Bye!\n");
                            }
                            else
                            {
                                session->sendResponse(true, messageStr);
                            }
                        }
                    }
                }
            }
        }
    });
}

void Server::listenForChatUsers()
{
    m_chatListener->listen(boost::beast::http::verb::post,
                           [this](std::shared_ptr<HTTPSession> session,
                                  const std::string& apiKey,
                                  const std::string& path,
                                  const std::string& jsonOrText) {
        
        if(path.find(std::string("/") + PRODUCT_NAME) != std::string::npos)
        {
            std::string endpoint = getLastAfter(path, "/");
            
            if(endpoint == "man")
            {
                std::string response = printUserManual();
                session->sendResponse(false, response);
            }
            else
            {
                std::string apiKeyErrors = isAPIKeyValid(m_db, apiKey);
                if(!apiKeyErrors.empty())
                {
                    // If JSON parse fails or something else
                    std::cerr << apiKeyErrors<< std::endl;
                    session->send_bad_request(apiKeyErrors);
                    return false;
                }
                
                web::json::value request;
                try {
                    // Convert std::string to wide string for Casablanca
                    utility::string_t wbody = utility::conversions::to_string_t(jsonOrText);
                    request = web::json::value::parse(wbody);
                }
                catch(std::exception& e)
                {
                    // If JSON parse fails or something else
                    std::cerr << "JSON parse error: " << e.what() << std::endl;
                    session->send_bad_request("Invalid JSON");
                }
                
                if(endpoint == "start")
                {
                    startAgentRequest(session, apiKey, request);
                }
                else if(endpoint == "stop")
                {
                    
                }
                else if(endpoint == "update")
                {
                    //updateAgentRequest(chat, requestFromClient, request);
                }
                else if(endpoint == "continue")
                {
                    
                }
                else
                {
                    session->handle_404_not_found();
                    return false;
                }
            }
            
            return true;
        }
        else
        {
            session->handle_404_not_found();
            return false;
        }
    });

    try {
        m_chatListener->run();
        std::cout << "Server listening on https://" << m_chatProxyIP << std::endl;
    }
    catch (const std::exception& e) {
        printf("error 3\n");
        std::wcerr << e.what() << std::endl;
    }
}

}
