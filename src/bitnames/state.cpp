// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitNames by LayerTwo Labs (plain-bitnames).

#include <bitnames/state.h>

#include <crypto/hmac_sha256.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <pubkey.h>
#include <script/solver.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <univalue.h>

#include <algorithm>
#include <map>

namespace bitnames {

namespace {
/** Kind byte of each operation in a marker; 0 is none. */
template <typename T>
void WriteOperation(DataStream& s, const T& op)
{
    s << T::KIND << op;
}

template <typename T>
bool ReadOperation(DataStream& s, std::optional<Operation>& out)
{
    T op;
    s >> op;
    out = std::move(op);
    return true;
}

/** Whether a list of output indexes is strictly increasing and names outputs of the transaction. */
bool GoodIndexes(const std::vector<uint32_t>& indexes, size_t outputs)
{
    for (size_t i{0}; i < indexes.size(); ++i) {
        if (indexes[i] >= outputs) return false;
        if (i > 0 && indexes[i] <= indexes[i - 1]) return false;
    }
    return true;
}

/** DNS records: a JSON object, no larger than MAX_RECORDS_SIZE. What is in it is for resolvers to read. */
bool GoodRecords(const std::string& records)
{
    if (records.size() > MAX_RECORDS_SIZE) return false;
    UniValue value;
    return value.read(records) && value.isObject();
}

bool GoodData(const NameData& data)
{
    if (data.records && !GoodRecords(*data.records)) return false;
    if (data.paymail_fee && !MoneyRange(*data.paymail_fee)) return false;
    if (data.encryption_key && !CPubKey{std::span{data.encryption_key->data(), data.encryption_key->size()}}.IsFullyValid()) return false;
    if (data.signing_key && !XOnlyPubKey{*data.signing_key}.IsFullyValid()) return false;
    return true;
}

bool GoodUpdates(const NameUpdates& updates)
{
    NameData set;
    if (updates.paymail_fee.kind == UpdateKind::SET) set.paymail_fee = updates.paymail_fee.value;
    if (updates.encryption_key.kind == UpdateKind::SET) set.encryption_key = updates.encryption_key.value;
    if (updates.signing_key.kind == UpdateKind::SET) set.signing_key = updates.signing_key.value;
    if (updates.records.kind == UpdateKind::SET) set.records = updates.records.value;
    return GoodData(set);
}

/** A value of a field, to add to the history of a name. */
struct HistoryEntry {
    uint32_t seq;
    Field field;
    int height;
    sidechain::StoreBytes stamped;
};

/** The history of the data a name is registered with: every field, set or not. */
void StampAll(std::vector<HistoryEntry>& out, uint32_t seq, const NameData& data, const Txid& txid, int height)
{
    ForEachField(data, [&](Field field, const auto& value) {
        using T = typename std::remove_cvref_t<decltype(value)>::value_type;
        out.push_back({seq, field, height, sidechain::EncodeValue(Stamped<T>{value, txid, height})});
    });
}

template <typename T>
void ApplyUpdate(std::optional<T>& current, const Update<T>& update, Field field, uint32_t seq, const Txid& txid, int height, std::vector<HistoryEntry>& out)
{
    if (update.kind == UpdateKind::RETAIN) return;
    current = update.kind == UpdateKind::SET ? update.value : std::nullopt;
    out.push_back({seq, field, height, sidechain::EncodeValue(Stamped<T>{current, txid, height})});
}

/** Whether a standard output that someone can spend: what a name or reservation may be carried by. */
bool Spendable(const CScript& script)
{
    std::vector<std::vector<unsigned char>> solutions;
    switch (Solver(script, solutions)) {
    case TxoutType::PUBKEYHASH:
    case TxoutType::SCRIPTHASH:
    case TxoutType::WITNESS_V0_KEYHASH:
    case TxoutType::WITNESS_V0_SCRIPTHASH:
    case TxoutType::WITNESS_V1_TAPROOT:
        return true;
    default:
        return false;
    }
}
} // namespace

namespace {
/** Whether a marker needs the extension: it sets DNS records, or publishes the text of a name. */
bool Extended(const Marker& marker)
{
    if (!marker.operation) return false;
    if (const auto* reg{std::get_if<Register>(&*marker.operation)}) return reg->data.records || reg->text;
    if (const auto* update{std::get_if<UpdateName>(&*marker.operation)}) return update->updates.records.kind != UpdateKind::RETAIN;
    if (const auto* sub{std::get_if<RegisterSubname>(&*marker.operation)}) return sub->data.records.has_value();
    return false;
}
} // namespace

CScript MarkerScript(const Marker& marker)
{
    DataStream s{};
    s.write(std::as_bytes(std::span{TAG}));
    const bool extended{Extended(marker)};
    s << (extended ? MARKER_VERSION_EXTENDED : MARKER_VERSION);
    if (!marker.operation) {
        s << uint8_t{0};
    } else {
        std::visit([&](const auto& op) { WriteOperation(s, op); }, *marker.operation);
    }
    s << marker.name_outputs << marker.reservation_outputs << marker.memos;
    if (extended) {
        if (const auto* reg{std::get_if<Register>(&*marker.operation)}) {
            s << BN_OPT(reg->data.records) << BN_OPT(reg->text);
        } else if (const auto* update{std::get_if<UpdateName>(&*marker.operation)}) {
            s << update->updates.records;
        } else if (const auto* sub{std::get_if<RegisterSubname>(&*marker.operation)}) {
            s << BN_OPT(sub->data.records);
        }
    }
    return CScript() << OP_RETURN << std::vector<unsigned char>{UCharCast(s.data()), UCharCast(s.data()) + s.size()};
}

namespace {
std::optional<std::vector<unsigned char>> MarkerData(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < sizeof(TAG) || !std::equal(std::begin(TAG), std::end(TAG), data.begin())) return std::nullopt;
    return data;
}
} // namespace

bool IsMarkerScript(const CScript& script)
{
    // OP_RETURN, then a push that starts with the tag: that much makes it a marker, good or bad.
    if (script.size() < 2 || script[0] != OP_RETURN) return false;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4) return false;
    return data.size() >= sizeof(TAG) && std::equal(std::begin(TAG), std::end(TAG), data.begin());
}

std::optional<Marker> ParseMarkerScript(const CScript& script)
{
    const auto data{MarkerData(script)};
    if (!data) return std::nullopt;
    try {
        DataStream s{std::span{*data}.subspan(sizeof(TAG))};
        uint8_t version, kind;
        s >> version >> kind;
        if (version != MARKER_VERSION && version != MARKER_VERSION_EXTENDED) return std::nullopt;
        Marker marker;
        switch (kind) {
        case 0: break;
        case Reserve::KIND: ReadOperation<Reserve>(s, marker.operation); break;
        case Register::KIND: ReadOperation<Register>(s, marker.operation); break;
        case UpdateName::KIND: ReadOperation<UpdateName>(s, marker.operation); break;
        case IcannNames::KIND: ReadOperation<IcannNames>(s, marker.operation); break;
        case Release::KIND: ReadOperation<Release>(s, marker.operation); break;
        case RegisterSubname::KIND: ReadOperation<RegisterSubname>(s, marker.operation); break;
        default: return std::nullopt;
        }
        s >> marker.name_outputs >> marker.reservation_outputs >> marker.memos;
        if (version == MARKER_VERSION_EXTENDED) {
            if (!marker.operation) return std::nullopt;
            if (auto* reg{std::get_if<Register>(&*marker.operation)}) {
                s >> BN_OPT(reg->data.records) >> BN_OPT(reg->text);
            } else if (auto* update{std::get_if<UpdateName>(&*marker.operation)}) {
                s >> update->updates.records;
            } else if (auto* sub{std::get_if<RegisterSubname>(&*marker.operation)}) {
                s >> BN_OPT(sub->data.records);
            } else {
                return std::nullopt;
            }
        }
        if (!s.empty()) return std::nullopt;
        // Written back the same: there is one way to write a marker.
        if (MarkerScript(marker) != script) return std::nullopt;
        return marker;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Marker> GetMarker(const CTransaction& tx, std::string* error)
{
    std::optional<Marker> found;
    bool seen{false};
    for (const CTxOut& out : tx.vout) {
        if (!IsMarkerScript(out.scriptPubKey)) continue;
        if (seen) {
            if (error) *error = "bad-bn-markers";
            return std::nullopt;
        }
        seen = true;
        found = ParseMarkerScript(out.scriptPubKey);
        if (!found) {
            if (error) *error = "bad-bn-marker";
            return std::nullopt;
        }
    }
    if (error) error->clear();
    return found;
}

NameHash HashName(const std::string& name)
{
    uint256 hash;
    CSHA256().Write(UCharCast(name.data()), name.size()).Finalize(hash.data());
    return hash;
}

uint256 ReservationCommitment(const NameHash& name, const uint256& nonce, const CScript& script)
{
    uint256 commitment;
    CHMAC_SHA256(nonce.data(), nonce.size()).Write(name.data(), name.size()).Write(script.data(), script.size()).Finalize(commitment.data());
    return commitment;
}

uint256 IcannMessage(const std::vector<std::string>& names, const COutPoint& first_input)
{
    HashWriter writer{};
    writer << std::string{"BitNames ICANN"} << names << first_input;
    return writer.GetSHA256();
}

bool IsIcannName(const std::string& name)
{
    if (name.empty() || name.size() > MAX_ICANN_NAME_SIZE) return false;
    if (std::count(name.begin(), name.end(), '.') > 1) return false;
    size_t start{0};
    while (true) {
        const size_t end{std::min(name.find('.', start), name.size())};
        const std::string label{name.substr(start, end - start)};
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (const char c : label) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        }
        if (end == name.size()) return true;
        start = end + 1;
    }
}

bool IsDnsLabel(const std::string& label)
{
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
    return std::all_of(label.begin(), label.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

NameHash SubnameHash(const NameHash& parent, const std::string& label)
{
    HashWriter writer{};
    writer << std::string{"BitNames subname"} << parent << label;
    return writer.GetSHA256();
}

namespace {
constexpr uint32_t SEQ_DISPLAY_OFFSET{23071990};
constexpr uint32_t SEQ_LOW{100000000};
} // namespace

std::string FormatSeq(uint32_t seq)
{
    const uint32_t hi{seq / SEQ_LOW};
    const uint32_t lo{(seq % SEQ_LOW + SEQ_DISPLAY_OFFSET) % SEQ_LOW};
    const std::string d{strprintf("%08u", lo)};
    const std::string first{d[4], d[3], d[1], d[6]};
    const std::string second{d[2], d[7], d[0], d[5]};
    if (hi > 0) return strprintf("%04u-%s-%s", hi, first, second);
    return first + "-" + second;
}

std::optional<uint32_t> ParseSeq(const std::string& text)
{
    std::vector<std::string> groups;
    size_t start{0};
    while (true) {
        const size_t end{std::min(text.find('-', start), text.size())};
        groups.push_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
    if (groups.size() != 2 && groups.size() != 3) return std::nullopt;
    for (const std::string& group : groups) {
        if (group.size() != 4 || !std::all_of(group.begin(), group.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    }
    uint64_t hi{0};
    if (groups.size() == 3) hi = std::stoul(groups[0]);
    const std::string& first{groups[groups.size() - 2]};
    const std::string& second{groups[groups.size() - 1]};
    std::string d(8, '0');
    d[4] = first[0];
    d[3] = first[1];
    d[1] = first[2];
    d[6] = first[3];
    d[2] = second[0];
    d[7] = second[1];
    d[0] = second[2];
    d[5] = second[3];
    const uint64_t lo{std::stoul(d)};
    const uint64_t low{(lo + SEQ_LOW - SEQ_DISPLAY_OFFSET) % SEQ_LOW};
    const uint64_t seq{hi * SEQ_LOW + low};
    if (seq > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    // Three groups only for numbers that need them.
    if (groups.size() == 3 && hi == 0) return std::nullopt;
    return static_cast<uint32_t>(seq);
}

bool CheckMarker(const CTransaction& tx, std::string& reject_reason)
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    std::string error;
    const auto marker{GetMarker(tx, &error)};
    if (!error.empty()) return invalid(error.c_str());
    if (!marker) return true;
    if (tx.IsCoinBase()) return invalid("bad-bn-coinbase");
    size_t marker_index{0};
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (IsMarkerScript(tx.vout[i].scriptPubKey)) marker_index = i;
    }
    if (tx.vout[marker_index].nValue != 0) return invalid("bad-bn-marker-value");
    if (!GoodIndexes(marker->name_outputs, tx.vout.size()) || !GoodIndexes(marker->reservation_outputs, tx.vout.size())) return invalid("bad-bn-outputs");
    if (marker->name_outputs.size() + marker->reservation_outputs.size() > MAX_TOKEN_OUTPUTS) return invalid("bad-bn-outputs");
    std::set<uint32_t> tokens;
    for (const uint32_t n : marker->name_outputs) tokens.insert(n);
    for (const uint32_t n : marker->reservation_outputs) {
        if (!tokens.insert(n).second) return invalid("bad-bn-outputs");
    }
    // A name or a reservation is carried by an output of no value that someone can spend.
    for (const uint32_t n : tokens) {
        if (n == marker_index) return invalid("bad-bn-outputs");
        if (tx.vout[n].nValue != 0 || !Spendable(tx.vout[n].scriptPubKey)) return invalid("bad-bn-token-output");
    }
    for (size_t i{0}; i < marker->memos.size(); ++i) {
        const auto& [n, memo] = marker->memos[i];
        if (n >= tx.vout.size() || n == marker_index || (i > 0 && n <= marker->memos[i - 1].first)) return invalid("bad-bn-memo");
        if (memo.empty() || memo.size() > MAX_MEMO_SIZE) return invalid("bad-bn-memo");
    }
    if (!marker->operation) return true;
    if (const auto* reg{std::get_if<Register>(&*marker->operation)}) {
        if (!GoodData(reg->data)) return invalid("bad-bn-data");
        if (reg->text && (reg->text->empty() || reg->text->size() > MAX_NAME_TEXT_SIZE || HashName(*reg->text) != reg->name)) return invalid("bad-bn-name-text");
    } else if (const auto* update{std::get_if<UpdateName>(&*marker->operation)}) {
        if (!GoodUpdates(update->updates)) return invalid("bad-bn-data");
    } else if (const auto* sub{std::get_if<RegisterSubname>(&*marker->operation)}) {
        if (!IsDnsLabel(sub->label)) return invalid("bad-bn-label");
        if (!GoodData(sub->data)) return invalid("bad-bn-data");
    } else if (const auto* icann{std::get_if<IcannNames>(&*marker->operation)}) {
        if (icann->names.empty() || icann->names.size() > MAX_ICANN_NAMES) return invalid("bad-bn-icann-names");
        std::set<std::string> distinct;
        for (const std::string& name : icann->names) {
            if (!IsIcannName(name) || !distinct.insert(name).second) return invalid("bad-bn-icann-names");
        }
        if (icann->signature.size() != 64) return invalid("bad-bn-icann-signature");
    }
    return true;
}

std::set<uint32_t> TokenOutputs(const CTransaction& tx)
{
    std::set<uint32_t> outputs;
    std::string error;
    const auto marker{GetMarker(tx, &error)};
    if (!marker) return outputs;
    outputs.insert(marker->name_outputs.begin(), marker->name_outputs.end());
    outputs.insert(marker->reservation_outputs.begin(), marker->reservation_outputs.end());
    return outputs;
}

struct State::Plan {
    std::vector<COutPoint> spent;
    std::vector<std::pair<COutPoint, Token>> created;
    //! New values of names, in order.
    std::vector<std::pair<NameHash, NameRecord>> names;
    std::optional<Txid> reservation_made;
    uint256 reservation_commitment;
    //! The script of the output of the reservation made, which its commitment binds.
    std::optional<CScript> reservation_origin;
    std::optional<Txid> reservation_revealed;
    std::optional<std::pair<uint32_t, NameHash>> seq;
    //! Names and reservations released.
    std::vector<NameHash> names_released;
    //! Names whose record a new one replaces (a top-level name squatting a subname): their numbers go.
    std::vector<NameHash> names_replaced;
    std::vector<Txid> reservations_released;
    //! Values to add to the history of names.
    std::vector<HistoryEntry> history;
    //! Policy: the most names the plan may release in all (see MAX_RELAYED_RELEASE).
    std::optional<uint32_t> max_released;
    //! Whether it releases more.
    bool too_many_released{false};

    /** Whether the plan releases more names than it may: `more` beyond those it lists. */
    bool OverLimit(size_t more = 0)
    {
        if (max_released && names_released.size() + names_replaced.size() + more > *max_released) too_many_released = true;
        return too_many_released;
    }
};

namespace {
const sidechain::Table<COutPoint, Token> TOKENS{0x20};
const sidechain::Table<uint256, NameRecord> NAMES{0x21};
const sidechain::Table<uint256, ReservationRecord> RESERVATIONS{0x22};
const sidechain::Table<uint32_t, uint256> SEQ{0x23};
const sidechain::Cell<uint32_t> NEXT_SEQ{0x25, 0};
const sidechain::TableId CHILDREN{0x27};
const sidechain::TableId NAME_TOKENS{0x28};
const sidechain::Cell<uint64_t> NAME_COUNT{0x2a, 0};
const sidechain::Cell<uint64_t> RESERVATION_COUNT{0x2b, 0};
const sidechain::TableId HISTORY{0x2c};
const sidechain::Table<uint256, CScript> ORIGINS{0x2d};

sidechain::StoreBytes ChildKey(const uint256& parent, const uint256& child)
{
    sidechain::StoreBytes key{CHILDREN};
    sidechain::KeyCodec<uint256>::Encode(key, parent);
    sidechain::KeyCodec<uint256>::Encode(key, child);
    return key;
}
sidechain::StoreBytes NameTokenKey(const uint256& name, const COutPoint& outpoint)
{
    sidechain::StoreBytes key{NAME_TOKENS};
    sidechain::KeyCodec<uint256>::Encode(key, name);
    sidechain::KeyCodec<COutPoint>::Encode(key, outpoint);
    return key;
}
/**
 * The key of the history of a name (by its number), of one of its fields, at a height: the latest
 * first (the height, then the place among the values of that height, stored complemented).
 */
sidechain::StoreBytes HistoryKey(uint32_t seq, std::optional<Field> field = std::nullopt, std::optional<int> height = std::nullopt)
{
    sidechain::StoreBytes key{HISTORY};
    sidechain::KeyCodec<uint32_t>::Encode(key, seq);
    if (field) {
        key.push_back(static_cast<uint8_t>(*field));
        if (height) sidechain::KeyCodec<uint32_t>::Encode(key, ~static_cast<uint32_t>(*height));
    }
    return key;
}
/** The entries under a prefix, as the keys that follow it. */
std::vector<sidechain::StoreBytes> Under(const sidechain::StoreView& view, const sidechain::StoreBytes& prefix)
{
    std::vector<sidechain::StoreBytes> rests;
    view.ForEach(prefix, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        rests.emplace_back(key.begin() + prefix.size(), key.end());
        return true;
    });
    return rests;
}
} // namespace

sidechain::StoreOverlay& State::Writable() const { return *Assert(m_overlay); }
std::optional<Token> State::GetToken(const COutPoint& outpoint) const { return TOKENS.Get(*m_view, outpoint); }
std::optional<NameRecord> State::GetName(const NameHash& name) const { return NAMES.Get(*m_view, name); }
bool State::HasName(const NameHash& name) const { return NAMES.Contains(*m_view, name); }
std::optional<ReservationRecord> State::GetReservationRecord(const Txid& id) const { return RESERVATIONS.Get(*m_view, id.ToUint256()); }
std::optional<uint256> State::GetReservation(const Txid& id) const
{
    if (const auto record{GetReservationRecord(id)}) return record->commitment;
    return std::nullopt;
}
std::optional<int> State::ReservationHeight(const Txid& id) const
{
    if (const auto record{GetReservationRecord(id)}) return record->height;
    return std::nullopt;
}

std::optional<CScript> State::GetReservationOrigin(const Txid& id) const { return ORIGINS.Get(*m_view, id.ToUint256()); }

std::optional<NameData> State::DataAtHeight(const NameRecord& record, int at) const
{
    if (at < record.height) return std::nullopt;
    NameData data;
    // Per field, the first entry from the height on down is the latest set by then.
    ForEachField(data, [&](Field field, auto& value) {
        using T = typename std::remove_cvref_t<decltype(value)>::value_type;
        if (const auto entry{m_view->Next(HistoryKey(record.seq, field, at), HistoryKey(record.seq, field))}) {
            value = sidechain::DecodeValue<Stamped<T>>(entry->second).value;
        }
    });
    return data;
}

NameHistory State::History(const NameRecord& record) const
{
    NameHistory history;
    ForEachField(history, [&](Field field, auto& entries) {
        using S = typename std::remove_cvref_t<decltype(entries)>::value_type;
        m_view->ForEach(HistoryKey(record.seq, field), [&](const sidechain::StoreBytes&, const sidechain::StoreBytes& value) {
            entries.push_back(sidechain::DecodeValue<S>(value));
            return true;
        });
        std::reverse(entries.begin(), entries.end());
    });
    return history;
}

void State::AddHistory(uint32_t seq, Field field, int height, const sidechain::StoreBytes& stamped)
{
    // After the values the name already has at this height, if any: the first of them is the latest.
    const sidechain::StoreBytes at{HistoryKey(seq, field, height)};
    uint32_t n{0};
    if (const auto latest{m_view->Next(at, at)}) {
        std::span<const unsigned char> in{latest->first};
        in = in.subspan(at.size());
        n = ~sidechain::KeyCodec<uint32_t>::Decode(in) + 1;
    }
    sidechain::StoreBytes key{at};
    sidechain::KeyCodec<uint32_t>::Encode(key, ~n);
    Writable().Put(key, stamped);
}

void State::EraseHistory(uint32_t seq)
{
    const sidechain::StoreBytes prefix{HistoryKey(seq)};
    std::vector<sidechain::StoreBytes> keys;
    m_view->ForEach(prefix, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        keys.push_back(key);
        return true;
    });
    for (const auto& key : keys) Writable().Erase(key);
}
std::optional<NameHash> State::NameOfSeq(uint32_t seq) const { return SEQ.Get(*m_view, seq); }
uint32_t State::NextSeq() const { return NEXT_SEQ.Get(*m_view); }
void State::ForEachSeq(uint32_t from, const std::function<bool(uint32_t, const NameHash&)>& fn) const { SEQ.ForEach(*m_view, fn, from); }
uint64_t State::NameCount() const { return NAME_COUNT.Get(*m_view); }
uint64_t State::ReservationCount() const { return RESERVATION_COUNT.Get(*m_view); }

void State::ForEachName(const std::function<bool(const NameHash&, const NameRecord&)>& fn) const { NAMES.ForEach(*m_view, fn); }
void State::ForEachToken(const std::function<bool(const COutPoint&, const Token&)>& fn) const { TOKENS.ForEach(*m_view, fn); }

std::vector<NameHash> State::ChildrenOf(const NameHash& parent) const
{
    sidechain::StoreBytes prefix{CHILDREN};
    sidechain::KeyCodec<uint256>::Encode(prefix, parent);
    std::vector<NameHash> children;
    for (const auto& rest : Under(*m_view, prefix)) {
        std::span<const unsigned char> in{rest};
        children.push_back(sidechain::KeyCodec<uint256>::Decode(in));
    }
    return children;
}

std::vector<COutPoint> State::TokensOf(const NameHash& name) const
{
    sidechain::StoreBytes prefix{NAME_TOKENS};
    sidechain::KeyCodec<uint256>::Encode(prefix, name);
    std::vector<COutPoint> outpoints;
    for (const auto& rest : Under(*m_view, prefix)) {
        std::span<const unsigned char> in{rest};
        outpoints.push_back(sidechain::KeyCodec<COutPoint>::Decode(in));
    }
    return outpoints;
}

void State::SetToken(const COutPoint& outpoint, const std::optional<Token>& token)
{
    sidechain::StoreOverlay& out{Writable()};
    if (const auto old{GetToken(outpoint)}; old && old->kind == Token::Kind::NAME) out.Erase(NameTokenKey(old->id, outpoint));
    if (token) {
        TOKENS.Put(out, outpoint, *token);
        if (token->kind == Token::Kind::NAME) out.Put(NameTokenKey(token->id, outpoint), {});
    } else {
        TOKENS.Erase(out, outpoint);
    }
}

void State::SetName(const NameHash& name, const std::optional<NameRecord>& record)
{
    sidechain::StoreOverlay& out{Writable()};
    const auto old{GetName(name)};
    if (old && old->parent) out.Erase(ChildKey(*old->parent, name));
    if (record) {
        NAMES.Put(out, name, *record);
        if (record->parent) out.Put(ChildKey(*record->parent, name), {});
        if (!old) NAME_COUNT.Put(out, NameCount() + 1);
    } else if (old) {
        NAMES.Erase(out, name);
        NAME_COUNT.Put(out, NameCount() - 1);
    }
}

void State::SetReservation(const Txid& id, const std::optional<ReservationRecord>& reservation)
{
    sidechain::StoreOverlay& out{Writable()};
    const auto old{GetReservationRecord(id)};
    if (reservation) {
        RESERVATIONS.Put(out, id.ToUint256(), *reservation);
        if (!old) RESERVATION_COUNT.Put(out, ReservationCount() + 1);
    } else if (old) {
        RESERVATIONS.Erase(out, id.ToUint256());
        RESERVATION_COUNT.Put(out, ReservationCount() - 1);
    }
}

std::vector<std::pair<uint32_t, Token>> State::SpentTokens(const CTransaction& tx) const
{
    std::vector<std::pair<uint32_t, Token>> spent;
    if (tx.IsCoinBase()) return spent;
    for (uint32_t i{0}; i < tx.vin.size(); ++i) {
        if (const auto token{GetToken(tx.vin[i].prevout)}) spent.emplace_back(i, *token);
    }
    return spent;
}

std::optional<std::set<NameHash>> State::ReleaseDescendants(Plan& plan, const std::vector<NameHash>& roots) const
{
    // Down from the roots, through the index of names by parent, no further than the plan may go.
    std::set<NameHash> gone{roots.begin(), roots.end()};
    std::vector<NameHash> todo{roots};
    if (plan.OverLimit()) return std::nullopt;
    while (!todo.empty()) {
        const NameHash at{todo.back()};
        todo.pop_back();
        sidechain::StoreBytes prefix{CHILDREN};
        sidechain::KeyCodec<uint256>::Encode(prefix, at);
        bool over{false};
        m_view->ForEach(prefix, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
            std::span<const unsigned char> in{key};
            in = in.subspan(prefix.size());
            const NameHash child{sidechain::KeyCodec<uint256>::Decode(in)};
            if (gone.insert(child).second) {
                plan.names_released.push_back(child);
                todo.push_back(child);
                if (plan.OverLimit()) {
                    over = true;
                    return false;
                }
            }
            return true;
        });
        if (over) return std::nullopt;
    }
    // Their holders keep the coins, which no longer carry a name; the roots' tokens too (a released
    // name's own token is spent by the transaction, unless it is a squat that gives way).
    const std::set<COutPoint> spent_here{plan.spent.begin(), plan.spent.end()};
    for (const NameHash& name : gone) {
        for (const COutPoint& outpoint : TokensOf(name)) {
            if (!spent_here.contains(outpoint)) plan.spent.push_back(outpoint);
        }
    }
    return gone;
}

void State::CountOut(Plan& plan, const std::set<NameHash>& gone) const
{
    // Each name that goes from under a name that stays takes itself and its own count off every
    // name above it.
    std::map<NameHash, NameRecord> above;
    for (const NameHash& name : gone) {
        const auto record{GetName(name)};
        if (!record || !record->parent || gone.contains(*record->parent)) continue;
        const uint32_t lost{record->descendants + 1};
        for (std::optional<NameHash> at{record->parent}; at && !gone.contains(*at);) {
            auto it{above.find(*at)};
            if (it == above.end()) {
                auto ancestor{GetName(*at)};
                if (!ancestor) break;
                it = above.emplace(*at, std::move(*ancestor)).first;
            }
            it->second.descendants -= std::min(it->second.descendants, lost);
            at = it->second.parent;
        }
    }
    for (auto& [name, record] : above) plan.names.emplace_back(name, std::move(record));
}

std::optional<State::Plan> State::MakePlan(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason,
                                           std::optional<uint32_t> max_released) const
{
    const auto invalid{[&](const char* reason) -> std::optional<Plan> {
        reject_reason = reason;
        return std::nullopt;
    }};
    const char* const too_large{"bn-release-too-large"};
    if (!CheckMarker(tx, reject_reason)) return std::nullopt;
    std::string error;
    const auto marker{GetMarker(tx, &error)};
    const auto spent{SpentTokens(tx)};
    if (!marker && spent.empty()) return Plan{};

    std::vector<NameHash> names_in;
    std::vector<Token> reservations_in;
    for (const auto& [n, token] : spent) {
        if (token.kind == Token::Kind::NAME) {
            names_in.push_back(token.id);
        } else {
            reservations_in.push_back(token);
        }
    }
    const size_t bi{names_in.size()}, ri{reservations_in.size()};
    const size_t bo{marker ? marker->name_outputs.size() : 0}, ro{marker ? marker->reservation_outputs.size() : 0};
    const Operation* op{marker && marker->operation ? &*marker->operation : nullptr};
    const Txid txid{tx.GetHash()};

    Plan plan;
    plan.max_released = max_released;
    for (const auto& [n, token] : spent) plan.spent.push_back(tx.vin[n].prevout);
    std::vector<NameHash> names_out{names_in};
    std::vector<Token> reservations_out{reservations_in};

    if (const auto* reserve{op ? std::get_if<Reserve>(op) : nullptr}) {
        if (ro != ri + 1 || bo != bi) return invalid("bad-bn-unbalanced");
        Token made;
        made.kind = Token::Kind::RESERVATION;
        made.id = txid.ToUint256();
        made.commitment = reserve->commitment;
        reservations_out.push_back(made);
        plan.reservation_made = txid;
        plan.reservation_commitment = reserve->commitment;
        // What its commitment binds: the script of its output (the last reservation output).
        plan.reservation_origin = tx.vout[marker->reservation_outputs.back()].scriptPubKey;
    } else if (const auto* reg{op ? std::get_if<Register>(op) : nullptr}) {
        // Names registered before, in this block included, are taken.
        if (HasName(reg->name)) return invalid("bad-bn-name-taken");
        // A reservation is bound to the script of its output: nobody but its maker can have made it (a
        // copy to another script commits to nothing anyone can open, one to the maker's script is the
        // maker's), so it reveals the name whatever other reservations there are.
        auto burned{reservations_out.end()};
        for (auto it{reservations_out.begin()}; it != reservations_out.end(); ++it) {
            const auto script{GetReservationOrigin(Txid::FromUint256(it->id))};
            if (script && it->commitment == ReservationCommitment(reg->name, reg->nonce, *script)) {
                burned = it;
                break;
            }
        }
        if (burned == reservations_out.end()) return invalid("bad-bn-no-reservation");
        if (ri != ro + 1 || bo != bi + 1) return invalid("bad-bn-unbalanced");
        const int reserved_at{ReservationHeight(Txid::FromUint256(burned->id)).value_or(0)};
        // Only once the reservation is deep enough: whoever makes this block saw the name and the
        // nonce of every registration waiting for it, and could have reserved the name under a
        // nonce of its own, in this very block, to register it first.
        if (height - reserved_at < params.reveal_depth) return invalid("bad-bn-reservation-too-young");
        plan.reservation_revealed = Txid::FromUint256(burned->id);
        reservations_out.erase(burned);
        names_out.push_back(reg->name);
        NameRecord record;
        record.seq = NextSeq();
        record.registration = txid;
        record.height = height;
        record.data = reg->data;
        StampAll(plan.history, record.seq, reg->data, txid, height);
        if (reg->text) record.text = *reg->text;
        plan.names.emplace_back(reg->name, std::move(record));
        plan.seq = std::make_pair(NextSeq(), reg->name);
    } else if (const auto* update{op ? std::get_if<UpdateName>(op) : nullptr}) {
        if (bi < 1 || bo != bi || ro != ri) return invalid(bi < 1 ? "bad-bn-no-name" : "bad-bn-unbalanced");
        // The name updated is the one spent last; the others only move.
        const NameHash& name{names_in.back()};
        const auto found{GetName(name)};
        if (!found) return invalid("bad-bn-name-unknown");
        NameRecord record{*found};
        const NameUpdates& u{update->updates};
        ApplyUpdate(record.data.commitment, u.commitment, Field::COMMITMENT, record.seq, txid, height, plan.history);
        ApplyUpdate(record.data.ipv4, u.ipv4, Field::IPV4, record.seq, txid, height, plan.history);
        ApplyUpdate(record.data.ipv6, u.ipv6, Field::IPV6, record.seq, txid, height, plan.history);
        ApplyUpdate(record.data.encryption_key, u.encryption_key, Field::ENCRYPTION_KEY, record.seq, txid, height, plan.history);
        ApplyUpdate(record.data.signing_key, u.signing_key, Field::SIGNING_KEY, record.seq, txid, height, plan.history);
        ApplyUpdate(record.data.paymail_fee, u.paymail_fee, Field::PAYMAIL_FEE, record.seq, txid, height, plan.history);
        ApplyUpdate(record.data.records, u.records, Field::RECORDS, record.seq, txid, height, plan.history);
        plan.names.emplace_back(name, std::move(record));
    } else if (const auto* icann{op ? std::get_if<IcannNames>(op) : nullptr}) {
        if (bo != bi || ro != ri) return invalid("bad-bn-unbalanced");
        if (params.icann_authority.IsNull()) return invalid("bad-bn-icann-disabled");
        if (!XOnlyPubKey{params.icann_authority}.VerifySchnorr(IcannMessage(icann->names, tx.vin[0].prevout), icann->signature)) {
            return invalid("bad-bn-icann-signature");
        }
        for (const std::string& text : icann->names) {
            const NameHash name{HashName(text)};
            const auto found{GetName(name)};
            if (!found) return invalid("bad-bn-icann-unknown");
            if (found->icann) return invalid("bad-bn-icann-already");
            NameRecord record{*found};
            record.icann = true;
            record.icann_name = text;
            plan.names.emplace_back(name, std::move(record));
        }
    } else if (op && std::holds_alternative<Release>(*op)) {
        if (bo > bi || ro > ri || bo + ro == bi + ri) return invalid("bad-bn-unbalanced");
        // The names and reservations carried on are the first spent; the rest go.
        if (plan.OverLimit(bi - bo)) return invalid(too_large);
        for (size_t i{bo}; i < bi; ++i) {
            if (!HasName(names_in[i])) return invalid("bad-bn-name-unknown");
            plan.names_released.push_back(names_in[i]);
        }
        // The subnames of the released names, at any depth, go with them.
        const auto released{ReleaseDescendants(plan, plan.names_released)};
        if (!released) return invalid(too_large);
        for (size_t i{0}; i < bo; ++i) {
            if (released->contains(names_in[i])) return invalid("bad-bn-release-subname-kept");
        }
        // The names above those released count them out.
        CountOut(plan, *released);
        for (size_t i{ro}; i < ri; ++i) plan.reservations_released.push_back(Txid::FromUint256(reservations_in[i].id));
        names_out.resize(bo);
        reservations_out.resize(ro);
    } else if (const auto* sub{op ? std::get_if<RegisterSubname>(op) : nullptr}) {
        if (bi < 1 || bo != bi + 1 || ro != ri) return invalid(bi < 1 ? "bad-bn-no-name" : "bad-bn-unbalanced");
        // A subname of the name spent last.
        const NameHash& parent{names_in.back()};
        const auto parent_record{GetName(parent)};
        if (!parent_record) return invalid("bad-bn-name-unknown");
        const uint32_t depth{parent_record->depth + 1};
        if (depth > MAX_SUBNAME_DEPTH) return invalid("bad-bn-subname-too-deep");
        const NameHash child{SubnameHash(parent, sub->label)};
        if (const auto taken{GetName(child)}) {
            // A top-level name with the hash of this subname can only be one registered by its hash,
            // with no text that hashes to it: a squat. It gives way, with whatever was made under it;
            // a real subname of that label is taken.
            if (taken->parent) return invalid("bad-bn-name-taken");
            plan.names_replaced.push_back(child);
            const auto gone{ReleaseDescendants(plan, {child})};
            if (!gone) return invalid(too_large);
            // Neither the squat nor anything under it carried on: the subname would be on two outputs,
            // or an output would carry a name that is no more.
            for (const NameHash& name : names_in) {
                if (gone->contains(name)) return invalid("bad-bn-released-name-kept");
            }
        }
        // Each name above it counts it; the one at the top counts every name in its tree.
        std::vector<std::pair<NameHash, NameRecord>> above;
        for (std::optional<NameHash> at{parent}; at;) {
            auto ancestor{GetName(*at)};
            if (!ancestor) break;
            const NameHash here{*at};
            at = ancestor->parent;
            above.emplace_back(here, std::move(*ancestor));
        }
        if (above.back().second.descendants >= MAX_SUBNAMES) return invalid("bad-bn-too-many-subnames");
        for (auto& [hash, ancestor] : above) {
            ++ancestor.descendants;
            plan.names.emplace_back(hash, std::move(ancestor));
        }
        names_out.push_back(child);
        NameRecord record;
        record.seq = NextSeq();
        record.registration = txid;
        record.height = height;
        record.parent = parent;
        record.label = sub->label;
        record.depth = depth;
        record.data = sub->data;
        StampAll(plan.history, record.seq, sub->data, txid, height);
        plan.names.emplace_back(child, std::move(record));
        plan.seq = std::make_pair(NextSeq(), child);
    } else {
        if (bo != bi || ro != ri) return invalid("bad-bn-unbalanced");
    }

    // What the outputs carry: what was spent, in the order of the inputs, then what is new.
    for (size_t i{0}; i < bo; ++i) {
        Token token;
        token.kind = Token::Kind::NAME;
        token.id = names_out[i];
        plan.created.emplace_back(COutPoint{txid, marker->name_outputs[i]}, token);
    }
    for (size_t i{0}; i < ro; ++i) plan.created.emplace_back(COutPoint{txid, marker->reservation_outputs[i]}, reservations_out[i]);
    return plan;
}

bool State::CheckTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason, std::optional<uint32_t> max_released) const
{
    return MakePlan(tx, height, params, reject_reason, max_released).has_value();
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason)
{
    const auto plan{MakePlan(tx, height, params, reject_reason)};
    if (!plan) return false;
    sidechain::StoreOverlay& out{Writable()};
    // In the order the rules have always applied a plan in.
    for (const COutPoint& outpoint : plan->spent) SetToken(outpoint, std::nullopt);
    for (const auto& [outpoint, token] : plan->created) SetToken(outpoint, token);
    if (plan->reservation_made) SetReservation(*plan->reservation_made, ReservationRecord{plan->reservation_commitment, height});
    if (plan->reservation_made && plan->reservation_origin) ORIGINS.Put(out, plan->reservation_made->ToUint256(), *plan->reservation_origin);
    if (plan->reservation_revealed) SetReservation(*plan->reservation_revealed, std::nullopt);
    const auto forget_origin{[&](const Txid& id) {
        if (ORIGINS.Contains(*m_view, id.ToUint256())) ORIGINS.Erase(out, id.ToUint256());
    }};
    if (plan->reservation_revealed) forget_origin(*plan->reservation_revealed);
    for (const NameHash& name : plan->names_replaced) {
        if (const auto record{GetName(name)}) {
            SEQ.Erase(out, record->seq);
            EraseHistory(record->seq);
        }
    }
    for (const auto& [name, record] : plan->names) SetName(name, record);
    if (plan->seq) {
        SEQ.Put(out, plan->seq->first, plan->seq->second);
        NEXT_SEQ.Put(out, NextSeq() + 1);
    }
    for (const HistoryEntry& entry : plan->history) AddHistory(entry.seq, entry.field, entry.height, entry.stamped);
    // Released names leave the registry and the numbering (their numbers are not given again), with
    // their history.
    for (const NameHash& name : plan->names_released) {
        const auto record{GetName(name)};
        if (!record) continue;
        SEQ.Erase(out, record->seq);
        EraseHistory(record->seq);
        SetName(name, std::nullopt);
    }
    for (const Txid& id : plan->reservations_released) {
        SetReservation(id, std::nullopt);
        forget_origin(id);
    }
    return true;
}

} // namespace bitnames
