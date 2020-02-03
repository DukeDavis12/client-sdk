/*
 * Copyright 2020 Intel Corporation
 * SPDX-License-Identifier: Apache 2.0
 */

/*
 * Abstraction Layer Library
 *
 * The file implements an abstraction layer for mbedos cortex M boards .
 */

#include "util.h"
#include "network_al.h"
#include "sdoCryptoHal.h"
#include "sdoprotctx.h"
#include "sdonet.h"
#include "safe_lib.h"
#include "snprintf_s.h"
#include "rest_interface.h"
#include "def.h"
#include "mbed_wait_api.h"
#include "platform/mbed_thread.h"
#include <lwip/ip4_addr.h>
#include <lwip/sockets.h>

/**
 * Read from socket until new-line is encountered.
 *
 * @param handle - socket-handle.
 * @param out -  out pointer for REST header line.
 * @param size - out REST header line length.
 * @param ssl -  SSL pointer if TLS is active
 * @retval true if line read was successful, false otherwise.
 */
static bool readUntilNewLine(sdoConHandle handle, char *out, size_t size,
			     void *ssl)
{
	size_t sz = 0;
	int n = 0;
	char c = 0;

	if (!out || !size)
		return false;

	--size; // leave room for NULL
	sz = 0;
	for (;;) {

		if (ssl)
			n = sdo_ssl_read(ssl, (uint8_t *)&c, 1);
		else
			n = mos_socketRecv(handle, (uint8_t *)&c, 1, 0);

		if (n <= 0) {
			LOG(LOG_ERROR,
			    "Socket Read Failed, ret=%d, "
			    "errno=%d, %d\n",
			    n, errno, __LINE__);
			return false;
		}
		if (sz < size)
			out[sz++] = c;

		if (c == '\n')
			break;
	}
	out[sz] = 0;
	/* remove \n and \r and don't process invalid string */
	if ((sz < size) && (sz >= 1)) {
		out[--sz] = 0; // remove NL
		if ((sz >= 1) && (out[sz - 1] == '\r'))
			out[--sz] = 0; // ... remove CRNL
	}
	return true;
}

/**
 * sdoConSetup Connection Setup.
 *
 * @param medium - specified network medium to connect to
 * @param params - parameters(if any) supported for 'medium'
 * @param count - number of valid string in params
 * @return 0 on success. -1 on failure
 */
int32_t sdoConSetup(char *medium, char **params, uint32_t count)
{
	/*TODO: make use of input params (if required ?)*/
	(void)medium;
	(void)params;
	(void)count;

	// Initiate REST context
	if (!initRESTContext()) {
		LOG(LOG_ERROR, "initRESTContext() failed!\n");
		return -1;
	}
	return 0;
}

/**
 * Perform a DNS look for a specified host.
 *
 * @param url - host's URL.
 * @param ipList - output IP address list for specified host URL.
 * @param ipListSize - output number of IP address in ipList
 * @retval -1 on failure, 0 on success.
 */
int32_t sdoConDnsLookup(const char *url, SDOIPAddress_t **ipList,
			uint32_t *ipListSize)
{
	int ret = -1;
	SDOIPAddress_t *sdotmpip = NULL;
	uint8_t resolved_ip[IP_TAG_LEN] = {0};
	if (!url || !ipList || !ipListSize) {
		LOG(LOG_ERROR, "Bad parameters received\n");
		goto err;
	}

	sdotmpip = (SDOIPAddress_t *)sdoAlloc(sizeof(SDOIPAddress_t));
	if (!sdotmpip) {
		LOG(LOG_ERROR, "Out of memory for ipList\n");
		goto err;
	}

	if (mos_resolvedns((char *)url, (char *)resolved_ip) != 0) {
		LOG(LOG_ERROR, "mos_resolve dns faileds\n");
		goto err;
	}
	sdotmpip->length = IPV4_ADDR_LEN;

	if (sdoPrintableToNet((const char *)resolved_ip,
			      (void *)sdotmpip->addr) != 1) {
		LOG(LOG_ERROR, "ascii ip to bin conversion failed\n");
		goto err;
	}

	*ipList = (SDOIPAddress_t *)sdotmpip;
	*ipListSize = 1;

	ret = 0;
err:
	return ret;
}

/**
 * sdoConConnect connects to the network socket
 *
 * @param ip_addr - pointer to IP address info
 * @param port - port number to connect
 * @param ssl - ssl handler in case of tls connection.
 * @return connection handle on success. -ve value on failure
 */

sdoConHandle sdoConConnect(SDOIPAddress_t *ip_addr, uint16_t port, void **ssl)
{
	sdoConHandle sock = SDO_CON_INVALID_HANDLE;
	char *ipv4 = NULL;
	char port_s[MAX_PORT_SIZE] = {0};

	if (!ip_addr)
		goto end;
	if (ssl) {
		/*
		 * Convert ip binary to string format as required by
		 * mbedtls ssl connect
		 */
		if ((ipv4 = ip4addr_ntoa((const void *)ip_addr->addr)) ==
		    NULL) {
			LOG(LOG_ERROR, "net to ascii ip failed failed!\n");
			goto end;
		}

		if (snprintf_s_i(port_s, sizeof port_s, "%d", port) < 0) {
			LOG(LOG_ERROR, "Snprintf() failed!\n");
			goto end;
		}

		*ssl = sdo_ssl_setup_connect(ipv4, port_s);

		if (NULL == *ssl) {
			LOG(LOG_ERROR, "TLS connection "
				       "setup "
				       "failed\n");
			goto end;
		}
		sock = (sdoConHandle *)get_ssl_socket();
		return sock;
	}
	sock = mos_socketConnect(ip_addr, port);

end:
	return sock;
}

/**
 * Disconnect the connection for a given connection handle.
 *
 * @param handle - connection handler (for ex: socket-id)
 * @param ssl - SSL handler in case of tls connection.
 * @retval -1 on failure, 0 on success.
 */
int32_t sdoConDisconnect(sdoConHandle handle, void *ssl)
{
	if (ssl) // SSL disconnect
		sdo_ssl_close(ssl);

	mos_socketClose(handle);
	return 0;
}

/**
 * Receive(read) protocol version, message type and length of rest body
 *
 * @param handle - connection handler (for ex: socket-id)
 * @param protocolVersion - out SDO protocol version
 * @param messageType - out message type of incoming SDO message.
 * @param msglen - out Number of received bytes.
 * @param ssl - handler in case of tls connection.
 * @retval -1 on failure, 0 on success.
 */
int32_t sdoConRecvMsgHeader(sdoConHandle handle, uint32_t *protocolVersion,
			    uint32_t *messageType, uint32_t *msglen, void *ssl)
{
	int32_t ret = -1;
	char hdr[REST_MAX_MSGHDR_SIZE] = {0};
	char tmp[REST_MAX_MSGHDR_SIZE];
	size_t hdrlen;
	RestCtx_t *rest = NULL;

	if (!protocolVersion || !messageType || !msglen)
		goto err;

	// read REST header
	for (;;) {
		if (memset_s(tmp, sizeof(tmp), 0) != 0) {
			LOG(LOG_ERROR, "Memset() failed!\n");
			goto err;
		}

		if (!readUntilNewLine(handle, tmp, REST_MAX_MSGHDR_SIZE, ssl)) {
			LOG(LOG_ERROR, "readUntilNewLine() failed!\n");
			goto err;
		}

		// end of header
		if (tmp[0] == getRESTHdrBodySeparator())
			break;

		// accumulate header content
		if (strncat_s(hdr, REST_MAX_MSGHDR_SIZE, tmp,
			      strnlen_s(tmp, REST_MAX_MSGHDR_SIZE)) != 0) {
			LOG(LOG_ERROR, "Strcat() failed!\n");
			goto err;
		}

		// append new line for convenient parsing in REST
		if (strcat_s(hdr, REST_MAX_MSGHDR_SIZE, "\n") != 0) {
			LOG(LOG_ERROR, "Strcat() failed!\n");
			goto err;
		}
	}

	hdrlen = strnlen_s(hdr, REST_MAX_MSGHDR_SIZE);

	/* Process REST header and get content-length of body */
	if (!getRESTContentLength(hdr, hdrlen, msglen)) {
		LOG(LOG_ERROR, "REST Header processing failed!!\n");
		goto err;
	}

	rest = getRESTContext();
	if (!rest) {
		LOG(LOG_ERROR, "REST context is NULL!\n");
		goto err;
	}

	// copy protver from REST context
	*protocolVersion = rest->protVer;
	*messageType = rest->msgType;

	ret = 0;

err:
	return ret;
}

/**
 * Receive(read) MsgBody
 *
 * @param handle - connection handler (for ex: socket-id)
 * @param buf - data buffer to read into.
 * @param length - Number of received bytes.
 * @param ssl - handler in case of tls connection.
 * @retval -1 on failure, number of bytes read on success.
 */
int32_t sdoConRecvMsgBody(sdoConHandle handle, uint8_t *buf, size_t length,
			  void *ssl)
{
	int n = 0;
	int32_t ret = -1;
	uint8_t *bufp = buf;
	int sz = length;

	if (!buf || !length)
		goto err;
	do {
		bufp = bufp + n;
		if (ssl)
			n = sdo_ssl_read(ssl, bufp, sz);
		else
			n = mos_socketRecv(handle, bufp, sz, 0);

		LOG(LOG_DEBUG, "Expected %d , got %d\n", sz, n);
		sz = sz - n;
		if (n < 0) {
			ret = -1;
			goto err;
		}
	} while (sz > 0);
	ret = length;
err:
	return ret;
}

/**
 * Send(write) data.
 *
 * @param handle - connection handler (for ex: socket-id)
 * @param protocolVersion - SDO protocol version
 * @param messageType - message type of outgoing SDO message.
 * @param buf - data buffer to write from.
 * @param length - Number of sent bytes.
 * @param ssl - handler in case of tls connection.
 * @retval -1 on failure, number of bytes written.
 */
int32_t sdoConSendMessage(sdoConHandle handle, uint32_t protocolVersion,
			  uint32_t messageType, const uint8_t *buf,
			  size_t length, void *ssl)
{
	int ret = -1;
	int n;
	RestCtx_t *rest = NULL;
	char restHdr[REST_MAX_MSGHDR_SIZE] = {0};
	size_t headerLen = 0;

	if (!buf || !length)
		goto err;

	rest = getRESTContext();

	if (!rest) {
		LOG(LOG_ERROR, "REST context is NULL!\n");
		goto err;
	}

	// supply info to REST for POST-URL construction
	rest->protVer = protocolVersion;
	rest->msgType = messageType;
	rest->contentLength = length;

	if (!constructRESTHeader(rest, restHdr, REST_MAX_MSGHDR_SIZE)) {
		LOG(LOG_ERROR, "Error during constrcution of REST hdr!\n");
		goto err;
	}

	headerLen = strnlen_s(restHdr, SDO_MAX_STR_SIZE);

	if (!headerLen || headerLen == SDO_MAX_STR_SIZE) {
		LOG(LOG_ERROR, "Strlen() failed!\n");
		goto err;
	}

	/* Send REST header */
	if (ssl) {
		n = sdo_ssl_write(ssl, restHdr, headerLen);

		if (n < 0) {
			LOG(LOG_ERROR, "SSL Header write Failed!\n");
			goto hdrerr;
		}
	} else {
		n = mos_socketSend(handle, restHdr, headerLen, 0);

		if (n <= 0) {
			LOG(LOG_ERROR,
			    "Socket write Failed, ret=%d, "
			    "errno=%d, %d\n",
			    n, errno, __LINE__);

			if (sdoConDisconnect(handle, ssl)) {
				LOG(LOG_ERROR, "Error during socket close()\n");
				goto hdrerr;
			}
			goto hdrerr;

		} else if (n < (int)headerLen) {
			LOG(LOG_ERROR,
			    "Rest Header write returns %d/%u bytes\n", n,
			    headerLen);
			goto hdrerr;

		} else
			LOG(LOG_DEBUG,
			    "Rest Header write returns %d/%u bytes\n\n", n,
			    headerLen);
	}

	LOG(LOG_DEBUG, "REST:header(%u):%s\n", headerLen, restHdr);

	/* Send REST body */
	if (ssl) {
		n = sdo_ssl_write(ssl, buf, length);
		if (n < 0) {
			LOG(LOG_ERROR, "SSL Body write Failed!\n");
			goto bodyerr;
		}
	} else {
		n = mos_socketSend(handle, (void *)buf, length, 0);

		if (n <= 0) {
			LOG(LOG_ERROR,
			    "Socket write Failed, ret=%d, "
			    "errno=%d, %d\n",
			    n, errno, __LINE__);

			if (sdoConDisconnect(handle, ssl)) {
				LOG(LOG_ERROR, "Error during socket close()\n");
				goto bodyerr;
			}
			goto bodyerr;

		} else if (n < (int)length) {
			LOG(LOG_ERROR, "Rest Body write returns %d/%u bytes\n",
			    n, length);
			goto bodyerr;

		} else
			LOG(LOG_DEBUG,
			    "Rest Body write returns %d/%u bytes\n\n", n,
			    length);
	}

	return n;

hdrerr:
	LOG(LOG_ERROR, "REST Header write not successful!\n");
	goto err;
bodyerr:
	LOG(LOG_ERROR, "REST Body write not successful!\n");
err:
	return ret;
}

/**
 * sdoConTearDown connection tear-down.
 *
 * @return 0 on success, -1 on failure
 */
int32_t sdoConTeardown(void)
{
	/* REST context over */
	exitRESTContext();
	return 0;
}

/**
 * Put the SDO device to low power state
 *
 * @param sec
 *        number of seconds to put the device to low power state
 *
 * @return none
 */
void sdoSleep(int sec)
{
	thread_sleep_for(sec * 1000);
}

/**
 * Convert from Network to Host byte order
 *
 * @param value
 *        Number in network byte order.
 *
 * @return
 *         Value in Host byte order.
 */
uint32_t sdoNetToHostLong(uint32_t value)
{
	return ntohl(value);
}

/**
 * Convert from Host to Network byte order
 *
 * @param value
 *         Value in Host byte order.
 *
 * @return
 *        Number in network byte order.
 */
uint32_t sdoHostToNetLong(uint32_t value)
{
	return htonl(value);
}

/**
 * Convert from ASCII to string format
 *
 * @param src
 *         Source address in ASCII format.
 * @param addr
 *         Source address in string.
 *
 * @return
 *        1 on success. -1 on error. 0 if input format is invalid
 */
int32_t sdoPrintableToNet(const char *src, void *addr)
{
	return ip4addr_aton(src, addr);
}

/**
 * get device model
 *
 * @return
 *        returns model as string
 */
const char *get_device_model(void)
{
	return "Intel-SDO-f32m7";
}

/**
 *  get device serial number
 *
 * @return
 *        returns device serial number as string.
 */
const char *get_device_serial_number(void)
{
	return "sdo-f32m7-1234";
}

/**
 * sdo_random generates random number and returns
 *
 * Note: this is only to be used for calculating random
 * network delay for retransmissions and NOT for crypto

 * @return
 *        returns random number
 */
int sdoRandom(void)
{
	return rand();
}
