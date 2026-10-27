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

    git -C core switch mono/core-patches-26.7.7
    # commit or rebase changes there, then
    rm core-patches/*.patch
    git -C core format-patch 26.7.7..mono/core-patches-26.7.7 -o ../deps/core-patches

For a new OPNsense release, rebase the branch onto its tag and regenerate.
The patches build on each other, so keep them in order.
