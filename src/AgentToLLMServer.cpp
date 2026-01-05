#include "Client.h"
#include "Utils.h"

using namespace utility;
using namespace web;
using namespace http;

namespace stdrave {

void Client::sendToServer(const json::value& requestBody, json::value& responseBody)
{
    auto requestStr = utility::conversions::to_utf8string(requestBody.serialize());
    
    m_llmClientEP->session()->send((void*)requestStr.c_str(), (uint32_t)requestStr.size()+1);
    
    auto msg = m_llmClientEP->session()->receive();
    if(msg)
    {
        responseBody = msg->json();
    }
}

void Client::initClient(const std::string& serverIP)
{
    m_llmClientEP = std::make_unique<ClientEP>(0, serverIP);
}

}
