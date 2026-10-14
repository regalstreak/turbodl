#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "common.h"
#include "sce.h"

#define NET_POOL   (1 * 1024 * 1024)
#define SSL_POOL   (4 * 1024 * 1024)
#define HTTP_POOL  (4 * 1024 * 1024)
#define UA "Mozilla/5.0 (PlayStation 4; TurboDL 1.0)"

static int s_http = -1;

int http_global_init(void) {
    int ret;
    sceNetInit();
    int pool = sceNetPoolCreate("turbodl_net", NET_POOL, 0);
    if (pool < 0) { log_msg("sceNetPoolCreate 0x%08X", pool); return -1; }
    int ssl = sceSslInit(SSL_POOL);
    if (ssl < 0) { log_msg("sceSslInit 0x%08X", ssl); return -1; }
    ret = sceHttpInit(pool, ssl, HTTP_POOL);
    if (ret < 0) { log_msg("sceHttpInit 0x%08X", ret); return -1; }
    s_http = ret;
    return 0;
}

static int skip_ssl(int a, unsigned int b, void *const c[], int d, void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return 1;
}

int http_ctx_init(hctx_t *h) {
    memset(h, 0, sizeof(*h));
    h->tmpl = sceHttpCreateTemplate(s_http, UA, HTTP_VER_1_1, 1);
    if (h->tmpl < 0) { h->last_err = h->tmpl; return -1; }
    sceHttpsSetSslCallback(h->tmpl, skip_ssl, NULL);
    sceHttpsDisableOption(h->tmpl, 0x01); // don't verify server cert (old PS4 CA store)
    sceHttpSetAutoRedirect(h->tmpl, 0);
    sceHttpSetConnectTimeOut(h->tmpl, 20 * 1000000);
    sceHttpSetResolveTimeOut(h->tmpl, 20 * 1000000);
    sceHttpSetSendTimeOut(h->tmpl, 30 * 1000000);
    sceHttpSetRecvTimeOut(h->tmpl, 30 * 1000000);
    return 0;
}

void http_close(hctx_t *h, int keep_conn) {
    if (h->req > 0) { sceHttpDeleteRequest(h->req); h->req = 0; }
    if (!keep_conn && h->conn > 0) { sceHttpDeleteConnection(h->conn); h->conn = 0; h->host[0] = 0; }
}

void http_ctx_free(hctx_t *h) {
    http_close(h, 0);
    if (h->tmpl > 0) sceHttpDeleteTemplate(h->tmpl);
    h->tmpl = 0;
}

// "https://Host:port/path" -> "https://host:port"
static void origin_of(const char *url, char *out, size_t n) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *e = strchr(p, '/');
    size_t len = e ? (size_t)(e - url) : strlen(url);
    if (len >= n) len = n - 1;
    memcpy(out, url, len);
    out[len] = 0;
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
}



static int get_header(int req, const char *name, char *out, size_t n) {
    char *hdr = NULL;
    size_t sz = 0;
    out[0] = 0;
    if (sceHttpGetAllResponseHeaders(req, &hdr, &sz) < 0 || !hdr) return 0;
    size_t nl = strlen(name);
    const char *p = hdr, *end = hdr + sz;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char *ve = eol;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ')) ve--;
            size_t len = (size_t)(ve - v);
            if (len >= n) len = n - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return 1;
        }
        p = eol + 1;
    }
    return 0;
}

// resolve Location against current url
static void resolve_url(const char *cur, const char *loc, char *out, size_t n) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
        snprintf(out, n, "%s", loc);
    } else if (loc[0] == '/' && loc[1] == '/') {
        const char *s = strstr(cur, "://");
        int sl = s ? (int)(s - cur) : 5;
        snprintf(out, n, "%.*s:%s", sl, cur, loc);
    } else if (loc[0] == '/') {
        char o[512];
        origin_of(cur, o, sizeof(o));
        snprintf(out, n, "%s%s", o, loc);
    } else {
        char base[2048];
        snprintf(base, sizeof(base), "%s", cur);
        char *q = strchr(base, '?');
        if (q) *q = 0;
        char *sl = strrchr(base, '/');
        if (sl) sl[1] = 0;
        snprintf(out, n, "%s%s", base, loc);
    }
}

// Opens a GET (optionally ranged) following redirects manually, re-evaluating the
// per-site cookie on every hop (e.g. login site -> storage/CDN subdomain). Returns 0 on success with h->req open.
// from < 0 => no Range header. to < 0 => open-ended range.
int http_open(hctx_t *h, const char *url0, int64_t from, int64_t to,
              int *status, uint64_t *clen, uint64_t *total, char *final_url, size_t flen) {
    char url[2048], next[2048];
    snprintf(url, sizeof(url), "%s", url0);
    *status = 0;
    if (clen) *clen = 0;
    if (total) *total = 0;
    http_close(h, 1);

    for (int hop = 0; hop < 10; hop++) {
        *status = 0;
        char origin[512];
        origin_of(url, origin, sizeof(origin));
        if (h->conn <= 0 || strcmp(origin, h->host)) {
            if (h->conn > 0) sceHttpDeleteConnection(h->conn);
            h->conn = sceHttpCreateConnectionWithURL(h->tmpl, url, true);
            if (h->conn < 0) { h->last_err = h->conn; h->conn = 0; h->host[0] = 0; return -1; }
            snprintf(h->host, sizeof(h->host), "%s", origin);
        }
        int req = sceHttpCreateRequestWithURL(h->conn, HTTP_GET, url, 0);
        if (req < 0) { h->last_err = req; http_close(h, 0); return -1; }
        h->req = req;
        sceHttpSetAutoRedirect(req, 0);
        if (from >= 0) {
            char r[80];
            if (to >= 0) snprintf(r, sizeof(r), "bytes=%lld-%lld", (long long)from, (long long)to);
            else snprintf(r, sizeof(r), "bytes=%lld-", (long long)from);
            sceHttpAddRequestHeader(req, "Range", r, HDR_OVERWRITE);
        }
        {
            char host[256], ck[4096];
            hostname_of(url, host, sizeof(host));
            if (cookie_lookup(host, ck, sizeof(ck))) sceHttpAddRequestHeader(req, "Cookie", ck, HDR_OVERWRITE);
        }

        int ret = sceHttpSendRequest(req, NULL, 0);
        if (ret < 0) { h->last_err = ret; http_close(h, 0); return -1; }
        int st = 0;
        sceHttpGetStatusCode(req, &st);
        *status = st;
        if (st >= 300 && st < 400 && st != 304) {
            char loc[2048];
            get_header(req, "Location", loc, sizeof(loc));
            http_close(h, 0); // body not drained; drop the connection
            if (!loc[0]) return -1;
            resolve_url(url, loc, next, sizeof(next));
            snprintf(url, sizeof(url), "%s", next);
            continue;
        }
        int t = 0;
        uint64_t l = 0;
        if (clen && sceHttpGetResponseContentLength(req, &t, &l) == 0 && t == 0) *clen = l;
        if (total) {
            char cr[160];
            if (get_header(req, "Content-Range", cr, sizeof(cr))) {
                char *sl = strrchr(cr, '/');
                if (sl && sl[1] != '*') *total = strtoull(sl + 1, NULL, 10);
            }
        }
        if (final_url) snprintf(final_url, flen, "%s", url);
        return 0;
    }
    return -1;
}

int http_read(hctx_t *h, void *buf, uint32_t n) {
    if (h->req <= 0) return -1;
    return sceHttpReadData(h->req, buf, n);
}
