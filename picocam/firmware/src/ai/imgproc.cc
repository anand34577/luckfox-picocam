#include "imgproc.h"

#include "geom.h"

#include <cstring>

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

static inline int even(float v) { return ((int)v) & ~1; }

Letterbox nv12_to_rgb(const Frame &f, RectF roi, uint8_t *dst, int dst_w, int dst_h, int dst_stride,
                      bool keep_aspect, bool bgr) {
    roi = roi.clip((float)f.w, (float)f.h);
    int x = even(roi.x1), y = even(roi.y1);
    int w = std::max(2, even(roi.x2) - x), h = std::max(2, even(roi.y2) - y);
    Letterbox lb;
    lb.roi = {(float)x, (float)y, (float)(x + w), (float)(y + h)};

    int tw = dst_w, th = dst_h;
    if (keep_aspect) {
        float s = std::min((float)dst_w / w, (float)dst_h / h);
        tw = std::max(2, (int)(w * s) & ~1);
        th = std::max(2, (int)(h * s) & ~1);
    }
    lb.sx = (float)tw / w;
    lb.sy = (float)th / h;
    lb.pad_x = (dst_w - tw) / 2;
    lb.pad_y = (dst_h - th) / 2;

    cv::Mat Y(f.h, f.w, CV_8UC1, (void *)f.nv12, f.stride);
    cv::Mat UV(f.h / 2, f.w / 2, CV_8UC2, (void *)(f.nv12 + (size_t)f.stride * f.h), f.stride);
    cv::Mat ys, uvs, rgb;
    cv::resize(Y(cv::Rect(x, y, w, h)), ys, cv::Size(tw, th), 0, 0, cv::INTER_LINEAR);
    cv::resize(UV(cv::Rect(x / 2, y / 2, w / 2, h / 2)), uvs, cv::Size(tw / 2, th / 2), 0, 0, cv::INTER_LINEAR);
    cv::cvtColorTwoPlane(ys, uvs, rgb, bgr ? cv::COLOR_YUV2BGR_NV12 : cv::COLOR_YUV2RGB_NV12);

    cv::Mat out(dst_h, dst_w, CV_8UC3, dst, dst_stride);
    if (tw != dst_w || th != dst_h) out.setTo(cv::Scalar(114, 114, 114));
    rgb.copyTo(out(cv::Rect(lb.pad_x, lb.pad_y, tw, th)));
    return lb;
}

bool align_face(const Frame &f, const float lm[10], uint8_t *dst, int dst_w, int dst_h, int dst_stride, bool bgr) {
    // ArcFace 112x112 template: left eye, right eye, nose, left mouth corner, right mouth corner
    static const float kT[10] = {38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f, 41.5493f, 92.3655f, 70.7299f, 92.2041f};
    float q[10];
    for (int i = 0; i < 5; ++i) q[2 * i] = kT[2 * i] * dst_w / 112.f, q[2 * i + 1] = kT[2 * i + 1] * dst_h / 112.f;
    float m[6];
    if (!similarity_transform(lm, q, 5, m)) return false;
    float det = m[0] * m[4] - m[1] * m[3];
    if (det < 1e-6f) return false;
    // the part of the picture the output covers: map the output corners back
    float xs[4], ys[4], cx[4] = {0, (float)dst_w, 0, (float)dst_w}, cy[4] = {0, 0, (float)dst_h, (float)dst_h};
    for (int i = 0; i < 4; ++i) {
        float dx = cx[i] - m[2], dy = cy[i] - m[5];
        xs[i] = (m[4] * dx - m[1] * dy) / det;
        ys[i] = (-m[3] * dx + m[0] * dy) / det;
    }
    RectF src{std::min({xs[0], xs[1], xs[2], xs[3]}), std::min({ys[0], ys[1], ys[2], ys[3]}),
              std::max({xs[0], xs[1], xs[2], xs[3]}), std::max({ys[0], ys[1], ys[2], ys[3]})};
    src = src.clip((float)f.w, (float)f.h);
    int x0 = even(src.x1), y0 = even(src.y1), w = even(src.x2) - x0, h = even(src.y2) - y0;
    if (w < 8 || h < 8 || w > 4096 || h > 4096) return false;
    std::vector<uint8_t> crop((size_t)w * h * 3);
    nv12_to_rgb(f, {(float)x0, (float)y0, (float)(x0 + w), (float)(y0 + h)}, crop.data(), w, h, w * 3, false, bgr);
    // output = M * (frame point); the crop's origin is at (x0, y0)
    float mc[6] = {m[0], m[1], m[2] + m[0] * x0 + m[1] * y0, m[3], m[4], m[5] + m[3] * x0 + m[4] * y0};
    // M maps frame -> output, M' maps crop -> output: x_out = a*(xc+x0) - b*(yc+y0) + tx
    mc[2] = m[2] + m[0] * x0 + m[1] * y0;
    mc[5] = m[5] + m[3] * x0 + m[4] * y0;
    cv::Mat in(h, w, CV_8UC3, crop.data()), out(dst_h, dst_w, CV_8UC3, dst, dst_stride);
    cv::Mat M(2, 3, CV_32F, mc);
    cv::warpAffine(in, out, M, cv::Size(dst_w, dst_h), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return true;
}

static void jpeg_sink(void *ctx, void *data, int size) {
    auto *v = (std::vector<uint8_t> *)ctx;
    v->insert(v->end(), (uint8_t *)data, (uint8_t *)data + size);
}

bool jpeg_encode(const Frame &f, RectF roi, int max_w, int min_w, int quality, std::vector<uint8_t> &out) {
    if (roi.empty()) roi = {0, 0, (float)f.w, (float)f.h};
    roi = roi.clip((float)f.w, (float)f.h);
    if (roi.empty()) return false;
    float s = 1.f;
    if (max_w > 0 && roi.w() > max_w) s = max_w / roi.w();
    if (min_w > 0 && roi.w() < min_w) s = min_w / roi.w();
    int w = std::max(2, (int)(roi.w() * s) & ~1), h = std::max(2, (int)(roi.h() * s) & ~1);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    nv12_to_rgb(f, roi, rgb.data(), w, h, w * 3, false);
    out.clear();
    out.reserve((size_t)w * h / 4);
    return stbi_write_jpg_to_func(jpeg_sink, &out, w, h, 3, rgb.data(), quality) != 0;
}

struct Yuv {
    uint8_t y, u, v;
};
static Yuv yuv_of(OverlayColor c) {
    switch (c) {
        case C_GREEN: return {149, 43, 21};
        case C_YELLOW: return {210, 16, 146};
        case C_RED: return {82, 90, 240};
        case C_BLUE: return {41, 240, 110};
        case C_ORANGE: return {151, 44, 201};
        default: return {235, 128, 128};
    }
}

static void fill(uint8_t *nv12, int w, int h, int stride, int x1, int y1, int x2, int y2, Yuv c) {
    x1 = std::max(0, x1) & ~1;
    y1 = std::max(0, y1) & ~1;
    x2 = std::min(w, x2 + 1) & ~1;
    y2 = std::min(h, y2 + 1) & ~1;
    if (x2 <= x1 || y2 <= y1) return;
    for (int y = y1; y < y2; ++y) memset(nv12 + (size_t)y * stride + x1, c.y, x2 - x1);
    uint8_t *uv = nv12 + (size_t)stride * h;
    for (int y = y1 / 2; y < y2 / 2; ++y) {
        uint8_t *p = uv + (size_t)y * stride + x1;
        for (int x = x1; x < x2; x += 2, p += 2) {
            p[0] = c.u;
            p[1] = c.v;
        }
    }
}

void draw_box(uint8_t *nv12, int w, int h, int stride, const RectF &r, OverlayColor col, int t) {
    Yuv c = yuv_of(col);
    int x1 = (int)r.x1, y1 = (int)r.y1, x2 = (int)r.x2, y2 = (int)r.y2;
    fill(nv12, w, h, stride, x1, y1, x2, y1 + t, c);
    fill(nv12, w, h, stride, x1, y2 - t, x2, y2, c);
    fill(nv12, w, h, stride, x1, y1, x1 + t, y2, c);
    fill(nv12, w, h, stride, x2 - t, y1, x2, y2, c);
}

void draw_label(uint8_t *nv12, int w, int h, int stride, int x, int y, const std::string &text, OverlayColor col) {
    if (text.empty()) return;
    const double scale = h >= 720 ? 0.8 : 0.55;
    int base = 0;
    cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, 2, &base);
    int bx = std::max(0, std::min(x, w - ts.width - 8));
    int by = y - ts.height - 10;
    if (by < 0) by = y + 2;
    fill(nv12, w, h, stride, bx, by, bx + ts.width + 8, by + ts.height + 10, yuv_of(col));
    cv::Mat Y(h, w, CV_8UC1, nv12, stride);
    // dark text on the coloured tag (luma only; chroma stays the tag colour)
    cv::putText(Y, text, cv::Point(bx + 4, by + ts.height + 4), cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(16), 2);
}
