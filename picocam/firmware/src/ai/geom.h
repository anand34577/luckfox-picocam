#pragma once

// Least-squares similarity transform (scale + rotation + translation, no reflection) mapping n source points onto n
// destination points: dst ~ [a -b; b a] * src + (tx, ty). m = {a, -b, tx, b, a, ty} (row-major 2x3, the layout of
// cv::warpAffine). Used to align faces on their landmarks. Returns false for degenerate input.
bool similarity_transform(const float *src_xy, const float *dst_xy, int n, float m[6]);
