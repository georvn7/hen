#include "Client.h"
#include "Utils.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

using namespace utility;
using namespace web;
using namespace http;

namespace hen {

namespace {

std::string framePreview(const Message& msg, std::size_t maxBytes = 16)
{
    std::ostringstream oss;
    oss << "size=" << msg.size() << " bytes=[";
    
    const auto* bytes = reinterpret_cast<const unsigned char*>(msg.c_str());
    const std::size_t previewSize = std::min(msg.size(), maxBytes);
    for(std::size_t i = 0; i < previewSize; ++i)
    {
        if(i > 0)
        {
            oss << ' ';
        }
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(bytes[i]);
    }
    if(msg.size() > previewSize)
    {
        oss << " ...";
    }
    oss << "]";
    return oss.str();
}

}

void Client::sendToServer(const json::value& requestBody, json::value& responseBody)
{
    auto requestStr = utility::conversions::to_utf8string(requestBody.serialize());
    
    m_llmClientEP->session()->send((void*)requestStr.c_str(), (uint32_t)requestStr.size()+1);
    
    auto msg = m_llmClientEP->session()->receive();
    if(msg)
    {
        try
        {
            responseBody = msg->json();
        }
        catch(const web::json::json_exception& e)
        {
            std::cerr << "Client::sendToServer malformed JSON frame: "
                      << framePreview(*msg) << " - " << e.what() << std::endl;
            
            responseBody = json::value::object();
            responseBody[U("error")] = json::value::object();
            responseBody[U("error")][U("code")] = json::value::string(U("transport_malformed_json"));
            utility::string_t errorMessage = U("Malformed JSON response from local LLM server: ");
            errorMessage += utility::conversions::to_string_t(std::string(e.what()));
            responseBody[U("error")][U("message")] = json::value::string(errorMessage);
            
            if(requestBody.has_field(U("request_id")))
            {
                responseBody[U("request_id")] = requestBody.at(U("request_id"));
            }
        }
    }
}

void Client::initClient(const std::string& serverIP)
{
    m_llmClientEP = std::make_unique<ClientEP>(0, serverIP);
}

}
