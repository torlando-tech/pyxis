// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_SD_ARCHIVE_FS_H
#define HARDWARE_TDECK_SD_ARCHIVE_FS_H

#ifdef ARDUINO

#include "SDAccess.h"

#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <Arduino.h>
#include <SD.h>
#include <FS.h>

namespace Hardware {
namespace TDeck {

// microStore::FileSystem adapter that piggybacks on the SD card already
// mounted by SDAccess::init() and serializes every operation through
// the shared SPI bus mutex.
//
// The stock microStore SDFileSystem adapter calls SD.begin() in its own
// init() and uses its own SPIClass instance — that conflicts with the
// pyxis HW init order (display + LoRa share HSPI). This adapter skips
// re-mounting and just delegates to the global SD object, wrapping each
// op with SDAccess::acquire_bus() / release_bus() so it cooperates with
// display and LoRa traffic.
//
// Used as the archive tier for LXMF::MessageStore — the LittleFS hot
// tier holds the most-recent ~50 messages per conversation; everything
// older is moved here.

namespace _SDArchive {

class FileImpl : public microStore::FileImpl {
private:
    fs::File _file;
    bool _open;
public:
    FileImpl(fs::File f) : microStore::FileImpl(), _file(f), _open(true) {}
    virtual ~FileImpl() { if (_open) close(); }

    inline virtual const char* name() const { return _file.name(); }
    inline virtual size_t size() const { return _file.size(); }
    inline virtual void close() {
        if (!_open) return;
        SDAccess::acquire_bus(500);
        _file.close();
        SDAccess::release_bus();
        _open = false;
    }
    inline virtual int read() {
        if (!SDAccess::acquire_bus(500)) return -1;
        int r = _file.read();
        SDAccess::release_bus();
        return r;
    }
    inline virtual size_t read(uint8_t* buf, size_t sz) {
        if (!SDAccess::acquire_bus(500)) return 0;
        size_t r = _file.read(buf, sz);
        SDAccess::release_bus();
        return r;
    }
    inline virtual size_t write(uint8_t b) {
        if (!SDAccess::acquire_bus(500)) return 0;
        size_t w = _file.write(b);
        SDAccess::release_bus();
        return w;
    }
    inline virtual size_t write(const uint8_t* buf, size_t sz) {
        if (!SDAccess::acquire_bus(500)) return 0;
        size_t w = _file.write(buf, sz);
        SDAccess::release_bus();
        return w;
    }
    inline virtual int available() {
        if (!SDAccess::acquire_bus(500)) return 0;
        int a = _file.available();
        SDAccess::release_bus();
        return a;
    }
    inline virtual int peek() {
        if (!SDAccess::acquire_bus(500)) return -1;
        int p = _file.peek();
        SDAccess::release_bus();
        return p;
    }
    inline virtual size_t tell() {
        if (!SDAccess::acquire_bus(500)) return 0;
        size_t t = _file.position();
        SDAccess::release_bus();
        return t;
    }
    inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
        if (!SDAccess::acquire_bus(500)) return -1;
        fs::SeekMode smode = fs::SeekSet;
        switch (mode) {
            case microStore::SeekMode::SeekModeCur: smode = fs::SeekCur; break;
            case microStore::SeekMode::SeekModeEnd: smode = fs::SeekEnd; break;
            default: smode = fs::SeekSet; break;
        }
        long ok = _file.seek(pos, smode);
        SDAccess::release_bus();
        return ok;
    }
    inline virtual void flush() {
        if (!SDAccess::acquire_bus(500)) return;
        _file.flush();
        SDAccess::release_bus();
    }
    inline virtual bool isValid() const { return _open && _file; }
};

class FileSystemImpl : public microStore::FileSystemImpl {
public:
    FileSystemImpl() : microStore::FileSystemImpl() {}
    virtual ~FileSystemImpl() {}

    // SD is already mounted by SDAccess::init() — we don't re-mount.
    virtual bool init() override { return SDAccess::is_ready(); }
    virtual bool format() override { return false; }

    virtual microStore::File open(const char* path, microStore::File::Mode mode,
                                  const bool create = false) override {
        const char* pmode = nullptr;
        switch (mode) {
            case microStore::File::ModeRead:        pmode = FILE_READ; break;
            case microStore::File::ModeWrite:       pmode = FILE_WRITE; break;
            case microStore::File::ModeAppend:      pmode = FILE_APPEND; break;
            case microStore::File::ModeReadWrite:   pmode = "w+"; break;
            case microStore::File::ModeReadAppend:  pmode = "a+"; break;
            default: return {};
        }
        if (!SDAccess::acquire_bus(500)) return {};
        fs::File f = SD.open(path, pmode);
        SDAccess::release_bus();
        if (!f) return {};
        return microStore::File(new FileImpl(f));
    }

    virtual bool exists(const char* path) override {
        if (!SDAccess::acquire_bus(500)) return false;
        bool r = SD.exists(path);
        SDAccess::release_bus();
        return r;
    }
    virtual bool remove(const char* path) override {
        if (!SDAccess::acquire_bus(500)) return false;
        bool r = SD.remove(path);
        SDAccess::release_bus();
        return r;
    }
    virtual bool rename(const char* from, const char* to) override {
        if (!SDAccess::acquire_bus(500)) return false;
        bool r = SD.rename(from, to);
        SDAccess::release_bus();
        return r;
    }
    virtual bool mkdir(const char* path) override {
        if (!SDAccess::acquire_bus(500)) return false;
        bool r = SD.mkdir(path);
        SDAccess::release_bus();
        return r;
    }
    virtual bool rmdir(const char* path) override {
        if (!SDAccess::acquire_bus(500)) return false;
        bool r = SD.rmdir(path);
        SDAccess::release_bus();
        return r;
    }
    virtual bool isDirectory(const char* path) override {
        if (!SDAccess::acquire_bus(500)) return false;
        fs::File f = SD.open(path, FILE_READ);
        bool r = false;
        if (f) {
            r = f.isDirectory();
            f.close();
        }
        SDAccess::release_bus();
        return r;
    }
    virtual std::list<std::string> listDirectory(const char* path,
            Callbacks::DirectoryListing callback = nullptr) override {
        std::list<std::string> files;
        if (!SDAccess::acquire_bus(500)) return files;
        fs::File root = SD.open(path);
        if (root) {
            fs::File f = root.openNextFile();
            while (f) {
                if (!f.isDirectory()) {
                    const char* name = f.name();
                    if (callback) callback(name);
                    else files.push_back(name);
                }
                f.close();
                f = root.openNextFile();
            }
            root.close();
        }
        SDAccess::release_bus();
        return files;
    }
    virtual size_t storageSize() override {
        if (!SDAccess::acquire_bus(500)) return 0;
        size_t s = SD.totalBytes();
        SDAccess::release_bus();
        return s;
    }
    virtual size_t storageAvailable() override {
        if (!SDAccess::acquire_bus(500)) return 0;
        size_t s = SD.totalBytes() - SD.usedBytes();
        SDAccess::release_bus();
        return s;
    }
    virtual bool isValid() const override { return SDAccess::is_ready(); }
};

}  // namespace _SDArchive

class SDArchiveFileSystem : public microStore::FileSystem {
public:
    SDArchiveFileSystem() : microStore::FileSystem(new _SDArchive::FileSystemImpl()) {}
    virtual ~SDArchiveFileSystem() {}
};

}  // namespace TDeck
}  // namespace Hardware

#endif  // ARDUINO
#endif  // HARDWARE_TDECK_SD_ARCHIVE_FS_H
