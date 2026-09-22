#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_MEM_CUSTOM 0
/* Host pool for the acceptance harness: the firmware routes LVGL memory
   through lv_mem_hybrid (PSRAM), but this harness uses the built-in static
   pool. The page-image scenario stores a 240x80 RGB565 buffer (38.4KB) via
   lv_mem_alloc, so the pool must exceed that plus normal widget usage. */
#define LV_MEM_SIZE (256U * 1024U)
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_FONT_COMPRESSED 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#endif
