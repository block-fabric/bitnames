<script lang="ts">
  // The pages of BitNames (chain/local.ts lists them): the one shown.
  import Directory from './pages/Directory.svelte';
  import Dns from './pages/Dns.svelte';
  import MyNames from './pages/MyNames.svelte';
  import Paymail from './pages/Paymail.svelte';
  import Register from './pages/Register.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'register'}<Register />
{:else if page === 'names'}<MyNames />
{:else if page === 'directory'}<Directory />
{:else if page === 'paymail'}<Paymail />
{:else if page === 'dns'}<Dns />
{/if}
