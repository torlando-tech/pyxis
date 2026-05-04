#pragma once

#include "Bytes.h"
#include "Log.h"
#include "Utilities/OS.h"

#include <functional>

namespace RNS {

	// Forward declaration to avoid circular includes
	class Resource;

	/**
	 * SegmentAccumulator collects multi-segment resources and fires a single callback
	 * when all segments have been received.
	 *
	 * Python RNS splits large resources (>MAX_EFFICIENT_SIZE, ~1MB) into multiple segments.
	 * Each segment is transferred as a separate Resource with its own hash/proof.
	 * Segments share the same original_hash and have segment_index 1..total_segments.
	 *
	 * This class:
	 * - Tracks incoming segments by original_hash
	 * - Stores segment data until all segments arrive
	 * - Fires the accumulated callback once with the complete concatenated data
	 * - Handles timeout cleanup for stale transfers
	 */
	class SegmentAccumulator {

	public:
		// Fixed-size pool limits to eliminate heap fragmentation
		static constexpr size_t MAX_PENDING_TRANSFERS = 8;
		static constexpr size_t MAX_SEGMENTS_PER_TRANSFER = 64;

		// Callback fired when all segments are received
		// Parameters: complete_data, original_hash
		using AccumulatedCallback = std::function<void(const Bytes& data, const Bytes& original_hash)>;

		// Callback for individual segment completion (optional, for progress tracking)
		using SegmentCallback = std::function<void(int segment_index, int total_segments, const Bytes& original_hash)>;

	public:
		SegmentAccumulator() = default;
		explicit SegmentAccumulator(AccumulatedCallback callback);

		/**
		 * Set the callback for completed multi-segment resources.
		 */
		void set_accumulated_callback(AccumulatedCallback callback);

		/**
		 * Set optional per-segment progress callback.
		 */
		void set_segment_callback(SegmentCallback callback);

		/**
		 * Called when a Resource segment completes.
		 *
		 * @param resource The completed resource (may be single or multi-segment)
		 * @return true if this was a multi-segment resource that was handled,
		 *         false if it was a single-segment resource (caller should invoke normal callback)
		 */
		bool segment_completed(const Resource& resource);

		/**
		 * Check for timed-out transfers and clean them up.
		 * Should be called periodically (e.g., from watchdog).
		 *
		 * @param timeout_seconds Maximum time since last activity before cleanup
		 */
		void check_timeouts(double timeout_seconds = 600.0);

		/**
		 * Manually cleanup a specific transfer.
		 */
		void cleanup(const Bytes& original_hash);

		/**
		 * Check if a transfer is in progress for the given original_hash.
		 */
		bool has_pending(const Bytes& original_hash) const;

		/**
		 * Get the number of pending (incomplete) transfers.
		 */
		size_t pending_count() const;

	private:
		struct SegmentInfo {
			int segment_index = 0;
			size_t data_size = 0;
			Bytes data;
			bool received = false;

			void clear() {
				segment_index = 0;
				data_size = 0;
				data.clear();
				received = false;
			}
		};

		struct PendingTransfer {
			Bytes original_hash;
			int total_segments = 0;
			int received_count = 0;
			SegmentInfo segments[MAX_SEGMENTS_PER_TRANSFER];  // Fixed array instead of std::vector
			size_t segment_count = 0;
			double started_at = 0.0;
			double last_activity = 0.0;

			void clear() {
				original_hash.clear();
				total_segments = 0;
				received_count = 0;
				for (size_t i = 0; i < segment_count; i++) {
					segments[i].clear();
				}
				segment_count = 0;
				started_at = 0.0;
				last_activity = 0.0;
			}
		};

		struct PendingTransferSlot {
			bool in_use = false;
			Bytes transfer_id;  // key (original_hash)
			PendingTransfer transfer;

			void clear() {
				in_use = false;
				transfer_id.clear();
				transfer.clear();
			}
		};

		// Fixed-size pool instead of std::map
		PendingTransferSlot _pending_pool[MAX_PENDING_TRANSFERS];

		AccumulatedCallback _accumulated_callback = nullptr;
		SegmentCallback _segment_callback = nullptr;

		/**
		 * Find a slot by transfer_id (original_hash).
		 * @return pointer to slot if found, nullptr otherwise
		 */
		PendingTransferSlot* find_slot(const Bytes& transfer_id);
		const PendingTransferSlot* find_slot(const Bytes& transfer_id) const;

		/**
		 * Allocate a new slot for a transfer.
		 * @return pointer to slot if available, nullptr if pool is full
		 */
		PendingTransferSlot* allocate_slot(const Bytes& transfer_id);

		/**
		 * Concatenate all segments in order and return complete data.
		 */
		Bytes assemble_segments(const PendingTransfer& transfer);
	};

}
