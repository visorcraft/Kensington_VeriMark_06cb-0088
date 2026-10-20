/*
 * test-capture-builder.c — capture programs built from the skeleton and a
 * calibration (validity-capture-builder.c).
 *
 * Synthetic calibrations (fixed seeds) must give exactly the programs the
 * code produced when it was verified against the full upstream template and
 * the Windows driver (SHA-256 values below). Run with --print-golden to show
 * the current values after an intended change.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "drivers_api.h"
#include "validity.h"

#include <stdio.h>

static gint failures;

#define CHECK(cond, ...) G_STMT_START {                       \
    if (cond) { printf ("ok   "); }                           \
    else      { printf ("FAIL "); failures++; }               \
    printf (__VA_ARGS__); printf ("\n");                      \
  } G_STMT_END

/* sha256 of {cal1, cal2, capture} for seeds 1..3. Identical for the old
 * builder with the full 18869-byte upstream template (commit 6d0ebf0^) and
 * the current skeleton. */
static const gchar *golden[3][3] = {
  { "fc8d4c0119859e2fa359d0aad4f8d0045cf55900ad2d254096894381b53599f2",
    "2f328e4f037aa0be39cb8b853f8bf0a9bc22b0b2b10d851efd4ec51c84a551db",
    "066b2b5dcb4e9d4ea1cede9cf552699098297cf21812620539c637d35b655895" },
  { "b2178b09666fd54009d0f6a73bf7128f7a68f5ca5658f385363b1532e9bdfbf5",
    "60cc28bb05dfe1e2cb936b72ad9d7c3127edca5c61ef513a292dbc248b82eee9",
    "03e54d5433530615cdcca1932faff0a64ca5723a22968e2a287d1ce7e0aa037e" },
  { "0b66414a7779a5127329136b8b41f8ad27f816a73d2fbfa6a4ac1fa8a6d73277",
    "16da3771e2bc2f18bd49ef1191416785062c9dfc6b1cee79ff8a6ca348ecb7e5",
    "b3e63e783b62a4c546240bc993d811ea9dca23718191aebddfd43497d8c267e9" },
};

static guint32
xs (guint32 *s)
{
  *s ^= *s << 13;
  *s ^= *s >> 17;
  *s ^= *s << 5;
  return *s;
}

/* A plausible calibration: trim spanning 5 bits (as the skeleton's transform
 * record expects), frame and baseline around mid grey. */
static void
synth_calibration (ValidityCalibration *cal, guint32 seed, gint trim_spread)
{
  guint32 s = seed * 747796405u + 1;

  memset (cal, 0, sizeof (*cal));
  for (gint i = 0; i < VALIDITY_CAL_TRIM_LEN; i++)
    cal->trim[i] = (guint8) (110 + xs (&s) % (guint32) (trim_spread + 1));
  cal->trim[0] = 110;
  cal->trim[1] = (guint8) (110 + trim_spread);
  for (gint i = 0; i < VALIDITY_CAL_FRAME_LEN; i++)
    {
      cal->frame[i] = (guint8) (108 + xs (&s) % 40);
      cal->baseline[i] = (guint8) (108 + xs (&s) % 40);
    }
  cal->have_baseline = TRUE;
  cal->valid = TRUE;
}

static gchar *
build_hash (const ValidityCalibration *cal, ValidityCalProgram kind, gsize *len)
{
  g_autoptr(GError) e = NULL;
  g_autofree guint8 *p = validity_cal_build_from_template (cal, kind, len, &e);

  if (p == NULL)
    return NULL;
  return g_compute_checksum_for_data (G_CHECKSUM_SHA256, p, *len);
}

int
main (int argc, char **argv)
{
  static const ValidityCalProgram kinds[3] = {
    VALIDITY_CAL_PROG_CAL1, VALIDITY_CAL_PROG_CAL2, VALIDITY_CAL_PROG_CAPTURE
  };
  static const gsize lens[3] = { 1917, 10269, 18869 };
  gboolean print_golden = argc > 1 && g_str_equal (argv[1], "--print-golden");
  g_autofree gchar *dir = g_dir_make_tmp ("validity-cal-XXXXXX", NULL);
  ValidityCalibration cal;

  g_setenv ("TEST_STORAGE_DIR", dir, TRUE);

  for (gint seed = 1; seed <= 3; seed++)
    {
      synth_calibration (&cal, seed, 25);
      for (gint k = 0; k < 3; k++)
        {
          gsize len = 0;
          g_autofree gchar *h = build_hash (&cal, kinds[k], &len);
          if (print_golden)
            {
              printf ("seed %d kind %d: \"%s\"\n", seed, k, h ? h : "NULL");
              continue;
            }
          CHECK (h && len == lens[k] && g_str_equal (h, golden[seed - 1][k]),
                 "program %s, calibration %d: %zu B, checksum %.12s… as verified",
                 k == 0 ? "calibration 1" : k == 1 ? "calibration 2" : "capture", seed, len, h ? h : "-");
        }
    }
  if (print_golden)
    return 0;

  /* a trim that needs more bits than the skeleton's transform record holds */
  {
    gsize len = 0;
    g_autofree gchar *h = NULL;
    synth_calibration (&cal, 9, 60);
    h = build_hash (&cal, VALIDITY_CAL_PROG_CAPTURE, &len);
    CHECK (h == NULL, "trim needing 6 bits is refused instead of producing a broken program");
  }

  /* validity_capture_build: no calibration -> error, then from calibration.bin */
  {
    g_autoptr(GError) e = NULL;
    gsize len = 0;
    g_autofree guint8 *cmd = validity_capture_build (NULL, VALIDITY_CAPTURE_MODE_ENROLL, &len, &e);
    CHECK (cmd == NULL && e && strstr (e->message, "calibration"),
           "capture without calibration.bin fails with a calibration error");
  }
  {
    g_autoptr(GError) e = NULL;
    g_autofree guint8 *c1 = NULL, *c2 = NULL;
    gsize l1 = 0, l2 = 0;
    ValidityCalibration back;

    synth_calibration (&cal, 2, 25);
    CHECK (validity_cal_save (&cal, &e), "calibration.bin written");
    CHECK (validity_cal_load (&back, &e) && memcmp (back.trim, cal.trim, sizeof cal.trim) == 0 &&
           memcmp (back.frame, cal.frame, sizeof cal.frame) == 0 &&
           memcmp (back.baseline, cal.baseline, sizeof cal.baseline) == 0,
           "calibration.bin read back unchanged");
    c1 = validity_capture_build (NULL, VALIDITY_CAPTURE_MODE_IDENTIFY, &l1, &e);
    c2 = validity_capture_build (NULL, VALIDITY_CAPTURE_MODE_ENROLL, &l2, &e);
    CHECK (c1 && c2 && l1 == 18869 && l2 == 18869 && c1[0] == 0x02,
           "capture program 18869 B, opcode 0x02");
    CHECK (c1 && c2 && c1[0x6d5] == 0x02 && c2[0x6d5] == 0x23 && memcmp (c1, c2, 0x6d5) == 0 &&
           memcmp (c1 + 0x6d6, c2 + 0x6d6, l1 - 0x6d6) == 0,
           "capture mode byte at 0x6d5 (0x02 identify / 0x23 enroll), rest identical");
  }

  printf ("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures != 0;
}
