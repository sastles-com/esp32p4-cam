/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cfgstore: the settings file system (LittleFS at CFG_BASE) and the name rules
 * that camctl's presets and recipe.c's recipes share. Private to camcore;
 * camctl.c owns the mount and implements these.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#define CFG_BASE            "/cfg"

/* A preset or recipe name: 1..CFG_NAME_MAX bytes of UTF-8 (16 kanji fit). */
#define CFG_NAME_MAX        48
#define CFG_BAD_NAME        "name must be 1-48 bytes of text, without \\\" or \\\\"

/* True once the file system is mounted (camctl_start / camctl_saved_orientation). */
bool cfg_fs_ok(void);

/* The whole file, parsed. NULL if missing, bigger than max bytes, or not JSON. */
cJSON *cfg_read_json(const char *path, size_t max);

/* Write text to tmp, then rename over path (atomic on LittleFS). */
bool cfg_write_file(const char *tmp, const char *path, const char *text);

/* 1..CFG_NAME_MAX bytes of valid UTF-8, no control characters, no " or \
 * (so it is safe inside any JSON we print by hand). */
bool cfg_name_ok(const char *s);

/* Query value of `key`, %XX- and +-decoded, into out (size outsz). False if
 * absent or too long. Sized for names (up to 3 * CFG_NAME_MAX encoded). */
bool cfg_query_text(const char *q, const char *key, char *out, size_t outsz);
