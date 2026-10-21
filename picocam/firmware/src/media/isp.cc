// Camera ISP control and the Smart Exposure controller.
//
// Smart Exposure runs on top of the Rockchip AE. The AIQ AE converges the *mean* luma to a setpoint, which fails
// in exactly the scenes a security camera sees most: a bright sky or sunlit wall pushes the whole picture dark,
// and a dark scene with a few lamps comes out washed. Four times a second the controller looks at the luma
// histogram of the sub stream and nudges the AE's exposure bias so that
//   - mid-tones (the median) sit near the user's brightness target, judged in linear light (stops), not gamma values,
//   - highlights stay out of clipping unless that would crush most of the picture (backlight), in which case
//     it holds exposure for the subject and raises DRC (digital WDR) to recover shadows instead,
//   - anti-flicker (which forces exposure to multiples of 10 ms) is only engaged when the scene is dim enough
//     to need >= 10 ms anyway; in sunlight it would force over-exposure.
//
// What the statistics see matches what the user configured:
//   - the histogram is weighted with the same metering grid the AE uses ("lower half", "spot", ...),
//   - privacy masks are left out (a black rectangle would otherwise pull the median down and over-expose the rest),
//   - with AI subject metering the controller exposes for the tracked vehicle / person / face instead of the scene,
//   - scene classes (backlight / bright / normal) have hysteresis and dwell time and are blended, so the controller
//     does not hunt around a threshold; night scenes with lamps are told apart from daytime backlight,
//   - the shutter can be capped from the speed of the tracked objects, so plates are not blurred (motion shutter),
//   - day/night switching is automatic (gray mode, optional IR-cut and IR-LED GPIOs).
#include "isp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/board.h"
#include "core/log.h"
#include "core/util.h"
#include "media/video.h"
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_awb.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_user_api2_sysctl.h"

namespace isp {

static const char *TAG = "isp";

static rk_aiq_sys_ctx_t *g_ctx = nullptr;
static std::recursive_mutex g_mu;  // AIQ calls come from the HTTP thread and the controller thread
static json g_applied;             // last applied image settings
static int g_fps = 25;

static float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

// metering weights on the AE's 15x15 grid
static void fill_weights(const std::string &metering, uint8_t *w) {
    for (int r = 0; r < 15; ++r)
        for (int c = 0; c < 15; ++c) {
            float dx = (c - 7) / 7.f, dy = (r - 7) / 7.f, d = std::sqrt(dx * dx + dy * dy);
            int v = 1;
            if (metering == "average") v = 8;
            else if (metering == "spot") v = d < 0.3f ? 32 : (d < 0.45f ? 4 : 1);
            else if (metering == "lower") v = r >= 7 ? 24 - (14 - r) : 4;  // driveways, gates: ignore the sky
            else if (metering == "upper") v = r <= 7 ? 24 - r : 4;
            else v = (int)std::round(32 - 24 * std::min(1.f, d));  // center-weighted
            w[r * 15 + c] = (uint8_t)std::max(1, std::min(32, v));
        }
}

// ---------------------------------------------------------------- histogram feed

static std::mutex g_hist_mu;
static uint32_t g_hist[64], g_hist_roi[64];
static uint32_t g_hist_n = 0, g_hist_roi_n = 0;
static bool g_hist_want = true;
static int64_t g_hist_ms = 0;
static uint8_t g_w15[225];          // metering weights (same grid as the AE)
static uint8_t g_maskmap[64 * 36];  // 1 = covered by a privacy mask
static std::vector<std::array<float, 4>> g_boxes;
static int64_t g_boxes_ms = 0;
static float g_speed = 0;
static bool g_stat_init = false;

static void stat_init_locked() {
    if (g_stat_init) return;
    fill_weights("center", g_w15);
    g_stat_init = true;
}

void set_masks(const json &masks) {
    uint8_t m[64 * 36] = {0};
    if (masks.is_array())
        for (auto &k : masks) {
            if (!jget<bool>(k, "enabled", true)) continue;
            float x = jget<float>(k, "x", 0), y = jget<float>(k, "y", 0), w = jget<float>(k, "w", 0.1f), h = jget<float>(k, "h", 0.1f);
            for (int r = 0; r < 36; ++r)
                for (int c = 0; c < 64; ++c) {
                    float cx = (c + 0.5f) / 64, cy = (r + 0.5f) / 36;
                    if (cx >= x - 0.01f && cx <= x + w + 0.01f && cy >= y - 0.01f && cy <= y + h + 0.01f) m[r * 64 + c] = 1;
                }
        }
    std::lock_guard<std::mutex> lk(g_hist_mu);
    memcpy(g_maskmap, m, sizeof(m));
}

void set_subject(const std::vector<std::array<float, 4>> &boxes, float speed_px_s) {
    std::lock_guard<std::mutex> lk(g_hist_mu);
    g_boxes = boxes;
    g_speed = speed_px_s;
    g_boxes_ms = now_ms();
}

void feed_luma(const uint8_t *y, int w, int h, int stride) {
    std::array<float, 4> boxes[16];
    int nb = 0;
    uint8_t w15[225], mm[64 * 36];
    {
        std::lock_guard<std::mutex> lk(g_hist_mu);
        if (!g_hist_want) return;
        stat_init_locked();
        memcpy(w15, g_w15, sizeof(w15));
        memcpy(mm, g_maskmap, sizeof(mm));
        if (now_ms() - g_boxes_ms < 1500)
            for (auto &b : g_boxes)
                if (nb < 16) boxes[nb++] = b;
    }
    uint32_t hist[64] = {0}, hr[64] = {0}, n = 0, nr = 0;
    // every 4th pixel of every 4th row: ~32k samples at 960x540, well under 1 ms
    for (int r = 2; r < h; r += 4) {
        const uint8_t *p = y + (size_t)r * stride;
        int my = r * 36 / h, gy = r * 15 / h;
        float fy = (float)r / h;
        for (int c = 2; c < w; c += 4) {
            int Y = p[c];
            if (nb) {
                float fx = (float)c / w;
                for (int k = 0; k < nb; ++k)
                    if (fx >= boxes[k][0] && fx <= boxes[k][2] && fy >= boxes[k][1] && fy <= boxes[k][3]) {
                        ++hr[Y >> 2];
                        ++nr;
                        break;
                    }
            }
            if (mm[my * 64 + c * 64 / w]) continue;  // privacy mask: not part of the scene
            uint32_t wt = w15[gy * 15 + c * 15 / w];
            hist[Y >> 2] += wt;
            n += wt;
        }
    }
    std::lock_guard<std::mutex> lk(g_hist_mu);
    memcpy(g_hist, hist, sizeof(hist));
    memcpy(g_hist_roi, hr, sizeof(hr));
    g_hist_n = n;
    g_hist_roi_n = nr;
    g_hist_want = false;
    g_hist_ms = now_ms();
}

// ---------------------------------------------------------------- controller state

struct Ctl {
    std::string mode = "smart";  // smart | auto | manual
    float target = 50, compensation = 0;
    std::string anti_flicker = "auto";  // auto | off | 50hz | 60hz
    std::string wdr_mode = "auto";      // auto | on | off
    int wdr_level = 50;
    float max_shutter_ms = 40, max_gain = 64;
    std::string strategy = "lowlight";  // lowlight | highlight priority

    // calibration knobs (config: image.exposure.tuning)
    float t_gain = 0.4f;      // fraction of the exposure error (stops) corrected per step
    float t_dead = 0.2f;      // dead band in stops
    float t_back_hi = 0.03f;  // share of clipped highlights / shadows that makes a backlit scene
    float t_back_lo = 0.18f;

    // AI subject metering and motion shutter
    bool ai_roi = false, motion_shutter = false;
    float blur_px = 3;
    float eff_shutter_ms = 40;  // shutter limit currently applied
    int64_t slow_since = 0;

    float ev = 0;     // exposure bias the controller currently applies (%)
    float drc = 1.f;  // DRC gain currently applied
    bool flicker_on = false;
    int flicker_hz = 50;
    int64_t bright_since = 0, dim_since = 0;
    std::string scene = "normal";
    bool paused = false;  // exposure test

    // scene classification with hysteresis (state + dwell) and smooth weights
    bool back_state = false, blown_state = false;
    int back_cnt = 0, blown_cnt = 0;
    float bw = 0, kw = 0;

    // day / night
    std::string dn_mode = "color";  // color | bw | auto
    bool night = false;
    int64_t dn_since = 0, dn_switch = 0;
    float night_lv = 0, day_lv = 0;
    int ircut = -1, irled = -1;
    bool ircut_inv = false;

    // last measurements
    float median = 0, p02 = 0, p98 = 0, clip_hi = 0, clip_lo = 0, mean = 0;
    float exp_ms = 0, gain = 0, env_lv = 0, aiq_luma = 0;
    bool converged = false, subject = false;
    uint32_t hist32[32] = {0};
};
static Ctl g_ctl;
static std::thread g_thread;
static bool g_run = false;

// base dynamic setpoint curve (dark scenes to the right get a slightly lower target to keep noise down)
static float g_dy_exp[6] = {0, 0, 0, 0, 0, 0};
static float g_dy_sp[6] = {0, 0, 0, 0, 0, 0};
static int g_dy_len = 0;
static float g_dy_base[6] = {60, 60, 54, 48, 42, 38};

static void set_drc(float gain) {
    gain = std::max(1.f, std::min(8.f, gain));
    rk_aiq_uapi2_setDrcGain(g_ctx, gain, 0.1f, 16.f);
    g_ctl.drc = gain;
}

static void set_flicker(bool on, int hz) {
    Uapi_ExpSwAttrV2_t a;
    if (rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &a) != XCAM_RETURN_NO_ERROR) return;
    a.stAuto.stAntiFlicker.enable = on;
    a.stAuto.stAntiFlicker.Frequency = hz == 60 ? AECV2_FLICKER_FREQUENCY_60HZ : AECV2_FLICKER_FREQUENCY_50HZ;
    // AUTO mode lets AE leave the flicker grid when the scene is too bright for a whole mains period
    a.stAuto.stAntiFlicker.Mode = AECV2_ANTIFLICKER_AUTO_MODE;
    rk_aiq_user_api2_ae_setExpSwAttr(g_ctx, a);
    g_ctl.flicker_on = on;
    g_ctl.flicker_hz = hz;
    LOGI(TAG, "anti-flicker %s (%d Hz)", on ? "on" : "off", hz);
}

static void set_ev(float ev) {
    Uapi_LinExpAttrV2_t l;
    if (rk_aiq_user_api2_ae_getLinExpAttr(g_ctx, &l) != XCAM_RETURN_NO_ERROR) return;
    l.Params.Evbias = ev;
    rk_aiq_user_api2_ae_setLinExpAttr(g_ctx, l);
    g_ctl.ev = ev;
}

// longest shutter the AE may use (user limit, or tighter while something fast is in view)
static void set_shutter_cap(float ms) {
    Uapi_ExpSwAttrV2_t a;
    if (rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &a) != XCAM_RETURN_NO_ERROR) return;
    a.stAdvanced.SetAeRangeEn = true;
    a.stAdvanced.SetLinAeRange = a.stAuto.LinAeRange;
    Aec_LinAeRange_t &r = a.stAdvanced.SetLinAeRange;
    if (r.stExpTimeRange.Min <= 0 || r.stExpTimeRange.Min > 0.001f) r.stExpTimeRange.Min = 0.00002f;
    r.stExpTimeRange.Max = ms / 1000.f;
    r.stGainRange.Min = 1.f;
    r.stGainRange.Max = g_ctl.max_gain;
    if (r.stIspDGainRange.Min < 1.f) r.stIspDGainRange.Min = 1.f;
    if (r.stIspDGainRange.Max < 1.f) r.stIspDGainRange.Max = 1.f;
    rk_aiq_user_api2_ae_setExpSwAttr(g_ctx, a);
    g_ctl.eff_shutter_ms = ms;
}

static void apply_exposure(const json &ex) {
    Ctl &c = g_ctl;
    c.mode = jget<std::string>(ex, "mode", "smart");
    c.target = jget<float>(ex, "target", 50);
    c.compensation = jget<float>(ex, "compensation", 0);
    c.anti_flicker = jget<std::string>(ex, "anti_flicker", "auto");
    c.strategy = jget<std::string>(ex, "strategy", "lowlight");
    c.ai_roi = jget<bool>(ex, "ai_roi", false);
    c.motion_shutter = jget<bool>(ex, "motion_shutter", false);
    c.blur_px = clampf(jget<float>(ex, "blur_px", 3), 0.5f, 20.f);
    json tn = ex.value("tuning", json::object());
    c.t_gain = clampf(jget<float>(tn, "gain", 0.4f), 0.1f, 0.9f);
    c.t_dead = clampf(jget<float>(tn, "deadband", 0.2f), 0.05f, 0.8f);
    c.t_back_hi = clampf(jget<float>(tn, "backlight_hi", 0.03f), 0.005f, 0.3f);
    c.t_back_lo = clampf(jget<float>(tn, "backlight_lo", 0.18f), 0.05f, 0.8f);
    float frame_ms = 1000.f / std::max(1, g_fps);
    c.max_shutter_ms = std::max(0.1f, std::min(frame_ms, jget<float>(ex, "max_shutter_ms", frame_ms)));
    c.max_gain = std::max(1.f, std::min(128.f, jget<float>(ex, "max_gain", 64)));
    c.eff_shutter_ms = c.max_shutter_ms;

    Uapi_ExpSwAttrV2_t a;
    if (rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &a) != XCAM_RETURN_NO_ERROR) {
        LOGE(TAG, "getExpSwAttr failed");
        return;
    }
    a.sync.sync_mode = RK_AIQ_UAPI_MODE_DEFAULT;
    if (c.mode == "manual") {
        a.AecOpType = RK_AIQ_OP_MODE_MANUAL;
        a.stManual.LinearAE.ManualTimeEn = true;
        a.stManual.LinearAE.ManualGainEn = true;
        a.stManual.LinearAE.ManualIspDgainEn = false;
        a.stManual.LinearAE.TimeValue = std::min(c.max_shutter_ms, jget<float>(ex, "shutter_ms", 10)) / 1000.f;
        a.stManual.LinearAE.GainValue = std::max(1.f, jget<float>(ex, "gain", 1));
    } else {
        a.AecOpType = RK_AIQ_OP_MODE_AUTO;
    }
    a.stAuto.stFrmRate.isFpsFix = true;
    a.stAuto.stFrmRate.FpsValue = (float)g_fps;

    // anti-flicker: fixed modes are applied as-is; "auto" starts off and the controller engages it in dim light
    int hz = c.anti_flicker == "60hz" ? 60 : 50;
    bool on = c.anti_flicker == "50hz" || c.anti_flicker == "60hz";
    if (c.anti_flicker == "auto") on = c.flicker_on;
    a.stAuto.stAntiFlicker.enable = on;
    a.stAuto.stAntiFlicker.Frequency = hz == 60 ? AECV2_FLICKER_FREQUENCY_60HZ : AECV2_FLICKER_FREQUENCY_50HZ;
    a.stAuto.stAntiFlicker.Mode = AECV2_ANTIFLICKER_AUTO_MODE;
    c.flicker_on = on;
    c.flicker_hz = hz;

    // exposure limits: shutter up to max_shutter, analog gain up to max_gain
    a.stAdvanced.SetAeRangeEn = true;
    a.stAdvanced.SetLinAeRange = a.stAuto.LinAeRange;
    Aec_LinAeRange_t &r = a.stAdvanced.SetLinAeRange;
    if (r.stExpTimeRange.Min <= 0 || r.stExpTimeRange.Min > 0.001f) r.stExpTimeRange.Min = 0.00002f;
    r.stExpTimeRange.Max = c.max_shutter_ms / 1000.f;
    r.stGainRange.Min = 1.f;
    r.stGainRange.Max = c.max_gain;
    if (r.stIspDGainRange.Min < 1.f) r.stIspDGainRange.Min = 1.f;
    if (r.stIspDGainRange.Max < 1.f) r.stIspDGainRange.Max = 1.f;

    // the AE and the statistics use the same metering grid
    uint8_t w[225];
    fill_weights(jget<std::string>(ex, "metering", "center"), w);
    memcpy(a.GridWeights.uCoeff, w, sizeof(w));
    {
        std::lock_guard<std::mutex> lk(g_hist_mu);
        memcpy(g_w15, w, sizeof(w));
        g_stat_init = true;
    }

    // AE speed: damping of over/under exposure correction
    std::string speed = jget<std::string>(ex, "speed", "normal");
    if (speed == "fast") a.stAuto.stAeSpeed.DampOver = 0.15f, a.stAuto.stAeSpeed.DampUnder = 0.25f;
    else if (speed == "slow") a.stAuto.stAeSpeed.DampOver = 0.6f, a.stAuto.stAeSpeed.DampUnder = 0.7f;
    else a.stAuto.stAeSpeed.DampOver = 0.3f, a.stAuto.stAeSpeed.DampUnder = 0.45f;

    if (rk_aiq_user_api2_ae_setExpSwAttr(g_ctx, a) != XCAM_RETURN_NO_ERROR) LOGE(TAG, "setExpSwAttr failed");

    // brightness target: scale the dynamic setpoint curve; compensation: exposure bias
    Uapi_LinExpAttrV2_t l;
    if (rk_aiq_user_api2_ae_getLinExpAttr(g_ctx, &l) == XCAM_RETURN_NO_ERROR) {
        int n = std::min(6, std::min(l.Params.DySetpoint.ExpLevel_len, l.Params.DySetpoint.DySetpoint_len));
        if (n > 0 && l.Params.DySetpoint.ExpLevel && l.Params.DySetpoint.DySetpoint) {
            float scale = 0.5f + c.target / 100.f;  // 0..100 -> 0.5x..1.5x
            for (int i = 0; i < n; ++i) {
                g_dy_exp[i] = l.Params.DySetpoint.ExpLevel[i];
                g_dy_sp[i] = std::max(10.f, std::min(160.f, g_dy_base[i] * scale));
            }
            g_dy_len = n;
            l.Params.DySetpoint.ExpLevel = g_dy_exp;
            l.Params.DySetpoint.DySetpoint = g_dy_sp;
            l.Params.DySetpoint.ExpLevel_len = l.Params.DySetpoint.DySetpoint_len = n;
        }
        // highlight priority: protect bright areas (headlights, lamps) over dark ones
        l.Params.StrategyMode = c.strategy == "highlight" ? AECV2_STRATEGY_MODE_HIGHLIGHT : AECV2_STRATEGY_MODE_LOWLIGHT;
        // smart mode does its own highlight handling; the AIQ's over-exposure control fights it
        if (c.mode == "smart") l.Params.OverExpCtrl.Enable = false;
        float ev = c.mode == "smart" ? c.ev : c.compensation;
        l.Params.Evbias = ev;
        c.ev = ev;
        if (rk_aiq_user_api2_ae_setLinExpAttr(g_ctx, l) != XCAM_RETURN_NO_ERROR) LOGE(TAG, "setLinExpAttr failed");
    }
}

static void apply_backlight(const std::string &mode) {
    // blc: expose for the dark subject; hlc: hold down highlights (headlights, lamps)
    rk_aiq_uapi2_setBLCMode(g_ctx, mode == "blc", AE_MEAS_AREA_AUTO);
    rk_aiq_uapi2_setHLCMode(g_ctx, mode == "hlc");
}

// ---------------------------------------------------------------- day / night (gray mode, IR-cut, IR LED)

static bool sysfs_write(const std::string &path, const std::string &v) {
    FILE *f = fopen(path.c_str(), "w");  // sysfs: a plain write, not write_file() (temp file + rename)
    if (!f) return false;
    fputs(v.c_str(), f);
    fclose(f);
    return true;
}

static void gpio_set(int n, bool v) {
    if (n < 0) return;
    std::string base = "/sys/class/gpio/gpio" + std::to_string(n);
    if (!file_exists(base + "/value")) {
        sysfs_write("/sys/class/gpio/export", std::to_string(n));
        usleep(100 * 1000);
        sysfs_write(base + "/direction", "out");
    }
    sysfs_write(base + "/value", v ? "1" : "0");
}

static void set_night(bool night, bool force = false) {
    Ctl &c = g_ctl;
    if (c.night == night && !force) return;
    rk_aiq_uapi2_setGrayMode(g_ctx, night ? RK_AIQ_GRAY_MODE_ON : RK_AIQ_GRAY_MODE_OFF);
    // IR-cut filter: in the light path by day (pin high unless inverted), out at night
    gpio_set(c.ircut, night ? c.ircut_inv : !c.ircut_inv);
    gpio_set(c.irled, night);
    c.night = night;
    c.dn_switch = now_ms();
    c.dn_since = 0;
    LOGI(TAG, "%s mode", night ? "night (black & white)" : "day (colour)");
}

static void apply_daynight(const json &d) {
    Ctl &c = g_ctl;
    c.dn_mode = jget<std::string>(d, "mode", "color");
    c.night_lv = jget<float>(d, "night_lv", 0);
    c.day_lv = jget<float>(d, "day_lv", 0);
    c.ircut = jget<int>(d, "ircut_gpio", -1);
    c.irled = jget<int>(d, "irled_gpio", -1);
    c.ircut_inv = jget<bool>(d, "ircut_invert", false);
    if (c.dn_mode == "bw") set_night(true, true);
    else if (c.dn_mode == "color") set_night(false, true);
    else set_night(c.night, true);  // auto: keep the current state, re-drive the pins
}

void apply(const json &img) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!g_ctx) {
        g_applied = img;
        return;
    }
    auto changed = [&](const char *k) { return g_applied.is_null() || g_applied.value(k, json()) != img.value(k, json()); };
    auto lvl = [&](const char *k) { return (unsigned)std::max(0, std::min(100, jget<int>(img, k, 50))); };

    // UI scale 0..100 (50 = neutral); the AIQ wants 0..255 (128 = neutral)
    if (changed("brightness")) rk_aiq_uapi2_setBrightness(g_ctx, lvl("brightness") * 255 / 100);
    if (changed("contrast")) rk_aiq_uapi2_setContrast(g_ctx, lvl("contrast") * 255 / 100);
    if (changed("saturation")) rk_aiq_uapi2_setSaturation(g_ctx, lvl("saturation") * 255 / 100);
    if (changed("hue")) rk_aiq_uapi2_setHue(g_ctx, lvl("hue") * 255 / 100);
    if (changed("sharpness")) rk_aiq_uapi2_setSharpness(g_ctx, lvl("sharpness"));
    if (changed("mirror") || changed("flip"))
        rk_aiq_uapi2_setMirrorFlip(g_ctx, jget<bool>(img, "mirror", false), jget<bool>(img, "flip", false), 4);

    if (changed("exposure")) apply_exposure(img.value("exposure", json::object()));
    if (changed("backlight")) apply_backlight(jget<std::string>(img, "backlight", "off"));

    if (changed("wdr")) {
        json w = img.value("wdr", json::object());
        g_ctl.wdr_mode = jget<std::string>(w, "mode", "auto");
        g_ctl.wdr_level = jget<int>(w, "level", 50);
        if (g_ctl.wdr_mode == "on") set_drc(1.f + g_ctl.wdr_level / 100.f * 7.f);
        else if (g_ctl.wdr_mode == "off") set_drc(1.f);
        // auto: the controller drives it
    }
    if (changed("dehaze")) {
        json d = img.value("dehaze", json::object());
        std::string m = jget<std::string>(d, "mode", "off");
        rk_aiq_uapi2_setDehazeEnable(g_ctx, m != "off");
        if (m == "on") rk_aiq_uapi2_setMDehazeStrth(g_ctx, (unsigned)jget<int>(d, "level", 50));
    }
    if (changed("nr")) {
        json n = img.value("nr", json::object());
        rk_aiq_uapi2_setMSpaNRStrth(g_ctx, true, (unsigned)jget<int>(n, "spatial", 50));
        rk_aiq_uapi2_setMTNRStrth(g_ctx, true, (unsigned)jget<int>(n, "temporal", 50));
    }
    if (changed("wb")) {
        json w = img.value("wb", json::object());
        std::string m = jget<std::string>(w, "mode", "auto");
        if (m == "auto" || m == "basic") {
            rk_aiq_uapi2_setWBMode(g_ctx, OP_AUTO);
        } else if (m == "manual") {
            rk_aiq_uapi2_setWBMode(g_ctx, OP_MANUAL);
            rk_aiq_wb_gain_t g;
            rk_aiq_uapi2_getWBGain(g_ctx, &g);
            // 50 = the gains auto WB had settled on
            g.rgain *= jget<int>(w, "r", 50) / 50.f;
            g.grgain *= jget<int>(w, "g", 50) / 50.f;
            g.gbgain *= jget<int>(w, "g", 50) / 50.f;
            g.bgain *= jget<int>(w, "b", 50) / 50.f;
            rk_aiq_uapi2_setMWBGain(g_ctx, &g);
        } else {
            rk_aiq_wb_scene_t s = m == "incandescent"  ? RK_AIQ_WBCT_INCANDESCENT
                                  : m == "fluorescent" ? RK_AIQ_WBCT_FLUORESCENT
                                  : m == "cloudy"      ? RK_AIQ_WBCT_CLOUDY_DAYLIGHT
                                  : m == "shade"       ? RK_AIQ_WBCT_SHADE
                                                       : RK_AIQ_WBCT_DAYLIGHT;
            rk_aiq_uapi2_setWBMode(g_ctx, OP_MANUAL);
            rk_aiq_uapi2_setMWBScene(g_ctx, s);
        }
    }
    if (changed("daynight")) apply_daynight(img.value("daynight", json::object()));
    g_applied = img;
}

// ---------------------------------------------------------------- scene presets and validation

static void apply_preset(json &img, const std::string &p) {
    json &ex = img["exposure"];
    auto E = [&](const char *k, json v) { ex[k] = v; };
    // common baseline
    E("mode", "smart"), E("compensation", 0), E("anti_flicker", "auto"), E("ai_roi", false), E("motion_shutter", false);
    E("strategy", "lowlight"), E("speed", "normal");
    img["backlight"] = "off";
    img["wdr"] = {{"mode", "auto"}, {"level", 50}};
    img["nr"] = {{"spatial", 50}, {"temporal", 50}};
    if (p == "anpr") {
        // number plates on a road: short shutter, headlight protection, meter the lower half, expose for the vehicle
        E("target", 55), E("max_shutter_ms", 6), E("max_gain", 32), E("metering", "lower"), E("speed", "fast");
        E("strategy", "highlight"), E("ai_roi", true), E("motion_shutter", true), E("blur_px", 3);
        img["backlight"] = "hlc";
        img["wdr"] = {{"mode", "auto"}, {"level", 40}};
        img["nr"] = {{"spatial", 55}, {"temporal", 25}};
    } else if (p == "indoor") {
        E("target", 50), E("max_shutter_ms", 40), E("max_gain", 64), E("metering", "center");
        img["wdr"] = {{"mode", "auto"}, {"level", 30}};
    } else if (p == "lowlight") {
        E("target", 60), E("max_shutter_ms", 40), E("max_gain", 128), E("metering", "center"), E("speed", "slow");
        img["wdr"] = {{"mode", "auto"}, {"level", 30}};
        img["nr"] = {{"spatial", 65}, {"temporal", 60}};
        json dn = img.value("daynight", json::object());
        dn["mode"] = "auto";
        img["daynight"] = dn;
    } else {  // general
        E("target", 50), E("max_shutter_ms", 40), E("max_gain", 64), E("metering", "center");
    }
}

std::string validate(json &img) {
    if (!img.contains("exposure") || !img["exposure"].is_object()) img["exposure"] = json::object();
    json &ex = img["exposure"];
    std::string p = jget<std::string>(ex, "preset", "general"), done = jget<std::string>(ex, "preset_applied", "");
    static const char *known[] = {"general", "anpr", "indoor", "lowlight", "custom"};
    bool ok = false;
    for (auto *k : known) ok |= p == k;
    if (!ok) return "unknown scene preset";
    if (p == "custom") {
        ex["preset_applied"] = "custom";
    } else if (p != done) {
        apply_preset(img, p);
        ex["preset_applied"] = p;
    }
    // ranges
    float frame_ms = 1000.f / std::max(1, g_fps);
    ex["max_shutter_ms"] = clampf(jget<float>(ex, "max_shutter_ms", frame_ms), 0.1f, frame_ms);
    ex["max_gain"] = clampf(jget<float>(ex, "max_gain", 64), 1, 128);
    std::string st = jget<std::string>(ex, "strategy", "lowlight");
    if (st != "lowlight" && st != "highlight") return "strategy must be lowlight or highlight";
    json dn = img.value("daynight", json::object());
    std::string dm = jget<std::string>(dn, "mode", "color");
    if (dm != "color" && dm != "bw" && dm != "auto") return "day/night mode must be color, bw or auto";
    for (const char *k : {"ircut_gpio", "irled_gpio"}) {
        int v = jget<int>(dn, k, -1);
        if (v < -1 || v > 511) return std::string(k) + " out of range";
    }
    return "";
}

// ---------------------------------------------------------------- controller loop

static float percentile(const uint32_t *h, uint32_t n, float q) {
    uint32_t want = (uint32_t)(n * q), acc = 0;
    for (int i = 0; i < 64; ++i) {
        acc += h[i];
        if (acc >= want) return i * 4 + 2;
    }
    return 255;
}

struct Stats {
    float median = 0, p02 = 0, p98 = 0, clip_hi = 0, clip_lo = 0, mean = 0;
};

static Stats stats_of(const uint32_t *h, uint32_t n) {
    Stats s;
    uint32_t hi = 0, lo = 0;
    double sum = 0;
    for (int i = 0; i < 64; ++i) {
        if (i >= 61) hi += h[i];  // >= 244
        if (i <= 3) lo += h[i];   // <= 15
        sum += (double)h[i] * (i * 4 + 2);
    }
    s.clip_hi = (float)hi / n;
    s.clip_lo = (float)lo / n;
    s.mean = (float)(sum / n);
    s.median = percentile(h, n, 0.5f);
    s.p02 = percentile(h, n, 0.02f);
    s.p98 = percentile(h, n, 0.98f);
    return s;
}

// exposure difference in stops between two gamma-encoded (display) values: luminance goes with value^2.2
static float stops(float want, float have) { return 2.2f * std::log2(std::max(4.f, want) / std::max(4.f, have)); }

// hysteresis with dwell: becomes true after `enter` consecutive ticks over the entry threshold, false after `leave`
// consecutive ticks under the exit threshold
static void schmitt(bool &state, int &cnt, bool over_enter, bool under_exit, int enter, int leave) {
    if (!state) {
        cnt = over_enter ? cnt + 1 : 0;
        if (cnt >= enter) state = true, cnt = 0;
    } else {
        cnt = under_exit ? cnt + 1 : 0;
        if (cnt >= leave) state = false, cnt = 0;
    }
}

static float approach(float cur, float target, float step) { return cur + clampf(target - cur, -step, step); }

static void daynight_step() {
    Ctl &c = g_ctl;
    if (c.dn_mode != "auto") return;
    int64_t t = now_ms();
    if (c.dn_switch && t - c.dn_switch < 60000) return;  // never flap
    bool want;
    if (c.night_lv > 0) {
        float day = c.day_lv > c.night_lv ? c.day_lv : c.night_lv * 1.5f;
        want = c.night ? !(c.env_lv > day) : c.env_lv < c.night_lv;
    } else {
        // no light-level thresholds: the AE running out of range means night; plenty of headroom means day
        bool sat = c.gain >= 0.9f * c.max_gain && c.exp_ms >= 0.85f * c.max_shutter_ms;
        bool plenty = c.gain <= 0.2f * c.max_gain && c.exp_ms <= 0.3f * c.max_shutter_ms;
        want = c.night ? !plenty : sat;
    }
    if (want == c.night) {
        c.dn_since = 0;
        return;
    }
    if (!c.dn_since) c.dn_since = t;
    if (t - c.dn_since > (c.night ? 30000 : 10000)) set_night(!c.night);
}

static void shutter_step() {
    Ctl &c = g_ctl;
    if (!c.motion_shutter || c.mode == "manual") {
        if (c.eff_shutter_ms != c.max_shutter_ms && c.mode != "manual") set_shutter_cap(c.max_shutter_ms);
        return;
    }
    float speed = 0;
    {
        std::lock_guard<std::mutex> lk(g_hist_mu);
        if (now_ms() - g_boxes_ms < 1500) speed = g_speed;
    }
    float want = c.max_shutter_ms;
    if (speed > 15) want = clampf(c.blur_px / speed * 1000.f, std::min(2.f, c.max_shutter_ms), c.max_shutter_ms);
    int64_t t = now_ms();
    float cur = c.eff_shutter_ms;
    if (want < cur * 0.85f) {  // something fast: tighten at once
        c.slow_since = t;
        set_shutter_cap(want);
    } else if (want > cur * 1.2f) {  // relax slowly, after 3 s without anything fast
        if (t - c.slow_since > 3000) set_shutter_cap(std::min(c.max_shutter_ms, cur * 1.5f));
    } else if (speed > 15) {
        c.slow_since = t;
    }
}

static void control_step() {
    Ctl &c = g_ctl;
    if (c.paused) return;
    Uapi_ExpQueryInfo_t q;
    if (rk_aiq_user_api2_ae_queryExpResInfo(g_ctx, &q) != XCAM_RETURN_NO_ERROR) return;
    c.exp_ms = q.LinAeInfo.LinearExp.integration_time * 1000.f;
    c.gain = q.LinAeInfo.LinearExp.analog_gain;
    c.env_lv = q.GlobalEnvLv;
    c.aiq_luma = q.LinAeInfo.MeanLuma;
    c.converged = q.IsConverged;

    daynight_step();
    shutter_step();

    uint32_t h[64], hr[64], n, nr;
    int64_t hist_age;
    {
        std::lock_guard<std::mutex> lk(g_hist_mu);
        memcpy(h, g_hist, sizeof(h));
        memcpy(hr, g_hist_roi, sizeof(hr));
        n = g_hist_n;
        nr = g_hist_roi_n;
        hist_age = now_ms() - g_hist_ms;
        g_hist_want = true;
    }
    if (n < 1000 || hist_age > 2000) return;  // no fresh frame (stream stopped?)

    Stats g = stats_of(h, n);
    for (int i = 0; i < 32; ++i) c.hist32[i] = h[2 * i] + h[2 * i + 1];
    c.clip_hi = g.clip_hi;
    c.clip_lo = g.clip_lo;
    c.mean = g.mean;
    c.median = g.median;
    c.p02 = g.p02;
    c.p98 = g.p98;

    if (c.mode == "manual") {
        c.scene = "manual";
        return;
    }

    // ---- anti-flicker auto: only when the scene needs >= one mains half-period anyway
    if (c.anti_flicker == "auto") {
        int64_t t = now_ms();
        float period = 1000.f / (2 * c.flicker_hz);  // 10 ms at 50 Hz
        if (c.flicker_on) {
            // stuck on the 10 ms floor at minimum gain with blown highlights: outdoor light, flicker-free
            bool forced = c.exp_ms >= period * 0.95f && c.gain <= 1.1f && c.clip_hi > 0.06f && c.median > 140;
            c.bright_since = forced ? (c.bright_since ? c.bright_since : t) : 0;
            if (c.bright_since && t - c.bright_since > 1500) {
                set_flicker(false, c.flicker_hz);
                c.bright_since = 0;
            }
        } else {
            // dim enough that AE wants more than a mains period: likely artificial light, prevent banding
            bool dim = c.exp_ms > period * 1.3f;
            c.dim_since = dim ? (c.dim_since ? c.dim_since : t) : 0;
            if (c.dim_since && t - c.dim_since > 4000) {
                set_flicker(true, c.flicker_hz);
                c.dim_since = 0;
            }
        }
    }

    if (c.mode != "smart") {
        c.scene = "auto";
        return;
    }

    // ---- Smart exposure bias
    float target_mid = 80.f + c.target * 0.9f + c.compensation * 0.6f;  // target 50 -> median ~125
    target_mid = clampf(target_mid, 40.f, 200.f);

    // AI subject metering: expose for the tracked object when there is one (the boxes stay fresh for 1.5 s)
    c.subject = c.ai_roi && nr >= 150;
    float e;
    bool night_like = c.gain >= 0.5f * c.max_gain || (c.exp_ms >= 0.85f * c.max_shutter_ms && c.gain >= 0.25f * c.max_gain);
    if (c.subject) {
        Stats s = stats_of(hr, nr);
        float e_mid = stops(target_mid, s.median);
        float head = stops(250.f, std::max(16.f, s.p98));
        e = std::min(e_mid, std::max(0.f, head) + 0.1f);
        if (s.clip_hi > 0.04f) e = std::min(e, -std::min(1.5f, s.clip_hi * 10.f));
        c.scene = "subject";
    } else {
        float e_mid = stops(target_mid, g.median);
        float head = stops(250.f, std::max(16.f, g.p98));
        // classes with hysteresis and dwell (1 s to enter, 1.5 s to leave), then blended: no hunting at a threshold
        schmitt(c.back_state, c.back_cnt, g.clip_hi > c.t_back_hi && g.clip_lo > c.t_back_lo,
                g.clip_hi < c.t_back_hi * 0.5f || g.clip_lo < c.t_back_lo * 0.55f, 4, 6);
        schmitt(c.blown_state, c.blown_cnt, g.clip_hi > 0.025f && !c.back_state, g.clip_hi < 0.012f || c.back_state, 4, 6);
        c.bw = approach(c.bw, c.back_state ? 1.f : 0.f, 0.25f);
        c.kw = approach(c.kw, c.blown_state ? 1.f : 0.f, 0.25f);
        if (c.bw + c.kw > 1.f) {
            float k = 1.f / (c.bw + c.kw);
            c.bw *= k, c.kw *= k;
        }
        float e_norm = std::min(e_mid, std::max(0.f, head) + 0.1f);
        // daytime backlight: keep the subject visible, accept the sky clipping, recover with DRC.
        // night with lamps: do not brighten the whole scene because of a few lights
        float e_back = night_like ? std::min(e_mid, 0.15f) * 0.5f : std::min(e_mid, 0.25f) * 0.6f;
        float e_bright = std::min(e_mid, -std::min(1.5f, g.clip_hi * 12.f));
        e = e_norm * (1.f - c.bw - c.kw) + e_back * c.bw + e_bright * c.kw;
        if (c.bw > 0.5f) c.scene = night_like ? "night_lamps" : "backlight";
        else if (c.kw > 0.5f) c.scene = "bright";
        else c.scene = g.median < 50 ? "dark" : "normal";
    }
    if (c.gain >= c.max_gain * 0.9f && c.exp_ms >= c.max_shutter_ms * 0.9f) c.scene = "night";

    // integral control on the bias with a dead band; small steps so the AIQ's own loop stays stable
    if (std::fabs(e) > c.t_dead && (c.converged || std::fabs(e) > 0.6f)) {
        float lin = (100.f + c.ev) / 100.f;
        lin *= std::pow(2.f, clampf(e * c.t_gain, -0.8f, 0.8f));
        float ev = clampf(lin * 100.f - 100.f, -75.f, 120.f);
        if (std::fabs(ev - c.ev) >= 1.f) set_ev(ev);
    }

    // ---- auto WDR (DRC gain): open up the shadows when the scene's range is wide
    if (c.wdr_mode == "auto") {
        float want = 1.f;
        float maxg = 1.f + c.wdr_level / 100.f * 5.f;
        if (c.back_state || (c.clip_lo > 0.12f && c.p98 > 200)) want = maxg;
        else if (c.clip_lo > 0.06f) want = 1.f + (maxg - 1.f) * 0.5f;
        if (night_like) want = 1.f + (want - 1.f) * 0.4f;  // DRC amplifies night noise
        float next = c.drc + std::max(-0.25f, std::min(0.25f, want - c.drc));
        if (std::fabs(next - c.drc) >= 0.05f) set_drc(next);
    }
}

static void loop() {
    while (g_run) {
        usleep(250 * 1000);
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (g_ctx) control_step();
    }
}

// ---------------------------------------------------------------- exposure test

json exposure_test() {
    json out = json::array();
    float base;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (!g_ctx) return out;
        g_ctl.paused = true;
        base = g_ctl.ev;
    }
    for (float off : {-1.f, 0.f, 1.f}) {
        {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            set_ev(clampf(((100.f + base) / 100.f * std::pow(2.f, off)) * 100.f - 100.f, -90.f, 300.f));
        }
        usleep(1600 * 1000);  // let the AE settle
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            g_hist_want = true;
            g_hist_ms = 0;
        }
        for (int i = 0; i < 20; ++i) {
            usleep(100 * 1000);
            std::lock_guard<std::mutex> lk(g_hist_mu);
            if (g_hist_ms) break;
        }
        uint32_t h[64], n;
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            memcpy(h, g_hist, sizeof(h));
            n = g_hist_n;
        }
        json item = {{"offset", off}};
        if (n) {
            Stats s = stats_of(h, n);
            json hist = json::array();
            for (int i = 0; i < 32; ++i) hist.push_back(h[2 * i] + h[2 * i + 1]);
            item["median"] = s.median;
            item["clip_hi_pct"] = std::round(s.clip_hi * 1000.f) / 10.f;
            item["clip_lo_pct"] = std::round(s.clip_lo * 1000.f) / 10.f;
            item["histogram"] = hist;
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            item["exposure_ms"] = std::round(g_ctl.exp_ms * 100.f) / 100.f;
            item["gain"] = std::round(g_ctl.gain * 100.f) / 100.f;
        }
        std::string jpg;
        if (video::snapshot(jpg)) item["jpeg"] = base64_encode(jpg);
        out.push_back(item);
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        set_ev(base);
        g_ctl.paused = false;
    }
    return out;
}

// ---------------------------------------------------------------- lifecycle

bool start(const std::string &iq_dir, int fps) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_ctx) return true;
    g_fps = fps;
    setenv("HDR_MODE", "0", 1);
    rk_aiq_static_info_t info;
    rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(0, &info);
    {
        // the camera module decides the largest picture (SC3336 2304x1296, SC4336 2560x1440, SC530AI 2880x1620, ...)
        int w = 0, h = 0, fps = 0;
        for (int i = 0; i < info.sensor_info.num && i < SUPPORT_FMT_MAX; ++i) {
            const rk_frame_fmt_t &f = info.sensor_info.support_fmt[i];
            if ((int64_t)f.width * f.height > (int64_t)w * h) w = f.width, h = f.height;
            fps = std::max(fps, (int)f.fps);
        }
        board::set_sensor(info.sensor_info.sensor_name, w, h, fps);
    }
    LOGI(TAG, "sensor %s %dx%d, iq %s", info.sensor_info.sensor_name, board::info().sensor_w, board::info().sensor_h, iq_dir.c_str());
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(info.sensor_info.sensor_name, "rkraw_rx", 2);
    if (rk_aiq_uapi2_sysctl_preInit_scene(info.sensor_info.sensor_name, "normal", "day") < 0)
        LOGW(TAG, "preInit_scene failed");
    g_ctx = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iq_dir.c_str(), nullptr, nullptr);
    if (!g_ctx) {
        LOGE(TAG, "AIQ init failed (is another camera app running?)");
        return false;
    }
    if (rk_aiq_uapi2_sysctl_prepare(g_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR ||
        rk_aiq_uapi2_sysctl_start(g_ctx) != XCAM_RETURN_NO_ERROR) {
        LOGE(TAG, "AIQ prepare/start failed");
        rk_aiq_uapi2_sysctl_deinit(g_ctx);
        g_ctx = nullptr;
        return false;
    }
    json img = g_applied;
    g_applied = json();
    if (!img.is_null()) apply(img);
    g_run = true;
    g_thread = std::thread(loop);
    return true;
}

void stop() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!g_ctx) return;
    rk_aiq_uapi2_sysctl_stop(g_ctx, false);
    rk_aiq_uapi2_sysctl_deinit(g_ctx);
    g_ctx = nullptr;
}

bool running() { return g_ctx != nullptr; }

json status() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    const Ctl &c = g_ctl;
    json hist = json::array();
    for (int i = 0; i < 32; ++i) hist.push_back(c.hist32[i]);
    return {{"running", g_ctx != nullptr},
            {"mode", c.mode},
            {"scene", c.scene},
            {"exposure_ms", std::round(c.exp_ms * 100.0) / 100.0},
            {"gain", std::round(c.gain * 100.0) / 100.0},
            {"env_lv", std::round(c.env_lv * 100.0) / 100.0},
            {"aiq_luma", std::round(c.aiq_luma)},
            {"converged", c.converged},
            {"ev_bias", std::round(c.ev)},
            {"drc_gain", std::round(c.drc * 100.0) / 100.0},
            {"anti_flicker", c.flicker_on},
            {"night", c.night},
            {"daynight", c.dn_mode},
            {"subject", c.subject},
            {"shutter_cap_ms", std::round(c.eff_shutter_ms * 10.0) / 10.0},
            {"luma", {{"mean", std::round(c.mean)}, {"median", c.median}, {"p02", c.p02}, {"p98", c.p98},
                      {"clip_hi_pct", std::round(c.clip_hi * 1000.0) / 10.0}, {"clip_lo_pct", std::round(c.clip_lo * 1000.0) / 10.0}}},
            {"histogram", hist}};
}

}  // namespace isp
