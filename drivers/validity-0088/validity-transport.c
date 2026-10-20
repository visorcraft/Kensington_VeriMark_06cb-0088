/*
 * validity-transport.c — Kensington VeriMark (06CB:0088) libfprint driver
 *
 * Command channel transport: USB bulk read/write, encrypted app-data
 * exchange and reply status checks (synchronous), and the async
 * exchange / interrupt-wait helpers used by the task SSMs in validity.c.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity.h"

/* ===========================================================================
 * USB transport helpers
 *
 * libfprint provides FpiUsbTransfer (built on gusb / libusb). We use
 * bulk OUT on endpoint 0x01 and bulk IN on endpoint 0x81 for the
 * command channel. EP 0x83 is the interrupt-event channel.
 *
 * For brevity, the helpers below are synchronous wrappers. A real
 * production driver should use the async FpiSsm-based pattern to
 * avoid blocking the main loop. See egismoc / goodixmoc for examples.
 * ========================================================================= */

gboolean
validity_usb_write (FpDevice    *dev,
                    const guint8 *data,
                    gsize         len,
                    GError      **error)
{
  GUsbDevice *udev = fpi_device_get_usb_device (dev);
  gsize transferred = 0;

  if (g_usb_device_get_vid (udev) != 0x06cb ||
      g_usb_device_get_pid (udev) != 0x0088)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "refusing to write to unexpected USB device %04x:%04x",
                   g_usb_device_get_vid (udev),
                   g_usb_device_get_pid (udev));
      return FALSE;
    }

  return g_usb_device_bulk_transfer (
      udev, VALIDITY_EP_CMD_OUT,
      (guint8 *) data, len, &transferred,
      VALIDITY_USB_SEND_TIMEOUT,
      fpi_device_get_cancellable (dev), error);
}

gboolean
validity_usb_read (FpDevice  *dev,
                   guint8    *buf,
                   gsize      max_len,
                   gsize     *out_len,
                   GError   **error)
{
  GUsbDevice *udev = fpi_device_get_usb_device (dev);
  return g_usb_device_bulk_transfer (
      udev, VALIDITY_EP_CMD_IN,
      buf, max_len, out_len,
      VALIDITY_USB_RECV_TIMEOUT,
      fpi_device_get_cancellable (dev), error);
}

gchar *
validity_hex_prefix (const guint8 *data,
                     gsize         len,
                     gsize         max_len)
{
  GString *hex = g_string_sized_new (MIN (len, max_len) * 2 + 4);

  for (gsize i = 0; i < MIN (len, max_len); i++)
    g_string_append_printf (hex, "%02x", data[i]);
  if (len > max_len)
    g_string_append (hex, "...");

  return g_string_free (hex, FALSE);
}

static gboolean
validity_read_app_plaintext (FpDevice  *dev,
                             guint8   **out_plain,
                             gsize     *out_len,
                             GError   **error)
{
  ValiditySession *session = validity_device_get_session (dev);
  guint8 rsp[VALIDITY_MAX_RECV_LEN];
  gsize rsp_len = 0;

  if (!validity_usb_read (dev, rsp, sizeof (rsp), &rsp_len, error))
    return FALSE;

  if (rsp_len < 5 ||
      rsp[0] != VALIDITY_CT_APPLICATION_DATA ||
      rsp[1] != 0x03 || rsp[2] != 0x03)
    {
      g_autofree gchar *hex = validity_hex_prefix (rsp, rsp_len, 32);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "expected TLS app_data record, got %zu B: %s",
                   rsp_len, hex);
      return FALSE;
    }

  guint16 body_len = validity_be16 (rsp + 3);
  if ((gsize) body_len + 5 > rsp_len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "truncated TLS app_data record: body=%u usb=%zu",
                   body_len, rsp_len);
      return FALSE;
    }

  *out_plain = validity_decrypt_record (session,
                                        VALIDITY_CT_APPLICATION_DATA,
                                        rsp + 5, body_len, out_len);
  if (*out_plain == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "failed to decrypt TLS app_data response");
      return FALSE;
    }

  return TRUE;
}

gboolean
validity_exchange_app_plaintext (FpDevice     *dev,
                                 const guint8 *plain,
                                 gsize         plain_len,
                                 guint8      **out_rsp,
                                 gsize        *out_rsp_len,
                                 GError      **error)
{
  ValiditySession *session = validity_device_get_session (dev);
  g_autofree guint8 *record = NULL;
  gsize record_len = 0;

  if (!session->cipher_active)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                           "TLS app_data requested before cipher activation");
      return FALSE;
    }

  record = validity_encrypt_record (session,
                                    VALIDITY_CT_APPLICATION_DATA,
                                    plain, plain_len, &record_len);
  if (record == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "validity_encrypt_record(app_data) failed");
      return FALSE;
    }

  if (!validity_usb_write (dev, record, record_len, error))
    return FALSE;

  return validity_read_app_plaintext (dev, out_rsp, out_rsp_len, error);
}

/* Status word interpretation.
 *
 * Every encrypted device response begins with a 2-byte status word.
 * Observed live across hundreds of successful exchanges:
 *   0x00 0x00 = OK / success
 *
 * Other status codes have not been observed live yet — the device tends to
 * either ACK with 0x00 0x00 or stall the channel entirely. When new non-zero
 * codes are observed in the wild, extend this switch with their semantic
 * mapping (e.g. probe-in-progress, retry, hardware error).
 *
 * Until then, treat any non-zero status as FP_DEVICE_ERROR_PROTO (protocol
 * violation) so libfprint surfaces a more specific error to callers than
 * the previous generic G_IO_ERROR_FAILED.
 */
static gboolean
validity_validate_status (const gchar  *label,
                          const guint8 *rsp,
                          gsize         rsp_len,
                          GError      **error)
{
  if (rsp_len < 2)
    {
      g_propagate_error (
          error,
          fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                    "%s response too short: %zu B",
                                    label, rsp_len));
      return FALSE;
    }

  /* Little-endian: matches flash_exchange_status() and the Windows driver. */
  guint16 status = validity_le16 (rsp);
  if (status == 0x0000)
    return TRUE;

  /* Map known status codes to specific libfprint errors as we learn them.
   * The current driver has not observed any non-zero status code in the
   * wild yet, so this is forward-looking scaffolding.
   */
  FpDeviceError code = FP_DEVICE_ERROR_PROTO;
  const gchar *meaning = "unknown status";
  switch (status)
    {
    /* Placeholder examples — populate from real captures when observed.
     *
     * case 0x0001: code = FP_DEVICE_ERROR_GENERAL; meaning = "device busy"; break;
     * case 0x0002: code = FP_DEVICE_ERROR_NOT_SUPPORTED; meaning = "command not supported"; break;
     */
    default:
      break;
    }

  fp_warn ("%s: unexpected device status 0x%04x (%s) — please report",
           label, status, meaning);
  g_propagate_error (
      error,
      fpi_device_error_new_msg (code,
                                "%s: device returned status 0x%04x (%s)",
                                label, status, meaning));
  return FALSE;
}

gboolean
validity_exchange_and_check_status (FpDevice     *dev,
                                    const gchar  *label,
                                    const guint8 *plain,
                                    gsize         plain_len,
                                    GError      **error)
{
  g_autofree guint8 *rsp = NULL;
  gsize rsp_len = 0;

  fp_dbg ("%s: send %zu B plaintext opcode 0x%02x",
          label, plain_len, plain_len > 0 ? plain[0] : 0xff);
  if (!validity_exchange_app_plaintext (dev, plain, plain_len,
                                        &rsp, &rsp_len, error))
    return FALSE;

  fp_dbg ("%s: got %zu B plaintext response", label, rsp_len);
  return validity_validate_status (label, rsp, rsp_len, error);
}

/* ===========================================================================
 * Async exchange helpers for the task SSMs
 * ========================================================================= */

typedef struct {
  FpiSsm              *ssm;
  ValidityAsyncRecvCb  cb;
  gpointer             cb_data;
  const gchar         *label;
  gboolean             check_status;
} ValidityExchangeCtx;

static void
validity_async_recv_cb (FpiUsbTransfer *transfer,
                        FpDevice       *device,
                        gpointer        userdata,
                        GError         *error)
{
  ValiditySession *session = validity_device_get_session (device);
  g_autofree ValidityExchangeCtx *ctx = userdata;
  g_autofree guint8 *plain = NULL;
  gsize plain_len = 0;
  const guint8 *rsp;
  gsize rsp_len;
  guint16 body_len;

  if (error)
    {
      fpi_ssm_mark_failed (ctx->ssm, error);
      return;
    }

  rsp = transfer->buffer;
  rsp_len = transfer->actual_length;

  if (rsp_len < 5 ||
      rsp[0] != VALIDITY_CT_APPLICATION_DATA ||
      rsp[1] != 0x03 || rsp[2] != 0x03)
    {
      g_autofree gchar *hex = validity_hex_prefix (rsp, rsp_len, 32);
      fpi_ssm_mark_failed (ctx->ssm,
          g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "%s: expected TLS app_data record, got %zu B: %s",
                       ctx->label, rsp_len, hex));
      return;
    }

  body_len = validity_be16 (rsp + 3);
  if ((gsize) body_len + 5 > rsp_len)
    {
      fpi_ssm_mark_failed (ctx->ssm,
          g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "%s: truncated TLS app_data: body=%u usb=%zu",
                       ctx->label, body_len, rsp_len));
      return;
    }

  plain = validity_decrypt_record (session,
                                   VALIDITY_CT_APPLICATION_DATA,
                                   rsp + 5, body_len, &plain_len);
  if (plain == NULL)
    {
      fpi_ssm_mark_failed (ctx->ssm,
          g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "%s: failed to decrypt TLS app_data", ctx->label));
      return;
    }

  fp_dbg ("%s: got %zu B plaintext response", ctx->label, plain_len);

  if (ctx->check_status)
    {
      GError *err = NULL;
      if (!validity_validate_status (ctx->label, plain, plain_len, &err))
        {
          fpi_ssm_mark_failed (ctx->ssm, err);
          return;
        }
    }

  if (ctx->cb != NULL)
    ctx->cb (device, ctx->ssm, plain, plain_len, ctx->cb_data);
  else
    fpi_ssm_next_state (ctx->ssm);
}

static void
validity_async_send_cb (FpiUsbTransfer *transfer,
                        FpDevice       *device,
                        gpointer        userdata,
                        GError         *error)
{
  ValidityExchangeCtx *ctx = userdata;
  FpiUsbTransfer *recv;

  if (error)
    {
      fpi_ssm_mark_failed (ctx->ssm, error);
      g_free (ctx);
      return;
    }

  recv = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (recv, VALIDITY_EP_CMD_IN, VALIDITY_MAX_RECV_LEN);
  fpi_usb_transfer_submit (recv, VALIDITY_USB_RECV_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           validity_async_recv_cb, ctx);
}

/* Submit one encrypted round-trip: encrypt plain, USB OUT, USB IN, decrypt.
 * If cb is NULL: on success, advance ssm to next state (after the optional
 * 2-byte status word check); on error, mark ssm failed.
 * If cb is non-NULL: invoke cb with the decrypted plaintext; cb owns the
 * SSM state transition. */
void
validity_ssm_exchange (FpiSsm              *ssm,
                       FpDevice            *device,
                       const gchar         *label,
                       const guint8        *plain,
                       gsize                plain_len,
                       gboolean             check_status,
                       ValidityAsyncRecvCb  cb,
                       gpointer             cb_data)
{
  ValiditySession *session = validity_device_get_session (device);
  ValidityExchangeCtx *ctx;
  FpiUsbTransfer *send;
  guint8 *record;
  gsize record_len = 0;

  if (!session->cipher_active)
    {
      fpi_ssm_mark_failed (ssm,
          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                               "TLS app_data requested before cipher activation"));
      return;
    }

  record = validity_encrypt_record (session,
                                    VALIDITY_CT_APPLICATION_DATA,
                                    plain, plain_len, &record_len);
  if (record == NULL)
    {
      fpi_ssm_mark_failed (ssm,
          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                               "validity_encrypt_record(app_data) failed"));
      return;
    }

  fp_dbg ("%s: send %zu B plaintext opcode 0x%02x",
          label, plain_len, plain_len > 0 ? plain[0] : 0xff);

  ctx = g_new (ValidityExchangeCtx, 1);
  ctx->ssm          = ssm;
  ctx->cb           = cb;
  ctx->cb_data      = cb_data;
  ctx->label        = label;
  ctx->check_status = check_status;

  send = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (send, VALIDITY_EP_CMD_OUT,
                                   record, record_len, g_free);
  fpi_usb_transfer_submit (send, VALIDITY_USB_SEND_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           validity_async_send_cb, ctx);
}

static void
validity_async_interrupt_cb (FpiUsbTransfer *transfer,
                             FpDevice       *device,
                             gpointer        userdata,
                             GError         *error)
{
  FpiSsm *ssm = userdata;
  const guint8 *event;
  gsize event_len;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  event = transfer->buffer;
  event_len = transfer->actual_length;
  {
    g_autofree gchar *hex = validity_hex_prefix (event, event_len, event_len);
    fp_dbg ("interrupt event len=%zu data=%s", event_len, hex);
  }

  if (event_len >= 1 && event[0] == 0x02)
    fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

  if (event_len >= 3 && event[0] == 0x03 && (event[2] & 0x04))
    {
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);
      fpi_ssm_next_state (ssm);
      return;
    }

  if (event_len >= 1 && event[0] == 0x00)
    fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);

  validity_async_submit_interrupt_read (ssm, device);
}

void
validity_async_submit_interrupt_read (FpiSsm *ssm, FpDevice *device)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN, 64);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_INTERRUPT_TIMEOUT,
                           validity_device_get_interrupt_cancellable (device),
                           validity_async_interrupt_cb, ssm);
}
