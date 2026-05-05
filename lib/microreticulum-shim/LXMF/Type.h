#pragma once

#include <Type.h>

#include <stdint.h>

namespace LXMF {

	/**
	 * @brief LXMF (Lightweight Extensible Messaging Format) type definitions
	 *
	 * This namespace contains all constants, enums, and type definitions for
	 * the LXMF messaging layer built on top of Reticulum Network Stack.
	 */
	namespace Type {

		/**
		 * @brief Message states during lifecycle
		 */
		namespace Message {
			enum State : uint8_t {
				GENERATING  = 0x00,  ///< Message is being generated
				OUTBOUND    = 0x01,  ///< Message queued for sending
				SENDING     = 0x02,  ///< Message is being sent
				SENT        = 0x04,  ///< Message sent successfully
				DELIVERED   = 0x08,  ///< Message delivered and confirmed
				REJECTED    = 0xFD,  ///< Message rejected by recipient
				CANCELLED   = 0xFE,  ///< Message sending cancelled
				FAILED      = 0xFF   ///< Message sending failed
			};

			/**
			 * @brief Message representation format
			 */
			enum Representation : uint8_t {
				UNKNOWN  = 0x00,  ///< Unknown representation
				PACKET   = 0x01,  ///< Single packet
				RESOURCE = 0x02   ///< Multi-packet resource
			};

			/**
			 * @brief Message delivery methods
			 */
			enum Method : uint8_t {
				OPPORTUNISTIC = 0x01,  ///< Single packet, fire-and-forget
				DIRECT        = 0x02,  ///< Via established link
				PROPAGATED    = 0x03,  ///< Store-and-forward via propagation nodes
				PAPER         = 0x05   ///< QR code / paper-based transfer
			};

			/**
			 * @brief Reasons why message verification failed
			 */
			enum UnverifiedReason : uint8_t {
				SOURCE_UNKNOWN    = 0x01,  ///< Source identity unknown
				SIGNATURE_INVALID = 0x02   ///< Ed25519 signature invalid
			};
		}

		/**
		 * @brief LXMF protocol constants
		 *
		 * These constants define the structure and size limits of LXMF messages.
		 * They are derived from the underlying Reticulum network stack parameters.
		 */
		namespace Constants {
			// Identity and signature sizes (from RNS)
			static const uint8_t DESTINATION_LENGTH = RNS::Type::Identity::TRUNCATED_HASHLENGTH / 8;  // 16 bytes
			static const uint8_t SIGNATURE_LENGTH = RNS::Type::Identity::SIGLENGTH / 8;              // 64 bytes
			static const uint8_t TICKET_LENGTH = RNS::Type::Identity::TRUNCATED_HASHLENGTH / 8;      // 16 bytes

			// Ticket expiry and renewal (in seconds)
			static const uint32_t TICKET_EXPIRY = 21 * 24 * 60 * 60;   // 3 weeks
			static const uint32_t TICKET_GRACE = 5 * 24 * 60 * 60;     // 5 days grace period
			static const uint32_t TICKET_RENEW = 14 * 24 * 60 * 60;    // Renew when <14 days remain
			static const uint32_t TICKET_INTERVAL = 1 * 24 * 60 * 60;  // Check daily
			static const uint16_t COST_TICKET = 0x100;

			// LXMF message structure overhead
			static const uint8_t TIMESTAMP_SIZE = 8;   // 8 bytes for timestamp
			static const uint8_t STRUCT_OVERHEAD = 8;  // 8 bytes for msgpack structure

			/**
			 * @brief Total LXMF overhead per message: 112 bytes
			 *
			 * Breakdown:
			 * - 16 bytes: destination hash
			 * - 16 bytes: source hash
			 * - 64 bytes: Ed25519 signature
			 * - 8 bytes: timestamp
			 * - 8 bytes: msgpack structure overhead
			 */
			static const uint8_t LXMF_OVERHEAD = 2 * DESTINATION_LENGTH + SIGNATURE_LENGTH + TIMESTAMP_SIZE + STRUCT_OVERHEAD;

			// Maximum Data Units (MDU) for different transport modes
			// With an MTU of 500, encrypted packet MDU is 391 bytes
			static const uint16_t ENCRYPTED_PACKET_MDU = RNS::Type::Packet::ENCRYPTED_MDU + TIMESTAMP_SIZE;  // 391 bytes

			// LoRa-constrained ENCRYPTED_PACKET_MDU (for devices with LoRa interfaces)
			// LoRa wire MTU=255, SINGLE header=19, IFAC=1 → max encrypted payload=235
			// Subtract ephemeral key(32) + IV(16) + HMAC(32) = 155 bytes for ciphertext
			// Max AES blocks: floor(155/16)=9 → max padded plaintext=144 → max plaintext=143
			// Add back destination hash(16) stripped in OPPORTUNISTIC: max packed_size=159
			static const uint16_t LORA_ENCRYPTED_PACKET_MDU = 159;

			/**
			 * @brief Max content in single encrypted packet: 295 bytes
			 *
			 * Calculation: ENCRYPTED_PACKET_MDU - LXMF_OVERHEAD + DESTINATION_LENGTH
			 * We add DESTINATION_LENGTH because we can infer it from the packet header.
			 */
			static const uint16_t ENCRYPTED_PACKET_MAX_CONTENT = ENCRYPTED_PACKET_MDU - LXMF_OVERHEAD + DESTINATION_LENGTH;

			/**
			 * @brief Link packet MDU: 431 bytes
			 *
			 * Links have less overhead per packet than standalone packets.
			 */
			static const uint16_t LINK_PACKET_MDU = RNS::Type::Link::MDU;

			// LoRa-constrained LINK_PACKET_MDU (for devices with LoRa interfaces)
			// LoRa wire MTU=255, SX1262 adds 1 IFAC byte → max at transmit=254
			// Link header=19 bytes (2 flags + 1 context + 16 link_id)
			// Max Token output = 254 - 19 = 235 bytes
			// Token = IV(16) + ciphertext + HMAC(32), max ciphertext = 235 - 48 = 187
			// Max AES blocks: floor(187/16)=11 → max padded ciphertext=176
			// Max plaintext: 175 (PKCS7: 175 mod 16 = 15, pad 1 → 176)
			static const uint16_t LORA_LINK_PACKET_MDU = 175;

			/**
			 * @brief Max content in single link packet: 319 bytes
			 *
			 * Calculation: LINK_PACKET_MDU - LXMF_OVERHEAD
			 * Messages larger than 319 bytes will use Resource transfer.
			 */
			static const uint16_t LINK_PACKET_MAX_CONTENT = LINK_PACKET_MDU - LXMF_OVERHEAD;

			// LoRa-constrained: max content in single link packet = 175 - 112 = 63 bytes
			// Messages with content > 63 bytes sent over links will use Resource transfer
			static const uint16_t LORA_LINK_PACKET_MAX_CONTENT = LORA_LINK_PACKET_MDU - LXMF_OVERHEAD;

			// Plain (unencrypted) packet MDU
			static const uint16_t PLAIN_PACKET_MDU = RNS::Type::Packet::PLAIN_MDU;

			/**
			 * @brief Max content in plain packet: 368 bytes
			 *
			 * For unencrypted messages (rarely used in practice).
			 */
			static const uint16_t PLAIN_PACKET_MAX_CONTENT = PLAIN_PACKET_MDU - LXMF_OVERHEAD + DESTINATION_LENGTH;

			// QR/Paper encoding constants
			static const uint16_t QR_MAX_STORAGE = 2953;  // Max bytes in QR code
			static const uint16_t PAPER_MDU = ((QR_MAX_STORAGE - 6) * 6) / 8;  // "lxm://" = 6 chars

			// Stamp proof-of-work constants
			static const uint8_t STAMP_SIZE = 32;                     // 256-bit stamp
			static const uint16_t STAMP_WORKBLOCK_ROUNDS = 3000;      // Direct message stamps
			static const uint16_t STAMP_WORKBLOCK_ROUNDS_PN = 1000;   // Propagation node stamps
		}

		/**
		 * @brief Encryption type descriptions for user display
		 */
		namespace Encryption {
			static const char* DESCRIPTION_AES = "AES-128";
			static const char* DESCRIPTION_EC = "Curve25519";
			static const char* DESCRIPTION_UNENCRYPTED = "Unencrypted";
		}

		/**
		 * @brief URI schema for QR/paper encoding
		 */
		namespace URI {
			static const char* SCHEMA = "lxm";
			static const char* QR_ERROR_CORRECTION = "ERROR_CORRECT_L";
		}

	}  // namespace Type

}  // namespace LXMF
