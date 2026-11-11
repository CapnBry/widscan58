/* Just enough JSON for PROFILE and STATUS: values by "key":, first match.
 * GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Pointer to the value after "key":, or NULL. */
const char *json_find(const char *js, const char *key);
bool json_num(const char *js, const char *key, double *out);
bool json_str(const char *js, const char *key, char *out, size_t size);
