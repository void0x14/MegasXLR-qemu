#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Intended for asynchronous QEMU guest-exec. All installs happen inside the VM.
set -eu
account=megas
wad=
apply=no
self_delete=no
for arg in "$@"; do
    if [ "$arg" = --self-delete ]; then self_delete=yes; fi
done
early_cleanup() {
    if [ "$self_delete" = yes ]; then
        case $0 in /root/megas-doom-*.sh) rm -f -- "$0" ;; esac
    fi
}
trap early_cleanup EXIT
for arg in "$@"; do
    case $arg in
        --apply) apply=yes ;;
        --self-delete) self_delete=yes ;;
        --user=*) account=${arg#--user=} ;;
        --wad=*) wad=${arg#--wad=} ;;
        *) printf 'Copy to your agent: Repair the Megas Doom guest launch arguments. Unknown option: %s\n' "$arg"; exit 1 ;;
    esac
done
case $account in ''|root|*[!a-zA-Z0-9_-]*) printf 'Copy to your agent: Choose an existing non-root Linux desktop account inside the Megas VM for Doom.\n'; exit 1 ;; esac
fail() {
    printf 'Copy to your agent: Repair the Megas Doom launcher inside the intended Linux VM. %s. Guest user: %s. Read /var/lib/megas/doom/events.log and game.log. Confirm the MEGAS-GUEST identity and working guest desktop. Preserve the host and existing files. Use Freedoom or my own existing WAD; do not download commercial game data.\n' "$1" "$account"
    exit 1
}
[ "$apply" = yes ] || fail 'explicit --apply is required; no changes made'
[ "$(id -u)" = 0 ] || fail 'guest administrator access is required; no changes made'
serial=$(cat /sys/class/dmi/id/product_serial 2>/dev/null || true)
marker=/sys/firmware/qemu_fw_cfg/by_name/opt/megas/guest/raw
if [ "$serial" != MEGAS-GUEST ]; then
    [ -r "$marker" ] && [ "$(cat "$marker")" = 1 ] || fail 'Megas guest identity marker missing; no changes made'
    virtual=$(systemd-detect-virt --vm 2>/dev/null || true)
    case $virtual in qemu|kvm) ;; *) fail 'QEMU guest identity not confirmed; no changes made' ;; esac
fi
id "$account" >/dev/null 2>&1 || fail 'selected desktop account does not exist'
uid=$(id -u "$account")
home=$(getent passwd "$account" | cut -d: -f6)
state=/var/lib/megas/doom
mkdir -p "$state"
chmod 0755 "$state"
log=$state/events.log
game_log=$state/game.log
umask 022
exec 2>>"$log"
stage=checking
complete=no
record() {
    printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*" >> "$log"
}
progress() {
    stage=$1
    printf '%s\n' "$2" > "$state/progress"
    printf '%s\n' "$2"
    record "progress stage=$stage text=$2"
}
install_packages() {
    versions=
    for package in "$@"; do
        version=$(apt-cache policy "$package" | awk '/Candidate:/ { print $2; exit }')
        [ -n "$version" ] && [ "$version" != '(none)' ] || fail "guest package $package has no trusted install candidate"
        case $version in *[!a-zA-Z0-9.+:~_-]*) fail 'unsupported guest package version syntax' ;; esac
        versions="$versions $package=$version"
    done
    record "process.begin program=apt-get action=install versions=$versions"
    # Only package/version tuples constructed above are word-split, never user input.
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 \
        -o Dir::Cache::archives="$state/downloads" -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false install -y \
        --no-allow-unauthenticated --no-install-recommends $versions >> "$log" 2>&1 || fail 'guest game packages could not be installed'
    record 'process.end program=apt-get action=install'
    rm -rf "$state/downloads"
}
cleanup() {
    result=$?
    if [ "$result" -ne 0 ] && [ "$complete" = no ]; then
        record "doom.failed stage=$stage"
        printf 'Copy to your agent: Repair the Megas Doom guest launch. Stage: %s. Read /var/lib/megas/doom/events.log and game.log inside the intended VM. User: %s. Keep all package changes inside the marked VM, preserve the host, and use Freedoom or an existing user-owned WAD.\n' "$stage" "$account"
    fi
    if [ "$self_delete" = yes ]; then
        case $0 in /root/megas-doom-*.sh) rm -f -- "$0" ;; esac
    fi
}
trap cleanup EXIT
record "doom.request user=$account helper_pid=$$ wad=${wad:-freedoom2}"
progress checking '[1/4] Checking the guest desktop and game files…'
runtime=/run/user/$uid
[ -d "$runtime" ] || fail 'no active desktop session exists for the selected guest account'
if [ -n "$wad" ]; then
    case $wad in /*) ;; *) fail 'WAD path must be absolute inside the guest' ;; esac
    [ -f "$wad" ] || fail 'the selected guest WAD file does not exist'
fi
if ! command -v chocolate-doom >/dev/null 2>&1 && [ ! -x /usr/games/chocolate-doom ]; then
    progress installing '[2/4] Installing the Doom engine and free game data inside the VM…'
    command -v apt-get >/dev/null 2>&1 || fail 'this guest needs a supported Debian/Ubuntu package adapter'
    record 'process.begin program=apt-get action=update'
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 update >> "$log" 2>&1 || fail 'trusted guest package metadata could not be refreshed'
    record 'process.end program=apt-get action=update'
    mkdir -p "$state/downloads/partial"
    install_packages chocolate-doom freedoom
else
    progress ready '[2/4] Doom engine already available.'
fi
if [ -z "$wad" ]; then
    for candidate in /usr/share/games/doom/freedoom2.wad /usr/share/doom/freedoom2.wad /usr/share/freedoom/freedoom2.wad; do
        if [ -f "$candidate" ]; then wad=$candidate; break; fi
    done
    if [ -z "$wad" ]; then
        progress installing-data '[2/4] Installing free game data inside the VM…'
        command -v apt-get >/dev/null 2>&1 || fail 'free game data requires a supported guest package adapter'
        mkdir -p "$state/downloads/partial"
        install_packages freedoom
        wad=/usr/share/games/doom/freedoom2.wad
    fi
fi
[ -f "$wad" ] || fail 'game data path is unavailable after installation'
engine=$(command -v chocolate-doom 2>/dev/null || printf '/usr/games/chocolate-doom')
[ -x "$engine" ] || fail 'Doom engine path is unavailable after installation'
display=${DISPLAY:-:0}
wayland=
for socket in "$runtime"/wayland-*; do
    if [ -S "$socket" ]; then wayland=${socket##*/}; break; fi
done
authority=$home/.Xauthority
for candidate in "$home/.Xauthority" "$runtime/gdm/Xauthority"; do
    if [ -f "$candidate" ]; then authority=$candidate; break; fi
done
if [ -f "$state/launcher.pid" ]; then
    previous=$(cat "$state/launcher.pid")
    case $previous in ''|*[!0-9]*) ;; *)
        if kill -0 "$previous" 2>/dev/null && [ -r "/proc/$previous/cmdline" ] && tr '\0' ' ' < "/proc/$previous/cmdline" | grep -q 'chocolate-doom'; then
            complete=yes
            progress running "Doom is already running inside the VM. Launcher PID: $previous."
            exit 0
        fi ;;
    esac
fi
progress launching '[3/4] Opening Doom in the guest desktop…'
record "process.spawn program=$engine account=$account display=$display wayland=${wayland:-none} wad=$wad output=$game_log"
# Detach only this explicitly requested game; its PID and output remain visible.
nohup setsid runuser -u "$account" -- env HOME="$home" XDG_RUNTIME_DIR="$runtime" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" DISPLAY="$display" \
    WAYLAND_DISPLAY="$wayland" XAUTHORITY="$authority" \
    "$engine" -iwad "$wad" -window -width 640 -height 400 > "$game_log" 2>&1 < /dev/null &
pid=$!
printf '%s\n' "$pid" > "$state/launcher.pid"
record "process.started launcher_pid=$pid"
sleep 1
kill -0 "$pid" 2>/dev/null || fail 'Doom did not remain running; inspect the guest game log'
if [ -r "/proc/$pid/task/$pid/children" ]; then record "process.children launcher_pid=$pid children=$(cat "/proc/$pid/task/$pid/children")"; fi
complete=yes
progress running "[4/4] Doom process is running in the VM. PID: $pid. Game output: $game_log. Verify the game on the live screen."
