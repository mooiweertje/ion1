#!/bin/bash
set -e

# Define submodules and their target paths
declare -A SUBMODULES=(
    ["https://github.com/void-spark/esp32_win_buildscripts.git"]="buildscripts"
    ["https://github.com/void-spark/esp32_wifi_helper.git"]="components/wifi_helper"
    ["https://github.com/void-spark/esp32_mqtt_helper.git"]="components/mqtt_helper"
    ["https://github.com/void-spark/esp32-button.git"]="components/esp32-button"
)

echo "=== Removing submodules and vendoring them into the repository ==="

for REPO in "${!SUBMODULES[@]}"; do
    TARGET="${SUBMODULES[$REPO]}"

    echo ""
    echo "--- Processing $TARGET ---"

    # Remove submodule link
    git submodule deinit -f "$TARGET" 2>/dev/null || true
    git rm -f "$TARGET" 2>/dev/null || true
    rm -rf ".git/modules/$TARGET"

    # Clone repo temporarily
    TMPDIR=$(mktemp -d)
    echo "Cloning $REPO ..."
    git clone --depth 1 "$REPO" "$TMPDIR"

    # Recreate target directory
    mkdir -p "$TARGET"

    # Copy contents
    echo "Copying files into $TARGET ..."
    cp -r "$TMPDIR"/* "$TARGET"/

    # Cleanup
    rm -rf "$TMPDIR"

    echo "Done with $TARGET"
done

# Remove .gitmodules if empty
if [ -f .gitmodules ]; then
    echo ""
    echo "Cleaning .gitmodules ..."
    sed -i '/^\s*$/d' .gitmodules
fi

echo ""
echo "=== Vendoring complete ==="
echo "Run: git add . && git commit -m \"Vendor submodules\""
