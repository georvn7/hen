#include <cpprest/http_listener.h>
#include <cpprest/producerconsumerstream.h>

#include "Peer.h"
#include "Utils.h"
#include "Client.h"

using namespace utility;
using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

namespace stdrave {

void Client::connectToServer()
{
    if(m_serverIP.empty()) return;
    
    m_clientEP = std::make_unique<ClientEP>(0, m_serverIP);
    if(m_clientEP)
    {
        m_clientEP->session()->send((void*)m_projectId.c_str(), (uint32_t)m_projectId.length()+1);
        std::cout << "Sending projectId to the server: " << m_project << std::endl;
    }
}

}
