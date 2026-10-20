/*
 * validity-printkey.h — encryption of stored print templates.
 *
 * See docs/print-encryption.md.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

/* fpi-data format version of encrypted prints: (u ay ay ay) =
 * (2, key id, nonce, AES-256-GCM ciphertext || tag) of the format-1 aay. */
#define VALIDITY_PRINT_FORMAT_ENCRYPTED 2u

/* systemd credential name and key file name in the storage directory. */
#define VALIDITY_PRINT_KEY_CREDENTIAL "validity0088-print-key"
#define VALIDITY_PRINT_KEY_FILE       "print-key"

/* TRUE unless VALIDITY0088_PRINT_ENCRYPTION=0. */
gboolean validity_print_encryption_enabled (void);

/* Encrypt @templates (type aay) for a print of @device_id (may be NULL).
 * Uses the credential key, else the key file, creating the file when no
 * key exists. Returns a floating (u ay ay ay) or NULL with @error set;
 * G_IO_ERROR_NOT_FOUND means there is no key and none could be created
 * (the caller may then store the print unencrypted). A broken existing key
 * file is never replaced and gives another error. */
GVariant *validity_print_encrypt (GVariant    *templates,
                                  const gchar *device_id,
                                  GError     **error);

/* TRUE if @data is an encrypted print (format 2). */
gboolean validity_print_is_encrypted (GVariant *data);

/* Decrypt an encrypted print's fpi-data. Returns the template array (aay,
 * full reference) or NULL with a FP_DEVICE_ERROR_DATA_INVALID @error. */
GVariant *validity_print_decrypt (GVariant    *data,
                                  const gchar *device_id,
                                  GError     **error);
