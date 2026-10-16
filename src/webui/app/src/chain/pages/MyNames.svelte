<script lang="ts">
  // My names: the names, subnames and reservations of the wallet in a list, and the one picked beside it,
  // whole and editable (NameView: records, profile, subnames, giving away or releasing, history). Names are
  // permanent: nothing to renew, and no expiry to watch. The "My names" tab of the Names page of chains-qt.
  import Icon from '../../components/Icon.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go, nav, PAGES, takeFocus } from '../../lib/nav.svelte';
  import { toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { narrowWindow } from '../../lib/media.svelte';
  import '../bn/bn.css';
  import Domain from '../bn/Domain.svelte';
  import NameView from '../bn/NameView.svelte';
  import { handleOf, shownName, TLD, type MyName } from '../bn/model';
  import { bn, namesOf, refreshNames, registrationsOf, startNames } from '../bn/store.svelte';

  $effect(() => startNames());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  let filter = $state('');
  let picked = $state<string | null>(null);
  let tab = $state('records');
  // In a window too narrow for the list and the name side by side (700 or 480 px), one or the other: the list,
  // then the name picked with a way back (audit port-2 #6: the name came under the whole list, 16 000 px down).
  const single = $derived(narrowWindow.matches);
  let listOpen = $state(true);
  function pick(handle: string) {
    picked = handle;
    listOpen = false;
  }

  // A name to show, given by another page (Get a name, the cards of the home), or a tab of the menus.
  $effect(() => {
    const key = takeFocus('names');
    if (key) pick(key);
    if (nav.page === 'names' && ['records', 'profile', 'subnames', 'transfer', 'history'].includes(nav.tab)) tab = nav.tab;
  });

  // With those whose last change waits for a block (the wallet does not list them meanwhile).
  const names = $derived(namesOf(wallets.current).sort((a, b) => shownName(a).localeCompare(shownName(b))));
  const reservations = $derived(bn.mine.filter((m) => m.kind === 'reservation'));
  const shown = $derived(names.filter((m) => !filter.trim() || shownName(m).includes(filter.trim().toLowerCase())));
  const under = $derived(registrationsOf(wallets.current).filter((r) => r.step === 'reserved' || r.step === 'registering'));

  // The first name, when none is picked (or the one picked left the wallet).
  $effect(() => {
    if (picked && picked.startsWith('r:')) return;
    if (!picked || !names.some((m) => handleOf(m.namehash!) === picked)) picked = names[0] ? handleOf(names[0].namehash!) : null;
  });
  const pickedName = $derived(names.find((m) => handleOf(m.namehash!) === picked));
  const pickedReservation = $derived(picked?.startsWith('r:') ? reservations.find((r) => `r:${r.txid}:${r.vout}` === picked) : undefined);

  function isSub(m: MyName): boolean {
    return !!m.name?.includes('.');
  }

  async function releaseReservation(r: MyName) {
    const wallet = wallets.current;
    if (wallet === null || !r.name) return;
    const ok = await ask({
      title: t('Release the reservation'),
      message: t('Give up the reservation of %1?', `${r.name}.${TLD}`),
      informative: t('Nothing is registered with it.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Release', kind: 'danger' },
      ],
    });
    if (ok.choice !== 'yes') return;
    try {
      const txid = await walletRpc<string>(wallet, 'releasereservation', [r.name]);
      if (txid === null) return;
      toast('success', t('Reservation released'), t('It is gone after the next block.'));
      picked = null;
      void refreshNames();
    } catch (e) {
      await inform(t('Release the reservation'), errorText(e), 'error');
    }
  }
  // Simple mode (lib/uimode.svelte.ts): the name in plain words; records, keys and the rest behind More or in Advanced.
  const simple = $derived(simpleMode());
</script>

<div class="page bn-page my-names">
  <header class="page-head">
    <h1>My names</h1>
    {#if !noWallet}<button type="button" class="btn primary" onclick={() => go('register')}><Icon name="plus" size={15} /> Get a name</button>{/if}
  </header>

  {#if noWallet}
    <NoWallet page={PAGES.names} />
  {:else if bn.loaded && names.length === 0 && reservations.length === 0}
    <section class="panel empty">
      <span class="bn-glyph big" aria-hidden="true"><Icon name="tag" size={30} /></span>
      <h2>No names yet</h2>
      <p class="muted">A name points to your servers as <span translate="no">yourname.{TLD}</span>, holds your keys, takes paid messages and hands out subnames. It is yours for good.</p>
      {#if under.length}<p class="muted">{tn('%n registration(s) under way.', under.length)}</p>{/if}
      <div class="row center"><button type="button" class="btn cta" onclick={() => go('register')}><Icon name="search" size={16} /> Find a name</button></div>
    </section>
  {:else}
    <div class="bn-split">
      {#if !single || listOpen}
      <section class="panel" aria-label="The names of the wallet">
        <div class="panel-head"><h2>{tn('%n name(s)', names.length)}</h2></div>
        <div class="panel-body filter"><input type="search" placeholder="Filter" aria-label="Filter the names" bind:value={filter} /></div>
        <ul class="bn-list">
          {#each shown as m (m.namehash)}
            <li>
              <button type="button" class="bn-item" aria-current={handleOf(m.namehash!) === picked ? 'true' : undefined} onclick={() => pick(handleOf(m.namehash!))}>
                <span class="bn-glyph" aria-hidden="true"><Icon name={isSub(m) ? 'subname' : 'tag'} size={16} /></span>
                <span class="main">
                  <Domain name={m.name} seq={m.seq} />
                  <span class="sub">{isSub(m) ? t('Subname') : t('Name')}{#if !simple} · <span translate="no">#{m.seq ?? '–'}</span>{/if}{#if m.leaving} · {t('leaving with the next block')}{:else if m.waiting} · {t('a change waits for a block')}{/if}</span>
                </span>
              </button>
            </li>
          {/each}
          {#if shown.length === 0 && names.length}<li class="bn-hint pad">Nothing matches.</li>{/if}
        </ul>
        {#if reservations.length}
          <div class="panel-head"><h2>Reservations</h2></div>
          <ul class="bn-list">
            {#each reservations as r (r.txid + r.vout)}
              <li>
                <button type="button" class="bn-item" aria-current={picked === `r:${r.txid}:${r.vout}` ? 'true' : undefined} onclick={() => pick(`r:${r.txid}:${r.vout}`)}>
                  <span class="bn-glyph" aria-hidden="true"><Icon name="clock" size={16} /></span>
                  <span class="main">
                    {#if r.name}<Domain name={r.name} />{:else}<span class="faint">{t('A hidden name')}</span>{/if}
                    <span class="sub">{t('Reserved, being registered')}</span>
                  </span>
                </button>
              </li>
            {/each}
          </ul>
        {/if}
      </section>
      {/if}

      {#if single && !listOpen}
        <div class="back"><button type="button" class="btn ghost small" onclick={() => (listOpen = true)}><Icon name="back" size={15} /> {tn('All %n name(s)', names.length)}</button></div>
      {/if}
      {#if single && listOpen}
        <!-- The list alone: a name opens on its own, with a way back. -->
      {:else if pickedName}
        {#key picked}
          <NameView handle={picked!} editable={!pickedName.leaving} knownText={pickedName.name} bind:tab ongone={() => (picked = null)} />
        {/key}
      {:else if pickedReservation}
        <section class="panel">
          <header class="bn-head">
            <div>
              {#if pickedReservation.name}<Domain name={pickedReservation.name} big />{:else}<span class="bn-domain big faint">{t('A hidden name')}</span>{/if}
              <div class="tags"><span class="badge warning">{t('Reservation')}</span></div>
            </div>
          </header>
          <div class="panel-body bn-form">
            <p class="muted">A reservation hides the name it is for until it is registered. {#if registrationsOf(wallets.current).some((x) => x.reserveTxid === pickedReservation?.txid)}The app is registering it: its steps are on the Get a name page.{:else}The app is not following it (it was made elsewhere): register it with registername in the console, or release it.{/if}</p>
            <dl class="facts">
              <dt>Held by</dt>
              <dd class="mono"><SafeText text={pickedReservation.address} /></dd>
              {#if !simple}
                <dt>Transaction</dt>
                <dd class="mono" translate="no">{shortHash(pickedReservation.txid, 12)}</dd>
              {/if}
            </dl>
            <div class="row">
              <button type="button" class="btn" onclick={() => go('register')}>Registrations</button>
              {#if pickedReservation.name}
                <button type="button" class="btn danger" onclick={() => pickedReservation && releaseReservation(pickedReservation)}>Release the reservation…</button>
              {:else}
                <span class="bn-hint">Unlock the wallet to see which name it is for.</span>
              {/if}
            </div>
          </div>
        </section>
      {:else}
        <section class="panel"><p class="bn-empty">Pick a name.</p></section>
      {/if}
    </div>
  {/if}
</div>

<style>
  .empty {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.5rem;
    padding: 2.4rem 1.5rem;
    text-align: center;
  }
  .empty p {
    max-width: 34rem;
  }
  .bn-glyph.big {
    width: 3.6rem;
    height: 3.6rem;
    border-radius: var(--radius-l);
  }
  .center {
    justify-content: center;
  }
  .center :global(.btn.cta) {
    width: auto;
    gap: 0.5rem;
  }
  .filter {
    padding-bottom: 0.2rem;
  }
  .pad {
    padding: 0.6rem 0.8rem;
  }
  .back {
    margin-bottom: -0.4rem;
  }
</style>
