#pragma once
#include <cstdlib>
inline void* ps_malloc(std::size_t bytes) { return std::malloc(bytes); }
