// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2019  Intel Corporation. All rights reserved.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <time.h>

#define _GNU_SOURCE
#include <ell/ell.h>

#include "src/shared/ad.h"

#include "mesh/mesh-defs.h"
#include "mesh/dbus.h"
#include "mesh/error.h"
#include "mesh/mesh.h"
#include "mesh/mesh-io.h"
#include "mesh/node.h"
#include "mesh/model.h"
#include "mesh/net.h"
#include "mesh/keyring.h"
#include "mesh/agent.h"
#include "mesh/provision.h"
#include "mesh/prov.h"
#include "mesh/remprv.h"
#include "mesh/manager.h"

struct prov_remote_data {
	struct l_dbus_message *msg;
	struct mesh_agent *agent;
	struct mesh_node *node;
	uint32_t disc_watch;
	uint16_t original;
	uint16_t primary;
	uint16_t net_idx;
	uint8_t transport;
	uint8_t num_ele;
	uint8_t uuid[16];
};

struct scan_req {
	struct mesh_node *node;
	struct l_timeout *timeout;
	uint16_t server;
	uint16_t net_idx;
	uint8_t uuid[16];
	int8_t rssi;
	bool ext;
};

static struct l_queue *scans;
static struct prov_remote_data *prov_pending;
static const uint8_t prvb[2] = {BT_AD_MESH_BEACON, 0x00};

static bool by_scan(const void *a, const void *b)
{
	return a == b;
}

static bool by_node(const void *a, const void *b)
{
	const struct scan_req *req = a;
	const struct mesh_node *node = b;

	return req->node == node;
}

static bool by_node_svr(const void *a, const void *b)
{
	const struct scan_req *req = a;
	const struct scan_req *test = b;

	return req->node == test->node && req->server == test->server;
}

static void scan_cancel(struct l_timeout *timeout, void *user_data)
{
	struct scan_req *req = user_data;
	struct mesh_io *io;
	struct mesh_net *net;
	uint8_t msg[4];
	int n;

	l_debug("");

	req = l_queue_remove_if(scans, by_scan, req);

	if (!req)
		return;

	l_timeout_remove(req->timeout);

	if (req->server) {
		n = mesh_model_opcode_set(OP_REM_PROV_SCAN_STOP, msg);
		mesh_model_send(req->node, 0, req->server, APP_IDX_DEV_REMOTE,
						req->net_idx, DEFAULT_TTL,
						true, n, msg);
	} else {
		net = node_get_net(req->node);
		io = mesh_net_get_io(net);
		mesh_io_deregister_recv_cb(io, prvb, sizeof(prvb));
	}

	initiator_scan_unreg(req->node);
	l_free(req);
}

static void free_pending_add_call(void)
{
	if (!prov_pending)
		return;

	if (prov_pending->disc_watch)
		l_dbus_remove_watch(dbus_get_bus(),
						prov_pending->disc_watch);

	if (prov_pending->msg)
		l_dbus_message_unref(prov_pending->msg);

	l_free(prov_pending);
	prov_pending = NULL;
}

static void prov_disc_cb(struct l_dbus *bus, void *user_data)
{
	if (!prov_pending)
		return;

	initiator_cancel(prov_pending);
	prov_pending->disc_watch = 0;

	free_pending_add_call();
}

static void append_dict_entry_basic(struct l_dbus_message_builder *builder,
					const char *key, const char *signature,
					const void *data)
{
	if (!builder)
		return;

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', key);
	l_dbus_message_builder_enter_variant(builder, signature);
	l_dbus_message_builder_append_basic(builder, signature[0], data);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);
}

static void send_add_failed(const char *owner, const char *path,
							uint8_t status)
{
	struct l_dbus *dbus = dbus_get_bus();
	struct l_dbus_message_builder *builder;
	struct l_dbus_message *msg;

	msg = l_dbus_message_new_method_call(dbus, owner, path,
						MESH_PROVISIONER_INTERFACE,
						"AddNodeFailed");

	builder = l_dbus_message_builder_new(msg);
	dbus_append_byte_array(builder, prov_pending->uuid, 16);
	l_dbus_message_builder_append_basic(builder, 's',
						mesh_prov_status_str(status));
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
	l_dbus_send(dbus, msg);

	free_pending_add_call();
}

static bool add_cmplt(void *user_data, uint8_t status,
					const struct mesh_prov_node_info *info)
{
	struct prov_remote_data *pending = user_data;
	struct mesh_node *node = pending->node;
	struct l_dbus *dbus = dbus_get_bus();
	struct l_dbus_message_builder *builder;
	struct l_dbus_message *msg;
	bool result;

	if (pending != prov_pending)
		return false;

	if (status != PROV_ERR_SUCCESS) {
		send_add_failed(node_get_owner(node), node_get_app_path(node),
									status);
		return false;
	}

	/* If Unicast address changing, delete old dev key */
	if (pending->transport == PB_NPPI_01)
		keyring_del_remote_dev_key_all(pending->node,
							pending->original);

	result = keyring_put_remote_dev_key(pending->node, info->unicast,
					info->num_ele, info->device_key);

	if (!result) {
		send_add_failed(node_get_owner(node), node_get_app_path(node),
						PROV_ERR_CANT_ASSIGN_ADDR);
		return false;
	}

	if (pending->transport > PB_NPPI_02)
		msg = l_dbus_message_new_method_call(dbus, node_get_owner(node),
						node_get_app_path(node),
						MESH_PROVISIONER_INTERFACE,
						"AddNodeComplete");
	else
		msg = l_dbus_message_new_method_call(dbus, node_get_owner(node),
						node_get_app_path(node),
						MESH_PROVISIONER_INTERFACE,
						"ReprovComplete");

	builder = l_dbus_message_builder_new(msg);

	if (pending->transport > PB_NPPI_02)
		dbus_append_byte_array(builder, pending->uuid, 16);
	else {
		uint8_t nppi = (uint8_t) pending->transport;

		l_dbus_message_builder_append_basic(builder, 'q',
							&pending->original);
		l_dbus_message_builder_append_basic(builder, 'y', &nppi);
	}

	l_dbus_message_builder_append_basic(builder, 'q', &info->unicast);
	l_dbus_message_builder_append_basic(builder, 'y', &info->num_ele);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	l_dbus_send(dbus, msg);

	free_pending_add_call();

	return true;
}

static void mgr_prov_data (struct l_dbus_message *reply, void *user_data)
{
	struct prov_remote_data *pending = user_data;
	uint16_t net_idx;
	uint16_t primary;

	if (pending != prov_pending)
		return;

	if (l_dbus_message_is_error(reply))
		return;

	if (pending->transport == PB_NPPI_01) {
		/* If performing NPPI, we only get new primary unicast here */
		if (!l_dbus_message_get_arguments(reply, "q", &primary))
			return;

		net_idx = pending->net_idx;

	} else if (!l_dbus_message_get_arguments(reply, "qq", &net_idx,
								&primary))
		return;

	pending->primary = primary;
	pending->net_idx = net_idx;
	initiator_prov_data(net_idx, primary, pending);
}

static bool add_data_get(void *user_data, uint8_t num_ele)
{
	struct prov_remote_data *pending = user_data;
	struct l_dbus_message *msg;
	struct l_dbus *dbus;
	const char *app_path;
	const char *sender;

	if (pending != prov_pending)
		return false;

	dbus = dbus_get_bus();
	app_path = node_get_app_path(pending->node);
	sender = node_get_owner(pending->node);

	if (pending->transport > PB_NPPI_02) {
		msg = l_dbus_message_new_method_call(dbus, sender, app_path,
						MESH_PROVISIONER_INTERFACE,
						"RequestProvData");

		l_dbus_message_set_arguments(msg, "y", num_ele);
	} else if (pending->transport == PB_NPPI_01) {
		msg = l_dbus_message_new_method_call(dbus, sender, app_path,
						MESH_PROVISIONER_INTERFACE,
						"RequestReprovData");

		l_dbus_message_set_arguments(msg, "qy", pending->original,
								num_ele);
	} else
		return false;

	l_dbus_send_with_reply(dbus, msg, mgr_prov_data, pending, NULL);

	pending->num_ele = num_ele;

	return true;
}

static void add_start(void *user_data, int err)
{
	struct l_dbus_message *reply;

	l_debug("Start callback");

	if (err == MESH_ERROR_NONE)
		reply = l_dbus_message_new_method_return(prov_pending->msg);
	else
		reply = dbus_error(prov_pending->msg, MESH_ERROR_FAILED,
				"Failed to start provisioning initiator");

	l_dbus_send(dbus_get_bus(), reply);
	l_dbus_message_unref(prov_pending->msg);

	prov_pending->msg = NULL;
}

static struct l_dbus_message *reprovision_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct l_dbus_message_iter options, var;
	struct l_dbus_message *reply;
	struct mesh_net *net = node_get_net(node);
	const char *key;
	uint16_t subidx;
	uint16_t server = 0;
	uint8_t nppi = 0;

	l_debug("Reprovision request");

	if (!l_dbus_message_get_arguments(msg, "qa{sv}", &server, &options))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!IS_UNICAST(server))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, "Bad Unicast");

	/* Default to nodes primary subnet index */
	subidx = mesh_net_get_primary_idx(net);

	/* Get Provisioning Options */
	while (l_dbus_message_iter_next_entry(&options, &key, &var)) {
		bool failed = true;

		if (!strcmp(key, "NPPI")) {
			if (l_dbus_message_iter_get_variant(&var, "y", &nppi)) {
				if (nppi <= 2)
					failed = false;
			}
		} else if (!strcmp(key, "Subnet")) {
			if (l_dbus_message_iter_get_variant(&var, "q",
								&subidx)) {
				if (subidx <= MAX_KEY_IDX)
					failed = false;
			}
		}

		if (failed)
			return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Invalid options");
	}

	/* AddNode cancels all outstanding Scanning from node */
	manager_scan_cancel(node);

	/* Invoke Prov Initiator */
	prov_pending = l_new(struct prov_remote_data, 1);

	prov_pending->transport = nppi;
	prov_pending->node = node;
	prov_pending->original = server;
	prov_pending->agent = node_get_agent(node);

	if (!node_is_provisioner(node) || (prov_pending->agent == NULL)) {
		reply = dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED,
							"Missing Interfaces");
		goto fail;
	}

	prov_pending->msg = l_dbus_message_ref(msg);
	initiator_start(prov_pending->transport, server, subidx, NULL, 99, 60,
					prov_pending->agent, add_start,
					add_data_get, add_cmplt, node,
					prov_pending);

	prov_pending->disc_watch = l_dbus_add_disconnect_watch(dbus,
						node_get_owner(node),
						prov_disc_cb, NULL, NULL);

	return NULL;
fail:
	l_free(prov_pending);
	prov_pending = NULL;
	return reply;
}

static struct l_dbus_message *add_node_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct l_dbus_message_iter iter_uuid, options, var;
	struct l_dbus_message *reply;
	struct mesh_net *net = node_get_net(node);
	const char *key;
	uint8_t *uuid;
	uint32_t n = 0;
	uint16_t subidx;
	uint16_t sec = 60;
	uint16_t server = 0;

	l_debug("AddNode request");

	if (!l_dbus_message_get_arguments(msg, "aya{sv}", &iter_uuid, &options))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!l_dbus_message_iter_get_fixed_array(&iter_uuid, &uuid, &n) ||
									n != 16)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Bad device UUID");

	/* Default to nodes primary subnet index */
	subidx = mesh_net_get_primary_idx(net);

	/* Get Provisioning Options */
	while (l_dbus_message_iter_next_entry(&options, &key, &var)) {
		bool failed = true;

		if (!strcmp(key, "Seconds")) {
			if (l_dbus_message_iter_get_variant(&var, "q", &sec))
				failed = false;
		} else if (!strcmp(key, "Server")) {
			if (l_dbus_message_iter_get_variant(&var, "q",
								&server)) {
				if (server < 0x8000)
					failed = false;
			}
		} else if (!strcmp(key, "Subnet")) {
			if (l_dbus_message_iter_get_variant(&var, "q",
								&subidx)) {
				if (subidx <= MAX_KEY_IDX)
					failed = false;
			}
		}

		if (failed)
			return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Invalid options");
	}

	/* Device Key update/Composition update requires remote server */
	if (!n && !server)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Invalid options");

	/* If no server specified, use local */
	if (!server)
		server = node_get_primary(node);

	/* AddNode cancels all outstanding Scanning from node */
	manager_scan_cancel(node);

	/* Invoke Prov Initiator */
	prov_pending = l_new(struct prov_remote_data, 1);

	if (n)
		memcpy(prov_pending->uuid, uuid, 16);
	else
		uuid = NULL;

	prov_pending->transport = PB_ADV;
	prov_pending->node = node;
	prov_pending->agent = node_get_agent(node);

	if (!node_is_provisioner(node) || (prov_pending->agent == NULL)) {
		l_debug("Provisioner: %d", node_is_provisioner(node));
		l_debug("Agent: %p", prov_pending->agent);
		reply = dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED,
							"Missing Interfaces");
		goto fail;
	}

	prov_pending->msg = l_dbus_message_ref(msg);
	initiator_start(PB_ADV, server, subidx, uuid, 99, sec,
					prov_pending->agent, add_start,
					add_data_get, add_cmplt, node,
					prov_pending);

	prov_pending->disc_watch = l_dbus_add_disconnect_watch(dbus,
						node_get_owner(node),
						prov_disc_cb, NULL, NULL);

	return NULL;
fail:
	l_free(prov_pending);
	prov_pending = NULL;
	return reply;
}


static struct l_dbus_message *import_node_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct l_dbus_message_iter iter_key;
	uint16_t primary;
	uint8_t num_ele;
	uint8_t *key;
	uint32_t n;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "qyay", &primary, &num_ele,
								&iter_key))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!l_dbus_message_iter_get_fixed_array(&iter_key, &key, &n)
								|| n != 16)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Bad device key");

	if (!keyring_put_remote_dev_key(node, primary, num_ele, key))
		return dbus_error(msg, MESH_ERROR_FAILED, NULL);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *delete_node_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct mesh_net *net = node_get_net(node);
	uint16_t primary;
	uint8_t num_ele;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "qy", &primary, &num_ele))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (mesh_net_is_local_address(net, primary, num_ele))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
					"Cannot remove local device key");

	keyring_del_remote_dev_key(node, primary, num_ele);

	return l_dbus_message_new_method_return(msg);
}

static void manager_scan_result(void *user_data, uint16_t server, bool ext,
					const uint8_t *data, uint16_t len)
{
	struct scan_req node_svr = {
		.node = user_data,
		.server = server,
	};
	struct scan_req *req;
	struct l_dbus_message_builder *builder;
	struct l_dbus_message *msg;
	struct l_dbus *dbus;
	int16_t rssi;

	l_debug("scan_result %4.4x %p", server, user_data);
	req = l_queue_find(scans, by_node_svr, &node_svr);
	if (!req) {
		l_debug("No scan_result req");
		return;
	}

	/* Filter repeats with weaker signal */
	if (!memcmp(data + 1, req->uuid, sizeof(req->uuid))) {
		if (!ext && ((int8_t) data[0] <= req->rssi)) {
			l_debug("Already Seen");
			return;
		}
	}

	if (!ext && ((int8_t) data[0] > req->rssi))
		req->rssi = (int8_t) data[0];

	rssi = req->rssi;
	memcpy(req->uuid, data + 1, sizeof(req->uuid));
	dbus = dbus_get_bus();
	msg = l_dbus_message_new_method_call(dbus, node_get_owner(req->node),
						node_get_app_path(req->node),
						MESH_PROVISIONER_INTERFACE,
						"ScanResult");

	builder = l_dbus_message_builder_new(msg);
	l_dbus_message_builder_append_basic(builder, 'n', &rssi);
	dbus_append_byte_array(builder, data + 1, len - 1);
	l_dbus_message_builder_enter_array(builder, "{sv}");
	append_dict_entry_basic(builder, "Server", "q", &server);
	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	l_dbus_send(dbus, msg);
}

static struct l_dbus_message *start_scan_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct scan_req new_req = {
		.node = user_data,
		.server = 0,
		.timeout = NULL,
		.ext = false,
	};
	struct scan_req *req;
	struct mesh_net *net;
	uint8_t *uuid, *ext = NULL;
	uint8_t scan_req[21];
	int n;
	uint32_t ext_len;
	uint32_t flen = 0;
	uint16_t sec = 60;
	const char *key;
	struct l_dbus_message_iter options, var;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(new_req.node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "a{sv}", &options))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!node_is_provisioner(new_req.node))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	net = node_get_net(new_req.node);
	new_req.net_idx = mesh_net_get_primary_idx(net);
	memset(new_req.uuid, 0, sizeof(new_req.uuid));

	while (l_dbus_message_iter_next_entry(&options, &key, &var)) {
		bool failed = true;

		if (!strcmp(key, "Seconds")) {
			if (l_dbus_message_iter_get_variant(&var, "q", &sec))
				failed = false;
		} else if (!strcmp(key, "Subnet")) {
			if (l_dbus_message_iter_get_variant(&var, "q",
							&new_req.net_idx)) {
				if (new_req.net_idx <= MAX_KEY_IDX)
					failed = false;
			}
		} else if (!strcmp(key, "Server")) {
			if (l_dbus_message_iter_get_variant(&var, "q",
							&new_req.server)) {
				if (new_req.server < 0x8000)
					failed = false;
			}
		} else if (!strcmp(key, "Filter")) {
			if (l_dbus_message_iter_get_variant(&var, "ay", &var)) {
				if (l_dbus_message_iter_get_fixed_array(&var,
								&uuid, &flen)) {
					if (flen == 16) {
						memcpy(new_req.uuid, uuid,
									flen);
						failed = false;
					}
				}
			}
		} else if (!strcmp(key, "Extended")) {
			if (l_dbus_message_iter_get_variant(&var, "ay", &var)) {
				if (l_dbus_message_iter_get_fixed_array(&var,
								&ext, &ext_len))
					failed = false;
			}
		}

		if (failed)
			return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Invalid options");
	}

	if (!scans)
		scans = l_queue_new();

	if (new_req.server) {
		if (!sec || sec > 60)
			return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Invalid options");
	} else {
		new_req.server = node_get_primary(new_req.node);
		if (!sec || sec > 60)
			sec = 60;
	}

	req = l_queue_remove_if(scans, by_node_svr, &new_req);

	if (!req)
		req = l_new(struct scan_req, 1);

	if (req->timeout) {
		l_timeout_remove(req->timeout);
		req->timeout = NULL;
	}

	*req = new_req;
	req->rssi = -128;

	if (sec)
		req->timeout = l_timeout_create(sec, scan_cancel, req, NULL);


	n = mesh_model_opcode_set(OP_REM_PROV_SCAN_START, scan_req);
	scan_req[n++] = 5;
	scan_req[n++] = sec;
	if (flen) {
		memcpy(scan_req + n, req->uuid, flen);
		n += flen;
	}

	mesh_model_send(req->node, 0, req->server, APP_IDX_DEV_REMOTE,
						req->net_idx, DEFAULT_TTL,
						true, n, scan_req);

	initiator_scan_reg(manager_scan_result, req->node);

	l_queue_push_tail(scans, req);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *cancel_scan_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)) || !node_is_provisioner(node))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	manager_scan_cancel(node);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *store_new_subnet(struct mesh_node *node,
					struct l_dbus_message *msg,
					uint16_t net_idx, uint8_t *new_key)
{
	struct keyring_net_key key;

	if (net_idx > MAX_KEY_IDX)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (keyring_get_net_key(node, net_idx, &key)) {
		/* Allow redundant calls only if key values match */
		if (!memcmp(key.old_key, new_key, 16))
			return l_dbus_message_new_method_return(msg);

		return dbus_error(msg, MESH_ERROR_ALREADY_EXISTS, NULL);
	}

	memcpy(key.old_key, new_key, 16);
	memcpy(key.new_key, new_key, 16);
	key.net_idx = net_idx;
	key.phase = KEY_REFRESH_PHASE_NONE;

	if (!keyring_put_net_key(node, net_idx, &key))
		return dbus_error(msg, MESH_ERROR_FAILED, NULL);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *create_subnet_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	uint8_t key[16];
	uint16_t net_idx;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "q", &net_idx) ||
						net_idx == PRIMARY_NET_IDX)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	/* Generate key and store */
	l_getrandom(key, sizeof(key));

	return store_new_subnet(node, msg, net_idx, key);
}

static struct l_dbus_message *update_subnet_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct keyring_net_key key;
	uint16_t net_idx;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "q", &net_idx) ||
						net_idx > MAX_KEY_IDX)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!keyring_get_net_key(node, net_idx, &key))
		return dbus_error(msg, MESH_ERROR_DOES_NOT_EXIST, NULL);

	switch (key.phase) {
	case KEY_REFRESH_PHASE_NONE:
		/* Generate Key and update phase */
		l_getrandom(key.new_key, sizeof(key.new_key));
		key.phase = KEY_REFRESH_PHASE_ONE;

		if (!keyring_put_net_key(node, net_idx, &key))
			return dbus_error(msg, MESH_ERROR_FAILED, NULL);

		/* Fall Through */

	case KEY_REFRESH_PHASE_ONE:
		/* Allow redundant calls to start Key Refresh */
		return l_dbus_message_new_method_return(msg);

	default:
		break;
	}

	/* All other phases mean KR already in progress over-the-air */
	return dbus_error(msg, MESH_ERROR_IN_PROGRESS,
					"Key Refresh in progress");
}

static struct l_dbus_message *delete_subnet_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	uint16_t net_idx;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "q", &net_idx) ||
						net_idx > MAX_KEY_IDX)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	keyring_del_net_key(node, net_idx);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *import_subnet_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct l_dbus_message_iter iter_key;
	uint16_t net_idx;
	uint8_t *key;
	uint32_t n;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "qay", &net_idx, &iter_key))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!l_dbus_message_iter_get_fixed_array(&iter_key, &key, &n)
								|| n != 16)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Bad network key");

	return store_new_subnet(node, msg, net_idx, key);
}

static struct l_dbus_message *store_new_appkey(struct mesh_node *node,
					struct l_dbus_message *msg,
					uint16_t net_idx, uint16_t app_idx,
					uint8_t *new_key)
{
	struct keyring_net_key net_key;
	struct keyring_app_key app_key;

	if (net_idx > MAX_KEY_IDX || app_idx > MAX_KEY_IDX)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!keyring_get_net_key(node, net_idx, &net_key))
		return dbus_error(msg, MESH_ERROR_DOES_NOT_EXIST,
						"Bound net key not found");

	if (keyring_get_app_key(node, app_idx, &app_key)) {
		/* Allow redundant calls with identical values */
		if (!memcmp(app_key.old_key, new_key, 16) &&
						app_key.net_idx == net_idx)
			return l_dbus_message_new_method_return(msg);

		return dbus_error(msg, MESH_ERROR_ALREADY_EXISTS, NULL);
	}

	memcpy(app_key.old_key, new_key, 16);
	memcpy(app_key.new_key, new_key, 16);
	app_key.net_idx = net_idx;
	app_key.app_idx = app_idx;

	if (!keyring_put_app_key(node, app_idx, net_idx, &app_key))
		return dbus_error(msg, MESH_ERROR_FAILED, NULL);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *create_appkey_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	uint16_t net_idx, app_idx;
	uint8_t key[16];
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "qq", &net_idx, &app_idx))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	l_getrandom(key, sizeof(key));

	return store_new_appkey(node, msg, net_idx, app_idx, key);
}

static struct l_dbus_message *update_appkey_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct keyring_net_key net_key;
	struct keyring_app_key app_key;
	uint16_t app_idx;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "q", &app_idx) ||
							app_idx > MAX_KEY_IDX)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!keyring_get_app_key(node, app_idx, &app_key) ||
			!keyring_get_net_key(node, app_key.net_idx, &net_key))
		return dbus_error(msg, MESH_ERROR_DOES_NOT_EXIST, NULL);

	if (net_key.phase != KEY_REFRESH_PHASE_ONE)
		return dbus_error(msg, MESH_ERROR_FAILED, "Invalid Phase");

	/* Generate Key if in acceptable phase */
	l_getrandom(app_key.new_key, sizeof(app_key.new_key));

	if (!keyring_put_app_key(node, app_idx, app_key.net_idx, &app_key))
		return dbus_error(msg, MESH_ERROR_FAILED, NULL);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *delete_appkey_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	uint16_t app_idx;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "q", &app_idx))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	keyring_del_app_key(node, app_idx);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *import_appkey_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct l_dbus_message_iter iter_key;
	uint16_t net_idx, app_idx;
	uint8_t *key;
	uint32_t n;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "qqay", &net_idx, &app_idx,
								&iter_key))
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!l_dbus_message_iter_get_fixed_array(&iter_key, &key, &n)
								|| n != 16)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS,
							"Bad application key");

	return store_new_appkey(node, msg, net_idx, app_idx, key);
}

static struct l_dbus_message *set_key_phase_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct mesh_node *node = user_data;
	struct keyring_net_key key;
	uint16_t net_idx;
	uint8_t phase;
	const char *sender = l_dbus_message_get_sender(msg);

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	if (!l_dbus_message_get_arguments(msg, "qy", &net_idx, &phase) ||
					phase == KEY_REFRESH_PHASE_ONE ||
					phase > KEY_REFRESH_PHASE_THREE)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (!keyring_get_net_key(node, net_idx, &key))
		return dbus_error(msg, MESH_ERROR_DOES_NOT_EXIST, NULL);

	/* Canceling Key Refresh only valid from Phase One */
	if (phase == KEY_REFRESH_PHASE_NONE &&
					key.phase >= KEY_REFRESH_PHASE_TWO)
		return dbus_error(msg, MESH_ERROR_INVALID_ARGS, NULL);

	if (phase == KEY_REFRESH_PHASE_THREE) {

		/* If we are already in Phase None, then nothing to do */
		if (key.phase == KEY_REFRESH_PHASE_NONE)
			return l_dbus_message_new_method_return(msg);

		memcpy(key.old_key, key.new_key, 16);
		key.phase = KEY_REFRESH_PHASE_THREE;

		if (!keyring_put_net_key(node, net_idx, &key))
			return dbus_error(msg, MESH_ERROR_FAILED, NULL);

		if (!keyring_finalize_app_keys(node, net_idx))
			return dbus_error(msg, MESH_ERROR_FAILED, NULL);

		key.phase = KEY_REFRESH_PHASE_NONE;
	} else
		key.phase = phase;

	if (!keyring_put_net_key(node, net_idx, &key))
		return dbus_error(msg, MESH_ERROR_FAILED, NULL);

	return l_dbus_message_new_method_return(msg);
}

static struct l_dbus_message *export_keys_call(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	const char *sender = l_dbus_message_get_sender(msg);
	struct l_dbus_message_builder *builder;
	struct l_dbus_message *reply;
	struct mesh_node *node = user_data;

	l_debug("Export Keys");

	if (strcmp(sender, node_get_owner(node)))
		return dbus_error(msg, MESH_ERROR_NOT_AUTHORIZED, NULL);

	reply = l_dbus_message_new_method_return(msg);
	builder = l_dbus_message_builder_new(reply);

	l_dbus_message_builder_enter_array(builder, "{sv}");

	if (!keyring_build_export_keys_reply(node, builder)) {
		l_dbus_message_builder_destroy(builder);
		l_dbus_message_unref(reply);
		return dbus_error(msg, MESH_ERROR_FAILED, NULL);
	}

	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	return reply;
}

static void setup_management_interface(struct l_dbus_interface *iface)
{
	l_dbus_interface_method(iface, "AddNode", 0, add_node_call, "",
						"aya{sv}", "uuid", "options");
	l_dbus_interface_method(iface, "ImportRemoteNode", 0, import_node_call,
				"", "qyay", "primary", "count", "dev_key");
	l_dbus_interface_method(iface, "Reprovision", 0, reprovision_call,
					"", "qa{sv}", "unicast", "options");
	l_dbus_interface_method(iface, "DeleteRemoteNode", 0, delete_node_call,
						"", "qy", "primary", "count");
	l_dbus_interface_method(iface, "UnprovisionedScan", 0, start_scan_call,
							"", "a{sv}", "options");
	l_dbus_interface_method(iface, "UnprovisionedScanCancel", 0,
						cancel_scan_call, "", "");
	l_dbus_interface_method(iface, "CreateSubnet", 0, create_subnet_call,
							"", "q", "net_index");
	l_dbus_interface_method(iface, "UpdateSubnet", 0, update_subnet_call,
							"", "q", "net_index");
	l_dbus_interface_method(iface, "DeleteSubnet", 0, delete_subnet_call,
							"", "q", "net_index");
	l_dbus_interface_method(iface, "ImportSubnet", 0, import_subnet_call,
					"", "qay", "net_index", "net_key");
	l_dbus_interface_method(iface, "CreateAppKey", 0, create_appkey_call,
					"", "qq", "net_index", "app_index");
	l_dbus_interface_method(iface, "UpdateAppKey", 0, update_appkey_call,
							"", "q", "app_index");
	l_dbus_interface_method(iface, "DeleteAppKey", 0, delete_appkey_call,
							"", "q", "app_index");
	l_dbus_interface_method(iface, "ImportAppKey", 0, import_appkey_call,
							"", "qqay", "net_index",
							"app_index", "app_key");
	l_dbus_interface_method(iface, "SetKeyPhase", 0, set_key_phase_call, "",
						"qy", "net_index", "phase");
	l_dbus_interface_method(iface, "ExportKeys", 0, export_keys_call,
							"a(qaya{sv})a(qay)", "",
							"net_keys", "dev_keys");
}

bool manager_dbus_init(struct l_dbus *bus)
{
	if (!l_dbus_register_interface(bus, MESH_MANAGEMENT_INTERFACE,
						setup_management_interface,
						NULL, false)) {
		l_debug("Unable to register %s interface",
						MESH_MANAGEMENT_INTERFACE);
		return false;
	}

	return true;
}

void manager_scan_cancel(struct mesh_node *node)
{
	struct scan_req *req;

	while ((req = l_queue_find(scans, by_node, node)))
		scan_cancel(NULL, req);
}
