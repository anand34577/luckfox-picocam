#include "detectors.h"

#include <cmath>
#include <cstring>

#include "geom.h"
#include "plugin.h"

// ---------------------------------------------------------------- YOLOv5

static const float kAnchors[3][6] = {{10, 13, 16, 30, 33, 23}, {30, 61, 62, 45, 59, 119}, {116, 90, 156, 198, 373, 326}};

// YOLOv8 / YOLO11 (Rockchip export): per scale a DFL box tensor (64 ch), a class tensor and
// optionally a 1-ch score-sum tensor used as a cheap pre-filter.
void Yolov5::decode_v8(const Letterbox &lb, const Frame &f, std::vector<Det> &dets) {
    struct Branch {
        int box = -1, cls = -1, sum = -1, h = 0, w = 0;
    };
    std::vector<Branch> br;
    for (int o = 0; o < model.num_outputs(); ++o) {
        const rknn_tensor_attr &a = model.out_attr(o);
        int h = a.dims[1], w = a.dims[2], c = a.dims[3];
        Branch *b = nullptr;
        for (auto &x : br)
            if (x.h == h && x.w == w) b = &x;
        if (!b) {
            br.push_back(Branch());
            b = &br.back();
            b->h = h;
            b->w = w;
        }
        // export order per scale is box, cls, sum (cls and sum are both 1-ch for single-class models)
        if (c == 64) b->box = o;
        else if (b->cls < 0) b->cls = o;
        else b->sum = o;
    }
    for (auto &b : br) {
        if (b.box < 0 || b.cls < 0) continue;
        int ncls = model.out_attr(b.cls).dims[3];
        float stride = (float)model.in_h() / b.h;
        const int8_t *pc = model.output(b.cls), *pb = model.output(b.box);
        // the sum map is only a pre-filter; skip it if its quantised range cannot reach the threshold
        bool use_sum = b.sum >= 0 && model.deq(b.sum, 127) > conf;
        const int8_t *ps = use_sum ? model.output(b.sum) : nullptr;
        int8_t sum_thr = b.sum >= 0 ? model.qnt(b.sum, conf) : 0;
        int8_t cls_thr = model.qnt(b.cls, conf);
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x) {
                int cell = y * b.w + x;
                if (ps && ps[cell] < sum_thr) continue;
                const int8_t *c = pc + (size_t)cell * ncls;
                int best = 0;
                for (int k = 1; k < ncls; ++k)
                    if (c[k] > c[best]) best = k;
                if (c[best] < cls_thr) continue;
                float score = model.deq(b.cls, c[best]);
                if (score < conf) continue;
                // DFL: 4 sides x 16 bins, softmax expectation = distance in grid cells
                const int8_t *d = pb + (size_t)cell * 64;
                float dist[4];
                for (int s = 0; s < 4; ++s) {
                    float mx = -1e9f, e[16], sum = 0, acc = 0;
                    for (int k = 0; k < 16; ++k) mx = std::max(mx, e[k] = model.deq(b.box, d[s * 16 + k]));
                    for (int k = 0; k < 16; ++k) sum += (e[k] = std::exp(e[k] - mx)), acc += e[k] * k;
                    dist[s] = acc / sum;
                }
                RectF m{(x + 0.5f - dist[0]) * stride, (y + 0.5f - dist[1]) * stride, (x + 0.5f + dist[2]) * stride,
                        (y + 0.5f + dist[3]) * stride};
                Det det;
                det.box = lb.to_frame(m).clip((float)f.w, (float)f.h);
                det.score = score;
                det.cls = best;
                if (det.box.w() >= min_box_px && det.box.h() >= min_box_px) dets.push_back(det);
            }
    }
}

void Yolov5::push(std::vector<Det> &dets, const Letterbox &lb, const Frame &f, RectF m, float score, int cls) {
    Det d;
    d.box = lb.to_frame(m).clip((float)f.w, (float)f.h);
    d.score = score;
    d.cls = cls;
    // sliver boxes are always noise (blur, reflections)
    if (d.box.w() >= min_box_px && d.box.h() >= min_box_px) dets.push_back(d);
}

std::string Yolov5::effective_decoder() const {
    if (decoder != "auto") return decoder;
    int n = model.num_outputs();
    if (n == 6 || n == 9) return "yolov8";
    if (n == 1) return "yolov10";
    if (n == 3) {
        int C = model.out_attr(0).dims[3];
        // yolov5 heads carry 3 anchors x (5 + classes) channels; yolox heads just 5 + classes
        return (C % 3 == 0 && C / 3 - 5 >= 1) ? "yolov5" : "yolox";
    }
    return "yolov8";
}

// YOLOv5 / YOLOv7: anchor-based heads. Heads are ordered by stride; anchors come from the manifest or are the COCO ones.
void Yolov5::decode_v5(const Letterbox &lb, const Frame &f, std::vector<Det> &dets) {
    // rank of each head by grid size (largest grid = stride 8 = anchors 0)
    std::vector<int> grids;
    for (int o = 0; o < model.num_outputs(); ++o) grids.push_back(model.out_attr(o).dims[1]);
    std::vector<int> sorted = grids;
    std::sort(sorted.rbegin(), sorted.rend());
    for (int o = 0; o < model.num_outputs(); ++o) {
        const rknn_tensor_attr &a = model.out_attr(o);
        int gh = a.dims[1], gw = a.dims[2], C = a.dims[3];
        // data is packed at C per cell; size_with_stride only reflects the (padded) allocation
        int cstride = C;
        int stride = model.in_h() / gh;
        int si = (int)(std::find(sorted.begin(), sorted.end(), gh) - sorted.begin());
        si = std::min(si, 2);
        int per_anchor = C / 3, ncls = per_anchor - 5;
        if (ncls < 1) continue;
        int8_t thr = model.qnt(o, conf);
        const int8_t *base = model.output(o);
        const float *anc = anchors.size() >= 18 ? &anchors[si * 6] : kAnchors[si];
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const int8_t *cell = base + (size_t)(y * gw + x) * cstride;
                for (int an = 0; an < 3; ++an) {
                    const int8_t *p = cell + an * per_anchor;
                    if (p[4] < thr) continue;
                    int best = 0;
                    for (int k = 1; k < ncls; ++k)
                        if (p[5 + k] > p[5 + best]) best = k;
                    float score = model.deq(o, p[4]) * model.deq(o, p[5 + best]);
                    if (score < conf) continue;
                    float bx = (model.deq(o, p[0]) * 2.f - 0.5f + x) * stride;
                    float by = (model.deq(o, p[1]) * 2.f - 0.5f + y) * stride;
                    float bw = model.deq(o, p[2]) * 2.f, bh = model.deq(o, p[3]) * 2.f;
                    bw = bw * bw * anc[an * 2];
                    bh = bh * bh * anc[an * 2 + 1];
                    push(dets, lb, f, {bx - bw / 2, by - bh / 2, bx + bw / 2, by + bh / 2}, score, best);
                }
            }
    }
}

// YOLOX: anchor-free; per cell x y w h obj cls...; x, y are offsets in cells, w, h are log-scale; obj and cls are sigmoid-ed
void Yolov5::decode_yolox(const Letterbox &lb, const Frame &f, std::vector<Det> &dets) {
    for (int o = 0; o < model.num_outputs(); ++o) {
        const rknn_tensor_attr &a = model.out_attr(o);
        int gh = a.dims[1], gw = a.dims[2], C = a.dims[3], ncls = C - 5;
        if (ncls < 1) continue;
        int stride = model.in_h() / gh;
        int8_t thr = model.qnt(o, conf);
        const int8_t *base = model.output(o);
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const int8_t *p = base + (size_t)(y * gw + x) * C;
                if (p[4] < thr) continue;
                int best = 0;
                for (int k = 1; k < ncls; ++k)
                    if (p[5 + k] > p[5 + best]) best = k;
                float score = model.deq(o, p[4]) * model.deq(o, p[5 + best]);
                if (score < conf) continue;
                float cx = (model.deq(o, p[0]) + x) * stride, cy = (model.deq(o, p[1]) + y) * stride;
                float w = std::exp(model.deq(o, p[2])) * stride, h = std::exp(model.deq(o, p[3])) * stride;
                push(dets, lb, f, {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}, score, best);
            }
    }
}

// YOLOv10 and other end-to-end exports: rows of x1 y1 x2 y2 score class, already de-duplicated
void Yolov5::decode_v10(const Letterbox &lb, const Frame &f, std::vector<Det> &dets) {
    const rknn_tensor_attr &a = model.out_attr(0);
    size_t rows = a.n_elems / 6;
    // some exports are normalised (0..1) instead of model-input pixels
    float maxv = 0;
    for (size_t r = 0; r < rows && r < 20; ++r) maxv = std::max(maxv, std::max(model.outf(0, r * 6 + 2), model.outf(0, r * 6 + 3)));
    float sx = maxv <= 1.5f ? (float)model.in_w() : 1.f, sy = maxv <= 1.5f ? (float)model.in_h() : 1.f;
    for (size_t r = 0; r < rows; ++r) {
        float score = model.outf(0, r * 6 + 4);
        if (score < conf) continue;
        push(dets, lb, f, {model.outf(0, r * 6) * sx, model.outf(0, r * 6 + 1) * sy, model.outf(0, r * 6 + 2) * sx, model.outf(0, r * 6 + 3) * sy},
             score, (int)std::lround(model.outf(0, r * 6 + 5)));
    }
}

void Yolov5::decode_plugin(const Letterbox &lb, const Frame &f, std::vector<Det> &dets) {
    if (!plugin || !plugin->decode) return;
    std::vector<pc_tensor> ts(model.num_outputs());
    for (int o = 0; o < model.num_outputs(); ++o) {
        const rknn_tensor_attr &a = model.out_attr(o);
        pc_tensor &t = ts[o];
        t.n_dims = std::min<int>(4, a.n_dims);
        for (int i = 0; i < 4; ++i) t.dims[i] = i < (int)a.n_dims ? a.dims[i] : 1;
        t.type = a.type == RKNN_TENSOR_FLOAT32 ? PC_FLOAT32 : a.type == RKNN_TENSOR_FLOAT16 ? PC_FLOAT16 : a.type == RKNN_TENSOR_UINT8 ? PC_UINT8 : PC_INT8;
        t.zp = a.zp;
        t.scale = a.scale;
        t.data = model.output(o);
        t.n_elems = a.n_elems;
    }
    pc_det out[300];
    int n = plugin->decode(ts.data(), (int)ts.size(), model.in_w(), model.in_h(), conf, out, 300);
    for (int i = 0; i < n && i < 300; ++i) push(dets, lb, f, {out[i].x1, out[i].y1, out[i].x2, out[i].y2}, out[i].score, out[i].cls);
}

std::vector<Det> Yolov5::detect(const Frame &f, RectF roi) {
    std::vector<Det> dets;
    if (roi.empty()) roi = {0, 0, (float)f.w, (float)f.h};
    Letterbox lb = model.set_input(f, roi, !stretch, bgr);
    if (!model.run()) return dets;
    std::string dec = effective_decoder();
    if (dec == "yolov5") decode_v5(lb, f, dets);
    else if (dec == "yolox") decode_yolox(lb, f, dets);
    else if (dec == "yolov10") {
        decode_v10(lb, f, dets);
        return dets;  // already suppressed
    } else if (dec == "plugin") decode_plugin(lb, f, dets);
    else decode_v8(lb, f, dets);
    nms(dets, nms_thr, !class_agnostic);
    return dets;
}

// ---------------------------------------------------------------- RetinaFace

bool RetinaFace::load(const std::string &path) {
    if (!model.load(path)) return false;
    // standard RetinaFace prior boxes
    const int steps[3] = {8, 16, 32};
    const int min_sizes[3][2] = {{16, 32}, {64, 128}, {256, 512}};
    int W = model.in_w(), H = model.in_h();
    priors_.clear();
    for (int s = 0; s < 3; ++s) {
        int fh = (int)std::ceil((float)H / steps[s]), fw = (int)std::ceil((float)W / steps[s]);
        for (int i = 0; i < fh; ++i)
            for (int j = 0; j < fw; ++j)
                for (int m = 0; m < 2; ++m) {
                    priors_.push_back((j + 0.5f) * steps[s] / W);
                    priors_.push_back((i + 0.5f) * steps[s] / H);
                    priors_.push_back((float)min_sizes[s][m] / W);
                    priors_.push_back((float)min_sizes[s][m] / H);
                }
    }
    LOGI("ai", "retinaface: %zu priors", priors_.size() / 4);
    return model.num_outputs() >= 3;
}

std::vector<Det> RetinaFace::detect(const Frame &f) {
    // overlapping tiles keep small faces above the detector's 16 px minimum
    std::vector<Det> all;
    float tw = (float)f.w / tiles_x, th = (float)f.h / tiles_y;
    float ox = tiles_x > 1 ? tw * 0.15f : 0, oy = tiles_y > 1 ? th * 0.15f : 0;
    for (int ty = 0; ty < tiles_y; ++ty)
        for (int tx = 0; tx < tiles_x; ++tx) {
            RectF r{tx * tw - ox, ty * th - oy, (tx + 1) * tw + ox, (ty + 1) * th + oy};
            auto d = detect_roi(f, r.clip((float)f.w, (float)f.h));
            all.insert(all.end(), d.begin(), d.end());
        }
    if (tiles_x * tiles_y > 1) nms(all, nms_thr, false);
    return all;
}

std::vector<Det> RetinaFace::detect_roi(const Frame &f, RectF roi) {
    std::vector<Det> dets;
    Letterbox lb = model.set_input(f, roi, true, bgr);
    if (!model.run()) return dets;

    size_t n = priors_.size() / 4;
    // elements per prior (4 box, 2 score, 10 landmark); data is packed even when the buffer is padded
    size_t ls = model.out_attr(0).n_elems / n, cs = model.out_attr(1).n_elems / n, ms = model.out_attr(2).n_elems / n;
    const float V0 = 0.1f, V1 = 0.2f;
    float W = (float)model.in_w(), H = (float)model.in_h();
    for (size_t i = 0; i < n; ++i) {
        float score = model.outf(1, i * cs + 1);
        if (score < conf) continue;
        const float *pr = &priors_[i * 4];
        float cx = pr[0] + model.outf(0, i * ls + 0) * V0 * pr[2];
        float cy = pr[1] + model.outf(0, i * ls + 1) * V0 * pr[3];
        float bw = pr[2] * std::exp(model.outf(0, i * ls + 2) * V1);
        float bh = pr[3] * std::exp(model.outf(0, i * ls + 3) * V1);
        Det d;
        d.score = score;
        d.box = lb.to_frame({(cx - bw / 2) * W, (cy - bh / 2) * H, (cx + bw / 2) * W, (cy + bh / 2) * H});
        d.has_lm = true;
        for (int k = 0; k < 5; ++k) {
            d.lm[2 * k] = lb.to_frame_x((pr[0] + model.outf(2, i * ms + 2 * k) * V0 * pr[2]) * W);
            d.lm[2 * k + 1] = lb.to_frame_y((pr[1] + model.outf(2, i * ms + 2 * k + 1) * V0 * pr[3]) * H);
        }
        d.box = d.box.clip((float)f.w, (float)f.h);
        if (!d.box.empty()) dets.push_back(d);
    }
    nms(dets, nms_thr, false);
    return dets;
}

// ---------------------------------------------------------------- Face embedding

std::vector<float> FaceEmbedder::embed(const Frame &f, const Det &face) {
    if (face.has_lm && align_face(f, face.lm, model.input(), model.in_w(), model.in_h(), model.in_stride(), bgr)) {
        // aligned on the landmarks
    } else {
        // square crop around the face with a small margin (matches the Luckfox facenet preprocessing)
        float side = std::max(face.box.w(), face.box.h()) * 1.1f;
        RectF roi{face.box.cx() - side / 2, face.box.cy() - side / 2, face.box.cx() + side / 2, face.box.cy() + side / 2};
        model.set_input(f, roi, true, bgr);
    }
    std::vector<float> v;
    if (!model.run()) return v;
    const rknn_tensor_attr &a = model.out_attr(0);
    v.resize(a.n_elems);
    double sum = 0;
    for (uint32_t i = 0; i < a.n_elems; ++i) {
        v[i] = model.outf(0, i);
        sum += v[i] * v[i];
    }
    float norm = (float)std::sqrt(sum) + 1e-6f;
    for (auto &x : v) x /= norm;
    return v;
}

// ---------------------------------------------------------------- Plate OCR

std::string PlateOcr::read(const Frame &f, RectF plate, float *conf) {
    *conf = 0;
    model.set_input(f, plate, false, bgr);
    if (!model.run()) return "";
    const rknn_tensor_attr &a = model.out_attr(0);
    // find (classes, steps) among the dims: classes == charset + blank
    int ncls = (int)charset.size() + 1, steps = 0;
    for (uint32_t d = 0; d < a.n_dims; ++d)
        if ((int)a.dims[d] != ncls && a.dims[d] > 1) steps = a.dims[d];
    if (steps == 0 || (int)a.n_elems != ncls * steps) {
        LOGW("ai", "ocr output shape does not match charset (%u elems, %d classes)", a.n_elems, ncls);
        return "";
    }
    std::string out;
    int blank_idx = blank < 0 || blank >= ncls ? ncls - 1 : blank;
    int prev = blank_idx;
    float conf_sum = 0;
    int nchar = 0;
    for (int t = 0; t < steps; ++t) {
        int best = 0;
        float bv = -1e9f, sum = 0;
        std::vector<float> logit(ncls);
        for (int c = 0; c < ncls; ++c) {
            logit[c] = model.outf(0, time_major ? (size_t)t * ncls + c : (size_t)c * steps + t);
            if (logit[c] > bv) bv = logit[c], best = c;
        }
        for (int c = 0; c < ncls; ++c) sum += std::exp(logit[c] - bv);
        if (best != blank_idx && best != prev) {
            int ci = best > blank_idx ? best - 1 : best;  // the charset has no entry for the blank
            if (ci >= 0 && ci < (int)charset.size()) {
                out += charset[ci];
                conf_sum += 1.f / sum;  // softmax prob of the winning class
                ++nchar;
            }
        }
        prev = best;
    }
    *conf = nchar ? conf_sum / nchar : 0;
    return out;
}

// ---------------------------------------------------------------- Indian plate format

static const char *kStates[] = {"AN", "AP", "AR", "AS", "BR", "CH", "CG", "DD", "DL", "DN", "GA", "GJ", "HR",
                                "HP", "JH", "JK", "KA", "KL", "LA", "LD", "MH", "ML", "MN", "MP", "MZ", "NL",
                                "OD", "OR", "PB", "PY", "RJ", "SK", "TN", "TR", "TS", "TG", "UK", "UA", "UP", "WB"};

static char to_digit(char c) {
    switch (c) {
        case 'O': case 'D': case 'Q': case 'U': return '0';
        case 'I': case 'L': case 'J': return '1';
        case 'Z': return '2';
        case 'A': return '4';
        case 'S': return '5';
        case 'G': return '6';
        case 'T': return '7';
        case 'B': return '8';
        default: return c;
    }
}
static char to_letter(char c) {
    switch (c) {
        case '0': return 'O';
        case '1': return 'I';
        case '2': return 'Z';
        case '4': return 'A';
        case '5': return 'S';
        case '6': return 'G';
        case '7': return 'T';
        case '8': return 'B';
        default: return c;
    }
}
static bool is_state(const std::string &s) {
    for (auto *st : kStates)
        if (s == st) return true;
    return false;
}

std::string normalize_indian_plate(const std::string &raw, bool *valid) {
    std::string s;
    for (char c : raw)
        if (isalnum((unsigned char)c)) s += (char)toupper((unsigned char)c);
    *valid = false;
    int n = (int)s.size();
    if (n < 6 || n > 11) return s;

    // BH series: YY BH #### L(L)
    if (n >= 9 && n <= 10) {
        std::string t = s;
        for (int i = 0; i < 2; ++i) t[i] = to_digit(t[i]);
        t[2] = to_letter(t[2]);
        t[3] = to_letter(t[3]);
        for (int i = 4; i < 8; ++i) t[i] = to_digit(t[i]);
        for (int i = 8; i < n; ++i) t[i] = to_letter(t[i]);
        if (t.compare(2, 2, "BH") == 0) {
            *valid = isdigit(t[0]) && isdigit(t[1]) && isalpha(t[n - 1]);
            if (*valid) return t;
        }
    }

    // Standard: SS D(D) L{0,3} D{1,4}; try 2-digit then 1-digit district, pick the cleanest
    std::string best = s;
    int best_cost = 1 << 20;
    for (int dd = 2; dd >= 1; --dd) {
        for (int tail = 4; tail >= 1; --tail) {
            int letters = n - 2 - dd - tail;
            if (letters < 0 || letters > 3) continue;
            std::string t = s;
            int cost = 0;
            auto fix = [&](int i, bool digit) {
                char c = digit ? to_digit(t[i]) : to_letter(t[i]);
                if (c != t[i]) ++cost;
                t[i] = c;
                return digit ? isdigit((unsigned char)c) : isalpha((unsigned char)c);
            };
            bool ok = fix(0, false) && fix(1, false);
            for (int i = 2; ok && i < 2 + dd; ++i) ok = fix(i, true);
            for (int i = 2 + dd; ok && i < 2 + dd + letters; ++i) ok = fix(i, false);
            for (int i = n - tail; ok && i < n; ++i) ok = fix(i, true);
            if (!ok || !is_state(t.substr(0, 2))) continue;
            cost += (4 - tail) * 2 + (dd == 1 && t.compare(0, 2, "DL") != 0 ? 3 : 0);  // prefer common shapes
            if (cost < best_cost) best_cost = cost, best = t;
        }
    }
    *valid = best_cost < (1 << 20);
    return best;
}

// ---------------------------------------------------------------- Classifier

int Classifier::classify(const Frame &f, RectF roi, float *prob) {
    *prob = 0;
    if (roi.empty()) roi = {0, 0, (float)f.w, (float)f.h};
    if (center_crop) {
        float side = std::min(roi.w(), roi.h());
        roi = {roi.cx() - side / 2, roi.cy() - side / 2, roi.cx() + side / 2, roi.cy() + side / 2};
    }
    model.set_input(f, roi, false, bgr);
    if (!model.run()) return -1;
    const rknn_tensor_attr &a = model.out_attr(0);
    int n = (int)a.n_elems;
    if (n <= 0) return -1;
    std::vector<float> v(n);
    float mx = -1e9f, sum = 0;
    bool is_prob = true;
    for (int i = 0; i < n; ++i) {
        v[i] = model.outf(0, i);
        mx = std::max(mx, v[i]);
        sum += v[i];
        if (v[i] < -1e-3f || v[i] > 1.001f) is_prob = false;
    }
    // already a distribution (softmax inside the model)? otherwise apply softmax
    if (!(is_prob && std::fabs(sum - 1.f) < 0.05f)) {
        sum = 0;
        for (auto &x : v) sum += (x = std::exp(x - mx));
        for (auto &x : v) x /= sum;
    }
    int best = (int)(std::max_element(v.begin(), v.end()) - v.begin());
    *prob = v[best];
    return best;
}
