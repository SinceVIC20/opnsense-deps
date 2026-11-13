# Changes to OPNsense core

The Mono Gateway package ships these patches to OPNsense core files and
`mono-core-patches` applies them on the router: after the package is
installed, and from OPNsense's update hook (`50-mono-core-patches`) after
every core update, which puts the original files back. Removing the package
reverts them. A patch that no longer fits a newer core is skipped and logged,
and that file stays as OPNsense shipped it. Images get the same patches at
build time (`make image`).

`mono-core-patches status` shows what is applied.

## Changing the series

The patches are generated from the core fork's integration branch, which is
the OPNsense release the routers run plus one commit per change:

    git -C core switch mono/core-patches-26.7.9
    # commit or rebase changes there, then
    rm core-patches/*.patch
    git -C core format-patch 26.7.9..mono/core-patches-26.7.9 -o ../deps/core-patches

When a change leaves the series (upstream took it, or it was replaced),
move its old patch file to `core-patches/retired/` instead of deleting it.
Routers that had it applied get it undone on the next apply; elsewhere it
is ignored.

For a new OPNsense release, rebase the branch onto its tag and regenerate.
The patches build on each other, so keep them in order.

## On OPNsense 26.7.9 (branch mono-gateway-new)

26.7.9 replaced the dashboard's widget manager with a new widget system
(opnsense/core #11015). The dashboard changes of the 26.7.7 series were
made again on it (0013-0024): the refresh changes and the narrow/wide
layout fix in its new `opnsense_widgets.js`; the traffic graph, widget
picture and Interface Statistics changes carried over as they were. The
two return-to-tab refresh changes are one change here.

Screenshots of the 26.7.7 dashboard (CPU widget and traffic graph,
2026-10-27 to 11-02) for comparison: port-work/mono-gateway-new/dashboard-reference/.
