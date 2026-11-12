/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * camctl - see camctl.h. The design notes live in
 * docs/SPEC-camera-controls.md; section numbers below refer to it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"
#include "esp_video_ioctl.h"
#include "esp_video_device.h"
#include "esp_video_isp_pipeline.h"
#include "esp_cam_sensor_types.h"
#include "cJSON.h"

#include "camctl.h"

static const char *TAG = "camctl";

/* ---- Tunables ----------------------------------------------------------- */

/* Shutter priority (§3.2.1). 'A': clamp the AGC's exposure range to one value
 * and let it move gain only. 'B': AGC off, our own gain loop. POST /ctrl?sp=
 * switches at run time so both can be measured off one flash.
 *
 * B is the default because A failed on hardware (2026-10-15, esp_ipa 2.4):
 * the exposure stays pinned, but the AGC sets one gain and stops regulating -
 * 20 ms came out at 13.5x and Y=181 against Y=128 in auto. B held Y at 96-105
 * for 8.3-20 ms, settling in about a second. */
#define SP_METHOD_DEFAULT   'B'

/* AF scan band, measured on this module (imx708_default.json "af" block). */
#define AF_SCAN_MIN         448
#define AF_SCAN_MAX         960
#define AF_COARSE_STEP      64
#define AF_FINE_SPAN        48
#define AF_FINE_STEP        16
/* Settle after a lens move: the sensor had frames in flight when the DAC was
 * written - four of them (FOCUS_SETTLE_FRAMES in imx708_snapshot), ~143 ms at
 * 28 fps. Counted in time, then one fresh frame, rather than in published
 * frames: the still app publishes only every other sensor frame (a 4 MB copy
 * each), so "5 published frames" was 360 ms a point and 7 s an AF. */
#define AF_SETTLE_MS        150
/* Boot AF waits this long after WiFi for AE to settle - a dark frame has no edges. */
#define AF_BOOT_DELAY_MS    2000

/* Gain loop for method B: target luma on 0..255, dead band, and period. The
 * target is taken from the picture at the moment AE hands over (agc_set), so
 * shutter priority keeps the brightness auto had; this is only the fallback
 * for booting straight into shutter priority. */
#define GL_TARGET_DEFAULT   110
#define GL_TARGET_MIN       30
#define GL_TARGET_MAX       200
#define GL_DEADBAND_PCT     10
#define GL_PERIOD_MS        200
#define GL_SETTLE_FRAMES    4
/* The gain menu steps at about 1/12 stop, so one stop of error is ~12 entries. */
#define GL_STEPS_PER_STOP   12
#define GL_MAX_STEP         12

/*
 * White balance (§3.7). Gains are milli-units on the ISP's red/blue balance
 * controls, which esp_video multiplies into the CCM's red and blue columns
 * (this chip/IDF has no separate WBG block). Measured on the published frame,
 * which is after gamma: an output ratio k needs roughly k^2.2 on the gain, so
 * each step applies k^WB_GAIN_EXP - under-correcting on purpose, so it cannot
 * overshoot, and still closing ~70% of the error per step.
 */
#define WB_MIN              300
#define WB_MAX              3999
#define WB_DEFAULT          1500
#define WB_PERIOD_MS        300
#define WB_DEADBAND_PCT     2
#define WB_GAIN_EXP         1.5f
#define WB_SETTLE_FRAMES    2
#define WB_ONEPUSH_ITERS    15

/* Coalesce slider drags into one flash write (§3.6). */
#define SAVE_DELAY_US       (2 * 1000 * 1000)

#define CFG_BASE            "/cfg"
#define CFG_PARTITION       "storage"
#define CFG_FILE            CFG_BASE "/config.json"
#define CFG_TMP             CFG_BASE "/config.json.tmp"
#define CFG_VERSION         1

#define GAIN_MENU_MAX       64

/* ---- State -------------------------------------------------------------- */

typedef enum { M_AUTO = 0, M_MANUAL = 1 } cmode_t;

static const char *mode_str(cmode_t m) { return m == M_MANUAL ? "manual" : "auto"; }

/* What the user asked for - this is what config.json holds. */
static struct {
    cmode_t exp_mode, gain_mode, focus_mode;
    int32_t exp_lines;      /* manual exposure, sensor lines */
    int32_t gain_index;     /* manual gain, menu index */
    int32_t focus_pos;      /* manual lens position, DAC code */
    cmode_t wb_mode;
    int32_t wb_red, wb_blue;    /* milli; in auto, where the loop has got to */
    bool hflip, vflip;          /* applied at boot only - see camctl_saved_orientation() */
} s_cfg = { M_AUTO, M_AUTO, M_AUTO, 0, 0, 512, M_AUTO, WB_DEFAULT, WB_DEFAULT, false, false };

/* What the hardware allows, read once at init. */
static struct {
    int32_t exp_min, exp_max, exp_step;     /* lines */
    uint32_t tline_ns;
    int32_t gain_min, gain_max;
    uint32_t gain_milli[GAIN_MENU_MAX];     /* 1000 = 1.00x */
    int32_t focus_min, focus_max;
} s_caps;

static int s_fd = -1;
static const camctl_frame_ops_t *s_ops;
static SemaphoreHandle_t s_lock;            /* every control ioctl and s_cfg */
static uint32_t s_boot_min_us, s_boot_max_us;
static bool s_boot_range_ok;
static char s_sp_method = SP_METHOD_DEFAULT;
static const char *s_config_state = "default";  /* loaded | default | error */
static bool s_fs_ok;

/* AF */
typedef enum { AF_IDLE, AF_RUNNING, AF_DONE, AF_FAILED } af_state_t;
static const char *const s_af_names[] = { "idle", "running", "done", "failed" };
static volatile af_state_t s_af_state = AF_IDLE;
static volatile bool s_af_abort;
static volatile bool s_af_boot_delay;
static uint32_t s_af_score, s_af_ms;
static TaskHandle_t s_af_task;

/* White balance: the ISP device, opened alongside the pipeline's own handle. */
static int s_isp_fd = -1;
typedef enum { WB_IDLE, WB_RUNNING, WB_DONE, WB_FAILED } wb_state_t;
static const char *const s_wb_names[] = { "idle", "running", "done", "failed" };
static volatile wb_state_t s_wb_state = WB_IDLE;
static TaskHandle_t s_wb_task;

/* Persistence */
static esp_timer_handle_t s_save_timer;
static TaskHandle_t s_save_task;
static void save_later(void);

/* ---- V4L2 helpers (from imx708_snapshot) -------------------------------- */
/*
 * Exposure and gain are USER-class controls, unlike focus, which is
 * CAMERA-class. Getting the class wrong is not an error - it is a lookup that
 * quietly finds nothing - so every write below is read back.
 */
static bool ctrl_get_user(uint32_t id, int32_t *out)
{
    struct v4l2_ext_control c = { .id = id };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &c };

    if (ioctl(s_fd, VIDIOC_G_EXT_CTRLS, &cs) != 0) {
        return false;
    }
    *out = c.value;
    return true;
}

static bool ctrl_set_user(uint32_t id, int32_t value)
{
    struct v4l2_ext_control c = { .id = id, .value = value };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &c };

    return ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &cs) == 0;
}

/* Raw 10-bit DAC code; bigger pushes the lens out, i.e. focuses closer. */
static int focus_get(void)
{
    struct v4l2_ext_control c = { .id = V4L2_CID_FOCUS_ABSOLUTE };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CID_CAMERA_CLASS, .count = 1, .controls = &c };

    return ioctl(s_fd, VIDIOC_G_EXT_CTRLS, &cs) == 0 ? c.value : -1;
}

static bool focus_set(int pos)
{
    struct v4l2_ext_control c = { .id = V4L2_CID_FOCUS_ABSOLUTE, .value = pos };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CID_CAMERA_CLASS, .count = 1, .controls = &c };

    return ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &cs) == 0;
}

static uint32_t lines_to_us(int32_t lines)
{
    return (uint32_t)(((uint64_t)lines * s_caps.tline_ns + 500) / 1000);
}

static int32_t us_to_lines(uint32_t us)
{
    return (int32_t)(((uint64_t)us * 1000 + s_caps.tline_ns / 2) / s_caps.tline_ns);
}

/* Onto the step grid first, then into range: esp_video rejects an off-grid
 * value outright (`value % step` in esp_video_cam.c). */
static int32_t clamp_lines(int32_t lines)
{
    int32_t step = s_caps.exp_step > 0 ? s_caps.exp_step : 1;
    lines -= lines % step;
    if (lines < s_caps.exp_min) lines = s_caps.exp_min;
    if (lines > s_caps.exp_max) lines = s_caps.exp_max - s_caps.exp_max % step;
    return lines;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- Exposure (§3.2) ---------------------------------------------------- */

static void write_exposure(int32_t lines);
static void write_gain(int32_t index);

/*
 * AGC on/off, keeping the ISP pipeline's view of the sensor honest.
 *
 * The pipeline's idea of the sensor (isp->sensor.cur_exposure/cur_gain and
 * prev_exposure_val/prev_gain_index in esp_video_isp_pipeline.c) is updated
 * only by its own writes, and AE plans from it; a write matching it is also
 * skipped. A manual write goes around all of that, so after manual -> auto AE
 * reasons about a sensor that is not there - measured: stuck at the manual
 * 2 ms / 16x. (Settling overbright after the switch is a different thing: the
 * AGC hold band, narrowed in ipa/imx708_control.json - see CMakeLists.txt.)
 *
 * So on the way out we remember what AE had put on the sensor, and on the way
 * back we put exactly that back *before* AE resumes. The order matters: AE may
 * write in the very next frame, and a restore landing after that is the same
 * mismatch again.
 */
static bool s_agc_on = true;
static int32_t s_ae_exp = -1, s_ae_gain = -1;
static uint32_t s_gl_target = GL_TARGET_DEFAULT;

static void agc_set(bool on)
{
    if (on == s_agc_on) return;
    if (!on) {
        if (!ctrl_get_user(V4L2_CID_EXPOSURE, &s_ae_exp)) s_ae_exp = -1;
        if (!ctrl_get_user(V4L2_CID_GAIN, &s_ae_gain)) s_ae_gain = -1;
        /* Only from a live picture: at boot, before the capture task runs,
         * the frame buffer holds nothing yet and the default stands. */
        uint32_t luma;
        if (s_ops && s_ops->mean_luma && s_ops->wait_frames(1, 200) && s_ops->mean_luma(&luma)) {
            s_gl_target = clamp_i32((int32_t)luma, GL_TARGET_MIN, GL_TARGET_MAX);
        }
    } else {
        if (s_ae_gain >= 0) write_gain(s_ae_gain);
        if (s_ae_exp >= 0) write_exposure(s_ae_exp);
    }
    esp_err_t r = esp_video_isp_pipeline_set_agc_status(on ? ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE
                                                           : ESP_VIDEO_ISP_PIPELINE_AGC_DISABLE);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "AGC %s failed: %s", on ? "enable" : "disable", esp_err_to_name(r));
        return;
    }
    s_agc_on = on;
}

/* Put the AGC's exposure range back to what the pipeline booted with. Without
 * this, "auto" after shutter priority stays pinned to the old shutter. */
static void agc_range_restore(void)
{
    if (!s_boot_range_ok) return;
    esp_err_t a = esp_video_isp_pipeline_set_agc_min_exposure(s_boot_min_us);
    esp_err_t b = esp_video_isp_pipeline_set_agc_max_exposure(s_boot_max_us);
    if (a != ESP_OK || b != ESP_OK) {
        ESP_LOGW(TAG, "restoring AGC range %" PRIu32 "..%" PRIu32 " us failed (%s / %s)",
                 s_boot_min_us, s_boot_max_us, esp_err_to_name(a), esp_err_to_name(b));
    }
}

/* Method A: pin the range to one exposure. Order keeps min <= max throughout. */
static void agc_range_pin(uint32_t us)
{
    esp_err_t a = s_boot_range_ok ? esp_video_isp_pipeline_set_agc_max_exposure(s_boot_max_us) : ESP_OK;
    esp_err_t b = esp_video_isp_pipeline_set_agc_min_exposure(us);
    esp_err_t c = esp_video_isp_pipeline_set_agc_max_exposure(us);
    if (a != ESP_OK || b != ESP_OK || c != ESP_OK) {
        ESP_LOGW(TAG, "pinning AGC range to %" PRIu32 " us failed (%s / %s / %s)", us,
                 esp_err_to_name(a), esp_err_to_name(b), esp_err_to_name(c));
    }
}

static void write_exposure(int32_t lines)
{
    int32_t got = -1;
    if (!ctrl_set_user(V4L2_CID_EXPOSURE, lines) || !ctrl_get_user(V4L2_CID_EXPOSURE, &got) || got != lines) {
        ESP_LOGW(TAG, "exposure: asked %" PRId32 " lines, reads back %" PRId32, lines, got);
    }
}

static void write_gain(int32_t index)
{
    int32_t got = -1;
    if (!ctrl_set_user(V4L2_CID_GAIN, index) || !ctrl_get_user(V4L2_CID_GAIN, &got) || got != index) {
        ESP_LOGW(TAG, "gain: asked index %" PRId32 ", reads back %" PRId32, index, got);
    }
}

static bool shutter_priority(void)
{
    return s_cfg.exp_mode == M_MANUAL && s_cfg.gain_mode == M_AUTO;
}

/* Apply s_cfg's exposure settings to the pipeline. Caller holds s_lock. */
static void apply_exposure_locked(void)
{
    if (s_cfg.exp_mode == M_AUTO) {
        agc_range_restore();
        agc_set(true);
        ESP_LOGI(TAG, "exposure: auto");
    } else if (s_cfg.gain_mode == M_AUTO) {
        if (s_sp_method == 'A') {
            uint32_t us = lines_to_us(s_cfg.exp_lines);
            agc_range_pin(us);
            agc_set(true);
            write_exposure(s_cfg.exp_lines);    /* immediate, rather than at AE's next step */
            ESP_LOGI(TAG, "exposure: shutter priority (A) %" PRId32 " lines = %" PRIu32 " us",
                     s_cfg.exp_lines, us);
        } else {
            agc_set(false);
            agc_range_restore();
            write_exposure(s_cfg.exp_lines);
            ESP_LOGI(TAG, "exposure: shutter priority (B) %" PRId32 " lines, gain loop on",
                     s_cfg.exp_lines);
        }
    } else {
        agc_set(false);
        agc_range_restore();
        write_gain(s_cfg.gain_index);
        write_exposure(s_cfg.exp_lines);
        ESP_LOGI(TAG, "exposure: manual %" PRId32 " lines, gain index %" PRId32,
                 s_cfg.exp_lines, s_cfg.gain_index);
    }
}

/* Method B's gain loop. Idle unless shutter priority with method B is in force. */
static void gain_loop_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GL_PERIOD_MS));
        if (!s_ops->mean_luma) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool active = shutter_priority() && s_sp_method == 'B';
        xSemaphoreGive(s_lock);
        if (!active) continue;

        uint32_t luma = 0;
        if (!s_ops->mean_luma(&luma)) continue;
        if (luma < 1) luma = 1;
        int32_t target = (int32_t)s_gl_target;
        int32_t err_pct = ((int32_t)luma - target) * 100 / target;
        if (abs(err_pct) <= GL_DEADBAND_PCT) continue;

        int delta = (int)lroundf(GL_STEPS_PER_STOP * log2f((float)target / (float)luma));
        delta = clamp_i32(delta, -GL_MAX_STEP, GL_MAX_STEP);
        if (delta == 0) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (shutter_priority() && s_sp_method == 'B') {
            int32_t cur = 0;
            if (ctrl_get_user(V4L2_CID_GAIN, &cur)) {
                int32_t next = clamp_i32(cur + delta, s_caps.gain_min, s_caps.gain_max);
                if (next != cur) write_gain(next);
            }
        }
        xSemaphoreGive(s_lock);
        s_ops->wait_frames(GL_SETTLE_FRAMES, 1000);
    }
}

/* ---- Focus (§3.3) ------------------------------------------------------- */

/* Move the lens unless the search has been called off. Returns false to stop. */
static bool af_move(int pos)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_af_abort) ok = focus_set(pos);
    xSemaphoreGive(s_lock);
    return ok;
}

static bool af_score_at(int pos, uint32_t *score)
{
    if (!af_move(pos)) return false;
    vTaskDelay(pdMS_TO_TICKS(AF_SETTLE_MS));
    if (!s_ops->wait_frames(1, 2000)) return false;
    return s_ops->sharpness(score);
}

/* One contrast search: coarse across the band, then fine around the winner
 * (whose own score is already known, so it is not measured twice). */
static bool af_run(void)
{
    int best = -1;
    uint32_t best_score = 0, score;

    for (int p = AF_SCAN_MIN; p <= AF_SCAN_MAX; p += AF_COARSE_STEP) {
        if (!af_score_at(p, &score)) return false;
        ESP_LOGD(TAG, "af coarse %4d -> %" PRIu32, p, score);
        if (best < 0 || score > best_score) { best = p; best_score = score; }
    }
    const int centre = best;
    int lo = clamp_i32(centre - AF_FINE_SPAN, s_caps.focus_min, s_caps.focus_max);
    int hi = clamp_i32(centre + AF_FINE_SPAN, s_caps.focus_min, s_caps.focus_max);
    for (int p = lo; p <= hi; p += AF_FINE_STEP) {
        if (p == centre) continue;
        if (!af_score_at(p, &score)) return false;
        ESP_LOGD(TAG, "af fine   %4d -> %" PRIu32, p, score);
        if (score > best_score) { best = p; best_score = score; }
    }
    if (!af_move(best)) return false;
    s_af_score = best_score;
    ESP_LOGI(TAG, "af: lens %d, sharpness %" PRIu32, best, best_score);
    return true;
}

static void af_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_af_boot_delay) {
            s_af_boot_delay = false;
            vTaskDelay(pdMS_TO_TICKS(AF_BOOT_DELAY_MS));
        }
        int64_t t0 = esp_timer_get_time();
        bool ok = af_run();
        s_af_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        s_af_state = ok ? AF_DONE : AF_FAILED;
        if (!ok) {
            ESP_LOGW(TAG, "af: %s after %" PRIu32 " ms", s_af_abort ? "cancelled" : "failed", s_af_ms);
        }
    }
}

/* Caller holds s_lock. False if a search is already running. */
static bool af_start_locked(void)
{
    if (s_af_state == AF_RUNNING) return false;
    s_af_abort = false;
    s_af_state = AF_RUNNING;
    xTaskNotifyGive(s_af_task);
    return true;
}

/* ---- White balance (§3.7) ----------------------------------------------- */

/* Put red/blue balance on the ISP. Caller holds s_lock. */
static void wb_write(int32_t red, int32_t blue)
{
    if (s_isp_fd < 0) return;
    struct v4l2_ext_control c[2] = {
        { .id = V4L2_CID_RED_BALANCE,  .value = red },
        { .id = V4L2_CID_BLUE_BALANCE, .value = blue },
    };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 2, .controls = c };
    if (ioctl(s_isp_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        ESP_LOGW(TAG, "white balance %" PRId32 "/%" PRId32 " not accepted", red, blue);
    }
}

/*
 * One step of the white-balance loop: measure, and pull red and blue towards
 * green. `centre` picks the one-push window over the whole frame. Sets
 * *neutral when the picture is already within the dead band. False if there
 * was nothing usable to measure.
 */
static bool wb_step(bool centre, bool *neutral)
{
    uint32_t rgb[3];
    if (!s_ops->mean_rgb || !s_ops->mean_rgb(centre, rgb) || !rgb[0] || !rgb[2]) return false;

    float kr = (float)rgb[1] / (float)rgb[0];
    float kb = (float)rgb[1] / (float)rgb[2];
    *neutral = fabsf(kr - 1.0f) * 100.0f <= WB_DEADBAND_PCT && fabsf(kb - 1.0f) * 100.0f <= WB_DEADBAND_PCT;
    if (*neutral) return true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.wb_red = clamp_i32((int32_t)lroundf(s_cfg.wb_red * powf(kr, WB_GAIN_EXP)), WB_MIN, WB_MAX);
    s_cfg.wb_blue = clamp_i32((int32_t)lroundf(s_cfg.wb_blue * powf(kb, WB_GAIN_EXP)), WB_MIN, WB_MAX);
    wb_write(s_cfg.wb_red, s_cfg.wb_blue);
    xSemaphoreGive(s_lock);
    return true;
}

/* Auto: grey world every WB_PERIOD_MS. One-push: on notification, converge on
 * the centre of the frame, then hold (wb_mode is already manual by then). */
static void wb_task(void *arg)
{
    for (;;) {
        bool neutral = false;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WB_PERIOD_MS))) {
            bool ok = false;
            for (int i = 0; i < WB_ONEPUSH_ITERS; i++) {
                if (!wb_step(true, &neutral)) break;
                if (neutral) { ok = true; break; }
                s_ops->wait_frames(WB_SETTLE_FRAMES, 1000);
            }
            s_wb_state = ok ? WB_DONE : WB_FAILED;
            ESP_LOGI(TAG, "wb one-push %s: red %" PRId32 " blue %" PRId32,
                     ok ? "done" : "did not settle", s_cfg.wb_red, s_cfg.wb_blue);
            save_later();
            continue;
        }
        if (s_cfg.wb_mode != M_AUTO || s_isp_fd < 0) continue;
        if (wb_step(false, &neutral) && !neutral) s_ops->wait_frames(WB_SETTLE_FRAMES, 1000);
    }
}

/* Caller holds s_lock. False if one-push is already running. */
static bool wb_onepush_start_locked(void)
{
    if (s_wb_state == WB_RUNNING) return false;
    s_cfg.wb_mode = M_MANUAL;       /* stops the grey-world loop; the result is then held */
    s_wb_state = WB_RUNNING;
    xTaskNotifyGive(s_wb_task);
    return true;
}

/* ---- Persistence (§3.6) ------------------------------------------------- */

static void save_now(void)
{
    if (!s_fs_ok) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "version", CFG_VERSION);
    cJSON_AddStringToObject(o, "exposure_mode", mode_str(s_cfg.exp_mode));
    cJSON_AddStringToObject(o, "gain_mode", mode_str(s_cfg.gain_mode));
    cJSON_AddNumberToObject(o, "exposure_us", lines_to_us(s_cfg.exp_lines));
    cJSON_AddNumberToObject(o, "gain_index", s_cfg.gain_index);
    cJSON_AddStringToObject(o, "focus_mode", mode_str(s_cfg.focus_mode));
    cJSON_AddNumberToObject(o, "focus_pos", s_cfg.focus_pos);
    cJSON_AddStringToObject(o, "wb_mode", mode_str(s_cfg.wb_mode));
    cJSON_AddNumberToObject(o, "wb_red", s_cfg.wb_red);
    cJSON_AddNumberToObject(o, "wb_blue", s_cfg.wb_blue);
    cJSON_AddBoolToObject(o, "hflip", s_cfg.hflip);
    cJSON_AddBoolToObject(o, "vflip", s_cfg.vflip);
    xSemaphoreGive(s_lock);

    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return;

    /* tmp then rename: LittleFS renames atomically over an existing name, so a
     * power cut mid-write leaves the old config.json intact. */
    FILE *f = fopen(CFG_TMP, "w");
    bool ok = f && fputs(text, f) >= 0;
    if (f) ok = (fclose(f) == 0) && ok;
    if (ok) {
        ok = rename(CFG_TMP, CFG_FILE) == 0;
    }
    ESP_LOGI(TAG, "config %s: %s", ok ? "saved" : "SAVE FAILED", text);
    cJSON_free(text);
}

static void save_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        save_now();
    }
}

static void save_timer_cb(void *arg)
{
    xTaskNotifyGive(s_save_task);
}

static void save_later(void)
{
    if (!s_save_timer) return;
    esp_timer_stop(s_save_timer);           /* restart: one write per burst */
    esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
}

static bool parse_mode(const char *s, cmode_t *out)
{
    if (!strcmp(s, "auto"))   { *out = M_AUTO;   return true; }
    if (!strcmp(s, "manual")) { *out = M_MANUAL; return true; }
    return false;
}

static cJSON *read_json(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

/* hflip / vflip from a parsed config.json; true and 1 both count. */
static void read_orientation(const cJSON *o)
{
    const cJSON *it;
    if ((it = cJSON_GetObjectItem(o, "hflip"))) s_cfg.hflip = cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint);
    if ((it = cJSON_GetObjectItem(o, "vflip"))) s_cfg.vflip = cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint);
}

/* Fill s_cfg from config.json. Unknown keys are ignored, out-of-range values
 * clamped, and anything unreadable leaves the defaults in place. */
static void load_config(void)
{
    struct stat st;
    const char *path = stat(CFG_FILE, &st) == 0 ? CFG_FILE : NULL;
    if (!path) {
        ESP_LOGI(TAG, "no config.json - defaults");
        return;
    }

    cJSON *o = read_json(path);
    cJSON *v = o ? cJSON_GetObjectItem(o, "version") : NULL;
    if (!o || !cJSON_IsNumber(v) || v->valueint != CFG_VERSION) {
        ESP_LOGW(TAG, "%s unreadable or wrong version - ignoring it, using defaults", path);
        s_config_state = "error";
        cJSON_Delete(o);
        return;
    }

    cmode_t m;
    cJSON *it;
    if ((it = cJSON_GetObjectItem(o, "exposure_mode")) && cJSON_IsString(it) && parse_mode(it->valuestring, &m)) s_cfg.exp_mode = m;
    if ((it = cJSON_GetObjectItem(o, "gain_mode")) && cJSON_IsString(it) && parse_mode(it->valuestring, &m)) s_cfg.gain_mode = m;
    if ((it = cJSON_GetObjectItem(o, "focus_mode")) && cJSON_IsString(it) && parse_mode(it->valuestring, &m)) s_cfg.focus_mode = m;
    if ((it = cJSON_GetObjectItem(o, "exposure_us")) && cJSON_IsNumber(it)) s_cfg.exp_lines = clamp_lines(us_to_lines((uint32_t)it->valuedouble));
    if ((it = cJSON_GetObjectItem(o, "gain_index")) && cJSON_IsNumber(it)) s_cfg.gain_index = clamp_i32(it->valueint, s_caps.gain_min, s_caps.gain_max);
    if ((it = cJSON_GetObjectItem(o, "focus_pos")) && cJSON_IsNumber(it)) s_cfg.focus_pos = clamp_i32(it->valueint, s_caps.focus_min, s_caps.focus_max);
    if ((it = cJSON_GetObjectItem(o, "wb_mode")) && cJSON_IsString(it) && parse_mode(it->valuestring, &m)) s_cfg.wb_mode = m;
    if ((it = cJSON_GetObjectItem(o, "wb_red")) && cJSON_IsNumber(it)) s_cfg.wb_red = clamp_i32(it->valueint, WB_MIN, WB_MAX);
    if ((it = cJSON_GetObjectItem(o, "wb_blue")) && cJSON_IsNumber(it)) s_cfg.wb_blue = clamp_i32(it->valueint, WB_MIN, WB_MAX);
    read_orientation(o);
    cJSON_Delete(o);

    s_config_state = "loaded";
    ESP_LOGI(TAG, "config loaded from %s: exposure %s/%s %" PRIu32 " us gain %" PRId32 ", focus %s %" PRId32,
             path, mode_str(s_cfg.exp_mode), mode_str(s_cfg.gain_mode), lines_to_us(s_cfg.exp_lines),
             s_cfg.gain_index, mode_str(s_cfg.focus_mode), s_cfg.focus_pos);
}

static void mount_fs(void)
{
    static bool tried;          /* camctl_saved_orientation() may have mounted it */
    if (tried) return;
    tried = true;
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_BASE,
        .partition_label = CFG_PARTITION,
        .format_if_mount_failed = true,
    };
    esp_err_t r = esp_vfs_littlefs_register(&conf);
    s_fs_ok = (r == ESP_OK);
    if (!s_fs_ok) {
        ESP_LOGW(TAG, "config FS mount failed (%s) - settings will not persist", esp_err_to_name(r));
    }
}

void camctl_saved_orientation(bool *hflip, bool *vflip)
{
    mount_fs();
    cJSON *o = s_fs_ok ? read_json(CFG_FILE) : NULL;
    cJSON *v = o ? cJSON_GetObjectItem(o, "version") : NULL;
    if (o && cJSON_IsNumber(v) && v->valueint == CFG_VERSION) read_orientation(o);
    cJSON_Delete(o);
    *hflip = s_cfg.hflip;
    *vflip = s_cfg.vflip;
}

/* A flip takes a restart (see camctl.h). Late enough for the reply to leave -
 * over the USB serial tunnel too. */
#define RESTART_DELAY_US    (800 * 1000)

static void restart_cb(void *arg)
{
    esp_restart();
}

static void restart_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "flip_restart" };
    if (!t && esp_timer_create(&ta, &t) != ESP_OK) return;
    ESP_LOGW(TAG, "orientation changed - restarting to apply it");
    esp_timer_start_once(t, RESTART_DELAY_US);
}

/* ---- Init --------------------------------------------------------------- */

static void read_caps(void)
{
    struct v4l2_query_ext_ctrl q = { .id = V4L2_CID_EXPOSURE };
    if (ioctl(s_fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        s_caps.exp_min = (int32_t)q.minimum;
        s_caps.exp_max = (int32_t)q.maximum;
        s_caps.exp_step = (int32_t)(q.step ? q.step : 1);
    } else {
        ESP_LOGE(TAG, "exposure range unreadable");
    }

    esp_cam_sensor_format_t sf;
    s_caps.tline_ns = 13361;    /* IMX708_TLINE_NS, if the format will not say */
    if (ioctl(s_fd, VIDIOC_G_SENSOR_FMT, &sf) == 0 && sf.isp_info && sf.isp_info->isp_v1_info.tline_ns) {
        s_caps.tline_ns = sf.isp_info->isp_v1_info.tline_ns;
    }

    q = (struct v4l2_query_ext_ctrl){ .id = V4L2_CID_GAIN };
    if (ioctl(s_fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        s_caps.gain_min = (int32_t)q.minimum;
        s_caps.gain_max = (int32_t)q.maximum;
        if (s_caps.gain_max >= GAIN_MENU_MAX) s_caps.gain_max = GAIN_MENU_MAX - 1;
    }
    for (int32_t i = s_caps.gain_min; i <= s_caps.gain_max; i++) {
        struct v4l2_querymenu qm = { .id = V4L2_CID_GAIN, .index = (uint32_t)i };
        s_caps.gain_milli[i] = ioctl(s_fd, VIDIOC_QUERYMENU, &qm) == 0 ? (uint32_t)qm.value : 0;
    }

    q = (struct v4l2_query_ext_ctrl){ .id = V4L2_CID_FOCUS_ABSOLUTE };
    if (ioctl(s_fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        s_caps.focus_min = (int32_t)q.minimum;
        s_caps.focus_max = (int32_t)q.maximum;
    } else {
        ESP_LOGE(TAG, "focus control not found - is the DW9807 detected?");
        s_caps.focus_min = 0;
        s_caps.focus_max = 1023;
    }

    ESP_LOGI(TAG, "caps: exposure %" PRId32 "..%" PRId32 " step %" PRId32 " lines (tline %" PRIu32
                  " ns = %" PRIu32 "..%" PRIu32 " us), gain index %" PRId32 "..%" PRId32
                  " (%" PRIu32 "..%" PRIu32 " milli), focus %" PRId32 "..%" PRId32,
             s_caps.exp_min, s_caps.exp_max, s_caps.exp_step, s_caps.tline_ns,
             lines_to_us(s_caps.exp_min), lines_to_us(s_caps.exp_max),
             s_caps.gain_min, s_caps.gain_max, s_caps.gain_milli[s_caps.gain_min],
             s_caps.gain_milli[s_caps.gain_max], s_caps.focus_min, s_caps.focus_max);
}

esp_err_t camctl_init(int fd, const camctl_frame_ops_t *ops)
{
    s_fd = fd;
    s_ops = ops;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    read_caps();

    s_boot_range_ok = esp_video_isp_pipeline_get_agc_min_exposure(&s_boot_min_us) == ESP_OK &&
                      esp_video_isp_pipeline_get_agc_max_exposure(&s_boot_max_us) == ESP_OK;
    ESP_LOGI(TAG, "AGC boot range: %s %" PRIu32 "..%" PRIu32 " us",
             s_boot_range_ok ? "ok" : "UNREADABLE", s_boot_min_us, s_boot_max_us);

    /* Defaults for the manual values: whatever the sensor holds right now. */
    int32_t v;
    if (ctrl_get_user(V4L2_CID_EXPOSURE, &v)) s_cfg.exp_lines = clamp_lines(v);
    if (ctrl_get_user(V4L2_CID_GAIN, &v)) s_cfg.gain_index = v;
    int f = focus_get();
    if (f >= 0) s_cfg.focus_pos = f;

    mount_fs();
    if (s_fs_ok) load_config();

    /* A second handle on the ISP device (esp_video counts references); white
     * balance lives there, not on the capture device. */
    s_isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (s_isp_fd < 0) {
        ESP_LOGW(TAG, "cannot open %s - white balance control unavailable", ESP_VIDEO_ISP1_DEVICE_NAME);
    }

    const esp_timer_create_args_t ta = { .callback = save_timer_cb, .name = "cfg_save" };
    esp_timer_create(&ta, &s_save_timer);

    if (xTaskCreate(af_task, "camctl_af", 4096, NULL, 4, &s_af_task) != pdPASS ||
        xTaskCreate(save_task, "camctl_save", 4096, NULL, 2, &s_save_task) != pdPASS ||
        xTaskCreate(gain_loop_task, "camctl_gain", 3072, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(wb_task, "camctl_wb", 3072, NULL, 3, &s_wb_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    wb_write(s_cfg.wb_red, s_cfg.wb_blue);
    apply_exposure_locked();
    if (s_cfg.focus_mode == M_MANUAL) {
        if (!focus_set(s_cfg.focus_pos)) ESP_LOGW(TAG, "could not move the lens to %" PRId32, s_cfg.focus_pos);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void camctl_boot_af(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.focus_mode == M_AUTO) {
        s_af_boot_delay = true;
        af_start_locked();
    }
    xSemaphoreGive(s_lock);
}

bool camctl_any_auto(void)
{
    if (!s_lock) return true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool any = s_cfg.exp_mode == M_AUTO || s_cfg.gain_mode == M_AUTO || s_cfg.wb_mode == M_AUTO;
    xSemaphoreGive(s_lock);
    return any;
}

/* ---- HTTP (§3.5) -------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body, int len)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[160];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return send_json(req, status, body, n);
}

static esp_err_t send_state(httpd_req_t *req, const char *status)
{
    int32_t exp = -1, gain = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ctrl_get_user(V4L2_CID_EXPOSURE, &exp);
    ctrl_get_user(V4L2_CID_GAIN, &gain);
    int focus = focus_get();
    cmode_t em = s_cfg.exp_mode, gm = s_cfg.gain_mode, fm = s_cfg.focus_mode, wm = s_cfg.wb_mode;
    int32_t wr = s_cfg.wb_red, wbl = s_cfg.wb_blue;
    bool hf = s_cfg.hflip, vf = s_cfg.vflip;
    xSemaphoreGive(s_lock);

    uint32_t gm_milli = (gain >= s_caps.gain_min && gain <= s_caps.gain_max) ? s_caps.gain_milli[gain] : 0;
    char body[600];
    int n = snprintf(body, sizeof(body),
        "{\"exposure_mode\":\"%s\",\"gain_mode\":\"%s\",\"focus_mode\":\"%s\","
        "\"exposure_us\":%" PRIu32 ",\"exposure_lines\":%" PRId32 ","
        "\"gain_index\":%" PRId32 ",\"gain_x\":%" PRIu32 ".%02" PRIu32 ","
        "\"focus_pos\":%d,\"af_state\":\"%s\",\"af_score\":%" PRIu32 ",\"af_ms\":%" PRIu32 ","
        "\"wb_mode\":\"%s\",\"wb_red\":%" PRId32 ",\"wb_blue\":%" PRId32 ",\"wb_state\":\"%s\","
        "\"hflip\":%s,\"vflip\":%s,\"shutter_priority\":\"%c\",\"config\":\"%s\"}",
        mode_str(em), mode_str(gm), mode_str(fm),
        exp >= 0 ? lines_to_us(exp) : 0, exp, gain, gm_milli / 1000, (gm_milli % 1000) / 10,
        focus, s_af_names[s_af_state], s_af_score, s_af_ms,
        mode_str(wm), wr, wbl, s_wb_names[s_wb_state], hf ? "true" : "false", vf ? "true" : "false",
        s_sp_method, s_config_state);
    return send_json(req, status, body, n);
}

static esp_err_t ctrl_get_handler(httpd_req_t *req)
{
    return send_state(req, NULL);
}

#define CAMCTL_STR_(x) #x
#define CAMCTL_STR(x)  CAMCTL_STR_(x)

static esp_err_t caps_handler(httpd_req_t *req)
{
    char body[900];
    int n = snprintf(body, sizeof(body),
        "{\"tline_ns\":%" PRIu32 ",\"exposure_us\":{\"min\":%" PRIu32 ",\"max\":%" PRIu32 ",\"step\":%" PRIu32 "},"
        "\"gain\":{\"min\":%" PRId32 ",\"max\":%" PRId32 ",\"x\":[",
        s_caps.tline_ns, lines_to_us(s_caps.exp_min), lines_to_us(s_caps.exp_max),
        lines_to_us(s_caps.exp_step), s_caps.gain_min, s_caps.gain_max);
    for (int32_t i = s_caps.gain_min; i <= s_caps.gain_max && n < (int)sizeof(body) - 120; i++) {
        uint32_t m = s_caps.gain_milli[i];
        n += snprintf(body + n, sizeof(body) - n, "%s%" PRIu32 ".%03" PRIu32,
                      i > s_caps.gain_min ? "," : "", m / 1000, m % 1000);
    }
    n += snprintf(body + n, sizeof(body) - n,
        "]},\"focus\":{\"min\":%" PRId32 ",\"max\":%" PRId32 ",\"scan_min\":%d,\"scan_max\":%d},"
        "\"wb\":%s,\"shutter_priority\":\"%c\"}",
        s_caps.focus_min, s_caps.focus_max, AF_SCAN_MIN, AF_SCAN_MAX,
        s_isp_fd >= 0 ? "{\"min\":" CAMCTL_STR(WB_MIN) ",\"max\":" CAMCTL_STR(WB_MAX) "}" : "null",
        s_sp_method);
    return send_json(req, NULL, body, n);
}

/* POST /ctrl?key=value... - only the keys given change (§3.5). */
static esp_err_t ctrl_post_handler(httpd_req_t *req)
{
    char q[256] = "", v[16];
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql >= sizeof(q)) return send_error(req, "400 Bad Request", "query too long");
    if (ql) httpd_req_get_url_query_str(req, q, sizeof(q));

    /* Parse everything before touching anything, so a bad key changes nothing. */
    static const char *const known[] = { "exposure_mode", "gain_mode", "exposure_us",
                                         "gain_index", "focus_mode", "focus_pos",
                                         "wb_mode", "wb_red", "wb_blue", "sp", "hflip", "vflip" };
    for (char *p = q; *p; ) {
        char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        char *eq = memchr(p, '=', len);
        size_t klen = eq ? (size_t)(eq - p) : len;
        bool ok = false;
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strlen(known[i]) == klen && !strncmp(p, known[i], klen)) ok = true;
        }
        if (!ok) return send_error(req, "400 Bad Request", "unknown key");
        p = amp ? amp + 1 : p + len;
    }

    bool has_em = false, has_gm = false, has_fm = false, has_eu = false, has_gi = false, has_fp = false;
    bool has_wm = false, has_wr = false, has_wb = false;
    cmode_t em = M_AUTO, gm = M_AUTO, fm = M_AUTO, wm = M_AUTO;
    long eu = 0, gi = 0, fp = 0, wr = 0, wbl = 0;
    char sp = 0;
    char *end;

#define GET_MODE(key, has, out) \
    if (httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK) { \
        if (!parse_mode(v, &out)) return send_error(req, "400 Bad Request", key " must be auto or manual"); \
        has = true; }
#define GET_INT(key, has, out) \
    if (httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK) { \
        out = strtol(v, &end, 10); \
        if (end == v || *end) return send_error(req, "400 Bad Request", key " must be an integer"); \
        has = true; }

    GET_MODE("exposure_mode", has_em, em);
    GET_MODE("gain_mode", has_gm, gm);
    GET_MODE("focus_mode", has_fm, fm);
    GET_INT("exposure_us", has_eu, eu);
    GET_INT("gain_index", has_gi, gi);
    GET_INT("focus_pos", has_fp, fp);
    GET_MODE("wb_mode", has_wm, wm);
    GET_INT("wb_red", has_wr, wr);
    GET_INT("wb_blue", has_wb, wbl);
#undef GET_MODE
#undef GET_INT
    if (httpd_query_key_value(q, "sp", v, sizeof(v)) == ESP_OK) {
        if (strcmp(v, "A") && strcmp(v, "B")) return send_error(req, "400 Bad Request", "sp must be A or B");
        sp = v[0];
    }
    int hf = -1, vf = -1;   /* -1 = not given */
    if (httpd_query_key_value(q, "hflip", v, sizeof(v)) == ESP_OK) {
        if (strcmp(v, "0") && strcmp(v, "1")) return send_error(req, "400 Bad Request", "hflip must be 0 or 1");
        hf = v[0] == '1';
    }
    if (httpd_query_key_value(q, "vflip", v, sizeof(v)) == ESP_OK) {
        if (strcmp(v, "0") && strcmp(v, "1")) return send_error(req, "400 Bad Request", "vflip must be 0 or 1");
        vf = v[0] == '1';
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool exp_dirty = false;
    int32_t cur;

    if (sp && sp != s_sp_method) { s_sp_method = sp; exp_dirty = true; }

    /* Entering manual without a value: start from what AE is using now, so the
     * picture does not jump at the switch. */
    if (has_em && em != s_cfg.exp_mode) {
        if (em == M_MANUAL) {
            /* Both from the sensor, whatever gain_mode says: a saved manual gain
             * paired with AE's live exposure is a jump in brightness. */
            if (!has_eu && ctrl_get_user(V4L2_CID_EXPOSURE, &cur)) s_cfg.exp_lines = clamp_lines(cur);
            if (!has_gi && ctrl_get_user(V4L2_CID_GAIN, &cur)) s_cfg.gain_index = cur;
        }
        s_cfg.exp_mode = em;
        exp_dirty = true;
    }
    if (has_gm && gm != s_cfg.gain_mode) {
        if (gm == M_MANUAL && !has_gi && ctrl_get_user(V4L2_CID_GAIN, &cur)) s_cfg.gain_index = cur;
        s_cfg.gain_mode = gm;
        exp_dirty = true;
    }
    if (has_eu) {
        long us = eu < 0 ? 0 : eu;
        s_cfg.exp_lines = clamp_lines(us_to_lines((uint32_t)us));
        exp_dirty = true;
    }
    if (has_gi) {
        s_cfg.gain_index = clamp_i32((int32_t)gi, s_caps.gain_min, s_caps.gain_max);
        exp_dirty = true;
    }
    if (exp_dirty && (s_cfg.exp_mode == M_MANUAL || has_em || sp)) apply_exposure_locked();

    bool start_af = false;
    if (has_fp) s_cfg.focus_pos = clamp_i32((int32_t)fp, s_caps.focus_min, s_caps.focus_max);
    if (has_fm) {
        if (fm == M_AUTO && s_cfg.focus_mode == M_MANUAL) start_af = true;
        /* Manual without a position keeps the lens where AF left it. */
        if (fm == M_MANUAL && s_cfg.focus_mode == M_AUTO && !has_fp) {
            int f = focus_get();
            if (f >= 0) s_cfg.focus_pos = f;
        }
        s_cfg.focus_mode = fm;
    }
    if (s_cfg.focus_mode == M_MANUAL && (has_fp || has_fm)) {
        s_af_abort = true;      /* a running search must not move the lens back */
        if (!focus_set(s_cfg.focus_pos)) ESP_LOGW(TAG, "focus_set(%" PRId32 ") failed", s_cfg.focus_pos);
    }
    if (start_af) af_start_locked();

    /* White balance: manual values are written as given; auto hands the
     * current gains to the grey-world loop as its starting point. A one-push
     * in progress is overridden by any explicit choice. */
    if (has_wr) s_cfg.wb_red = clamp_i32((int32_t)wr, WB_MIN, WB_MAX);
    if (has_wb) s_cfg.wb_blue = clamp_i32((int32_t)wbl, WB_MIN, WB_MAX);
    if (has_wm) s_cfg.wb_mode = wm;
    if ((has_wr || has_wb) && s_cfg.wb_mode == M_MANUAL) wb_write(s_cfg.wb_red, s_cfg.wb_blue);

    /* Orientation applies at boot only (camctl.h): save now, reply, restart. */
    bool reorient = (hf >= 0 && hf != s_cfg.hflip) || (vf >= 0 && vf != s_cfg.vflip);
    if (hf >= 0) s_cfg.hflip = hf;
    if (vf >= 0) s_cfg.vflip = vf;
    xSemaphoreGive(s_lock);

    if (reorient) {
        if (s_save_timer) esp_timer_stop(s_save_timer);
        save_now();
        esp_err_t r = send_state(req, NULL);
        restart_soon();
        return r;
    }
    save_later();
    return send_state(req, NULL);
}

static esp_err_t af_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = af_start_locked();
    if (ok && s_cfg.focus_mode != M_AUTO) {
        s_cfg.focus_mode = M_AUTO;
        save_later();
    }
    xSemaphoreGive(s_lock);
    if (!ok) return send_error(req, "409 Conflict", "af already running");
    static const char body[] = "{\"af_state\":\"running\"}";
    return send_json(req, "202 Accepted", body, sizeof(body) - 1);
}

static esp_err_t onepush_handler(httpd_req_t *req)
{
    if (s_isp_fd < 0) return send_error(req, "503 Service Unavailable", "white balance unavailable");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = wb_onepush_start_locked();
    xSemaphoreGive(s_lock);
    if (!ok) return send_error(req, "409 Conflict", "one-push already running");
    static const char body[] = "{\"wb_state\":\"running\"}";
    return send_json(req, "202 Accepted", body, sizeof(body) - 1);
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    if (s_fs_ok) {
        unlink(CFG_FILE);
        unlink(CFG_TMP);
    }
    if (s_save_timer) esp_timer_stop(s_save_timer);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.exp_mode = s_cfg.gain_mode = s_cfg.wb_mode = M_AUTO;
    bool was_manual_focus = s_cfg.focus_mode == M_MANUAL;
    s_cfg.focus_mode = M_AUTO;
    apply_exposure_locked();
    if (was_manual_focus) af_start_locked();
    s_config_state = "default";
    bool was_flipped = s_cfg.hflip || s_cfg.vflip;
    s_cfg.hflip = s_cfg.vflip = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "config reset to defaults");
    esp_err_t r = send_state(req, NULL);
    if (was_flipped) restart_soon();   /* the sensor is still flipped until then */
    return r;
}

esp_err_t camctl_register_http(httpd_handle_t server)
{
    static const httpd_uri_t uris[CAMCTL_URI_COUNT] = {
        { .uri = "/ctrl",         .method = HTTP_GET,  .handler = ctrl_get_handler },
        { .uri = "/ctrl",         .method = HTTP_POST, .handler = ctrl_post_handler },
        { .uri = "/ctrl/caps",    .method = HTTP_GET,  .handler = caps_handler },
        { .uri = "/af",           .method = HTTP_POST, .handler = af_handler },
        { .uri = "/wb/onepush",   .method = HTTP_POST, .handler = onepush_handler },
        { .uri = "/config/reset", .method = HTTP_POST, .handler = reset_handler },
    };
    for (size_t i = 0; i < CAMCTL_URI_COUNT; i++) {
        esp_err_t r = httpd_register_uri_handler(server, &uris[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(r));
            return r;
        }
    }
    return ESP_OK;
}
