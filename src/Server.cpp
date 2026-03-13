#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <cpprest/json.h>  // For JSON functionality
#include <cpprest/http_listener.h>

#include "Server.h"
#include "Utils.h"

using namespace utility;                    // Common utilities like string conversions
using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features
using namespace concurrency::streams;       // Asynchronous streams
using namespace http::experimental::listener;

#define INVALID_REQUEST_ID 0xffffffff

namespace hen {

bool AgentServerEP::receive(std::shared_ptr<RemoteEP> remote, std::shared_ptr<Message> msg)
{
    bool found = false;
    for(auto ep : m_agentEPs)
    {
        if(ep.second == remote)
        {
            found = true;
            break;
        }
    }
    
    if(!found)
    {
        std::string sessionId = msg->c_str();
        if(!sessionId.empty())
        {
            //uint64_t sessionId = *sessionIdPtr;
            m_agentEPs[sessionId] = remote;
        }
    }
    
    
    //Receive automatically only the first time to get the sessionId and associate it with the remote.
    //Returning false will not call receive in the future when there is message available.
    //The AgentServerEP will have to call getConnection()->receive();
    return false;
}

std::shared_ptr<RemoteEP> AgentServerEP::getConnection(const std::string& sessionId)
{
    //TODO: Make it multithreading safe
    
    auto connIt = m_agentEPs.find(sessionId);
    if(connIt == m_agentEPs.end())
    {
        return nullptr;
    }
    
    return connIt->second;
}

bool AgentAtService::connected()
{
    return false;
}

std::string AgentAtService::popNextMessage()
{
    const uint32_t c_line_max_characters = 200;
    
    std::istringstream iss(m_log);
    
    if(m_log.length()==0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return "";
    }
    
    std::string line;
    if (!std::getline(iss, line)) {
        line = m_log;
    }

    std::string message;
    if(line.length() <= c_line_max_characters)
    {
        message = popNextLine();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    else
    {
        message = popNextWord();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    return message;
}

std::string AgentAtService::popNextLine()
{
    if (m_log.empty()) {
        return "";
    }

    // Look for the position of the next '\n' in m_log
    std::size_t pos = m_log.find('\n');
    if (pos == std::string::npos) {
        // No newline found => we don't have a complete "line" to return yet
        return "";
    }

    // Extract the substring up to and including the '\n'
    std::string line = m_log.substr(0, pos + 1);

    // Erase that line from m_log
    m_log.erase(0, pos + 1);

    // If we have a Windows-style "\r\n", remove the '\r' so we end with just "\n"
    // e.g. "Hello\r\n" -> "Hello\n"
    if (line.size() >= 2 && line[line.size() - 2] == '\r') {
        line.erase(line.size() - 2, 1);
    }

    // Append this line to m_history
    // (Decide how you want to store it in history—here we just append directly.)
    m_history += line;

    // Return the line (which includes the '\n')
    return line;
}

std::string AgentAtService::popNextWord()
{
    // If m_log is empty, return immediately
    if (m_log.empty()) {
        return "";
    }

    // We’ll look for the earliest occurrence of any of these delimiters:
    // space (' '), tab ('\t'), carriage return ('\r'), line feed ('\n').
    static const char* delimiters = " \t\r\n";

    // Find position of the earliest delimiter in m_log
    std::size_t pos = m_log.find_first_of(delimiters);

    // We want to *include* that delimiter in the returned chunk, if it exists.
    std::string chunk;
    if (pos == std::string::npos) {
        // No delimiter found => take the whole string
        chunk = m_log;
        m_log.clear();
    } else {
        // Extract up to and including the delimiter
        chunk = m_log.substr(0, pos + 1);
        m_log.erase(0, pos + 1);
    }

    // Append the chunk we extracted to m_history
    // (optionally add your own separator if desired)
    if (!m_history.empty()) {
        m_history.push_back('\n');  // Example of adding a newline in between
    }
    m_history += chunk;

    // Return the extracted chunk
    return chunk;
}

AgentAtService::AgentAtService(const std::string& projectId,
                std::unique_ptr<boost_prc::process> process,  // Changed from child to process
                const std::string& apiKey):
                m_projectId(projectId),
                m_process(std::move(process)),
                m_apiKey(apiKey)
{
    
}

class AsyncLLMSession : public std::enable_shared_from_this<AsyncLLMSession>
{
public:
    // Constructor
    AsyncLLMSession(// std::shared_ptr<Server> server,
                    Server* server,
                    std::shared_ptr<RemoteEP> remote,
                    boost::beast::http::request<boost::beast::http::string_body> req,
                    std::shared_ptr<LLMConfig> llm,
                    const std::string& projectId)
      : server_(std::move(server)),
        remote_(std::move(remote)),
        req_(std::move(req)),
        llm_(std::move(llm)),
        projectId_(projectId),
        requestId_(INVALID_REQUEST_ID),
        resolver_(*getAsioContext()),  // You presumably have getAsioContext() returning a shared_ptr<io_context>
        sslContext_(ssl::context::tlsv12_client),
        stream_(*getAsioContext(), sslContext_)
    {
        // Optionally set verify paths, etc.
        sslContext_.set_verify_mode(boost::asio::ssl::verify_peer);
        sslContext_.set_default_verify_paths();
    }

    // Start the async chain
    void run()
    {
        // Parse the URL from llm_->url
        auto result = boost::urls::parse_uri(llm_->url);
        if(!result) {
            handleError("Invalid URL: " + llm_->url);
            return;
        }
        url_ = *result;

        scheme_ = std::string(url_.scheme());
        if (scheme_ != "https" && scheme_ != "http")
        {
            handleError("Unsupported URL scheme: " + scheme_);
            return;
        }
        useSSL_ = (scheme_ == "https");
        
        host_   = std::string(url_.host());
        port_   = url_.has_port()
                    ? std::string(url_.port())
                    : (scheme_ == "https" ? "443" : "80");

        // Resolve host and port
        resolver_.async_resolve(host_, port_,
            [self = shared_from_this()](boost::beast::error_code ec,
                                        boost::asio::ip::tcp::resolver::results_type results)
            {
                self->onResolve(ec, results);
            }
        );
    }
    
    void setRequestId(uint32_t requestId) { requestId_ = requestId; }

private:
    // Step 1: onResolve
    void onResolve(boost::beast::error_code ec, boost::asio::ip::tcp::resolver::results_type results)
    {
        if(ec) {
            handleError("resolve", ec);
            return;
        }

        // async_connect
        boost::asio::async_connect(stream_.next_layer().socket(),
                                   results,
            [self = shared_from_this()](boost::beast::error_code ec, auto endpoint)
            {
                self->onConnect(ec);
            }
        );
    }

    // Step 2: onConnect
    void onConnect(boost::beast::error_code ec)
    {
        if(ec) {
            handleError("connect", ec);
            return;
        }
        
        if(useSSL_)
        {
            // Set SNI (Server Name Indication)
            ::SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str());
            
            // async_handshake
            stream_.async_handshake(ssl::stream_base::client,
                [self = shared_from_this()](boost::beast::error_code ec){
                    self->onHandshake(ec);
                }
            );
        }
        else
        {
            writeRequest();
        }
    }

    // Step 3: onHandshake
    void onHandshake(boost::beast::error_code ec)
    {
        if(ec) {
            handleError("handshake", ec);
            return;
        }

        writeRequest();
    }
    
    void writeRequest()
    {
        // Write request
        // If you need to update .target(...), do it here before writing
        // e.g. combine path + query from url_ if not already in req_
        if(useSSL_)
        {
            boost::beast::http::async_write(stream_, req_,
                [self = shared_from_this()](boost::beast::error_code ec, size_t)
                {
                    self->onWrite(ec);
                }
            );
        }
        else
        {
            boost::beast::http::async_write(stream_.next_layer(), req_,
                [self = shared_from_this()](boost::beast::error_code ec, size_t)
                {
                    self->onWrite(ec);
                }
            );
        }
    }

    // Step 4: onWrite
    void onWrite(boost::beast::error_code ec)
    {
        if(ec) {
            handleError("write", ec);
            return;
        }

        // Read the response
        if(useSSL_)
        {
            boost::beast::http::async_read(stream_, buffer_, res_,
                [self = shared_from_this()](boost::beast::error_code ec, size_t)
                {
                    self->onRead(ec);
                }
            );
        }
        else
        {
            boost::beast::http::async_read(stream_.next_layer(), buffer_, res_,
                [self = shared_from_this()](boost::beast::error_code ec, size_t)
                {
                    self->onRead(ec);
                }
            );
        }
    }

    // Step 5: onRead
    void onRead(boost::beast::error_code ec)
    {
        if(ec) {
            handleError("read", ec);
            return;
        }

        // Now we have the response in res_
        if(useSSL_)
        {
            // We'll shut down SSL, but some servers might not do a clean shutdown
            stream_.async_shutdown(
                [self = shared_from_this()](boost::beast::error_code shutdownEc){
                    // ignoring shutdownEc for brevity
                    self->onShutdown();
                }
            );
        }
        else
        {
            onShutdown();
        }
    }

    // Step 6: onShutdown
    void onShutdown()
    {
        // At this point, we can parse JSON and send the result back to `remote_`
        try
        {
            // Convert res_.body() into JSON
            auto parsedJson = web::json::value::parse(
                    utility::conversions::to_string_t(res_.body()));

            // Call your handleResponse (like in your old chain)
            auto processedResponse = server_->handleResponse(parsedJson, llm_, projectId_);
            
            if(requestId_ != INVALID_REQUEST_ID)
            {
                processedResponse[U("request_id")] = json::value::number(requestId_);
            }

            // Convert to string and send
            auto replyStr = utility::conversions::to_utf8string(
                                processedResponse.serialize());
            remote_->send((void*)replyStr.c_str(),
                          static_cast<uint32_t>(replyStr.size() + 1));
        }
        catch(const std::exception& e)
        {
            std::cout << res_.body();
            // Send error JSON
            handleError(std::string("parse/handle error: ") + e.what());
        }
    }

    void handleError(const std::string& what, boost::beast::error_code ec = {})
    {
        if(ec)
            std::cerr << "AsyncLLMSession error in " << what << ": "
                      << ec.message() << "\n";
        else
            std::cerr << "AsyncLLMSession error: " << what << "\n";

        // Optionally return an error to remote_
        web::json::value errorReply;
        errorReply[U("error")] = web::json::value::string(utility::conversions::to_string_t(what));
        
        if(requestId_ != INVALID_REQUEST_ID)
        {
            errorReply[U("request_id")] = json::value::number(requestId_);
        }
        
        auto errorStr = utility::conversions::to_utf8string(errorReply.serialize());
        remote_->send((void*)errorStr.c_str(),
                      static_cast<uint32_t>(errorStr.size()+1));
    }

private:
    // References/data we need:
    //std::shared_ptr<Server> server_;
    Server* server_;
    std::shared_ptr<RemoteEP> remote_;
    boost::beast::http::request<boost::beast::http::string_body> req_;
    std::shared_ptr<LLMConfig> llm_;
    std::string projectId_;

    boost::asio::ip::tcp::resolver resolver_;
    ssl::context sslContext_;
    boost::beast::ssl_stream<boost::beast::tcp_stream> stream_;

    boost::beast::flat_buffer buffer_;
    boost::beast::http::response<boost::beast::http::string_body> res_;

    // Our parsed URL parts
    boost::urls::url_view url_;
    std::string scheme_;
    std::string host_;
    std::string port_;
    bool useSSL_ = true;
    uint32_t requestId_;
};

bool LLMServerEP::receive(std::shared_ptr<RemoteEP> remote, std::shared_ptr<Message> msg)
{
    auto requestBody = msg->json();
    
    uint32_t requestId = INVALID_REQUEST_ID;
    if (requestBody.has_field(U("request_id"))) {
        requestId = requestBody[U("request_id")].as_number().to_uint32();
        requestBody.erase(U("request_id"));
    }

    // Prepare the request
    std::shared_ptr<LLMConfig> llm;
    std::string projectId;
    auto beastRequest = m_appServer->prepareBeastRequest(requestBody, llm, projectId);
    if(!llm || llm->url.empty())
    {
        // Possibly send an error back
        web::json::value errorReply;
        errorReply[U("error")] = web::json::value::string(U("Invalid config or URL"));
        auto errorStr = utility::conversions::to_utf8string(errorReply.serialize());
        remote->send((void*)errorStr.c_str(), static_cast<uint32_t>(errorStr.size() + 1));
        return true;
    }

    // Create session and start it
    auto session = std::make_shared<AsyncLLMSession>(
        m_appServer,        // your Server pointer
        remote,
        beastRequest,
        llm,
        projectId
    );
    session->run();  // schedule async operations
    session->setRequestId(requestId);

    // Return immediately. The chain will complete in background.
    return true;
}

Server::Server():
m_db(nullptr)
{
}

std::string Server::doBeastRequest(
    const boost_bst::http::request<boost_bst::http::string_body>& beastRequest,
    std::shared_ptr<LLMConfig> llm)
{
    // 1) Parse the URL
    //    e.g. "https://api.openai.com:443/v1/chat/completions"
    auto urlResult = boost::urls::parse_uri(llm->url);
    if (!urlResult)
    {
        // Handle invalid URL
        throw std::runtime_error("Invalid URL for doBeastRequest: " + llm->url);
    }
    boost::urls::url_view urlView = *urlResult;

    // 2) Extract the scheme, host, port, and path/target
    //    - If no explicit port is present, pick 443 if scheme==https, else 80.
    std::string scheme = std::string(urlView.scheme());
    std::string host   = std::string(urlView.host());
    std::string port   = urlView.has_port()
                           ? std::string(urlView.port())            // e.g. "443"
                           : (scheme == "https" ? "443" : "80");

    // The path portion + optional query
    // e.g. "/v1/chat/completions" + "?myparam=123"
    std::string target = std::string(urlView.encoded_path());
    if (urlView.has_query())
    {
        target += "?";
        target += std::string(urlView.encoded_query());
    }

    // 3) Prepare the SSL context (we'll assume https if scheme=="https")
    //    Otherwise, we could do plain TCP. For simplicity, let's always do SSL
    auto ioc = getAsioContext();  // Your existing io_context
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv12_client);

    // [Optional but recommended] Enable peer verification and load system CAs
    ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    ctx.set_default_verify_paths();
    // If you need to trust a custom CA:
    // ctx.load_verify_file("/path/to/your/ca.crt");

    // 4) Create sslStream
    boost::asio::ip::tcp::resolver resolver(*ioc);
    auto endpoints = resolver.resolve(host, port);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> sslStream(*ioc, ctx);

    // 5) Connect TCP
    boost::asio::connect(sslStream.next_layer(), endpoints);

    // 6) SNI (Server Name Indication) is often required
    //    If omitted, many modern servers will close the connection
    SSL_set_tlsext_host_name(sslStream.native_handle(), host.c_str());

    // 7) SSL handshake
    sslStream.handshake(boost::asio::ssl::stream_base::client);

    // 8) Prepare a copy of the request with the correct target
    auto reqCopy = beastRequest;
    reqCopy.target(target);

    // [Optional] Always good to set Host header
    reqCopy.set(boost_bst::http::field::host, host);

    // 9) Send the request
    boost_bst::flat_buffer buffer;
    boost_bst::http::write(sslStream, reqCopy);

    // 10) Receive the response
    boost_bst::http::response<boost_bst::http::string_body> res;
    boost_bst::http::read(sslStream, buffer, res);

    // 11) Shutdown SSL (if using SSL)
    boost_bst::error_code ec;
    sslStream.shutdown(ec);
    // Some servers won't do a clean shutdown, so ec might be non-zero

    // 12) Return the body as string
    return res.body();
}

template<class Body>
std::string beast_to_curl(const boost_bst::http::request<Body>& req, const std::string& base_url = "https://") {
    std::ostringstream curl_cmd;
    
    // Start with curl and method
    curl_cmd << "curl -X " << req.method_string();
    
    // Add all headers
    for(const auto& field : req) {
        curl_cmd << " \\\n  -H \"" << field.name_string() << ": " << field.value() << "\"";
    }
    
    // Add body if it exists (C++14 compatible check)
    if (std::is_same<Body, boost_bst::http::string_body>::value) {
        if (!req.body().empty()) {
            // Escape single quotes in the body for shell safety
            std::string body = req.body();
            size_t pos = 0;
            while ((pos = body.find("'", pos)) != std::string::npos) {
                body.replace(pos, 1, "'\"'\"'");
                pos += 5;
            }
            curl_cmd << " \\\n  -d '" << body << "'";
        }
    }
    
    // Construct full URL from Host header and target
    std::string host;
    auto host_it = req.find(boost_bst::http::field::host);
    if (host_it != req.end()) {
        host = std::string(host_it->value());
    }
    
    // Build URL
    std::string url = base_url + host + std::string(req.target());
    curl_cmd << " \\\n  \"" << url << "\"";
    
    return curl_cmd.str();
}

// Prepare a Beast request with appropriate headers/body
boost_bst::http::request<boost_bst::http::string_body> Server::prepareBeastRequest(
    web::json::value& requestFromClientBody,
    std::shared_ptr<LLMConfig>& llm, std::string& projectId)
{
    projectId = prepareBody(requestFromClientBody, llm);

    // Build a JSON string from requestFromClientBody
    std::string bodyStr = utility::conversions::to_utf8string(requestFromClientBody.serialize());
    
    //std::cout << "Request body: " << std::endl << bodyStr << std::endl;

    // Create Beast request
    boost_bst::http::request<boost_bst::http::string_body> req;
    req.method(boost_bst::http::verb::post);
    
    std::string target = getTargetFromUrl(llm->url);
    req.target(target);
    req.version(11); // HTTP/1.1

    // Set headers
    boost::system::result<boost::urls::url_view> url = boost::urls::parse_uri(llm->url);
    std::string host = url->host();
    req.set(boost_bst::http::field::host, host);
    
    req.set(boost_bst::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(boost_bst::http::field::content_type, "application/json");

    if (llm->provider == "openai" ||
        llm->provider == "deepinfra" ||
        llm->provider == "deepseek" ||
        llm->provider == "xAI")
    {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
    }
    else if (llm->provider == "anthropic") {
        req.set("x-api-key", llm->api_key);
        req.set("anthropic-version", "2023-06-01");
    }
    else if (llm->provider == "google")
    {
        req.set("X-goog-api-key", llm->api_key);
    }
    else if (llm->provider == "groq") {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
    }
    else if (llm->provider == "cerebras") {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
    }
    else if (llm->provider == "zai") {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
        //req.set("User-Agent", "opencode/1.0.0");
        req.set("User-Agent", PRODUCT_NAME);
    }
    else if (llm->provider == "minimax") {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
        //req.set("User-Agent", "opencode/1.0.0");
        req.set("User-Agent", PRODUCT_NAME);
    }
    else if (llm->provider == "mistral")
    {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
        req.set("User-Agent", PRODUCT_NAME);
    }
    else if (llm->provider == "alibaba")
    {
        req.set(boost_bst::http::field::authorization, "Bearer " + llm->api_key);
        req.set("User-Agent", PRODUCT_NAME);
    }

    // Set the body
    req.body() = bodyStr;
    req.prepare_payload();  // Automatically sets Content-Length, etc.
    
    //std::cout << "PRINT REQUEST BODY:" << std::endl << beast_to_curl(req) << std::endl;

    return req;
}

bool Server::initLLMProxyListener()
{
    if(m_llmProxyIP.empty() || m_llmProxyIP == "http://")
    {
        std::cout << "Empty string for llm proxy IP" << std::endl;
        return false;
    }
    
    std::vector<std::string> addressAndPort;
    boost::split(addressAndPort, m_llmProxyIP, boost::is_any_of(":"));
    
    unsigned short port = std::atoi(addressAndPort[addressAndPort.size()-1].c_str());
    m_llmServerEP = std::make_unique<LLMServerEP>(port, this);
    
    return true;
}

int Server::init(int argc, char* argv[])
{
    Peer::init(argc, argv);
    
    m_agentsPortRange[0] = 0;
    m_agentsPortRange[1] = 0;
    m_serverPort = 0;
    
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "-ver")
        {
            std::cout << "0.1\n";
        }
        else if (std::string(argv[i]) == "-key")
        {
            std::string provider = argv[i + 1];
            std::vector<std::string> kayValue;
            boost::split(kayValue, provider, boost::is_any_of("="));
            m_keys[kayValue[0]] = kayValue[1];
        }
        else if (std::string(argv[i]) == "-sp")
        {
            std::string serverPortStr = argv[i + 1];
            m_serverPort += static_cast<unsigned int>(std::stoul(serverPortStr));
        }
        else if (std::string(argv[i]) == "-apr")
        {
            std::string aprStr = argv[i + 1];
            
            std::vector<std::string> ports;
            boost::split(ports, aprStr, boost::is_any_of(":"));
            
            if(ports.size() != 2)
            {
                std::cout << "Invalid agents ports range" << std::endl;
            }
            else
            {
                m_agentsPortRange[0] = std::atoi(ports[0].c_str());
                m_agentsPortRange[1] = std::atoi(ports[1].c_str());
            }
        }
        else if(std::string(argv[i]) == "-cpx")
        {
            //m_chatProxyIP
        }
        else if(std::string(argv[i]) == "-root")
        {
            m_rootDirectory = argv[i + 1];
        }
        else if (std::string(argv[i]) == "-kt")
        {
            m_keytabFile = argv[i + 1];
        }
        else
        {
            // Parse other arguments
        }
    }
    
    startDatabse();
    
    for(auto& llm : m_llms.llms)
    {
        auto key = m_keys.find(llm->provider);
        if(key != m_keys.end())
        {
            llm->api_key = key->second;
        }
    }
    
    m_keys.clear();

    initLLMProxyListener();
    
    if(initChatListener())
    {
        //m_agentsPortAllocator = std::make_shared<PortAllocator>(m_agentsPortRange[0], m_agentsPortRange[1]);
        listenForChatUsers();
    }

    return 0;
}

web::json::value Server::convertToGoogle(const web::json::value& openAIRequestBody)
{
    // 1. Create a new JSON object for the Google API request
    web::json::value googleRequestBody;

    // 2. Ensure the 'messages' field exists and is an array
    if (!openAIRequestBody.has_field(U("messages")) || !openAIRequestBody.at(U("messages")).is_array())
    {
        // Return an empty object or throw an exception if the format is invalid
        return googleRequestBody;
    }
    const auto& messages = openAIRequestBody.at(U("messages")).as_array();

    // 3. Create the new 'contents' array for Google's format
    web::json::value contentsArray = web::json::value::array();
    size_t contentIndex = 0; // Index for the new contentsArray

    // 4. Iterate through the original messages
    for (const auto& message : messages)
    {
        // Skip if message is not a valid object with role and content
        if (!message.is_object() || !message.has_field(U("role")) || !message.has_field(U("content")))
        {
            continue;
        }

        utility::string_t role = message.at(U("role")).as_string();
        const utility::string_t& content = message.at(U("content")).as_string();

        // A. Handle the 'system' message
        if (role == U("system"))
        {
            web::json::value systemInstruction;
            web::json::value partsArray = web::json::value::array(1);
            web::json::value part;
            part[U("text")] = web::json::value::string(content);
            partsArray[0] = part;
            systemInstruction[U("parts")] = partsArray;
            googleRequestBody[U("system_instruction")] = systemInstruction;
        }
        // B. Handle 'user' and 'assistant' messages
        else
        {
            web::json::value newContent;
            utility::string_t newRole = (role == U("assistant")) ? U("model") : U("user");
            newContent[U("role")] = web::json::value::string(newRole);

            web::json::value partsArray = web::json::value::array(1);
            web::json::value part;
            part[U("text")] = web::json::value::string(content);
            partsArray[0] = part;
            newContent[U("parts")] = partsArray;

            contentsArray[contentIndex++] = newContent;
        }
    }

    // 5. Add the completed 'contents' array to the new request body
    googleRequestBody[U("contents")] = contentsArray;

    return googleRequestBody;
}

static std::string extractResponsesText(const web::json::value& contentValue)
{
    using namespace web;
    std::string content;
    
    if (contentValue.is_string())
    {
        return utility::conversions::to_utf8string(contentValue.as_string());
    }
    
    if (contentValue.is_array())
    {
        for (const auto& part : contentValue.as_array())
        {
            if (part.is_string())
            {
                content += utility::conversions::to_utf8string(part.as_string());
            }
            else if (part.is_object())
            {
                if (part.has_field(U("text")) && part.at(U("text")).is_string())
                {
                    content += utility::conversions::to_utf8string(part.at(U("text")).as_string());
                }
                else if (part.has_field(U("content")) && part.at(U("content")).is_string())
                {
                    content += utility::conversions::to_utf8string(part.at(U("content")).as_string());
                }
            }
        }
        return content;
    }
    
    if (contentValue.is_object())
    {
        if (contentValue.has_field(U("text")) && contentValue.at(U("text")).is_string())
        {
            return utility::conversions::to_utf8string(contentValue.at(U("text")).as_string());
        }
        
        if (contentValue.has_field(U("content")) && contentValue.at(U("content")).is_string())
        {
            return utility::conversions::to_utf8string(contentValue.at(U("content")).as_string());
        }
    }
    
    return content;
}

static utility::string_t buildResponsesInputFromMessages(const web::json::value& messagesValue)
{
    using namespace web;
    
    if (messagesValue.is_string())
    {
        return messagesValue.as_string();
    }
    
    std::string input;
    auto appendMessage = [&input](const json::value& msg)
    {
        if (!msg.is_object())
        {
            return;
        }
        
        std::string role = "user";
        if (msg.has_field(U("role")) && msg.at(U("role")).is_string())
        {
            role = utility::conversions::to_utf8string(msg.at(U("role")).as_string());
        }
        
        std::string content;
        if (msg.has_field(U("content")))
        {
            content = extractResponsesText(msg.at(U("content")));
        }
        else if (msg.has_field(U("text")))
        {
            content = extractResponsesText(msg.at(U("text")));
        }
        
        if (content.empty())
        {
            return;
        }
        
        if (!input.empty())
        {
            input += "\n\n";
        }
        
        input += "[";
        input += role;
        input += "]\n";
        input += content;
    };
    
    if (messagesValue.is_array())
    {
        for (const auto& message : messagesValue.as_array())
        {
            appendMessage(message);
        }
    }
    else if (messagesValue.is_object())
    {
        appendMessage(messagesValue);
    }
    
    return utility::conversions::to_string_t(input);
}

std::string Server::prepareBody(json::value& requestFromClientBody, std::shared_ptr<LLMConfig>& llm)
{
    std::string llmCfgStr = utility::conversions::to_utf8string(requestFromClientBody[U("llm")].as_string());
    
    auto llmCfg = splitByFirstOccurence(llmCfgStr, '/');
    
    requestFromClientBody.erase(U("llm"));
    
    llm = findLLM(llmCfg.first, llmCfg.second);
    
    std::string projectId;
    if (requestFromClientBody.has_field(U("projectId"))) {
        
        auto uProjectId = requestFromClientBody[U("projectId")].as_string();
        projectId = utility::conversions::to_utf8string(uProjectId);
        requestFromClientBody.erase(U("projectId"));
    }
    
    //Enforce rate limits for this LLM
    if(llm->rate_limit > 0 && llm->tokens_last_infer > 0)
    {
        std::chrono::duration<double, std::milli> elapsed = std::chrono::high_resolution_clock::now() - llm->time_last_infer;
        
        // Calculate the expected duration for these tokens
        float expected_duration_millisec = ((float)llm->tokens_last_infer/(float)llm->rate_limit)*1000;
        std::chrono::duration<double, std::milli> expected_duration(expected_duration_millisec);

        // Calculate the time to sleep (if any)
        auto sleep_duration = expected_duration - elapsed;

        // Sleep if necessary (only if sleep_duration is positive)
        if (sleep_duration > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(sleep_duration);
        }
    }
    
    if (llm->rate_limit_rpm > 0) {
        using clock = std::chrono::steady_clock;       // monotonic; safe against system time jumps
        const auto now = clock::now();

        // Minimum interval between requests: 60 seconds / RPM
        const auto min_interval = std::chrono::duration<double>(60.0 / static_cast<double>(llm->rate_limit_rpm));

        const auto elapsed = now - llm->time_last_infer;
        if (elapsed < min_interval) {
            std::this_thread::sleep_for(min_interval - elapsed);  // no arbitrary +1000 ms
        }
    }
    
    if(llm->provider != "google")
    {
        requestFromClientBody[U("model")] = json::value::string(utility::conversions::to_string_t(llm->model));
    }
    
    bool groq = llm->provider == "groq";
    
    if(llm->provider == "openai" ||
       llm->provider == "deepinfra" ||
       llm->provider == "deepseek" ||
       llm->provider == "xAI" ||
       llm->provider == "cerebras" ||
       llm->provider == "zai" ||
       llm->provider == "minimax" ||
       llm->provider == "mistral" ||
       llm->provider == "alibaba")
    {
        bool openai_reasoning = startsWith(llm->model, "o1") ||
                              startsWith(llm->model, "o3") ||
                              startsWith(llm->model, "o4") ||
                              startsWith(llm->model, "gpt-5") ||
                              startsWith(llm->model, "codex");
        
        utility::string_t llmUrl = utility::conversions::to_string_t(llm->url);
        bool openaiResponsesEndpoint = llmUrl.find(U("/v1/responses")) != utility::string_t::npos;
        
        if(openaiResponsesEndpoint)
        {
            if (requestFromClientBody.has_field(U("messages")))
            {
                auto input = buildResponsesInputFromMessages(requestFromClientBody[U("messages")]);
                requestFromClientBody[U("input")] = json::value::string(input);
                requestFromClientBody.erase(U("messages"));
            }
            else if (requestFromClientBody.has_field(U("input")) && !requestFromClientBody[U("input")].is_string())
            {
                requestFromClientBody[U("input")] = json::value::string(buildResponsesInputFromMessages(requestFromClientBody[U("input")]));
            }
            
            utility::string_t reasoningEffort;
            bool hasReasoningEffort = false;
            if (requestFromClientBody.has_field(U("reasoning_effort")))
            {
                reasoningEffort = requestFromClientBody[U("reasoning_effort")].as_string();
                requestFromClientBody.erase(U("reasoning_effort"));
                hasReasoningEffort = true;
            }
            else if(llm->reasoning_effort != "na")
            {
                reasoningEffort = utility::conversions::to_string_t(llm->reasoning_effort);
                hasReasoningEffort = true;
            }
            
            if (hasReasoningEffort)
            {
                requestFromClientBody[U("reasoning")][U("effort")] = json::value::string(reasoningEffort);
            }
            
            utility::string_t verbosity;
            bool hasVerbosity = false;
            if (requestFromClientBody.has_field(U("verbosity")))
            {
                verbosity = requestFromClientBody[U("verbosity")].as_string();
                requestFromClientBody.erase(U("verbosity"));
                hasVerbosity = true;
            }
            else if(llm->verbosity != "na")
            {
                verbosity = utility::conversions::to_string_t(llm->verbosity);
                hasVerbosity = true;
            }
            
            if (hasVerbosity)
            {
                requestFromClientBody[U("text")][U("verbosity")] = json::value::string(verbosity);
            }
        }
        else
        {
            if(!openai_reasoning)
            {
                requestFromClientBody[U("temperature")] = json::value::number(0);
            }
            else //openAIModel_o1
            {
                //Skip the 'system' role message
                uint32_t messagesCount = (uint32_t)requestFromClientBody[U("messages")].as_array().size();
                if(messagesCount > 1)
                {
                    utility::string_t role = requestFromClientBody[U("messages")].as_array()[0][U("role")].as_string();
                    if(role == U("system"))
                    {
                        requestFromClientBody[U("messages")].as_array().erase(0);
                    }
                }
            }
            
            if(llm->reasoning_effort != "na")
            {
                requestFromClientBody[U("reasoning_effort")] = json::value::string(utility::conversions::to_string_t(llm->reasoning_effort));
            }
            
            if(llm->verbosity != "na")
            {
                requestFromClientBody[U("verbosity")] = json::value::string(utility::conversions::to_string_t(llm->verbosity));
            }
        }
        
        if(startsWith(llm->model, "o3-mini"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("o3-mini"));
        }
        else if(startsWith(llm->model, "o4-mini"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("o4-mini"));
        }
        else if(startsWith(llm->model, "gpt-5-mini"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("gpt-5-mini"));
        }
        
        if(llm->provider == "deepinfra")
        {
            requestFromClientBody[U("temperature")] = json::value::number(0.3);
            //requestFromClientBody[U("temperature")] = json::value::number(0.5);
            requestFromClientBody[U("max_tokens")] = json::value::number(1536);
            requestFromClientBody[U("frequency_penalty")] = json::value::number(0.2);
            requestFromClientBody[U("repetition_penalty")] = json::value::number(1.05);
        }
    }
    else if(llm->provider == "anthropic")
    {
        if(llm->reasoning_effort != "na")
        {
            requestFromClientBody[U("max_tokens")] = json::value::number(6000);
            auto thinking = json::value::object();
            //thinking[U("type")] = json::value::string(U("adaptive"));
            thinking[U("type")] = json::value::string(U("enabled"));
            thinking[U("budget_tokens")] = json::value::number(2000);
            requestFromClientBody[U("thinking")] = thinking;
            requestFromClientBody[U("temperature")] = json::value::number(1);
        }
        else
        {
            requestFromClientBody[U("max_tokens")] = json::value::number(4096);
            requestFromClientBody[U("temperature")] = json::value::number(0);
        }
        
        if(startsWith(llm->model, "claude-sonnet-4-0"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("claude-sonnet-4-0"));
        }
        else if(startsWith(llm->model, "claude-sonnet-4-5"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("claude-sonnet-4-5"));
        }
        else if(startsWith(llm->model, "claude-haiku-4-5"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("claude-haiku-4-5"));
        }
        else if(startsWith(llm->model, "claude-opus-4-0"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("claude-opus-4-0"));
        }
        else if(startsWith(llm->model, "claude-opus-4-5"))
        {
            requestFromClientBody[U("model")] = json::value::string(U("claude-opus-4-5"));
        }
        
        //Remove the system message from the start of the messages array and put it at the top of the request
        uint32_t messagesCount = (uint32_t)requestFromClientBody[U("messages")].as_array().size();
        if(messagesCount > 1)
        {
            utility::string_t role = requestFromClientBody[U("messages")].as_array()[0][U("role")].as_string();
            if(role == U("system"))
            {
                auto systemText = requestFromClientBody[U("messages")].as_array()[0][U("content")].as_string();
                auto systemBlock = json::value::object();
                systemBlock[U("type")] = json::value::string(U("text"));
                systemBlock[U("text")] = json::value::string(systemText);
                systemBlock[U("cache_control")] = json::value::object();
                systemBlock[U("cache_control")][U("type")] = json::value::string(U("ephemeral"));

                auto systemArray = json::value::array();
                systemArray[0] = systemBlock;
                requestFromClientBody[U("system")] = systemArray;
                
                requestFromClientBody[U("messages")].as_array().erase(0);
            }
        }
        
        alternateRoles(requestFromClientBody);
        
        // Mark the last user message content for caching
        auto& msgs = requestFromClientBody[U("messages")].as_array();
        for (int i = (int)msgs.size() - 1; i >= 0; --i) {
            if (msgs[i][U("role")].as_string() == U("user")) {
                if(msgs[i][U("content")].is_string())
                {
                    auto content = msgs[i][U("content")].as_string();
                    
                    auto contentBlock = json::value::object();
                    contentBlock[U("type")] = json::value::string(U("text"));
                    contentBlock[U("text")] = json::value::string(content);
                    auto contentArray = json::value::array();
                    contentArray[0] = contentBlock;
                    msgs[i][U("content")] = contentArray;
                }
                
                if(msgs[i][U("content")].is_array() && msgs[i][U("content")].as_array().size() > 0)
                {
                    auto& contentArray = msgs[i][U("content")].as_array();
                    int lastTextBlock = -1;
                    for(int j = (int)contentArray.size() - 1; j >= 0; --j)
                    {
                        if(contentArray[j].is_object() &&
                           contentArray[j].has_field(U("type")) &&
                           contentArray[j][U("type")].is_string() &&
                           contentArray[j][U("type")].as_string() == U("text"))
                        {
                            lastTextBlock = j;
                            break;
                        }
                    }
                    
                    if(lastTextBlock >= 0)
                    {
                        contentArray[lastTextBlock][U("cache_control")] = json::value::object();
                        contentArray[lastTextBlock][U("cache_control")][U("type")] = json::value::string(U("ephemeral"));
                    }
                }
                break;
            }
        }
    }
    else if(llm->provider == "google")
    {
        // Convert the request body
        web::json::value googleRequestBody = convertToGoogle(requestFromClientBody);

        // Replace the original body with the newly formatted one before sending
        requestFromClientBody = googleRequestBody;

        // Optional: Print the new body to verify
        //ucout << U("Converted Google Request Body:\n") << requestFromClientBody.serialize() << std::endl;
    }
    else if(groq)
    {
        if(startsWith(llm->model, "openai/gpt-oss-"))
        {
            if(llm->reasoning_effort != "na")
            {
                requestFromClientBody[U("reasoning_effort")] = json::value::string(utility::conversions::to_string_t(llm->reasoning_effort));
            }
            
            if(llm->verbosity != "na")
            {
                requestFromClientBody[U("verbosity")] = json::value::string(utility::conversions::to_string_t(llm->verbosity));
            }
            
            //requestFromClientBody[U("temperature")] = json::value::number(0.2);
            //requestFromClientBody[U("top_p")] = json::value::number(0.95);
        }
        else
        {
            requestFromClientBody[U("temperature")] = json::value::number(0.3);
            requestFromClientBody[U("max_tokens")] = json::value::number(1536);
            requestFromClientBody[U("frequency_penalty")] = json::value::number(0.2);
            //requestFromClientBody[U("repetition_penalty")] = json::value::number(1.05);
        }
    }
    else
    {
        requestFromClientBody[U("stream")] = json::value::boolean(false);
        //For Ollama served models we enforce the context size specified in the registry
        requestFromClientBody[U("options")][U("num_ctx")] = json::value::number(llm->context_size*1024);
        requestFromClientBody[U("max_tokens")] = json::value::number(4096);
        requestFromClientBody[U("temperature")] = json::value::number(0.0);
        requestFromClientBody[U("top_p")] = json::value::number(1.0);
        requestFromClientBody[U("verbosity")] = json::value::string(U("medium"));
        requestFromClientBody[U("reasoning_effort")] = json::value::string(U("medium"));
    }
    
    //This is for test
    if(llm->provider == "cerebras")
    {
        requestFromClientBody[U("temperature")] = json::value::number(0.7);
    }
    
    
    return projectId;
}

// Helper to create a provider-agnostic usage object
web::json::value Server::updateUsage(web::json::value& usageField,
                                     std::shared_ptr<LLMConfig> llm,
                                     const std::string& projectId)
{
    using namespace web;
    json::value usage = json::value::object();

    // Default values
    uint32_t input_tokens = 0;
    uint32_t output_tokens = 0;
    
    uint32_t cache_write_tokens = 0;
    uint32_t cache_read_tokens = 0;
    
    //TODO: I'm not quite sure how to properly handle the thinking tokens here

    // Make sure usageField is valid
    if (!usageField.is_object()) {
        // Return with zeros if usageField is not an object
        usage[U("input_tokens")] = json::value::number(input_tokens);
        usage[U("cache_write_tokens")] = json::value::number(cache_write_tokens);
        usage[U("cache_read_tokens")] = json::value::number(cache_read_tokens);
        usage[U("output_tokens")] = json::value::number(output_tokens);
        usage[U("prompt_tokens")] = json::value::number(input_tokens + cache_write_tokens + cache_read_tokens);
        usage[U("completion_tokens")] = json::value::number(output_tokens);
        usage[U("total_tokens")] = json::value::number(input_tokens + cache_write_tokens + cache_read_tokens + output_tokens);
        usage[U("step_credits")] = json::value::number(0);
        usage[U("consumed_credits")] = json::value::number(0);
        usage[U("limit_credits")] = json::value::number(0);
        return usage;
    }

    // Distinguish by provider
    if (llm->provider == "groq" ||
        llm->provider == "openai" ||
        llm->provider == "deepinfra" ||
        llm->provider == "deepseek" ||
        llm->provider == "xAI" ||
        llm->provider == "cerebras" ||
        llm->provider == "zai" ||
        llm->provider == "minimax" ||
        llm->provider == "mistral" ||
        llm->provider == "alibaba"
        )
    {
        if (usageField.has_field(U("prompt_tokens"))) {
            input_tokens = static_cast<uint32_t>(usageField[U("prompt_tokens")].as_number().to_uint64());
        }
        if (usageField.has_field(U("completion_tokens"))) {
            output_tokens = static_cast<uint32_t>(usageField[U("completion_tokens")].as_number().to_uint64());
        }
        
        if (input_tokens == 0 && output_tokens == 0)
        {
            if (usageField.has_field(U("input_tokens"))) {
                input_tokens = static_cast<uint32_t>(usageField[U("input_tokens")].as_number().to_uint64());
            }
            if (usageField.has_field(U("output_tokens"))) {
                output_tokens = static_cast<uint32_t>(usageField[U("output_tokens")].as_number().to_uint64());
            }
        }
        
        if (usageField.has_field(U("prompt_tokens_details")) &&
            usageField[U("prompt_tokens_details")].is_object() &&
            usageField[U("prompt_tokens_details")].has_field(U("cached_tokens")))
        {
            cache_read_tokens = static_cast<uint32_t>(usageField[U("prompt_tokens_details")][U("cached_tokens")].as_number().to_uint64());
            if(cache_read_tokens <= input_tokens)
            {
                input_tokens -= cache_read_tokens;
            }
            else
            {
                input_tokens = 0;
            }
        }
    }
    else if (llm->provider == "anthropic")
    {
        if (usageField.has_field(U("input_tokens"))) {
            input_tokens = static_cast<uint32_t>(usageField[U("input_tokens")].as_number().to_uint64());
        }
        if (usageField.has_field(U("output_tokens"))) {
            output_tokens = static_cast<uint32_t>(usageField[U("output_tokens")].as_number().to_uint64());
        }
        
        // Prompt caching token fields
        if (usageField.has_field(U("cache_creation_input_tokens"))) {
            cache_write_tokens = static_cast<uint32_t>(usageField[U("cache_creation_input_tokens")].as_number().to_uint64());
        }
        if (usageField.has_field(U("cache_read_input_tokens"))) {
            cache_read_tokens = static_cast<uint32_t>(usageField[U("cache_read_input_tokens")].as_number().to_uint64());
        }
    }
    else if (llm->provider == "google")
    {
       // Google uses 'promptTokenCount' and 'candidatesTokenCount'
       if (usageField.has_field(U("promptTokenCount"))) {
           input_tokens = static_cast<uint32_t>(usageField[U("promptTokenCount")].as_number().to_uint64());
       }
       if (usageField.has_field(U("candidatesTokenCount"))) {
           output_tokens = static_cast<uint32_t>(usageField[U("candidatesTokenCount")].as_number().to_uint64());
       }
    }
    else //if (llm->provider == "ollama" || llm->provider == "vllm")
    {
        // Ollama uses 'prompt_eval_count' for prompt tokens
        if (usageField.has_field(U("prompt_eval_count"))) {
            input_tokens = static_cast<uint32_t>(usageField[U("prompt_eval_count")].as_number().to_uint64());
        }

        // And 'eval_count' for completion tokens
        if (usageField.has_field(U("eval_count"))) {
            output_tokens = static_cast<uint32_t>(usageField[U("eval_count")].as_number().to_uint64());
        }
    }
    // else if (...) for other providers

    const uint32_t prompt_tokens = input_tokens + cache_write_tokens + cache_read_tokens;
    const uint32_t completion_tokens = output_tokens;
    usage[U("input_tokens")] = json::value::number(input_tokens);
    usage[U("cache_write_tokens")] = json::value::number(cache_write_tokens);
    usage[U("cache_read_tokens")] = json::value::number(cache_read_tokens);
    usage[U("output_tokens")] = json::value::number(output_tokens);
    usage[U("prompt_tokens")] = json::value::number(prompt_tokens);
    usage[U("completion_tokens")] = json::value::number(completion_tokens);
    usage[U("total_tokens")] = json::value::number(prompt_tokens + completion_tokens);
    
    float cacheWriteMultiplier = 1.0f;
    float cacheReadMultiplier = 1.0f;
    if(llm->provider == "anthropic")
    {
        cacheWriteMultiplier = 1.25f;
        cacheReadMultiplier = 0.1f;
    }
    else if(llm->provider == "openai")
    {
        if(startsWith(llm->model, "gpt-5"))
        {
            cacheReadMultiplier = 0.1f;
        }
        else if(llm->model == "codex-mini-latest")
        {
            cacheReadMultiplier = 0.25f;
        }
    }
    float credits =
        (input_tokens / 1000000.0f) * llm->input_tokens_price +
        (cache_write_tokens / 1000000.0f) * llm->input_tokens_price * cacheWriteMultiplier +
        (cache_read_tokens / 1000000.0f) * llm->input_tokens_price * cacheReadMultiplier +
        (output_tokens / 1000000.0f) * llm->output_tokens_price;
    
    float creditsConsumed = 0;
    uint32_t creditsLimit = 0;
    
    if(!projectId.empty())
    {
        std::string apiKey;
        {
            std::lock_guard<std::mutex> lock(m_agentsMutex);
            auto agentIt = m_activeAgents.find(projectId);
            if(agentIt != m_activeAgents.end())
            {
                apiKey = agentIt->second->m_apiKey;
            }
        }
        
        if(!apiKey.empty())
        {
            APIKeyFlags keyFlags;
            getValues(m_db, apiKey, creditsConsumed, creditsLimit, keyFlags);
            if(consumeCredits(m_db, apiKey, credits))
            {
                creditsConsumed += credits;
            }
        }
    }
    
    usage[U("step_credits")] = json::value::number(credits);
    usage[U("consumed_credits")] = json::value::number(creditsConsumed);
    usage[U("limit_credits")] = json::value::number(creditsLimit);

    return usage;
}

json::value Server::handleResponse(json::value& json_response, std::shared_ptr<LLMConfig> llm, const std::string& projectId)
{
    //TODO: Log the json_response since it has useful info - tokens count etc
    
    json::value reply;
    if (json_response.has_field(U("output")) && json_response[U("output")].is_array())
    {
        std::string fullContent;
        auto outputArray = json_response[U("output")].as_array();
        for (auto& outputItem : outputArray)
        {
            if (!outputItem.is_object() ||
                !outputItem.has_field(U("type")) ||
                outputItem[U("type")].as_string() != U("message"))
            {
                continue;
            }
            
            if (!outputItem.has_field(U("content")) || !outputItem[U("content")].is_array())
            {
                continue;
            }
            
            auto contentArray = outputItem[U("content")].as_array();
            for (auto& contentItem : contentArray)
            {
                if (!contentItem.is_object()) {
                    continue;
                }
                
                if (contentItem.has_field(U("type")) &&
                    contentItem[U("type")].as_string() == U("output_text"))
                {
                    if (contentItem.has_field(U("text")))
                    {
                        fullContent += utility::conversions::to_utf8string(contentItem[U("text")].as_string());
                    }
                }
                else if (contentItem.has_field(U("text")))
                {
                    fullContent += utility::conversions::to_utf8string(contentItem[U("text")].as_string());
                }
            }
        }
        
        reply[U("message")][U("role")] = json::value::string(U("assistant"));
        reply[U("message")][U("content")] = json::value::string(utility::conversions::to_string_t(fullContent));
    }
    else if (json_response.has_field(U("choices")) && json_response[U("choices")].is_array() && json_response[U("choices")].as_array().size() > 0) {
        //OpenAI format
        auto choicesArray = json_response[U("choices")].as_array();
        reply = choicesArray[0];
    }
    else if (json_response.has_field(U("candidates")) && json_response[U("candidates")].is_array() && json_response[U("candidates")].as_array().size() > 0)
    {
        // Google Gemini format
        auto& firstCandidate = json_response[U("candidates")].as_array().at(0);
        if (firstCandidate.has_field(U("content")) && firstCandidate[U("content")].has_field(U("parts")) && firstCandidate[U("content")][U("parts")].is_array())
        {
            auto& partsArray = firstCandidate[U("content")][U("parts")].as_array();
            std::string fullContent;
            for (const auto& part : partsArray)
            {
                if (part.has_field(U("text")))
                {
                    fullContent += part.at(U("text")).as_string();
                }
            }
            reply[U("message")][U("role")] = json::value::string(U("assistant"));
            reply[U("message")][U("content")] = json::value::string(fullContent);
        }
    }
    else if (json_response.has_field(U("messages")) && json_response[U("messages")].is_array() && json_response[U("messages")].as_array().size() > 0)
    {
        auto messagesArray = json_response[U("messages")].as_array();
        reply[U("message")] = messagesArray[0];
    }
    else if (json_response.has_field(U("content")) && json_response[U("content")].is_array() && json_response[U("content")].as_array().size() > 0)
    {
        //Anthropic format
        auto contentArray = json_response[U("content")].as_array();
        auto role = json_response[U("role")].as_string();
        reply[U("message")][U("role")] = json::value::string(role);
        
        std::string fullContent;
        std::string thinkingContent;
        
        for (auto& contentItem : contentArray)
        {
            if (contentItem.has_field(U("type")) && contentItem.has_field(U("text")))
            {
                auto type = contentItem[U("type")].as_string();
                auto text = contentItem[U("text")].as_string();
                
                if (type == U("text"))
                {
                    // Regular response content
                    fullContent += text;
                }
                else if (type == U("thinking"))
                {
                    // Thinking content (you might want to store this separately or ignore it)
                    thinkingContent += text;
                }
            }
            else if (contentItem.has_field(U("text")))
            {
                // Fallback for older format without explicit type
                fullContent += contentItem[U("text")].as_string();
            }
        }
        
        reply[U("message")][U("content")] = json::value::string(fullContent);
    }
    else
    {
        //Assumes Ollama format
        reply = json_response;
    }
    
    llm->time_last_infer = std::chrono::high_resolution_clock::now();
    
    if (llm->provider == "google" && json_response.has_field(U("usageMetadata")))
    {
        // Create standard usage object
        json::value usage = updateUsage(json_response[U("usageMetadata")], llm, projectId);

        // Update LLM config's last tokens count
        llm->tokens_last_infer = static_cast<uint32_t>(usage[U("total_tokens")].as_number().to_uint64());

        // Put the standardized usage into the reply
        reply[U("usage")] = usage;
    }
    else if (json_response.has_field(U("usage")))
    {
        // Create standard usage object
        json::value usage = updateUsage(json_response[U("usage")], llm, projectId);

        // Update LLM config's last tokens count
        llm->tokens_last_infer = static_cast<uint32_t>(usage[U("total_tokens")].as_number().to_uint64());

        // Put the standardized usage into the reply
        reply[U("usage")] = usage;
    }
    else
    {
        // If usage was not present, you could still expose a zeroed usage block
        json::value usage = json::value::object();
        usage[U("input_tokens")]        = json::value::number(0);
        usage[U("cache_write_tokens")]  = json::value::number(0);
        usage[U("cache_read_tokens")]   = json::value::number(0);
        usage[U("output_tokens")]       = json::value::number(0);
        usage[U("prompt_tokens")]       = json::value::number(0);
        usage[U("completion_tokens")]   = json::value::number(0);
        usage[U("total_tokens")]        = json::value::number(0);
        
        usage[U("step_credits")]        = json::value::number(0);
        usage[U("consumed_credits")]    = json::value::number(0);
        usage[U("limit_credits")]       = json::value::number(0);
        
        reply[U("usage")] = usage;
    }
    
    return reply;
}

bool Server::update()
{
    if (!m_run)
    {
        //TODO: Code on server exit here
        return false;
    }

    return true;
}

void Server::shutdown()
{
    m_run = false;
    
    shutdownDatabase();
}

}
