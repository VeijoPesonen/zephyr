/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2021 Lingao Meng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/byteorder.h>

#include <net/buf.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt.h>
#include <bluetooth/mesh.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_PROXY)
#define LOG_MODULE_NAME bt_mesh_proxy
#include "common/log.h"

#include "mesh.h"
#include "adv.h"
#include "net.h"
#include "rpl.h"
#include "transport.h"
#include "host/ecc.h"
#include "prov.h"
#include "beacon.h"
#include "foundation.h"
#include "access.h"
#include "proxy.h"
#include "proxy_msg.h"

#define PDU_SAR(data)      (data[0] >> 6)

/* Mesh Profile 1.0 Section 6.6:
 * "The timeout for the SAR transfer is 20 seconds. When the timeout
 *  expires, the Proxy Server shall disconnect."
 */
#define PROXY_SAR_TIMEOUT  K_SECONDS(20)

#define SAR_COMPLETE       0x00
#define SAR_FIRST          0x01
#define SAR_CONT           0x02
#define SAR_LAST           0x03

#define PDU_HDR(sar, type) (sar << 6 | (type & BIT_MASK(6)))

static uint8_t __noinit bufs[CONFIG_BT_MAX_CONN * CONFIG_BT_MESH_PROXY_MSG_LEN];

static struct bt_mesh_proxy_role roles[CONFIG_BT_MAX_CONN];

static void proxy_sar_timeout(struct k_work *work)
{
	struct bt_mesh_proxy_role *role;

	BT_WARN("Proxy SAR timeout");

	role = CONTAINER_OF(work, struct bt_mesh_proxy_role, sar_timer);
	if (role->conn) {
		bt_conn_disconnect(role->conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

ssize_t bt_mesh_proxy_msg_recv(struct bt_mesh_proxy_role *role,
			       const void *buf, uint16_t len)
{
	const uint8_t *data = buf;

	switch (PDU_SAR(data)) {
	case SAR_COMPLETE:
		if (role->buf.len) {
			BT_WARN("Complete PDU while a pending incomplete one");
			return -EINVAL;
		}

		role->msg_type = PDU_TYPE(data);
		net_buf_simple_add_mem(&role->buf, data + 1, len - 1);
		role->cb.recv(role);
		net_buf_simple_reset(&role->buf);
		break;

	case SAR_FIRST:
		if (role->buf.len) {
			BT_WARN("First PDU while a pending incomplete one");
			return -EINVAL;
		}

		k_work_reschedule(&role->sar_timer, PROXY_SAR_TIMEOUT);
		role->msg_type = PDU_TYPE(data);
		net_buf_simple_add_mem(&role->buf, data + 1, len - 1);
		break;

	case SAR_CONT:
		if (!role->buf.len) {
			BT_WARN("Continuation with no prior data");
			return -EINVAL;
		}

		if (role->msg_type != PDU_TYPE(data)) {
			BT_WARN("Unexpected message type in continuation");
			return -EINVAL;
		}

		k_work_reschedule(&role->sar_timer, PROXY_SAR_TIMEOUT);
		net_buf_simple_add_mem(&role->buf, data + 1, len - 1);
		break;

	case SAR_LAST:
		if (!role->buf.len) {
			BT_WARN("Last SAR PDU with no prior data");
			return -EINVAL;
		}

		if (role->msg_type != PDU_TYPE(data)) {
			BT_WARN("Unexpected message type in last SAR PDU");
			return -EINVAL;
		}

		/* If this fails, the work handler exits early, as there's no
		 * active SAR buffer.
		 */
		(void)k_work_cancel_delayable(&role->sar_timer);
		net_buf_simple_add_mem(&role->buf, data + 1, len - 1);
		role->cb.recv(role);
		net_buf_simple_reset(&role->buf);
		break;
	}

	return len;
}

int bt_mesh_proxy_msg_send(struct bt_mesh_proxy_role *role, uint8_t type,
			   struct net_buf_simple *msg,
			   bt_gatt_complete_func_t end, void *user_data)
{
	int err;
	uint16_t mtu;
	struct bt_conn *conn = role->conn;

	BT_DBG("conn %p type 0x%02x len %u: %s", (void *)conn, type, msg->len,
	       bt_hex(msg->data, msg->len));

	/* ATT_MTU - OpCode (1 byte) - Handle (2 bytes) */
	mtu = bt_gatt_get_mtu(conn) - 3;
	if (mtu > msg->len) {
		net_buf_simple_push_u8(msg, PDU_HDR(SAR_COMPLETE, type));
		return role->cb.send(conn, msg->data, msg->len, end, user_data);
	}

	net_buf_simple_push_u8(msg, PDU_HDR(SAR_FIRST, type));
	err = role->cb.send(conn, msg->data, mtu, NULL, NULL);
	if (err) {
		return err;
	}

	net_buf_simple_pull(msg, mtu);

	while (msg->len) {
		if (msg->len + 1 < mtu) {
			net_buf_simple_push_u8(msg, PDU_HDR(SAR_LAST, type));
			err = role->cb.send(conn, msg->data, msg->len, end, user_data);
			if (err) {
				return err;
			}

			break;
		}

		net_buf_simple_push_u8(msg, PDU_HDR(SAR_CONT, type));
		err = role->cb.send(conn, msg->data, mtu, NULL, NULL);
		if (err) {
			return err;
		}

		net_buf_simple_pull(msg, mtu);
	}

	return 0;
}

static void proxy_msg_init(struct bt_mesh_proxy_role *role)
{
	/* Check if buf has been allocated, in this way, we no longer need
	 * to repeat the operation.
	 */
	if (role->buf.__buf) {
		net_buf_simple_reset(&role->buf);
		return;
	}

	net_buf_simple_init_with_data(&role->buf,
				      &bufs[bt_conn_index(role->conn) *
					    CONFIG_BT_MESH_PROXY_MSG_LEN],
				      CONFIG_BT_MESH_PROXY_MSG_LEN);

	net_buf_simple_reset(&role->buf);

	k_work_init_delayable(&role->sar_timer, proxy_sar_timeout);
}

struct bt_mesh_proxy_role *bt_mesh_proxy_role_setup(struct bt_conn *conn,
						    proxy_send_cb_t send,
						    proxy_recv_cb_t recv)
{
	struct bt_mesh_proxy_role *role;

	role = &roles[bt_conn_index(conn)];

	role->conn = bt_conn_ref(conn);
	proxy_msg_init(role);

	role->cb.recv = recv;
	role->cb.send = send;

	return role;
}

static void gatt_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_mesh_proxy_role *role;

	BT_DBG("conn %p reason 0x%02x", (void *)conn, reason);

	role = &roles[bt_conn_index(conn)];

	/* If this fails, the work handler exits early, as
	 * there's no active connection.
	 */
	(void)k_work_cancel_delayable(&role->sar_timer);
	bt_conn_unref(role->conn);
	role->conn = NULL;

	bt_mesh_adv_update();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.disconnected = gatt_disconnected,
};
