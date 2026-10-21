#include "http.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "core/log.h"
#include "core/util.h"

HttpServer g_http;
static const size_t kMaxHeader = 16 * 1024;
static const int64_t kMemBody = 256 * 1024;        // larger bodies spool to disk
static const int64_t kMaxBody = 96 * 1024 * 1024;  // firmware packages, models
static std::atomic<int> g_async_running{0};
static std::atomic<int> g_longpoll_running{0};

// ---------------------------------------------------------------- request / response helpers

std::string HttpRequest::header(const std::string &k, const std::string &def) const {
    auto it = headers.find(k);
    return it == headers.end() ? def : it->second;
}

std::string HttpRequest::param(const std::string &k, const std::string &def) const {
    auto it = params.find(k);
    return it == params.end() ? def : it->second;
}

json HttpRequest::json_body() const {
    if (!body_file.empty()) {
        std::string s;
        read_file(body_file, s);
        return json::parse(s);
    }
    return json::parse(body.empty() ? "{}" : body);
}

void HttpResponse::send_json(const json &j, int code) {
    status = code;
    headers["Content-Type"] = "application/json";
    body = j.dump(-1, ' ', false, json::error_handler_t::replace);
}

void HttpResponse::error(int code, const std::string &msg) { send_json({{"error", msg}}, code); }

static const char *status_text(int c) {
    switch (c) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 423: return "Locked";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string mime_type(const std::string &p) {
    static const std::pair<const char *, const char *> m[] = {
        {".html", "text/html; charset=utf-8"}, {".js", "application/javascript"}, {".css", "text/css"},
        {".json", "application/json"},         {".svg", "image/svg+xml"},         {".png", "image/png"},
        {".jpg", "image/jpeg"},                {".jpeg", "image/jpeg"},           {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},              {".mp4", "video/mp4"},             {".txt", "text/plain"},
        {".webmanifest", "application/manifest+json"}};
    for (auto &e : m)
        if (ends_with(p, e.first)) return e.second;
    return "application/octet-stream";
}

// ---------------------------------------------------------------- digest / basic

static std::string nonce_key() {
    static std::string k = random_token(16);
    return k;
}

std::string make_nonce() {
    char ts[24];
    snprintf(ts, sizeof(ts), "%llx", (unsigned long long)now_ms());
    return std::string(ts) + sha256_hex(std::string(ts) + nonce_key()).substr(0, 16);
}

static bool nonce_valid(const std::string &n, bool *stale) {
    *stale = false;
    if (n.size() < 17) return false;
    std::string ts = n.substr(0, n.size() - 16);
    if (sha256_hex(ts + nonce_key()).substr(0, 16) != n.substr(n.size() - 16)) return false;
    int64_t t = strtoll(ts.c_str(), nullptr, 16);
    if (now_ms() - t > 30 * 60 * 1000) {
        *stale = true;
        return false;
    }
    return true;
}

std::string digest_challenge(bool stale) {
    std::string s = std::string("Digest realm=\"") + auth::realm() + "\", nonce=\"" + make_nonce() + "\", qop=\"auth\"";
    if (stale) s += ", stale=TRUE";
    return s;
}

static std::map<std::string, std::string> parse_kv(const std::string &s) {
    std::map<std::string, std::string> kv;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string k = to_lower(trim(s.substr(i, eq - i)));
        i = eq + 1;
        std::string v;
        if (i < s.size() && s[i] == '"') {
            size_t e = s.find('"', i + 1);
            v = s.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
            i = e == std::string::npos ? s.size() : e + 1;
        } else {
            size_t e = s.find(',', i);
            v = trim(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
            i = e == std::string::npos ? s.size() : e;
        }
        kv[k] = v;
    }
    return kv;
}

auth::Role verify_authorization(const std::string &authz, const std::string &method, std::string *user,
                                bool allow_basic) {
    if (starts_with(authz, "Basic ") && allow_basic) {
        std::string up = base64_decode(authz.substr(6));
        size_t c = up.find(':');
        if (c == std::string::npos) return auth::NONE;
        auth::Role r = auth::check_password(up.substr(0, c), up.substr(c + 1));
        if (r != auth::NONE && user) *user = up.substr(0, c);
        return r;
    }
    if (starts_with(authz, "Digest ")) {
        auto kv = parse_kv(authz.substr(7));
        std::string u = kv["username"], ha1 = auth::ha1(u);
        bool stale;
        if (ha1.empty() || !nonce_valid(kv["nonce"], &stale)) return auth::NONE;
        std::string ha2 = md5_hex(method + ":" + kv["uri"]);
        std::string want = kv.count("qop") && !kv["qop"].empty()
                               ? md5_hex(ha1 + ":" + kv["nonce"] + ":" + kv["nc"] + ":" + kv["cnonce"] + ":" + kv["qop"] + ":" + ha2)
                               : md5_hex(ha1 + ":" + kv["nonce"] + ":" + ha2);
        if (!const_time_eq(want, to_lower(kv["response"]))) return auth::NONE;
        if (user) *user = u;
        // role comes from the user record
        for (auto &x : auth::list_users())
            if (x.value("name", "") == u) return auth::role_from(x.value("role", ""));
    }
    return auth::NONE;
}

// ---------------------------------------------------------------- connection

class HttpConn : public Conn {
public:
    HttpConn(int fd, const std::string &peer, HttpServer *srv) : Conn(fd, peer), srv_(srv) {}
    ~HttpConn() override {
        if (spool_fd_ >= 0) ::close(spool_fd_);
        if (!req_.body_file.empty()) unlink(req_.body_file.c_str());
    }

    void respond(HttpRequest &req, HttpResponse &res);
    void upgrade_ws(std::shared_ptr<WsSession> s);
    void ws_send(uint8_t opcode, const std::string &header_extra, std::shared_ptr<const std::string> payload);
    void ws_close();
    bool is_ws() const { return ws_ != nullptr; }
    bool idle_exempt() const override { return ws_ != nullptr || busy_; }
    bool busy_ = false;  // async handler running
    // an async handler answered: continue with pipelined requests, if any
    void finish_async() {
        busy_ = false;
        if (closed()) return;
        reset_request();
        if (!in_.empty()) on_data(in_);
    }

protected:
    void on_data(std::string &in) override;
    void on_close() override {
        if (ws_) ws_->on_close();
        ws_.reset();
    }

private:
    bool parse_head(std::string &in);
    void reset_request();
    void ws_data(std::string &in);

    HttpServer *srv_;
    enum { HEAD, BODY } state_ = HEAD;
    HttpRequest req_;
    int64_t body_left_ = 0;
    int spool_fd_ = -1;
    bool keep_alive_ = true;
    std::shared_ptr<WsSession> ws_;
    std::string ws_frag_;
};

void HttpConn::reset_request() {
    if (spool_fd_ >= 0) ::close(spool_fd_);
    spool_fd_ = -1;
    if (!req_.body_file.empty()) unlink(req_.body_file.c_str());
    req_ = HttpRequest();
    req_.peer = peer_;
    req_.tls = is_tls();
    state_ = HEAD;
    body_left_ = 0;
}

bool HttpConn::parse_head(std::string &in) {
    size_t end = in.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (in.size() > kMaxHeader) {
            HttpResponse r;
            r.error(400, "header too large");
            keep_alive_ = false;
            respond(req_, r);
        }
        return false;
    }
    std::string head = in.substr(0, end);
    in.erase(0, end + 4);
    auto lines = split(head, '\n');
    auto first = split(trim(lines[0]), ' ');
    if (first.size() < 3) {
        close();
        return false;
    }
    req_.method = first[0];
    std::string target = first[1];
    req_.version = first[2];
    size_t q = target.find('?');
    req_.path = url_decode(target.substr(0, q));
    if (q != std::string::npos) {
        req_.query = target.substr(q + 1);
        for (auto &kv : split(req_.query, '&')) {
            size_t e = kv.find('=');
            if (kv.empty()) continue;
            req_.params[url_decode(kv.substr(0, e))] = e == std::string::npos ? "" : url_decode(kv.substr(e + 1));
        }
    }
    for (size_t i = 1; i < lines.size(); ++i) {
        size_t c = lines[i].find(':');
        if (c == std::string::npos) continue;
        req_.headers[to_lower(trim(lines[i].substr(0, c)))] = trim(lines[i].substr(c + 1));
    }
    std::string conn = to_lower(req_.header("connection"));
    keep_alive_ = req_.version == "HTTP/1.1" ? conn.find("close") == std::string::npos : conn.find("keep-alive") != std::string::npos;
    req_.body_len = atoll(req_.header("content-length", "0").c_str());
    if (req_.body_len < 0 || req_.body_len > kMaxBody) {
        HttpResponse r;
        r.error(413, "body too large");
        keep_alive_ = false;
        respond(req_, r);
        return false;
    }
    body_left_ = req_.body_len;
    if (req_.body_len > kMemBody) {
        std::string dir = g_paths.data + "/tmp";
        mkdirs(dir);
        req_.body_file = dir + "/upload-" + random_token(6);
        spool_fd_ = open(req_.body_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (spool_fd_ < 0) {
            HttpResponse r;
            r.error(500, "cannot store upload");
            keep_alive_ = false;
            respond(req_, r);
            return false;
        }
    }
    state_ = BODY;
    return true;
}

void HttpConn::on_data(std::string &in) {
    if (ws_) {
        ws_data(in);
        return;
    }
    while (!closed() && !busy_) {
        if (state_ == HEAD) {
            if (!parse_head(in)) return;
        }
        if (state_ == BODY) {
            int64_t take = std::min<int64_t>(body_left_, (int64_t)in.size());
            if (take > 0) {
                if (spool_fd_ >= 0) {
                    if (write(spool_fd_, in.data(), take) != take) {
                        HttpResponse r;
                        r.error(507, "storage full");
                        keep_alive_ = false;
                        respond(req_, r);
                        return;
                    }
                } else {
                    req_.body.append(in, 0, take);
                }
                in.erase(0, take);
                body_left_ -= take;
            }
            if (body_left_ > 0) return;
            if (spool_fd_ >= 0) {
                ::close(spool_fd_);
                spool_fd_ = -1;
            }
            HttpRequest req = req_;
            req_.body_file.clear();  // ownership moves to the request being dispatched
            auto self = std::static_pointer_cast<HttpConn>(shared_from_this());
            srv_->dispatch(self, req);
            if (ws_) {
                if (!in.empty()) ws_data(in);
                return;
            }
            if (!busy_) reset_request();
        }
    }
}

void HttpConn::respond(HttpRequest &req, HttpResponse &res) {
    if (closed()) return;
    std::string h = "HTTP/1.1 " + std::to_string(res.status) + " " + status_text(res.status) + "\r\n";
    int64_t len = res.file.empty() ? (int64_t)res.body.size() : res.file_len;
    if (!res.headers.count("Content-Type")) res.headers["Content-Type"] = "text/plain";
    res.headers["Content-Length"] = std::to_string(len);
    res.headers["Connection"] = keep_alive_ ? "keep-alive" : "close";
    if (!res.headers.count("Cache-Control")) res.headers["Cache-Control"] = "no-store";
    res.headers["X-Content-Type-Options"] = "nosniff";
    for (auto &kv : res.headers) h += kv.first + ": " + kv.second + "\r\n";
    h += "\r\n";
    bool head_only = req.method == "HEAD" || res.no_body;
    if (!head_only && res.file.empty()) h += res.body;
    send(std::move(h));
    if (!head_only && !res.file.empty() && len > 0) send_file(res.file, res.file_off, len);
    if (!req.body_file.empty()) unlink(req.body_file.c_str());
    if (!keep_alive_) close_after_send();
}

// ---------------------------------------------------------------- websocket

void HttpConn::upgrade_ws(std::shared_ptr<WsSession> s) {
    std::string key = req_.header("sec-websocket-key");
    std::string accept = base64_encode(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    send("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
         accept + "\r\n\r\n");
    ws_ = s;
    s->conn = std::static_pointer_cast<HttpConn>(shared_from_this());
    s->req = req_;
    s->on_open();
}

static std::string ws_header(uint8_t opcode, size_t len) {
    std::string h;
    h += (char)(0x80 | opcode);
    if (len < 126) {
        h += (char)len;
    } else if (len < 65536) {
        h += (char)126;
        h += (char)(len >> 8);
        h += (char)(len & 0xff);
    } else {
        h += (char)127;
        for (int i = 7; i >= 0; --i) h += (char)((uint64_t)len >> (i * 8));
    }
    return h;
}

void HttpConn::ws_send(uint8_t opcode, const std::string &extra, std::shared_ptr<const std::string> payload) {
    size_t len = extra.size() + (payload ? payload->size() : 0);
    send(ws_header(opcode, len) + extra);
    if (payload && !payload->empty()) send(payload);
}

void HttpConn::ws_close() {
    send(ws_header(8, 0));
    close_after_send();
}

void HttpConn::ws_data(std::string &in) {
    while (in.size() >= 2) {
        const uint8_t *p = (const uint8_t *)in.data();
        bool fin = p[0] & 0x80;
        int op = p[0] & 0x0f;
        bool masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7f;
        size_t off = 2;
        if (len == 126) {
            if (in.size() < 4) return;
            len = (p[2] << 8) | p[3];
            off = 4;
        } else if (len == 127) {
            if (in.size() < 10) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            off = 10;
        }
        if (len > 1024 * 1024) {
            close();
            return;
        }
        if (in.size() < off + (masked ? 4 : 0) + len) return;
        std::string data = in.substr(off + (masked ? 4 : 0), len);
        if (masked)
            for (size_t i = 0; i < data.size(); ++i) data[i] ^= p[off + (i & 3)];
        in.erase(0, off + (masked ? 4 : 0) + len);
        if (op == 8) {  // close
            ws_close();
            return;
        } else if (op == 9) {  // ping
            ws_send(10, data, nullptr);
        } else if (op == 1 || op == 2 || op == 0) {
            ws_frag_ += data;
            if (fin) {
                std::string msg;
                msg.swap(ws_frag_);
                if (ws_) ws_->on_text(msg);
            }
        }
    }
}

void WsSession::send_text(const std::string &s) {
    if (auto c = conn.lock()) c->ws_send(1, s, nullptr);
}

void WsSession::send_binary(const std::string &header, std::shared_ptr<const std::string> payload) {
    if (auto c = conn.lock()) c->ws_send(2, header, payload);
}

size_t WsSession::pending() const {
    auto c = conn.lock();
    return c ? c->pending() : 0;
}

bool WsSession::alive() const {
    auto c = conn.lock();
    return c && !c->closed();
}

void WsSession::close() {
    if (auto c = conn.lock()) c->ws_close();
}

// ---------------------------------------------------------------- server

void HttpServer::route(const std::string &method, const std::string &path, auth::Role min_role, Handler h, bool async,
                       bool longpoll) {
    routes_.push_back({method, path, !path.empty() && path.back() == '/', min_role, std::move(h), async || longpoll, longpoll});
}

void HttpServer::websocket(const std::string &path, auth::Role min_role, WsFactory f) {
    ws_.push_back({path, min_role, std::move(f)});
}

bool HttpServer::start(int port, const std::string &www) {
    www_ = www;
    return listener_.listen_tcp(port, [this](int fd, const std::string &peer) {
        return std::make_shared<HttpConn>(fd, peer, this);
    });
}

bool HttpServer::start_tls(int port) {
    return tls_listener_.listen_tcp(
        port, [this](int fd, const std::string &peer) { return std::make_shared<HttpConn>(fd, peer, this); }, true);
}

void HttpServer::stop() {
    listener_.close();
    tls_listener_.close();
}

static std::string cookie(const HttpRequest &r, const std::string &name) {
    for (auto &kv : split(r.header("cookie"), ';')) {
        std::string t = trim(kv);
        if (starts_with(t, name + "=")) return t.substr(name.size() + 1);
    }
    return "";
}

// Who is calling: session cookie (web UI), bearer token / API key, ?token= (WebSocket, <img> URLs), Basic/Digest.
static void authenticate(HttpRequest &req) {
    std::string tok = cookie(req, "pc_session");
    std::string authz = req.header("authorization");
    if (tok.empty() && starts_with(authz, "Bearer ")) tok = authz.substr(7);
    if (tok.empty()) tok = req.param("token");
    if (!tok.empty()) {
        req.role = starts_with(tok, "pk_") ? auth::check_api_key(tok, &req.user) : auth::check_session(tok, &req.user);
        if (req.role != auth::NONE) return;
    }
    if (!authz.empty() && !starts_with(authz, "Bearer ")) {
        if (auth::locked_out(req.peer)) return;
        req.role = verify_authorization(authz, req.method, &req.user);
        if (req.role != auth::NONE) auth::note_success(req.peer);
        else auth::note_failure(req.peer);
    }
}

void HttpServer::dispatch(std::shared_ptr<HttpConn> c, HttpRequest &req) {
    HttpResponse res;
    req.tls = c->is_tls();
    // optional: browsers on plain HTTP go to HTTPS (NVR/ONVIF/snapshot clients are left alone)
    if (redirect_https && !req.tls && req.method == "GET" && !starts_with(req.path, "/onvif") && !starts_with(req.path, "/api/") &&
        req.path != "/snapshot.jpg" && !starts_with(req.path, "/ISAPI") && to_lower(req.header("upgrade")) != "websocket" &&
        req.peer != "127.0.0.1") {
        std::string host = req.header("host");
        host = host.substr(0, host.find(':'));
        res.status = 301;
        res.headers["Location"] = "https://" + host + (https_port == 443 ? "" : ":" + std::to_string(https_port)) + req.path +
                                  (req.query.empty() ? "" : "?" + req.query);
        res.body = "";
        c->respond(req, res);
        return;
    }
    authenticate(req);

    // WebSocket upgrades
    if (to_lower(req.header("upgrade")) == "websocket") {
        for (auto &w : ws_) {
            if (w.path != req.path) continue;
            if (req.role < w.min_role) {
                res.error(401, "unauthorized");
                c->respond(req, res);
                return;
            }
            auto s = w.f(req);
            if (!s) {
                res.error(400, "bad request");
                c->respond(req, res);
                return;
            }
            c->upgrade_ws(s);
            return;
        }
        res.error(404, "no such websocket");
        c->respond(req, res);
        return;
    }

    for (auto &r : routes_) {
        if (r.method != req.method && !(r.method == "GET" && req.method == "HEAD")) continue;
        if (r.prefix ? !starts_with(req.path, r.path) : req.path != r.path) continue;
        if (r.prefix) {
            req.path_args.clear();
            for (auto &s : split(req.path.substr(r.path.size()), '/'))
                if (!s.empty()) req.path_args.push_back(s);
        }
        if (req.role < r.min_role) {
            if (req.role == auth::NONE) {
                res.error(401, "login required");
                // browsers would pop up a password dialog; only challenge non-UI clients (NVRs, curl)
                if (req.header("x-picocam-ui").empty()) res.headers["WWW-Authenticate"] = digest_challenge();
            } else {
                res.error(403, "insufficient rights");
            }
            c->respond(req, res);
            return;
        }
        if (!r.async) {
            try {
                r.h(req, res);
            } catch (const std::exception &e) {
                res.error(400, e.what());
            }
            c->respond(req, res);
            return;
        }
        // slow handler: run off the loop thread, answer when done
        std::atomic<int> &running = r.longpoll ? g_longpoll_running : g_async_running;
        if (running >= (r.longpoll ? 8 : 3)) {
            res.error(503, "busy, try again");
            c->respond(req, res);
            return;
        }
        ++running;
        c->busy_ = true;
        std::weak_ptr<HttpConn> wc = c;
        Handler h = r.h;
        std::atomic<int> *counter = &running;
        std::thread([wc, h, req, counter]() mutable {
            HttpResponse res;
            try {
                h(req, res);
            } catch (const std::exception &e) {
                res.error(400, e.what());
            }
            --*counter;
            g_loop.post([wc, req, res]() mutable {
                auto c = wc.lock();
                if (!c) {
                    if (!req.body_file.empty()) unlink(req.body_file.c_str());
                    return;
                }
                c->respond(req, res);
                c->finish_async();
            });
        }).detach();
        return;
    }

    if (req.method == "GET" || req.method == "HEAD") {
        serve_static(req, res);
    } else {
        res.error(404, "not found");
    }
    c->respond(req, res);
}

void HttpServer::serve_static(HttpRequest &req, HttpResponse &res) {
    std::string p = req.path;
    if (p.find("..") != std::string::npos || starts_with(p, "/api/")) {
        res.error(404, "not found");
        return;
    }
    if (p == "/" || p.find('.') == std::string::npos) p = "/index.html";  // SPA routes
    std::string full = www_ + p;
    bool gz = false;
    if (req.header("accept-encoding").find("gzip") != std::string::npos && file_exists(full + ".gz")) {
        full += ".gz";
        gz = true;
    }
    struct stat st;
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        res.error(404, "not found");
        return;
    }
    std::string etag = "\"" + std::to_string(st.st_size) + "-" + std::to_string(st.st_mtime) + "\"";
    res.headers["ETag"] = etag;
    res.headers["Content-Type"] = mime_type(p);
    if (gz) res.headers["Content-Encoding"] = "gzip";
    res.headers["Vary"] = "Accept-Encoding";
    // hashed build assets never change; the shell must always revalidate
    res.headers["Cache-Control"] = starts_with(p, "/assets/") ? "public, max-age=31536000, immutable" : "no-cache";
    if (req.header("if-none-match") == etag) {
        res.status = 304;
        res.no_body = true;
        res.headers.erase("Content-Type");
        return;
    }
    res.file = full;
    res.file_len = st.st_size;
}
