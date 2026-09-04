// Benchmark: conversation-list store load path (the work ConversationListScreen::refresh()
// does per conversation), compiled against the EXACT pinned microLXMF source in .pio/libdeps.
//
// Modes:
//   baseline : get_conversations() + per-conversation get_last_message_hash() +
//              load_message_metadata(last_hash) + get_display_name() +
//              get_conversation_unread_count()   (== PR #95 refresh() I/O)
//   index    : same, but preview/timestamp come from in-memory index accessors when
//              available (compiled only when the pinned store supports them)
//
// Prints median-of-iterations wall time per mode so a before/after table can be built.

#include <LXMF/MessageStore.h>
#include <LXMF/LXMessage.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Utilities/OS.h>

#include <microStore/Adapters/PosixFileSystem.h>
#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

using LXMF::MessageStore;
using LXMF::LXMessage;
using RNS::Bytes;

// ---------- prefixing filesystem (mirror of test_messagestore_tiers) ----------
namespace bench_fs {

class PrefixedFileImpl : public microStore::FileImpl {
private:
    int _fd;
    bool _closed;
public:
    explicit PrefixedFileImpl(int fd) : microStore::FileImpl(), _fd(fd), _closed(false) {}
    ~PrefixedFileImpl() override { if (!_closed) close(); }
    const char* name() const override { return ""; }
    size_t size() const override { struct stat st; ::fstat(_fd, &st); return st.st_size; }
    void close() override { if (!_closed) { ::close(_fd); _closed = true; } }
    int read() override { uint8_t b; if (::read(_fd, &b, 1) != 1) return -1; return b; }
    int peek() override { return -1; }
    size_t read(uint8_t* buf, size_t sz) override {
        ssize_t n = ::read(_fd, buf, sz); return n < 0 ? 0 : (size_t)n;
    }
    size_t write(uint8_t b) override { return ::write(_fd, &b, 1) == 1 ? 1 : 0; }
    size_t write(const uint8_t* buf, size_t sz) override {
        ssize_t n = ::write(_fd, buf, sz); return n < 0 ? 0 : (size_t)n;
    }
    int available() override { return 0; }
    size_t tell() override { return ::lseek(_fd, 0, SEEK_CUR); }
    long seek(uint32_t pos, microStore::SeekMode m) override {
        int wh = SEEK_SET;
        if (m == microStore::SeekMode::SeekModeCur) wh = SEEK_CUR;
        else if (m == microStore::SeekMode::SeekModeEnd) wh = SEEK_END;
        return ::lseek(_fd, pos, wh);
    }
    void flush() override {}
    bool isValid() const override { return !_closed && _fd >= 0; }
};

class PrefixedFSImpl : public microStore::FileSystemImpl {
private:
    std::string _prefix;
    std::string fp(const char* path) const { return _prefix + path; }
    void mkparents(const std::string& path) const {
        size_t p = 0;
        while ((p = path.find('/', p + 1)) != std::string::npos) {
            std::string dir = path.substr(0, p);
            ::mkdir(dir.c_str(), 0755);
        }
    }
public:
    explicit PrefixedFSImpl(const std::string& prefix) : _prefix(prefix) {
        ::mkdir(prefix.c_str(), 0755);
    }
    bool init(bool) override { return true; }
    bool format() override { return false; }
    microStore::File open(const char* path, microStore::File::Mode mode,
                          const bool create = false) override {
        std::string full = fp(path);
        int flags;
        switch (mode) {
            case microStore::File::ModeRead:       flags = O_RDONLY; break;
            case microStore::File::ModeWrite:      flags = O_WRONLY|O_CREAT|O_TRUNC; break;
            case microStore::File::ModeAppend:     flags = O_WRONLY|O_CREAT|O_APPEND; break;
            case microStore::File::ModeReadWrite:  flags = O_RDWR|O_CREAT|O_TRUNC; break;
            case microStore::File::ModeReadAppend: flags = O_RDWR|O_CREAT|O_APPEND; break;
            default: return {};
        }
        if (flags & O_CREAT) mkparents(full);
        int fd = ::open(full.c_str(), flags, 0644);
        if (fd == -1) return {};
        return microStore::File(new PrefixedFileImpl(fd));
    }
    bool exists(const char* path) override { struct stat st; return ::stat(fp(path).c_str(), &st) == 0; }
    bool remove(const char* path) override { return ::unlink(fp(path).c_str()) == 0; }
    bool rename(const char* a, const char* b) override { return ::rename(fp(a).c_str(), fp(b).c_str()) == 0; }
    bool mkdir(const char* path) override {
        std::string full = fp(path); mkparents(full);
        return ::mkdir(full.c_str(), 0755) == 0 || errno == EEXIST;
    }
    bool rmdir(const char* path) override { return ::rmdir(fp(path).c_str()) == 0; }
    size_t size(const char* path) override { struct stat st; if (::stat(fp(path).c_str(), &st) != 0) return 0; return st.st_size; }
    bool isDirectory(const char* path) override {
        struct stat st; if (::stat(fp(path).c_str(), &st) != 0) return false;
        return S_ISDIR(st.st_mode);
    }
    std::list<std::string> listDirectory(const char* path,
            Callbacks::DirectoryListing cb = nullptr) override {
        std::list<std::string> out;
        DIR* d = ::opendir(fp(path).c_str());
        if (!d) return out;
        while (auto* ent = ::readdir(d)) {
            if (ent->d_name[0] == '.') continue;
            if (cb) cb(ent->d_name); else out.push_back(ent->d_name);
        }
        ::closedir(d);
        return out;
    }
    size_t storageSize() override { return 0; }
    size_t storageAvailable() override { return 0; }
};

class PrefixedFS : public microStore::FileSystem {
public:
    explicit PrefixedFS(const std::string& prefix)
        : microStore::FileSystem(new PrefixedFSImpl(prefix)) {}
};

}  // namespace bench_fs

// ---------- synthetic messages (mirror of test_messagestore_tiers) ----------
static Bytes make_msgpack_payload(double timestamp,
                                  const std::string& title,
                                  const std::string& content) {
    Bytes p;
    p.append((uint8_t)0x94);  // arr_size 4
    p.append((uint8_t)0xcb);
    union { double d; uint64_t u; } cv;
    cv.d = timestamp;
    for (int i = 7; i >= 0; --i) p.append((uint8_t)((cv.u >> (i * 8)) & 0xff));
    p.append((uint8_t)0xc4);
    p.append((uint8_t)title.size());
    p.append((const uint8_t*)title.data(), title.size());
    if (content.size() < 256) {
        p.append((uint8_t)0xc4);
        p.append((uint8_t)content.size());
    } else {
        p.append((uint8_t)0xc5);
        p.append((uint8_t)((content.size() >> 8) & 0xff));
        p.append((uint8_t)(content.size() & 0xff));
    }
    p.append((const uint8_t*)content.data(), content.size());
    p.append((uint8_t)0x80);  // empty fields map
    return p;
}

static LXMessage make_message(const Bytes& peer_hash, const Bytes& self_hash,
                              double timestamp, const std::string& content,
                              bool incoming) {
    // save_message() keys a conversation by source for incoming,
    // destination for outgoing — set the endpoints so the conversation
    // always lands under peer_hash regardless of direction.
    const Bytes& dest = incoming ? self_hash : peer_hash;
    const Bytes& src  = incoming ? peer_hash : self_hash;
    Bytes raw;
    raw.append(dest.data(), 16);
    raw.append(src.data(), 16);
    for (int i = 0; i < 64; ++i) raw.append((uint8_t)0);
    Bytes payload = make_msgpack_payload(timestamp, "t", content);
    raw.append(payload.data(), payload.size());
    LXMessage m = LXMessage::unpack_from_bytes(raw, LXMF::Type::Message::DIRECT, true);
    m.incoming(incoming);
    return m;
}

// ---------- seed a store shaped like a real T-Deck: N conversations ----------
static void seed_store(MessageStore& store, int convs, int msgs_per_conv) {
    double base_ts = 1750000000.0;
    for (int c = 0; c < convs; ++c) {
        Bytes peer(16), self(16);
        for (int i = 0; i < 16; ++i) {
            peer.append((uint8_t)(0xa0 + c * 7 + i));
            self.append((uint8_t)(0x50 + i));
        }
        store.set_display_name(peer, "peer_" + std::to_string(c));
        for (int m = 0; m < msgs_per_conv; ++m) {
            // ~240-char content: realistic chat text plus a tail so previews
            // exercise truncation the same way the list does.
            std::string content =
                "Check in when you land, the uplink window was tight today ";
            while (content.size() < 200) content += "and the router was hopping paths. ";
            content = content.substr(0, 220);
            double ts = base_ts + (c * 5000 + m * 3600);
            LXMessage msg = make_message(peer, self, ts, content, m % 3 != 0);
            if (!store.save_message(msg)) {
                std::cerr << "seed save failed c=" << c << " m=" << m << "\n";
                std::exit(2);
            }
        }
    }
}

// ---------- the two load paths ----------
// baseline == exactly the per-conversation work of PR #95 refresh()
static double run_baseline(MessageStore& store) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::vector<Bytes> peers = store.get_conversations();
    for (const auto& peer : peers) {
        Bytes last = store.get_last_message_hash(peer);
        if (!last) continue;
        MessageStore::MessageMetadata meta = store.load_message_metadata(last);
        if (!meta.valid) continue;
        std::string preview = meta.content.substr(0, 30);  // the 30-char preview
        double ts = meta.timestamp;
        std::string name = store.get_display_name(peer);
        size_t unread = store.get_conversation_unread_count(peer);
        (void)preview; (void)ts; (void)name; (void)unread;
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

#ifdef BENCH_INDEX_ACCESSORS
// index == store provides in-memory per-conversation preview/timestamp; no
// message-file I/O on the hot path.
static double run_index(MessageStore& store) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::vector<Bytes> peers = store.get_conversations();
    for (const auto& peer : peers) {
        Bytes last = store.get_last_message_hash(peer);
        if (!last) continue;
        std::string preview;
        double ts = 0.0;
        bool from_index = store.get_last_message_preview(peer, preview, ts);
        MessageStore::MessageMetadata meta;
        meta.valid = false;
        if (!from_index) {
            meta = store.load_message_metadata(last);  // fallback (no cached preview)
            if (!meta.valid) continue;
            preview = meta.content.substr(0, 30);
            ts = meta.timestamp;
        }
        std::string name = store.get_display_name(peer);
        size_t unread = store.get_conversation_unread_count(peer);
        (void)preview; (void)ts; (void)name; (void)unread;
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}
#endif

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0.0;
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "/tmp/bench-convlist";
    int convs = argc > 2 ? std::atoi(argv[2]) : 9;
    int msgs = argc > 3 ? std::atoi(argv[3]) : 24;
    int iters = argc > 4 ? std::atoi(argv[4]) : 25;

    ::system(("rm -rf " + root + " && mkdir -p " + root + "/hot").c_str());

    bench_fs::PrefixedFS hot_fs(root + "/hot");
    RNS::Utilities::OS::register_filesystem(hot_fs);

    std::vector<double> baseline_ms;
    std::vector<double> index_warm_ms;
    std::vector<double> index_cold_ms;

    {
        MessageStore store("/lxmf");
        seed_store(store, convs, msgs);
        for (int i = 0; i < iters; ++i) baseline_ms.push_back(run_baseline(store));
#ifdef BENCH_INDEX_ACCESSORS
        // Warm path: the store the app holds right now (in-memory index
        // includes the preview cache).
        for (int i = 0; i < iters; ++i) index_warm_ms.push_back(run_index(store));
#endif
    }  // drop the in-memory store (reboot shape)

#ifdef BENCH_INDEX_ACCESSORS
    {
        // Cold-boot path: reconstruct the store from disk (reboot / fresh
        // firmware boot). The index on disk now carries last_preview (new
        // pin), so the first refresh still hits the cache with zero
        // message-file reads — that is the whole point of persisting it.
        MessageStore store("/lxmf");
        for (int i = 0; i < iters; ++i) index_cold_ms.push_back(run_index(store));
    }
#endif

    std::cout << "convlist-bench convs=" << convs << " msgs_per_conv=" << msgs
              << " iters=" << iters << "\n";
    std::cout << "baseline_ms=" << median(baseline_ms)
              << " (min=" << *std::min_element(baseline_ms.begin(), baseline_ms.end())
              << " max=" << *std::max_element(baseline_ms.begin(), baseline_ms.end()) << ")\n";
#ifdef BENCH_INDEX_ACCESSORS
    std::cout << "index_warm_ms=" << median(index_warm_ms)
              << " (min=" << *std::min_element(index_warm_ms.begin(), index_warm_ms.end())
              << " max=" << *std::max_element(index_warm_ms.begin(), index_warm_ms.end()) << ")\n";
    std::cout << "index_cold_ms=" << median(index_cold_ms)
              << " (min=" << *std::min_element(index_cold_ms.begin(), index_cold_ms.end())
              << " max=" << *std::max_element(index_cold_ms.begin(), index_cold_ms.end()) << ")\n";
#endif
    std::cout << "convlist-bench: done\n";
    return 0;
}
