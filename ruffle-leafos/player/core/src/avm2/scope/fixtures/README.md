# ScopeChain regression inputs

The two SWFs are copied byte-for-byte from Ruffle PR
[#24193](https://github.com/ruffle-rs/ruffle/pull/24193), head
`084e2796b53e87662d1b46feff92b6443b917218`, under
`tests/tests/swfs/avm2/scopes_dont_cache/order-{1,2}/test.swf`.

Both upstream Flash-reference outputs contain exactly `inner-value`.
They populate a captured scope chain with narrow namespace lookups in
opposite orders before a broad namespace lookup. The innermost matching
scope must win regardless of lookup history. Runtime `MultinameL` names
prevent the verifier from replacing these lookups with static scope access.

These fixtures retain Ruffle's MIT OR Apache-2.0 license, provided at the
player source root.
