# Reserved Trusted-Extension Location

This directory is **not scanned** by the current ManyForge runtime. Copying a
folder here does not install, validate, authorize, or enable an external node.

The former filesystem-root loader and `manyforge.external_node.v0` contract
were deleted in commit `c42087c`. Trusted extensions return only through the
content-addressed v2 package/compiler boundary: implementation availability and
addressed execution. No path loader or Python factory table
will be restored.

Program nodes are built in. For a build-time node example, see
[`../manyforge_example_nodes/README.md`](../manyforge_example_nodes/README.md).
