/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "seed.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u

static uint32_t both32(const unsigned char *p)
{
    uint32_t little = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t big = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                   ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    return little == big ? little : 0;
}

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "test_seed: %s\n", message);
    return 1;
}

static int read_file(const char *path, unsigned char **data, size_t *length)
{
    FILE *file = mgp_fopen(path, "rb");
    long size;
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return -1;
    }
    *data = malloc((size_t)size);
    if (!*data || fread(*data, 1, (size_t)size, file) != (size_t)size) {
        free(*data);
        *data = NULL;
        fclose(file);
        return -1;
    }
    fclose(file);
    *length = (size_t)size;
    return 0;
}

static int check_joliet_file(const unsigned char *image, size_t image_size,
                             const char *name, const char *content,
                             size_t content_length)
{
    const unsigned char *root = image + 24 * SECTOR;
    size_t offset = root[0] + root[32 + root[32]];
    size_t i;

    while (offset < 172 && root[offset]) {
        const unsigned char *record = root + offset;
        size_t record_length = record[0], name_length = record[32];
        size_t name_units = name_length / 2;
        uint32_t extent, length;

        if (record_length < 34 || offset + record_length > 172 ||
            name_length > record_length - 33 ||
            (name_length != 1 && (name_length & 1))) {
            return -1;
        }
        if (name_units == strlen(name)) {
            for (i = 0; i < name_units; i++) {
                if (record[33 + i * 2] || record[34 + i * 2] != (unsigned char)name[i]) {
                    break;
                }
            }
            if (i == name_units) {
                extent = both32(record + 2);
                length = both32(record + 10);
                if (!length || (size_t)extent * SECTOR + length > image_size ||
                    (content && (length != content_length ||
                     memcmp(image + (size_t)extent * SECTOR, content, length)))) {
                    return -1;
                }
                return 0;
            }
        }
        offset += record_length;
    }
    return -1;
}

int main(void)
{
    const char *path = "build/test-seed.iso";
    const char *metadata = "instance-id: iid-megas\nlocal-hostname: megas\n";
    unsigned char *image = NULL;
    size_t image_size = 0, old_size;
    char error[256];
    int failed = 0;

    if (mg_seed_write(path, "megas", 1, error, sizeof(error))) {
        fprintf(stderr, "test_seed: seed generation failed: %s\n", error);
        return 1;
    }
    failed |= expect(!read_file(path, &image, &image_size), "could not read ISO output");
    if (failed) return failed;
    failed |= expect(image_size % SECTOR == 0 && image_size < MG_SEED_FILE_MAX,
                     "ISO size is not within the expected sector and file limits");
    failed |= expect(image_size >= 26 * SECTOR, "ISO volume is unexpectedly short");
    failed |= expect(image[16 * SECTOR] == 1 &&
                     !memcmp(image + 16 * SECTOR + 1, "CD001", 5),
                     "primary volume descriptor is missing");
    failed |= expect(image[17 * SECTOR] == 2 &&
                     !memcmp(image + 17 * SECTOR + 1, "CD001", 5) &&
                     !memcmp(image + 17 * SECTOR + 88, "%/E", 3),
                     "Joliet supplementary descriptor is missing");
    failed |= expect(image[18 * SECTOR] == 255 &&
                     !memcmp(image + 18 * SECTOR + 1, "CD001", 5),
                     "volume descriptor terminator is missing");
    failed |= expect(!memcmp(image + 16 * SECTOR + 40, "CIDATA", 6),
                     "primary volume label is not CIDATA");
    failed |= expect(both32(image + 16 * SECTOR + 80) == image_size / SECTOR,
                     "volume sector count does not match the file length");
    failed |= expect(check_joliet_file(image, image_size, "user-data", NULL, 0) == 0,
                     "user-data unexpectedly has an empty payload");
    {
        const unsigned char *root = image + 24 * SECTOR;
        size_t offset = 34 + 34;
        const unsigned char *record = root + offset;
        uint32_t extent = both32(record + 2), length = both32(record + 10);
        const char *user_data = (const char *)image + (size_t)extent * SECTOR;
        failed |= expect(record[32] == 18 && record[34] == 'u' &&
                         length && (size_t)extent * SECTOR + length <= image_size,
                         "Joliet user-data directory entry is malformed");
        if (length && (size_t)extent * SECTOR + length <= image_size) {
            failed |= expect(strstr(user_data, "name: megas\n") != NULL,
                             "cloud-init default user was not set to megas");
            failed |= expect(strstr(user_data, "ALL=(ALL) NOPASSWD:ALL") != NULL,
                             "passwordless sudo rule is missing");
            failed |= expect(strstr(user_data, "qemu-guest-agent") != NULL,
                             "guest agent package bootstrap is missing");
            failed |= expect(strstr(user_data, "password:") == NULL,
                             "user-data unexpectedly includes a password field");
            failed |= expect(strstr(user_data, "lock_passwd: true") != NULL,
                             "cloud-init was not asked to leave the account password locked");
        }
    }
    failed |= expect(!check_joliet_file(image, image_size, "meta-data",
                                        metadata, strlen(metadata)),
                     "NoCloud meta-data is not readable through the Joliet tree");
    if (!mgp_file_size(path) || mgp_file_size(path) != image_size) {
        failed |= expect(0, "atomic ISO output size changed unexpectedly");
    }
    old_size = image_size;
    failed |= expect(mg_seed_write(path, "bad\nname", 1,
                                   error, sizeof(error)) != 0,
                     "invalid username with YAML injection characters was accepted");
    failed |= expect(mg_seed_write(path, "root", 1, error, sizeof(error)) != 0,
                     "reserved root login was accepted as the managed user");
    failed |= expect(mg_seed_write(path, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 1,
                                   error, sizeof(error)) != 0,
                     "uppercase username was accepted");
    failed |= expect(mg_seed_write(path, "abcdefghijklmnopqrstuvwxyzabcdefg", 1,
                                   error, sizeof(error)) != 0,
                     "username longer than 32 bytes was accepted");
    failed |= expect(mgp_file_size(path) == old_size,
                     "rejected input changed the previously written ISO");
    {
        unsigned char *after = NULL;
        size_t after_size = 0;
        failed |= expect(!read_file(path, &after, &after_size) &&
                         after_size == image_size && !memcmp(after, image, image_size),
                         "rejected input changed the previous ISO contents");
        free(after);
    }
    failed |= expect(mg_seed_write(path, "abcdefghijklmnopqrstuvwxyzabcdef", 1,
                                   error, sizeof(error)) == 0,
                     "32-byte Linux username was rejected");
    failed |= expect(mg_seed_write(path, "valid_name-7", 0,
                                   error, sizeof(error)) == 0,
                     "valid Linux username was rejected");
    free(image);
    image = NULL;
    if (!failed && !read_file(path, &image, &image_size)) {
        failed |= expect(check_joliet_file(image, image_size, "meta-data",
                                           metadata, strlen(metadata)) == 0,
                         "regenerated ISO lost its metadata file");
        {
            const unsigned char *root = image + 24 * SECTOR;
            const unsigned char *record = root + 34 + 34;
            uint32_t extent = both32(record + 2), length = both32(record + 10);
            const char *user_data = (const char *)image + (size_t)extent * SECTOR;
            if ((size_t)extent * SECTOR + length <= image_size) {
                failed |= expect(strstr(user_data, "name: valid_name-7\n") != NULL,
                                 "regenerated ISO has the wrong username");
                failed |= expect(strstr(user_data, "ALL=(ALL) ALL") != NULL,
                                 "password-required sudo rule was not selected");
            }
        }
    } else if (!failed) {
        failed |= expect(0, "could not read regenerated ISO");
    }
    free(image);
    mgp_remove(path);
    if (!failed) puts("NoCloud seed ISO checks passed.");
    return failed ? 1 : 0;
}
