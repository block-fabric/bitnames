<script lang="ts">
  // What BitNames keeps doing on every page from the start of the app (views/Shell.svelte): the names reserved are
  // registered by themselves once their reservation is deep enough (bn/store.svelte.ts), after a restart too, and their
  // progress shows in the top bar (lib/pending.svelte.ts).
  import { startNames } from './bn/store.svelte';

  $effect(() => startNames());
</script>
