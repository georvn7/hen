#pragma once

#include <iostream>
#include <unordered_map>
#include <string>
#include <memory>
#include <queue>
#include <utility>
#include <span>
#include <mutex>

#include <cpprest/json.h>

#include "IncludeBoost.h"

using namespace boost::asio;
using ip::tcp;
using namespace utility;
using namespace web;

namespace hen {

    std::shared_ptr<boost::asio::io_context> getAsioContext();

    void shutdownAsioContext();

    class EndPoint;

    class Message {
    private:
        std::vector<uint8_t> m_storage;
        friend class RemoteEP;
        friend class ServerEP;
    public:
        uint32_t m_size;
        void resize() { m_storage.resize(m_size); }
        
        size_t size() const { return m_storage.size(); }
        
        const char* c_str() const {return (const char*)m_storage.data();}
        
        template<typename T>
        boost::optional<T> as() const {
            if(sizeof(T) <= size() && m_storage.data() != nullptr) {
                // Check alignment
                //if(reinterpret_cast<std::uintptr_t>(m_storage.data()) % alignof(T) == 0)
                {
                    return *reinterpret_cast<const T*>(m_storage.data());
                }
            }
            return boost::none;
        }
        
        json::value json() const;
    };

    class RemoteEP : public std::enable_shared_from_this<RemoteEP>
    {
        EndPoint& m_localEP;
        tcp::socket m_socket;
        unsigned short m_cachedRemotePort;
        std::mutex m_sendMutex;
        
        void startAsyncRead(std::promise<std::shared_ptr<Message>> promise);
        
    public:
        RemoteEP(tcp::socket socket, EndPoint& localEP);
        std::shared_ptr<Message> receive();
        bool hasDataToRead();
        void send(void* data, uint32_t size);
        tcp::socket& socket() {return m_socket;}
        unsigned short remotePort();
        bool isConnected();
    };

    class EndPoint
    {
    protected:
    
        tcp::endpoint m_endpoint;
    public:
        virtual void disconnected(std::shared_ptr<RemoteEP> session) {}
        EndPoint(unsigned short port);
    };

    //Server endpoint receiver function is called each time message is received
    class ServerEP : public EndPoint
    {
    private:
        tcp::acceptor m_acceptor;
        
        std::mutex m_sessionsMutex;
        std::set<std::shared_ptr<RemoteEP>> m_sessions;
        
        void startAccept();
        void handleAccept(std::shared_ptr<RemoteEP> session, const boost::system::error_code& error);
        
        void startAsyncRead(std::shared_ptr<RemoteEP> session);
        void handleRead(const boost::system::error_code& ec, std::size_t bytes_transferred, std::shared_ptr<RemoteEP> session, std::shared_ptr<Message> msg);
        
    public:
        
        virtual void accept(std::shared_ptr<RemoteEP> remote);
        virtual bool receive(std::shared_ptr<RemoteEP> remote, std::shared_ptr<Message> msg);
        ServerEP(unsigned short port);
        void disconnected(std::shared_ptr<RemoteEP> session);
    };

    //Client endpoint does serial send/receive only
    class ClientEP : public EndPoint
    {
        unsigned short m_localPort;
        std::string m_serverAddress;
        std::shared_ptr<RemoteEP> m_session;
        std::mutex m_sessionMutex;

        std::shared_ptr<RemoteEP> connectSession();
    public:
        ClientEP(unsigned short port, const std::string serverIP);

        std::shared_ptr<RemoteEP> session();
        void reconnect();
        void disconnected(std::shared_ptr<RemoteEP> session) override;
    };

    std::pair<std::string, std::string> getHostAndPort(const std::string& address);
}
