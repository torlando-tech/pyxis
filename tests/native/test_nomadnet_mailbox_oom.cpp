#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <new>
#include <vector>
#include "NomadNetMailbox.h"

using UI::LXMF::NomadNet::AsyncMailbox;

namespace {
bool fail_allocations = false;
}

void* operator new(std::size_t size) {
    if (fail_allocations) throw std::bad_alloc();
    if (void* value = std::malloc(size)) return value;
    throw std::bad_alloc();
}

void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

int main() {
    AsyncMailbox mailbox;
    const std::vector<std::uint8_t> link{1};
    const std::vector<std::uint8_t> request{2};
    const std::vector<std::uint8_t> response(4096, 0x5a);
    mailbox.begin(link, 77);
    mailbox.expect_request(request);

    bool accepted = false;
    bool escaped = false;
    fail_allocations = true;
    try {
        accepted = mailbox.publish_response(
            request, response.data(), response.size(), response.size());
    } catch (const std::bad_alloc&) {
        escaped = true;
    }
    fail_allocations = false;

    AsyncMailbox::Event event;
    const bool bounded_failure = mailbox.take(event) &&
        event.kind == AsyncMailbox::Kind::FAILED && event.data.empty() &&
        event.transfer_size == 0 && event.generation == 77;
    if (escaped || !accepted || !bounded_failure) {
        std::cerr << "escaped=" << escaped << " accepted=" << accepted
                  << " bounded_failure=" << bounded_failure << "\n";
        return 1;
    }
    std::cout << "passed\n";
    return 0;
}
