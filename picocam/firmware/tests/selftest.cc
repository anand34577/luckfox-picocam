// Host-side self-test for the pure logic (builds natively, no board needed): run tests/run.sh
//   - HMAC / SHA-256 vectors, the Indian plate normaliser, the IP allow-list
//   - TLS: certificate generation, then a real handshake and data exchange between tls::Server and tls::Client
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>

#include "ai/detectors.h"
#include "ai/geom.h"
#include "ai/textvote.h"
#include "app/faces.h"
#include "ai/tracker.h"
#include "core/audit.h"
#include "core/board.h"
#include "core/config.h"
#include "core/util.h"
#include "net/ipfilter.h"
#include "net/tls.h"

static int g_fail = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);   \
            ++g_fail;                                             \
        }                                                         \
    } while (0)

static void test_hashes() {
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // RFC 4231 test case 2
    CHECK(hmac_sha256_hex("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // key longer than the block size
    CHECK(hmac_sha256_hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

static void test_board() {
    // before the ISP has reported a sensor the defaults of the SC3336 apply; afterwards the sensor decides
    CHECK(board::max_width("main") == 2304 && board::max_height("main") == 1296);
    board::set_sensor("sc4336", 2560, 1440, 30);
    CHECK(board::max_width("main") == 2560 && board::max_height("main") == 1440);
    CHECK(board::max_width("sub") == 1920 && board::max_height("sub") == 1080);  // the sub stream never exceeds 1080p
    board::set_sensor("sc200ai", 1920, 1080, 30);
    CHECK(board::max_width("main") == 1920 && board::max_height("sub") == 1080);
    board::set_sensor("small", 1280, 720, 30);
    CHECK(board::max_width("sub") == 1280 && board::max_height("sub") == 720);
    json o = board::cma_options();
    CHECK(o.is_array() && o.size() >= 3 && o[0] == 24);  // every board offers at least 24/28/32 MB
    CHECK(board::to_json().value("sensor", "") == "small");
    CHECK(board::info().soc.size() >= 6);
}

static void test_plates() {
    bool v;
    CHECK(normalize_indian_plate("MH12AB1234", &v) == "MH12AB1234" && v);
    CHECK(normalize_indian_plate("TG09AB1234", &v) == "TG09AB1234" && v);  // Telangana (new code)
    CHECK(normalize_indian_plate("TS09AB1234", &v) == "TS09AB1234" && v);
    CHECK(normalize_indian_plate("mh 12 ab 1234", &v) == "MH12AB1234" && v);
    CHECK(normalize_indian_plate("MHI2A81234", &v) == "MH12AB1234" && v);  // I/1 and 8/B fixed by position
    CHECK(normalize_indian_plate("ZZ12AB1234", &v).size() == 10 && !v);    // not a state
}

static void test_text() {
    float w;
    int sup;
    // three reads of the same plate, each wrong in a different character: the per-position consensus is the true plate
    std::vector<std::pair<std::string, float>> reads = {{"MH12A81234", 0.9f}, {"MH12AB1Z34", 0.8f}, {"NH12AB1234", 0.7f}};
    CHECK(text_consensus(reads, &w, &sup) == "MH12AB1234");
    // reads of another length do not pollute the consensus
    reads.push_back({"MH12AB123", 0.5f});
    CHECK(text_consensus(reads, &w, &sup) == "MH12AB1234");
    CHECK(text_distance("MH12AB1234", "MH12AB1234") == 0.f);
    CHECK(text_distance("MH12AB1234", "MH12A81234") < 0.5f);   // B misread as 8
    CHECK(text_distance("MH12AB1234", "MH12AX1234") == 1.f);   // an unrelated character
    CHECK(text_distance("MH12AB1234", "MH12AB123") == 1.f);    // a lost character
}

static void test_tracker() {
    IouTracker t;
    t.iou_thr = 0.25f;
    t.max_misses = 5;
    // A 60 px wide vehicle, observed every 200 ms. It starts slowly (30 px per step: consecutive boxes overlap, the
    // tracker learns its speed), then speeds up to 60 px per step: consecutive boxes no longer touch at all, so plain
    // IoU against the last box would split the track; the velocity prediction keeps the identity.
    int id = -1;
    float x = 100;
    for (int i = 0; i < 10; ++i) {
        Det d;
        d.box = {x, 100, x + 60, 140};
        d.cls = 2;
        d.score = 0.9f;
        t.update({d}, 1000 + 200 * i);
        x += i < 3 ? 30.f : 60.f;
        if (i >= 1) {
            CHECK(t.tracks().size() == 1);
            if (id < 0) id = t.tracks()[0].id;
            CHECK(t.tracks()[0].id == id);
        }
    }
    // two objects: assignment is by best overlap, not by list order, and a different class cannot hijack the track
    IouTracker u;
    Det a, b;
    a.box = {0, 0, 100, 100}, a.cls = 0, a.score = 0.9f;
    b.box = {300, 0, 400, 100}, b.cls = 0, b.score = 0.9f;
    u.update({a, b}, 0);
    u.update({a, b}, 100);
    int ida = u.tracks()[0].id, idb = u.tracks()[1].id;
    u.update({b, a}, 200);  // detections arrive in the other order
    CHECK(u.tracks()[0].id == ida && u.tracks()[1].id == idb);
}

static void test_geometry() {
    // five landmarks, rotated by 20 degrees, scaled by 1.7 and moved: the transform must be recovered exactly
    float src[10] = {30, 40, 70, 38, 50, 62, 36, 85, 66, 84}, dst[10];
    float a = 1.7f * std::cos(0.349f), b = 1.7f * std::sin(0.349f);
    for (int i = 0; i < 5; ++i) {
        dst[2 * i] = a * src[2 * i] - b * src[2 * i + 1] + 10;
        dst[2 * i + 1] = b * src[2 * i] + a * src[2 * i + 1] - 5;
    }
    float m[6];
    CHECK(similarity_transform(src, dst, 5, m));
    CHECK(std::fabs(m[0] - a) < 1e-3f && std::fabs(m[3] - b) < 1e-3f && std::fabs(m[2] - 10) < 1e-2f && std::fabs(m[5] + 5) < 1e-2f);
    float same[10] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    CHECK(!similarity_transform(same, dst, 5, m));  // all points on one spot: no transform exists
}

static void test_faces() {
    g_paths.data = "/tmp/picocam-selftest";
    run_cmd("rm -rf /tmp/picocam-selftest; mkdir -p /tmp/picocam-selftest");
    faces::load();
    std::vector<float> a = {1, 0, 0, 0}, b = {0.9f, 0.1f, 0, 0}, c = {0, 1, 0, 0};
    CHECK(std::fabs(faces::cosine(a, a) - 1.f) < 1e-6f);
    CHECK(faces::cosine(a, c) == 0.f);
    CHECK(faces::empty());
    CHECK(!faces::enroll_start("", 3).empty());  // a name is required
    CHECK(faces::enroll_start("Ann", 2).empty());
    CHECK(faces::offer(a, 7));          // first shot, locks the track
    CHECK(!faces::offer(c, 8));         // another person's track is ignored
    usleep(800 * 1000);
    CHECK(faces::offer(b, 7));          // second shot completes the enrolment
    CHECK(!faces::empty());
    faces::Match mt;
    CHECK(faces::match(a, 0.8f, &mt) && mt.name == "Ann");
    CHECK(!faces::match(c, 0.5f, &mt));  // a different face is not Ann
    faces::load();                       // survives a restart
    CHECK(!faces::empty());
}

static void test_ipfilter() {
    CHECK(ipfilter::validate(json::array({"192.168.1.0/24", "10.0.0.5"})).empty());
    CHECK(!ipfilter::validate(json::array({"300.1.1.1"})).empty());
    CHECK(!ipfilter::validate(json::array({"1.2.3.4/40"})).empty());
    ipfilter::configure(json::array({"192.168.1.0/24", "10.0.0.5"}));
    CHECK(ipfilter::allowed("192.168.1.77"));
    CHECK(ipfilter::allowed("10.0.0.5"));
    CHECK(!ipfilter::allowed("10.0.0.6"));
    CHECK(ipfilter::allowed("127.0.0.1"));  // the camera itself is never locked out
    ipfilter::configure(json::array());
    CHECK(ipfilter::allowed("8.8.8.8"));
}

static void test_tls() {
    g_paths.data = "/tmp/picocam-selftest";
    run_cmd("rm -rf /tmp/picocam-selftest; mkdir -p /tmp/picocam-selftest");
    g_settings.load();
    CHECK(tls::init());
    CHECK(tls::available());
    json info = tls::info();
    CHECK(info["available"] == true && info["fingerprint"].get<std::string>().size() == 64);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    // server side on its own thread, the way Conn drives it: raw bytes in, ciphertext out
    std::string got_by_server;
    std::thread srv([&] {
        tls::Server s;
        char buf[4096];
        bool replied = false;
        for (int i = 0; i < 200; ++i) {
            ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
            if (n <= 0) break;
            std::string plain;
            int r = s.feed(buf, (size_t)n, plain);
            if (r == -1) break;
            got_by_server += plain;
            if (!plain.empty() && !replied) {
                replied = true;
                s.encrypt("hello from the camera", 21);
            }
            while (s.roff < s.rout.size()) {
                ssize_t w = send(sv[1], s.rout.data() + s.roff, s.rout.size() - s.roff, 0);
                if (w <= 0) return;
                s.roff += (size_t)w;
            }
            s.rout.clear();
            s.roff = 0;
            if (r == -2) break;
        }
    });
    timeval to{5, 0};
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    tls::Client c;
    std::string err;
    tls::ClientOpts o;
    o.verify = "none";
    CHECK(c.connect(sv[0], "picocam", o, err));
    if (!err.empty()) printf("  tls error: %s\n", err.c_str());
    CHECK(c.write("GET / HTTP/1.1\r\n\r\n", 18) == 18);
    char rb[64] = {0};
    int n = c.read(rb, sizeof(rb) - 1);
    CHECK(n == 21 && std::string(rb, 21) == "hello from the camera");
    c.close();
    close(sv[0]);
    srv.join();
    close(sv[1]);
    CHECK(got_by_server == "GET / HTTP/1.1\r\n\r\n");

    // pinning: the right fingerprint is accepted, a wrong one refused
    for (int pass = 0; pass < 2; ++pass) {
        int p[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, p);
        std::thread s2([&] {
            tls::Server s;
            char buf[4096];
            for (int i = 0; i < 50; ++i) {
                ssize_t n2 = recv(p[1], buf, sizeof(buf), 0);
                if (n2 <= 0) break;
                std::string pl;
                int r = s.feed(buf, (size_t)n2, pl);
                while (s.roff < s.rout.size()) {
                    ssize_t w = send(p[1], s.rout.data() + s.roff, s.rout.size() - s.roff, 0);
                    if (w <= 0) break;
                    s.roff += (size_t)w;
                }
                s.rout.clear();
                s.roff = 0;
                if (r < 0) break;
            }
        });
        setsockopt(p[0], SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        tls::Client c2;
        tls::ClientOpts po;
        po.verify = "pin";
        po.pin = pass == 0 ? info["fingerprint"].get<std::string>() : std::string(64, '0');
        std::string e2;
        bool ok = c2.connect(p[0], "picocam", po, e2);
        CHECK(pass == 0 ? ok : !ok);
        c2.close();
        close(p[0]);
        s2.join();
        close(p[1]);
    }
    // a certificate that does not belong to the key is refused
    CHECK(!tls::install("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n", "x").empty());
}

int main() {
    test_hashes();
    test_board();
    test_plates();
    test_text();
    test_tracker();
    test_geometry();
    test_faces();
    test_ipfilter();
    test_tls();
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
