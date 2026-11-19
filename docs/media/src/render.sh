#!/usr/bin/env bash
# Regenerate the README overview cards: SVG via overview_cards.py, then PNG
# (1080x1350) via headless Firefox. Writes into docs/media/.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
media="$(dirname "$here")"

python3 -I "$here/overview_cards.py" "$media/manyforge-logo.svg" "$media"

# Snap Firefox only reaches non-hidden folders under $HOME, so render there.
tmp="$(mktemp -d -p "$HOME" mf-render.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/profile"
for n in 1 2; do
  firefox --headless --no-remote --profile "$tmp/profile" \
    --screenshot "$tmp/card-$n.png" --window-size=1080,1350 \
    "file://$media/manyforge-overview-$n.svg" >/dev/null 2>&1
  mv "$tmp/card-$n.png" "$media/manyforge-overview-$n.png"
done
echo "Wrote $media/manyforge-overview-{1,2}.{svg,png}"
