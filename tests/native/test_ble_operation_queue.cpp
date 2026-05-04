// Native unit tests for BLEOperationQueue.
//
// The queue serializes GATT ops (BLE stacks don't queue internally) and is
// driven from the BLE loop task. The deferred-disconnect path documented
// in pyxis MEMORY routes through this — operations get cancelled with
// DISCONNECTED when a peer drops, and timeouts complete with TIMEOUT.
//
// Tests:
//   - enqueue increments depth
//   - process dispatches to executeOperation and sets isBusy
//   - complete invokes the op callback with result+data and clears busy
//   - complete() with no current op is a no-op (just warns)
//   - process while busy returns false and does not start a new op
//   - executeOperation returning false → callback fires with ERROR, busy clears
//   - FIFO ordering: enqueue order == execute order
//   - timeout fires after default+per-op timeout
//   - clearForConnection cancels matching pending + current ops, leaves others
//   - clear() cancels all
//   - GATTOperationBuilder fluent API populates fields correctly

#include "../../lib/ble_interface/BLEOperationQueue.h"
#include "Utilities/OS.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using RNS::Bytes;
using RNS::BLE::BLEOperationQueue;
using RNS::BLE::GATTOperation;
using RNS::BLE::GATTOperationBuilder;
using RNS::BLE::OperationResult;
using RNS::BLE::OperationType;

// ── minimal test framework ──

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(actual, expected)                                            \
    do {                                                                       \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (!(_a == _e)) {                                                     \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: %s != %s",                 \
                          __FILE__, __LINE__, #actual, #expected);             \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: expected %s",              \
                          __FILE__, __LINE__, #cond);                          \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define RUN(name)                                                              \
    do {                                                                       \
        try {                                                                  \
            name();                                                            \
            ++g_pass;                                                          \
            std::printf("PASS %s\n", #name);                                   \
        } catch (const std::exception& e) {                                    \
            ++g_fail;                                                          \
            std::printf("FAIL %s: %s\n", #name, e.what());                     \
        }                                                                      \
    } while (0)

// Test fixture: subclass that records executeOperation calls and lets the
// test choose whether to "succeed" or "fail" each one.
class TestQueue : public BLEOperationQueue {
public:
    std::vector<OperationType> executed_types;
    std::vector<uint16_t> executed_handles;
    bool fail_next_execute = false;

    bool executeOperation(const GATTOperation& op) override {
        executed_types.push_back(op.type);
        executed_handles.push_back(op.conn_handle);
        if (fail_next_execute) {
            fail_next_execute = false;
            return false;
        }
        return true;
    }
};

// Helper to build a basic READ op with a callback that records its result.
struct CallbackRecord {
    bool fired = false;
    OperationResult result = OperationResult::SUCCESS;
    Bytes data;
};

static GATTOperation make_read_op(uint16_t conn_handle,
                                   uint16_t char_handle,
                                   CallbackRecord* rec) {
    return GATTOperationBuilder()
        .read(conn_handle, char_handle)
        .withCallback([rec](OperationResult r, const Bytes& d) {
            rec->fired = true;
            rec->result = r;
            rec->data = d;
        })
        .build();
}

// ── tests ──

static void enqueue_increments_depth() {
    TestQueue q;
    EXPECT_EQ(q.depth(), (size_t)0);
    EXPECT_TRUE(!q.isBusy());

    CallbackRecord rec;
    q.enqueue(make_read_op(1, 0x100, &rec));
    EXPECT_EQ(q.depth(), (size_t)1);
    EXPECT_TRUE(!q.isBusy());   // not busy until process()
}

static void process_starts_operation_and_marks_busy() {
    TestQueue q;
    CallbackRecord rec;
    q.enqueue(make_read_op(1, 0x100, &rec));

    EXPECT_TRUE(q.process());
    EXPECT_TRUE(q.isBusy());
    EXPECT_EQ(q.depth(), (size_t)0);
    EXPECT_EQ(q.executed_types.size(), (size_t)1);
    EXPECT_EQ(q.executed_types[0], OperationType::READ);
}

static void complete_fires_callback_and_clears_busy() {
    TestQueue q;
    CallbackRecord rec;
    q.enqueue(make_read_op(1, 0x100, &rec));
    q.process();

    Bytes payload;
    payload.append((uint8_t)0xAA); payload.append((uint8_t)0xBB);
    q.complete(OperationResult::SUCCESS, payload);

    EXPECT_TRUE(rec.fired);
    EXPECT_EQ(rec.result, OperationResult::SUCCESS);
    EXPECT_EQ(rec.data.size(), (size_t)2);
    EXPECT_EQ(rec.data.data()[0], (uint8_t)0xAA);
    EXPECT_TRUE(!q.isBusy());
}

static void complete_with_no_current_op_is_noop() {
    TestQueue q;
    // Should not crash, should not fire any callback (none registered).
    q.complete(OperationResult::SUCCESS, Bytes());
    EXPECT_TRUE(!q.isBusy());
}

static void process_while_busy_does_not_start_another() {
    TestQueue q;
    CallbackRecord rec1, rec2;
    q.enqueue(make_read_op(1, 0x100, &rec1));
    q.enqueue(make_read_op(1, 0x101, &rec2));

    EXPECT_TRUE(q.process());
    EXPECT_TRUE(q.isBusy());
    EXPECT_EQ(q.executed_types.size(), (size_t)1);

    // Second process must not start the queued op.
    EXPECT_TRUE(!q.process());
    EXPECT_EQ(q.executed_types.size(), (size_t)1);

    // Complete the first; next process should start the second.
    q.complete(OperationResult::SUCCESS, Bytes());
    EXPECT_TRUE(q.process());
    EXPECT_EQ(q.executed_types.size(), (size_t)2);
}

static void execute_returning_false_fires_error_and_clears_busy() {
    TestQueue q;
    CallbackRecord rec;
    q.fail_next_execute = true;
    q.enqueue(make_read_op(1, 0x100, &rec));

    EXPECT_TRUE(!q.process());          // start failed
    EXPECT_TRUE(rec.fired);
    EXPECT_EQ(rec.result, OperationResult::ERROR);
    EXPECT_TRUE(!q.isBusy());
}

static void fifo_ordering_preserved() {
    TestQueue q;
    CallbackRecord r1, r2, r3;
    q.enqueue(make_read_op(1, 0x100, &r1));
    q.enqueue(make_read_op(2, 0x200, &r2));
    q.enqueue(make_read_op(3, 0x300, &r3));

    q.process(); q.complete(OperationResult::SUCCESS, Bytes());
    q.process(); q.complete(OperationResult::SUCCESS, Bytes());
    q.process(); q.complete(OperationResult::SUCCESS, Bytes());

    EXPECT_EQ(q.executed_handles.size(), (size_t)3);
    EXPECT_EQ(q.executed_handles[0], (uint16_t)1);
    EXPECT_EQ(q.executed_handles[1], (uint16_t)2);
    EXPECT_EQ(q.executed_handles[2], (uint16_t)3);
}

static void timeout_completes_with_timeout_result() {
    TestQueue q;

    // GATTOperation::timeout_ms defaults to 5000 in the struct, so setTimeout()
    // alone won't apply. Build the op with an explicit per-op timeout.
    CallbackRecord rec;
    auto op = GATTOperationBuilder()
        .read(1, 0x100)
        .withTimeout(500)   // 0.5s
        .withCallback([&rec](OperationResult r, const Bytes& d) {
            rec.fired = true; rec.result = r; rec.data = d;
        })
        .build();

    RNS::Utilities::OS::set_fake_time(0.0);
    q.enqueue(std::move(op));
    q.process();
    EXPECT_TRUE(q.isBusy());

    // Advance past timeout; process() should call checkTimeout and fire complete.
    RNS::Utilities::OS::set_fake_time(1.0);
    q.process();
    EXPECT_TRUE(rec.fired);
    EXPECT_EQ(rec.result, OperationResult::TIMEOUT);
    EXPECT_TRUE(!q.isBusy());

    RNS::Utilities::OS::clear_fake_time();
}

static void clear_for_connection_cancels_matching_pending() {
    TestQueue q;
    CallbackRecord r_a, r_b1, r_b2;
    q.enqueue(make_read_op(0xA, 0x100, &r_a));
    q.enqueue(make_read_op(0xB, 0x200, &r_b1));
    q.enqueue(make_read_op(0xB, 0x201, &r_b2));
    EXPECT_EQ(q.depth(), (size_t)3);

    q.clearForConnection(0xB);
    EXPECT_EQ(q.depth(), (size_t)1);     // only conn 0xA remains
    EXPECT_TRUE(r_b1.fired);
    EXPECT_EQ(r_b1.result, OperationResult::DISCONNECTED);
    EXPECT_TRUE(r_b2.fired);
    EXPECT_TRUE(!r_a.fired);             // unrelated peer untouched
}

static void clear_for_connection_cancels_current_op_if_matching() {
    TestQueue q;
    CallbackRecord rec;
    q.enqueue(make_read_op(0xC, 0x100, &rec));
    q.process();
    EXPECT_TRUE(q.isBusy());

    q.clearForConnection(0xC);
    EXPECT_TRUE(rec.fired);
    EXPECT_EQ(rec.result, OperationResult::DISCONNECTED);
    EXPECT_TRUE(!q.isBusy());
}

static void clear_cancels_everything() {
    TestQueue q;
    CallbackRecord r1, r2, r3;
    q.enqueue(make_read_op(1, 0x100, &r1));
    q.enqueue(make_read_op(2, 0x200, &r2));
    q.process();   // r1 becomes current
    q.enqueue(make_read_op(3, 0x300, &r3));

    q.clear();
    EXPECT_EQ(q.depth(), (size_t)0);
    EXPECT_TRUE(!q.isBusy());
    EXPECT_TRUE(r1.fired);
    EXPECT_TRUE(r2.fired);
    EXPECT_TRUE(r3.fired);
    EXPECT_EQ(r1.result, OperationResult::DISCONNECTED);
    EXPECT_EQ(r2.result, OperationResult::DISCONNECTED);
    EXPECT_EQ(r3.result, OperationResult::DISCONNECTED);
}

static void builder_populates_write_op_correctly() {
    Bytes data;
    data.append((uint8_t)0xDE); data.append((uint8_t)0xAD);

    GATTOperation op = GATTOperationBuilder()
        .write(7, 0x500, data)
        .withTimeout(3000)
        .build();

    EXPECT_EQ(op.type, OperationType::WRITE);
    EXPECT_EQ(op.conn_handle, (uint16_t)7);
    EXPECT_EQ(op.char_handle, (uint16_t)0x500);
    EXPECT_EQ(op.data.size(), (size_t)2);
    EXPECT_EQ(op.timeout_ms, (uint32_t)3000);
}

static void builder_mtu_request_encodes_big_endian() {
    GATTOperation op = GATTOperationBuilder()
        .requestMTU(1, 517)   // 517 = 0x0205
        .build();
    EXPECT_EQ(op.type, OperationType::MTU_REQUEST);
    EXPECT_EQ(op.data.size(), (size_t)2);
    EXPECT_EQ(op.data.data()[0], (uint8_t)0x02);
    EXPECT_EQ(op.data.data()[1], (uint8_t)0x05);
}

int main() {
    RUN(enqueue_increments_depth);
    RUN(process_starts_operation_and_marks_busy);
    RUN(complete_fires_callback_and_clears_busy);
    RUN(complete_with_no_current_op_is_noop);
    RUN(process_while_busy_does_not_start_another);
    RUN(execute_returning_false_fires_error_and_clears_busy);
    RUN(fifo_ordering_preserved);
    RUN(timeout_completes_with_timeout_result);
    RUN(clear_for_connection_cancels_matching_pending);
    RUN(clear_for_connection_cancels_current_op_if_matching);
    RUN(clear_cancels_everything);
    RUN(builder_populates_write_op_correctly);
    RUN(builder_mtu_request_encodes_big_endian);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
