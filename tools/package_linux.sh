#!/bin/bash
# Packs a Release build into dist/Transistor-808-<version>-Linux-x86_64.tar.gz (with an install script)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -nE 's/^project\(Transistor808 VERSION ([0-9.]+)\).*/\1/p' CMakeLists.txt)
ART="build/Transistor808_artefacts/Release"
NAME="Transistor-808-$VERSION-Linux-$(uname -m)"
STAGE="dist/$NAME"

rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -R "$ART/VST3/Transistor 808.vst3"   "$STAGE/"
cp -R "$ART/LV2/Transistor 808.lv2"     "$STAGE/"
cp    "$ART/Standalone/Transistor 808"  "$STAGE/"

cat > "$STAGE/install.sh" <<'EOS'
#!/bin/bash
# Installs Transistor 808 for the current user (no root needed).
set -e
cd "$(dirname "$0")"
mkdir -p ~/.vst3 ~/.lv2 ~/.local/bin
rm -rf ~/.vst3/"Transistor 808.vst3" ~/.lv2/"Transistor 808.lv2"
cp -R "Transistor 808.vst3" ~/.vst3/
cp -R "Transistor 808.lv2"  ~/.lv2/
cp "Transistor 808" ~/.local/bin/transistor808 && chmod +x ~/.local/bin/transistor808
echo "Installed: ~/.vst3/Transistor 808.vst3, ~/.lv2/Transistor 808.lv2, ~/.local/bin/transistor808"
echo "Rescan plug-ins in your DAW (Bitwig, Reaper, Ardour...)."
EOS

cat > "$STAGE/uninstall.sh" <<'EOS'
#!/bin/bash
rm -rf ~/.vst3/"Transistor 808.vst3" ~/.lv2/"Transistor 808.lv2" ~/.local/bin/transistor808
echo "Transistor 808 removed."
EOS
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh"

cat > "$STAGE/README.txt" <<EOS
Transistor 808 $VERSION - circuit-level 808 Rhythm Composer - Transistor Labs 2026

Install:    ./install.sh      (VST3 -> ~/.vst3, LV2 -> ~/.lv2, app -> ~/.local/bin/transistor808)
Uninstall:  ./uninstall.sh
Needs: ALSA or JACK, X11, FreeType (standard on desktop distributions).
EOS

tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
rm -rf "$STAGE"
echo "==> Done: dist/$NAME.tar.gz"
