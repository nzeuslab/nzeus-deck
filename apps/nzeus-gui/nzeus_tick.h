#ifndef NZEUS_TICK_H
#define NZEUS_TICK_H
#include <stdint.h>

/* Milliseconds since start. LVGL uses it for animations and timers. */
uint32_t nzeus_tick_ms(void);

#endif
