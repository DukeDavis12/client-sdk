/*
 * Copyright 2020 Intel Corporation
 * SPDX-License-Identifier: Apache 2.0
 */

/*!
 * \file
 * \brief EPID SDK library Abstraction Layer. Has wrapper APIs epid init/sign.
 */

/* this api.h MUST be before others because it insists on defining bool */
#if (defined EPID_R6 || defined EPID_TINY)
#include "epid/member/src/write_precomp.h"
#include "epid/common/stdtypes.h"
#include "epid/member/software_member.h"
#include "epid/common/bitsupplier.h"
#include "epid/common/types.h"
#include "ext/ipp/include/ippcp.h"
#include "epid/common/src/memory.h"
#ifdef TARGET_OS_FREERTOS
#include "esp_clk.h"
#endif
#endif
#include "epid/member/api.h"
#include "epid/common/file_parser.h"
#include "epid.h"
#include "sdoCryptoHal.h"
#include "util.h"
#include <stdlib.h>
#include "safe_lib.h"

/* Show if the initialization has been completed */
static bool g_epidInitialized = false;

/* CA certificate */
static EpidCaCertificate g_cacert = {0};

static GroupPubKey g_groupPublicKey = {0};

static PrivKey g_priv_key = {0};

/* EPID member context */
static MemberCtx *g_member_ctx = NULL;

/* Member pre-computed settings */
static MemberPrecomp g_member_precomp = {0};
static bool is_precompute_available = false;

/* Hash algorithm */
static HashAlg g_hashalg = kSha256;

/**
 * Verify that CaCert is valid
 *
 * @param data
 *        Input CA data.
 * @param size
 *        Input CA data size
 * @return bool
 *        return true on success. false value on failure.
 */
bool IsCaCertAuthorizedByRootCa(void const *data, size_t size)
{
	/*
	 * TODO: Implementation of this function is out of scope of the sample.
	 * In an actual implementation Issuing CA certificate must be validated
	 * with CA Root certificate before using it in parse functions.
	 */
	(void)data;
	(void)size;
	return true;
}

/**
 * EPID requires an RNG function with this prototype.
 *
 * @param rand_data
 *        Output buffer to load random data.
 * @param num_bits
 *        Number of bits of Random value.
 * @param user_data
 *        Used as entropy to generate random data.
 * @return ret
 *        return 0 on success. -ve value on failure.
 */
int __STDCALL epid_prng_gen(unsigned int *rand_data, int num_bits,
			    void *user_data)
{
	return _sdoCryptoRandomBytes((uint8_t *)rand_data, num_bits / 8);
}

#if (defined EPID_R6 || defined EPID_TINY)
void SetMemberParams(BitSupplier rnd_func, void *rnd_param, const FpElemStr *f,
		     MemberParams *params)
{
#ifdef TPM_TSS
	(void)rnd_func;
	(void)rnd_param;
	params->f = f;
#else
	params->rnd_func = rnd_func;
	params->rnd_param = rnd_param;
	params->f = f;
#endif
}

EpidStatus epid_r6_init(void)
{
	EpidStatus sts = kEpidErr;
	MemberParams params = {
	    0,
	};
	size_t member_size = 0;
	char myctx[] = "None";

	SetMemberParams(&epid_prng_gen, myctx, NULL, &params);
	// create member
	sts = EpidMemberGetSize(&params, &member_size);
	if (kEpidNoErr != sts) {
		goto err2;
	}

	g_member_ctx = (MemberCtx *)EpidAlloc(member_size);
	if (!g_member_ctx) {
		sts = kEpidNoMemErr;
		goto err2;
	}

	sts = EpidMemberInit(&params, g_member_ctx);
	if (kEpidNoErr != sts) {
		goto err2;
	}

	sts = EpidMemberSetHashAlg(g_member_ctx, g_hashalg);
	if (kEpidNoErr != sts) {
		goto err2;
	}

	sts = EpidProvisionKey(g_member_ctx, &g_groupPublicKey, &g_priv_key,
			       is_precompute_available ? &g_member_precomp
						       : NULL);
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "EPID provisioning failed (%d)\n", (int)sts);
		goto err2;
	}

	sts = EpidMemberStartup(g_member_ctx);
	if (kEpidNoErr != sts) {
		goto err2;
	}

#if !defined(EPID_TINY) && defined(TARGET_OS_FREERTOS)
	/* return member pre-computation blob if requested */
	if (!is_precompute_available) {
		sts = EpidMemberWritePrecomp(g_member_ctx, &g_member_precomp);
		if (kEpidNoErr != sts) {
			LOG(LOG_ERROR,
			    "Could not write Epid Member precomp. sts: %d\n",
			    sts);
			goto err2;
		}
		is_precompute_available = true;
	}
#endif

	return sts;

err2:
	EPID_Close();
	return sts;
}
#endif

/**
 * Initialize EPID engine by supplying it EPID Group ID, Private Key,
 * Certificate,
 *
 * @param signedGroupPublicKey
 *        Public key of the Group to which the private key belongs.
 * @param signedGroupPublicKeyLen
 *        Group public key length.
 * @param privateKey
 *        Private key of the device.
 * @param privateKeyLen
 *        Length of private key of the device.
 * @param cacert_data
 *        Pointer to certificate data corresponding to public key.
 * @param cacert_size
 *        Length of certificate data.
 * @param signed_sig_rl
 *        List of signature whose private keys have been revoked.
 * @param signed_sig_rl_size
 *        Number of signatures that are revoked.
 * @param sdoprecomp_data
 *        Pre computed basic attestation of group membership, without
 * considering revocation lists. This is to reduce runtime computation which may
 * not be possible on low MIPS MCUs.
 * @param precomp_size
 *        Size of precomputed data.
 * @return ret
 *        return 0 on success. -ve value on failure.
 */
int EPID_Init(const uint8_t *signedGroupPublicKey,
	      size_t signedGroupPublicKeyLen, const uint8_t *privateKey,
	      size_t privateKeyLen, const uint8_t *cacert_data,
	      size_t cacert_size, const uint8_t *signed_sig_rl,
	      size_t signed_sig_rl_size, const uint8_t *sdoprecomp_data,
	      size_t precomp_size)
{
	EpidStatus sts = kEpidErr;

	/*
	 * Read in the EPID group public key, private key, SigRl, and cacert
	 * in a real product the private key and cacert data would be in the
	 * TEE, the Public key would come from the OProxy
	 * the sigrl we would load from a network resource for our group
	 */

	LOG(LOG_DEBUG, "\n++++EPID Initializing++++\n");

	/* CA certificate */
	if (cacert_data) {
		if (cacert_size != sizeof(EpidCaCertificate)) {
			LOG(LOG_ERROR, "cacert size mismatch\n");
			return -1;
		}
		if (memcpy_s(&g_cacert, cacert_size,
			     (EpidCaCertificate *)cacert_data,
			     cacert_size) != 0) {
			LOG(LOG_ERROR, "Memcpy failed\n");
			return -1;
		}
	}

	/* Security note:
	 * Application must confirm that IoT EPID Issuing CA certificate is
	 * authorized by IoT EPID Root CA, e.g., signed by IoT EPID Root CA.
	 */
	if (!IsCaCertAuthorizedByRootCa(&g_cacert, sizeof(g_cacert))) {
		LOG(LOG_ERROR, "CA certificate is not authorized\n");
		return -1;
	}

	/* SigRl */
	LOG(LOG_DEBUG, "signed_sig_rl_size %d\n", (int)signed_sig_rl_size);

	/* Group public key file */
	LOG(LOG_DEBUG, "signedGroupPublicKeyLen %d\n",
	    (int)signedGroupPublicKeyLen);
	if (signedGroupPublicKeyLen && signedGroupPublicKey) {
		if (memcpy_s(&g_groupPublicKey, sizeof(GroupPubKey),
			     (GroupPubKey *)signedGroupPublicKey,
			     signedGroupPublicKeyLen) != 0) {
			LOG(LOG_ERROR, "Memcpy failed\n");
			return -1;
		}
	}

	/* Member private key */
	if (privateKey) {
		LOG(LOG_DEBUG, "Private Key read\n");

		/* decompress private key if needed */
		if (privateKeyLen == sizeof(PrivKey)) {
			/* no decompression needed, use as is */
			if (memcpy_s(&g_priv_key, sizeof(PrivKey),
				     (PrivKey *)privateKey,
				     sizeof(PrivKey)) != 0) {
				LOG(LOG_ERROR, "Memcpy failed\n");
				return -1;
			}

		} else if (privateKeyLen == sizeof(CompressedPrivKey)) {
			/* Compressed key needs decompression */
			sts = EpidDecompressPrivKey(
			    &g_groupPublicKey, (CompressedPrivKey *)privateKey,
			    &g_priv_key);
			if (kEpidNoErr != sts) {
				LOG(LOG_ERROR,
				    "Error decompressing "
				    "Private Key.  sts: "
				    "%d\n",
				    (int)sts);
				return -1;

			} else {
				LOG(LOG_DEBUG, "Private Key decompressed\n");
			}
		} else {
			LOG(LOG_ERROR, "Private Key size incorrect\n");
			return -1;
		}
		LOG(LOG_DEBUG, "PrivateKey ready, size:%lu\n", sizeof(PrivKey));
		/* hexdump("Private Key", &g_priv_key, sizeof(PrivKey)); */
	}

#if (defined EPID_R6 || defined EPID_TINY) && defined(TARGET_OS_FREERTOS)
	sts = epid_r6_init();
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "Failed to init R6 EPID (%d)\n", sts);
		return -1;
	}
	EpidMemberDelete(&g_member_ctx);
#else
	/* acquire PRNG */
	int ret;
	ret = random_init();
	if (ret != 0) {
		LOG(LOG_ERROR, "Could not create PRNG\n");
		return ret;
	} else
		LOG(LOG_DEBUG, "PRNG Created\n");
#endif

	LOG(LOG_DEBUG, "Keys have been set. pub:%lu bytes, priv:%lu bytes\n",
	    sizeof(g_groupPublicKey), privateKeyLen);
	LOG(LOG_DEBUG, "Hash algorithm: %s\n",
	    (g_hashalg == kSha512) ? "SHA-512" : "SHA-256");
	LOG(LOG_DEBUG, "++++EPID Initialized++++\n\n");

	g_epidInitialized = true;
	return 0;
}

/**
 * Internal API
 */

/**
 * Release EPID engine resources.
 */
void EPID_Close(void)
{
	if (!g_epidInitialized)
		return;
	g_epidInitialized = false;
#if (!defined EPID_R6 && !defined EPID_TINY)
	random_close();
#endif
	EpidMemberDelete(&g_member_ctx);

	/* Note:
	 * EPID_Close() is last call in sequence,
	 * thus memset failure does not return
	 */
	if (memset_s(&g_priv_key, sizeof(PrivKey), 0))
		LOG(LOG_ERROR, "Failed to clear g_priv_key\n");
	g_member_ctx = NULL;
	is_precompute_available = false;
}

/**
 * EPID_Sign - sign the buffer passed
 * When we get here EPID_init must have been called before to setup our
 * calculation values
 * @param data
 *        pointer to the data buffer to sign, data is assumed to be
 *        binary, no text assumption
 * @param data_len
 *        The number of 8 bit bytes of data to sign
 * @param bGroupPublicKey
 *        Group public key to which the private key of device belongs.
 * @param GroupPublicKeyLen
 *        Length of group public key.
 * @param bSigrl
 *        List of signatures of Private keys that are revoked in this group.
 * @param SigRlSize
 *        Number of signatures of Private keys that are revoked in this group.
 * @return ret
 *        The signature as a byte array
 */

#if (defined EPID_R6 || defined EPID_TINY)
SDOBits_t *EPID_Sign(uint8_t *data, size_t data_len,
		     const uint8_t *bGroupPublicKey, size_t GroupPublicKeyLen,
		     const uint8_t *bSigrl, size_t SigRlSize)
{
	EpidStatus sts = kEpidErr;
	EpidSignature *sig;
	SigRl *eBsigrl = NULL;
	SDOBits_t *sig_bits = NULL;
	size_t sig_size = 0;

	eBsigrl = (SigRl *)bSigrl;

	/* Sanity checks */
	if (!data || !data_len) {
		LOG(LOG_ERROR, "Invalid data input for EPID_Sign!\n");
		goto err1;
	}

	/* Do not run this if the initialization has not been completed */
	if (!g_epidInitialized) {
		LOG(LOG_ERROR, "Epid has not been initialized\n");
		goto err1;
	}

/*
 * In MCU, precompute is used which is relying on the public key stored
 * in the platform storage system. In Linux based platforms, public key
 * is used from eB info and pre-compute is not possible and not required
 * as the compute is sufficient to do signing fast enough
 */
#ifndef TARGET_OS_FREERTOS
	if (!GroupPublicKeyLen || !bGroupPublicKey) {
		goto err1;
	}

	if (memcpy_s(&g_groupPublicKey, sizeof(GroupPubKey),
		     (GroupPubKey *)bGroupPublicKey, GroupPublicKeyLen) != 0) {
		LOG(LOG_ERROR, "Publicj key memcpy failed\n");
		goto err1;
	}
#endif

	LOG(LOG_DEBUG, "EPID_sign:  SigRl size %d, pubkeysz %d\n",
	    (int)SigRlSize, (int)GroupPublicKeyLen);

	sts = epid_r6_init();
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "Failed to init R6 EPID (%d)\n", (int)sts);
		goto err1;
	}

	if (SigRlSize) {
		sts = EpidMemberSetSigRl(g_member_ctx, eBsigrl, SigRlSize);
		if (kEpidNoErr != sts) {
			LOG(LOG_ERROR, "Setting Sigrl failed(%d)\n", (int)sts);
			goto err2;
		}
	}

	sig_size = EpidGetSigSize(eBsigrl);

	sig = sdoAlloc(sig_size);
	if (!sig) {
		LOG(LOG_ERROR, "Sig Malloc failed for EPID\n");
		goto err2;
	}

	// sign message
	sts = EpidSign(g_member_ctx, data, data_len, NULL, 0, sig, sig_size);
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "Data Signing failed(%d)\n", (int)sts);
		sdoFree(sig);
		goto err2;
	}

	/* Construct return object */
	sig_bits = sdoAlloc(sizeof(SDOBits_t));
	if (!sig_bits) {
		LOG(LOG_ERROR, "failed to create memory for signature bytes\n");
		sdoFree(sig);
		goto err2;
	}
	sig_bits->bytes = (uint8_t *)sig;
	sig_bits->byteSz = sig_size;

err2:
	EpidMemberDelete(&g_member_ctx);
err1:
	return sig_bits;
}
#else

SDOBits_t *EPID_Sign(uint8_t *data, size_t data_len,
		     const uint8_t *bGroupPublicKey, size_t GroupPublicKeyLen,
		     const uint8_t *bSigrl, size_t SigRlSize)
{
	EpidStatus sts = kEpidErr;
	EpidSignature *sig;
	SigRl *eBsigrl = NULL;
	GroupPubKey *PublicKey = (GroupPubKey *)bGroupPublicKey;
	MemberCtx *member = NULL;
	SDOBits_t *sig_bits = NULL;
	size_t sig_size = 0;

	/* intermediate return value for EPID functions */
	EpidStatus result = kEpidErr;

	/* Sanity checks */
	if (!data || !data_len) {
		LOG(LOG_ERROR, "Invalid data input for EPID_Sign!\n");
		return NULL;
	}

	eBsigrl = (SigRl *)bSigrl;

	LOG(LOG_DEBUG, "EPID_sign:  SigRl size %d, pubkeysz %d", (int)SigRlSize,
	    (int)GroupPublicKeyLen);

	/* Do not run this if the initialization has not been completed */
	if (!g_epidInitialized) {
		LOG(LOG_ERROR, "Epid has not been initialized\n");
		return NULL;
	}

	/* create member */
	sts = EpidMemberCreate(PublicKey, &g_priv_key, NULL, epid_prng_gen,
			       NULL, &member);
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR,
		    "Could not create Epid Member context. sts: %d\n", sts);
		return NULL;
	}

	/* return member pre-computation blob if requested */
	sts = EpidMemberWritePrecomp(member, &g_member_precomp);
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "Could not write Epid Member precomp. sts: %d\n",
		    sts);
		return NULL;
	}

	sts = EpidMemberSetHashAlg(member, g_hashalg);
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "Could not set Hash Alg. sts: %d\n", sts);
		return NULL;
	}

	/*
	 * Signature
	 * Note: Signature size must be computed after sig_rl is loaded.
	 */
	sig_size = EpidGetSigSize(eBsigrl);

	sig = malloc(sig_size);
	if (!sig) {
		LOG(LOG_ERROR,
		    "Could not allocate memory for epid "
		    "signature. sts: %d\n",
		    sts);
		return NULL;
	}

	/* sign message */
	sts = EpidSign(member, data, data_len, NULL, 0, eBsigrl, SigRlSize, sig,
		       sig_size);
	if (kEpidNoErr != sts) {
		LOG(LOG_ERROR, "Failed creating epid signature. sts: %d\n",
		    sts);
		sdoFree(sig);
		return NULL;
	}
	sts = kEpidNoErr;

	/* Report Result */
	if (kEpidNoErr != result) {
		if (kEpidSigRevokedInSigRl == result) {
			LOG(LOG_ERROR, "signature revoked in SigRL\n");
		}
	}

	/* Construct return object */
	sig_bits = malloc(sizeof(SDOBits_t));
	if (!sig_bits) {
		LOG(LOG_ERROR, "failed to create memory for signature bytes\n");
		sdoFree(sig);
		return NULL;
	}
	sig_bits->bytes = (uint8_t *)sig;
	sig_bits->byteSz = sig_size;

	EpidMemberDelete(&member);
	return sig_bits;
}
#endif
