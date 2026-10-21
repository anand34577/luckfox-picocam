#include "httpc.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>

#include "core/util.h"

namespace httpc {

bool parse_url(const std::string &url, Url &u) {
    std::string s = url;
    if (starts_with(s, "https://")) u.https = true, s = s.substr(8);
    else if (starts_with(s, "http://")) s = s.substr(7);
    else return false;
    size_t slash = s.find('/');
    std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : s.substr(slash);
    size_t at = hostport.find('@');
    if (at != std::string::npos) {
        u.userinfo = hostport.substr(0, at);
        hostport = hostport.substr(at + 1);
    }
    size_t colon = hostport.find(':');
    u.host = hostport.substr(0, colon);
    u.port = colon == std::string::npos ? (u.https ? "443" : "80") : hostport.substr(colon + 1);
    return !u.host.empty() && !u.port.empty();
}

// CR/LF in a header value would let a configured header smuggle extra headers or a second request
static std::string clean(const std::string &s) {
    std::string o;
    for (char c : s)
        if (c != '\r' && c != '\n') o += c;
    return o;
}

// decodes "Transfer-Encoding: chunked"
static std::string dechunk(const std::string &in) {
    std::string out;
    size_t p = 0;
    while (p < in.size()) {
        size_t e = in.find("\r\n", p);
        if (e == std::string::npos) break;
        size_t len = strtoul(in.substr(p, e - p).c_str(), nullptr, 16);
        if (len == 0) break;
        p = e + 2;
        if (p + len > in.size()) {
            out.append(in, p, std::string::npos);
            break;
        }
        out.append(in, p, len);
        p += len + 2;
    }
    return out;
}

Result request(const std::string &method, const std::string &url, const std::string &ctype, const std::string &body,
               const std::map<std::string, std::string> &headers, int timeout_s, const tls::ClientOpts &topts) {
    Result r;
    Url u;
    if (!parse_url(url, u)) {
        r.error = "URL must start with http:// or https://";
        return r;
    }
    std::string authz = u.userinfo.empty() ? "" : "Basic " + base64_encode(u.userinfo);

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
        r.error = "cannot resolve " + u.host;
        return r;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    timeval to{timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    bool ok = connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        close(fd);
        r.error = "cannot connect to " + u.host + ":" + u.port;
        return r;
    }
    tls::Client tc;
    if (u.https) {
        std::string err;
        if (!tc.connect(fd, u.host, topts, err)) {
            close(fd);
            r.error = err;
            return r;
        }
    }
    auto wr = [&](const char *d, size_t n) -> bool {
        if (u.https) return tc.write(d, n) == (int)n;
        size_t off = 0;
        while (off < n) {
            ssize_t k = send(fd, d + off, n - off, MSG_NOSIGNAL);
            if (k <= 0) return false;
            off += k;
        }
        return true;
    };
    std::string hostport = u.host + ((u.https && u.port == "443") || (!u.https && u.port == "80") ? "" : ":" + u.port);
    std::string req = clean(method) + " " + clean(u.path) + " HTTP/1.1\r\nHost: " + hostport + "\r\nUser-Agent: PicoCam\r\nConnection: close\r\n";
    if (!ctype.empty()) req += "Content-Type: " + clean(ctype) + "\r\n";
    if (!authz.empty()) req += "Authorization: " + authz + "\r\n";
    for (auto &h : headers) req += clean(h.first) + ": " + clean(h.second) + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;
    if (!wr(req.data(), req.size())) {
        if (u.https) tc.close();
        close(fd);
        r.error = "send failed";
        return r;
    }
    std::string in;
    char buf[4096];
    for (;;) {
        ssize_t n = u.https ? tc.read(buf, sizeof(buf)) : recv(fd, buf, sizeof(buf), 0);
        if (n <= 0 || in.size() > 1024 * 1024) break;
        in.append(buf, n);
    }
    if (u.https) tc.close();
    close(fd);
    if (in.size() < 12) {
        r.error = "no response";
        return r;
    }
    r.status = atoi(in.c_str() + 9);
    size_t he = in.find("\r\n\r\n");
    r.body = he == std::string::npos ? "" : in.substr(he + 4);
    if (he != std::string::npos && to_lower(in.substr(0, he)).find("transfer-encoding: chunked") != std::string::npos) r.body = dechunk(r.body);
    return r;
}

}  // namespace httpc
