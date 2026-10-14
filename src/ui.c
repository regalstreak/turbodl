// TV UI: raw VideoOut framebuffers + FreeType text with a glyph cache.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <proto-include.h>
#include "common.h"
#include "sce.h"
#include "font_ttf.h"

#define W 1920
#define H 1080

int job_action(int id, const char *op);

typedef struct { int w, h, left, top, adv; uint8_t *bmp; } glyph_t;
enum { F_SMALL, F_MED, F_BIG, F_COUNT };
static const int s_font_px[F_COUNT] = {22, 28, 44};
static glyph_t s_gl[F_COUNT][128];
static int s_asc[F_COUNT];

// ---- raw VideoOut (SDL's PS4 port failed with "Failed to AllocFramebuffers") ----
int sceVideoOutOpen(int user, int bus, int index, const void *param);
void sceVideoOutSetBufferAttribute(void *attr, uint32_t fmt, uint32_t tmode, uint32_t aspect, uint32_t w, uint32_t h, uint32_t pitch);
int sceVideoOutRegisterBuffers(int handle, int start, void *const *addrs, int num, const void *attr);
int sceVideoOutSubmitFlip(int handle, int idx, uint32_t mode, int64_t arg);
int sceVideoOutSetFlipRate(int handle, int rate);
int sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int type, off_t *out);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, off_t *out);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t *out_start, size_t *out_size);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t off, size_t align);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);

typedef struct { uint32_t *pixels; int pitch; } canvas_t; // pitch in bytes
static canvas_t s_cv;
static canvas_t *s_canvas = NULL;
static int s_video = -1;
static uint32_t *s_fb[2];
static int s_fb_idx = 0;
static int s_ui_ok = 0;
static int s_pad = -1;
static int s_font_ok = 0;

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
    // font is embedded: /app0 isn't reachable after the GoldHEN jailbreak changes our root
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

// y = top of the line box
static int text(int x, int y, int f, uint32_t col, const char *s, int maxw) {
    if (!s_font_ok) return x;
    uint32_t *pix = s_canvas->pixels;
    int pitch = s_canvas->pitch / 4;
    int base = y + s_asc[f];
    int x0 = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        glyph_t *g = &s_gl[f][c];
        if (maxw > 0 && x - x0 + g->adv > maxw) break;
        for (int gy = 0; gy < g->h; gy++) {
            int py = base - g->top + gy;
            if (py < 0 || py >= H) continue;
            for (int gx = 0; gx < g->w; gx++) {
                int pxx = x + g->left + gx;
                if (pxx < 0 || pxx >= W) continue;
                blend(&pix[py * pitch + pxx], col, g->bmp[gy * g->w + gx]);
            }
        }
        x += g->adv;
    }
    return x;
}

static int video_init(void) {
    const size_t fbsize = (size_t)W * H * 4;
    const size_t align = 0x200000;
    size_t total = (fbsize * 2 + align - 1) / align * align;

    s_video = sceVideoOutOpen(0xFF, 0, 0, NULL);
    if (s_video < 0) { log_msg("ui: sceVideoOutOpen 0x%08X", s_video); return -1; }

    off_t avs = 0; size_t avsz = 0;
    sceKernelAvailableDirectMemorySize(0, (off_t)sceKernelGetDirectMemorySize(), align, &avs, &avsz);
    log_msg("ui: direct mem total %zu MB, available %zu MB", sceKernelGetDirectMemorySize() >> 20, avsz >> 20);

    void *mem = NULL;
    off_t off = 0;
    int r = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), total, align, 3, &off);
    if (r < 0) {
        log_msg("ui: AllocateDirectMemory(WC_GARLIC) 0x%08X, trying main/WB", r);
        r = sceKernelAllocateMainDirectMemory(total, align, 0, &off);
    }
    if (r >= 0) {
        r = sceKernelMapDirectMemory(&mem, total, 0x33, 0, off, align);
        if (r < 0) { log_msg("ui: MapDirectMemory 0x%08X", r); mem = NULL; }
    } else {
        log_msg("ui: AllocateMainDirectMemory 0x%08X, trying flexible memory", r);
    }
    if (!mem) {
        r = sceKernelMapFlexibleMemory(&mem, total, 0x33, 0);
        if (r < 0) { log_msg("ui: MapFlexibleMemory 0x%08X - giving up on TV UI", r); return -1; }
    }
    s_fb[0] = mem;
    s_fb[1] = (uint32_t *)((char *)mem + fbsize);
    memset(mem, 0, fbsize * 2);

    uint64_t attr[8] = {0}; // OrbisVideoOutBufferAttribute (40 bytes)
    sceVideoOutSetBufferAttribute(attr, 0x80000000 /* A8R8G8B8 sRGB */, 1 /* linear */, 0, W, H, W);
    r = sceVideoOutRegisterBuffers(s_video, 0, (void *const *)s_fb, 2, attr);
    if (r < 0) { log_msg("ui: RegisterBuffers 0x%08X", r); return -1; }
    sceVideoOutSetFlipRate(s_video, 0);
    return 0;
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

static int s_sel = 0, s_scroll = 0;
static int s_confirm_del = -1;
static uint64_t s_confirm_until = 0;

static void draw(void) {
    char b[512], a1[32], a2[32], a3[32];
    rect(0, 0, W, H, C_BG);

    // header
    rect(0, 0, W, 110, C_PANEL);
    rect(0, 110, W, 2, C_LINE);
    text(60, 22, F_BIG, C_TEXT, APP_NAME, 0);
    snprintf(b, sizeof(b), "Web panel:  http://%s:%d", g_ip[0] ? g_ip : "?", WEB_PORT);
    int hx = W - 60 - text_w(F_MED, b);
    text(hx, 18, F_MED, C_BLUE, b, 0);
    fmt_bytes(a1, sizeof(a1), disk_free(APP_DIR));
    snprintf(b, sizeof(b), "%s   free %s   conns %d   cookie %s   jailbreak %s   net %s",
             g_remote ? "BACKGROUND MODE (keeps downloading in games)" : "in-app mode",
             a1, g_conns, g_cookie[0] ? "yes" : "NO", g_jb_ok ? "yes" : "NO", g_net_ok ? "ok" : "FAIL");
    text(W - 60 - text_w(F_SMALL, b), 64, F_SMALL, C_MUTED, b, 0);

    // jobs
    int ids[MAX_JOBS], n = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (g_jobs[i].used) ids[n++] = i;
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < 0) s_sel = 0;
    const int top = 140, rowh = 118, vis = 6;
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + vis) s_scroll = s_sel - vis + 1;

    if (!n) {
        text(60, top + 20, F_MED, C_TEXT, "Queue is empty.", 0);
        snprintf(b, sizeof(b), "Open  http://%s:%d  on your phone or PC to add links / browse the catalog.", g_ip, WEB_PORT);
        text(60, top + 70, F_SMALL, C_MUTED, b, 0);
    }
    for (int r = 0; r < vis && s_scroll + r < n; r++) {
        int k = s_scroll + r;
        job_t *j = &g_jobs[ids[k]];
        int y = top + r * rowh;
        rect(40, y, W - 80, rowh - 12, k == s_sel ? C_SEL : C_PANEL);
        if (k == s_sel) rect(40, y, 6, rowh - 12, C_BLUE);
        text(70, y + 10, F_MED, C_TEXT, j->name, W - 560);

        uint32_t sc = C_MUTED;
        if (j->state == J_ERROR) sc = C_RED;
        else if (j->state == J_INSTALLED || j->state == J_DONE) sc = C_GREEN;
        else if (j->state == J_INSTALLING) sc = C_AMBER;
        else if (j->state == J_DOWNLOADING) sc = C_BLUE;
        const char *ss = state_str(j->state);
        text(W - 80 - text_w(F_MED, ss), y + 10, F_MED, sc, ss, 0);

        double pct = j->size ? (double)j->done * 100.0 / (double)j->size : 0;
        if (pct > 100) pct = 100;
        fmt_bytes(a1, sizeof(a1), j->done);
        fmt_bytes(a2, sizeof(a2), j->size);
        if (j->state == J_DOWNLOADING) {
            fmt_bytes(a3, sizeof(a3), j->speed);
            uint64_t eta = j->speed ? (j->size - j->done) / j->speed : 0;
            snprintf(b, sizeof(b), "%s / %s   %.1f%%   %s/s   ETA %lluh %02llum %02llus", a1, a2, pct, a3,
                     (unsigned long long)(eta / 3600), (unsigned long long)(eta % 3600 / 60), (unsigned long long)(eta % 60));
        } else if (j->state == J_INSTALLING) {
            snprintf(b, sizeof(b), "Installing... %d%%", j->install_pct);
            pct = j->install_pct;
        } else if (j->state == J_ERROR) {
            snprintf(b, sizeof(b), "%s", j->err);
        } else {
            snprintf(b, sizeof(b), "%s / %s   %.1f%%", a1, j->size ? a2 : "?", pct);
        }
        text(70, y + 50, F_SMALL, j->state == J_ERROR ? C_RED : C_MUTED, b, W - 200);
        int bw = W - 140;
        rect(70, y + 86, bw, 10, C_LINE);
        rect(70, y + 86, (int)(bw * pct / 100.0), 10, sc == C_MUTED ? C_BLUE : sc);
    }
    if (n > vis) {
        snprintf(b, sizeof(b), "%d-%d of %d", s_scroll + 1, s_scroll + vis < n ? s_scroll + vis : n, n);
        text(W - 60 - text_w(F_SMALL, b), top + vis * rowh - 6, F_SMALL, C_MUTED, b, 0);
    }

    // log
    int ly = top + vis * rowh + 24;
    rect(40, ly, W - 80, 2, C_LINE);
    char lines[6][160];
    int ln = g_remote ? remote_log_lines(lines, 6) : log_lines(lines, 6);
    for (int i = 0; i < ln; i++) text(60, ly + 12 + i * 30, F_SMALL, C_MUTED, lines[i], W - 120);

    // footer
    rect(0, H - 60, W, 60, C_PANEL);
    const char *help = (s_confirm_del >= 0 && now_ms() < s_confirm_until)
        ? "Press SQUARE again to remove this item and delete its file"
        : "X pause/resume/retry    TRIANGLE install    SQUARE remove    L1/R1 connections -/+    UP/DOWN select";
    text(60, H - 46, F_SMALL, (s_confirm_del >= 0 && now_ms() < s_confirm_until) ? C_AMBER : C_TEXT, help, 0);
}

static uint32_t s_prev = 0;
static uint64_t s_rep_t = 0;

static void input(void) {
    if (s_pad < 0) return;
    PadData pd;
    memset(&pd, 0, sizeof(pd));
    if (scePadReadState(s_pad, &pd) != 0) return;
    uint32_t b = pd.buttons;
    uint32_t pressed = b & ~s_prev;
    uint64_t t = now_ms();
    // key repeat for up/down
    if (b & (PAD_UP | PAD_DOWN)) {
        if (pressed & (PAD_UP | PAD_DOWN)) s_rep_t = t + 400;
        else if (t >= s_rep_t) { pressed |= b & (PAD_UP | PAD_DOWN); s_rep_t = t + 90; }
    }
    s_prev = b;
    if (!pressed) return;

    int ids[MAX_JOBS], n = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (g_jobs[i].used) ids[n++] = i;
    if (pressed & PAD_UP) s_sel--;
    if (pressed & PAD_DOWN) s_sel++;
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < 0) s_sel = 0;
    if (pressed & PAD_L1 && g_conns > 1) ui_set_conns(g_conns - 1);
    if (pressed & PAD_R1 && g_conns < MAX_CONNS) ui_set_conns(g_conns + 1);
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
}

void ui_run(void) {
    if (!s_ui_ok) return;
    int64_t frame = 0;
    for (;;) {
        input();
        draw();
        memcpy(s_fb[s_fb_idx], s_cv.pixels, (size_t)W * H * 4);
        sceVideoOutSubmitFlip(s_video, s_fb_idx, 1 /* vsync */, ++frame);
        s_fb_idx ^= 1;
        sceKernelUsleep(50000); // ~20 fps; previous buffer has long flipped by now
    }
}
