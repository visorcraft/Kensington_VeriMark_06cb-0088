/*
 * validity.c — Kensington VeriMark (06CB:0088) libfprint driver
 *
 * Top-level driver: GObject class, device lifecycle, handshake
 * state machine. Delegates crypto to validity-crypto.c and command
 * building to validity-commands.c.
 *
 * Status: SCAFFOLD with the protocol primitives wired up. Functions
 * marked TODO need device-side iteration to complete. The handshake
 * flow is the highest-confidence part because every piece is
 * cross-referenced with , , 
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity.h"
#include "validity-match.h"

/* ===========================================================================
 * Driver class
 * ========================================================================= */

struct _FpiDeviceValidity0088
{
  FpDevice           parent;
  ValiditySession    session;
  FpiSsm            *task_ssm;
  GCancellable      *interrupt_cancellable;
  ValidityPhase      phase;

  /* Per-task state for the async enroll/verify SSMs. Reset at the
   * start of each task; freed by the SSM done callback. */
  FpPrint           *stage_print;
  guint8             captured_tpl[VALIDITY_IMAGE_DATA_LEN];
  GPtrArray         *enroll_templates;   /* GBytes, one per accepted tap */
  gdouble            verify_score;
  FpiMatchResult     verify_result;
  FpPrint           *identify_match;     /* borrowed from the identify gallery */
  gint               enroll_completed;
  gint               enroll_attempts;
  gboolean           enroll_needs_scan_setup;
  ValidityCalibration *cal_work;         /* non-NULL while calibrating */
};

G_DEFINE_TYPE (FpiDeviceValidity0088, fpi_device_validity_0088, FP_TYPE_DEVICE);

ValiditySession *
validity_device_get_session (FpDevice *dev)
{
  return &FPI_DEVICE_VALIDITY_0088 (dev)->session;
}

GCancellable *
validity_device_get_interrupt_cancellable (FpDevice *dev)
{
  return FPI_DEVICE_VALIDITY_0088 (dev)->interrupt_cancellable;
}

static const FpIdEntry validity_id_table[] = {
  { .vid = 0x06CB, .pid = 0x0088, .driver_data = 0 },  /* Kensington VeriMark */
  /* Siblings (uncomment when validated):
   * { .vid = 0x06CB, .pid = 0x0081, .driver_data = 0 },
   * { .vid = 0x06CB, .pid = 0x009A, .driver_data = 0 },
   */
  { .vid = 0, .pid = 0, .driver_data = 0 }
};

/* ---------------------------------------------------------------------------
 * LED helpers
 *
 * Send a custom LED glow command (opcode 0x39).  Used to indicate match
 * results: red flash on NO_MATCH, optional green pulse on MATCH.
 *
 * Env-var override `VALIDITY0088_LED_RAW_PAYLOAD` accepts a 250-char hex
 * string for the 125-byte raw payload, bypassing the structured builder.
 * Useful for live iteration on color/timing without rebuild.
 * ------------------------------------------------------------------------- */
/* Send raw 125-byte LED payload (bypasses structured builder). */
static gboolean
validity_send_led_raw (FpDevice     *dev,
                       const gchar  *label,
                       const guint8 *payload,
                       gsize         payload_len,
                       GError      **error)
{
  guint8 *cmd = g_memdup2 (payload, payload_len);
  fp_dbg ("LED %s (raw): %zu bytes, mode=0x%02x%02x%02x%02x",
          label, payload_len, payload[4], payload[3], payload[2], payload[1]);
  gboolean ok = validity_exchange_and_check_status (dev, label, cmd,
                                                    payload_len, error);
  g_free (cmd);
  return ok;
}

/* Known-good LED payloads. The COLOR is encoded in the mode u32 at
 * bytes 1..4, NOT in slot color bytes.
 *
 * Each variant requires a preceding 0x51 status query to transition
 * the device into the "between verify attempts" state where the LED
 * color is honored. Outside that state, color commands are silently
 * ignored (the LED stays off or whatever it was).
 */

/* Variant 2 — RED FAIL FLASH (mode 0x000002ee). */
static const guint8 LED_VARIANT_RED[125] = {
  0x39, 0xee, 0x02, 0x00, 0x00, 0x4b, 0x00, 0x00,
  0x00, 0x01, 0xbf, 0x00, 0x20, 0xbf, 0xbf, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x4b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x4b, 0x00, 0x00, 0x00, 0x01, 0xbf, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x20, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Variant 3 — SUCCESS pulse (mode 0x000001f4). */
static const guint8 LED_VARIANT_SUCCESS[125] = {
  0x39, 0xf4, 0x01, 0x00, 0x00, 0xf4, 0x01, 0x00,
  0x00, 0x01, 0xbf, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xbf, 0xbf, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x01, 0x00,
  0x00, 0x00, 0xbf, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Send a state-context-aware LED command. The 0x51 status query
 * transitions the device into the color-honoring state, then the LED
 * payload is sent and renders correctly.
 */
static gboolean
validity_send_led_with_status (FpDevice     *dev,
                               const gchar  *label,
                               const guint8 *led_payload,
                               gsize         led_payload_len,
                               GError      **error)
{
  static const guint8 status_query[] = { 0x51, 0x00, 0x20, 0x00, 0x00 };
  g_autofree guint8 *rsp = NULL;
  gsize rsp_len = 0;

  fp_dbg ("LED %s: 0x51 status query (transition to color-honor state)",
          label);
  if (!validity_exchange_app_plaintext (dev, status_query,
                                        sizeof (status_query),
                                        &rsp, &rsp_len, error))
    return FALSE;
  fp_dbg ("LED %s: 0x51 returned %zu B (discarded)", label, rsp_len);

  return validity_send_led_raw (dev, label, led_payload, led_payload_len,
                                error);
}

/* ===========================================================================
 * HostPart read/write send helpers (P5 — PAIRING.md)
 *
 * Wire 0x3e — write hostPart to device (1-byte opcode, no payload).
 * Wire 0x40 — read hostPart from device (13-byte opcode + 12-B params).
 *
 * Decompiled references:
 *   FUN_18004e490 (wrapper) + FUN_18007f7a0 (framer) for 0x3e
 *   FUN_18004e8a0 (wrapper) + FUN_18007f9a0 (framer) for 0x40
 * ========================================================================= */

/**
 * validity_send_host_part_write:
 *
 * Sends wire 0x3e (1 B) over the encrypted session. This tells the device
 * that the host identity is ready. Equivalent to FUN_18004e490.
 * Per FUN_18007f7a0: allocates [0x3e] (1 B), dispatches via 0x88 tag.
 **/
gboolean
validity_send_host_part_write (FpDevice *dev, GError **error)
{
  guint8 cmd[1] = { 0x3e };
  return validity_exchange_and_check_status (dev, "hostpart-write-0x3e",
                                             cmd, 1, error);
}

/**
 * validity_send_host_part_read:
 * @tag1: first tag byte (wrapper param_2 / descriptor +0x20)
 * @tag2: second tag byte (wrapper param_3 / descriptor +0x21)
 * @val1: u32 LE value 1 (descriptor +0x24)
 * @val2: u32 LE value 2 (descriptor +0x28)
 * @out_data: (out) (transfer full) decrypted response plaintext
 * @out_data_len: (out) length of response
 *
 * Sends wire 0x40 (FUN_18007f9a0 framer) to read hostPart data.
 * Wire format: [0x40] [tag1] [tag2] [0,0] [val1 LE] [val2 LE] (13 B total).
 **/
gboolean
validity_send_host_part_read (FpDevice  *dev,
                               guint8     tag1,
                               guint8     tag2,
                               guint32    val1,
                               guint32    val2,
                               guint8   **out_data,
                               gsize     *out_data_len,
                               GError   **error)
{
  guint8 cmd[13];
  memset (cmd, 0, sizeof (cmd));
  cmd[0] = 0x40;
  cmd[1] = tag1;
  cmd[2] = tag2;
  validity_put_le32 (cmd + 5, val1);
  validity_put_le32 (cmd + 9, val2);

  return validity_exchange_app_plaintext (dev, cmd, 13,
                                          out_data, out_data_len,
                                          error);
}

/* ===========================================================================
 * Pairing: cert blob acquisition (opcode 0x4f / internal 0x10)
 *
 * Sends a minimal 0x4f request (fresh pair: just the opcode byte, no TLV
 * payload, per FUN_180065d90 state 3 calling FUN_180050a30(ctx, 0, 0, 0)).
 * Parses the TLV response to extract tag 0x0001 (0xb8-byte cert blob).
 * ========================================================================= */

/**
 * validity_find_tlv:
 * @data: TLV-encoded buffer
 * @data_len: length of @data
 * @target_tag: the TLV tag to find (LE ushort)
 * @out_val: (out) pointer to value inside @data (not allocated)
 * @out_val_len: (out) length of value
 *
 * TLV format: [tag LE u16][length LE u16][value N B]
 * Returns TRUE if found, FALSE if absent or malformed.
 **/
static gboolean
validity_find_tlv (const guint8 *data,
                    gsize         data_len,
                    guint16       target_tag,
                    const guint8 **out_val,
                    gsize        *out_val_len)
{
  gsize off = 0;
  while (off + 4 <= data_len)
    {
      guint16 tag    = validity_le16 (data + off);
      guint16 length = validity_le16 (data + off + 2);

      if (off + 4 + (gsize) length > data_len)
        return FALSE;

      if (tag == target_tag)
        {
          *out_val = data + off + 4;
          *out_val_len = (gsize) length;
          return TRUE;
        }

      off += 4 + (gsize) length;
    }

  return FALSE;
}

/**
 * validity_send_cert_acquisition:
 * @dev: fingerprint device
 * @out_cert_blob: (out) 0xb8-byte buffer for the certificate blob
 * @error: return location for a GError, or %NULL
 *
 * Sends wire 0x4f to acquire the 0xb8-byte cert blob from the device.
 * Must be called inside an active encrypted TLS session (after handshake).
 *
 * Response is a TLV envelope after the 2-byte status word. Tag 0x0001
 * carries the cert body. Save it to disk for TLS handshake replay.
 **/
gboolean
validity_send_cert_acquisition (FpDevice  *dev,
                                 guint8    out_cert_blob[0xb8],
                                 guint16  *out_status,
                                 GError  **error)
{
  /* Virgin-path 0x4f payload is a 5-byte zero-padded buffer
   * `4f 00 00 00 00`, not a bare 1-byte opcode. A 1-byte send
   * returns 0x0401 because the device's command framer expects
   * the full 5-byte form. */
  guint8 request[5] = { 0x4f, 0x00, 0x00, 0x00, 0x00 };
  g_autofree guint8 *response = NULL;
  gsize response_len = 0;

  if (out_status != NULL)
    *out_status = 0xFFFFu;
  fp_dbg ("sending 0x4f cert acquisition request (5-byte virgin-path payload)");

  if (!validity_exchange_app_plaintext (dev, request, sizeof (request),
                                        &response, &response_len,
                                        error))
    return FALSE;

  if (response_len < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "0x4f response too short: %zu B", response_len);
      return FALSE;
    }

  guint16 status = validity_le16 (response);
  if (out_status != NULL)
    *out_status = status;
  if (status != 0x0000)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "0x4f returned status 0x%04x", status);
      return FALSE;
    }

  const guint8 *tlv_start = response + 2;
  gsize tlv_len = response_len - 2;
  const guint8 *val = NULL;
  gsize val_len = 0;

  if (!validity_find_tlv (tlv_start, tlv_len, 0x0001, &val, &val_len))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "0x4f: TLV tag 0x0001 not found in %zu B", tlv_len);
      return FALSE;
    }

  if (val_len != 0xb8)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "0x4f: tag 1 expected 0xb8 B, got %zu", val_len);
      return FALSE;
    }

  memcpy (out_cert_blob, val, 0xb8);
  fp_dbg ("0x4f cert acquisition OK: 0xb8-byte blob received");
  return TRUE;
}

/* ===========================================================================
 * Plaintext init phase
 *
 * Order on the wire:
 *   1. INIT_MSG4   (= wire 0x06 + 660 B static payload)
 *   2. opcode 0x5b (1 byte; not in dispatch table — provenance TBD)
 *   3. INIT_MSG1   (= wire 0x01, 1 byte)
 *   4. INIT_MSG2   (= wire 0x19, 1 byte)
 *
 * INIT_MSG4's 660-byte payload is the cached host-device identity blob.
 * Persists to the per-device pairing storage directory.
 * ========================================================================= */

/* Send one wire frame: write the framed bytes then read the device's
 * response into rsp_buf. Per the response on EP 0x81 begins
 * with a 2-byte status (`00 00` on success). Caller may ignore body.
 */
static gboolean
validity_send_wire (FpDevice    *dev,
                    const guint8 *wire,
                    gsize         wire_len,
                    guint8       *rsp_buf,
                    gsize         rsp_buf_size,
                    gsize        *rsp_len,
                    GError      **error)
{
  if (!validity_usb_write (dev, wire, wire_len, error)) return FALSE;
  return validity_usb_read (dev, rsp_buf, rsp_buf_size, rsp_len, error);
}

static gboolean
validity_send_opcode_only (FpDevice *dev,
                           guint8    opcode,
                           guint8   *rsp_buf,
                           gsize     rsp_buf_size,
                           gsize    *rsp_len,
                           GError  **error)
{
  return validity_send_wire (dev, &opcode, 1,
                             rsp_buf, rsp_buf_size, rsp_len, error);
}

/* DEBUG-ONLY hard-reset attempt. Two-step DoUnpairing sequence:
 *
 *   step 1: bulk OUT `01`         -> bulk IN 0x26 bytes (VFM state query)
 *   step 2: bulk OUT `05 02 00`   -> bulk IN 2 bytes    (DeviceReset)
 *
 * NOT a single isolated `05 02 00` and NOT a loop of N `05 02 00`s -
 * the device's state machine requires the pair in this exact order.
 *
 * The state byte to watch in step-1 response is `response[0x23] & 0x0f`.
 * On a normally paired 06cb:0088 the byte should be `5` or `7` (which
 * map to the direct success path). Other values indicate a cleanup-path
 * detour would apply.
 *
 * Whether this is sufficient to actually erase pairing ownership is
 * unclear - the real ownership erase may be elsewhere. */
static gboolean
validity_try_hard_reset (FpDevice *dev, GError **error)
{
  static const guint8 vfm_state_query[1] = { 0x01 };
  static const guint8 reset_cmd[3]       = { 0x05, 0x02, 0x00 };
  guint8 rsp[VALIDITY_MAX_RECV_LEN];
  gsize  rsp_len = 0;

  fp_info ("VALIDITY0088_TRY_HARD_RESET=1: starting 2-step "
           "DoUnpairing sequence");

  /* Step 1: VFM state query (`01` -> 0x26 bytes). */
  fp_info ("step 1/2: bulk OUT [01] (VFM state query)");
  if (!validity_send_wire (dev, vfm_state_query, sizeof (vfm_state_query),
                           rsp, sizeof (rsp), &rsp_len, error))
    {
      fp_warn ("step 1 wire send failed: %s",
               error && *error ? (*error)->message : "?");
      return FALSE;
    }
  {
    g_autoptr(GString) hex = g_string_new (NULL);
    for (gsize j = 0; j < rsp_len; j++)
      g_string_append_printf (hex, "%02x ", rsp[j]);
    fp_info ("step 1/2 response (%zu B): %s", rsp_len,
             hex->str ? hex->str : "(empty)");
    if (rsp_len > 0x23)
      {
        guint8 raw_state = rsp[0x23] & 0x0f;
        fp_info ("  VFM low-level state @resp[0x23] & 0x0f = 0x%02x "
                 "(paired direct-path expected: 5 or 7; cleanup-path: "
                 "3, 8, or session 0x0f)", raw_state);
      }
  }

  g_usleep (50 * 1000); /* 50 ms; let the device settle */

  /* Step 2: DeviceReset (`05 02 00` -> 2 bytes). */
  fp_info ("step 2/2: bulk OUT [05 02 00] (DeviceReset)");
  if (!validity_send_wire (dev, reset_cmd, sizeof (reset_cmd),
                           rsp, sizeof (rsp), &rsp_len, error))
    {
      fp_warn ("step 2 wire send failed: %s",
               error && *error ? (*error)->message : "?");
      return FALSE;
    }
  {
    g_autoptr(GString) hex = g_string_new (NULL);
    for (gsize j = 0; j < rsp_len; j++)
      g_string_append_printf (hex, "%02x ", rsp[j]);
    fp_info ("step 2/2 response (%zu B): %s", rsp_len,
             hex->str ? hex->str : "(empty)");
  }

  /* Force caller to bail. The user should unplug + replug + unset
   * VALIDITY0088_TRY_HARD_RESET before opening the device again. */
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "VALIDITY0088_TRY_HARD_RESET sequence complete; "
                       "unplug + replug + unset the env var to continue");
  return FALSE;
}

static gboolean
validity_send_plaintext_init (FpDevice *dev,
                              gboolean  is_enroll_session,
                              GError  **error)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (dev);
  guint8 rsp[VALIDITY_MAX_RECV_LEN];
  gsize  rsp_len = 0;

  /* When the upstream operation is enrollment, capture 03 shows opcode
   * 0x1a is sent FIRST (before INIT_MSG4). See §summary. */
  if (is_enroll_session)
    {
      if (!validity_send_opcode_only (dev, VALIDITY_OP_ENROLL_SESSION,
                                      rsp, sizeof (rsp), &rsp_len, error))
        return FALSE;
    }

  /* 1. INIT_MSG4: wire byte 0x06 + 660 B static payload */
  gsize wire_len;
  guint8 *wire = validity_wrap_wire_frame (
      VALIDITY_OP_INIT_MSG4,
      validity_init_msg4_payload, VALIDITY_INIT_MSG4_PAYLOAD_LEN,
      &wire_len);
  gboolean ok = validity_send_wire (dev, wire, wire_len,
                                    rsp, sizeof (rsp), &rsp_len, error);
  g_free (wire);
  if (!ok) return FALSE;

  /* 2. Opcode 0x5b — not in static dispatch table, emit blindly per
   * §0x5b (python-validity does the same for the sibling). */
  if (!validity_send_opcode_only (dev, VALIDITY_OP_UNKNOWN_5B,
                                  rsp, sizeof (rsp), &rsp_len, error))
    return FALSE;

  /* 3. INIT_MSG1 (RomInfo.get). */
  if (!validity_send_opcode_only (dev, VALIDITY_OP_INIT_MSG1,
                                  rsp, sizeof (rsp), &rsp_len, error))
    return FALSE;
  if (!validity_parse_rom_info_response (&self->session.rom_info,
                                         rsp, rsp_len, error))
    return FALSE;
  self->session.device_prefix[0] = self->session.rom_info.major;
  self->session.device_prefix[1] = self->session.rom_info.minor;
  self->session.device_prefix[2] = self->session.rom_info.u1;

  /* 4. INIT_MSG2 */
  if (!validity_send_opcode_only (dev, VALIDITY_OP_INIT_MSG2,
                                  rsp, sizeof (rsp), &rsp_len, error))
    return FALSE;

  /* 5. get_fw_info(0x02) - probe the fwext partition. python-validity
   * usb.py:89 sends this between INIT_MSG2 and INIT_MSG4 to decide
   * whether to follow up with init_hardcoded_clean_slate. Purely
   * diagnostic here: working, paired 0088 units answer 0x04af on every
   * open (0x4af is in the Windows driver's status table) and everything
   * after it works, so only other non-zero values are worth a warning. */
  guint8 fw_probe[2] = { VALIDITY_OP_GET_FW_INFO, 0x02 };
  if (validity_send_wire (dev, fw_probe, sizeof (fw_probe),
                          rsp, sizeof (rsp), &rsp_len, NULL))
    {
      if (rsp_len >= 2)
        {
          guint16 fw_status = validity_le16 (rsp);
          if (fw_status == 0 || fw_status == VALIDITY_STATUS_FWEXT_NOT_LOADED)
            fp_dbg ("get_fw_info(fwext) status 0x%04x", fw_status);
          else
            fp_warn ("validity-0088: get_fw_info(fwext) returned unexpected "
                     "status 0x%04x; if pairing or the TLS handshake fails "
                     "next, this is the likely cause", fw_status);
        }
    }
  /* Probe failure is non-fatal: the driver works without it on
   * already-paired devices, this is purely a diagnostic enhancement. */

  return TRUE;
}

/* ===========================================================================
 * TLS handshake phase
 *
 * Outline:
 *   C → ClientHello                                    (uses 44 00 00 00 prefix)
 *   S → ServerHello                                    (selects cipher 0xc005)
 *   S → Certificate (custom TLV body)
 *   S → ServerHelloDone
 *   C → ClientKeyExchange (ephemeral pub key)
 *   C → ChangeCipherSpec
 *   C → Finished                                       (encrypted)
 *   S → ChangeCipherSpec
 *   S → Finished
 *
 * After this, session.cipher_active = TRUE and application_data
 * records can flow in both directions.
 * ========================================================================= */

/* TLS handshake — full implementation lives in validity-handshake.c.
 * This wrapper just delegates so existing callers in this file don't
 * need to change signature. */
static gboolean
validity_tls_handshake (FpDevice *dev, GError **error)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (dev);
  return validity_run_tls_handshake (dev, &self->session, error);
}

/* ===========================================================================
 * Async task SSMs (validity_verify + validity_enroll)
 *
 * Each public operation runs as an FpiSsm: the entry point starts the
 * task_ssm and returns immediately. State callbacks issue async USB
 * transfers; their completion callbacks advance the SSM. This is the
 * libfprint-canonical pattern; without it, pam_fprintd's signal
 * subscription races our synchronous VerifyStart and silently drops
 * the verify-match result.
 *
 * The encrypted-session prep sequence (11 wire commands) is shared
 * between verify and enroll, so it runs as a sub-SSM whose done-cb
 * advances the parent task_ssm. After prep, verify performs one
 * capture+match cycle; enroll loops capture+stage-commit until 10
 * usable stages are collected.
 * ========================================================================= */

static void
validity_rom_info_response_cb (FpDevice     *device,
                               FpiSsm       *ssm,
                               const guint8 *plain,
                               gsize         plain_len,
                               gpointer      user_data)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *error = NULL;

  (void) user_data;

  if (!validity_parse_rom_info_response (&self->session.rom_info,
                                         plain, plain_len, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->session.device_prefix[0] = self->session.rom_info.major;
  self->session.device_prefix[1] = self->session.rom_info.minor;
  self->session.device_prefix[2] = self->session.rom_info.u1;

  fpi_ssm_next_state (ssm);
}

/* --- Session prep sub-SSM ----------------------------------------------- */

typedef enum {
  PREP_S_CLEANUP_1 = 0,
  PREP_S_MSG4_1,
  PREP_S_5B_1,
  PREP_S_ROM_INFO,
  PREP_S_MSG2,
  PREP_S_75,
  PREP_S_SCAN_SETUP,
  PREP_S_CLEANUP_2,
  PREP_S_MSG4_2,
  PREP_S_5B_2,
  PREP_S_HOSTPART,
  PREP_S_CAL_CHECK,
  PREP_S_CAL_CONFIG,
  PREP_S_CAL_PROG1,
  PREP_S_CAL_RAW1,
  PREP_S_CAL_PROG2,
  PREP_S_CAL_RAW2,
  PREP_S_CAL_SAVE,
  PREP_NUM_STATES,
} ValiditySessionPrepState;

/* --- Native sensor calibration (runs inside session prep) --------------- */

static void
validity_cal_config_cb (FpDevice     *device,
                        FpiSsm       *ssm,
                        const guint8 *plain,
                        gsize         plain_len,
                        gpointer      user_data)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *err = NULL;

  (void) user_data;
  validity_cal_dump ("cal-config.bin", plain, plain_len);
  if (!validity_cal_parse_config_response (plain, plain_len,
                                           self->cal_work->trim, &err))
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  {
    g_autofree gchar *hex = validity_hex_prefix (self->cal_work->trim,
                                                 VALIDITY_CAL_TRIM_LEN, 16);
    fp_info ("validity-0088: calibration: factory trim %s...", hex);
  }
  fpi_ssm_next_state (ssm);
}

static void
validity_cal_raw_cb (FpiUsbTransfer *transfer,
                     FpDevice       *device,
                     gpointer        userdata,
                     GError         *error)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  FpiSsm *ssm = userdata;
  gboolean second = fpi_ssm_get_cur_state (ssm) == PREP_S_CAL_RAW2;
  guint8 *dst = second ? self->cal_work->baseline : self->cal_work->frame;
  GError *err = NULL;
  gdouble mean = 0;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  validity_cal_dump (second ? "cal-raw-2.bin" : "cal-raw-1.bin",
                     transfer->buffer, transfer->actual_length);
  fp_dbg ("calibration: raw stream %d: %zd B", second ? 2 : 1,
          transfer->actual_length);

  if (!validity_cal_frame_from_raw (transfer->buffer, transfer->actual_length,
                                    dst, &err))
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  if (!validity_cal_frame_plausible (dst, &mean))
    {
      fpi_ssm_mark_failed (ssm,
          g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "calibration frame %d looks wrong (mean %.1f); keep "
                       "the sensor clean and untouched and retry",
                       second ? 2 : 1, mean));
      return;
    }
  fp_info ("validity-0088: calibration: frame %d ok (mean %.1f)",
           second ? 2 : 1, mean);
  if (second)
    self->cal_work->have_baseline = TRUE;
  fpi_ssm_next_state (ssm);
}

static void
validity_cal_send_program (FpiSsm *ssm, FpDevice *device, ValidityCalProgram kind)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *err = NULL;
  gsize len = 0;
  g_autofree guint8 *cmd = validity_cal_build_from_template (self->cal_work,
                                                             kind, &len, &err);

  if (cmd == NULL)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  validity_cal_dump (kind == VALIDITY_CAL_PROG_CAL1 ? "cal-prog-1.bin"
                                                    : "cal-prog-2.bin",
                     cmd, len);
  validity_ssm_exchange (ssm, device,
                         kind == VALIDITY_CAL_PROG_CAL1 ? "calibration-program-1"
                                                        : "calibration-program-2",
                         cmd, len, TRUE, NULL, NULL);
}

static void
validity_cal_read_raw (FpiSsm *ssm, FpDevice *device)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (device);

  /* 337 x 152 B is not a multiple of the packet size, so the device ends
   * the stream with a short packet and the transfer completes. */
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_BULK_IN_ALT,
                              VALIDITY_CAL_RAW_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_RECV_TIMEOUT,
                           fpi_device_get_cancellable (device),
                           validity_cal_raw_cb, ssm);
}

static void
validity_session_prep_run_state (FpiSsm   *ssm,
                                 FpDevice *device)
{
  switch ((ValiditySessionPrepState) fpi_ssm_get_cur_state (ssm))
    {
    case PREP_S_CLEANUP_1:
    case PREP_S_CLEANUP_2:
      {
        guint8 cmd = VALIDITY_OP_ENROLL_SESSION;
        validity_ssm_exchange (ssm, device, "encrypted-cleanup",
                               &cmd, 1, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_MSG4_1:
    case PREP_S_MSG4_2:
      {
        gsize cmd_len;
        g_autofree guint8 *cmd = validity_wrap_wire_frame (
            VALIDITY_OP_INIT_MSG4,
            validity_init_msg4_payload,
            VALIDITY_INIT_MSG4_PAYLOAD_LEN,
            &cmd_len);
        validity_ssm_exchange (ssm, device, "encrypted-init-msg4",
                               cmd, cmd_len, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_5B_1:
    case PREP_S_5B_2:
      {
        guint8 cmd = VALIDITY_OP_UNKNOWN_5B;
        validity_ssm_exchange (ssm, device, "encrypted-5b",
                               &cmd, 1, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_ROM_INFO:
      {
        guint8 cmd = VALIDITY_OP_INIT_MSG1;
        validity_ssm_exchange (ssm, device, "encrypted-rom-info",
                               &cmd, 1, TRUE,
                               validity_rom_info_response_cb, NULL);
      }
      break;

    case PREP_S_MSG2:
      {
        guint8 cmd = VALIDITY_OP_INIT_MSG2;
        validity_ssm_exchange (ssm, device, "encrypted-init-msg2",
                               &cmd, 1, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_75:
      {
        guint8 cmd = VALIDITY_OP_UNKNOWN_75;
        validity_ssm_exchange (ssm, device, "encrypted-75",
                               &cmd, 1, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_SCAN_SETUP:
      {
        GError *err = NULL;
        gsize cmd_len;
        g_autofree guint8 *cmd =
            validity_build_enroll_scan_setup_command (&cmd_len, &err);
        if (cmd == NULL) { fpi_ssm_mark_failed (ssm, err); return; }
        validity_ssm_exchange (ssm, device, "enroll-scan-setup",
                               cmd, cmd_len, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_HOSTPART:
      {
        GError *err = NULL;
        gsize cmd_len;
        g_autofree guint8 *cmd =
            validity_build_enroll_hostpart_command (&cmd_len, &err);
        if (cmd == NULL) { fpi_ssm_mark_failed (ssm, err); return; }
        validity_ssm_exchange (ssm, device, "enroll-hostpart",
                               cmd, cmd_len, TRUE, NULL, NULL);
      }
      break;

    case PREP_S_CAL_CHECK:
      {
        FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
        ValidityCalibration cal;
        gboolean force = g_strcmp0 (g_getenv ("VALIDITY0088_RECALIBRATE"), "1") == 0;

        if (self->cal_work == NULL && !force && validity_cal_load (&cal, NULL))
          {
            fpi_ssm_mark_completed (ssm);
            break;
          }
        if (self->cal_work != NULL)
          {
            /* Second pass through the prep tail after calibrating. */
            g_clear_pointer (&self->cal_work, g_free);
            fpi_ssm_mark_completed (ssm);
            break;
          }
        fp_info ("validity-0088: calibrating the sensor - do not touch it");
        self->cal_work = g_new0 (ValidityCalibration, 1);
        fpi_ssm_next_state (ssm);
      }
      break;

    case PREP_S_CAL_CONFIG:
      {
        guint8 cmd[9] = { VALIDITY_OP_READ_CONFIG, 0x00, 0x0e, 0, 0, 0, 0, 0, 0 };
        validity_ssm_exchange (ssm, device, "calibration-read-config",
                               cmd, sizeof (cmd), FALSE,
                               validity_cal_config_cb, NULL);
      }
      break;

    case PREP_S_CAL_PROG1:
      validity_cal_send_program (ssm, device, VALIDITY_CAL_PROG_CAL1);
      break;

    case PREP_S_CAL_PROG2:
      validity_cal_send_program (ssm, device, VALIDITY_CAL_PROG_CAL2);
      break;

    case PREP_S_CAL_RAW1:
    case PREP_S_CAL_RAW2:
      validity_cal_read_raw (ssm, device);
      break;

    case PREP_S_CAL_SAVE:
      {
        FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
        GError *err = NULL;

        self->cal_work->valid = TRUE;
        if (!validity_cal_save (self->cal_work, &err))
          {
            fpi_ssm_mark_failed (ssm, err);
            break;
          }
        /* Re-run the end of the prep so the capture starts from the same
         * state as without calibration. */
        fpi_ssm_jump_to_state (ssm, PREP_S_CLEANUP_2);
      }
      break;

    case PREP_NUM_STATES:
      g_assert_not_reached ();
    }
}

static void
validity_session_prep_done_cb (FpiSsm   *prep_ssm,
                               FpDevice *device,
                               GError   *error)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  g_clear_pointer (&self->cal_work, g_free);
  if (error)
    fpi_ssm_mark_failed (self->task_ssm, error);
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
validity_start_session_prep (FpDevice *device)
{
  FpiSsm *prep = fpi_ssm_new (device, validity_session_prep_run_state,
                              PREP_NUM_STATES);
  fpi_ssm_start (prep, validity_session_prep_done_cb);
}

/* --- Shared capture-phase callbacks ------------------------------------- */

static void
validity_capture_response_cb (FpDevice     *device,
                              FpiSsm       *ssm,
                              const guint8 *plain,
                              gsize         plain_len,
                              gpointer      user_data)
{
  if (plain_len == VALIDITY_MATCH_RESPONSE_LEN)
    {
      const ValidityMatchResponse *match = (const ValidityMatchResponse *) plain;
      fp_dbg ("capture response: score=%u minutiae=%u threshold=%u "
              "probe=0x%02x status=0x%08x",
              match->score, match->minutia_count, match->threshold,
              match->probe_flag, match->match_result);
    }
  fpi_ssm_next_state (ssm);
}

/* --- Verify SSM --------------------------------------------------------- */

typedef enum {
  VERIFY_S_SESSION_PREP = 0,
  VERIFY_S_CAPTURE,
  VERIFY_S_WAIT_TAP,
  VERIFY_S_READ_IMAGE,
  VERIFY_S_DETECT_MINUTIAE,
  VERIFY_S_MATCH,
  VERIFY_S_LED,
  VERIFY_NUM_STATES,
} ValidityVerifyState;

static void
verify_read_image_cb (FpDevice     *device,
                      FpiSsm       *ssm,
                      const guint8 *plain,
                      gsize         plain_len,
                      gpointer      user_data)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *err = validity_no_image_error (plain, plain_len);

  if (err == NULL)
    validity_store_template (self->captured_tpl, plain, &err);
  if (err != NULL)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
  fpi_ssm_next_state (ssm);
}

static gboolean
verify_led_done_timeout (gpointer user_data)
{
  FpDevice *device = user_data;
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  if (self->task_ssm != NULL)
    fpi_ssm_mark_completed (self->task_ssm);
  return G_SOURCE_REMOVE;
}

/* Identify: pick the best-scoring gallery print at or above the threshold.
 * Prints without usable templates (other drivers, damaged data) are skipped
 * so one bad print does not block identification against the others. */
static void
validity_identify_best (FpiDeviceValidity0088      *self,
                        FpDevice                   *device,
                        const ValiditySiftFeatures *probe,
                        gint                        threshold)
{
  GPtrArray *prints = NULL;
  gint best = -1;     /* highest score of any print, for the log */

  fpi_device_get_identify_data (device, &prints);
  self->identify_match = NULL;

  for (guint i = 0; prints != NULL && i < prints->len; i++)
    {
      FpPrint *print = g_ptr_array_index (prints, i);
      g_autoptr(GVariant) data = NULL;
      g_autoptr(GError) err = NULL;
      gint idx, score;

      g_object_get (print, "fpi-data", &data, NULL);
      if (data == NULL)
        {
          fp_dbg ("identify: print %u has no data, skipped", i);
          continue;
        }
      score = validity_tpl_best_score (probe, data,
                                       fp_print_get_device_id (print),
                                       &idx, &err);
      if (score < 0)
        {
          fp_dbg ("identify: print %u skipped: %s", i, err->message);
          continue;
        }
      fp_dbg ("identify: print %u scores %d (template %d)", i, score, idx);
      if (score > best)
        {
          best = score;
          self->identify_match = score >= threshold ? print : NULL;
        }
    }

  self->verify_score = best;
  self->verify_result = self->identify_match != NULL ?
                        FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
  fp_dbg ("identify result: %s (best %d consistent keypoints over %u prints, threshold %d)",
          self->identify_match != NULL ? "MATCH" : "NO_MATCH",
          best, prints != NULL ? prints->len : 0, threshold);
}

/* Shared by verify and identify; only the match step and the report
 * differ (see fpi_device_get_current_action). */
static void
validity_verify_run_state (FpiSsm   *ssm,
                           FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  switch ((ValidityVerifyState) fpi_ssm_get_cur_state (ssm))
    {
    case VERIFY_S_SESSION_PREP:
      validity_start_session_prep (device);
      break;

    case VERIFY_S_CAPTURE:
      {
        GError *err = NULL;
        gsize cmd_len;
        /* VALIDITY0088_VERIFY_MODE=enroll selects capture mode 0x23 (used by
         * enrollment taps 2..10) instead of 0x02; default is unchanged. */
        guint8 vmode = (g_strcmp0 (g_getenv ("VALIDITY0088_VERIFY_MODE"),
                                   "enroll") == 0) ?
                       VALIDITY_CAPTURE_MODE_ENROLL :
                       VALIDITY_CAPTURE_MODE_IDENTIFY;
        g_autofree guint8 *cmd =
            validity_build_enroll_capture_command (
                device, vmode, &cmd_len, &err);
        if (cmd == NULL) { fpi_ssm_mark_failed (ssm, err); return; }
        fp_dbg ("verify: send capture program (mode 0x%02x); tap finger when prompted",
                vmode);
        validity_ssm_exchange (ssm, device, "verify-capture",
                               cmd, cmd_len, TRUE,
                               validity_capture_response_cb, NULL);
      }
      break;

    case VERIFY_S_WAIT_TAP:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      validity_async_submit_interrupt_read (ssm, device);
      break;

    case VERIFY_S_READ_IMAGE:
      {
        gsize cmd_len;
        g_autofree guint8 *cmd =
            validity_build_enroll_read_image_command (0x2000, &cmd_len);
        /* check_status=FALSE: a bad scan answers 0x51 with non-zero status
         * (0x0007); verify_read_image_cb() turns that into a retry. */
        validity_ssm_exchange (ssm, device, "verify-read-image",
                               cmd, cmd_len, FALSE,
                               verify_read_image_cb, NULL);
      }
      break;

    case VERIFY_S_DETECT_MINUTIAE:
      /* No minutiae step any more: just reject scans without ridge contrast. */
      if (!validity_tpl_quality_ok (self->captured_tpl))
        {
          fpi_ssm_mark_failed (ssm,
              fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                        "Not enough ridge detail, please retry"));
          return;
        }
      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
        {
          validity_dump_scan (self->captured_tpl, "identify", FP_FINGER_UNKNOWN);
        }
      else
        {
          FpPrint *enrolled = NULL;
          fpi_device_get_verify_data (device, &enrolled);
          validity_dump_scan (self->captured_tpl, "verify",
                              fp_print_get_finger (enrolled));
        }
      fpi_ssm_next_state (ssm);
      break;

    case VERIFY_S_MATCH:
      {
        g_autoptr(ValiditySiftFeatures) probe =
            validity_sift_extract (self->captured_tpl, VALIDITY_IMAGE_WIDTH,
                                   VALIDITY_IMAGE_HEIGHT);
        FpPrint *enrolled = NULL;
        g_autoptr(GVariant) data = NULL;
        gint threshold = validity_get_match_threshold ();
        gint best_idx = -1;
        GError *err = NULL;
        gint score;

        fp_dbg ("scan: %d keypoints", probe->n);
        if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
          {
            validity_identify_best (self, device, probe, threshold);
            fpi_ssm_next_state (ssm);
            break;
          }

        fpi_device_get_verify_data (device, &enrolled);
        g_object_get (enrolled, "fpi-data", &data, NULL);
        if (data == NULL)
          {
            fpi_ssm_mark_failed (ssm,
                fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                    "Stored print has no templates; please enroll again"));
            return;
          }

        score = validity_tpl_best_score (probe, data,
                                         fp_print_get_device_id (enrolled),
                                         &best_idx, &err);
        if (score < 0)
          {
            fpi_ssm_mark_failed (ssm, err);
            return;
          }
        self->verify_score = score;
        self->verify_result = (score >= threshold) ?
                              FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
        fp_dbg ("verify result: %s (%d consistent keypoints with template %d, threshold %d)",
                self->verify_result == FPI_MATCH_SUCCESS ? "MATCH" : "NO_MATCH",
                score, best_idx, threshold);
        fpi_ssm_next_state (ssm);
      }
      break;

    case VERIFY_S_LED:
      {
        const gchar *env = g_getenv ("VALIDITY0088_LED_RESULT");
        gboolean led_enabled = (env == NULL || g_strcmp0 (env, "0") != 0);
        GError *led_err = NULL;

        if (!led_enabled)
          {
            fpi_ssm_mark_completed (ssm);
            break;
          }

        if (self->verify_result == FPI_MATCH_SUCCESS)
          {
            validity_send_led_with_status (device, "led-success",
                                           LED_VARIANT_SUCCESS,
                                           sizeof (LED_VARIANT_SUCCESS),
                                           &led_err);
            if (led_err)
              {
                fp_warn ("LED indicator failed: %s", led_err->message);
                g_clear_error (&led_err);
              }
            fpi_ssm_mark_completed (ssm);
          }
        else
          {
            validity_send_led_with_status (device, "led-red-fail",
                                           LED_VARIANT_RED,
                                           sizeof (LED_VARIANT_RED),
                                           &led_err);
            if (led_err)
              {
                fp_warn ("LED indicator failed: %s", led_err->message);
                g_clear_error (&led_err);
              }
            g_timeout_add (800, verify_led_done_timeout, device);
          }
      }
      break;

    case VERIFY_NUM_STATES:
      g_assert_not_reached ();
    }
}

static void
validity_verify_ssm_done (FpiSsm   *ssm,
                          FpDevice *device,
                          GError   *error)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  gboolean identify =
      fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY;

  g_assert (self->task_ssm == ssm);
  self->task_ssm = NULL;

  if (error)
    {
      /* A retry (bad tap) is a reported result, anything else fails. */
      if (error->domain == FP_DEVICE_RETRY)
        {
          if (identify)
            fpi_device_identify_report (device, NULL, NULL, error);
          else
            fpi_device_verify_report (device, FPI_MATCH_ERROR, NULL, error);
          error = NULL;
        }
      if (identify)
        fpi_device_identify_complete (device, error);
      else
        fpi_device_verify_complete (device, error);
      g_clear_object (&self->stage_print);
      self->identify_match = NULL;
      return;
    }

  if (identify)
    {
      fpi_device_identify_report (device, self->identify_match,
                                  g_steal_pointer (&self->stage_print), NULL);
      fpi_device_identify_complete (device, NULL);
    }
  else
    {
      fpi_device_verify_report (device, self->verify_result,
                                g_steal_pointer (&self->stage_print), NULL);
      fpi_device_verify_complete (device, NULL);
    }
  self->identify_match = NULL;
}

/* --- Enroll SSM --------------------------------------------------------- */

typedef enum {
  ENROLL_S_SESSION_PREP = 0,
  ENROLL_S_STAGE_BEGIN,
  ENROLL_S_STAGE_SCAN_SETUP,
  ENROLL_S_CAPTURE,
  ENROLL_S_WAIT_TAP,
  ENROLL_S_READ_IMAGE,
  ENROLL_S_DETECT_MINUTIAE,
  ENROLL_S_STAGE_COMMIT,
  ENROLL_NUM_STATES,
} ValidityEnrollState;

static gboolean
enroll_bump_attempts_or_fail (FpiSsm *ssm,
                              FpiDeviceValidity0088 *self)
{
  self->enroll_attempts++;
  if (self->enroll_attempts > VALIDITY_MAX_ENROLL_STAGES * 4)
    {
      fpi_ssm_mark_failed (ssm,
          fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                    "too many rejected enrollment captures"));
      return FALSE;
    }
  return TRUE;
}

static void
enroll_read_image_cb (FpDevice     *device,
                      FpiSsm       *ssm,
                      const guint8 *plain,
                      gsize         plain_len,
                      gpointer      user_data)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *err = validity_no_image_error (plain, plain_len);

  if (err != NULL)
    {
      fp_dbg ("enroll capture retry: %s", err->message);
      fpi_device_enroll_progress (device, self->enroll_completed, NULL, err);
      self->enroll_needs_scan_setup = TRUE;
      if (!enroll_bump_attempts_or_fail (ssm, self))
        return;
      fpi_ssm_jump_to_state (ssm, ENROLL_S_STAGE_BEGIN);
      return;
    }

  if (!validity_store_template (self->captured_tpl, plain, &err))
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
  fpi_ssm_next_state (ssm);
}

static void
validity_enroll_run_state (FpiSsm   *ssm,
                           FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  switch ((ValidityEnrollState) fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_S_SESSION_PREP:
      validity_start_session_prep (device);
      break;

    case ENROLL_S_STAGE_BEGIN:
      if (self->enroll_needs_scan_setup)
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_jump_to_state (ssm, ENROLL_S_CAPTURE);
      break;

    case ENROLL_S_STAGE_SCAN_SETUP:
      {
        GError *err = NULL;
        gsize cmd_len;
        g_autofree guint8 *cmd =
            validity_build_enroll_scan_setup_command (&cmd_len, &err);
        if (cmd == NULL) { fpi_ssm_mark_failed (ssm, err); return; }
        validity_ssm_exchange (ssm, device, "enroll-scan-setup",
                               cmd, cmd_len, TRUE, NULL, NULL);
      }
      break;

    case ENROLL_S_CAPTURE:
      {
        GError *err = NULL;
        gsize cmd_len;
        g_autofree guint8 *cmd =
            validity_build_enroll_capture_command (
                device,
                /* Windows sends mode 0x02 (identify) for the first capture of
                 * an enrollment and 0x23 (enroll) for every later one. */
                (self->enroll_completed == 0 && self->enroll_attempts == 0) ?
                    VALIDITY_CAPTURE_MODE_IDENTIFY : VALIDITY_CAPTURE_MODE_ENROLL,
                &cmd_len, &err);
        if (cmd == NULL) { fpi_ssm_mark_failed (ssm, err); return; }
        fp_dbg ("enroll stage %d/%d: send capture program",
                self->enroll_completed + 1, VALIDITY_MAX_ENROLL_STAGES);
        validity_ssm_exchange (ssm, device, "enroll-capture",
                               cmd, cmd_len, TRUE,
                               validity_capture_response_cb, NULL);
      }
      break;

    case ENROLL_S_WAIT_TAP:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      validity_async_submit_interrupt_read (ssm, device);
      break;

    case ENROLL_S_READ_IMAGE:
      {
        gsize cmd_len;
        g_autofree guint8 *cmd =
            validity_build_enroll_read_image_command (0x2000, &cmd_len);
        /* check_status=FALSE: a bad scan answers 0x51 with non-zero status
         * (0x0007); enroll_read_image_cb() turns that into a retry. */
        validity_ssm_exchange (ssm, device, "enroll-read-image",
                               cmd, cmd_len, FALSE,
                               enroll_read_image_cb, NULL);
      }
      break;

    case ENROLL_S_DETECT_MINUTIAE:
      /* No minutiae step any more: reject scans without ridge contrast. */
      if (!validity_tpl_quality_ok (self->captured_tpl))
        {
          fpi_device_enroll_progress (device, self->enroll_completed, NULL,
              fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                        "Not enough ridge detail, please retry"));
          self->enroll_needs_scan_setup = TRUE;
          if (!enroll_bump_attempts_or_fail (ssm, self))
            return;
          fpi_ssm_jump_to_state (ssm, ENROLL_S_STAGE_BEGIN);
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_S_STAGE_COMMIT:
      {
        FpPrint *enroll_print = NULL;

        fpi_device_get_enroll_data (device, &enroll_print);
        validity_dump_scan (self->captured_tpl, "enroll",
                            fp_print_get_finger (enroll_print));
        if (self->enroll_templates == NULL)
          self->enroll_templates =
              g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
        {
          g_autoptr(ValiditySiftFeatures) feat =
              validity_sift_extract (self->captured_tpl, VALIDITY_IMAGE_WIDTH,
                                     VALIDITY_IMAGE_HEIGHT);
          fp_dbg ("enroll tap: %d keypoints", feat->n);
          g_ptr_array_add (self->enroll_templates,
                           validity_sift_serialize (feat));
        }
        /* Keep a print to the newest VALIDITY_PRINT_MAX_TAPS taps. */
        if (self->enroll_templates->len > VALIDITY_PRINT_MAX_TAPS)
          g_ptr_array_remove_range (self->enroll_templates, 0,
                                    self->enroll_templates->len -
                                    VALIDITY_PRINT_MAX_TAPS);
        self->enroll_completed++;
        fp_dbg ("enroll stage %d/%d accepted",
                self->enroll_completed, VALIDITY_MAX_ENROLL_STAGES);
        fpi_device_enroll_progress (device, self->enroll_completed,
                                    NULL, NULL);
        self->enroll_needs_scan_setup = TRUE;

        if (self->enroll_completed < VALIDITY_MAX_ENROLL_STAGES)
          fpi_ssm_jump_to_state (ssm, ENROLL_S_STAGE_BEGIN);
        else
          {
            GError *err = NULL;
            GVariant *data = validity_tpl_print_data (
                self->enroll_templates,
                fp_print_get_device_id (enroll_print), &err);

            if (data == NULL)
              {
                fpi_ssm_mark_failed (ssm, err);
                return;
              }
            g_object_set (enroll_print, "fpi-data", data, NULL);
            fpi_ssm_mark_completed (ssm);
          }
      }
      break;

    case ENROLL_NUM_STATES:
      g_assert_not_reached ();
    }
}

static void
validity_enroll_ssm_done (FpiSsm   *ssm,
                          FpDevice *device,
                          GError   *error)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  FpPrint *enroll_print = NULL;

  g_assert (self->task_ssm == ssm);
  self->task_ssm = NULL;

  g_clear_pointer (&self->enroll_templates, g_ptr_array_unref);

  if (error)
    {
      fpi_device_enroll_complete (device, NULL, error);
      g_clear_object (&self->stage_print);
      return;
    }

  fpi_device_get_enroll_data (device, &enroll_print);
  fpi_device_enroll_complete (device, g_object_ref (enroll_print), NULL);
}

/* ===========================================================================
 * GObject lifecycle
 * ========================================================================= */

static void
validity_probe (FpDevice *device)
{
  GUsbDevice *usb_dev;
  GError *error = NULL;
  g_autofree gchar *serial = NULL;

  usb_dev = fpi_device_get_usb_device (device);
  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    serial = g_strdup ("emulated-device");
  else
    serial = g_usb_device_get_string_descriptor (
        usb_dev,
        g_usb_device_get_serial_number_index (usb_dev),
        &error);
  if (error)
    {
      g_clear_error (&error);
      serial = g_strdup ("unknown");
    }

  fpi_device_set_nr_enroll_stages (device, VALIDITY_MAX_ENROLL_STAGES);
  g_usb_device_release_interface (usb_dev, 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);

  fpi_device_probe_complete (device, serial, NULL, NULL);
}

static void
validity_open (FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device),
                                     0, 0, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  /* DEBUG: VALIDITY0088_TRY_HARD_RESET=1 sends the hard-reset bulk
   * command then bails so the user can replug and re-test as a fresh
   * device. Always errors out — caller must unset the env var before
   * re-opening. */
  {
    const gchar *try_reset = g_getenv ("VALIDITY0088_TRY_HARD_RESET");
    if (try_reset != NULL && g_strcmp0 (try_reset, "0") != 0)
      {
        GError *reset_err = NULL;
        validity_try_hard_reset (device, &reset_err);
        fpi_device_open_complete (device, reset_err);
        return;
      }
  }

  /* VALIDITY0088_FORCE_REPAIR=1 wipes the cached pairing artifacts so
   * the post-handshake backfill below runs a fresh ceremony. Useful for
   * iterating on the pairing flow without manually deleting files. */
  {
    const gchar *force = g_getenv ("VALIDITY0088_FORCE_REPAIR");
    if (force != NULL && g_strcmp0 (force, "0") != 0)
      {
        GError *wipe_err = NULL;
        if (!validity_pairing_wipe_state (&wipe_err))
          {
            fp_warn ("VALIDITY0088_FORCE_REPAIR wipe failed (continuing): %s",
                     wipe_err ? wipe_err->message : "unknown");
            g_clear_error (&wipe_err);
          }
      }
  }

  self->interrupt_cancellable = g_cancellable_new ();
  self->phase = VALIDITY_PHASE_PLAINTEXT_INIT;
  validity_session_clear_fresh_pair (&self->session);
  memset (&self->session, 0, sizeof (self->session));

  /* One-time bootstrap: if validity-pubkeys.c has baked-in cert + host
   * scalar and the on-disk pairing storage doesn't yet have a paired
   * cert+keypair, copy the baked-in arrays to disk. This makes
   * load_cached_host_keypair / validity_pairing_load_cert_blob succeed
   * on subsequent opens. Idempotent + safe; never touches the device. */
  {
    GError *boot_err = NULL;
    if (!validity_pairing_bootstrap_from_hardcoded (&boot_err))
      {
        fp_warn ("hardcoded->on-disk bootstrap failed (non-fatal): %s",
                 boot_err ? boot_err->message : "unknown");
        g_clear_error (&boot_err);
      }
  }

  /* Autonomous fresh-pair: if there's no on-disk pairing material AND the
   * baked arrays in validity-pubkeys.c are zeroed, run the pre-TLS ceremony
   * NOW to obtain fresh cert+priv from the device (clean-slate INIT_MSG4 +
   * CSR-signed 0x4f + 0x50 ECDH extract). See validity-pretls-pair.c.
   * Set VALIDITY0088_SKIP_AUTO_PAIR=1 to disable. */
  if (!validity_pairing_have_per_pairing_material ()
      && g_getenv ("VALIDITY0088_SKIP_AUTO_PAIR") == NULL)
    {
      GError *cer_err = NULL;
      fp_info ("no on-disk pair material AND validity-pubkeys.c is zeroed; "
               "running Linux-autonomous pre-TLS fresh-pair ceremony");
      if (!validity_pairing_run_pretls_fresh_pair_ceremony (device, &cer_err))
        {
          /* Couldn't pair — bail with actionable error including the env-var
           * disable hint. */
          g_set_error (
              &error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
              "Linux-autonomous fresh-pair ceremony failed: %s — "
              "Set VALIDITY0088_SKIP_AUTO_PAIR=1 to disable the ceremony "
              "and supply your own cert-blob + host-key.pem manually.",
              cer_err ? cer_err->message : "unknown");
          g_clear_error (&cer_err);
          fpi_device_open_complete (device, error);
          return;
        }
      fp_info ("pre-TLS fresh-pair complete; proceeding with TLS handshake "
               "using device-issued cert + priv");
    }

  /* Plaintext init phase. On open() we don't yet know whether the
   * upstream operation will be enroll vs verify; the device-side
   * enroll-session opcode (0x1a) only matters for enrollment, so
   * defer it to the enroll path and send the four common opcodes here. */
  if (!validity_send_plaintext_init (device, FALSE, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }
  self->phase = VALIDITY_PHASE_TLS_HANDSHAKE;

  /* TLS handshake */
  if (!validity_tls_handshake (device, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }
  self->phase = VALIDITY_PHASE_APP_DATA;

  /* Phase 9 - first-pair backfill. If the handshake succeeded but the
   * pairing storage dir is missing one of {hostPart, cert blob, OSB},
   * run the ceremony now to populate them. This covers:
   *   - already-paired devices opened from a wiped storage dir
   *   - users who set VALIDITY0088_FORCE_REPAIR above
   * There is no automatic recovery when the handshake fails (open fails;
   * VALIDITY0088_FORCE_REPAIR=1 forces a fresh pairing); this
   * backfill path is for the case where the device IS already paired
   * and we just need to repopulate local state. */
  if (!validity_pairing_state_complete ())
    {
      GError *pair_err = NULL;
      fp_info ("pairing state incomplete; running ceremony to backfill");
      if (!validity_pairing_run_ceremony (device, &pair_err))
        {
          fp_warn ("pairing ceremony failed (non-fatal): %s",
                   pair_err ? pair_err->message : "unknown");
          g_clear_error (&pair_err);
        }
    }


  /* Host-UUID continuity check.
   * After a successful handshake, verify that the host identity hasn't
   * changed since the last pairing. On mismatch, the pairing code will
   * need to re-wipe and re-pair — but we don't block the open here.
   * The enroll/verify paths check the wipe marker and re-pair on demand.
   */
  {
    gboolean needs_re_pair = FALSE;
    GError *guid_error = NULL;

    if (!validity_pairing_check_guid_continuity (&needs_re_pair, &guid_error))
      {
        /* Non-fatal: log warning but don't block open. The guid continuity
         * check failing is only an I/O error on the host side; the device
         * connection itself is fine. */
        fp_warn ("GUID continuity check failed (non-fatal): %s",
                 guid_error ? guid_error->message : "unknown error");
        g_clear_error (&guid_error);
      }
    else if (needs_re_pair)
      {
        fp_warn ("host UUID changed since last pairing — re-pair needed. "
                 "Will attempt fresh pairing on next enroll/verify.");
        /* TODO: trigger fresh pairing ceremony once P1-P5 are complete.
         * For now: just flag it. The device will likely reject enroll/verify
         * operations until re-pairing happens. */
      }
    else
      {
        fp_dbg ("host identity continuity verified — operating normally");
      }
  }

  fpi_device_open_complete (device, NULL);
}

static void
validity_close (FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  GError *error = NULL;

  if (self->session.handshake_hash_ctx)
    {
      /* free EVP_MD_CTX — done by validity-crypto.c on next init,
       * but proactively zero out here too. */
      self->session.handshake_hash_ctx = NULL;
    }
  validity_session_clear_fresh_pair (&self->session);
  memset (&self->session, 0, sizeof (self->session));

  g_usb_device_release_interface (fpi_device_get_usb_device (device),
                                  0, 0, &error);
  fpi_device_close_complete (device, error);
}

static void validity_cancel_reset (FpDevice *device);

static void
validity_enroll (FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);
  FpPrint *enroll_print = NULL;
  FpiPrintType print_type = FPI_PRINT_UNDEFINED;

  validity_cancel_reset (device);

  fpi_device_get_enroll_data (device, &enroll_print);
  g_object_get (enroll_print, "fpi-type", &print_type, NULL);
  g_clear_pointer (&self->enroll_templates, g_ptr_array_unref);

  /* Print update (FP_DEVICE_FEATURE_UPDATE_PRINT): libfprint passes an
   * existing print as template; its taps are kept and the new ones added,
   * so enrolling a finger again in another sitting widens its coverage. */
  if (print_type != FPI_PRINT_UNDEFINED)
    {
      g_autoptr(GVariant) data = NULL;
      GError *err = NULL;

      g_object_get (enroll_print, "fpi-data", &data, NULL);
      if (data != NULL)
        {
          self->enroll_templates = validity_tpl_load (
              data, fp_print_get_device_id (enroll_print), &err);
          if (self->enroll_templates == NULL)
            {
              fpi_device_enroll_complete (device, NULL, err);
              return;
            }
          fp_info ("validity-0088: updating a print with %u taps; adding %d",
                   self->enroll_templates->len, VALIDITY_MAX_ENROLL_STAGES);
        }
    }
  if (print_type != FPI_PRINT_RAW)
    fpi_print_set_type (enroll_print, FPI_PRINT_RAW);

  g_clear_object (&self->stage_print);
  self->enroll_completed = 0;
  self->enroll_attempts = 0;
  self->enroll_needs_scan_setup = FALSE;

  fp_dbg ("starting validity-0088 async enrollment");

  self->task_ssm = fpi_ssm_new (device, validity_enroll_run_state,
                                ENROLL_NUM_STATES);
  fpi_ssm_start (self->task_ssm, validity_enroll_ssm_done);
}

static void
validity_verify (FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  validity_cancel_reset (device);

  g_clear_object (&self->stage_print);
  self->verify_result = FPI_MATCH_ERROR;
  self->identify_match = NULL;

  fp_dbg ("starting validity-0088 async %s",
          fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY ?
          "identify" : "verify");

  self->task_ssm = fpi_ssm_new (device, validity_verify_run_state,
                                VERIFY_NUM_STATES);
  fpi_ssm_start (self->task_ssm, validity_verify_ssm_done);
}

static void
validity_cancel (FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  fp_dbg ("validity_cancel: aborting in-flight USB ops");

  /* Wake any pending EP 0x83 interrupt wait. The dedicated cancellable
   * gets replaced with a fresh one on the next operation start.
   */
  if (self->interrupt_cancellable)
    g_cancellable_cancel (self->interrupt_cancellable);

  /* Bulk USB transfers (out + in) hook fpi_device_get_cancellable()
   * directly, so libfprint's own cancel of that cancellable will abort
   * them. Nothing extra to do here for them.
   */
}

/* Reset the interrupt cancellable so a fresh enroll/verify can wait.
 * Called at the start of each public operation.
 */
static void
validity_cancel_reset (FpDevice *device)
{
  FpiDeviceValidity0088 *self = FPI_DEVICE_VALIDITY_0088 (device);

  if (self->interrupt_cancellable &&
      g_cancellable_is_cancelled (self->interrupt_cancellable))
    {
      g_object_unref (self->interrupt_cancellable);
      self->interrupt_cancellable = g_cancellable_new ();
    }
  else if (self->interrupt_cancellable == NULL)
    {
      self->interrupt_cancellable = g_cancellable_new ();
    }
}

static void
fpi_device_validity_0088_init (FpiDeviceValidity0088 *self)
{
  /* Zero session state; allocated members are populated lazily. */
  validity_session_clear_fresh_pair (&self->session);
  memset (&self->session, 0, sizeof (self->session));
  self->phase = VALIDITY_PHASE_PLAINTEXT_INIT;
}

static void
fpi_device_validity_0088_class_init (FpiDeviceValidity0088Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id        = "validity-0088";
  dev_class->full_name = "Synaptics/Validity (pre-Prometheus)";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table  = validity_id_table;
  dev_class->nr_enroll_stages = VALIDITY_MAX_ENROLL_STAGES;

  dev_class->probe   = validity_probe;
  dev_class->open    = validity_open;
  dev_class->close   = validity_close;
  dev_class->enroll  = validity_enroll;
  dev_class->verify  = validity_verify;
  dev_class->identify = validity_verify;     /* same SSM, see VERIFY_S_MATCH */
  dev_class->cancel  = validity_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features |= FP_DEVICE_FEATURE_UPDATE_PRINT;
}
