#include "Ratchet.h"
#include "Fernet.h"
#include <Utilities/OS.h>

#include <Curve25519.h>
#include <SHA256.h>

#include <stdexcept>
#include <cstring>

using namespace RNS;
using namespace RNS::Cryptography;

// Constructor from existing key material
Ratchet::Ratchet(const Bytes& private_key, const Bytes& public_key, double created_at)
	: _private_key(private_key), _public_key(public_key), _created_at(created_at)
{
	if (private_key.size() != RATCHET_LENGTH) {
		throw std::invalid_argument("Ratchet private key must be exactly 32 bytes");
	}
	if (public_key.size() != RATCHET_LENGTH) {
		throw std::invalid_argument("Ratchet public key must be exactly 32 bytes");
	}
	if (_created_at == 0.0) {
		_created_at = Utilities::OS::time();
	}
}

// Copy constructor
Ratchet::Ratchet(const Ratchet& ratchet)
	: _private_key(ratchet._private_key)
	, _public_key(ratchet._public_key)
	, _created_at(ratchet._created_at)
{
}

// Assignment operator
Ratchet& Ratchet::operator=(const Ratchet& ratchet) {
	if (this != &ratchet) {
		_private_key = ratchet._private_key;
		_public_key = ratchet._public_key;
		_created_at = ratchet._created_at;
	}
	return *this;
}

// Generate a new ratchet with fresh X25519 keypair
Ratchet Ratchet::generate() {
	Bytes private_key;
	Bytes public_key;

	// Generate random X25519 keypair using Curve25519::dh1
	// This matches X25519PrivateKey::generate() pattern
	Curve25519::dh1(public_key.writable(RATCHET_LENGTH), private_key.writable(RATCHET_LENGTH));

	double created_at = Utilities::OS::time();

	TRACE("Ratchet::generate: Generated new ratchet");
	DEBUG("  Private key: " + private_key.toHex());
	DEBUG("  Public key:  " + public_key.toHex());
	DEBUG("  Created at:  " + std::to_string(created_at));

	return Ratchet(private_key, public_key, created_at);
}

// Derive ratchet ID from public key bytes
Bytes Ratchet::get_ratchet_id(const Bytes& public_bytes) {
	if (public_bytes.size() != RATCHET_LENGTH) {
		throw std::invalid_argument("Ratchet public key must be exactly 32 bytes");
	}

	// Ratchet ID is first 10 bytes of SHA-256(public_key)
	// This matches Python implementation
	Bytes hash = sha256(public_bytes);
	Bytes ratchet_id = hash.left(RATCHET_ID_LENGTH);

	DEBUG("Ratchet::get_ratchet_id: " + ratchet_id.toHex() + " from pubkey " + public_bytes.toHex());

	return ratchet_id;
}

// Derive ratchet ID from X25519PublicKey object
Bytes Ratchet::get_ratchet_id(X25519PublicKey& public_key) {
	return get_ratchet_id(public_key.public_bytes());
}

// Get public key bytes for announcement
Bytes Ratchet::public_bytes() const {
	return _public_key;
}

// Get private key bytes (use with caution!)
Bytes Ratchet::private_bytes() const {
	return _private_key;
}

// Get ratchet ID for this ratchet
Bytes Ratchet::get_id() const {
	return get_ratchet_id(_public_key);
}

// Derive shared secret with peer's ratchet
Bytes Ratchet::derive_shared_secret(const Bytes& peer_public_key) const {
	if (!_private_key || !_public_key) {
		throw std::runtime_error("Cannot derive shared secret from empty ratchet");
	}
	if (peer_public_key.size() != RATCHET_LENGTH) {
		throw std::invalid_argument("Peer public key must be exactly 32 bytes");
	}

	// Perform X25519 ECDH: shared = ECDH(my_private, peer_public)
	// This uses Curve25519::eval() like X25519PrivateKey::exchange()
	Bytes shared_secret;
	if (!Curve25519::eval(shared_secret.writable(RATCHET_LENGTH), _private_key.data(), peer_public_key.data())) {
		throw std::runtime_error("Peer ratchet key is invalid");
	}

	DEBUG("Ratchet::derive_shared_secret:");
	DEBUG("  My private:   " + _private_key.toHex());
	DEBUG("  My public:    " + _public_key.toHex());
	DEBUG("  Peer public:  " + peer_public_key.toHex());
	DEBUG("  Shared:       " + shared_secret.toHex());

	return shared_secret;
}

// Derive encryption/decryption key from shared secret
Bytes Ratchet::derive_key(const Bytes& shared_secret) const {
	if (shared_secret.size() != RATCHET_LENGTH) {
		throw std::invalid_argument("Shared secret must be exactly 32 bytes");
	}

	// Use HKDF to derive a Fernet-compatible key from the shared secret
	// Fernet requires a 32-byte key (256 bits)
	// Python implementation uses HKDF with no salt or context for ratchet keys
	Bytes derived_key = hkdf(32, shared_secret);

	DEBUG("Ratchet::derive_key:");
	DEBUG("  Shared secret: " + shared_secret.toHex());
	DEBUG("  Derived key:   " + derived_key.toHex());

	return derived_key;
}

// Encrypt plaintext using this ratchet and peer's public key
Bytes Ratchet::encrypt(const Bytes& plaintext, const Bytes& peer_public_key) const {
	if (!_private_key || !_public_key) {
		throw std::runtime_error("Cannot encrypt with empty ratchet");
	}

	// 1. Perform X25519 ECDH to get shared secret
	Bytes shared_secret = derive_shared_secret(peer_public_key);

	// 2. Derive encryption key using HKDF
	Bytes encryption_key = derive_key(shared_secret);

	// 3. Encrypt with Fernet
	Fernet fernet(encryption_key);
	Bytes ciphertext = fernet.encrypt(plaintext);

	DEBUG("Ratchet::encrypt: Encrypted " + std::to_string(plaintext.size()) +
	      " bytes to " + std::to_string(ciphertext.size()) + " bytes");

	return ciphertext;
}

// Decrypt ciphertext using this ratchet and peer's public key
Bytes Ratchet::decrypt(const Bytes& ciphertext, const Bytes& peer_public_key) const {
	if (!_private_key || !_public_key) {
		throw std::runtime_error("Cannot decrypt with empty ratchet");
	}

	try {
		// 1. Perform X25519 ECDH to get shared secret
		Bytes shared_secret = derive_shared_secret(peer_public_key);

		// 2. Derive decryption key using HKDF
		Bytes decryption_key = derive_key(shared_secret);

		// 3. Decrypt with Fernet
		Fernet fernet(decryption_key);
		Bytes plaintext = fernet.decrypt(ciphertext);

		DEBUG("Ratchet::decrypt: Decrypted " + std::to_string(ciphertext.size()) +
		      " bytes to " + std::to_string(plaintext.size()) + " bytes");

		return plaintext;
	}
	catch (const std::exception& e) {
		ERROR("Ratchet::decrypt failed: " + std::string(e.what()));
		throw;
	}
}
