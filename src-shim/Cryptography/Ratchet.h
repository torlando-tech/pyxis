#pragma once

#include <Bytes.h>
#include <Type.h>
#include <Log.h>
#include "X25519.h"
#include "HKDF.h"
#include "Fernet.h"
#include "Hashes.h"

#include <memory>
#include <stdint.h>

namespace RNS { namespace Cryptography {

	/**
	 * @brief Represents a single ratchet (X25519 keypair) used for forward secrecy
	 *
	 * Ratchets provide forward secrecy by rotating encryption keys at regular intervals.
	 * Each ratchet consists of an X25519 keypair. The public key is announced, while
	 * the private key is used to derive shared secrets with peer ratchets.
	 */
	class Ratchet {

	public:
		using Ptr = std::shared_ptr<Ratchet>;

		// Constants from Python implementation
		static const uint8_t RATCHET_LENGTH = 32;          // X25519 key length
		static const uint8_t RATCHET_ID_LENGTH = 10;       // Truncated hash length
		static const uint16_t MAX_RATCHETS = 128;          // Maximum ratchets to store
		static const uint32_t DEFAULT_RATCHET_INTERVAL = 1800;  // 30 minutes in seconds

	public:
		/**
		 * @brief Default constructor creates an empty ratchet
		 */
		Ratchet() : _created_at(0.0) {}

		/**
		 * @brief Constructs a ratchet from existing key material
		 * @param private_key The X25519 private key (32 bytes)
		 * @param public_key The X25519 public key (32 bytes)
		 * @param created_at Timestamp when ratchet was created
		 */
		Ratchet(const Bytes& private_key, const Bytes& public_key, double created_at = 0.0);

		/**
		 * @brief Copy constructor
		 */
		Ratchet(const Ratchet& ratchet);

		/**
		 * @brief Assignment operator
		 */
		Ratchet& operator=(const Ratchet& ratchet);

		/**
		 * @brief Destructor
		 */
		~Ratchet() {}

		/**
		 * @brief Validity check
		 */
		inline operator bool() const {
			return _private_key && _public_key;
		}

	public:
		/**
		 * @brief Generates a new ratchet with fresh X25519 keypair
		 * @return A new Ratchet instance
		 */
		static Ratchet generate();

		/**
		 * @brief Derives a ratchet ID from public key bytes
		 *
		 * The ratchet ID is used to identify which ratchet was used to encrypt a packet.
		 * It's computed as the first 10 bytes of SHA-256(public_key).
		 *
		 * @param public_bytes The X25519 public key (32 bytes)
		 * @return Ratchet ID (10 bytes)
		 */
		static Bytes get_ratchet_id(const Bytes& public_bytes);

		/**
		 * @brief Derives a ratchet ID from an X25519PublicKey object
		 * @param public_key The X25519 public key object
		 * @return Ratchet ID (10 bytes)
		 */
		static Bytes get_ratchet_id(X25519PublicKey& public_key);

	public:
		/**
		 * @brief Gets the public key bytes for announcement
		 * @return X25519 public key (32 bytes)
		 */
		Bytes public_bytes() const;

		/**
		 * @brief Gets the private key bytes (use with caution!)
		 * @return X25519 private key (32 bytes)
		 */
		Bytes private_bytes() const;

		/**
		 * @brief Gets the ratchet ID for this ratchet
		 * @return Ratchet ID (10 bytes)
		 */
		Bytes get_id() const;

		/**
		 * @brief Gets the creation timestamp
		 * @return Unix timestamp (seconds since epoch)
		 */
		double created_at() const { return _created_at; }

		/**
		 * @brief Sets the creation timestamp
		 * @param timestamp Unix timestamp
		 */
		void set_created_at(double timestamp) { _created_at = timestamp; }

		/**
		 * @brief Encrypts plaintext using this ratchet and peer's public key
		 *
		 * Process:
		 * 1. Perform X25519 ECDH: shared = ECDH(my_private, peer_public)
		 * 2. Derive encryption key: key = HKDF(shared, ...)
		 * 3. Encrypt with Fernet: ciphertext = Fernet.encrypt(key, plaintext)
		 *
		 * @param plaintext The data to encrypt
		 * @param peer_public_key The peer's X25519 public key (32 bytes)
		 * @return Encrypted ciphertext
		 */
		Bytes encrypt(const Bytes& plaintext, const Bytes& peer_public_key) const;

		/**
		 * @brief Decrypts ciphertext using this ratchet and peer's public key
		 *
		 * Process:
		 * 1. Perform X25519 ECDH: shared = ECDH(my_private, peer_public)
		 * 2. Derive decryption key: key = HKDF(shared, ...)
		 * 3. Decrypt with Fernet: plaintext = Fernet.decrypt(key, ciphertext)
		 *
		 * @param ciphertext The encrypted data
		 * @param peer_public_key The peer's X25519 public key (32 bytes)
		 * @return Decrypted plaintext
		 */
		Bytes decrypt(const Bytes& ciphertext, const Bytes& peer_public_key) const;

		/**
		 * @brief Derives a shared secret with peer's ratchet
		 *
		 * Performs X25519 Elliptic Curve Diffie-Hellman key exchange.
		 *
		 * @param peer_public_key The peer's X25519 public key (32 bytes)
		 * @return Shared secret (32 bytes)
		 */
		Bytes derive_shared_secret(const Bytes& peer_public_key) const;

		/**
		 * @brief Derives an encryption/decryption key from a shared secret
		 *
		 * Uses HKDF (HMAC-based Key Derivation Function) to derive a key suitable
		 * for Fernet encryption from the X25519 shared secret.
		 *
		 * @param shared_secret The result of X25519 ECDH (32 bytes)
		 * @return Derived encryption key (32 bytes for Fernet)
		 */
		Bytes derive_key(const Bytes& shared_secret) const;

	private:
		Bytes _private_key;  // X25519 private key (32 bytes)
		Bytes _public_key;   // X25519 public key (32 bytes)
		double _created_at;  // Unix timestamp
	};

} }
