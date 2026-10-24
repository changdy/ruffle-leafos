# A2 service fixture paks

This directory owns the five runnable service-behavior fixtures required by
the A2 supervisor phase:

- `normal-exit`
- `ignore-term`
- `orphan-descendant`
- `daemonizes`
- `supervisor-death`

`fixture_service.c` is compiled once and copied into each template pak by
`materialize.py`. The supervisor-death mode keeps descriptor 3 open in both
the leader and its descendant, delays cooperative exit, and uses Linux
`PR_SET_PDEATHSIG` in the descendant. Its macOS fallback polls parent identity
so desktop integration tests can still exercise the lease handoff.

The invalid packages are not vendored here. `make service-fixtures` copies the
canonical A0 fixtures from the sibling `umrk-workspace` checkout into ignored
`build/service-fixtures/` output, preserving symlinks and arranging each case
as an isolated Apps/Userdata scan root. `make service-fixture-test` then runs
the real supervisor against all five behavior paks and every canonical invalid
shape reason-for-reason.

These packages are test assets only and are not part of Jawaka packaging.
