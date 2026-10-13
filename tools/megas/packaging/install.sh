#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Install a verified bundle for this user. Never invoke a system package manager.
set -eu
exec 3>&2
diagnostic_dir=${XDG_STATE_HOME:-"$HOME/.local/state"}/megas/installation
if mkdir -p -- "$diagnostic_dir" 2>/dev/null; then
    diagnostic=$diagnostic_dir/last-install.log
else
    diagnostic=$(mktemp "${TMPDIR:-/tmp}/megas-install.XXXXXX" 2>/dev/null) || { printf 'Copy to your agent: Repair Megas installation; no writable diagnostic directory is available.\n' >&3; exit 1; }
fi
exec 2>>"$diagnostic"
bundle=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prefix=${XDG_DATA_HOME:-"$HOME/.local/share"}/megas
desktop=ask
agent=no
for arg in "$@"; do
    case $arg in
        --desktop-icon=yes) desktop=yes ;;
        --desktop-icon=no) desktop=no ;;
        --agent) agent=yes ;;
        --prefix=*) prefix=${arg#--prefix=} ;;
        *) printf 'Copy to your agent: Review the Megas installer arguments. Unknown option: %s. Diagnostic log: %s\n' "$arg" "$diagnostic" >&3; exit 1 ;;
    esac
done
repair() {
    printf '\nCopy this to your agent:\nRepair the Megas per-user installation. Stage: %s. Bundle: %s. Destination: %s. Backup: %s. Diagnostic log: %s. Read packaging/install.sh and SHA256SUMS, preserve my VM disks and other applications, then retry with --desktop-icon=yes or --desktop-icon=no after asking my preference. Do not install global dependencies.\n' "$1" "$bundle" "$prefix" "${backup:-none}" "$diagnostic" >&3
}
stage=checking
staging=
backup=
cleanup() {
    result=$?
    [ -z "$staging" ] || rm -rf -- "$staging" 2>/dev/null || true
    if [ "$result" -ne 0 ]; then
        if [ -n "$backup" ] && [ -d "$backup" ]; then
            if [ -f "$prefix/.megas-install" ]; then rm -rf -- "$prefix" 2>/dev/null || true; fi
            if [ ! -e "$prefix" ]; then mv -- "$backup" "$prefix" 2>/dev/null || true
            else rmdir -- "$backup" 2>/dev/null || true
            fi
        fi
        repair "$stage"
    fi
}
trap cleanup EXIT
printf 'Installer process ID: %s\n' "$$"
printf '[1/4] Checking the bundled application and dependencies…\n'
[ -f "$bundle/SHA256SUMS" ] || exit 1
(cd "$bundle" && sha256sum --quiet -c SHA256SUMS) || exit 1
[ -f "$bundle/megas" ] && [ -f "$bundle/qemu/bin/qemu-system-x86_64" ] || exit 1
case $prefix in /*) ;; *) repair 'destination must be absolute'; exit 1 ;; esac
[ "$prefix" != / ] && [ "$prefix" != "$HOME" ] || exit 1
[ ! -L "$prefix" ] || { stage='destination is a symbolic link'; exit 1; }
if [ "$desktop" = ask ]; then
    if [ "$agent" = yes ] || [ ! -t 0 ]; then
        repair 'desktop shortcut choice required; installing agent must ask its user'
        exit 1
    fi
    printf 'Add a desktop icon? [y/N] '
    read -r choice
    case $choice in y|Y|yes|YES) desktop=yes ;; *) desktop=no ;; esac
fi
stage=copying
printf '[2/4] Installing in your personal application folder…\n'
parent=$(dirname -- "$prefix")
case $parent/ in "$bundle/"*) exit 1 ;; esac
mkdir -p -- "$parent"
staging=$(mktemp -d "$parent/.megas-install.XXXXXX")
cp -R -- "$bundle/." "$staging/"
printf 'Megas managed application directory\n' > "$staging/.megas-install"
if [ -e "$prefix" ]; then
    [ -f "$prefix/.megas-install" ] || exit 1
    backup=$(mktemp -d "$parent/.megas-old.XXXXXX")
    rmdir -- "$backup"
    mv -- "$prefix" "$backup"
fi
mv -- "$staging" "$prefix"
staging=
stage=shortcuts
printf '[3/4] Adding Megas to application search…\n'
applications=${XDG_DATA_HOME:-"$HOME/.local/share"}/applications
mkdir -p -- "$applications"
# Freedesktop Exec quoting; no eval and no shell interpreting user paths.
escaped=$(printf '%s' "$prefix/megas" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g; s/%/%%/g')
cat > "$applications/megas.desktop" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Megas
Comment=See and control your virtual computer
Exec="$escaped" gui
Icon=computer
Terminal=false
Categories=System;Emulator;
Keywords=VM;QEMU;virtual machine;Megas;
EOF
if [ "$desktop" = yes ]; then
    desktop_dir=$(xdg-user-dir DESKTOP 2>/dev/null || printf '%s/Desktop' "$HOME")
    mkdir -p -- "$desktop_dir"
    cp -- "$applications/megas.desktop" "$desktop_dir/megas.desktop"
    chmod u+x -- "$desktop_dir/megas.desktop"
fi
stage=complete
[ -z "$backup" ] || rm -rf -- "$backup"
printf '[4/4] Ready. Search for “Megas” from your application menu.\n'
