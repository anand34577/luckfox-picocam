#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

int64_t now_ms();   // monotonic
int64_t now_us();   // monotonic
int64_t wall_ms();  // unix epoch
std::string format_time(time_t t, const char *fmt, bool local = true);
std::string iso8601(int64_t ms, bool utc = true);

// strings
std::vector<std::string> split(const std::string &s, char sep);
std::string trim(const std::string &s);
std::string to_lower(std::string s);
bool starts_with(const std::string &s, const std::string &p);
bool ends_with(const std::string &s, const std::string &p);
std::string url_decode(const std::string &s);
std::string html_escape(const std::string &s);
std::string xml_escape(const std::string &s);

// files
bool read_file(const std::string &path, std::string &out);
// atomic replace (write temp + fsync + rename)
bool write_file(const std::string &path, const std::string &data);
bool file_exists(const std::string &path);
int64_t file_size(const std::string &path);
bool mkdirs(const std::string &path);
bool remove_tree(const std::string &path);
std::string run_cmd(const std::string &cmd, int *status = nullptr);  // stdout of a shell command

// encodings and hashes
std::string base64_encode(const uint8_t *d, size_t n);
inline std::string base64_encode(const std::string &s) { return base64_encode((const uint8_t *)s.data(), s.size()); }
std::string base64_decode(const std::string &s);
std::string hex(const uint8_t *d, size_t n);
std::string sha1(const std::string &data);    // raw 20 bytes
std::string sha256(const std::string &data);  // raw 32 bytes
std::string md5(const std::string &data);     // raw 16 bytes
std::string md5_hex(const std::string &data);
std::string sha256_hex(const std::string &data);
std::string hmac_sha256_hex(const std::string &key, const std::string &data);
std::string random_bytes(size_t n);
std::string random_token(size_t bytes = 16);  // hex
bool const_time_eq(const std::string &a, const std::string &b);

// system
long meminfo_kb(const char *key);  // e.g. "MemAvailable"
std::string iface_ipv4(const std::string &ifname);
std::string iface_mac(const std::string &ifname);
std::string primary_iface();  // eth0 when it has a link/address, else usb0
