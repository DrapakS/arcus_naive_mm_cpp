#include "fake_server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <format>

namespace nmm::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using clock_type = std::chrono::steady_clock;

namespace {

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

std::string decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

HttpRequest convert(const http::request<http::string_body>& req) {
    HttpRequest r;
    r.method = std::string(req.method_string());
    r.target = std::string(req.target());
    auto q = r.target.find('?');
    r.path = r.target.substr(0, q);
    if (q != std::string::npos) {
        std::string query = r.target.substr(q + 1);
        size_t start = 0;
        while (start <= query.size()) {
            size_t amp = query.find('&', start);
            std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (!pair.empty()) {
                auto eq = pair.find('=');
                r.query[decode(pair.substr(0, eq))] = eq == std::string::npos ? "" : decode(pair.substr(eq + 1));
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
    }
    for (const auto& field : req) {
        std::string name(field.name_string());
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        r.headers[name] = std::string(field.value());
    }
    r.body = req.body();
    return r;
}

template <class Handler>
const Handler* match(const std::vector<std::pair<std::string, Handler>>& routes, const std::string& path) {
    for (const auto& [pattern, handler] : routes) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (path.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0) return &handler;
        } else if (pattern == path) {
            return &handler;
        }
    }
    return nullptr;
}

class ServerWsImpl : public ServerWs {
public:
    explicit ServerWsImpl(beast::tcp_stream stream) : ws_(std::move(stream)), timer_(ws_.get_executor()) {}

    websocket::stream<beast::tcp_stream>& ws() { return ws_; }

    void send(std::string frame) override { enqueue(std::move(frame), clock_type::now() + to_duration(delay)); }

    void send_now(std::string frame) override { enqueue(std::move(frame), clock_type::now()); }

    void close() override {
        if (closed_ || closing_) return;
        closing_ = true;
        ws_.async_close(websocket::close_code::normal,
                        [self = std::static_pointer_cast<ServerWsImpl>(shared_from_this())](beast::error_code) {
                            self->finish();
                        });
    }

    void abort() override {
        if (closed_) return;
        beast::get_lowest_layer(ws_).close();
        finish();
    }

    bool closed() const override { return closed_ || closing_; }

    asio::awaitable<void> read_loop() {
        auto self = std::static_pointer_cast<ServerWsImpl>(shared_from_this());
        beast::flat_buffer buffer;
        for (;;) {
            auto [ec, n] = co_await ws_.async_read(buffer, use_tuple);
            if (ec) break;
            std::string text = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            if (on_message && !closing_) {
                auto callback = on_message;  // it may replace itself
                callback(text);
            }
        }
        finish();
    }

    void finish() {
        if (closed_) return;
        closed_ = true;
        timer_.cancel();
        queue_.clear();
        auto callback = std::move(on_close);
        on_close = nullptr;
        on_message = nullptr;
        if (callback) callback();
    }

private:
    static clock_type::duration to_duration(double s) {
        return std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(s));
    }

    void enqueue(std::string frame, clock_type::time_point due) {
        if (closed_ || closing_) return;
        // in order of when each is due; frames due at the same time keep their order
        auto it = queue_.end();
        while (it != queue_.begin() && std::prev(it)->first > due) --it;
        queue_.insert(it, {due, std::move(frame)});
        if (!writing_) {
            writing_ = true;
            asio::co_spawn(ws_.get_executor(), write_loop(), asio::detached);
        } else {
            timer_.cancel();  // something may be due earlier than what the writer waits for
        }
    }

    asio::awaitable<void> write_loop() {
        auto self = std::static_pointer_cast<ServerWsImpl>(shared_from_this());
        while (!queue_.empty() && !closed_ && !closing_) {
            auto due = queue_.front().first;
            if (due > clock_type::now()) {
                timer_.expires_at(due);
                co_await timer_.async_wait(use_tuple);
                continue;  // look again: an earlier frame may have come
            }
            std::string frame = std::move(queue_.front().second);
            queue_.pop_front();
            ws_.text(true);
            auto [ec, n] = co_await ws_.async_write(asio::buffer(frame), use_tuple);
            if (ec) break;
        }
        writing_ = false;
    }

    websocket::stream<beast::tcp_stream> ws_;
    asio::steady_timer timer_;
    std::deque<std::pair<clock_type::time_point, std::string>> queue_;
    bool writing_ = false;
    bool closing_ = false;
    bool closed_ = false;
};

}  // namespace

struct FakeServer::Impl : std::enable_shared_from_this<FakeServer::Impl> {
    explicit Impl(asio::io_context& ioc) : ioc(ioc), acceptor(ioc) {}

    asio::io_context& ioc;
    tcp::acceptor acceptor;
    std::vector<std::pair<std::string, GetHandler>> gets;
    std::vector<std::pair<std::string, WsHandler>> sockets;
    std::vector<std::shared_ptr<ServerWs>>* connections = nullptr;
    std::vector<std::weak_ptr<beast::tcp_stream>> http_streams;
    bool open = false;

    asio::awaitable<void> accept_loop() {
        auto self = shared_from_this();
        while (open) {
            auto [ec, socket] = co_await acceptor.async_accept(use_tuple);
            if (ec || !open) break;
            socket.set_option(tcp::no_delay(true));
            asio::co_spawn(ioc, session(std::move(socket)), asio::detached);
        }
    }

    asio::awaitable<void> session(tcp::socket socket) {
        auto self = shared_from_this();
        auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
        http_streams.push_back(stream);
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        auto [ec, n] = co_await http::async_read(*stream, buffer, parser, use_tuple);
        if (ec || !open) co_return;
        http::request<http::string_body> req = parser.release();
        HttpRequest request = convert(req);
        if (websocket::is_upgrade(req)) {
            const WsHandler* handler = match(sockets, request.path);
            if (handler == nullptr) {
                co_await reply(*stream, HttpReply(404, "not found", "text/plain"));
                co_return;
            }
            std::erase_if(http_streams, [&](const std::weak_ptr<beast::tcp_stream>& w) { return w.lock() == stream; });
            auto conn = std::make_shared<ServerWsImpl>(std::move(*stream));
            conn->request = request;
            auto [aec] = co_await conn->ws().async_accept(req, use_tuple);
            if (aec || !open) co_return;
            if (connections != nullptr) connections->push_back(conn);
            (*handler)(conn);
            co_await conn->read_loop();
            co_return;
        }
        const GetHandler* handler = match(gets, request.path);
        HttpReply answer = handler != nullptr ? (*handler)(request) : HttpReply(404, "not found", "text/plain");
        if (answer.delay_s > 0) {
            asio::steady_timer timer(ioc);
            timer.expires_after(std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(answer.delay_s)));
            co_await timer.async_wait(use_tuple);
        }
        if (open) co_await reply(*stream, answer);
    }

    asio::awaitable<void> reply(beast::tcp_stream& stream, const HttpReply& answer) {
        http::response<http::string_body> res {static_cast<http::status>(answer.status), 11};
        res.set(http::field::content_type, answer.content_type);
        res.set(http::field::connection, "close");
        for (const auto& [name, value] : answer.headers) res.set(name, value);
        res.body() = answer.body;
        res.prepare_payload();
        co_await http::async_write(stream, res, use_tuple);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
};

FakeServer::FakeServer(asio::io_context& ioc) : ioc_(ioc), impl_(std::make_shared<Impl>(ioc)) {
    impl_->connections = &connections;
}

FakeServer::~FakeServer() { close(); }

void FakeServer::start() {
    tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
    impl_->acceptor.open(endpoint.protocol());
    impl_->acceptor.set_option(tcp::acceptor::reuse_address(true));
    impl_->acceptor.bind(endpoint);
    impl_->acceptor.listen();
    port_ = impl_->acceptor.local_endpoint().port();
    impl_->open = true;
    asio::co_spawn(ioc_, impl_->accept_loop(), asio::detached);
}

void FakeServer::close() {
    if (!impl_->open) return;
    impl_->open = false;
    impl_->connections = nullptr;
    beast::error_code ec;
    impl_->acceptor.close(ec);
    for (auto& conn : connections) conn->abort();
    for (auto& weak : impl_->http_streams)
        if (auto stream = weak.lock()) stream->socket().close(ec);
    impl_->gets.clear();
    impl_->sockets.clear();
}

void FakeServer::on_get(std::string path, GetHandler handler) { impl_->gets.emplace_back(std::move(path), std::move(handler)); }

void FakeServer::on_ws(std::string path, WsHandler handler) { impl_->sockets.emplace_back(std::move(path), std::move(handler)); }

std::string FakeServer::base() const { return std::format("127.0.0.1:{}", port_); }

}  // namespace nmm::test
