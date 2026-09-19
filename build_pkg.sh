#!/bin/bash
# Builds a standalone macOS universal installer (.pkg) for Brother DCP-T230
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_pkg"
PKG_ROOT="$BUILD_DIR/root"
PKG_SCRIPTS="$BUILD_DIR/scripts"
OUTPUT_PKG="$SCRIPT_DIR/Brother_DCP-T230_macOS_Installer.pkg"

echo "==> Compiling universal native binary (arm64 + x86_64)..."
clang -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 \
    "$SCRIPT_DIR/brother_dcpt230_pjl.c" \
    -o "$SCRIPT_DIR/brother_dcpt230_pjl_bin"

echo "==> Preparing package root directory..."
rm -rf "$BUILD_DIR"
mkdir -p "$PKG_ROOT/Library/Printers/Brother/DCP-T230"
mkdir -p "$PKG_ROOT/Library/Printers/PPDs/Contents/Resources"
mkdir -p "$PKG_ROOT/usr/local/libexec/cups/filter"
mkdir -p "$PKG_SCRIPTS"

# 1. Install universal binary in Vendor dir AND in CUPS local filter dir
cp "$SCRIPT_DIR/brother_dcpt230_pjl_bin" "$PKG_ROOT/Library/Printers/Brother/DCP-T230/brother_dcpt230_pjl"
chmod 0755 "$PKG_ROOT/Library/Printers/Brother/DCP-T230/brother_dcpt230_pjl"

cp "$SCRIPT_DIR/brother_dcpt230_pjl_bin" "$PKG_ROOT/usr/local/libexec/cups/filter/brother_dcpt230_pjl"
chmod 0755 "$PKG_ROOT/usr/local/libexec/cups/filter/brother_dcpt230_pjl"

# 2. Install gzipped PPD
gzip -9 -c "$SCRIPT_DIR/brother-dcpt230.ppd" > "$PKG_ROOT/Library/Printers/PPDs/Contents/Resources/Brother-DCP-T230.ppd.gz"
chmod 0644 "$PKG_ROOT/Library/Printers/PPDs/Contents/Resources/Brother-DCP-T230.ppd.gz"

# 3. Create postinstall script
cat > "$PKG_SCRIPTS/postinstall" << 'EOF'
#!/bin/bash
set -e

PRINTER_NAME="DCP_T230"
PPD_PATH="/Library/Printers/PPDs/Contents/Resources/Brother-DCP-T230.ppd.gz"
VENDOR_FILTER="/Library/Printers/Brother/DCP-T230/brother_dcpt230_pjl"
LOCAL_FILTER="/usr/local/libexec/cups/filter/brother_dcpt230_pjl"
SYS_FILTER="/usr/libexec/cups/filter/brother_dcpt230_pjl"

# Fix permissions
chmod 0755 "$VENDOR_FILTER" || true
chown root:wheel "$VENDOR_FILTER" || true

# Ensure filter exists in /usr/local/libexec/cups/filter
mkdir -p /usr/local/libexec/cups/filter
cp -f "$VENDOR_FILTER" "$LOCAL_FILTER" || ln -sf "$VENDOR_FILTER" "$LOCAL_FILTER" || true
chmod 0755 "$LOCAL_FILTER" || true
chown root:wheel "$LOCAL_FILTER" || true

# Also try system filter dir if writable
if mkdir -p /usr/libexec/cups/filter 2>/dev/null; then
    ln -sf "$VENDOR_FILTER" "$SYS_FILTER" 2>/dev/null || cp -f "$VENDOR_FILTER" "$SYS_FILTER" 2>/dev/null || true
    chmod 0755 "$SYS_FILTER" 2>/dev/null || true
    chown -h root:wheel "$SYS_FILTER" 2>/dev/null || true
fi

chmod 0644 "$PPD_PATH" || true
chown root:wheel "$PPD_PATH" || true

# Restart CUPS to reload filter registry
killall -HUP cupsd 2>/dev/null || true
sleep 1

# Auto-register printer if USB cable is plugged in
if command -v lpinfo >/dev/null 2>&1; then
    URI="$(lpinfo -v 2>/dev/null | awk '/usb:.*DCP[_-]?T230/ {print $2; exit}')"
    if [ -n "$URI" ]; then
        lpadmin -p "$PRINTER_NAME" -E -v "$URI" -P "$PPD_PATH" -o media=Letter
        lpadmin -p "$PRINTER_NAME" -o printer-is-shared=false || true
        cupsenable "$PRINTER_NAME" || true
        cupsaccept "$PRINTER_NAME" || true
    fi
fi

exit 0
EOF
chmod 0755 "$PKG_SCRIPTS/postinstall"

# Clean extended attributes and dot-underscore files
COPYFILE_DISABLE=1
export COPYFILE_DISABLE
find "$PKG_ROOT" -name "._*" -delete || true
find "$PKG_SCRIPTS" -name "._*" -delete || true
dot_clean -m "$PKG_ROOT" || true
dot_clean -m "$PKG_SCRIPTS" || true

echo "==> Building .pkg installer..."
pkgbuild \
    --root "$PKG_ROOT" \
    --scripts "$PKG_SCRIPTS" \
    --identifier "com.brother.dcpt230.driver" \
    --version "1.0.1" \
    --ownership recommended \
    --install-location "/" \
    "$OUTPUT_PKG"

rm -rf "$BUILD_DIR"
echo "==> Package created successfully:"
ls -lh "$OUTPUT_PKG"
