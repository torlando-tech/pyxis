#include "LXMRouter.h"
#include "PropagationNodeManager.h"
#include <Log.h>
#include <Utilities/OS.h>
#include <Packet.h>
#include <Transport.h>
#include <Resource.h>

#include <MsgPack.h>

using namespace LXMF;
using namespace RNS;

// ============== Static Fixed Pool Definitions ==============

// Router registry fixed pool (zero heap fragmentation)
static constexpr size_t ROUTER_REGISTRY_SIZE = 4;
static constexpr size_t REGISTRY_HASH_SIZE = 16;  // Truncated hash size
struct RouterRegistrySlot {
	bool in_use = false;
	uint8_t destination_hash[REGISTRY_HASH_SIZE];
	LXMRouter* router = nullptr;
	Bytes destination_hash_bytes() const { return Bytes(destination_hash, REGISTRY_HASH_SIZE); }
	void set_destination_hash(const Bytes& b) {
		size_t len = std::min(b.size(), REGISTRY_HASH_SIZE);
		memcpy(destination_hash, b.data(), len);
		if (len < REGISTRY_HASH_SIZE) memset(destination_hash + len, 0, REGISTRY_HASH_SIZE - len);
	}
	bool destination_hash_equals(const Bytes& b) const {
		if (b.size() != REGISTRY_HASH_SIZE) return false;
		return memcmp(destination_hash, b.data(), REGISTRY_HASH_SIZE) == 0;
	}
	void clear() {
		in_use = false;
		memset(destination_hash, 0, REGISTRY_HASH_SIZE);
		router = nullptr;
	}
};
static RouterRegistrySlot _router_registry_pool[ROUTER_REGISTRY_SIZE];

static RouterRegistrySlot* find_router_registry_slot(const Bytes& hash) {
	for (size_t i = 0; i < ROUTER_REGISTRY_SIZE; i++) {
		if (_router_registry_pool[i].in_use && _router_registry_pool[i].destination_hash_equals(hash)) {
			return &_router_registry_pool[i];
		}
	}
	return nullptr;
}

static RouterRegistrySlot* find_empty_router_registry_slot() {
	for (size_t i = 0; i < ROUTER_REGISTRY_SIZE; i++) {
		if (!_router_registry_pool[i].in_use) {
			return &_router_registry_pool[i];
		}
	}
	return nullptr;
}

// Outbound resources fixed pool (zero heap fragmentation)
// Fixed arrays eliminate ~0.8KB Bytes metadata overhead (16 slots × 2 Bytes × 24 bytes)
static constexpr size_t OUTBOUND_RESOURCES_SIZE = 16;
static constexpr size_t OUTBOUND_HASH_SIZE = 32;  // SHA256 hash size
struct OutboundResourceSlot {
	bool in_use = false;
	uint8_t resource_hash[OUTBOUND_HASH_SIZE];
	uint8_t message_hash[OUTBOUND_HASH_SIZE];
	Bytes resource_hash_bytes() const { return Bytes(resource_hash, OUTBOUND_HASH_SIZE); }
	Bytes message_hash_bytes() const { return Bytes(message_hash, OUTBOUND_HASH_SIZE); }
	void set_resource_hash(const Bytes& b) {
		size_t len = std::min(b.size(), OUTBOUND_HASH_SIZE);
		memcpy(resource_hash, b.data(), len);
		if (len < OUTBOUND_HASH_SIZE) memset(resource_hash + len, 0, OUTBOUND_HASH_SIZE - len);
	}
	void set_message_hash(const Bytes& b) {
		size_t len = std::min(b.size(), OUTBOUND_HASH_SIZE);
		memcpy(message_hash, b.data(), len);
		if (len < OUTBOUND_HASH_SIZE) memset(message_hash + len, 0, OUTBOUND_HASH_SIZE - len);
	}
	bool resource_hash_equals(const Bytes& b) const {
		if (b.size() != OUTBOUND_HASH_SIZE) return false;
		return memcmp(resource_hash, b.data(), OUTBOUND_HASH_SIZE) == 0;
	}
	void clear() {
		in_use = false;
		memset(resource_hash, 0, OUTBOUND_HASH_SIZE);
		memset(message_hash, 0, OUTBOUND_HASH_SIZE);
	}
};
static OutboundResourceSlot _outbound_resources_pool[OUTBOUND_RESOURCES_SIZE];

static OutboundResourceSlot* find_outbound_resource_slot(const Bytes& hash) {
	for (size_t i = 0; i < OUTBOUND_RESOURCES_SIZE; i++) {
		if (_outbound_resources_pool[i].in_use && _outbound_resources_pool[i].resource_hash_equals(hash)) {
			return &_outbound_resources_pool[i];
		}
	}
	return nullptr;
}

static OutboundResourceSlot* find_empty_outbound_resource_slot() {
	for (size_t i = 0; i < OUTBOUND_RESOURCES_SIZE; i++) {
		if (!_outbound_resources_pool[i].in_use) {
			return &_outbound_resources_pool[i];
		}
	}
	return nullptr;
}

// Static pending proofs pool definition
LXMRouter::PendingProofSlot LXMRouter::_pending_proofs_pool[LXMRouter::PENDING_PROOFS_SIZE];

// Static pending propagation resources pool definition
LXMRouter::PropResourceSlot LXMRouter::_pending_prop_resources_pool[LXMRouter::PENDING_PROP_RESOURCES_SIZE];

// ============== End Static Fixed Pool Definitions ==============

// ============== Fixed Pool Helper Functions ==============

LXMRouter::DirectLinkSlot* LXMRouter::find_direct_link_slot(const Bytes& hash) {
	for (size_t i = 0; i < DIRECT_LINKS_SIZE; i++) {
		if (_direct_links_pool[i].in_use && _direct_links_pool[i].destination_hash_equals(hash)) {
			return &_direct_links_pool[i];
		}
	}
	return nullptr;
}

LXMRouter::DirectLinkSlot* LXMRouter::find_empty_direct_link_slot() {
	for (size_t i = 0; i < DIRECT_LINKS_SIZE; i++) {
		if (!_direct_links_pool[i].in_use) {
			return &_direct_links_pool[i];
		}
	}
	return nullptr;
}

size_t LXMRouter::direct_links_count() {
	size_t count = 0;
	for (size_t i = 0; i < DIRECT_LINKS_SIZE; i++) {
		if (_direct_links_pool[i].in_use) count++;
	}
	return count;
}

LXMRouter::PendingProofSlot* LXMRouter::find_pending_proof_slot(const Bytes& hash) {
	for (size_t i = 0; i < PENDING_PROOFS_SIZE; i++) {
		if (_pending_proofs_pool[i].in_use && _pending_proofs_pool[i].packet_hash_equals(hash)) {
			return &_pending_proofs_pool[i];
		}
	}
	return nullptr;
}

LXMRouter::PendingProofSlot* LXMRouter::find_empty_pending_proof_slot() {
	for (size_t i = 0; i < PENDING_PROOFS_SIZE; i++) {
		if (!_pending_proofs_pool[i].in_use) {
			return &_pending_proofs_pool[i];
		}
	}
	return nullptr;
}

LXMRouter::PropResourceSlot* LXMRouter::find_prop_resource_slot(const Bytes& hash) {
	for (size_t i = 0; i < PENDING_PROP_RESOURCES_SIZE; i++) {
		if (_pending_prop_resources_pool[i].in_use && _pending_prop_resources_pool[i].resource_hash_equals(hash)) {
			return &_pending_prop_resources_pool[i];
		}
	}
	return nullptr;
}

LXMRouter::PropResourceSlot* LXMRouter::find_empty_prop_resource_slot() {
	for (size_t i = 0; i < PENDING_PROP_RESOURCES_SIZE; i++) {
		if (!_pending_prop_resources_pool[i].in_use) {
			return &_pending_prop_resources_pool[i];
		}
	}
	return nullptr;
}

bool LXMRouter::transient_ids_contains(const Bytes& id) {
	for (size_t i = 0; i < _transient_ids_count; i++) {
		size_t idx = (_transient_ids_head + TRANSIENT_IDS_SIZE - _transient_ids_count + i) % TRANSIENT_IDS_SIZE;
		if (_transient_ids_buffer[idx] == id) {
			return true;
		}
	}
	return false;
}

void LXMRouter::transient_ids_add(const Bytes& id) {
	_transient_ids_buffer[_transient_ids_head] = id;
	_transient_ids_head = (_transient_ids_head + 1) % TRANSIENT_IDS_SIZE;
	if (_transient_ids_count < TRANSIENT_IDS_SIZE) {
		_transient_ids_count++;
	}
}

// ============== End Fixed Pool Helper Functions ==============

// Static packet callback for destination
static void static_packet_callback(const Bytes& data, const Packet& packet) {
	// Look up router by destination hash
	RouterRegistrySlot* slot = find_router_registry_slot(packet.destination_hash());
	if (slot) {
		slot->router->on_packet(data, packet);
	}
}

// Static link callbacks
static void static_link_established_callback(Link& link) {
	// Find router that owns this link destination
	RouterRegistrySlot* slot = find_router_registry_slot(link.destination().hash());
	if (slot) {
		slot->router->on_link_established(link);
	}
}

static void static_link_closed_callback(Link& link) {
	// Find router that owns this link destination
	RouterRegistrySlot* slot = find_router_registry_slot(link.destination().hash());
	if (slot) {
		slot->router->on_link_closed(link);
	}
}

// Static callback for incoming link established on delivery destination
static void static_delivery_link_established_callback(Link& link) {
	// Find router that owns this destination
	RouterRegistrySlot* slot = find_router_registry_slot(link.destination().hash());
	if (slot) {
		slot->router->on_incoming_link_established(link);
	}
}

// Static callback for resource concluded on delivery links (receiving)
static void static_resource_concluded_callback(const Resource& resource) {
	Link link = resource.link();
	if (!link) {
		ERROR("static_resource_concluded_callback: Resource has no link");
		return;
	}

	// Find router that owns this link's destination
	RouterRegistrySlot* slot = find_router_registry_slot(link.destination().hash());
	if (slot) {
		slot->router->on_resource_concluded(resource);
	}
}

// Static callback for outbound resource concluded (sending)
// Called when our resource transfer completes (receiver sent RESOURCE_PRF)
static void static_outbound_resource_concluded(const Resource& resource) {
	Bytes resource_hash = resource.hash();
	char buf[128];
	snprintf(buf, sizeof(buf), "Outbound resource concluded: %.16s...", resource_hash.toHex().c_str());
	DEBUG(buf);
	snprintf(buf, sizeof(buf), "  Status: %d", (int)resource.status());
	DEBUG(buf);

	// Check if this resource is one we're tracking
	OutboundResourceSlot* slot = find_outbound_resource_slot(resource_hash);
	if (!slot) {
		DEBUG("  Resource not in pending outbound map");
		return;
	}

	Bytes message_hash = slot->message_hash_bytes();

	// Check if resource completed successfully
	if (resource.status() == RNS::Type::Resource::COMPLETE) {
		snprintf(buf, sizeof(buf), "DIRECT delivery confirmed for message %.16s...", message_hash.toHex().c_str());
		INFO(buf);

		// Use the public static method to trigger delivered callback
		LXMRouter::handle_direct_proof(message_hash);
	} else {
		snprintf(buf, sizeof(buf), "DIRECT resource transfer failed with status %d", (int)resource.status());
		WARNING(buf);
	}

	// Remove from pending map
	slot->clear();
}

// Static proof callback - called when delivery proof is received
void LXMRouter::static_proof_callback(const PacketReceipt& receipt) {
	DEBUG(">>> PROOF CALLBACK ENTRY");
#ifdef ARDUINO
	Serial.flush();
#endif

	// Get packet hash from receipt
	DEBUG(">>> Getting packet hash from receipt");
#ifdef ARDUINO
	Serial.flush();
#endif
	Bytes packet_hash = receipt.hash();
	char buf[128];

	DEBUG(">>> Looking up pending proof slot");
#ifdef ARDUINO
	Serial.flush();
#endif

	// Look up message hash for this packet
	PendingProofSlot* slot = find_pending_proof_slot(packet_hash);
	if (slot) {
		DEBUG(">>> Found slot, getting message hash");
#ifdef ARDUINO
		Serial.flush();
#endif
		Bytes message_hash = slot->message_hash_bytes();
		snprintf(buf, sizeof(buf), "Delivery proof received for message %.16s...", message_hash.toHex().c_str());
		INFO(buf);
#ifdef ARDUINO
		Serial.flush();
#endif

		// Track notified routers to avoid duplicates (max ROUTER_REGISTRY_SIZE)
		LXMRouter* notified_routers[ROUTER_REGISTRY_SIZE];
		size_t notified_count = 0;

		DEBUG(">>> Iterating router registry");
#ifdef ARDUINO
		Serial.flush();
#endif

		// Find the router that sent this message and call its delivered callback
		for (size_t i = 0; i < ROUTER_REGISTRY_SIZE; i++) {
			if (_router_registry_pool[i].in_use) {
				LXMRouter* router = _router_registry_pool[i].router;
				// Check if already notified
				bool already_notified = false;
				for (size_t j = 0; j < notified_count; j++) {
					if (notified_routers[j] == router) {
						already_notified = true;
						break;
					}
				}
				if (!already_notified && router && router->_delivered_callback) {
					DEBUGF(">>> Calling delivered callback for router %zu", i);
#ifdef ARDUINO
					Serial.flush();
#endif
					notified_routers[notified_count++] = router;
					// Create a minimal message with just the hash for the callback
					// The callback can look up full message from storage if needed
					Bytes empty_hash;
					LXMessage msg(empty_hash, empty_hash);
					msg.hash(message_hash);
					msg.state(Type::Message::DELIVERED);
					DEBUG(">>> About to invoke callback");
#ifdef ARDUINO
					Serial.flush();
#endif
					router->_delivered_callback(msg);
					DEBUG(">>> Callback returned");
#ifdef ARDUINO
					Serial.flush();
#endif
				}
			}
		}

		// Remove from pending proofs
		DEBUG(">>> Clearing slot");
#ifdef ARDUINO
		Serial.flush();
#endif
		slot->clear();
		DEBUG(">>> Slot cleared");
#ifdef ARDUINO
		Serial.flush();
#endif
	} else {
		snprintf(buf, sizeof(buf), "Received proof for unknown packet: %.16s...", packet_hash.toHex().c_str());
		DEBUG(buf);
	}
	DEBUG(">>> PROOF CALLBACK EXIT");
#ifdef ARDUINO
	Serial.flush();
#endif
}

// Constructor
LXMRouter::LXMRouter(
	const Identity& identity,
	const std::string& storage_path,
	bool announce_at_start
) :
	_identity(identity),
	_delivery_destination(RNS::Type::NONE),
	_storage_path(storage_path),
	_announce_at_start(announce_at_start)
{
	INFO("Initializing LXMF Router");

	// Create delivery destination: <identity>/lxmf/delivery
	_delivery_destination = Destination(
		_identity,
		RNS::Type::Destination::IN,
		RNS::Type::Destination::SINGLE,
		"lxmf",
		"delivery"
	);

	// Register this router in global registry for callback dispatch
	RouterRegistrySlot* slot = find_empty_router_registry_slot();
	if (slot) {
		slot->in_use = true;
		slot->set_destination_hash(_delivery_destination.hash());
		slot->router = this;
	} else {
		ERROR("Router registry full - cannot register router!");
	}

	// Register packet callback for receiving LXMF messages (OPPORTUNISTIC)
	_delivery_destination.set_packet_callback(static_packet_callback);

	// Register link established callback for incoming links (DIRECT delivery)
	_delivery_destination.set_link_established_callback(static_delivery_link_established_callback);

	char buf[128];
	snprintf(buf, sizeof(buf), "  Delivery destination: %s", _delivery_destination.hash().toHex().c_str());
	INFO(buf);
	snprintf(buf, sizeof(buf), "  Destination type: %u", (uint8_t)_delivery_destination.type());
	INFO(buf);
	snprintf(buf, sizeof(buf), "  Destination direction: %u", (uint8_t)_delivery_destination.direction());
	INFO(buf);

	// Announce at start if enabled
	if (_announce_at_start) {
		INFO("  Auto-announce enabled");
		announce();
	}

	_initialized = true;
	INFO("LXMF Router initialized");
}

LXMRouter::~LXMRouter() {
	// Unregister from global registry
	RouterRegistrySlot* slot = find_router_registry_slot(_delivery_destination.hash());
	if (slot) {
		slot->clear();
	}
	TRACE("LXMRouter destroyed");
}

// ============== Circular Buffer Helpers ==============

bool LXMRouter::pending_outbound_push(const LXMessage& msg) {
	if (_pending_outbound_count >= PENDING_OUTBOUND_SIZE) {
		WARNING("Pending outbound queue full, dropping oldest message");
		// Advance tail to drop oldest
		_pending_outbound_pool[_pending_outbound_tail] = LXMessage();  // Clear slot
		_pending_outbound_tail = (_pending_outbound_tail + 1) % PENDING_OUTBOUND_SIZE;
		_pending_outbound_count--;
	}
	_pending_outbound_pool[_pending_outbound_head] = msg;
	_pending_outbound_head = (_pending_outbound_head + 1) % PENDING_OUTBOUND_SIZE;
	_pending_outbound_count++;
	return true;
}

LXMessage* LXMRouter::pending_outbound_front() {
	if (_pending_outbound_count == 0) return nullptr;
	return &_pending_outbound_pool[_pending_outbound_tail];
}

bool LXMRouter::pending_outbound_pop(LXMessage& msg) {
	if (_pending_outbound_count == 0) return false;
	msg = _pending_outbound_pool[_pending_outbound_tail];
	_pending_outbound_pool[_pending_outbound_tail] = LXMessage();  // Clear slot
	_pending_outbound_tail = (_pending_outbound_tail + 1) % PENDING_OUTBOUND_SIZE;
	_pending_outbound_count--;
	return true;
}

bool LXMRouter::pending_inbound_push(const LXMessage& msg) {
	if (_pending_inbound_count >= PENDING_INBOUND_SIZE) {
		WARNING("Pending inbound queue full, dropping oldest message");
		// Advance tail to drop oldest
		_pending_inbound_pool[_pending_inbound_tail] = LXMessage();  // Clear slot
		_pending_inbound_tail = (_pending_inbound_tail + 1) % PENDING_INBOUND_SIZE;
		_pending_inbound_count--;
	}
	_pending_inbound_pool[_pending_inbound_head] = msg;
	_pending_inbound_head = (_pending_inbound_head + 1) % PENDING_INBOUND_SIZE;
	_pending_inbound_count++;
	return true;
}

LXMessage* LXMRouter::pending_inbound_front() {
	if (_pending_inbound_count == 0) return nullptr;
	return &_pending_inbound_pool[_pending_inbound_tail];
}

bool LXMRouter::pending_inbound_pop(LXMessage& msg) {
	if (_pending_inbound_count == 0) return false;
	msg = _pending_inbound_pool[_pending_inbound_tail];
	_pending_inbound_pool[_pending_inbound_tail] = LXMessage();  // Clear slot
	_pending_inbound_tail = (_pending_inbound_tail + 1) % PENDING_INBOUND_SIZE;
	_pending_inbound_count--;
	return true;
}

bool LXMRouter::failed_outbound_push(const LXMessage& msg) {
	if (_failed_outbound_count >= FAILED_OUTBOUND_SIZE) {
		WARNING("Failed outbound queue full, dropping oldest message");
		// Advance tail to drop oldest
		_failed_outbound_pool[_failed_outbound_tail] = LXMessage();  // Clear slot
		_failed_outbound_tail = (_failed_outbound_tail + 1) % FAILED_OUTBOUND_SIZE;
		_failed_outbound_count--;
	}
	_failed_outbound_pool[_failed_outbound_head] = msg;
	_failed_outbound_head = (_failed_outbound_head + 1) % FAILED_OUTBOUND_SIZE;
	_failed_outbound_count++;
	return true;
}

bool LXMRouter::failed_outbound_pop(LXMessage& msg) {
	if (_failed_outbound_count == 0) return false;
	msg = _failed_outbound_pool[_failed_outbound_tail];
	_failed_outbound_pool[_failed_outbound_tail] = LXMessage();  // Clear slot
	_failed_outbound_tail = (_failed_outbound_tail + 1) % FAILED_OUTBOUND_SIZE;
	_failed_outbound_count--;
	return true;
}

// ============== End Circular Buffer Helpers ==============

// Register callbacks
void LXMRouter::register_delivery_callback(DeliveryCallback callback) {
	_delivery_callback = callback;
	DEBUG("Delivery callback registered");
}

void LXMRouter::register_sent_callback(SentCallback callback) {
	_sent_callback = callback;
	DEBUG("Sent callback registered");
}

void LXMRouter::register_delivered_callback(DeliveredCallback callback) {
	_delivered_callback = callback;
	DEBUG("Delivered callback registered");
}

void LXMRouter::register_failed_callback(FailedCallback callback) {
	_failed_callback = callback;
	DEBUG("Failed callback registered");
}

// Queue outbound message
void LXMRouter::handle_outbound(LXMessage& message) {
	INFO("Handling outbound LXMF message");
	char buf[128];
	snprintf(buf, sizeof(buf), "  Destination: %s", message.destination_hash().toHex().c_str());
	DEBUG(buf);
	snprintf(buf, sizeof(buf), "  Content size: %zu bytes", message.content().size());
	DEBUG(buf);

	// Pack the message
	message.pack();

	// Check if message fits in a single packet - use OPPORTUNISTIC if so
	// OPPORTUNISTIC is simpler (no link needed) and works when identity is known
	if (message.packed_size() <= Type::Constants::ENCRYPTED_PACKET_MDU) {
		INFO("  Message fits in single packet, will use OPPORTUNISTIC delivery");
	} else {
		INFO("  Message too large for single packet, will use DIRECT (link) delivery");
	}

	// Set state to outbound
	message.state(Type::Message::OUTBOUND);

	// Add to pending queue
	pending_outbound_push(message);

	snprintf(buf, sizeof(buf), "Message queued for delivery (%zu pending)", _pending_outbound_count);
	INFO(buf);
}

// Process outbound queue
void LXMRouter::process_outbound() {
	if (_pending_outbound_count == 0) {
		return;
	}

	// Check backoff timer - don't process if we're in retry delay
	double now = Utilities::OS::time();
	if (now < _next_outbound_process_time) {
		return;  // Wait until retry delay expires
	}

	// Process one message per call to avoid blocking
	LXMessage* message_ptr = pending_outbound_front();
	if (!message_ptr) return;
	LXMessage& message = *message_ptr;
	char buf[128];

	snprintf(buf, sizeof(buf), "Processing outbound message to %s", message.destination_hash().toHex().c_str());
	DEBUG(buf);

	try {
		// If propagation-only mode is enabled, send via propagation node
		if (_propagation_only) {
			DEBUG("  Using PROPAGATED delivery (propagation-only mode)");
			message.set_method(Type::Message::PROPAGATED);
			if (send_propagated(message)) {
				INFO("Message sent via PROPAGATED delivery");
				if (_sent_callback) {
					_sent_callback(message);
				}
				LXMessage dummy;
				pending_outbound_pop(dummy);
			} else {
				// Propagation not ready yet - wait and retry
				DEBUG("  Propagation delivery not ready, will retry...");
				_next_outbound_process_time = now + OUTBOUND_RETRY_DELAY;
			}
			return;
		}

		// Determine delivery method based on message size
		bool use_opportunistic = (message.packed_size() <= Type::Constants::ENCRYPTED_PACKET_MDU);

		if (use_opportunistic) {
			// OPPORTUNISTIC delivery - send as single encrypted packet
			DEBUG("  Using OPPORTUNISTIC delivery (single packet)");

			// Check if we have a path to the destination
			if (!Transport::has_path(message.destination_hash())) {
				// Request path from network
				INFO("  No path to destination, requesting...");
				Transport::request_path(message.destination_hash());
				_next_outbound_process_time = now + PATH_REQUEST_WAIT;
				return;
			}

			// Try to recall the destination identity
			Identity dest_identity = Identity::recall(message.destination_hash());
			if (!dest_identity) {
				// Path exists but identity not cached yet - wait for announce
				INFO("  Path exists but identity not known, waiting for announce...");
				_next_outbound_process_time = now + OUTBOUND_RETRY_DELAY;
				return;
			}

			// Create destination and send packet
			if (send_opportunistic(message, dest_identity)) {
				INFO("Message sent via OPPORTUNISTIC delivery");

				// Call sent callback if registered
				if (_sent_callback) {
					_sent_callback(message);
				}

				// Remove from pending queue
				LXMessage dummy;
				pending_outbound_pop(dummy);
			} else {
				ERROR("Failed to send OPPORTUNISTIC message");
				message.state(Type::Message::FAILED);

				if (_failed_callback) {
					_failed_callback(message);
				}

				failed_outbound_push(message);
				LXMessage dummy;
				pending_outbound_pop(dummy);
			}
		} else {
			// DIRECT delivery - need a link for large messages
			DEBUG("  Using DIRECT delivery (via link)");

			// Check if we have a path to the destination
			if (!Transport::has_path(message.destination_hash())) {
				// Request path from network
				INFO("  No path to destination, requesting...");
				Transport::request_path(message.destination_hash());
				_next_outbound_process_time = now + PATH_REQUEST_WAIT;
				return;
			}

			// Get or establish link
			Link link = get_link_for_destination(message.destination_hash());

			if (!link) {
				WARNING("Failed to establish link for message delivery");
				// Set backoff timer to avoid tight loop
				_next_outbound_process_time = now + OUTBOUND_RETRY_DELAY;
				snprintf(buf, sizeof(buf), "  Will retry in %d seconds", (int)OUTBOUND_RETRY_DELAY);
				INFO(buf);
				return;
			}

			// Check link status
			if (link.status() != RNS::Type::Link::ACTIVE) {
				DEBUG("Link not yet active, waiting...");
				// Set shorter backoff for pending links
				_next_outbound_process_time = now + 1.0;  // Check again in 1 second
				return;
			}

			// Send via link
			if (send_via_link(message, link)) {
				INFO("Message sent successfully via link");

				// Call sent callback if registered
				if (_sent_callback) {
					_sent_callback(message);
				}

				// Remove from pending queue
				LXMessage dummy;
				pending_outbound_pop(dummy);
			} else {
				ERROR("Failed to send message via link");
				message.state(Type::Message::FAILED);

				// Call failed callback
				if (_failed_callback) {
					_failed_callback(message);
				}

				// Move to failed queue
				failed_outbound_push(message);
				LXMessage dummy;
				pending_outbound_pop(dummy);
			}
		}

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Exception processing outbound message: %s", e.what());
		ERROR(buf);
		message.state(Type::Message::FAILED);

		// Call failed callback
		if (_failed_callback) {
			_failed_callback(message);
		}

		// Move to failed queue
		failed_outbound_push(message);
		LXMessage dummy;
		pending_outbound_pop(dummy);
	}
}

// Process inbound queue
void LXMRouter::process_inbound() {
	if (_pending_inbound_count == 0) {
		return;
	}

	// Process one message per call
	LXMessage* message_ptr = pending_inbound_front();
	if (!message_ptr) return;
	LXMessage& message = *message_ptr;
	char buf[128];

	snprintf(buf, sizeof(buf), "Processing inbound message from %s", message.source_hash().toHex().c_str());
	DEBUG(buf);

	try {
		// Message is already unpacked and validated in on_packet()
		// Just invoke the delivery callback
		if (_delivery_callback) {
			_delivery_callback(message);
		}

		// Remove from pending queue
		LXMessage dummy;
		pending_inbound_pop(dummy);

		snprintf(buf, sizeof(buf), "Inbound message processed (%zu remaining)", _pending_inbound_count);
		INFO(buf);

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Exception processing inbound message: %s", e.what());
		ERROR(buf);
		// Discard message on error
		LXMessage dummy;
		pending_inbound_pop(dummy);
	}
}

// Announce delivery destination
void LXMRouter::announce(const Bytes& app_data, bool path_response) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Announcing LXMF delivery destination: %s", _delivery_destination.hash().toHex().c_str());
	INFO(buf);

	try {
		Bytes announce_data;

		// If app_data provided, use it directly
		if (app_data && app_data.size() > 0) {
			announce_data = app_data;
		}
		// Otherwise build LXMF-format app_data: [display_name, stamp_cost]
		else if (!_display_name.empty()) {
			MsgPack::Packer packer;
			// LXMF 0.5.0+ format: [display_name_bytes, stamp_cost]
			packer.pack(MsgPack::arr_size_t(2));
			// Pack display name as raw bytes
			std::vector<uint8_t> name_bytes(_display_name.begin(), _display_name.end());
			MsgPack::bin_t<uint8_t> name_bin;
			name_bin = name_bytes;
			packer.pack(name_bin);
			// Pack stamp_cost as nil (not used)
			packer.packNil();

			announce_data = Bytes(packer.data(), packer.size());
			snprintf(buf, sizeof(buf), "  Built LXMF app_data for display_name: %s", _display_name.c_str());
			DEBUG(buf);
		}

		snprintf(buf, sizeof(buf), "  Name hash: %s", RNS::Destination::name_hash("lxmf", "delivery").toHex().c_str());
		DEBUG(buf);
		snprintf(buf, sizeof(buf), "  App_data (%zu bytes): %s", announce_data.size(),
		         (announce_data.size() > 0 ? announce_data.toHex().c_str() : "(empty)"));
		DEBUG(buf);
		DEBUG("  Calling _delivery_destination.announce()...");
		_delivery_destination.announce(announce_data, path_response);
		_last_announce_time = Utilities::OS::time();
		INFO("Announce sent successfully");

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Failed to announce: %s", e.what());
		ERROR(buf);
	}
}

// Set announce interval
void LXMRouter::set_announce_interval(uint32_t interval) {
	_announce_interval = interval;
	if (interval > 0) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Auto-announce interval set to %u seconds", interval);
		INFO(buf);
	} else {
		INFO("Auto-announce disabled");
	}
}

// Set announce at start
void LXMRouter::set_announce_at_start(bool enabled) {
	_announce_at_start = enabled;
	DEBUG(enabled ? "Announce at start: enabled" : "Announce at start: disabled");
}

// Set display name for announces
void LXMRouter::set_display_name(const std::string& name) {
	_display_name = name;
	if (!name.empty()) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Display name set to: %s", name.c_str());
		INFO(buf);
	}
}

// Clear failed outbound
void LXMRouter::clear_failed_outbound() {
	size_t count = _failed_outbound_count;
	// Reset circular buffer indices
	for (size_t i = 0; i < FAILED_OUTBOUND_SIZE; i++) {
		_failed_outbound_pool[i] = LXMessage();
	}
	_failed_outbound_head = 0;
	_failed_outbound_tail = 0;
	_failed_outbound_count = 0;
	char buf[64];
	snprintf(buf, sizeof(buf), "Cleared %zu failed outbound messages", count);
	INFO(buf);
}

// Retry failed outbound
void LXMRouter::retry_failed_outbound() {
	if (_failed_outbound_count == 0) {
		return;
	}

	char buf[64];
	snprintf(buf, sizeof(buf), "Retrying %zu failed messages", _failed_outbound_count);
	INFO(buf);

	// Move all failed messages back to pending
	LXMessage message;
	while (failed_outbound_pop(message)) {
		message.state(Type::Message::OUTBOUND);
		pending_outbound_push(message);
	}
}

// Packet callback - receive LXMF messages
void LXMRouter::on_packet(const Bytes& data, const Packet& packet) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Received LXMF message packet (%zu bytes)", data.size());
	INFO(buf);
	snprintf(buf, sizeof(buf), "  From: %s", packet.destination_hash().toHex().c_str());
	DEBUG(buf);
	snprintf(buf, sizeof(buf), "  Destination type: %u", (uint8_t)packet.destination_type());
	DEBUG(buf);

	try {
		// Build LXMF data based on delivery method (matches Python LXMF exactly)
		Bytes lxmf_data;
		Type::Message::Method method;

		if (packet.destination_type() != RNS::Type::Destination::LINK) {
			// OPPORTUNISTIC delivery: destination hash is NOT in the encrypted data
			// We need to prepend it from the packet destination
			method = Type::Message::OPPORTUNISTIC;
			INFO("  Delivery method: OPPORTUNISTIC (prepending destination hash)");
			lxmf_data = _delivery_destination.hash() + data;
		} else {
			// DIRECT delivery via Link: data already contains everything
			method = Type::Message::DIRECT;
			INFO("  Delivery method: DIRECT (data complete)");
			lxmf_data = data;
		}

		snprintf(buf, sizeof(buf), "  LXMF data size after processing: %zu bytes", lxmf_data.size());
		DEBUG(buf);

		// Unpack LXMF message from packet data
		LXMessage message = LXMessage::unpack_from_bytes(lxmf_data, method);

		snprintf(buf, sizeof(buf), "  Message hash: %s", message.hash().toHex().c_str());
		DEBUG(buf);
		snprintf(buf, sizeof(buf), "  Source: %s", message.source_hash().toHex().c_str());
		DEBUG(buf);
		snprintf(buf, sizeof(buf), "  Content size: %zu bytes", message.content().size());
		DEBUG(buf);

		// Verify destination matches our delivery destination
		if (message.destination_hash() != _delivery_destination.hash()) {
			WARNING("Message destination mismatch - ignoring");
			return;
		}

		// Signature validation
		if (!message.signature_validated()) {
			WARNING("Message signature not validated");
			snprintf(buf, sizeof(buf), "  Unverified reason: %u", (uint8_t)message.unverified_reason());
			DEBUG(buf);

			// For Phase 1 MVP, we'll still accept messages with unknown source
			// (signature will be validated later if source identity is learned)
			if (message.unverified_reason() != Type::Message::SOURCE_UNKNOWN) {
				WARNING("  Rejecting message with invalid signature");
				return;
			}
		}

		// Stamp enforcement (if enabled)
		if (_stamp_cost > 0 && _enforce_stamps) {
			if (!message.validate_stamp(_stamp_cost)) {
				snprintf(buf, sizeof(buf), "  Rejecting message with invalid or missing stamp (required cost=%u)", _stamp_cost);
				WARNING(buf);
				return;
			}
			INFO("  Stamp validated");
		}

		// Send delivery proof back to sender (matches Python LXMF packet.prove())
		// Make a non-const copy to call prove() since Packet uses shared_ptr internally
		Packet proof_packet = packet;
		INFO("  Sending delivery proof");
		proof_packet.prove();

		// Add to inbound queue
		pending_inbound_push(message);

		snprintf(buf, sizeof(buf), "Message queued for processing (%zu pending)", _pending_inbound_count);
		INFO(buf);

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Failed to unpack LXMF message: %s", e.what());
		ERROR(buf);
	}
}

// Get or establish link to destination
Link LXMRouter::get_link_for_destination(const Bytes& destination_hash) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Getting link for destination %s", destination_hash.toHex().c_str());
	DEBUG(buf);

	// Check if we already have an active link
	DirectLinkSlot* slot = find_direct_link_slot(destination_hash);
	if (slot) {
		Link& existing_link = slot->link;

		// Check if link is still valid
		if (existing_link && existing_link.status() == RNS::Type::Link::ACTIVE) {
			DEBUG("  Using existing active link");
			return existing_link;
		} else if (existing_link && existing_link.status() == RNS::Type::Link::PENDING) {
			// Check if pending link has timed out
			double age = Utilities::OS::time() - slot->creation_time;
			if (age > LINK_ESTABLISHMENT_TIMEOUT) {
				snprintf(buf, sizeof(buf), "  Pending link timed out after %ds, removing", (int)age);
				WARNING(buf);
				slot->clear();
				// Fall through to create new link
			} else {
				snprintf(buf, sizeof(buf), "  Using existing pending link (age: %ds)", (int)age);
				DEBUG(buf);
				return existing_link;
			}
		} else {
			DEBUG("  Existing link is not active, removing");
			slot->clear();
		}
	}

	// Need to establish new link
	snprintf(buf, sizeof(buf), "  Establishing new link to %s", destination_hash.toHex().c_str());
	INFO(buf);

	try {
		// Create destination object for link
		// We need to recall the identity if we have it
		Identity dest_identity = Identity::recall(destination_hash);

		if (!dest_identity) {
			// We need the identity to establish link
			WARNING("  Don't have identity for destination - cannot establish link");
			WARNING("  Destination must announce first");
			return Link(RNS::Type::NONE);
		}

		// Create destination for link
		Destination link_destination(
			dest_identity,
			RNS::Type::Destination::OUT,
			RNS::Type::Destination::SINGLE,
			"lxmf",
			"delivery"
		);

		// Create link
		Link link(link_destination);

		// Register in global registry for link callbacks
		RouterRegistrySlot* reg_slot = find_router_registry_slot(link_destination.hash());
		if (!reg_slot) {
			reg_slot = find_empty_router_registry_slot();
			if (reg_slot) {
				reg_slot->in_use = true;
				reg_slot->set_destination_hash(link_destination.hash());
				reg_slot->router = this;
			}
		}

		// Register link callbacks (using static functions)
		link.set_link_established_callback(static_link_established_callback);
		link.set_link_closed_callback(static_link_closed_callback);

		// Store link and creation time in fixed pool
		DirectLinkSlot* new_slot = find_empty_direct_link_slot();
		if (new_slot) {
			new_slot->in_use = true;
			new_slot->set_destination_hash(destination_hash);
			new_slot->link = link;
			new_slot->creation_time = Utilities::OS::time();
		} else {
			WARNING("  Direct links pool full - cannot store new link");
		}

		INFO("  Link establishment initiated");
		return link;

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "  Failed to establish link: %s", e.what());
		ERROR(buf);
		return Link(RNS::Type::NONE);
	}
}

// Send message via link
bool LXMRouter::send_via_link(LXMessage& message, Link& link) {
	INFO("Sending LXMF message via link");
	char buf[128];
	snprintf(buf, sizeof(buf), "  Message size: %zu bytes", message.packed_size());
	DEBUG(buf);
	snprintf(buf, sizeof(buf), "  Representation: %u", (uint8_t)message.representation());
	DEBUG(buf);

	try {
		// Ensure message is packed
		if (!message.packed_size()) {
			message.pack();
		}

		// Check that link is active
		if (!link || link.status() != RNS::Type::Link::ACTIVE) {
			ERROR("Cannot send message - link is not active");
			return false;
		}

		message.state(Type::Message::SENDING);

		if (message.representation() == Type::Message::PACKET) {
			// Send as single packet over link
			snprintf(buf, sizeof(buf), "  Sending as single packet (%zu bytes)", message.packed_size());
			INFO(buf);

			Packet packet(link, message.packed());
			packet.send();

			message.state(Type::Message::SENT);
			INFO("Message sent successfully as packet");
			return true;

		} else if (message.representation() == Type::Message::RESOURCE) {
			// Send as resource over link with concluded callback
			snprintf(buf, sizeof(buf), "  Sending as resource (%zu bytes)", message.packed_size());
			INFO(buf);

			// Create resource with our callback to track completion
			Resource resource(message.packed(), link, true, true, static_outbound_resource_concluded);

			// Track this resource so we can match the callback to the message
			if (resource.hash()) {
				OutboundResourceSlot* slot = find_empty_outbound_resource_slot();
				if (slot) {
					slot->in_use = true;
					slot->set_resource_hash(resource.hash());
					slot->set_message_hash(message.hash());
					snprintf(buf, sizeof(buf), "  Tracking resource %.16s for message %.16s",
					         resource.hash().toHex().c_str(), message.hash().toHex().c_str());
					DEBUG(buf);
				} else {
					WARNING("  Outbound resources pool full - cannot track resource");
				}
			}

			message.state(Type::Message::SENT);
			INFO("Message resource transfer initiated");
			return true;

		} else {
			ERROR("Unknown message representation");
			message.state(Type::Message::FAILED);
			return false;
		}

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Failed to send message: %s", e.what());
		ERROR(buf);
		return false;
	}
}

// Send message via OPPORTUNISTIC delivery (single encrypted packet)
bool LXMRouter::send_opportunistic(LXMessage& message, const Identity& dest_identity) {
	INFO("Sending LXMF message via OPPORTUNISTIC delivery");
	char buf[128];
	snprintf(buf, sizeof(buf), "  Message size: %zu bytes", message.packed_size());
	DEBUG(buf);

	try {
		// Create destination object for the remote peer's LXMF delivery
		Destination destination(
			dest_identity,
			RNS::Type::Destination::OUT,
			RNS::Type::Destination::SINGLE,
			"lxmf",
			"delivery"
		);

		// Verify destination hash matches
		if (destination.hash() != message.destination_hash()) {
			ERROR("Destination hash mismatch!");
			snprintf(buf, sizeof(buf), "  Expected: %s", message.destination_hash().toHex().c_str());
			DEBUG(buf);
			snprintf(buf, sizeof(buf), "  Got: %s", destination.hash().toHex().c_str());
			DEBUG(buf);
			return false;
		}

		// For OPPORTUNISTIC, we strip the destination hash from the packed data
		// since it's already in the packet header
		Bytes packet_data = message.packed().mid(Type::Constants::DESTINATION_LENGTH);
		snprintf(buf, sizeof(buf), "  Packet data size: %zu bytes", packet_data.size());
		DEBUG(buf);

		// Create and send packet
		Packet packet(destination, packet_data, RNS::Type::Packet::DATA);
		PacketReceipt receipt = packet.send();

		// Register proof callback to track delivery confirmation
		if (receipt) {
			receipt.set_delivery_callback(static_proof_callback);
			PendingProofSlot* slot = find_empty_pending_proof_slot();
			if (slot) {
				slot->in_use = true;
				slot->set_packet_hash(receipt.hash());
				slot->set_message_hash(message.hash());
				snprintf(buf, sizeof(buf), "  Registered proof callback for packet %.16s...", receipt.hash().toHex().c_str());
				DEBUG(buf);
			} else {
				WARNING("  Pending proofs pool full - cannot track delivery proof");
			}
		}

		message.state(Type::Message::SENT);
		INFO("  OPPORTUNISTIC packet sent");

		return true;

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Failed to send OPPORTUNISTIC message: %s", e.what());
		ERROR(buf);
		return false;
	}
}

// Handle delivery proof for DIRECT messages
void LXMRouter::handle_direct_proof(const Bytes& message_hash) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Processing DIRECT delivery proof for message %.16s...", message_hash.toHex().c_str());
	INFO(buf);

	// Track notified routers to avoid duplicates (max ROUTER_REGISTRY_SIZE)
	LXMRouter* notified_routers[ROUTER_REGISTRY_SIZE];
	size_t notified_count = 0;

	// Call delivered callback for all unique routers
	for (size_t i = 0; i < ROUTER_REGISTRY_SIZE; i++) {
		if (_router_registry_pool[i].in_use) {
			LXMRouter* router = _router_registry_pool[i].router;
			// Check if already notified
			bool already_notified = false;
			for (size_t j = 0; j < notified_count; j++) {
				if (notified_routers[j] == router) {
					already_notified = true;
					break;
				}
			}
			if (!already_notified && router && router->_delivered_callback) {
				notified_routers[notified_count++] = router;
				Bytes empty_hash;
				LXMessage msg(empty_hash, empty_hash);
				msg.hash(message_hash);
				msg.state(Type::Message::DELIVERED);
				router->_delivered_callback(msg);
			}
		}
	}
}

// Link established callback
void LXMRouter::on_link_established(const Link& link) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Link established to %s", link.destination().hash().toHex().c_str());
	INFO(buf);
	// Link is now active, process_outbound() will pick it up
	// Note: Delivery confirmation comes from Resource completed callback
}

// Link closed callback
void LXMRouter::on_link_closed(const Link& link) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Link closed to %s", link.destination().hash().toHex().c_str());
	INFO(buf);

	// Remove from active links
	DirectLinkSlot* slot = find_direct_link_slot(link.destination().hash());
	if (slot) {
		slot->clear();
		DEBUG("  Removed link from cache");
	}
}

// Incoming link established callback (for DIRECT delivery to our destination)
void LXMRouter::on_incoming_link_established(Link& link) {
	INFO("Incoming link established from remote peer");
	char buf[128];
	snprintf(buf, sizeof(buf), "  Link ID: %s", link.link_id().toHex().c_str());
	DEBUG(buf);

	// Set up resource concluded callback to receive LXMF messages over this link
	link.set_resource_concluded_callback(static_resource_concluded_callback);
	DEBUG("  Resource callback registered for incoming LXMF messages");
}

// Resource concluded callback (LXMF message received via DIRECT delivery)
void LXMRouter::on_resource_concluded(const RNS::Resource& resource) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Resource concluded, status=%d", (int)resource.status());
	DEBUG(buf);

	if (resource.status() != RNS::Type::Resource::COMPLETE) {
		snprintf(buf, sizeof(buf), "Resource transfer failed with status %d", (int)resource.status());
		WARNING(buf);
		return;
	}

	// Get the resource data - this is the LXMF message
	Bytes data = resource.data();
	snprintf(buf, sizeof(buf), "Received LXMF message via DIRECT delivery (%zu bytes)", data.size());
	INFO(buf);

	try {
		// DIRECT delivery via resource: data is the full packed LXMF message
		// Format: destination_hash + source_hash + signature + payload
		// (same as OPPORTUNISTIC, just delivered via link resource instead of single packet)

		// Unpack the LXMF message
		LXMessage message = LXMessage::unpack_from_bytes(data, Type::Message::DIRECT);

		snprintf(buf, sizeof(buf), "  Message hash: %s", message.hash().toHex().c_str());
		DEBUG(buf);
		snprintf(buf, sizeof(buf), "  Source: %s", message.source_hash().toHex().c_str());
		DEBUG(buf);
		snprintf(buf, sizeof(buf), "  Content size: %zu bytes", message.content().size());
		DEBUG(buf);

		// Verify destination matches
		if (message.destination_hash() != _delivery_destination.hash()) {
			WARNING("Message destination mismatch - ignoring");
			return;
		}

		// Signature validation (same as on_packet)
		if (!message.signature_validated()) {
			WARNING("Message signature not validated");
			snprintf(buf, sizeof(buf), "  Unverified reason: %u", (uint8_t)message.unverified_reason());
			DEBUG(buf);

			// Accept messages with unknown source (signature validated later)
			if (message.unverified_reason() != Type::Message::SOURCE_UNKNOWN) {
				WARNING("  Rejecting message with invalid signature");
				return;
			}
		}

		// Stamp enforcement (if enabled)
		if (_stamp_cost > 0 && _enforce_stamps) {
			if (!message.validate_stamp(_stamp_cost)) {
				snprintf(buf, sizeof(buf), "  Rejecting message with invalid or missing stamp (required cost=%u)", _stamp_cost);
				WARNING(buf);
				return;
			}
			INFO("  Stamp validated");
		}

		// Note: We don't need to send a custom delivery proof here.
		// The sender gets delivery confirmation when the Resource completes
		// (via RESOURCE_PRF from RNS layer), which triggers their callback.

		// Add to inbound queue for processing
		pending_inbound_push(message);
		INFO("  Message queued for delivery");

	} catch (const std::exception& e) {
		snprintf(buf, sizeof(buf), "Failed to process DIRECT message: %s", e.what());
		ERROR(buf);
	}
}

// ============== Propagation Node Support ==============

void LXMRouter::set_propagation_node_manager(PropagationNodeManager* manager) {
	_propagation_manager = manager;
	INFO("Propagation node manager set");
}

void LXMRouter::set_outbound_propagation_node(const Bytes& node_hash) {
	if (node_hash.size() == 0) {
		_outbound_propagation_node = {};
		_outbound_propagation_link = Link(RNS::Type::NONE);
		INFO("Cleared outbound propagation node");
		return;
	}

	// Check if changing to a different node
	if (_outbound_propagation_node != node_hash) {
		// Tear down existing link if any
		if (_outbound_propagation_link && _outbound_propagation_link.status() != RNS::Type::Link::CLOSED) {
			_outbound_propagation_link.teardown();
		}
		_outbound_propagation_link = Link(RNS::Type::NONE);
	}

	_outbound_propagation_node = node_hash;
	char buf[64];
	snprintf(buf, sizeof(buf), "Set outbound propagation node to %.16s...", node_hash.toHex().c_str());
	INFO(buf);
}

void LXMRouter::register_sync_complete_callback(SyncCompleteCallback callback) {
	_sync_complete_callback = callback;
	DEBUG("Sync complete callback registered");
}

// Static callback for outbound propagation resource
void LXMRouter::static_propagation_resource_concluded(const Resource& resource) {
	Bytes resource_hash = resource.hash();
	char buf[128];
	snprintf(buf, sizeof(buf), "Propagation resource concluded: %.16s...", resource_hash.toHex().c_str());
	DEBUG(buf);
	snprintf(buf, sizeof(buf), "  Status: %d", (int)resource.status());
	DEBUG(buf);

	PropResourceSlot* slot = find_prop_resource_slot(resource_hash);
	if (!slot) {
		DEBUG("  Resource not in pending propagation map");
		return;
	}

	Bytes message_hash = slot->message_hash_bytes();

	if (resource.status() == RNS::Type::Resource::COMPLETE) {
		snprintf(buf, sizeof(buf), "PROPAGATED delivery to node confirmed for message %.16s...", message_hash.toHex().c_str());
		INFO(buf);

		// For PROPAGATED, "delivered" means delivered to propagation node, not final recipient
		// We mark it as SENT (not DELIVERED) to indicate it's on the propagation network
		LXMRouter* notified_routers[ROUTER_REGISTRY_SIZE];
		size_t notified_count = 0;

		for (size_t i = 0; i < ROUTER_REGISTRY_SIZE; i++) {
			if (_router_registry_pool[i].in_use) {
				LXMRouter* router = _router_registry_pool[i].router;
				bool already_notified = false;
				for (size_t j = 0; j < notified_count; j++) {
					if (notified_routers[j] == router) {
						already_notified = true;
						break;
					}
				}
				if (!already_notified && router && router->_sent_callback) {
					notified_routers[notified_count++] = router;
					Bytes empty_hash;
					LXMessage msg(empty_hash, empty_hash);
					msg.hash(message_hash);
					msg.state(Type::Message::SENT);
					router->_sent_callback(msg);
				}
			}
		}
	} else {
		snprintf(buf, sizeof(buf), "PROPAGATED resource transfer failed with status %d", (int)resource.status());
		WARNING(buf);
	}

	slot->clear();
}

bool LXMRouter::send_propagated(LXMessage& message) {
	INFO("Sending LXMF message via PROPAGATED delivery");
	char buf[128];

	// Get propagation node
	Bytes prop_node = _outbound_propagation_node;
	if (prop_node.size() == 0 && _propagation_manager) {
		DEBUG("  Looking for propagation node via manager...");
		auto nodes = _propagation_manager->get_nodes();
		snprintf(buf, sizeof(buf), "  Manager has %zu nodes", nodes.size());
		DEBUG(buf);
		prop_node = _propagation_manager->get_effective_node();
	}

	if (prop_node.size() == 0) {
		WARNING("No propagation node available for PROPAGATED delivery");
		return false;
	}

	snprintf(buf, sizeof(buf), "  Using propagation node: %.16s...", prop_node.toHex().c_str());
	DEBUG(buf);

	// Check/establish link to propagation node
	if (!_outbound_propagation_link ||
	    _outbound_propagation_link.status() == RNS::Type::Link::CLOSED) {

		// Check if we have a path
		if (!Transport::has_path(prop_node)) {
			INFO("  No path to propagation node, requesting...");
			Transport::request_path(prop_node);
			return false;  // Will retry next cycle
		}

		// Recall identity for propagation node
		Identity node_identity = Identity::recall(prop_node);
		if (!node_identity) {
			INFO("  Propagation node identity not known, waiting for announce...");
			return false;
		}

		// Create destination for propagation node
		Destination prop_dest(
			node_identity,
			RNS::Type::Destination::OUT,
			RNS::Type::Destination::SINGLE,
			"lxmf",
			"propagation"
		);

		// Create link with established callback
		_outbound_propagation_link = Link(prop_dest);
		INFO("  Establishing link to propagation node...");
		return false;  // Will retry when link established
	}

	// Check if link is active
	if (_outbound_propagation_link.status() != RNS::Type::Link::ACTIVE) {
		DEBUG("  Propagation link not yet active, waiting...");
		return false;  // Will retry
	}

	// Generate propagation stamp if required by node
	if (_propagation_manager) {
		auto node_info = _propagation_manager->get_node(prop_node);
		if (node_info && node_info.stamp_cost > 0) {
			snprintf(buf, sizeof(buf), "  Generating propagation stamp (cost=%u)...", node_info.stamp_cost);
			DEBUG(buf);
			Bytes stamp = message.generate_propagation_stamp(node_info.stamp_cost);
			if (stamp.size() == 0) {
				WARNING("  Failed to generate propagation stamp, sending anyway");
			}
		}
	}

	// Pack message for propagation
	Bytes prop_packed = message.pack_propagated();
	if (!prop_packed || prop_packed.size() == 0) {
		ERROR("  Failed to pack message for propagation");
		return false;
	}

	snprintf(buf, sizeof(buf), "  Propagated message size: %zu bytes", prop_packed.size());
	DEBUG(buf);

	// Send via resource with callback
	Resource resource(prop_packed, _outbound_propagation_link, true, true, static_propagation_resource_concluded);

	// Track this resource
	if (resource.hash()) {
		PropResourceSlot* slot = find_empty_prop_resource_slot();
		if (slot) {
			slot->in_use = true;
			slot->set_resource_hash(resource.hash());
			slot->set_message_hash(message.hash());
			snprintf(buf, sizeof(buf), "  Tracking propagation resource %.16s", resource.hash().toHex().c_str());
			DEBUG(buf);
		} else {
			WARNING("  Propagation resources pool full - cannot track resource");
		}
	}

	message.state(Type::Message::SENDING);
	INFO("  PROPAGATED resource transfer initiated");
	return true;
}

void LXMRouter::request_messages_from_propagation_node() {
	if (_sync_state != PR_IDLE && _sync_state != PR_COMPLETE && _sync_state != PR_FAILED) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Sync already in progress (state=%d)", _sync_state);
		WARNING(buf);
		return;
	}

	// Get propagation node
	Bytes prop_node = _outbound_propagation_node;
	if (!prop_node && _propagation_manager) {
		prop_node = _propagation_manager->get_effective_node();
	}

	if (!prop_node) {
		WARNING("No propagation node available for sync");
		_sync_state = PR_FAILED;
		return;
	}

	char buf[64];
	snprintf(buf, sizeof(buf), "Requesting messages from propagation node %.16s...", prop_node.toHex().c_str());
	INFO(buf);
	_sync_progress = 0.0f;

	// Check if link exists and is active
	if (_outbound_propagation_link && _outbound_propagation_link.status() == RNS::Type::Link::ACTIVE) {
		_sync_state = PR_LINK_ESTABLISHED;

		// TODO: Implement link.identify() and link.request() for full sync protocol
		// For now, we log that sync would happen here
		INFO("  Link active - sync protocol not yet implemented");
		INFO("  (Requires Link.identify() and Link.request() support)");

		_sync_state = PR_COMPLETE;
		_sync_progress = 1.0f;
		if (_sync_complete_callback) {
			_sync_complete_callback(0);
		}
	} else {
		// Need to establish link first
		if (!Transport::has_path(prop_node)) {
			INFO("  No path to propagation node, requesting...");
			Transport::request_path(prop_node);
			_sync_state = PR_PATH_REQUESTED;
		} else {
			Identity node_identity = Identity::recall(prop_node);
			if (!node_identity) {
				INFO("  Propagation node identity not known");
				_sync_state = PR_FAILED;
				return;
			}

			Destination prop_dest(
				node_identity,
				RNS::Type::Destination::OUT,
				RNS::Type::Destination::SINGLE,
				"lxmf",
				"propagation"
			);

			_outbound_propagation_link = Link(prop_dest);
			_sync_state = PR_LINK_ESTABLISHING;
			INFO("  Establishing link for sync...");
		}
	}
}

void LXMRouter::on_message_list_response(const Bytes& response) {
	// TODO: Implement when Link.request() is available
	DEBUG("on_message_list_response: Not yet implemented");
}

void LXMRouter::on_message_get_response(const Bytes& response) {
	// TODO: Implement when Link.request() is available
	DEBUG("on_message_get_response: Not yet implemented");
}

void LXMRouter::process_propagated_lxmf(const Bytes& lxmf_data) {
	// lxmf_data format: dest_hash (16 bytes) + encrypted_content
	if (lxmf_data.size() < Type::Constants::DESTINATION_LENGTH) {
		WARNING("Propagated LXMF data too short");
		return;
	}

	Bytes dest_hash = lxmf_data.left(Type::Constants::DESTINATION_LENGTH);

	// Verify this is for us
	if (dest_hash != _delivery_destination.hash()) {
		DEBUG("Received propagated message not addressed to us");
		return;
	}

	// Decrypt the content
	Bytes encrypted = lxmf_data.mid(Type::Constants::DESTINATION_LENGTH);
	Bytes decrypted = _identity.decrypt(encrypted);

	if (!decrypted || decrypted.size() == 0) {
		WARNING("Failed to decrypt propagated message");
		return;
	}

	// Reconstruct full LXMF data: dest_hash + decrypted
	Bytes full_data;
	full_data << dest_hash << decrypted;

	try {
		LXMessage message = LXMessage::unpack_from_bytes(full_data, Type::Message::PROPAGATED);

		// Check if message was unpacked successfully (has valid hash)
		if (message.hash().size() > 0) {
			// Track transient ID to avoid re-downloading
			Bytes transient_id = Identity::full_hash(lxmf_data);
			transient_ids_add(transient_id);

			// Queue for delivery
			pending_inbound_push(message);
			INFO("Propagated message queued for delivery");
		}
	} catch (const std::exception& e) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Failed to unpack propagated message: %s", e.what());
		ERROR(buf);
	}
}
