// What the pages of BitNames work out without the node: the shapes of what the node answers, the checks of
// what is typed (a name, a label, a query, a DNS record, the data of a name), the steps of a registration and
// their deadlines, and how names are shown. The rules are those of the chain (doc/bitnames.md); the node and
// the wallet check everything again, and their words are the last.
import { t } from '../../lib/i18n.svelte';

/** getbitnamesinfo */
export interface BitNamesInfo {
  names: number;
  reservations: number;
  icann: boolean;
  /** How many blocks deep a reservation is before a registration in the next block can reveal it. */
  revealdepth: number;
  /** Where this node answers DNS for .x (-dnsbind), if it does. */
  dns?: string;
}

/** The data of a name: every field optional. */
export interface NameData {
  commitment?: string;
  ipv4?: string;
  ipv6?: string;
  encryptionkey?: string;
  signingkey?: string;
  paymailfee?: number;
  records?: Record<string, unknown>;
}

/** getname, listnames */
export interface NameInfo {
  namehash: string;
  /** The text, if it and its parents are public (blog.core for a subname). Text of the chain. */
  name?: string;
  parent?: string;
  label?: string;
  seq: string;
  registered: number;
  registration: string;
  icann?: string;
  data?: NameData;
  owner: { txid?: string; vout?: number; address?: string };
}

/** listmynames */
export interface MyName {
  kind: 'name' | 'reservation';
  name?: string;
  namehash?: string;
  seq?: string;
  address: string;
  txid: string;
  vout: number;
}

/** A DNS record as a row (getnamerecords, setnamerecords). */
export interface RecordRow {
  host: string;
  type: RecordType;
  value: string;
  priority?: number;
  weight?: number;
  port?: number;
}

/** listpaymail */
export interface Paymail {
  txid: string;
  vout: number;
  amount: number;
  confirmations: number;
  /** The text, if it decrypts. Text of whoever sent it. */
  message?: string;
}

/** resolvename */
export interface Resolution {
  status: 'NOERROR' | 'NXDOMAIN' | 'SERVFAIL' | 'REFUSED';
  records: { name: string; type: string; data: string }[];
}

/** getnamehistory: per field, oldest first. */
export type NameHistory = Record<string, { value: unknown; txid: string; height: number }[]>;

export const TLD = 'x';
export const RECORD_TYPES = ['A', 'AAAA', 'CNAME', 'TXT', 'MX', 'SRV', 'NS', 'TLSA', 'DELEGATE'] as const;
export type RecordType = (typeof RECORD_TYPES)[number];
export const LOOKUP_TYPES = ['ANY', 'A', 'AAAA', 'CNAME', 'TXT', 'MX', 'SRV', 'TLSA', 'NS'] as const;
/** The most a name holds of records (bytes of JSON), and of a message (bytes, encrypted). */
export const MAX_RECORDS_BYTES = 1024;
export const MAX_MEMO_BYTES = 1024;
/** What encryption adds to a message: a version byte, the two ElligatorSwift keys (64 bytes each), the tag of ChaCha20-Poly1305 (16). */
export const MEMO_OVERHEAD = 145;
/** Subnames: at most 8 levels under the top-level name. */
export const MAX_DEPTH = 8;

/** The bytes of a text in UTF-8. */
export function utf8Bytes(text: string): number {
  return new TextEncoder().encode(text).length;
}

/** The text of a name the wallet makes: 1 to 255 lower-case letters, digits, - and _ (IsGoodNameText). */
export function isGoodName(text: string): boolean {
  return /^[a-z0-9_-]{1,255}$/.test(text);
}

/**
 * Whether a label of the text starts with xn-- (IDNA punycode): browsers may show it as letters of another
 * script that look like another name, so the wallet makes no such names or subnames (IsPunycodeName).
 */
export function isPunycode(text: string): boolean {
  return text.split('.').some((l) => l.toLowerCase().startsWith('xn--'));
}

const PUNYCODE = () => t('A name starting with xn-- can show in browsers as letters of another script, a look-alike of another name: the wallet does not make it.');

/** A label of a subname: 1 to 63 lower-case letters, digits and hyphens, not at either end (IsDnsLabel). */
export function isLabel(text: string): boolean {
  return /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(text);
}

export function isHash(text: string): boolean {
  return /^0x[0-9a-f]{64}$/i.test(text);
}

/** A number of a name as commands take it: #1739-0029. */
export function isSeq(text: string): boolean {
  return /^#\d{4}-\d{4}$/.test(text);
}

/** What a search box was given. */
export type QueryKind = 'empty' | 'domain' | 'seq' | 'hash' | 'subname' | 'name' | 'bad';

export function queryKind(raw: string): QueryKind {
  const q = raw.trim();
  if (!q) return 'empty';
  if (isSeq(q)) return 'seq';
  if (isHash(q)) return 'hash';
  const lower = q.toLowerCase();
  if (lower === TLD || lower.endsWith(`.${TLD}`) || lower.endsWith(`.${TLD}.`)) return 'domain';
  if (q.includes('.')) return q.split('.').every((l) => isGoodName(l)) ? 'subname' : 'bad';
  return isGoodName(q) ? 'name' : 'bad';
}

/** A domain under .x without its TLD (www.core.x → www.core), or null. */
export function withoutTld(domain: string): string | null {
  const d = domain.trim().toLowerCase().replace(/\.$/, '');
  if (!d.endsWith(`.${TLD}`)) return null;
  return d.slice(0, -TLD.length - 1);
}

/**
 * Why a name typed to get cannot be had as it is ('' when it can be asked for): the rules of the wallet
 * (CheckNewName), with what was meant suggested.
 */
export function newNameProblem(raw: string): string {
  const name = raw.trim();
  if (!name) return '';
  if (name.toLowerCase() !== name) return t('Names are lower case, as DNS looks them up: %1', name.toLowerCase());
  if (name.length > 255) return t('A name is at most 255 characters.');
  if (!isGoodName(name)) return t('A name is lower-case letters, digits, - and _.');
  if (isPunycode(name)) return PUNYCODE();
  return '';
}

/** Why a subname typed as label.parent cannot be made as it is ('' when it can be asked for). */
export function subnameProblem(raw: string): string {
  const text = raw.trim();
  const dot = text.indexOf('.');
  if (dot < 0) return '';
  const label = text.slice(0, dot);
  const parent = text.slice(dot + 1);
  if (!isLabel(label)) return t('A label is 1 to 63 lower-case letters, digits and hyphens, not at either end.');
  if (isPunycode(label)) return PUNYCODE();
  if (!parent || !parent.split('.').every((l) => isGoodName(l))) return t('The name it goes under is lower-case letters, digits, - and _.');
  if (parent.split('.').length >= MAX_DEPTH + 1) return t('A subname is at most %1 levels under its top-level name.', MAX_DEPTH);
  return '';
}

/** How a name is shown: its text, or its number when its text is private. */
export function shownName(n: { name?: string; seq?: string }): string {
  return n.name ?? (n.seq ? `#${n.seq}` : '');
}

/** The handle commands take for a name known by its hash. */
export function handleOf(namehash: string): string {
  return `0x${namehash}`;
}

/** The domain of a name, when its text is known. */
export function domainOf(n: { name?: string }): string | null {
  return n.name ? `${n.name}.${TLD}` : null;
}

// --- DNS records ---

const IPV4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;

export function isIPv4(text: string): boolean {
  return IPV4.test(text);
}

export function isIPv6(text: string): boolean {
  if (!text.includes(':') || !/^[0-9a-fA-F:.]+$/.test(text)) return false;
  const halves = text.split('::');
  if (halves.length > 2) return false;
  const groups = halves.flatMap((h) => (h ? h.split(':') : []));
  // An IPv4 address at the end counts for two groups.
  const last = groups[groups.length - 1] ?? '';
  const v4 = last.includes('.');
  if (v4 && !isIPv4(last)) return false;
  const words = groups.length + (v4 ? 1 : 0);
  if (groups.slice(0, v4 ? -1 : undefined).some((g) => !/^[0-9a-fA-F]{1,4}$/.test(g))) return false;
  return halves.length === 2 ? words < 8 : words === 8;
}

/** A host name of a record: absolute with a final dot, else under the name; @ is the name itself. */
export function isHostName(text: string): boolean {
  if (text === '@') return true;
  const t2 = text.endsWith('.') ? text.slice(0, -1) : text;
  if (!t2 || t2.length > 253) return false;
  return t2.split('.').every((l) => /^[A-Za-z0-9_]([A-Za-z0-9_-]{0,61}[A-Za-z0-9_])?$/.test(l));
}

/** The host of a row: @, *, or labels under the name (www, a.b, *.b). */
export function isRecordHost(text: string): boolean {
  if (text === '' || text === '@' || text === '*') return true;
  return text.split('.').every((l) => l === '*' || /^[a-z0-9_]([a-z0-9_-]{0,61}[a-z0-9_])?$/.test(l));
}

function port(n: unknown): boolean {
  return Number.isInteger(n) && (n as number) >= 0 && (n as number) <= 65535;
}

/** What is wrong with a row ('' when nothing): at once as it is typed; the wallet's own check follows. */
export function rowProblem(row: RecordRow): string {
  const value = row.value.trim();
  if (!isRecordHost(row.host.trim())) return t('Host: @ for the name itself, or the labels under it (www, a.b, * for any).');
  if (!value) return t('A record needs a value.');
  switch (row.type) {
    case 'A':
      return isIPv4(value) ? '' : t('A: an IPv4 address, such as 203.0.113.7');
    case 'AAAA':
      return isIPv6(value) ? '' : t('AAAA: an IPv6 address, such as 2001:db8::7');
    case 'CNAME':
    case 'NS':
      return isHostName(value) ? '' : t('A host name: absolute if it ends with a dot (example.com.), else under the name (mail).');
    case 'MX':
      if (!port(row.priority ?? 10)) return t('The priority is a number from 0 to 65535.');
      return isHostName(value) ? '' : t('A host name: absolute if it ends with a dot (example.com.), else under the name (mail).');
    case 'SRV':
      if (![row.priority ?? 10, row.weight ?? 0, row.port ?? 0].every(port)) return t('Priority, weight and port are numbers from 0 to 65535.');
      return isHostName(value) ? '' : t('A host name: absolute if it ends with a dot (example.com.), else under the name (mail).');
    case 'TLSA': {
      const parts = value.split(/\s+/);
      if (parts.length !== 4 || !parts.slice(0, 3).every((p) => /^\d{1,3}$/.test(p)) || !/^[0-9a-fA-F]+$/.test(parts[3])) return t('TLSA: usage selector type data, such as 3 1 1 ab12…');
      return '';
    }
    case 'DELEGATE':
      return isHash(value) || value.split('.').every((l) => isGoodName(l)) ? '' : t('DELEGATE: a name, a subname such as blog.core, or the hash of a name (0x and 64 hex digits).');
    case 'TXT':
      return '';
  }
  return t('Record types: A, AAAA, CNAME, TXT, MX, SRV, NS, TLSA, DELEGATE');
}

/**
 * Whether an address is one of a private or local network (or this computer): a name may point there, but
 * the pages of whoever holds the name can then reach that network from a browser (DNS rebinding).
 */
export function isPrivateAddress(text: string): boolean {
  const a = text.trim().toLowerCase();
  if (isIPv4(a)) {
    const [x, y] = a.split('.').map(Number);
    return x === 10 || x === 127 || x === 0 || (x === 169 && y === 254) || (x === 172 && y >= 16 && y <= 31) || (x === 192 && y === 168) || (x === 100 && y >= 64 && y <= 127);
  }
  if (isIPv6(a)) {
    if (a === '::1' || a === '::') return true;
    if (/^f[cd][0-9a-f]{0,2}:/.test(a) || /^fe[89ab][0-9a-f]?:/.test(a)) return true;
    const mapped = /^::ffff:(\d+\.\d+\.\d+\.\d+)$/.exec(a);
    return mapped ? isPrivateAddress(mapped[1]) : false;
  }
  return false;
}

/** A note on a row that is valid but may not be what is meant ('' when none). */
export function rowWarning(row: RecordRow): string {
  if ((row.type === 'A' || row.type === 'AAAA') && isPrivateAddress(row.value)) {
    return t('A private or local address: only computers on that network reach it, and web pages under the name can reach that network.');
  }
  return '';
}

/** A row as setnamerecords takes it (the fields of its type only). */
export function rowForWallet(row: RecordRow): Record<string, unknown> {
  const out: Record<string, unknown> = { host: row.host.trim() || '@', type: row.type, value: row.value.trim() };
  if (row.type === 'MX' || row.type === 'SRV') out.priority = row.priority ?? 10;
  if (row.type === 'SRV') {
    out.weight = row.weight ?? 0;
    out.port = row.port ?? 0;
  }
  return out;
}

/** The rows as the node gave them, typed. */
export function rowsFrom(raw: unknown): RecordRow[] {
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((r): r is Record<string, unknown> => !!r && typeof r === 'object')
    .map((r) => ({
      host: typeof r.host === 'string' ? r.host : '@',
      type: (RECORD_TYPES as readonly string[]).includes(String(r.type)) ? (String(r.type) as RecordType) : 'TXT',
      value: typeof r.value === 'string' ? r.value : String(r.value ?? ''),
      ...(typeof r.priority === 'number' ? { priority: r.priority } : {}),
      ...(typeof r.weight === 'number' ? { weight: r.weight } : {}),
      ...(typeof r.port === 'number' ? { port: r.port } : {}),
    }));
}

/** The domain a row answers for: www under core is www.core.x. */
export function rowDomain(row: RecordRow, name: string): string {
  const host = row.host.trim();
  return host === '' || host === '@' ? `${name}.${TLD}` : `${host}.${name}.${TLD}`;
}

// --- The data of a name ---

export const DATA_FIELDS = ['ipv4', 'ipv6', 'commitment', 'encryptionkey', 'signingkey', 'paymailfee'] as const;
export type DataField = (typeof DATA_FIELDS)[number];

/** What is wrong with a field of the data ('' when nothing, or when it is empty: deleting it). */
export function dataProblem(field: DataField, raw: string): string {
  const v = raw.trim();
  if (!v) return '';
  switch (field) {
    case 'ipv4': {
      const m = v.match(/^([\d.]+)(?::(\d{1,5}))?$/);
      return m && isIPv4(m[1]) && (m[2] === undefined || Number(m[2]) <= 65535) ? '' : t('An IPv4 address, with a port or not: 203.0.113.7 or 203.0.113.7:443');
    }
    case 'ipv6': {
      const m = v.match(/^\[([^\]]+)\]:(\d{1,5})$/);
      const ok = m ? isIPv6(m[1]) && Number(m[2]) <= 65535 : isIPv6(v);
      return ok ? '' : t('An IPv6 address, with a port or not: 2001:db8::7 or [2001:db8::7]:443');
    }
    case 'commitment':
      return /^[0-9a-fA-F]{64}$/.test(v) ? '' : t('A hash: 64 hex digits (32 bytes).');
    case 'encryptionkey':
      return /^0[23][0-9a-fA-F]{64}$/.test(v) ? '' : t('A compressed public key: 66 hex digits, starting with 02 or 03.');
    case 'signingkey':
      return /^[0-9a-fA-F]{64}$/.test(v) ? '' : t('An x-only public key: 64 hex digits.');
    case 'paymailfee':
      return /^\d+(\.\d{1,8})?$/.test(v) && Number(v) <= 21_000_000 ? '' : t('An amount, such as 0.0001');
  }
  return '';
}

/** The text of a field of the data as a form shows it. */
export function dataText(data: NameData | undefined, field: DataField): string {
  const v = data?.[field];
  if (v === undefined || v === null) return '';
  return typeof v === 'number' ? v.toFixed(8).replace(/0+$/, '').replace(/\.$/, '') : String(v);
}

/**
 * The fields of the form that changed, as updatename takes them: a value sets it, null deletes it, a field
 * left out is kept.
 */
export function dataUpdates(before: NameData | undefined, form: Record<DataField, string>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const f of DATA_FIELDS) {
    const now = form[f].trim();
    if (now === dataText(before, f)) continue;
    if (!now) out[f] = null;
    else out[f] = now;
  }
  return out;
}

// --- A registration, step by step ---

/** A name this app is registering: kept by the host (bitnames.store.*) so that it goes on after a restart. */
export interface Registration {
  wallet: string;
  /** The name (text of the user, checked: lower-case letters, digits, - and _). */
  name: string;
  public: boolean;
  /** The data given at registration (ipv4...). */
  data?: Record<string, string>;
  /** The step reached: reserved (waiting for the reservation to be deep enough), registering, done, failed. */
  step: 'reserved' | 'registering' | 'done' | 'failed';
  reserveTxid: string;
  /** The block the reservation is in, once it is. */
  reserveHeight?: number;
  registerTxid?: string;
  /** The tip when the registration was sent. */
  registerSent?: number;
  /** The block that registered the name. */
  registeredHeight?: number;
  /** Seconds. */
  started: number;
  updated: number;
  error?: string;
  /**
   * Deep enough, and the host would have to ask (no authorization since the app started again, or a locked wallet):
   * the user confirms it on its card. Not kept in the store.
   */
  needsConfirm?: string;
}

export type StageId = 'reserve' | 'confirm' | 'wait' | 'register' | 'yours';
export const STAGES: { id: StageId; label: string }[] = [
  { id: 'reserve', label: 'Reserved' },
  { id: 'confirm', label: 'In a block' },
  { id: 'wait', label: 'Hidden' },
  { id: 'register', label: 'Revealed' },
  { id: 'yours', label: 'Yours' },
];

export interface Progress {
  /** The stage now (0 to 4). */
  index: number;
  stage: StageId;
  /** The block from which the registration may be in a block (reservation height + reveal depth). */
  revealFrom?: number;
  /** Blocks until then (0: the next block may take it). */
  blocksLeft?: number;
  /** The block the registration should be in by: later than that, someone may be keeping it out. */
  deadline?: number;
  /** The registration is waiting longer than reveal_depth blocks. */
  late?: boolean;
  /** 0 to 1, along the whole flow. */
  fraction: number;
}

/**
 * Where a registration is, at the tip `tip` (the next block is tip + 1): a registration in block h of a
 * reservation in block r needs h - r ≥ revealdepth.
 */
export function progressOf(r: Registration, tip: number, revealdepth: number): Progress {
  if (r.step === 'done') return { index: 4, stage: 'yours', fraction: 1 };
  if (r.step === 'registering') {
    const deadline = (r.registerSent ?? tip) + Math.max(1, revealdepth);
    return { index: 3, stage: 'register', deadline, late: tip > deadline, fraction: 0.85 };
  }
  if (r.reserveHeight === undefined) return { index: 1, stage: 'confirm', fraction: 0.15 };
  const revealFrom = r.reserveHeight + revealdepth;
  const blocksLeft = Math.max(0, revealFrom - (tip + 1));
  const done = revealdepth > 0 ? 1 - blocksLeft / revealdepth : 1;
  return { index: blocksLeft > 0 ? 2 : 3, stage: blocksLeft > 0 ? 'wait' : 'register', revealFrom, blocksLeft, fraction: 0.2 + 0.6 * Math.max(0, Math.min(1, done)) };
}

/** About how long a number of blocks takes, in words of the app. */
export function blocksTime(blocks: number, spacing: number): string {
  const secs = Math.max(0, blocks) * Math.max(1, spacing);
  if (blocks <= 0) return t('the next block');
  if (secs < 90) return t('about a minute');
  if (secs < 3600) return t('about %1 minutes', Math.round(secs / 60));
  return t('about %1 hours', (secs / 3600).toFixed(secs < 36000 ? 1 : 0));
}
