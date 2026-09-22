# Production patch queues

Only hardware-accepted production patches belong here. Each tree has an
ordered `series` file. Empty queues are intentional while migration is being
revalidated.

Candidate and diagnostic patches belong only in a temporary builder workspace
below `/root/ubuntu26_temp/<run>/`; they are ignored if placed under local
`patches/testing/` or `patches/candidates/` by mistake.

Patches are applied to fresh checkouts on the locked upstream branches at
commits from `sources.lock`; Fenix's own git update stays compatible with
those pins.
Do not make patch application conditional on marker strings or retained dirty
state.
