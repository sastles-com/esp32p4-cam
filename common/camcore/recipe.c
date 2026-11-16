/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * recipe: named shooting recipes (set shooting) kept on the board, in
 * CFG_BASE/recipes.json. The board only stores and checks them; the web UI
 * runs them through /presets/load, /io, /ctrl and /snapshot.jpg. See
 * usb-app/API.md § recipes for the data model.
 *
 * recipes.json = {"version":1,"recipes":[<recipe>, ...]} in the order first saved.
 * A recipe is stored as save normalised it (every step key present), so what
 * GET /recipes returns - and the UI exports - is exactly what is on the board.
 *
 * Only the httpd task touches the file (all the handlers below), so no lock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "cfgstore.h"
#include "recipe.h"

static const char *TAG = "recipe";

#define RECIPES_FILE        CFG_BASE "/recipes.json"
#define RECIPES_TMP         CFG_BASE "/recipes.json.tmp"
#define RECIPES_VERSION     1
#define RECIPES_MAX         20
#define RECIPE_STEPS_MAX    30
/* One recipe as sent: 30 steps with every override is about 7 kB. */
#define RECIPE_BODY_MAX     (12 * 1024)
/* The whole file: RECIPES_MAX full recipes, with room to spare. Read into
 * PSRAM (malloc above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL goes there). */
#define RECIPES_READ_MAX    (RECIPES_MAX * RECIPE_BODY_MAX)

#define STEP_Q_DEFAULT      95
#define STEP_SETTLE_DEFAULT 800
#define STEP_SETTLE_MAX     60000

/* ---- HTTP helpers (each module keeps its own, as camctl / shot / ioexp do) ---- */

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
    char body[200];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    if (n >= (int)sizeof(body)) n = sizeof(body) - 1;
    return send_json(req, status, body, n);
}

/* ---- Validation --------------------------------------------------------- */

static bool is_int(const cJSON *it, long lo, long hi)
{
    if (!cJSON_IsNumber(it)) return false;
    double v = it->valuedouble;
    return v == floor(v) && v >= lo && v <= hi;
}

static bool is_mode(const cJSON *it)
{
    return cJSON_IsString(it) && (!strcmp(it->valuestring, "auto") || !strcmp(it->valuestring, "manual"));
}

/* The keys a step may override: the preset's camera keys (not the orientation,
 * sp or the shot settings). Ints are checked loosely; the board clamps them
 * to the sensor's range when they are applied through /ctrl. */
static const char *const MODE_KEYS[] = { "exposure_mode", "gain_mode", "focus_mode", "wb_mode" };
static const char *const INT_KEYS[]  = { "exposure_us", "gain_index", "focus_pos", "wb_red", "wb_blue" };
#define N_OF(a) (sizeof(a) / sizeof((a)[0]))

static bool in_list(const char *k, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!strcmp(k, list[i])) return true;
    return false;
}

/* overrides -> a fresh normalised copy in *out. NULL if fine, else the message. */
static const char *check_overrides(const cJSON *ov, cJSON **out)
{
    *out = cJSON_CreateObject();
    if (!ov || cJSON_IsNull(ov)) return NULL;
    if (!cJSON_IsObject(ov)) return "overrides must be an object";
    const cJSON *it;
    cJSON_ArrayForEach(it, ov) {
        const char *k = it->string;
        if (in_list(k, MODE_KEYS, N_OF(MODE_KEYS))) {
            if (!is_mode(it)) return "overrides: *_mode must be \\\"auto\\\" or \\\"manual\\\"";
            cJSON_AddStringToObject(*out, k, it->valuestring);
        } else if (in_list(k, INT_KEYS, N_OF(INT_KEYS))) {
            if (!is_int(it, 0, 100000000)) return "overrides: values must be integers >= 0";
            cJSON_AddNumberToObject(*out, k, it->valuedouble);
        } else {
            return "overrides: unknown key (allowed: exposure_mode, exposure_us, gain_mode, gain_index, "
                   "focus_mode, focus_pos, wb_mode, wb_red, wb_blue)";
        }
    }
    return NULL;
}

/* One step -> normalised (every key present) in *out. NULL if fine. */
static const char *check_step(const cJSON *st, cJSON **out)
{
    *out = NULL;
    if (!cJSON_IsObject(st)) return "each step must be an object";
    const cJSON *it;
    cJSON_ArrayForEach(it, st) {
        if (strcmp(it->string, "enabled") && strcmp(it->string, "overrides") && strcmp(it->string, "light_out") &&
            strcmp(it->string, "q") && strcmp(it->string, "settle_ms")) {
            return "step: unknown key (allowed: enabled, overrides, light_out, q, settle_ms)";
        }
    }
    cJSON *o = cJSON_CreateObject();
    *out = o;

    it = cJSON_GetObjectItem(st, "enabled");
    if (it && !cJSON_IsBool(it)) return "enabled must be true or false";
    cJSON_AddBoolToObject(o, "enabled", !it || cJSON_IsTrue(it));

    cJSON *ov;
    const char *err = check_overrides(cJSON_GetObjectItem(st, "overrides"), &ov);
    cJSON_AddItemToObject(o, "overrides", ov);
    if (err) return err;

    it = cJSON_GetObjectItem(st, "light_out");
    if (!it || cJSON_IsNull(it)) cJSON_AddNullToObject(o, "light_out");
    else if (is_int(it, 0, 255)) cJSON_AddNumberToObject(o, "light_out", it->valuedouble);
    else return "light_out must be 0..255 or null";

    it = cJSON_GetObjectItem(st, "q");
    if (it && !is_int(it, 1, 100)) return "q must be 1..100";
    cJSON_AddNumberToObject(o, "q", it ? it->valuedouble : STEP_Q_DEFAULT);

    it = cJSON_GetObjectItem(st, "settle_ms");
    if (it && !is_int(it, 0, STEP_SETTLE_MAX)) return "settle_ms must be 0..60000";
    cJSON_AddNumberToObject(o, "settle_ms", it ? it->valuedouble : STEP_SETTLE_DEFAULT);
    return NULL;
}

/* A recipe as sent -> normalised copy in *out (caller frees). NULL if fine. */
static const char *check_recipe(const cJSON *r, cJSON **out)
{
    *out = NULL;
    if (!cJSON_IsObject(r)) return "body must be a JSON object";
    const cJSON *it;
    cJSON_ArrayForEach(it, r) {
        if (strcmp(it->string, "name") && strcmp(it->string, "base_preset") && strcmp(it->string, "steps")) {
            return "unknown key (allowed: name, base_preset, steps)";
        }
    }
    const cJSON *name = cJSON_GetObjectItem(r, "name");
    if (!cJSON_IsString(name) || !cfg_name_ok(name->valuestring)) return CFG_BAD_NAME;
    const cJSON *base = cJSON_GetObjectItem(r, "base_preset");
    if (base && !cJSON_IsNull(base) && (!cJSON_IsString(base) || !cfg_name_ok(base->valuestring))) {
        return "base_preset must be a preset name or null";
    }
    const cJSON *steps = cJSON_GetObjectItem(r, "steps");
    if (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) < 1) return "steps must be a non-empty array";
    if (cJSON_GetArraySize(steps) > RECIPE_STEPS_MAX) return "too many steps (max 30)";

    cJSON *o = cJSON_CreateObject();
    *out = o;
    cJSON_AddStringToObject(o, "name", name->valuestring);
    if (base && cJSON_IsString(base)) cJSON_AddStringToObject(o, "base_preset", base->valuestring);
    else cJSON_AddNullToObject(o, "base_preset");
    cJSON *arr = cJSON_AddArrayToObject(o, "steps");
    cJSON_ArrayForEach(it, steps) {
        cJSON *st;
        const char *err = check_step(it, &st);
        if (st) cJSON_AddItemToArray(arr, st);
        if (err) return err;
    }
    return NULL;
}

/* ---- Storage ------------------------------------------------------------ */

/* The stored list (never NULL). Entries that no longer pass check_recipe are dropped. */
static cJSON *recipes_read(void)
{
    cJSON *o = cfg_fs_ok() ? cfg_read_json(RECIPES_FILE, RECIPES_READ_MAX) : NULL;
    cJSON *v = o ? cJSON_GetObjectItem(o, "version") : NULL;
    cJSON *src = NULL;
    if (o && cJSON_IsNumber(v) && v->valueint == RECIPES_VERSION) src = cJSON_GetObjectItem(o, "recipes");
    else if (o) ESP_LOGW(TAG, "%s: wrong version - ignored", RECIPES_FILE);
    cJSON *arr = cJSON_CreateArray();
    const cJSON *it;
    if (cJSON_IsArray(src)) {
        cJSON_ArrayForEach(it, src) {
            cJSON *r;
            const char *err = check_recipe(it, &r);
            if (err) {
                ESP_LOGW(TAG, "stored recipe dropped: %s", err);
                cJSON_Delete(r);
            } else {
                cJSON_AddItemToArray(arr, r);
            }
        }
    }
    cJSON_Delete(o);
    return arr;
}

static bool recipes_write(cJSON *arr)
{
    if (!cfg_fs_ok()) return false;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "version", RECIPES_VERSION);
    cJSON_AddItemReferenceToObject(o, "recipes", arr);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);                        /* a reference: arr itself stays */
    bool ok = text && cfg_write_file(RECIPES_TMP, RECIPES_FILE, text);
    if (!ok) ESP_LOGE(TAG, "recipes: SAVE FAILED");
    cJSON_free(text);
    return ok;
}

static int recipe_index(cJSON *arr, const char *name)
{
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, i), "name");
        if (cJSON_IsString(it) && !strcmp(it->valuestring, name)) return i;
    }
    return -1;
}

/* {"max":N,"max_steps":M,"recipes":[…]}. Frees arr. */
static esp_err_t send_recipes(httpd_req_t *req, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "max", RECIPES_MAX);
    cJSON_AddNumberToObject(o, "max_steps", RECIPE_STEPS_MAX);
    cJSON_AddItemToObject(o, "recipes", arr);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return send_error(req, "500 Internal Server Error", "no memory");
    esp_err_t r = send_json(req, NULL, text, (int)strlen(text));
    cJSON_free(text);
    return r;
}

/* The query and its decoded name=. Sends the 400 itself and returns false if bad. */
#define RECIPE_QUERY_MAX    (2 * 3 * CFG_NAME_MAX + 32)
static bool name_query(httpd_req_t *req, char *q, char *name)
{
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql >= RECIPE_QUERY_MAX) { send_error(req, "400 Bad Request", "query too long"); return false; }
    q[0] = '\0';
    if (ql) httpd_req_get_url_query_str(req, q, RECIPE_QUERY_MAX);
    if (!cfg_query_text(q, "name", name, CFG_NAME_MAX + 1) || !cfg_name_ok(name)) {
        send_error(req, "400 Bad Request", CFG_BAD_NAME);
        return false;
    }
    return true;
}

/* The request body, whole, NUL-terminated (caller frees). NULL after sending the error. */
static char *read_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0) { send_error(req, "400 Bad Request", "a JSON body is required"); return NULL; }
    if (len > RECIPE_BODY_MAX) { send_error(req, "413 Payload Too Large", "recipe too large"); return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { send_error(req, "500 Internal Server Error", "no memory"); return NULL; }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (r <= 0) {
            free(buf);
            send_error(req, "400 Bad Request", "body cut short");
            return NULL;
        }
        got += (size_t)r;
    }
    buf[len] = '\0';
    return buf;
}

/* ---- Handlers ----------------------------------------------------------- */

static esp_err_t recipes_get_handler(httpd_req_t *req)
{
    return send_recipes(req, recipes_read());
}

/* POST /recipes/save, body = one recipe. Replaces one of the same name (keeping its place). */
static esp_err_t recipe_save_handler(httpd_req_t *req)
{
    if (!cfg_fs_ok()) return send_error(req, "503 Service Unavailable", "settings storage unavailable");
    char *body = read_body(req);
    if (!body) return ESP_OK;
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) return send_error(req, "400 Bad Request", "body is not JSON");
    cJSON *r;
    const char *err = check_recipe(in, &r);
    cJSON_Delete(in);
    if (err) {
        cJSON_Delete(r);
        return send_error(req, "400 Bad Request", err);
    }

    const char *name = cJSON_GetObjectItem(r, "name")->valuestring;
    cJSON *arr = recipes_read();
    int i = recipe_index(arr, name);
    if (i < 0 && cJSON_GetArraySize(arr) >= RECIPES_MAX) {
        cJSON_Delete(r);
        cJSON_Delete(arr);
        return send_error(req, "409 Conflict", "too many recipes");
    }
    ESP_LOGI(TAG, "recipe saved: %s (%d steps)", name, cJSON_GetArraySize(cJSON_GetObjectItem(r, "steps")));
    if (i >= 0) cJSON_ReplaceItemInArray(arr, i, r);
    else cJSON_AddItemToArray(arr, r);
    if (!recipes_write(arr)) {
        cJSON_Delete(arr);
        return send_error(req, "500 Internal Server Error", "could not write recipes");
    }
    return send_recipes(req, arr);
}

/* POST /recipes/delete?name=… */
static esp_err_t recipe_delete_handler(httpd_req_t *req)
{
    char q[RECIPE_QUERY_MAX], name[CFG_NAME_MAX + 1];
    if (!name_query(req, q, name)) return ESP_OK;
    cJSON *arr = recipes_read();
    int i = recipe_index(arr, name);
    if (i < 0) {
        cJSON_Delete(arr);
        return send_error(req, "404 Not Found", "no such recipe");
    }
    cJSON_DeleteItemFromArray(arr, i);
    if (!recipes_write(arr)) {
        cJSON_Delete(arr);
        return send_error(req, "500 Internal Server Error", "could not write recipes");
    }
    ESP_LOGI(TAG, "recipe deleted: %s", name);
    return send_recipes(req, arr);
}

/* POST /recipes/rename?name=…&to=… */
static esp_err_t recipe_rename_handler(httpd_req_t *req)
{
    char q[RECIPE_QUERY_MAX], name[CFG_NAME_MAX + 1], to[CFG_NAME_MAX + 1];
    if (!name_query(req, q, name)) return ESP_OK;
    if (!cfg_query_text(q, "to", to, sizeof(to)) || !cfg_name_ok(to)) {
        return send_error(req, "400 Bad Request", "to: " CFG_BAD_NAME);
    }
    cJSON *arr = recipes_read();
    int i = recipe_index(arr, name);
    int j = recipe_index(arr, to);
    if (i < 0 || (j >= 0 && j != i)) {
        cJSON_Delete(arr);
        return i < 0 ? send_error(req, "404 Not Found", "no such recipe")
                     : send_error(req, "409 Conflict", "a recipe with that name exists");
    }
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(arr, i), "name", cJSON_CreateString(to));
    if (!recipes_write(arr)) {
        cJSON_Delete(arr);
        return send_error(req, "500 Internal Server Error", "could not write recipes");
    }
    ESP_LOGI(TAG, "recipe renamed: %s -> %s", name, to);
    return send_recipes(req, arr);
}

esp_err_t recipe_register_http(httpd_handle_t server)
{
    static const httpd_uri_t uris[RECIPE_URI_COUNT] = {
        { .uri = "/recipes",        .method = HTTP_GET,  .handler = recipes_get_handler },
        { .uri = "/recipes/save",   .method = HTTP_POST, .handler = recipe_save_handler },
        { .uri = "/recipes/delete", .method = HTTP_POST, .handler = recipe_delete_handler },
        { .uri = "/recipes/rename", .method = HTTP_POST, .handler = recipe_rename_handler },
    };
    for (size_t i = 0; i < RECIPE_URI_COUNT; i++) {
        esp_err_t r = httpd_register_uri_handler(server, &uris[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(r));
            return r;
        }
    }
    return ESP_OK;
}
