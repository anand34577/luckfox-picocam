#pragma once
#include <map>
#include <string>

#include "tls.h"

// Minimal blocking HTTP/1.1 client (http:// and https://) for webhooks, the EdgeAI server and snapshot uploads.
namespace httpc {

struct Result {
    int status = 0;  // 0 = connection failed
    std::string body, error;
};

Result request(const std::string &method, const std::string &url, const std::string &content_type,
               const std::string &body, const std::map<std::string, std::string> &headers = {}, int timeout_s = 6,
               const tls::ClientOpts &tls = {});

// splits http(s)://[user:pass@]host[:port]/path
struct Url {
    bool https = false;
    std::string host, port, path, userinfo;
};
bool parse_url(const std::string &url, Url &u);

}  // namespace httpc
