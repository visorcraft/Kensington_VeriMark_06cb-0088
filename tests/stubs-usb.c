/*
 * stubs-usb.c — USB entry points for test-pairing-key, which includes
 * validity-pretls-pair.c but never talks to a device. Calling them is a test
 * bug, so they fail loudly.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>

gboolean validity_usb_write (void *dev, const guint8 *data, gsize len, GError **error);
gboolean validity_usb_read (void *dev, guint8 *buf, gsize max_len, gsize *out_len, GError **error);

gboolean
validity_usb_write (void *dev, const guint8 *data, gsize len, GError **error)
{
  g_error ("validity_usb_write called in a unit test");
}

gboolean
validity_usb_read (void *dev, guint8 *buf, gsize max_len, gsize *out_len, GError **error)
{
  g_error ("validity_usb_read called in a unit test");
}
