#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#include "Telemetry/LocationPersistenceLittleFS.h"

namespace {
int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)
}

int main() {
    char directory[] = "/tmp/pyxis-location-fs-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    char live[160]{};
    char temp[160]{};
    char backup[160]{};
    std::snprintf(live, sizeof(live), "%s/live", directory);
    std::snprintf(temp, sizeof(temp), "%s/temp", directory);
    std::snprintf(backup, sizeof(backup), "%s/backup", directory);

    Telemetry::LocationPersistenceLittleFS unavailable(
        false, live, temp, backup);
    bool exists = true;
    CHECK(!unavailable.stat(Telemetry::LocationPersistenceSlot::LIVE, exists));
    CHECK(!exists);

    Telemetry::LocationPersistenceLittleFS storage(true, live, temp, backup);
    CHECK(storage.stat(Telemetry::LocationPersistenceSlot::TEMP, exists));
    CHECK(!exists);
    const uint8_t bytes[] = {1, 2, 3, 4, 5};
    CHECK(storage.write(Telemetry::LocationPersistenceSlot::TEMP,
                        bytes, sizeof(bytes)));
    CHECK(storage.stat(Telemetry::LocationPersistenceSlot::TEMP, exists));
    CHECK(exists);
    uint8_t output[8]{};
    std::size_t size = 0;
    CHECK(storage.read(Telemetry::LocationPersistenceSlot::TEMP,
                       output, sizeof(output), size));
    CHECK(size == sizeof(bytes));
    CHECK(std::memcmp(bytes, output, sizeof(bytes)) == 0);
    CHECK(storage.rename(Telemetry::LocationPersistenceSlot::TEMP,
                         Telemetry::LocationPersistenceSlot::LIVE));
    CHECK(storage.remove(Telemetry::LocationPersistenceSlot::LIVE));

    std::string too_long(5000, 'x');
    Telemetry::LocationPersistenceLittleFS broken(
        true, too_long.c_str(), temp, backup);
    exists = true;
    CHECK(!broken.stat(Telemetry::LocationPersistenceSlot::LIVE, exists));
    CHECK(!exists);

    ::rmdir(directory);
    std::cout << "location persistence POSIX adapter: " << passed
              << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
