<script lang="ts">
  // A name as a domain: its text (text of the chain, shown as it is), then .x; or its number when its text is
  // private (#1739-0029).
  import SafeText from '../../components/SafeText.svelte';
  import { TLD } from './model';

  let { name, seq, big = false, tld = true }: { name?: string | null; seq?: string | null; big?: boolean; tld?: boolean } = $props();
</script>

<span class="bn-domain" class:big>{#if name}<SafeText text={name} />{#if tld}<span class="tld" translate="no">.{TLD}</span>{/if}{:else if seq}<span translate="no">#{seq}</span>{/if}</span>
