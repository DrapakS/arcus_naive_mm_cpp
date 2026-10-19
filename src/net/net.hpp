// Websockets and HTTPS GET over Boost.Beast: ws:// and wss://, http:// and
// https://, through the HTTP proxy the environment names (as aiohttp with
// trust_env: WSS_PROXY / HTTPS_PROXY for wss, HTTPS_PROXY for https, the
// plain ones for ws / http; NO_PROXY and localhost go direct). TCP_NODELAY on
// every socket; no compression.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "util/aio.hpp"

namespace nmm {

struct Url {
    std::string scheme;  // ws, wss, http, https
    std::string host;
    std::string port;
    std::string target;  // path and query, "/" at least
    bool tls() const { return scheme == "wss" || scheme == "https"; }
    // Host header: the host, with the port unless it is the scheme's default.
    std::string host_header() const;
    static Url parse(const std::string& url);  // throws std::invalid_argument
};

// "a b" -> "a%20b": a query value.
std::string url_encode(std::string_view s);
// url + "?" + k=v&... (or "&" when the url has a query already).
std::string with_params(const std::string& url, const std::vector<std::pair<std::string, std::string>>& params);

class HttpError : public std::runtime_error {
public:
    HttpError(int status, const std::string& what) : std::runtime_error(what), status(status) {}
    int status;
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    // raise_for_status: throws HttpError for a 4xx or 5xx.
    void raise_for_status(const std::string& url) const;
};

// GET `url` within `timeout_s` in all; throws on a network failure or the timeout.
awaitable<HttpResponse> http_get(std::string url, std::vector<std::pair<std::string, std::string>> headers = {},
                                 double timeout_s = 10.0);

// One websocket connection.
class WsConnection {
public:
    virtual ~WsConnection() = default;

    // Connect and handshake within `timeout_s`. `heartbeat_s` (aiohttp's heartbeat):
    // a ping goes out after half of it with nothing received, and the connection is
    // dropped after all of it; 0 = none. Messages of any size.
    static awaitable<std::shared_ptr<WsConnection>> connect(std::string url, double heartbeat_s = 20.0,
                                                            double timeout_s = 10.0);

    // The next text (or binary) message; nullopt once the server has closed the
    // connection. Throws on an error, a heartbeat timeout or close().
    virtual awaitable<std::optional<std::string>> read() = 0;
    // Queue a text message: it goes out as soon as the ones before it are written.
    // `done(error)` is called once it is written (error empty) or failed. True when
    // the write started at once (nothing queued ahead of it): the frame is on its
    // way to the socket before send returns.
    virtual bool send(std::string text, std::function<void(const std::string&)> done = {}) = 0;
    // Close it at once: a read in progress fails.
    virtual void close() = 0;
    // A close handshake, then the socket closed (at most `timeout_s`).
    virtual awaitable<void> close_gracefully(double timeout_s = 1.0) = 0;
    virtual bool closed() const = 0;
    // The code of the server's close frame (0 when none came).
    virtual int close_code() const = 0;
};

// The process-wide TLS settings: the system's CAs, or SSL_CERT_FILE.
void init_tls();

}  // namespace nmm
