// Minimal reader for FPKGi-format list files: {"DATA":{"<url>":{"name":..,"title_id":..,"size":..,"region":..,"version":..}}}
// Produces "name<TAB>title_id<TAB>size<TAB>region<TAB>version<TAB>url" lines for the TV UI.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "common.h"

static const char *skip_ws(const char *p) { while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++; return p; }

// reads a JSON string starting at '"' ; returns pointer after it, copies unescaped text
static const char *json_str(const char *p, char *out, size_t n) {
    if (*p != '"') { if (out && n) out[0] = 0; return p; }
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            c = e == 'n' ? ' ' : e == 't' ? ' ' : e; // keep it simple: no control chars in these files
        }
        if (out && o + 1 < n) out[o++] = c;
    }
    if (out && n) out[o] = 0;
    if (*p == '"') p++;
    return p;
}

// find "key" inside the object at p (bounded), copy its value into out
static int obj_field(const char *p, const char *end, const char *key, char *out, size_t n) {
    out[0] = 0;
    for (const char *s = p; s < end; s++) {
        if (*s != '"') continue;
        char k[64];
        const char *after = json_str(s, k, sizeof(k));
        if (!strcmp(k, key)) {
            const char *v = skip_ws(after);
            if (*v == ':') {
                v = skip_ws(v + 1);
                if (*v == '"') json_str(v, out, n);
                else {
                    size_t o = 0;
                    while (v[o] && v[o] != ',' && v[o] != '}' && v[o] != '\n' && o + 1 < n) { out[o] = v[o]; o++; }
                    out[o] = 0;
                }
                return 1;
            }
        }
        s = after - 1;
    }
    return 0;
}

int catalog_tsv(const char *json, char *out, size_t outn) {
    const char *d = strstr(json, "\"DATA\"");
    if (!d) return -1;
    const char *p = strchr(d, '{');
    if (!p) return -1;
    p++;
    size_t o = 0;
    while (*p && o + 700 < outn) {
        p = skip_ws(p);
        if (*p == '}' || !*p) break;
        if (*p != '"') { p++; continue; }
        char url[600];
        p = json_str(p, url, sizeof(url));
        p = skip_ws(p);
        if (*p != ':') continue;
        p = skip_ws(p + 1);
        if (*p != '{') continue;
        // find the end of this object (no nested objects in these files)
        const char *end = strchr(p, '}');
        if (!end) break;
        char name[200] = "", tid[32] = "", size[24] = "", region[16] = "", ver[16] = "";
        obj_field(p, end, "name", name, sizeof(name));
        obj_field(p, end, "title_id", tid, sizeof(tid));
        obj_field(p, end, "size", size, sizeof(size));
        obj_field(p, end, "region", region, sizeof(region));
        obj_field(p, end, "version", ver, sizeof(ver));
        if (name[0] || url[0]) {
            o += (size_t)snprintf(out + o, outn - o, "%s\t%s\t%s\t%s\t%s\t%s\n",
                                  name, tid, size, region, ver, url);
        }
        p = end + 1;
    }
    return (int)o;
}
