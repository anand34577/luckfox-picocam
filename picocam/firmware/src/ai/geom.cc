#include "geom.h"

#include <cmath>

bool similarity_transform(const float *src, const float *dst, int n, float m[6]) {
    if (n < 2) return false;
    double mpx = 0, mpy = 0, mqx = 0, mqy = 0;
    for (int i = 0; i < n; ++i) {
        mpx += src[2 * i], mpy += src[2 * i + 1];
        mqx += dst[2 * i], mqy += dst[2 * i + 1];
    }
    mpx /= n, mpy /= n, mqx /= n, mqy /= n;
    double sdot = 0, scross = 0, var = 0;
    for (int i = 0; i < n; ++i) {
        double px = src[2 * i] - mpx, py = src[2 * i + 1] - mpy, qx = dst[2 * i] - mqx, qy = dst[2 * i + 1] - mqy;
        sdot += px * qx + py * qy;
        scross += px * qy - py * qx;
        var += px * px + py * py;
    }
    if (var < 1e-6) return false;
    double a = sdot / var, b = scross / var;
    m[0] = (float)a, m[1] = (float)-b, m[3] = (float)b, m[4] = (float)a;
    m[2] = (float)(mqx - (a * mpx - b * mpy));
    m[5] = (float)(mqy - (b * mpx + a * mpy));
    return true;
}
