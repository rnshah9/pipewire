/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef AVB_ACMP_H
#define AVB_ACMP_H

#include "packets.h"
#include "internal.h"

#define AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND		0
#define AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE		1
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND		2
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE		3
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND		4
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE		5
#define AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND		6
#define AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE		7
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND		8
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE		9
#define AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND		10
#define AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE		11
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND		12
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE	13

#define AVB_ACMP_STATUS_SUCCESS				0
#define AVB_ACMP_STATUS_LISTENER_UNKNOWN_ID		1
#define AVB_ACMP_STATUS_TALKER_UNKNOWN_ID		2
#define AVB_ACMP_STATUS_TALKER_DEST_MAC_FAIL		3
#define AVB_ACMP_STATUS_TALKER_NO_STREAM_INDEX		4
#define AVB_ACMP_STATUS_TALKER_NO_BANDWIDTH		5
#define AVB_ACMP_STATUS_TALKER_EXCLUSIVE		6
#define AVB_ACMP_STATUS_LISTENER_TALKER_TIMEOUT		7
#define AVB_ACMP_STATUS_LISTENER_EXCLUSIVE		8
#define AVB_ACMP_STATUS_STATE_UNAVAILABLE		9
#define AVB_ACMP_STATUS_NOT_CONNECTED			10
#define AVB_ACMP_STATUS_NO_SUCH_CONNECTION		11
#define AVB_ACMP_STATUS_COULD_NOT_SEND_MESSAGE		12
#define AVB_ACMP_STATUS_TALKER_MISBEHAVING		13
#define AVB_ACMP_STATUS_LISTENER_MISBEHAVING		14
#define AVB_ACMP_STATUS_RESERVED			15
#define AVB_ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED	16
#define AVB_ACMP_STATUS_INCOMPATIBLE_REQUEST		17
#define AVB_ACMP_STATUS_LISTENER_INVALID_CONNECTION	18
#define AVB_ACMP_STATUS_NOT_SUPPORTED			31

#define AVB_ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS		2000
#define AVB_ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS	200
#define AVB_ACMP_TIMEOUT_GET_TX_STATE_COMMAND		200
#define AVB_ACMP_TIMEOUT_CONNECT_RX_COMMAND_MS		4500
#define AVB_ACMP_TIMEOUT_DISCONNECT_RX_COMMAND_MS	500
#define AVB_ACMP_TIMEOUT_GET_RX_STATE_COMMAND_MS	200
#define AVB_ACMP_TIMEOUT_GET_TX_CONNECTION_COMMAND	200

struct avb_packet_acmp {
	struct avb_packet_header hdr;
	uint64_t stream_id;
	uint64_t controller_guid;
	uint64_t talker_guid;
	uint64_t listener_guid;
	uint16_t talker_unique_id;
	uint16_t listener_unique_id;
	char stream_dest_mac[6];
	uint16_t connection_count;
	uint16_t sequence_id;
	uint16_t flags;
	uint16_t stream_vlan_id;
	uint16_t reserved;
} __attribute__ ((__packed__));

#define AVB_PACKET_ACMP_SET_MESSAGE_TYPE(p,v)		AVB_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVB_PACKET_ACMP_SET_STATUS(p,v)			AVB_PACKET_SET_SUB2(&(p)->hdr, v)

#define AVB_PACKET_ACMP_GET_MESSAGE_TYPE(p)		AVB_PACKET_GET_SUB1(&(p)->hdr)
#define AVB_PACKET_ACMP_GET_STATUS(p)			AVB_PACKET_GET_SUB2(&(p)->hdr)

struct avb_acmp *avb_acmp_register(struct server *server);

#endif /* AVB_ACMP_H */
