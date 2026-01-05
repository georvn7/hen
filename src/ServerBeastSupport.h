#pragma once

#include <cpprest/json.h>

#include "IncludeBoost.h"

class SSLListener;

// Forward declare
void load_server_certificate(boost::asio::ssl::context& ctx, const std::string& directory);

// A simple HTTPS session that reads a single request, processes it, and writes a response.
class HTTPSession : public std::enable_shared_from_this<HTTPSession>
{
public:
    HTTPSession(std::shared_ptr<SSLListener> listener, boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context& ctx);

    // Start the async handshake
    void run();
    
    void sendResponse(bool chunked, const std::string& response);
    void handle_404_not_found();
    void send_bad_request(const std::string& message);
    
private:
    
    void sendChunk(const std::string& data);
    void sendLastChunk(const std::string& data);
    
    // Perform an asynchronous read to get a request
    void do_read();

    // Here we parse the request and create a response
    void handle_request();
    
    void do_write();
    void queue_write(const std::string& data);

private:
    // Boost.Beast SSL stream
    boost::beast::ssl_stream<boost::beast::tcp_stream> stream_;
    // Buffer for reading
    boost::beast::flat_buffer buffer_;
    // Request object
    boost::beast::http::request<boost::beast::http::string_body> req_;
    boost::beast::http::response<boost::beast::http::string_body> response_;
    std::shared_ptr<SSLListener> listener_;
    bool streaming_;
    
    std::mutex write_mutex_;
    std::queue<std::string> write_queue_;
    bool write_in_progress_ = false;
};

// Accepts incoming connections and launches sessions
class SSLListener : public std::enable_shared_from_this<SSLListener>
{
public:
    SSLListener(boost::asio::io_context& ioc,
                boost::asio::ssl::context& ctx,
                boost::asio::ip::tcp::endpoint endpoint);

    void run();
    
    void listen(boost::beast::http::verb apiType,
                const std::function<bool(std::shared_ptr<HTTPSession>, const std::string&, const std::string&, const std::string&)>& onRequest);

private:
    
    friend class HTTPSession;
    
    void do_accept();
    void respondOnRequest(std::shared_ptr<HTTPSession> session,
                          boost::beast::http::verb apiType,
                          const std::string& apiKey,
                          const std::string& path,
                          const std::string& jsonOrText);

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    
    std::map<boost::beast::http::verb, std::function<bool(std::shared_ptr<HTTPSession>, const std::string&, const std::string&, const std::string&)>> responders_;
};

