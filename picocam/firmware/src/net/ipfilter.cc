#include "ipfilter.h"

#include <arpa/inet.h>

#include <mutex>

#include "core/util.h"

namespace ipfilter {

struct Net {
    uint32_t addr, mask;
};
static std::mutex g_mu;
static std::vector<Net> g_allow;

static bool parse_cidr(const std::string &s, Net &n) {
    std::string a = s;
    int bits = 32;
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        a = s.substr(0, slash);
        char *end = nullptr;
        bits = (int)strtol(s.c_str() + slash + 1, &end, 10);
        if (!*(s.c_str() + slash + 1) || *end || bits < 0 || bits > 32) return false;
    }
    in_addr ia;
    if (inet_pton(AF_INET, a.c_str(), &ia) != 1) return false;
    n.mask = bits ? 0xffffffffu << (32 - bits) : 0;
    n.addr = ntohl(ia.s_addr) & n.mask;
    return true;
}

std::string validate(const json &list) {
    if (!list.is_array()) return "allow list must be a list of addresses";
    if (list.size() > 32) return "at most 32 entries";
    for (auto &e : list) {
        Net n;
        if (!e.is_string() || !parse_cidr(e.get<std::string>(), n)) return "invalid address or range (use 192.168.1.0/24 or 10.0.0.5)";
    }
    return "";
}

void configure(const json &list) {
    std::vector<Net> v;
    if (list.is_array())
        for (auto &e : list) {
            Net n;
            if (e.is_string() && parse_cidr(e.get<std::string>(), n)) v.push_back(n);
        }
    std::lock_guard<std::mutex> lk(g_mu);
    g_allow = v;
}

bool allowed(const std::string &ip) {
    in_addr ia;
    if (inet_pton(AF_INET, ip.c_str(), &ia) != 1) return true;
    uint32_t a = ntohl(ia.s_addr);
    if ((a >> 24) == 127) return true;  // the camera itself
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_allow.empty()) return true;
    for (auto &n : g_allow)
        if ((a & n.mask) == n.addr) return true;
    return false;
}

}  // namespace ipfilter
