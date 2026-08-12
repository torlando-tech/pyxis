#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "LittleFSInitializationPolicy.h"

using Storage::mount_or_initialize_erased_littlefs;

static int passed = 0;
static int failed = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failed; std::cerr << "FAIL line " << __LINE__ << ": " #expr "\n"; } } while (0)

struct Harness {
    std::vector<uint8_t> partition;
    bool mount_result = false;
    bool resolve_result = true;
    bool read_result = true;
    bool format_mount_result = true;
    int mount_calls = 0;
    int resolve_calls = 0;
    int read_calls = 0;
    int format_mount_calls = 0;

    bool run() {
        return mount_or_initialize_erased_littlefs(
            [this]() {
                ++mount_calls;
                return mount_result;
            },
            [this](std::size_t& size) {
                ++resolve_calls;
                if (!resolve_result) return false;
                size = partition.size();
                return true;
            },
            [this](std::size_t offset, uint8_t* output, std::size_t size) {
                ++read_calls;
                if (!read_result || offset + size > partition.size()) return false;
                for (std::size_t i = 0; i < size; ++i) output[i] = partition[offset + i];
                return true;
            },
            [this]() {
                ++format_mount_calls;
                return format_mount_result;
            });
    }
};

int main() {
    {
        Harness harness;
        harness.mount_result = true;
        CHECK(harness.run());
        CHECK(harness.mount_calls == 1);
        CHECK(harness.resolve_calls == 0);
        CHECK(harness.read_calls == 0);
        CHECK(harness.format_mount_calls == 0);
    }
    {
        Harness harness;
        harness.partition.assign(8193, 0xFF);
        CHECK(harness.run());
        CHECK(harness.read_calls == 3);
        CHECK(harness.format_mount_calls == 1);
    }
    {
        Harness harness;
        harness.partition.assign(8193, 0xFF);
        harness.partition.back() = 0x00;
        CHECK(!harness.run());
        CHECK(harness.read_calls == 3);
        CHECK(harness.format_mount_calls == 0);
    }
    {
        Harness harness;
        harness.resolve_result = false;
        CHECK(!harness.run());
        CHECK(harness.read_calls == 0);
        CHECK(harness.format_mount_calls == 0);
    }
    {
        Harness harness;
        harness.partition.assign(4096, 0xFF);
        harness.read_result = false;
        CHECK(!harness.run());
        CHECK(harness.read_calls == 1);
        CHECK(harness.format_mount_calls == 0);
    }
    {
        Harness harness;
        harness.partition.assign(4096, 0xFF);
        harness.format_mount_result = false;
        CHECK(!harness.run());
        CHECK(harness.format_mount_calls == 1);
    }

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
