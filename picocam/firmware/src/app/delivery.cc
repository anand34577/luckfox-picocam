#include "delivery.h"

#include <dirent.h>
#include <netdb.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "app/events.h"
#include "app/system.h"
#include "core/gpio.h"
#include "core/log.h"
#include "core/util.h"
#include "net/httpc.h"
#include "net/mqtt.h"
#include "net/tls.h"

namespace delivery {

static const char *TAG = "delivery";
static const size_t kMaxQueued = 300;           // per target; the oldest is dropped beyond this
static const int64_t kMaxAgeMs = 24LL * 3600 * 1000;  // an event older than this is no longer worth sending
static const int kWorkers = 3;

using Files = std::vector<std::pair<std::string, std::string>>;  // name -> JPEG bytes

struct Item {
    std::string file;  // queue file on flash
    json rec;
    int attempts = 0;
    int64_t created = 0;
};

struct Target {
    std::string key, kind;
    json cfg;
    std::deque<Item> q;
    bool busy = false;
    int fails = 0;
    int64_t next_try = 0, last_at = 0, last_enq = 0;
    bool last_ok = true, seen = false;
    std::string last_err;
};

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::vector<std::shared_ptr<Target>> g_targets;
static json g_gpio = json::array();
static std::atomic<bool> g_run{false};
static std::vector<std::thread> g_workers;
static std::thread g_hb;
static std::atomic<int64_t> g_dropped{0}, g_seq{0};
static json g_edgeai = json::object();

static std::string outbox_dir() { return g_paths.data + "/outbox"; }

// ---------------------------------------------------------------- helpers

static bool in_filter(const json &f, const std::string &v) {
    if (!f.is_array() || f.empty()) return true;
    for (auto &x : f)
        if (x.is_string() && x.get<std::string>() == v) return true;
    return false;
}

static bool wants(const json &cfg, const json &rec) {
    return in_filter(cfg.value("types", json::array()), rec.value("type", "")) &&
           in_filter(cfg.value("rules", json::array()), rec.value("rule_id", "")) &&
           in_filter(cfg.value("labels", json::array()), rec.value("label", ""));
}

static tls::ClientOpts tls_opts(const json &cfg) {
    tls::ClientOpts o;
    o.verify = jget<std::string>(cfg, "verify", "system");
    o.pin = jget<std::string>(cfg, "pin", "");
    return o;
}

static json device_json() {
    json d = g_settings.get("device");
    return {{"id", d.value("id", "")}, {"name", d.value("name", "")}, {"location", d.value("location", "")}, {"ip", iface_ipv4(primary_iface())}};
}

static std::string read_image(const std::string &day, const std::string &file) {
    std::string s;
    if (day.size() == 10 && file.find("..") == std::string::npos && file.find('/') == std::string::npos)
        read_file(events::dir() + "/" + day + "/" + file, s);
    return s;
}

// pictures of an event: "snapshot" first, then the crops
static Files load_images(const json &rec) {
    Files out;
    std::string day = rec.value("day", "");
    json im = rec.value("images", json::object());
    if (im.contains("snapshot")) {
        std::string b = read_image(day, im["snapshot"].get<std::string>());
        if (!b.empty()) out.push_back({"snapshot", b});
    }
    for (auto it = im.begin(); it != im.end(); ++it) {
        if (it.key() == "snapshot" || !it.value().is_string()) continue;
        std::string b = read_image(day, it.value().get<std::string>());
        if (!b.empty()) out.push_back({it.key(), b});
    }
    return out;
}

static std::string jdump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

// ---------------------------------------------------------------- line protocols over a plain or TLS socket

struct Line {
    int fd = -1;
    tls::Client tc;
    bool secure = false;
    std::string buf;
    bool send(const std::string &d) {
        if (secure) return tc.write(d.data(), d.size()) == (int)d.size();
        size_t off = 0;
        while (off < d.size()) {
            ssize_t n = ::send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }
    bool readline(std::string &line) {
        for (;;) {
            size_t p = buf.find('\n');
            if (p != std::string::npos) {
                line = trim(buf.substr(0, p));
                buf.erase(0, p + 1);
                return true;
            }
            char tmp[1024];
            int n = secure ? tc.read(tmp, sizeof(tmp)) : (int)recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buf.append(tmp, n);
        }
    }
    // reply code of a (possibly multi-line) reply: "250-..." continues, "250 ..." ends
    int reply(std::string *text = nullptr) {
        std::string l, all;
        while (readline(l)) {
            all += l + "\n";
            if (l.size() >= 4 && isdigit((unsigned char)l[0]) && l[3] == ' ') {
                if (text) *text = all;
                return atoi(l.c_str());
            }
            if (l.size() < 4) break;
        }
        if (text) *text = all;
        return -1;
    }
    void close_all() {
        if (fd < 0) return;
        if (secure) tc.close();
        ::close(fd);
        fd = -1;
    }
    ~Line() { close_all(); }
};

static int tcp_connect(const std::string &host, int port, int timeout_s, std::string &err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        err = "cannot resolve " + host;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    timeval to{timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    bool ok = connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        close(fd);
        err = "cannot connect to " + host + ":" + std::to_string(port);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------- targets

static bool deliver_webhook(const json &wh, const json &rec, std::string &err, bool &permanent) {
    json body = {{"event", rec}, {"device", device_json()}};
    std::string imgs = jget<std::string>(wh, "include_image", "none");
    if (imgs != "none") {
        json im = json::object();
        Files f = load_images(rec);
        for (auto &p : f)
            if (p.first == "snapshot" || imgs == "all") im[p.first] = base64_encode(p.second);
        body["images"] = im;
    }
    std::string payload = jdump(body);
    std::map<std::string, std::string> hdr;
    for (auto &kv : wh.value("headers", json::object()).items())
        if (kv.value().is_string()) hdr[kv.key()] = kv.value().get<std::string>();
    hdr["X-PicoCam-Event-Id"] = rec.value("id", "");  // lets the receiver drop duplicates after a retry
    std::string ts = std::to_string(wall_ms());
    hdr["X-PicoCam-Timestamp"] = ts;
    std::string secret = jget<std::string>(wh, "secret", "");
    if (!secret.empty()) hdr["X-PicoCam-Signature"] = "sha256=" + hmac_sha256_hex(secret, ts + "." + payload);
    auto r = httpc::request(jget<std::string>(wh, "method", "POST") == "PUT" ? "PUT" : "POST", jget<std::string>(wh, "url", ""),
                            "application/json", payload, hdr, 6, tls_opts(wh));
    bool ok = r.status / 100 == 2;
    if (!ok) {
        err = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        permanent = r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429;
    }
    return ok;
}

// EdgeAI server (server/ in this repo): multipart POST /api/v1/events with meta + frame + crops
static bool deliver_edgeai(const json &cfg, const json &rec, std::string &err, bool &permanent) {
    std::string url = jget<std::string>(cfg, "url", "");
    while (!url.empty() && url.back() == '/') url.pop_back();
    json attrs = rec.value("attributes", json::object());
    std::string label = rec.value("label", "");
    bool face = label == "face";
    json meta = {{"type", face ? "face" : (attrs.contains("plate_text") || attrs.contains("plate") ? "vehicle" : rec.value("type", "event"))},
                 {"device_id", device_json()["id"]},
                 {"track_id", rec.value("track_id", 0)},
                 {"ts_ms", rec.value("ts", (int64_t)0)},
                 {"rule", rec.value("rule", "")},
                 {"label", label},
                 {"event_id", rec.value("id", "")},
                 {"attributes", attrs}};
    if (!face) {
        meta["vehicle_type"] = label;
        meta["vehicle_conf"] = rec.value("score", 0.0);
        std::string plate = attrs.value("plate_text", attrs.value("plate", ""));
        meta["plate_text"] = plate;
        meta["plate_valid"] = !plate.empty();
    } else if (rec.contains("embedding")) {
        meta["embedding"] = rec["embedding"];
    }
    const std::string B = "----picocam7MA4YWxkTrZu0gW";
    std::string body;
    auto part = [&](const std::string &name, const std::string &fname, const std::string &type, const std::string &data) {
        body += "--" + B + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
        if (!fname.empty()) body += "; filename=\"" + fname + "\"";
        body += "\r\nContent-Type: " + type + "\r\n\r\n" + data + "\r\n";
    };
    part("meta", "", "application/json", meta.dump());
    for (auto &c : load_images(rec)) {
        if (c.first == "snapshot") {
            part("frame", "frame.jpg", "image/jpeg", c.second);
            continue;
        }
        std::string n = c.first == "plate" || c.first == "plate_det" ? "plate" : c.first == "object" ? (face ? "face" : "vehicle") : c.first;
        part("crop_" + n, n + ".jpg", "image/jpeg", c.second);
    }
    body += "--" + B + "--\r\n";
    auto r = httpc::request("POST", url + "/api/v1/events", "multipart/form-data; boundary=" + B, body,
                            {{"X-Device-Id", device_json()["id"].get<std::string>()}}, 8, tls_opts(cfg));
    bool ok = r.status / 100 == 2;
    if (!ok) {
        err = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        permanent = r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429;
    }
    return ok;
}

static bool deliver_mqtt(const json &mq, const json &rec, std::string &err) {
    std::string type = rec.value("type", "");
    json m = {{"event", rec}, {"device", device_json()}};
    if (jget<bool>(mq, "include_image", false)) {
        Files f = load_images(rec);
        if (!f.empty() && f[0].first == "snapshot") m["snapshot"] = base64_encode(f[0].second);
    }
    err = mqtt::publish_wait("events", jdump(m));
    if (!err.empty()) return false;
    err = mqtt::publish_wait("events/" + type, jdump(rec));
    return err.empty();
}

static std::string safe_seg(const std::string &s) {
    std::string o;
    for (char c : s) o += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
    return o;
}

// plain FTP (passive mode): <path>/<date>/<event id>_<picture>.jpg. Use it on a trusted network.
static bool deliver_ftp(const json &cfg, const json &rec, std::string &err) {
    Files files = load_images(rec);
    if (jget<std::string>(cfg, "what", "snapshot") == "snapshot") {
        Files only;
        for (auto &f : files)
            if (f.first == "snapshot") only.push_back(f);
        files = only;
    }
    if (files.empty()) return true;  // nothing to upload
    Line c;
    c.fd = tcp_connect(jget<std::string>(cfg, "host", ""), jget<int>(cfg, "port", 21), 8, err);
    if (c.fd < 0) return false;
    auto cmd = [&](const std::string &s, std::string *text = nullptr) {
        if (!c.send(s + "\r\n")) return -1;
        return c.reply(text);
    };
    if (c.reply() != 220) return err = "FTP server did not greet", false;
    int r = cmd("USER " + jget<std::string>(cfg, "user", "anonymous"));
    if (r == 331) r = cmd("PASS " + jget<std::string>(cfg, "password", ""));
    if (r != 230) return err = "FTP login failed", false;
    cmd("TYPE I");
    std::string path = jget<std::string>(cfg, "path", "/") + "/" + rec.value("day", "");
    if (!path.empty() && path[0] == '/') cmd("CWD /");
    for (auto &seg : split(path, '/')) {
        if (seg.empty()) continue;
        std::string s = safe_seg(seg);
        if (cmd("CWD " + s) / 100 != 2) {
            cmd("MKD " + s);
            if (cmd("CWD " + s) / 100 != 2) return err = "FTP cannot enter " + s, false;
        }
    }
    sockaddr_in peer{};
    socklen_t pl = sizeof(peer);
    getpeername(c.fd, (sockaddr *)&peer, &pl);
    for (auto &f : files) {
        std::string text;
        if (cmd("PASV", &text) != 227) return err = "FTP server refused passive mode", false;
        size_t a = text.find('('), b = text.find(')');
        int h[6] = {0};
        if (a == std::string::npos || b == std::string::npos || sscanf(text.c_str() + a + 1, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6)
            return err = "bad PASV reply", false;
        int dfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        timeval to{8, 0};
        setsockopt(dfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(dfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
        sockaddr_in d = peer;  // the control connection's address (the PASV address is wrong behind NAT)
        d.sin_port = htons((uint16_t)(h[4] * 256 + h[5]));
        if (connect(dfd, (sockaddr *)&d, sizeof(d)) != 0) {
            close(dfd);
            return err = "FTP data connection failed", false;
        }
        int sr = cmd("STOR " + safe_seg(rec.value("id", "event")) + "_" + safe_seg(f.first) + ".jpg");
        if (sr != 150 && sr != 125) {
            close(dfd);
            return err = "FTP upload refused", false;
        }
        size_t off = 0;
        bool ok = true;
        while (off < f.second.size() && ok) {
            ssize_t n = send(dfd, f.second.data() + off, f.second.size() - off, MSG_NOSIGNAL);
            if (n <= 0) ok = false;
            else off += n;
        }
        close(dfd);
        if (!ok || c.reply() / 100 != 2) return err = "FTP upload failed", false;
    }
    cmd("QUIT");
    return true;
}

static std::string b64_wrapped(const std::string &d) {
    std::string e = base64_encode(d), o;
    for (size_t i = 0; i < e.size(); i += 76) o += e.substr(i, 76) + "\r\n";
    return o;
}

static bool deliver_smtp(const json &cfg, const json &rec, std::string &err) {
    std::string sec = jget<std::string>(cfg, "security", "starttls");
    Line c;
    c.fd = tcp_connect(jget<std::string>(cfg, "host", ""), jget<int>(cfg, "port", sec == "tls" ? 465 : 587), 10, err);
    if (c.fd < 0) return false;
    std::string host = jget<std::string>(cfg, "host", "");
    if (sec == "tls") {
        if (!c.tc.connect(c.fd, host, tls_opts(cfg), err)) return false;
        c.secure = true;
    }
    auto cmd = [&](const std::string &s, std::string *text = nullptr) {
        if (!c.send(s + "\r\n")) return -1;
        return c.reply(text);
    };
    if (c.reply() != 220) return err = "SMTP server did not greet", false;
    if (cmd("EHLO picocam.local") / 100 != 2) return err = "SMTP EHLO refused", false;
    if (sec == "starttls") {
        if (cmd("STARTTLS") != 220) return err = "server does not offer STARTTLS", false;
        if (!c.tc.connect(c.fd, host, tls_opts(cfg), err)) return false;
        c.secure = true;
        c.buf.clear();
        if (cmd("EHLO picocam.local") / 100 != 2) return err = "SMTP EHLO refused after STARTTLS", false;
    }
    std::string user = jget<std::string>(cfg, "user", "");
    if (!user.empty()) {
        std::string plain = std::string(1, '\0') + user + std::string(1, '\0') + jget<std::string>(cfg, "password", "");
        if (cmd("AUTH PLAIN " + base64_encode(plain)) != 235) return err = "SMTP login failed", false;
    }
    std::string from = jget<std::string>(cfg, "from", user.empty() ? "picocam@localhost" : user);
    if (cmd("MAIL FROM:<" + from + ">") / 100 != 2) return err = "sender refused", false;
    std::string to_hdr;
    for (auto &t : split(jget<std::string>(cfg, "to", ""), ',')) {
        std::string a = trim(t);
        if (a.empty()) continue;
        if (cmd("RCPT TO:<" + a + ">") / 100 != 2) return err = "recipient refused: " + a, false;
        to_hdr += (to_hdr.empty() ? "" : ", ") + a;
    }
    if (to_hdr.empty()) return err = "no recipient", false;
    if (cmd("DATA") != 354) return err = "SMTP DATA refused", false;

    json dev = device_json();
    std::string subject = jget<std::string>(cfg, "subject", "PicoCam alert");
    for (auto &ch : subject)
        if (ch == '\r' || ch == '\n') ch = ' ';
    std::string text = "Camera: " + dev.value("name", "") + " (" + dev.value("ip", "") + ")\r\nTime: " + rec.value("time", "") +
                       "\r\nEvent: " + rec.value("type", "") + (rec.value("rule", "").empty() ? "" : " / " + rec.value("rule", "")) +
                       "\r\nObject: " + rec.value("label", "") + "\r\n";
    for (auto &kv : rec.value("attributes", json::object()).items())
        text += kv.key() + ": " + (kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump()) + "\r\n";
    const std::string B = "=_picocam_" + random_token(6);
    std::string msg = "From: " + dev.value("name", "PicoCam") + " <" + from + ">\r\nTo: " + to_hdr + "\r\nSubject: " + subject +
                      "\r\nDate: " + format_time(time(nullptr), "%a, %d %b %Y %H:%M:%S +0000", false) +
                      "\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"" + B + "\"\r\n\r\n--" + B +
                      "\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n" + text + "\r\n";
    if (jget<bool>(cfg, "include_image", true))
        for (auto &f : load_images(rec)) {
            if (f.first != "snapshot") continue;
            msg += "--" + B + "\r\nContent-Type: image/jpeg; name=\"snapshot.jpg\"\r\nContent-Transfer-Encoding: base64\r\n"
                   "Content-Disposition: attachment; filename=\"snapshot.jpg\"\r\n\r\n" + b64_wrapped(f.second);
        }
    msg += "--" + B + "--\r\n";
    // dot-stuffing: a line that starts with "." would end the message early
    std::string out;
    bool bol = true;
    for (char ch : msg) {
        if (bol && ch == '.') out += '.';
        out += ch;
        bol = ch == '\n';
    }
    if (!c.send(out + ".\r\n")) return err = "send failed", false;
    if (c.reply() / 100 != 2) return err = "message rejected", false;
    cmd("QUIT");
    return true;
}

static bool deliver_telegram(const json &cfg, const json &rec, std::string &err, bool &permanent) {
    std::string token = jget<std::string>(cfg, "token", ""), chat = jget<std::string>(cfg, "chat_id", "");
    json dev = device_json();
    std::string caption = "📷 " + dev.value("name", "PicoCam") + " — " + rec.value("type", "") + (rec.value("label", "").empty() ? "" : ": " + rec.value("label", ""));
    for (auto &kv : rec.value("attributes", json::object()).items())
        caption += "\n" + kv.key() + ": " + (kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump());
    caption += "\n" + rec.value("time", "");
    Files imgs = load_images(rec);
    std::string snap;
    for (auto &f : imgs)
        if (f.first == "snapshot") snap = f.second;
    httpc::Result r;
    tls::ClientOpts o;  // verified against the CA bundle
    if (jget<bool>(cfg, "include_image", true) && !snap.empty()) {
        const std::string B = "----picocamTG" + random_token(6);
        std::string body = "--" + B + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chat + "\r\n--" + B +
                           "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n--" + B +
                           "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"snapshot.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n" + snap +
                           "\r\n--" + B + "--\r\n";
        r = httpc::request("POST", "https://api.telegram.org/bot" + token + "/sendPhoto", "multipart/form-data; boundary=" + B, body, {}, 12, o);
    } else {
        r = httpc::request("POST", "https://api.telegram.org/bot" + token + "/sendMessage", "application/json",
                           jdump({{"chat_id", chat}, {"text", caption}}), {}, 12, o);
    }
    bool ok = r.status / 100 == 2;
    if (!ok) {
        json j = json::parse(r.body, nullptr, false);
        err = r.error.empty() ? "Telegram: " + (j.is_object() ? j.value("description", "HTTP " + std::to_string(r.status)) : "HTTP " + std::to_string(r.status)) : r.error;
        permanent = r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429;
    }
    return ok;
}

static bool deliver(Target &t, const json &rec, std::string &err, bool &permanent) {
    if (t.kind == "webhook") return deliver_webhook(t.cfg, rec, err, permanent);
    if (t.kind == "edgeai") return deliver_edgeai(t.cfg, rec, err, permanent);
    if (t.kind == "mqtt") return deliver_mqtt(t.cfg, rec, err);
    if (t.kind == "ftp") return deliver_ftp(t.cfg, rec, err);
    if (t.kind == "smtp") return deliver_smtp(t.cfg, rec, err);
    if (t.kind == "telegram") return deliver_telegram(t.cfg, rec, err, permanent);
    err = "unknown target";
    permanent = true;
    return false;
}

// ---------------------------------------------------------------- queue

static std::string item_path(const std::string &name) { return outbox_dir() + "/" + name; }

static void persist(Item &it, const std::string &key) {
    mkdirs(outbox_dir());
    char n[64];
    snprintf(n, sizeof(n), "%013lld-%06lld.json", (long long)wall_ms(), (long long)(g_seq++ % 1000000));
    it.file = item_path(n);
    write_file(it.file, jdump({{"t", key}, {"rec", it.rec}, {"created", it.created}}));
}

static void drop_item(Item &it) {
    if (!it.file.empty()) unlink(it.file.c_str());
}

static void load_outbox() {
    DIR *d = opendir(outbox_dir().c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
        if (e->d_name[0] != '.' && ends_with(e->d_name, ".json")) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    int n = 0;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &name : names) {
        std::string s;
        read_file(item_path(name), s);
        json j = json::parse(s, nullptr, false);
        std::shared_ptr<Target> tg;
        if (j.is_object())
            for (auto &t : g_targets)
                if (t->key == j.value("t", "")) tg = t;
        if (!tg || !j.contains("rec")) {
            unlink(item_path(name).c_str());  // target removed or file damaged
            continue;
        }
        Item it;
        it.file = item_path(name);
        it.rec = j["rec"];
        it.created = j.value("created", wall_ms());
        tg->q.push_back(std::move(it));
        ++n;
    }
    if (n) LOGI(TAG, "%d undelivered event(s) restored from the queue", n);
}

static void rebuild_targets(const json &integ) {
    std::vector<std::pair<std::string, std::pair<std::string, json>>> want;  // key -> (kind, cfg)
    for (auto &wh : integ.value("webhooks", json::array()))
        if (jget<bool>(wh, "enabled", true) && !jget<std::string>(wh, "url", "").empty())
            want.push_back({"webhook:" + jget<std::string>(wh, "name", jget<std::string>(wh, "url", "")), {"webhook", wh}});
    json mq = integ.value("mqtt", json::object());
    if (jget<bool>(mq, "enabled", false) && !jget<std::string>(mq, "host", "").empty()) want.push_back({"mqtt", {"mqtt", mq}});
    json ea = integ.value("edgeai_server", json::object());
    if (jget<bool>(ea, "enabled", false) && !jget<std::string>(ea, "url", "").empty()) want.push_back({"edgeai_server", {"edgeai", ea}});
    json ftp = integ.value("ftp", json::object());
    if (jget<bool>(ftp, "enabled", false) && !jget<std::string>(ftp, "host", "").empty()) want.push_back({"ftp", {"ftp", ftp}});
    json sm = integ.value("smtp", json::object());
    if (jget<bool>(sm, "enabled", false) && !jget<std::string>(sm, "host", "").empty() && !jget<std::string>(sm, "to", "").empty())
        want.push_back({"smtp", {"smtp", sm}});
    json tg = integ.value("telegram", json::object());
    if (jget<bool>(tg, "enabled", false) && !jget<std::string>(tg, "token", "").empty() && !jget<std::string>(tg, "chat_id", "").empty())
        want.push_back({"telegram", {"telegram", tg}});

    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::shared_ptr<Target>> next;
    for (auto &w : want) {
        std::shared_ptr<Target> t;
        for (auto &old : g_targets)
            if (old->key == w.first) t = old;
        if (!t) {
            t = std::make_shared<Target>();
            t->key = w.first;
        }
        t->kind = w.second.first;
        t->cfg = w.second.second;
        next.push_back(t);
    }
    for (auto &old : g_targets) {  // targets that went away: forget their queued items
        bool kept = false;
        for (auto &t : next) kept |= t == old;
        if (!kept)
            for (auto &it : old->q) drop_item(it);
    }
    g_targets = next;
    g_gpio = integ.value("gpio", json::array());
    g_edgeai = ea;
}

// ---------------------------------------------------------------- workers

static void worker() {
    prctl(PR_SET_NAME, "delivery", 0, 0, 0);
    size_t rr = 0;
    while (g_run) {
        std::shared_ptr<Target> tg;
        Item item;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            auto pick = [&]() {
                int64_t now = now_ms();
                for (size_t i = 0; i < g_targets.size(); ++i) {
                    auto &t = g_targets[(rr + i) % g_targets.size()];
                    if (!t->busy && !t->q.empty() && now >= t->next_try) {
                        rr = (rr + i + 1) % std::max<size_t>(1, g_targets.size());
                        return t;
                    }
                }
                return std::shared_ptr<Target>();
            };
            tg = pick();
            if (!tg) {
                g_cv.wait_for(lk, std::chrono::milliseconds(700));
                continue;
            }
            tg->busy = true;
            item = tg->q.front();  // copy: the front stays queued until it was delivered
        }
        std::string err;
        bool permanent = false;
        bool ok = false;
        if (wall_ms() - item.created > kMaxAgeMs) {
            err = "too old";
            permanent = true;
        } else {
            ok = deliver(*tg, item.rec, err, permanent);
        }
        std::lock_guard<std::mutex> lk(g_mu);
        tg->busy = false;
        tg->seen = true;
        tg->last_at = wall_ms();
        tg->last_ok = ok;
        tg->last_err = ok ? "" : err;
        bool drop = ok;
        if (!ok) {
            ++tg->fails;
            if (tg->q.empty()) continue;
            Item &f = tg->q.front();
            ++f.attempts;
            if (err == "too old" || (permanent && f.attempts >= 5) || f.attempts >= 60) drop = true;
            else tg->next_try = now_ms() + std::min<int64_t>(300000, 2000LL << std::min(tg->fails, 7));
        } else {
            tg->fails = 0;
            tg->next_try = 0;
        }
        if (drop && !tg->q.empty()) {
            drop_item(tg->q.front());
            tg->q.pop_front();
            if (!ok) ++g_dropped;
        }
        if (!ok) LOGW(TAG, "%s: %s (queued: %zu)", tg->key.c_str(), err.c_str(), tg->q.size());
    }
}

// EdgeAI heartbeats (and the clock it provides) are independent of event traffic
static void heartbeat_loop() {
    prctl(PR_SET_NAME, "heartbeat", 0, 0, 0);
    int64_t last = 0;
    while (g_run) {
        for (int i = 0; i < 10 && g_run; ++i) usleep(100 * 1000);
        json ea;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            ea = g_edgeai;
        }
        if (!jget<bool>(ea, "enabled", false) || jget<std::string>(ea, "url", "").empty()) continue;
        if (now_ms() - last < std::max(5, jget<int>(ea, "heartbeat_s", 10)) * 1000LL) continue;
        last = now_ms();
        std::string url = jget<std::string>(ea, "url", "");
        while (!url.empty() && url.back() == '/') url.pop_back();
        json st = sys::stats();
        std::string ip = device_json()["ip"].get<std::string>();
        json hb = {{"device_id", device_json()["id"]},
                   {"profile", "picocam"},
                   {"ip", ip},
                   {"streams", {{"main", "rtsp://" + ip + "/live/main"}, {"sub", "rtsp://" + ip + "/live/sub"}}},
                   {"mem_avail_kb", st["mem_avail_kb"]},
                   {"temp_c", st["temp_c"]},
                   {"cpu_pct", st["cpu_pct"]},
                   {"npu_load_pct", st["npu_load_pct"]},
                   {"cma_used_kb", st["media_mem_used_kb"]},
                   {"cma_total_kb", st["media_mem_total_kb"]}};
        auto r = httpc::request("POST", url + "/api/v1/heartbeat", "application/json", hb.dump(), {}, 5, tls_opts(ea));
        if (r.status / 100 == 2) {
            // the server is the clock source when the board has no NTP
            json j = json::parse(r.body, nullptr, false);
            if (j.is_object() && j.contains("server_time_ms")) {
                int64_t srv = j["server_time_ms"].get<int64_t>();
                if (std::llabs(srv - wall_ms()) > 2000) sys::set_clock(srv);
            }
        }
    }
}

// ---------------------------------------------------------------- API

void enqueue(const json &rec) {
    // local actions first: a gate relay must not wait for the network
    json gp;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        gp = g_gpio;
    }
    if (gp.is_array())
        for (auto &g : gp)
            if (jget<bool>(g, "enabled", true) && jget<int>(g, "gpio", -1) >= 0 && wants(g, rec))
                gpio::pulse(jget<int>(g, "gpio", -1), jget<bool>(g, "active_high", true), jget<int>(g, "pulse_ms", 1000));

    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &t : g_targets) {
        if (!wants(t->cfg, rec)) continue;
        if (t->kind == "smtp" || t->kind == "telegram") {
            // chat and mail are for people: one message per interval, not one per frame of motion
            int64_t gap = (int64_t)jget<int>(t->cfg, "min_interval_s", t->kind == "smtp" ? 30 : 5) * 1000;
            if (now_ms() - t->last_enq < gap && rec.value("type", "") != "test") continue;
            t->last_enq = now_ms();
        }
        Item it;
        it.rec = rec;
        it.created = wall_ms();
        persist(it, t->key);
        t->q.push_back(std::move(it));
        while (t->q.size() > kMaxQueued) {
            drop_item(t->q.front());
            t->q.pop_front();
            ++g_dropped;
        }
    }
    g_cv.notify_all();
}

json status() {
    std::lock_guard<std::mutex> lk(g_mu);
    json integ = json::object();
    size_t total = 0;
    for (auto &t : g_targets) {
        integ[t->key] = {{"ok", t->last_ok}, {"error", t->last_err}, {"at", t->last_at}, {"queued", t->q.size()}, {"seen", t->seen}};
        total += t->q.size();
    }
    return {{"integrations", integ}, {"queued", total}, {"dropped", g_dropped.load()}};
}

static std::string check_url(const std::string &u, const char *what) {
    httpc::Url x;
    if (!httpc::parse_url(u, x)) return std::string(what) + " must start with http:// or https://";
    return "";
}

std::string validate(json &i) {
    if (i.contains("webhooks") && !i["webhooks"].is_array()) return "webhooks must be a list";
    for (auto &w : i.value("webhooks", json::array())) {
        std::string e = check_url(jget<std::string>(w, "url", ""), "webhook URL");
        if (!e.empty()) return e;
        std::string v = jget<std::string>(w, "verify", "system");
        if (v != "system" && v != "pin" && v != "none") return "webhook certificate check must be system, pin or none";
    }
    json ea = i.value("edgeai_server", json::object());
    if (jget<bool>(ea, "enabled", false)) {
        std::string e = check_url(jget<std::string>(ea, "url", ""), "EdgeAI server URL");
        if (!e.empty()) return e;
    }
    json mq = i.value("mqtt", json::object());
    int port = jget<int>(mq, "port", 1883);
    if (port < 1 || port > 65535) return "invalid MQTT port";
    json sm = i.value("smtp", json::object());
    std::string sec = jget<std::string>(sm, "security", "starttls");
    if (sec != "starttls" && sec != "tls" && sec != "none") return "mail security must be starttls, tls or none";
    for (auto &g : i.value("gpio", json::array())) {
        int p = jget<int>(g, "gpio", -1);
        if (p < -1 || p > 511) return "GPIO number out of range";
    }
    return "";
}

void apply(const json &now) {
    rebuild_targets(now);
    mqtt::configure(now.value("mqtt", json::object()));
    g_cv.notify_all();
}

void start() {
    json integ = g_settings.get("integrations");
    rebuild_targets(integ);
    load_outbox();
    mqtt::configure(integ.value("mqtt", json::object()));
    g_run = true;
    for (int i = 0; i < kWorkers; ++i) g_workers.emplace_back(worker);
    g_hb = std::thread(heartbeat_loop);
}

void stop() {
    g_run = false;
    g_cv.notify_all();
    for (auto &t : g_workers)
        if (t.joinable()) t.join();
    g_workers.clear();
    if (g_hb.joinable()) g_hb.join();
    mqtt::shutdown();
}

}  // namespace delivery
