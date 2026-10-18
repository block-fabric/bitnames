// The pages of BitNames: what model.ts works out (the checks of names, queries, records and data, the steps of
// a registration and their deadlines), the registrations the store takes a step further with each block (kept
// in the file of the host), and the pages driven against a pretend node: getting a name and a subname, the
// names of the wallet (records, profile, subnames, giving away, releasing), the directory, paymail, DNS, the
// cards of the home, where the pages are in the menus, and chain text shown as it is.
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN } from './config';
import { LOCAL_COMMANDS, LOCAL_PAGES, type LocalPage } from './local';
import LocalCards from './LocalCards.svelte';
import DialogHost from '../components/DialogHost.svelte';
import Directory from './pages/Directory.svelte';
import Dns from './pages/Dns.svelte';
import MyNames from './pages/MyNames.svelte';
import Paymail from './pages/Paymail.svelte';
import Register from './pages/Register.svelte';
import {
  blocksTime,
  dataProblem,
  dataUpdates,
  isIPv6,
  isPrivateAddress,
  newNameProblem,
  progressOf,
  queryKind,
  rowForWallet,
  rowProblem,
  rowWarning,
  subnameProblem,
  withoutTld,
  type MyName,
  type NameInfo,
  type Registration,
} from './bn/model';
import { bn, refreshNames, resetNames } from './bn/store.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav } from '../lib/nav.svelte';
import { handleShortcut } from '../lib/keyboard';
import { narrowWindow } from '../lib/media.svelte';
import { app, node, prefs, toasts, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { encode, program } from '../test/bech32';

const HRP = CHAIN.networks.regtest.hrp;
const ME = encode(HRP, [0, ...program(20)], false);
const OTHER = encode(HRP, [0, ...Array.from({ length: 32 }, (_, i) => (i * 7 + 3) % 32)], false);
const hash = (c: string) => c.repeat(64);
const tx = (c: string) => c.repeat(64);

function nameInfo(c: string, extra: Partial<NameInfo>): NameInfo {
  return { namehash: hash(c), seq: `000${c.charCodeAt(0) % 10}-0001`, registered: 5, registration: tx(c), owner: { txid: tx(c), vout: 0, address: OTHER }, data: {}, ...extra };
}

let registry: NameInfo[] = [];
let mine: MyName[] = [];
let records: Record<string, unknown[]> = {};
let wallettx: Record<string, { confirmations: number; blockheight?: number }> = {};
let store: unknown = null;
let info = { names: 0, reservations: 0, icann: false, revealdepth: 2, dns: '127.0.0.1:5335' as string | undefined };
const calls: BridgeRequest[] = [];

function find(arg: string): NameInfo | undefined {
  if (arg.startsWith('0x')) return registry.find((n) => n.namehash === arg.slice(2));
  if (arg.startsWith('#')) return registry.find((n) => `#${n.seq}` === arg);
  return registry.find((n) => n.name === arg);
}

function fail(message: string, code = -8): never {
  throw Object.assign(new Error(message), { code });
}

// The questions the host asked (each a dialog on the desktop) and the second steps it was told to run without one
// (confirm.h Authorizations: [wallet, method, params] as JSON).
const asked: string[] = [];
const authorized: string[] = [];

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  if (r.method === 'rpc.confirmed') {
    const c = r.params as { method: string; params: unknown[]; wallet?: string; then?: { method: string; params: unknown[] }; authorizedOnly?: boolean };
    const key = JSON.stringify([c.wallet, c.method, c.params]);
    const i = authorized.indexOf(key);
    if (i >= 0) authorized.splice(i, 1);
    else if (c.authorizedOnly) fail(`${c.method} needs your confirmation`, -32801);
    else asked.push(c.then ? `${c.method}+${c.then.method}` : c.method);
    const result = await fakeNode({ ...r, kind: 'rpc', method: c.method, params: c.params, wallet: c.wallet } as BridgeRequest);
    if (!c.then) return result;
    authorized.push(JSON.stringify([c.wallet, c.then.method, c.then.params]));
    return { ...(result as object), authorized: 3600 };
  }
  calls.push(r);
  const p = (r.params ?? []) as unknown[];
  switch (r.method) {
    case 'getbitnamesinfo':
      return { ...info, names: registry.length };
    case 'listnames': {
      const [count, skip, match, parent] = p as [number, number, string | null | undefined, string | undefined];
      const parentHash = parent === undefined ? undefined : (find(parent)?.namehash ?? parent);
      const list = registry.filter((n) => (!match || n.name?.includes(match)) && (parentHash === undefined || n.parent === parentHash));
      return list.slice(skip ?? 0, (skip ?? 0) + (count ?? 100));
    }
    case 'getname': {
      const n = find(String(p[0]));
      if (!n) fail(String(p[0]).includes('.') ? `No such subname: ${p[0]}` : 'No such name is registered');
      return n;
    }
    case 'getnamerecords': {
      const n = find(String(p[0]));
      if (!n) fail('No such name is registered');
      return records[n!.namehash] ?? [];
    }
    case 'getnamehistory':
      return { ipv4: [{ value: '203.0.113.7', txid: tx('7'), height: 6 }], records: [{ value: { ip: ['203.0.113.7'] }, txid: tx('8'), height: 7 }] };
    case 'listmynames':
      return mine;
    case 'gettransaction': {
      const t = wallettx[String(p[0])];
      if (!t) fail('Invalid or non-wallet transaction id', -5);
      return t;
    }
    case 'reservename':
      return { txid: tx('r'), namehash: hash('n') };
    case 'registername':
      return { txid: tx('g'), namehash: hash('n') };
    case 'createsubname':
      return { txid: tx('s'), namehash: hash('s') };
    case 'setnamerecords':
    case 'updatename':
    case 'transfername':
    case 'releasename':
    case 'releasereservation':
    case 'sendpaymail':
      return tx('t');
    case 'getnewnamekeys':
      return { encryptionkey: `02${'1'.repeat(64)}`, signingkey: '2'.repeat(64) };
    case 'encryptnamemessage':
      return 'ee'.repeat(80);
    case 'decryptnamemessage':
      return 'the secret';
    case 'signasname':
      return 'ab'.repeat(64);
    case 'verifynamesignature':
      return p[2] === 'ab'.repeat(64);
    case 'listpaymail':
      return [{ txid: tx('m'), vout: 1, amount: 0.002, confirmations: 3, message: 'Hello <b>alice</b>' }, { txid: tx('q'), vout: 1, amount: 0.001, confirmations: 1 }];
    case 'resolvename':
      if (!String(p[0]).endsWith('.x')) return { status: 'REFUSED', records: [] };
      return String(p[0]).includes('alice') ? { status: 'NOERROR', records: [{ name: String(p[0]), type: 'A', data: '203.0.113.7' }] } : { status: 'NXDOMAIN', records: [] };
    case 'validateaddress':
      return { isvalid: true, address: p[0] };
    case 'bitnames.store.load':
      return structuredClone(store);
    case 'bitnames.store.save':
      store = structuredClone((r.params as { value: unknown }).value);
      return true;
    case 'bitnames.records.check': {
      const rows = (r.params as { rows: { type: string; value: string }[] }).rows;
      const bad = rows.find((x) => x.type === 'TXT' && x.value === 'too long');
      return bad ? { ok: false, error: 'The records take 1100 bytes, more than the 1024 a name holds' } : { ok: true, bytes: JSON.stringify(rows).length };
    }
    case 'bitnames.records.editable':
      return !JSON.stringify((r.params as { records: unknown }).records).includes('WWW');
    case 'app.notify':
    case 'app.copy':
      return true;
    default:
      return null;
  }
}

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  (globalThis as Record<string, unknown>).ResizeObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 20, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  registry = [
    nameInfo('a', { name: 'alice', owner: { txid: tx('a'), vout: 0, address: ME }, seq: '0001-0001', data: { ipv4: '203.0.113.7', paymailfee: 0.001, encryptionkey: `02${'3'.repeat(64)}`, records: { ip: ['203.0.113.7'] } } }),
    nameInfo('b', { name: 'bob', seq: '0001-0002', registered: 6, data: { encryptionkey: `03${'4'.repeat(64)}`, paymailfee: 0.0005 } }),
    nameInfo('c', { seq: '0001-0003', registered: 7 }),
    nameInfo('d', { name: 'blog.alice', parent: hash('a'), label: 'blog', seq: '0001-0004', registered: 8, owner: { address: OTHER } }),
  ];
  mine = [{ kind: 'name', name: 'alice', namehash: hash('a'), seq: '0001-0001', address: ME, txid: tx('a'), vout: 0 }];
  records = { [hash('a')]: [{ host: '@', type: 'A', value: '203.0.113.7' }, { host: 'www', type: 'CNAME', value: '@' }] };
  wallettx = {};
  store = null;
  info = { names: 0, reservations: 0, icann: false, revealdepth: 2, dns: '127.0.0.1:5335' };
  calls.length = 0;
  asked.length = 0;
  authorized.length = 0;
  resetNames();
  setTransport(fakeNode);
});

afterEach(() => {
  resetNames();
  setTransport(null);
  dialogs.queue.splice(0);
  toasts.list.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  nav.tab = '';
  nav.focus = null;
});

async function answer(choice: string) {
  await waitFor(() => expect(document.querySelector(`button[data-choice="${choice}"]`)).not.toBeNull());
  await fireEvent.click(document.querySelector(`button[data-choice="${choice}"]`)!);
}
const called = (method: string) => calls.filter((c) => c.method === method);
const loaded = () => waitFor(() => expect(bn.loaded && bn.wallet === 'w').toBe(true));

describe('the rules of names, as the wallet checks them', () => {
  it('reads what a search box was given', () => {
    expect(queryKind('')).toBe('empty');
    expect(queryKind('alice')).toBe('name');
    expect(queryKind('blog.alice')).toBe('subname');
    expect(queryKind('www.alice.x')).toBe('domain');
    expect(queryKind('#1739-0029')).toBe('seq');
    expect(queryKind(`0x${hash('a')}`)).toBe('hash');
    expect(queryKind('Alice')).toBe('bad');
    expect(queryKind('аlice')).toBe('bad');
    expect(withoutTld('www.alice.x.')).toBe('www.alice');
    expect(withoutTld('alice.com')).toBeNull();
  });

  it('says why a name or a subname cannot be had as it is typed', () => {
    expect(newNameProblem('alice')).toBe('');
    expect(newNameProblem('Alice')).toMatch(/lower case.*alice/);
    expect(newNameProblem('a b')).toMatch(/lower-case letters/);
    expect(newNameProblem('x'.repeat(256))).toMatch(/255/);
    expect(subnameProblem('blog.alice')).toBe('');
    expect(subnameProblem('-blog.alice')).toMatch(/label/);
    expect(subnameProblem('blog.Alice')).toMatch(/goes under/);
    expect(subnameProblem(`a.${Array(9).fill('b').join('.')}`)).toMatch(/8 levels/);
  });

  it('refuses xn-- (punycode) names and subnames, as the wallet does (audit 7 E-BN7)', () => {
    expect(newNameProblem('xn--pple-43d')).toMatch(/xn--/);
    expect(newNameProblem('xnapple')).toBe('');
    expect(subnameProblem('xn--pple-43d.alice')).toMatch(/xn--/);
    expect(subnameProblem('blog.alice')).toBe('');
  });

  it('warns about private and local addresses in records (audit 7 E-BN4)', () => {
    for (const a of ['10.1.2.3', '127.0.0.1', '192.168.1.1', '172.16.0.1', '172.31.255.255', '169.254.1.1', '100.64.0.1', '::1', 'fd00::1', 'fe80::1', '::ffff:192.168.0.1']) {
      expect(isPrivateAddress(a), a).toBe(true);
    }
    for (const a of ['203.0.113.7', '172.32.0.1', '8.8.8.8', '2001:db8::7', '::ffff:8.8.8.8']) expect(isPrivateAddress(a), a).toBe(false);
    expect(rowWarning({ host: '@', type: 'A', value: '192.168.1.10' })).toMatch(/private/);
    expect(rowWarning({ host: '@', type: 'A', value: '203.0.113.7' })).toBe('');
    expect(rowWarning({ host: '@', type: 'TXT', value: '10.0.0.1' })).toBe('');
  });

  it('checks records as they are typed, row by row', () => {
    expect(rowProblem({ host: '@', type: 'A', value: '203.0.113.7' })).toBe('');
    expect(rowProblem({ host: '@', type: 'A', value: '203.0.113.256' })).toMatch(/IPv4/);
    expect(rowProblem({ host: '@', type: 'AAAA', value: '2001:db8::7' })).toBe('');
    expect(rowProblem({ host: '@', type: 'AAAA', value: '203.0.113.7' })).toMatch(/IPv6/);
    expect(rowProblem({ host: 'www', type: 'CNAME', value: 'example.com.' })).toBe('');
    expect(rowProblem({ host: 'WWW', type: 'CNAME', value: '@' })).toMatch(/Host/);
    expect(rowProblem({ host: '*', type: 'MX', value: 'mail', priority: 70000 })).toMatch(/priority/i);
    expect(rowProblem({ host: '_sip._tcp', type: 'SRV', value: 'sip', priority: 10, weight: 5, port: 5060 })).toBe('');
    expect(rowProblem({ host: '@', type: 'TLSA', value: '3 1 1 ab12' })).toBe('');
    expect(rowProblem({ host: '@', type: 'TLSA', value: '3 1 ab12' })).toMatch(/TLSA/);
    expect(rowProblem({ host: 'shop', type: 'DELEGATE', value: 'blog.core' })).toBe('');
    expect(rowProblem({ host: 'shop', type: 'DELEGATE', value: 'Not a name' })).toMatch(/DELEGATE/);
    expect(rowProblem({ host: '@', type: 'TXT', value: '' })).toMatch(/value/);
    expect(isIPv6('::1')).toBe(true);
    expect(isIPv6('1:2:3:4:5:6:7:8:9')).toBe(false);
    expect(isIPv6('::ffff:192.0.2.1')).toBe(true);
    // The fields of its type only, as setnamerecords takes them.
    expect(rowForWallet({ host: '', type: 'MX', value: ' mail ', weight: 3 })).toEqual({ host: '@', type: 'MX', value: 'mail', priority: 10 });
    expect(rowForWallet({ host: 'x', type: 'SRV', value: 't', port: 443 })).toEqual({ host: 'x', type: 'SRV', value: 't', priority: 10, weight: 0, port: 443 });
  });

  it('checks the data of a name, and sends only what changed (null deletes)', () => {
    expect(dataProblem('ipv4', '203.0.113.7:443')).toBe('');
    expect(dataProblem('ipv4', '203.0.113.7:99999')).toMatch(/IPv4/);
    expect(dataProblem('ipv6', '[2001:db8::7]:443')).toBe('');
    expect(dataProblem('commitment', 'ab')).toMatch(/64 hex/);
    expect(dataProblem('encryptionkey', `04${'1'.repeat(64)}`)).toMatch(/02 or 03/);
    expect(dataProblem('signingkey', '2'.repeat(64))).toBe('');
    expect(dataProblem('paymailfee', '0.000000001')).toMatch(/amount/);
    const before = { ipv4: '203.0.113.7', paymailfee: 0.001 };
    expect(dataUpdates(before, { ipv4: '203.0.113.7', ipv6: '', commitment: '', encryptionkey: '', signingkey: '', paymailfee: '0.001' })).toEqual({});
    expect(dataUpdates(before, { ipv4: '198.51.100.1', ipv6: '::1', commitment: '', encryptionkey: '', signingkey: '', paymailfee: '' })).toEqual({ ipv4: '198.51.100.1', ipv6: '::1', paymailfee: null });
  });

  it('follows a registration step by step, with its blocks and deadlines', () => {
    const r: Registration = { wallet: 'w', name: 'carol', public: true, step: 'reserved', reserveTxid: tx('r'), started: 0, updated: 0 };
    expect(progressOf(r, 20, 2)).toMatchObject({ stage: 'confirm', index: 1 });
    // In block 21, revealdepth 2: the registration may be in block 23, so sent once the tip is 22.
    expect(progressOf({ ...r, reserveHeight: 21 }, 21, 2)).toMatchObject({ stage: 'wait', index: 2, revealFrom: 23, blocksLeft: 1 });
    expect(progressOf({ ...r, reserveHeight: 21 }, 22, 2)).toMatchObject({ stage: 'register', index: 3, blocksLeft: 0 });
    expect(progressOf({ ...r, step: 'registering', registerSent: 22 }, 23, 2)).toMatchObject({ stage: 'register', deadline: 24, late: false });
    expect(progressOf({ ...r, step: 'registering', registerSent: 22 }, 25, 2)).toMatchObject({ late: true });
    expect(progressOf({ ...r, step: 'done' }, 30, 2)).toMatchObject({ stage: 'yours', fraction: 1 });
    expect(blocksTime(0, 60)).toBe('the next block');
    expect(blocksTime(6, 60)).toBe('about 6 minutes');
    expect(blocksTime(120, 60)).toBe('about 2.0 hours');
  });
});

describe('the pages of BitNames', () => {
  it('are in the Chain section and the menus, before the tools, with their quick actions', () => {
    const own = (LOCAL_PAGES as readonly LocalPage[]).map((p) => p.id);
    expect(own).toEqual(['register', 'names', 'directory', 'paymail', 'dns']);
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toEqual(['peg', ...own, 'explorer']);
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toEqual(['peg', ...own, 'explorer']);
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toEqual(expect.arrayContaining(['Get a name', 'Paymail']));
    const chain = menuSections(buildCommands()).find((s) => s.group === 'Chains')!;
    for (const c of LOCAL_COMMANDS) expect(chain.items.map((i) => i.label)).toContain(c.label);
    chain.items.find((c) => c.label === 'Edit DNS records…')!.run();
    expect([nav.page, nav.tab]).toEqual(['names', 'records']);
    // Alt+6 opens My names, as it opened the names of bitnames-qt (audit port-2 #8).
    go('overview');
    expect(handleShortcut(new KeyboardEvent('keydown', { key: '6', altKey: true }))).toBe(true);
    expect(nav.page).toBe('names');
    expect(CHAIN.name).toBe('BitNames');
    expect(CHAIN.uriScheme).toBe('bitnames');
  });

  it('Get a name: whether it is free as it is typed, then reserved and registered by itself, block by block', async () => {
    render(Register);
    render(DialogHost);
    await loaded();
    const search = document.querySelector<HTMLInputElement>('#name-search')!;
    await fireEvent.input(search, { target: { value: 'Carol' } });
    await waitFor(() => expect(screen.getByText(/lower case/)).toBeTruthy());
    await fireEvent.input(search, { target: { value: 'bob' } });
    await waitFor(() => expect(screen.getByText(/Taken/)).toBeTruthy());
    await fireEvent.input(search, { target: { value: 'alice.x' } });
    await waitFor(() => expect(screen.getByText('Already yours')).toBeTruthy());
    await fireEvent.input(search, { target: { value: 'carol' } });
    await waitFor(() => expect(screen.getByText('is free')).toBeTruthy());
    await fireEvent.input(document.querySelector('#name-ipv4')!, { target: { value: '198.51.100.9' } });
    await fireEvent.click(document.querySelector('#name-get')!);
    await waitFor(() => expect(called('reservename').length).toBe(1));
    expect(called('reservename')[0].params).toEqual(['carol']);
    // One question, the host's, for both steps (no question of the page first).
    expect(asked).toEqual(['reservename+registername']);
    // Kept by the host, so that it goes on after a restart.
    await waitFor(() => expect((store as { registrations: Registration[] }).registrations).toMatchObject([{ wallet: 'w', name: 'carol', public: true, step: 'reserved', reserveTxid: tx('r'), data: { ipv4: '198.51.100.9' } }]));
    await waitFor(() => expect(screen.getByRole('listitem', { name: 'Registration of carol.x' })).toBeTruthy());
    expect(screen.getByText(/waits for a block/)).toBeTruthy();

    // In block 21: hidden one more block.
    wallettx[tx('r')] = { confirmations: 1, blockheight: 21 };
    node.tip = { ...node.tip!, height: 21, hash: 'cd'.repeat(32) };
    await refreshNames();
    await waitFor(() => expect(screen.getByText(/Hidden for 1 more block/)).toBeTruthy());
    expect(called('registername').length).toBe(0);
    // Deep enough with block 22: the registration goes out, with the data and public.
    node.tip = { ...node.tip!, height: 22, hash: 'ce'.repeat(32) };
    await refreshNames();
    await waitFor(() => expect(called('registername').length).toBe(1));
    expect(called('registername')[0].params).toEqual(['carol', { ipv4: '198.51.100.9' }, true]);
    // Without a second question.
    expect(asked).toEqual(['reservename+registername']);
    await waitFor(() => expect(screen.getByText(/should be in a block by block 24/)).toBeTruthy());
    // In block 23: yours.
    wallettx[tx('g')] = { confirmations: 1, blockheight: 23 };
    node.tip = { ...node.tip!, height: 23, hash: 'cf'.repeat(32) };
    await refreshNames();
    await waitFor(() => expect(screen.getByText('Yours since block 23.')).toBeTruthy());
    expect(toasts.list.some((t) => t.title === 'The name is yours')).toBe(true);
    expect((store as { registrations: Registration[] }).registrations[0]).toMatchObject({ step: 'done', registeredHeight: 23 });
  });

  it('Get a name: a locked wallet waits for the user, a name taken meanwhile stops it, a subname is made at once', async () => {
    wallets.list = [{ name: 'w', encrypted: true, locked: true, privateKeysDisabled: false, hd: true }];
    store = {
      registrations: [
        { wallet: 'w', name: 'dave', public: false, step: 'reserved', reserveTxid: tx('1'), started: 1, updated: 1 },
        { wallet: 'w', name: 'bob', public: false, step: 'reserved', reserveTxid: tx('2'), started: 2, updated: 2 },
        { wallet: 'w', name: 'BAD name', public: false, step: 'reserved', reserveTxid: tx('3'), started: 3, updated: 3 },
      ],
    };
    wallettx[tx('1')] = { confirmations: 3, blockheight: 18 };
    wallettx[tx('2')] = { confirmations: 3, blockheight: 18 };
    render(Register);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByText(/register it now \(the wallet asks for its passphrase\)/)).toBeTruthy());
    expect(called('registername').length).toBe(0);
    // Never a dialog by itself: its card asks for the user's confirmation.
    await waitFor(() => expect(screen.getAllByRole('button', { name: 'Confirm registration' }).length).toBeGreaterThan(0));
    expect(asked).toEqual([]);
    await waitFor(() => expect(screen.getByText(/Someone else registered this name first/)).toBeTruthy());
    // What does not have the shape of a registration is dropped.
    expect(screen.queryByText(/BAD name/)).toBeNull();

    await fireEvent.input(document.querySelector('#name-search')!, { target: { value: 'shop.alice' } });
    await waitFor(() => expect(screen.getByText('can be yours')).toBeTruthy());
    await fireEvent.click(document.querySelector('#name-get')!);
    await answer('yes');
    await waitFor(() => expect(called('createsubname').length + called('wallet.withKeys').length).toBeGreaterThan(0));
  });

  it('My names: the records edited and saved, the profile, a subname, giving away and releasing', async () => {
    render(MyNames);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('region', { name: 'A name' })).toBeTruthy());
    const view = screen.getByRole('region', { name: 'A name' });
    await waitFor(() => expect(within(view).getAllByRole('group').length).toBe(2));
    expect(within(view).getByText('Permanent')).toBeTruthy();
    // A row that is not an IPv4 address: said at once, nothing saved.
    const first = within(view).getAllByRole('group')[0];
    await fireEvent.input(within(first).getByRole('textbox', { name: 'Value' }), { target: { value: '2001:db8::7' } });
    await waitFor(() => expect(within(view).getByText(/A: an IPv4 address/)).toBeTruthy());
    expect((document.querySelector('#records-save') as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.change(within(first).getByRole('combobox', { name: 'Type' }), { target: { value: 'AAAA' } });
    await fireEvent.click(within(view).getByRole('button', { name: 'Mail server' }));
    await waitFor(() => expect(called('bitnames.records.check').length).toBeGreaterThan(0));
    await waitFor(() => expect((document.querySelector('#records-save') as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(document.querySelector('#records-save')!);
    await answer('yes');
    await waitFor(() => expect(called('setnamerecords').length).toBe(1));
    expect(called('setnamerecords')[0].params).toEqual([`0x${hash('a')}`, [
      { host: '@', type: 'AAAA', value: '2001:db8::7' },
      { host: 'www', type: 'CNAME', value: '@' },
      { host: '@', type: 'MX', value: 'mail', priority: 10 },
    ]]);

    // The profile: a new address, the paymail fee deleted, new keys.
    await fireEvent.click(within(view).getByRole('tab', { name: 'Profile' }));
    await waitFor(() => expect(document.querySelector('#profile-ipv4')).not.toBeNull());
    await fireEvent.input(document.querySelector('#profile-ipv4')!, { target: { value: '198.51.100.1:443' } });
    await fireEvent.input(document.querySelector('#profile-paymailfee')!, { target: { value: '' } });
    await fireEvent.click(within(view).getByRole('button', { name: /Use new keys/ }));
    await waitFor(() => expect((document.querySelector('#profile-signingkey') as HTMLInputElement).value).toBe('2'.repeat(64)));
    await fireEvent.click(document.querySelector('#profile-save')!);
    await answer('yes');
    await waitFor(() => expect(called('updatename').length).toBe(1));
    expect(called('updatename')[0].params).toEqual([`0x${hash('a')}`, { ipv4: '198.51.100.1:443', encryptionkey: `02${'1'.repeat(64)}`, signingkey: '2'.repeat(64), paymailfee: null }]);

    // Its subnames, and a new one.
    await fireEvent.click(within(view).getByRole('tab', { name: 'Subnames' }));
    await waitFor(() => expect(within(view).getByRole('table', { name: 'Subnames' })).toBeTruthy());
    expect(within(view).getByText('blog.alice')).toBeTruthy();
    // Asked of the node by their parent, in one call: none left out past 10 000 names (audit port-2 #11).
    expect(called('listnames').filter((c) => (c.params as unknown[])[3] !== undefined).map((c) => c.params)).toEqual([[2000, 0, null, `0x${hash('a')}`]]);
    await fireEvent.input(document.querySelector('#subname-label')!, { target: { value: 'shop' } });
    await fireEvent.click(document.querySelector('#subname-create')!);
    await answer('yes');
    await waitFor(() => expect(called('createsubname').length).toBe(1));
    expect(called('createsubname')[0].params).toEqual([`0x${hash('a')}`, 'shop']);

    // Given away, keys deleted; then released only with the box ticked.
    await fireEvent.click(within(view).getByRole('tab', { name: 'Give or release' }));
    await fireEvent.input(document.querySelector('#give-to')!, { target: { value: OTHER } });
    await waitFor(() => expect((document.querySelector('#give-send') as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(document.querySelector('#give-send')!);
    await answer('yes');
    await waitFor(() => expect(called('transfername').length).toBe(1));
    expect(called('transfername')[0].params).toEqual([`0x${hash('a')}`, OTHER, true]);
  });

  it('My names: in a narrow window, the list or the name picked, with a way back (audit port-2 #6)', async () => {
    (narrowWindow as { matches: boolean }).matches = true;
    try {
      mine = [
        { kind: 'name', name: 'alice', namehash: hash('a'), seq: '0001-0001', address: ME, txid: tx('a'), vout: 0 },
        ...Array.from({ length: 300 }, (_, i) => ({ kind: 'name' as const, name: `n${String(i).padStart(3, '0')}`, namehash: `7${(i + 1).toString(16).padStart(63, '0')}`, seq: `0002-${String(i).padStart(4, '0')}`, address: ME, txid: tx('a'), vout: i + 1 })),
      ];
      render(MyNames);
      await loaded();
      await waitFor(() => expect(screen.getByRole('region', { name: 'The names of the wallet' })).toBeTruthy());
      await refreshNames();
      await waitFor(() => expect(bn.mine.length).toBe(301));
      // The list alone: no name under it.
      expect(screen.queryByRole('region', { name: 'A name' })).toBeNull();
      const alice = await waitFor(() => {
        const b = within(screen.getByRole('region', { name: 'The names of the wallet' })).getAllByRole('button').find((x) => /alice/.test(x.textContent ?? ''));
        expect(b).toBeTruthy();
        return b!;
      });
      await fireEvent.click(alice);
      // The name alone, at the top, with the way back.
      await waitFor(() => expect(screen.getByRole('region', { name: 'A name' })).toBeTruthy());
      expect(screen.queryByRole('region', { name: 'The names of the wallet' })).toBeNull();
      await fireEvent.click(screen.getByRole('button', { name: /All 301 names/ }));
      await waitFor(() => expect(screen.getByRole('region', { name: 'The names of the wallet' })).toBeTruthy());
      expect(screen.queryByRole('region', { name: 'A name' })).toBeNull();
    } finally {
      (narrowWindow as { matches: boolean }).matches = false;
    }
  });

  it('My names: a new block keeps what is being typed in the records and the profile', async () => {
    go('names', 'profile');
    render(MyNames);
    await loaded();
    await waitFor(() => expect(document.querySelector('#profile-ipv4')).not.toBeNull());
    await fireEvent.input(document.querySelector('#profile-ipv4')!, { target: { value: '198.51.100.2' } });
    node.tip = { ...node.tip!, height: 21, hash: 'dd'.repeat(32) };
    await refreshNames();
    await waitFor(() => expect(called('getname').length).toBeGreaterThan(1));
    expect((document.querySelector('#profile-ipv4') as HTMLInputElement).value).toBe('198.51.100.2');
    // The data changed on the chain: the form shows it.
    registry[0] = { ...registry[0], data: { ...registry[0].data, ipv4: '192.0.2.1' } };
    await refreshNames();
    await waitFor(() => expect((document.querySelector('#profile-ipv4') as HTMLInputElement).value).toBe('192.0.2.1'));
  });

  it('My names: a name whose change waits for a block stays, marked, until the change is in one', async () => {
    go('names', 'profile');
    render(MyNames);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.querySelector('#profile-ipv4')).not.toBeNull());
    await fireEvent.input(document.querySelector('#profile-ipv4')!, { target: { value: '198.51.100.3' } });
    await fireEvent.click(document.querySelector('#profile-save')!);
    await answer('yes');
    await waitFor(() => expect(called('updatename').length).toBe(1));
    // The wallet no longer lists the name: its output is spent by a transaction not in a block yet.
    mine = [];
    wallettx[tx('t')] = { confirmations: 0 };
    await refreshNames();
    await waitFor(() => expect(screen.getByText(/a change waits for a block/)).toBeTruthy());
    expect(screen.getByText(/A change of this name waits for the next block/)).toBeTruthy();
    // In a block: listed again, no longer marked.
    mine = [{ kind: 'name', name: 'alice', namehash: hash('a'), seq: '0001-0001', address: ME, txid: tx('t'), vout: 0 }];
    wallettx[tx('t')] = { confirmations: 1, blockheight: 21 };
    await refreshNames();
    await waitFor(() => expect(screen.queryByText(/a change waits for a block/)).toBeNull());
    await waitFor(() => expect(document.querySelector('.bn-item[aria-current="true"]')?.textContent).toContain('alice'));
  });

  it('My names: releasing asks for the box to be ticked', async () => {
    go('names', 'transfer');
    render(MyNames);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.querySelector('#release-name')).not.toBeNull());
    await fireEvent.click(document.querySelector('#release-name')!);
    await answer('yes');
    await waitFor(() => expect(screen.getByText(/Nothing was released/)).toBeTruthy());
    expect(called('releasename').length).toBe(0);
    await answer('ok').catch(() => undefined);
    dialogs.queue.splice(0);
    await fireEvent.click(document.querySelector('#release-name')!);
    await waitFor(() => expect(document.querySelector('dialog[open] input[type="checkbox"]')).not.toBeNull());
    await fireEvent.click(document.querySelector('dialog[open] input[type="checkbox"]')!);
    await answer('yes');
    await waitFor(() => expect(called('releasename').length).toBe(1));
    expect(called('releasename')[0].params).toEqual([`0x${hash('a')}`, true]);
  });

  it('the directory: newest first, a search by text, a name by its number, read only with its history', async () => {
    render(Directory);
    await loaded();
    await waitFor(() => expect(screen.getAllByRole('listitem').length).toBeGreaterThan(3));
    const items = screen.getAllByRole('button').filter((b) => b.classList.contains('bn-item'));
    // The newest (blog.alice, number 4) first; a private name by its number.
    expect(items[0].textContent).toContain('blog.alice');
    expect(screen.getAllByText('#0001-0003').length).toBeGreaterThan(0);
    // A short placeholder, what it takes said under it (audit port-2 #14: cut at 480 px).
    expect((document.querySelector('#directory-search') as HTMLInputElement).placeholder).toBe('Find a name');
    expect(document.getElementById('directory-search-hint')?.textContent).toContain('#1739-0029');
    await fireEvent.input(document.querySelector('#directory-search')!, { target: { value: 'bo' } });
    await waitFor(() => expect(called('listnames').some((c) => (c.params as unknown[])[2] === 'bo')).toBe(true));
    await fireEvent.input(document.querySelector('#directory-search')!, { target: { value: '#0001-0002' } });
    await fireEvent.submit(document.querySelector('form[role="search"]')!);
    await waitFor(() => expect(screen.getByRole('region', { name: 'A name' })).toBeTruthy());
    const view = screen.getByRole('region', { name: 'A name' });
    await waitFor(() => expect(within(view).getByText('bob')).toBeTruthy());
    // Read only: no editor, no giving away.
    expect(within(view).queryByRole('tab', { name: 'Give or release' })).toBeNull();
    expect(document.querySelector('#records-save')).toBeNull();
    await fireEvent.click(within(view).getByRole('tab', { name: 'History' }));
    await waitFor(() => expect(within(view).getByRole('table', { name: 'History' })).toBeTruthy());
    expect(within(view).getByText('IPv4 address')).toBeTruthy();
  });

  it('Paymail: the inbox shown as it is, a paid message to a name, signing and checking', async () => {
    render(Paymail);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('table', { name: 'Paymail received' })).toBeTruthy());
    // The text of whoever sent it, never rendered.
    expect(screen.getByText('Hello <b>alice</b>')).toBeTruthy();
    expect(screen.getByText('Hello <b>alice</b>').getAttribute('translate')).toBe('no');
    go('paymail', 'send');
    await waitFor(() => expect(document.querySelector('#paymail-to')).not.toBeNull());
    await fireEvent.input(document.querySelector('#paymail-to')!, { target: { value: 'bob' } });
    await waitFor(() => expect(screen.getByText(/takes paid messages of at least/)).toBeTruthy());
    await waitFor(() => expect((document.querySelector('#paymail-amount') as HTMLInputElement).value).toBe('0.0005'));
    await fireEvent.input(document.querySelector('#paymail-text')!, { target: { value: 'Hi Bob' } });
    await waitFor(() => expect((document.querySelector('#paymail-send') as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(document.querySelector('#paymail-send')!);
    await answer('yes');
    await waitFor(() => expect(called('sendpaymail').length).toBe(1));
    expect(called('sendpaymail')[0].params).toEqual(['bob', '0.0005', 'Hi Bob']);
    // A name that takes no messages.
    await fireEvent.input(document.querySelector('#paymail-to')!, { target: { value: '#0001-0003' } });
    await waitFor(() => expect(screen.getByText(/no encryption key/)).toBeTruthy());

    go('paymail', 'tools');
    await waitFor(() => expect(document.querySelector('#tool-name')).not.toBeNull());
    await fireEvent.input(document.querySelector('#tool-name')!, { target: { value: 'alice' } });
    await fireEvent.input(document.querySelector('#tool-text')!, { target: { value: 'I am alice' } });
    await fireEvent.click(screen.getByRole('button', { name: /Sign as the name/ }));
    await waitFor(() => expect(called('signasname').length).toBe(1));
    await fireEvent.input(document.querySelector('#tool-signature')!, { target: { value: 'ab'.repeat(64) } });
    await fireEvent.click(screen.getByRole('button', { name: /Check the signature/ }));
    await waitFor(() => expect(screen.getByText(/Good: the holder of this name signed this text/)).toBeTruthy());
  });

  it('Paymail: a locked wallet opens its messages only when asked', async () => {
    wallets.list = [{ name: 'w', encrypted: true, locked: true, privateKeysDisabled: false, hd: true }];
    render(Paymail);
    await loaded();
    await waitFor(() => expect(screen.getByRole('button', { name: /Unlock and read/ })).toBeTruthy());
    expect(called('listpaymail').length).toBe(0);
  });

  it('DNS: the server on or off, the commands of each system, a lookup', async () => {
    const page = render(Dns);
    await loaded();
    await waitFor(() => expect(screen.getByText(/This node answers DNS for .x/)).toBeTruthy());
    expect(screen.getByText('resolvectl dns lo 127.0.0.1:5335')).toBeTruthy();
    await fireEvent.click(screen.getByRole('tab', { name: 'Windows' }));
    expect(screen.getByText('Add-DnsClientNrptRule -Namespace ".x" -NameServers 127.0.0.1')).toBeTruthy();
    await fireEvent.input(document.querySelector('#dns-domain')!, { target: { value: 'www.alice' } });
    await fireEvent.change(screen.getByRole('combobox', { name: 'Record type' }), { target: { value: 'A' } });
    await fireEvent.click(document.querySelector('#dns-lookup')!);
    await waitFor(() => expect(called('resolvename').length).toBe(1));
    expect(called('resolvename')[0].params).toEqual(['www.alice.x', 'A']);
    await waitFor(() => expect(screen.getByRole('table', { name: 'Answer' })).toBeTruthy());
    expect(screen.getByText('NOERROR')).toBeTruthy();
    page.unmount();

    resetNames();
    info.dns = undefined;
    render(Dns);
    await loaded();
    await waitFor(() => expect(screen.getByText(/does not answer DNS/)).toBeTruthy());
    expect(screen.getAllByText('dnsbind=127.0.0.1:5335').length).toBeGreaterThan(0);
  });

  it('the cards of the home: my names, what is coming up, the names registered last', async () => {
    store = { registrations: [{ wallet: 'w', name: 'erin', public: true, step: 'reserved', reserveTxid: tx('5'), started: 1, updated: 1 }] };
    render(LocalCards);
    await waitFor(() => expect(screen.getByRole('heading', { name: 'My names' })).toBeTruthy());
    expect(screen.getByRole('heading', { name: 'Coming up' })).toBeTruthy();
    expect(screen.getByRole('heading', { name: 'Registered last' })).toBeTruthy();
    await waitFor(() => expect(screen.getByText('erin')).toBeTruthy());
    expect(screen.getByText('blog.alice')).toBeTruthy();
  });

  it('shows the text of the chain as it is: markup, and a name that turns text around', async () => {
    registry.push(nameInfo('e', { name: '<img src=x onerror=alert(1)>', seq: '0001-0005', registered: 9 }));
    registry.push(nameInfo('f', { name: '‮evil', seq: '0001-0006', registered: 10 }));
    render(Directory);
    await loaded();
    await waitFor(() => expect(screen.getByText('<img src=x onerror=alert(1)>')).toBeTruthy());
    expect(document.querySelector('img')).toBeNull();
    const evil = screen.getByText('‮evil');
    expect(evil.getAttribute('dir')).toBe('auto');
    expect(evil.getAttribute('translate')).toBe('no');
  });
});

// Simple mode (lib/uimode.svelte.ts, user decision 2026-10-16): Get a name first, in plain words (free or not,
// what it costs); My names in a few words, records and keys behind More; the directory, paymail and DNS in
// Advanced. The calls are those of Advanced.
describe('Simple mode on BitNames', () => {
  beforeEach(() => {
    prefs.uiMode = 'simple';
  });

  it('Get a name first; the directory, paymail and DNS leave the navigation', async () => {
    const { pageShown } = await import('../lib/uimode.svelte');
    expect(pageShown('register' as never)).toBe(true);
    expect(pageShown('names' as never)).toBe(true);
    for (const id of ['directory', 'paymail', 'dns']) expect(pageShown(id as never), id).toBe(false);
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages.indexOf('register' as never)).toBeLessThan(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages.indexOf('names' as never));
    expect(buildCommands().some((c) => c.id === 'page.paymail')).toBe(true);
  });

  it('Get a name: free or taken, what it costs, and taken with the call of Advanced', async () => {
    render(Register);
    render(DialogHost);
    await loaded();
    const search = document.querySelector<HTMLInputElement>('#name-search')!;
    await fireEvent.input(search, { target: { value: 'bob' } });
    await waitFor(() => expect(screen.getByText(/Taken/)).toBeTruthy());
    await fireEvent.input(search, { target: { value: 'carol' } });
    await waitFor(() => expect(screen.getByText('is free')).toBeTruthy());
    expect(screen.getByText(/What it costs: nothing but two small network/)).toBeTruthy();
    expect(document.querySelector('#name-ipv4')).toBeNull();
    await fireEvent.click(document.querySelector('#name-get')!);
    await waitFor(() => expect(called('reservename').length).toBe(1));
    expect(called('reservename')[0].params).toEqual(['carol']);
    await waitFor(() => expect((store as { registrations: Registration[] }).registrations).toMatchObject([{ wallet: 'w', name: 'carol', public: true, step: 'reserved' }]));
    expect(screen.queryByText(/Reveal depth/)).toBeNull();
  });

  it('My names: the name in a few words, its records behind More, given away with the call of Advanced', async () => {
    render(MyNames);
    render(DialogHost);
    await loaded();
    const view = await waitFor(() => screen.getByRole('region', { name: 'A name' }));
    await waitFor(() => expect(within(view).getByText(/yours for good/)).toBeTruthy());
    expect(within(view).queryByRole('tab', { name: 'Records' })).toBeNull();
    expect(within(view).queryByText('Hash')).toBeNull();
    await fireEvent.click(within(view).getByRole('button', { name: 'Give it away…' }));
    await waitFor(() => expect(document.querySelector('#give-to')).not.toBeNull());
    await fireEvent.input(document.querySelector('#give-to')!, { target: { value: OTHER } });
    await waitFor(() => expect((document.querySelector('#give-send') as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(document.querySelector('#give-send')!);
    await answer('yes');
    await waitFor(() => expect(called('transfername').length).toBe(1));
    expect(called('transfername')[0].params).toEqual([`0x${hash('a')}`, OTHER, true]);
    // More: every tab of Advanced.
    await fireEvent.click(within(view).getByRole('tab', { name: 'Records' }));
    expect(within(view).getAllByRole('tab').length).toBe(5);
  });

  it('the cards of the home: my names and what is coming up', async () => {
    render(LocalCards);
    await waitFor(() => expect(screen.getByRole('heading', { name: 'My names' })).toBeTruthy());
    expect(screen.queryByRole('heading', { name: 'Registered last' })).toBeNull();
    expect(screen.queryByText('DNS of .x')).toBeNull();
  });
});
