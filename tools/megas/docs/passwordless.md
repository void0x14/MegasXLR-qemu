# Passwordless policy and first-boot seed

Megas creates a small NoCloud seed ISO for a new managed VM. The ISO has the
`CIDATA` volume label and the `user-data` and `meta-data` files used by
cloud-init. It sets the distribution's configured default cloud user to the
requested Linux login, adds that account to `adm` and `sudo`, and requests
`qemu-guest-agent` so Megas can verify and manage the guest after boot. With
passwordless mode enabled, the seed gives that user `NOPASSWD` sudo. No account
password is included in the ISO.

If passwordless mode is disabled while creating the seed, the sudo rule instead
requires a password. Since the seed never carries one, a password must be set
inside the guest before that sudo rule can be used.

The seed is intended for Ubuntu or Debian cloud images that already have
cloud-init installed and can use its NoCloud datasource. Cloud-init consumes
the seed during first boot. The guest needs access to its configured package
repositories to install `qemu-guest-agent` if it is not already installed.
The seed does not install cloud-init itself. If cloud-init is missing or the
image does not support NoCloud, the seed cannot configure the guest.

The account password remains locked in the initial cloud-init configuration.
The separately guarded Linux guest policy helper applies a passwordless local
login and display-manager auto-login only after Megas verifies the
`MEGAS-GUEST` identity through QEMU Guest Agent. It does not change desktop
themes. A custom or pre-existing guest may have a different account, display
manager, screen-lock service, or domain policy; Megas does not claim to
rewrite those settings by attaching an ISO. Existing disks that have already
completed cloud-init first boot do not reapply this first-boot seed.

This seed format is not a Windows provisioning template. Windows guest
account policy is a separate, explicit operation handled by the Windows guest
helper. An arbitrary Windows or Linux disk cannot have its accounts changed
by the QEMU engine alone.

The cloud-init account fields follow the
[users and groups configuration](https://docs.cloud-init.io/en/latest/reference/yaml_examples/user_groups.html).
NoCloud reads local user-data and meta-data from ISO9660 or VFAT media labelled
`cidata` or `CIDATA`; see the
[NoCloud datasource reference](https://docs.cloud-init.io/en/latest/reference/datasources/nocloud.html).

The generator caps user-data at 64 KiB and the complete ISO at 1 MiB. The
current template is much smaller. Its instance ID stays stable so an ordinary
VM restart does not make cloud-init treat the same disk as a new instance and
rerun first-boot actions.
