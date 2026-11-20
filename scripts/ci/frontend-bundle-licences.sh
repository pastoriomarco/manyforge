#!/usr/bin/env bash
# Copy the licence files of the npm packages the Composer frontend's
# production bundle can contain (its production dependencies and theirs, as
# installed by `npm ci`) verbatim into a directory, one folder per package at
# its node_modules location, with packages.txt listing every package, its
# version and its declared licence. Dockerfile.manyforge runs it; the files are
# the owners', never written here.
#
# Usage: scripts/ci/frontend-bundle-licences.sh <frontend-dir> <destination>
set -euo pipefail
(($# == 2)) || { echo "Usage: $0 <frontend-dir> <destination>" >&2; exit 64; }
destination=$(realpath -m "$2")
mkdir -p "$destination"
cd "$1"
npm query .prod | node -e '
const fs = require("fs");
const path = require("path");
const destination = process.argv[1];
const licence = /^(licen[cs]e|copying|notice)/i;
const rows = [];
for (const row of JSON.parse(fs.readFileSync(0, "utf8"))) {
  if (!row.location) continue;
  const files = fs.readdirSync(row.path).filter(
    (name) => licence.test(name) && fs.statSync(path.join(row.path, name)).isFile());
  const target = path.join(destination, row.location);
  fs.mkdirSync(target, { recursive: true });
  for (const name of files) fs.copyFileSync(path.join(row.path, name), path.join(target, name));
  const declared = typeof row.license === "string" ? row.license : JSON.stringify(row.license ?? null);
  rows.push(`${row.location} ${row.name}@${row.version} ${declared}` +
    (files.length ? "" : " (the package ships no licence file)"));
}
if (!rows.length) throw new Error("no production npm packages found; run npm ci first");
fs.writeFileSync(path.join(destination, "packages.txt"), rows.sort().join("\n") + "\n");
' "$destination"
