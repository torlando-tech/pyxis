#include <cstdlib>
#include <new>

#include "NomadNetForm.h"

namespace {
bool fail_next_allocation = false;
}

void* operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    using namespace UI::LXMF::NomadNet;
    FormState state;
    ExternalVector<uint8_t> output;
    fail_next_allocation = true;
    try {
        const auto result = state.encode("", output);
        return result == FormEncodeResult::OUTPUT_TOO_LARGE && output.empty() ? 0 : 1;
    } catch (...) {
        return 2;
    }
}
