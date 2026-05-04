#pragma once

#include "Type.h"
#include <Bytes.h>
#include <Identity.h>
#include <Transport.h>

#include <vector>
#include <functional>
#include <memory>

namespace LXMF {

	// Maximum number of propagation nodes to track (fixed pool size)
	static constexpr size_t MAX_PROPAGATION_NODES = 32;

	/**
	 * @brief Information about a discovered propagation node
	 */
	struct PropagationNodeInfo {
		RNS::Bytes node_hash;          // Destination hash of the propagation node
		std::string name;              // Display name from metadata
		double timebase = 0;           // Node's timebase from announce
		bool enabled = false;          // Whether propagation is enabled
		uint32_t transfer_limit = 0;   // Per-transfer limit in KB
		uint32_t sync_limit = 0;       // Per-sync limit in KB
		uint8_t stamp_cost = 0;        // Required stamp cost
		uint8_t stamp_flexibility = 0; // Stamp cost flexibility
		uint8_t peering_cost = 0;      // Cost for peering
		uint8_t hops = 0xFF;           // Hop count (0xFF = unknown)
		double last_seen = 0;          // Timestamp of last announce

		// Check if this node info is valid
		operator bool() const { return node_hash.size() > 0; }
	};

	/**
	 * @brief Fixed-size slot for propagation node storage
	 *
	 * Used to avoid heap fragmentation from std::map operations.
	 */
	struct PropagationNodeSlot {
		bool in_use = false;
		RNS::Bytes node_hash;  // key for lookup
		PropagationNodeInfo info;

		void clear() {
			in_use = false;
			node_hash = RNS::Bytes();
			info = PropagationNodeInfo();
		}
	};

	/**
	 * @brief Manages discovery and tracking of LXMF propagation nodes
	 *
	 * Listens for propagation node announces (aspect "lxmf.propagation") and
	 * maintains a list of known nodes. Supports manual selection and auto-selection
	 * of the best available node.
	 *
	 * Usage:
	 *   PropagationNodeManager manager;
	 *   Transport::register_announce_handler(HAnnounceHandler(&manager));
	 *
	 *   // Get list of known nodes
	 *   auto nodes = manager.get_nodes();
	 *
	 *   // Auto-select best node
	 *   Bytes best = manager.get_best_node();
	 */
	class PropagationNodeManager : public RNS::AnnounceHandler {
	public:
		using NodeUpdateCallback = std::function<void()>;

		// Propagation node metadata keys (from Python LXMF)
		static constexpr uint8_t PN_META_VERSION = 0x00;
		static constexpr uint8_t PN_META_NAME = 0x01;
		static constexpr uint8_t PN_META_SYNC_STRATUM = 0x02;
		static constexpr uint8_t PN_META_SYNC_THROTTLE = 0x03;
		static constexpr uint8_t PN_META_AUTH_BAND = 0x04;
		static constexpr uint8_t PN_META_UTIL_PRESSURE = 0x05;
		static constexpr uint8_t PN_META_CUSTOM = 0xFF;

		// Stale node timeout (1 hour)
		static constexpr double NODE_STALE_TIMEOUT = 3600.0;

	public:
		/**
		 * @brief Construct PropagationNodeManager
		 *
		 * Registers for "lxmf.propagation" announces.
		 */
		PropagationNodeManager();
		virtual ~PropagationNodeManager() = default;

		/**
		 * @brief AnnounceHandler callback
		 *
		 * Called by Transport when a propagation node announce is received.
		 */
		virtual void received_announce(
			const RNS::Bytes& destination_hash,
			const RNS::Identity& announced_identity,
			const RNS::Bytes& app_data
		) override;

		/**
		 * @brief Get list of all known propagation nodes
		 *
		 * @return Vector of node info, sorted by hops (closest first)
		 */
		std::vector<PropagationNodeInfo> get_nodes() const;

		/**
		 * @brief Get info for a specific node
		 *
		 * @param hash Destination hash of the node
		 * @return Node info (or empty if not found)
		 */
		PropagationNodeInfo get_node(const RNS::Bytes& hash) const;

		/**
		 * @brief Check if a node is known
		 *
		 * @param hash Destination hash of the node
		 * @return True if node is known
		 */
		bool has_node(const RNS::Bytes& hash) const;

		/**
		 * @brief Get number of known nodes
		 *
		 * @return Number of nodes
		 */
		size_t node_count() const { return nodes_count(); }

		/**
		 * @brief Manually select a propagation node
		 *
		 * @param hash Destination hash of the node to select (empty to clear)
		 */
		void set_selected_node(const RNS::Bytes& hash);

		/**
		 * @brief Get the manually selected node
		 *
		 * @return Destination hash of selected node (or empty if none)
		 */
		RNS::Bytes get_selected_node() const { return _selected_node; }

		/**
		 * @brief Auto-select the best available propagation node
		 *
		 * Selection criteria:
		 * 1. Node must be enabled
		 * 2. Prefer nodes with fewer hops
		 * 3. Prefer nodes seen more recently
		 *
		 * @return Destination hash of best node (or empty if none available)
		 */
		RNS::Bytes get_best_node() const;

		/**
		 * @brief Get the effective propagation node (selected or best)
		 *
		 * Returns the manually selected node if set, otherwise auto-selects.
		 *
		 * @return Destination hash of effective node
		 */
		RNS::Bytes get_effective_node() const;

		/**
		 * @brief Set callback for node list updates
		 *
		 * @param callback Function to call when nodes are added/updated
		 */
		void set_update_callback(NodeUpdateCallback callback) { _update_callback = callback; }

		/**
		 * @brief Clean up stale nodes
		 *
		 * Removes nodes that haven't been seen recently.
		 */
		void clean_stale_nodes();

	private:
		/**
		 * @brief Parse announce app_data into PropagationNodeInfo
		 *
		 * App data format (msgpack):
		 * [0] Legacy flag (bool)
		 * [1] Node timebase (timestamp)
		 * [2] Propagation enabled (bool)
		 * [3] Per-transfer limit (KB)
		 * [4] Per-sync limit (KB)
		 * [5] [stamp_cost, flexibility, peering_cost]
		 * [6] Metadata dict
		 *
		 * @param app_data Raw announce app_data
		 * @return Parsed node info
		 */
		PropagationNodeInfo parse_announce_data(const RNS::Bytes& app_data);

		/**
		 * @brief Find a node slot by hash
		 *
		 * @param hash Destination hash to search for
		 * @return Pointer to slot if found, nullptr otherwise
		 */
		PropagationNodeSlot* find_node_slot(const RNS::Bytes& hash);
		const PropagationNodeSlot* find_node_slot(const RNS::Bytes& hash) const;

		/**
		 * @brief Find an empty slot in the pool
		 *
		 * @return Pointer to empty slot, nullptr if pool is full
		 */
		PropagationNodeSlot* find_empty_node_slot();

		/**
		 * @brief Get the number of nodes currently in use
		 *
		 * @return Number of active nodes in the pool
		 */
		size_t nodes_count() const;

	private:
		PropagationNodeSlot _nodes_pool[MAX_PROPAGATION_NODES];
		RNS::Bytes _selected_node;
		NodeUpdateCallback _update_callback;
	};

}  // namespace LXMF
