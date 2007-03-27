/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2007  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/bnep.h>

#include <glib.h>

#include "logging.h"
#include "dbus.h"

#define NETWORK_PATH "/org/bluez/network"
#define NETWORK_MANAGER_INTERFACE "org.bluez.network.Manager"

#include "error.h"
#include "bridge.h"
#include "manager.h"
#include "server.h"
#include "connection.h"
#include "common.h"

struct manager {
	bdaddr_t src;		/* Local adapter BT address */
	GSList *servers;	/* Network registered servers paths */
	GSList *connections;	/* Network registered connections paths */
};

struct pending_reply {
	DBusConnection *conn;
	DBusMessage *msg;
	struct manager *mgr;
	uint16_t id;
	char *addr;
	char *path;
	char *adapter_path;
};

static DBusConnection *connection = NULL;

static void pending_reply_free(struct pending_reply *pr)
{
	if (pr->addr)
		g_free(pr->addr);

	if (pr->path)
		g_free(pr->path);

	if (pr->adapter_path)
		g_free(pr->adapter_path);
}

static DBusHandlerResult create_path(DBusConnection *conn,
					DBusMessage *msg, char *path,
					const char *sname)
{
	DBusMessage *reply, *signal;

	/* emit signal when it is a new path */
	if (sname) {
		signal = dbus_message_new_signal(NETWORK_PATH,
			NETWORK_MANAGER_INTERFACE, sname);

		dbus_message_append_args(signal,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

		send_message_and_unref(conn, signal);
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	g_free(path);
	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult list_paths(DBusConnection *conn, DBusMessage *msg,
					GSList *list)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array_iter;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array_iter);

	for (; list; list = list->next) {
		dbus_message_iter_append_basic(&array_iter,
						DBUS_TYPE_STRING,
						&list->data);
	}
	dbus_message_iter_close_container(&iter, &array_iter);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult remove_path(DBusConnection *conn,
					DBusMessage *msg, GSList **list,
					const char *sname)
{
	const char *path;
	DBusMessage *reply, *signal;
	DBusError derr;
	GSList *l;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &path,
				DBUS_TYPE_INVALID)) {
		err_invalid_args(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	l = g_slist_find_custom(*list, path, (GCompareFunc) strcmp);
	if (!l)
		return err_does_not_exist(conn, msg, "Path doesn't exist");

	g_free(l->data);
	*list = g_slist_remove(*list, l->data);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	if (!dbus_connection_unregister_object_path(conn, path))
		error("Network path unregister failed");

	signal = dbus_message_new_signal(NETWORK_PATH,
			NETWORK_MANAGER_INTERFACE, sname);

	dbus_message_append_args(signal,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

	send_message_and_unref(conn, signal);

	return send_message_and_unref(conn, reply);
}

static void pan_record_reply(DBusPendingCall *call, void *data)
{
	struct pending_reply *pr = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError derr;
	int len;
	uint8_t *rec_bin;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		if (dbus_error_has_name(&derr, "org.bluez.Error.ConnectionAttemptFailed"))
			err_connection_failed(pr->conn, pr->msg, derr.message);
		else
			err_not_supported(pr->conn, pr->msg);

		error("GetRemoteServiceRecord failed: %s(%s)", derr.name,
			derr.message);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &rec_bin, &len,
				DBUS_TYPE_INVALID)) {
		err_not_supported(pr->conn, pr->msg);
		error("%s: %s", derr.name, derr.message);
		goto fail;
	}

	if (len == 0) {
		err_not_supported(pr->conn, pr->msg);
		error("Invalid PAN service record length");
		goto fail;
	}


	if (connection_register(pr->conn, pr->path, pr->addr, pr->id) == -1) {
		err_failed(pr->conn, pr->msg, "D-Bus path registration failed");
		goto fail;
	}

	pr->mgr->connections = g_slist_append(pr->mgr->connections,
						g_strdup(pr->path));

	create_path(pr->conn, pr->msg, g_strdup (pr->path), "ConnectionCreated");
fail:
	dbus_error_free(&derr);
	pending_reply_free(pr);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static int get_record(struct pending_reply *pr, uint32_t handle,
					DBusPendingCallNotifyFunction cb)
{
	DBusMessage *msg;
	DBusPendingCall *pending;

	msg = dbus_message_new_method_call("org.bluez", pr->adapter_path,
			"org.bluez.Adapter", "GetRemoteServiceRecord");
	if (!msg)
		return -1;

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &pr->addr,
			DBUS_TYPE_UINT32, &handle,
			DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(pr->conn, msg, &pending, -1) == FALSE) {
		error("Can't send D-Bus message.");
		return -1;
	}

	dbus_pending_call_set_notify(pending, cb, pr, NULL);
	dbus_message_unref(msg);

	return 0;
}

static void pan_handle_reply(DBusPendingCall *call, void *data)
{
	struct pending_reply *pr = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError derr;
	uint32_t *phandle;
	int len;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		if (dbus_error_has_name(&derr, "org.bluez.Error.ConnectionAttemptFailed"))
			err_connection_failed(pr->conn, pr->msg, derr.message);
		else
			err_not_supported(pr->conn, pr->msg);

		error("GetRemoteServiceHandles: %s(%s)", derr.name, derr.message);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &phandle, &len,
				DBUS_TYPE_INVALID)) {
		err_not_supported(pr->conn, pr->msg);
		error("%s: %s", derr.name, derr.message);
		goto fail;
	}

	if (!len) {
		err_not_supported(pr->conn, pr->msg);
		goto fail;
	}

	if (get_record(pr, *phandle, pan_record_reply) < 0) {
		err_not_supported(pr->conn, pr->msg);
		goto fail;
	}

	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
	return;
fail:
	dbus_error_free(&derr);
	pending_reply_free(pr);
}

static int get_handles(struct pending_reply *pr, DBusPendingCallNotifyFunction cb)
{
	DBusMessage *msg;
	DBusPendingCall *pending;
	const char *uuid;

	msg = dbus_message_new_method_call("org.bluez", pr->adapter_path,
			"org.bluez.Adapter", "GetRemoteServiceHandles");
	if (!msg)
		return -1;

	uuid = bnep_uuid(pr->id);
	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &pr->addr,
			DBUS_TYPE_STRING, &uuid,
			DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(pr->conn, msg, &pending, -1) == FALSE) {
		error("Can't send D-Bus message.");
		return -1;
	}

	dbus_pending_call_set_notify(pending, cb, pr, NULL);
	dbus_message_unref(msg);

	return 0;
}

static DBusHandlerResult list_servers(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct manager *mgr = data;

	return list_paths(conn, msg, mgr->servers);
}

static DBusHandlerResult create_server(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct manager *mgr = data;
	DBusError derr;
	const char *str;
	char *path;
	int id;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &str,
				DBUS_TYPE_INVALID)) {
		err_invalid_args(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	id = bnep_service_id(str);
	if ((id != BNEP_SVC_GN) && (id != BNEP_SVC_NAP))
		return err_invalid_args(conn, msg, "Not supported");

	path = g_new0(char, 32);
	snprintf(path, 32, NETWORK_PATH "/server/%X", id);

	/* Path already registered */
	if (g_slist_find_custom(mgr->servers, path, (GCompareFunc) strcmp))
		return create_path(conn, msg, path, NULL); /* Return already exist error */

	/* FIXME: define which type should be used -- string/uuid str/uui128 */
	if (server_register(conn, path, id) == -1) {
		err_failed(conn, msg, "D-Bus path registration failed");
		g_free(path);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	mgr->servers = g_slist_append(mgr->servers, g_strdup(path));

	return create_path(conn, msg, path, "ServerCreated");
}

static DBusHandlerResult remove_server(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct manager *mgr = data;

	return remove_path(conn, msg, &mgr->servers, "ServerRemoved");
}

static DBusHandlerResult list_connections(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct manager *mgr = data;

	return list_paths(conn, msg, mgr->connections);
}

static DBusHandlerResult create_connection(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct manager *mgr = data;
	struct pending_reply *pr;
	static int uid = 0;
	DBusError derr;
	char src_addr[18];
	const char *addr;
	const char *str;
	uint16_t id;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &addr,
				DBUS_TYPE_STRING, &str,
				DBUS_TYPE_INVALID)) {
		err_invalid_args(conn, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	id = bnep_service_id(str);
	if ((id != BNEP_SVC_GN) && (id != BNEP_SVC_NAP))
		return err_invalid_args(conn, msg, "Not supported");

	pr = g_new(struct pending_reply, 1);
	pr->conn = conn;
	pr->msg = dbus_message_ref(msg);
	pr->mgr = mgr;
	pr->addr = g_strdup(addr);
	pr->id = id;
	pr->path = g_new0(char, 48);
	snprintf(pr->path, 48, NETWORK_PATH "/connection%d", uid++);
	ba2str(&mgr->src, src_addr);
	pr->adapter_path = g_new0(char, 32);
	snprintf(pr->adapter_path , 32, "/org/bluez/hci%d", hci_devid(src_addr));

	if (get_handles(pr, pan_handle_reply) < 0) {
		err_failed(conn, msg, "D-Bus path registration failed");
		pending_reply_free(pr);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult remove_connection(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct manager *mgr = data;

	return remove_path(conn, msg, &mgr->connections, "ConnectionRemoved");
}

static DBusHandlerResult manager_message(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const char *path, *iface, *member;

	path = dbus_message_get_path(msg);
	iface = dbus_message_get_interface(msg);
	member = dbus_message_get_member(msg);

	/* Catching fallback paths */
	if (strcmp(NETWORK_PATH, path) != 0)
		return err_unknown_connection(conn, msg);

	/* Accept messages from the manager interface only */
	if (strcmp(NETWORK_MANAGER_INTERFACE, iface))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(member, "ListServers") == 0)
		return list_servers(conn, msg, data);

	if (strcmp(member, "CreateServer") == 0)
		return create_server(conn, msg, data);

	if (strcmp(member, "RemoveServer") == 0)
		return remove_server(conn, msg, data);

	if (strcmp(member, "ListConnections") == 0)
		return list_connections(conn, msg, data);

	if (strcmp(member, "CreateConnection") == 0)
		return create_connection(conn, msg, data);

	if (strcmp(member, "RemoveConnection") == 0)
		return remove_connection(conn, msg, data);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void manager_free(struct manager *mgr)
{
	if (!mgr)
		return;

	if (mgr->servers) {
		g_slist_foreach(mgr->servers, (GFunc)g_free, NULL);
		g_slist_free(mgr->servers);
	}

	if (mgr->connections) {
		g_slist_foreach(mgr->connections, (GFunc)g_free, NULL);
		g_slist_free(mgr->connections);
	}

	g_free (mgr);
	bnep_kill_all_connections();
}

static void manager_unregister(DBusConnection *conn, void *data)
{
	struct manager *mgr = data;

	info("Unregistered manager path");

	manager_free(mgr);
}

/* Virtual table to handle manager object path hierarchy */
static const DBusObjectPathVTable manager_table = {
	.message_function = manager_message,
	.unregister_function = manager_unregister,
};

int network_dbus_init(void)
{
	struct manager *mgr;
	bdaddr_t src;
#if 0
	int dev_id;
#endif

	dbus_connection_set_exit_on_disconnect(connection, TRUE);

	mgr = g_new0(struct manager, 1);

	/* Fallback to catch invalid network path */
	if (dbus_connection_register_fallback(connection, NETWORK_PATH,
						&manager_table, mgr) == FALSE) {
		error("D-Bus failed to register %s path", NETWORK_PATH);
		goto fail;
	}

	info("Registered manager path:%s", NETWORK_PATH);

	/* Set the default adapter */
	bacpy(&src, BDADDR_ANY);
#if 0
	dev_id = hci_get_route(&src);
	if (dev_id < 0) {
		error("Bluetooth device not available");
		goto fail;
	}

	if (hci_devba(dev_id, &src) < 0) {
		error("Can't get local adapter device info");
		goto fail;
	}
#endif

	bacpy(&mgr->src, &src);

	return 0;

fail:
	manager_free(mgr);

	return -1;
}

void network_dbus_exit(void)
{
	dbus_connection_unregister_object_path(connection, NETWORK_PATH);
}

int network_init(DBusConnection *conn)
{
	int err;

	if (bridge_init() < 0) {
		error("Can't init bridge module");
		return -1;
	}

	if (bridge_create("pan0") < 0) {
		error("Can't create bridge");
		return -1;
	}

	if (bnep_init()) {
		error("Can't init bnep module");
		return -1;
	}

	connection = dbus_connection_ref(conn);

	err = network_dbus_init();
	if (err < 0)
		dbus_connection_unref(connection);

	return err;
}

void network_exit(void)
{
	network_dbus_exit();

	dbus_connection_unref(connection);

	connection = NULL;

	if (bridge_remove("pan0") < 0)
		error("Can't remove bridge");

	bnep_cleanup();
	bridge_cleanup();
}
