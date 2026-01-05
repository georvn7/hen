#include "ServerBeastSupport.h"

HTTPSession::HTTPSession(std::shared_ptr<SSLListener> listener, boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context& ctx)
    : listener_(listener), stream_(std::move(socket), ctx), streaming_(false)
{
}

// Start the async handshake
void HTTPSession::run()
{
    auto self = shared_from_this();
    stream_.async_handshake(
        boost::asio::ssl::stream_base::server,
        [self](boost::system::error_code ec)
        {
            if (!ec)
            {
                self->do_read();
            }
            else
            {
                std::cerr << "Handshake error: " << ec.message() << std::endl;
            }
        });
}

void HTTPSession::do_read()
{
    auto self = shared_from_this();

    // Make sure we reset the buffer and request objects
    buffer_.consume(buffer_.size());
    req_ = {};

    boost::beast::http::async_read(
        stream_, buffer_, req_,
        [self](boost::system::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);
            if(!ec)
            {
                self->handle_request();
            }
            else
            {
                std::cerr << "Read error: " << ec.message() << std::endl;
            }
        });
}

// Extract the API key from an Authorization header of the form:
// "Authorization: Bearer gkr_some-uuid-or-other-string"
template<class Body, class Allocator>
std::string extractApiKey(const boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>& req)
{
    // Look up the 'Authorization' header
    auto it = req.find(boost::beast::http::field::authorization);
    if (it == req.end())
    {
        // No Authorization header found
        return {};
    }

    // Convert the header's value to a std::string
    std::string authValue(it->value().begin(), it->value().end());

    // We expect something like: "Bearer gkr_xxx..."
    static const std::string bearerPrefix = "Bearer ";
    if (authValue.size() < bearerPrefix.size())
    {
        // The header is too short to match "Bearer "
        return {};
    }

    // Check if the header actually starts with "Bearer "
    if (authValue.compare(0, bearerPrefix.size(), bearerPrefix) != 0)
    {
        // The header doesn't start with "Bearer "
        return {};
    }

    // Extract the substring after "Bearer "
    std::string apiKey = authValue.substr(bearerPrefix.size());
    return apiKey;
}

// Here we parse the request and create a response
void HTTPSession::handle_request()
{
    // We only handle a POST request to illustrate how to parse JSON
    if (req_.method() == boost::beast::http::verb::post)
    {
        std::cout << "req_.method(): " << req_.method_string() << "\n";
        std::cout << "req_.target(): " << req_.target() << "\n";
        std::cout << "req_.body.size(): " << req_.body().size() << "\n";
        
        // Retrieve the body as a string
        std::string body(req_.body().data(), req_.body().size());
        
        std::cout << "Received POST request: " << std::endl << body << std::endl;

        std::string apiKey = extractApiKey(req_);
        
        // Example: parse JSON using cpprestsdk
        try
        {
            std::string path(req_.target());
            
            listener_->respondOnRequest(shared_from_this(), req_.method(), apiKey, path, body);
        }
        catch(std::exception& e)
        {
            // If JSON parse fails or something else
            std::cerr << "JSON parse error: " << e.what() << std::endl;
            send_bad_request("Invalid JSON");
        }
    }
    else
    {
        // Only support POST in this minimal example
        send_bad_request("Only POST is supported in this minimal example");
    }
}

void HTTPSession::send_bad_request(const std::string& message)
{
    // Create an HTTP response
    response_ = {};
    response_.result(boost::beast::http::status::bad_request);
    response_.version(req_.version());
    response_.set(boost::beast::http::field::content_type, "text/plain");
    response_.body() = message;
    response_.prepare_payload();

    auto self = shared_from_this();
    boost::beast::http::async_write(
        stream_, response_,
        [self](boost::system::error_code ec, std::size_t)
        {
            // Gracefully close
            self->stream_.async_shutdown(
                [self](boost::system::error_code ec2)
                {
                    if(ec2)
                    {
                        std::cerr << "Shutdown error: " << ec2.message() << std::endl;
                    }
                });
        });
}

void HTTPSession::handle_404_not_found()
{
    response_ = {};
    response_.result(boost::beast::http::status::not_found);
    response_.version(req_.version());
    response_.set(boost::beast::http::field::content_type, "text/plain");
    response_.body() = "Not Found";
    response_.prepare_payload();

    auto self = shared_from_this();
    boost::beast::http::async_write(
        stream_,
        response_,
        [self](boost::system::error_code ec, std::size_t)
        {
            // Optionally shut down or read again
        }
    );
}

void HTTPSession::sendResponse(bool chunked, const std::string& response)
{
    if(chunked)
    {
        if(!streaming_)
        {
            std::stringstream headers;
            headers << "HTTP/1.1 200 OK\r\n"
                   << "Transfer-Encoding: chunked\r\n"
                   << "Content-Type: text/plain\r\n"
                   << "\r\n";
            
            streaming_ = true;
            queue_write(headers.str());
            sendChunk(response);
        }
        else
        {
            sendChunk(response);
        }
    }
    else if(streaming_)
    {
        sendLastChunk(response);
        streaming_ = false;
    }
    else
    {
        std::stringstream ss;
        ss << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/plain\r\n"
           << "Content-Length: " << response.length() << "\r\n"
           << "\r\n"
           << response;
        
        queue_write(ss.str());
    }
}

void HTTPSession::sendChunk(const std::string& data)
{
    if(data.empty()) return;
    
    std::stringstream ss;
    ss << std::hex << data.length() << "\r\n" << data << "\r\n";
    
    queue_write(ss.str());
}

void HTTPSession::sendLastChunk(const std::string& data)
{
    if(!data.empty())
    {
        sendChunk(data);
    }
    
    queue_write("0\r\n\r\n");
}

void HTTPSession::do_write()
{
    if (write_in_progress_ || write_queue_.empty()) {
        return;
    }

    write_in_progress_ = true;
    auto data = write_queue_.front();
    write_queue_.pop();

    auto self = shared_from_this();
    boost::asio::async_write(
        stream_,
        boost::asio::buffer(data),
        [this, self](boost::system::error_code ec, std::size_t)
        {
            write_in_progress_ = false;
            
            if(ec) {
                std::cerr << "Write error: " << ec.message() << std::endl;
                return;
            }

            // Process next write if any
            do_write();
        });
}

void HTTPSession::queue_write(const std::string& data)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    write_queue_.push(data);
    do_write();
}

SSLListener::SSLListener(boost::asio::io_context& ioc,
            boost::asio::ssl::context& ctx,
            boost::asio::ip::tcp::endpoint endpoint)
    : ioc_(ioc)
    , ctx_(ctx)
    , acceptor_(ioc)
{
    boost::system::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if(ec) { throw std::runtime_error("open: " + ec.message()); }

    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if(ec) { throw std::runtime_error("set_option: " + ec.message()); }

    acceptor_.bind(endpoint, ec);
    if(ec) { throw std::runtime_error("bind: " + ec.message()); }

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec) { throw std::runtime_error("listen: " + ec.message()); }
}

void SSLListener::run()
{
    do_accept();
}

void SSLListener::listen(boost::beast::http::verb apiType,
                         const std::function<bool(std::shared_ptr<HTTPSession>,
                                                  const std::string&,
                                                  const std::string&,
                                                  const std::string&)>& onRequest)
{
    responders_[apiType] = onRequest;
}

void SSLListener::do_accept()
{
    acceptor_.async_accept(
                           boost::asio::make_strand(ioc_),
                           [self = shared_from_this()](boost::system::error_code ec,
                                                       boost::asio::ip::tcp::socket socket)
                           {
                               if(!ec)
                               {
                                   // Create the HTTPSession and run it
                                   std::make_shared<HTTPSession>(self, std::move(socket), self->ctx_)->run();
                               }
                               else
                               {
                                   std::cerr << "Accept error: " << ec.message() << std::endl;
                               }
                               
                               // Accept another connection
                               self->do_accept();
                           });
}

void SSLListener::respondOnRequest(std::shared_ptr<HTTPSession> session,
                                   boost::beast::http::verb apiType,
                                   const std::string& apiKey,
                                   const std::string& path,
                                   const std::string& jsonOrText)
{
    responders_[apiType](session, apiKey, path, jsonOrText);
}

