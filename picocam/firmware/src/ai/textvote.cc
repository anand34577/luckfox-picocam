#include "textvote.h"

#include <algorithm>
#include <cstring>
#include <map>

std::string text_consensus(const std::vector<std::pair<std::string, float>> &reads, float *weight, int *support) {
    if (weight) *weight = 0;
    if (support) *support = 0;
    if (reads.empty()) return "";
    std::map<size_t, float> by_len;
    for (auto &r : reads) by_len[r.first.size()] += r.second;
    size_t len = 0;
    float best = -1;
    for (auto &kv : by_len)
        if (kv.second > best) best = kv.second, len = kv.first;
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        std::map<char, float> w;
        for (auto &r : reads)
            if (r.first.size() == len) w[r.first[i]] += r.second;
        char c = 0;
        float bw = -1;
        for (auto &kv : w)
            if (kv.second > bw) bw = kv.second, c = kv.first;
        out += c;
    }
    if (weight) *weight = best;
    if (support) {
        int n = 0;
        for (auto &r : reads) n += r.first == out;
        *support = n;
    }
    return out;
}

static bool look_alike(char a, char b) {
    static const char *groups[] = {"0ODQ", "1IL", "8B", "5S", "2Z", "6G", "4A", "7T"};
    for (auto *g : groups)
        if (strchr(g, a) && strchr(g, b)) return true;
    return false;
}

float text_distance(const std::string &a, const std::string &b) {
    size_t n = a.size(), m = b.size();
    std::vector<float> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (float)j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = (float)i;
        for (size_t j = 1; j <= m; ++j) {
            char x = (char)toupper((unsigned char)a[i - 1]), y = (char)toupper((unsigned char)b[j - 1]);
            float sub = x == y ? 0.f : (look_alike(x, y) ? 0.3f : 1.f);
            cur[j] = std::min({prev[j] + 1.f, cur[j - 1] + 1.f, prev[j - 1] + sub});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}
