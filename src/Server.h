#pragma once


#include <string>
#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <cpprest/json.h>  // For JSON functionality
#include <cpprest/http_listener.h>
#include <sqlite3.h>

#include "Peer.h"
#include "Singleton.h"
#include "ClientServer.h"
#include "ServerBeastSupport.h"
#include "CreditSystem.h"

#define USE_BOOST_BEAST

using namespace utility;                    // Common utilities like string conversions
using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features
using namespace concurrency::streams;       // Asynchronous streams
using namespace http::experimental::listener;

namespace stdrave {

    class Server;

    class AgentServerEP : ServerEP
    {
        std::map<std::string, std::shared_ptr<RemoteEP>> m_agentEPs;
    public:
        bool receive(std::shared_ptr<RemoteEP> remote, std::shared_ptr<Message> msg);
        std::shared_ptr<RemoteEP> getConnection(const std::string& sessionId);
        AgentServerEP(unsigned short port):ServerEP(port) {}
    };

    class LLMServerEP : ServerEP
    {
        Server* m_appServer;
    public:
        bool receive(std::shared_ptr<RemoteEP> remote, std::shared_ptr<Message> msg);
        LLMServerEP(unsigned short port, Server* appServer):ServerEP(port),m_appServer(appServer) {}
    };

    class AgentAtService
    {
    public:
        std::string m_projectId;
        std::unique_ptr<boost_prc::process> m_process;  // Changed from child to process
        std::string m_ipAddress;
        std::unique_ptr<std::thread> m_commsThread;
        std::string m_log;
        std::string m_history;
        
        // v2 requires an io_context - store it here
        boost::asio::io_context m_ioContext;
        
        //Update this key with each request!
        std::string m_apiKey;
        
        AgentAtService(const std::string& projectId,
                       std::unique_ptr<boost_prc::process> process,  // Updated parameter type
                       const std::string& apiKey);
        bool connected();
        std::string popNextMessage();
        std::string popNextLine();
        std::string popNextWord();
    };

	class Server : public Peer, public Singleton<Server>
	{
#ifdef USE_BOOST_BEAST
        std::shared_ptr<SSLListener> m_chatListener;
        std::shared_ptr<boost::asio::ssl::context> m_sslContext;
#else
        //Between server and the chat users
        std::unique_ptr<http_listener> m_chatListener;
#endif
        
        unsigned short              m_serverPort;
        std::unique_ptr<AgentServerEP>   m_serverEP;
        
        std::mutex m_agentsMutex;
        std::map<std::string, std::shared_ptr<AgentAtService>> m_activeAgents;
        
        std::string m_rootDirectory;
        
        std::unique_ptr<LLMServerEP> m_llmServerEP;
        
		bool m_run = true;
        utility::string_t m_a3Token;
        utility::string_t m_acack;
        
        std::string                         m_keytabFile;
        std::string                         m_acUser;
        std::map<std::string, std::string>  m_keys;
        
        uint32_t m_agentsPortRange[2];
        
        sqlite3* m_db;
        
        std::chrono::time_point<std::chrono::high_resolution_clock> m_lasACTime;
        
        bool initLLMProxyListener();
        bool initChatListener();
        
        std::pair<std::string, std::string> getLLMForTheRole(json::value& requestFromClientBody, LLMRole role, const std::string& llmRole);
        std::map<std::string, std::string> getLLMParty(json::value& requestFromClientBody);
        std::string printLLMSpecs();
        
        
        std::string setupContextForAgent(const std::string& context);
        std::string setupDescriptionForAgent(const std::string& description);
        
        void setupAgentEnvironment(const std::string& apiKey, const std::string& projectId, std::map<std::string, std::string>& llmParty);
        
        std::string prepareBody(json::value& requestFromClientBody, std::shared_ptr<LLMConfig>& llm);
        http_request prepareCpprestsdkRequest(json::value& requestFromClientBody, std::shared_ptr<LLMConfig>& llm);
        json::value handleResponse(json::value& json_response, std::shared_ptr<LLMConfig> llm, const std::string& projectId);
        
        std::string doBeastRequest(const boost_bst::http::request<boost_bst::http::string_body>& beastRequest, std::shared_ptr<LLMConfig> llm);
        boost_bst::http::request<boost_bst::http::string_body> prepareBeastRequest(
        web::json::value& requestFromClientBody, std::shared_ptr<LLMConfig>& llm, std::string& projectId);
        
        web::json::value updateUsage(web::json::value& usageField, std::shared_ptr<LLMConfig> llm, const std::string& projectId);
        web::json::value convertToGoogle(const web::json::value& openAIRequestBody);
        
        friend class LLMServerEP;
        friend class AsyncLLMSession;
        
	public:
		Server();
		int init(int argc, char* argv[]) final;
        std::string printUserManual();
        void startAgentRequest(std::shared_ptr<HTTPSession> session, const std::string& apiKey, json::value& requestFromClientBody);
        void startAgentUdate(std::shared_ptr<HTTPSession> session, std::map<std::string, std::string> llmParty, const std::string projectId, bool stream);
        void updateAgentRequest(/*const std::string& chat,*/ http_request& requestFromClient, json::value& requestFromClientBody);
        std::string getProjectId(const std::string& chat, json::value& requestFromClientBody);
        void listenForChatUsers();
        
        void shutdown();
		virtual bool update() final;
        
        bool startDatabse();
        bool shutdownDatabase();
	};

}
