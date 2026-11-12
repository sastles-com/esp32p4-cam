/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * camctl: exposure, gain, focus and white balance control for the IMX708, over
 * HTTP, with the settings kept in a config.json on LittleFS.
 *
 * White balance needs the IPA's AWB out of the tuning JSON (no runtime switch
 * for it exists); camctl then owns the ISP's red/blue balance.
 *
 * Built for CONFIG_ESP_VIDEO_ISP_PIPELINE_CONTROL_CAMERA_MOTOR=n: the
 * application owns the lens, and "auto" focus is a one-shot contrast search
 * run here rather than the IPA's closed loop.
 *
 * camctl knows nothing about how frames are stored. The application hands it
 * three callbacks that work on whatever it publishes - RGB565 for stills, a Y
 * plane for video - so the same module can sit behind either pipeline.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Block until `n` frames newer than the call have been published. False on timeout. */
    bool (*wait_frames)(uint32_t n, uint32_t timeout_ms);
    /* Centre sharpness of the newest frame; bigger is sharper. Units are the app's own. */
    bool (*sharpness)(uint32_t *score);
    /* Mean luma of the newest frame, 0..255. Only used by shutter-priority method B. */
    bool (*mean_luma)(uint32_t *luma);
    /*
     * Mean R, G, B of the newest frame on one common 0..255 scale, over pixels
     * that are neither near black nor clipped in any channel. `centre` limits
     * it to the middle quarter of the frame (one-push white balance); otherwise
     * the whole frame (grey world). False if too few pixels qualified.
     */
    bool (*mean_rgb)(bool centre, uint32_t rgb[3]);
} camctl_frame_ops_t;

/*
 * Call once, after VIDIOC_STREAMON. Reads the control ranges, records the AE
 * exposure range the pipeline started with, mounts the config FS and applies
 * the saved settings (exposure, and the lens position if focus is manual).
 * `fd` is the capture device; `ops` must outlive the program.
 */
esp_err_t camctl_init(int fd, const camctl_frame_ops_t *ops);

/* After WiFi is up: if focus is auto, run one AF search once AE has settled. */
void camctl_boot_af(void);

/*
 * True while anything still adjusts the picture by itself - AE, shutter-priority
 * gain or grey-world white balance - so a change of lighting needs time to
 * settle. False when exposure, gain and white balance are all manual.
 */
bool camctl_any_auto(void);

/*
 * The saved image orientation (hflip / vflip in config.json), for camcore to
 * apply right after opening the camera. It has to be then: the ISP takes the
 * sensor's Bayer phase only when the format is set, so a flip changed while
 * streaming gives wrong colours. Changing it over POST /ctrl therefore saves
 * and restarts the board. Mounts the config FS if camctl_init has not.
 */
void camctl_saved_orientation(bool *hflip, bool *vflip);

/* Adds GET/POST /ctrl, GET /ctrl/caps, POST /af, POST /wb/onepush and POST /config/reset. */
esp_err_t camctl_register_http(httpd_handle_t server);

/* Number of URI handlers camctl_register_http() adds, for max_uri_handlers. */
#define CAMCTL_URI_COUNT 6

#ifdef __cplusplus
}
#endif
