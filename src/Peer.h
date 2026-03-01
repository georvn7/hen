#pragma once

#include <string>
#include <cpprest/http_client.h>
#include <cpprest/json.h>

#include "LLMConfig.h"

using namespace web;
using namespace web::http;
using namespace web::http::client;

#define MERCH_PRODUCT_NAME "hen"
#define PRODUCT_NAME "hen"
#define PRODUCT_FILENAME_EX "hen"
#define AUTHOR_NAME "George Raven"
#define AUTHOR_CONTACTS "georaven7@gmail.com"

namespace hen {

	class Peer
	{
    protected:
        std::string m_environmentDirectory;
        LLMRegistry m_llms;
        
        std::pair<std::string, std::string> m_llmDirector;
        std::pair<std::string, std::string> m_llmExpert;
        std::pair<std::string, std::string> m_llmDeveloper;
        std::pair<std::string, std::string> m_llmDebugger;
        
        std::string m_llmProxyIP;
        std::string m_chatProxyIP;

        void sendRequest(http_client& client, const json::value& request, json::value& response);
	public:
		Peer();
        
        std::shared_ptr<LLMConfig> findLLM(const std::string& provider, const std::string& model);
        
		virtual int init(int argc, char* argv[]);
        virtual void shutdown() {}
		virtual bool update() { return false; }
		const std::string& getEnvironmentDir() const {return m_environmentDirectory;}
        static std::string getHeader();
        static std::string getDisclamer();
        static std::string getProductDescription();
	};
}
