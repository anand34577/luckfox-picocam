#pragma once
#include <string>
#include <vector>

#include "aicommon.h"
#include "imgproc.h"
#include "npu.h"

// Object detector. Rockchip exports of the YOLO family are understood out of the box and picked automatically from the
// model's outputs; anything else can say what it is in its manifest (decoder, anchors, ...) or bring its own decoder
// plugin (see pc_plugin.h). Boxes are returned in frame coords.
//   yolov5 : 3 heads, anchors (YOLOv5, YOLOv7; custom anchors from the manifest)
//   yolov8 : DFL heads, 6 or 9 outputs (YOLOv8, YOLO11)
//   yolox  : 3 heads, anchor-free, 5+classes channels
//   yolov10: one output [N, 6] = x1 y1 x2 y2 score class, NMS-free (YOLOv10, YOLO26)
//   plugin : your own decoder
struct Plugin;
struct Yolov5 {
    NpuModel model;
    float conf = 0.35f, nms_thr = 0.45f;
    float min_box_px = 8;
    bool bgr = false;
    std::string decoder = "auto";
    std::vector<float> anchors;  // yolov5: 3 heads x 3 (w, h) pairs in model-input pixels; empty = COCO defaults
    bool class_agnostic = false;  // one object may only have one class (suppress across classes)
    bool stretch = false;         // resize without letterbox padding (models trained that way)
    std::shared_ptr<Plugin> plugin;
    bool load(const std::string &path) { return model.load(path); }
    std::vector<Det> detect(const Frame &f, RectF roi);
    std::string effective_decoder() const;

private:
    void decode_v5(const Letterbox &lb, const Frame &f, std::vector<Det> &dets);
    void decode_v8(const Letterbox &lb, const Frame &f, std::vector<Det> &dets);
    void decode_yolox(const Letterbox &lb, const Frame &f, std::vector<Det> &dets);
    void decode_v10(const Letterbox &lb, const Frame &f, std::vector<Det> &dets);
    void decode_plugin(const Letterbox &lb, const Frame &f, std::vector<Det> &dets);
    void push(std::vector<Det> &dets, const Letterbox &lb, const Frame &f, RectF m, float score, int cls);
};

// RetinaFace (mobilenet0.25) with 5 landmarks. Boxes/landmarks in frame coords.
struct RetinaFace {
    NpuModel model;
    float conf = 0.5f, nms_thr = 0.3f;
    bool bgr = true;
    int tiles_x = 2, tiles_y = 2;  // frame is analysed as overlapping tiles
    bool load(const std::string &path);
    std::vector<Det> detect(const Frame &f);

private:
    std::vector<Det> detect_roi(const Frame &f, RectF roi);
    std::vector<float> priors_;  // cx, cy, w, h (normalised)
};

// Face embedding (MobileFaceNet / FaceNet style). Returns an L2-normalised vector.
struct FaceEmbedder {
    NpuModel model;
    bool bgr = true;
    bool load(const std::string &path) { return model.load(path); }
    // with landmarks (RetinaFace) the face is aligned to the standard 112x112 template first, which matters a lot for
    // recognition accuracy; without them the box is cropped as is
    std::vector<float> embed(const Frame &f, const Det &face);
};

// CTC plate recogniser (ocr/train.py PlateNet, or LPRNet-style). Output tensor is [1, classes, steps]
// with blank as the last class; input is the plate crop stretched to the model size.
struct PlateOcr {
    NpuModel model;
    std::string charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    bool bgr = false;
    bool time_major = false;  // output is [steps, classes] instead of [classes, steps]
    int blank = -1;           // index of the CTC blank class; -1 = the last one (0 = first, as PaddleOCR)
    bool load(const std::string &path) { return model.load(path); }
    // returns raw text and mean per-character confidence
    std::string read(const Frame &f, RectF plate, float *conf);
};

// Indian registration formats (MH12AB1234, DL3CAB1234, 22BH1234AB). Fixes common OCR letter/digit
// confusions by position. Returns the cleaned text; *valid tells whether it matches a known format.
std::string normalize_indian_plate(const std::string &raw, bool *valid);

// Image classifier (MobileNet/ResNet/YOLO-cls style): one output of class scores (logits or probabilities).
// The crop is stretched to the model input. Returns the top class and its probability.
struct Classifier {
    NpuModel model;
    bool bgr = false;
    bool center_crop = false;  // take the central square of the crop instead of stretching it
    bool load(const std::string &path) { return model.load(path); }
    int classify(const Frame &f, RectF roi, float *prob);
};
