#include "RouterLock.h"

#ifdef ARDUINO

namespace UI {
namespace LXMF {

SemaphoreHandle_t RouterLock::mutex_ = nullptr;

bool RouterLock::initialize() {
    if (mutex_) return true;
    mutex_ = xSemaphoreCreateRecursiveMutex();
    return mutex_ != nullptr;
}

RouterLock::RouterLock(TickType_t timeout) {
    if (mutex_) {
        acquired_ = xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
    }
}

RouterLock::~RouterLock() {
    if (acquired_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace LXMF
}  // namespace UI

#endif
