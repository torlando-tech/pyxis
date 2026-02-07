/**
 * @file BLEOperationQueue.cpp
 * @brief GATT operation queue implementation
 */

#include "BLEOperationQueue.h"
#include "Log.h"

namespace RNS { namespace BLE {

BLEOperationQueue::BLEOperationQueue() {
}

void BLEOperationQueue::enqueue(GATTOperation op) {
    op.queued_at = Utilities::OS::time();

    if (op.timeout_ms == 0) {
        op.timeout_ms = _default_timeout_ms;
    }

    _queue.push(std::move(op));

    TRACE("BLEOperationQueue: Enqueued operation, queue depth: " +
          std::to_string(_queue.size()));
}

bool BLEOperationQueue::process() {
    // Check for timeout on current operation
    if (_has_current_op) {
        checkTimeout();
        return false;  // Still busy
    }

    // Nothing to process
    if (_queue.empty()) {
        return false;
    }

    // Dequeue next operation
    _current_op = std::move(_queue.front());
    _has_current_op = true;
    _queue.pop();

    GATTOperation& op = _current_op;
    op.started_at = Utilities::OS::time();

    TRACE("BLEOperationQueue: Starting operation type " +
          std::to_string(static_cast<int>(op.type)));

    // Execute the operation (implemented by subclass)
    bool started = executeOperation(op);

    if (!started) {
        // Operation failed to start - call callback with error
        if (op.callback) {
            op.callback(OperationResult::ERROR, Bytes());
        }
        _has_current_op = false;
        return false;
    }

    return true;
}

void BLEOperationQueue::complete(OperationResult result, const Bytes& response_data) {
    if (!_has_current_op) {
        WARNING("BLEOperationQueue: complete() called with no current operation");
        return;
    }

    GATTOperation& op = _current_op;

    double duration = Utilities::OS::time() - op.started_at;
    TRACE("BLEOperationQueue: Operation completed in " +
          std::to_string(static_cast<int>(duration * 1000)) + "ms, result: " +
          std::to_string(static_cast<int>(result)));

    // Invoke callback
    if (op.callback) {
        op.callback(result, response_data);
    }

    // Clear current operation
    _has_current_op = false;
}

void BLEOperationQueue::clearForConnection(uint16_t conn_handle) {
    // Create temporary queue for non-matching operations
    std::queue<GATTOperation> remaining;

    while (!_queue.empty()) {
        GATTOperation op = std::move(_queue.front());
        _queue.pop();

        if (op.conn_handle != conn_handle) {
            remaining.push(std::move(op));
        } else {
            // Cancel this operation
            if (op.callback) {
                op.callback(OperationResult::DISCONNECTED, Bytes());
            }
        }
    }

    _queue = std::move(remaining);

    // Also cancel current operation if it matches
    if (_has_current_op && _current_op.conn_handle == conn_handle) {
        if (_current_op.callback) {
            _current_op.callback(OperationResult::DISCONNECTED, Bytes());
        }
        _has_current_op = false;
    }

    TRACE("BLEOperationQueue: Cleared operations for connection " +
          std::to_string(conn_handle));
}

void BLEOperationQueue::clear() {
    // Cancel all pending operations
    while (!_queue.empty()) {
        GATTOperation op = std::move(_queue.front());
        _queue.pop();

        if (op.callback) {
            op.callback(OperationResult::DISCONNECTED, Bytes());
        }
    }

    // Cancel current operation
    if (_has_current_op) {
        if (_current_op.callback) {
            _current_op.callback(OperationResult::DISCONNECTED, Bytes());
        }
        _has_current_op = false;
    }

    TRACE("BLEOperationQueue: Cleared all operations");
}

void BLEOperationQueue::checkTimeout() {
    if (!_has_current_op) {
        return;
    }

    GATTOperation& op = _current_op;
    double elapsed = Utilities::OS::time() - op.started_at;
    double timeout_sec = op.timeout_ms / 1000.0;

    if (elapsed > timeout_sec) {
        WARNING("BLEOperationQueue: Operation timed out after " +
                std::to_string(static_cast<int>(elapsed * 1000)) + "ms");

        // Complete with timeout error
        complete(OperationResult::TIMEOUT, Bytes());
    }
}

//=============================================================================
// GATTOperationBuilder
//=============================================================================

GATTOperationBuilder& GATTOperationBuilder::read(uint16_t conn_handle, uint16_t char_handle) {
    _op.type = OperationType::READ;
    _op.conn_handle = conn_handle;
    _op.char_handle = char_handle;
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::write(uint16_t conn_handle, uint16_t char_handle,
                                                   const Bytes& data) {
    _op.type = OperationType::WRITE;
    _op.conn_handle = conn_handle;
    _op.char_handle = char_handle;
    _op.data = data;
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::writeNoResponse(uint16_t conn_handle,
                                                             uint16_t char_handle,
                                                             const Bytes& data) {
    _op.type = OperationType::WRITE_NO_RESPONSE;
    _op.conn_handle = conn_handle;
    _op.char_handle = char_handle;
    _op.data = data;
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::enableNotify(uint16_t conn_handle) {
    _op.type = OperationType::NOTIFY_ENABLE;
    _op.conn_handle = conn_handle;
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::disableNotify(uint16_t conn_handle) {
    _op.type = OperationType::NOTIFY_DISABLE;
    _op.conn_handle = conn_handle;
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::requestMTU(uint16_t conn_handle, uint16_t mtu) {
    _op.type = OperationType::MTU_REQUEST;
    _op.conn_handle = conn_handle;
    // Store requested MTU in data (as 2-byte big-endian)
    _op.data = Bytes(2);
    uint8_t* ptr = _op.data.writable(2);
    ptr[0] = static_cast<uint8_t>((mtu >> 8) & 0xFF);
    ptr[1] = static_cast<uint8_t>(mtu & 0xFF);
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::withTimeout(uint32_t timeout_ms) {
    _op.timeout_ms = timeout_ms;
    return *this;
}

GATTOperationBuilder& GATTOperationBuilder::withCallback(
    std::function<void(OperationResult, const Bytes&)> callback) {
    _op.callback = callback;
    return *this;
}

GATTOperation GATTOperationBuilder::build() {
    return std::move(_op);
}

}} // namespace RNS::BLE
