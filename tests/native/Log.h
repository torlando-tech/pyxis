// Native-test shim for microReticulum's Log.h. Replaces all logging macros
// with no-ops so we can compile pyxis source files standalone.
//
// Real Log.h has a substantial logger with levels, callbacks, formatting,
// etc. None of that is relevant for unit tests; we just want the compile
// to succeed.
#pragma once

#include <cstdio>

#ifndef TRACE
#define TRACE(...)        ((void)0)
#endif
#ifndef DEBUG
#define DEBUG(...)        ((void)0)
#endif
#ifndef INFO
#define INFO(...)         ((void)0)
#endif
#ifndef WARN
#define WARN(...)         ((void)0)
#endif
#ifndef WARNING
#define WARNING(...)      ((void)0)
#endif
#ifndef ERROR
#define ERROR(...)        ((void)0)
#endif

namespace RNS {
namespace Log {
    inline void log(const char*) {}
    inline void log(const char*, ...) {}
}  // namespace Log
}  // namespace RNS
