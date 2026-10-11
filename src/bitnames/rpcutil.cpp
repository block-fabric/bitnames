// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitnames/rpcutil.h>
#include <core_io.h>

#include <netaddress.h>
#include <netbase.h>
#include <pubkey.h>
#include <rpc/protocol.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace bitnames {

std::vector<RPCArg> DataArgs()
{
    return {
        {"commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A hash (32 bytes, hex) of data kept elsewhere, such as a zone file"},
        {"ipv4", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An IPv4 address, with a port or not: \"203.0.113.7\" or \"203.0.113.7:443\". It is the A record of the name in DNS, unless its records have an \"ip\""},
        {"ipv6", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An IPv6 address, with a port or not: \"2001:db8::7\" or \"[2001:db8::7]:443\". It is the AAAA record of the name, unless its records have an \"ip6\""},
        {"encryptionkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A public key (33 bytes, hex) that messages to the owner are encrypted to; see getnewnamekeys"},
        {"signingkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "An x-only public key (32 bytes, hex) the owner signs messages with; see getnewnamekeys"},
        {"paymailfee", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The least a payment with a message to the name has to pay for the owner to see it"},
        {"records", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "DNS records, as Namecoin domains have them: a JSON object (at most 1024 bytes), "
            "such as {\"ip\": \"203.0.113.7\", \"ip6\": \"2001:db8::7\", \"alias\": \"host.\", \"txt\": [...], \"mx\": [[10, \"mail.example.com.\"]], "
            "\"srv\": [[prio, weight, port, \"target.\"]], \"tls\": [[usage, selector, type, \"hex\"]], \"ns\": [...], "
            "\"map\": {\"www\": {...}, \"shop\": {\"delegate\": \"othername\"}, \"*\": {...}}}; see doc/bitnames.md"},
    };
}

std::vector<RPCResult> DataResults()
{
    return {
        {RPCResult::Type::STR_HEX, "commitment", /*optional=*/true, "A hash of data kept elsewhere"},
        {RPCResult::Type::STR, "ipv4", /*optional=*/true, "An IPv4 address and port"},
        {RPCResult::Type::STR, "ipv6", /*optional=*/true, "An IPv6 address and port"},
        {RPCResult::Type::STR_HEX, "encryptionkey", /*optional=*/true, "The key messages to the owner are encrypted to"},
        {RPCResult::Type::STR_HEX, "signingkey", /*optional=*/true, "The key the owner signs messages with"},
        {RPCResult::Type::STR_AMOUNT, "paymailfee", /*optional=*/true, "The least a payment with a message has to pay"},
        {RPCResult::Type::OBJ_DYN, "records", /*optional=*/true, "DNS records", {{RPCResult::Type::ANY, "record", ""}}},
    };
}

std::string FormatSocket(const SocketV4& socket)
{
    const std::string address{strprintf("%u.%u.%u.%u", socket.ip[0], socket.ip[1], socket.ip[2], socket.ip[3])};
    // Port 0: an address alone, as for DNS.
    return socket.port == 0 ? address : strprintf("%s:%u", address, socket.port);
}

std::string FormatSocket(const SocketV6& socket)
{
    in6_addr addr;
    std::memcpy(&addr, socket.ip.data(), 16);
    if (socket.port == 0) return CNetAddr{addr}.ToStringAddr();
    return CService{CNetAddr{addr}, socket.port}.ToStringAddrPort();
}

std::optional<SocketV4> ParseSocketV4(const std::string& text)
{
    const auto service{Lookup(text, 0, /*fAllowLookup=*/false)};
    if (!service || !service->IsIPv4()) return std::nullopt;
    in_addr addr;
    if (!service->GetInAddr(&addr)) return std::nullopt;
    SocketV4 socket;
    std::memcpy(socket.ip.data(), &addr, 4);
    socket.port = service->GetPort();
    return socket;
}

std::optional<SocketV6> ParseSocketV6(const std::string& text)
{
    const auto service{Lookup(text, 0, /*fAllowLookup=*/false)};
    if (!service || !service->IsIPv6()) return std::nullopt;
    in6_addr addr;
    if (!service->GetIn6Addr(&addr)) return std::nullopt;
    SocketV6 socket;
    std::memcpy(socket.ip.data(), &addr, 16);
    socket.port = service->GetPort();
    return socket;
}

namespace {
uint256 ParseCommitment(const UniValue& value)
{
    const auto hash{uint256::FromHex(value.get_str())};
    if (!hash) throw JSONRPCError(RPC_INVALID_PARAMETER, "commitment: 32 bytes in hex");
    return *hash;
}

SocketV4 ParseV4(const UniValue& value)
{
    const auto socket{ParseSocketV4(value.get_str())};
    if (!socket) throw JSONRPCError(RPC_INVALID_PARAMETER, "ipv4: an IPv4 address, such as \"203.0.113.7\", with a port or not");
    return *socket;
}

SocketV6 ParseV6(const UniValue& value)
{
    const auto socket{ParseSocketV6(value.get_str())};
    if (!socket) throw JSONRPCError(RPC_INVALID_PARAMETER, "ipv6: an IPv6 address, such as \"2001:db8::7\", or in brackets with a port, \"[2001:db8::7]:443\"");
    return *socket;
}

EncryptionKey ParseEncryptionKey(const UniValue& value)
{
    const std::vector<unsigned char> bytes{ParseHex(value.get_str())};
    if (bytes.size() != 33 || !CPubKey{bytes}.IsFullyValid()) throw JSONRPCError(RPC_INVALID_PARAMETER, "encryptionkey: a compressed public key, 33 bytes in hex");
    EncryptionKey key;
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

SigningKey ParseSigningKey(const UniValue& value)
{
    // In the order of its bytes, as keys are written (not reversed, as hashes are).
    const std::vector<unsigned char> bytes{ParseHex(value.get_str())};
    if (bytes.size() != 32 || !XOnlyPubKey{bytes}.IsFullyValid()) throw JSONRPCError(RPC_INVALID_PARAMETER, "signingkey: an x-only public key, 32 bytes in hex");
    return uint256{std::span{bytes}};
}

CAmount ParseFee(const UniValue& value)
{
    const CAmount fee{AmountFromValue(value)};
    return fee;
}

std::string ParseRecords(const UniValue& value)
{
    std::string text{value.isStr() ? value.get_str() : value.write()};
    UniValue parsed;
    if (!parsed.read(text) || !parsed.isObject()) throw JSONRPCError(RPC_INVALID_PARAMETER, "records: a JSON object");
    text = parsed.write();
    if (text.size() > MAX_RECORDS_SIZE) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("records: at most %u bytes of JSON", MAX_RECORDS_SIZE));
    return text;
}

template <typename T, typename F>
void ReadUpdate(const UniValue& obj, const std::string& key, Update<T>& update, F parse)
{
    if (!obj.exists(key)) return;
    if (obj[key].isNull()) {
        update.kind = UpdateKind::DELETE;
        return;
    }
    update.kind = UpdateKind::SET;
    update.value = parse(obj[key]);
}

const std::vector<std::string> FIELDS{"commitment", "ipv4", "ipv6", "encryptionkey", "signingkey", "paymailfee", "records"};

void CheckFields(const UniValue& obj)
{
    if (!obj.isObject()) throw JSONRPCError(RPC_INVALID_PARAMETER, "The data of a name is an object");
    for (const std::string& key : obj.getKeys()) {
        if (std::find(FIELDS.begin(), FIELDS.end(), key) == FIELDS.end()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown field of name data: " + key);
    }
}
} // namespace

namespace {
/** Records being built: the values of each key of a node, and the nodes under it. */
struct Node {
    std::map<std::string, std::vector<UniValue>> values;
    std::map<std::string, Node> map;

    UniValue ToJSON() const
    {
        UniValue obj(UniValue::VOBJ);
        for (const auto& [key, list] : values) {
            // One value stands alone; several make a list. A CNAME or a delegation is one name.
            if (list.size() == 1 && (key == "alias" || key == "delegate" || !list[0].isArray())) {
                obj.pushKV(key, list[0]);
            } else {
                UniValue arr(UniValue::VARR);
                for (const UniValue& v : list) arr.push_back(v);
                obj.pushKV(key, std::move(arr));
            }
        }
        if (!map.empty()) {
            UniValue m(UniValue::VOBJ);
            for (const auto& [label, node] : map) m.pushKV(label, node.ToJSON());
            obj.pushKV("map", std::move(m));
        }
        return obj;
    }
};

/** The node for a host: "@" is the name, "a.b" is map.b.map.a. */
Node& NodeFor(Node& root, const std::string& host)
{
    Node* node{&root};
    if (host.empty() || host == "@") return *node;
    std::vector<std::string> labels{util::SplitString(ToLower(host), '.')};
    for (auto it{labels.rbegin()}; it != labels.rend(); ++it) {
        // Service labels (_sip, _tcp, _443) start with an underscore.
        const bool service{it->size() > 1 && (*it)[0] == '_' && IsDnsLabel(it->substr(1))};
        if (*it != "*" && !service && !IsDnsLabel(*it)) throw JSONRPCError(RPC_INVALID_PARAMETER, "host: labels of lower-case letters, digits and hyphens (_ in front for services), or *");
        node = &node->map[*it];
    }
    return *node;
}

int64_t Number(const UniValue& row, const char* key, int64_t fallback)
{
    if (!row.exists(key) || row[key].isNull()) return fallback;
    return ToIntegral<int64_t>(row[key].isNum() ? row[key].getValStr() : row[key].get_str()).value_or(fallback);
}

void RowsOf(const UniValue& node, const std::string& host, UniValue& rows)
{
    const auto add{[&](const std::string& type, const std::string& value, std::optional<int64_t> priority = {}, std::optional<int64_t> weight = {}, std::optional<int64_t> port = {}) {
        UniValue row(UniValue::VOBJ);
        row.pushKV("host", host);
        row.pushKV("type", type);
        row.pushKV("value", value);
        if (priority) row.pushKV("priority", *priority);
        if (weight) row.pushKV("weight", *weight);
        if (port) row.pushKV("port", *port);
        rows.push_back(std::move(row));
    }};
    const auto many{[&](const char* key) {
        std::vector<UniValue> out;
        if (!node.exists(key)) return out;
        if (node[key].isArray()) return node[key].getValues();
        return std::vector<UniValue>{node[key]};
    }};
    if (node.exists("alias") && node["alias"].isStr()) add("CNAME", node["alias"].get_str());
    if (node.exists("delegate") && node["delegate"].isStr()) add("DELEGATE", node["delegate"].get_str());
    for (const UniValue& v : many("ip")) if (v.isStr()) add("A", v.get_str());
    for (const UniValue& v : many("ip6")) if (v.isStr()) add("AAAA", v.get_str());
    for (const UniValue& v : many("txt")) if (v.isStr()) add("TXT", v.get_str());
    for (const UniValue& v : many("ns")) if (v.isStr()) add("NS", v.get_str());
    for (const UniValue& v : many("mx")) {
        std::optional<int64_t> pref;
        std::string host_name;
        bool bad{false};
        for (const UniValue& part : v.isArray() ? v.getValues() : std::vector<UniValue>{v}) {
            if (part.isNum()) {
                pref = ToIntegral<int64_t>(part.getValStr());
                bad |= !pref;
            }
            if (part.isStr()) host_name = part.get_str();
        }
        if (!bad) add("MX", host_name, pref.value_or(10));
    }
    // Numbers of records are anyone's JSON: one that is not a whole number leaves its record out.
    const auto whole{[](const UniValue& v, size_t i) { return v[i].isNum() ? ToIntegral<int64_t>(v[i].getValStr()) : std::nullopt; }};
    for (const UniValue& v : many("srv")) {
        if (!v.isArray() || v.size() != 4 || !v[3].isStr()) continue;
        const auto a{whole(v, 0)}, b{whole(v, 1)}, c{whole(v, 2)};
        if (a && b && c) add("SRV", v[3].get_str(), *a, *b, *c);
    }
    for (const UniValue& v : many("tls")) {
        if (!v.isArray() || v.size() != 4 || !v[3].isStr()) continue;
        const auto a{whole(v, 0)}, b{whole(v, 1)}, c{whole(v, 2)};
        if (a && b && c) add("TLSA", strprintf("%d %d %d %s", *a, *b, *c, v[3].get_str()));
    }
    if (node.exists("map") && node["map"].isObject()) {
        const UniValue& map{node["map"]};
        for (const std::string& label : map.getKeys()) {
            const UniValue& sub{map[label]};
            const std::string sub_host{label.empty() ? host : (host == "@" ? label : label + "." + host)};
            if (sub.isStr()) {
                // An address alone, as DNS reads it: {"ip": address}, of the host under this one.
                UniValue node(UniValue::VOBJ);
                node.pushKV("ip", sub.get_str());
                RowsOf(node, sub_host, rows);
            } else {
                RowsOf(sub, sub_host, rows);
            }
        }
    }
}
} // namespace

std::string RecordsFromRows(const UniValue& rows)
{
    if (!rows.isArray()) throw JSONRPCError(RPC_INVALID_PARAMETER, "records: a list of rows");
    Node root;
    for (const UniValue& row : rows.getValues()) {
        if (!row.isObject() || !row.exists("type") || !row.exists("value")) throw JSONRPCError(RPC_INVALID_PARAMETER, "Each record has a type and a value");
        const std::string host{row.exists("host") && !row["host"].isNull() ? row["host"].get_str() : "@"};
        const std::string type{ToUpper(row["type"].get_str())};
        const std::string value{row["value"].isStr() ? row["value"].get_str() : row["value"].getValStr()};
        Node& node{NodeFor(root, host)};
        if (type == "A") {
            const auto addr{LookupHost(value, false)};
            if (!addr || !addr->IsIPv4()) throw JSONRPCError(RPC_INVALID_PARAMETER, "A: an IPv4 address, such as 203.0.113.7");
            node.values["ip"].push_back(UniValue{value});
        } else if (type == "AAAA") {
            const auto addr{LookupHost(value, false)};
            if (!addr || !addr->IsIPv6()) throw JSONRPCError(RPC_INVALID_PARAMETER, "AAAA: an IPv6 address, such as 2001:db8::7");
            node.values["ip6"].push_back(UniValue{value});
        } else if (type == "CNAME") {
            if (node.values.contains("alias")) throw JSONRPCError(RPC_INVALID_PARAMETER, "One CNAME per host");
            node.values["alias"].push_back(UniValue{value});
        } else if (type == "DELEGATE") {
            // As DNS follows it: a name, a subname (blog.core) or a hash ("0x" and 64 hex digits).
            if (!IsNameTextForm(value)) throw JSONRPCError(RPC_INVALID_PARAMETER, "DELEGATE: a name, a subname such as blog.core (lower-case letters, digits, - and _), or the hash of a name (\"0x\" and 64 hex digits)");
            node.values["delegate"] = {UniValue{value}};
        } else if (type == "TXT") {
            node.values["txt"].push_back(UniValue{value});
        } else if (type == "NS") {
            node.values["ns"].push_back(UniValue{value});
        } else if (type == "MX") {
            UniValue mx(UniValue::VARR);
            mx.push_back(Number(row, "priority", 10));
            mx.push_back(value);
            node.values["mx"].push_back(std::move(mx));
        } else if (type == "SRV") {
            UniValue srv(UniValue::VARR);
            srv.push_back(Number(row, "priority", 10));
            srv.push_back(Number(row, "weight", 0));
            srv.push_back(Number(row, "port", 0));
            srv.push_back(value);
            node.values["srv"].push_back(std::move(srv));
        } else if (type == "TLSA") {
            // "usage selector matching-type hex", as written in zone files.
            std::vector<std::string> parts{util::SplitString(value, ' ')};
            std::erase(parts, "");
            if (parts.size() != 4) throw JSONRPCError(RPC_INVALID_PARAMETER, "TLSA: usage selector type data, such as 3 1 1 ab12...");
            UniValue tls(UniValue::VARR);
            for (int i{0}; i < 3; ++i) tls.push_back(ToIntegral<int64_t>(parts[i]).value_or(0));
            tls.push_back(parts[3]);
            node.values["tls"].push_back(std::move(tls));
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Record types: A, AAAA, CNAME, TXT, MX, SRV, NS, TLSA, DELEGATE");
        }
    }
    const std::string text{root.ToJSON().write()};
    if (text.size() > MAX_RECORDS_SIZE) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The records take %u bytes, more than the %u a name holds", text.size(), MAX_RECORDS_SIZE));
    return text;
}

std::vector<std::string> RecordHosts(const std::string& records)
{
    UniValue root;
    if (records.empty() || !root.read(records) || !root.isObject() || !root.exists("map") || !root["map"].isObject()) return {};
    return root["map"].getKeys();
}

void CheckRecordsBesideSubnames(const std::string& records, const std::vector<std::string>& subnames)
{
    for (const std::string& host : RecordHosts(records)) {
        if (std::find(subnames.begin(), subnames.end(), ToLower(host)) != subnames.end()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s and what is under it belong to the subname %s, whose holder sets their records: leave the host %s out", host, host, host));
        }
    }
}

UniValue RowsFromRecords(const std::string& records)
{
    UniValue rows(UniValue::VARR);
    UniValue root;
    if (records.empty() || !root.read(records) || !root.isObject()) return rows;
    RowsOf(root, "@", rows);
    return rows;
}

namespace {
/**
 * Records in one form, to compare: keys sorted, a value alone as a list of one, an empty list left out, a
 * string of the map as {"ip": string}. A CNAME or a delegation stays one value, as the rows keep it.
 */
UniValue NormalRecords(const UniValue& node)
{
    if (!node.isObject()) return node;
    std::vector<std::string> keys{node.getKeys()};
    std::sort(keys.begin(), keys.end());
    UniValue out(UniValue::VOBJ);
    for (const std::string& key : keys) {
        const UniValue& value{node[key]};
        if (key == "map" && value.isObject()) {
            std::vector<std::string> labels{value.getKeys()};
            std::sort(labels.begin(), labels.end());
            UniValue map(UniValue::VOBJ);
            for (const std::string& label : labels) {
                UniValue sub{value[label]};
                if (sub.isStr()) {
                    UniValue ip(UniValue::VOBJ);
                    ip.pushKV("ip", sub);
                    sub = std::move(ip);
                }
                map.pushKV(label, NormalRecords(sub));
            }
            if (!map.empty()) out.pushKV(key, std::move(map));
        } else if (key == "alias" || key == "delegate" || key == "map") {
            out.pushKV(key, value);
        } else if (value.isArray()) {
            if (!value.empty()) out.pushKV(key, value);
        } else {
            UniValue list(UniValue::VARR);
            list.push_back(value);
            out.pushKV(key, std::move(list));
        }
    }
    return out;
}
} // namespace

bool RowsKeepRecords(const std::string& records)
{
    if (records.empty()) return true;
    UniValue before;
    if (!before.read(records) || !before.isObject()) return false;
    UniValue after;
    try {
        if (!after.read(RecordsFromRows(RowsFromRecords(records)))) return false;
    } catch (const UniValue&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
    return NormalRecords(before).write() == NormalRecords(after).write();
}

NameData ParseData(const UniValue& value)
{
    CheckFields(value);
    NameUpdates updates{ParseUpdates(value)};
    NameData data;
    if (updates.commitment.kind == UpdateKind::SET) data.commitment = updates.commitment.value;
    if (updates.ipv4.kind == UpdateKind::SET) data.ipv4 = updates.ipv4.value;
    if (updates.ipv6.kind == UpdateKind::SET) data.ipv6 = updates.ipv6.value;
    if (updates.encryption_key.kind == UpdateKind::SET) data.encryption_key = updates.encryption_key.value;
    if (updates.signing_key.kind == UpdateKind::SET) data.signing_key = updates.signing_key.value;
    if (updates.paymail_fee.kind == UpdateKind::SET) data.paymail_fee = updates.paymail_fee.value;
    if (updates.records.kind == UpdateKind::SET) data.records = updates.records.value;
    return data;
}

NameUpdates ParseUpdates(const UniValue& value)
{
    CheckFields(value);
    NameUpdates updates;
    ReadUpdate(value, "commitment", updates.commitment, ParseCommitment);
    ReadUpdate(value, "ipv4", updates.ipv4, ParseV4);
    ReadUpdate(value, "ipv6", updates.ipv6, ParseV6);
    ReadUpdate(value, "encryptionkey", updates.encryption_key, ParseEncryptionKey);
    ReadUpdate(value, "signingkey", updates.signing_key, ParseSigningKey);
    ReadUpdate(value, "paymailfee", updates.paymail_fee, ParseFee);
    ReadUpdate(value, "records", updates.records, ParseRecords);
    return updates;
}

UniValue DataToJSON(const NameData& data)
{
    UniValue obj(UniValue::VOBJ);
    if (data.commitment) obj.pushKV("commitment", data.commitment->GetHex());
    if (data.ipv4) obj.pushKV("ipv4", FormatSocket(*data.ipv4));
    if (data.ipv6) obj.pushKV("ipv6", FormatSocket(*data.ipv6));
    if (data.encryption_key) obj.pushKV("encryptionkey", HexStr(*data.encryption_key));
    if (data.signing_key) obj.pushKV("signingkey", HexStr(*data.signing_key));
    if (data.paymail_fee) obj.pushKV("paymailfee", ValueFromAmount(*data.paymail_fee));
    if (data.records) {
        UniValue records;
        if (records.read(*data.records)) obj.pushKV("records", std::move(records));
    }
    return obj;
}

bool IsGoodNameText(const std::string& text)
{
    if (text.empty() || text.size() > MAX_NAME_TEXT_SIZE) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
}

namespace {
/** The text a top-level name is shown by: its public text, if it is of the characters names are made of (or the ICANN name it was marked as). */
std::string ShownText(const NameRecord& record)
{
    if (record.icann && record.text == record.icann_name) return record.text;
    return IsGoodNameText(record.text) ? record.text : std::string{};
}
} // namespace

std::string FullText(const State& state, const NameRecord& record)
{
    std::string text{record.parent ? record.label : ShownText(record)};
    if (text.empty()) return {};
    NameRecord at{record};
    for (int depth{0}; at.parent && depth < 64; ++depth) {
        auto parent{state.GetName(*at.parent)};
        if (!parent) return {};
        at = std::move(*parent);
        const std::string part{at.parent ? at.label : ShownText(at)};
        if (part.empty()) return {};
        text += "." + part;
    }
    // Deeper than that: no text rather than a part of it, which would be the text of another name.
    if (at.parent) return {};
    return text;
}

NameHash ParseNameArg(const UniValue& value, const State& state)
{
    return ParseNameArg(value, [&](const NameHash& h) { return state.GetName(h); }, [&](uint32_t seq) { return state.NameOfSeq(seq); });
}

std::optional<NameHash> ResolveNameText(const std::string& text, const NameLookup& lookup, const SeqLookup& by_seq, std::string& error)
{
    const auto fail{[&](std::string why) -> std::optional<NameHash> {
        error = std::move(why);
        return std::nullopt;
    }};
    if (text.empty()) return fail("The name is empty");
    if (text.size() == 66 && text.starts_with("0x")) {
        if (const auto hash{uint256::FromHex(text.substr(2))}) return *hash;
    }
    // A number has a form of its own: "1739-0029" is a name like any other text.
    if (text.starts_with('#')) {
        const auto seq{ParseSeq(text.substr(1))};
        if (!seq) return fail(strprintf("A number of a name is # and two groups of four digits, such as #1739-0029: %s is none", text));
        if (!by_seq) return fail("Names are not given by number here");
        if (const auto name{by_seq(*seq)}) return *name;
        return fail(strprintf("No name has the number %s", text));
    }
    // DNS looks names up in lower case: Core.x is core.x. Another name, of the hash of "Core", is
    // not what is meant.
    if (std::any_of(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
        return fail(strprintf("Names are lower case, as DNS looks them up: %s. A name registered by the hash of another text is given by its hash (\"0x\" and 64 hex digits)", ToLower(text)));
    }
    // Only the characters names are made of: anything else (a look-alike letter of another script,
    // a space) is the hash of a text that only looks like the name meant, which anyone can register.
    const std::vector<std::string> labels{util::SplitString(text, '.')};
    if (!std::all_of(labels.begin(), labels.end(), IsGoodNameText)) {
        return fail(strprintf("A name is lower-case letters (a-z), digits, - and _, its labels separated by dots: %s is not. A name registered by the hash of another text is given by its hash (\"0x\" and 64 hex digits)", text));
    }
    if (labels.size() == 1) return HashName(text);
    // An ICANN name has dots: the authority vouches for its text. It goes before a subname of the same
    // text, which the holder of the name of its last label ("com") could make to stand for any ICANN
    // name under it.
    if (const auto record{lookup(HashName(text))}; record && record->icann && record->icann_name == text) return HashName(text);
    // A subname: down from the last label, each step a subname of the one above (not a top-level
    // name registered by its hash).
    NameHash at{HashName(labels.back())};
    bool found{lookup(at).has_value()};
    for (size_t i{labels.size() - 1}; found && i-- > 0;) {
        const NameHash parent{at};
        at = SubnameHash(at, labels[i]);
        const auto record{lookup(at)};
        found = record && record->parent == parent && record->label == labels[i];
    }
    if (found) return at;
    // Never the name whose hash is that of the dotted text: anyone can register a name by any hash,
    // and what is sent to "pay.shop" would go to it.
    return fail(strprintf("No such subname: %s (a name with dots is a subname, found from its last label down; a name registered by the hash of a text with dots is given by its hash)", text));
}

NameHash ParseNameArg(const UniValue& value, const NameLookup& lookup, const SeqLookup& by_seq)
{
    std::string error;
    const auto name{ResolveNameText(value.get_str(), lookup, by_seq, error)};
    if (!name) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    return *name;
}

bool IsNameTextForm(const std::string& text)
{
    if (text.size() == 66 && text.starts_with("0x") && uint256::FromHex(text.substr(2))) return true;
    const std::vector<std::string> labels{util::SplitString(text, '.')};
    return !text.empty() && std::all_of(labels.begin(), labels.end(), IsGoodNameText);
}

} // namespace bitnames
