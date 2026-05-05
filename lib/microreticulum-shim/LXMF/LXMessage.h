#pragma once

#include "Type.h"
#include <Bytes.h>
#include <Identity.h>
#include <Destination.h>
#include <Link.h>

#include <string>
#include <memory>

namespace LXMF {

	/**
	 * @brief Fixed-size field entry to avoid heap fragmentation from std::map
	 */
	struct FieldEntry {
		bool in_use = false;
		RNS::Bytes key;
		RNS::Bytes value;
		void clear() { in_use = false; key.clear(); value.clear(); }
	};

	/// Maximum number of fields in an LXMF message
	static constexpr size_t MAX_FIELDS = 16;

	/**
	 * @brief LXMF Message class
	 *
	 * Represents a Lightweight Extensible Messaging Format message that can be
	 * sent over Reticulum network. Supports message packing, unpacking, signing,
	 * and verification.
	 *
	 * Message structure (packed):
	 * - 16 bytes: destination hash
	 * - 16 bytes: source hash
	 * - 64 bytes: Ed25519 signature
	 * - Variable: msgpack([timestamp, title, content, fields])
	 */
	class LXMessage {

	public:
		using Ptr = std::shared_ptr<LXMessage>;

	public:
		/**
		 * @brief Default constructor for use in fixed pools
		 */
		LXMessage() = default;

		/**
		 * @brief Construct a new LXMF message
		 *
		 * @param destination Destination object (or {Type::NONE} if creating from hash)
		 * @param source Source destination object (our identity)
		 * @param content Message content as bytes or string
		 * @param title Message title (optional)
		 * @param desired_method Delivery method (default: DIRECT)
		 */
		LXMessage(
			const RNS::Destination& destination,
			const RNS::Destination& source,
			const RNS::Bytes& content = {},
			const RNS::Bytes& title = {},
			Type::Message::Method desired_method = Type::Message::DIRECT
		);

		/**
		 * @brief Construct message from hashes (for unpacking)
		 */
		LXMessage(
			const RNS::Bytes& destination_hash,
			const RNS::Bytes& source_hash,
			const RNS::Bytes& content = {},
			const RNS::Bytes& title = {},
			Type::Message::Method desired_method = Type::Message::DIRECT
		);

		~LXMessage();

	public:
		/**
		 * @brief Pack the message into binary format
		 *
		 * Creates the LXMF message structure:
		 * 1. Payload = [timestamp, title, content, fields]
		 * 2. Hashed part = dest_hash + source_hash + msgpack(payload)
		 * 3. Hash (message ID) = SHA256(hashed_part)
		 * 4. Signed part = hashed_part + hash
		 * 5. Signature = Ed25519Sign(signed_part)
		 * 6. Packed = dest_hash + source_hash + signature + msgpack(payload)
		 *
		 * @return Packed message bytes
		 */
		const RNS::Bytes& pack();

		/**
		 * @brief Unpack an LXMF message from bytes
		 *
		 * @param lxmf_bytes Packed LXMF message
		 * @param original_method Original delivery method (optional)
		 * @param skip_signature_validation Skip Ed25519 validation (for trusted local storage)
		 * @return LXMessage object (or throws on error)
		 */
		static LXMessage unpack_from_bytes(const RNS::Bytes& lxmf_bytes, Type::Message::Method original_method = Type::Message::DIRECT, bool skip_signature_validation = false);

		/**
		 * @brief Validate the message signature
		 *
		 * Verifies the Ed25519 signature using the source identity.
		 * Requires source identity to be known (cached in Identity::_known_destinations).
		 *
		 * @return true if signature is valid, false otherwise
		 */
		bool validate_signature();

		/**
		 * @brief Send the message via a link
		 *
		 * For DIRECT delivery method:
		 * - If packed size <= 319 bytes: send as single packet
		 * - If packed size > 319 bytes: send as resource
		 *
		 * @param link Active link to send over
		 * @return true if send initiated successfully
		 */
		bool send_via_link(const RNS::Link& link);

		/**
		 * @brief Pack the message for PROPAGATED delivery
		 *
		 * Creates the LXMF propagation format:
		 * 1. Encrypt packed message (minus dest_hash) to destination
		 * 2. Format: [timestamp, [dest_hash + encrypted_data]]
		 * 3. Pack with msgpack
		 *
		 * @return Propagation-formatted message bytes
		 */
		RNS::Bytes pack_propagated();

		// ========== Stamp Proof-of-Work Methods ==========

		/**
		 * @brief Set the required stamp cost for this message
		 *
		 * @param cost Number of leading zero bits required (0 = no stamp)
		 */
		inline void set_stamp_cost(uint8_t cost) { _stamp_cost = cost; }

		/**
		 * @brief Get the required stamp cost
		 */
		inline uint8_t stamp_cost() const { return _stamp_cost; }

		/**
		 * @brief Check if message has a valid stamp
		 */
		inline bool has_valid_stamp() const { return _stamp_valid; }

		/**
		 * @brief Get the attached stamp
		 */
		inline const RNS::Bytes& stamp() const { return _stamp; }

		/**
		 * @brief Set the stamp (for received messages)
		 */
		inline void stamp(const RNS::Bytes& stamp) { _stamp = stamp; }

		/**
		 * @brief Validate the attached stamp against a required cost
		 *
		 * Generates workblock from message hash and verifies stamp.
		 *
		 * @param required_cost Minimum number of leading zero bits
		 * @return true if stamp is valid
		 */
		bool validate_stamp(uint8_t required_cost);

		/**
		 * @brief Generate a stamp for this message (blocking, CPU-intensive)
		 *
		 * Generates workblock and mines for valid stamp.
		 * Sets _stamp and _stamp_valid on success.
		 *
		 * @return The generated stamp, or empty on failure
		 */
		RNS::Bytes generate_stamp();

		/**
		 * @brief Set the propagation stamp for PROPAGATED delivery
		 *
		 * @param stamp The stamp to append to lxmf_data
		 */
		inline void set_propagation_stamp(const RNS::Bytes& stamp) { _propagation_stamp = stamp; }

		/**
		 * @brief Get the propagation stamp
		 */
		inline const RNS::Bytes& propagation_stamp() const { return _propagation_stamp; }

		/**
		 * @brief Generate propagation stamp for this message
		 *
		 * Uses transient_id and PN-specific workblock rounds.
		 *
		 * @param target_cost Required stamp cost from propagation node
		 * @return The generated stamp, or empty on failure
		 */
		RNS::Bytes generate_propagation_stamp(uint8_t target_cost);

		/**
		 * @brief Get message state
		 */
		inline Type::Message::State state() const { return _state; }

		/**
		 * @brief Set message state
		 */
		inline void state(Type::Message::State state) { _state = state; }

		inline int delivery_attempts() const { return _delivery_attempts; }
		inline void increment_delivery_attempts() { _delivery_attempts++; }

		/**
		 * @brief Get message hash (ID)
		 */
		inline const RNS::Bytes& hash() const { return _hash; }

		/**
		 * @brief Set message hash (for delivery confirmation callbacks)
		 */
		inline void hash(const RNS::Bytes& hash) { _hash = hash; }

		/**
		 * @brief Get message content
		 */
		inline const RNS::Bytes& content() const { return _content; }

		/**
		 * @brief Set message content
		 */
		inline void content(const RNS::Bytes& content) { _content = content; _packed_valid = false; }

		/**
		 * @brief Get message title
		 */
		inline const RNS::Bytes& title() const { return _title; }

		/**
		 * @brief Set message title
		 */
		inline void title(const RNS::Bytes& title) { _title = title; _packed_valid = false; }

		/**
		 * @brief Get field count
		 */
		inline size_t fields_count() const { return _fields_count; }

		/**
		 * @brief Get field at index (for iteration)
		 * @return Pointer to field entry, or nullptr if index out of range
		 */
		inline const FieldEntry* field_at(size_t index) const {
			if (index >= MAX_FIELDS) return nullptr;
			return _fields_pool[index].in_use ? &_fields_pool[index] : nullptr;
		}

		/**
		 * @brief Set a field value
		 * @return true if successful, false if pool is full
		 */
		bool fields_set(const RNS::Bytes& key, const RNS::Bytes& value);

		/**
		 * @brief Get a field value by key
		 * @return Pointer to value, or nullptr if not found
		 */
		const RNS::Bytes* fields_get(const RNS::Bytes& key) const;

		/**
		 * @brief Check if a field exists
		 */
		bool fields_has(const RNS::Bytes& key) const;

		/**
		 * @brief Clear all fields
		 */
		void fields_clear();

		/**
		 * @brief Get timestamp
		 */
		inline double timestamp() const { return _timestamp; }

		/**
		 * @brief Get destination hash
		 */
		inline const RNS::Bytes& destination_hash() const { return _destination_hash; }

		/**
		 * @brief Set destination hash (for when identity is not known)
		 */
		inline void destination_hash(const RNS::Bytes& hash) { _destination_hash = hash; _packed_valid = false; }

		/**
		 * @brief Get source hash
		 */
		inline const RNS::Bytes& source_hash() const { return _source_hash; }

		/**
		 * @brief Check if signature was validated
		 */
		inline bool signature_validated() const { return _signature_validated; }

		/**
		 * @brief Get unverified reason (if signature not validated)
		 */
		inline Type::Message::UnverifiedReason unverified_reason() const { return _unverified_reason; }

		/**
		 * @brief Get packed message bytes
		 */
		inline const RNS::Bytes& packed() const { return _packed; }

		/**
		 * @brief Get packed message size
		 */
		inline size_t packed_size() const { return _packed.size(); }

		/**
		 * @brief Get delivery method
		 */
		inline Type::Message::Method method() const { return _method; }

		/**
		 * @brief Set delivery method
		 */
		inline void set_method(Type::Message::Method method) { _method = method; _desired_method = method; }

		/**
		 * @brief Get message representation (PACKET or RESOURCE)
		 */
		inline Type::Message::Representation representation() const { return _representation; }

		/**
		 * @brief Check if this is an incoming message
		 */
		inline bool incoming() const { return _incoming; }

		/**
		 * @brief Set incoming flag (for loading from storage)
		 */
		inline void incoming(bool incoming) { _incoming = incoming; }

		/**
		 * @brief String representation of message
		 */
		std::string toString() const;

	private:
		// Core message data
		RNS::Bytes _destination_hash;
		RNS::Bytes _source_hash;
		RNS::Bytes _content;
		RNS::Bytes _title;

		// Fixed-size field pool to avoid heap fragmentation
		FieldEntry _fields_pool[MAX_FIELDS];
		size_t _fields_count = 0;

		// Destination/Source objects (may be {Type::NONE} if creating from hashes)
		RNS::Destination _destination;
		RNS::Destination _source;

		// Message metadata
		RNS::Bytes _hash;           // Message ID (SHA256 of hashed_part)
		RNS::Bytes _signature;      // Ed25519 signature (64 bytes)
		double _timestamp = 0.0;    // Unix timestamp

		// Packing state
		RNS::Bytes _packed;         // Packed message bytes
		bool _packed_valid = false; // Whether _packed is up-to-date

		// Delivery parameters
		Type::Message::Method _desired_method = Type::Message::DIRECT;
		Type::Message::Method _method = Type::Message::DIRECT;
		Type::Message::Representation _representation = Type::Message::UNKNOWN;

		// Message state
		Type::Message::State _state = Type::Message::GENERATING;
		int _delivery_attempts = 0;

		// Signature validation
		bool _signature_validated = false;
		Type::Message::UnverifiedReason _unverified_reason = Type::Message::SOURCE_UNKNOWN;

		// Direction
		bool _incoming = false;  // true if received, false if created locally

		// Stamp proof-of-work
		RNS::Bytes _stamp;                    // 32-byte stamp for direct messages
		RNS::Bytes _propagation_stamp;        // 32-byte stamp for propagation delivery
		bool _stamp_valid = false;            // Whether stamp has been validated
		uint8_t _stamp_cost = 0;              // Required stamp cost (0 = no stamp needed)

		// Cached propagation data (to ensure stamp matches encrypted content)
		RNS::Bytes _propagation_encrypted;    // Cached encrypted content for propagation

	private:
		/**
		 * @brief Calculate message hash from components
		 */
		void calculate_hash();

		/**
		 * @brief Sign the message with source identity
		 */
		void sign_message();
	};

}  // namespace LXMF
