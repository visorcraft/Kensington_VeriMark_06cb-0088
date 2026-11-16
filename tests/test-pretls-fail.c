/*
 * test-pretls-fail.c — a failure before step 10 of the fresh-pair ceremony
 * must return the GError from that step. Jumping to the cleanup label past
 * uninitialised g_autofree pointers aborts in free() and drops the error
 * (GitHub issue 10).
 *
 * The driver source is included so the static ceremony is what runs. USB
 * writes fail immediately; nothing here talks to a device.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "validity-pretls-pair.c"

#include <stdio.h>

static gint failures;

#define CHECK(cond, ...) G_STMT_START {                       \
    if (cond) { printf ("ok   "); }                           \
    else      { printf ("FAIL "); failures++; }               \
    printf (__VA_ARGS__); printf ("\n");                      \
  } G_STMT_END

gboolean
validity_usb_write (FpDevice *dev, const guint8 *data, gsize len, GError **error)
{
  (void) dev;
  (void) data;
  (void) len;
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "pretls usb write failed");
  return FALSE;
}

gboolean
validity_usb_read (FpDevice *dev, guint8 *buf, gsize max_len,
                   gsize *out_len, GError **error)
{
  (void) dev;
  (void) buf;
  (void) max_len;
  (void) out_len;
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "pretls usb read failed");
  return FALSE;
}

int
main (void)
{
  GError *error = NULL;
  gboolean ok;

  ok = validity_pairing_run_pretls_fresh_pair_ceremony (NULL, &error);
  CHECK (!ok, "ceremony fails when the first USB write fails");
  CHECK (error != NULL
         && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED)
         && g_strcmp0 (error->message, "pretls usb write failed") == 0,
         "the original USB error is returned (%s)",
         error ? error->message : "no error");
  g_clear_error (&error);

  ok = validity_pairing_run_pretls_fresh_pair_ceremony (NULL, NULL);
  CHECK (!ok, "the same failure with a NULL GError does not abort");

  printf ("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures != 0;
}
