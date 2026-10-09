// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The names under fuzzing, on the store of the sidechain state: blocks of fuzzed reservations (bound
// to the script of their output, copied, or to the name alone), registrations, subnames, updates,
// releases and transfers, by two owners. After every block: undone, the store is
// exactly what it was (through serialization of the undo data too); a transaction that breaks a rule
// changes nothing; and every index is exactly what the tables imply.

#include <bitnames/state.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bitnames;

namespace {
void initialize_bitnames()
{
    static const auto testing_setup{MakeNoLogFileContext<>()};
}

/** Two owners: names and reservations are carried by outputs someone can spend (a script of a standard type). */
const CScript PAYEES[2]{CScript() << OP_0 << std::vector<unsigned char>(20, 1), CScript() << OP_0 << std::vector<unsigned char>(20, 2)};

CMutableTransaction Tx(const std::vector<COutPoint>& inputs, size_t names, size_t reservations, std::optional<Operation> op, uint32_t salt, const CScript& payee)
{
    CMutableTransaction tx;
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(salt)}), salt});
    Marker marker;
    marker.operation = std::move(op);
    for (size_t i{0}; i < names; ++i) {
        marker.name_outputs.push_back(tx.vout.size());
        tx.vout.emplace_back(0, payee);
    }
    for (size_t i{0}; i < reservations; ++i) {
        marker.reservation_outputs.push_back(tx.vout.size());
        tx.vout.emplace_back(0, payee);
    }
    tx.vout.emplace_back(0, MarkerScript(marker));
    return tx;
}

/** The entries under a table byte, as raw keys. */
std::set<sidechain::StoreBytes> KeysUnder(const sidechain::StoreView& view, uint8_t table)
{
    std::set<sidechain::StoreBytes> keys;
    view.ForEach(sidechain::StoreBytes{table}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        keys.insert(key);
        return true;
    });
    return keys;
}

/** Every index, rebuilt from the tables, is the one in the store; the counts are the tables' sizes. */
void CheckIndexes(const sidechain::StoreView& view)
{
    std::set<sidechain::StoreBytes> children, name_tokens;
    std::set<uint256> bound;
    uint64_t names{0}, reservations{0};
    sidechain::Table<uint256, NameRecord>{0x21}.ForEach(view, [&](const uint256& hash, const NameRecord& record) {
        ++names;
        if (record.parent) {
            sidechain::StoreBytes key{0x27};
            sidechain::KeyCodec<uint256>::Encode(key, *record.parent);
            sidechain::KeyCodec<uint256>::Encode(key, hash);
            children.insert(key);
        }
        return true;
    });
    sidechain::Table<COutPoint, Token>{0x20}.ForEach(view, [&](const COutPoint& outpoint, const Token& token) {
        if (token.kind == Token::Kind::NAME) {
            sidechain::StoreBytes key{0x28};
            sidechain::KeyCodec<uint256>::Encode(key, token.id);
            sidechain::KeyCodec<COutPoint>::Encode(key, outpoint);
            name_tokens.insert(key);
        }
        return true;
    });
    sidechain::Table<uint256, ReservationRecord>{0x22}.ForEach(view, [&](const uint256& id, const ReservationRecord&) {
        ++reservations;
        // The script a reservation is bound to is kept for each one, and no other.
        bound.insert(id);
        return true;
    });
    assert(KeysUnder(view, 0x27) == children);
    assert(KeysUnder(view, 0x28) == name_tokens);
    // Tables of the order of reservations, no longer kept.
    assert(KeysUnder(view, 0x24).empty() && KeysUnder(view, 0x26).empty() && KeysUnder(view, 0x29).empty());
    std::set<uint256> origins;
    sidechain::Table<uint256, CScript>{0x2d}.ForEach(view, [&](const uint256& id, const CScript&) {
        origins.insert(id);
        return true;
    });
    assert(origins == bound);
    const State state{view};
    assert(state.NameCount() == names);
    assert(state.ReservationCount() == reservations);
    // Every number names a name that has it, or one released (its number is not given again).
    for (uint32_t seq{0}; seq < state.NextSeq(); ++seq) {
        if (const auto name{state.NameOfSeq(seq)}) {
            const auto record{state.GetName(*name)};
            assert(!record || record->seq == seq);
        }
    }
    // The history is that of names there are, by their numbers: an entry per field from the
    // registration on, the latest of each the value the record has now.
    std::map<uint32_t, std::set<uint8_t>> fields;
    view.ForEach(sidechain::StoreBytes{0x2c}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        assert(key.size() == 1 + 4 + 1 + 4 + 4);
        std::span<const unsigned char> in{key};
        in = in.subspan(1);
        const uint32_t seq{sidechain::KeyCodec<uint32_t>::Decode(in)};
        const auto name{state.NameOfSeq(seq)};
        assert(name);
        const auto record{state.GetName(*name)};
        assert(record && record->seq == seq);
        fields[seq].insert(key[5]);
        return true;
    });
    size_t with_history{0};
    state.ForEachName([&](const NameHash&, const NameRecord& record) {
        assert(fields[record.seq].size() == 7);
        ++with_history;
        assert(state.DataAtHeight(record, std::numeric_limits<int>::max()) == record.Current());
        return true;
    });
    assert(std::count_if(fields.begin(), fields.end(), [](const auto& f) { return !f.second.empty(); }) == static_cast<ptrdiff_t>(with_history));
    // Each name counts the names under it, and knows how deep it is. A subname has its parent, and
    // was made under the parent's registration: releasing a name takes its subnames along, so none
    // is ever left for the next holder.
    std::function<uint32_t(const NameHash&)> under{[&](const NameHash& name) {
        uint32_t n{0};
        for (const NameHash& child : state.ChildrenOf(name)) n += 1 + under(child);
        return n;
    }};
    state.ForEachName([&](const NameHash& hash, const NameRecord& record) {
        assert(record.descendants == under(hash));
        if (record.parent) {
            const auto parent{state.GetName(*record.parent)};
            assert(parent);
            assert(record.depth == parent->depth + 1);
            assert(record.height >= parent->height);
        } else {
            assert(record.depth == 0);
        }
        return true;
    });
}
} // namespace

FUZZ_TARGET(bitnames_state, .init = initialize_bitnames)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    Consensus::BitNamesParams params;
    params.reveal_depth = fdp.ConsumeIntegralInRange<int>(0, 3);
    const std::vector<std::string> texts{"a", "b", "core", "blog", "x"};
    const std::vector<std::string> labels{"www", "blog", "a"};

    sidechain::EmptyStore empty;
    sidechain::StoreOverlay cache{empty, /*journal=*/false};
    uint32_t salt{0};

    for (int height{1}; height <= 30 && fdp.remaining_bytes() > 0; ++height) {
        const uint256 before{sidechain::StoreHash(cache)};
        sidechain::StoreOverlay block{cache, /*journal=*/true};
        State state{block};
        const int txs{fdp.ConsumeIntegralInRange<int>(0, 6)};
        for (int t{0}; t < txs; ++t) {
            // Inputs: some of what outputs carry now.
            std::vector<std::pair<COutPoint, Token>> carried;
            state.ForEachToken([&](const COutPoint& o, const Token& tok) { carried.emplace_back(o, tok); return true; });
            std::vector<COutPoint> inputs;
            const int n_in{carried.empty() ? 0 : fdp.ConsumeIntegralInRange<int>(0, std::min<int>(3, carried.size()))};
            for (int i{0}; i < n_in; ++i) inputs.push_back(carried[fdp.ConsumeIntegralInRange<size_t>(0, carried.size() - 1)].first);
            const std::string text{texts[fdp.ConsumeIntegralInRange<size_t>(0, texts.size() - 1)]};
            const uint256 nonce{static_cast<uint8_t>(fdp.ConsumeIntegralInRange<int>(1, 3))};
            const CScript& payee{PAYEES[fdp.ConsumeIntegralInRange<int>(0, 1)]};
            std::optional<Operation> op;
            switch (fdp.ConsumeIntegralInRange<int>(0, 6)) {
            case 0:
                // Bound to the script of its output, to another one (a copy of someone else's), or to nothing anyone opens.
                switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
                case 0: op = Reserve{ReservationCommitment(HashName(text), nonce, payee)}; break;
                case 1: op = Reserve{ReservationCommitment(HashName(text), nonce, PAYEES[fdp.ConsumeIntegralInRange<int>(0, 1)])}; break;
                default: op = Reserve{HashName(text)}; break;
                }
                break;
            case 1: op = Register{HashName(text), nonce, {}, fdp.ConsumeBool() ? std::optional{text} : std::nullopt}; break;
            case 2: op = RegisterSubname{labels[fdp.ConsumeIntegralInRange<size_t>(0, labels.size() - 1)], {}}; break;
            case 3: {
                UpdateName update;
                update.updates.records = {UpdateKind::SET, fdp.ConsumeBool() ? "{}" : "{\"txt\": \"x\"}"};
                op = update;
                break;
            }
            case 4: op = Release{}; break;
            default: break; // a transfer: no operation
            }
            const CMutableTransaction tx{Tx(inputs, fdp.ConsumeIntegralInRange<size_t>(0, 3), fdp.ConsumeIntegralInRange<size_t>(0, 2), op, ++salt, payee)};
            const uint256 before_tx{sidechain::StoreHash(block)};
            std::string reason;
            if (!state.ApplyTx(CTransaction{tx}, height, params, reason)) {
                assert(!reason.empty());
                assert(sidechain::StoreHash(block) == before_tx);
            }
        }
        const sidechain::StoreUndo undo{block.TakeUndo()};
        block.MergeInto(cache);
        CheckIndexes(cache);
        // Undone, through serialization: exactly what was before the block.
        DataStream stream{};
        stream << undo;
        sidechain::StoreUndo read;
        stream >> read;
        sidechain::StoreOverlay reverted{cache, /*journal=*/false};
        reverted.Revert(read);
        assert(sidechain::StoreHash(reverted) == before);
        CheckIndexes(reverted);
    }
}
