/* PicoCam decoder plugin ABI.
 *
 * For a model whose output layout PicoCam does not know, write a decoder as a small shared library, cross-compile it
 * for the camera (arm-rockchip830-linux-uclibcgnueabihf-gcc -shared -fPIC -O2), upload it in AI > Models > Plugins
 * (administrators only: a plugin runs inside the camera daemon) and set the model's decoder to "plugin" with the plugin's
 * file name. See sdk/example_decoder.c.
 *
 * pc_decode() is called after every inference of the model. Boxes are returned in MODEL INPUT pixels (the camera maps
 * them back to the picture, undoing letterboxing).
 */
#ifndef PC_PLUGIN_H
#define PC_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

enum { PC_INT8 = 0, PC_UINT8 = 1, PC_FLOAT32 = 2, PC_FLOAT16 = 3 };

typedef struct {
    int n_dims;
    int dims[4];          /* NHWC order as the NPU stores it */
    int type;             /* PC_INT8 ... */
    int zp;               /* quantisation: real = (q - zp) * scale */
    float scale;
    const void *data;     /* contiguous, n_elems elements of `type` */
    unsigned n_elems;
} pc_tensor;

typedef struct {
    float x1, y1, x2, y2; /* model input pixels */
    float score;
    int cls;
} pc_det;

/* returns the number of detections written to `dets` (at most max_dets) */
int pc_decode(const pc_tensor *outs, int n_outs, int in_w, int in_h, float conf, pc_det *dets, int max_dets);

#ifdef __cplusplus
}
#endif
#endif
