/*
 * Copyright 2020 Intel Corporation
 * SPDX-License-Identifier: Apache 2.0
 */

/*!
 * \file
 * \brief Abstraction layer for RSA encryption routines of openssl library.
 */

#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>
#include <openssl/ossl_typ.h>
#include "BN_support.h"
#include "sdoCryptoHal.h"
#include "util.h"
#include "safe_lib.h"

/* An Example Public Key
 * Formats are described in the SDO documentation, but here is a public key
encoded with the RSAMODEXP format:

["0228", # length in bytes
  4, # message type = persisted public key
  5, # version = 0.5
  # public key object
  [   1, # algorithm = RSA
      3, # encoding = RSAMODENC
      [   257, # modulus length in bytes, followed by modulus in base64
	 "00a293ae46ca4e532c5abe7e173cb0fa91a12eee06ea355b2a785d654401bfe7d13b97d5bbce977788a701c038032ea5b30f6892fa343205bdeda3eb5516e7782e44bbdfe9eafe3cce65b0d2d92dbbc879483506fb355ad35f8b2f48d53ac44e39d05ad51d816b44e36704eae9dd631a392de7147d0a512c489e9d36fdf98230972247618f833c6cb7cae01688be27ab827c75554425b42787c80fa4937a2e80bd0ca1c1759c6f18d59b68a4833f266911fbaf536c9b1e527b6da2332daa288e9c3bb06f42d2324419404b596582968d513d2b04c9e77dd6abf90ffcdf25bea063164dbf70385d706f9c20afcf5a103135986fa444076354bcdefcc86b0c763adf",
	 3, # exponent length in bytes, followed by exponent in hex
	 "010001"
      ]
  ] #end public key
] #end persisted message

 */

/* **************************************************************************
// CONVERT2PKEY
// Convert a public key into an OpenSSL PKEY for use by the libcrypto routines
//
// PURPOSE: Convert a public key into an OpenSSL key
//
// REQUIRE: An public key in SDOPublicKey structure
//          A pointer to a PKEY structure, which will be allocated by this
routine
//          using OpenSSL's EVP_PKEY alloc and free functions
//
// PROMISE: if SDOPublicKey is a valid RSA public key in version 0.5 structure
in
RSAMODENC
//          this routine will take the modulus and public key and format them
into a PKEY structure
//
// RESULT: Return 0 with the modulus and exponent placed into a the PKEY
structure in the "out" parameter
//         or return -1 on failure
//
// Author: David M. Wheeler, 16 June 2016
// Copyright (c) Intel 2016
// ************************************************************************** */
static int convert2pkey(EVP_PKEY **out, RSA **rsa_in, const uint8_t *key1,
			uint32_t keyParam1Length1, const uint8_t *key2,
			uint32_t keyParam1Length2)
{
	RSA *rsa = NULL;

	if (!out || key1 == NULL || key2 == NULL) {
		return -1;
	}

	if (*out != NULL) {
		EVP_PKEY_free(*out);
		*out = NULL;
	}

	if (*rsa_in != NULL) {
		RSA_free(*rsa_in); /* deep free of all rsa elements */
	}

	*rsa_in = RSA_new();
	rsa = *rsa_in;

	*out = EVP_PKEY_new();
	if (*out == NULL) {
		return -1;
	}
	BIGNUM *n = NULL;
	BIGNUM *d = NULL;
	BIGNUM *e = NULL;
	BIGNUM *p = NULL;
	BIGNUM *q = NULL;
	BIGNUM *dmp1 = NULL;
	BIGNUM *dmq1 = NULL;
	BIGNUM *iqmp = NULL;

	/* We need the RSA components non-NULL. */
	if (rsa == NULL) {
		return -1;
	} else if ((n = BN_new()) == NULL) {
		return -1;
	} else if ((d = BN_new()) == NULL) {
		return -1;
	} else if ((e = BN_new()) == NULL) {
		return -1;
	} else if ((p = BN_new()) == NULL) {
		return -1;
	} else if ((q = BN_new()) == NULL) {
		return -1;
	} else if ((dmp1 = BN_new()) == NULL) {
		return -1;
	} else if ((dmq1 = BN_new()) == NULL) {
		return -1;
	} else if ((iqmp = BN_new()) == NULL) {
		return -1;
	}

	/* Set verifier key's MODULUS. */
	if (BN_bin2bn((const unsigned char *)key1, keyParam1Length1, n) ==
	    NULL) {
		goto err;
	}

	/* Set verifier key's EXPONENT. */
	if (BN_bin2bn((const unsigned char *)key2, keyParam1Length2, e) ==
	    NULL) {
		goto err;
	}

	if (0 == RSA_set0_key(rsa, n, e, d) ||
	    0 == RSA_set0_factors(rsa, p, q) ||
	    0 == RSA_set0_crt_params(rsa, dmp1, dmq1, iqmp))
		goto err;

	if (!EVP_PKEY_set1_RSA(*out, rsa))
		goto err;
	return 0;
err:
	// no null check here as lib has it
	BN_clear_free(n);
	BN_clear_free(d);
	BN_clear_free(e);
	BN_clear_free(p);
	BN_clear_free(q);
	BN_clear_free(dmp1);
	BN_clear_free(dmq1);
	BN_clear_free(iqmp);
	return -1;
}
/**
 * sdoCryptoRSAEncrypt -  Encrypt the block passed using the public key
 * passed, the key must be RSA
 * @param hashType - Hash type (SDO_CRYPTO_HASH_TYPE_SHA_256)
 * @param keyEncoding - RSA Key encoding typee.
 * @param keyAlgorithm - RSA public key algorithm.
 * @param clearText - Input text to be encrypted.
 * @param clearTextLength - Plain text size in bytes.
 * @param cipherText - Encrypted text(output).
 * @param cipherTextLength - Encrypted text size in bytes.
 * @param keyParam1 - pointer of type uint8_t, holds the public key1.
 * @param keyParam1Length - size of public key1, type size_t.
 * @param keyParam2 - pointer of type uint8_t,holds the public key2.
 * @param keyParam2Length - size of public key2, type size_t
 * @return ret
 *        return 0 on success. -1 on failure.
 *        return cypherLength in bytes while cipherText passed as NULL, & all
 *        other parameters are passed as it is.
 */
int32_t sdoCryptoRSAEncrypt(uint8_t hashType, uint8_t keyEncoding,
			    uint8_t keyAlgorithm, const uint8_t *clearText,
			    uint32_t clearTextLength, uint8_t *cipherText,
			    uint32_t cipherTextLength, const uint8_t *keyParam1,
			    uint32_t keyParam1Length, const uint8_t *keyParam2,
			    uint32_t keyParam2Length)
{
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *pkey = NULL;
	static const EVP_MD *evp_md;
	RSA *rkey = NULL; /* The pubkey is converted to this RSA public key. */

	unsigned char *out = NULL;
	size_t outlen = 0;
	uint32_t cipherCalLength = 0;
	int ret = 0;

	LOG(LOG_DEBUG, "rsa_encrypt starting.\n");

	/* Make sure we have a correct type of key. */
	if (keyEncoding != SDO_CRYPTO_PUB_KEY_ENCODING_RSA_MOD_EXP ||
	    keyAlgorithm != SDO_CRYPTO_PUB_KEY_ALGO_RSA) {
		LOG(LOG_ERROR, "Incorrect key type.\n");
		return -1;
	}

	if (NULL == clearText || 0 == clearTextLength) {
		LOG(LOG_ERROR, "Incorrect input text.\n");
		return -1;
	}
	if (keyParam1 == NULL || keyParam1Length == 0) {
		LOG(LOG_ERROR, "Missing Key1.\n");
		return -1;
	}
	if (keyParam2 == NULL || keyParam2Length == 0) {
		LOG(LOG_ERROR, "Missing Key2.\n");
		return -1;
	}

	/* Convert the representation to an RSA key. */
	if (convert2pkey(&pkey, &rkey, keyParam1, keyParam1Length, keyParam2,
			 keyParam2Length) != 0) {
		LOG(LOG_ERROR,
		    "Cannot convert public key to OpenSSL EVP_PKEY.\n ");
		return -1;
	}

	LOG(LOG_DEBUG, "Public key converted to rkey & pkey.\n");
	if (rkey)
		cipherCalLength = RSA_size(rkey);

	/* send back required cipher budffer size */
	if (cipherText == NULL) {
		RSA_free(rkey);
		EVP_PKEY_free(pkey);
		return cipherCalLength;
	}

	/*When caller sends cipher buffer */
	if (cipherCalLength > cipherTextLength)
		return -1;

	uint8_t encrypt[RSA_size(rkey)];
	int encrypt_len = 0;
	char err[130] = {0};
	switch (hashType) {
	case SDO_PK_HASH_SHA1:
		if ((encrypt_len = RSA_public_encrypt(
			 clearTextLength, (unsigned char *)clearText, encrypt,
			 rkey, RSA_PKCS1_OAEP_PADDING)) == -1) {
			ERR_load_crypto_strings();
			ERR_error_string(ERR_get_error(), err);
			LOG(LOG_ERROR, "Error encrypting message: %s.\n", err);
			ret = -1;
			goto error;
		}

		break;
	case SDO_PK_HASH_SHA384:
		evp_md = EVP_sha384();
	case SDO_PK_HASH_SHA256:
		/*
		 * if evp_md is initialized then its SHA284, proceed else if
		 * evp_md is NULL then its SHA256, initialize with EVP_sha256
		 */
		if (evp_md == NULL)
			evp_md = EVP_sha256();

		ctx = EVP_PKEY_CTX_new(pkey, NULL);
		if (!ctx) {
			/* Error */
			LOG(LOG_ERROR, "Unable to get the PKEY context\n");
			goto error;
		}
		if (EVP_PKEY_encrypt_init(ctx) <= 0) {
			/* Error */
			LOG(LOG_ERROR, "PKEYencrypt init failed\n");
			goto error;
		}
		EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING);
		EVP_PKEY_CTX_set_rsa_oaep_md(ctx, evp_md);
		EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, evp_md);
		/* Determine the length of buffer */
		if (EVP_PKEY_encrypt(ctx, NULL, &outlen,
				     (unsigned char *)clearText,
				     clearTextLength) <= 0) {
			/* Error	*/
			LOG(LOG_ERROR, "Error in PKEY encrypt\n");
			ret = -1;
			goto error;
		}
		out = OPENSSL_malloc(outlen);
		if (!out) {
			/* malloc failure */
			LOG(LOG_ERROR, "Malloc error\n");
			ret = -1;
			goto error;
		}
		if (EVP_PKEY_encrypt(ctx, out, &outlen,
				     (unsigned char *)clearText,
				     clearTextLength) <= 0) {
			/* Error */
			LOG(LOG_ERROR, "PKEY encrypt failed\n");
			ret = -1;
			goto error;
		}
		break;
	default:
		ret = -1;
		goto error;
	}
	LOG(LOG_DEBUG, "rsa_encrypt, encrypt_len : %d.\n", encrypt_len);
	/* Copy to the ecrypted buffer */
	if (memcpy_s(cipherText, cipherCalLength, (char *)out,
		     cipherCalLength) != 0) {
		LOG(LOG_ERROR, "Memcpy failed for rsa encrypted msg \n");
		ret = -1;
	}
error:
	if (ctx)
		EVP_PKEY_CTX_free(ctx);
	if (pkey)
		EVP_PKEY_free(pkey);
	if (out)
		OPENSSL_free(out);
	if (rkey)
		RSA_free(rkey);

	return ret;
}

/**
 * sdoCryptoRSALen - Returns the cipher length
 * @param keyParam1 - pointer of type uint8_t, holds the public key1.
 * @param keyParam1Length - size of public key1, type size_t.
 * @param keyParam2 - pointer of type uint8_t,holds the public key2.
 * @param keyParam2Length - size of public key2, type size_t
 * @return ret
 *        return cypherLength in bytes else 0 on failure.
 */
uint32_t sdoCryptoRSALen(const uint8_t *keyParam1, uint32_t keyParam1Length,
			 const uint8_t *keyParam2, uint32_t keyParam2Length)
{
	EVP_PKEY *pkey = NULL;
	RSA *rkey = NULL; /* The pubkey is converted to this RSA public key. */
	uint32_t cipherCalLength = 0;

	if ((NULL != keyParam1) && (0 != keyParam1Length) &&
	    (NULL != keyParam2) && (0 != keyParam2Length)) {
		LOG(LOG_ERROR, "Invalid parameters.\n ");
		return 0;
	}
	/* Convert the representation to an RSA key. */
	if (convert2pkey(&pkey, &rkey, keyParam1, keyParam1Length, keyParam2,
			 keyParam2Length) != 0) {
		LOG(LOG_ERROR,
		    "Cannot convert public key to OpenSSL EVP_PKEY.\n ");
		goto err;
	}

	if (NULL != rkey) {
		cipherCalLength = RSA_size(rkey);
	}

err:
	RSA_free(rkey);
	EVP_PKEY_free(pkey);
	/* send back required cipher budffer size */
	return cipherCalLength;
}
