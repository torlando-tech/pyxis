#include "LXMessage.h"
#include "LXStamper.h"
#include <Log.h>
#include <Utilities/OS.h>
#include <Packet.h>
#include <Resource.h>

#include <MsgPack.h>

using namespace LXMF;
using namespace RNS;

// Constructor with destination and source objects
LXMessage::LXMessage(
	const Destination& destination,
	const Destination& source,
	const Bytes& content,
	const Bytes& title,
	Type::Message::Method desired_method
) :
	_destination(destination),
	_source(source),
	_content(content),
	_title(title),
	_desired_method(desired_method),
	_method(desired_method)
{
	// Extract hashes from destination objects
	if (destination) {
		_destination_hash = destination.hash();
	}
	if (source) {
		_source_hash = source.hash();
	}

	INFO("Created new LXMF message");
	DEBUG("  Destination: " + _destination_hash.toHex());
	DEBUG("  Source: " + _source_hash.toHex());
	DEBUG("  Content size: " + std::to_string(_content.size()) + " bytes");
}

// Constructor with hashes only (for unpacking)
LXMessage::LXMessage(
	const Bytes& destination_hash,
	const Bytes& source_hash,
	const Bytes& content,
	const Bytes& title,
	Type::Message::Method desired_method
) :
	_destination(RNS::Type::NONE),
	_source(RNS::Type::NONE),
	_destination_hash(destination_hash),
	_source_hash(source_hash),
	_content(content),
	_title(title),
	_desired_method(desired_method),
	_method(desired_method)
{
	DEBUG("Created LXMF message from hashes");
}

LXMessage::~LXMessage() {
	TRACE("LXMessage destroyed");
}

// Field helper methods for fixed-size array
bool LXMessage::fields_set(const Bytes& key, const Bytes& value) {
	// Check if key already exists, update if so
	for (size_t i = 0; i < MAX_FIELDS; ++i) {
		if (_fields_pool[i].in_use && _fields_pool[i].key == key) {
			_fields_pool[i].value = value;
			_packed_valid = false;
			return true;
		}
	}

	// Find an empty slot
	for (size_t i = 0; i < MAX_FIELDS; ++i) {
		if (!_fields_pool[i].in_use) {
			_fields_pool[i].in_use = true;
			_fields_pool[i].key = key;
			_fields_pool[i].value = value;
			++_fields_count;
			_packed_valid = false;
			return true;
		}
	}

	// Pool is full
	WARNING("LXMessage field pool full, cannot add more fields");
	return false;
}

const Bytes* LXMessage::fields_get(const Bytes& key) const {
	for (size_t i = 0; i < MAX_FIELDS; ++i) {
		if (_fields_pool[i].in_use && _fields_pool[i].key == key) {
			return &_fields_pool[i].value;
		}
	}
	return nullptr;
}

bool LXMessage::fields_has(const Bytes& key) const {
	return fields_get(key) != nullptr;
}

void LXMessage::fields_clear() {
	for (size_t i = 0; i < MAX_FIELDS; ++i) {
		_fields_pool[i].clear();
	}
	_fields_count = 0;
	_packed_valid = false;
}

// Pack the message into binary format
const Bytes& LXMessage::pack() {
	if (_packed_valid) {
		return _packed;
	}

	INFO("Packing LXMF message");

	// 1. Set timestamp if not already set
	if (_timestamp == 0.0) {
		_timestamp = Utilities::OS::time();
	}

	// 2. Pack 4-element payload (without stamp) for hash/signature computation.
	// Per Python LXMF: hash and signature are ALWAYS computed over the 4-element
	// payload [timestamp, title, content, fields], even when a stamp is present.
	// The stamp is only appended as a 5th element in the wire format.
	MsgPack::Packer packer;
	packer.packArraySize(4);

	// Element 0: timestamp (float64)
	packer.pack(_timestamp);

	// Element 1: title (binary)
	packer.packBinary(_title.data(), _title.size());

	// Element 2: content (binary)
	packer.packBinary(_content.data(), _content.size());

	// Element 3: fields (map) - iterate over fixed array
	packer.packMapSize(_fields_count);
	for (size_t i = 0; i < MAX_FIELDS; ++i) {
		if (_fields_pool[i].in_use) {
			packer.packBinary(_fields_pool[i].key.data(), _fields_pool[i].key.size());
			packer.packBinary(_fields_pool[i].value.data(), _fields_pool[i].value.size());
		}
	}

	Bytes payload_without_stamp(packer.data(), packer.size());

	// 3. Calculate hash: SHA256(dest_hash + source_hash + payload_without_stamp)
	// Hash is always over the 4-element payload (matching Python LXMF)
	Bytes hashed_part;
	hashed_part << _destination_hash;
	hashed_part << _source_hash;
	hashed_part << payload_without_stamp;

	_hash = Identity::full_hash(hashed_part);

	DEBUG("  Message hash: " + _hash.toHex());

	// 4. Create signed part: hashed_part + hash
	Bytes signed_part;
	signed_part << hashed_part;
	signed_part << _hash;

	// 5. Sign with source identity
	if (_source) {
		_signature = _source.sign(signed_part);
		_signature_validated = true;
		DEBUG("  Message signed (" + std::to_string(_signature.size()) + " bytes)");
	} else {
		ERROR("Cannot sign message - source destination not available");
		throw std::runtime_error("Cannot sign message without source destination");
	}

	// 6. Build wire payload — append stamp as 5th element if present
	bool has_stamp = (_stamp.size() == LXStamper::STAMP_SIZE);
	Bytes wire_payload;
	if (has_stamp) {
		MsgPack::Packer wire_packer;
		wire_packer.packArraySize(5);
		wire_packer.pack(_timestamp);
		wire_packer.packBinary(_title.data(), _title.size());
		wire_packer.packBinary(_content.data(), _content.size());
		wire_packer.packMapSize(_fields_count);
		for (size_t i = 0; i < MAX_FIELDS; ++i) {
			if (_fields_pool[i].in_use) {
				wire_packer.packBinary(_fields_pool[i].key.data(), _fields_pool[i].key.size());
				wire_packer.packBinary(_fields_pool[i].value.data(), _fields_pool[i].value.size());
			}
		}
		wire_packer.packBinary(_stamp.data(), _stamp.size());
		wire_payload = Bytes(wire_packer.data(), wire_packer.size());
		DEBUG("  Stamp included in wire payload (" + std::to_string(_stamp.size()) + " bytes)");
	} else {
		wire_payload = payload_without_stamp;
	}

	// 7. Pack final message: dest_hash + source_hash + signature + wire_payload
	_packed.clear();
	_packed << _destination_hash;
	_packed << _source_hash;
	_packed << _signature;
	_packed << wire_payload;

	_packed_valid = true;

	// 8. Determine delivery method and representation
	size_t content_size = wire_payload.size() - Type::Constants::TIMESTAMP_SIZE - Type::Constants::STRUCT_OVERHEAD;

	if (_desired_method == Type::Message::DIRECT) {
		// Use LoRa-constrained limit (63 bytes content) to ensure link packets fit within LoRa wire MTU
		if (content_size <= Type::Constants::LORA_LINK_PACKET_MAX_CONTENT) {
			_method = Type::Message::DIRECT;
			_representation = Type::Message::PACKET;
			INFO("  Message will be sent as single packet (" + std::to_string(_packed.size()) + " bytes)");
		} else {
			_method = Type::Message::DIRECT;
			_representation = Type::Message::RESOURCE;
			INFO("  Message will be sent as resource (" + std::to_string(_packed.size()) + " bytes)");
		}
	} else if (_desired_method == Type::Message::PROPAGATED) {
		// PROPAGATED: always use resource transfer to propagation node
		_method = Type::Message::PROPAGATED;
		_representation = Type::Message::RESOURCE;
		INFO("  Message will be sent via propagation (" + std::to_string(_packed.size()) + " bytes)");
	} else if (_desired_method == Type::Message::OPPORTUNISTIC) {
		// OPPORTUNISTIC: single encrypted packet, no link required
		if (_packed.size() <= Type::Constants::LORA_ENCRYPTED_PACKET_MDU) {
			_method = Type::Message::OPPORTUNISTIC;
			_representation = Type::Message::PACKET;
			INFO("  Message will be sent opportunistically (" + std::to_string(_packed.size()) + " bytes)");
		} else {
			// Too large for single packet, fall back to DIRECT
			_method = Type::Message::DIRECT;
			_representation = Type::Message::RESOURCE;
			INFO("  Message too large for OPPORTUNISTIC, using DIRECT resource (" + std::to_string(_packed.size()) + " bytes)");
		}
	} else {
		// Default fallback
		_method = Type::Message::DIRECT;
		_representation = Type::Message::PACKET;
	}

	_state = Type::Message::OUTBOUND;

	INFO("Message packed successfully (" + std::to_string(_packed.size()) + " bytes total)");
	DEBUG("  Overhead: " + std::to_string(Type::Constants::LXMF_OVERHEAD) + " bytes");
	DEBUG("  Payload: " + std::to_string(wire_payload.size()) + " bytes");

	return _packed;
}

// Unpack an LXMF message from bytes
LXMessage LXMessage::unpack_from_bytes(const Bytes& lxmf_bytes, Type::Message::Method original_method, bool skip_signature_validation) {
	INFO("Unpacking LXMF message from " + std::to_string(lxmf_bytes.size()) + " bytes");

	// 1. Extract fixed-size fields
	if (lxmf_bytes.size() < 2 * Type::Constants::DESTINATION_LENGTH + Type::Constants::SIGNATURE_LENGTH) {
		throw std::runtime_error("LXMF message too short");
	}

	size_t offset = 0;

	Bytes destination_hash = lxmf_bytes.mid(offset, Type::Constants::DESTINATION_LENGTH);
	offset += Type::Constants::DESTINATION_LENGTH;

	Bytes source_hash = lxmf_bytes.mid(offset, Type::Constants::DESTINATION_LENGTH);
	offset += Type::Constants::DESTINATION_LENGTH;

	Bytes signature = lxmf_bytes.mid(offset, Type::Constants::SIGNATURE_LENGTH);
	offset += Type::Constants::SIGNATURE_LENGTH;

	Bytes packed_payload = lxmf_bytes.mid(offset);

	DEBUG("  Destination hash: " + destination_hash.toHex());
	DEBUG("  Source hash: " + source_hash.toHex());
	DEBUG("  Signature: " + std::to_string(signature.size()) + " bytes");
	DEBUG("  Payload: " + std::to_string(packed_payload.size()) + " bytes");

	// Check for empty payload - this would cause msgpack to crash
	if (packed_payload.size() == 0) {
		ERROR("LXMF message has empty payload - cannot unpack");
		ERROR("  Raw bytes (" + std::to_string(lxmf_bytes.size()) + "): " + lxmf_bytes.toHex());
		throw std::runtime_error("LXMF message has empty payload");
	}

	// Log first few bytes of payload for debugging
	if (packed_payload.size() > 0) {
		DEBUG("  Payload first bytes: " + packed_payload.left(std::min((size_t)16, packed_payload.size())).toHex());
	}

	// 2. Unpack payload: [timestamp, title, content, fields] - matches Python LXMF exactly
	// The payload is msgpack array: [float64_timestamp, bin_title, bin_content, map_fields]
	MsgPack::Unpacker unpacker;
	unpacker.feed(packed_payload.data(), packed_payload.size());

	double timestamp = 0.0;
	Bytes title;
	Bytes content;

	// Temporary storage for fields before copying to message
	FieldEntry temp_fields[MAX_FIELDS];
	size_t temp_fields_count = 0;

	Bytes stamp;  // Optional 5th element

	try {
		MsgPack::bin_t<uint8_t> title_bin;
		MsgPack::bin_t<uint8_t> content_bin;

		// First, read the array header (should be 4 or 5 elements)
		MsgPack::arr_size_t arr_size;
		unpacker.deserialize(arr_size);
		DEBUG("  Msgpack array size: " + std::to_string(arr_size.size()));

		if (arr_size.size() < 4) {
			throw std::runtime_error("LXMF payload array too short: " + std::to_string(arr_size.size()));
		}

		// Unpack timestamp (element 0)
		unpacker.deserialize(timestamp);
		DEBUG("  Parsed timestamp: " + std::to_string(timestamp));

		// Unpack title (element 1) - as binary
		unpacker.deserialize(title_bin);
		title = Bytes(title_bin);
		DEBUG("  Parsed title: " + std::to_string(title.size()) + " bytes");

		// Unpack content (element 2) - as binary
		unpacker.deserialize(content_bin);
		content = Bytes(content_bin);
		DEBUG("  Parsed content: " + std::to_string(content.size()) + " bytes");

		// Unpack fields map (element 3)
		MsgPack::map_size_t map_size;
		unpacker.deserialize(map_size);
		DEBUG("  Msgpack map size: " + std::to_string(map_size.size()));

		// Unpack each field (key-value pairs) into temporary storage
		for (size_t i = 0; i < map_size.size() && temp_fields_count < MAX_FIELDS; ++i) {
			MsgPack::bin_t<uint8_t> key_bin;
			MsgPack::bin_t<uint8_t> value_bin;

			unpacker.deserialize(key_bin);
			unpacker.deserialize(value_bin);

			temp_fields[temp_fields_count].in_use = true;
			temp_fields[temp_fields_count].key = Bytes(key_bin);
			temp_fields[temp_fields_count].value = Bytes(value_bin);
			++temp_fields_count;
		}

		if (map_size.size() > MAX_FIELDS) {
			WARNING("LXMF message has " + std::to_string(map_size.size()) +
					" fields, but max is " + std::to_string(MAX_FIELDS) + " - some fields truncated");
		}

		// Unpack stamp (element 4) - optional, 32 bytes
		if (arr_size.size() > 4) {
			MsgPack::bin_t<uint8_t> stamp_bin;
			unpacker.deserialize(stamp_bin);
			stamp = Bytes(stamp_bin);
			DEBUG("  Parsed stamp: " + std::to_string(stamp.size()) + " bytes");
		}

	} catch (const std::exception& e) {
		ERROR("Failed to unpack LXMF message payload: " + std::string(e.what()));
		throw;
	}

	DEBUG("  Timestamp: " + std::to_string(timestamp));
	DEBUG("  Title size: " + std::to_string(title.size()) + " bytes");
	DEBUG("  Content size: " + std::to_string(content.size()) + " bytes");
	DEBUG("  Fields: " + std::to_string(temp_fields_count));

	// 3. Create message object
	LXMessage message(destination_hash, source_hash, content, title, original_method);

	// Copy fields to message
	for (size_t i = 0; i < temp_fields_count; ++i) {
		message.fields_set(temp_fields[i].key, temp_fields[i].value);
	}
	message._timestamp = timestamp;
	message._signature = signature;
	message._packed = lxmf_bytes;
	message._packed_valid = true;
	message._incoming = true;
	message._state = Type::Message::DELIVERED;

	// Assign stamp if present
	if (stamp.size() == LXStamper::STAMP_SIZE) {
		message._stamp = stamp;
		DEBUG("  Stamp attached to message");
	}

	// 4. Calculate hash for verification
	// Per Python LXMF: hash is computed over 4-element payload (without stamp).
	// If stamp was present, re-pack without it.
	Bytes payload_for_hash;
	if (stamp.size() == LXStamper::STAMP_SIZE) {
		MsgPack::Packer repacker;
		repacker.packArraySize(4);
		repacker.pack(timestamp);
		repacker.packBinary(title.data(), title.size());
		repacker.packBinary(content.data(), content.size());
		repacker.packMapSize(temp_fields_count);
		for (size_t i = 0; i < temp_fields_count; ++i) {
			repacker.packBinary(temp_fields[i].key.data(), temp_fields[i].key.size());
			repacker.packBinary(temp_fields[i].value.data(), temp_fields[i].value.size());
		}
		payload_for_hash = Bytes(repacker.data(), repacker.size());
	} else {
		payload_for_hash = packed_payload;
	}

	Bytes hashed_part;
	hashed_part << destination_hash;
	hashed_part << source_hash;
	hashed_part << payload_for_hash;

	message._hash = Identity::full_hash(hashed_part);

	DEBUG("  Calculated hash: " + message._hash.toHex());

	// 5. Try to validate signature (skip if loading from trusted storage)
	if (skip_signature_validation) {
		// Trust that signature was validated when originally received
		message._signature_validated = true;
		DEBUG("  Skipping signature validation (trusted storage)");
	} else {
		// Check if we have the source identity cached
		Identity source_identity = Identity::recall(source_hash);
		if (source_identity) {
			INFO("  Source identity found in cache, validating signature");
			message._source = Destination(source_identity, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE, "lxmf", "delivery");

			// Validate signature
			Bytes signed_part;
			signed_part << hashed_part;
			signed_part << message._hash;

			if (source_identity.validate(signature, signed_part)) {
				message._signature_validated = true;
				INFO("  Signature validated successfully");
			} else {
				message._signature_validated = false;
				message._unverified_reason = Type::Message::SIGNATURE_INVALID;
				WARNING("  Signature validation failed!");
			}
		} else {
			message._signature_validated = false;
			message._unverified_reason = Type::Message::SOURCE_UNKNOWN;
			DEBUG("  Source identity unknown, signature not validated");
		}
	}

	// Similarly try to get destination identity
	Identity dest_identity = Identity::recall(destination_hash);
	if (dest_identity) {
		message._destination = Destination(dest_identity, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE, "lxmf", "delivery");
	}

	INFO("Message unpacked successfully");
	return message;
}

// Validate the message signature
bool LXMessage::validate_signature() {
	if (_signature_validated) {
		return true;
	}

	INFO("Validating message signature");

	// Try to get source identity if not already available
	if (!_source) {
		Identity source_identity = Identity::recall(_source_hash);
		if (source_identity) {
			_source = Destination(source_identity, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE, "lxmf", "delivery");
		} else {
			_unverified_reason = Type::Message::SOURCE_UNKNOWN;
			WARNING("Cannot validate signature - source identity unknown");
			return false;
		}
	}

	// Reconstruct signed part — must match pack() exactly
	Bytes hashed_part;
	hashed_part << _destination_hash;
	hashed_part << _source_hash;

	// Repack 4-element payload for hash/sig (without stamp, matching Python LXMF)
	MsgPack::Packer packer;
	packer.packArraySize(4);
	packer.pack(_timestamp);
	packer.packBinary(_title.data(), _title.size());
	packer.packBinary(_content.data(), _content.size());
	packer.packMapSize(_fields_count);
	for (size_t i = 0; i < MAX_FIELDS; ++i) {
		if (_fields_pool[i].in_use) {
			packer.packBinary(_fields_pool[i].key.data(), _fields_pool[i].key.size());
			packer.packBinary(_fields_pool[i].value.data(), _fields_pool[i].value.size());
		}
	}
	Bytes packed_payload(packer.data(), packer.size());
	hashed_part << packed_payload;

	Bytes signed_part;
	signed_part << hashed_part;
	signed_part << _hash;

	// Validate signature
	if (_source.identity().validate(_signature, signed_part)) {
		_signature_validated = true;
		INFO("Signature validated successfully");
		return true;
	} else {
		_signature_validated = false;
		_unverified_reason = Type::Message::SIGNATURE_INVALID;
		WARNING("Signature validation failed");
		return false;
	}
}

// Send the message via a link
bool LXMessage::send_via_link(const Link& link) {
	INFO("Sending LXMF message via link");

	// Ensure message is packed
	if (!_packed_valid) {
		pack();
	}

	// Check that link is active
	if (!link || link.status() != RNS::Type::Link::ACTIVE) {
		ERROR("Cannot send message - link is not active");
		return false;
	}

	_state = Type::Message::SENDING;

	try {
		if (_representation == Type::Message::PACKET) {
			// Send as single packet over link
			INFO("  Sending as single packet (" + std::to_string(_packed.size()) + " bytes)");

			Packet packet(link, _packed);
			packet.send();

			_state = Type::Message::SENT;
			INFO("Message sent successfully as packet");
			return true;

		} else if (_representation == Type::Message::RESOURCE) {
			// Send as resource over link
			INFO("  Sending as resource (" + std::to_string(_packed.size()) + " bytes)");

			// TODO: Implement resource transfer with callbacks
			// For now, we'll create the resource but won't set callbacks
			Resource resource(_packed, link);

			_state = Type::Message::SENT;
			INFO("Message resource transfer initiated");
			return true;

		} else {
			ERROR("Unknown message representation");
			_state = Type::Message::FAILED;
			return false;
		}
	} catch (const std::exception& e) {
		ERROR("Failed to send message: " + std::string(e.what()));
		_state = Type::Message::FAILED;
		return false;
	}
}

// String representation
std::string LXMessage::toString() const {
	if (_hash) {
		return "<LXMessage " + _hash.toHex() + ">";
	} else {
		return "<LXMessage [unpacked]>";
	}
}

// Pack the message for PROPAGATED delivery
Bytes LXMessage::pack_propagated() {
	INFO("Packing LXMF message for PROPAGATED delivery");

	// Ensure message is packed
	if (!_packed_valid) {
		pack();
	}

	// Get the destination identity for encryption
	Identity dest_identity;
	if (_destination) {
		dest_identity = _destination.identity();
	} else {
		dest_identity = Identity::recall(_destination_hash);
	}

	if (!dest_identity) {
		ERROR("Cannot pack for propagation - destination identity unknown");
		return {};
	}

	// Encrypt everything after the destination hash
	// Format: packed = dest_hash (16) + source_hash (16) + signature (64) + payload
	// We encrypt: source_hash + signature + payload
	Bytes encrypted;

	// Use cached encrypted data if available (from generate_propagation_stamp)
	// This ensures the stamp matches the encrypted content
	if (_propagation_encrypted.size() > 0) {
		encrypted = _propagation_encrypted;
		DEBUG("  Using cached encrypted data: " + std::to_string(encrypted.size()) + " bytes");
	} else {
		Bytes to_encrypt = _packed.mid(Type::Constants::DESTINATION_LENGTH);
		DEBUG("  To encrypt: " + std::to_string(to_encrypt.size()) + " bytes");

		encrypted = dest_identity.encrypt(to_encrypt);
		if (!encrypted || encrypted.size() == 0) {
			ERROR("Failed to encrypt message for propagation");
			return {};
		}
		DEBUG("  Encrypted: " + std::to_string(encrypted.size()) + " bytes");
	}

	// Build lxmf_data: dest_hash + encrypted
	Bytes lxmf_data;
	lxmf_data << _destination_hash << encrypted;

	// Append propagation stamp if present
	// The stamp is appended to lxmf_data before packing
	if (_propagation_stamp.size() == LXStamper::STAMP_SIZE) {
		lxmf_data << _propagation_stamp;
		DEBUG("  Propagation stamp appended (" + std::to_string(_propagation_stamp.size()) + " bytes)");
	}

	// Pack as msgpack: [timestamp, [lxmf_data]]
	// Format: array(2) [ float64(time), array(1) [ bin(lxmf_data) ] ]
	MsgPack::Packer packer;

	packer.packArraySize(2);

	// Current timestamp
	double current_time = Utilities::OS::time();
	packer.pack(current_time);

	// Inner array with lxmf_data (includes stamp if present)
	packer.packArraySize(1);
	packer.packBinary(lxmf_data.data(), lxmf_data.size());

	Bytes result(packer.data(), packer.size());

	INFO("  Propagation packed size: " + std::to_string(result.size()) + " bytes");

	return result;
}

// Validate the attached stamp against a required cost
bool LXMessage::validate_stamp(uint8_t required_cost) {
	INFO("Validating stamp with required cost " + std::to_string(required_cost));

	if (_stamp.size() != LXStamper::STAMP_SIZE) {
		DEBUG("  No valid stamp attached (size=" + std::to_string(_stamp.size()) + ")");
		_stamp_valid = false;
		return false;
	}

	if (!_hash) {
		// Need message hash - try packing if not done
		if (!_packed_valid) {
			ERROR("  Cannot validate stamp - message not packed and hash not available");
			_stamp_valid = false;
			return false;
		}
	}

	// Generate workblock from message hash
	Bytes workblock = LXStamper::stamp_workblock(_hash, LXStamper::WORKBLOCK_EXPAND_ROUNDS);

	// Validate stamp
	_stamp_valid = LXStamper::stamp_valid(_stamp, required_cost, workblock);

	if (_stamp_valid) {
		uint8_t value = LXStamper::stamp_value(workblock, _stamp);
		INFO("  Stamp valid with value " + std::to_string(value));
	} else {
		DEBUG("  Stamp invalid (does not meet cost " + std::to_string(required_cost) + ")");
	}

	return _stamp_valid;
}

// Generate a stamp for this message
Bytes LXMessage::generate_stamp() {
	if (_stamp_cost == 0) {
		DEBUG("No stamp cost set, skipping stamp generation");
		return {};
	}

	// Ensure message is packed so we have the hash
	if (!_hash) {
		if (!_packed_valid) {
			pack();
		}
	}

	if (!_hash) {
		ERROR("Cannot generate stamp - no message hash available");
		return {};
	}

	INFO("Generating stamp for message " + _hash.toHex() + " with cost " + std::to_string(_stamp_cost));

	auto [stamp, value] = LXStamper::generate_stamp(_hash, _stamp_cost, LXStamper::WORKBLOCK_EXPAND_ROUNDS);

	if (stamp.size() == LXStamper::STAMP_SIZE) {
		_stamp = stamp;
		_stamp_valid = true;
		INFO("Stamp generated with value " + std::to_string(value));

		// Invalidate packed so it gets repacked with stamp
		_packed_valid = false;
	} else {
		ERROR("Stamp generation failed");
	}

	return _stamp;
}

// Generate propagation stamp for this message
Bytes LXMessage::generate_propagation_stamp(uint8_t target_cost) {
	if (target_cost == 0) {
		DEBUG("No propagation stamp cost specified, skipping");
		return {};
	}

	// Ensure message is packed
	if (!_packed_valid) {
		pack();
	}

	// Get the destination identity for encryption
	Identity dest_identity;
	if (_destination) {
		dest_identity = _destination.identity();
	} else {
		dest_identity = Identity::recall(_destination_hash);
	}

	if (!dest_identity) {
		ERROR("Cannot generate propagation stamp - destination identity unknown");
		return {};
	}

	// Build lxmf_data (same as pack_propagated but without stamp)
	Bytes to_encrypt = _packed.mid(Type::Constants::DESTINATION_LENGTH);
	Bytes encrypted = dest_identity.encrypt(to_encrypt);
	if (!encrypted || encrypted.size() == 0) {
		ERROR("Failed to encrypt message for propagation stamp calculation");
		return {};
	}

	// Cache the encrypted data so pack_propagated uses the same ciphertext
	// (encryption produces different output each time due to ephemeral keys)
	_propagation_encrypted = encrypted;

	Bytes lxmf_data;
	lxmf_data << _destination_hash << encrypted;

	// Calculate transient_id = SHA256(lxmf_data)
	Bytes transient_id = Identity::full_hash(lxmf_data);

	INFO("Generating propagation stamp for transient_id " + transient_id.toHex() +
		 " with cost " + std::to_string(target_cost));

	// Generate stamp using PN-specific workblock rounds
	auto [stamp, value] = LXStamper::generate_stamp(
		transient_id, target_cost, LXStamper::WORKBLOCK_EXPAND_ROUNDS_PN);

	if (stamp.size() == LXStamper::STAMP_SIZE) {
		_propagation_stamp = stamp;
		INFO("Propagation stamp generated with value " + std::to_string(value));
	} else {
		ERROR("Propagation stamp generation failed");
	}

	return _propagation_stamp;
}
