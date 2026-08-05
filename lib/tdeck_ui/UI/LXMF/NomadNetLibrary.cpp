#include "NomadNetLibrary.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace UI::LXMF::NomadNet {
namespace {

bool is_hex32(const std::string& value) {
    if (value.size() != 32) return false;
    for (const unsigned char c : value)
        if (!std::isxdigit(c)) return false;
    return true;
}

std::string lower_hex(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string utf8_prefix(const std::string& value, std::size_t max_bytes);

bool valid_page_url(const std::string& value) {
    if (value.size() > Library::MAX_URL_BYTES || value.size() <= 39 ||
        !is_hex32(value.substr(0, 32)) || value[32] != ':' ||
        value.compare(33, 6, "/page/") != 0) return false;
    if (utf8_prefix(value, value.size()) != value) return false;
    return std::none_of(value.begin() + 33, value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

std::string utf8_prefix(const std::string& value, std::size_t max_bytes) {
    std::size_t i = 0;
    while (i < value.size() && i < max_bytes) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        std::size_t length = 1;
        if (c < 0x80) length = 1;
        else if (c >= 0xc2 && c <= 0xdf) length = 2;
        else if (c >= 0xe0 && c <= 0xef) length = 3;
        else if (c >= 0xf0 && c <= 0xf4) length = 4;
        else break;
        if (i + length > value.size() || i + length > max_bytes) break;
        bool continuation = true;
        for (std::size_t j = 1; j < length; ++j)
            continuation = continuation &&
                ((static_cast<uint8_t>(value[i + j]) & 0xc0) == 0x80);
        if (continuation && length == 3) {
            const uint8_t second = static_cast<uint8_t>(value[i + 1]);
            if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0)) continuation = false;
        } else if (continuation && length == 4) {
            const uint8_t second = static_cast<uint8_t>(value[i + 1]);
            if ((c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) continuation = false;
        }
        if (!continuation) break;
        i += length;
    }
    return value.substr(0, i);
}

char nibble(uint8_t value) { return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10); }

std::string hex_encode(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const uint8_t c : value) {
        encoded.push_back(nibble(c >> 4));
        encoded.push_back(nibble(c & 0x0f));
    }
    return encoded;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

bool hex_decode(const std::string& encoded, std::string& value, std::size_t max_bytes) {
    if ((encoded.size() & 1) != 0 || encoded.size() / 2 > max_bytes) return false;
    value.clear();
    value.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        const int high = hex_value(encoded[i]);
        const int low = hex_value(encoded[i + 1]);
        if (high < 0 || low < 0) return false;
        value.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool parse_u64(const std::string& value, uint64_t& parsed) {
    if (value.empty()) return false;
    parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    return true;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto next = line.find('\t', start);
        fields.push_back(line.substr(start, next == std::string::npos ? next : next - start));
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return fields;
}

template <typename Record>
void move_to_front(std::vector<Record>& records, std::size_t index) {
    if (index == 0) return;
    Record record = std::move(records[index]);
    records.erase(records.begin() + static_cast<std::ptrdiff_t>(index));
    records.insert(records.begin(), std::move(record));
}

template <typename Record>
bool make_room(std::vector<Record>& records, std::size_t limit) {
    if (records.size() < limit) return true;
    for (std::size_t i = records.size(); i-- > 0;) {
        if (!records[i].saved) {
            records.erase(records.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void sort_nodes(std::vector<NodeRecord>& records) {
    std::stable_sort(records.begin(), records.end(), [](const NodeRecord& left, const NodeRecord& right) {
        return left.last_heard > right.last_heard;
    });
}

void sort_pages(std::vector<PageRecord>& records) {
    std::stable_sort(records.begin(), records.end(), [](const PageRecord& left, const PageRecord& right) {
        return left.last_opened > right.last_opened;
    });
}

} // namespace

std::string sanitize_directory_name(const uint8_t* data, std::size_t size) {
    if (!data || size == 0) return {};
    const std::size_t bounded = std::min(size, Library::MAX_NAME_BYTES);
    std::string value(reinterpret_cast<const char*>(data), bounded);
    value = utf8_prefix(value, Library::MAX_NAME_BYTES);
    for (char& c : value) {
        const uint8_t byte = static_cast<uint8_t>(c);
        if (byte < 0x20 || byte == 0x7f) c = ' ';
    }
    while (!value.empty() && value.back() == ' ') value.pop_back();
    std::size_t first = 0;
    while (first < value.size() && value[first] == ' ') ++first;
    return value.substr(first);
}

std::string page_title(const std::string& fallback, const std::vector<std::string>& heading_runs) {
    std::string title;
    for (const auto& run : heading_runs) title += run;
    title = sanitize_directory_name(reinterpret_cast<const uint8_t*>(title.data()), title.size());
    if (title.empty()) title = fallback;
    return utf8_prefix(title, Library::MAX_TITLE_BYTES);
}

bool Library::hear_node(const std::string& destination_hex, const std::string& name,
                        uint64_t timestamp, uint8_t hops) {
    if (!is_hex32(destination_hex)) return false;
    const std::string normalized = lower_hex(destination_hex);
    const std::string safe_name = sanitize_directory_name(
        reinterpret_cast<const uint8_t*>(name.data()), name.size());
    for (std::size_t i = 0; i < _nodes.size(); ++i) {
        if (_nodes[i].destination_hex != normalized) continue;
        const bool changed = timestamp > _nodes[i].last_heard || _nodes[i].hops != hops ||
                             (!safe_name.empty() && _nodes[i].name != safe_name);
        if (!changed) return false;
        _nodes[i].last_heard = std::max(_nodes[i].last_heard, timestamp);
        _nodes[i].hops = hops;
        if (!safe_name.empty()) _nodes[i].name = safe_name;
        move_to_front(_nodes, i);
        sort_nodes(_nodes);
        return true;
    }
    if (!make_room(_nodes, MAX_NODES)) return false;
    _nodes.insert(_nodes.begin(), NodeRecord{normalized, safe_name, timestamp, hops, false});
    sort_nodes(_nodes);
    return true;
}

bool Library::record_page(const std::string& url, const std::string& title, uint64_t timestamp) {
    if (!valid_page_url(url)) return false;
    const std::string normalized = lower_hex(url.substr(0, 32)) + url.substr(32);
    const std::string safe_title = utf8_prefix(sanitize_directory_name(
        reinterpret_cast<const uint8_t*>(title.data()), title.size()), MAX_TITLE_BYTES);
    for (std::size_t i = 0; i < _pages.size(); ++i) {
        if (_pages[i].url != normalized) continue;
        const bool changed = timestamp > _pages[i].last_opened ||
                             (!safe_title.empty() && _pages[i].title != safe_title);
        if (!changed) return false;
        _pages[i].last_opened = std::max(_pages[i].last_opened, timestamp);
        if (!safe_title.empty()) _pages[i].title = safe_title;
        move_to_front(_pages, i);
        sort_pages(_pages);
        return true;
    }
    if (!make_room(_pages, MAX_PAGES)) return false;
    _pages.insert(_pages.begin(), PageRecord{normalized, safe_title, timestamp, false});
    sort_pages(_pages);
    return true;
}

bool Library::set_node_saved(const std::string& destination_hex, bool saved) {
    if (!is_hex32(destination_hex)) return false;
    const std::string normalized = lower_hex(destination_hex);
    for (auto& node : _nodes) {
        if (node.destination_hex == normalized) { node.saved = saved; return true; }
    }
    if (!saved || !make_room(_nodes, MAX_NODES)) return false;
    _nodes.insert(_nodes.begin(), NodeRecord{normalized, {}, 0, 0, true});
    sort_nodes(_nodes);
    return true;
}

bool Library::set_page_saved(const std::string& url, bool saved) {
    if (!valid_page_url(url)) return false;
    const std::string normalized = lower_hex(url.substr(0, 32)) + url.substr(32);
    for (auto& page : _pages) {
        if (page.url != normalized) continue;
        if (saved && !set_node_saved(normalized.substr(0, 32), true)) return false;
        page.saved = saved;
        if (!saved) {
            const std::string destination = normalized.substr(0, 32);
            const bool another_saved_page = std::any_of(_pages.begin(), _pages.end(),
                [&](const PageRecord& candidate) {
                    return candidate.saved && candidate.url.compare(0, 32, destination) == 0;
                });
            if (!another_saved_page) set_node_saved(destination, false);
        }
        return true;
    }
    if (!saved) return false;
    // Saving an unrecorded page is transactional: node admission and page
    // eviction must either both succeed or leave the live library untouched.
    Library candidate = *this;
    if (!candidate.set_node_saved(normalized.substr(0, 32), true) ||
        !candidate.record_page(normalized, {}, 0)) return false;
    for (auto& page : candidate._pages) {
        if (page.url == normalized) {
            page.saved = true;
            *this = std::move(candidate);
            return true;
        }
    }
    return false;
}

bool Library::remove_heard_node(const std::string& destination_hex) {
    if (!is_hex32(destination_hex)) return false;
    const std::string normalized = lower_hex(destination_hex);
    const auto found = std::find_if(_nodes.begin(), _nodes.end(), [&](const NodeRecord& node) {
        return node.destination_hex == normalized;
    });
    if (found == _nodes.end() || found->saved) return false;
    _nodes.erase(found);
    return true;
}

bool Library::node_saved(const std::string& destination_hex) const {
    if (!is_hex32(destination_hex)) return false;
    const std::string normalized = lower_hex(destination_hex);
    return std::any_of(_nodes.begin(), _nodes.end(), [&](const NodeRecord& node) {
        return node.destination_hex == normalized && node.saved;
    });
}

bool Library::page_saved(const std::string& url) const {
    if (!valid_page_url(url)) return false;
    const std::string normalized = lower_hex(url.substr(0, 32)) + url.substr(32);
    return std::any_of(_pages.begin(), _pages.end(), [&](const PageRecord& page) {
        return page.url == normalized && page.saved;
    });
}

ExternalVector<uint8_t> Library::encode() const {
    ExternalVector<uint8_t> output;
    output.reserve(4096);
    const auto append = [&](const std::string& value) {
        output.insert(output.end(), value.begin(), value.end());
    };
    append("PXNN1\n");
    for (const auto& node : _nodes) {
        append("N\t" + node.destination_hex + "\t" + hex_encode(node.name) + "\t" +
               std::to_string(node.last_heard) + "\t" + std::to_string(node.hops) + "\t" +
               (node.saved ? "1\n" : "0\n"));
        if (output.size() > MAX_ENCODED_BYTES) return {};
    }
    for (const auto& page : _pages) {
        append("P\t" + hex_encode(page.url) + "\t" + hex_encode(page.title) + "\t" +
               std::to_string(page.last_opened) + "\t" + (page.saved ? "1\n" : "0\n"));
        if (output.size() > MAX_ENCODED_BYTES) return {};
    }
    return output;
}

bool Library::decode(const uint8_t* data, std::size_t size) {
    if (!data || size < 6 || size > MAX_ENCODED_BYTES) return false;
    if (!std::equal(data, data + 6, reinterpret_cast<const uint8_t*>("PXNN1\n")) || data[size - 1] != '\n') return false;

    Library candidate;
    std::size_t start = 6;
    while (start < size) {
        std::size_t end = start;
        while (end < size && data[end] != '\n') ++end;
        if (end == size || end - start > 1400) return false;
        const std::string line(reinterpret_cast<const char*>(data + start), end - start);
        start = end + 1;
        if (line.empty()) continue;
        const auto fields = split_tabs(line);
        if (fields[0] == "N") {
            if (fields.size() != 6 || candidate._nodes.size() >= MAX_NODES ||
                !is_hex32(fields[1]) || (fields[5] != "0" && fields[5] != "1")) return false;
            std::string name;
            uint64_t timestamp = 0, hops = 0;
            if (!hex_decode(fields[2], name, MAX_NAME_BYTES) ||
                !parse_u64(fields[3], timestamp) || !parse_u64(fields[4], hops) || hops > 255) return false;
            const std::string destination = lower_hex(fields[1]);
            if (candidate.node_saved(destination) || std::any_of(candidate._nodes.begin(), candidate._nodes.end(),
                    [&](const NodeRecord& node) { return node.destination_hex == destination; })) return false;
            const std::string canonical_name = sanitize_directory_name(
                reinterpret_cast<const uint8_t*>(name.data()), name.size());
            if (canonical_name != name) return false;
            candidate._nodes.push_back(NodeRecord{destination, canonical_name, timestamp,
                static_cast<uint8_t>(hops), fields[5] == "1"});
        } else if (fields[0] == "P") {
            if (fields.size() != 5 || candidate._pages.size() >= MAX_PAGES ||
                (fields[4] != "0" && fields[4] != "1")) return false;
            std::string url, title;
            uint64_t timestamp = 0;
            if (!hex_decode(fields[1], url, MAX_URL_BYTES) || !valid_page_url(url) ||
                !hex_decode(fields[2], title, MAX_TITLE_BYTES) || !parse_u64(fields[3], timestamp)) return false;
            const std::string normalized = lower_hex(url.substr(0, 32)) + url.substr(32);
            if (std::any_of(candidate._pages.begin(), candidate._pages.end(),
                    [&](const PageRecord& page) { return page.url == normalized; })) return false;
            const std::string canonical_title = sanitize_directory_name(
                reinterpret_cast<const uint8_t*>(title.data()), title.size());
            if (canonical_title != title) return false;
            candidate._pages.push_back(PageRecord{normalized, canonical_title, timestamp, fields[4] == "1"});
        } else return false;
    }
    _nodes = std::move(candidate._nodes);
    _pages = std::move(candidate._pages);
    return true;
}

void Library::clear() {
    _nodes.clear();
    _pages.clear();
}

} // namespace UI::LXMF::NomadNet
