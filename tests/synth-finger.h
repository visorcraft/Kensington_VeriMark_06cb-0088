/*
 * synth-finger.h — artificial fingerprints for the matcher tests.
 *
 * A "finger" is a large ridge pattern generated from a seed; a "tap" is a
 * 56 x 144 window of it at a given position and rotation, with noise and
 * contrast variation, scaled like the sensor's images (grey mean 128.5,
 * standard deviation ~58, ridge period 9-11 px — measured on real scans).
 * Everything is deterministic for a given seed.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#define SYNTH_TAP_W 56
#define SYNTH_TAP_H 144

typedef struct _SynthFinger SynthFinger;

/* Generate finger number @seed (~0.3 s). */
SynthFinger *synth_finger_new (guint32 seed);
void         synth_finger_free (SynthFinger *f);

/* Size of the generated pattern (pixels); taps are taken around its centre. */
gint synth_finger_size (const SynthFinger *f);

/* Render a tap centred at (@cx, @cy) of the pattern, rotated by @angle_deg,
 * into @out (SYNTH_TAP_W * SYNTH_TAP_H bytes, row-major). @noise_seed
 * selects the noise and pressure variation of this tap. */
void synth_finger_tap (const SynthFinger *f, gdouble cx, gdouble cy,
                       gdouble angle_deg, guint32 noise_seed, guint8 *out);

/* Write a tap as a binary PGM (for looking at it). */
gboolean synth_write_pgm (const gchar *path, const guint8 *tap);
