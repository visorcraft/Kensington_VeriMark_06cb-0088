/*
 * validity-match.c — Kensington VeriMark (06CB:0088) libfprint driver
 *
 * Matcher glue between the task SSMs in validity.c and the SIFT matcher in
 * validity-sift.c: scan quality gate, image extraction from 0x51 replies,
 * print (de)serialisation and scoring.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity.h"
#include "validity-match.h"
#include "validity-printkey.h"

#include <math.h>

/* ===========================================================================
 * SIFT matcher glue (matcher itself: validity-sift.c)
 *
 * The sensor is a tiny 56 x 144 window (about 7 x 3 mm) with only 3-9
 * minutiae, so minutiae matching cannot work. Each accepted enrollment tap
 * is reduced to SIFT keypoints (position + 128-value gradient histogram);
 * a scan matches when enough of its keypoints agree with one template under
 * a single rotation/translation (RANSAC). The score is that number of
 * consistent keypoints. On a 4-finger test set (40 scans), same-finger
 * scores were 19-150 (also with the finger rotated by up to 25 degrees),
 * other fingers scored at most 5.
 *
 * Prints store only the keypoints, never the scan itself:
 *   fpi-data = (u aay): format version 1, one serialised feature set per tap;
 *   fpi-data = (u ay ay ay): format 2, the same aay encrypted with
 *   AES-256-GCM (validity-printkey.c), written for all new prints unless
 *   VALIDITY0088_PRINT_ENCRYPTION=0.
 * Prints from the earlier correlation matcher (fpi-data = aay of raw 56x144
 * scans) are still accepted; their keypoints are computed on the fly.
 * ========================================================================= */

#define VTPL_W        VALIDITY_IMAGE_WIDTH
#define VTPL_H        VALIDITY_IMAGE_HEIGHT
#define VTPL_N        VALIDITY_IMAGE_DATA_LEN
#define VTPL_HP_R     6
#define VTPL_MIN_STD  15.0

/* Minimum number of geometrically consistent keypoints for a match.
 * Raise it for fewer false accepts, lower it for fewer false rejects.
 * Can be overridden at run time with VALIDITY0088_SIFT_THRESHOLD. */
#define VSIFT_DEFAULT_THRESHOLD  12

gint
validity_get_match_threshold (void)
{
  const gchar *env = g_getenv ("VALIDITY0088_SIFT_THRESHOLD");
  gchar *end = NULL;
  gint64 value;

  if (env == NULL || env[0] == '\0')
    return VSIFT_DEFAULT_THRESHOLD;

  value = g_ascii_strtoll (env, &end, 10);
  if (end == env || *end != '\0' || value < 4 || value > 10000)
    {
      fp_warn ("ignoring invalid VALIDITY0088_SIFT_THRESHOLD=%s", env);
      return VSIFT_DEFAULT_THRESHOLD;
    }
  return (gint) value;
}

/* Subtract a (2r+1)^2 box mean: removes brightness drift, keeps ridges. */
static void
validity_tpl_highpass (const guint8 *src, gfloat *dst)
{
  guint32 sum[(VTPL_H + 1) * (VTPL_W + 1)];

  memset (sum, 0, sizeof (sum));
  for (gint y = 0; y < VTPL_H; y++)
    {
      guint32 row = 0;

      for (gint x = 0; x < VTPL_W; x++)
        {
          row += src[y * VTPL_W + x];
          sum[(y + 1) * (VTPL_W + 1) + x + 1] =
              sum[y * (VTPL_W + 1) + x + 1] + row;
        }
    }

  for (gint y = 0; y < VTPL_H; y++)
    for (gint x = 0; x < VTPL_W; x++)
      {
        gint x0 = MAX (0, x - VTPL_HP_R), x1 = MIN (VTPL_W, x + VTPL_HP_R + 1);
        gint y0 = MAX (0, y - VTPL_HP_R), y1 = MIN (VTPL_H, y + VTPL_HP_R + 1);
        gdouble s = (gdouble) sum[y1 * (VTPL_W + 1) + x1]
                    - sum[y0 * (VTPL_W + 1) + x1]
                    - sum[y1 * (VTPL_W + 1) + x0]
                    + sum[y0 * (VTPL_W + 1) + x0];

        dst[y * VTPL_W + x] =
            (gfloat) (src[y * VTPL_W + x] - s / ((x1 - x0) * (y1 - y0)));
      }
}

/* Feature set for one stored template, either format. */
static ValiditySiftFeatures *
validity_tpl_features (GVariant *child, gboolean legacy_raw)
{
  gsize n = 0;
  const guint8 *d = g_variant_get_fixed_array (child, &n, 1);

  if (d == NULL)
    return NULL;
  if (legacy_raw)
    return n == VTPL_N ? validity_sift_extract (d, VTPL_W, VTPL_H) : NULL;
  return validity_sift_deserialize (d, n);
}

/* Returns the best keypoint score of @probe over all templates of one
 * print (-1 if the print holds nothing usable) and the index of that
 * template. */
/* The template array (aay) of a print's fpi-data, any format; *legacy is
 * set for prints of raw scans (keypoints still to be computed). */
static GVariant *
validity_tpl_array (GVariant *data, const gchar *device_id, gboolean *legacy,
                    GError **error)
{
  GVariant *templates = NULL;

  *legacy = FALSE;
  if (validity_print_is_encrypted (data))
    return validity_print_decrypt (data, device_id, error);

  if (g_variant_is_of_type (data, G_VARIANT_TYPE ("(uaay)")))
    {
      guint32 format;
      g_variant_get (data, "(u@aay)", &format, &templates);
      if (format != VALIDITY_PRINT_FORMAT_SIFT)
        {
          g_variant_unref (templates);
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                       "Stored print uses unknown format %u; please enroll again",
                       format);
          return NULL;
        }
      return templates;
    }
  if (g_variant_is_of_type (data, G_VARIANT_TYPE ("aay")))
    {
      *legacy = TRUE;
      fp_dbg ("print stores raw scans (older driver); computing keypoints now");
      return g_variant_ref (data);
    }
  g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
               "Stored print has no templates; please enroll again");
  return NULL;
}

GPtrArray *
validity_tpl_load (GVariant *data, const gchar *device_id, GError **error)
{
  g_autoptr(GVariant) templates = NULL;
  g_autoptr(GPtrArray) out = NULL;
  gboolean legacy;
  gsize count;

  templates = validity_tpl_array (data, device_id, &legacy, error);
  if (templates == NULL)
    return NULL;
  out = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  count = g_variant_n_children (templates);
  for (gsize i = 0; i < count; i++)
    {
      g_autoptr(GVariant) child = g_variant_get_child_value (templates, i);
      g_autoptr(ValiditySiftFeatures) tpl = validity_tpl_features (child, legacy);

      if (tpl != NULL)
        g_ptr_array_add (out, validity_sift_serialize (tpl));
    }
  if (out->len == 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Stored print has no usable templates; please enroll again");
      return NULL;
    }
  return g_steal_pointer (&out);
}

gint
validity_tpl_best_score (const ValiditySiftFeatures *probe, GVariant *data,
                         const gchar *device_id, gint *best_idx,
                         GError **error)
{
  g_autoptr(GVariant) templates = NULL;
  gboolean legacy;
  gsize count;
  gint best = -1;

  *best_idx = -1;
  templates = validity_tpl_array (data, device_id, &legacy, error);
  if (templates == NULL)
    return -1;

  count = g_variant_n_children (templates);
  for (gsize i = 0; i < count; i++)
    {
      g_autoptr(GVariant) child = g_variant_get_child_value (templates, i);
      g_autoptr(ValiditySiftFeatures) tpl = validity_tpl_features (child, legacy);
      gint v;

      if (tpl == NULL)
        continue;
      v = validity_sift_match (probe, tpl);
      fp_dbg ("keypoint matches vs template %zu: %d", i, v);
      if (v > best)
        {
          best = v;
          *best_idx = (gint) i;
        }
    }

  if (best < 0)
    g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                 "Stored print has no usable templates; please enroll again");
  return best;
}

void
validity_dump_scan (const guint8 *tpl, const gchar *action, gint finger)
{
  const gchar *dir = g_getenv ("VALIDITY0088_DUMP_SCANS");
  g_autoptr(GDateTime) now = NULL;
  g_autofree gchar *stamp = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *hdr = NULL;
  g_autoptr(GByteArray) pgm = NULL;
  g_autoptr(GError) err = NULL;

  if (dir == NULL || dir[0] == '\0')
    return;
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      fp_warn ("validity-0088: cannot create scan dump dir %s", dir);
      return;
    }

  now = g_date_time_new_now_local ();
  stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
  path = g_strdup_printf ("%s/%s.%03d-%s-f%d.pgm", dir, stamp,
                          g_date_time_get_microsecond (now) / 1000,
                          action, finger);
  hdr = g_strdup_printf ("P5\n%d %d\n255\n", VTPL_W, VTPL_H);
  pgm = g_byte_array_new ();
  g_byte_array_append (pgm, (const guint8 *) hdr, strlen (hdr));
  g_byte_array_append (pgm, tpl, VTPL_N);

  if (!g_file_set_contents_full (path, (const gchar *) pgm->data, pgm->len,
                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err))
    fp_warn ("validity-0088: cannot save scan: %s", err->message);
  else
    fp_dbg ("scan saved to %s", path);
}

/* Reject scans with (almost) no ridge contrast, e.g. a partial/blank touch. */
gboolean
validity_tpl_quality_ok (const guint8 *raw)
{
  g_autofree gfloat *hp = g_new (gfloat, VTPL_N);
  gdouble sum2 = 0;

  validity_tpl_highpass (raw, hp);
  for (gint i = 0; i < VTPL_N; i++)
    sum2 += (gdouble) hp[i] * hp[i];
  fp_dbg ("scan contrast (high-pass rms): %.1f", sqrt (sum2 / VTPL_N));
  return sqrt (sum2 / VTPL_N) >= VTPL_MIN_STD;
}

/* A 0x51 reply holds an image only with status 0 and the full record
 * length. Anything else (seen: 18 B, status 0x0007) means the tap was too
 * short or too light; report it as a retry. */
GError *
validity_no_image_error (const guint8 *plain, gsize plain_len)
{
  g_autofree gchar *hex = NULL;

  if (plain_len == VALIDITY_IMAGE_RECORD_LEN &&
      validity_le16 (plain) == 0)
    return NULL;

  hex = validity_hex_prefix (plain, plain_len, 32);
  return g_error_new (FP_DEVICE_RETRY, FP_DEVICE_RETRY_TOO_SHORT,
                      "device did not return an image (%zu B: %s)",
                      plain_len, hex);
}

/* Keep the scan as a 56 x 144 row-major template. The header says 56 x 144,
 * but the pixel stream is column-major: 56 columns of 144 samples each, so
 * transpose it. */
gboolean
validity_store_template (guint8       *tpl,
                         const guint8 *plain,
                         GError      **error)
{
  const ValidityImageRecord *record = (const ValidityImageRecord *) plain;

  if (!validity_image_record_validate (record))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "not a valid image record");
      return FALSE;
    }

  for (gint x = 0; x < VALIDITY_IMAGE_WIDTH; x++)
    for (gint y = 0; y < VALIDITY_IMAGE_HEIGHT; y++)
      tpl[y * VALIDITY_IMAGE_WIDTH + x] =
          record->pixels[x * VALIDITY_IMAGE_HEIGHT + y];
  return TRUE;
}

/* Print data for the keypoints of all enrollment taps (GBytes from
 * validity_sift_serialize, no images): encrypted (u ay ay ay), or (u aay)
 * with VALIDITY0088_PRINT_ENCRYPTION=0. Returns a floating ref, or NULL
 * with @error set. */
GVariant *
validity_tpl_print_data (GPtrArray *templates, const gchar *device_id,
                         GError **error)
{
  GVariantBuilder builder;
  g_autoptr(GVariant) aay = NULL;
  g_autoptr(GError) err = NULL;
  GVariant *enc;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < templates->len; i++)
    {
      gsize len;
      gconstpointer d = g_bytes_get_data (
          g_ptr_array_index (templates, i), &len);

      g_variant_builder_add_value (&builder,
          g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, d, len, 1));
    }
  aay = g_variant_ref_sink (g_variant_builder_end (&builder));
  if (!validity_print_encryption_enabled ())
    return g_variant_new ("(u@aay)", VALIDITY_PRINT_FORMAT_SIFT, aay);

  enc = validity_print_encrypt (g_variant_ref (aay), device_id, &err);
  if (enc != NULL)
    return enc;
  /* No key and none can be created (storage not writable): keep the
   * enrollment and store this print unencrypted. Any other failure, e.g.
   * a broken key file, fails the enrollment. */
  if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      fp_warn ("validity-0088: print stored unencrypted: %s", err->message);
      return g_variant_new ("(u@aay)", VALIDITY_PRINT_FORMAT_SIFT, aay);
    }
  g_propagate_error (error, g_steal_pointer (&err));
  return NULL;
}
