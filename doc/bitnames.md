# BitNames: names on a sidechain

BitNames is a sidechain of Chains (slot 3) on which names are owned like coins. A name points to
addresses, keys and DNS records; its owner can be paid with messages only they can read. It is
inspired by **BitNames by LayerTwo Labs** (plain-bitnames), built anew on this Bitcoin Core based
sidechain, with Bitcoin's keys and transactions, without the defects of the original, and with
features of Namecoin added: DNS records on the chain, subnames, public names and a DNS
server for the `.x` domain.

## Names

- **Taking a name** takes two steps, so that nobody can see it coming and take it first. A
  *reservation* commits to the name without showing it (HMAC-SHA256 of the name's hash, under a
  nonce only the wallet can make); once it is in a block, the *registration* reveals the nonce and
  creates the name. The window ("Names", in the sidebar) does both steps.
- **A name is known on the chain by the SHA-256 hash of its text.** A registration may also make
  the text *public*, so that the name can be listed and searched for, as Namecoin names are.
- **Names are permanent.** They do not expire. Their owner can give them away
  (`transfername`) or release them (`releasename`): a released name leaves the registry with its
  data, and anyone can register it again. A reservation that is not needed can be released too
  (`releasereservation`).
- **Numbers.** Each name gets a number in the order of registration, shown as BitNames shows it,
  `1739-0029` for the first.
- **Data.** Every field is optional: a 32-byte commitment to data kept elsewhere, an IPv4 and an
  IPv6 address with port, a key messages to the owner are encrypted to, a key the owner signs
  with, the paymail fee, and DNS records. Every value is kept with the transaction and height that
  set it: `getname <name> <height>` gives the data of a name at any height, `getnamehistory` all
  its values.
- **Subnames.** The owner of `core` can make `blog.core` and give it to any address
  (`createsubname core blog <address>`). A subname is a name of its own: its holder sets its data
  and records, gives it on, and makes subnames of it. The owner of the parent cannot take it back.
  On the chain a subname is known by SHA-256 of its parent's hash and its label.
- **ICANN names.** An authority set per network can mark names as ICANN domain names. None is set
  on the test network.

## How it is carried

Names and reservations are carried by outputs of no value. A *marker*, an OP_RETURN output of the
transaction, lists which outputs carry names and which carry reservations, and says what the
transaction does (reserve, register, update, make a subname, release, mark ICANN names). The names
and reservations a transaction spends go to the outputs it lists, in order; what it creates comes
last. Their number cannot change except as the operation says, so a name cannot be lost by
accident: a transaction that spends a name without carrying it on is invalid.

## Messages

- `getnewnamekeys` makes keys to set on a name: an encryption key (secp256k1) and a signing key
  (BIP340, x-only).
- `encryptnamemessage` encrypts to the encryption key of a name, as BIP324 sets up its
  connections: an ElligatorSwift Diffie-Hellman with a key made for the message, then
  ChaCha20-Poly1305. `decryptnamemessage` opens a message to a key of the wallet.
- `signasname` and `verifynamesignature` sign and check messages with the signing key of a name.
- **Paymail.** `sendpaymail <name> <amount> <message>` pays the owner of a name, at least the
  name's paymail fee, with a message encrypted to it; `listpaymail` lists what the wallet received.

## DNS records

The `records` of a name are a JSON object of at most 1024 bytes, in the format of Namecoin's
domains:

| Key | Value | DNS |
|---|---|---|
| `ip` | `"203.0.113.7"` or a list | A |
| `ip6` | `"2001:db8::7"` or a list | AAAA |
| `alias` | a host name | CNAME (stands for all records of the node) |
| `txt` | a string or a list | TXT |
| `mx` | `[[10, "mail"], ...]` | MX |
| `srv` | `[[priority, weight, port, "target"], ...]` | SRV |
| `tls` | `[[usage, selector, matching type, "hex"], ...]` | TLSA |
| `ns` | a list of host names | NS |
| `map` | `{"label": {...}, "*": {...}, "": {...}}` | subdomains; `*` matches any label, `""` adds to the node itself |
| `delegate` | the text of another name (or `0x` and its hash) | the records of that name stand for this node |

A host name ending with a dot is absolute (`example.com.`); otherwise it is under the name
(`mail` in the records of `core` is `mail.core.x`); `@` is the name itself. Without an `ip` or
`ip6` of their own, the name's IPv4 and IPv6 addresses answer.

A domain `a.b.core.x` resolves down from `core`: for each label, a subname on the chain first,
then the `map` of the records, then `*`. `resolvename www.core.x A` shows what the DNS server
answers.

## The DNS server

Start the node with `-dnsbind=127.0.0.1:5335` (or `dnsbind=127.0.0.1:5335` in `bitnames.conf`);
it answers DNS questions for `.x` over UDP (TTL 60 seconds), and refuses the others. Then send the
`.x` questions of the computer to it:

- **Linux, systemd-resolved:** `resolvectl dns lo 127.0.0.1:5335` and `resolvectl domain lo ~x`
- **Linux, dnsmasq:** `server=/x/127.0.0.1#5335`
- **macOS:** a file `/etc/resolver/x` with the lines `nameserver 127.0.0.1` and `port 5335`
- **Windows:** run the server on port 53 (`-dnsbind=127.0.0.1:53`), then in PowerShell
  `Add-DnsClientNrptRule -Namespace ".x" -NameServers 127.0.0.1`

Then `ping core.x` or a browser at `http://core.x` reach the address the name points to.

## Commands

Node: `getname`, `listnames`, `getnamehistory`, `getbitnamesinfo`, `resolvename`,
`verifynamesignature`.
Wallet: `reservename`, `registername`, `updatename`, `transfername`, `createsubname`,
`releasename`, `releasereservation`, `listmynames`, `getnewnamekeys`, `encryptnamemessage`,
`decryptnamemessage`, `signasname`, `sendpaymail`, `listpaymail`.

## Differences from the original

- Bitcoin's keys, addresses and transactions instead of ed25519 and BLAKE3; SHA-256 for name
  hashes, HMAC-SHA256 for reservations, secp256k1 for messages.
- Fixed: two registrations of the same name in one block (the original accepted both, then could
  not undo the block); transactions without inputs that could be replayed; ICANN marking, which
  could not work (its key was all zeros, its undo always failed).
- Added: DNS records, subnames, public names, release, the DNS server, paymail sending.
