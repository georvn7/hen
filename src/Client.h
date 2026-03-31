#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <cpprest/http_listener.h>

#include "Peer.h"
#include "Singleton.h"
#include "Project.h"
#include "ClientServer.h"
#include "CreditSystem.h"

#include "IncludeBoost.h"

using namespace utility;                    // Common utilities like string conversions
using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features
using namespace concurrency::streams;       // Asynchronous streams
using namespace http::experimental::listener;

namespace hen {

    enum InferenceIntent
    {
        DEFINE = 0,
        BREAKDOWN_HI,
        BREAKDOWN_LO,
        SEARCH_LIB,
        RESOLVE,
        DATA,
        IMPLEMENT_OPTIMISTIC ,
        IMPLEMENT,
        FIX_OPTIMISTIC,
        FIX,
        FIX_PANIC,
        REASON_DEFINE,
        REASON_BREAKDOWN,
        REASON_RESOLVE,
        REASON_IMPLEMENT,
        REASON_DATA,
        REASON_FIX,
        REASON,
        DEBUG_ANALYSIS,
        DEBUG_ASSISTANT,
        WRITE_TESTS,
        INFERENCE_INTENT_COUNT
    };

	class Client : public Peer, public Singleton<Client>
	{
        int initProject(const web::json::value& jsonProj);
		int initProject();

        void logRequest(const json::value& jsonObject, const std::string& fileName);
        void logResponse(const json::value& jsonObject, const std::string& fileName);
        void logChat(const json::value& request, const json::value& response, const std::string& fileName);
        
		void log(const json::value& jsonObject, const std::string& fileName);
        
		uint32_t m_requestId;
		std::string m_conversation;
        
		Project* m_project;
		bool m_auto;
        bool m_supportsFunctionCalls;

		std::map<std::string, boost_opt::options_description> m_commands;
        
        LLMRole m_currentLLM;
        bool m_llmLock;
        
        std::string m_ctxLogDir;
        std::string m_ctxPrompt;
        std::string m_ctxLLMRequested;
        std::string m_ctxLLMUsed;
        LLMRole m_ctxLLMRoleUsed;
        
        std::map<std::string, std::string>  m_log;
        bool                                m_logEnabled;
        std::string m_projectDirectory;
        
        std::string m_stepHint;
        
        std::string                         m_serverIP;
        std::unique_ptr<ClientEP>           m_clientEP;
        std::string                         m_projectId;
        std::string                         m_clientInstanceId;
        bool                                m_disableUserCommands;
        
        std::unique_ptr<ClientEP>           m_llmClientEP;
        std::mutex                          m_llmRequestMutex;
        bool                                m_debugLoggingNoticeShown = false;
        
        bool m_localUser;
        
        AccountState                        m_creditsState;
        std::string                         m_creditsHint;
        
        uint16_t                            m_debugPort;
        
        void checkLLMContextSize(const json::value& messages, json::value& request);
        void maybePrintDebugLoggingNotice(const std::string& logDir);
        void sendToServer(const json::value& request, json::value& response);
        
        void initClient(const std::string& serverIP);
        void connectToServer();
        
	public:
        Client() {
            
        }
        
        bool stop() {
            bool ret = m_auto; m_auto = false; return ret;
        }
        bool run() {
            bool ret = m_auto; m_auto = true; return ret;
        }
        
        std::string getUserInput();
        
        bool processUserInput();
		bool sendRequest(const json::value& messages, json::value& response, const json::value* schemas = nullptr);
		int init(int argc, char* argv[]) final;
        void shutdown() final;
		virtual bool update() final;
        void registerCommand(const std::string& command, boost_opt::options_description cliOptions) {
			m_commands[command].add(cliOptions);
		}
        bool supportsFunctionCalls() const {return m_supportsFunctionCalls;}
        
        void useDirectorLLM();
        void useDebuggerLLM();
        void useExpertLLM();
        void useDeveloperLLM();
        bool escalateLLM();
        void selectLLM(InferenceIntent intent);
        LLMRole getLLMIntent(InferenceIntent intent);
        void setLLM(LLMRole role);
        LLMRole getLLM() const;
        std::shared_ptr<LLMConfig> currentLLMConfig();
        std::shared_ptr<LLMConfig> getLLMConfig(LLMRole forRole);
        
        void lockLLM() { m_llmLock = true; }
        void unlockLLM() { m_llmLock = false; }
        
        void setContextLogDir(const std::string& nodeDir) {m_ctxLogDir = nodeDir;}
        void setContextPrompt(const std::string& prompt) {m_ctxPrompt = prompt;}
        uint32_t getRequestId() const {return m_requestId;}
        void setRequestId(uint32_t requestId) {m_requestId = requestId;}
        void flushLog();
        
        void setStepHint(const std::string& stepHint);
        void setStepCost(uint32_t inputTokens,
                         uint32_t cacheWriteTokens,
                         uint32_t cacheReadTokens,
                         uint32_t outputTokens,
                         float lastStep,
                         float consumed,
                         uint32_t limit);
        void sendStepLog();
        void agentToServer(const std::string& message);
        
        const std::string& getProjectId() const { return m_projectId; }
        const std::string& getProjectDirectory() const { return m_projectDirectory; }
        
        void disableUserCommands(bool state) {m_disableUserCommands = state;}

		Project* project() { return m_project; }
        
        void setHackyProject(Project* project) {m_project = project;}
        
        bool enableLog(bool state) { bool prev = m_logEnabled; m_logEnabled = state; return prev; }
        
        uint16_t getDebugPort() const {return m_debugPort;}
	};

}
