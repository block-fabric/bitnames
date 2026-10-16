# The page scripts of BitNames' own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# bitnames.js: a name taken, set up, paid and given, with the DNS server of the node on.
webui_local_e2e(bitnames 29780 --app-arg=-dnsbind=127.0.0.1:29786)
# bitnames_simple.js: Simple mode, as a user chooses it: Get a name in plain words (free, taken, yours),
# My names in a few words, in Midnight and Light.
webui_local_e2e(bitnames_simple 41860 --js-var SIMPLE=true)
