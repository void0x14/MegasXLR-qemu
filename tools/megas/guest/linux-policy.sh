#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run INSIDE a Megas-managed Linux guest as root. Requires an explicit apply flag.
set -eu
exec 3>&2
mode=passwordless
account=megas
apply=no
self_delete=no
for arg in "$@"; do
    if [ "$arg" = --self-delete ]; then self_delete=yes; fi
done
early_cleanup() {
    if [ "$self_delete" = yes ]; then
        case $0 in /root/megas-linux-policy-*.sh) rm -f -- "$0" ;; esac
    fi
}
trap early_cleanup EXIT
for arg in "$@"; do
    case $arg in
        --apply) apply=yes ;;
        --self-delete) self_delete=yes ;;
        --user=*) account=${arg#--user=} ;;
        --mode=passwordless) mode=passwordless ;;
        --mode=password) mode=password ;;
        --mode=restore) mode=restore ;;
        *) printf 'Copy to your agent: Repair the Megas Linux guest policy arguments. Unknown option: %s\n' "$arg" >&2; exit 1 ;;
    esac
done
case $account in ''|root|*[!a-zA-Z0-9_-]*) printf 'Copy to your agent: Choose a non-root local guest username for Megas.\n' >&2; exit 1 ;; esac
fail() {
    printf 'Copy to your agent: Repair the Megas Linux guest policy. %s. This helper may run only inside the intended Megas VM; preserve the host and existing accounts. Requested user: %s. Mode: %s. Read /var/lib/megas/policy/events.log for exact diagnostics.\n' "$1" "$account" "$mode" >&3
    exit 1
}
[ "$apply" = yes ] || fail 'explicit --apply is required'
[ "$(id -u)" = 0 ] || fail 'guest administrator access is required'
marker=/sys/firmware/qemu_fw_cfg/by_name/opt/megas/guest/raw
serial=$(cat /sys/class/dmi/id/product_serial 2>/dev/null || true)
if [ "$serial" != MEGAS-GUEST ]; then
    [ -r "$marker" ] && [ "$(cat "$marker")" = 1 ] || fail 'Megas guest identity marker missing; no changes made'
    virtual=$(systemd-detect-virt --vm 2>/dev/null || true)
    case $virtual in qemu|kvm) ;; *) fail 'QEMU guest identity not confirmed; no changes made' ;; esac
fi
state=/var/lib/megas/policy
umask 077
mkdir -p "$state/$account" 2>/dev/null || fail 'the guest policy diagnostic folder is not writable'
log=$state/events.log
temporary_sudoers=
exec 2>>"$log"
printf '%s policy.begin user=%s mode=%s pid=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$account" "$mode" "$$" >> "$log"
cleanup() {
    result=$?
    [ -z "$temporary_sudoers" ] || rm -f "$temporary_sudoers"
    if [ "$result" -ne 0 ]; then
        printf '%s policy.failed user=%s mode=%s status=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$account" "$mode" "$result" >> "$log"
        printf 'Copy to your agent: Repair the Megas Linux guest policy inside the intended VM. User: %s. Mode: %s. Read /var/lib/megas/policy/events.log. Preserve saved account policy and the host; identify the failed command before retrying.\n' "$account" "$mode" >&3
    fi
    if [ "$self_delete" = yes ]; then
        case $0 in /root/megas-linux-policy-*.sh) rm -f -- "$0" ;; esac
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
local_account() {
    awk -F: -v user="$account" '$1 == user { found=1 } END { exit !found }' /etc/passwd
}
snapshot_file() {
    destination=$1
    [ ! -L "$destination" ] || fail "cannot safely snapshot a symbolic-link policy file: $destination"
    key="$state/$account/$(printf '%s' "$destination" | tr / _)"
    absent="$key.absent"
    if [ ! -e "$key" ] && [ ! -e "$absent" ]; then
        if [ -e "$destination" ]; then
            cp -p "$destination" "$key"
        else
            : > "$absent"
        fi
    fi
}
snapshot_settings() {
    if command -v gsettings >/dev/null 2>&1 && command -v runuser >/dev/null 2>&1 && command -v dbus-run-session >/dev/null 2>&1 && local_account; then
        for tuple in 'org.gnome.desktop.screensaver lock-enabled' 'org.gnome.desktop.session idle-delay' 'org.gnome.desktop.lockdown disable-lock-screen'; do
            set -- $tuple
            setting_backup="$state/$account/$1.$2"
            if [ ! -f "$setting_backup" ]; then
                runuser -u "$account" -- dbus-run-session -- gsettings get "$1" "$2" > "$setting_backup" 2>> "$log" || rm -f "$setting_backup"
            fi
        done
    fi
}
snapshot_login_policy() {
    snapshot_file /etc/lightdm/lightdm.conf.d/90-megas.conf
    snapshot_file /etc/sddm.conf.d/90-megas.conf
    snapshot_file /etc/gdm/custom.conf
    snapshot_file /etc/gdm3/custom.conf
    snapshot_file "/etc/sudoers.d/90-megas-$account"
    snapshot_settings
}
restore_login_policy() {
    for destination in /etc/lightdm/lightdm.conf.d/90-megas.conf /etc/sddm.conf.d/90-megas.conf /etc/gdm/custom.conf /etc/gdm3/custom.conf "/etc/sudoers.d/90-megas-$account"; do
        key="$state/$account/$(printf '%s' "$destination" | tr / _)"
        absent="$key.absent"
        if [ -f "$key" ]; then
            mkdir -p "$(dirname "$destination")"
            cp -p "$key" "$destination"
        elif [ -f "$absent" ]; then
            rm -f "$destination"
        fi
    done
    if command -v gsettings >/dev/null 2>&1 && command -v runuser >/dev/null 2>&1 && command -v dbus-run-session >/dev/null 2>&1 && local_account; then
        for tuple in 'org.gnome.desktop.screensaver lock-enabled' 'org.gnome.desktop.session idle-delay' 'org.gnome.desktop.lockdown disable-lock-screen'; do
            set -- $tuple
            setting_backup="$state/$account/$1.$2"
            if [ -f "$setting_backup" ]; then
                runuser -u "$account" -- dbus-run-session -- gsettings set "$1" "$2" "$(cat "$setting_backup")" >> "$log" 2>&1 || fail 'could not restore a saved screen lock setting'
            fi
        done
    fi
}
apply_sudoers_policy() {
    destination="/etc/sudoers.d/90-megas-$account"
    mkdir -p /etc/sudoers.d
    [ ! -L "$destination" ] || fail 'the Megas sudoers destination is a symbolic link; it was preserved'
    temporary_sudoers=$(mktemp /etc/sudoers.d/.megas-policy.XXXXXX)
    printf '# MEGAS-MANAGED-SUDOERS %s\n%s ALL=(ALL) NOPASSWD: ALL\n' "$account" "$account" > "$temporary_sudoers"
    chown root:root "$temporary_sudoers"
    chmod 0440 "$temporary_sudoers"
    command -v visudo >/dev/null 2>&1 || fail 'visudo is required to validate the guest sudo rule'
    visudo -c -f "$temporary_sudoers" >> "$log" 2>&1 || fail 'the managed guest sudo rule did not pass visudo validation'
    if [ -e "$destination" ] && ! cmp -s "$destination" "$temporary_sudoers"; then
        fail 'the Megas sudoers destination already contains other policy and was preserved'
    fi
    mv -f "$temporary_sudoers" "$destination"
    temporary_sudoers=
}
set_daemon() {
    destination=$1
    [ -f "$destination" ] || return 0
    snapshot_file "$destination"
    temporary=$(mktemp "${destination}.megas.XXXXXX")
    awk -v user="$account" '
        BEGIN { daemon=0; seen=0 }
        /^\[daemon\]$/ { daemon=1; seen=1; print; print "AutomaticLoginEnable=true"; print "AutomaticLogin=" user; next }
        /^\[/ { daemon=0 }
        daemon && /^[[:space:]]*(AutomaticLoginEnable|AutomaticLogin)[[:space:]]*=/ { next }
        { print }
        END { if (!seen) print "\n[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=" user }
    ' "$destination" > "$temporary"
    chmod --reference="$destination" "$temporary"
    chown --reference="$destination" "$temporary"
    mv "$temporary" "$destination"
}
if [ "$mode" = password ]; then
    # QGA delivers this as guest-exec stdin. Never use argv, a command prompt,
    # or the diagnostic journal for the password.
    password=$(cat)
    carriage_return=$(printf '\r')
    case $password in *'
'*|*"$carriage_return"*) unset password; fail 'password input must be one UTF-8 line with no control characters' ;; esac
    [ -n "$password" ] || fail 'password input was empty'
fi
if [ "$mode" = passwordless ] || [ "$mode" = password ]; then
    if ! local_account; then
        [ "$mode" = passwordless ] || [ -n "$password" ] || fail 'a password is required before creating the local guest account'
        useradd -m -s /bin/sh "$account" >> "$log" 2>&1 || fail 'could not create the local guest account'
    fi
    if [ ! -f "$state/$account/shadow" ]; then getent shadow "$account" > "$state/$account/shadow"; fi
    snapshot_login_policy
fi
if [ "$mode" = passwordless ]; then
    printf '[1/3] Preparing a passwordless local account…\n'
    passwd -d "$account" >> "$log" 2>&1 || fail 'could not remove guest password'
    printf '[2/4] Enabling passwordless administrator tasks inside this guest…\n'
    apply_sudoers_policy
    printf '[3/4] Enabling automatic desktop sign-in…\n'
    if [ -d /etc/lightdm ]; then
        mkdir -p /etc/lightdm/lightdm.conf.d
        cat > /etc/lightdm/lightdm.conf.d/90-megas.conf <<EOF
[Seat:*]
autologin-user=$account
autologin-user-timeout=0
EOF
    fi
    if [ -d /etc/sddm.conf.d ] || command -v sddm >/dev/null 2>&1; then
        mkdir -p /etc/sddm.conf.d
        cat > /etc/sddm.conf.d/90-megas.conf <<EOF
[Autologin]
User=$account
Relogin=true
EOF
    fi
    set_daemon /etc/gdm/custom.conf
    set_daemon /etc/gdm3/custom.conf
    printf '[4/4] Disabling screen lock for this guest account…\n'
    if command -v gsettings >/dev/null 2>&1 && command -v runuser >/dev/null 2>&1 && command -v dbus-run-session >/dev/null 2>&1; then
        for setting in 'org.gnome.desktop.screensaver lock-enabled false' 'org.gnome.desktop.session idle-delay 0' 'org.gnome.desktop.lockdown disable-lock-screen true'; do
            # Each fixed tuple contains only known schemas/keys/values, not user text.
            set -- $setting
            setting_backup="$state/$account/$1.$2"
            runuser -u "$account" -- dbus-run-session -- gsettings set "$1" "$2" "$3" >> "$log" 2>&1 || printf '%s policy.unsupported-setting schema=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$1" >> "$log"
        done
    fi
elif [ "$mode" = password ]; then
    printf '[1/2] Setting the explicit guest account password…\n'
    printf '%s:%s\n' "$account" "$password" | chpasswd >> "$log" 2>&1 || { unset password; fail 'could not set the guest account password'; }
    unset password
    printf '[2/2] Restoring the saved interactive sign-in and screen-lock policy…\n'
    restore_login_policy
else
    printf '[1/2] Restoring the saved guest account policy…\n'
    [ -f "$state/$account/shadow" ] || fail 'no saved account policy exists'
    awk -F: '{ print $1 ":" $2 }' "$state/$account/shadow" | chpasswd -e >> "$log" 2>&1 || fail 'could not restore guest password'
    restore_login_policy
    printf '[2/2] Account policy restored. Set a new password inside the guest if the original account was also passwordless.\n'
fi
printf '%s policy.complete user=%s mode=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$account" "$mode" >> "$log"
printf 'Guest account policy applied. Desktop settings depend on the installed desktop; unsupported settings are recorded in %s.\n' "$log"
