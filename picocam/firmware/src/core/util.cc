#include "util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

int64_t now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int64_t now_us() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int64_t wall_ms() {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

std::string format_time(time_t t, const char *fmt, bool local) {
    struct tm tm;
    if (local) localtime_r(&t, &tm);
    else gmtime_r(&t, &tm);
    char buf[96];
    strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

std::string iso8601(int64_t ms, bool utc) {
    if (utc) return format_time(ms / 1000, "%Y-%m-%dT%H:%M:%SZ", false);
    return format_time(ms / 1000, "%Y-%m-%dT%H:%M:%S", true);
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string url_decode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string html_escape(const std::string &s) { return xml_escape(s); }

std::string xml_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '&': o += "&amp;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

bool read_file(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        off += n;
    }
    fsync(fd);
    close(fd);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool file_exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

int64_t file_size(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
}

bool mkdirs(const std::string &path) {
    std::string cur;
    for (auto &part : split(path, '/')) {
        if (part.empty()) {
            if (cur.empty()) cur = "/";
            continue;
        }
        cur += (cur.empty() || cur.back() == '/' ? "" : "/") + part;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool remove_tree(const std::string &path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path.c_str());
        if (d) {
            while (dirent *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                remove_tree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

std::string run_cmd(const std::string &cmd, int *status) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) {
        if (status) *status = -1;
        return out;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int st = pclose(p);
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}

// ------------------------------------------------------------------ encodings

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t *d, size_t n) {
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16;
        if (i + 1 < n) v |= d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        o += kB64[(v >> 18) & 63];
        o += kB64[(v >> 12) & 63];
        o += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        o += i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}

std::string base64_decode(const std::string &s) {
    std::string o;
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        const char *p = strchr(kB64, c);
        if (!p || !c) continue;  // skips '=', whitespace
        v = (v << 6) | (uint32_t)(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o += (char)((v >> bits) & 0xff);
        }
    }
    return o;
}

std::string hex(const uint8_t *d, size_t n) {
    static const char *h = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; ++i) {
        o += h[d[i] >> 4];
        o += h[d[i] & 15];
    }
    return o;
}

// ------------------------------------------------------------------ SHA-1

static inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string sha1(const std::string &data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; --i) m += (char)((bits >> (i * 8)) & 0xff);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = (const uint8_t *)m.data() + off + i * 4;
            w[i] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out(20, '\0');
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (char)(h[i] >> (24 - j * 8));
    return out;
}

// ------------------------------------------------------------------ SHA-256

static const uint32_t kK256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static inline uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

std::string sha256(const std::string &data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; --i) m += (char)((bits >> (i * 8)) & 0xff);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = (const uint8_t *)m.data() + off + i * 4;
            w[i] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + kK256[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    std::string out(32, '\0');
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (char)(h[i] >> (24 - j * 8));
    return out;
}

// ------------------------------------------------------------------ MD5 (RTSP digest auth)

std::string md5(const std::string &data) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    std::string m = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 0; i < 8; ++i) m += (char)((bits >> (i * 8)) & 0xff);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = (const uint8_t *)m.data() + off + i * 4;
            w[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[i] + w[g], R[i]);
            a = t;
        }
        h0 += a, h1 += b, h2 += c, h3 += d;
    }
    std::string out(16, '\0');
    uint32_t hs[4] = {h0, h1, h2, h3};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (char)(hs[i] >> (j * 8));
    return out;
}

std::string md5_hex(const std::string &data) {
    std::string d = md5(data);
    return hex((const uint8_t *)d.data(), d.size());
}

std::string sha256_hex(const std::string &data) {
    std::string d = sha256(data);
    return hex((const uint8_t *)d.data(), d.size());
}

std::string hmac_sha256_hex(const std::string &key, const std::string &data) {
    std::string k = key.size() > 64 ? sha256(key) : key;
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (char)(k[i] ^ 0x36);
        opad[i] = (char)(k[i] ^ 0x5c);
    }
    std::string inner = sha256(ipad + data);
    std::string d = sha256(opad + inner);
    return hex((const uint8_t *)d.data(), d.size());
}

std::string random_bytes(size_t n) {
    std::string out(n, '\0');
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t r = read(fd, &out[off], n - off);
            if (r <= 0) break;
            off += r;
        }
        close(fd);
        if (off == n) return out;
    }
    // zeros would make session tokens and keys predictable: never continue without real randomness
    fprintf(stderr, "FATAL: /dev/urandom unavailable\n");
    abort();
}

std::string random_token(size_t bytes) {
    std::string r = random_bytes(bytes);
    return hex((const uint8_t *)r.data(), r.size());
}

bool const_time_eq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

// ------------------------------------------------------------------ system

long meminfo_kb(const char *key) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[128];
    long kb = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, key, klen) && line[klen] == ':') {
            kb = atol(line + klen + 1);
            break;
        }
    }
    fclose(fp);
    return kb;
}

std::string iface_ipv4(const std::string &ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    ifreq ifr{};
    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    std::string ip;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) ip = inet_ntoa(((sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    close(fd);
    return ip;
}

std::string iface_mac(const std::string &ifname) {
    std::string s;
    if (!read_file("/sys/class/net/" + ifname + "/address", s)) return "";
    return trim(s);
}

std::string primary_iface() {
    std::string carrier;
    if (read_file("/sys/class/net/eth0/carrier", carrier) && trim(carrier) == "1" && !iface_ipv4("eth0").empty())
        return "eth0";
    if (!iface_ipv4("usb0").empty()) return "usb0";
    if (file_exists("/sys/class/net/eth0")) return "eth0";
    return "usb0";
}
