#pragma once
#include <stdint.h>
#include "asteroids.h"
#ifdef __cplusplus
extern "C" {
#endif
void render_init(void);
/* hand a display list to the render task; returns false if it is still busy with the last one */
bool render_submit(const ast_line_t *lines, int n);
uint32_t render_frames_drawn(void);
uint32_t render_frames_dropped(void);
uint64_t render_busy_us(void);
#ifdef __cplusplus
}
#endif
