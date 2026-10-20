/*
 * test-print.c — stored prints: encryption (validity-printkey.c), the print
 * data builder and its fallback, and all print formats scored with the
 * driver's code on artificial fingerprints (validity-match.c).
 *
 * The driver's storage directory is $TEST_STORAGE_DIR (stubs.c); each group
 * uses a fresh temporary directory.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "drivers_api.h"
#include "validity.h"
#include "validity-match.h"
#include "validity-printkey.h"
#include "synth-finger.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static gint failures;

#define CHECK(cond, ...) G_STMT_START {                       \
    if (cond) { printf ("ok   "); }                           \
    else      { printf ("FAIL "); failures++; }               \
    printf (__VA_ARGS__); printf ("\n");                      \
  } G_STMT_END

static gchar *
fresh_storage (void)
{
  gchar *dir = g_dir_make_tmp ("validity-test-XXXXXX", NULL);

  g_setenv ("TEST_STORAGE_DIR", dir, TRUE);
  g_unsetenv ("CREDENTIALS_DIRECTORY");
  g_unsetenv ("VALIDITY0088_PRINT_ENCRYPTION");
  return dir;
}

static GVariant *
sample_templates (void)
{
  GVariantBuilder b;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));
  for (gint i = 0; i < 10; i++)
    {
      guint8 buf[3000];
      for (gint j = 0; j < 3000; j++)
        buf[j] = (guint8) (i * 7 + j);
      g_variant_builder_add_value (&b, g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, buf, sizeof buf, 1));
    }
  return g_variant_ref_sink (g_variant_builder_end (&b));
}

/* copy of an encrypted print with one byte of child @child flipped */
static GVariant *
tamper (GVariant *v, gint child, gsize at)
{
  GVariant *kids[4];
  g_autoptr(GVariant) c = g_variant_get_child_value (v, child);
  gsize n;
  const guint8 *src = g_variant_get_fixed_array (c, &n, 1);
  g_autofree guint8 *d = g_memdup2 (src, n);
  GVariant *r;

  d[at % n] ^= 1;
  for (gint i = 0; i < 4; i++)
    kids[i] = i == child ? g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, d, n, 1)
                         : g_variant_get_child_value (v, i);
  r = g_variant_ref_sink (g_variant_new_tuple (kids, 4));
  for (gint i = 0; i < 4; i++)
    if (i != child)
      g_variant_unref (kids[i]);
  return r;
}

static guint32
format_of (GVariant *v)
{
  guint32 f = 0;

  g_variant_get_child (v, 0, "u", &f);
  return f;
}

static void
test_encryption (void)
{
  g_autofree gchar *dir = fresh_storage ();
  g_autofree gchar *keyfile = g_build_filename (dir, "print-key", NULL);
  g_autofree gchar *cred = g_build_filename (dir, "cred", NULL);
  g_autofree gchar *credkey = g_build_filename (cred, VALIDITY_PRINT_KEY_CREDENTIAL, NULL);
  g_autoptr(GVariant) tpl = sample_templates ();
  g_autoptr(GVariant) enc = NULL, enc2 = NULL, dec = NULL, f1 = NULL;
  g_autoptr(GVariant) id1 = NULL, id2 = NULL;
  g_autoptr(GError) e = NULL;
  struct stat st;
  guint8 k2[32];
  gsize esz;

  enc = validity_print_encrypt (g_variant_ref (tpl), "dev1", &e);
  if (enc)
    g_variant_ref_sink (enc);
  CHECK (enc && stat (keyfile, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == 32,
         "encryption: key file created at first use (32 B, mode 600)");
  if (enc == NULL)
    return;
  CHECK (validity_print_is_encrypted (enc), "encryption: format 2 recognised");
  dec = validity_print_decrypt (enc, "dev1", &e);
  CHECK (dec && g_variant_equal (dec, tpl), "encryption: round trip gives the same templates");
  g_clear_pointer (&dec, g_variant_unref);
  esz = g_variant_get_size (enc);
  CHECK (esz < g_variant_get_size (tpl) + 64, "encryption: overhead %zu B (< 64)", esz - g_variant_get_size (tpl));
  CHECK (memmem (g_variant_get_data (enc), esz, (guint8[]) { 0, 1, 2, 3, 4, 5, 6, 7 }, 8) == NULL,
         "encryption: plaintext not visible in the ciphertext");

  g_clear_error (&e);
  dec = validity_print_decrypt (enc, "dev2", &e);
  CHECK (dec == NULL && e != NULL, "encryption: print of another device id rejected");
  g_clear_error (&e);
  g_clear_pointer (&dec, g_variant_unref);
  dec = validity_print_decrypt (enc, NULL, &e);
  CHECK (dec == NULL, "encryption: missing device id rejected");
  g_clear_pointer (&dec, g_variant_unref);

  for (gint c = 1; c <= 3; c++)
    {
      g_autoptr(GVariant) bad = tamper (enc, c, c == 3 ? 100 : 0);
      g_clear_error (&e);
      dec = validity_print_decrypt (bad, "dev1", &e);
      CHECK (dec == NULL && e != NULL, "encryption: tampered %s rejected",
             c == 1 ? "key id" : c == 2 ? "nonce" : "ciphertext");
      g_clear_pointer (&dec, g_variant_unref);
    }
  {
    g_autoptr(GVariant) bad = tamper (enc, 3, (gsize) -1);
    g_clear_error (&e);
    dec = validity_print_decrypt (bad, "dev1", &e);
    CHECK (dec == NULL && e != NULL, "encryption: tampered tag rejected");
    g_clear_pointer (&dec, g_variant_unref);
  }

  f1 = g_variant_ref_sink (g_variant_new ("(u@aay)", 1, tpl));
  CHECK (!validity_print_is_encrypted (f1) && !validity_print_is_encrypted (tpl),
         "encryption: format 1 and legacy prints not taken as encrypted");

  /* credential key: preferred for new prints, file-key prints still readable */
  g_mkdir (cred, 0700);
  for (gint i = 0; i < 32; i++)
    k2[i] = (guint8) (200 - i);
  g_file_set_contents (credkey, (gchar *) k2, 32, NULL);
  g_setenv ("CREDENTIALS_DIRECTORY", cred, TRUE);
  g_clear_error (&e);
  enc2 = g_variant_ref_sink (validity_print_encrypt (g_variant_ref (tpl), "dev1", &e));
  id1 = g_variant_get_child_value (enc, 1);
  id2 = g_variant_get_child_value (enc2, 1);
  CHECK (!g_variant_equal (id1, id2), "keys: new prints use the credential key");
  dec = validity_print_decrypt (enc, "dev1", &e);
  CHECK (dec != NULL, "keys: print made with the file key still readable");
  g_clear_pointer (&dec, g_variant_unref);
  dec = validity_print_decrypt (enc2, "dev1", &e);
  CHECK (dec && g_variant_equal (dec, tpl), "keys: credential round trip");
  g_clear_pointer (&dec, g_variant_unref);

  unlink (keyfile);
  g_clear_error (&e);
  dec = validity_print_decrypt (enc, "dev1", &e);
  CHECK (dec == NULL && e && strstr (e->message, "not available"),
         "keys: missing key gives a 'not available' error naming the key");
  g_clear_pointer (&dec, g_variant_unref);

  g_file_set_contents (keyfile, "short", 5, NULL);
  unlink (credkey);
  g_clear_error (&e);
  dec = validity_print_encrypt (g_variant_ref (tpl), "dev1", &e);
  {
    g_autofree gchar *c = NULL;
    gsize n = 0;
    g_file_get_contents (keyfile, &c, &n, NULL);
    CHECK (dec == NULL && e && n == 5, "keys: a broken key file fails and is never replaced");
  }
}

/* fpi-data of a print made of the given serialised taps */
static GVariant *
print_data (GPtrArray *taps, const gchar *dev, GError **e)
{
  GVariant *v = validity_tpl_print_data (taps, dev, e);

  return v ? g_variant_ref_sink (v) : NULL;
}

static void
test_print_data_fallback (void)
{
  g_autofree gchar *dir = fresh_storage ();
  g_autoptr(GPtrArray) t = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  g_autoptr(GError) e = NULL;
  g_autoptr(GVariant) v = NULL;

  for (gint i = 0; i < 3; i++)
    g_ptr_array_add (t, g_bytes_new ("keypoints", 9));

  v = print_data (t, "dev", &e);
  CHECK (v && format_of (v) == 2, "print data: encrypted (format 2) by default");
  g_clear_pointer (&v, g_variant_unref);

  if (geteuid () == 0)
    {
      printf ("skip print data: unwritable storage (running as root)\n");
    }
  else
    {
      g_autofree gchar *ro = g_build_filename (dir, "ro", NULL);
      g_autofree gchar *rokey = g_build_filename (ro, "print-key", NULL);

      g_mkdir (ro, 0500);
      g_setenv ("TEST_STORAGE_DIR", ro, TRUE);
      g_clear_error (&e);
      v = print_data (t, "dev", &e);
      CHECK (v && format_of (v) == 1 && e == NULL,
             "print data: storage not writable -> stored unencrypted (format 1)");
      CHECK (!g_file_test (rokey, G_FILE_TEST_EXISTS), "print data: no key file left behind");
      g_clear_pointer (&v, g_variant_unref);
      chmod (ro, 0700);
    }

  {
    g_autofree gchar *br = g_build_filename (dir, "broken", NULL);
    g_autofree gchar *bk = g_build_filename (br, "print-key", NULL);
    g_autofree gchar *c = NULL;
    gsize n = 0;

    g_mkdir (br, 0700);
    g_file_set_contents (bk, "short", 5, NULL);
    g_setenv ("TEST_STORAGE_DIR", br, TRUE);
    g_clear_error (&e);
    v = print_data (t, "dev", &e);
    g_file_get_contents (bk, &c, &n, NULL);
    CHECK (v == NULL && e != NULL && n == 5, "print data: broken key file -> enrollment fails, file kept");
    g_clear_pointer (&v, g_variant_unref);
  }

  g_setenv ("TEST_STORAGE_DIR", dir, TRUE);
  g_setenv ("VALIDITY0088_PRINT_ENCRYPTION", "0", TRUE);
  g_clear_error (&e);
  v = print_data (t, "dev", &e);
  CHECK (v && format_of (v) == 1, "print data: VALIDITY0088_PRINT_ENCRYPTION=0 -> format 1");
  g_unsetenv ("VALIDITY0088_PRINT_ENCRYPTION");
}

/* Taps of synthetic finger @seed: raw images and serialised keypoints. */
static void
synth_taps (guint32 seed, gint n, guint32 noise0, GPtrArray *raw, GPtrArray *ser)
{
  SynthFinger *f = synth_finger_new (seed);
  gdouble c = synth_finger_size (f) / 2.0;

  for (gint i = 0; i < n; i++)
    {
      guint8 *tap = g_malloc (SYNTH_TAP_W * SYNTH_TAP_H);
      synth_finger_tap (f, c + (i % 3 - 1) * 20, c + (i / 3 - 1) * 25, (i % 5 - 2) * 4, noise0 + i, tap);
      if (ser != NULL)
        {
          g_autoptr(ValiditySiftFeatures) feat = validity_sift_extract (tap, SYNTH_TAP_W, SYNTH_TAP_H);
          g_ptr_array_add (ser, validity_sift_serialize (feat));
        }
      if (raw != NULL)
        g_ptr_array_add (raw, tap);
      else
        g_free (tap);
    }
  synth_finger_free (f);
}

static gint
score (GVariant *data, const gchar *dev, const ValiditySiftFeatures *probe)
{
  gint idx;
  g_autoptr(GError) e = NULL;

  return validity_tpl_best_score (probe, data, dev, &idx, &e);
}

static void
test_formats (void)
{
  g_autofree gchar *dir = fresh_storage ();
  g_autoptr(GPtrArray) raw = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) ser = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  g_autoptr(GPtrArray) praw = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) oraw = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GVariant) fmt2 = NULL, fmt1 = NULL, legacy = NULL;
  g_autoptr(ValiditySiftFeatures) probe = NULL, other = NULL;
  g_autoptr(GPtrArray) loaded = NULL;
  g_autoptr(GError) e = NULL;
  GVariantBuilder b;
  gint s2, s1, sl, so;
  gboolean same = TRUE;

  synth_taps (31, 6, 100, raw, ser);
  synth_taps (31, 1, 900, praw, NULL);        /* probe of the same finger */
  synth_taps (32, 1, 950, oraw, NULL);        /* another finger */
  probe = validity_sift_extract (g_ptr_array_index (praw, 0), SYNTH_TAP_W, SYNTH_TAP_H);
  other = validity_sift_extract (g_ptr_array_index (oraw, 0), SYNTH_TAP_W, SYNTH_TAP_H);

  fmt2 = print_data (ser, "devX", &e);
  g_setenv ("VALIDITY0088_PRINT_ENCRYPTION", "0", TRUE);
  fmt1 = print_data (ser, "devX", &e);
  g_unsetenv ("VALIDITY0088_PRINT_ENCRYPTION");
  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < raw->len; i++)
    g_variant_builder_add_value (&b, g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                     g_ptr_array_index (raw, i), SYNTH_TAP_W * SYNTH_TAP_H, 1));
  legacy = g_variant_ref_sink (g_variant_builder_end (&b));

  s2 = score (fmt2, "devX", probe);
  s1 = score (fmt1, "devX", probe);
  sl = score (legacy, "devX", probe);
  so = score (fmt2, "devX", other);
  CHECK (fmt2 && format_of (fmt2) == 2 && fmt1 && format_of (fmt1) == 1, "formats: format 2 and format 1 written");
  CHECK (s2 >= 12 && s2 == s1, "formats: same finger matches the encrypted and the plain print equally (%d / %d)", s2, s1);
  CHECK (sl >= 12, "formats: legacy raw-scan print still matches (%d)", sl);
  CHECK (so >= 0 && so < 12, "formats: another finger does not match (%d)", so);
  CHECK (score (fmt2, "devY", probe) < 0, "formats: encrypted print of another device id is not usable");

  loaded = validity_tpl_load (fmt2, "devX", &e);
  CHECK (loaded && loaded->len == ser->len, "print update: all %u taps of the encrypted print loaded",
         loaded ? loaded->len : 0);
  for (guint i = 0; loaded && i < loaded->len && i < ser->len; i++)
    same &= g_bytes_equal (g_ptr_array_index (loaded, i), g_ptr_array_index (ser, i));
  CHECK (loaded && same, "print update: loaded taps are identical to the stored ones");
  g_clear_pointer (&loaded, g_ptr_array_unref);
  loaded = validity_tpl_load (legacy, "devX", &e);
  CHECK (loaded && loaded->len == raw->len, "print update: legacy print converted to %u keypoint taps",
         loaded ? loaded->len : 0);
  g_clear_pointer (&loaded, g_ptr_array_unref);
  g_clear_error (&e);
  loaded = validity_tpl_load (fmt2, "devY", &e);
  CHECK (loaded == NULL && e != NULL, "print update: unreadable print refused with an error");
}

int
main (void)
{
  test_encryption ();
  test_print_data_fallback ();
  test_formats ();
  printf ("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures != 0;
}
