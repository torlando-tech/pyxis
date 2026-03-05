// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "LVGLFSDriver.h"

#ifdef ARDUINO

#include "SDAccess.h"
#include <SD.h>
#include <Log.h>

namespace Hardware {
namespace TDeck {

// File handle wrapper — holds an open SD File object
struct FSFileHandle {
    File file;
};

void LVGLFSDriver::init() {
    if (!SDAccess::is_ready()) {
        WARNING("LVGLFSDriver: SD card not ready, skipping init");
        return;
    }

    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);

    drv.letter = 'S';
    drv.open_cb = fs_open;
    drv.close_cb = fs_close;
    drv.read_cb = fs_read;
    drv.seek_cb = fs_seek;
    drv.tell_cb = fs_tell;

    lv_fs_drv_register(&drv);

    INFO("LVGLFSDriver: Registered drive 'S' for SD card");
}

void* LVGLFSDriver::fs_open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    (void)drv;

    if (mode != LV_FS_MODE_RD) {
        return NULL;  // Read-only
    }

    // Build full SD path: prepend / prefix
    // SD.open() already uses the /sd mount point internally
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/%s", path);

    if (!SDAccess::acquire_bus(500)) {
        return NULL;
    }

    FSFileHandle* handle = new FSFileHandle();
    handle->file = SD.open(full_path, FILE_READ);

    SDAccess::release_bus();

    if (!handle->file) {
        delete handle;
        return NULL;
    }

    return handle;
}

lv_fs_res_t LVGLFSDriver::fs_close(lv_fs_drv_t* drv, void* file_p) {
    (void)drv;
    FSFileHandle* handle = (FSFileHandle*)file_p;

    if (!SDAccess::acquire_bus(500)) {
        // Still delete handle to avoid leak
        delete handle;
        return LV_FS_RES_HW_ERR;
    }

    handle->file.close();
    SDAccess::release_bus();

    delete handle;
    return LV_FS_RES_OK;
}

lv_fs_res_t LVGLFSDriver::fs_read(lv_fs_drv_t* drv, void* file_p,
                                    void* buf, uint32_t btr, uint32_t* br) {
    (void)drv;
    FSFileHandle* handle = (FSFileHandle*)file_p;

    if (!SDAccess::acquire_bus(500)) {
        *br = 0;
        return LV_FS_RES_HW_ERR;
    }

    *br = handle->file.read((uint8_t*)buf, btr);

    SDAccess::release_bus();
    return LV_FS_RES_OK;
}

lv_fs_res_t LVGLFSDriver::fs_seek(lv_fs_drv_t* drv, void* file_p,
                                    uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;
    FSFileHandle* handle = (FSFileHandle*)file_p;

    if (!SDAccess::acquire_bus(500)) {
        return LV_FS_RES_HW_ERR;
    }

    SeekMode mode = SeekSet;
    if (whence == LV_FS_SEEK_CUR) mode = SeekCur;
    else if (whence == LV_FS_SEEK_END) mode = SeekEnd;

    bool ok = handle->file.seek(pos, mode);

    SDAccess::release_bus();
    return ok ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

lv_fs_res_t LVGLFSDriver::fs_tell(lv_fs_drv_t* drv, void* file_p, uint32_t* pos_p) {
    (void)drv;
    FSFileHandle* handle = (FSFileHandle*)file_p;

    if (!SDAccess::acquire_bus(500)) {
        return LV_FS_RES_HW_ERR;
    }

    *pos_p = handle->file.position();

    SDAccess::release_bus();
    return LV_FS_RES_OK;
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
