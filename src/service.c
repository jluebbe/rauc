#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <stdio.h>

#include "artifacts.h"
#include "bundle.h"
#include "bootchooser.h"
#include "config_file.h"
#include "context.h"
#include "install.h"
#include "manifest.h"
#include "mark.h"
#include "nbd.h"
#include "rauc-installer-generated.h"
#include "service.h"
#include "status_file.h"
#include "utils.h"

GMainLoop *service_loop = NULL;
RInstaller *r_installer = NULL;
RPoller *r_poller = NULL;
guint r_bus_name_id = 0;

static gboolean service_install_notify(gpointer data)
{
	RaucInstallArgs *args = data;

	g_mutex_lock(&args->status_mutex);
	while (!g_queue_is_empty(&args->status_messages)) {
		g_autofree gchar *msg = g_queue_pop_head(&args->status_messages);
		g_message("installing %s: %s", args->name, msg);
	}
	g_mutex_unlock(&args->status_mutex);

	return G_SOURCE_REMOVE;
}

static gboolean service_install_cleanup(gpointer data)
{
	RaucInstallArgs *args = data;

	g_mutex_lock(&args->status_mutex);
	if (args->status_result == 0) {
		g_message("installing `%s` succeeded", args->name);
	} else {
		g_message("installing `%s` failed: %d", args->name, args->status_result);
	}
	r_installer_emit_completed(r_installer, args->status_result);
	r_installer_set_operation(r_installer, "idle");
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(r_installer));
	g_mutex_unlock(&args->status_mutex);

	install_args_free(args);

	return G_SOURCE_REMOVE;
}

/*
 * Constructs RaucBundleAccessArgs from a GVariant dictionary.
 */
static void convert_dict_to_bundle_access_args(
		GVariantDict *dict,
		RaucBundleAccessArgs *access_args)
{
	g_return_if_fail(dict);
	g_return_if_fail(access_args);

	if (g_variant_dict_lookup(dict, "tls-cert", "s", &access_args->tls_cert))
		g_variant_dict_remove(dict, "tls-cert");
	if (g_variant_dict_lookup(dict, "tls-key", "s", &access_args->tls_key))
		g_variant_dict_remove(dict, "tls-key");
	if (g_variant_dict_lookup(dict, "tls-ca", "s", &access_args->tls_ca))
		g_variant_dict_remove(dict, "tls-ca");
	if (g_variant_dict_lookup(dict, "tls-no-verify", "b", &access_args->tls_no_verify))
		g_variant_dict_remove(dict, "tls-no-verify");
	if (g_variant_dict_lookup(dict, "http-headers", "^as", &access_args->http_headers))
		g_variant_dict_remove(dict, "http-headers");
}

static gboolean r_on_handle_install_bundle(
		RInstaller *interface,
		GDBusMethodInvocation *invocation,
		const gchar *source,
		GVariant *arg_args)
{
	RaucInstallArgs *args = install_args_new();
	g_auto(GVariantDict) dict = G_VARIANT_DICT_INIT(arg_args);
	g_autoptr(GVariant) dict_rest = NULL;
	GVariantIter iter;
	gchar *key;
	g_autofree gchar *message = NULL;
	gboolean res;

	g_print("input bundle: %s\n", source);

	res = !r_context_get_busy();
	if (!res) {
		message = g_strdup("Already processing a different method");
		args->status_result = 1;
		goto out;
	}

	args->name = g_strdup(source);
	args->notify = service_install_notify;
	args->cleanup = service_install_cleanup;

	if (g_variant_dict_lookup(&dict, "ignore-compatible", "b", &args->ignore_compatible))
		g_variant_dict_remove(&dict, "ignore-compatible");

	if (g_variant_dict_lookup(&dict, "ignore-version-limit", "b", &args->ignore_version_limit))
		g_variant_dict_remove(&dict, "ignore-version-limit");

	if (g_variant_dict_lookup(&dict, "transaction-id", "s", &args->transaction))
		g_variant_dict_remove(&dict, "transaction-id");

	if (g_variant_dict_lookup(&dict, "require-manifest-hash", "s", &args->require_manifest_hash))
		g_variant_dict_remove(&dict, "require-manifest-hash");

	convert_dict_to_bundle_access_args(&dict, &args->access_args);

	/* Check for unhandled keys */
	dict_rest = g_variant_dict_end(&dict);
	g_variant_iter_init(&iter, dict_rest);
	while (g_variant_iter_next(&iter, "{sv}", &key, NULL)) {
		message = g_strdup_printf("Unsupported key: %s", key);
		g_free(key);
		res = FALSE;
		args->status_result = 2;
		goto out;
	}

	r_config_file_modified_check();

	r_installer_set_operation(r_installer, "installing");
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(r_installer));
	res = install_run(args);
	if (!res) {
		message = g_strdup("Failed to launch install thread");
		args->status_result = 1;
		goto out;
	}
	args = NULL;

out:
	g_clear_pointer(&args, install_args_free);
	if (res) {
		r_installer_complete_install(interface, invocation);
	} else {
		r_installer_set_operation(r_installer, "idle");
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"%s", message);
	}

	return TRUE;
}

static gboolean r_on_handle_install(RInstaller *interface,
		GDBusMethodInvocation  *invocation,
		const gchar *arg_source)
{
	g_message("Using deprecated 'Install' D-Bus Method (replaced by 'InstallBundle')");
	return r_on_handle_install_bundle(interface, invocation, arg_source, NULL);
}

static gboolean r_on_handle_inspect_bundle(RInstaller *interface,
		GDBusMethodInvocation  *invocation,
		const gchar *arg_bundle, GVariant *arg_args)
{
	g_auto(RaucBundleAccessArgs) access_args = {0};
	g_auto(GVariantDict) dict = G_VARIANT_DICT_INIT(arg_args);
	g_autoptr(GVariant) remaining = NULL;
	GVariantIter iter;
	gchar *key;
	g_autoptr(RaucManifest) manifest = NULL;
	g_autoptr(RaucBundle) bundle = NULL;
	g_autofree gchar *message = NULL;
	GError *error = NULL;
	gboolean res = TRUE;

	g_print("bundle: %s\n", arg_bundle);

	res = !r_context_get_busy();
	if (!res) {
		message = g_strdup("already processing a different method");
		goto out;
	}

	convert_dict_to_bundle_access_args(&dict, &access_args);

	/* Check for unhandled keys */
	remaining = g_variant_dict_end(&dict);
	g_variant_iter_init(&iter, remaining);
	while (g_variant_iter_next(&iter, "{sv}", &key, NULL)) {
		message = g_strdup_printf("Unsupported key: %s", key);
		g_free(key);
		res = FALSE;
		goto out;
	}

	g_assert(access_args.http_info_headers == NULL);
	access_args.http_info_headers = assemble_info_headers(NULL);

	res = check_bundle(arg_bundle, &bundle, CHECK_BUNDLE_DEFAULT, &access_args, &error);
	if (!res) {
		message = g_strdup(error->message);
		g_clear_error(&error);
		goto out;
	}

	if (bundle->manifest) {
		manifest = g_steal_pointer(&bundle->manifest);
	} else {
		res = load_manifest_from_bundle(bundle, &manifest, &error);
		if (!res) {
			message = g_strdup(error->message);
			g_clear_error(&error);
			goto out;
		}
	}

out:
	if (!res) {
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"%s", message);
		return TRUE;
	}

	if (arg_args) {
		GVariant *info_variant;

		info_variant = r_manifest_to_dict(manifest);

		r_installer_complete_inspect_bundle(
				interface,
				invocation,
				info_variant);
	} else {
		/* arg_args unset means legacy API */
		r_installer_complete_info(
				interface,
				invocation,
				manifest->update_compatible,
				manifest->update_version ? manifest->update_version : "");
	}

	return TRUE;
}

static gboolean r_on_handle_info(RInstaller *interface,
		GDBusMethodInvocation  *invocation,
		const gchar *arg_bundle)
{
	g_message("Using deprecated 'Info' D-Bus Method (replaced by 'InspectBundle')");
	return r_on_handle_inspect_bundle(interface, invocation, arg_bundle, NULL);
}

static gboolean r_on_handle_mark(RInstaller *interface,
		GDBusMethodInvocation  *invocation,
		const gchar *arg_state,
		const gchar *arg_slot_identifier)
{
	g_autofree gchar *slot_name = NULL;
	g_autofree gchar *message = NULL;
	gboolean res;

	res = !r_context_get_busy();
	if (!res) {
		message = g_strdup("already processing a different method");
		goto out;
	}

	res = mark_run(arg_state, arg_slot_identifier, &slot_name, &message);

out:
	if (res) {
		r_installer_complete_mark(interface, invocation, slot_name, message);
	} else {
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"%s", message);
	}
	if (message)
		g_message("rauc mark: %s", message);

	return TRUE;
}

/*
 * Constructs a GVariant dictionary representing a slot status.
 */
static GVariant* convert_slot_status_to_dict(RaucSlot *slot)
{
	RaucSlotStatus *slot_state = NULL;
	GVariantDict dict;

	r_slot_status_load(slot);
	slot_state = slot->status;

	g_variant_dict_init(&dict, NULL);

	if (slot->sclass)
		g_variant_dict_insert(&dict, "class", "s", slot->sclass);
	if (slot->device)
		g_variant_dict_insert(&dict, "device", "s", slot->device);
	if (slot->type)
		g_variant_dict_insert(&dict, "type", "s", slot->type);
	if (slot->bootname)
		g_variant_dict_insert(&dict, "bootname", "s", slot->bootname);
	if (slot->state)
		g_variant_dict_insert(&dict, "state", "s", r_slot_slotstate_to_str(slot->state));
	if (slot->description)
		g_variant_dict_insert(&dict, "description", "s", slot->description);
	if (slot->parent)
		g_variant_dict_insert(&dict, "parent", "s", slot->parent->name);
	if (slot->mount_point || slot->ext_mount_point)
		g_variant_dict_insert(&dict, "mountpoint", "s", slot->mount_point ? slot->mount_point : slot->ext_mount_point);
	if (slot->bootname)
		g_variant_dict_insert(&dict, "boot-status", "s", slot->boot_good ? "good" : "bad");

	if (slot_state->bundle_compatible)
		g_variant_dict_insert(&dict, "bundle.compatible", "s", slot_state->bundle_compatible);

	if (slot_state->bundle_version)
		g_variant_dict_insert(&dict, "bundle.version", "s", slot_state->bundle_version);

	if (slot_state->bundle_description)
		g_variant_dict_insert(&dict, "bundle.description", "s", slot_state->bundle_description);

	if (slot_state->bundle_build)
		g_variant_dict_insert(&dict, "bundle.build", "s", slot_state->bundle_build);

	if (slot_state->bundle_hash)
		g_variant_dict_insert(&dict, "bundle.hash", "s", slot_state->bundle_hash);

	if (slot_state->status)
		g_variant_dict_insert(&dict, "status", "s", slot_state->status);

	if (slot_state->checksum.digest && slot_state->checksum.type == G_CHECKSUM_SHA256) {
		g_variant_dict_insert(&dict, "sha256", "s", slot_state->checksum.digest);
		g_variant_dict_insert(&dict, "size", "t", (guint64) slot_state->checksum.size);
	}

	if (slot_state->installed_txn)
		g_variant_dict_insert(&dict, "installed.transaction", "s", slot_state->installed_txn);

	if (slot_state->installed_timestamp) {
		g_variant_dict_insert(&dict, "installed.timestamp", "s", slot_state->installed_timestamp);
		g_variant_dict_insert(&dict, "installed.count", "u", slot_state->installed_count);
	}

	if (slot_state->activated_timestamp) {
		g_variant_dict_insert(&dict, "activated.timestamp", "s", slot_state->activated_timestamp);
		g_variant_dict_insert(&dict, "activated.count", "u", slot_state->activated_count);
	}

	return g_variant_dict_end(&dict);
}

/*
 * Makes slot status information available via DBUS.
 */
static GVariant* create_slotstatus_array(GError **error)
{
	gint slot_number = g_hash_table_size(r_context()->config->slots);
	GVariant **slot_status_tuples;
	GVariant *slot_status_array;
	gint slot_count = 0;
	GError *ierror = NULL;
	gboolean res = FALSE;
	GHashTableIter iter;
	RaucSlot *slot;

	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	g_assert_nonnull(r_installer);

	res = update_external_mount_points(&ierror);
	if (!res) {
		g_propagate_prefixed_error(
				error,
				ierror,
				"Failed to update mount points: ");
		return NULL;
	}

	res = determine_boot_states(&ierror);
	if (!res) {
		g_message("Failed to determine boot states: %s", ierror->message);
		g_clear_error(&ierror);
	}

	slot_status_tuples = g_new(GVariant*, slot_number);

	g_hash_table_iter_init(&iter, r_context()->config->slots);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer*) &slot)) {
		GVariant* slot_status[2];

		slot_status[0] = g_variant_new_string(slot->name);
		slot_status[1] = convert_slot_status_to_dict(slot);

		slot_status_tuples[slot_count] = g_variant_new_tuple(slot_status, 2);
		slot_count++;
	}

	/* it's an array of (slotname, dict) tuples */
	slot_status_array = g_variant_new_array(G_VARIANT_TYPE("(sa{sv})"), slot_status_tuples, slot_number);
	g_free(slot_status_tuples);

	return slot_status_array;
}

static gboolean r_on_handle_get_slot_status(RInstaller *interface,
		GDBusMethodInvocation  *invocation)
{
	GVariant *slotstatus;
	GError *ierror = NULL;
	gboolean res;

	res = !r_context_get_busy();

	if (!res) {
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"already processing a different method");
		return TRUE;
	}

	r_config_file_modified_check();

	slotstatus = create_slotstatus_array(&ierror);
	if (!slotstatus) {
		g_dbus_method_invocation_return_gerror(invocation, ierror);
		return TRUE;
	}

	r_installer_complete_get_slot_status(interface, invocation, slotstatus);

	return TRUE;
}

static gboolean r_on_handle_get_artifact_status(RInstaller *interface,
		GDBusMethodInvocation  *invocation)
{
	GVariant *artifactstatus;
	GError *ierror = NULL;
	gboolean res;

	res = !r_context_get_busy();

	if (!res) {
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"already processing a different method");
		return TRUE;
	}

	artifactstatus = r_artifacts_to_dict();
	if (!artifactstatus) {
		g_dbus_method_invocation_return_gerror(invocation, ierror);
		return TRUE;
	}

	r_installer_complete_get_artifact_status(interface, invocation, artifactstatus);

	return TRUE;
}

static gboolean r_on_handle_get_primary(RInstaller *interface,
		GDBusMethodInvocation  *invocation)
{
	GError *ierror = NULL;
	RaucSlot *primary = NULL;

	if (r_context_get_busy()) {
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"already processing a different method");
		return TRUE;
	}

	primary = r_boot_get_primary(&ierror);
	if (!primary) {
		g_dbus_method_invocation_return_error(invocation,
				G_IO_ERROR,
				G_IO_ERROR_FAILED_HANDLED,
				"Failed getting primary slot: %s\n", ierror->message);
		g_printerr("Failed getting primary slot: %s\n", ierror->message);
		g_clear_error(&ierror);
		return TRUE;
	}

	r_installer_complete_get_primary(interface, invocation, primary->name);

	return TRUE;
}

static gboolean auto_install(const gchar *source)
{
	RaucInstallArgs *args = install_args_new();
	gboolean res = TRUE;

	if (!g_file_test(r_context()->config->autoinstall_path, G_FILE_TEST_EXISTS))
		return FALSE;

	g_message("input bundle: %s", source);

	res = !r_context_get_busy();
	if (!res)
		goto out;

	args->name = g_strdup(source);
	args->notify = service_install_notify;
	args->cleanup = service_install_cleanup;

	res = install_run(args);
	if (!res) {
		goto out;
	}
	args = NULL;

out:
	g_clear_pointer(&args, g_free);

	return res;
}

void set_last_error(const gchar *message)
{
	if (r_installer)
		r_installer_set_last_error(r_installer, message);
}

static void send_progress_callback(gint percentage,
		const gchar *message,
		gint nesting_depth)
{
	GVariant *progress_update_tuple;

	progress_update_tuple = g_variant_new("(isi)", percentage, message, nesting_depth);

	r_installer_set_progress(r_installer, progress_update_tuple);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(r_installer));
}

static gboolean on_handle_poll(RPoller *interface, GDBusMethodInvocation *invocation);

static void r_on_bus_acquired(GDBusConnection *connection,
		const gchar     *name,
		gpointer user_data)
{
	GError *ierror = NULL;

	g_signal_connect(r_installer, "handle-install",
			G_CALLBACK(r_on_handle_install),
			NULL);

	g_signal_connect(r_installer, "handle-install-bundle",
			G_CALLBACK(r_on_handle_install_bundle),
			NULL);

	g_signal_connect(r_installer, "handle-info",
			G_CALLBACK(r_on_handle_info),
			NULL);

	g_signal_connect(r_installer, "handle-inspect-bundle",
			G_CALLBACK(r_on_handle_inspect_bundle),
			NULL);

	g_signal_connect(r_installer, "handle-mark",
			G_CALLBACK(r_on_handle_mark),
			NULL);

	g_signal_connect(r_installer, "handle-get-slot-status",
			G_CALLBACK(r_on_handle_get_slot_status),
			NULL);

	g_signal_connect(r_installer, "handle-get-artifact-status",
			G_CALLBACK(r_on_handle_get_artifact_status),
			NULL);

	g_signal_connect(r_installer, "handle-get-primary",
			G_CALLBACK(r_on_handle_get_primary),
			NULL);

	r_context_register_progress_callback(send_progress_callback);

	// Set initial Operation status to "idle"
	r_installer_set_operation(r_installer, "idle");

	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(r_installer),
			connection,
			"/",
			&ierror)) {
		g_error("Failed to export interface: %s", ierror->message);
		g_error_free(ierror);
	}

	r_installer_set_compatible(r_installer, r_context()->config->system_compatible);
	r_installer_set_variant(r_installer, r_context()->config->system_variant);
	r_installer_set_boot_slot(r_installer, r_context()->bootslot);

	if (r_poller) {
		g_signal_connect(r_poller, "handle-poll",
				G_CALLBACK(on_handle_poll),
				NULL);

		if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(r_poller),
				connection,
				"/",
				&ierror)) {
			g_error("Failed to export interface: %s", ierror->message);
			g_error_free(ierror);
		}
		g_message("poller skeleton set up");
	}

	return;
}

static void r_on_name_acquired(GDBusConnection *connection,
		const gchar     *name,
		gpointer user_data)
{
	g_debug("name '%s' acquired", name);

	if (r_context()->config->autoinstall_path)
		auto_install(r_context()->config->autoinstall_path);

	return;
}

static void r_on_name_lost(GDBusConnection *connection,
		const gchar     *name,
		gpointer user_data)
{
	gboolean *service_return = (gboolean*)user_data;
	const gchar *bus_type_name = (!g_strcmp0(g_getenv("DBUS_STARTER_BUS_TYPE"), "session"))
	                             ? "session" : "system";

	if (connection == NULL) {
		if (r_installer)
			g_printerr("Lost connection to the %s bus\n", bus_type_name);
		else
			g_printerr("Connection to the %s bus can't be made for %s\n", bus_type_name, name);
	} else {
		g_printerr("Failed to obtain name %s on %s bus\n", name, bus_type_name);
	}

	/* Abort service with exit code */
	*service_return = FALSE;

	if (service_loop) {
		g_main_loop_quit(service_loop);
	}

	return;
}

static gboolean r_on_signal(gpointer user_data)
{
	g_message("stopping service");
	if (service_loop) {
		g_main_loop_quit(service_loop);
	}
	return G_SOURCE_REMOVE;
}

typedef struct {
	GSource source;

	gboolean installation_running;

	guint64 attempt_count;
	guint64 recent_error_count; /* since last success */
	gint64 last_attempt_time; /* monotonic */
	gint64 last_success_time; /* monotonic */
	gchar *last_error_message;
	gboolean update_available;
	gchar *summary;
	gchar *attempted_hash; /* manifest hash of the attempted update */

	/* from the last successful attempt */
	RaucManifest *manifest;
	guint64 bundle_size;
	gchar *bundle_effective_url;
	guint64 bundle_modified_time;
	gchar *bundle_etag;
} RPollSource;

typedef enum {
	POLL_DELAY_NORMAL = 0,
	POLL_DELAY_SHORT,
	POLL_DELAY_NOW,
	POLL_DELAY_INITIAL,
} RPollDelay;

static void poll_reschedule(RPollSource *poll_source, RPollDelay delay)
{
	gint64 delay_ms = 0;

	switch (delay) {
		case POLL_DELAY_NORMAL:
			delay_ms = r_context()->config->poll_interval_ms * (poll_source->recent_error_count+1);
			delay_ms = MIN(delay_ms, r_context()->config->poll_max_interval_ms);
			break;
		case POLL_DELAY_SHORT:
			delay_ms = 15 * 1000;
			break;
		case POLL_DELAY_NOW:
			delay_ms = 2 * 1000;
			break;
		case POLL_DELAY_INITIAL:
			delay_ms = r_context()->config->poll_interval_ms / g_random_int_range(1, 10);
			break;
		default:
			g_assert_not_reached();
	}

	//g_message("delay_ms=%"G_GINT64_FORMAT, delay_ms);
	if (r_context()->mock.poll_speedup)
		delay_ms = delay_ms / r_context()->mock.poll_speedup;
	//g_message("speedup delay_ms=%"G_GINT64_FORMAT, delay_ms);

	gint64 next = g_get_monotonic_time() + delay_ms * 1000;
	r_poller_set_next_poll(r_poller, next);
	//g_message("now=%"G_GINT64_FORMAT" next poll=%"G_GINT64_FORMAT, g_get_monotonic_time(), next);
	g_source_set_ready_time(&poll_source->source, next);

	g_autofree gchar *duration_str = r_format_duration(delay_ms / 1000);
	g_message("scheduled next poll in: %s", duration_str);
}

static gboolean poll_fetch(RPollSource *poll_source, GError **error)
{
	GError *ierror = NULL;

	g_return_val_if_fail(poll_source, FALSE);

	/* fetch manifest */
	g_auto(RaucBundleAccessArgs) access_args = {0};
	access_args.http_info_headers = assemble_info_headers(NULL);
	if (poll_source->bundle_etag) {
		g_ptr_array_add(access_args.http_info_headers, g_strdup_printf("If-None-Match: %s", poll_source->bundle_etag));
	}

	g_autoptr(RaucBundle) bundle = NULL;
	if (!check_bundle(r_context()->config->poll_source, &bundle, CHECK_BUNDLE_DEFAULT, &access_args, &ierror)) {
		if (g_error_matches(ierror, R_NBD_ERROR, R_NBD_ERROR_NO_CONTENT)) {
			g_message("polling: no bundle available");
			/* TODO update summary? */
			return TRUE; /* FIXME should this be an error? */
		} else if (g_error_matches(ierror, R_NBD_ERROR, R_NBD_ERROR_NOT_MODIFIED)) {
			g_message("polling: bundle not modified");
			/* TODO update summary? */
			return TRUE;
		} else {
			g_propagate_error(error, ierror);
			return FALSE;
		}
	}

	if (!bundle->manifest) {
		g_message("polling failed: no manifest found");
		return FALSE;
	}

	g_clear_pointer(&poll_source->manifest, free_manifest);
	poll_source->manifest = g_steal_pointer(&bundle->manifest);

	g_assert(bundle->nbd_srv);
	poll_source->bundle_size = bundle->nbd_srv->data_size;
	poll_source->bundle_modified_time = bundle->nbd_srv->modified_time;
	r_replace_strdup(&poll_source->bundle_effective_url, bundle->nbd_srv->effective_url);
	r_replace_strdup(&poll_source->bundle_etag, bundle->nbd_srv->etag);

	return TRUE;
}

static gboolean poll_check_available(RPollSource *poll_source, GError **error)
{
	/* decide if we need to install (using system version?) */
	g_message("polling: system-version=%s bundle-version=%s",
			r_context()->system_version, poll_source->manifest->update_version);

	/* TODO check install_if */

	if (g_strcmp0(r_context()->system_version, poll_source->manifest->update_version) != 0) {
		r_replace_strdup(&poll_source->summary, "update available: different system version");
		return TRUE;
	}

	r_replace_strdup(&poll_source->summary, "no update available");
	return FALSE;
}

static gboolean poll_install_cleanup(gpointer data)
{
	RaucInstallArgs *args = data;
	RPollSource *poll_source = args->data;

	poll_source->installation_running = FALSE;

	g_mutex_lock(&args->status_mutex);
	if (args->status_result == 0) {
		g_message("installing `%s` succeeded", args->name);
	} else {
		g_message("installing `%s` failed: %d", args->name, args->status_result);
	}
	/* TODO expose error? */
	r_installer_emit_completed(r_installer, args->status_result);
	r_installer_set_operation(r_installer, "idle");
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(r_installer));
	g_mutex_unlock(&args->status_mutex);

	install_args_free(args);

	poll_reschedule(poll_source, POLL_DELAY_SHORT);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(r_poller));

	return G_SOURCE_REMOVE;
}

static gboolean poll_install(RPollSource *poll_source, GError **error)
{
	RaucInstallArgs *args = install_args_new();
	g_autofree gchar *message = NULL;
	gboolean res;

	g_return_val_if_fail(poll_source, FALSE);
	g_return_val_if_fail(poll_source->manifest, FALSE);
	g_return_val_if_fail(poll_source->manifest->hash, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	args->name = g_strdup(r_context()->config->poll_source);
	args->cleanup = poll_install_cleanup;
	args->data = poll_source;
	/* lock bundle via manifest hash */
	args->require_manifest_hash = g_strdup(poll_source->manifest->hash);

	r_installer_set_operation(r_installer, "installing");
	res = install_run(args);
	if (!res) {
		message = g_strdup("Failed to launch install thread");
		args->status_result = 1;
		goto out;
	}
	args = NULL;
	poll_source->installation_running = TRUE;

out:
	g_clear_pointer(&args, install_args_free);
	if (res) {
	} else {
		r_installer_set_operation(r_installer, "idle");
	}

	return TRUE;

	/* TODO trigger installation, which may reboot at the end */
}

static gboolean poll_step(RPollSource *poll_source, GError **error)
{
	GError *ierror = NULL;

	if (!poll_fetch(poll_source, &ierror)) {
		g_propagate_error(error, ierror);
		return FALSE;
	}
	/* TODO store cache info */

	g_clear_pointer(&poll_source->summary, g_free);
	poll_source->update_available = FALSE;
	gboolean available = poll_check_available(poll_source, &ierror);
	if (ierror) {
		g_propagate_prefixed_error(error, ierror, "availability check failed: ");
		return FALSE;
	} else if (!available) {
		return TRUE;
	} else {
		poll_source->update_available = TRUE;
	}

	/* retry if manifest hash has changed */
	if (g_strcmp0(poll_source->manifest->hash, poll_source->attempted_hash) != 0) {
		r_replace_strdup(&poll_source->attempted_hash, poll_source->manifest->hash);
		if (!poll_install(poll_source, &ierror)) {
			return FALSE;
		}
	}

	return TRUE;
}

static void poll_update_status(RPollSource *poll_source)
{
	g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(&builder, "{sv}", "attempt-count", g_variant_new_uint64(poll_source->attempt_count));
	g_variant_builder_add(&builder, "{sv}", "recent-error-count", g_variant_new_uint64(poll_source->recent_error_count));

	g_variant_builder_add(&builder, "{sv}", "last-attempt-time", g_variant_new_uint64(poll_source->last_attempt_time));
	g_variant_builder_add(&builder, "{sv}", "last-success-time", g_variant_new_uint64(poll_source->last_success_time));
	if (poll_source->last_error_message)
		g_variant_builder_add(&builder, "{sv}", "last-error-message", g_variant_new_string(poll_source->last_error_message));
	g_variant_builder_add(&builder, "{sv}", "update-available", g_variant_new_boolean(poll_source->update_available));
	if (poll_source->summary)
		g_variant_builder_add(&builder, "{sv}", "summary", g_variant_new_string(poll_source->summary));
	if (poll_source->attempted_hash)
		g_variant_builder_add(&builder, "{sv}", "attempted-hash", g_variant_new_string(poll_source->attempted_hash));

	if (poll_source->manifest) {
		/* manifest dict */
		g_variant_builder_add(&builder, "{sv}", "manifest", r_manifest_to_dict(poll_source->manifest));

		/* bundle dict */
		g_variant_builder_open(&builder, G_VARIANT_TYPE("{sv}"));
		g_variant_builder_add(&builder, "s", "bundle");
		g_variant_builder_open(&builder, G_VARIANT_TYPE("v"));
		g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
		if (poll_source->bundle_size)
			g_variant_builder_add(&builder, "{sv}", "size", g_variant_new_uint64(poll_source->bundle_size));
		if (poll_source->bundle_effective_url)
			g_variant_builder_add(&builder, "{sv}", "effective-url", g_variant_new_string(poll_source->bundle_effective_url));
		if (poll_source->bundle_modified_time)
			g_variant_builder_add(&builder, "{sv}", "modified-time", g_variant_new_uint64(poll_source->bundle_modified_time));
		if (poll_source->bundle_etag)
			g_variant_builder_add(&builder, "{sv}", "etag", g_variant_new_string(poll_source->bundle_etag));
		g_variant_builder_close(&builder); /* inner a{sv} */
		g_variant_builder_close(&builder); /* inner v */
		g_variant_builder_close(&builder); /* outer {sv} */
	}

	r_poller_set_status(r_poller, g_variant_builder_end(&builder));
}

static gboolean on_handle_poll(RPoller *interface, GDBusMethodInvocation *invocation)
{
	RPollSource *poll_source = (RPollSource *)g_object_get_data(G_OBJECT(interface), "r-poll");
	g_assert(poll_source);

	poll_reschedule(poll_source, POLL_DELAY_NOW);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(r_poller));
	r_poller_complete_poll(interface, invocation);

	return TRUE;
}

static gboolean poll_source_dispatch(GSource *source, GSourceFunc _callback, gpointer _user_data)
{
	RPollSource *poll_source = (RPollSource *)source;
	g_autoptr(GError) ierror = NULL;

	/* check busy state */
	if (r_context_get_busy()) {
		g_debug("context busy, will try again later");
		poll_reschedule(poll_source, POLL_DELAY_SHORT);
		return G_SOURCE_CONTINUE;
	}

	/* check inhibit */
	for (gchar **p = r_context()->config->poll_inhibit_files; p && *p; p++) {
		if (g_file_test(*p, G_FILE_TEST_EXISTS)) {
			g_debug("inhibited by %s", *p);
			poll_reschedule(poll_source, POLL_DELAY_SHORT);
			return G_SOURCE_CONTINUE;
		}
	}

	/* poll once */
	poll_source->last_attempt_time = g_get_monotonic_time();
	poll_source->attempt_count += 1;
	/* TODO add some headers? recent errors? */
	if (!poll_step(poll_source, &ierror)) {
		g_message("polling failed: %s", ierror->message);
		r_replace_strdup(&poll_source->last_error_message, g_strdup(ierror->message));
		poll_source->recent_error_count += 1;
	} else {
		g_clear_pointer(&poll_source->last_error_message, g_free);
		poll_source->last_success_time = g_get_monotonic_time();
		poll_source->recent_error_count = 0;
	}

	poll_update_status(poll_source);

	if (poll_source->installation_running) {
		/* wait until the installation has completed */
		g_source_set_ready_time(&poll_source->source, -1);
	} else {
		/* schedule next poll */
		poll_reschedule(poll_source, POLL_DELAY_NORMAL);
	}

	return G_SOURCE_CONTINUE;
}

static void poll_source_finalize(GSource *source)
{
	RPollSource *poll_source = (RPollSource *)source;

	g_clear_pointer(&poll_source->last_error_message, g_free);
	g_clear_pointer(&poll_source->manifest, free_manifest);
	g_clear_pointer(&poll_source->summary, g_free);
	g_clear_pointer(&poll_source->attempted_hash, g_free);
	g_clear_pointer(&poll_source->bundle_effective_url, g_free);
	g_clear_pointer(&poll_source->bundle_etag, g_free);
}

static GSourceFuncs source_funcs = {
	.dispatch = poll_source_dispatch,
	.finalize = poll_source_finalize,
};

static RPollSource *poll_setup(void)
{
	g_assert(r_context()->config->poll_source);

	GSource *source = g_source_new(&source_funcs, sizeof(RPollSource));
	RPollSource *poll_source = (RPollSource *)source;

	poll_update_status(poll_source);
	poll_reschedule(poll_source, POLL_DELAY_INITIAL);
	g_source_attach(source, NULL);

	g_message("Setup done");

	return poll_source;
}

gboolean r_service_run(void)
{
	gboolean service_return = TRUE;
	GBusType bus_type = (!g_strcmp0(g_getenv("DBUS_STARTER_BUS_TYPE"), "session"))
	                    ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
	RPollSource *poll_source = NULL;

	service_loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGTERM, r_on_signal, NULL);

	r_installer = r_installer_skeleton_new();

	if (r_context()->config->poll_source) {
		r_poller = r_poller_skeleton_new();
		poll_source = poll_setup();
		g_object_set_data(G_OBJECT(r_poller), "r-poll", poll_source);
	}

	r_bus_name_id = g_bus_own_name(bus_type,
			"de.pengutronix.rauc",
			G_BUS_NAME_OWNER_FLAGS_NONE,
			r_on_bus_acquired,
			r_on_name_acquired,
			r_on_name_lost,
			&service_return, NULL);

	g_main_loop_run(service_loop);

	if (r_bus_name_id)
		g_bus_unown_name(r_bus_name_id);

	g_main_loop_unref(service_loop);
	service_loop = NULL;

	g_clear_pointer(&r_installer, g_object_unref);
	if (poll_source)
		g_source_unref(&poll_source->source);
	g_clear_pointer(&r_poller, g_object_unref);

	return service_return;
}
