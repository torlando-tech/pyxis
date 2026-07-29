#ifndef UI_LXMF_ROUTER_LOCK_H
#define UI_LXMF_ROUTER_LOCK_H

#ifdef ARDUINO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace UI {
namespace LXMF {

// Shared recursive lock for every task that enters Reticulum/LXMRouter state.
// initialize() is called during single-threaded setup before the LVGL task.
class RouterLock {
public:
    static bool initialize();

    explicit RouterLock(TickType_t timeout = portMAX_DELAY);
    ~RouterLock();

    RouterLock(const RouterLock&) = delete;
    RouterLock& operator=(const RouterLock&) = delete;

    bool acquired() const { return acquired_; }

private:
    static SemaphoreHandle_t mutex_;
    bool acquired_ = false;
};

}  // namespace LXMF
}  // namespace UI

#endif
#endif
