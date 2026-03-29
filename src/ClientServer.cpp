#include "ClientServer.h"

namespace hen {

json::value Message::json() const
{
    size_t payloadSize = m_storage.size();
    if(payloadSize > 0 && m_storage[payloadSize - 1] == 0)
    {
        payloadSize--;
    }
    
    std::string payload(reinterpret_cast<const char*>(m_storage.data()), payloadSize);
    string_t ustr = conversions::to_string_t(payload);
    return json::value::parse(ustr);
}

static std::shared_ptr<boost::asio::io_context> g_io_context;
static std::vector<std::shared_ptr<std::thread>> g_io_threads;

std::shared_ptr<boost::asio::io_context> getAsioContext()
{
    if(g_io_context)
    {
        return g_io_context;
    }
    
    g_io_context = std::make_unique<boost::asio::io_context>();
    
    // Add work guard to prevent io_context from stopping when it runs out of work
    static auto work = boost::asio::make_work_guard(*g_io_context);  // Make static
    
    uint32_t thread_count = 3;
    for(unsigned int i = 0; i < thread_count; ++i) {
            g_io_threads.push_back(std::make_unique<std::thread>(
                [](){
                    std::cout << "IO context thread starting" << std::endl;
                    g_io_context->run();
                    std::cout << "IO context thread ending" << std::endl;
                }
            ));
        }
    
    return g_io_context;
}

void shutdownAsioContext()
{
    if(!g_io_context) return;
    
    g_io_context->stop();
    for(auto& thread : g_io_threads) {
       if(thread && thread->joinable()) {
           thread->join();
       }
    }
    g_io_threads.clear();
    g_io_context.reset();
}

EndPoint::EndPoint(unsigned short port):
m_endpoint(ip::tcp::v4(), (ip::port_type)port)
{
    getAsioContext();
}

ServerEP::ServerEP(unsigned short port):
EndPoint(port),
m_acceptor(*getAsioContext(), m_endpoint)
{
    std::cout << "Server starting on port " << port << std::endl;
        
    boost::system::error_code ec;
    m_acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        std::cout << "Set reuse_address failed: " << ec.message() << std::endl;
    }
    
    m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cout << "Listen failed: " << ec.message() << std::endl;
    }
    
    std::cout << "Acceptor state:"
              << "\n - is_open: " << m_acceptor.is_open()
              << "\n - local endpoint: " << m_acceptor.local_endpoint()
              << std::endl;
              
    startAccept();
}

void ServerEP::startAccept() {
    std::cout << "Server waiting for connections..." << std::endl;
    
    auto session = std::make_shared<RemoteEP>(tcp::socket(*getAsioContext()), *this);
    m_acceptor.async_accept(session->socket(),
        [this, session](const boost::system::error_code& error) {
        handleAccept(session, error);
        });
}

void ServerEP::disconnected(std::shared_ptr<RemoteEP> session)
{
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    m_sessions.erase(session);
}

void ServerEP::handleAccept(std::shared_ptr<RemoteEP> session, const boost::system::error_code& error) {
    
    std::cout << "HandleAccept called" << std::endl;  // Debug line
    
    if (error) {
        
        std::cout << "Accept error: " << error.message() << std::endl;
        
        // Log specific error conditions
        if (error == boost::asio::error::operation_aborted) {
            std::cerr << "Accept operation cancelled: " << error.message() << std::endl;
            return; // Don't continue accepting if deliberately cancelled
        }
        
        if (error == boost::asio::error::connection_reset) {
            std::cerr << "Connection reset during accept: " << error.message() << std::endl;
        } else {
            std::cerr << "Accept error: " << error.message() << std::endl;
        }
        
    } else {
        std::cout << "Connection accepted successfully" << std::endl;  // Debug line
        
        try {
        
            {
                std::lock_guard<std::mutex> lock(m_sessionsMutex);
                m_sessions.insert(session);
            }
            accept(session);
        }
        catch (const std::exception& e) {
            std::cerr << "Error processing accepted connection: " << e.what() << std::endl;
        }
    }
    
    // Only continue accepting if acceptor is still open
    if (m_acceptor.is_open()) {
        std::cout << "Starting next accept" << std::endl;  // Debug line
        startAccept();
    }
}

void ServerEP::startAsyncRead(std::shared_ptr<RemoteEP> session)
{
    auto msg = std::make_shared<Message>();
    
    boost::asio::async_read(session->socket(),
    boost::asio::buffer(&msg->m_size, sizeof(msg->m_size)),
    [this, session, msg](const boost::system::error_code& ec, std::size_t bytes_transferred) {
        if (!ec) {
            
            //std::cout << "Async reading message with size: " << sizeof(msg->m_size) << " transferred: " << bytes_transferred << std::endl;
           
            msg->resize();
    
            boost::asio::async_read(session->socket(),
                boost::asio::buffer(msg->m_storage.data(), msg->m_storage.size()),
                [this, session, msg](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                    //std::cout << "Async reading message with size: " << msg->m_storage.size() << " transferred: " << bytes_transferred << std::endl;
                    handleRead(ec, bytes_transferred, session, msg);
                });
        }
    });
}

void ServerEP::handleRead(const boost::system::error_code& ec, std::size_t bytes_transferred,
                          std::shared_ptr<RemoteEP> session, std::shared_ptr<Message> msg)
{
    if (ec)
    {
        if (ec == boost::asio::error::connection_reset ||
            ec == boost::asio::error::eof ||
            ec == boost::asio::error::broken_pipe)
        {
            disconnected(session);
        }
        return;
    }
    
    if(receive(session, msg))
    {
        startAsyncRead(session);
    }
}

void ServerEP::accept(std::shared_ptr<RemoteEP> remote)
{
    startAsyncRead(remote);
}

bool ServerEP::receive(std::shared_ptr<RemoteEP> remote, std::shared_ptr<Message> msg)
{
    
}

std::pair<std::string, std::string> getHostAndPort(const std::string& address)
{
    std::string host;
    std::string port;
    bool isFullUrl = address.find("://") != std::string::npos;
    if(isFullUrl)
    {
        auto result = boost::urls::parse_uri(address);
        host = result->host();
        port = result->port();
    }
    else
    {
        auto result = boost::urls::parse_authority(address);
        host = result->host();
        port = result->port();
    }
    
    return std::make_pair(host, port);
}

std::shared_ptr<RemoteEP> ClientEP::connectSession()
{
    std::cout << "Client starting on local port " << m_localPort << std::endl;
    std::cout << "Attempting to connect to " << m_serverAddress << std::endl;

    tcp::socket socket(*getAsioContext());
    socket.open(tcp::v4());

    socket.set_option(boost::asio::socket_base::linger(true, 300));
    socket.set_option(boost::asio::socket_base::keep_alive(true));
    socket.set_option(tcp::acceptor::reuse_address(true));

    boost::system::error_code ec;
    socket.bind(m_endpoint, ec);

    if (ec) {
        std::cout << "Bind failed with error: " << ec.message() << std::endl;
        throw std::runtime_error("Socket bind failed: " + ec.message());
    }

    auto session = std::make_shared<RemoteEP>(std::move(socket), *this);

    auto hostInfo = getHostAndPort(m_serverAddress);

    std::cout << "Resolving " << hostInfo.first << ":" << hostInfo.second << std::endl;

    tcp::resolver resolver(*getAsioContext());
    auto endpoints = resolver.resolve(hostInfo.first, hostInfo.second);

    std::cout << "Connecting..." << std::endl;
    boost::asio::connect(session->socket(), endpoints, ec);

    if (ec) {
        std::cout << "Connect failed with error: " << ec.message() << std::endl;
        throw std::runtime_error("Connect failed: " + ec.message());
    }

    std::cout << "Connected successfully" << std::endl;
    return session;
}

ClientEP::ClientEP(unsigned short localPort, const std::string serverAddress):
EndPoint(localPort),
m_localPort(localPort),
m_serverAddress(serverAddress)
{
    reconnect();
}

std::shared_ptr<RemoteEP> ClientEP::session()
{
    std::lock_guard<std::mutex> lock(m_sessionMutex);
    return m_session;
}

void ClientEP::reconnect()
{
    std::shared_ptr<RemoteEP> oldSession;
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        oldSession = m_session;
        m_session.reset();
    }

    if(oldSession)
    {
        boost::system::error_code ec;
        oldSession->socket().cancel(ec);
        oldSession->socket().shutdown(tcp::socket::shutdown_both, ec);
        oldSession->socket().close(ec);
    }

    auto newSession = connectSession();
    std::lock_guard<std::mutex> lock(m_sessionMutex);
    m_session = std::move(newSession);
}

void ClientEP::disconnected(std::shared_ptr<RemoteEP> session)
{
    std::lock_guard<std::mutex> lock(m_sessionMutex);
    if(m_session == session)
    {
        m_session.reset();
    }
}

RemoteEP::RemoteEP(tcp::socket socket, EndPoint& localEP):
m_socket(std::move(socket)),
m_localEP(localEP)
{
}

void RemoteEP::send(void* data, uint32_t size)
{
    std::lock_guard<std::mutex> lock(m_sendMutex);
    
    try {
        auto transferred = boost::asio::write(m_socket, boost::asio::buffer(&size, sizeof(size)));
        transferred = boost::asio::write(m_socket, boost::asio::buffer(data, size));
    }
    catch(const boost::system::system_error& e) {
       if(e.code() == boost::asio::error::connection_reset ||
          e.code() == boost::asio::error::eof ||
          e.code() == boost::asio::error::broken_pipe) {
           // Connection is dead
           m_socket.close();
           
           auto self(shared_from_this());
           m_localEP.disconnected(self);
           
           throw; // Re-throw to notify caller
       }
       throw; // Other errors
    }
}

unsigned short RemoteEP::remotePort()
{
    if(!m_socket.is_open())
    {
        return m_cachedRemotePort;
    }
    
    m_cachedRemotePort = m_socket.remote_endpoint().port();
    return m_cachedRemotePort;
}

void RemoteEP::startAsyncRead(std::promise<std::shared_ptr<Message>> promise)
{
    auto self = shared_from_this();

    struct ReadState : std::enable_shared_from_this<ReadState> {
        RemoteEP* that;
        std::shared_ptr<std::promise<std::shared_ptr<Message>>> p;
        std::shared_ptr<Message> msg;
        boost::asio::steady_timer timer;
        std::chrono::milliseconds timeout;
        bool done = false;

        ReadState(RemoteEP* t,
                  std::promise<std::shared_ptr<Message>> pr,
                  std::chrono::milliseconds to)
            : that(t)
            , p(std::make_shared<std::promise<std::shared_ptr<Message>>>(std::move(pr)))
            , msg(std::make_shared<Message>())
            , timer(that->m_socket.get_executor())
            , timeout(to) {}

        void arm_timer()
        {
            timer.expires_after(timeout);
            auto s = shared_from_this();
            timer.async_wait([s](const boost::system::error_code& ec) {
                if (!ec && !s->done) {
                    s->done = true;
                    // Report a timeout and abort the pending read
                    s->p->set_exception(std::make_exception_ptr(
                        std::runtime_error("read timeout")));
                    boost::system::error_code ignore;
                    s->that->m_socket.cancel(ignore);
                }
            });
        }

        void complete_ok()
        {
            if (done) return;
            done = true;
            timer.cancel();
            p->set_value(msg);
        }

        void complete_err(const boost::system::error_code& ec)
        {
            if (done) return;
            done = true;
            timer.cancel();
            p->set_exception(std::make_exception_ptr(boost::system::system_error(ec)));
        }

        void complete_bad_frame(std::uint32_t sz)
        {
            if (done) return;
            done = true;
            timer.cancel();
            p->set_exception(std::make_exception_ptr(
                std::runtime_error("bad frame length: " + std::to_string(sz))));
        }
    };

    // Long-horizon reasoning requests can legitimately take well beyond 5 minutes.
    auto st = std::make_shared<ReadState>(this, std::move(promise), std::chrono::seconds(300));

    // 1) Header
    st->arm_timer();
    boost::asio::async_read(
        m_socket,
        boost::asio::buffer(&st->msg->m_size, sizeof(st->msg->m_size)),
        [self, st](const boost::system::error_code& ec, std::size_t) {
            if (st->done) return;
            if (ec) { st->complete_err(ec); return; }

            static constexpr std::uint32_t kMaxFrame = 1u << 20; // 1 MiB
            if (st->msg->m_size == 0 || st->msg->m_size > kMaxFrame) {
                st->complete_bad_frame(st->msg->m_size);
                return;
            }

            st->msg->resize();

            // 2) Body
            st->arm_timer(); // re-arm for the body read
            boost::asio::async_read(
                st->that->m_socket,
                boost::asio::buffer(st->msg->m_storage.data(), st->msg->m_storage.size()),
                [self, st](const boost::system::error_code& ec2, std::size_t) {
                    if (st->done) return;
                    if (ec2) { st->complete_err(ec2); return; }
                    st->complete_ok();
                });
        });
}

std::shared_ptr<Message> RemoteEP::receive()
{
    // 1) Create a promise/future
    std::promise<std::shared_ptr<Message>> promise;
    auto future = promise.get_future();

    // 2) Start an async read, capturing the promise in a lambda
    startAsyncRead(std::move(promise));

    // 3) Block on the future until async reading is done
    try
    {
        return future.get();                    // may re-throw what the handler stored
    }
    catch (const std::exception& e)          // catches set_exception(...)
    {
        // optional diagnostics
        std::cerr << "RemoteEP::receive() failed: " << e.what() << '\n';
        return nullptr;                      // signal “no message” to the caller
    }
}

bool RemoteEP::hasDataToRead()
{
    boost::asio::socket_base::bytes_readable command(true);
    m_socket.io_control(command);
    return command.get() >= sizeof(uint32_t);
}

bool RemoteEP::isConnected()
{
    boost::system::error_code ec;
    return m_socket.is_open() &&
          m_socket.remote_endpoint(ec).port() != 0 &&
          !ec;
}

}
