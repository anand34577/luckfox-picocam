#include "rawstage.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "core/util.h"
#include "net/httpc.h"

json RawModel::run(const Frame &f, RectF roi) {
    if (roi.empty()) roi = {0, 0, (float)f.w, (float)f.h};
    nv12_to_rgb(f, roi, model.input(), model.in_w(), model.in_h(), model.in_stride(), false, bgr);
    if (!model.run()) return json();
    json outs = json::array();
    for (int o = 0; o < model.num_outputs(); ++o) {
        const rknn_tensor_attr &a = model.out_attr(o);
        size_t esz = a.type == RKNN_TENSOR_FLOAT32 ? 4 : a.type == RKNN_TENSOR_FLOAT16 ? 2 : 1;
        std::string dims;
        for (uint32_t i = 0; i < a.n_dims; ++i) dims += (i ? "x" : "") + std::to_string(a.dims[i]);
        outs.push_back({{"dims", dims},
                        {"type", a.type == RKNN_TENSOR_FLOAT32 ? "float32" : a.type == RKNN_TENSOR_FLOAT16 ? "float16" : a.type == RKNN_TENSOR_UINT8 ? "uint8" : "int8"},
                        {"scale", a.scale},
                        {"zp", a.zp},
                        {"data", base64_encode((const uint8_t *)model.output(o), (size_t)a.n_elems * esz)}});
    }
    return {{"outputs", outs}, {"input", {{"w", model.in_w()}, {"h", model.in_h()}}}};
}

void RawModel::bench(const Frame &f, int n, double *avg_ms, double *min_ms) {
    double sum = 0, best = 1e9;
    for (int i = 0; i < n; ++i) {
        int64_t t0 = now_us();
        nv12_to_rgb(f, RectF{0, 0, (float)f.w, (float)f.h}, model.input(), model.in_w(), model.in_h(), model.in_stride(), false, bgr);
        model.run();
        double ms = (now_us() - t0) / 1000.0;
        sum += ms;
        best = std::min(best, ms);
    }
    *avg_ms = n ? sum / n : 0;
    *min_ms = best == 1e9 ? 0 : best;
}

namespace rawstage {

struct Req {
    std::string url, verify, pin;
    int track = 0;
    json body;
};

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<Req> g_q;
static std::vector<std::pair<int, json>> g_results;
static std::thread g_th;
static bool g_run = false;

static void loop() {
    while (true) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_q.empty() || !g_run; });
            if (!g_run) return;
            r = std::move(g_q.front());
            g_q.pop_front();
        }
        tls::ClientOpts o;
        o.verify = r.verify.empty() ? "system" : r.verify;
        o.pin = r.pin;
        auto res = httpc::request("POST", r.url, "application/json", r.body.dump(-1, ' ', false, json::error_handler_t::replace), {}, 5, o);
        if (res.status / 100 != 2) {
            LOGW("raw", "%s: %s", r.url.c_str(), res.error.empty() ? ("HTTP " + std::to_string(res.status)).c_str() : res.error.c_str());
            continue;
        }
        json j = json::parse(res.body, nullptr, false);
        if (j.is_object() && j.contains("attributes") && j["attributes"].is_object()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_results.push_back({r.track, j["attributes"]});
        }
    }
}

void send(const std::string &url, const std::string &verify, const std::string &pin, int track_id, json body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_run) {
        g_run = true;
        g_th = std::thread(loop);
    }
    if (g_q.size() >= 4) g_q.pop_front();  // inference is faster than the network: keep the freshest
    g_q.push_back({url, verify, pin, track_id, std::move(body)});
    g_cv.notify_one();
}

std::vector<std::pair<int, json>> take_results() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::pair<int, json>> out;
    out.swap(g_results);
    return out;
}

void stop() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_run = false;
        g_cv.notify_all();
    }
    if (g_th.joinable()) g_th.join();
}

}  // namespace rawstage
