// Host shim for <SD.h> — models the SD card as a host-filesystem tree rooted
// at HostSD::root(), mounted under the same "/sd" prefix the firmware uses.
//
// HostFile mirrors the Arduino sdio File semantics the production code relies
// on: copy constructs duplicate the open handle (both stay usable and close
// independently), read() returns data in bounded SPI bursts, and close()
// releases the underlying descriptor. Directory handles expose the
// openNextFile()/isDirectory()/path() iteration API used by
// MapTileStoreSD::nextList.
#ifndef SDHOSTSHIM_SD_H
#define SDHOSTSHIM_SD_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "SPI.h"

#define FILE_READ 1
#define FILE_WRITE 2
#define CARD_NONE 0
#define CARD_MMC 1
#define CARD_SD 2
#define CARD_SDHC 3

class HostFile;

class HostSD {
public:
    static void set_root(const char* path) { root_ = path; present_ = true; }
    static const char* root() { return root_.empty() ? "/tmp" : root_.c_str(); }
    static bool ready() { return present_; }

    static bool begin(std::uint8_t, const HostSPI&, std::uint32_t,
                      const char* mount, std::uint8_t, bool) {
        (void)mount;
        return true;
    }
    static std::uint8_t cardType() { return present_ ? CARD_SDHC : CARD_NONE; }
    static std::uint64_t cardSize() { return 119850ULL * 1024ULL * 1024ULL; }

    static std::string absolute(const char* path) { return std::string(root()) + path; }
    static bool exists(const char* path) {
        struct stat st;
        return ::stat(absolute(path).c_str(), &st) == 0;
    }
    static bool mkdir(const char* path) { return ::mkdir(absolute(path).c_str(), 0755) == 0; }
    static bool remove(const char* path) { return ::unlink(absolute(path).c_str()) == 0; }
    static bool rename(const char* from, const char* to) {
        return ::rename(absolute(from).c_str(), absolute(to).c_str()) == 0;
    }
    static HostFile open(const char* path, int);

private:
    static std::string root_;
    static bool present_;
};

extern HostSD SD;

class HostFile {
public:
    HostFile() : fd_(-1), dir_(NULL) {}
    HostFile(const HostFile& other)
        : fd_(other.fd_ >= 0 ? ::dup(other.fd_) : -1),
          dir_(other.isDir() ? ::opendir(other.absolute().c_str()) : NULL),
          path_(other.path_) {}
    HostFile& operator=(const HostFile& other) {
        if (this == &other) return *this;
        close();
        fd_ = other.fd_ >= 0 ? ::dup(other.fd_) : -1;
        dir_ = other.isDir() ? ::opendir(other.absolute().c_str()) : NULL;
        path_ = other.path_;
        return *this;
    }
    ~HostFile() { close(); }

    explicit operator bool() const { return fd_ >= 0 || dir_ != NULL; }
    bool isDirectory() const { return dir_ != NULL; }

    // Device-relative path of this entry ("" for a closed handle).
    const char* path() const { return path_.c_str(); }
    std::string name() const {
        const std::size_t slash = path_.find_last_of('/');
        return path_.substr(slash + 1);
    }

    // Bounded SPI read burst: the SD layer never returns more than this many
    // bytes per transfer, so callers always loop. Returns the number of bytes
    // actually read (0 at end-of-file or error), matching Arduino File::read.
    static const int SPI_BURST_BYTES = 512;

    std::size_t read(void* buffer, std::size_t count) {
        if (fd_ < 0) return 0U;
        const std::size_t burst = count < static_cast<std::size_t>(SPI_BURST_BYTES)
            ? count : static_cast<std::size_t>(SPI_BURST_BYTES);
        if (burst == 0U) return 0U;
        const ssize_t got = ::read(fd_, buffer, burst);
        return (got < 0) ? 0U : static_cast<std::size_t>(got);
    }

    std::size_t size() const {
        if (fd_ < 0) return 0U;
        struct stat st;
        if (::fstat(fd_, &st) != 0) return 0U;
        return static_cast<std::size_t>(st.st_size);
    }

    bool available() const {
        if (fd_ < 0) return false;
        const off_t cur = ::lseek(fd_, 0, SEEK_CUR);
        if (cur < 0) return false;
        const off_t end = ::lseek(fd_, 0, SEEK_END);
        (void)::lseek(fd_, cur, SEEK_SET);
        return end > cur;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (dir_ != NULL) {
            ::closedir(dir_);
            dir_ = NULL;
        }
    }

    // Next entry inside a directory handle (dotfiles skipped), or a closed
    // handle at end-of-directory (errno=0).
    HostFile openNextFile() const {
        if (dir_ == NULL) {
            errno = ENOTDIR;
            return HostFile();
        }
        struct dirent* entry = NULL;
        while ((entry = ::readdir(dir_)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            const std::string abs_parent = absolute();
            const std::string rel_child = path_ + "/" + entry->d_name;
            const std::string abs_child = abs_parent + "/" + entry->d_name;
            struct stat st;
            if (::stat(abs_child.c_str(), &st) != 0) continue;
            HostFile child;
            child.path_ = rel_child;
            if (S_ISDIR(st.st_mode)) {
                child.dir_ = ::opendir(abs_child.c_str());
                if (child.dir_ == NULL) child = HostFile();
            } else {
                child.fd_ = ::open(abs_child.c_str(), O_RDONLY);
                if (child.fd_ < 0) child = HostFile();
            }
            if (child) {
                errno = 0;
                return child;
            }
        }
        errno = 0;
        return HostFile();
    }

private:
    friend class HostSD;
    explicit HostFile(int fd, const std::string& path) : fd_(fd), dir_(NULL), path_(path) {}
    HostFile(DIR* dir, const std::string& path) : fd_(-1), dir_(dir), path_(path) {}

    std::string absolute() const { return HostSD::absolute(path_.c_str()); }
    bool isDir() const { return dir_ != NULL; }
    int fd_;
    DIR* dir_;
    std::string path_;
};

using File = HostFile;

namespace fs {
using File = HostFile;
}

inline HostFile HostSD::open(const char* path, int) {
    const std::string absolute = HostSD::absolute(path);
    struct stat st;
    if (::stat(absolute.c_str(), &st) != 0) return HostFile();
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = ::opendir(absolute.c_str());
        return (dir == NULL) ? HostFile() : HostFile(dir, path);
    }
    const int fd = ::open(absolute.c_str(), O_RDONLY);
    return (fd < 0) ? HostFile() : HostFile(fd, path);
}

#endif
