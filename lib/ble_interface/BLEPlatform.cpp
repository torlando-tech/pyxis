/**
 * @file BLEPlatform.cpp
 * @brief BLE Platform factory implementation
 */

#include "BLEPlatform.h"
#include "Log.h"

// Include platform implementations based on compile-time detection
#if defined(ESP32) && (defined(USE_NIMBLE) || defined(CONFIG_BT_NIMBLE_ENABLED))
#include "platforms/NimBLEPlatform.h"
#endif

#if defined(ESP32) && defined(USE_BLUEDROID)
#include "platforms/BluedroidPlatform.h"
#endif

#if defined(ZEPHYR) || defined(CONFIG_BT)
// Future: #include "platforms/ZephyrPlatform.h"
#endif

namespace RNS { namespace BLE {

IBLEPlatform::Ptr BLEPlatformFactory::create() {
    return create(getDetectedPlatform());
}

IBLEPlatform::Ptr BLEPlatformFactory::create(PlatformType type) {
    switch (type) {
#if defined(ESP32) && (defined(USE_NIMBLE) || defined(CONFIG_BT_NIMBLE_ENABLED))
        case PlatformType::NIMBLE_ARDUINO:
            INFO("BLEPlatformFactory: Creating NimBLE platform");
            return std::make_shared<NimBLEPlatform>();
#endif

#if defined(ESP32) && defined(USE_BLUEDROID)
        case PlatformType::ESP_IDF:
            INFO("BLEPlatformFactory: Creating Bluedroid platform");
            return std::make_shared<BluedroidPlatform>();
#endif

#if defined(ZEPHYR) || defined(CONFIG_BT)
        case PlatformType::ZEPHYR:
            // Future: return std::make_shared<ZephyrPlatform>();
            ERROR("BLEPlatformFactory: Zephyr platform not yet implemented");
            return nullptr;
#endif

        default:
            ERROR("BLEPlatformFactory: No platform available for type " +
                  std::to_string(static_cast<int>(type)));
            return nullptr;
    }
}

PlatformType BLEPlatformFactory::getDetectedPlatform() {
#if defined(ESP32) && defined(USE_BLUEDROID)
    // Bluedroid takes priority when explicitly selected
    return PlatformType::ESP_IDF;
#elif defined(ESP32) && (defined(USE_NIMBLE) || defined(CONFIG_BT_NIMBLE_ENABLED))
    return PlatformType::NIMBLE_ARDUINO;
#elif defined(ZEPHYR) || defined(CONFIG_BT)
    return PlatformType::ZEPHYR;
#else
    return PlatformType::NONE;
#endif
}

}} // namespace RNS::BLE
