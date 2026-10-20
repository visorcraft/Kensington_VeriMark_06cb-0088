/*
 * test-matcher.c — matcher tests on artificial fingerprints (synth-finger.c).
 *
 * Uses only validity-sift.c; no device, no libfprint. Deterministic.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>

#include "synth-finger.h"
#include "validity-sift.h"

#define THRESHOLD 12          /* driver default (VSIFT_DEFAULT_THRESHOLD) */

static gint failures;

#define CHECK(cond, ...) G_STMT_START {                       \
    if (cond) { printf ("ok   "); }                           \
    else      { printf ("FAIL "); failures++; }               \
    printf (__VA_ARGS__); printf ("\n");                      \
  } G_STMT_END

/* deterministic positions / angles */
static guint32 rng = 4242;
static gdouble
urand (void)
{
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return (rng >> 8) / 16777216.0;
}

static ValiditySiftFeatures *
tap_features (SynthFinger *f, gdouble dx, gdouble dy, gdouble angle, guint32 noise)
{
  guint8 tap[SYNTH_TAP_W * SYNTH_TAP_H];
  gdouble c = synth_finger_size (f) / 2.0;

  synth_finger_tap (f, c + dx, c + dy, angle, noise, tap);
  return validity_sift_extract (tap, SYNTH_TAP_W, SYNTH_TAP_H);
}

/* Best score of @probe over @n templates (the driver's verify rule). */
static gint
best_score (const ValiditySiftFeatures *probe, GPtrArray *print)
{
  gint best = 0;

  for (guint i = 0; i < print->len; i++)
    best = MAX (best, validity_sift_match (probe, g_ptr_array_index (print, i)));
  return best;
}

static gint
cmp_int (gconstpointer a, gconstpointer b)
{
  return *(const gint *) a - *(const gint *) b;
}

/* 1. Same finger matches, other fingers stay below the threshold.
 *
 * Statistical on purpose: the generator builds every loop core from the same
 * model, so cores of different synthetic fingers look more alike than real
 * ones. On 2400 synthetic wrong-finger comparisons one reached 12 (real
 * data, 1800 comparisons: maximum 7). The checks therefore bound the
 * false-accept rate and the bulk of the distribution, plus a hard maximum. */
static void
test_separation (void)
{
  enum { NF = 6, NE = 8, NP = 6 };
  GPtrArray *print[NF];
  GPtrArray *probe[NF];
  gint gen_min = G_MAXINT, gen_fail = 0, imp_max = 0, imp_n = 0, imp_fa = 0;
  g_autoptr(GArray) imp = g_array_new (FALSE, FALSE, sizeof (gint));

  for (gint f = 0; f < NF; f++)
    {
      SynthFinger *sf = synth_finger_new (100 + f);

      print[f] = g_ptr_array_new_with_free_func ((GDestroyNotify) validity_sift_features_free);
      probe[f] = g_ptr_array_new_with_free_func ((GDestroyNotify) validity_sift_features_free);
      for (gint i = 0; i < NE; i++)
        g_ptr_array_add (print[f], tap_features (sf, (urand () - .5) * 80, (urand () - .5) * 80,
                                                 (urand () - .5) * 20, 1000 * f + i));
      for (gint i = 0; i < NP; i++)
        g_ptr_array_add (probe[f], tap_features (sf, (urand () - .5) * 60, (urand () - .5) * 60,
                                                 (urand () - .5) * 30, 5000 + 1000 * f + i));
      synth_finger_free (sf);
    }

  for (gint f = 0; f < NF; f++)
    for (guint p = 0; p < probe[f]->len; p++)
      for (gint g = 0; g < NF; g++)
        {
          gint s = best_score (g_ptr_array_index (probe[f], p), print[g]);
          if (g == f)
            {
              gen_min = MIN (gen_min, s);
              gen_fail += s < THRESHOLD;
            }
          else
            {
              imp_max = MAX (imp_max, s);
              imp_fa += s >= THRESHOLD;
              imp_n++;
              g_array_append_val (imp, s);
            }
        }
  g_array_sort (imp, cmp_int);

  CHECK (imp_fa * 100 <= imp_n, "separation: false accepts at %d: %d of %d (at most 1 %%)",
         THRESHOLD, imp_fa, imp_n);
  CHECK (imp_max < 15, "separation: no other finger reaches 15 (max %d)", imp_max);
  CHECK (g_array_index (imp, gint, imp_n * 95 / 100) <= 8,
         "separation: 95 %% of other-finger scores at most 8 (p95 %d, median %d)",
         g_array_index (imp, gint, imp_n * 95 / 100), g_array_index (imp, gint, imp_n / 2));
  CHECK (gen_fail == 0, "separation: every probe of the enrolled finger matches (%d failed, min score %d)",
         gen_fail, gen_min);

  for (gint f = 0; f < NF; f++)
    {
      g_ptr_array_unref (print[f]);
      g_ptr_array_unref (probe[f]);
    }
}

/* 2. Rotated placement of the same spot still matches. */
static void
test_rotation (void)
{
  static const gdouble angles[] = { -20, -15, -10, -5, 5, 10, 15, 20 };
  SynthFinger *sf = synth_finger_new (7);
  g_autoptr(GPtrArray) print = g_ptr_array_new_with_free_func ((GDestroyNotify) validity_sift_features_free);
  gint worst = G_MAXINT;
  gdouble worst_angle = 0;

  g_ptr_array_add (print, tap_features (sf, 0, 0, 0, 1));
  for (guint i = 0; i < G_N_ELEMENTS (angles); i++)
    {
      g_autoptr(ValiditySiftFeatures) p = tap_features (sf, 3, -4, angles[i], 10 + i);
      gint s = best_score (p, print);
      if (s < worst)
        {
          worst = s;
          worst_angle = angles[i];
        }
    }
  CHECK (worst >= THRESHOLD, "rotation: up to ±20° matches a single enrolled tap (lowest %d at %+.0f°)",
         worst, worst_angle);
  synth_finger_free (sf);
}

/* 3. Coverage: taps from one area do not cover a distant area; adding taps
 *    there (as a print update does) fixes it. */
static void
test_coverage_update (void)
{
  enum { NE = 6, NP = 8 };
  SynthFinger *sf = synth_finger_new (11);
  g_autoptr(GPtrArray) print = g_ptr_array_new_with_free_func ((GDestroyNotify) validity_sift_features_free);
  g_autoptr(GPtrArray) probes = g_ptr_array_new_with_free_func ((GDestroyNotify) validity_sift_features_free);
  gint fail_before = 0, fail_after = 0;

  /* enrollment: upper part of the finger only (window is 144 px tall) */
  for (gint i = 0; i < NE; i++)
    g_ptr_array_add (print, tap_features (sf, (urand () - .5) * 30, -95 + (urand () - .5) * 20, 0, 100 + i));
  /* probes: lower part */
  for (gint i = 0; i < NP; i++)
    g_ptr_array_add (probes, tap_features (sf, (urand () - .5) * 30, 95 + (urand () - .5) * 20,
                                           (urand () - .5) * 16, 200 + i));
  for (guint p = 0; p < probes->len; p++)
    fail_before += best_score (g_ptr_array_index (probes, p), print) < THRESHOLD;

  /* "print update": a second enrollment adds taps of the lower part */
  for (gint i = 0; i < NE; i++)
    g_ptr_array_add (print, tap_features (sf, (urand () - .5) * 30, 95 + (urand () - .5) * 20, 0, 300 + i));
  for (guint p = 0; p < probes->len; p++)
    fail_after += best_score (g_ptr_array_index (probes, p), print) < THRESHOLD;

  CHECK (fail_before >= NP / 2, "coverage: an area not enrolled is not matched (%d/%d probes rejected)",
         fail_before, NP);
  CHECK (fail_after == 0, "coverage: after adding taps of that area all probes match (%d/%d rejected)",
         fail_after, NP);
  synth_finger_free (sf);
}

/* 4. Serialised features (the stored print format) give the same scores. */
static void
test_serialization (void)
{
  SynthFinger *sf = synth_finger_new (21);
  g_autoptr(ValiditySiftFeatures) a = tap_features (sf, 0, 0, 0, 1);
  g_autoptr(ValiditySiftFeatures) b = tap_features (sf, 5, 8, 6, 2);
  g_autoptr(GBytes) bytes = validity_sift_serialize (a);
  gsize len;
  const guint8 *d = g_bytes_get_data (bytes, &len);
  g_autoptr(ValiditySiftFeatures) a2 = validity_sift_deserialize (d, len);
  gint s1 = validity_sift_match (b, a), s2 = a2 ? validity_sift_match (b, a2) : -1;
  g_autofree guint8 *bad = g_memdup2 (d, len);

  CHECK (a2 != NULL && a2->n == a->n, "serialization: round trip keeps %d keypoints", a->n);
  CHECK (abs (s1 - s2) <= 1, "serialization: score before/after round trip %d/%d", s1, s2);
  CHECK (len == 8 + (gsize) a->n * 132, "serialization: %zu bytes for %d keypoints", len, a->n);
  bad[0] ^= 0xff;
  CHECK (validity_sift_deserialize (bad, len) == NULL, "serialization: wrong magic rejected");
  CHECK (validity_sift_deserialize (d, len - 1) == NULL, "serialization: truncated data rejected");
  synth_finger_free (sf);
}

/* 5. The generator is deterministic. */
static void
test_determinism (void)
{
  guint8 t1[SYNTH_TAP_W * SYNTH_TAP_H], t2[SYNTH_TAP_W * SYNTH_TAP_H];
  SynthFinger *f1 = synth_finger_new (5), *f2 = synth_finger_new (5);
  gdouble mean = 0;

  synth_finger_tap (f1, 160, 160, 7, 3, t1);
  synth_finger_tap (f2, 160, 160, 7, 3, t2);
  for (gint i = 0; i < SYNTH_TAP_W * SYNTH_TAP_H; i++)
    mean += t1[i];
  mean /= SYNTH_TAP_W * SYNTH_TAP_H;
  CHECK (memcmp (t1, t2, sizeof t1) == 0, "generator: same seed gives the same tap");
  CHECK (mean > 120 && mean < 137, "generator: grey mean %.1f like the sensor (128.5)", mean);
  synth_finger_free (f1);
  synth_finger_free (f2);
}

int
main (int argc, char **argv)
{
  /* optional: write a few taps for looking at them */
  if (argc == 3 && g_str_equal (argv[1], "--write-taps"))
    {
      guint8 tap[SYNTH_TAP_W * SYNTH_TAP_H];
      for (gint f = 0; f < 3; f++)
        {
          SynthFinger *sf = synth_finger_new (100 + f);
          g_autofree gchar *path = g_strdup_printf ("%s/synth-finger%d.pgm", argv[2], f);
          synth_finger_tap (sf, 160, 160, 0, 1, tap);
          synth_write_pgm (path, tap);
          synth_finger_free (sf);
        }
      return 0;
    }

  test_determinism ();
  test_serialization ();
  test_rotation ();
  test_coverage_update ();
  test_separation ();
  printf ("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures != 0;
}
