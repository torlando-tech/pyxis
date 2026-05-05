#pragma once

#include "LXMessage.h"
#include <Bytes.h>

#include <ArduinoJson.h>
#include <string>
#include <vector>

namespace LXMF {

	// Fixed pool sizes to eliminate heap fragmentation
	static constexpr size_t MAX_CONVERSATIONS = 32;
	static constexpr size_t MAX_MESSAGES_PER_CONVERSATION = 256;
	static constexpr size_t MESSAGE_HASH_SIZE = 32;  // SHA256 hash
	static constexpr size_t PEER_HASH_SIZE = 16;     // Truncated hash

	/**
	 * @brief Message persistence and conversation management for LXMF
	 *
	 * Stores LXMF messages on the filesystem organized by conversation (peer).
	 * Maintains an index of conversations and message order for efficient retrieval.
	 *
	 * Storage structure:
	 *   <base_path>/
	 *     conversations.json         - Conversation index
	 *     messages/<hash>.json       - Individual message files
	 *     conversations/<peer_hash>/ - Per-conversation metadata
	 *
	 * Usage:
	 *   MessageStore store("/path/to/storage");
	 *   store.save_message(message);
	 *
	 *   auto conversations = store.get_conversations();
	 *   auto messages = store.get_messages_for_conversation(peer_hash);
	 *   LXMessage msg = store.load_message(message_hash);
	 */
	class MessageStore {

	public:
		/**
		 * @brief Conversation metadata with fixed-size message hash storage
		 */
		struct ConversationInfo {
			// Fixed arrays eliminate ~6KB Bytes metadata overhead per conversation
			// (256 messages × 24 bytes metadata = 6.1KB saved per conversation)
			uint8_t peer_hash[PEER_HASH_SIZE];
			uint8_t message_hashes[MAX_MESSAGES_PER_CONVERSATION][MESSAGE_HASH_SIZE];
			size_t message_count = 0;          // Number of messages in this conversation
			double last_activity = 0.0;        // Timestamp of most recent message
			size_t unread_count = 0;           // Number of unread messages
			uint8_t last_message_hash[MESSAGE_HASH_SIZE];

			// Helper methods for accessing fixed arrays as Bytes
			RNS::Bytes peer_hash_bytes() const { return RNS::Bytes(peer_hash, PEER_HASH_SIZE); }
			RNS::Bytes message_hash_bytes(size_t idx) const {
				if (idx >= message_count) return RNS::Bytes();
				return RNS::Bytes(message_hashes[idx], MESSAGE_HASH_SIZE);
			}
			RNS::Bytes last_message_hash_bytes() const { return RNS::Bytes(last_message_hash, MESSAGE_HASH_SIZE); }

			void set_peer_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), PEER_HASH_SIZE);
				memcpy(peer_hash, b.data(), len);
				if (len < PEER_HASH_SIZE) memset(peer_hash + len, 0, PEER_HASH_SIZE - len);
			}
			void set_last_message_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), MESSAGE_HASH_SIZE);
				memcpy(last_message_hash, b.data(), len);
				if (len < MESSAGE_HASH_SIZE) memset(last_message_hash + len, 0, MESSAGE_HASH_SIZE - len);
			}
			bool peer_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != PEER_HASH_SIZE) return false;
				return memcmp(peer_hash, b.data(), PEER_HASH_SIZE) == 0;
			}

			/**
			 * @brief Add a message hash to this conversation
			 * @param hash Message hash to add
			 * @return True if added, false if already exists or pool full
			 */
			bool add_message_hash(const RNS::Bytes& hash);

			/**
			 * @brief Check if conversation has a specific message
			 * @param hash Message hash to check
			 * @return True if message exists in this conversation
			 */
			bool has_message(const RNS::Bytes& hash) const;

			/**
			 * @brief Remove a message hash from this conversation
			 * @param hash Message hash to remove
			 * @return True if removed, false if not found
			 */
			bool remove_message_hash(const RNS::Bytes& hash);

			/**
			 * @brief Clear all data in this conversation info
			 */
			void clear();
		};

		/**
		 * @brief Fixed-size slot for conversation storage
		 */
		struct ConversationSlot {
			bool in_use = false;
			uint8_t peer_hash[PEER_HASH_SIZE];
			ConversationInfo info;

			// Helper methods
			RNS::Bytes peer_hash_bytes() const { return RNS::Bytes(peer_hash, PEER_HASH_SIZE); }
			void set_peer_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), PEER_HASH_SIZE);
				memcpy(peer_hash, b.data(), len);
				if (len < PEER_HASH_SIZE) memset(peer_hash + len, 0, PEER_HASH_SIZE - len);
			}
			bool peer_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != PEER_HASH_SIZE) return false;
				return memcmp(peer_hash, b.data(), PEER_HASH_SIZE) == 0;
			}

			/**
			 * @brief Clear this slot and mark as not in use
			 */
			void clear();
		};

		/**
		 * @brief Lightweight message metadata for fast loading
		 *
		 * Contains only fields needed for chat list display, avoiding
		 * expensive msgpack unpacking.
		 */
		struct MessageMetadata {
			RNS::Bytes hash;
			std::string content;
			double timestamp;
			bool incoming;
			int state;  // Type::Message::State as int
			bool valid;  // True if loaded successfully
		};

	public:
		/**
		 * @brief Construct MessageStore
		 *
		 * @param base_path Base directory for message storage
		 */
		MessageStore(const std::string& base_path);

		~MessageStore();

	public:
		/**
		 * @brief Save a message to storage
		 *
		 * Saves the message and updates the conversation index.
		 * Messages are organized by peer (the other party in the conversation).
		 *
		 * @param message Message to save
		 * @return True if saved successfully
		 */
		bool save_message(const LXMessage& message);

		/**
		 * @brief Load a message from storage
		 *
		 * @param message_hash Hash of the message to load
		 * @return LXMessage object (or empty if not found)
		 */
		LXMessage load_message(const RNS::Bytes& message_hash);

		/**
		 * @brief Load only message metadata (fast path for chat list)
		 *
		 * Reads content/timestamp/state directly from JSON without msgpack unpacking.
		 * Much faster than load_message() for displaying message lists.
		 *
		 * @param message_hash Hash of the message to load
		 * @return MessageMetadata struct (check .valid field)
		 */
		MessageMetadata load_message_metadata(const RNS::Bytes& message_hash);

		/**
		 * @brief Update message state in storage
		 *
		 * Updates just the state field of a stored message.
		 *
		 * @param message_hash Hash of the message to update
		 * @param state New state value
		 * @return True if updated successfully
		 */
		bool update_message_state(const RNS::Bytes& message_hash, Type::Message::State state);

		/**
		 * @brief Delete a message from storage
		 *
		 * Removes the message file and updates the conversation index.
		 *
		 * @param message_hash Hash of the message to delete
		 * @return True if deleted successfully
		 */
		bool delete_message(const RNS::Bytes& message_hash);

		/**
		 * @brief Get list of all conversation peer hashes
		 *
		 * Returns peer hashes sorted by last activity (most recent first).
		 *
		 * @return Vector of peer hashes
		 */
		std::vector<RNS::Bytes> get_conversations();

		/**
		 * @brief Get conversation info for a peer
		 *
		 * @param peer_hash Hash of the peer
		 * @return ConversationInfo (or empty if not found)
		 */
		ConversationInfo get_conversation_info(const RNS::Bytes& peer_hash);

		/**
		 * @brief Get all message hashes for a conversation
		 *
		 * Returns messages in chronological order (oldest first).
		 *
		 * @param peer_hash Hash of the peer
		 * @return Vector of message hashes
		 */
		std::vector<RNS::Bytes> get_messages_for_conversation(const RNS::Bytes& peer_hash);

		/**
		 * @brief Mark all messages in conversation as read
		 *
		 * @param peer_hash Hash of the peer
		 */
		void mark_conversation_read(const RNS::Bytes& peer_hash);

		/**
		 * @brief Delete entire conversation
		 *
		 * Removes all messages and conversation metadata.
		 *
		 * @param peer_hash Hash of the peer
		 * @return True if deleted successfully
		 */
		bool delete_conversation(const RNS::Bytes& peer_hash);

		/**
		 * @brief Get total number of stored messages
		 *
		 * @return Message count
		 */
		size_t get_message_count() const;

		/**
		 * @brief Get total number of conversations
		 *
		 * @return Conversation count
		 */
		size_t get_conversation_count() const;

		/**
		 * @brief Get total unread message count across all conversations
		 *
		 * @return Unread message count
		 */
		size_t get_unread_count() const;

		/**
		 * @brief Clear all stored messages and conversations
		 *
		 * WARNING: This permanently deletes all data.
		 *
		 * @return True if cleared successfully
		 */
		bool clear_all();

	private:
		/**
		 * @brief Initialize storage directories
		 *
		 * Creates base_path, messages/, and conversations/ directories if needed.
		 *
		 * @return True if initialized successfully
		 */
		bool initialize_storage();

		/**
		 * @brief Load conversation index from disk
		 *
		 * Loads conversations.json into _conversations_pool.
		 */
		void load_index();

		/**
		 * @brief Save conversation index to disk
		 *
		 * Persists _conversations_pool to conversations.json.
		 *
		 * @return True if saved successfully
		 */
		bool save_index();

		/**
		 * @brief Get filesystem path for a message file
		 *
		 * @param message_hash Hash of the message
		 * @return Full path to message JSON file
		 */
		std::string get_message_path(const RNS::Bytes& message_hash) const;

		/**
		 * @brief Get filesystem path for conversation directory
		 *
		 * @param peer_hash Hash of the peer
		 * @return Full path to conversation directory
		 */
		std::string get_conversation_path(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Determine peer hash from message
		 *
		 * For incoming messages: peer = source
		 * For outgoing messages: peer = destination
		 *
		 * @param message The message
		 * @param our_hash Our local identity hash
		 * @return Peer hash
		 */
		RNS::Bytes get_peer_hash(const LXMessage& message, const RNS::Bytes& our_hash) const;

		/**
		 * @brief Find a conversation slot by peer hash
		 *
		 * @param peer_hash Hash of the peer
		 * @return Pointer to ConversationSlot or nullptr if not found
		 */
		ConversationSlot* find_conversation(const RNS::Bytes& peer_hash);
		const ConversationSlot* find_conversation(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Get or create a conversation slot for a peer
		 *
		 * @param peer_hash Hash of the peer
		 * @return Pointer to ConversationSlot or nullptr if pool is full
		 */
		ConversationSlot* get_or_create_conversation(const RNS::Bytes& peer_hash);

		/**
		 * @brief Count number of active conversations in pool
		 *
		 * @return Number of in-use conversation slots
		 */
		size_t count_conversations() const;

	private:
		std::string _base_path;
		ConversationSlot _conversations_pool[MAX_CONVERSATIONS];
		bool _initialized;

		// Reusable JSON document to reduce heap fragmentation
		// Note: This class is assumed to be used from a single thread (main loop).
		// If called from multiple threads, this would need per-thread documents or locking.
		JsonDocument _json_doc;
	};

}  // namespace LXMF
