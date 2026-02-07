/**
 * @file BLEOperationQueue.h
 * @brief GATT operation queue for serializing BLE operations
 *
 * BLE stacks typically do not queue operations internally - attempting to
 * perform multiple GATT operations simultaneously leads to failures or
 * undefined behavior. This queue ensures operations are processed one at
 * a time in order.
 *
 * Platform implementations inherit from this class and implement
 * executeOperation() to perform the actual BLE stack calls.
 */
#pragma once

#include "BLETypes.h"
#include "Bytes.h"
#include "Utilities/OS.h"

#include <queue>
#include <functional>

namespace RNS { namespace BLE {

/**
 * @brief Base class for GATT operation queuing
 *
 * Subclasses must implement executeOperation() to perform the actual
 * BLE stack calls. Call process() from the main loop to execute queued
 * operations, and complete() from BLE callbacks to signal completion.
 */
class BLEOperationQueue {
public:
    BLEOperationQueue();
    virtual ~BLEOperationQueue() = default;

    /**
     * @brief Add operation to queue
     *
     * @param op Operation to queue
     */
    void enqueue(GATTOperation op);

    /**
     * @brief Process queue - call from loop()
     *
     * Starts the next operation if none is in progress.
     * @return true if an operation was started
     */
    bool process();

    /**
     * @brief Mark current operation complete
     *
     * Call this from BLE callbacks when an operation completes.
     *
     * @param result Operation result
     * @param response_data Response data (for reads)
     */
    void complete(OperationResult result, const Bytes& response_data = Bytes());

    /**
     * @brief Check if operation is in progress
     */
    bool isBusy() const { return _has_current_op; }

    /**
     * @brief Get current operation (if any)
     * @return Pointer to current operation, or nullptr if none
     */
    const GATTOperation* currentOperation() const {
        return _has_current_op ? &_current_op : nullptr;
    }

    /**
     * @brief Clear all pending operations for a connection
     *
     * Call this when a connection is terminated to remove orphaned operations.
     *
     * @param conn_handle Connection handle
     */
    void clearForConnection(uint16_t conn_handle);

    /**
     * @brief Clear entire queue
     */
    void clear();

    /**
     * @brief Get queue depth
     */
    size_t depth() const { return _queue.size(); }

    /**
     * @brief Set operation timeout
     * @param timeout_ms Timeout in milliseconds
     */
    void setTimeout(uint32_t timeout_ms) { _default_timeout_ms = timeout_ms; }

protected:
    /**
     * @brief Execute a single operation - implement in subclass
     *
     * Subclasses must implement this to call platform-specific BLE APIs.
     * Return true if the operation was started successfully.
     * Call complete() from the BLE callback when the operation finishes.
     *
     * @param op Operation to execute
     * @return true if operation was started
     */
    virtual bool executeOperation(const GATTOperation& op) = 0;

private:
    /**
     * @brief Check for timeout on current operation
     */
    void checkTimeout();

    std::queue<GATTOperation> _queue;
    GATTOperation _current_op;
    bool _has_current_op = false;
    uint32_t _default_timeout_ms = 5000;
};

/**
 * @brief Helper class for building GATT operations
 */
class GATTOperationBuilder {
public:
    GATTOperationBuilder& read(uint16_t conn_handle, uint16_t char_handle);
    GATTOperationBuilder& write(uint16_t conn_handle, uint16_t char_handle, const Bytes& data);
    GATTOperationBuilder& writeNoResponse(uint16_t conn_handle, uint16_t char_handle, const Bytes& data);
    GATTOperationBuilder& enableNotify(uint16_t conn_handle);
    GATTOperationBuilder& disableNotify(uint16_t conn_handle);
    GATTOperationBuilder& requestMTU(uint16_t conn_handle, uint16_t mtu);
    GATTOperationBuilder& withTimeout(uint32_t timeout_ms);
    GATTOperationBuilder& withCallback(std::function<void(OperationResult, const Bytes&)> callback);

    GATTOperation build();

private:
    GATTOperation _op;
};

}} // namespace RNS::BLE
