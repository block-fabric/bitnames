<script lang="ts">
  import { stacked } from '../../lib/actions';
  // Get a name: one field, and whether the name can be had as it is typed (the rules of the wallet first,
  // then the registry); then the whole of taking it, as one flow the app follows by itself: the reservation
  // that hides the name, the blocks it has to stay hidden (revealdepth), the registration that reveals it,
  // and the block that makes it yours, each with its block and about how long. A subname of a name of the
  // wallet (blog.yourname) is made at once instead. The "Get a name" panel of the Names page of chains-qt.
  import AddressInput from '../../components/AddressInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go, goTo, PAGES, takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { app, node, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../bn/bn.css';
  import Domain from '../bn/Domain.svelte';
  import StepTrack from '../bn/StepTrack.svelte';
  import { blocksTime, dataProblem, handleOf, newNameProblem, progressOf, subnameProblem, TLD, type NameInfo, type Registration } from '../bn/model';
  import { beginRegistration, bn, forget, registerNow, registrationsOf, releaseRegistration, startNames, walletLocked } from '../bn/store.svelte';

  $effect(() => startNames());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const spacing = $derived(app.info?.blockSpacing ?? 60);
  const tip = $derived(node.tip?.height ?? bn.height);
  const depth = $derived(bn.info?.revealdepth ?? 0);

  let query = $state('');
  // A name given by another page (a name of the directory that is free again).
  $effect(() => {
    const key = takeFocus('register');
    if (key) query = key;
  });
  const typed = $derived(query.trim().replace(/\.x\.?$/i, ''));
  const isSub = $derived(typed.includes('.'));

  type Verdict =
    | { kind: 'empty' }
    | { kind: 'bad'; why: string }
    | { kind: 'checking' }
    | { kind: 'taken'; info: NameInfo }
    | { kind: 'free' }
    | { kind: 'mine' }
    | { kind: 'sub-free'; parent: string; label: string }
    | { kind: 'sub-no'; why: string }
    | { kind: 'error'; why: string };
  let verdict = $state<Verdict>({ kind: 'empty' });

  // As it is typed: the rules of the wallet at once, then the registry, once the typing stops.
  let generation = 0;
  $effect(() => {
    const name = typed;
    const mine = ++generation;
    if (!name) {
      verdict = { kind: 'empty' };
      return;
    }
    const problem = name.includes('.') ? subnameProblem(name) : newNameProblem(name);
    if (problem) {
      verdict = { kind: 'bad', why: problem };
      return;
    }
    verdict = { kind: 'checking' };
    const timer = setTimeout(async () => {
      const v = await judge(name);
      if (mine === generation) verdict = v;
    }, 300);
    return () => clearTimeout(timer);
  });

  async function judge(name: string): Promise<Verdict> {
    try {
      const found = await rpc.raw<NameInfo>('getname', [name]).catch((e) => {
        if (/No such (sub)?name|No name has the number/i.test(errorText(e))) return null;
        throw e;
      });
      if (!name.includes('.')) {
        if (!found) return { kind: 'free' };
        return bn.mine.some((m) => m.kind === 'name' && m.namehash === found.namehash) ? { kind: 'mine' } : { kind: 'taken', info: found };
      }
      if (found && found.parent) return { kind: 'taken', info: found };
      const dot = name.indexOf('.');
      const label = name.slice(0, dot);
      const parent = name.slice(dot + 1);
      const p = await rpc.raw<NameInfo>('getname', [parent]).catch(() => null);
      if (!p) return { kind: 'sub-no', why: t('%1 is not registered: register it first, then make its subnames.', `${parent}.${TLD}`) };
      if (!bn.mine.some((m) => m.kind === 'name' && m.namehash === p.namehash)) return { kind: 'sub-no', why: t('Only the holder of %1 can make its subnames.', `${parent}.${TLD}`) };
      return { kind: 'sub-free', parent, label };
    } catch (e) {
      return { kind: 'error', why: errorText(e) };
    }
  }

  // --- The options of a name ---
  let makePublic = $state(true);
  let ipv4 = $state('');
  const ipProblem = $derived(dataProblem('ipv4', ipv4));
  let holder = $state('');
  let holderValid = $state(false);
  let busy = $state(false);

  const balance = $derived(wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null);

  async function getIt() {
    const wallet = wallets.current;
    if (wallet === null || busy) return;
    if (verdict.kind === 'free') {
      const name = typed;
      const r = await ask({
        title: t('Get the name'),
        message: tn('Reserve %1 now, and register it once the reservation is %n block(s) deep?', depth, `${name}.${TLD}`),
        informative: makePublic
          ? t('Public: its text is on the chain, so anyone can find it by name. Two small transactions of the wallet pay their fees.')
          : t('Private: only its hash is on the chain; it works the same, but only those who know it can find it. Two small transactions of the wallet pay their fees.'),
        level: 'question',
        choices: [
          { id: 'no', label: 'Cancel' },
          { id: 'yes', label: 'Get it', kind: 'primary' },
        ],
      });
      if (r.choice !== 'yes') return;
      busy = true;
      try {
        const reg = await beginRegistration(wallet, name, makePublic, ipv4.trim() ? { ipv4: ipv4.trim() } : {});
        if (!reg) return;
        toast('success', t('Name reserved'), t('The app registers it by itself once the reservation is deep enough.'));
        query = '';
        ipv4 = '';
      } catch (e) {
        await inform(t('Get the name'), errorText(e), 'error');
      } finally {
        busy = false;
      }
    } else if (verdict.kind === 'sub-free') {
      const { parent, label } = verdict;
      const shown = `${label}.${parent}.${TLD}`;
      const r = await ask({
        title: t('Make a subname'),
        message: holder.trim() ? t('Make %1 and give it to this address?', shown) : t('Make %1, held by this wallet?', shown),
        code: holder.trim() || undefined,
        informative: t('A subname needs no reservation: it exists after the next block, for as long as its parent is not released.'),
        level: 'question',
        choices: [
          { id: 'no', label: 'Cancel' },
          { id: 'yes', label: 'Make it', kind: 'primary' },
        ],
      });
      if (r.choice !== 'yes') return;
      busy = true;
      try {
        const done = await walletRpc<{ txid: string }>(wallet, 'createsubname', holder.trim() ? [parent, label, holder.trim()] : [parent, label]);
        if (!done) return;
        toast('success', t('Subname on its way'), t('%1 exists after the next block.', shown));
        query = '';
        holder = '';
      } catch (e) {
        await inform(t('Make a subname'), errorText(e), 'error');
      } finally {
        busy = false;
      }
    }
  }

  // --- The registrations under way ---
  const mine = $derived(registrationsOf(wallets.current));
  const locked = $derived(walletLocked(wallets.current));

  function stepLine(r: Registration): string {
    const p = progressOf(r, tip, depth);
    if (r.step === 'failed') return r.error ?? t('Stopped.');
    if (p.stage === 'confirm') return t('The reservation waits for a block. The name stays hidden meanwhile.');
    if (p.stage === 'wait') return tn('Hidden for %n more block(s): it can be revealed from block %1, in %2.', p.blocksLeft ?? 0, p.revealFrom, blocksTime(p.blocksLeft ?? 0, spacing));
    if (p.stage === 'register' && r.step === 'reserved') return locked ? t('Deep enough: register it now (the wallet asks for its passphrase).') : t('Deep enough: the registration goes out with this block.');
    if (p.stage === 'register') return p.late ? tn('Not in a block %n block(s) after it was sent: whoever makes the blocks may be keeping it out. It is sent again if it drops.', depth) : t('Revealed: it should be in a block by block %1.', p.deadline);
    return t('Yours since block %1.', r.registeredHeight ?? '–');
  }

  async function registerManually(r: Registration) {
    busy = true;
    try {
      if (await registerNow(r, true)) toast('success', t('Registration sent'), t('The name is yours once it is in a block.'));
    } catch (e) {
      await inform(t('Register the name'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  async function giveUp(r: Registration) {
    const ok = await ask({
      title: t('Give up the name'),
      message: r.step === 'failed' && !r.reserveHeight ? t('Take %1 off the list?', `${r.name}.${TLD}`) : t('Give up the reservation of %1?', `${r.name}.${TLD}`),
      informative: t('Nothing is registered with it. Anyone may reserve and register the name meanwhile.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Give it up', kind: 'danger' },
      ],
    });
    if (ok.choice !== 'yes') return;
    busy = true;
    try {
      await releaseRegistration(r);
    } catch (e) {
      await inform(t('Give up the name'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  function openName(r: Registration) {
    const m = bn.mine.find((x) => x.kind === 'name' && x.name === r.name);
    if (m?.namehash) goTo('names', '', handleOf(m.namehash));
    else go('names');
  }

  // Reservations of the wallet this app is not following (made in the console, or by chains-qt).
  const strays = $derived(bn.mine.filter((m) => m.kind === 'reservation' && !mine.some((r) => r.reserveTxid === m.txid)));
</script>

<div class="page bn-page register">
  <header class="page-head"><h1>Get a name</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.register} />
  {:else}
    <section class="panel hero">
      <div class="panel-body">
        <p class="lead">A name is yours for good: point it at your servers (<span translate="no">yourname.{TLD}</span>), hand out subnames, get paid with messages. Type one to see whether it is free.</p>
        <label class="bn-search">
          <span class="icon-lead"><Icon name="search" size={20} /></span>
          <input id="name-search" type="text" placeholder="yourname" aria-label="The name to get" bind:value={query} spellcheck="false" autocomplete="off" autocapitalize="off" />
          <span class="tld" aria-hidden="true" translate="no">.x</span>
        </label>

        {#if verdict.kind === 'bad' || verdict.kind === 'sub-no' || verdict.kind === 'error'}
          <div class="bn-verdict bad" role="status"><Icon name="alert" size={20} /><div><strong>{verdict.kind === 'error' ? t('The node could not say') : t('Not as it is')}</strong><span class="why"><SafeText text={verdict.why} /></span></div></div>
        {:else if verdict.kind === 'checking'}
          <div class="bn-verdict" role="status"><Icon name="clock" size={20} /><div><strong>Looking in the registry…</strong></div></div>
        {:else if verdict.kind === 'taken'}
          <div class="bn-verdict taken" role="status">
            <Icon name="lock" size={20} />
            <div class="grow">
              <strong>{t('Taken')} · <Domain name={verdict.info.name ?? typed} /> <span class="faint" translate="no">#{verdict.info.seq}</span></strong>
              <span class="why">{t('Registered in block %1', verdict.info.registered)}{#if verdict.info.owner.address} · {t('held by')} <span class="mono"><SafeText text={shortHash(verdict.info.owner.address, 10)} /></span>{/if}</span>
            </div>
            <button type="button" class="btn small" onclick={() => verdict.kind === 'taken' && goTo('directory', '', handleOf(verdict.info.namehash))}>See it</button>
          </div>
        {:else if verdict.kind === 'mine'}
          <div class="bn-verdict free" role="status"><Icon name="check" size={20} /><div class="grow"><strong>{t('Already yours')}</strong></div><button type="button" class="btn small" onclick={() => go('names')}>My names</button></div>
        {:else if verdict.kind === 'free' || verdict.kind === 'sub-free'}
          <div class="bn-verdict free" role="status">
            <Icon name="check" size={20} />
            <div class="grow">
              <strong><Domain name={typed} /> {verdict.kind === 'free' ? t('is free') : t('can be yours')}</strong>
              <span class="why">{verdict.kind === 'free' ? tn('Reserved now, revealed after %n block(s): about %1 in all.', depth, blocksTime(depth + 2, spacing)) : t('A subname of your %1: it exists after the next block.', `${verdict.parent}.${TLD}`)}</span>
            </div>
          </div>
          <form class="bn-form options" onsubmit={(e) => { e.preventDefault(); getIt(); }}>
            {#if verdict.kind === 'free'}
              <label class="switch"><input type="checkbox" bind:checked={makePublic} id="name-public" /><span class="track"></span><span>Public</span></label>
              <p class="bn-hint">{makePublic ? t('Its text is on the chain: anyone can find it by name and see it listed.') : t('Only its hash is on the chain: it works the same, but only those who know the name can find it.')}</p>
              <label class="field">
                <span class="label">Point it at (optional)</span>
                <input id="name-ipv4" type="text" class="mono" placeholder="203.0.113.7" bind:value={ipv4} spellcheck="false" autocomplete="off" aria-invalid={ipProblem ? 'true' : undefined} />
                {#if ipProblem}<span class="error">{ipProblem}</span>{:else}<span class="hint">An IPv4 address the name answers with. Records, keys and more can be set once it is yours.</span>{/if}
              </label>
            {:else}
              <AddressInput id="name-holder" label="Held by (empty: this wallet)" bind:value={holder} bind:valid={holderValid} required={false} />
            {/if}
            <div class="row">
              <button type="submit" class="btn cta" id="name-get" disabled={busy || !!ipProblem || (verdict.kind === 'sub-free' && !!holder.trim() && !holderValid)}>
                <Icon name="plus" size={16} />
                {verdict.kind === 'free' ? t('Get %1', `${typed}.${TLD}`) : t('Make %1', `${typed}.${TLD}`)}
              </button>
              {#if balance !== null && balance === 0n}<span class="bn-error">The wallet has no coins for the fees: deposit some first (Peg).</span>{/if}
            </div>
          </form>
        {:else}
          <p class="bn-hint">Lower-case letters, digits, - and _. For a subname of a name of yours, type it under it: <span translate="no">blog.yourname</span>.</p>
        {/if}
      </div>
    </section>

    <section class="panel" aria-label="Registrations">
      <div class="panel-head"><h2>Registrations</h2>{#if bn.info}<span class="muted small">{tn('Reveal depth: %n block(s)', depth)}</span>{/if}</div>
      {#if mine.length === 0 && strays.length === 0}
        <p class="bn-empty">None under way. A name you get shows here, step by step, until it is yours.</p>
      {:else}
        <ul class="regs">
          {#each mine as r (r.reserveTxid)}
            {@const p = progressOf(r, tip, depth)}
            <li class="reg" aria-label={t('Registration of %1', `${r.name}.${TLD}`)}>
              <div class="reg-head">
                <Domain name={r.name} />
                <span class="badge" class:accent={r.step === 'done'} class:danger={r.step === 'failed'}>{r.step === 'done' ? t('Yours') : r.step === 'failed' ? t('Stopped') : r.public ? t('Public') : t('Private')}</span>
                <span class="faint small">{t('since %1', formatTime(r.started, true))}</span>
              </div>
              {#if r.step !== 'failed'}<StepTrack progress={p} />{/if}
              <p class="step" class:bn-error={r.step === 'failed' || p.late}>{#if r.step === 'failed'}<SafeText text={stepLine(r)} />{:else}{stepLine(r)}{/if}</p>
              {#if r.error && r.step !== 'failed'}<p class="bn-error"><SafeText text={r.error} /></p>{/if}
              <div class="row">
                {#if r.step === 'reserved' && p.stage === 'register'}<button type="button" class="btn primary small" disabled={busy} onclick={() => registerManually(r)}>Register now</button>{/if}
                {#if r.step === 'done'}
                  <button type="button" class="btn small" onclick={() => openName(r)}>Records and more</button>
                  <button type="button" class="btn small ghost" onclick={() => forget(r)}>Dismiss</button>
                {:else if r.step === 'failed'}
                  <button type="button" class="btn small ghost" disabled={busy} onclick={() => giveUp(r)}>{r.reserveHeight !== undefined ? t('Release the reservation') : t('Dismiss')}</button>
                {:else if r.step === 'reserved'}
                  <button type="button" class="btn small ghost" disabled={busy} onclick={() => giveUp(r)}>Give it up</button>
                {/if}
                <span class="faint small mono" translate="no">{shortHash(r.registerTxid ?? r.reserveTxid, 8)}</span>
              </div>
            </li>
          {/each}
        </ul>
        {#if strays.length}
          <div class="panel-body">
            <h3>Other reservations of this wallet</h3>
            <p class="bn-hint">Made outside this page (the console, another app): register them with registername, or release them in My names.</p>
            <div class="side-table-wrap">
              <table class="side-table" use:stacked aria-label="Other reservations">
                <thead><tr><th>Name</th><th>Transaction</th></tr></thead>
                <tbody>
                  {#each strays as s (s.txid + s.vout)}
                    <tr><td>{#if s.name}<Domain name={s.name} />{:else}<span class="faint">{t('hidden (unlock the wallet to see it)')}</span>{/if}</td><td class="mono" translate="no">{shortHash(s.txid, 10)}</td></tr>
                  {/each}
                </tbody>
              </table>
            </div>
          </div>
        {/if}
      {/if}
    </section>

    <section class="panel how">
      <div class="panel-head"><h2>How a name is taken</h2></div>
      <ol class="panel-body steps">
        <li><strong>Reserved.</strong> A transaction commits to the name without showing it, so that nobody can see it coming and take it first.</li>
        <li><strong>{t('Hidden.')}</strong> {tn('The reservation stays hidden %n block(s), so that whoever makes the next block cannot take the name as it is revealed.', depth)}</li>
        <li><strong>{t('Revealed.')}</strong> {tn('The app sends the registration, which shows the name; it should be in a block within %n block(s).', depth)}</li>
        <li><strong>Yours.</strong> The name never expires: it is yours until you give it away or release it.</li>
      </ol>
    </section>
  {/if}
</div>

<style>
  .hero .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    padding-top: 1.2rem;
  }
  .lead {
    margin: 0;
    color: var(--text-muted);
    max-width: 52rem;
  }
  .options {
    max-width: 40rem;
  }
  .regs {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
    margin: 0;
    padding: 0 1.3rem 1.2rem;
    list-style: none;
  }
  .reg {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    padding: 0.9rem 1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
  }
  .reg-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .step {
    margin: 0;
    font-size: 0.92rem;
  }
  .steps {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(15rem, 100%), 1fr));
    gap: 0.6rem 1.2rem;
    margin: 0;
    padding-left: 2.6rem;
    color: var(--text-muted);
    font-size: 0.92rem;
  }
  .steps strong {
    color: var(--text);
  }
  .small {
    font-size: 0.82rem;
  }
  h3 {
    margin: 0.4rem 0;
    font-size: 1rem;
  }
</style>
