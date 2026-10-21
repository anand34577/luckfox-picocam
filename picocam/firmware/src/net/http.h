#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/auth.h"
#include "core/config.h"
#include "loop.h"

struct HttpRequest {
    std::string method, path, query, version, peer;
    std::map<std::string, std::string> headers;  // lower-case names
    std::map<std::string, std::string> params;   // decoded query parameters
    std::string body;                            // small bodies
    std::string body_file;                       // large bodies are spooled to this file
    int64_t body_len = 0;
    auth::Role role = auth::NONE;
    std::string user;
    bool tls = false;  // arrived over HTTPS
    std::vector<std::string> path_args;  // segments matched by a prefix route ("/api/x/" + args)

    std::string header(const std::string &k, const std::string &def = "") const;
    std::string param(const std::string &k, const std::string &def = "") const;
    json json_body() const;  // throws on bad JSON
};

struct HttpResponse {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string file;  // serve this file instead of body
    int64_t file_off = 0, file_len = -1;
    bool no_body = false;

    void send_json(const json &j, int code = 200);
    void error(int code, const std::string &msg);
    void ok() { send_json({{"ok", true}}); }
};

class HttpConn;

// WebSocket endpoint session (one per connection).
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    virtual ~WsSession() {}
    virtual void on_open() {}
    virtual void on_text(const std::string &) {}
    virtual void on_close() {}
    void send_text(const std::string &s);
    void send_binary(const std::string &header, std::shared_ptr<const std::string> payload);
    size_t pending() const;
    bool alive() const;
    void close();

    std::weak_ptr<HttpConn> conn;
    HttpRequest req;
};

class HttpServer {
public:
    using Handler = std::function<void(HttpRequest &, HttpResponse &)>;
    using WsFactory = std::function<std::shared_ptr<WsSession>(const HttpRequest &)>;

    // exact path, or prefix when the path ends with '/'
    // async: run on a worker thread. longpoll: may hold the request for many seconds (own, larger pool so it can't
    // starve the short async handlers)
    void route(const std::string &method, const std::string &path, auth::Role min_role, Handler h, bool async = false,
               bool longpoll = false);
    void websocket(const std::string &path, auth::Role min_role, WsFactory f);
    bool start(int port, const std::string &www);
    bool start_tls(int port);  // HTTPS / WSS (needs tls::init())
    bool redirect_https = false;  // answer plain-HTTP browser requests with a redirect to https://
    int https_port = 443;
    void stop();
    void stop_plain() { listener_.close(); }
    void stop_tls() { tls_listener_.close(); }

    // internal
    struct Route {
        std::string method, path;
        bool prefix;
        auth::Role min_role;
        Handler h;
        bool async;
        bool longpoll;
    };
    struct WsRoute {
        std::string path;
        auth::Role min_role;
        WsFactory f;
    };
    void dispatch(std::shared_ptr<HttpConn> c, HttpRequest &req);
    std::string www_;

private:
    void serve_static(HttpRequest &req, HttpResponse &res);
    std::vector<Route> routes_;
    std::vector<WsRoute> ws_;
    Listener listener_, tls_listener_;
};

extern HttpServer g_http;

// HTTP/RTSP Digest + Basic credential checks (shared with the RTSP server)
std::string make_nonce();
auth::Role verify_authorization(const std::string &authz, const std::string &method, std::string *user,
                                bool allow_basic = true);
std::string digest_challenge(bool stale = false);
std::string mime_type(const std::string &path);
