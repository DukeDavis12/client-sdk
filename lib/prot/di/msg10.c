/*
 * Copyright 2020 Intel Corporation
 * SPDX-License-Identifier: Apache 2.0
 */

/*!
 * \file
 * \brief Implementation of msg number 10; first step of Device Initialize
 * Protocol
 */

#include "util.h"
#include "sdoprot.h"

/* TODO: Move m-string generation here */

/**
 * msg10() - DI.AppStart
 * This is the beginning of state machine for ownership transfer of device.The
 * device prepares the "m" string to communicate with the manufacturer, so, it
 * gets the first ownership voucher after Device Initialize (DI) stage is
 * complete.
 *
 * Message format (String defintion is given as part of "m" string generation)
 * ---------------------------------------------------------------------------
 * {
 *    "m": String
 * }
 */
int32_t msg10(SDOProt_t *ps)
{
	int ret = -1;

	/* Start the "m" string */
	sdoWNextBlock(&ps->sdow, SDO_DI_APP_START);
	sdoWBeginObject(&ps->sdow);
	sdoWriteTag(&ps->sdow, "m");

#if !defined(DEVICE_TPM20_ENABLED)
	/* Get the m-string in the ps object */
	ret = ps_get_m_string(ps);
	if (ret) {
		LOG(LOG_ERROR, "Failed to get m-string in ps\n");
		goto err;
	}
#else
	SDOByteArray_t *mstring = NULL;
	int mstring_size = get_file_size(DEVICE_MSTRING);

	mstring = sdoByteArrayAlloc(mstring_size + 1);
	if (NULL == mstring) {
		LOG(LOG_ERROR,
		    "Failed to allocate memory for device mstring.\n");
		goto err;
	}

	if (0 != read_buffer_from_file(DEVICE_MSTRING, mstring->bytes,
				       mstring_size)) {
		LOG(LOG_ERROR, "Failed to read %s file!\n", DEVICE_MSTRING);
		sdoByteArrayFree(mstring);
		goto err;
	}

	LOG(LOG_DEBUG, "csr content start: \n%s\ncsr content end\n",
	    mstring->bytes);
	sdoWriteString(&ps->sdow, (char *)mstring->bytes);
	sdoByteArrayFree(mstring);
#endif

	/* End the object */
	sdoWEndObject(&ps->sdow);

	/* This state manages the transition to the next protocol message */
	ps->state = SDO_STATE_DI_SET_CREDENTIALS;
	ret = 0;

err:
	return ret;
}
