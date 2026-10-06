// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITNAMES_MESSAGE_H
#define BITCOIN_BITNAMES_MESSAGE_H

#include <key.h>
#include <pubkey.h>
#include <uint256.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

/**
 * Messages to the owners of names, and signatures by them.
 *
 * A message is encrypted to the encryption key of a name the way BIP324 sets up
 * its encrypted connections: an ElligatorSwift Diffie-Hellman between a key made
 * for the message and the key of the name, then ChaCha20-Poly1305. What is sent:
 *
 *   version (1 byte, 1) | the key made for the message (64 bytes, ElligatorSwift)
 *   | the key of the name (64 bytes, ElligatorSwift) | the text encrypted, with its tag (16 bytes)
 *
 * The owner signs with the signing key of the name (BIP340), over a hash of the
 * text tagged "BitNames/message".
 */
namespace bitnames {

inline constexpr uint8_t MESSAGE_VERSION{1};
inline constexpr size_t MESSAGE_OVERHEAD{1 + 64 + 64 + 16};

/** Encrypt `text` to `to`, a compressed public key. Nullopt if it is no valid key. */
std::optional<std::vector<unsigned char>> EncryptMessage(const CPubKey& to, std::span<const unsigned char> text);
/** The key a message was encrypted to, if it is a message. */
std::optional<CPubKey> MessageRecipient(std::span<const unsigned char> message);
/** Decrypt a message with the key it was encrypted to. Nullopt if it is not one, or was changed. */
std::optional<std::vector<unsigned char>> DecryptMessage(const CKey& key, std::span<const unsigned char> message);

/** What a name signs for a message. */
uint256 MessageSigHash(std::span<const unsigned char> text);

} // namespace bitnames

#endif // BITCOIN_BITNAMES_MESSAGE_H
