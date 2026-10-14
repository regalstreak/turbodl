// TV UI: raw VideoOut framebuffers + FreeType text with a glyph cache.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>
#include "common.h"
#include "sce.h"
#include <orbis/ImeDialog.h>
#include <proto-include.h>
#include "font_ttf.h"

#define W 1920
#define H 1080

// layout
#define HDR_H      118
#define LIST_TOP   132
#define ROW_H      108
#define LIST_ROWS  6
#define LIST_BOT   (LIST_TOP + ROW_H * LIST_ROWS)   // 780
#define LOG_TOP    (LIST_BOT + 14)                  // 794
#define LOG_LINE   30
#define LOG_ROWS   5
#define LOG_BOT    (LOG_TOP + 14 + LOG_LINE * LOG_ROWS)  // 958
#define FOOT_TOP   1010

typedef struct { int w, h, left, top, adv; uint8_t *bmp; } glyph_t;
enum { F_SMALL, F_MED, F_BIG, F_COUNT };
static const int s_font_px[F_COUNT] = {22, 28, 44};
static glyph_t s_gl[F_COUNT][128];
static int s_asc[F_COUNT];

typedef struct { uint32_t *pixels; int pitch; } canvas_t;
static canvas_t s_cv;
static canvas_t *s_canvas = NULL;
static int s_video = -1;
static uint32_t *s_fb[2];
static int s_fb_idx = 0;
static int s_ui_ok = 0;
static int s_pad = -1;
static int s_font_ok = 0;

// ---- raw VideoOut (SDL's PS4 port fails to allocate framebuffers here) ----
int sceVideoOutOpen(int user, int bus, int index, const void *param);
void sceVideoOutSetBufferAttribute(void *attr, uint32_t fmt, uint32_t tmode, uint32_t aspect, uint32_t w, uint32_t h, uint32_t pitch);
int sceVideoOutRegisterBuffers(int handle, int start, void *const *addrs, int num, const void *attr);
int sceVideoOutSubmitFlip(int handle, int idx, uint32_t mode, int64_t arg);
int sceVideoOutSetFlipRate(int handle, int rate);
int sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int type, off_t *out);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, off_t *out);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t *start_out, size_t *size_out);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t off, size_t align);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);

#define C_BG     0xFF0F1218
#define C_PANEL  0xFF161B24
#define C_SEL    0xFF22324D
#define C_LINE   0xFF252C38
#define C_TEXT   0xFFE6E9EF
#define C_MUTED  0xFF8B95A7
#define C_BLUE   0xFF2F6FEC
#define C_GREEN  0xFF2EA043
#define C_RED    0xFFD04545
#define C_AMBER  0xFFD29922

static int load_font(void) {
    FT_Library lib;
    FT_Face face;
    int r = FT_Init_FreeType(&lib);
    if (r) { log_msg("ui: FT_Init_FreeType %d", r); return 0; }
    // embedded font: /app0 is unreachable once the jailbreak changes our root
    r = FT_New_Memory_Face(lib, font_ttf, (long)font_ttf_len, 0, &face);
    if (r) { log_msg("ui: FT_New_Memory_Face %d", r); return 0; }
    for (int f = 0; f < F_COUNT; f++) {
        FT_Set_Pixel_Sizes(face, 0, s_font_px[f]);
        s_asc[f] = (int)(face->size->metrics.ascender >> 6);
        for (int c = 32; c < 127; c++) {
            if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER)) continue;
            FT_GlyphSlot g = face->glyph;
            glyph_t *o = &s_gl[f][c];
            o->w = (int)g->bitmap.width;
            o->h = (int)g->bitmap.rows;
            o->left = g->bitmap_left;
            o->top = g->bitmap_top;
            o->adv = (int)(g->advance.x >> 6);
            if (o->w && o->h) {
                o->bmp = malloc((size_t)(o->w * o->h));
                for (int y = 0; y < o->h; y++)
                    memcpy(o->bmp + y * o->w, g->bitmap.buffer + y * g->bitmap.pitch, (size_t)o->w);
            }
        }
    }
    return 1;
}

static inline void blend(uint32_t *px, uint32_t col, int a) {
    if (a <= 0) return;
    if (a >= 255) { *px = col; return; }
    uint32_t d = *px;
    int r = (int)(((col >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    int g = (int)(((col >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    int b = (int)((col & 255) * a + (d & 255) * (255 - a)) / 255;
    *px = 0xFF000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t *row = s_cv.pixels + yy * W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

static int text_w(int f, const char *s) {
    int w = 0;
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c < 128) w += s_gl[f][c].adv; }
    return w;
}

// y = top of the line box; clips to [ymin,ymax)
static void text_at(int x, int y, int f, uint32_t col, const char *s, int maxw, int ymin, int ymax) {
    if (!s_font_ok) return;
    uint32_t *pix = s_cv.pixels;
    int pitch = s_cv.pitch / 4;
    int base = y + s_asc[f];
    int x0 = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        glyph_t *g = &s_gl[f][c];
        if (maxw > 0 && x - x0 + g->adv > maxw) break;
        for (int gy = 0; gy < g->h; gy++) {
            int py = base - g->top + gy;
            if (py < 0 || py >= H || py < ymin || py >= ymax) continue;
            for (int gx = 0; gx < g->w; gx++) {
                int pxx = x + g->left + gx;
                if (pxx < 0 || pxx >= W) continue;
                blend(&pix[py * pitch + pxx], col, g->bmp[gy * g->w + gx]);
            }
        }
        x += g->adv;
    }
}

static void text(int x, int y, int f, uint32_t col, const char *s, int maxw) {
    text_at(x, y, f, col, s, maxw, 0, H);
}

static int video_init(void) {
    const size_t fbsize = (size_t)W * H * 4;
    const size_t align = 0x200000;
    size_t total = (fbsize * 2 + align - 1) / align * align;

    s_video = sceVideoOutOpen(0xFF, 0, 0, NULL);
    if (s_video < 0) { log_msg("ui: sceVideoOutOpen 0x%08X", s_video); return -1; }

    off_t avs = 0;
    size_t avsz = 0;
    sceKernelAvailableDirectMemorySize(0, (off_t)sceKernelGetDirectMemorySize(), align, &avs, &avsz);
    log_msg("ui: direct mem total %zu MB, available %zu MB", sceKernelGetDirectMemorySize() >> 20, avsz >> 20);

    void *mem = NULL;
    off_t off = 0;
    int r = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), total, align, 3, &off);
    if (r < 0) {
        log_msg("ui: AllocateDirectMemory 0x%08X, trying main", r);
        r = sceKernelAllocateMainDirectMemory(total, align, 0, &off);
    }
    if (r >= 0) {
        r = sceKernelMapDirectMemory(&mem, total, 0x33, 0, off, align);
        if (r < 0) { log_msg("ui: MapDirectMemory 0x%08X", r); mem = NULL; }
    } else {
        log_msg("ui: AllocateMainDirectMemory 0x%08X, trying flexible", r);
    }
    if (!mem) {
        r = sceKernelMapFlexibleMemory(&mem, total, 0x33, 0);
        if (r < 0) { log_msg("ui: MapFlexibleMemory 0x%08X - no TV UI", r); return -1; }
    }
    s_fb[0] = mem;
    s_fb[1] = (uint32_t *)((char *)mem + fbsize);
    memset(mem, 0, fbsize * 2);

    uint64_t attr[8] = {0};
    sceVideoOutSetBufferAttribute(attr, 0x80000000, 1, 0, W, H, W);
    r = sceVideoOutRegisterBuffers(s_video, 0, (void *const *)s_fb, 2, attr);
    if (r < 0) { log_msg("ui: RegisterBuffers 0x%08X", r); return -1; }
    sceVideoOutSetFlipRate(s_video, 0);
    return 0;
}

// ---------------- on-screen keyboard (system IME) ----------------
char *ui_ask_text(const char *title, const char *initial) {
    static char out[128];
    static wchar_t wbuf[128], wtitle[64], wplace[64];
    out[0] = 0;
    sceSysmoduleLoadModule(SYSMOD_IME_DIALOG);
    sceSysmoduleLoadModule(SYSMOD_LIBIME);

    memset(wbuf, 0, sizeof(wbuf));
    if (initial && *initial) {
        size_t n = strlen(initial);
        if (n > 120) n = 120;
        for (size_t i = 0; i < n; i++) wbuf[i] = (wchar_t)(unsigned char)initial[i];
    }
    const char *t = title ? title : "Search";
    size_t tn = strlen(t) > 60 ? 60 : strlen(t);
    for (size_t i = 0; i < tn; i++) wtitle[i] = (wchar_t)(unsigned char)t[i];
    wtitle[tn] = 0;
    const char *p = "type here";
    for (size_t i = 0; i < strlen(p); i++) wplace[i] = (wchar_t)p[i];

    OrbisImeDialogSetting s;
    memset(&s, 0, sizeof(s));
    s.userId = 0xFFFFFFFF;
    s.type = ORBIS_TYPE_DEFAULT;
    s.enterLabel = ORBIS_BUTTON_LABEL_SEARCH;
    s.inputMethod = ORBIS__DEFAULT;
    s.maxTextLength = 120;
    s.inputTextBuffer = wbuf;
    s.posx = 0.0f;                       // 0 with center alignment = centred panel
    s.posy = 0.0f;
    s.horizontalAlignment = ORBIS_H_CENTER;
    s.verticalAlignment = ORBIS_V_CENTER;
    s.placeholder = wplace;
    s.title = wtitle;

    int r = sceImeDialogInit(&s, NULL);
    if (r != 0) { log_msg("ui: IME not available (0x%08X)", r); return NULL; }
    // wait for the panel to come up; bail out if it never does
    int waited = 0, running = 0;
    while (waited < 3000) {
        if (sceImeDialogGetStatus() == ORBIS_DIALOG_STATUS_RUNNING) { running = 1; break; }
        sceKernelUsleep(100000);
        waited += 100;
    }
    if (!running) { log_msg("ui: IME did not open"); sceImeDialogTerm(); return NULL; }
    while (sceImeDialogGetStatus() == ORBIS_DIALOG_STATUS_RUNNING) sceKernelUsleep(100000);

    OrbisDialogResult res;
    memset(&res, 0, sizeof(res));
    int ok = sceImeDialogGetResult(&res) == 0 && res.endstatus == ORBIS_DIALOG_OK;
    sceImeDialogTerm();
    log_msg("ui: IME result %d (%d chars)", ok, ok ? (int)wcslen(wbuf) : 0);
    if (!ok) return NULL;
    size_t n = 0;
    while (n < 120 && wbuf[n]) { out[n] = (char)(wbuf[n] & 0x7F); n++; }
    out[n] = 0;
    return out;
}

int ui_init(void) {
    s_cv.pixels = malloc((size_t)W * H * 4);
    s_cv.pitch = W * 4;
    if (!s_cv.pixels) return -1;
    s_canvas = &s_cv;
    if (video_init() != 0) return -1;
    s_ui_ok = 1;
    sceSysmoduleLoadModule(SYSMOD_FREETYPE_OL);
    s_font_ok = load_font();
    if (!s_font_ok) log_msg("font load failed");

    int uid = -1;
    sceUserServiceGetInitialUser(&uid);
    scePadInit();
    s_pad = scePadOpen(uid, 0, 0, NULL);
    if (s_pad < 0) log_msg("scePadOpen failed 0x%08X", s_pad);
    return 0;
}

// ---------------- catalog ----------------
#define CAT_MAX 4000
static char *s_cat = NULL;          // raw TSV
static int s_cat_off[CAT_MAX];      // offsets into s_cat
static int s_cat_n = 0;
static int s_cat_hit[CAT_MAX];      // 1 = matches the filter
static int s_cat_shown = 0;
static char s_filter[128] = "";
static const char *s_cat_type = "GAMES";
static int s_tab = 0;               // 0 = downloads, 1 = catalog
static int s_cat_sel = 0, s_cat_scroll = 0;
static int s_sel = 0, s_scroll = 0;
static char s_note[160] = "";
static uint64_t s_note_until = 0;

static void cat_index(void) {
    s_cat_n = 0;
    if (!s_cat) return;
    char *p = s_cat;
    while (*p && s_cat_n < CAT_MAX) {
        s_cat_off[s_cat_n++] = (int)(p - s_cat);
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
}

static void cat_field(int idx, int field, char *out, size_t outn) {
    out[0] = 0;
    if (idx < 0 || idx >= s_cat_n) return;
    char *p = s_cat + s_cat_off[idx];
    for (int f = 0; f < field; f++) {
        p = strchr(p, '\t');
        if (!p) return;
        p++;
    }
    size_t n = 0;
    while (p[n] && p[n] != '\t' && p[n] != '\n' && n < outn - 1) n++;
    memcpy(out, p, n);
    out[n] = 0;
}

static int cat_matches(int idx) {
    if (!s_filter[0]) return 1;
    char name[260], tid[40];
    cat_field(idx, 0, name, sizeof(name));
    cat_field(idx, 1, tid, sizeof(tid));
    char nl[200], tl[32];
    size_t n = strlen(name) < sizeof(nl) - 1 ? strlen(name) : sizeof(nl) - 1;
    for (size_t i = 0; i < n; i++) nl[i] = (char)tolower((unsigned char)name[i]);
    nl[n] = 0;
    n = strlen(tid) < sizeof(tl) - 1 ? strlen(tid) : sizeof(tl) - 1;
    for (size_t i = 0; i < n; i++) tl[i] = (char)tolower((unsigned char)tid[i]);
    tl[n] = 0;
    return strstr(nl, s_filter) != NULL || strstr(tl, s_filter) != NULL;
}

static void cat_apply_filter(void) {
    char lf[128];
    size_t n = strlen(s_filter) < sizeof(lf) - 1 ? strlen(s_filter) : sizeof(lf) - 1;
    for (size_t i = 0; i < n; i++) lf[i] = (char)tolower((unsigned char)s_filter[i]);
    lf[n] = 0;
    memcpy(s_filter, lf, n + 1);
    s_cat_shown = 0;
    for (int i = 0; i < s_cat_n; i++) {
        s_cat_hit[i] = s_cat_n && s_cat && cat_matches(i);
        if (s_cat_hit[i]) s_cat_shown++;
    }
    s_cat_sel = 0;
    s_cat_scroll = 0;
}

static void cat_load(void) {
    static char path[128];
    snprintf(path, sizeof(path), "/api/catalog.tsv?t=%s", s_cat_type);
    free(s_cat);
    s_cat = malloc(512 * 1024);
    s_cat_n = 0;
    if (!s_cat) return;
    int st = remote_get(path, s_cat, 512 * 1024);
    if (st != 200) {
        free(s_cat);
        s_cat = NULL;
        log_msg("catalog: no list (%d) for %s", st, s_cat_type);
        return;
    }
    cat_index();
    cat_apply_filter();
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: %d entries", s_cat_type, s_cat_n);
    log_msg("catalog: %s", msg);
}

// index (into s_cat_hit order) of the nth shown entry
static int cat_shown_sel(int sel) {
    int k = 0;
    for (int i = 0; i < s_cat_n; i++) {
        if (!s_cat_hit[i]) continue;
        if (k == sel) return i;
        k++;
    }
    return -1;
}

// ---------------- input ----------------
static uint32_t s_prev = 0;
static uint64_t s_rep_t = 0;
static int s_confirm_del = -1;
static uint64_t s_confirm_until = 0;
static int s_ime_running = 0;

static void set_note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_note, sizeof(s_note), fmt, ap);
    va_end(ap);
    s_note_until = now_ms() + 4000;
}

static int job_ids(int *ids) {
    int n = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (g_jobs[i].used) ids[n++] = i;
    return n;
}

static void input(void) {
    if (s_pad < 0 || s_ime_running) return;
    PadData pd;
    memset(&pd, 0, sizeof(pd));
    if (scePadReadState(s_pad, &pd) != 0) return;
    uint32_t b = pd.buttons;
    uint32_t pressed = b & ~s_prev;
    uint64_t t = now_ms();
    if (b & (PAD_UP | PAD_DOWN)) {
        if (pressed & (PAD_UP | PAD_DOWN)) s_rep_t = t + 400;
        else if (t >= s_rep_t) { pressed |= b & (PAD_UP | PAD_DOWN); s_rep_t = t + 90; }
    }
    s_prev = b;
    if (!pressed) return;

    int ids[MAX_JOBS];
    int n = job_ids(ids);

    if (pressed & PAD_OPTIONS) {   // switch tab
        s_tab = !s_tab;
        if (s_tab == 1 && !s_cat) cat_load();
        return;
    }
    if (pressed & PAD_L2 && g_max_parallel > 1) ui_set_parallel(g_max_parallel - 1);
    if (pressed & PAD_R2 && g_max_parallel < MAX_PARALLEL) ui_set_parallel(g_max_parallel + 1);
    if (pressed & PAD_L1 && g_conns > 1) ui_set_conns(g_conns - 1);
    if (pressed & PAD_R1 && g_conns < MAX_CONNS) ui_set_conns(g_conns + 1);

    if (s_tab == 0) {
        if (pressed & PAD_UP) s_sel--;
        if (pressed & PAD_DOWN) s_sel++;
        if (s_sel >= n) s_sel = n - 1;
        if (s_sel < 0) s_sel = 0;
        if (!n) return;
        job_t *j = &g_jobs[ids[s_sel]];
        if (pressed & PAD_CROSS) ui_job_action(j->id, "toggle");
        if (pressed & PAD_TRIANGLE) ui_job_action(j->id, "install");
        if (pressed & PAD_SQUARE) {
            if (s_confirm_del == j->id && t < s_confirm_until) {
                ui_job_action(j->id, "delete");
                s_confirm_del = -1;
            } else {
                s_confirm_del = j->id;
                s_confirm_until = t + 3000;
            }
        }
        return;
    }

    // catalog tab
    if (pressed & PAD_UP) s_cat_sel--;
    if (pressed & PAD_DOWN) s_cat_sel++;
    if (pressed & PAD_LEFT) s_cat_sel -= LIST_ROWS;
    if (pressed & PAD_RIGHT) s_cat_sel += LIST_ROWS;
    if (pressed & PAD_SQUARE) {     // search with the system keyboard
        s_ime_running = 1;
        char *q = ui_ask_text("Search catalog", s_filter);
        s_ime_running = 0;
        if (q) {
            snprintf(s_filter, sizeof(s_filter), "%s", q);
            cat_apply_filter();
            set_note("Filter: %s (%d matches)", s_filter[0] ? s_filter : "none", s_cat_shown);
        }
    }
    if (pressed & PAD_TRIANGLE) {   // clear filter
        s_filter[0] = 0;
        cat_apply_filter();
    }
    if (pressed & PAD_L3) {         // reload list from disk
        cat_load();
        set_note("Reloaded %s: %d entries", s_cat_type, s_cat_n);
    }
    if (s_cat_sel >= s_cat_shown) s_cat_sel = s_cat_shown - 1;
    if (s_cat_sel < 0) s_cat_sel = 0;
    if (s_cat_sel < s_cat_scroll) s_cat_scroll = s_cat_sel;
    if (s_cat_sel >= s_cat_scroll + LIST_ROWS) s_cat_scroll = s_cat_sel - LIST_ROWS + 1;
    if (pressed & PAD_CROSS) {
        int idx = cat_shown_sel(s_cat_sel);
        if (idx >= 0) {
            char url[600], name[260];
            cat_field(idx, 5, url, sizeof(url));
            cat_field(idx, 0, name, sizeof(name));
            if (url[0]) {
                ui_add_url(url);
                set_note("Queued: %s", name);
                log_msg("catalog: queued %s (%s)", name, url);
            } else set_note("No link for that entry");
        }
    }
}

// ---------------- drawing ----------------
static void draw_header(void) {
    rect(0, 0, W, HDR_H, C_PANEL);
    rect(0, HDR_H, W, 2, C_LINE);
    text(60, 18, F_BIG, C_TEXT, APP_NAME, 0);

    // web panel url, right aligned
    char b[256];
    snprintf(b, sizeof(b), "http://%s:%d", g_ip[0] ? g_ip : "?", WEB_PORT);
    text(W - 60 - text_w(F_MED, b), 14, F_MED, C_BLUE, b, 0);

    // second line: mode summary right aligned, transient status message clipped on the left
    char a1[32], a2[32], mode[360];
    uint64_t total = 0;
    for (int i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].used && g_jobs[i].state == J_DOWNLOADING) total += g_jobs[i].speed;
    fmt_bytes(a1, sizeof(a1), total);
    fmt_bytes(a2, sizeof(a2), disk_free(APP_DIR));
    snprintf(mode, sizeof(mode), "%s    %s/s    free %s    %dx%d conns    cookies %s    jb %s",
             g_remote ? "BACKGROUND MODE" : "in-app mode", a1, a2, g_max_parallel, g_conns,
             g_cookie[0] ? "yes" : "-", g_jb_ok ? "yes" : "NO");
    int mw = text_w(F_SMALL, mode);
    int mx = W - 60 - mw;
    if (mx < 60) mx = 60;
    text(mx, 76, F_SMALL, C_MUTED, mode, 0);

    int avail = mx - 60 - 30;   // room left of the right-aligned text
    if (g_loading) {
        static const char spin[] = "|/-\\";
        char sp[4] = {spin[(now_ms() / 150) % 4], 0, 0, 0};
        text(60, 76, F_SMALL, C_AMBER, sp, 0);
        if (avail > 60) text(76, 76, F_SMALL, C_AMBER, g_status, avail - 16);
    } else if (g_status[0] && strcmp(g_status, "Ready") != 0 && avail > 60) {
        text(60, 76, F_SMALL, C_AMBER, g_status, avail);
    }
}

static void progress_bar(int x, int y, int w, int h, double pct, uint32_t col) {
    rect(x, y, w, h, C_LINE);
    int fill = (int)(w * pct / 100.0);
    if (fill > 0) rect(x, y, fill, h, col);
}

static void draw_jobs(void) {
    char b[512], a1[32], a2[32], a3[32];
    int ids[MAX_JOBS];
    int n = job_ids(ids);
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < 0) s_sel = 0;
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + LIST_ROWS) s_scroll = s_sel - LIST_ROWS + 1;

    if (!n) {
        text(60, LIST_TOP + 20, F_MED, C_TEXT, "No downloads yet.", 0);
        snprintf(b, sizeof(b), "Add links from your phone or PC at http://%s:%d", g_ip, WEB_PORT);
        text(60, LIST_TOP + 70, F_SMALL, C_MUTED, b, 0);
        text(60, LIST_TOP + 110, F_SMALL, C_MUTED, "or press OPTIONS to browse the catalog on the console.", 0);
        return;
    }
    for (int r = 0; r < LIST_ROWS && s_scroll + r < n; r++) {
        int k = s_scroll + r;
        job_t *j = &g_jobs[ids[k]];
        int y = LIST_TOP + r * ROW_H;
        rect(40, y, W - 80, ROW_H - 12, k == s_sel ? C_SEL : C_PANEL);
        if (k == s_sel) rect(40, y, 6, ROW_H - 12, C_BLUE);
        text(70, y + 8, F_MED, C_TEXT, j->name, W - 620);

        uint32_t sc = C_MUTED;
        if (j->state == J_ERROR) sc = C_RED;
        else if (j->state == J_INSTALLED || j->state == J_DONE) sc = C_GREEN;
        else if (j->state == J_INSTALLING) sc = C_AMBER;
        else if (j->state == J_DOWNLOADING) sc = C_BLUE;
        const char *ss = state_str(j->state);
        text(W - 80 - text_w(F_MED, ss), y + 8, F_MED, sc, ss, 0);

        double pct = j->size ? (double)j->done * 100.0 / (double)j->size : 0;
        if (pct > 100) pct = 100;
        fmt_bytes(a1, sizeof(a1), j->done);
        fmt_bytes(a2, sizeof(a2), j->size);
        if (j->state == J_DOWNLOADING) {
            fmt_bytes(a3, sizeof(a3), j->speed);
            uint64_t eta = j->speed ? (j->size - j->done) / j->speed : 0;
            snprintf(b, sizeof(b), "%s / %s   %.1f%%   %s/s   ETA %lluh %02llum", a1, a2, pct, a3,
                     (unsigned long long)(eta / 3600), (unsigned long long)(eta % 3600 / 60));
        } else if (j->state == J_INSTALLING) {
            snprintf(b, sizeof(b), "Installing... %d%%", j->install_pct);
            pct = j->install_pct;
        } else if (j->state == J_ERROR) {
            snprintf(b, sizeof(b), "%s", j->err);
        } else {
            snprintf(b, sizeof(b), "%s / %s   %.1f%%", a1, j->size ? a2 : "?", pct);
        }
        text(70, y + 48, F_SMALL, j->state == J_ERROR ? C_RED : C_MUTED, b, W - 200);
        progress_bar(70, y + 84, W - 140, 10, pct, sc == C_MUTED ? C_BLUE : sc);
    }
    if (n > LIST_ROWS) {
        snprintf(b, sizeof(b), "%d-%d of %d", s_scroll + 1, s_scroll + LIST_ROWS < n ? s_scroll + LIST_ROWS : n, n);
        text(W - 60 - text_w(F_SMALL, b), LIST_BOT - 26, F_SMALL, C_MUTED, b, 0);
    }
}

static void draw_catalog(void) {
    char b[512];
    if (!s_cat) {
        text(60, LIST_TOP + 20, F_MED, C_TEXT, "No catalog on the console.", 0);
        text(60, LIST_TOP + 66, F_SMALL, C_MUTED,
             "Copy a list file (FPKGi JSON) to:", 0);
        text(60, LIST_TOP + 100, F_SMALL, C_BLUE, APP_DIR "/catalogs/GAMES.json", 0);
        text(60, LIST_TOP + 140, F_SMALL, C_MUTED, "Then press L3 to reload. No lists are bundled with TurboDL.", 0);
        return;
    }
    if (!s_cat_shown) {
        text(60, LIST_TOP + 20, F_MED, C_TEXT, "No matches.", 0);
        snprintf(b, sizeof(b), "Filter \"%s\" - press TRIANGLE to clear it", s_filter);
        text(60, LIST_TOP + 66, F_SMALL, C_MUTED, b, 0);
        return;
    }
    for (int r = 0; r < LIST_ROWS; r++) {
        int sel = s_cat_scroll + r;
        if (sel >= s_cat_shown) break;
        int idx = cat_shown_sel(sel);
        if (idx < 0) break;
        int y = LIST_TOP + r * ROW_H;
        rect(40, y, W - 80, ROW_H - 12, sel == s_cat_sel ? C_SEL : C_PANEL);
        if (sel == s_cat_sel) rect(40, y, 6, ROW_H - 12, C_BLUE);
        char nm[260], tid[40], sz[32], rg[16], vr[16], szbuf[32];
        cat_field(idx, 0, nm, sizeof(nm));
        cat_field(idx, 1, tid, sizeof(tid));
        cat_field(idx, 2, sz, sizeof(sz));
        cat_field(idx, 3, rg, sizeof(rg));
        cat_field(idx, 4, vr, sizeof(vr));
        if (sz[0]) fmt_bytes(szbuf, sizeof(szbuf), strtoull(sz, NULL, 10));
        else snprintf(szbuf, sizeof(szbuf), "size unknown");
        text(70, y + 24, F_MED, C_TEXT, nm, W - 700);
        snprintf(b, sizeof(b), "%s%s%s   %s", tid, rg[0] ? "   " : "", rg, szbuf);
        text(70, y + 62, F_SMALL, C_MUTED, b, W - 200);
    }
    snprintf(b, sizeof(b), "%s: %d of %d entries%s%s", s_cat_type, s_cat_sel + 1, s_cat_shown,
             s_filter[0] ? "   filter: " : "", s_filter);
    text(W - 60 - text_w(F_SMALL, b), LIST_BOT - 26, F_SMALL, C_MUTED, b, 0);
}

static void draw_log(void) {
    rect(40, LOG_TOP, W - 80, 1, C_LINE);
    char lines[LOG_ROWS][160];
    int ln = g_remote ? remote_log_lines(lines, LOG_ROWS) : log_lines(lines, LOG_ROWS);
    int inner_top = LOG_TOP + 16;
    for (int i = 0; i < ln && i < LOG_ROWS; i++)
        text_at(60, inner_top + i * LOG_LINE, F_SMALL, C_MUTED, lines[i], W - 120, inner_top, LOG_BOT);
}

static void draw_footer(void) {
    rect(0, FOOT_TOP, W, H - FOOT_TOP, C_PANEL);
    uint64_t t = now_ms();
    const char *help;
    if (s_note[0] && t < s_note_until) {
        text(60, FOOT_TOP + 14, F_SMALL, C_AMBER, s_note, W - 120);
        return;
    }
    if (s_tab == 1)
        help = "X queue download   SQUARE search   TRIANGLE clear filter   L3 reload list   ARROWS move";
    else if (s_confirm_del >= 0 && t < s_confirm_until)
        help = "Press SQUARE again to remove this item and delete its file";
    else
        help = "X pause/resume/retry   TRIANGLE install   SQUARE remove   L1/R1 connections   L2/R2 parallel";
    const char *tabhint = s_tab ? "OPTIONS: downloads" : "OPTIONS: catalog";
    int avail = W - 120 - text_w(F_SMALL, tabhint) - 30;
    text(60, FOOT_TOP + 14, F_SMALL, (s_confirm_del >= 0 && t < s_confirm_until) ? C_AMBER : C_TEXT, help, avail);
    text(W - 60 - text_w(F_SMALL, tabhint), FOOT_TOP + 14, F_SMALL, C_BLUE, tabhint, 0);
}

static void draw(void) {
    rect(0, 0, W, H, C_BG);
    draw_header();
    if (s_tab == 0) draw_jobs(); else draw_catalog();
    draw_log();
    draw_footer();
}

void ui_run(void) {
    if (!s_ui_ok) return;
    int64_t frame = 0;
    for (;;) {
        input();
        if (!s_ime_running) {
            draw();
            memcpy(s_fb[s_fb_idx], s_cv.pixels, (size_t)W * H * 4);
            sceVideoOutSubmitFlip(s_video, s_fb_idx, 1, ++frame);
            s_fb_idx ^= 1;
        }
        sceKernelUsleep(50000);
    }
}
