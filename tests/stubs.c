/*
 * stubs.c — replacements for the few libfprint / pairing functions that the
 * unit tests link against without a libfprint build. The driver's storage
 * directory is $TEST_STORAGE_DIR.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

gchar *validity_pairing_get_storage_dir (void);
gboolean validity_pairing_ensure_storage_dir (GError **error);
GQuark fp_device_error_quark (void);
GQuark fp_device_retry_quark (void);
gchar *validity_hex_prefix (const guint8 *data, gsize len, gsize max_len);

gchar *
validity_pairing_get_storage_dir (void)
{
  return g_strdup (g_getenv ("TEST_STORAGE_DIR"));
}

gboolean
validity_pairing_ensure_storage_dir (GError **error)
{
  g_mkdir_with_parents (g_getenv ("TEST_STORAGE_DIR"), 0700);
  return TRUE;
}

GQuark
fp_device_error_quark (void)
{
  return g_quark_from_static_string ("fp-device-error-quark");
}

GQuark
fp_device_retry_quark (void)
{
  return g_quark_from_static_string ("fp-device-retry-quark");
}

gchar *
validity_hex_prefix (const guint8 *data, gsize len, gsize max_len)
{
  return g_strdup ("");
}
