# BitNames

BitNames is a sidechain of Chains on which names are owned like coins. A name points to addresses,
keys and DNS records; its owner can be paid with messages only they can read. Names are permanent,
can have subnames, and the node can answer DNS for them under the `.x` domain.

| | |
|---|---|
| Slot on the mainchain | 3 |
| Coin | CHN, deposited from [Chains](https://github.com/block-fabric/chains) and withdrawn back to it |
| Addresses | `bn1…` (`tbn1…` on the test networks) |
| P2P port | 9955 (testnet 19955, signet 39955, regtest 29955) |
| RPC port | 9954 (testnet 19954, signet 39954, regtest 29954) |

BitNames is the [sidechain template](doc/sidechain.md) of Chains with names added. A BitNames node needs a
Chains node (`chainsd` or `chains-app`) to follow, reached over its RPC interface.

Its GUI is `bitnames-app`: the node and its wallet with an HTML interface in a native web view
(WebKitGTK on Linux, WebView2 on Windows), with pages for finding and registering names, the names of
the wallet and their records, paid messages and the DNS of `.x` ([doc/webui.md](doc/webui.md)). There is no Qt GUI.

## Installing

Builds for Linux (x86-64) are at https://blockfab.org/drivechains/. Unpack, check `sha256sum -c SHA256SUMS`, and run what is in `bin/`. The app needs WebKitGTK (on Ubuntu 24.04: `sudo apt install libwebkit2gtk-4.1-0 libsqlite3-0`); the node and the tools need none of it. On Windows, the installer or the zip of `bitnames-app` (the WebView2 Runtime of Windows 10 and 11).

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev libgtk-3-dev libwebkit2gtk-4.1-dev nodejs npm
git clone https://github.com/block-fabric/bitnames-app.git
cd bitnames-app
cmake -B build
cmake --build build -j$(nproc)
```

`-DBUILD_WEBUI=OFF` (and no GTK, WebKitGTK or Node.js) for a node without a window. Windows: cross-built
with the depends system (`make -C depends HOST=x86_64-w64-mingw32 NO_QT=1`), see [doc/webui.md](doc/webui.md).
Other systems and options: [doc/build-unix.md](doc/build-unix.md) and the other `doc/build-*.md`.

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `bitnames-app` | the wallet, with its window |
| `bitnamesd` | the node, without a window |
| `bitnames-cli` | commands for a running node or wallet |
| `bitnames-wallet`, `bitnames-tx`, `bitnames-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The
sidechain tests need a built Chains tree next to this one (`../chains`), or `MAINCHAIN_BIN_DIR`
set to the folder of its programs.

## Running it

First a Chains node, with RPC on (`server=1` in `chains.conf`, or `-server`), on the same network.

The simplest is to let the Chains wallet run BitNames: in its **Sidechain Nodes** window, select
BitNames, press **Locate programs…** and choose the folder with `bitnamesd` and `bitnames-app`, then **Start node**
or **Open wallet**.

By hand, on the same computer as the Chains node:

```sh
bitnames-app                # the main network
bitnames-app -testnet       # the test network
```

It finds the Chains node by itself: RPC on 127.0.0.1 at the port of the network, and the cookie
file in the Chains data folder (`~/.chains/.cookie`, `~/.chains/testnet/.cookie`). Options for
anything else:

| Option | What it is |
|---|---|
| `-mainchainrpcconnect=<ip>` | address of the Chains node (default 127.0.0.1) |
| `-mainchainrpcport=<port>` | its RPC port (default 9554, testnet 19554, signet 39554, regtest 29554) |
| `-mainchainrpccookiefile=<file>` | its cookie file |
| `-mainchaindatadir=<dir>` | its data folder, where the cookie file is looked for (default `~/.chains`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Chains wallet that pays for merged mining (default: its only loaded wallet) |

The slot is fixed on the main and test networks; `-sidechainslot=<n>` is for regtest only, where
it makes the chain a sidechain in slot `n` (see [doc/sidechain.md](doc/sidechain.md)).

Data and the configuration file `bitnames.conf` are in `~/.bitnames`; the test network has a subfolder
`testnet`. The networks have no DNS seeds: give the node a peer with `addnode`. A sample
`~/.bitnames/bitnames.conf`:

```ini
server=1
fallbackfee=0.0002
# Answer DNS for .x names on this computer:
# dnsbind=127.0.0.1:5335

# For the test network instead, uncomment:
# testnet=1

[main]
addnode=<peer>:9955
mainchainrpcwallet=<wallet of the Chains node>

[test]
addnode=<peer>:19955
mainchainrpcwallet=<wallet of the Chains node>
```

On the test network, the `-mainchainrpcport`, `-mainchainrpccookiefile` and `-mainchainrpcwallet`
options are read only from the `[test]` section.

## Coins in and out

- **Deposit.** In BitNames, get a deposit address (`getdepositaddress`, or the **Peg** page):
  `s3_<address>_<checksum>`. In Chains, send to it from the **Sidechains** page, or
  `chains-cli createsidechaindeposit 3 <deposit address> <amount>`. The coins appear with the
  next BitNames block.
- **Withdraw.** `createwithdrawal <Chains address> <amount> ( <fee for Chains miners> )`, or the
  **Peg** page. Withdrawals are paid in bundles that Chains miners vote on (about three
  months on the main network, 600 blocks on the test network). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** BitNames blocks are mined by Chains miners, for a fee in CHN paid by the Chains
  wallet. `setbmm true <BitNames address>` has the node ask for a block whenever the fees waiting pay
  for one; it offers the miners 99% and keeps 1% at the address. `requestbmmblock <address>
  <amount>` asks for one block now, for a fee of your choice (for a deposit on a quiet chain).
  `getbmminfo` shows how it goes.

How deposits, withdrawals, bundles and merged mining work: [doc/sidechain.md](doc/sidechain.md).

## Names

- **Getting a name** takes two steps, a hidden reservation then the registration, so that nobody can
  see it coming and take it first. The **Get a name** page does both, as one guided flow.
- A name holds addresses, keys, a commitment and **DNS records**; it can be sent to another owner,
  have **subnames** owned by anyone, and be released.
- **Paid messages** (paymail): anyone can send the owner of a name an encrypted message with a payment.
- **DNS.** With `-dnsbind=127.0.0.1:5335` the node answers DNS questions for `.x` names, so that
  `http://yourname.x` works on the computer once its resolver asks the node (the **DNS** page says
  how, and tests lookups).

How names work: [doc/bitnames.md](doc/bitnames.md).

## Commands

BitNames, on the node: `getbitnamesinfo`, `getname`, `getnamehistory`, `getnamerecords`, `listnames`,
`resolvename`, `verifynamesignature`.

BitNames, in the wallet: `reservename`, `registername`, `releasereservation`, `updatename`,
`setnamerecords`, `createsubname`, `transfername`, `releasename`, `listmynames`, `getnewnamekeys`,
`signasname`, `encryptnamemessage`, `decryptnamemessage`, `sendpaymail`, `listpaymail`.

Sidechain, on the node: `getmainchaininfo`, `syncmainchain`, `setbmm`, `getbmminfo`, `requestbmmblock`, `createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`.

Sidechain, in the wallet: `getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

## Wallet

The pages of `bitnames-app`: those of every sidechain (Overview, Send, Receive, Transactions, Wallets,
**Peg**, **Merged mining**, the explorer, the node window), and those of the names: **Get a name**
(search with live availability, then reservation and registration as one guided flow), **My names**
(records, keys, subnames, giving away, releasing), **Directory** (every public name, its history),
**Paymail** (paid messages, encrypting and signing as a name) and **DNS** (the resolver of `.x` and a
lookup tester). How they are made: [doc/webui.md](doc/webui.md).

## Credits

BitNames is inspired by **BitNames by LayerTwo Labs** (plain-bitnames), with features of Namecoin
added. It is built on the sidechain template of Chains, which is based on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING).
