/*
 * validity-sift.h - compact SIFT matcher for the VeriMark 06cb:0088 scans.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#define VALIDITY_SIFT_DESC_LEN 128

typedef struct {
  gint    n;      /* number of keypoints */
  gfloat *xy;     /* n x 2 positions (2x upscaled image coordinates) */
  gfloat *desc;   /* n x 128 descriptors */
} ValiditySiftFeatures;

ValiditySiftFeatures *validity_sift_extract (const guint8 *img,
                                             gint          w,
                                             gint          h);
void validity_sift_features_free (ValiditySiftFeatures *f);
gint validity_sift_match (const ValiditySiftFeatures *a,
                          const ValiditySiftFeatures *b);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ValiditySiftFeatures, validity_sift_features_free)

GBytes *validity_sift_serialize (const ValiditySiftFeatures *f);
ValiditySiftFeatures *validity_sift_deserialize (const guint8 *data, gsize len);
