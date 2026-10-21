#include "mqtt.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <new>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "core/util.h"
#include "tls.h"

namespace mqtt {

static const char *TAG = "mqtt";

struct Ack {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::string err;
};

struct Item {
    std::string sub, payload;
    bool retain = false;
    std::shared_ptr<Ack> ack;  // set: QoS 1, the caller waits
};

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<Item> g_q;
static json g_cfg;  // written by configure()
static json g_cur;  // loop thread's copy
static std::atomic<bool> g_run{false}, g_connected{false}, g_reconf{false};
static std::thread g_th;
static std::string g_error;
static int64_t g_sent = 0;

static std::string topic_base() {
    std::string t = jget<std::string>(g_cur, "topic", "picocam/{device}");
    std::string dev = g_settings.get("device").value("id", "picocam");
    size_t p = t.find("{device}");
    if (p != std::string::npos) t.replace(p, 8, dev);
    while (!t.empty() && t.back() == '/') t.pop_back();
    return t;
}

static void put_str(std::string &b, const std::string &s) {
    b += (char)(s.size() >> 8);
    b += (char)(s.size() & 0xff);
    b += s;
}

static std::string packet(uint8_t type, const std::string &body) {
    std::string p;
    p += (char)type;
    size_t len = body.size();
    do {
        uint8_t d = len % 128;
        len /= 128;
        if (len) d |= 0x80;
        p += (char)d;
    } while (len);
    return p + body;
}

// plain or TLS socket with receive timeouts
struct Sock {
    int fd = -1;
    bool secure = false;
    tls::Client tc;
    int cur_to = -1;
    bool send_all(const std::string &d) {
        if (secure) return tc.write(d.data(), d.size()) == (int)d.size();
        size_t off = 0;
        while (off < d.size()) {
            ssize_t n = send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }
    // bytes, 0 = closed, -1 = error, -2 = nothing arrived within timeout_ms
    int read(char *buf, size_t n, int timeout_ms) {
        if (timeout_ms != cur_to) {
            timeval to{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (!to.tv_sec && !to.tv_usec) to.tv_usec = 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
            cur_to = timeout_ms;
        }
        if (secure) return tc.read(buf, n);
        ssize_t r = recv(fd, buf, n, 0);
        if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
        return (int)r;
    }
    bool read_exact(char *buf, size_t n, int timeout_ms) {
        size_t got = 0;
        while (got < n) {
            int r = read(buf + got, n - got, timeout_ms);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }
    // one MQTT control packet; type byte and body. false on timeout/closed
    bool read_packet(uint8_t &type, std::string &body, int timeout_ms) {
        char b;
        int r = read(&b, 1, timeout_ms);
        if (r <= 0) return false;
        type = (uint8_t)b;
        size_t len = 0, mul = 1;
        for (int i = 0; i < 4; ++i) {
            if (!read_exact(&b, 1, 2000)) return false;
            len += (size_t)((uint8_t)b & 127) * mul;
            mul *= 128;
            if (!((uint8_t)b & 128)) break;
        }
        body.assign(len, '\0');
        return len == 0 || read_exact(&body[0], len, 3000);
    }
    void renew() {  // fresh state for a new connection (tls::Client is not assignable)
        tc.~Client();
        new (&tc) tls::Client();
        fd = -1;
        secure = false;
        cur_to = -1;
    }
    void close_all() {
        if (fd < 0) return;
        if (secure) tc.close();
        ::close(fd);
        fd = -1;
    }
};

static bool connect_broker(Sock &s) {
    std::string host = jget<std::string>(g_cur, "host", "");
    bool use_tls = jget<bool>(g_cur, "tls", false);
    int port = jget<int>(g_cur, "port", use_tls ? 8883 : 1883);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        g_error = "cannot resolve " + host;
        return false;
    }
    s.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    timeval to{5, 0};
    setsockopt(s.fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    bool ok = connect(s.fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        s.close_all();
        g_error = "cannot connect to " + host + ":" + std::to_string(port);
        return false;
    }
    setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    s.cur_to = 5000;
    if (use_tls) {
        tls::ClientOpts o;
        o.verify = jget<std::string>(g_cur, "verify", "system");
        o.pin = jget<std::string>(g_cur, "pin", "");
        std::string err;
        if (!s.tc.connect(s.fd, host, o, err)) {
            ::close(s.fd);
            s.fd = -1;
            g_error = err;
            return false;
        }
        s.secure = true;
    }
    std::string user = jget<std::string>(g_cur, "username", ""), pass = jget<std::string>(g_cur, "password", "");
    std::string cid = jget<std::string>(g_cur, "client_id", "");
    if (cid.empty()) cid = g_settings.get("device").value("id", "picocam");
    std::string will = topic_base() + "/state";
    // CONNECT: clean session, last will "offline" (retained), keepalive 30 s
    uint8_t flags = 0x02 | 0x04 | 0x20;
    if (!user.empty()) flags |= 0x80;
    if (!pass.empty()) flags |= 0x40;
    std::string b;
    put_str(b, "MQTT");
    b += (char)4;
    b += (char)flags;
    b += (char)0;
    b += (char)30;
    put_str(b, cid);
    put_str(b, will);
    put_str(b, "offline");
    if (!user.empty()) put_str(b, user);
    if (!pass.empty()) put_str(b, pass);
    uint8_t type;
    std::string body;
    if (!s.send_all(packet(0x10, b)) || !s.read_packet(type, body, 5000) || type != 0x20 || body.size() < 2 || body[1] != 0) {
        s.close_all();
        g_error = "broker refused the connection (check user/password)";
        return false;
    }
    g_error.clear();
    return true;
}

static std::string publish_pkt(const std::string &topic, const std::string &payload, bool retain, uint16_t id = 0) {
    std::string b;
    put_str(b, topic);
    if (id) {  // QoS 1 carries a packet identifier
        b += (char)(id >> 8);
        b += (char)(id & 0xff);
    }
    b += payload;
    return packet(0x30 | (id ? 2 : 0) | (retain ? 1 : 0), b);
}

// Home Assistant MQTT discovery: the camera shows up as a device with motion, last event and last plate sensors
static void publish_discovery(Sock &s) {
    std::string base = topic_base();
    std::string dev = g_settings.get("device").value("id", "picocam");
    std::string name = g_settings.get("device").value("name", "PicoCam");
    json device = {{"identifiers", {dev}}, {"name", name}, {"manufacturer", "PicoCam"}, {"model", "IP camera"}};
    auto cfg = [&](const std::string &component, const std::string &obj, json c) {
        c["device"] = device;
        c["unique_id"] = dev + "_" + obj;
        c["availability_topic"] = base + "/state";
        s.send_all(publish_pkt("homeassistant/" + component + "/" + dev + "/" + obj + "/config", c.dump(), true));
    };
    cfg("binary_sensor", "motion", {{"name", name + " motion"}, {"state_topic", base + "/events/motion"}, {"value_template", "ON"}, {"payload_on", "ON"}, {"off_delay", 10}, {"device_class", "motion"}});
    cfg("binary_sensor", "person", {{"name", name + " person"}, {"state_topic", base + "/events/presence"}, {"value_template", "{{ 'ON' if value_json.label == 'person' else 'OFF' }}"}, {"payload_on", "ON"}, {"off_delay", 15}, {"device_class", "occupancy"}});
    cfg("sensor", "last_event", {{"name", name + " last event"}, {"state_topic", base + "/events"}, {"value_template", "{{ value_json.event.type }}: {{ value_json.event.label }}"}, {"json_attributes_topic", base + "/events"}, {"json_attributes_template", "{{ value_json.event | tojson }}"}});
    cfg("sensor", "last_plate", {{"name", name + " last plate"}, {"state_topic", base + "/events/track"}, {"value_template", "{{ value_json.attributes.plate_text | default('') }}"}, {"icon", "mdi:car"}});
    cfg("sensor", "line_in", {{"name", name + " line A to B"}, {"state_topic", base + "/events/line_cross"}, {"value_template", "{{ value_json.extra.count_a_to_b | default(0) }}"}, {"state_class", "total_increasing"}});
    cfg("sensor", "line_out", {{"name", name + " line B to A"}, {"state_topic", base + "/events/line_cross"}, {"value_template", "{{ value_json.extra.count_b_to_a | default(0) }}"}, {"state_class", "total_increasing"}});
}

static void loop() {
    Sock s;
    int64_t last_ping = 0, backoff = 2000, next_try = 0;
    uint16_t next_id = 1;
    auto fail_waiters = [](const std::string &why) {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &it : g_q)
            if (it.ack) {
                std::lock_guard<std::mutex> a(it.ack->mu);
                it.ack->err = why;
                it.ack->done = true;
                it.ack->cv.notify_all();
            }
        g_q.clear();
    };
    while (g_run) {
        if (g_reconf && s.fd >= 0) {
            s.send_all(packet(0xe0, ""));
            s.close_all();
            g_connected = false;
        }
        if (g_reconf || g_cur.is_null()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_cur = g_cfg;
        }
        g_reconf = false;
        if (!jget<bool>(g_cur, "enabled", false) || jget<std::string>(g_cur, "host", "").empty()) {
            fail_waiters("MQTT is off");
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::seconds(1));
            continue;
        }
        if (s.fd < 0) {
            if (now_ms() < next_try) {
                usleep(200 * 1000);
                continue;
            }
            s.renew();
            if (!connect_broker(s)) {
                next_try = now_ms() + backoff;
                backoff = std::min<int64_t>(backoff * 2, 60000);
                LOGW(TAG, "%s", g_error.c_str());
                fail_waiters(g_error);
                continue;
            }
            backoff = 2000;
            g_connected = true;
            last_ping = now_ms();
            LOGI(TAG, "connected to %s%s", jget<std::string>(g_cur, "host", "").c_str(), s.secure ? " (TLS)" : "");
            s.send_all(publish_pkt(topic_base() + "/state", "online", true));
            if (jget<bool>(g_cur, "ha_discovery", false)) publish_discovery(s);
        }
        Item item;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_q.empty() || !g_run || g_reconf; });
            if (!g_q.empty()) {
                item = std::move(g_q.front());
                g_q.pop_front();
                have = true;
            }
        }
        bool ok = true;
        if (have) {
            std::string topic = topic_base() + "/" + item.sub;
            if (item.ack) {
                uint16_t id = next_id++;
                if (!next_id) next_id = 1;
                ok = s.send_all(publish_pkt(topic, item.payload, item.retain, id));
                std::string why = ok ? "" : "connection lost";
                // wait for the PUBACK of this packet
                int64_t until = now_ms() + 6000;
                while (ok) {
                    uint8_t type;
                    std::string body;
                    if (now_ms() > until) {
                        why = "broker did not acknowledge";
                        ok = false;
                        break;
                    }
                    if (!s.read_packet(type, body, 500)) continue;
                    if (type == 0x40 && body.size() >= 2 && (((uint8_t)body[0] << 8) | (uint8_t)body[1]) == id) break;
                }
                if (ok) ++g_sent;
                std::lock_guard<std::mutex> a(item.ack->mu);
                item.ack->err = why;
                item.ack->done = true;
                item.ack->cv.notify_all();
            } else {
                ok = s.send_all(publish_pkt(topic, item.payload, item.retain));
                if (ok) ++g_sent;
            }
        }
        if (ok && now_ms() - last_ping > 15000) {
            ok = s.send_all(packet(0xc0, ""));
            last_ping = now_ms();
            // drain PINGRESP and anything else the broker sent
            uint8_t type;
            std::string body;
            while (s.read_packet(type, body, 30)) {
            }
        }
        if (!ok) {
            s.close_all();
            g_connected = false;
            g_error = "connection lost";
            next_try = now_ms() + backoff;
        }
    }
    if (s.fd >= 0) {
        s.send_all(publish_pkt(topic_base() + "/state", "offline", true));
        s.send_all(packet(0xe0, ""));
        s.close_all();
    }
}

void configure(const json &cfg) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cfg = cfg;
    }
    g_reconf = true;
    g_cv.notify_all();
    if (!g_run) {
        g_run = true;
        g_th = std::thread(loop);
    }
}

void publish(const std::string &sub, const std::string &payload, bool retain) {
    if (!g_connected) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_q.size() >= 32) g_q.pop_front();
    g_q.push_back({sub, payload, retain, nullptr});
    g_cv.notify_one();
}

std::string publish_wait(const std::string &sub, const std::string &payload, bool retain) {
    if (!g_connected) return g_error.empty() ? "MQTT broker not connected" : g_error;
    auto ack = std::make_shared<Ack>();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_q.push_back({sub, payload, retain, ack});
        g_cv.notify_one();
    }
    std::unique_lock<std::mutex> lk(ack->mu);
    if (!ack->cv.wait_for(lk, std::chrono::seconds(9), [&] { return ack->done; })) return "timeout";
    return ack->err;
}

json status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return {{"enabled", jget<bool>(g_cfg, "enabled", false)}, {"connected", g_connected.load()}, {"error", g_error}, {"sent", g_sent},
            {"tls", jget<bool>(g_cfg, "tls", false)}};
}

void shutdown() {
    g_run = false;
    g_cv.notify_all();
    if (g_th.joinable()) g_th.join();
}

}  // namespace mqtt
