/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "seed.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ISO_SECTOR 2048u
#define ISO_FIRST_DATA_SECTOR 25u
#define ISO_MAX_PATH 1024u

static void set_error(char *error, size_t cap, const char *message)
{
    if (error && cap) {
        snprintf(error, cap, "%s", message);
    }
}

static void put_733(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
    p[4] = (unsigned char)(value >> 24);
    p[5] = (unsigned char)(value >> 16);
    p[6] = (unsigned char)(value >> 8);
    p[7] = (unsigned char)value;
}

static void put_723(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void put_731(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put_732(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void put_be16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void put_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static int checked_username(const char *name)
{
    static const char *const reserved[] = {
        "root", "nobody", "daemon", "bin", "sys", "sync", "shutdown", "halt"
    };
    size_t i, length;

    if (!name || !(length = strlen(name)) || length > 32 ||
        !((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) {
        return 0;
    }
    for (i = 1; i < length; i++) {
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') ||
              name[i] == '_' || name[i] == '-')) {
            return 0;
        }
    }
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (!strcmp(name, reserved[i])) {
            return 0;
        }
    }
    return 1;
}

static int record_size(size_t identifier_length)
{
    size_t result = 33 + identifier_length;

    if ((identifier_length & 1u) == 0) {
        result++;
    }
    return result <= 255 ? (int)result : -1;
}

static unsigned char *put_directory_record(unsigned char *record,
                                           uint32_t extent,
                                           uint32_t data_length,
                                           unsigned flags,
                                           const unsigned char *identifier,
                                           size_t identifier_length)
{
    int length = record_size(identifier_length);
    static const unsigned char date[7] = {126, 1, 1, 0, 0, 0, 0};

    if (length < 0) {
        return NULL;
    }
    memset(record, 0, (size_t)length);
    record[0] = (unsigned char)length;
    put_733(record + 2, extent);
    put_733(record + 10, data_length);
    memcpy(record + 18, date, sizeof(date));
    record[25] = (unsigned char)flags;
    put_723(record + 28, 1);
    record[32] = (unsigned char)identifier_length;
    if (identifier_length) {
        memcpy(record + 33, identifier, identifier_length);
    }
    return record + length;
}

static unsigned char *put_ascii_record(unsigned char *record,
                                       uint32_t extent, uint32_t size,
                                       const char *identifier, unsigned flags)
{
    return put_directory_record(record, extent, size, flags,
                                (const unsigned char *)identifier,
                                strlen(identifier));
}

static unsigned char *put_joliet_record(unsigned char *record,
                                        uint32_t extent, uint32_t size,
                                        const char *identifier, unsigned flags)
{
    unsigned char encoded[64];
    size_t i, length = strlen(identifier);

    if (length > sizeof(encoded) / 2) {
        return NULL;
    }
    for (i = 0; i < length; i++) {
        encoded[i * 2] = 0;
        encoded[i * 2 + 1] = (unsigned char)identifier[i];
    }
    return put_directory_record(record, extent, size, flags, encoded,
                                length * 2);
}

static void put_path_table(unsigned char *path_table, uint32_t root_sector,
                           int big_endian)
{
    memset(path_table, 0, 10);
    path_table[0] = 1; /* Root identifier length. */
    if (big_endian) {
        put_be32(path_table + 2, root_sector);
        put_be16(path_table + 6, 1);
    } else {
        put_731(path_table + 2, root_sector);
        path_table[6] = 1;
    }
    path_table[8] = 0; /* Root directory identifier. */
}

static void set_volume_identifier(unsigned char *volume, int joliet)
{
    size_t i;

    memset(volume, joliet ? 0 : ' ', 32);
    for (i = 0; i < 6; i++) {
        if (joliet) {
            volume[i * 2] = 0;
            volume[i * 2 + 1] = (unsigned char)"CIDATA"[i];
        } else {
            volume[i] = (unsigned char)"CIDATA"[i];
        }
    }
}

static void init_volume_descriptor(unsigned char *descriptor, int type,
                                   uint32_t sector_count, uint32_t root_sector,
                                   uint32_t path_l, uint32_t path_m,
                                   int joliet)
{
    memset(descriptor, 0, ISO_SECTOR);
    descriptor[0] = (unsigned char)type;
    memcpy(descriptor + 1, "CD001", 5);
    descriptor[6] = 1;
    memset(descriptor + 8, ' ', 32);
    memset(descriptor + 40, ' ', 32);
    set_volume_identifier(descriptor + 40, joliet);
    put_733(descriptor + 80, sector_count);
    if (joliet) {
        /* Joliet level 3, with UCS-2BE names in the supplementary tree. */
        memcpy(descriptor + 88, "%/E", 3);
    }
    put_723(descriptor + 120, 1); /* Volume set size. */
    put_723(descriptor + 124, 1); /* Volume sequence number. */
    put_723(descriptor + 128, ISO_SECTOR); /* Logical block size. */
    put_733(descriptor + 132, 10); /* Single root path-table record. */
    put_731(descriptor + 140, path_l);
    put_732(descriptor + 148, path_m);
    if (joliet) {
        unsigned char dot = 0;
        (void)put_directory_record(descriptor + 156, root_sector,
                                   172, 2, &dot, 1);
    } else {
        unsigned char dot = 0;
        (void)put_directory_record(descriptor + 156, root_sector,
                                   156, 2, &dot, 1);
    }
    descriptor[881] = 1; /* File structure version. */
}

static int append_root_directory(unsigned char *directory, int joliet,
                                 uint32_t root_sector, uint32_t data_sector,
                                 uint32_t data_length, uint32_t meta_sector,
                                 uint32_t meta_length)
{
    unsigned char dot = 0, dotdot = 1;
    unsigned char *p = directory;

    if (joliet) {
        p = put_directory_record(p, root_sector, 172, 2, &dot, 1);
        if (!p) return -1;
        p = put_directory_record(p, root_sector, 172, 2, &dotdot, 1);
        if (!p) return -1;
        p = put_joliet_record(p, data_sector, data_length, "user-data", 0);
        if (!p) return -1;
        p = put_joliet_record(p, meta_sector, meta_length, "meta-data", 0);
        if (!p) return -1;
        return (size_t)(p - directory) == 172 ? 0 : -1;
    }
    p = put_directory_record(p, root_sector, 156, 2, &dot, 1);
    if (!p) return -1;
    p = put_directory_record(p, root_sector, 156, 2, &dotdot, 1);
    if (!p) return -1;
    p = put_ascii_record(p, data_sector, data_length, "USER-DATA;1", 0);
    if (!p) return -1;
    p = put_ascii_record(p, meta_sector, meta_length, "META-DATA;1", 0);
    if (!p) return -1;
    return (size_t)(p - directory) == 156 ? 0 : -1;
}

static int build_iso(unsigned char **image_out, size_t *image_size_out,
                     const char *username, int passwordless)
{
    static const char metadata[] =
        "instance-id: iid-megas\n"
        "local-hostname: megas\n";
    char user_data[MG_SEED_USER_DATA_MAX];
    const char *sudo_rule = passwordless ?
        "ALL=(ALL) NOPASSWD:ALL" : "ALL=(ALL) ALL";
    int written;
    size_t user_length, metadata_length = sizeof(metadata) - 1;
    size_t user_sectors, metadata_sectors, total_sectors, image_size;
    uint32_t data_sector = ISO_FIRST_DATA_SECTOR;
    uint32_t meta_sector;
    unsigned char *image;

    written = snprintf(user_data, sizeof(user_data),
        "#cloud-config\n"
        "disable_root: true\n"
        "ssh_pwauth: false\n"
        "user:\n"
        "  name: %s\n"
        "  gecos: Megas\n"
        "  groups: [adm, sudo]\n"
        "  shell: /bin/bash\n"
        "  lock_passwd: true\n"
        "  sudo:\n"
        "    - \"%s\"\n"
        "packages:\n"
        "  - qemu-guest-agent\n"
        "runcmd:\n"
        "  - [systemctl, enable, --now, qemu-guest-agent]\n",
        username, sudo_rule);
    if (written < 0 || (size_t)written >= sizeof(user_data) ||
        (size_t)written > MG_SEED_USER_DATA_MAX) {
        return -1;
    }
    user_length = (size_t)written;
    user_sectors = (user_length + ISO_SECTOR - 1) / ISO_SECTOR;
    metadata_sectors = (metadata_length + ISO_SECTOR - 1) / ISO_SECTOR;
    meta_sector = data_sector + (uint32_t)user_sectors;
    total_sectors = ISO_FIRST_DATA_SECTOR + user_sectors + metadata_sectors;
    if (user_length > MG_SEED_USER_DATA_MAX ||
        user_length > MG_SEED_FILE_MAX || metadata_length > MG_SEED_FILE_MAX ||
        total_sectors > MG_SEED_FILE_MAX / ISO_SECTOR) {
        return -1;
    }
    image_size = total_sectors * ISO_SECTOR;
    image = calloc(1, image_size);
    if (!image) {
        return -1;
    }

    init_volume_descriptor(image + 16 * ISO_SECTOR, 1,
                           (uint32_t)total_sectors, 23, 19, 20, 0);
    init_volume_descriptor(image + 17 * ISO_SECTOR, 2,
                           (uint32_t)total_sectors, 24, 21, 22, 1);
    image[18 * ISO_SECTOR] = 255;
    memcpy(image + 18 * ISO_SECTOR + 1, "CD001", 5);
    image[18 * ISO_SECTOR + 6] = 1;
    put_path_table(image + 19 * ISO_SECTOR, 23, 0);
    put_path_table(image + 20 * ISO_SECTOR, 23, 1);
    put_path_table(image + 21 * ISO_SECTOR, 24, 0);
    put_path_table(image + 22 * ISO_SECTOR, 24, 1);

    if (append_root_directory(image + 23 * ISO_SECTOR, 0, 23,
                              data_sector, (uint32_t)user_length,
                              meta_sector, (uint32_t)metadata_length) ||
        append_root_directory(image + 24 * ISO_SECTOR, 1, 24,
                              data_sector, (uint32_t)user_length,
                              meta_sector, (uint32_t)metadata_length)) {
        free(image);
        return -1;
    }
    memcpy(image + (size_t)data_sector * ISO_SECTOR, user_data, user_length);
    memcpy(image + (size_t)meta_sector * ISO_SECTOR, metadata, metadata_length);
    *image_out = image;
    *image_size_out = image_size;
    return 0;
}

static int write_atomic(const char *path, const unsigned char *image,
                        size_t image_size, char *error, size_t error_cap)
{
    char temporary[ISO_MAX_PATH + 48];
    uint64_t random_value;
    FILE *file = NULL;
    size_t path_length = strlen(path);
    unsigned int attempt;
    int created = 0, failed = 0;

    if (!path_length || path_length >= ISO_MAX_PATH ||
        path_length + 24 >= sizeof(temporary)) {
        set_error(error, error_cap, "Seed ISO path is empty or too long.");
        return -1;
    }
    for (attempt = 0; attempt < 16; attempt++) {
        if (mgp_random(&random_value, sizeof(random_value))) {
            set_error(error, error_cap, "Could not choose a temporary seed path.");
            return -1;
        }
        snprintf(temporary, sizeof(temporary), "%s.tmp.%016llx", path,
                 (unsigned long long)random_value);
        if (!mgp_create_exclusive(temporary)) {
            created = 1;
            break;
        }
    }
    if (!created) {
        set_error(error, error_cap, "Could not create a temporary seed ISO.");
        return -1;
    }
    file = mgp_fopen(temporary, "wb");
    if (!file) {
        mgp_remove(temporary);
        set_error(error, error_cap, "Could not open the temporary seed ISO.");
        return -1;
    }
    if (fwrite(image, 1, image_size, file) != image_size || fflush(file)) {
        failed = 1;
    }
    if (fclose(file)) {
        failed = 1;
    }
    if (failed) {
        mgp_remove(temporary);
        set_error(error, error_cap, "Could not write the complete seed ISO.");
        return -1;
    }
    if (mgp_replace(temporary, path)) {
        mgp_remove(temporary);
        set_error(error, error_cap, "Could not atomically replace the seed ISO.");
        return -1;
    }
    return 0;
}

int mg_seed_write(const char *path, const char *username, int passwordless,
                  char *error, size_t error_cap)
{
    unsigned char *image = NULL;
    size_t image_size = 0;
    int result;

    if (error && error_cap) {
        error[0] = '\0';
    }
    if (!path || !checked_username(username)) {
        set_error(error, error_cap,
                  "Seed needs a valid lowercase Linux username.");
        return -1;
    }
    if (build_iso(&image, &image_size, username, passwordless != 0)) {
        set_error(error, error_cap, "Could not build the NoCloud seed ISO.");
        return -1;
    }
    result = write_atomic(path, image, image_size, error, error_cap);
    free(image);
    return result;
}
