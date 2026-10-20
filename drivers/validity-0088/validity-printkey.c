/*
 * validity-printkey.c — Kensington VeriMark (06CB:0088) libfprint driver
 *
 * Encryption of stored print templates (fpi-data) with AES-256-GCM.
 *
 * Keys, in lookup order:
 *   1. $CREDENTIALS_DIRECTORY/validity0088-print-key (systemd credential,
 *      set up by packaging/setup-fprintd.sh --encrypt-prints);
 *   2. print-key in the driver's storage directory (mode 600), created on
 *      the first enrollment when no key exists.
 * New prints use the first key found; decryption picks the key by key id,
 * so prints made with the file key keep working once a credential exists.
 * Design and threat model: docs/print-encryption.md.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity.h"
#include "validity-printkey.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define VPK_KEY_LEN    32
#define VPK_ID_LEN     8
#define VPK_NONCE_LEN  12
#define VPK_TAG_LEN    16
#define VPK_ID_LABEL   "validity-0088 print key"
#define VPK_AAD_LABEL  "validity-0088 print v2"

typedef struct
{
  guint8 key[VPK_KEY_LEN];
  guint8 id[VPK_ID_LEN];
} VpkKey;

static void
vpk_key_clear (gpointer p)
{
  OPENSSL_cleanse (p, sizeof (VpkKey));
}

gboolean
validity_print_encryption_enabled (void)
{
  return g_strcmp0 (g_getenv ("VALIDITY0088_PRINT_ENCRYPTION"), "0") != 0;
}

/* Key id: first 8 bytes of SHA-256(label || key). */
static gboolean
vpk_key_id (VpkKey *k)
{
  guint8 buf[sizeof (VPK_ID_LABEL) - 1 + VPK_KEY_LEN];
  guint8 md[32];
  unsigned int md_len = 0;
  gboolean ok;

  memcpy (buf, VPK_ID_LABEL, sizeof (VPK_ID_LABEL) - 1);
  memcpy (buf + sizeof (VPK_ID_LABEL) - 1, k->key, VPK_KEY_LEN);
  ok = EVP_Digest (buf, sizeof (buf), md, &md_len, EVP_sha256 (), NULL) == 1;
  OPENSSL_cleanse (buf, sizeof (buf));
  if (ok)
    memcpy (k->id, md, VPK_ID_LEN);
  return ok;
}

static gchar *
vpk_hex (const guint8 *d, gsize n)
{
  GString *s = g_string_sized_new (n * 2);

  for (gsize i = 0; i < n; i++)
    g_string_append_printf (s, "%02x", d[i]);
  return g_string_free (s, FALSE);
}

/* Read a 32-byte key file. FALSE without @error if the file does not exist. */
static gboolean
vpk_read_key (const gchar *path, VpkKey *out, GError **error)
{
  g_autofree gchar *data = NULL;
  gsize len = 0;
  g_autoptr(GError) err = NULL;

  if (!g_file_get_contents (path, &data, &len, &err))
    {
      if (!g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_propagate_error (error, g_steal_pointer (&err));
      return FALSE;
    }
  if (len != VPK_KEY_LEN)
    {
      OPENSSL_cleanse (data, len);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s: print key must be %d B, got %zu", path, VPK_KEY_LEN, len);
      return FALSE;
    }
  memcpy (out->key, data, VPK_KEY_LEN);
  OPENSSL_cleanse (data, len);
  if (!vpk_key_id (out))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "SHA-256 failed");
      return FALSE;
    }
  return TRUE;
}

static gchar *
vpk_file_path (void)
{
  g_autofree gchar *dir = validity_pairing_get_storage_dir ();

  return g_build_filename (dir, VALIDITY_PRINT_KEY_FILE, NULL);
}

/* All available keys, credential first. Unreadable key files are logged
 * and skipped. */
static GArray *
vpk_load_keys (void)
{
  GArray *keys = g_array_new (FALSE, TRUE, sizeof (VpkKey));
  const gchar *cred_dir = g_getenv ("CREDENTIALS_DIRECTORY");
  g_autofree gchar *cred_path = NULL;
  g_autofree gchar *file_path = vpk_file_path ();
  const gchar *paths[2];
  VpkKey k;

  g_array_set_clear_func (keys, vpk_key_clear);
  if (cred_dir != NULL && cred_dir[0] != '\0')
    cred_path = g_build_filename (cred_dir, VALIDITY_PRINT_KEY_CREDENTIAL, NULL);
  paths[0] = cred_path;
  paths[1] = file_path;

  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      g_autoptr(GError) err = NULL;

      if (paths[i] == NULL)
        continue;
      if (vpk_read_key (paths[i], &k, &err))
        {
          g_autofree gchar *id = vpk_hex (k.id, VPK_ID_LEN);
          fp_dbg ("print key %s from %s", id, paths[i]);
          g_array_append_val (keys, k);
        }
      else if (err != NULL)
        {
          fp_warn ("validity-0088: ignoring print key: %s", err->message);
        }
    }
  OPENSSL_cleanse (&k, sizeof (k));
  return keys;
}

/* Create the key file with a new random key (mode 600). If another process
 * created it first, use that one. Failing to create or write the file gives
 * G_IO_ERROR_NOT_FOUND ("no key available"); an existing file that is not a
 * valid key gives its own error and is left alone. */
static gboolean
vpk_create_key_file (VpkKey *out, GError **error)
{
  g_autofree gchar *path = vpk_file_path ();
  gsize done = 0;
  int fd;

  {
    g_autoptr(GError) err = NULL;
    if (!validity_pairing_ensure_storage_dir (&err))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "cannot create a print key: %s", err->message);
        return FALSE;
      }
  }
  if (RAND_bytes (out->key, VPK_KEY_LEN) != 1 || !vpk_key_id (out))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "cannot generate a print key");
      return FALSE;
    }

  fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      int e = errno;
      if (e == EEXIST && vpk_read_key (path, out, error))
        return TRUE;
      if (e != EEXIST)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "cannot create %s: %s", path, g_strerror (e));
      return FALSE;
    }
  while (done < VPK_KEY_LEN)
    {
      ssize_t n = write (fd, out->key + done, VPK_KEY_LEN - done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        {
          int e = errno;
          close (fd);
          unlink (path);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "cannot write %s: %s", path, g_strerror (e));
          return FALSE;
        }
      done += n;
    }
  if (fsync (fd) != 0 || close (fd) != 0)
    {
      int e = errno;
      unlink (path);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "cannot write %s: %s", path, g_strerror (e));
      return FALSE;
    }
  fp_info ("validity-0088: created print encryption key %s", path);
  return TRUE;
}

static GByteArray *
vpk_aad (const guint8 *id, const gchar *device_id)
{
  GByteArray *aad = g_byte_array_new ();

  g_byte_array_append (aad, (const guint8 *) VPK_AAD_LABEL,
                       sizeof (VPK_AAD_LABEL) - 1);
  g_byte_array_append (aad, id, VPK_ID_LEN);
  if (device_id != NULL)
    g_byte_array_append (aad, (const guint8 *) device_id, strlen (device_id));
  return aad;
}

static GVariant *
vpk_bytes (const guint8 *d, gsize n)
{
  return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, d, n, 1);
}

GVariant *
validity_print_encrypt (GVariant *templates, const gchar *device_id,
                        GError **error)
{
  g_autoptr(GVariant) owned = g_variant_ref_sink (templates);
  g_autoptr(GVariant) plain = g_variant_get_normal_form (owned);
  g_autoptr(GArray) keys = vpk_load_keys ();
  g_autoptr(GByteArray) aad = NULL;
  g_autofree guint8 *ct = NULL;
  EVP_CIPHER_CTX *ctx = NULL;
  guint8 nonce[VPK_NONCE_LEN];
  gsize plain_len = g_variant_get_size (plain);
  VpkKey k;
  int n = 0, fin = 0;
  gboolean ok;

  if (keys->len > 0)
    k = g_array_index (keys, VpkKey, 0);
  else if (!vpk_create_key_file (&k, error))
    return NULL;

  if (RAND_bytes (nonce, sizeof (nonce)) != 1)
    {
      OPENSSL_cleanse (&k, sizeof (k));
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no random nonce");
      return NULL;
    }

  aad = vpk_aad (k.id, device_id);
  ct = g_malloc (plain_len + VPK_TAG_LEN);
  ctx = EVP_CIPHER_CTX_new ();
  ok = ctx != NULL &&
       EVP_EncryptInit_ex (ctx, EVP_aes_256_gcm (), NULL, NULL, NULL) == 1 &&
       EVP_CIPHER_CTX_ctrl (ctx, EVP_CTRL_GCM_SET_IVLEN, VPK_NONCE_LEN, NULL) == 1 &&
       EVP_EncryptInit_ex (ctx, NULL, NULL, k.key, nonce) == 1 &&
       EVP_EncryptUpdate (ctx, NULL, &n, aad->data, aad->len) == 1 &&
       EVP_EncryptUpdate (ctx, ct, &n, g_variant_get_data (plain), plain_len) == 1 &&
       EVP_EncryptFinal_ex (ctx, ct + n, &fin) == 1 &&
       (gsize) (n + fin) == plain_len &&
       EVP_CIPHER_CTX_ctrl (ctx, EVP_CTRL_GCM_GET_TAG, VPK_TAG_LEN,
                            ct + plain_len) == 1;
  EVP_CIPHER_CTX_free (ctx);
  OPENSSL_cleanse (k.key, sizeof (k.key));
  if (!ok)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "AES-256-GCM encryption failed");
      return NULL;
    }

  return g_variant_new ("(u@ay@ay@ay)", VALIDITY_PRINT_FORMAT_ENCRYPTED,
                        vpk_bytes (k.id, VPK_ID_LEN),
                        vpk_bytes (nonce, VPK_NONCE_LEN),
                        vpk_bytes (ct, plain_len + VPK_TAG_LEN));
}

gboolean
validity_print_is_encrypted (GVariant *data)
{
  guint32 format;

  if (!g_variant_is_of_type (data, G_VARIANT_TYPE ("(uayayay)")))
    return FALSE;
  g_variant_get_child (data, 0, "u", &format);
  return format == VALIDITY_PRINT_FORMAT_ENCRYPTED;
}

GVariant *
validity_print_decrypt (GVariant *data, const gchar *device_id,
                        GError **error)
{
  g_autoptr(GVariant) idv = NULL, noncev = NULL, ctv = NULL;
  g_autoptr(GArray) keys = NULL;
  g_autoptr(GByteArray) aad = NULL;
  g_autofree gchar *id_hex = NULL;
  const guint8 *id, *nonce, *ct;
  gsize id_len, nonce_len, ct_len, plain_len;
  guint8 *plain;
  const VpkKey *k = NULL;
  EVP_CIPHER_CTX *ctx;
  guint32 format;
  int n = 0, fin = 0;
  gboolean ok;
  g_autoptr(GBytes) plain_bytes = NULL;

  if (!validity_print_is_encrypted (data))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Stored print is not an encrypted print");
      return NULL;
    }
  g_variant_get (data, "(u@ay@ay@ay)", &format, &idv, &noncev, &ctv);
  id = g_variant_get_fixed_array (idv, &id_len, 1);
  nonce = g_variant_get_fixed_array (noncev, &nonce_len, 1);
  ct = g_variant_get_fixed_array (ctv, &ct_len, 1);
  if (id_len != VPK_ID_LEN || nonce_len != VPK_NONCE_LEN || ct_len < VPK_TAG_LEN)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Stored print is damaged; please enroll again");
      return NULL;
    }

  id_hex = vpk_hex (id, id_len);
  keys = vpk_load_keys ();
  for (guint i = 0; i < keys->len; i++)
    if (memcmp (g_array_index (keys, VpkKey, i).id, id, VPK_ID_LEN) == 0)
      k = &g_array_index (keys, VpkKey, i);
  if (k == NULL)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Stored print is encrypted with key %s, which is not "
                   "available; restore the key or enroll again", id_hex);
      return NULL;
    }

  plain_len = ct_len - VPK_TAG_LEN;
  plain = g_malloc (MAX (plain_len, 1));
  aad = vpk_aad (id, device_id);
  ctx = EVP_CIPHER_CTX_new ();
  ok = ctx != NULL &&
       EVP_DecryptInit_ex (ctx, EVP_aes_256_gcm (), NULL, NULL, NULL) == 1 &&
       EVP_CIPHER_CTX_ctrl (ctx, EVP_CTRL_GCM_SET_IVLEN, VPK_NONCE_LEN, NULL) == 1 &&
       EVP_DecryptInit_ex (ctx, NULL, NULL, k->key, nonce) == 1 &&
       EVP_DecryptUpdate (ctx, NULL, &n, aad->data, aad->len) == 1 &&
       EVP_DecryptUpdate (ctx, plain, &n, ct, plain_len) == 1 &&
       EVP_CIPHER_CTX_ctrl (ctx, EVP_CTRL_GCM_SET_TAG, VPK_TAG_LEN,
                            (void *) (ct + plain_len)) == 1 &&
       EVP_DecryptFinal_ex (ctx, plain + n, &fin) == 1;
  EVP_CIPHER_CTX_free (ctx);
  if (!ok)
    {
      OPENSSL_cleanse (plain, plain_len);
      g_free (plain);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Stored print (key %s) is damaged or belongs to another "
                   "device; please enroll again", id_hex);
      return NULL;
    }

  /* Authenticated by the GCM tag; still parsed as untrusted (FALSE). */
  plain_bytes = g_bytes_new_take (plain, plain_len);
  return g_variant_ref_sink (
      g_variant_new_from_bytes (G_VARIANT_TYPE ("aay"), plain_bytes, FALSE));
}
