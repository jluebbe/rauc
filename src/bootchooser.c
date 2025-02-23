#include "barebox.h"
#include "bootchooser.h"
#include "config_file.h"
#include "context.h"
#include "custom.h"
#include "efi.h"
#include "grub.h"
#include "uboot.h"

GQuark r_bootchooser_error_quark(void)
{
	return g_quark_from_static_string("r_bootchooser_error_quark");
}

static const gchar *supported_bootloaders[] = {"barebox", "grub", "uboot", "efi", "custom", "noop", NULL};

gboolean r_boot_is_supported_bootloader(const gchar *bootloader)
{
	return g_strv_contains(supported_bootloaders, bootloader);
}

GString *r_bootchooser_order_primary(RaucSlot *slot)
{
	GString *order = NULL;
	g_autoptr(GList) slots = NULL;

	g_return_val_if_fail(slot, NULL);

	order = g_string_new(slot->bootname);

	/* Iterate over boot selection-handled slots (bootname set) */
	slots = g_hash_table_get_values(r_context()->config->slots);
	for (GList *l = slots; l != NULL; l = l->next) {
		RaucSlot *s = l->data;
		if (s == slot)
			continue;
		if (!s->bootname)
			continue;

		g_string_append_c(order, ' ');
		g_string_append(order, s->bootname);
	}

	return order;
}

gboolean r_boot_get_state(RaucSlot *slot, gboolean *good, GError **error)
{
	gboolean res = FALSE;
	GError *ierror = NULL;

	g_return_val_if_fail(slot, FALSE);
	g_return_val_if_fail(good, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* Function must not be called for slots without a bootname! */
	g_assert_nonnull(slot->bootname);

	if (g_strcmp0(r_context()->config->system_bootloader, "barebox") == 0) {
		res = r_barebox_get_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "grub") == 0) {
		res = r_grub_get_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "uboot") == 0) {
		res = r_uboot_get_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "efi") == 0) {
		res = r_efi_get_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "custom") == 0) {
		res = r_custom_get_state(slot, good, &ierror);
	} else {
		g_set_error(
				error,
				R_BOOTCHOOSER_ERROR,
				R_BOOTCHOOSER_ERROR_NOT_SUPPORTED,
				"Obtaining state from bootloader '%s' not supported yet", r_context()->config->system_bootloader);
		return FALSE;
	}

	if (!res) {
		g_propagate_prefixed_error(
				error,
				ierror,
				"%s backend: ", r_context()->config->system_bootloader);
	}

	return res;
}

gboolean r_boot_set_state(RaucSlot *slot, gboolean good, GError **error)
{
	gboolean res = FALSE;
	GError *ierror = NULL;

	g_return_val_if_fail(slot, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (g_strcmp0(r_context()->config->system_bootloader, "barebox") == 0) {
		res = r_barebox_set_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "grub") == 0) {
		res = r_grub_set_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "uboot") == 0) {
		res = r_uboot_set_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "efi") == 0) {
		res = r_efi_set_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "custom") == 0) {
		res = r_custom_set_state(slot, good, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "noop") == 0) {
		g_message("noop bootloader: ignore setting slot %s status to %s", slot->name, good ? "good" : "bad");
		res = TRUE;
	} else {
		g_set_error(
				error,
				R_BOOTCHOOSER_ERROR,
				R_BOOTCHOOSER_ERROR_NOT_SUPPORTED,
				"Bootloader type '%s' not supported yet", r_context()->config->system_bootloader);
		return FALSE;
	}

	if (!res) {
		g_propagate_prefixed_error(
				error,
				ierror,
				"%s backend: ", r_context()->config->system_bootloader);
	}

	return res;
}

RaucSlot *r_boot_get_primary(GError **error)
{
	RaucSlot *slot = NULL;
	GError *ierror = NULL;

	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (g_strcmp0(r_context()->config->system_bootloader, "barebox") == 0) {
		slot = r_barebox_get_primary(&ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "grub") == 0) {
		slot = r_grub_get_primary(&ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "uboot") == 0) {
		slot = r_uboot_get_primary(&ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "efi") == 0) {
		slot = r_efi_get_primary(&ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "custom") == 0) {
		slot = r_custom_get_primary(&ierror);
	} else {
		g_set_error(
				error,
				R_BOOTCHOOSER_ERROR,
				R_BOOTCHOOSER_ERROR_NOT_SUPPORTED,
				"Obtaining primary entry from bootloader '%s' not supported yet", r_context()->config->system_bootloader);
		return NULL;
	}

	if (!slot) {
		g_propagate_prefixed_error(
				error,
				ierror,
				"%s backend: ", r_context()->config->system_bootloader);
	}

	return slot;
}

gboolean r_boot_set_primary(RaucSlot *slot, GError **error)
{
	gboolean res = FALSE;
	GError *ierror = NULL;

	g_return_val_if_fail(slot, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (g_strcmp0(r_context()->config->system_bootloader, "barebox") == 0) {
		res = r_barebox_set_primary(slot, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "grub") == 0) {
		res = r_grub_set_primary(slot, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "uboot") == 0) {
		res = r_uboot_set_primary(slot, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "efi") == 0) {
		res = r_efi_set_primary(slot, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "custom") == 0) {
		res = r_custom_set_primary(slot, &ierror);
	} else if (g_strcmp0(r_context()->config->system_bootloader, "noop") == 0) {
		g_message("noop bootloader: ignore setting slot %s as primary", slot->name);
		res = TRUE;
	} else {
		g_set_error(
				error,
				R_BOOTCHOOSER_ERROR,
				R_BOOTCHOOSER_ERROR_NOT_SUPPORTED,
				"Bootloader type '%s' not supported yet", r_context()->config->system_bootloader);
		return FALSE;
	}

	if (!res) {
		g_propagate_prefixed_error(
				error,
				ierror,
				"%s backend: ", r_context()->config->system_bootloader);
	}

	return res;
}
