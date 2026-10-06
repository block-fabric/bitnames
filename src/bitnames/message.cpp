// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitnames/message.h>

#include <crypto/chacha20poly1305.h>
#include <hash.h>
#include <random.h>

#include <secp256k1.h>
#include <secp256k1_ellswift.h>

#include <algorithm>

namespace bitnames {

namespace {
/** An ElligatorSwift encoding of a public key, with fresh randomness: messages to the same key do not look alike. */
std::optional<EllSwiftPubKey> EncodeEllSwift(const CPubKey& key)
{
    secp256k1_pubkey parsed;
    if (!key.IsFullyValid() || !secp256k1_ec_pubkey_parse(secp256k1_context_static, &parsed, key.data(), key.size())) return std::nullopt;
    std::array<unsigned char, 64> encoded;
    const uint256 rnd{GetRandHash()};
    if (!secp256k1_ellswift_encode(secp256k1_context_static, encoded.data(), &parsed, rnd.data())) return std::nullopt;
    return EllSwiftPubKey{std::as_bytes(std::span{encoded})};
}

/** The key of the cipher, from the secret both ends share. */
std::array<std::byte, AEADChaCha20Poly1305::KEYLEN> CipherKey(const ECDHSecret& secret)
{
    uint256 key;
    CSHA256().Write(UCharCast(secret.data()), secret.size()).Write(UCharCast("BitNames message key"), 20).Finalize(key.data());
    std::array<std::byte, AEADChaCha20Poly1305::KEYLEN> out;
    std::copy(std::as_bytes(std::span{key}).begin(), std::as_bytes(std::span{key}).end(), out.begin());
    return out;
}
} // namespace

std::optional<std::vector<unsigned char>> EncryptMessage(const CPubKey& to, std::span<const unsigned char> text)
{
    const auto theirs{EncodeEllSwift(to)};
    if (!theirs) return std::nullopt;
    CKey ephemeral;
    ephemeral.MakeNewKey(true);
    const uint256 entropy{GetRandHash()};
    const EllSwiftPubKey ours{ephemeral.EllSwiftCreate(std::as_bytes(std::span{entropy}))};
    const ECDHSecret secret{ephemeral.ComputeBIP324ECDHSecret(*theirs, ours, /*initiating=*/true)};

    std::vector<unsigned char> message;
    message.push_back(MESSAGE_VERSION);
    message.insert(message.end(), UCharCast(ours.data()), UCharCast(ours.data()) + ours.size());
    message.insert(message.end(), UCharCast(theirs->data()), UCharCast(theirs->data()) + theirs->size());
    const size_t header{message.size()};
    message.resize(header + text.size() + AEADChaCha20Poly1305::EXPANSION);
    AEADChaCha20Poly1305 aead{CipherKey(secret)};
    // A key is used for one message only: the nonce can be fixed.
    aead.Encrypt(std::as_bytes(text), std::as_bytes(std::span{message}.first(header)), {0, 0},
                 std::as_writable_bytes(std::span{message}.subspan(header)));
    return message;
}

std::optional<CPubKey> MessageRecipient(std::span<const unsigned char> message)
{
    if (message.size() < MESSAGE_OVERHEAD || message[0] != MESSAGE_VERSION) return std::nullopt;
    return EllSwiftPubKey{std::as_bytes(message.subspan(65, 64))}.Decode();
}

std::optional<std::vector<unsigned char>> DecryptMessage(const CKey& key, std::span<const unsigned char> message)
{
    if (message.size() < MESSAGE_OVERHEAD || message[0] != MESSAGE_VERSION) return std::nullopt;
    const EllSwiftPubKey theirs{std::as_bytes(message.subspan(1, 64))};
    const EllSwiftPubKey ours{std::as_bytes(message.subspan(65, 64))};
    if (ours.Decode() != key.GetPubKey()) return std::nullopt;
    const ECDHSecret secret{key.ComputeBIP324ECDHSecret(theirs, ours, /*initiating=*/false)};
    const size_t header{1 + 64 + 64};
    std::vector<unsigned char> text(message.size() - header - AEADChaCha20Poly1305::EXPANSION);
    AEADChaCha20Poly1305 aead{CipherKey(secret)};
    if (!aead.Decrypt(std::as_bytes(message.subspan(header)), std::as_bytes(message.first(header)), {0, 0}, std::as_writable_bytes(std::span{text}))) {
        return std::nullopt;
    }
    return text;
}

uint256 MessageSigHash(std::span<const unsigned char> text)
{
    HashWriter writer{TaggedHash("BitNames/message")};
    writer.write(std::as_bytes(text));
    return writer.GetSHA256();
}

} // namespace bitnames
