// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitNames by LayerTwo Labs (plain-bitnames).

#include <bitnames/dns.h>
#include <bitnames/message.h>
#include <bitnames/rpcutil.h>
#include <core_io.h>
#include <bitnames/state.h>
#include <coins.h>
#include <key_io.h>
#include <node/context.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <script/solver.h>
#include <util/strencodings.h>
#include <validation.h>

#include <univalue.h>

using node::NodeContext;

namespace {

/** Where a name is: the output that carries it, and the address of its owner. */
UniValue OwnerToJSON(Chainstate& chainstate, const bitnames::State& state, const bitnames::NameHash& name) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    UniValue obj(UniValue::VOBJ);
    for (const COutPoint& outpoint : state.TokensOf(name)) {
        obj.pushKV("txid", outpoint.hash.GetHex());
        obj.pushKV("vout", outpoint.n);
        if (const auto coin{chainstate.CoinsTip().GetCoin(outpoint)}) {
            CTxDestination dest;
            if (ExtractDestination(coin->out.scriptPubKey, dest)) obj.pushKV("address", EncodeDestination(dest));
        }
        break;
    }
    return obj;
}

UniValue NameToJSON(Chainstate& chainstate, const bitnames::State& state, const bitnames::NameHash& name, const bitnames::NameRecord& record,
                    std::optional<int> height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("namehash", name.GetHex());
    if (const std::string text{bitnames::FullText(state, record)}; !text.empty()) obj.pushKV("name", text);
    if (record.parent) {
        obj.pushKV("parent", record.parent->GetHex());
        obj.pushKV("label", record.label);
    }
    obj.pushKV("seq", bitnames::FormatSeq(record.seq));
    obj.pushKV("registered", record.height);
    obj.pushKV("registration", record.registration.GetHex());
    if (record.icann) obj.pushKV("icann", record.icann_name);
    const auto data{height ? state.DataAtHeight(record, *height) : std::optional{record.Current()}};
    if (data) obj.pushKV("data", bitnames::DataToJSON(*data));
    obj.pushKV("owner", OwnerToJSON(chainstate, state, name));
    return obj;
}

std::vector<RPCResult> NameResults()
{
    return {
        {RPCResult::Type::STR_HEX, "namehash", "The hash of the name, by which the chain knows it"},
        {RPCResult::Type::STR, "name", /*optional=*/true, "The text of the name (blog.core for a subname), if it and its parents are public, and of lower-case letters, digits, - and _"},
        {RPCResult::Type::STR_HEX, "parent", /*optional=*/true, "For a subname: the hash of its parent"},
        {RPCResult::Type::STR, "label", /*optional=*/true, "For a subname: its label"},
        {RPCResult::Type::STR, "seq", "Its number, in the order of registration (given to commands with # in front: #1739-0029)"},
        {RPCResult::Type::NUM, "registered", "The height of the block that registered it"},
        {RPCResult::Type::STR_HEX, "registration", "The transaction that registered it"},
        {RPCResult::Type::STR, "icann", /*optional=*/true, "The ICANN domain name it was marked as"},
        {RPCResult::Type::OBJ, "data", /*optional=*/true, "Its data; left out at a height before its registration", bitnames::DataResults()},
        {RPCResult::Type::OBJ, "owner", "The output that carries it",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM, "vout", ""},
            {RPCResult::Type::STR, "address", /*optional=*/true, "The address of the owner"},
        }},
    };
}

RPCMethod getname()
{
    return RPCMethod{
        "getname",
        "A registered name: its data, now or at a past height, and its owner.",
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, the hash of the name (\"0x\" and 64 hex digits), or its number (\"#1739-0029\")"},
            {"height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The height to give the data at; now if left out"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", NameResults()},
        RPCExamples{HelpExampleCli("getname", "\"satoshi\"") + HelpExampleCli("getname", "\"#1739-0029\" 1200")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const bitnames::State state{chainstate.SideState().BitNames()};
    const bitnames::NameHash name{bitnames::ParseNameArg(request.params[0], state)};
    const auto found{state.GetName(name)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such name is registered");
    const std::optional<int> height{request.params[1].isNull() ? std::nullopt : std::optional<int>{request.params[1].getInt<int>()}};
    return NameToJSON(chainstate, state, name, *found, height);
},
    };
}

RPCMethod listnames()
{
    return RPCMethod{
        "listnames",
        "The registered names, in the order of registration. Without a filter, a page is read from the numbers alone (the newest\n"
        "from the end); with a parent, from its index of subnames; a match reads every name.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many"},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many to skip"},
            {"match", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only the public names that contain this text"},
            {"parent", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only the subnames of this name (the name, the hash of the name, or its number), one level down"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", NameResults()}}},
        RPCExamples{HelpExampleCli("listnames", "10 0") + HelpExampleCli("listnames", "100 0 null \"satoshi\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int count{request.params[0].isNull() ? 100 : request.params[0].getInt<int>()};
    const int skip{request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count or skip");
    LOCK(::cs_main);
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const bitnames::State state{chainstate.SideState().BitNames()};
    UniValue result(UniValue::VARR);
    const std::optional<std::string> match{request.params[2].isNull() ? std::nullopt : std::optional{ToLower(request.params[2].get_str())}};
    const std::optional<bitnames::NameHash> parent{request.params[3].isNull() ? std::nullopt : std::optional{bitnames::ParseNameArg(request.params[3], state)}};
    const auto wanted{[&](const bitnames::NameRecord& record) {
        if (parent && record.parent != parent) return false;
        return !match || ToLower(bitnames::FullText(state, record)).find(*match) != std::string::npos;
    }};
    if (parent) {
        // The subnames of a name, from its index of children (at most MAX_SUBNAMES), in the order of registration.
        std::vector<std::pair<uint32_t, std::pair<bitnames::NameHash, bitnames::NameRecord>>> children;
        for (const bitnames::NameHash& child : state.ChildrenOf(*parent)) {
            auto record{state.GetName(child)};
            if (record && wanted(*record)) children.push_back({record->seq, {child, std::move(*record)}});
        }
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i{static_cast<size_t>(skip)}; i < children.size() && result.size() < static_cast<size_t>(count); ++i) {
            result.push_back(NameToJSON(chainstate, state, children[i].second.first, children[i].second.second, std::nullopt));
        }
        return result;
    }
    if (match) {
        // A search reads every name (and the names above it, for its text): that is what was asked.
        int skipped{0};
        state.ForEachSeq(0, [&](uint32_t, const bitnames::NameHash& name) {
            if (result.size() >= static_cast<size_t>(count)) return false;
            const auto record{state.GetName(name)};
            if (!record || !wanted(*record)) return true;
            if (skipped++ < skip) return true;
            result.push_back(NameToJSON(chainstate, state, name, *record, std::nullopt));
            return true;
        });
        return result;
    }
    // No filter: the names are those of the numbers (released names leave theirs empty), so the ones
    // skipped are stepped over without reading them. Toward the end of the list (the newest names, as the
    // app shows them) the numbers are walked from the end: in windows back from NextSeq, each twice as
    // large, so that only the newest are read.
    const uint64_t total{state.NameCount()};
    if (static_cast<uint64_t>(skip) >= total || count == 0) return result;
    const uint64_t tail{total - skip};
    std::vector<bitnames::NameHash> page;
    if (tail <= static_cast<uint64_t>(skip)) {
        const uint32_t next{state.NextSeq()};
        std::vector<bitnames::NameHash> last;
        for (uint64_t window{std::max<uint64_t>(tail, 16)};; window *= 2) {
            const uint32_t from{window >= next ? 0 : static_cast<uint32_t>(next - window)};
            last.clear();
            state.ForEachSeq(from, [&](uint32_t, const bitnames::NameHash& name) {
                last.push_back(name);
                return true;
            });
            if (last.size() >= tail || from == 0) break;
        }
        // The names from position `skip` on are the last `tail` of them.
        const size_t first{last.size() > tail ? last.size() - static_cast<size_t>(tail) : 0};
        for (size_t i{first}; i < last.size() && page.size() < static_cast<size_t>(count); ++i) page.push_back(last[i]);
    } else {
        int skipped{0};
        state.ForEachSeq(0, [&](uint32_t, const bitnames::NameHash& name) {
            if (page.size() >= static_cast<size_t>(count)) return false;
            if (skipped++ < skip) return true;
            page.push_back(name);
            return true;
        });
    }
    for (const bitnames::NameHash& name : page) {
        if (const auto record{state.GetName(name)}) result.push_back(NameToJSON(chainstate, state, name, *record, std::nullopt));
    }
    return result;
},
    };
}

RPCMethod getnamehistory()
{
    return RPCMethod{
        "getnamehistory",
        "Every value each field of the data of a name has had, with the transaction and height that set it.",
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, the hash of the name, or its number (\"#1739-0029\")"},
        },
        RPCResult{RPCResult::Type::OBJ_DYN, "", "Per field, oldest first",
        {
            {RPCResult::Type::ARR, "field", "", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::ANY, "value", "The value; null where it was deleted"},
                {RPCResult::Type::STR_HEX, "txid", ""},
                {RPCResult::Type::NUM, "height", ""},
            }}}},
        }},
        RPCExamples{HelpExampleCli("getnamehistory", "\"satoshi\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitnames::State state{chainman.ActiveChainstate().SideState().BitNames()};
    const bitnames::NameHash name{bitnames::ParseNameArg(request.params[0], state)};
    const auto found{state.GetName(name)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such name is registered");
    const bitnames::NameHistory record{state.History(*found)};
    UniValue result(UniValue::VOBJ);
    const auto add{[&](const std::string& field, const auto& history, const auto& format) {
        UniValue entries(UniValue::VARR);
        for (const auto& entry : history) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("value", entry.value ? format(*entry.value) : UniValue{});
            obj.pushKV("txid", entry.txid.GetHex());
            obj.pushKV("height", entry.height);
            entries.push_back(std::move(obj));
        }
        result.pushKV(field, std::move(entries));
    }};
    add("commitment", record.commitment, [](const uint256& v) { return UniValue{v.GetHex()}; });
    add("ipv4", record.ipv4, [](const bitnames::SocketV4& v) { return UniValue{bitnames::FormatSocket(v)}; });
    add("ipv6", record.ipv6, [](const bitnames::SocketV6& v) { return UniValue{bitnames::FormatSocket(v)}; });
    add("encryptionkey", record.encryption_key, [](const bitnames::EncryptionKey& v) { return UniValue{HexStr(v)}; });
    add("signingkey", record.signing_key, [](const bitnames::SigningKey& v) { return UniValue{HexStr(v)}; });
    add("paymailfee", record.paymail_fee, [](const CAmount& v) { return ValueFromAmount(v); });
    add("records", record.records, [](const std::string& v) {
        UniValue records;
        return records.read(v) ? records : UniValue{v};
    });
    return result;
},
    };
}

RPCMethod getbitnamesinfo()
{
    return RPCMethod{
        "getbitnamesinfo",
        "How many names are registered and reserved.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "names", "Names registered"},
            {RPCResult::Type::NUM, "reservations", "Reservations not revealed yet"},
            {RPCResult::Type::BOOL, "icann", "Whether this network has an authority that marks ICANN names"},
            {RPCResult::Type::NUM, "revealdepth", "How many blocks deep a reservation has to be for a registration in the next block to reveal it"},
            {RPCResult::Type::STR, "dns", /*optional=*/true, "Where this node answers DNS questions for .x (-dnsbind), if it does"},
        }},
        RPCExamples{HelpExampleCli("getbitnamesinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitnames::State state{chainman.ActiveChainstate().SideState().BitNames()};
    UniValue result(UniValue::VOBJ);
    result.pushKV("names", state.NameCount());
    result.pushKV("reservations", state.ReservationCount());
    const Consensus::BitNamesParams& params{chainman.GetConsensus().sidechain.bitnames};
    result.pushKV("icann", !params.icann_authority.IsNull());
    result.pushKV("revealdepth", params.reveal_depth);
    if (const auto dns{bitnames::DnsServerAddress()}) result.pushKV("dns", *dns);
    return result;
},
    };
}

RPCMethod verifynamesignature()
{
    return RPCMethod{
        "verifynamesignature",
        "Check that the owner of a name signed a message (see signasname), with the signing key the name has now.",
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, its hash, or its number"},
            {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The text"},
            {"signature", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The signature"},
        },
        RPCResult{RPCResult::Type::BOOL, "", "Whether the signature is good"},
        RPCExamples{HelpExampleCli("verifynamesignature", "\"satoshi\" \"hello\" \"signature\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitnames::State state{chainman.ActiveChainstate().SideState().BitNames()};
    const bitnames::NameHash name{bitnames::ParseNameArg(request.params[0], state)};
    const auto found{state.GetName(name)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such name is registered");
    const auto key{found->Current().signing_key};
    if (!key) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name has no signing key");
    const std::string text{request.params[1].get_str()};
    const std::vector<unsigned char> signature{ParseHex(request.params[2].get_str())};
    if (signature.size() != 64) return false;
    return XOnlyPubKey{*key}.VerifySchnorr(bitnames::MessageSigHash(std::span{UCharCast(text.data()), text.size()}), signature);
},
    };
}

RPCMethod getnamerecords()
{
    return RPCMethod{
        "getnamerecords",
        "The DNS records of a name or subname, one row per record (see setnamerecords).",
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, a subname (blog.core), its hash or its number"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "host", "@ for the name itself, or the labels under it (www, a.b, *)"},
                {RPCResult::Type::STR, "type", "A, AAAA, CNAME, TXT, MX, SRV, NS, TLSA, or DELEGATE (the records of another name stand for this host)"},
                {RPCResult::Type::STR, "value", ""},
                {RPCResult::Type::NUM, "priority", /*optional=*/true, "For MX and SRV"},
                {RPCResult::Type::NUM, "weight", /*optional=*/true, "For SRV"},
                {RPCResult::Type::NUM, "port", /*optional=*/true, "For SRV"},
            }},
        }},
        RPCExamples{HelpExampleCli("getnamerecords", "\"core\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitnames::State state{chainman.ActiveChainstate().SideState().BitNames()};
    const auto found{state.GetName(bitnames::ParseNameArg(request.params[0], state))};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such name is registered");
    return bitnames::RowsFromRecords(found->Current().records.value_or(""));
},
    };
}

RPCMethod resolvename()
{
    return RPCMethod{
        "resolvename",
        "What the DNS server of the names (-dnsbind) answers for a domain name under ." + bitnames::DNS_TLD + ".",
        {
            {"domain", RPCArg::Type::STR, RPCArg::Optional::NO, "The domain name, such as www.core.x"},
            {"type", RPCArg::Type::STR, RPCArg::Default{"ANY"}, "The record type: A, AAAA, CNAME, TXT, MX, SRV, TLSA, NS or ANY"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "status", "NOERROR, NXDOMAIN (no such name), SERVFAIL (a delegation that leads nowhere) or REFUSED (not under ." + bitnames::DNS_TLD + ")"},
            {RPCResult::Type::ARR, "records", "", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", ""},
                {RPCResult::Type::STR, "type", ""},
                {RPCResult::Type::STR, "data", ""},
            }}}},
        }},
        RPCExamples{HelpExampleCli("resolvename", "\"www.core.x\" A")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const std::string type_name{request.params[1].isNull() ? "ANY" : request.params[1].get_str()};
    const auto type{bitnames::dns::ParseType(type_name)};
    if (!type) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown record type");
    LOCK(::cs_main);
    const auto answer{bitnames::Resolve(chainman.ActiveChainstate().SideState().BitNames(), request.params[0].get_str(), *type)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("status", answer.rcode == bitnames::dns::NOERROR ? "NOERROR" : answer.rcode == bitnames::dns::NXDOMAIN ? "NXDOMAIN" : answer.rcode == bitnames::dns::SERVFAIL ? "SERVFAIL" : "REFUSED");
    UniValue records(UniValue::VARR);
    for (const auto& record : answer.records) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("name", record.name);
        obj.pushKV("type", bitnames::dns::TypeName(record.type));
        obj.pushKV("data", record.text);
        records.push_back(std::move(obj));
    }
    result.pushKV("records", std::move(records));
    return result;
},
    };
}

} // namespace

void RegisterBitNamesRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"bitnames", &getname},
        {"bitnames", &listnames},
        {"bitnames", &getnamehistory},
        {"bitnames", &getbitnamesinfo},
        {"bitnames", &verifynamesignature},
        {"bitnames", &resolvename},
        {"bitnames", &getnamerecords},
    };
    for (const auto& c : commands) t.appendCommand(c.name, &c);
}
