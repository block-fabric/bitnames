// The state of the names, kept up to date while the app runs: what the node says (getbitnamesinfo, the names
// registered last) and what the wallet holds (listmynames), asked again with every block of this chain, one
// refresh at a time.
//
// And the registrations this app is making: a name is taken in two steps (a reservation that hides it, then,
// once the reservation is revealdepth blocks deep, the registration that reveals it). The text of the name and
// its step are kept by the host in a file of the data directory (bitnames.store.*, src/webui/local.cpp), as
// chains-qt kept them in its settings, so that the registration goes on after a restart. With every block the
// store takes each one a step further: the block of the reservation, the registration sent once it may be in
// the next block (at once if the wallet needs no passphrase; else the page asks), the block that registered it.
import { request } from '../../lib/bridge';
import { errorText, walletRpc, walletRpcIfAuthorized, walletRpcTwoSteps } from '../../lib/dialogs.svelte';
import { go } from '../../lib/nav.svelte';
import { publishPending, type PendingOp } from '../../lib/pending.svelte';
import { t } from '../../lib/i18n.svelte';
import { every } from '../../lib/poll';
import { rpc } from '../../lib/rpc';
import { node, toast, wallets } from '../../lib/state.svelte';
import type { BitNamesInfo, MyName, NameInfo, Registration } from './model';

/** Registrations done are shown this long (seconds), then forgotten. */
const KEEP_DONE = 7 * 24 * 3600;
const MAX_REGISTRATIONS = 200;
/** The names registered last, for the home and the directory. */
export const RECENT = 8;

export const bn = $state({
  info: null as BitNamesInfo | null,
  /** The names and reservations of the wallet loaded. */
  mine: [] as MyName[],
  /** The names registered last, newest first. */
  recent: [] as NameInfo[],
  /** The registrations this app is making, of every wallet. */
  registrations: [] as Registration[],
  /**
   * Names of the wallet whose last change waits for a block: the wallet does not list a name while the output
   * that carries it is spent by a transaction not in a block yet. Shown meanwhile, marked; `leaving`: given
   * away or released.
   */
  inflight: [] as { wallet: string; txid: string; name: MyName; leaving: boolean }[],
  /** The wallet `mine` is of. */
  wallet: null as string | null,
  height: -1,
  /** Why the node could not be asked. */
  error: '',
  loaded: false,
  /** Counts the refreshes. */
  version: 0,
});

let storeLoaded = false;
let storeWarned = false;

function sanitize(raw: unknown): Registration[] {
  const list = raw && typeof raw === 'object' && Array.isArray((raw as { registrations?: unknown }).registrations) ? (raw as { registrations: unknown[] }).registrations : [];
  const now = Date.now() / 1000;
  return list
    .filter((r): r is Registration => {
      if (!r || typeof r !== 'object') return false;
      const x = r as Record<string, unknown>;
      return typeof x.wallet === 'string' && typeof x.name === 'string' && /^[a-z0-9_-]{1,255}$/.test(x.name) && typeof x.reserveTxid === 'string' && /^[0-9a-f]{64}$/.test(x.reserveTxid) && ['reserved', 'registering', 'done', 'failed'].includes(String(x.step));
    })
    .map((r) => ({ ...r, public: !!r.public, started: Number(r.started) || now, updated: Number(r.updated) || now, needsConfirm: undefined }))
    .filter((r) => !(r.step === 'done' && now - r.updated > KEEP_DONE))
    .slice(-MAX_REGISTRATIONS);
}

async function loadStore(): Promise<void> {
  if (storeLoaded) return;
  try {
    bn.registrations = sanitize(await request('host', 'bitnames.store.load', {}));
    storeLoaded = true;
  } catch {
    // The node is starting: asked again with the next refresh.
  }
}

async function saveStore(): Promise<void> {
  publish();
  try {
    // What the host would ask about now is not kept: after a restart it is known again.
    const registrations = $state.snapshot(bn.registrations).map(({ needsConfirm: _, ...r }) => r);
    await request('host', 'bitnames.store.save', { value: { registrations } });
  } catch (e) {
    if (!storeWarned) {
      storeWarned = true;
      toast('error', t('The registrations could not be saved'), errorText(e));
    }
  }
}

/** A change of a name of the wallet sent (txid): the name is shown, marked, until the change is in a block. */
export function noteChange(wallet: string, txid: string, namehash: string, leaving = false): void {
  const name = bn.mine.find((m) => m.kind === 'name' && m.namehash === namehash) ?? bn.inflight.find((x) => x.name.namehash === namehash)?.name;
  if (!name) return;
  bn.inflight = [...bn.inflight.filter((x) => !(x.wallet === wallet && x.name.namehash === namehash)), { wallet, txid, name, leaving }];
}

/** The names of the wallet: those it lists, and those whose change waits for a block. */
export function namesOf(wallet: string | null): (MyName & { waiting?: boolean; leaving?: boolean })[] {
  const listed = bn.wallet === wallet ? bn.mine.filter((m) => m.kind === 'name' && m.namehash) : [];
  const waiting = bn.inflight.filter((x) => x.wallet === wallet && !listed.some((m) => m.namehash === x.name.namehash)).map((x) => ({ ...x.name, waiting: true, leaving: x.leaving }));
  return [...listed.map((m) => ({ ...m, waiting: bn.inflight.some((x) => x.wallet === wallet && x.name.namehash === m.namehash) })), ...waiting];
}

/** The registrations of a wallet, newest first. */
export function registrationsOf(wallet: string | null): Registration[] {
  return bn.registrations.filter((r) => r.wallet === wallet).sort((a, b) => b.started - a.started);
}

/** The wallet needs its passphrase for what signs. */
export function walletLocked(wallet: string | null): boolean {
  return !!wallets.list.find((w) => w.name === wallet)?.locked;
}

function update(r: Registration, changes: Partial<Registration>): void {
  const i = bn.registrations.findIndex((x) => x.wallet === r.wallet && x.reserveTxid === r.reserveTxid);
  if (i < 0) return;
  bn.registrations[i] = { ...bn.registrations[i], ...changes, updated: Math.floor(Date.now() / 1000) };
}

interface WalletTx {
  confirmations: number;
  blockheight?: number;
}

/** The block of a transaction of the wallet: its height, 0 while it waits, -1 if it lost to another, null if unknown. */
async function blockOf(wallet: string, txid: string): Promise<number | null> {
  try {
    const tx = await rpc.raw<WalletTx>('gettransaction', [txid], { wallet });
    if (tx.confirmations < 0) return -1;
    return tx.confirmations > 0 && typeof tx.blockheight === 'number' ? tx.blockheight : 0;
  } catch {
    return null;
  }
}

/**
 * Begin taking a name: the reservation. Resolves with the registration, null when the user gave no passphrase;
 * throws what the wallet refuses (the name taken, no coins for the fee).
 */
export async function beginRegistration(wallet: string, name: string, isPublic: boolean, data: Record<string, string> = {}): Promise<Registration | null> {
  await loadStore();
  // Both steps in one question of the host: the reservation now, and the registration exactly as shown, which the app
  // then sends by itself without asking again (confirm.h, Reveal; local.cpp RegisterAfterReserve).
  const done = await walletRpcTwoSteps<{ txid: string; namehash: string }>(wallet, 'reservename', [name], { method: 'registername', params: registerParams({ name, data, public: isPublic }) });
  if (!done) return null;
  const now = Math.floor(Date.now() / 1000);
  const r: Registration = { wallet, name, public: isPublic, step: 'reserved', reserveTxid: done.txid, started: now, updated: now, ...(Object.keys(data).length ? { data } : {}) };
  bn.registrations = [...bn.registrations, r].slice(-MAX_REGISTRATIONS);
  await saveStore();
  void refreshNames();
  return r;
}

/**
 * Send the registration of a reservation deep enough. `interactive`: the user asked (a passphrase may be asked
 * for); else only a wallet that needs none, and an error that says the reservation is not deep enough yet is
 * kept quiet.
 */
export async function registerNow(r: Registration, interactive: boolean): Promise<boolean> {
  try {
    let txid: string;
    if (interactive) {
      const done = await walletRpc<{ txid: string }>(r.wallet, 'registername', registerParams(r));
      if (!done) return false;
      txid = done.txid;
    } else {
      // By itself: only what the user accepted with the reservation, never a question; else its card asks.
      const done = await walletRpcIfAuthorized<{ txid: string }>(r.wallet, 'registername', registerParams(r));
      if ('needed' in done) {
        if (r.needsConfirm === undefined) {
          update(r, { needsConfirm: done.needed });
          toast('info', t('%1 is ready to register', `${r.name}.x`), t('Confirm its registration on its card in Get a name.'), 12000, [{ label: t('Confirm'), run: () => void registerByUser(r) }]);
          await saveStore();
        }
        return false;
      }
      txid = done.result.txid;
    }
    update(r, { step: 'registering', registerTxid: txid, registerSent: node.tip?.height ?? bn.height, error: undefined, needsConfirm: undefined });
    await saveStore();
    return true;
  } catch (e) {
    const why = errorText(e);
    if (!interactive && /can be revealed from block/.test(why)) return false;
    if (!interactive && r.error !== why) toast('error', t('%1 was not registered', `${r.name}.x`), why, 12000, [{ label: t('Try again'), run: () => void registerByUser(r) }]);
    update(r, { error: why });
    await saveStore();
    if (interactive) throw e;
    return false;
  }
}

/** The parameters of registername for a registration: [name, data, public], the same in its question and when sent. */
export function registerParams(r: Pick<Registration, 'name' | 'data' | 'public'>): unknown[] {
  return [r.name, r.data ?? {}, r.public];
}

/** The user registers it from its card or a toast (asked as the host asks). */
async function registerByUser(r: Registration): Promise<void> {
  const now = bn.registrations.find((x) => x.wallet === r.wallet && x.reserveTxid === r.reserveTxid) ?? r;
  try {
    await registerNow(now, true);
  } catch {
    // Said on its card.
  }
}

/**
 * The progress of a registration, for its card (components/PendingCard.svelte) and the indicator of the top bar:
 * reserved, waiting for the depth (block k of revealdepth), the user's word needed, sent, registered.
 */
export function pendingOpOf(r: Registration, tip = bn.height, revealdepth = bn.info?.revealdepth ?? 1): PendingOp {
  const needed = Math.max(1, revealdepth);
  const base = { key: `${r.wallet}\u0000${r.reserveTxid}`, what: t('Name'), name: `${r.name}.x`, wallet: r.wallet, needed, page: 'register' as const };
  if (r.step === 'done') return { ...base, stage: 'done', depth: needed, ended: r.updated * 1000 };
  if (r.step === 'failed') return { ...base, stage: 'failed', depth: null, error: r.error, ended: r.updated * 1000 };
  if (r.step === 'registering') return { ...base, stage: 'registering', depth: needed };
  if (r.reserveHeight === undefined) return { ...base, stage: 'reserved', depth: null };
  const left = Math.max(0, r.reserveHeight + revealdepth - (tip + 1));
  const depth = Math.max(0, needed - left);
  if (left > 0) return { ...base, stage: 'waiting', depth };
  if (r.needsConfirm !== undefined || r.error) {
    return { ...base, stage: 'confirm', depth, error: r.error, action: { label: r.error ? 'Try again' : 'Confirm registration', run: () => void registerByUser(r) } };
  }
  return { ...base, stage: 'waiting', depth };
}

/** What this store is doing, for the indicator of the top bar (lib/pending.svelte.ts). */
function publish(): void {
  const wallet = wallets.current;
  publishPending('bitnames', bn.registrations.filter((r) => r.wallet === wallet).map((r) => pendingOpOf(r)));
}

/** Give up a registration: its reservation released, if it is in a block. */
export async function releaseRegistration(r: Registration): Promise<boolean> {
  if (r.reserveHeight !== undefined && r.step !== 'done') {
    const done = await walletRpc<string>(r.wallet, 'releasereservation', [r.name]);
    if (!done) return false;
  }
  forget(r);
  return true;
}

/** Take a registration off the list (done, failed, or given up). */
export function forget(r: Registration): void {
  bn.registrations = bn.registrations.filter((x) => !(x.wallet === r.wallet && x.reserveTxid === r.reserveTxid));
  void saveStore();
}

/** Each registration of the wallet a step further, with the block `tip`. */
async function advance(wallet: string, tip: number, info: BitNamesInfo, mine: MyName[]): Promise<boolean> {
  let changed = false;
  for (const r of bn.registrations.filter((x) => x.wallet === wallet && (x.step === 'reserved' || x.step === 'registering'))) {
    if (r.step === 'reserved') {
      let height = r.reserveHeight;
      if (height === undefined) {
        const at = await blockOf(wallet, r.reserveTxid);
        if (at === -1 || at === null) {
          update(r, { step: 'failed', error: at === -1 ? t('The reservation was replaced by another transaction, or dropped.') : t('The wallet does not know the transaction of the reservation.') });
          changed = true;
          continue;
        }
        if (at === 0) continue;
        height = at;
        update(r, { reserveHeight: at });
        changed = true;
      }
      // Taken meanwhile: by this wallet (registered from elsewhere), or by someone else.
      const taken = await rpc.raw<NameInfo>('getname', [r.name]).catch(() => null);
      if (taken) {
        const ours = mine.some((m) => m.kind === 'name' && m.namehash === taken.namehash);
        update(r, ours ? { step: 'done', registeredHeight: taken.registered, error: undefined } : { step: 'failed', error: t('Someone else registered this name first. Release the reservation to get its output back.') });
        changed = true;
        continue;
      }
      if (tip + 1 - height >= info.revealdepth) changed = (await registerNow({ ...r, reserveHeight: height }, false)) || changed;
    } else if (r.registerTxid) {
      const at = await blockOf(wallet, r.registerTxid);
      if (at === -1) {
        // Lost to another transaction: the reservation may still be there, to register again.
        update(r, { step: 'reserved', registerTxid: undefined, registerSent: undefined, error: t('The registration was replaced by another transaction: it is sent again.') });
        changed = true;
      } else if (at && at > 0) {
        update(r, { step: 'done', registeredHeight: at, error: undefined });
        toast('success', t('The name is yours'), t('%1 was registered in block %2.', `${r.name}.x`, at), 8000, [{ label: t('Open'), run: () => go('names') }]);
        changed = true;
      }
    }
  }
  return changed;
}

let running = false;
let again = false;

/** Ask the node again: at most one refresh at a time, and one more if asked meanwhile. */
export async function refreshNames(): Promise<void> {
  if (running) {
    again = true;
    return;
  }
  running = true;
  try {
    do {
      again = false;
      await refreshOnce();
    } while (again);
  } finally {
    running = false;
  }
}

async function refreshOnce(): Promise<void> {
  await loadStore();
  const wallet = wallets.current;
  const tip = node.tip?.height ?? -1;
  try {
    const info = await rpc.raw<BitNamesInfo>('getbitnamesinfo');
    if (!info || typeof info.revealdepth !== 'number') throw new Error(t('This node does not run BitNames.'));
    const skip = Math.max(0, info.names - RECENT);
    const [recentRaw, mineRaw] = await Promise.all([
      rpc.raw<NameInfo[]>('listnames', [RECENT, skip]).catch(() => [] as NameInfo[]),
      wallet !== null ? rpc.raw<MyName[]>('listmynames', [], { wallet }).catch(() => [] as MyName[]) : Promise.resolve([] as MyName[]),
    ]);
    const recent = Array.isArray(recentRaw) ? recentRaw.slice().reverse() : [];
    const mine = Array.isArray(mineRaw) ? mineRaw : [];
    bn.info = info;
    bn.recent = recent;
    bn.mine = mine;
    bn.wallet = wallet;
    bn.height = tip;
    bn.error = '';
    bn.loaded = true;
    if (wallet !== null) {
      // A change in a block (or lost): the wallet lists the name again, or not (given away, released).
      const still: typeof bn.inflight = [];
      for (const x of bn.inflight) {
        if (x.wallet !== wallet) {
          still.push(x);
          continue;
        }
        const at = await blockOf(wallet, x.txid);
        if (at === 0) still.push(x);
      }
      bn.inflight = still;
    }
    if (wallet !== null && storeLoaded && (await advance(wallet, tip, info, mine))) await saveStore();
    publish();
  } catch (e) {
    bn.error = errorText(e);
    bn.loaded = true;
  }
  bn.version++;
}

let lastTip = '';
let lastWallet: string | null | undefined;
let timer: (() => void) | null = null;

/**
 * Follow the chain: a refresh with every new block and with a change of wallet, and every 30 seconds (a
 * transaction of a name sent from the console). The pages and the cards of the home call it; the first starts it.
 */
export function startNames(): void {
  const tick = () => {
    const tip = node.tip?.hash ?? '';
    if (tip !== lastTip || wallets.current !== lastWallet) {
      lastTip = tip;
      lastWallet = wallets.current;
      void refreshNames();
    }
  };
  tick();
  if (timer) return;
  let ticks = 0;
  // Resting while the window is hidden, as the timers of the pages (lib/poll.ts).
  timer = every(3000, () => {
    tick();
    if (++ticks % 10 === 0) void refreshNames();
  });
}

/** For the tests. */
export function resetNames(): void {
  timer?.();
  timer = null;
  lastTip = '';
  lastWallet = undefined;
  storeLoaded = false;
  storeWarned = false;
  Object.assign(bn, { info: null, mine: [], recent: [], registrations: [], inflight: [], wallet: null, height: -1, error: '', loaded: false, version: 0 });
}
