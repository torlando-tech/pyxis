#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

static bool fail_allocations;

void *test_malloc(size_t size) {
    return fail_allocations ? NULL : malloc(size);
}

void *test_realloc(void *ptr, size_t size) {
    return fail_allocations ? NULL : realloc(ptr, size);
}

void test_free(void *ptr) {
    free(ptr);
}

static void flush(lv_disp_drv_t *display, const lv_area_t *area, lv_color_t *pixels) {
    (void)area;
    (void)pixels;
    lv_disp_flush_ready(display);
}

int main(void) {
    lv_init();

    static lv_color_t pixels[100];
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, pixels, NULL, 100);

    lv_disp_drv_t display;
    lv_disp_drv_init(&display);
    display.draw_buf = &draw_buffer;
    display.hor_res = 10;
    display.ver_res = 10;
    display.flush_cb = flush;
    assert(lv_disp_drv_register(&display) != NULL);

    lv_obj_t *textarea = lv_textarea_create(lv_scr_act());
    assert(textarea != NULL);
    assert(!lv_textarea_get_password_mode(textarea));

    fail_allocations = true;
    lv_textarea_set_password_mode(textarea, true);
    fail_allocations = false;

    /* The application must observe this false state and destroy the editor
     * before loading sensitive text. */
    assert(!lv_textarea_get_password_mode(textarea));
    assert(lv_textarea_get_text(textarea) != NULL);
    assert(lv_textarea_get_text(textarea)[0] == '\0');

    /* A constrained non-password replacement must be atomic as a whole. A
     * failed clear may not be followed by characterwise append, which would
     * corrupt the visible browser address while Back remains uncommitted. */
    lv_textarea_set_max_length(textarea, 511);
    lv_textarea_set_text(textarea, "node:/page/a.mu#details");
    assert(strcmp(lv_textarea_get_text(textarea), "node:/page/a.mu#details") == 0);
    fail_allocations = true;
    lv_textarea_set_text(textarea, "node:/page/a.mu");
    fail_allocations = false;
    assert(strcmp(lv_textarea_get_text(textarea), "node:/page/a.mu#details") == 0);

    lv_obj_del(textarea);
    return 0;
}
