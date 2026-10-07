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

template <typename T>
std::optional<T> Latest(const std::vector<Stamped<T>>& history)
{
    return history.empty() ? std::nullopt : history.back().value;
}

template <typename T>
std::optional<T> AtHeightOf(const std::vector<Stamped<T>>& history, int height)
{
    std::optional<T> value;
    for (const Stamped<T>& entry : history) {
        if (entry.height > height) break;
        value = entry.value;
    }
    return value;
}

template <typename T>
void ApplyUpdate(std::vector<Stamped<T>>& history, const Update<T>& update, const Txid& txid, int height)
{
    if (update.kind == UpdateKind::RETAIN) return;
    history.push_back({update.kind == UpdateKind::SET ? update.value : std::nullopt, txid, height});
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

uint256 ReservationCommitment(const NameHash& name, const uint256& nonce)
{
    uint256 commitment;
    CHMAC_SHA256(nonce.data(), nonce.size()).Write(name.data(), name.size()).Finalize(commitment.data());
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

NameData NameRecord::Current() const
{
    NameData data;
    data.commitment = Latest(commitment);
    data.ipv4 = Latest(ipv4);
    data.ipv6 = Latest(ipv6);
    data.encryption_key = Latest(encryption_key);
    data.signing_key = Latest(signing_key);
    data.paymail_fee = Latest(paymail_fee);
    data.records = Latest(records);
    return data;
}

std::optional<NameData> NameRecord::AtHeight(int at) const
{
    if (at < height) return std::nullopt;
    NameData data;
    data.commitment = AtHeightOf(commitment, at);
    data.ipv4 = AtHeightOf(ipv4, at);
    data.ipv6 = AtHeightOf(ipv6, at);
    data.encryption_key = AtHeightOf(encryption_key, at);
    data.signing_key = AtHeightOf(signing_key, at);
    data.paymail_fee = AtHeightOf(paymail_fee, at);
    data.records = AtHeightOf(records, at);
    return data;
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
    std::optional<Txid> reservation_revealed;
    std::optional<std::pair<uint32_t, NameHash>> seq;
    //! Names and reservations released.
    std::vector<NameHash> names_released;
    std::vector<Txid> reservations_released;
};

std::vector<std::pair<uint32_t, Token>> State::SpentTokens(const CTransaction& tx) const
{
    std::vector<std::pair<uint32_t, Token>> spent;
    if (tx.IsCoinBase()) return spent;
    for (uint32_t i{0}; i < tx.vin.size(); ++i) {
        if (const auto it{m_tokens.find(tx.vin[i].prevout)}; it != m_tokens.end()) spent.emplace_back(i, it->second);
    }
    return spent;
}

std::optional<NameHash> State::NameOfSeq(uint32_t seq) const
{
    const auto it{m_seq.find(seq)};
    if (it == m_seq.end()) return std::nullopt;
    return it->second;
}

std::optional<State::Plan> State::MakePlan(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) -> std::optional<Plan> {
        reject_reason = reason;
        return std::nullopt;
    }};
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
    } else if (const auto* reg{op ? std::get_if<Register>(op) : nullptr}) {
        // Names registered before, in this block included, are taken.
        if (m_names.contains(reg->name)) return invalid("bad-bn-name-taken");
        const uint256 implied{ReservationCommitment(reg->name, reg->nonce)};
        const auto burned{std::find_if(reservations_out.begin(), reservations_out.end(), [&](const Token& t) { return t.commitment == implied; })};
        if (burned == reservations_out.end()) return invalid("bad-bn-no-reservation");
        if (ri != ro + 1 || bo != bi + 1) return invalid("bad-bn-unbalanced");
        plan.reservation_revealed = Txid::FromUint256(burned->id);
        reservations_out.erase(burned);
        names_out.push_back(reg->name);
        NameRecord record;
        record.seq = m_next_seq;
        record.registration = txid;
        record.height = height;
        record.commitment.push_back({reg->data.commitment, txid, height});
        record.ipv4.push_back({reg->data.ipv4, txid, height});
        record.ipv6.push_back({reg->data.ipv6, txid, height});
        record.encryption_key.push_back({reg->data.encryption_key, txid, height});
        record.signing_key.push_back({reg->data.signing_key, txid, height});
        record.paymail_fee.push_back({reg->data.paymail_fee, txid, height});
        record.records.push_back({reg->data.records, txid, height});
        if (reg->text) record.text = *reg->text;
        plan.names.emplace_back(reg->name, std::move(record));
        plan.seq = std::make_pair(m_next_seq, reg->name);
    } else if (const auto* update{op ? std::get_if<UpdateName>(op) : nullptr}) {
        if (bi < 1 || bo != bi || ro != ri) return invalid(bi < 1 ? "bad-bn-no-name" : "bad-bn-unbalanced");
        // The name updated is the one spent last; the others only move.
        const NameHash& name{names_in.back()};
        const auto it{m_names.find(name)};
        if (it == m_names.end()) return invalid("bad-bn-name-unknown");
        NameRecord record{it->second};
        ApplyUpdate(record.commitment, update->updates.commitment, txid, height);
        ApplyUpdate(record.ipv4, update->updates.ipv4, txid, height);
        ApplyUpdate(record.ipv6, update->updates.ipv6, txid, height);
        ApplyUpdate(record.encryption_key, update->updates.encryption_key, txid, height);
        ApplyUpdate(record.signing_key, update->updates.signing_key, txid, height);
        ApplyUpdate(record.paymail_fee, update->updates.paymail_fee, txid, height);
        ApplyUpdate(record.records, update->updates.records, txid, height);
        plan.names.emplace_back(name, std::move(record));
    } else if (const auto* icann{op ? std::get_if<IcannNames>(op) : nullptr}) {
        if (bo != bi || ro != ri) return invalid("bad-bn-unbalanced");
        if (params.icann_authority.IsNull()) return invalid("bad-bn-icann-disabled");
        if (!XOnlyPubKey{params.icann_authority}.VerifySchnorr(IcannMessage(icann->names, tx.vin[0].prevout), icann->signature)) {
            return invalid("bad-bn-icann-signature");
        }
        for (const std::string& text : icann->names) {
            const NameHash name{HashName(text)};
            const auto it{m_names.find(name)};
            if (it == m_names.end()) return invalid("bad-bn-icann-unknown");
            if (it->second.icann) return invalid("bad-bn-icann-already");
            NameRecord record{it->second};
            record.icann = true;
            record.icann_name = text;
            plan.names.emplace_back(name, std::move(record));
        }
    } else if (op && std::holds_alternative<Release>(*op)) {
        if (bo > bi || ro > ri || bo + ro == bi + ri) return invalid("bad-bn-unbalanced");
        // The names and reservations carried on are the first spent; the rest go.
        for (size_t i{bo}; i < bi; ++i) {
            const auto it{m_names.find(names_in[i])};
            if (it == m_names.end()) return invalid("bad-bn-name-unknown");
            plan.names_released.push_back(names_in[i]);
        }
        if (height >= params.release_subnames_height) {
            // The subnames of the released names, at any depth, go with them; their holders keep the
            // coins, which no longer carry a name.
            std::set<NameHash> gone{plan.names_released.begin(), plan.names_released.end()};
            for (bool grew{true}; grew;) {
                grew = false;
                for (const auto& [hash, record] : m_names) {
                    if (record.parent && gone.contains(*record.parent) && gone.insert(hash).second) {
                        plan.names_released.push_back(hash);
                        grew = true;
                    }
                }
            }
            for (size_t i{0}; i < bo; ++i) {
                if (gone.contains(names_in[i])) return invalid("bad-bn-release-subname-kept");
            }
            const std::set<COutPoint> spent_here{plan.spent.begin(), plan.spent.end()};
            for (const auto& [outpoint, token] : m_tokens) {
                if (token.kind == Token::Kind::NAME && gone.contains(token.id) && !spent_here.contains(outpoint)) plan.spent.push_back(outpoint);
            }
        }
        for (size_t i{ro}; i < ri; ++i) plan.reservations_released.push_back(Txid::FromUint256(reservations_in[i].id));
        names_out.resize(bo);
        reservations_out.resize(ro);
    } else if (const auto* sub{op ? std::get_if<RegisterSubname>(op) : nullptr}) {
        if (bi < 1 || bo != bi + 1 || ro != ri) return invalid(bi < 1 ? "bad-bn-no-name" : "bad-bn-unbalanced");
        // A subname of the name spent last.
        const NameHash& parent{names_in.back()};
        if (!m_names.contains(parent)) return invalid("bad-bn-name-unknown");
        const NameHash child{SubnameHash(parent, sub->label)};
        if (m_names.contains(child)) return invalid("bad-bn-name-taken");
        names_out.push_back(child);
        NameRecord record;
        record.seq = m_next_seq;
        record.registration = txid;
        record.height = height;
        record.parent = parent;
        record.label = sub->label;
        record.commitment.push_back({sub->data.commitment, txid, height});
        record.ipv4.push_back({sub->data.ipv4, txid, height});
        record.ipv6.push_back({sub->data.ipv6, txid, height});
        record.encryption_key.push_back({sub->data.encryption_key, txid, height});
        record.signing_key.push_back({sub->data.signing_key, txid, height});
        record.paymail_fee.push_back({sub->data.paymail_fee, txid, height});
        record.records.push_back({sub->data.records, txid, height});
        plan.names.emplace_back(child, std::move(record));
        plan.seq = std::make_pair(m_next_seq, child);
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

bool State::CheckTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reject_reason) const
{
    return MakePlan(tx, height, params, reject_reason).has_value();
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, StateUndo& undo, std::string& reject_reason)
{
    const auto plan{MakePlan(tx, height, params, reject_reason)};
    if (!plan) return false;
    if (!undo.next_seq) undo.next_seq = m_next_seq;
    // Every change is noted with the value it replaced; reverting them in reverse order restores the state.
    const auto set_token{[&](const COutPoint& outpoint, const std::optional<Token>& value) {
        const auto it{m_tokens.find(outpoint)};
        undo.tokens.emplace_back(outpoint, it == m_tokens.end() ? std::nullopt : std::optional<Token>{it->second});
        if (value) {
            m_tokens.insert_or_assign(outpoint, *value);
        } else if (it != m_tokens.end()) {
            m_tokens.erase(it);
        }
    }};
    const auto set_reservation{[&](const Txid& id, const std::optional<uint256>& value) {
        const auto it{m_reservations.find(id)};
        undo.reservations.emplace_back(id, it == m_reservations.end() ? std::nullopt : std::optional<uint256>{it->second});
        if (value) {
            m_reservations.insert_or_assign(id, *value);
        } else if (it != m_reservations.end()) {
            m_reservations.erase(it);
        }
    }};
    for (const COutPoint& outpoint : plan->spent) set_token(outpoint, std::nullopt);
    for (const auto& [outpoint, token] : plan->created) set_token(outpoint, token);
    if (plan->reservation_made) set_reservation(*plan->reservation_made, plan->reservation_commitment);
    if (plan->reservation_revealed) set_reservation(*plan->reservation_revealed, std::nullopt);
    for (const auto& [name, record] : plan->names) {
        const auto it{m_names.find(name)};
        undo.names.emplace_back(name, it == m_names.end() ? std::nullopt : std::optional<NameRecord>{it->second});
        m_names.insert_or_assign(name, record);
    }
    if (plan->seq) {
        m_seq.emplace(plan->seq->first, plan->seq->second);
        ++m_next_seq;
    }
    // Released names leave the registry and the numbering (their numbers are not given again).
    for (const NameHash& name : plan->names_released) {
        const auto it{m_names.find(name)};
        if (it == m_names.end()) continue;
        undo.names.emplace_back(name, it->second);
        undo.seqs.emplace_back(it->second.seq, name);
        m_seq.erase(it->second.seq);
        m_names.erase(it);
    }
    for (const Txid& id : plan->reservations_released) set_reservation(id, std::nullopt);
    return true;
}

void State::Revert(const StateUndo& undo)
{
    for (auto it{undo.tokens.rbegin()}; it != undo.tokens.rend(); ++it) {
        if (it->second) {
            m_tokens.insert_or_assign(it->first, *it->second);
        } else {
            m_tokens.erase(it->first);
        }
    }
    for (auto it{undo.names.rbegin()}; it != undo.names.rend(); ++it) {
        if (it->second) {
            m_names.insert_or_assign(it->first, *it->second);
        } else {
            m_names.erase(it->first);
        }
    }
    for (auto it{undo.reservations.rbegin()}; it != undo.reservations.rend(); ++it) {
        if (it->second) {
            m_reservations.insert_or_assign(it->first, *it->second);
        } else {
            m_reservations.erase(it->first);
        }
    }
    if (undo.next_seq) {
        m_seq.erase(m_seq.lower_bound(*undo.next_seq), m_seq.end());
        m_next_seq = *undo.next_seq;
    }
    for (const auto& [seq, name] : undo.seqs) m_seq.emplace(seq, name);
}

} // namespace bitnames
