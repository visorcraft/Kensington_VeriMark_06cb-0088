/*
 * validity-sift.c - compact SIFT keypoint matcher for the 56 x 144 VeriMark
 * (06cb:0088) scans.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Self-contained implementation of Lowe's SIFT (no OpenCV):
 *   - image upscaled 2x, local contrast normalisation,
 *   - Gaussian/DoG pyramid (3 octaves x 3 intervals), scale-space extrema
 *     with sub-pixel refinement, contrast and edge rejection,
 *   - dominant orientation(s) from a 36-bin gradient histogram,
 *   - 4 x 4 x 8 descriptor, normalised and clipped at 0.2.
 * Only the 250 strongest keypoints of a scan are kept (~33 KB per tap).
 * Matching: nearest neighbour with Lowe's ratio test, then RANSAC over a
 * similarity transform (rotation + translation + small scale); the score is
 * the number of geometrically consistent matches.
 *
 * Written from the published algorithm (no third-party code). The SIFT
 * patent (US 6,711,293) expired in March 2020.
 */

#include <glib.h>
#include <math.h>
#include <string.h>

#include "validity-sift.h"

#define VS_UPSCALE        2
#define VS_INTERVALS      3
#define VS_OCTAVES        3
#define VS_SIGMA          1.6f
#define VS_INIT_SIGMA     0.5f
#define VS_BORDER         6       /* pixels of the upscaled image ignored */
#define VS_EDGE_R         20.0f
#define VS_ORI_BINS       36
#define VS_ORI_PEAK       0.8f
#define VS_DESC_W         4
#define VS_DESC_B         8
#define VS_DESC_LEN       (VS_DESC_W * VS_DESC_W * VS_DESC_B)
#define VS_DESC_CLIP      0.2f
#define VS_LCN_R          8       /* local normalisation radius (upscaled px) */
#define VS_MAX_KP         2000
#define VS_KEEP           250     /* strongest keypoints kept per scan */
#define VS_CONTRAST       0.04f   /* DoG contrast threshold (image in 0..1) */

#define VS_RATIO          0.8f
#define VS_RANSAC_ITERS   600
#define VS_RANSAC_TOL     (2.0f * VS_UPSCALE)
#define VS_MAX_ROT_DEG    25.0f
#define VS_MAX_LOG_SCALE  0.15f

G_STATIC_ASSERT (VS_DESC_LEN == VALIDITY_SIFT_DESC_LEN);

typedef struct {
  gint    w, h;
  gfloat *d;
} VsImg;

static VsImg
vs_img_new (gint w, gint h)
{
  VsImg im = { w, h, g_new0 (gfloat, (gsize) w * h) };
  return im;
}

static inline gfloat
vs_at (const VsImg *im, gint x, gint y)
{
  x = CLAMP (x, 0, im->w - 1);
  y = CLAMP (y, 0, im->h - 1);
  return im->d[y * im->w + x];
}

/* Separable Gaussian blur with edge clamping. */
static void
vs_blur (const VsImg *src, VsImg *dst, gfloat sigma)
{
  gint r = MAX (1, (gint) ceilf (sigma * 3.0f));
  g_autofree gfloat *k = g_new (gfloat, 2 * r + 1);
  g_autofree gfloat *tmp = g_new (gfloat, (gsize) src->w * src->h);
  gfloat sum = 0;

  for (gint i = -r; i <= r; i++)
    sum += k[i + r] = expf (-(gfloat) (i * i) / (2 * sigma * sigma));
  for (gint i = 0; i <= 2 * r; i++)
    k[i] /= sum;

  for (gint y = 0; y < src->h; y++)
    for (gint x = 0; x < src->w; x++)
      {
        gfloat a = 0;
        for (gint i = -r; i <= r; i++)
          a += k[i + r] * vs_at (src, x + i, y);
        tmp[y * src->w + x] = a;
      }
  for (gint y = 0; y < src->h; y++)
    for (gint x = 0; x < src->w; x++)
      {
        gfloat a = 0;
        for (gint i = -r; i <= r; i++)
          a += k[i + r] * tmp[CLAMP (y + i, 0, src->h - 1) * src->w + x];
        dst->d[y * dst->w + x] = a;
      }
}

/* Bilinear 2x upscale followed by local mean/std normalisation, which
 * plays the role of the CLAHE step used in the prototype. Output is
 * roughly in [0, 1]. */
static VsImg
vs_prepare (const guint8 *img, gint w, gint h)
{
  VsImg up = vs_img_new (w * VS_UPSCALE, h * VS_UPSCALE);
  VsImg out = vs_img_new (up.w, up.h);
  g_autofree gdouble *s1 = g_new0 (gdouble, (gsize) (up.w + 1) * (up.h + 1));
  g_autofree gdouble *s2 = g_new0 (gdouble, (gsize) (up.w + 1) * (up.h + 1));

  for (gint y = 0; y < up.h; y++)
    for (gint x = 0; x < up.w; x++)
      {
        gfloat fx = (x + 0.5f) / VS_UPSCALE - 0.5f;
        gfloat fy = (y + 0.5f) / VS_UPSCALE - 0.5f;
        gint x0 = (gint) floorf (fx), y0 = (gint) floorf (fy);
        gfloat ax = fx - x0, ay = fy - y0;
        gint xa = CLAMP (x0, 0, w - 1), xb = CLAMP (x0 + 1, 0, w - 1);
        gint ya = CLAMP (y0, 0, h - 1), yb = CLAMP (y0 + 1, 0, h - 1);
        up.d[y * up.w + x] =
            (1 - ay) * ((1 - ax) * img[ya * w + xa] + ax * img[ya * w + xb]) +
            ay * ((1 - ax) * img[yb * w + xa] + ax * img[yb * w + xb]);
      }

  for (gint y = 0; y < up.h; y++)
    {
      gdouble r1 = 0, r2 = 0;
      for (gint x = 0; x < up.w; x++)
        {
          gdouble v = up.d[y * up.w + x];
          r1 += v;
          r2 += v * v;
          s1[(y + 1) * (up.w + 1) + x + 1] = s1[y * (up.w + 1) + x + 1] + r1;
          s2[(y + 1) * (up.w + 1) + x + 1] = s2[y * (up.w + 1) + x + 1] + r2;
        }
    }

  for (gint y = 0; y < up.h; y++)
    for (gint x = 0; x < up.w; x++)
      {
        gint x0 = MAX (0, x - VS_LCN_R), x1 = MIN (up.w, x + VS_LCN_R + 1);
        gint y0 = MAX (0, y - VS_LCN_R), y1 = MIN (up.h, y + VS_LCN_R + 1);
        gdouble n = (gdouble) (x1 - x0) * (y1 - y0);
#define BOX(s) (s[y1 * (up.w + 1) + x1] - s[y0 * (up.w + 1) + x1] - \
                s[y1 * (up.w + 1) + x0] + s[y0 * (up.w + 1) + x0])
        gdouble m = BOX (s1) / n;
        gdouble var = MAX (BOX (s2) / n - m * m, 0.0);
#undef BOX
        gdouble z = (up.d[y * up.w + x] - m) / (sqrt (var) + 8.0);
        out.d[y * out.w + x] = (gfloat) CLAMP (0.5 + 0.25 * z, 0.0, 1.0);
      }

  g_free (up.d);
  return out;
}

typedef struct {
  gfloat x, y;          /* in upscaled image coordinates */
  gfloat sigma;         /* absolute scale */
  gint   oct, layer;
  gfloat ox, oy;        /* position in octave coordinates */
  gfloat osigma;        /* scale within octave */
} VsKey;

/* Quadratic sub-pixel refinement; returns FALSE if the point is rejected. */
static gboolean
vs_refine (VsImg *dog, gint layers, gint *px, gint *py, gint *pl,
           gfloat contrast_thr, gfloat *ox, gfloat *oy, gfloat *ol)
{
  gint x = *px, y = *py, l = *pl;
  gfloat dx = 0, dy = 0, ds = 0, val = 0;

  for (gint it = 0; it < 5; it++)
    {
      const VsImg *c = &dog[l], *p = &dog[l - 1], *n = &dog[l + 1];
      gfloat v = vs_at (c, x, y);
      gfloat gx = 0.5f * (vs_at (c, x + 1, y) - vs_at (c, x - 1, y));
      gfloat gy = 0.5f * (vs_at (c, x, y + 1) - vs_at (c, x, y - 1));
      gfloat gs = 0.5f * (vs_at (n, x, y) - vs_at (p, x, y));
      gfloat hxx = vs_at (c, x + 1, y) + vs_at (c, x - 1, y) - 2 * v;
      gfloat hyy = vs_at (c, x, y + 1) + vs_at (c, x, y - 1) - 2 * v;
      gfloat hss = vs_at (n, x, y) + vs_at (p, x, y) - 2 * v;
      gfloat hxy = 0.25f * (vs_at (c, x + 1, y + 1) - vs_at (c, x - 1, y + 1) -
                            vs_at (c, x + 1, y - 1) + vs_at (c, x - 1, y - 1));
      gfloat hxs = 0.25f * (vs_at (n, x + 1, y) - vs_at (n, x - 1, y) -
                            vs_at (p, x + 1, y) + vs_at (p, x - 1, y));
      gfloat hys = 0.25f * (vs_at (n, x, y + 1) - vs_at (n, x, y - 1) -
                            vs_at (p, x, y + 1) + vs_at (p, x, y - 1));
      /* Solve H * d = -g with Cramer's rule. */
      gfloat det = hxx * (hyy * hss - hys * hys) - hxy * (hxy * hss - hys * hxs) +
                   hxs * (hxy * hys - hyy * hxs);
      if (fabsf (det) < 1e-12f)
        return FALSE;
      dx = -(gx * (hyy * hss - hys * hys) - hxy * (gy * hss - hys * gs) +
             hxs * (gy * hys - hyy * gs)) / det;
      dy = -(hxx * (gy * hss - hys * gs) - gx * (hxy * hss - hys * hxs) +
             hxs * (hxy * gs - gy * hxs)) / det;
      ds = -(hxx * (hyy * gs - gy * hys) - hxy * (hxy * gs - gy * hxs) +
             gx * (hxy * hys - hyy * hxs)) / det;
      val = v + 0.5f * (gx * dx + gy * dy + gs * ds);

      if (fabsf (dx) < 0.5f && fabsf (dy) < 0.5f && fabsf (ds) < 0.5f)
        {
          /* edge response test on the 2x2 spatial Hessian */
          gfloat tr = hxx + hyy, dt = hxx * hyy - hxy * hxy;
          if (dt <= 0 || tr * tr * VS_EDGE_R >= (VS_EDGE_R + 1) * (VS_EDGE_R + 1) * dt)
            return FALSE;
          if (fabsf (val) * VS_INTERVALS < contrast_thr)
            return FALSE;
          *px = x; *py = y; *pl = l;
          *ox = x + dx; *oy = y + dy; *ol = l + ds;
          return TRUE;
        }
      x += (gint) roundf (dx);
      y += (gint) roundf (dy);
      l += (gint) roundf (ds);
      if (l < 1 || l > layers - 2 || x < 1 || y < 1 ||
          x >= c->w - 1 || y >= c->h - 1)
        return FALSE;
    }
  return FALSE;
}

static void
vs_descriptor (const VsImg *g, gfloat x, gfloat y, gfloat sigma, gfloat ori,
               gfloat *out)
{
  gfloat hist[(VS_DESC_W + 2) * (VS_DESC_W + 2) * (VS_DESC_B + 2)] = { 0 };
  gfloat cw = 3.0f * sigma;                       /* histogram cell width */
  gint radius = (gint) roundf (cw * 1.41421356f * (VS_DESC_W + 1) * 0.5f);
  gfloat co = cosf (ori), si = sinf (ori);
  gfloat norm = 0;

  for (gint j = -radius; j <= radius; j++)
    for (gint i = -radius; i <= radius; i++)
      {
        gint ix = (gint) roundf (x) + i, iy = (gint) roundf (y) + j;
        gfloat rx, ry, rbin, cbin, obin, gx, gy, mag, ang, w;
        gint r0, c0, o0;

        if (ix < 1 || iy < 1 || ix >= g->w - 1 || iy >= g->h - 1)
          continue;
        rx = (co * (ix - x) + si * (iy - y)) / cw;
        ry = (-si * (ix - x) + co * (iy - y)) / cw;
        rbin = ry + VS_DESC_W / 2 - 0.5f;
        cbin = rx + VS_DESC_W / 2 - 0.5f;
        if (rbin <= -1 || rbin >= VS_DESC_W || cbin <= -1 || cbin >= VS_DESC_W)
          continue;
        gx = g->d[iy * g->w + ix + 1] - g->d[iy * g->w + ix - 1];
        gy = g->d[(iy + 1) * g->w + ix] - g->d[(iy - 1) * g->w + ix];
        mag = sqrtf (gx * gx + gy * gy);
        ang = atan2f (gy, gx) - ori;
        while (ang < 0) ang += 2 * G_PI;
        while (ang >= 2 * G_PI) ang -= 2 * G_PI;
        w = expf (-(rx * rx + ry * ry) / (2.0f * (0.5f * VS_DESC_W) * (0.5f * VS_DESC_W)));
        obin = ang * VS_DESC_B / (2 * G_PI);

        r0 = (gint) floorf (rbin); c0 = (gint) floorf (cbin); o0 = (gint) floorf (obin);
        rbin -= r0; cbin -= c0; obin -= o0;
        for (gint dr = 0; dr <= 1; dr++)
          for (gint dc = 0; dc <= 1; dc++)
            for (gint dob = 0; dob <= 1; dob++)
              {
                gfloat v = mag * w * (dr ? rbin : 1 - rbin) *
                           (dc ? cbin : 1 - cbin) * (dob ? obin : 1 - obin);
                gint o = (o0 + dob) % VS_DESC_B;
                hist[((r0 + dr + 1) * (VS_DESC_W + 2) + (c0 + dc + 1)) *
                     (VS_DESC_B + 2) + o] += v;
              }
      }

  for (gint r = 0; r < VS_DESC_W; r++)
    for (gint c = 0; c < VS_DESC_W; c++)
      for (gint o = 0; o < VS_DESC_B; o++)
        out[(r * VS_DESC_W + c) * VS_DESC_B + o] =
            hist[((r + 1) * (VS_DESC_W + 2) + (c + 1)) * (VS_DESC_B + 2) + o];

  for (gint i = 0; i < VS_DESC_LEN; i++)
    norm += out[i] * out[i];
  norm = sqrtf (norm) + 1e-7f;
  for (gint pass = 0; pass < 2; pass++)
    {
      gfloat n2 = 0;
      for (gint i = 0; i < VS_DESC_LEN; i++)
        {
          out[i] /= norm;
          if (pass == 0 && out[i] > VS_DESC_CLIP)
            out[i] = VS_DESC_CLIP;
          n2 += out[i] * out[i];
        }
      norm = sqrtf (n2) + 1e-7f;
    }
}

static gint
vs_cmp_resp (gconstpointer a, gconstpointer b, gpointer user)
{
  const gfloat *r = user;
  gfloat ra = r[*(const gint *) a], rb = r[*(const gint *) b];
  return ra > rb ? -1 : (ra < rb ? 1 : (*(const gint *) a - *(const gint *) b));
}

ValiditySiftFeatures *
validity_sift_extract (const guint8 *img, gint w, gint h)
{
  const gfloat contrast_thr = VS_CONTRAST;
  ValiditySiftFeatures *f = g_new0 (ValiditySiftFeatures, 1);
  GArray *xy = g_array_new (FALSE, FALSE, sizeof (gfloat));
  GArray *desc = g_array_new (FALSE, FALSE, sizeof (gfloat));
  GArray *resp = g_array_new (FALSE, FALSE, sizeof (gfloat));
  const gint layers = VS_INTERVALS + 3;
  VsImg base = vs_prepare (img, w, h);
  VsImg cur;
  gfloat k = powf (2.0f, 1.0f / VS_INTERVALS);
  gfloat sig_diff = sqrtf (MAX (VS_SIGMA * VS_SIGMA -
                                VS_INIT_SIGMA * VS_UPSCALE * VS_INIT_SIGMA * VS_UPSCALE,
                                0.01f));

  cur = vs_img_new (base.w, base.h);
  vs_blur (&base, &cur, sig_diff);
  g_free (base.d);

  for (gint oct = 0; oct < VS_OCTAVES && MIN (cur.w, cur.h) >= 16; oct++)
    {
      VsImg gs[VS_INTERVALS + 3], dog[VS_INTERVALS + 2];
      gfloat scale = (gfloat) (1 << oct);

      gs[0] = cur;
      for (gint l = 1; l < layers; l++)
        {
          gfloat sp = VS_SIGMA * powf (k, l - 1), st = sp * k;
          gs[l] = vs_img_new (cur.w, cur.h);
          vs_blur (&gs[l - 1], &gs[l], sqrtf (st * st - sp * sp));
        }
      for (gint l = 0; l < layers - 1; l++)
        {
          dog[l] = vs_img_new (cur.w, cur.h);
          for (gint i = 0; i < cur.w * cur.h; i++)
            dog[l].d[i] = gs[l + 1].d[i] - gs[l].d[i];
        }

      for (gint l = 1; l <= VS_INTERVALS; l++)
        for (gint y = 1; y < cur.h - 1; y++)
          for (gint x = 1; x < cur.w - 1; x++)
            {
              gfloat v = dog[l].d[y * cur.w + x];
              gboolean is_max = TRUE, is_min = TRUE;
              gint px = x, py = y, pl = l;
              gfloat ox, oy, ol, osig, ax, ay, rv;

              if (fabsf (v) < 0.5f * contrast_thr / VS_INTERVALS)
                continue;
              for (gint dl = -1; dl <= 1 && (is_max || is_min); dl++)
                for (gint dy = -1; dy <= 1; dy++)
                  for (gint dx = -1; dx <= 1; dx++)
                    {
                      gfloat n;
                      if (!dl && !dx && !dy)
                        continue;
                      n = dog[l + dl].d[(y + dy) * cur.w + x + dx];
                      if (n >= v) is_max = FALSE;
                      if (n <= v) is_min = FALSE;
                    }
              if (!is_max && !is_min)
                continue;
              if (!vs_refine (dog, layers - 1, &px, &py, &pl, contrast_thr,
                              &ox, &oy, &ol))
                continue;

              ax = ox * scale;
              ay = oy * scale;
              if (ax < VS_BORDER || ay < VS_BORDER ||
                  ax >= w * VS_UPSCALE - VS_BORDER || ay >= h * VS_UPSCALE - VS_BORDER)
                continue;
              osig = VS_SIGMA * powf (2.0f, ol / VS_INTERVALS);
              rv = fabsf (v);

              /* orientation histogram on the Gaussian image of this layer */
              {
                const VsImg *g = &gs[pl];
                gfloat hist[VS_ORI_BINS] = { 0 }, sm[VS_ORI_BINS], mx = 0;
                gfloat ws = 1.5f * osig;
                gint rad = (gint) roundf (3.0f * ws);

                for (gint j = -rad; j <= rad; j++)
                  for (gint i = -rad; i <= rad; i++)
                    {
                      gint ix = px + i, iy = py + j;
                      gfloat gx, gy;
                      gint b;
                      if (ix < 1 || iy < 1 || ix >= g->w - 1 || iy >= g->h - 1)
                        continue;
                      gx = g->d[iy * g->w + ix + 1] - g->d[iy * g->w + ix - 1];
                      gy = g->d[(iy + 1) * g->w + ix] - g->d[(iy - 1) * g->w + ix];
                      b = (gint) roundf (VS_ORI_BINS * (atan2f (gy, gx) + G_PI) / (2 * G_PI));
                      hist[((b % VS_ORI_BINS) + VS_ORI_BINS) % VS_ORI_BINS] +=
                          expf (-(i * i + j * j) / (2 * ws * ws)) * sqrtf (gx * gx + gy * gy);
                    }
                for (gint b = 0; b < VS_ORI_BINS; b++)
                  {
                    gint p2 = (b + VS_ORI_BINS - 2) % VS_ORI_BINS, p1 = (b + VS_ORI_BINS - 1) % VS_ORI_BINS;
                    gint n1 = (b + 1) % VS_ORI_BINS, n2 = (b + 2) % VS_ORI_BINS;
                    sm[b] = (hist[p2] + hist[n2]) / 16.0f + (hist[p1] + hist[n1]) * 4.0f / 16.0f +
                            hist[b] * 6.0f / 16.0f;
                    mx = MAX (mx, sm[b]);
                  }
                for (gint b = 0; b < VS_ORI_BINS && xy->len / 2 < VS_MAX_KP; b++)
                  {
                    gint l1 = (b + VS_ORI_BINS - 1) % VS_ORI_BINS, r1 = (b + 1) % VS_ORI_BINS;
                    gfloat bin, ori, d[VS_DESC_LEN], pt[2];
                    if (sm[b] <= sm[l1] || sm[b] <= sm[r1] || sm[b] < VS_ORI_PEAK * mx)
                      continue;
                    bin = b + 0.5f * (sm[l1] - sm[r1]) / (sm[l1] - 2 * sm[b] + sm[r1]);
                    ori = bin * 2 * G_PI / VS_ORI_BINS - G_PI;
                    vs_descriptor (g, ox, oy, osig, ori, d);
                    pt[0] = ax;
                    pt[1] = ay;
                    g_array_append_vals (xy, pt, 2);
                    g_array_append_vals (desc, d, VS_DESC_LEN);
                    g_array_append_val (resp, rv);
                  }
              }
            }

      /* next octave starts from the layer with twice the base sigma */
      {
        VsImg nx = vs_img_new (cur.w / 2, cur.h / 2);
        for (gint y = 0; y < nx.h; y++)
          for (gint x = 0; x < nx.w; x++)
            nx.d[y * nx.w + x] = gs[VS_INTERVALS].d[(2 * y) * cur.w + 2 * x];
        for (gint l = 0; l < layers; l++)
          g_free (gs[l].d);
        for (gint l = 0; l < layers - 1; l++)
          g_free (dog[l].d);
        cur = nx;
      }
    }
  g_free (cur.d);

  /* keep the strongest keypoints only (bounded template size) */
  {
    gint n = xy->len / 2, keep = MIN (n, VS_KEEP);
    g_autofree gint *order = g_new (gint, n);
    const gfloat *r = (const gfloat *) resp->data;

    for (gint i = 0; i < n; i++)
      order[i] = i;
    g_qsort_with_data (order, n, sizeof (gint), vs_cmp_resp, (gpointer) r);
    f->n = keep;
    f->xy = g_new (gfloat, 2 * keep);
    f->desc = g_new (gfloat, (gsize) keep * VS_DESC_LEN);
    for (gint i = 0; i < keep; i++)
      {
        gint o = order[i];
        f->xy[2 * i] = g_array_index (xy, gfloat, 2 * o);
        f->xy[2 * i + 1] = g_array_index (xy, gfloat, 2 * o + 1);
        memcpy (f->desc + (gsize) i * VS_DESC_LEN,
                (const gfloat *) desc->data + (gsize) o * VS_DESC_LEN,
                VS_DESC_LEN * sizeof (gfloat));
      }
  }
  g_array_free (xy, TRUE);
  g_array_free (desc, TRUE);
  g_array_free (resp, TRUE);
  return f;
}

void
validity_sift_features_free (ValiditySiftFeatures *f)
{
  if (f == NULL)
    return;
  g_free (f->xy);
  g_free (f->desc);
  g_free (f);
}

static guint32
vs_rand (guint32 *s)
{
  *s = *s * 1664525u + 1013904223u;
  return *s >> 8;
}

gint
validity_sift_match (const ValiditySiftFeatures *a,
                     const ValiditySiftFeatures *b)
{
  g_autofree gint *mi = NULL;
  gint nm = 0, best = 0;
  guint32 seed = 0x5eed1234u;

  if (a->n < 3 || b->n < 3)
    return 0;

  /* nearest neighbour + ratio test (squared distances) */
  mi = g_new (gint, a->n * 2);
  for (gint i = 0; i < a->n; i++)
    {
      const gfloat *da = a->desc + (gsize) i * VS_DESC_LEN;
      gfloat d1 = G_MAXFLOAT, d2 = G_MAXFLOAT;
      gint j1 = -1;
      for (gint j = 0; j < b->n; j++)
        {
          const gfloat *db = b->desc + (gsize) j * VS_DESC_LEN;
          gfloat d = 0;
          for (gint t = 0; t < VS_DESC_LEN && d < d2; t++)
            {
              gfloat e = da[t] - db[t];
              d += e * e;
            }
          if (d < d1) { d2 = d1; d1 = d; j1 = j; }
          else if (d < d2) d2 = d;
        }
      if (j1 >= 0 && d1 < VS_RATIO * VS_RATIO * d2)
        {
          mi[2 * nm] = i;
          mi[2 * nm + 1] = j1;
          nm++;
        }
    }
  if (nm < 3)
    return nm;

  /* RANSAC on a similarity transform from 2-point samples */
  for (gint it = 0; it < VS_RANSAC_ITERS; it++)
    {
      gint p = vs_rand (&seed) % nm, q = vs_rand (&seed) % nm, cnt = 0;
      const gfloat *a1, *a2, *b1, *b2;
      gfloat ux, uy, vx, vy, den, c, s, tx, ty, sc, ang;

      if (p == q)
        continue;
      a1 = a->xy + 2 * mi[2 * p];     b1 = b->xy + 2 * mi[2 * p + 1];
      a2 = a->xy + 2 * mi[2 * q];     b2 = b->xy + 2 * mi[2 * q + 1];
      ux = a2[0] - a1[0]; uy = a2[1] - a1[1];
      vx = b2[0] - b1[0]; vy = b2[1] - b1[1];
      den = ux * ux + uy * uy;
      if (den < 16.0f)
        continue;
      c = (ux * vx + uy * vy) / den;            /* s*cos */
      s = (ux * vy - uy * vx) / den;            /* s*sin */
      sc = sqrtf (c * c + s * s);
      ang = atan2f (s, c) * 180.0f / G_PI;
      if (fabsf (logf (sc)) > VS_MAX_LOG_SCALE || fabsf (ang) > VS_MAX_ROT_DEG)
        continue;
      tx = b1[0] - (c * a1[0] - s * a1[1]);
      ty = b1[1] - (s * a1[0] + c * a1[1]);
      for (gint m = 0; m < nm; m++)
        {
          const gfloat *pa = a->xy + 2 * mi[2 * m], *pb = b->xy + 2 * mi[2 * m + 1];
          gfloat ex = c * pa[0] - s * pa[1] + tx - pb[0];
          gfloat ey = s * pa[0] + c * pa[1] + ty - pb[1];
          if (ex * ex + ey * ey <= VS_RANSAC_TOL * VS_RANSAC_TOL)
            cnt++;
        }
      best = MAX (best, cnt);
    }
  return best;
}

/* Storage format (one template):
 *   u32 LE magic "VSF1", u32 LE n,
 *   n x (u16 LE x*16, u16 LE y*16),
 *   n x 128 B descriptor, quantised as min(255, v * 512).
 * No image is stored, only keypoint positions and gradient histograms. */
#define VS_MAGIC 0x31465356u   /* "VSF1" */

GBytes *
validity_sift_serialize (const ValiditySiftFeatures *f)
{
  gsize len = 8 + (gsize) f->n * (4 + VS_DESC_LEN);
  guint8 *buf = g_malloc (len), *p = buf;

  p[0] = VS_MAGIC & 0xff; p[1] = (VS_MAGIC >> 8) & 0xff;
  p[2] = (VS_MAGIC >> 16) & 0xff; p[3] = VS_MAGIC >> 24;
  p[4] = f->n & 0xff; p[5] = (f->n >> 8) & 0xff; p[6] = (f->n >> 16) & 0xff; p[7] = 0;
  p += 8;
  for (gint i = 0; i < f->n; i++)
    for (gint c = 0; c < 2; c++)
      {
        guint v = (guint) CLAMP (lroundf (f->xy[2 * i + c] * 16.0f), 0, 65535);
        *p++ = v & 0xff;
        *p++ = v >> 8;
      }
  for (gint i = 0; i < f->n * VS_DESC_LEN; i++)
    *p++ = (guint8) MIN (255, lroundf (f->desc[i] * 512.0f));
  return g_bytes_new_take (buf, len);
}

ValiditySiftFeatures *
validity_sift_deserialize (const guint8 *data, gsize len)
{
  ValiditySiftFeatures *f;
  guint32 magic, n;
  const guint8 *p;

  if (len < 8)
    return NULL;
  magic = data[0] | (data[1] << 8) | (data[2] << 16) | ((guint32) data[3] << 24);
  n = data[4] | (data[5] << 8) | (data[6] << 16) | ((guint32) data[7] << 24);
  if (magic != VS_MAGIC || n > VS_MAX_KP * 4 ||
      len != 8 + (gsize) n * (4 + VS_DESC_LEN))
    return NULL;

  f = g_new0 (ValiditySiftFeatures, 1);
  f->n = (gint) n;
  f->xy = g_new (gfloat, 2 * n);
  f->desc = g_new (gfloat, (gsize) n * VS_DESC_LEN);
  p = data + 8;
  for (guint i = 0; i < 2 * n; i++, p += 2)
    f->xy[i] = (p[0] | (p[1] << 8)) / 16.0f;
  for (gsize i = 0; i < (gsize) n * VS_DESC_LEN; i++)
    f->desc[i] = *p++ / 512.0f;
  return f;
}
