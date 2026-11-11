/* GPL-3.0-or-later. */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *json_find(const char *js, const char *key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? p + strlen(pat) : NULL;
}

bool json_num(const char *js, const char *key, double *out) {
    const char *p = json_find(js, key);
    if (!p) return false;
    char *end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

bool json_str(const char *js, const char *key, char *out, size_t size) {
    const char *p = json_find(js, key);
    if (!p || *p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) out[n++] = *p++;
    out[n] = 0;
    return true;
}
