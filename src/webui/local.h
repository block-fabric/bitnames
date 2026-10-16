// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_LOCAL_H
#define BITCOIN_WEBUI_LOCAL_H

#include <univalue.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/guard.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class uint256;
namespace interfaces {
class Chain;
class Node;
} // namespace interfaces

/**
 * What a chain adds to the host for its own pages (the pages of src/webui/app/src/chain/local.ts), in
 * local.cpp, the one host file a chain made from the template changes: the kit itself stays as it is, so
 * merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
 */
namespace webui::local {

/** RPC methods the chain's pages call, added to the page's allowlist (rpcpolicy.cpp). */
std::map<std::string, Bridge::Thread> RpcMethods();

/** Of these, the wallet methods that need the keys (they go through wallet.withKeys on an encrypted wallet). */
std::set<std::string> KeyedRpcMethods();

/** The node, while it runs (throws a HostError before); the host methods take it per call. */
using NodeFn = std::function<interfaces::Node&()>;

/**
 * Host methods of the chain's pages ("<chain>.something"), registered after the kit's. `confirm` asks the
 * user on the desktop (Window::Confirm, outside the page): what a page must not do on its own word, such as
 * showing a key, asks there first and answers BRIDGE_CANCELLED on a no (audit port-2 #3).
 */
void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm);

/**
 * BitNames: the store of what the chain does not keep (bitnames.store.load, bitnames.store.save): the names the
 * app is registering (the text of a name a reservation hides, its next step), a JSON object in a file of the
 * data directory of the network, written whole and flushed before it replaces the old one.
 */
inline constexpr const char* STORE_FILE{"bitnames-app.json"};
inline constexpr size_t MAX_STORE_BYTES{4 << 20};
/** The object of a store; null when there is none (or it could not be read: then kept aside as <file>.unreadable). */
UniValue ReadStore(const fs::path& path);
/** Write a store (an object). Throws HostError. */
void WriteStore(const fs::path& path, const UniValue& value);

/**
 * DNS records given as rows (setnamerecords), checked as the wallet checks them: {"ok": true, "bytes": n} with
 * the size of the records on the chain, or {"ok": false, "error": why}.
 */
UniValue CheckRecordRows(const UniValue& rows);
/** Whether the rows of these records (the JSON of a name's data) give them back, so that an editor of rows may save them. */
bool RecordsEditable(const UniValue& records);

/**
 * What each output of the coinbase of a block of the wallet pays, by the chain's own words (a short id,
 * "" for an output it has nothing to say of): the history shows such an output by its role rather than
 * "Mined" (chain/roles.ts, COINBASE_ROLES). The template has none: empty.
 */
std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block);

} // namespace webui::local

#endif // BITCOIN_WEBUI_LOCAL_H
