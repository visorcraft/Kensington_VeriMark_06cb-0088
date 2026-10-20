/*
 * test-pairing-key.c — the P-256 key built from a raw scalar for signing the
 * pairing certificate request (ec_pkey_from_scalar_be in
 * validity-pretls-pair.c, OpenSSL 3 EVP API).
 *
 * The source file is included so its static helpers can be tested; the
 * device functions it also contains are never called (their USB and storage
 * calls resolve to stubs-usb.c and stubs.c).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "validity-pretls-pair.c"

#include <openssl/rand.h>
#include <stdio.h>

static gint failures;

#define CHECK(cond, ...) G_STMT_START {                       \
    if (cond) { printf ("ok   "); }                           \
    else      { printf ("FAIL "); failures++; }               \
    printf (__VA_ARGS__); printf ("\n");                      \
  } G_STMT_END

/* P-256 base point G (SEC 2 / FIPS 186), uncompressed: 04 || X || Y */
static const char *G_HEX =
  "04"
  "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
  "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

static gchar *
pub_hex (EVP_PKEY *k)
{
  guint8 buf[65];
  size_t len = 0;
  GString *s = g_string_new (NULL);

  if (EVP_PKEY_get_octet_string_param (k, OSSL_PKEY_PARAM_PUB_KEY, buf, sizeof buf, &len) != 1)
    return g_string_free (s, FALSE);
  for (size_t i = 0; i < len; i++)
    g_string_append_printf (s, "%02x", buf[i]);
  return g_string_free (s, FALSE);
}

static gboolean
sign_verify (EVP_PKEY *k)
{
  const unsigned char msg[] = "validity-0088 certificate body";
  g_autofree guint8 *sig = NULL;
  gsize sig_len = 0;
  EVP_MD_CTX *c;
  gboolean ok;

  sig = ecdsa_sha256_sign_der (k, msg, sizeof msg, &sig_len, NULL);
  if (sig == NULL)
    return FALSE;
  c = EVP_MD_CTX_new ();
  ok = EVP_DigestVerifyInit (c, NULL, EVP_sha256 (), NULL, k) == 1 &&
       EVP_DigestVerify (c, sig, sig_len, msg, sizeof msg) == 1;
  EVP_MD_CTX_free (c);
  return ok;
}

int
main (void)
{
  guint8 one[32] = { 0 };
  EVP_PKEY *k;
  g_autofree gchar *hex = NULL;
  gint bad = 0;

  one[31] = 1;
  k = ec_pkey_from_scalar_be (one, NULL);
  hex = k ? pub_hex (k) : NULL;
  CHECK (hex && g_str_equal (hex, G_HEX), "scalar 1 gives the P-256 base point G (known answer)");
  CHECK (k && sign_verify (k), "signature made with the key verifies");
  EVP_PKEY_free (k);

  for (gint i = 0; i < 50; i++)
    {
      guint8 d[32];
      BIGNUM *priv = NULL, *want;

      RAND_bytes (d, sizeof d);
      d[0] &= 0x7f;                               /* stay below the group order */
      k = ec_pkey_from_scalar_be (d, NULL);
      want = BN_bin2bn (d, 32, NULL);
      if (!k || EVP_PKEY_get_bn_param (k, OSSL_PKEY_PARAM_PRIV_KEY, &priv) != 1 ||
          BN_cmp (priv, want) != 0 || !sign_verify (k))
        bad++;
      BN_free (priv);
      BN_free (want);
      EVP_PKEY_free (k);
    }
  CHECK (bad == 0, "50 random scalars: private part kept, signatures verify (%d bad)", bad);

  {
    guint8 hs_le[32], hs_be[32];
    g_autofree gchar *h1 = NULL, *h2 = NULL;
    EVP_PKEY *a, *b;

    compute_hs_key (hs_le);
    for (gint i = 0; i < 32; i++)
      hs_be[i] = hs_le[31 - i];
    a = ec_pkey_from_scalar_be (hs_be, NULL);
    b = ec_pkey_from_scalar_be (hs_be, NULL);
    h1 = a ? pub_hex (a) : NULL;
    h2 = b ? pub_hex (b) : NULL;
    CHECK (a && h1 && h2 && g_str_equal (h1, h2) && sign_verify (a),
           "the pairing signing key (hs_key) builds deterministically and signs");
    EVP_PKEY_free (a);
    EVP_PKEY_free (b);
  }

  printf ("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures != 0;
}
