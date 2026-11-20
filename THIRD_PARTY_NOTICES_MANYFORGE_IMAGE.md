# Third-party notices: ManyForge image

This file lists the third-party software that the ManyForge image adds to the reference ROS image: the image built from
`Dockerfile.manyforge` and published as `ghcr.io/pastoriomarco/manyforge`. That image is the reference ROS image, named
by digest, plus ManyForge built from this repository. Everything [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists
for the reference ROS image applies to it unchanged; this file covers only what it adds. ManyForge itself is licensed
under Apache-2.0 (see `LICENSE` and `NOTICE`).

This is a factual inventory, not legal advice.

## Licence files in the image

In addition to the reference ROS image's files under `/usr/share/licenses/manyforge-image/`:

| Path | Contents | Taken from |
| --- | --- | --- |
| `manyforge/` | ManyForge's `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md` and this file | this repository |
| `frontend/<node_modules location>/` | the `LICENSE*`, `LICENCE*`, `COPYING*` and `NOTICE*` files of each npm package below, verbatim | the package as `npm ci` installed it at image build time |
| `frontend/packages.txt` | one line per npm package below: its `node_modules` location, name, version and the licence its `package.json` declares, marking packages that ship no licence file | the same packages |

## Composer frontend bundle

The image holds the Composer frontend's production bundle (`manyforge_composer/frontend/dist`), which contains code of the
npm packages in the frontend's production dependency closure: `manyforge_composer/frontend/package.json`
`dependencies` and theirs, at the versions in its `package-lock.json`. `scripts/ci/frontend-bundle-licences.sh` copies
their owners' licence files and writes `packages.txt` when the image is built, so the list always matches the bundle in
that image; ManyForge never writes or infers a licence for them.
