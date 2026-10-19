// A small HTTP and websocket server on 127.0.0.1 for the tests: fake exchanges
// run on the same event loop as the code under test.
//
// GET handlers by path answer with a status and a body (optionally after a
// delay). Websocket handlers by path get each accepted connection: they send
// text frames (each connection may hold its frames back by an extra delay, its
// "route", in order), get every frame the client sends (on_message), and can
// close the connection (a kick). A path ending in '*' matches every path with
// that prefix ("/v1/l2OrderBook/*").
#pragma once

#include <boost/asio/io_context.hpp>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nmm::test {

struct HttpRequest {
    std::string method;
    std::string path;  // without the query
    std::string target;  // as sent: path and query
    std::map<std::string, std::string> query;  // decoded
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
};

struct HttpReply {
    HttpReply() = default;
    HttpReply(int status, std::string body, std::string content_type = "application/json")
        : status(status), body(std::move(body)), content_type(std::move(content_type)) {}

    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    double delay_s = 0.0;  // answer this much later
    std::map<std::string, std::string> headers;
};

// One accepted websocket connection.
class ServerWs : public std::enable_shared_from_this<ServerWs> {
public:
    virtual ~ServerWs() = default;
    // Send `frame` after `delay` (the connection's own route delay), in order.
    virtual void send(std::string frame) = 0;
    // Send `frame` now: it goes ahead of delayed frames not yet due (as a direct
    // send on the connection, outside the delayed queue).
    virtual void send_now(std::string frame) = 0;
    // A close frame (code 1000), then the connection ends: the server kicks the client.
    virtual void close() = 0;
    // Drop the connection at once, without a close frame.
    virtual void abort() = 0;
    virtual bool closed() const = 0;

    HttpRequest request;  // the upgrade request
    double delay = 0.0;  // extra hold on each frame sent with send()
    int index = -1;  // free for the fakes: the order clients joined in
    std::function<void(const std::string&)> on_message;  // every text frame the client sends
    std::function<void()> on_close;  // once the connection is over
};

class FakeServer {
public:
    using GetHandler = std::function<HttpReply(const HttpRequest&)>;
    using WsHandler = std::function<void(std::shared_ptr<ServerWs>)>;

    explicit FakeServer(boost::asio::io_context& ioc);
    virtual ~FakeServer();
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    // Listen on 127.0.0.1, a free port.
    void start();
    // Stop listening and drop every connection.
    virtual void close();

    void on_get(std::string path, GetHandler handler);
    void on_ws(std::string path, WsHandler handler);

    unsigned short port() const { return port_; }
    std::string base() const;  // "127.0.0.1:<port>"

    std::vector<std::shared_ptr<ServerWs>> connections;  // every websocket accepted, in order

protected:
    boost::asio::io_context& ioc_;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    unsigned short port_ = 0;
};

}  // namespace nmm::test
