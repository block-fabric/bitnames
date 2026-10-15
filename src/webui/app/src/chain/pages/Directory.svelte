<script lang="ts">
  // The directory: every name of the registry, newest first or in the order of registration, a page at a
  // time; a search through the public names by their text (listnames with its filter), or straight to one by
  // its text, subname, number (#1739-0029) or hash; and the one picked, whole, read only (NameView): its
  // records, profile, subnames and history. The Explore tab of the Names page of chains-qt.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { goTo, takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import '../bn/bn.css';
  import Domain from '../bn/Domain.svelte';
  import NameView from '../bn/NameView.svelte';
  import { handleOf, queryKind, withoutTld, type NameInfo } from '../bn/model';
  import { bn, startNames } from '../bn/store.svelte';

  $effect(() => startNames());

  const PAGE = 50;
  let query = $state('');
  let order = $state<'newest' | 'oldest'>('newest');
  let page = $state(0);
  let list = $state<NameInfo[]>([]);
  let loading = $state(false);
  let error = $state('');
  let picked = $state<string | null>(null);
  let tab = $state('records');
  let notFound = $state('');

  $effect(() => {
    const key = takeFocus('directory');
    if (key) picked = key;
  });

  const total = $derived(bn.info?.names ?? 0);
  const kind = $derived(queryKind(query));
  const mineSet = $derived(new Set(bn.mine.filter((m) => m.kind === 'name').map((m) => m.namehash)));

  // The page of the list: a search by text asks the node for the public names that hold it.
  let generation = 0;
  $effect(() => {
    const q = query.trim().toLowerCase();
    const k = kind;
    const p = page;
    const o = order;
    void bn.version;
    const mine = ++generation;
    const timer = setTimeout(async () => {
      loading = true;
      error = '';
      try {
        let got: NameInfo[];
        if (k === 'name' || k === 'subname' || k === 'domain') {
          const text = k === 'domain' ? (withoutTld(q) ?? q) : q;
          got = await rpc.raw<NameInfo[]>('listnames', [PAGE, p * PAGE, text]);
        } else if (o === 'newest') {
          const skip = Math.max(0, total - (p + 1) * PAGE);
          const count = Math.min(PAGE, total - p * PAGE);
          got = count > 0 ? (await rpc.raw<NameInfo[]>('listnames', [count, skip])).slice().reverse() : [];
        } else {
          got = await rpc.raw<NameInfo[]>('listnames', [PAGE, p * PAGE]);
        }
        if (mine === generation) list = Array.isArray(got) ? got : [];
      } catch (e) {
        if (mine === generation) error = errorText(e);
      } finally {
        if (mine === generation) loading = false;
      }
    }, 200);
    return () => clearTimeout(timer);
  });

  $effect(() => {
    void query;
    page = 0;
    notFound = '';
  });

  /** Enter: straight to the name typed (a subname, a number, a hash, a private name known by its text). */
  async function open() {
    const q = query.trim();
    if (!q || kind === 'bad') return;
    const text = kind === 'domain' ? (withoutTld(q.toLowerCase()) ?? q) : q;
    try {
      const found = await rpc.raw<NameInfo>('getname', [text]);
      picked = handleOf(found.namehash);
      notFound = '';
    } catch (e) {
      notFound = errorText(e);
    }
  }

  const pages = $derived(Math.max(1, Math.ceil(total / PAGE)));
  const searching = $derived(kind === 'name' || kind === 'subname' || kind === 'domain');
</script>

<div class="page bn-page directory">
  <header class="page-head">
    <h1>Name directory</h1>
    {#if bn.info}<span class="muted">{t('%1 names registered, %2 reservations waiting', bn.info.names, bn.info.reservations)}</span>{/if}
  </header>

  <form class="bn-search" role="search" onsubmit={(e) => { e.preventDefault(); open(); }}>
    <span class="icon-lead"><Icon name="search" size={20} /></span>
    <input id="directory-search" type="text" placeholder="A name, blog.core, #1739-0029 or 0x…" aria-label="Find a name" bind:value={query} spellcheck="false" autocomplete="off" autocapitalize="off" />
    <button type="submit" class="btn small" disabled={!query.trim() || kind === 'bad'}>Open</button>
  </form>
  {#if kind === 'bad'}<p class="bn-error">A name is lower-case letters, digits, - and _; a number is # and two groups of four digits; a hash is 0x and 64 hex digits.</p>{/if}
  {#if notFound}<p class="bn-error" role="alert"><SafeText text={notFound} /></p>{/if}

  <div class="bn-split">
    <section class="panel" aria-label="Names">
      <div class="panel-head">
        <h2>{searching ? t('Public names with “%1”', query.trim()) : t('Every name')}</h2>
        {#if !searching}
          <select aria-label="Order" bind:value={order} onchange={() => (page = 0)}>
            <option value="newest">Newest first</option>
            <option value="oldest">Oldest first</option>
          </select>
        {/if}
      </div>
      {#if error}
        <p class="bn-empty"><SafeText text={error} /></p>
      {:else if list.length === 0}
        <p class="bn-empty">{loading ? t('Reading the registry…') : searching ? t('No public name holds this text. A private name is found by its whole text: press Enter.') : t('No names yet.')}</p>
      {:else}
        <ul class="bn-list">
          {#each list as n (n.namehash)}
            <li>
              <button type="button" class="bn-item" aria-current={handleOf(n.namehash) === picked ? 'true' : undefined} onclick={() => (picked = handleOf(n.namehash))}>
                <span class="bn-glyph" aria-hidden="true"><Icon name={n.parent ? 'subname' : 'tag'} size={16} /></span>
                <span class="main">
                  {#if n.name}<Domain name={n.name} />{:else}<span class="bn-domain faint" translate="no">#{n.seq}</span>{/if}
                  <span class="sub">{[`#${n.seq}`, t('block %1', n.registered), n.name ? '' : t('private'), mineSet.has(n.namehash) ? t('yours') : ''].filter(Boolean).join(' · ')}</span>
                </span>
              </button>
            </li>
          {/each}
        </ul>
      {/if}
      <div class="panel-body pager">
        <button type="button" class="btn small ghost" disabled={page === 0} onclick={() => (page = Math.max(0, page - 1))} aria-label="Previous page"><span class="flip"><Icon name="chevron-right" size={15} /></span></button>
        <span class="muted small">{searching ? t('Page %1', page + 1) : t('Page %1 of %2', page + 1, pages)}</span>
        <button type="button" class="btn small ghost" disabled={searching ? list.length < PAGE : page + 1 >= pages} onclick={() => (page += 1)} aria-label="Next page"><Icon name="chevron-right" size={15} /></button>
      </div>
    </section>

    {#if picked}
      {#key picked}
        <div class="detail">
          <NameView handle={picked} bind:tab />
          {#if bn.mine.some((m) => m.kind === 'name' && handleOf(m.namehash ?? '') === picked)}
            <p class="bn-hint">This name is yours: <button type="button" class="btn small ghost" onclick={() => goTo('names', '', picked!)}>edit it in My names</button></p>
          {/if}
        </div>
      {/key}
    {:else}
      <section class="panel recent">
        <div class="panel-head"><h2>Registered last</h2></div>
        {#if bn.recent.length === 0}
          <p class="bn-empty">No names yet.</p>
        {:else}
          <ul class="bn-list">
            {#each bn.recent as n (n.namehash)}
              <li>
                <button type="button" class="bn-item" onclick={() => (picked = handleOf(n.namehash))}>
                  <span class="main"><Domain name={n.name} seq={n.seq} /><span class="sub">{t('block %1', n.registered)} · <span class="mono" translate="no">{shortHash(n.owner.address ?? '', 8)}</span></span></span>
                </button>
              </li>
            {/each}
          </ul>
        {/if}
      </section>
    {/if}
  </div>
</div>

<style>
  .pager {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding-top: 0.6rem;
  }
  .flip {
    display: inline-flex;
    transform: rotate(180deg);
  }
  .small {
    font-size: 0.82rem;
  }
  select {
    width: auto;
  }
  .detail {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    min-width: 0;
  }
</style>
