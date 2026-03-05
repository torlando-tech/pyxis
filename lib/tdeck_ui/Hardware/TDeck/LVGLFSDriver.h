// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_LVGLFSDRIVER_H
#define HARDWARE_TDECK_LVGLFSDRIVER_H

#ifdef ARDUINO

#include <lvgl.h>

namespace Hardware {
namespace TDeck {

/**
 * LVGL filesystem driver for SD card access.
 *
 * Registers drive letter 'S' so LVGL can load files like:
 *   "S:tiles/14/8192/5455.png"
 *
 * Uses SDAccess mutex for SPI bus arbitration. The mutex is acquired
 * per-read (not per-open) to avoid blocking display flushes during
 * long tile decode operations.
 */
class LVGLFSDriver {
public:
    static void init();

private:
    static void* fs_open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode);
    static lv_fs_res_t fs_close(lv_fs_drv_t* drv, void* file_p);
    static lv_fs_res_t fs_read(lv_fs_drv_t* drv, void* file_p,
                                void* buf, uint32_t btr, uint32_t* br);
    static lv_fs_res_t fs_seek(lv_fs_drv_t* drv, void* file_p,
                                uint32_t pos, lv_fs_whence_t whence);
    static lv_fs_res_t fs_tell(lv_fs_drv_t* drv, void* file_p, uint32_t* pos_p);
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_LVGLFSDRIVER_H
