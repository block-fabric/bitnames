// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of BitNames (src/webui/app/src/chain/local.ts): the RPC methods of the names they
// call (src/rpc/bitnames.cpp, src/wallet/rpc/bitnames.cpp), the checks of DNS records the wallet makes (so that
// the record editor says what the chain would refuse before anything is sent), and the store of what the chain
// does not keep (the names being registered), in a file of the data directory.

#include <webui/local.h>

#include <bitnames/rpcutil.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace webui::local {

namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

/** Where a store is kept: in the data directory of the network, apart from the settings of the app. */
fs::path StorePath(interfaces::Node& node, const std::string& name)
{
    node::NodeContext* context{node.context()};
    if (!context || !context->args) throw HostError("The node is still starting");
    return context->args->GetDataDirNet() / fs::u8path(name);
}

/** The message of an error of an RPC helper (a JSONRPCError is a UniValue object). */
std::string ErrorMessage(const UniValue& error)
{
    const UniValue& message{error.isObject() ? error.find_value("message") : NullUniValue};
    return message.isStr() ? message.get_str() : std::string{"The records are not valid"};
}

/** At most this many rows are checked at once: a name holds 1024 bytes of records. */
constexpr size_t MAX_CHECKED_ROWS{256};
} // namespace

UniValue ReadStore(const fs::path& path)
{
    std::ostringstream text;
    {
        // Closed before anything else: on Windows an open file cannot be renamed.
        std::ifstream file{path.std_path(), std::ios::binary};
        if (!file) return UniValue{};
        text << file.rdbuf();
    }
    UniValue value;
    if (!value.read(text.str()) || !value.isObject()) {
        // Kept aside rather than lost: the names a reservation hides, which only the user may still know.
        std::error_code ec;
        fs::rename(path, fs::PathFromString(fs::PathToString(path) + ".unreadable"), ec);
        return UniValue{};
    }
    return value;
}

void WriteStore(const fs::path& path, const UniValue& value)
{
    if (!value.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "A store is an object");
    const std::string text{value.write()};
    if (text.size() > MAX_STORE_BYTES) throw HostError(strprintf("The record is too large (%u bytes; at most %u)", text.size(), MAX_STORE_BYTES));
    // Written whole to another file, flushed to the disk, then put in place: a crash leaves the old one or the new one.
    const fs::path temp{fs::PathFromString(fs::PathToString(path) + ".new")};
    FILE* file{fsbridge::fopen(temp, "wb")};
    if (!file) throw HostError(strprintf("%s cannot be written", fs::PathToString(temp)));
    const bool written{std::fwrite(text.data(), 1, text.size(), file) == text.size() && FileCommit(file)};
    if (std::fclose(file) != 0 || !written || !RenameOver(temp, path)) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw HostError(strprintf("%s could not be written", fs::PathToString(path)));
    }
}

UniValue CheckRecordRows(const UniValue& rows)
{
    if (!rows.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, "The rows are a list");
    if (rows.size() > MAX_CHECKED_ROWS) throw HostError(BRIDGE_INVALID_REQUEST, "Too many rows");
    UniValue out{UniValue::VOBJ};
    try {
        const std::string records{bitnames::RecordsFromRows(rows)};
        out.pushKV("ok", true);
        out.pushKV("bytes", records.size());
    } catch (const UniValue& error) {
        out.pushKV("ok", false);
        out.pushKV("error", ErrorMessage(error));
    } catch (const std::exception& e) {
        // A field of the wrong type (a number given as an object, say): what the wallet would refuse too.
        out.pushKV("ok", false);
        out.pushKV("error", e.what());
    }
    return out;
}

bool RecordsEditable(const UniValue& records)
{
    if (records.isNull()) return true;
    if (!records.isObject()) return false;
    return bitnames::RowsKeepRecords(records.write());
}

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // What the node says of the names: read only.
        {"getbitnamesinfo", Bridge::Thread::WORKER},
        {"getname", Bridge::Thread::WORKER},
        {"listnames", Bridge::Thread::WORKER},
        {"getnamehistory", Bridge::Thread::WORKER},
        {"getnamerecords", Bridge::Thread::WORKER},
        {"resolvename", Bridge::Thread::WORKER},
        {"verifynamesignature", Bridge::Thread::WORKER},
        // What the wallet holds, and the blocks its transactions of names are in.
        {"listmynames", Bridge::Thread::WORKER},
        {"gettransaction", Bridge::Thread::WORKER},
        {"encryptnamemessage", Bridge::Thread::WORKER},
        // What the wallet does with its keys: keys it makes, messages it opens (the keys, below); what it signs is confirmed on
        // the desktop (ConfirmedRpcMethods).
        {"getnewnamekeys", Bridge::Thread::WORKER},
        {"decryptnamemessage", Bridge::Thread::WORKER},
        {"listpaymail", Bridge::Thread::WORKER},
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // Each needs the wallet unlocked: it makes keys or opens a message (the passphrase asked by the host).
    return {"getnewnamekeys", "decryptnamemessage", "listpaymail"};
}

namespace {
/** The second step of reservename: registername of the same name, [name, data, public]. */
confirm::Reveal RegisterAfterReserve()
{
    confirm::Reveal reveal;
    reveal.method = "registername";
    reveal.title = "Get a name";
    reveal.accept = "Reserve and register";
    reveal.first_label = "Reservation";
    reveal.second_label = "Registration";
    reveal.what = "Two transactions: the reservation hides the name until it is deep enough, so that nobody who sees it coming can "
                  "take it first; the registration then makes it yours, with the data below.";
    reveal.matches = confirm::SameFirstParam;
    // A registration spends the reservation and makes the name's output and the change; its data adds to it.
    reveal.second_vsize = [](const UniValue& params) { return int64_t{300} + (params.size() > 1 ? static_cast<int64_t>(params[1].write().size()) : 0); };
    reveal.when = confirm::AfterDepth("getbitnamesinfo", "revealdepth");
    return reveal;
}
} // namespace

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // What the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        // Reserved and registered in one question: the registration, exactly as shown, is sent by the app without asking
        // again once the reservation is deep enough (confirm::Reveal).
        {"reservename", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Reserve a name", "Reserve", "The wallet pays for a reservation of a name, which hides it until it is registered."), /*fee_unknown=*/true, 1, false, RegisterAfterReserve()}},
        {"registername", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Register a name", "Register", "The wallet registers a name it reserved, to the owner shown."), /*fee_unknown=*/true}},
        {"releasereservation", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release a reservation", "Release", "The wallet gives up a reservation of a name."), /*fee_unknown=*/true}},
        {"updatename", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Update a name", "Update", "The wallet signs a change of a name it owns."), /*fee_unknown=*/true}},
        {"setnamerecords", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Change the records of a name", "Change", "The wallet signs new records for a name it owns."), /*fee_unknown=*/true}},
        {"createsubname", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Create a subname", "Create", "The wallet creates a subname of a name it owns, owned by the address shown.", {"address"}, {}), /*fee_unknown=*/true}},
        {"transfername", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Transfer a name", "Transfer", "The wallet gives a name it owns to the address shown: it cannot be taken back.", {"address"}, {}), /*fee_unknown=*/true}},
        {"releasename", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release a name", "Release", "The wallet gives up a name it owns, and its subnames: anyone may register it then."), /*fee_unknown=*/true}},
        {"sendpaymail", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Send paymail", "Send", "The wallet pays and sends a message to a name.", {"name"}, {"amount"}), /*fee_unknown=*/true}},
        {"signasname", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Sign as a name", "Sign", "The wallet signs this message with the key of a name it owns."), /*fee_unknown=*/true}},
    };
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    (void)keys;
    // The names this app is registering, by wallet: the text a reservation hides and the step it is at, so that
    // the registration goes on after a restart (chains-qt kept them in its settings).
    bridge.RegisterHost("bitnames.store.load", [node](const UniValue&) {
        return ReadStore(StorePath(node(), STORE_FILE));
    }, Bridge::Thread::LONG);
    bridge.RegisterHost("bitnames.store.save", [node](const UniValue& params) {
        WriteStore(StorePath(node(), STORE_FILE), Param(params, "value"));
        return UniValue{true};
    }, Bridge::Thread::LONG);
    // The record editor: the rows checked as setnamerecords checks them, and whether rows can stand for the
    // records a name has (else saving rows would change records the editor does not show).
    bridge.RegisterHost("bitnames.records.check", [](const UniValue& params) {
        return CheckRecordRows(Param(params, "rows"));
    });
    bridge.RegisterHost("bitnames.records.editable", [](const UniValue& params) {
        return UniValue{RecordsEditable(Param(params, "records"))};
    });
    (void)confirm;
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local
