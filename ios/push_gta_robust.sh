#!/bin/bash
# Robust GTA data push to the iPhone: per-file retries, skips already-transferred files
# (devicectl skips unmodified files), logs progress. Works over Wi-Fi or USB automatically.
set -u
DEV=${1:-YOUR_IOS_UDID}
BUNDLE=org.macnativegames.gta
R="/Volumes/YOUR_WORKSPACE/Grand Theft Auto V (homebrew port) for Nintendo Switch [NSP]/atmosphere/contents/0100B00B51230000/romfs"
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
SEED=${GTA_SEED_STORE:-/Users/your-user/Games/gta-port/ios-seed-store}
LOG=/tmp/gta-push-robust.log
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
: > "$LOG"

try_copy() {   # source, dest, attempts
  local src=$1 dst=$2 tries=${3:-3} n=0
  while [ $n -lt $tries ]; do
    n=$((n+1))
    if xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer \
         --domain-identifier "$BUNDLE" --timeout 900 --source "$src" --destination "$dst" >>"$LOG" 2>&1; then
      echo "OK   $dst" | tee -a "$LOG"
      return 0
    fi
    echo "RETRY($n/$tries) $dst" | tee -a "$LOG"
    sleep 5
  done
  echo "FAIL $dst" | tee -a "$LOG"
  return 1
}

echo "== seed ==" | tee -a "$LOG"
if ! /usr/bin/python3 "$SCRIPT_DIR/push_gta_seed.py" --device "$DEV" --bundle-id "$BUNDLE" --store "$SEED" >>"$LOG" 2>&1; then
  echo "FAIL shader seed; game transfer not started" | tee -a "$LOG"
  exit 1
fi

echo "== game data ==" | tee -a "$LOG"
while IFS= read -r f; do
  rel="${f#"$R"/}"
  try_copy "$f" "Documents/GameData/gta/$rel"
done < <(find "$R" -type f | sort)
echo "== done ==" | tee -a "$LOG"
