/*
  This file is part of GNUnet
  Copyright (C) 2014,2015 GNUnet e.V.

  GNUnet is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  GNUnet is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

/**
 * @file util/test_crypto_rsa.c
 * @brief testcase for utility functions for RSA cryptography
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#include "platform.h"
#include <gcrypt.h>
#include "gnunet_util_lib.h"

#define KEY_SIZE 1024


gcry_error_t
rsa_full_domain_hash (gcry_mpi_t *r,
                      const struct GNUNET_HashCode *hash,
                      const struct GNUNET_CRYPTO_RsaPublicKey *pkey,
                      size_t *rsize);


int
main (int argc,
      char *argv[])
{
#define RND_BLK_SIZE 4096
  unsigned char rnd_blk[RND_BLK_SIZE];
  struct GNUNET_CRYPTO_RsaPrivateKey *priv;
  struct GNUNET_CRYPTO_RsaPrivateKey *priv_copy;
  struct GNUNET_CRYPTO_RsaPublicKey *pub;
  struct GNUNET_CRYPTO_RsaPublicKey *pub_copy;
  struct GNUNET_CRYPTO_RsaSignature *sig;
  struct GNUNET_CRYPTO_RsaSignature *sig_copy;
  struct GNUNET_CRYPTO_RsaSignature *bsig;
  struct GNUNET_CRYPTO_RsaBlindingKeySecret bsec;
  struct GNUNET_HashCode hash;
  char *blind_buf;
  size_t bsize;
  gcry_mpi_t v;

  GNUNET_log_setup ("test-rsa", "WARNING", NULL);
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              rnd_blk,
                              RND_BLK_SIZE);
  GNUNET_CRYPTO_hash (rnd_blk,
                      RND_BLK_SIZE,
                      &hash);
  priv = GNUNET_CRYPTO_rsa_private_key_create (KEY_SIZE);
  priv_copy = GNUNET_CRYPTO_rsa_private_key_dup (priv);
  GNUNET_assert (NULL != priv_copy);
  GNUNET_assert (0 == GNUNET_CRYPTO_rsa_private_key_cmp (priv, priv_copy));
  pub = GNUNET_CRYPTO_rsa_private_key_get_public (priv);

  /* Encoding */
  size_t size;
  char *enc;
  enc = NULL;
  size = GNUNET_CRYPTO_rsa_private_key_encode (priv, &enc);

  /* Decoding */
  GNUNET_CRYPTO_rsa_private_key_free (priv);
  priv = NULL;
  priv = GNUNET_CRYPTO_rsa_private_key_decode (enc, size);
  GNUNET_assert (NULL != priv);
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              enc, size);
  GNUNET_assert (NULL == GNUNET_CRYPTO_rsa_private_key_decode (enc, size));
  (void) fprintf (stderr, "The above warning is expected.\n");
  GNUNET_free (enc);

  /* test full domain hash size */
  GNUNET_assert (0 == rsa_full_domain_hash (&v, &hash, pub, NULL));
  GNUNET_assert (gcry_mpi_get_nbits(v) < KEY_SIZE);
  gcry_mpi_clear_highbit (v, gcry_mpi_get_nbits(v)-1); /* clear the set high bit */
  GNUNET_assert (gcry_mpi_get_nbits(v) > 3*KEY_SIZE/4);
  /* This test necessarily randomly fails with probability 2^(3 - KEY_SIZE/4) */
  gcry_mpi_release(v);

  /* try ordinary sig first */
  sig = GNUNET_CRYPTO_rsa_sign_fdh (priv,
                                    &hash);
  sig_copy = GNUNET_CRYPTO_rsa_signature_dup (sig);
  GNUNET_assert (NULL != sig);
  GNUNET_assert (0 == GNUNET_CRYPTO_rsa_signature_cmp (sig, sig_copy));
  pub_copy = GNUNET_CRYPTO_rsa_public_key_dup (pub);
  GNUNET_assert (NULL != pub_copy);
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_rsa_verify (&hash, sig, pub_copy));
  /* corrupt our hash and see if the signature is still valid */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK, &hash,
                              sizeof (struct GNUNET_HashCode));
  GNUNET_assert (GNUNET_OK != GNUNET_CRYPTO_rsa_verify (&hash,
                                                        sig,
                                                        pub));
  (void) fprintf (stderr, "The above warning is expected.\n");
  GNUNET_CRYPTO_rsa_signature_free (sig);

  /* test blind signing */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
			      &bsec,
			      sizeof (bsec));
  bsize = GNUNET_CRYPTO_rsa_blind (&hash,
				   &bsec,
				   pub,
				   &blind_buf);
  GNUNET_assert (0 != bsize);
  bsig = GNUNET_CRYPTO_rsa_sign_blinded (priv,
                                         blind_buf,
                                         bsize);
  GNUNET_free (blind_buf);
  sig = GNUNET_CRYPTO_rsa_unblind (bsig,
				   &bsec,
				   pub);
  GNUNET_CRYPTO_rsa_signature_free (bsig);
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_rsa_verify (&hash, sig, pub));  
  GNUNET_CRYPTO_rsa_signature_free (sig);
  GNUNET_CRYPTO_rsa_signature_free (sig_copy);
  GNUNET_CRYPTO_rsa_private_key_free (priv);
  GNUNET_CRYPTO_rsa_private_key_free (priv_copy);
  GNUNET_CRYPTO_rsa_public_key_free (pub);
  GNUNET_CRYPTO_rsa_public_key_free (pub_copy);
  return 0;
}
