// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitNames by LayerTwo Labs (plain-bitnames).

#ifndef BITCOIN_BITNAMES_STATE_H
#define BITCOIN_BITNAMES_STATE_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

/**
 * Names (BitNames, after the sidechain of that name by LayerTwo Labs).
 *
 *  - A **name** is known on the chain by the SHA-256 hash of its text. Whoever
 *    holds the output that carries it owns it, and moves it as coins are moved.
 *  - It is taken in two steps, so that nobody can see a name coming and take it
 *    first: a **reservation** commits to the name under a secret nonce
 *    (HMAC-SHA256 with the nonce as key, of the name hash), and a later
 *    **registration** reveals the nonce, spends the reservation and creates the
 *    name.
 *  - A name holds **data**, every field optional: a commitment to data kept
 *    elsewhere (32 bytes), an IPv4 and an IPv6 address with a port, a key to
 *    encrypt messages to its owner and a key the owner signs messages with
 *    (secp256k1), and the least a payment with a message to it has to pay
 *    ("paymail"). Its owner **updates** them. Every value is kept with the
 *    transaction and height that set it, so the data of a name at any height is
 *    known.
 *  - An authority set per network can mark names as **ICANN** domain names, by
 *    revealing their text and signing it.
 *
 * Names and reservations are carried by outputs of no value, which a
 * **marker** output of the transaction (an OP_RETURN) lists. What each holds
 * follows from the inputs: the names and reservations spent go, in the order
 * of the inputs, to the outputs listed, in the order of the outputs; a name
 * registered, or a reservation made, comes last. Their number cannot change
 * except by a registration (one reservation fewer, one name more) and a
 * reservation (one more): names cannot be destroyed, and a transaction that
 * spends a name without carrying it on is invalid.
 */
namespace bitnames {

using NameHash = uint256;

/** Serializes a std::optional as a flag and, if set, the value. */
struct OptionalFormatter {
    template <typename Stream, typename T>
    void Ser(Stream& s, const std::optional<T>& v)
    {
        s << v.has_value();
        if (v) s << *v;
    }
    template <typename Stream, typename T>
    void Unser(Stream& s, std::optional<T>& v)
    {
        bool has_value;
        s >> has_value;
        v.reset();
        if (has_value) s >> v.emplace();
    }
};
#define BN_OPT(x) Using<OptionalFormatter>(x)

/** The marker of a BitNames transaction: OP_RETURN, then a push of these bytes and what follows. */
inline constexpr unsigned char TAG[4]{0x42, 0x4E, 0x41, 0x4D}; // "BNAM"
inline constexpr uint8_t MARKER_VERSION{0};
/** Markers that set DNS records or publish the text of a name: version 0 with an extension after it. */
inline constexpr uint8_t MARKER_VERSION_EXTENDED{1};
/** Most bytes of the DNS records of a name: a JSON object in the format of Namecoin's domains (see doc/bitnames.md). */
inline constexpr size_t MAX_RECORDS_SIZE{1024};
/** Longest text of a name published with its registration. */
inline constexpr size_t MAX_NAME_TEXT_SIZE{255};

/** Most bytes of the memo of an output (an encrypted message, for paymail). */
inline constexpr size_t MAX_MEMO_SIZE{1024};
/** Most outputs one transaction lists as names or reservations. */
inline constexpr size_t MAX_TOKEN_OUTPUTS{64};
/** Most names one ICANN statement marks. */
inline constexpr size_t MAX_ICANN_NAMES{64};
/** Longest ICANN domain name. */
inline constexpr size_t MAX_ICANN_NAME_SIZE{253};

struct SocketV4 {
    std::array<uint8_t, 4> ip{};
    uint16_t port{0};
    SERIALIZE_METHODS(SocketV4, obj) { READWRITE(obj.ip, obj.port); }
    friend bool operator==(const SocketV4&, const SocketV4&) = default;
};

struct SocketV6 {
    std::array<uint8_t, 16> ip{};
    uint16_t port{0};
    SERIALIZE_METHODS(SocketV6, obj) { READWRITE(obj.ip, obj.port); }
    friend bool operator==(const SocketV6&, const SocketV6&) = default;
};

/** A compressed secp256k1 public key, for encrypting messages to the owner of a name. */
using EncryptionKey = std::array<uint8_t, 33>;
/** An x-only secp256k1 public key (BIP340), that the owner of a name signs messages with. */
using SigningKey = uint256;

/** The data of a name. Every field may be absent. */
struct NameData {
    std::optional<uint256> commitment;
    std::optional<SocketV4> ipv4;
    std::optional<SocketV6> ipv6;
    std::optional<EncryptionKey> encryption_key;
    std::optional<SigningKey> signing_key;
    std::optional<CAmount> paymail_fee;
    //! DNS records (a JSON object); carried in the extension of the marker, not here.
    std::optional<std::string> records;

    SERIALIZE_METHODS(NameData, obj) { READWRITE(BN_OPT(obj.commitment), BN_OPT(obj.ipv4), BN_OPT(obj.ipv6), BN_OPT(obj.encryption_key), BN_OPT(obj.signing_key), BN_OPT(obj.paymail_fee)); }
    friend bool operator==(const NameData&, const NameData&) = default;
};

/** How an update treats one field: delete it, leave it, or set it. */
enum class UpdateKind : uint8_t { DELETE = 0, RETAIN = 1, SET = 2 };

template <typename T>
struct Update {
    UpdateKind kind{UpdateKind::RETAIN};
    //! The value, for SET.
    std::optional<T> value;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << static_cast<uint8_t>(kind);
        if (kind == UpdateKind::SET) s << *value;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t k;
        s >> k;
        if (k > static_cast<uint8_t>(UpdateKind::SET)) throw std::ios_base::failure("unknown update kind");
        kind = static_cast<UpdateKind>(k);
        value.reset();
        if (kind == UpdateKind::SET) s >> value.emplace();
    }
    friend bool operator==(const Update&, const Update&) = default;
};

struct NameUpdates {
    Update<uint256> commitment;
    Update<SocketV4> ipv4;
    Update<SocketV6> ipv6;
    Update<EncryptionKey> encryption_key;
    Update<SigningKey> signing_key;
    Update<CAmount> paymail_fee;
    //! Carried in the extension of the marker.
    Update<std::string> records;

    SERIALIZE_METHODS(NameUpdates, obj) { READWRITE(obj.commitment, obj.ipv4, obj.ipv6, obj.encryption_key, obj.signing_key, obj.paymail_fee); }
    friend bool operator==(const NameUpdates&, const NameUpdates&) = default;
};

//
// What a transaction does, beyond moving names and reservations.
//

struct Reserve {
    static constexpr uint8_t KIND{1};
    //! ReservationCommitment(name, nonce).
    uint256 commitment;
    SERIALIZE_METHODS(Reserve, obj) { READWRITE(obj.commitment); }
    friend bool operator==(const Reserve&, const Reserve&) = default;
};

struct Register {
    static constexpr uint8_t KIND{2};
    NameHash name;
    uint256 nonce;
    NameData data;
    //! The text of the name, if the registration makes it public; carried in the extension of the marker.
    std::optional<std::string> text;
    SERIALIZE_METHODS(Register, obj) { READWRITE(obj.name, obj.nonce, obj.data); }
    friend bool operator==(const Register&, const Register&) = default;
};

/** An update of the data of the name the transaction spends last. */
struct UpdateName {
    static constexpr uint8_t KIND{3};
    NameUpdates updates;
    SERIALIZE_METHODS(UpdateName, obj) { READWRITE(obj.updates); }
    friend bool operator==(const UpdateName&, const UpdateName&) = default;
};

/** The ICANN authority of the network marks names as ICANN domain names. */
struct IcannNames {
    static constexpr uint8_t KIND{4};
    //! The names, in lower case ASCII.
    std::vector<std::string> names;
    //! BIP340 signature by the authority of IcannMessage(names, first input).
    std::vector<unsigned char> signature;
    SERIALIZE_METHODS(IcannNames, obj) { READWRITE(obj.names, obj.signature); }
    friend bool operator==(const IcannNames&, const IcannNames&) = default;
};

/**
 * Release names and reservations: those the transaction spends and does not carry on (the ones
 * spent last, in the order of the inputs) are gone. A released name is free to be registered again.
 */
struct Release {
    static constexpr uint8_t KIND{5};
    SERIALIZE_METHODS(Release, obj) {}
    friend bool operator==(const Release&, const Release&) = default;
};

/**
 * A subname of the name the transaction spends last, which the transaction carries on: a name of
 * its own, known by SubnameHash(parent, label), that the transaction gives to its last name output.
 * Only the owner of a name makes subnames of it; once made, a subname is its holder's.
 */
struct RegisterSubname {
    static constexpr uint8_t KIND{6};
    //! A DNS label: lower-case letters, digits and hyphens.
    std::string label;
    NameData data;
    SERIALIZE_METHODS(RegisterSubname, obj) { READWRITE(obj.label, obj.data); }
    friend bool operator==(const RegisterSubname&, const RegisterSubname&) = default;
};

using Operation = std::variant<Reserve, Register, UpdateName, IcannNames, Release, RegisterSubname>;

/** The marker output of a transaction. */
struct Marker {
    std::optional<Operation> operation;
    //! The outputs that carry names, in increasing order.
    std::vector<uint32_t> name_outputs;
    //! The outputs that carry reservations, in increasing order.
    std::vector<uint32_t> reservation_outputs;
    //! Messages that go with outputs, by output: for paymail. Not looked into by the rules.
    std::vector<std::pair<uint32_t, std::vector<unsigned char>>> memos;

    friend bool operator==(const Marker&, const Marker&) = default;
};

CScript MarkerScript(const Marker& marker);
/** Whether the script claims to be a BitNames marker, well formed or not. */
bool IsMarkerScript(const CScript& script);
std::optional<Marker> ParseMarkerScript(const CScript& script);
/** The marker of a transaction: none if it has none; the error if it has a bad one, or several. */
std::optional<Marker> GetMarker(const CTransaction& tx, std::string* error = nullptr);

/** The hash of the text of a name. */
NameHash HashName(const std::string& name);
/** What a reservation commits to: HMAC-SHA256 with the nonce as key, of the name hash. */
uint256 ReservationCommitment(const NameHash& name, const uint256& nonce);
/** What the ICANN authority signs: the names, and the first input of the transaction, so that it cannot be replayed. */
uint256 IcannMessage(const std::vector<std::string>& names, const COutPoint& first_input);
/** Whether a name can be marked as an ICANN domain name: lower-case letters, digits and hyphens, in at most two labels. */
bool IsIcannName(const std::string& name);
/** Whether text is a DNS label: 1 to 63 lower-case letters, digits and hyphens, not at either end. */
bool IsDnsLabel(const std::string& label);
/** The hash a subname is known by: of its parent, and its label. */
NameHash SubnameHash(const NameHash& parent, const std::string& label);

/**
 * The number of a name in the order of registration, shown as two groups of four
 * digits (more for very large numbers), shuffled so that neighbours do not look
 * alike: "1739-0029" for the first. As BitNames shows it.
 */
std::string FormatSeq(uint32_t seq);
std::optional<uint32_t> ParseSeq(const std::string& text);

//
// The state.
//

/** What an output holds, if it carries a name or a reservation. */
struct Token {
    enum class Kind : uint8_t { NAME = 0, RESERVATION = 1 };
    Kind kind{Kind::NAME};
    //! The name; for a reservation, the transaction that made it.
    uint256 id;
    //! For a reservation: its commitment.
    uint256 commitment;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << static_cast<uint8_t>(kind) << id << commitment;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t k;
        s >> k >> id >> commitment;
        if (k > static_cast<uint8_t>(Kind::RESERVATION)) throw std::ios_base::failure("unknown token kind");
        kind = static_cast<Kind>(k);
    }
    friend bool operator==(const Token&, const Token&) = default;
};

/** A value of a field of a name, with where it was set. */
template <typename T>
struct Stamped {
    std::optional<T> value;
    Txid txid;
    int32_t height{0};
    SERIALIZE_METHODS(Stamped, obj) { READWRITE(BN_OPT(obj.value), obj.txid, obj.height); }
    friend bool operator==(const Stamped&, const Stamped&) = default;
};

/** A registered name: its data now and at every height since its registration. */
struct NameRecord {
    uint32_t seq{0};
    Txid registration;
    int32_t height{0};
    bool icann{false};
    //! For ICANN names: the text, as the authority revealed it.
    std::string icann_name;
    std::vector<Stamped<uint256>> commitment;
    std::vector<Stamped<SocketV4>> ipv4;
    std::vector<Stamped<SocketV6>> ipv6;
    std::vector<Stamped<EncryptionKey>> encryption_key;
    std::vector<Stamped<SigningKey>> signing_key;
    std::vector<Stamped<CAmount>> paymail_fee;
    std::vector<Stamped<std::string>> records;
    //! The text of the name, if its registration made it public.
    std::string text;
    //! For a subname: its parent, and its label under it.
    std::optional<NameHash> parent;
    std::string label;

    SERIALIZE_METHODS(NameRecord, obj)
    {
        READWRITE(obj.seq, obj.registration, obj.height, obj.icann, obj.icann_name, obj.commitment, obj.ipv4, obj.ipv6,
                  obj.encryption_key, obj.signing_key, obj.paymail_fee, obj.records, obj.text, BN_OPT(obj.parent), obj.label);
    }
    friend bool operator==(const NameRecord&, const NameRecord&) = default;

    /** The data now. */
    NameData Current() const;
    /** The data as it was at the end of the block at `height`; nullopt before the registration. */
    std::optional<NameData> AtHeight(int height) const;
};

namespace detail {
template <typename Stream, typename Key, typename Value>
void WriteChanges(Stream& s, const std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    WriteCompactSize(s, changes.size());
    for (const auto& [key, value] : changes) {
        s << key << value.has_value();
        if (value) s << *value;
    }
}
template <typename Stream, typename Key, typename Value>
void ReadChanges(Stream& s, std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    changes.clear();
    const uint64_t count{ReadCompactSize(s)};
    for (uint64_t i{0}; i < count; ++i) {
        Key key;
        bool has_value;
        s >> key >> has_value;
        std::optional<Value> value;
        if (has_value) s >> value.emplace();
        changes.emplace_back(std::move(key), std::move(value));
    }
}
} // namespace detail

/**
 * The names, on the store of the sidechain state (sidechain/store.h), one entry per key, so that
 * nothing the size of the registry is ever read, copied or written whole. Tables (the first byte of
 * the keys): 0x20 what outputs carry, 0x21 names, 0x22 reservations not revealed yet, 0x23 names by
 * number, 0x24 the order of reservations; single values 0x25 the next number, 0x26 the next order,
 * 0x2a and 0x2b how many names and reservations there are. Indexes, so that no rule reads a table
 * whole: 0x27 names by parent, 0x28 outputs by the name they carry, 0x29 reservations by commitment
 * then order.
 *
 * What a block changes, the store's journal notes: the undo data of the block.
 */
class State
{
public:
    /** Read only. */
    explicit State(const sidechain::StoreView& view) : m_view{&view}, m_overlay{nullptr} {}
    /** Read and write. */
    explicit State(sidechain::StoreOverlay& overlay) : m_view{&overlay}, m_overlay{&overlay} {}

    /**
     * Check a transaction against the state and, if it follows the rules, apply it: record what
     * its outputs carry and what it does to names. A transaction that does not follow the rules
     * changes nothing.
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason);
    /** Whether ApplyTx would accept the transaction now. */
    [[nodiscard]] bool CheckTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason) const;
    /** What the inputs of a transaction carry that this state knows of, in the order of the inputs. */
    std::vector<std::pair<uint32_t, Token>> SpentTokens(const CTransaction& tx) const;

    std::optional<Token> GetToken(const COutPoint& outpoint) const;
    std::optional<NameRecord> GetName(const NameHash& name) const;
    bool HasName(const NameHash& name) const;
    std::optional<uint256> GetReservation(const Txid& id) const;
    std::optional<NameHash> NameOfSeq(uint32_t seq) const;
    uint32_t NextSeq() const;
    uint64_t NameCount() const;
    uint64_t ReservationCount() const;
    /** Every name, by hash, until `fn` returns false. */
    void ForEachName(const std::function<bool(const NameHash&, const NameRecord&)>& fn) const;
    /** Every output that carries something, by outpoint, until `fn` returns false. */
    void ForEachToken(const std::function<bool(const COutPoint&, const Token&)>& fn) const;
    /** The outputs that carry `name`. */
    std::vector<COutPoint> TokensOf(const NameHash& name) const;
    /** The subnames of `parent`, by hash. */
    std::vector<NameHash> ChildrenOf(const NameHash& parent) const;

private:
    struct Plan;
    /** Everything ApplyTx would do, worked out without changing anything. */
    std::optional<Plan> MakePlan(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason) const;
    /** Add the descendants of `roots` to the names a plan releases, and the tokens of `roots` and of them to what it spends. @return roots and descendants */
    std::set<NameHash> ReleaseDescendants(Plan& plan, const std::vector<NameHash>& roots) const;
    std::optional<uint64_t> OrderOf(const Txid& id) const;

    // Writes, with the indexes kept in step.
    sidechain::StoreOverlay& Writable() const;
    void SetToken(const COutPoint& outpoint, const std::optional<Token>& token);
    void SetName(const NameHash& name, const std::optional<NameRecord>& record);
    void SetReservation(const Txid& id, const std::optional<uint256>& commitment);
    void SetOrder(const Txid& id, const std::optional<uint64_t>& order);

    const sidechain::StoreView* m_view;
    sidechain::StoreOverlay* m_overlay;
};

/** Whether the marker of a transaction, if any, is well formed. This depends on the transaction alone. */
[[nodiscard]] bool CheckMarker(const CTransaction& tx, std::string& reject_reason);
/** The outputs a transaction marks as carrying a name or a reservation: they carry no value, and are not dust. */
std::set<uint32_t> TokenOutputs(const CTransaction& tx);

} // namespace bitnames

#endif // BITCOIN_BITNAMES_STATE_H
