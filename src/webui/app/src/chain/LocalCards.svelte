<script lang="ts">
  // The cards of BitNames on the home of the dashboard: the names of the wallet, what is coming up (the
  // registrations under way, each with the block it waits for; names themselves never expire, so nothing
  // else falls due), and the names registered last. Each opens its page.
  import Icon from '../components/Icon.svelte';
  import { t } from '../lib/i18n.svelte';
  import { go, goTo } from '../lib/nav.svelte';
  import { app, node, wallets } from '../lib/state.svelte';
  import Card from '../views/home/Card.svelte';
  import './bn/bn.css';
  import Domain from './bn/Domain.svelte';
  import { blocksTime, handleOf, progressOf } from './bn/model';
  import { bn, registrationsOf, startNames } from './bn/store.svelte';

  $effect(() => startNames());

  const names = $derived(bn.mine.filter((m) => m.kind === 'name'));
  const subs = $derived(names.filter((m) => m.name?.includes('.')).length);
  const tip = $derived(node.tip?.height ?? bn.height);
  const depth = $derived(bn.info?.revealdepth ?? 0);
  const spacing = $derived(app.info?.blockSpacing ?? 60);
  const under = $derived(
    registrationsOf(wallets.current)
      .filter((r) => r.step === 'reserved' || r.step === 'registering')
      .map((r) => ({ r, p: progressOf(r, tip, depth) }))
      .sort((a, b) => (a.p.blocksLeft ?? 0) - (b.p.blocksLeft ?? 0))
      .slice(0, 3),
  );
</script>

{#if bn.loaded && !bn.error}
  <div class="bn-cards">
    <Card title="My names" icon="tag" open={() => go('names')} openLabel="Open My names" stat>
      <span class="value num">{wallets.current === null ? '–' : names.length}</span>
      <dl class="pairs">
        <dt>Subnames</dt><dd class="num">{subs}</dd>
        <dt>Being registered</dt><dd class="num">{registrationsOf(wallets.current).filter((r) => r.step === 'reserved' || r.step === 'registering').length}</dd>
        <dt>DNS of .x</dt><dd>{bn.info?.dns ? t('on') : t('off')}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('register')}><Icon name="tagplus" size={15} /> Get a name</button>
        <button type="button" class="btn ghost" onclick={() => go('names', 'records')}>Records</button>
      {/snippet}
    </Card>

    <Card title="Coming up" icon="clock" open={() => go('register')} openLabel="Open the registrations" stat>
      {#if under.length === 0}
        <p class="muted">Nothing is due: names never expire.</p>
      {:else}
        <ul class="due">
          {#each under as { r, p } (r.reserveTxid)}
            <li>
              <Domain name={r.name} />
              <span class="faint small">
                {#if p.stage === 'confirm'}{t('waiting for a block')}
                {:else if p.stage === 'wait'}{t('revealed in %1 block(s), %2', p.blocksLeft, blocksTime(p.blocksLeft ?? 0, spacing))}
                {:else if r.step === 'reserved'}{t('ready to register')}
                {:else}{t('registering, by block %1', p.deadline)}{/if}
              </span>
            </li>
          {/each}
        </ul>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('register')}>Registrations</button>
      {/snippet}
    </Card>

    <Card title="Registered last" icon="atlas" open={() => go('directory')} openLabel="Open the name directory" stat>
      {#if bn.recent.length === 0}
        <p class="muted">No names yet.</p>
      {:else}
        <ul class="due">
          {#each bn.recent.slice(0, 4) as n (n.namehash)}
            <li>
              <button type="button" class="link" onclick={() => goTo('directory', '', handleOf(n.namehash))}><Domain name={n.name} seq={n.seq} /></button>
              <span class="faint small">{t('block %1', n.registered)}</span>
            </li>
          {/each}
        </ul>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('directory')}>Directory</button>
        <button type="button" class="btn ghost" onclick={() => go('dns')}>DNS</button>
      {/snippet}
    </Card>
  </div>
{/if}

<style>
  .bn-cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(17rem, 100%), 1fr));
    gap: 1rem;
  }
  .due {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .due li {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .due :global(.bn-domain) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .link {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
