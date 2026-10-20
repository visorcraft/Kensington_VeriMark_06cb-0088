/*
 * synth-finger.c — artificial fingerprints for the matcher tests.
 *
 * Ridge patterns are grown from noise by repeated oriented Gabor filtering
 * (the approach of SFinGe-style generators): the orientation field has a
 * core and a delta (zero-pole model) plus a smooth random perturbation, and
 * the ridge frequency varies slightly over the finger. Taps are rotated,
 * shifted windows of the pattern with pressure, blur and noise variation.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "synth-finger.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SF_SIZE   320          /* pattern size in pixels (~16 x 16 mm) */
#define SF_KR     7            /* Gabor kernel radius */
#define SF_ITERS  7
#define SF_ANGLES 32           /* quantised orientations for the kernels */

struct _SynthFinger
{
  gint    n;
  gfloat *ridge;               /* n * n, values in [-1, 1] */
};

/* Small deterministic PRNG (xorshift32), independent of GLib's. */
static guint32
sf_rand (guint32 *s)
{
  guint32 x = *s ? *s : 0x9e3779b9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *s = x;
}

static gdouble
sf_uniform (guint32 *s)
{
  return (sf_rand (s) >> 8) / 16777216.0;
}

static gdouble
sf_gauss (guint32 *s)
{
  gdouble u = MAX (sf_uniform (s), 1e-9), v = sf_uniform (s);
  return sqrt (-2.0 * log (u)) * cos (2.0 * G_PI * v);
}

gint
synth_finger_size (const SynthFinger *f)
{
  return f->n;
}

SynthFinger *
synth_finger_new (guint32 seed)
{
  guint32 s = seed * 2654435761u + 12345u;
  gint n = SF_SIZE, kw = 2 * SF_KR + 1;
  SynthFinger *f = g_new0 (SynthFinger, 1);
  gfloat *theta = g_new (gfloat, n * n), *freq = g_new (gfloat, n * n);
  gfloat *a = g_new (gfloat, n * n), *b = g_new (gfloat, n * n);
  gfloat *kern = g_new (gfloat, SF_ANGLES * 4 * kw * kw);
  gdouble cx = n * (0.35 + 0.3 * sf_uniform (&s));
  gdouble cy = n * (0.30 + 0.25 * sf_uniform (&s));
  gdouble dx = cx + n * (sf_uniform (&s) - 0.5) * 0.5;
  gdouble dy = cy + n * (0.35 + 0.2 * sf_uniform (&s));
  gdouble rot = (sf_uniform (&s) - 0.5) * 0.8;
  gdouble per = 8.5 + 3.0 * sf_uniform (&s);       /* ridge period, px */
  gdouble pa[3], pk[3], pp[3], pq[3];
  /* per-finger texture: slowly varying ridge thickness and ridge breaks */
  enum { NBUMP = 40, NBREAK = 60 };
  gdouble bx[NBUMP], by[NBUMP], bw[NBUMP], ba[NBUMP];
  gdouble kx[NBREAK], ky[NBREAK], kr[NBREAK];
  gboolean whorl = sf_uniform (&s) < 0.3;

  for (gint i = 0; i < NBUMP; i++)
    {
      bx[i] = n * sf_uniform (&s);
      by[i] = n * sf_uniform (&s);
      bw[i] = 15 + 30 * sf_uniform (&s);
      ba[i] = (sf_uniform (&s) - 0.5) * 1.2;
    }
  for (gint i = 0; i < NBREAK; i++)
    {
      kx[i] = n * sf_uniform (&s);
      ky[i] = n * sf_uniform (&s);
      kr[i] = 2.0 + 3.0 * sf_uniform (&s);
    }
  for (gint i = 0; i < 3; i++)
    {
      pa[i] = 0.45 * sf_uniform (&s);
      pk[i] = (0.5 + 2.0 * sf_uniform (&s)) * 2 * G_PI / n;
      pp[i] = 2 * G_PI * sf_uniform (&s);
      pq[i] = 2 * G_PI * sf_uniform (&s);
    }

  /* orientation (zero-pole model: core +, delta -; a whorl has two cores)
   * and frequency fields */
  for (gint y = 0; y < n; y++)
    for (gint x = 0; x < n; x++)
      {
        gdouble t = 0.5 * atan2 (y - cy, x - cx) - 0.5 * atan2 (y - dy, x - dx);
        gdouble pert = 0;
        if (whorl)
          t += 0.5 * atan2 (y - (cy + 18), x - (cx + 6));
        for (gint i = 0; i < 3; i++)
          pert += pa[i] * sin (pk[i] * x + pp[i]) * cos (pk[i] * y + pq[i]);
        theta[y * n + x] = (gfloat) (t + rot + pert);
        freq[y * n + x] = (gfloat) (1.0 / (per * (1.0 + 0.12 * sin (pk[0] * (x + y) + pp[1]))));
      }

  /* Gabor kernels for SF_ANGLES orientations and 4 frequencies */
  for (gint ai = 0; ai < SF_ANGLES; ai++)
    for (gint fi = 0; fi < 4; fi++)
      {
        gdouble ang = G_PI * ai / SF_ANGLES;
        gdouble fr = 1.0 / (per * (0.92 + 0.053 * fi));
        gdouble sx = 3.6, sy = 3.6, sum = 0;
        gfloat *k = kern + (ai * 4 + fi) * kw * kw;
        for (gint v = -SF_KR; v <= SF_KR; v++)
          for (gint u = -SF_KR; u <= SF_KR; u++)
            {
              /* ridges run along ang: the wave goes across them */
              gdouble xr = u * cos (ang) + v * sin (ang);
              gdouble yr = -u * sin (ang) + v * cos (ang);
              gdouble g = exp (-0.5 * (xr * xr / (sx * sx) + yr * yr / (sy * sy))) *
                          cos (2 * G_PI * fr * yr);
              k[(v + SF_KR) * kw + (u + SF_KR)] = (gfloat) g;
              sum += fabs (g);
            }
        for (gint i = 0; i < kw * kw; i++)
          k[i] /= (gfloat) sum;
      }

  for (gint i = 0; i < n * n; i++)
    a[i] = (gfloat) (sf_uniform (&s) * 2 - 1);

  for (gint it = 0; it < SF_ITERS; it++)
    {
      for (gint y = 0; y < n; y++)
        for (gint x = 0; x < n; x++)
          {
            gdouble t = fmod (theta[y * n + x], G_PI);
            gint ai, fi;
            gdouble acc = 0;
            const gfloat *k;

            if (t < 0)
              t += G_PI;
            ai = (gint) lround (t / G_PI * SF_ANGLES) % SF_ANGLES;
            fi = CLAMP ((gint) lround ((1.0 / freq[y * n + x] / per - 0.92) / 0.053), 0, 3);
            k = kern + (ai * 4 + fi) * kw * kw;
            for (gint v = -SF_KR; v <= SF_KR; v++)
              {
                gint yy = CLAMP (y + v, 0, n - 1);
                for (gint u = -SF_KR; u <= SF_KR; u++)
                  acc += k[(v + SF_KR) * kw + (u + SF_KR)] *
                         a[yy * n + CLAMP (x + u, 0, n - 1)];
              }
            b[y * n + x] = (gfloat) tanh (6.0 * acc);
          }
      memcpy (a, b, sizeof (gfloat) * n * n);
    }

  /* Texture: thickness field (sum of Gaussian bumps) shifts the ridge/valley
   * threshold; breaks cut short gaps into ridges (ridge endings, islands). */
  for (gint y = 0; y < n; y++)
    for (gint x = 0; x < n; x++)
      {
        gdouble t = 0, v = a[y * n + x];
        for (gint i = 0; i < NBUMP; i++)
          {
            gdouble ddx = x - bx[i], ddy = y - by[i];
            t += ba[i] * exp (-(ddx * ddx + ddy * ddy) / (2 * bw[i] * bw[i]));
          }
        for (gint i = 0; i < NBREAK; i++)
          {
            gdouble ddx = x - kx[i], ddy = y - ky[i];
            if (ddx * ddx + ddy * ddy < kr[i] * kr[i])
              v = -fabs (v);         /* force valley inside the break */
          }
        b[y * n + x] = (gfloat) tanh (3.0 * (v + 0.5 * t));
      }
  memcpy (a, b, sizeof (gfloat) * n * n);

  f->n = n;
  f->ridge = a;
  g_free (b);
  g_free (theta);
  g_free (freq);
  g_free (kern);
  return f;
}

void
synth_finger_free (SynthFinger *f)
{
  if (f == NULL)
    return;
  g_free (f->ridge);
  g_free (f);
}

static gdouble
sf_sample (const SynthFinger *f, gdouble x, gdouble y)
{
  gint n = f->n, x0 = (gint) floor (x), y0 = (gint) floor (y);
  gdouble fx = x - x0, fy = y - y0;
#define P(xx, yy) f->ridge[CLAMP (yy, 0, n - 1) * n + CLAMP (xx, 0, n - 1)]
  return (1 - fy) * ((1 - fx) * P (x0, y0) + fx * P (x0 + 1, y0)) +
         fy * ((1 - fx) * P (x0, y0 + 1) + fx * P (x0 + 1, y0 + 1));
#undef P
}

void
synth_finger_tap (const SynthFinger *f, gdouble cx, gdouble cy,
                  gdouble angle_deg, guint32 noise_seed, guint8 *out)
{
  guint32 s = noise_seed * 2246822519u + 7u;
  gdouble c = cos (angle_deg * G_PI / 180), sn = sin (angle_deg * G_PI / 180);
  gdouble gain = 0.8 + 0.4 * sf_uniform (&s);        /* pressure */
  gdouble bias = (sf_uniform (&s) - 0.5) * 0.5;       /* wet / dry */
  gdouble noise = 0.15 + 0.15 * sf_uniform (&s);
  gfloat v[SYNTH_TAP_W * SYNTH_TAP_H];
  gdouble mean = 0, var = 0;
  gint np = SYNTH_TAP_W * SYNTH_TAP_H;

  for (gint y = 0; y < SYNTH_TAP_H; y++)
    for (gint x = 0; x < SYNTH_TAP_W; x++)
      {
        gdouble u = x - SYNTH_TAP_W / 2.0, w = y - SYNTH_TAP_H / 2.0;
        gdouble px = cx + c * u - sn * w, py = cy + sn * u + c * w;
        /* light blur: average of the point and 4 neighbours */
        gdouble r = 0.5 * sf_sample (f, px, py) +
                    0.125 * (sf_sample (f, px + 1, py) + sf_sample (f, px - 1, py) +
                             sf_sample (f, px, py + 1) + sf_sample (f, px, py - 1));
        r = tanh (2.0 * (gain * r + bias)) + noise * sf_gauss (&s);
        v[y * SYNTH_TAP_W + x] = (gfloat) r;
        mean += r;
      }
  mean /= np;
  for (gint i = 0; i < np; i++)
    var += (v[i] - mean) * (v[i] - mean);
  var = sqrt (var / np) + 1e-9;
  for (gint i = 0; i < np; i++)
    out[i] = (guint8) CLAMP (lround (128.5 + 58.0 * (v[i] - mean) / var), 0, 255);
}

gboolean
synth_write_pgm (const gchar *path, const guint8 *tap)
{
  g_autofree gchar *hdr = g_strdup_printf ("P5\n%d %d\n255\n", SYNTH_TAP_W, SYNTH_TAP_H);
  g_autoptr(GByteArray) b = g_byte_array_new ();

  g_byte_array_append (b, (const guint8 *) hdr, strlen (hdr));
  g_byte_array_append (b, tap, SYNTH_TAP_W * SYNTH_TAP_H);
  return g_file_set_contents (path, (const gchar *) b->data, b->len, NULL);
}
