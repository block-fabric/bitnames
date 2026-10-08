// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitNames by LayerTwo Labs (plain-bitnames).

#include <addresstype.h>
#include <bitnames/message.h>
#include <bitnames/rpcutil.h>
#include <bitnames/state.h>
#include <core_io.h>
#include <hash.h>
#include <key_io.h>
#include <rpc/util.h>
#include <random.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {
namespace {

using bitnames::Marker;
using bitnames::NameHash;
using bitnames::Token;

/** The label the wallet gives the outputs of a name, so that it knows the text of its names. */
std::string NameLabel(const std::string& name) { return "bitname:" + name; }

std::optional<std::string> LabelOf(const CWallet& wallet, const CTxDestination& dest)
{
    LOCK(wallet.cs_wallet);
    const CAddressBookData* entry{wallet.FindAddressBookEntry(dest)};
    if (!entry || !entry->label) return std::nullopt;
    return *entry->label;
}

std::shared_ptr<CWallet> ReadyWallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return nullptr;
    pwallet->BlockUntilSyncedToCurrentChain();
    EnsureWalletIsUnlocked(*pwallet);
    return pwallet;
}

CTxDestination NewDestination(CWallet& wallet, const std::string& label)
{
    const auto dest{wallet.GetNewDestination(OutputType::BECH32, label)};
    if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
    return *dest;
}

/**
 * The nonce of a reservation: a signature by the key of the output that carries it, of the name,
 * hashed. The wallet can make it again from the reservation alone, so there is nothing to store,
 * and nobody else can.
 */
uint256 ReservationNonce(const CWallet& wallet, const CTxDestination& dest, const NameHash& name)
{
    const auto* keyhash{std::get_if<WitnessV0KeyHash>(&dest)};
    if (!keyhash) throw JSONRPCError(RPC_WALLET_ERROR, "A reservation of this wallet is carried by an address of a single key");
    std::string signature;
    const SigningResult result{wallet.SignMessage(strprintf("BitNames reservation %s", name.GetHex()), PKHash{ToKeyID(*keyhash)}, signature)};
    if (result != SigningResult::OK) throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Cannot make the nonce of the reservation: %s", SigningResultString(result)));
    return Hash(*Assert(DecodeBase64(signature)));
}

/** An output of the wallet, not spent, that carries a name or a reservation. */
struct OwnedToken {
    COutPoint outpoint;
    CTxDestination dest;
    Token token;
};

std::vector<OwnedToken> OwnedTokens(CWallet& wallet)
{
    std::vector<OwnedToken> owned;
    LOCK(wallet.cs_wallet);
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        const CTransactionRef wtx_tx{wtx.GetTx()};
        for (uint32_t n{0}; n < wtx_tx->vout.size(); ++n) {
            const CTxOut& out{wtx_tx->vout[n]};
            if (out.nValue != 0 || !wallet.IsMine(out)) continue;
            const COutPoint outpoint{txid, n};
            if (wallet.IsSpent(outpoint)) continue;
            const auto token{wallet.chain().getBitNamesToken(outpoint)};
            if (!token) continue;
            CTxDestination dest;
            ExtractDestination(out.scriptPubKey, dest);
            owned.push_back({outpoint, dest, *token});
        }
    }
    return owned;
}

std::optional<OwnedToken> OwnedName(CWallet& wallet, const NameHash& name)
{
    for (const OwnedToken& t : OwnedTokens(wallet)) {
        if (t.token.kind == Token::Kind::NAME && t.token.id == name) return t;
    }
    return std::nullopt;
}

/** Why this wallet cannot use a name now: a change to it is waiting for a block, or the wallet does not hold it. */
[[noreturn]] void NotHeld(CWallet& wallet, const NameHash& name)
{
    {
        LOCK(wallet.cs_wallet);
        for (const auto& [txid, wtx] : wallet.mapWallet) {
            if (wallet.GetTxDepthInMainChain(wtx) != 0 || wtx.isAbandoned()) continue;
            for (const CTxIn& in : wtx.GetTx()->vin) {
                const auto token{wallet.chain().getBitNamesToken(in.prevout)};
                if (token && token->kind == Token::Kind::NAME && token->id == name) {
                    throw JSONRPCError(RPC_WALLET_ERROR, strprintf("The last change to this name (transaction %s) is waiting for the next block: try again once it is in", txid.GetHex()));
                }
            }
        }
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, "This wallet does not hold this name");
}

/**
 * Make, sign and send a BitNames transaction: `names` and `reservations` are where the names and
 * reservations it carries go, in this order, followed by the marker, then other payments and the change.
 */
CTransactionRef SendBitNames(CWallet& wallet, Marker marker, const std::vector<CTxDestination>& names, const std::vector<CTxDestination>& reservations,
                             const std::vector<COutPoint>& inputs, const std::vector<CRecipient>& payments = {})
{
    std::vector<CRecipient> recipients;
    marker.name_outputs.clear();
    marker.reservation_outputs.clear();
    for (const CTxDestination& dest : names) {
        marker.name_outputs.push_back(recipients.size());
        recipients.push_back({dest, 0, /*fSubtractFeeFromAmount=*/false});
    }
    for (const CTxDestination& dest : reservations) {
        marker.reservation_outputs.push_back(recipients.size());
        recipients.push_back({dest, 0, /*fSubtractFeeFromAmount=*/false});
    }
    const size_t marker_index{recipients.size()};
    recipients.push_back({CNoDestination{}, 0, false});
    for (const CRecipient& payment : payments) recipients.push_back(payment);
    // The memos name outputs after the marker by their place among the payments.
    for (auto& [n, memo] : marker.memos) n += marker_index + 1;
    recipients[marker_index].dest = CNoDestination{bitnames::MarkerScript(marker)};

    CCoinControl coin_control;
    for (const COutPoint& input : inputs) coin_control.Select(input);
    // The change goes last, so that the outputs keep the places the marker gives them.
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/recipients.size(), coin_control, /*sign=*/true)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    const CTransactionRef tx{res->tx};
    wallet.CommitTransaction(tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    if (!wallet.chain().isInMempool(tx->GetHash())) {
        wallet.AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The transaction was not accepted into the mempool (see the debug log for why)");
    }
    return tx;
}

/** The private key of a public key of this wallet. */
std::optional<CKey> PrivateKey(const CWallet& wallet, const CPubKey& pubkey)
{
    const CScript script{GetScriptForDestination(WitnessV0KeyHash{pubkey})};
    for (ScriptPubKeyMan* spkm : wallet.GetScriptPubKeyMans(script)) {
        const auto* desc{dynamic_cast<DescriptorScriptPubKeyMan*>(spkm)};
        if (!desc) continue;
        const auto provider{desc->GetSigningProvider(pubkey)};
        CKey key;
        if (provider && provider->GetKey(pubkey.GetID(), key)) return key;
    }
    return std::nullopt;
}

/** The public key of a new address of this wallet, kept under a label so that it is not used for payments. */
CPubKey NewKey(CWallet& wallet, const std::string& label)
{
    const CTxDestination dest{NewDestination(wallet, label)};
    const CScript script{GetScriptForDestination(dest)};
    const auto provider{wallet.GetSolvingProvider(script)};
    CPubKey pubkey;
    if (!provider || !provider->GetPubKey(ToKeyID(std::get<WitnessV0KeyHash>(dest)), pubkey)) throw JSONRPCError(RPC_WALLET_ERROR, "Cannot get the key of a new address");
    return pubkey;
}

/**
 * The reservation of this wallet for a name: the one whose commitment the nonce of its key opens, bound
 * to the script of its output (from audit2_height) or, made before, to the name alone.
 */
std::optional<std::pair<COutPoint, uint256>> FindReservation(CWallet& wallet, const NameHash& hash)
{
    for (const auto& owned : OwnedTokens(wallet)) {
        if (owned.token.kind != Token::Kind::RESERVATION || !std::holds_alternative<WitnessV0KeyHash>(owned.dest)) continue;
        const uint256 nonce{ReservationNonce(wallet, owned.dest, hash)};
        if (bitnames::ReservationCommitment(hash, nonce, GetScriptForDestination(owned.dest)) == owned.token.commitment ||
            bitnames::ReservationCommitment(hash, nonce) == owned.token.commitment) {
            return std::make_pair(owned.outpoint, nonce);
        }
    }
    return std::nullopt;
}

/** The current data of a registered name. */
bitnames::NameData CurrentData(CWallet& wallet, const NameHash& name)
{
    const auto record{wallet.chain().getBitName(name)};
    if (!record) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such name is registered");
    return record->Current();
}

/** A name given to a command of the wallet: by text (a subname by its dotted text), hash, or number (#1739-0029), as the node's commands read it. */
NameHash ParseName(CWallet& wallet, const UniValue& value)
{
    return bitnames::ParseNameArg(value, [&](const NameHash& h) { return wallet.chain().getBitName(h); }, [&](uint32_t seq) { return wallet.chain().getBitNameOfSeq(seq); });
}

/** The text of a name this wallet reserves or registers: what DNS can look up, and what is shown as it is. */
void CheckNewName(const std::string& name)
{
    if (name.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name is empty");
    // A dot makes a name DNS cannot reach: blog.core.x is looked for under core. What is meant is a subname.
    if (name.find('.') != std::string::npos) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A name has no dots: for %s, make a subname of %s with createsubname", name, name.substr(name.find('.') + 1)));
    }
    if (ToLower(name) != name) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Names are lower case, as DNS looks them up: %s", ToLower(name)));
    if (!bitnames::IsGoodNameText(name)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A name is 1 to %u lower-case letters, digits, - and _", bitnames::MAX_NAME_TEXT_SIZE));
    }
}

} // namespace

RPCMethod reservename()
{
    return RPCMethod{
        "reservename",
        "Reserve a name: the first of two steps to take it. The reservation commits to the name without showing it,\n"
        "so that nobody can see it coming and take it first; registername takes it once the reservation is in a block." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "namehash", "The hash of the name, by which the chain knows it"},
        }},
        RPCExamples{HelpExampleCli("reservename", "\"satoshi\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const std::string name{request.params[0].get_str()};
    CheckNewName(name);
    const NameHash hash{bitnames::HashName(name)};
    if (pwallet->chain().getBitName(hash)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name is taken");
    const CTxDestination dest{NewDestination(*pwallet, NameLabel(name))};
    const uint256 nonce{ReservationNonce(*pwallet, dest, hash)};
    // From audit2_height, the commitment binds the reservation to the script of its output: a copy of
    // it, made by someone who saw it, can never reveal the name.
    const auto [params, next_height]{pwallet->chain().getBitNamesParams()};
    Marker marker;
    marker.operation = bitnames::Reserve{next_height >= params.audit2_height ? bitnames::ReservationCommitment(hash, nonce, GetScriptForDestination(dest))
                                                                             : bitnames::ReservationCommitment(hash, nonce)};
    const CTransactionRef tx{SendBitNames(*pwallet, marker, {}, std::vector<CTxDestination>{dest}, {})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("namehash", hash.GetHex());
    return result;
},
    };
}

RPCMethod registername()
{
    return RPCMethod{
        "registername",
        "Take a name reserved with reservename, once the reservation is deep enough in the chain (revealdepth in\n"
        "getbitnamesinfo: so that whoever makes a block cannot see the name revealed and take it in that block)." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name"},
            {"data", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "The data of the name", bitnames::DataArgs()},
            {"public", RPCArg::Type::BOOL, RPCArg::Default{false}, "Publish the text of the name, so that it can be listed and searched for (as all Namecoin names are)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "namehash", "The hash of the name"},
        }},
        RPCExamples{HelpExampleCli("registername", "\"satoshi\" '{\"ipv4\": \"203.0.113.7:443\"}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const std::string name{request.params[0].get_str()};
    CheckNewName(name);
    const NameHash hash{bitnames::HashName(name)};
    if (pwallet->chain().getBitName(hash)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name is taken");
    const auto reservation{FindReservation(*pwallet, hash)};
    if (!reservation) throw JSONRPCError(RPC_INVALID_PARAMETER, "This wallet has no reservation of the name in a block (see reservename)");
    // Revealed only once the reservation is deep enough (see getbitnamesinfo).
    if (const auto token{pwallet->chain().getBitNamesToken(reservation->first)}) {
        const auto [params, next_height]{pwallet->chain().getBitNamesParams()};
        const auto reserved_at{pwallet->chain().getBitNamesReservationHeight(Txid::FromUint256(token->id))};
        if (reserved_at && next_height >= params.audit2_height && next_height - *reserved_at < params.reveal_depth) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The reservation is in block %d: it can be revealed from block %d, once it is %d blocks deep (the next block is %d)",
                                                                *reserved_at, *reserved_at + params.reveal_depth, params.reveal_depth, next_height));
        }
    }
    Marker marker;
    bitnames::Register reg;
    reg.name = hash;
    reg.nonce = reservation->second;
    if (!request.params[1].isNull()) reg.data = bitnames::ParseData(request.params[1]);
    if (!request.params[2].isNull() && request.params[2].get_bool()) reg.text = name;
    marker.operation = reg;
    const CTransactionRef tx{SendBitNames(*pwallet, marker, std::vector<CTxDestination>{NewDestination(*pwallet, NameLabel(name))}, {}, {reservation->first})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("namehash", hash.GetHex());
    return result;
},
    };
}

RPCMethod updatename()
{
    return RPCMethod{
        "updatename",
        "Change the data of a name of this wallet. A field left out is kept, a field set to null is deleted." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or the hash of the name"},
            {"data", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::NO, "The fields to change", bitnames::DataArgs()},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The transaction"},
        RPCExamples{HelpExampleCli("updatename", "\"satoshi\" '{\"ipv4\": null, \"paymailfee\": 0.0001}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const NameHash hash{ParseName(*pwallet, request.params[0])};
    const auto owned{OwnedName(*pwallet, hash)};
    if (!owned) NotHeld(*pwallet, hash);
    Marker marker;
    const bitnames::NameUpdates updates{bitnames::ParseUpdates(request.params[1])};
    if (updates.records.kind == bitnames::UpdateKind::SET) bitnames::CheckRecordsBesideSubnames(*updates.records.value, pwallet->chain().getBitNameSubnames(hash));
    marker.operation = bitnames::UpdateName{updates};
    const auto label{LabelOf(*pwallet, owned->dest)};
    const CTransactionRef tx{SendBitNames(*pwallet, marker, std::vector<CTxDestination>{NewDestination(*pwallet, label.value_or(""))}, {}, {owned->outpoint})};
    return tx->GetHash().GetHex();
},
    };
}

RPCMethod transfername()
{
    return RPCMethod{
        "transfername",
        "Give a name of this wallet to someone else: the address receives the output that carries it." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or the hash of the name"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the new owner"},
            {"clearkeys", RPCArg::Type::BOOL, RPCArg::Default{false}, "Delete the encryption key, the signing key and the paymail fee of the name in the same transaction: they are\n"
                "keys of this wallet, so messages to the name would still come here, and this wallet could still sign as it"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The transaction"},
        RPCExamples{HelpExampleCli("transfername", "\"satoshi\" \"address\"") + HelpExampleCli("transfername", "\"satoshi\" \"address\" true")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const NameHash hash{ParseName(*pwallet, request.params[0])};
    const CTxDestination dest{DecodeDestination(request.params[1].get_str())};
    if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    const auto owned{OwnedName(*pwallet, hash)};
    if (!owned) NotHeld(*pwallet, hash);
    Marker marker;
    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        // An update of the name, given on in the same transaction (the name updated is the one spent).
        const bitnames::NameData data{CurrentData(*pwallet, hash)};
        bitnames::NameUpdates updates;
        if (data.encryption_key) updates.encryption_key.kind = bitnames::UpdateKind::DELETE;
        if (data.signing_key) updates.signing_key.kind = bitnames::UpdateKind::DELETE;
        if (data.paymail_fee) updates.paymail_fee.kind = bitnames::UpdateKind::DELETE;
        if (!(updates == bitnames::NameUpdates{})) marker.operation = bitnames::UpdateName{updates};
    }
    const CTransactionRef tx{SendBitNames(*pwallet, marker, std::vector<CTxDestination>{dest}, {}, {owned->outpoint})};
    return tx->GetHash().GetHex();
},
    };
}

RPCMethod listmynames()
{
    return RPCMethod{
        "listmynames",
        "The names and reservations this wallet holds, in blocks.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "kind", "\"name\" or \"reservation\""},
                {RPCResult::Type::STR, "name", /*optional=*/true, "The text of the name, if this wallet reserved or registered it"},
                {RPCResult::Type::STR_HEX, "namehash", /*optional=*/true, "For a name: its hash"},
                {RPCResult::Type::STR, "seq", /*optional=*/true, "For a name: its number"},
                {RPCResult::Type::STR, "address", "The address that holds it"},
                {RPCResult::Type::STR_HEX, "txid", "The output that carries it"},
                {RPCResult::Type::NUM, "vout", ""},
            }},
        }},
        RPCExamples{HelpExampleCli("listmynames", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();
    UniValue result(UniValue::VARR);
    for (const OwnedToken& owned : OwnedTokens(*pwallet)) {
        UniValue obj(UniValue::VOBJ);
        const bool is_name{owned.token.kind == Token::Kind::NAME};
        obj.pushKV("kind", is_name ? "name" : "reservation");
        const auto label{LabelOf(*pwallet, owned.dest)};
        if (label && label->starts_with("bitname:")) obj.pushKV("name", label->substr(8));
        if (is_name) {
            obj.pushKV("namehash", owned.token.id.GetHex());
            if (const auto record{pwallet->chain().getBitName(owned.token.id)}) obj.pushKV("seq", bitnames::FormatSeq(record->seq));
        }
        obj.pushKV("address", EncodeDestination(owned.dest));
        obj.pushKV("txid", owned.outpoint.hash.GetHex());
        obj.pushKV("vout", owned.outpoint.n);
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod getnewnamekeys()
{
    return RPCMethod{
        "getnewnamekeys",
        "New keys of this wallet for a name: one that messages to the owner are encrypted to, one the owner signs with.\n"
        "Set them on a name with registername or updatename.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "encryptionkey", "A compressed public key"},
            {RPCResult::Type::STR_HEX, "signingkey", "An x-only public key"},
        }},
        RPCExamples{HelpExampleCli("getnewnamekeys", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    UniValue result(UniValue::VOBJ);
    result.pushKV("encryptionkey", HexStr(NewKey(*pwallet, "bitnames:encryption")));
    result.pushKV("signingkey", HexStr(XOnlyPubKey{NewKey(*pwallet, "bitnames:signing")}));
    return result;
},
    };
}

RPCMethod encryptnamemessage()
{
    return RPCMethod{
        "encryptnamemessage",
        "Encrypt a message to the owner of a name, with the encryption key of the name.",
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or the hash of the name"},
            {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The text"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The message, encrypted"},
        RPCExamples{HelpExampleCli("encryptnamemessage", "\"satoshi\" \"hello\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    const bitnames::NameData data{CurrentData(*pwallet, ParseName(*pwallet, request.params[0]))};
    if (!data.encryption_key) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name has no encryption key");
    const std::string text{request.params[1].get_str()};
    const auto message{bitnames::EncryptMessage(CPubKey{*data.encryption_key}, std::span{UCharCast(text.data()), text.size()})};
    if (!message) throw JSONRPCError(RPC_INVALID_PARAMETER, "The encryption key of the name is not valid");
    return HexStr(*message);
},
    };
}

RPCMethod decryptnamemessage()
{
    return RPCMethod{
        "decryptnamemessage",
        "Decrypt a message sent to one of the names of this wallet." + HELP_REQUIRING_PASSPHRASE,
        {
            {"message", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The message, encrypted"},
        },
        RPCResult{RPCResult::Type::STR, "", "The text"},
        RPCExamples{HelpExampleCli("decryptnamemessage", "\"hex\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const std::vector<unsigned char> message{ParseHex(request.params[0].get_str())};
    const auto recipient{bitnames::MessageRecipient(message)};
    if (!recipient) throw JSONRPCError(RPC_INVALID_PARAMETER, "This is not an encrypted message");
    const auto key{PrivateKey(*pwallet, *recipient)};
    if (!key) throw JSONRPCError(RPC_WALLET_ERROR, "The message is not to a key of this wallet");
    const auto text{bitnames::DecryptMessage(*key, message)};
    if (!text) throw JSONRPCError(RPC_INVALID_PARAMETER, "The message does not decrypt: it was changed");
    return std::string{text->begin(), text->end()};
},
    };
}

RPCMethod signasname()
{
    return RPCMethod{
        "signasname",
        "Sign a message as the owner of a name of this wallet, with the signing key of the name." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or the hash of the name"},
            {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The text"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The signature (BIP340)"},
        RPCExamples{HelpExampleCli("signasname", "\"satoshi\" \"hello\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const bitnames::NameData data{CurrentData(*pwallet, ParseName(*pwallet, request.params[0]))};
    if (!data.signing_key) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name has no signing key");
    // The key is the x coordinate: the wallet's key is one of the two points with it.
    std::optional<CKey> key;
    for (const unsigned char prefix : {0x02, 0x03}) {
        std::vector<unsigned char> bytes{prefix};
        bytes.insert(bytes.end(), data.signing_key->begin(), data.signing_key->end());
        key = PrivateKey(*pwallet, CPubKey{bytes});
        if (key) break;
    }
    if (!key) throw JSONRPCError(RPC_WALLET_ERROR, "The signing key of the name is not a key of this wallet");
    const std::string text{request.params[1].get_str()};
    std::vector<unsigned char> signature(64);
    if (!key->SignSchnorr(bitnames::MessageSigHash(std::span{UCharCast(text.data()), text.size()}), signature, nullptr, GetRandHash())) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Signing failed");
    }
    return HexStr(signature);
},
    };
}

RPCMethod sendpaymail()
{
    return RPCMethod{
        "sendpaymail",
        "Pay the owner of a name, with a message encrypted to the name. The payment has to be at least the paymail fee\n"
        "of the name for the owner to see the message." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or the hash of the name"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "What to pay"},
            {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The text"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The transaction"},
        RPCExamples{HelpExampleCli("sendpaymail", "\"satoshi\" 0.001 \"hello\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const NameHash name{ParseName(*pwallet, request.params[0])};
    const bitnames::NameData data{CurrentData(*pwallet, name)};
    if (!data.encryption_key) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name has no encryption key");
    if (!data.paymail_fee) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name takes no paymail (it has no paymail fee)");
    const CAmount amount{AmountFromValue(request.params[1])};
    if (amount < *data.paymail_fee) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The name takes paymail of %s at least", FormatMoney(*data.paymail_fee)));
    const auto owner{pwallet->chain().getBitNameOwner(name)};
    if (!owner) throw JSONRPCError(RPC_INVALID_PARAMETER, "The owner of the name is not known");
    const std::string text{request.params[2].get_str()};
    const auto message{bitnames::EncryptMessage(CPubKey{*data.encryption_key}, std::span{UCharCast(text.data()), text.size()})};
    if (!message || message->size() > bitnames::MAX_MEMO_SIZE) throw JSONRPCError(RPC_INVALID_PARAMETER, "The message is too long, or the key of the name is not valid");
    CTxDestination dest;
    if (!ExtractDestination(owner->second, dest)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The owner of the name cannot be paid");
    Marker marker;
    marker.memos.emplace_back(0, *message);
    const CTransactionRef tx{SendBitNames(*pwallet, marker, {}, {}, {}, {CRecipient{dest, amount, false}})};
    return tx->GetHash().GetHex();
},
    };
}

RPCMethod listpaymail()
{
    return RPCMethod{
        "listpaymail",
        "Paymail received by this wallet: payments with a message, decrypted. Only those that paid at least the paymail fee\n"
        "of a name of this wallet with the key the message is encrypted to, as the fee was when the payment was made, are\n"
        "listed: anyone can send a message with any payment." + HELP_REQUIRING_PASSPHRASE,
        {},
        RPCResult{RPCResult::Type::ARR, "", "Newest first",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", ""},
                {RPCResult::Type::NUM, "vout", ""},
                {RPCResult::Type::STR_AMOUNT, "amount", ""},
                {RPCResult::Type::NUM, "confirmations", ""},
                {RPCResult::Type::STR, "message", /*optional=*/true, "The text, if it decrypts"},
            }},
        }},
        RPCExamples{HelpExampleCli("listpaymail", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    std::vector<std::pair<int64_t, UniValue>> mail;
    // The names of this wallet: a message counts if it paid the fee of one with the key it is encrypted to.
    std::vector<NameHash> names;
    for (const OwnedToken& owned : OwnedTokens(*pwallet)) {
        if (owned.token.kind == Token::Kind::NAME) names.push_back(owned.token.id);
    }
    const int tip{pwallet->chain().getBitNamesParams().second - 1};
    const auto paid_enough{[&](const CPubKey& recipient, CAmount amount, int height) {
        for (const NameHash& name : names) {
            // As the sender saw it (the block before), or as the block left it.
            for (const int at : {height - 1, height}) {
                const auto data{pwallet->chain().getBitNameDataAtHeight(name, at)};
                if (!data || !data->encryption_key || !data->paymail_fee) continue;
                if (CPubKey{*data->encryption_key} == recipient && amount >= *data->paymail_fee) return true;
            }
        }
        return false;
    }};
    LOCK(pwallet->cs_wallet);
    for (const auto& [txid, wtx] : pwallet->mapWallet) {
        const CTransactionRef ptx{wtx.GetTx()};
        const auto marker{bitnames::GetMarker(*ptx)};
        if (!marker) continue;
        for (const auto& [n, memo] : marker->memos) {
            if (n >= ptx->vout.size() || !pwallet->IsMine(ptx->vout[n])) continue;
            const auto recipient{bitnames::MessageRecipient(memo)};
            if (!recipient) continue;
            const auto* confirmed{wtx.state<TxStateConfirmed>()};
            if (!paid_enough(*recipient, ptx->vout[n].nValue, confirmed ? confirmed->confirmed_block_height : tip)) continue;
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", txid.GetHex());
            obj.pushKV("vout", n);
            obj.pushKV("amount", ValueFromAmount(ptx->vout[n].nValue));
            obj.pushKV("confirmations", pwallet->GetTxDepthInMainChain(wtx));
            if (const auto key{PrivateKey(*pwallet, *recipient)}) {
                if (const auto text{bitnames::DecryptMessage(*key, memo)}) obj.pushKV("message", std::string{text->begin(), text->end()});
            }
            mail.emplace_back(wtx.nTimeReceived, std::move(obj));
        }
    }
    std::sort(mail.begin(), mail.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    UniValue result(UniValue::VARR);
    for (auto& [time, obj] : mail) result.push_back(std::move(obj));
    return result;
},
    };
}

} // namespace wallet

namespace wallet {

RPCMethod releasereservation()
{
    return RPCMethod{
        "releasereservation",
        "Give up a reservation of this wallet: the reservation is gone, and nothing is registered with it." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name the reservation is for"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The transaction"},
        RPCExamples{HelpExampleCli("releasereservation", "\"satoshi\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const auto reservation{FindReservation(*pwallet, bitnames::HashName(request.params[0].get_str()))};
    if (!reservation) throw JSONRPCError(RPC_INVALID_PARAMETER, "This wallet has no reservation of the name in a block");
    Marker marker;
    marker.operation = bitnames::Release{};
    return SendBitNames(*pwallet, marker, {}, {}, {reservation->first})->GetHash().GetHex();
},
    };
}

RPCMethod releasename()
{
    return RPCMethod{
        "releasename",
        "Give up a name of this wallet for good: it leaves the registry, with its data, and anyone can register it again.\n"
        "Its subnames are released with it, at any depth, whoever holds them: whoever registers the name next starts with\n"
        "none." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or its hash (\"0x\" and 64 hex digits)"},
            {"confirm", RPCArg::Type::BOOL, RPCArg::Optional::NO, "true: there is no undoing it"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The transaction"},
        RPCExamples{HelpExampleCli("releasename", "\"satoshi\" true")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    if (!request.params[1].get_bool()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Releasing a name cannot be undone: pass true to confirm");
    const NameHash hash{ParseName(*pwallet, request.params[0])};
    const auto owned{OwnedName(*pwallet, hash)};
    if (!owned) NotHeld(*pwallet, hash);
    Marker marker;
    marker.operation = bitnames::Release{};
    return SendBitNames(*pwallet, marker, {}, {}, {owned->outpoint})->GetHash().GetHex();
},
    };
}

RPCMethod createsubname()
{
    return RPCMethod{
        "createsubname",
        "Make a subname of a name of this wallet, such as blog under core (blog.core, blog.core.x in DNS), and give it to an\n"
        "address: this wallet's, or someone else's. Only its holder sets its data and records, gives it on, and makes\n"
        "subnames of it, and the holder of the parent cannot make another of the same label while it exists. But it\n"
        "depends on its parent, as a DNS subdomain on its domain: if the parent is released, it is released with it\n"
        "(whoever registers the parent next can make it again, for anyone), and a delegate in the records of the parent\n"
        "takes its domain elsewhere. A name has at most 1000 names under it, at most 8 levels deep." + HELP_REQUIRING_PASSPHRASE,
        {
            {"parent", RPCArg::Type::STR, RPCArg::Optional::NO, "The name of this wallet to make it under, or its hash"},
            {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label: lower-case letters, digits and hyphens"},
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"a new address of this wallet"}, "Who holds it"},
            {"data", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "Its data", bitnames::DataArgs()},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "namehash", "The hash the subname is known by"},
        }},
        RPCExamples{HelpExampleCli("createsubname", "\"core\" \"blog\" \"address\" '{\"ipv4\": \"203.0.113.9:443\"}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const NameHash parent{ParseName(*pwallet, request.params[0])};
    const std::string label{request.params[1].get_str()};
    if (!bitnames::IsDnsLabel(label)) throw JSONRPCError(RPC_INVALID_PARAMETER, "A label is 1 to 63 lower-case letters, digits and hyphens, not at either end");
    const NameHash child{bitnames::SubnameHash(parent, label)};
    if (const auto taken{pwallet->chain().getBitName(child)}) {
        // A top-level name with the hash of the subname is a squat, which gives way to it (from
        // audit_rules_height); a subname is taken.
        const auto [params, next_height]{pwallet->chain().getBitNamesParams()};
        if (taken->parent || next_height < params.audit_rules_height) throw JSONRPCError(RPC_INVALID_PARAMETER, "The subname exists");
    }
    const auto owned{OwnedName(*pwallet, parent)};
    if (!owned) NotHeld(*pwallet, parent);
    // The records of the parent that answer for the label would be left behind the subname, never seen.
    if (const auto record{pwallet->chain().getBitName(parent)}) {
        const auto hosts{bitnames::RecordHosts(record->Current().records.value_or(""))};
        if (std::find(hosts.begin(), hosts.end(), label) != hosts.end()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The records of the name answer for %s already: remove the host %s from them first", label, label));
        }
    }
    const auto parent_label{LabelOf(*pwallet, owned->dest)};
    const std::string parent_text{parent_label && parent_label->starts_with("bitname:") ? parent_label->substr(8) : "0x" + parent.GetHex()};
    CTxDestination holder;
    if (request.params[2].isNull() || request.params[2].get_str().empty()) {
        holder = NewDestination(*pwallet, NameLabel(label + "." + parent_text));
    } else {
        holder = DecodeDestination(request.params[2].get_str());
        if (!IsValidDestination(holder)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }
    bitnames::RegisterSubname sub;
    sub.label = label;
    if (!request.params[3].isNull()) sub.data = bitnames::ParseData(request.params[3]);
    Marker marker;
    marker.operation = sub;
    // The parent goes on to a new address of this wallet, the subname after it.
    const CTxDestination keep{NewDestination(*pwallet, parent_label.value_or(""))};
    const CTransactionRef tx{SendBitNames(*pwallet, marker, std::vector<CTxDestination>{keep, holder}, {}, {owned->outpoint})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("namehash", child.GetHex());
    return result;
},
    };
}

} // namespace wallet

namespace wallet {

RPCMethod setnamerecords()
{
    return RPCMethod{
        "setnamerecords",
        "Set the DNS records of a name or subname of this wallet, in place of those it has: what <name>.x and the hosts\n"
        "under it answer. An empty list removes them." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name, or a subname of this wallet (blog.core)"},
            {"rows", RPCArg::Type::ARR, RPCArg::Optional::NO, "The records, as rows",
            {
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                {
                    {"host", RPCArg::Type::STR, RPCArg::Default{"@"}, "@ for the name itself, or the labels under it: www, a.b, * for any"},
                    {"type", RPCArg::Type::STR, RPCArg::Optional::NO, "A, AAAA, CNAME, TXT, MX, SRV, NS, TLSA, or DELEGATE (the records of another name stand for this host)"},
                    {"value", RPCArg::Type::STR, RPCArg::Optional::NO, "An address, a host name (absolute if it ends with a dot, else under the name), a text, or for TLSA \"usage selector type hex\""},
                    {"priority", RPCArg::Type::NUM, RPCArg::Default{10}, "For MX and SRV"},
                    {"weight", RPCArg::Type::NUM, RPCArg::Default{0}, "For SRV"},
                    {"port", RPCArg::Type::NUM, RPCArg::Default{0}, "For SRV"},
                }},
            }},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The transaction"},
        RPCExamples{HelpExampleCli("setnamerecords", "\"core\" '[{\"type\": \"A\", \"value\": \"203.0.113.7\"}, {\"host\": \"www\", \"type\": \"CNAME\", \"value\": \"@\"}, {\"type\": \"MX\", \"value\": \"mail\", \"priority\": 10}]'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const NameHash hash{ParseName(*pwallet, request.params[0])};
    const auto owned{OwnedName(*pwallet, hash)};
    if (!owned) NotHeld(*pwallet, hash);
    bitnames::NameUpdates updates;
    if (request.params[1].empty()) {
        updates.records.kind = bitnames::UpdateKind::DELETE;
    } else {
        updates.records.kind = bitnames::UpdateKind::SET;
        updates.records.value = bitnames::RecordsFromRows(request.params[1]);
        bitnames::CheckRecordsBesideSubnames(*updates.records.value, pwallet->chain().getBitNameSubnames(hash));
    }
    Marker marker;
    marker.operation = bitnames::UpdateName{updates};
    const auto label{LabelOf(*pwallet, owned->dest)};
    return SendBitNames(*pwallet, marker, std::vector<CTxDestination>{NewDestination(*pwallet, label.value_or(""))}, {}, {owned->outpoint})->GetHash().GetHex();
},
    };
}

} // namespace wallet
