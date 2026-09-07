/*
 * render.cpp - rasterize the DVG's line list onto the 240x280 panel from a dedicated task.
 *
 * Asteroids is a black-and-white vector monitor, and the cabinet is horizontal: MAME's visible
 * area is 1044 x 788, which is 4:3. So the medal stays upright and the picture is letterboxed
 * to 240 x 180 with a 50-row bar above and below, the same shape TRENCHRUNNER uses, and the
 * enclosure does not have to change.
 *
 * Only the beam brightness varies, so the palette is 32 shades of white and an index is just
 * the DVG intensity. Lines are additive: where the beam crosses itself the phosphor is brighter,
 * which is most of what makes a vector picture look like one.
 */
#include "render.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "RENDER";

#define FB_W DISPLAY_WIDTH                        /* 240 */
#define FB_H DISPLAY_HEIGHT                       /* 280 */
#define ROWS_PER_CHUNK 14
#define NUM_LISTS 2
#define PIC_W FB_W                                /* 240 */
#define PIC_H (FB_W * 3 / 4)                      /* 180: the 4:3 shape of the real monitor */
#define TOP_BAR ((FB_H - PIC_H) / 2)              /* 50 blank rows above and below */

typedef struct { int16_t x0, y0, x1, y1; uint8_t b; } seg_t;
typedef struct { seg_t s[AST_MAX_LINES]; int n; } vlist_t;

static vlist_t *lists[NUM_LISTS];
static QueueHandle_t free_q, frame_q;
static uint8_t *fb;                               /* FB_W * FB_H intensity */
static uint16_t *chunk;
static uint16_t palette[256];
static uint32_t frames_drawn, frames_dropped;
static uint64_t busy_us;

static void build_palette(void)
{
    for (int i = 0; i < 256; i++) {
        unsigned v = i > 255 ? 255 : (unsigned)i;
        uint16_t c = (uint16_t)(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3));
        palette[i] = (uint16_t)((c >> 8) | (c << 8));   /* the panel wants big-endian */
    }
    palette[0] = 0;
}

/* the beam window, y running upwards, onto the letterboxed picture, y running downwards */
#define BEAM_W (AST_X_MAX - AST_X_MIN)
#define BEAM_H (AST_Y_MAX - AST_Y_MIN)
static inline void beam_to_panel(int bx, int by, int *px, int *py)
{
    bx -= AST_X_MIN; by -= AST_Y_MIN;
    if (bx < 0) bx = 0; else if (bx > BEAM_W - 1) bx = BEAM_W - 1;
    if (by < 0) by = 0; else if (by > BEAM_H - 1) by = BEAM_H - 1;
    *px = bx * (PIC_W - 1) / (BEAM_W - 1);
    *py = TOP_BAR + (BEAM_H - 1 - by) * (PIC_H - 1) / (BEAM_H - 1);
}

/*
 * Anti-aliased lines. At this size a hard one-pixel Bresenham line is the wrong tool: a stroke
 * that falls between two pixel rows lands entirely on one of them, so small text breaks up and
 * becomes unreadable. Spreading each step's brightness across the two neighbouring pixels costs
 * one extra write and makes the glyphs legible, which is also closer to what a real phosphor
 * does. Writes are additive, so where the beam crosses itself the trace is brighter.
 */
static inline void add_px(int x, int y, int b)
{
    if (b <= 0 || (unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
    uint8_t *p = &fb[y * FB_W + x];
    int v = *p + b;
    *p = (uint8_t)(v > 255 ? 255 : v);
}

static void line(int x0, int y0, int x1, int y1, int b)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx == 0 && ady == 0) { add_px(x0, y0, b); return; }
    if (adx >= ady) {                       /* x is the major axis */
        if (x0 > x1) { int t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        int32_t y = (int32_t)y0 << 16;
        int32_t step = (((int32_t)(y1 - y0)) << 16) / (x1 - x0);
        for (int x = x0; x <= x1; x++, y += step) {
            int yi = (int)(y >> 16), f = (int)((y >> 8) & 0xff);
            add_px(x, yi, (b * (256 - f)) >> 8);
            add_px(x, yi + 1, (b * f) >> 8);
        }
    } else {                                /* y is the major axis */
        if (y0 > y1) { int t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        int32_t x = (int32_t)x0 << 16;
        int32_t step = (((int32_t)(x1 - x0)) << 16) / (y1 - y0);
        for (int y = y0; y <= y1; y++, x += step) {
            int xi = (int)(x >> 16), f = (int)((x >> 8) & 0xff);
            add_px(xi, y, (b * (256 - f)) >> 8);
            add_px(xi + 1, y, (b * f) >> 8);
        }
    }
}

static void rasterize(const vlist_t *l)
{
    memset(fb, 0, FB_W * FB_H);
    for (int i = 0; i < l->n; i++) {
        const seg_t *s = &l->s[i];
        int x0, y0, x1, y1;
        beam_to_panel(s->x0, s->y0, &x0, &y0);
        beam_to_panel(s->x1, s->y1, &x1, &y1);
        line(x0, y0, x1, y1, s->b);
    }
}

static void present(void)
{
    display_set_window(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (int row = 0; row < DISPLAY_HEIGHT; row += ROWS_PER_CHUNK) {
        int rows = (row + ROWS_PER_CHUNK <= DISPLAY_HEIGHT) ? ROWS_PER_CHUNK : (DISPLAY_HEIGHT - row);
        const uint8_t *src = fb + row * FB_W;
        uint16_t *dst = chunk;
        for (int i = 0; i < rows * FB_W; i++) *dst++ = palette[*src++];
        display_write_preswapped(chunk, rows * DISPLAY_WIDTH);
    }
    display_wait_done();
}

static void render_task(void *arg)
{
    (void)arg;
    for (;;) {
        vlist_t *l;
        if (xQueueReceive(frame_q, &l, portMAX_DELAY) != pdTRUE) continue;
        int64_t t0 = esp_timer_get_time();
        rasterize(l);
        present();
        busy_us += esp_timer_get_time() - t0;
        xQueueSend(free_q, &l, 0);
        frames_drawn++;
    }
}

void render_init(void)
{
    build_palette();
    fb = (uint8_t *)heap_caps_malloc(FB_W * FB_H, MALLOC_CAP_8BIT);
    chunk = (uint16_t *)heap_caps_malloc(ROWS_PER_CHUNK * DISPLAY_WIDTH * sizeof(uint16_t), MALLOC_CAP_8BIT);
    free_q = xQueueCreate(NUM_LISTS, sizeof(vlist_t *));
    frame_q = xQueueCreate(NUM_LISTS, sizeof(vlist_t *));
    for (int i = 0; i < NUM_LISTS; i++) {
        lists[i] = (vlist_t *)heap_caps_malloc(sizeof(vlist_t), MALLOC_CAP_8BIT);
        if (!lists[i]) { ESP_LOGE(TAG, "vector list allocation failed"); abort(); }
        xQueueSend(free_q, &lists[i], 0);
    }
    if (!fb || !chunk) { ESP_LOGE(TAG, "frame buffer allocation failed"); abort(); }
    xTaskCreate(render_task, "render", 4096, nullptr, 6, nullptr);
    ESP_LOGI(TAG, "render task started (%dx%d picture, %d-row bars)", PIC_W, PIC_H, TOP_BAR);
}

bool render_submit(const ast_line_t *src, int n)
{
    vlist_t *l;
    if (xQueueReceive(free_q, &l, 0) != pdTRUE) { frames_dropped++; return false; }
    if (n > AST_MAX_LINES) n = AST_MAX_LINES;
    l->n = n;
    for (int i = 0; i < n; i++) {
        l->s[i].x0 = src[i].x0; l->s[i].y0 = src[i].y0;
        l->s[i].x1 = src[i].x1; l->s[i].y1 = src[i].y1;
        /* the DVG's 4-bit intensity, opened up to most of the panel's range */
        int b = 40 + src[i].bright * 14;
        l->s[i].b = (uint8_t)(b > 255 ? 255 : b);
    }
    xQueueSend(frame_q, &l, 0);
    return true;
}

uint32_t render_frames_drawn(void) { uint32_t v = frames_drawn; frames_drawn = 0; return v; }
uint32_t render_frames_dropped(void) { uint32_t v = frames_dropped; frames_dropped = 0; return v; }
uint64_t render_busy_us(void) { uint64_t v = busy_us; busy_us = 0; return v; }
