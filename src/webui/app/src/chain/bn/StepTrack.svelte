<script lang="ts">
  // The steps of a registration: reserved, in a block, hidden for revealdepth blocks, revealed, yours.
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { css } from '../../lib/actions';
  import { STAGES, type Progress } from './model';

  let { progress }: { progress: Progress } = $props();
</script>

<ol class="bn-track" aria-label={t('Steps of the registration')}>
  {#each STAGES as s, i (s.id)}
    <li class:done={i < progress.index || progress.index === 4} class:now={i === progress.index && progress.index < 4} aria-current={i === progress.index ? 'step' : undefined}>
      <span class="dot">{#if i < progress.index || progress.index === 4}<Icon name="check" size={11} />{/if}</span>
      <span>{t(s.label)}</span>
    </li>
  {/each}
</ol>
<div class="bn-bar" role="progressbar" aria-label={t('How far along')} aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(progress.fraction * 100)}>
  <span use:css={{ width: `${Math.round(progress.fraction * 100)}%` }}></span>
</div>
