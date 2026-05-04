#pragma once

#include "Type.h"
#include "LXMessage.h"
#include <Bytes.h>
#include <Identity.h>
#include <Destination.h>
#include <Link.h>
#include <Packet.h>

#include <vector>
#include <string>
#include <functional>
#include <memory>

namespace LXMF {

	// Forward declarations
	class PropagationNodeManager;

	/**
	 * @brief LXMF Router - Message delivery orchestration
	 *
	 * Manages message queues, link establishment, and delivery for LXMF messages.
	 * Supports DIRECT delivery method (via established links) for Phase 1 MVP.
	 *
	 * Usage:
	 *   LXMRouter router(identity, "/path/to/storage");
	 *   router.register_delivery_callback([](LXMessage& msg) {
	 *       // Handle received message
	 *   });
	 *   router.announce();  // Announce delivery destination
	 *
	 *   // Send message
	 *   LXMessage msg(dest, source, content);
	 *   router.handle_outbound(msg);
	 *
	 *   // Process queues periodically
	 *   loop() {
	 *       router.process_outbound();
	 *       router.process_inbound();
	 *   }
	 */
	class LXMRouter {

	public:
		using Ptr = std::shared_ptr<LXMRouter>;

		/**
		 * @brief Callback for message delivery (incoming messages)
		 * @param message The delivered message
		 */
		using DeliveryCallback = std::function<void(LXMessage& message)>;

		/**
		 * @brief Callback for message sent confirmation
		 * @param message The sent message
		 */
		using SentCallback = std::function<void(LXMessage& message)>;

		/**
		 * @brief Callback for message delivery confirmation
		 * @param message The delivered message
		 */
		using DeliveredCallback = std::function<void(LXMessage& message)>;

		/**
		 * @brief Callback for message failure
		 * @param message The failed message
		 */
		using FailedCallback = std::function<void(LXMessage& message)>;

		/**
		 * @brief Callback for sync completion
		 * @param messages_received Number of messages received from propagation node
		 */
		using SyncCompleteCallback = std::function<void(size_t messages_received)>;

		/**
		 * @brief Propagation sync state
		 */
		enum PropagationSyncState : uint8_t {
			PR_IDLE              = 0x00,
			PR_PATH_REQUESTED    = 0x01,
			PR_LINK_ESTABLISHING = 0x02,
			PR_LINK_ESTABLISHED  = 0x03,
			PR_REQUEST_SENT      = 0x04,
			PR_RECEIVING         = 0x05,
			PR_COMPLETE          = 0x06,
			PR_FAILED            = 0x07
		};

	public:
		/**
		 * @brief Construct LXMF Router
		 *
		 * @param identity Local identity for sending/receiving messages
		 * @param storage_path Path for message persistence (optional)
		 * @param announce_at_start Announce delivery destination on startup (default: false)
		 */
		LXMRouter(
			const RNS::Identity& identity,
			const std::string& storage_path = "",
			bool announce_at_start = false
		);

		~LXMRouter();

	public:
		/**
		 * @brief Register callback for incoming message delivery
		 *
		 * Called when a message is received and validated.
		 *
		 * @param callback Function to call with delivered messages
		 */
		void register_delivery_callback(DeliveryCallback callback);

		/**
		 * @brief Register callback for message sent confirmation
		 *
		 * Called when a message has been sent (packet transmitted or resource started).
		 *
		 * @param callback Function to call when message is sent
		 */
		void register_sent_callback(SentCallback callback);

		/**
		 * @brief Register callback for message delivery confirmation
		 *
		 * Called when remote confirms message delivery.
		 *
		 * @param callback Function to call when message is delivered
		 */
		void register_delivered_callback(DeliveredCallback callback);

		/**
		 * @brief Register callback for message failure
		 *
		 * Called when message sending fails (link timeout, etc).
		 *
		 * @param callback Function to call when message fails
		 */
		void register_failed_callback(FailedCallback callback);

		/**
		 * @brief Queue an outbound message for delivery
		 *
		 * Message will be sent via DIRECT method (link-based delivery).
		 * Link will be established if needed.
		 *
		 * @param message Message to send
		 */
		void handle_outbound(LXMessage& message);

		/**
		 * @brief Process outbound message queue
		 *
		 * Call periodically (e.g., in main loop) to process pending outbound messages.
		 * Establishes links, sends messages, handles retries.
		 */
		void process_outbound();

		/**
		 * @brief Process inbound message queue
		 *
		 * Call periodically to process received messages and invoke delivery callbacks.
		 */
		void process_inbound();

		/**
		 * @brief Announce the delivery destination
		 *
		 * Sends an announce for the LXMF delivery destination so others can
		 * discover this node and send messages to it.
		 *
		 * @param app_data Optional application data to include in announce
		 * @param path_response Optional path response
		 */
		void announce(const RNS::Bytes& app_data = {}, bool path_response = false);

		/**
		 * @brief Set announce interval
		 *
		 * @param interval Seconds between announces (0 = disable auto-announce)
		 */
		void set_announce_interval(uint32_t interval);

		/**
		 * @brief Enable/disable auto-announce on startup
		 *
		 * @param enabled True to announce on startup
		 */
		void set_announce_at_start(bool enabled);

		/**
		 * @brief Set display name for announces
		 *
		 * @param name Display name to include in announces
		 */
		void set_display_name(const std::string& name);

		/**
		 * @brief Get current display name
		 *
		 * @return Display name string
		 */
		inline const std::string& display_name() const { return _display_name; }

		/**
		 * @brief Get the delivery destination
		 *
		 * @return Destination for receiving LXMF messages
		 */
		inline const RNS::Destination& delivery_destination() const { return _delivery_destination; }

		/**
		 * @brief Get the local identity
		 *
		 * @return Identity used by this router
		 */
		inline const RNS::Identity& identity() const { return _identity; }

		/**
		 * @brief Get pending outbound message count
		 *
		 * @return Number of messages waiting to be sent
		 */
		inline size_t pending_outbound_count() const { return _pending_outbound_count; }

		/**
		 * @brief Get pending inbound message count
		 *
		 * @return Number of messages waiting to be processed
		 */
		inline size_t pending_inbound_count() const { return _pending_inbound_count; }

		/**
		 * @brief Get failed outbound message count
		 *
		 * @return Number of failed messages
		 */
		inline size_t failed_outbound_count() const { return _failed_outbound_count; }

		/**
		 * @brief Clear failed outbound messages
		 */
		void clear_failed_outbound();

		/**
		 * @brief Retry failed outbound messages
		 */
		void retry_failed_outbound();

		// ============== Propagation Node Support ==============

		/**
		 * @brief Set the propagation node manager
		 *
		 * @param manager Pointer to PropagationNodeManager (not owned)
		 */
		void set_propagation_node_manager(PropagationNodeManager* manager);

		/**
		 * @brief Set the outbound propagation node
		 *
		 * @param node_hash Destination hash of the propagation node
		 */
		void set_outbound_propagation_node(const RNS::Bytes& node_hash);

		/**
		 * @brief Get the current outbound propagation node
		 *
		 * @return Destination hash of the propagation node (or empty)
		 */
		RNS::Bytes get_outbound_propagation_node() const { return _outbound_propagation_node; }

		/**
		 * @brief Enable/disable fallback to PROPAGATED delivery
		 *
		 * When enabled, messages that fail DIRECT/OPPORTUNISTIC delivery will
		 * automatically be retried via a propagation node.
		 *
		 * @param enabled True to enable fallback
		 */
		void set_fallback_to_propagation(bool enabled) { _fallback_to_propagation = enabled; }

		/**
		 * @brief Check if fallback to propagation is enabled
		 *
		 * @return True if fallback is enabled
		 */
		bool fallback_to_propagation() const { return _fallback_to_propagation; }

		/**
		 * @brief Enable/disable propagation-only mode
		 *
		 * When enabled, all messages are sent via propagation node only.
		 * DIRECT and OPPORTUNISTIC delivery are skipped.
		 *
		 * @param enabled True to enable propagation-only mode
		 */
		void set_propagation_only(bool enabled) { _propagation_only = enabled; }

		/**
		 * @brief Check if propagation-only mode is enabled
		 *
		 * @return True if propagation-only mode is enabled
		 */
		bool propagation_only() const { return _propagation_only; }

		/**
		 * @brief Request messages from the propagation node
		 *
		 * Initiates a sync with the configured/selected propagation node to
		 * retrieve any pending messages.
		 */
		void request_messages_from_propagation_node();

		/**
		 * @brief Get the current sync state
		 *
		 * @return Current propagation sync state
		 */
		PropagationSyncState get_sync_state() const { return _sync_state; }

		/**
		 * @brief Get the current sync progress
		 *
		 * @return Progress from 0.0 to 1.0
		 */
		float get_sync_progress() const { return _sync_progress; }

		/**
		 * @brief Register callback for sync completion
		 *
		 * @param callback Function to call when sync completes
		 */
		void register_sync_complete_callback(SyncCompleteCallback callback);

		// ============== End Propagation Node Support ==============

		// ============== Stamp Enforcement ==============

		/**
		 * @brief Set the required stamp cost for incoming messages
		 *
		 * Messages without a valid stamp meeting this cost will be rejected
		 * when enforcement is enabled.
		 *
		 * @param cost Required number of leading zero bits (0 = no stamp required)
		 */
		void set_stamp_cost(uint8_t cost) { _stamp_cost = cost; }

		/**
		 * @brief Get the current stamp cost requirement
		 *
		 * @return Required stamp cost
		 */
		uint8_t stamp_cost() const { return _stamp_cost; }

		/**
		 * @brief Enable stamp enforcement
		 *
		 * When enabled, incoming messages must have a valid stamp meeting
		 * the configured cost or they will be dropped.
		 */
		void enforce_stamps() { _enforce_stamps = true; }

		/**
		 * @brief Disable stamp enforcement
		 *
		 * Incoming messages will be accepted regardless of stamp validity.
		 */
		void ignore_stamps() { _enforce_stamps = false; }

		/**
		 * @brief Check if stamp enforcement is enabled
		 *
		 * @return True if stamps are enforced
		 */
		bool stamps_enforced() const { return _enforce_stamps; }

		// ============== End Stamp Enforcement ==============

		/**
		 * @brief Packet callback for receiving LXMF messages
		 *
		 * Called by RNS when a packet is received for the delivery destination.
		 * NOTE: Public for static callback access, not intended for direct use.
		 *
		 * @param data Packet data
		 * @param packet The received packet
		 */
		void on_packet(const RNS::Bytes& data, const RNS::Packet& packet);

		/**
		 * @brief Handle link established callback
		 *
		 * NOTE: Public for static callback access, not intended for direct use.
		 *
		 * @param link The established link
		 */
		void on_link_established(const RNS::Link& link);

		/**
		 * @brief Handle link closed callback
		 *
		 * NOTE: Public for static callback access, not intended for direct use.
		 *
		 * @param link The closed link
		 */
		void on_link_closed(const RNS::Link& link);

		/**
		 * @brief Handle incoming link established to our delivery destination
		 *
		 * Called when a remote peer establishes a link to send us DIRECT messages.
		 * Sets up resource callbacks to receive LXMF messages.
		 *
		 * @param link The incoming link (non-const to set callbacks)
		 */
		void on_incoming_link_established(RNS::Link& link);

		/**
		 * @brief Handle resource concluded on incoming link
		 *
		 * Called when an LXMF message resource transfer completes.
		 * Unpacks and queues the message for delivery.
		 *
		 * @param resource The completed resource containing LXMF message
		 */
		void on_resource_concluded(const RNS::Resource& resource);

	private:
		/**
		 * @brief Establish a link to a destination
		 *
		 * Creates or retrieves an existing link for DIRECT delivery.
		 *
		 * @param destination_hash Hash of destination to link to
		 * @return Link object (or empty if failed)
		 */
		RNS::Link get_link_for_destination(const RNS::Bytes& destination_hash);

		/**
		 * @brief Send message via existing link
		 *
		 * @param message Message to send
		 * @param link Link to send over
		 * @return True if send initiated successfully
		 */
		bool send_via_link(LXMessage& message, RNS::Link& link);

		/**
		 * @brief Send message via OPPORTUNISTIC delivery (single packet)
		 *
		 * @param message Message to send
		 * @param dest_identity Destination identity (for encryption)
		 * @return True if send initiated successfully
		 */
		bool send_opportunistic(LXMessage& message, const RNS::Identity& dest_identity);

		/**
		 * @brief Send message via PROPAGATED delivery
		 *
		 * @param message Message to send
		 * @return True if send initiated successfully
		 */
		bool send_propagated(LXMessage& message);

		/**
		 * @brief Handle message list response from propagation node
		 */
		void on_message_list_response(const RNS::Bytes& response);

		/**
		 * @brief Handle message get response from propagation node
		 */
		void on_message_get_response(const RNS::Bytes& response);

		/**
		 * @brief Process received propagated LXMF data
		 */
		void process_propagated_lxmf(const RNS::Bytes& lxmf_data);

		/**
		 * @brief Static callback for outbound propagation resource
		 */
		static void static_propagation_resource_concluded(const RNS::Resource& resource);

		// Circular buffer helpers for message queues
		bool pending_outbound_push(const LXMessage& msg);
		bool pending_outbound_pop(LXMessage& msg);
		LXMessage* pending_outbound_front();
		bool pending_inbound_push(const LXMessage& msg);
		bool pending_inbound_pop(LXMessage& msg);
		LXMessage* pending_inbound_front();
		bool failed_outbound_push(const LXMessage& msg);
		bool failed_outbound_pop(LXMessage& msg);

	private:
		// Core components
		RNS::Identity _identity;                   // Local identity
		RNS::Destination _delivery_destination;    // For receiving messages
		std::string _storage_path;                 // Storage path for persistence

		// Message queues as fixed circular buffers (zero heap fragmentation)
		static constexpr size_t PENDING_OUTBOUND_SIZE = 16;
		LXMessage _pending_outbound_pool[PENDING_OUTBOUND_SIZE];
		size_t _pending_outbound_head = 0;
		size_t _pending_outbound_tail = 0;
		size_t _pending_outbound_count = 0;

		static constexpr size_t PENDING_INBOUND_SIZE = 16;
		LXMessage _pending_inbound_pool[PENDING_INBOUND_SIZE];
		size_t _pending_inbound_head = 0;
		size_t _pending_inbound_tail = 0;
		size_t _pending_inbound_count = 0;

		static constexpr size_t FAILED_OUTBOUND_SIZE = 8;
		LXMessage _failed_outbound_pool[FAILED_OUTBOUND_SIZE];
		size_t _failed_outbound_head = 0;
		size_t _failed_outbound_tail = 0;
		size_t _failed_outbound_count = 0;

		// Link management for DIRECT delivery - fixed pool (zero heap fragmentation)
		static constexpr size_t DIRECT_LINKS_SIZE = 8;
		static constexpr size_t DEST_HASH_SIZE = 16;  // Truncated hash size
		struct DirectLinkSlot {
			bool in_use = false;
			uint8_t destination_hash[DEST_HASH_SIZE];
			RNS::Link link;
			double creation_time = 0;
			RNS::Bytes destination_hash_bytes() const { return RNS::Bytes(destination_hash, DEST_HASH_SIZE); }
			void set_destination_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), DEST_HASH_SIZE);
				memcpy(destination_hash, b.data(), len);
				if (len < DEST_HASH_SIZE) memset(destination_hash + len, 0, DEST_HASH_SIZE - len);
			}
			bool destination_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != DEST_HASH_SIZE) return false;
				return memcmp(destination_hash, b.data(), DEST_HASH_SIZE) == 0;
			}
			void clear() {
				in_use = false;
				memset(destination_hash, 0, DEST_HASH_SIZE);
				link = RNS::Link(RNS::Type::NONE);
				creation_time = 0;
			}
		};
		DirectLinkSlot _direct_links_pool[DIRECT_LINKS_SIZE];
		DirectLinkSlot* find_direct_link_slot(const RNS::Bytes& hash);
		DirectLinkSlot* find_empty_direct_link_slot();
		size_t direct_links_count();
		static constexpr double LINK_ESTABLISHMENT_TIMEOUT = 30.0;  // Seconds to wait for pending links

		// Proof tracking for delivery confirmation - fixed pool (zero heap fragmentation)
		// Maps packet hash -> message hash so we can update message state when proof arrives
		// Fixed arrays eliminate ~0.8KB Bytes metadata overhead (16 slots × 2 Bytes × 24 bytes)
		static constexpr size_t PENDING_PROOFS_SIZE = 16;
		static constexpr size_t HASH_SIZE = 32;  // SHA256 hash size
		struct PendingProofSlot {
			bool in_use = false;
			uint8_t packet_hash[HASH_SIZE];
			uint8_t message_hash[HASH_SIZE];
			RNS::Bytes packet_hash_bytes() const { return RNS::Bytes(packet_hash, HASH_SIZE); }
			RNS::Bytes message_hash_bytes() const { return RNS::Bytes(message_hash, HASH_SIZE); }
			void set_packet_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), HASH_SIZE);
				memcpy(packet_hash, b.data(), len);
				if (len < HASH_SIZE) memset(packet_hash + len, 0, HASH_SIZE - len);
			}
			void set_message_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), HASH_SIZE);
				memcpy(message_hash, b.data(), len);
				if (len < HASH_SIZE) memset(message_hash + len, 0, HASH_SIZE - len);
			}
			bool packet_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != HASH_SIZE) return false;
				return memcmp(packet_hash, b.data(), HASH_SIZE) == 0;
			}
			void clear() {
				in_use = false;
				memset(packet_hash, 0, HASH_SIZE);
				memset(message_hash, 0, HASH_SIZE);
			}
		};
		static PendingProofSlot _pending_proofs_pool[PENDING_PROOFS_SIZE];
		static PendingProofSlot* find_pending_proof_slot(const RNS::Bytes& hash);
		static PendingProofSlot* find_empty_pending_proof_slot();
		static void static_proof_callback(const RNS::PacketReceipt& receipt);

	public:
		// Handle delivery proof for DIRECT messages (called from link packet callback)
		static void handle_direct_proof(const RNS::Bytes& message_hash);

	private:

		// Callbacks
		DeliveryCallback _delivery_callback;
		SentCallback _sent_callback;
		DeliveredCallback _delivered_callback;
		FailedCallback _failed_callback;

		// Announce settings
		uint32_t _announce_interval = 0;           // Seconds (0 = disabled)
		bool _announce_at_start = true;            // Announce on startup
		double _last_announce_time = 0.0;          // Last announce timestamp
		std::string _display_name;                 // Display name for announces

		// Internal state
		bool _initialized = false;

		// Retry backoff
		double _next_outbound_process_time = 0.0;  // Next time to process outbound queue
		static constexpr double OUTBOUND_RETRY_DELAY = 5.0;  // Seconds between retries
		static constexpr double PATH_REQUEST_WAIT = 3.0;     // Seconds to wait after path request

		// Propagation node support
		PropagationNodeManager* _propagation_manager = nullptr;
		RNS::Bytes _outbound_propagation_node;
		RNS::Link _outbound_propagation_link{RNS::Type::NONE};
		bool _fallback_to_propagation = true;
		bool _propagation_only = false;

		// Propagation sync state
		PropagationSyncState _sync_state = PR_IDLE;
		float _sync_progress = 0.0f;
		SyncCompleteCallback _sync_complete_callback;

		// Locally delivered transient IDs circular buffer (zero heap fragmentation)
		// Note: Keep as Bytes for now - converting to static array causes issues
		static constexpr size_t TRANSIENT_IDS_SIZE = 64;
		RNS::Bytes _transient_ids_buffer[TRANSIENT_IDS_SIZE];
		size_t _transient_ids_head = 0;
		size_t _transient_ids_count = 0;
		bool transient_ids_contains(const RNS::Bytes& id);
		void transient_ids_add(const RNS::Bytes& id);

		// Track outbound propagation resources - fixed pool (zero heap fragmentation)
		// Fixed arrays eliminate ~0.8KB Bytes metadata overhead (16 slots × 2 Bytes × 24 bytes)
		static constexpr size_t PENDING_PROP_RESOURCES_SIZE = 16;
		struct PropResourceSlot {
			bool in_use = false;
			uint8_t resource_hash[HASH_SIZE];
			uint8_t message_hash[HASH_SIZE];
			RNS::Bytes resource_hash_bytes() const { return RNS::Bytes(resource_hash, HASH_SIZE); }
			RNS::Bytes message_hash_bytes() const { return RNS::Bytes(message_hash, HASH_SIZE); }
			void set_resource_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), HASH_SIZE);
				memcpy(resource_hash, b.data(), len);
				if (len < HASH_SIZE) memset(resource_hash + len, 0, HASH_SIZE - len);
			}
			void set_message_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), HASH_SIZE);
				memcpy(message_hash, b.data(), len);
				if (len < HASH_SIZE) memset(message_hash + len, 0, HASH_SIZE - len);
			}
			bool resource_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != HASH_SIZE) return false;
				return memcmp(resource_hash, b.data(), HASH_SIZE) == 0;
			}
			void clear() {
				in_use = false;
				memset(resource_hash, 0, HASH_SIZE);
				memset(message_hash, 0, HASH_SIZE);
			}
		};
		static PropResourceSlot _pending_prop_resources_pool[PENDING_PROP_RESOURCES_SIZE];
		static PropResourceSlot* find_prop_resource_slot(const RNS::Bytes& hash);
		static PropResourceSlot* find_empty_prop_resource_slot();

		// Stamp enforcement
		uint8_t _stamp_cost = 0;       // Required stamp cost (0 = no stamp required)
		bool _enforce_stamps = false;  // Whether to enforce stamp requirements
	};

}  // namespace LXMF
