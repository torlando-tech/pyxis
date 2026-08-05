#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "NomadNetMemory.h"

namespace UI::LXMF::NomadNet {

struct NodeRecord {
    std::string destination_hex;
    std::string name;
    uint64_t last_heard = 0;
    uint8_t hops = 0;
    bool saved = false;
    NodeRecord() = default;
    NodeRecord(std::string destination, std::string display_name, uint64_t heard,
               uint8_t hop_count, bool is_saved)
        : destination_hex(std::move(destination)), name(std::move(display_name)),
          last_heard(heard), hops(hop_count), saved(is_saved) {}
};

struct PageRecord {
    std::string url;
    std::string title;
    uint64_t last_opened = 0;
    bool saved = false;
    PageRecord() = default;
    PageRecord(std::string page_url, std::string page_title, uint64_t opened, bool is_saved)
        : url(std::move(page_url)), title(std::move(page_title)),
          last_opened(opened), saved(is_saved) {}
};

class Library {
public:
    static constexpr std::size_t MAX_NODES = 32;
    static constexpr std::size_t MAX_PAGES = 16;
    static constexpr std::size_t MAX_NAME_BYTES = 64;
    static constexpr std::size_t MAX_TITLE_BYTES = 96;
    static constexpr std::size_t MAX_URL_BYTES = 511;
    static constexpr std::size_t MAX_ENCODED_BYTES = 64 * 1024;

    const std::vector<NodeRecord>& nodes() const { return _nodes; }
    const std::vector<PageRecord>& pages() const { return _pages; }

    bool hear_node(const std::string& destination_hex, const std::string& name,
                   uint64_t timestamp, uint8_t hops);
    bool record_page(const std::string& url, const std::string& title, uint64_t timestamp);
    bool set_node_saved(const std::string& destination_hex, bool saved);
    bool set_page_saved(const std::string& url, bool saved);
    bool node_saved(const std::string& destination_hex) const;
    bool page_saved(const std::string& url) const;

    ExternalVector<uint8_t> encode() const;
    bool decode(const uint8_t* data, std::size_t size);
    void clear();

private:
    std::vector<NodeRecord> _nodes;
    std::vector<PageRecord> _pages;
};

std::string sanitize_directory_name(const uint8_t* data, std::size_t size);
std::string page_title(const std::string& fallback, const std::vector<std::string>& heading_runs);

} // namespace UI::LXMF::NomadNet
