/*
 * validity-match.h — matcher glue between validity.c and validity-sift.c.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "validity-sift.h"

/* fpi-data format version of prints holding SIFT keypoints: (u aay). */
#define VALIDITY_PRINT_FORMAT_SIFT 1u

/* Match threshold in consistent keypoints (VALIDITY0088_SIFT_THRESHOLD). */
gint validity_get_match_threshold (void);

/* Best score of @probe over all templates of one print's fpi-data @data
 * (any format; encrypted prints are decrypted with @device_id, the print's
 * device id), or -1 with @error set if nothing in it is usable.
 * @best_idx gets the index of the best template. */
gint validity_tpl_best_score (const ValiditySiftFeatures *probe,
                              GVariant                   *data,
                              const gchar                *device_id,
                              gint                       *best_idx,
                              GError                    **error);

/* With VALIDITY0088_DUMP_SCANS=<dir>, save the 56 x 144 scan @tpl as
 * <dir>/<time>-<action>-f<finger>.pgm (mode 600) for offline analysis.
 * Biometric data: never commit these files. No-op when the variable is
 * unset. */
void validity_dump_scan (const guint8 *tpl,
                         const gchar  *action,
                         gint          finger);

/* Taps of an existing print (any format) as serialised keypoints (GBytes),
 * for extending it in an enrollment; NULL with @error set if unreadable. */
GPtrArray *validity_tpl_load (GVariant    *data,
                              const gchar *device_id,
                              GError     **error);

/* FALSE for a 56 x 144 scan with (almost) no ridge contrast. */
gboolean validity_tpl_quality_ok (const guint8 *raw);

/* NULL if the 0x51 reply holds an image, else a FP_DEVICE_RETRY error. */
GError *validity_no_image_error (const guint8 *plain,
                                 gsize         plain_len);

/* Copy the image of a 0x51 reply into @tpl (VALIDITY_IMAGE_DATA_LEN B,
 * row-major 56 x 144). */
gboolean validity_store_template (guint8       *tpl,
                                  const guint8 *plain,
                                  GError      **error);

/* fpi-data for an enrolled print of @device_id from the serialised
 * features of its taps (GBytes); encrypted unless disabled, unencrypted
 * (with a warning) when no print key exists or can be created. Returns a
 * floating reference, or NULL with @error set. */
GVariant *validity_tpl_print_data (GPtrArray   *templates,
                                   const gchar *device_id,
                                   GError     **error);
