/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_SEED_H
#define MEGAS_SEED_H

#include <stddef.h>

/* NoCloud input limits. The generated user-data is intentionally tiny. */
#define MG_SEED_USER_DATA_MAX (64u * 1024u)
#define MG_SEED_FILE_MAX (1024u * 1024u)

/* Write a self-contained ISO9660/Joliet NoCloud seed with volume label
 * CIDATA. The ISO replaces path atomically after it is completely written.
 * username must be an ASCII Linux login name. passwordless selects the
 * generated user's sudo rule; no password is accepted or embedded. */
int mg_seed_write(const char *path, const char *username, int passwordless,
                  char *error, size_t error_cap);

#endif
