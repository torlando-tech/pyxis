#include "SegmentAccumulator.h"
#include "Resource.h"

using namespace RNS;
using namespace RNS::Utilities;

SegmentAccumulator::SegmentAccumulator(AccumulatedCallback callback)
	: _accumulated_callback(callback)
{
}

void SegmentAccumulator::set_accumulated_callback(AccumulatedCallback callback) {
	_accumulated_callback = callback;
}

void SegmentAccumulator::set_segment_callback(SegmentCallback callback) {
	_segment_callback = callback;
}

SegmentAccumulator::PendingTransferSlot* SegmentAccumulator::find_slot(const Bytes& transfer_id) {
	for (size_t i = 0; i < MAX_PENDING_TRANSFERS; i++) {
		if (_pending_pool[i].in_use && _pending_pool[i].transfer_id == transfer_id) {
			return &_pending_pool[i];
		}
	}
	return nullptr;
}

const SegmentAccumulator::PendingTransferSlot* SegmentAccumulator::find_slot(const Bytes& transfer_id) const {
	for (size_t i = 0; i < MAX_PENDING_TRANSFERS; i++) {
		if (_pending_pool[i].in_use && _pending_pool[i].transfer_id == transfer_id) {
			return &_pending_pool[i];
		}
	}
	return nullptr;
}

SegmentAccumulator::PendingTransferSlot* SegmentAccumulator::allocate_slot(const Bytes& transfer_id) {
	for (size_t i = 0; i < MAX_PENDING_TRANSFERS; i++) {
		if (!_pending_pool[i].in_use) {
			_pending_pool[i].in_use = true;
			_pending_pool[i].transfer_id = transfer_id;
			return &_pending_pool[i];
		}
	}
	return nullptr;  // Pool is full
}

bool SegmentAccumulator::segment_completed(const Resource& resource) {
	// Check if this is a multi-segment resource
	if (!resource.is_segmented()) {
		// Single-segment resource - caller should handle normally
		return false;
	}

	int segment_index = resource.segment_index();
	int total_segments = resource.total_segments();
	Bytes original_hash = resource.original_hash();

	// Use resource hash as fallback if original_hash not set
	if (!original_hash) {
		original_hash = resource.hash();
		DEBUG("SegmentAccumulator: No original_hash, using resource hash as key");
	}

	std::string hash_short = original_hash.toHex().substr(0, 16);
	DEBUGF("SegmentAccumulator: Received segment %d/%d for %s (%zu bytes)",
		segment_index, total_segments,
		hash_short.c_str(),
		resource.data().size());

	// Validate total_segments doesn't exceed our fixed array size
	if (total_segments > static_cast<int>(MAX_SEGMENTS_PER_TRANSFER)) {
		ERRORF("SegmentAccumulator: Transfer has %d segments, exceeds max %zu",
			total_segments, MAX_SEGMENTS_PER_TRANSFER);
		return true;  // We handled it (by rejecting)
	}

	double now = OS::time();

	// Find or create pending transfer
	PendingTransferSlot* slot = find_slot(original_hash);
	if (slot == nullptr) {
		// New transfer - allocate a slot
		slot = allocate_slot(original_hash);
		if (slot == nullptr) {
			ERRORF("SegmentAccumulator: Cannot track transfer %s, pool full (%zu max)",
				hash_short.c_str(), MAX_PENDING_TRANSFERS);
			return true;  // We handled it (by rejecting due to pool exhaustion)
		}

		// Initialize the transfer
		PendingTransfer& transfer = slot->transfer;
		transfer.original_hash = original_hash;
		transfer.total_segments = total_segments;
		transfer.received_count = 0;
		transfer.segment_count = static_cast<size_t>(total_segments);
		transfer.started_at = now;
		transfer.last_activity = now;

		// Initialize segment slots
		for (int i = 0; i < total_segments; i++) {
			transfer.segments[i].segment_index = i + 1;
			transfer.segments[i].received = false;
		}

		INFOF("SegmentAccumulator: Started tracking %d-segment transfer for %s",
			total_segments, hash_short.c_str());
	}

	PendingTransfer& transfer = slot->transfer;
	transfer.last_activity = now;

	// Validate segment index
	if (segment_index < 1 || segment_index > transfer.total_segments) {
		WARNINGF("SegmentAccumulator: Invalid segment_index %d (expected 1-%d)",
			segment_index, transfer.total_segments);
		return true;  // We handled it (by rejecting)
	}

	// Store segment data (segment_index is 1-based)
	int idx = segment_index - 1;
	if (!transfer.segments[idx].received) {
		transfer.segments[idx].data = resource.data();
		transfer.segments[idx].data_size = resource.data().size();
		transfer.segments[idx].received = true;
		transfer.received_count++;

		DEBUGF("SegmentAccumulator: Stored segment %d, %d/%d received",
			segment_index, transfer.received_count, transfer.total_segments);

		// Fire per-segment callback if set
		if (_segment_callback) {
			_segment_callback(segment_index, total_segments, original_hash);
		}
	} else {
		DEBUGF("SegmentAccumulator: Duplicate segment %d, ignoring", segment_index);
	}

	// Check if all segments received
	if (transfer.received_count == transfer.total_segments) {
		std::string hash_short = original_hash.toHex().substr(0, 16);
		INFOF("SegmentAccumulator: All %d segments received for %s, assembling...",
			transfer.total_segments, hash_short.c_str());

		// Assemble complete data
		Bytes complete_data = assemble_segments(transfer);

		INFOF("SegmentAccumulator: Assembled %zu bytes from %d segments",
			complete_data.size(), transfer.total_segments);

		// Fire accumulated callback
		if (_accumulated_callback) {
			_accumulated_callback(complete_data, original_hash);
		}

		// Cleanup - clear the slot
		slot->clear();
	}

	return true;  // We handled this multi-segment resource
}

Bytes SegmentAccumulator::assemble_segments(const PendingTransfer& transfer) {
	// Calculate total size
	size_t total_size = 0;
	for (int i = 0; i < transfer.total_segments; i++) {
		total_size += transfer.segments[i].data_size;
	}

	// Concatenate in order
	Bytes result;
	result.reserve(total_size);

	for (int i = 0; i < transfer.total_segments; i++) {
		const SegmentInfo& seg = transfer.segments[i];
		if (!seg.received) {
			ERRORF("SegmentAccumulator: Missing segment %d during assembly!", i + 1);
			continue;
		}
		result += seg.data;
	}

	return result;
}

void SegmentAccumulator::check_timeouts(double timeout_seconds) {
	double now = OS::time();

	for (size_t i = 0; i < MAX_PENDING_TRANSFERS; i++) {
		if (!_pending_pool[i].in_use) {
			continue;
		}

		const PendingTransfer& transfer = _pending_pool[i].transfer;
		double inactive_time = now - transfer.last_activity;

		if (inactive_time > timeout_seconds) {
			std::string hash_short = transfer.original_hash.toHex().substr(0, 16);
			WARNINGF("SegmentAccumulator: Transfer %s timed out (%.1fs inactive, %d/%d segments)",
				hash_short.c_str(),
				inactive_time, transfer.received_count, transfer.total_segments);
			_pending_pool[i].clear();
		}
	}
}

void SegmentAccumulator::cleanup(const Bytes& original_hash) {
	PendingTransferSlot* slot = find_slot(original_hash);
	if (slot != nullptr) {
		std::string hash_short = original_hash.toHex().substr(0, 16);
		DEBUGF("SegmentAccumulator: Cleaning up transfer %s (%d/%d segments received)",
			hash_short.c_str(),
			slot->transfer.received_count, slot->transfer.total_segments);
		slot->clear();
	}
}

bool SegmentAccumulator::has_pending(const Bytes& original_hash) const {
	return find_slot(original_hash) != nullptr;
}

size_t SegmentAccumulator::pending_count() const {
	size_t count = 0;
	for (size_t i = 0; i < MAX_PENDING_TRANSFERS; i++) {
		if (_pending_pool[i].in_use) {
			count++;
		}
	}
	return count;
}
