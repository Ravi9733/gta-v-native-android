#!/bin/bash
# Copy the GTA game data (.rpf tree, 49 GB) + the shader seed pack to the iPhone's
# app container (Documents/GameData/gta and Documents/GameData). Runs when the device
# is reachable (USB or Wi-Fi). Usage: zsh ios/push_gta_data.sh [udid]
set -eu
DEV=${1:-YOUR_IOS_UDID}
BUNDLE=org.macnativegames.gta
R="/Volumes/YOUR_WORKSPACE/Grand Theft Auto V (homebrew port) for Nintendo Switch [NSP]/atmosphere/contents/0100B00B51230000/romfs"
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
SEED=${GTA_SEED_STORE:-/Users/your-user/Games/gta-port/ios-seed-store}
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

echo "== device check =="
xcrun devicectl list devices | grep -q "AFF2642A" || { echo "device not available"; exit 1; }

echo "== seed (pack + marker) =="
/usr/bin/python3 "$SCRIPT_DIR/push_gta_seed.py" --device "$DEV" --bundle-id "$BUNDLE" --store "$SEED"

echo "== game data: 105 files =="
# devicectl copies one item per --source; loop over the tree, one file at a time.
while IFS= read -r f; do
  rel="${f#"$R"/}"
  dest="Documents/GameData/gta/$rel"
  xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
    --source "$f" --destination "$dest"
done < <(find "$R" -type f | sort)
echo "== done =="
