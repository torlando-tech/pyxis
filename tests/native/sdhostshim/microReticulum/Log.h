// Host shim for <microReticulum/Log.h> — SDAccess.cpp pulls in
// `using namespace RNS;` for its logging helpers.
#ifndef SDHOSTSHIM_MICRORETICULUM_LOG_H
#define SDHOSTSHIM_MICRORETICULUM_LOG_H

namespace RNS {
inline void log(const char*) {}
inline void logf(const char*, ...) {}
inline void logError(const char*) {}
inline void logWarning(const char*) {}
}

#endif
