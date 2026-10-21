/* Example decoder plugin: a single-output detector whose output is rows of
 *   [cx, cy, w, h, objectness, class scores...]   in model-input pixels
 * (the layout of many "raw" YOLO-style exports). Copy it, change the decode, build for the camera:
 *
 *   docker run --rm -v "$PWD":/w -w /w luckfox-ai:1.1 sh -c \
 *     '/opt/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc -shared -fPIC -O2 -o my_decoder.so example_decoder.c -I.'
 *
 * Upload my_decoder.so in AI > Models > Decoder plugins (administrator), then give your model the manifest
 * {"decoder": "plugin", "plugin": "my_decoder.so"} (or build a package with models/package.py --plugin).
 */
#include <math.h>
#include "pc_plugin.h"

static float value(const pc_tensor *t, unsigned i) {
    switch (t->type) {
        case PC_FLOAT32: return ((const float *)t->data)[i];
        case PC_UINT8: return ((float)((const unsigned char *)t->data)[i] - t->zp) * t->scale;
        default: return ((float)((const signed char *)t->data)[i] - t->zp) * t->scale;  /* PC_INT8 */
    }
}

int pc_decode(const pc_tensor *outs, int n_outs, int in_w, int in_h, float conf, pc_det *dets, int max_dets) {
    if (n_outs < 1) return 0;
    const pc_tensor *t = &outs[0];
    int cols = t->dims[t->n_dims - 1];       /* 5 + number of classes */
    int rows = cols ? (int)(t->n_elems / cols) : 0;
    int n = 0;
    for (int r = 0; r < rows && n < max_dets; ++r) {
        float obj = value(t, r * cols + 4);
        if (obj < conf) continue;
        int best = 0;
        for (int k = 1; k < cols - 5; ++k)
            if (value(t, r * cols + 5 + k) > value(t, r * cols + 5 + best)) best = k;
        float score = obj * value(t, r * cols + 5 + best);
        if (score < conf) continue;
        float cx = value(t, r * cols), cy = value(t, r * cols + 1), w = value(t, r * cols + 2), h = value(t, r * cols + 3);
        dets[n].x1 = cx - w / 2, dets[n].y1 = cy - h / 2, dets[n].x2 = cx + w / 2, dets[n].y2 = cy + h / 2;
        dets[n].score = score, dets[n].cls = best;
        ++n;
    }
    (void)in_w; (void)in_h;
    return n;  /* the camera runs NMS on what you return */
}
