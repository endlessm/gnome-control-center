/*
 * Copyright (C) 2010 Intel, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Sergey Udaltsov <svu@gnome.org>
 *
 */

#include <config.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <polkit/polkit.h>

#include "cc-region-panel.h"
#include "cc-region-resources.h"
#include "cc-language-chooser.h"
#include "cc-format-chooser.h"
#include "cc-input-chooser.h"
#include "cc-input-options.h"

#include "cc-common-language.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-languages.h>
#include <libgnome-desktop/gnome-xkb-info.h>

#ifdef HAVE_IBUS
#include <ibus.h>
#include "cc-ibus-utils.h"
#endif

#include <act/act.h>

#include <libgd/gd-notification.h>

#define GNOME_DESKTOP_INPUT_SOURCES_DIR "org.gnome.desktop.input-sources"
#define KEY_CURRENT_INPUT_SOURCE "current"
#define KEY_INPUT_SOURCES        "sources"

#define GNOME_SYSTEM_LOCALE_DIR "org.gnome.system.locale"
#define KEY_REGION "region"

#define INPUT_SOURCE_TYPE_XKB "xkb"
#define INPUT_SOURCE_TYPE_IBUS "ibus"

CC_PANEL_REGISTER (CcRegionPanel, cc_region_panel)

#define WID(s) GTK_WIDGET (gtk_builder_get_object (self->priv->builder, s))

#define REGION_PANEL_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CC_TYPE_REGION_PANEL, CcRegionPanelPrivate))

typedef enum {
        CHOOSE_LANGUAGE,
        ADD_INPUT,
        REMOVE_INPUT
} SystemOp;

struct _CcRegionPanelPrivate {
	GtkBuilder *builder;

        GtkWidget   *login_button;
        GtkWidget   *login_label;
        gboolean     login;
        gboolean     login_auto_apply;
        GPermission *permission;
        SystemOp     op;
        GDBusProxy  *localed;
        GDBusProxy  *session;
        GCancellable *cancellable;

        GtkWidget *hbox_selector;
        GtkWidget *session_language_button;
        GtkWidget *login_language_button;

        GtkWidget *overlay;
        GtkWidget *notification;

        GtkWidget     *language_section;
        GtkListBoxRow *language_row;
        GtkWidget     *language_label;
        GtkListBoxRow *formats_row;
        GtkWidget     *formats_label;
        GtkListBoxRow *layouts_row;
        GtkWidget     *layouts_label;

        ActUserManager *user_manager;
        ActUser        *user;
        GSettings      *locale_settings;

        gchar *language;
        gchar *region;
        gchar *layout_type;
        gchar *layout_id;
        gchar *layout_name;
        gchar *system_language;
        gchar *system_region;
        gchar *system_layout_type;
        gchar *system_layout_id;
        gchar *system_layout_name;

        GSettings *input_settings;
        GnomeXkbInfo *xkb_info;
#ifdef HAVE_IBUS
        IBusBus *ibus;
        GHashTable *ibus_engines;
        GCancellable *ibus_cancellable;
#endif
};

static void add_input (CcRegionPanel *self);

static void
cc_region_panel_finalize (GObject *object)
{
	CcRegionPanel *self = CC_REGION_PANEL (object);
	CcRegionPanelPrivate *priv = self->priv;

        g_cancellable_cancel (priv->cancellable);
        g_clear_object (&priv->cancellable);

        if (priv->user_manager) {
                g_signal_handlers_disconnect_by_data (priv->user_manager, self);
                priv->user_manager = NULL;
        }

        if (priv->user) {
                g_signal_handlers_disconnect_by_data (priv->user, self);
                priv->user = NULL;
        }

        g_clear_object (&priv->permission);
        g_clear_object (&priv->localed);
        g_clear_object (&priv->session);
        g_clear_object (&priv->builder);
        g_clear_object (&priv->locale_settings);
        g_clear_object (&priv->input_settings);
        g_clear_object (&priv->xkb_info);
#ifdef HAVE_IBUS
        g_clear_object (&priv->ibus);
        if (priv->ibus_cancellable)
                g_cancellable_cancel (priv->ibus_cancellable);
        g_clear_object (&priv->ibus_cancellable);
        g_clear_pointer (&priv->ibus_engines, g_hash_table_destroy);
#endif
        g_free (priv->language);
        g_free (priv->region);
        g_free (priv->layout_id);
        g_free (priv->layout_name);
        g_free (priv->system_language);
        g_free (priv->system_region);
        g_free (priv->system_layout_id);
        g_free (priv->system_layout_name);

	G_OBJECT_CLASS (cc_region_panel_parent_class)->finalize (object);
}

static const char *
cc_region_panel_get_help_uri (CcPanel *panel)
{
        return "help:gnome-help/session-language";
}

static void
cc_region_panel_class_init (CcRegionPanelClass * klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	CcPanelClass * panel_class = CC_PANEL_CLASS (klass);

	g_type_class_add_private (klass, sizeof (CcRegionPanelPrivate));

	panel_class->get_help_uri = cc_region_panel_get_help_uri;

	object_class->finalize = cc_region_panel_finalize;
}

static void
restart_now (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;

        gd_notification_dismiss (GD_NOTIFICATION (self->priv->notification));

        g_dbus_proxy_call (priv->session,
                           "Logout",
                           g_variant_new ("(u)", 0),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);
}

static void
show_restart_notification (CcRegionPanel *self,
                           const gchar   *locale)
{
	CcRegionPanelPrivate *priv = self->priv;
        GtkWidget *box;
        GtkWidget *label;
        GtkWidget *button;
        gchar *current_locale;

        if (priv->notification)
                return;

        if (locale) {
                current_locale = g_strdup (setlocale (LC_MESSAGES, NULL));
                setlocale (LC_MESSAGES, locale);
        }

        priv->notification = gd_notification_new ();
        g_object_add_weak_pointer (G_OBJECT (priv->notification),
                                   (gpointer *)&priv->notification);
        box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
        gtk_widget_set_margin_left (box, 12);
        gtk_widget_set_margin_right (box, 12);
        gtk_widget_set_margin_top (box, 6);
        gtk_widget_set_margin_bottom (box, 6);
        label = gtk_label_new (_("Your session needs to be restarted for changes to take effect"));
        button = gtk_button_new_with_label (_("Restart Now"));
        g_signal_connect_swapped (button, "clicked",
                                  G_CALLBACK (restart_now), self);
        gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
        gtk_widget_show_all (box);

        gtk_container_add (GTK_CONTAINER (priv->notification), box);
        gtk_overlay_add_overlay (GTK_OVERLAY (self->priv->overlay), priv->notification);
        gtk_widget_show (priv->notification);

        if (locale) {
                setlocale (LC_MESSAGES, current_locale);
                g_free (current_locale);
        }
}

static void
update_header_func (GtkListBoxRow  *row,
                    GtkListBoxRow  *before,
                    gpointer    user_data)
{
  GtkWidget *current;

  if (before == NULL)
    return;

  current = gtk_list_box_row_get_header (row);
  if (current == NULL)
    {
      current = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_show (current);
      gtk_list_box_row_set_header (row, current);
    }
}

typedef struct {
        CcRegionPanel *self;
        int category;
        gchar *target_locale;
} MaybeNotifyData;

static void
maybe_notify_finish (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        MaybeNotifyData *mnd = data;
        CcRegionPanel *self = mnd->self;
        GError *error = NULL;
        GVariant *retval = NULL;
        gchar *current_lang_code = NULL;
        gchar *current_country_code = NULL;
        gchar *target_lang_code = NULL;
        gchar *target_country_code = NULL;
        const gchar *current_locale = NULL;

        retval = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
        if (!retval) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to get locale: %s\n", error->message);
                goto out;
        }

        g_variant_get (retval, "(&s)", &current_locale);

        if (!gnome_parse_locale (current_locale,
                                 &current_lang_code,
                                 &current_country_code,
                                 NULL,
                                 NULL))
                goto out;

        if (!gnome_parse_locale (mnd->target_locale,
                                 &target_lang_code,
                                 &target_country_code,
                                 NULL,
                                 NULL))
                goto out;

        if (g_str_equal (current_lang_code, target_lang_code) == FALSE ||
            g_str_equal (current_country_code, target_country_code) == FALSE)
                show_restart_notification (self,
                                           mnd->category == LC_MESSAGES ? mnd->target_locale : NULL);
out:
        g_free (target_country_code);
        g_free (target_lang_code);
        g_free (current_country_code);
        g_free (current_lang_code);
        g_clear_pointer (&retval, g_variant_unref);
        g_clear_error (&error);
        g_free (mnd->target_locale);
        g_free (mnd);
}

static void
maybe_notify (CcRegionPanel *self,
              int            category,
              const gchar   *target_locale)
{
        CcRegionPanelPrivate *priv = self->priv;
        MaybeNotifyData *mnd;

        mnd = g_new0 (MaybeNotifyData, 1);
        mnd->self = self;
        mnd->category = category;
        mnd->target_locale = g_strdup (target_locale);

        g_dbus_proxy_call (priv->session,
                           "GetLocale",
                           g_variant_new ("(i)", category),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           priv->cancellable,
                           maybe_notify_finish,
                           mnd);
}

static void set_localed_locale (CcRegionPanel *self);

static void
set_system_language (CcRegionPanel *self,
                     const gchar   *language)
{
        CcRegionPanelPrivate *priv = self->priv;

        if (g_strcmp0 (language, priv->system_language) == 0)
                return;

        g_free (priv->system_language);
        priv->system_language = g_strdup (language);

        set_localed_locale (self);
}

static void
update_language (CcRegionPanel *self,
                 const gchar   *language)
{
	CcRegionPanelPrivate *priv = self->priv;

        if (priv->login) {
                set_system_language (self, language);
        } else {
                if (g_strcmp0 (language, priv->language) == 0)
                        return;
                act_user_set_language (priv->user, language);
                if (priv->login_auto_apply)
                        set_system_language (self, language);
                maybe_notify (self, LC_MESSAGES, language);
        }
}

static void
language_response (GtkDialog     *chooser,
                   gint           response_id,
                   CcRegionPanel *self)
{
        const gchar *language;

        if (response_id == GTK_RESPONSE_OK) {
                language = cc_language_chooser_get_language (GTK_WIDGET (chooser));
                update_language (self, language);
        }

        gtk_widget_destroy (GTK_WIDGET (chooser));
}

static void
update_region (CcRegionPanel *self,
               const gchar   *region)
{
	CcRegionPanelPrivate *priv = self->priv;

        if (g_strcmp0 (region, priv->region) == 0)
                return;

        g_settings_set_string (priv->locale_settings, KEY_REGION, region);
        maybe_notify (self, LC_TIME, region);
}

static void
format_response (GtkDialog *chooser,
                 gint       response_id,
                 CcRegionPanel *self)
{
        const gchar *region;

        if (response_id == GTK_RESPONSE_OK) {
                region = cc_format_chooser_get_region (GTK_WIDGET (chooser));
                update_region (self, region);
        }

        gtk_widget_destroy (GTK_WIDGET (chooser));
}

static void
show_language_chooser (CcRegionPanel *self,
                       const gchar   *language)
{
        GtkWidget *toplevel;
        GtkWidget *chooser;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
        chooser = cc_language_chooser_new (toplevel);
        cc_language_chooser_set_language (chooser, language);
        g_signal_connect (chooser, "response",
                          G_CALLBACK (language_response), self);
        gtk_window_present (GTK_WINDOW (chooser));
}

static void show_input_chooser (CcRegionPanel *self);

static void
permission_acquired (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        CcRegionPanel *self = data;
	CcRegionPanelPrivate *priv = self->priv;
        GError *error = NULL;
        gboolean allowed;

        allowed = g_permission_acquire_finish (priv->permission, res, &error);
        if (error) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to acquire permission: %s\n", error->message);
                g_error_free (error);
                return;
        }

        if (allowed) {
                switch (priv->op) {
                case CHOOSE_LANGUAGE:
                        show_language_chooser (self, priv->system_language);
                        break;
                case ADD_INPUT:
                        show_input_chooser (self);
                        break;
                default:
                        g_warning ("Unknown privileged operation: %d\n", priv->op);
                        break;
                }
        }
}

static void
show_format_chooser (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GtkWidget *toplevel;
        GtkWidget *chooser;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
        chooser = cc_format_chooser_new (toplevel);
        cc_format_chooser_set_region (chooser, priv->region);
        g_signal_connect (chooser, "response",
                          G_CALLBACK (format_response), self);
        gtk_window_present (GTK_WINDOW (chooser));
}

static void
activate_language_row (CcRegionPanel *self,
                       GtkListBoxRow *row)
{
	CcRegionPanelPrivate *priv = self->priv;

        if (row == priv->language_row) {
                if (!priv->login) {
                        show_language_chooser (self, priv->language);
                } else if (g_permission_get_allowed (priv->permission)) {
                        show_language_chooser (self, priv->system_language);
                } else if (g_permission_get_can_acquire (priv->permission)) {
                        priv->op = CHOOSE_LANGUAGE;
                        g_permission_acquire_async (priv->permission,
                                                    NULL,
                                                    permission_acquired,
                                                    self);
                }
        } else if (row == priv->formats_row) {
                show_format_chooser (self);
        } else if (row == priv->layouts_row) {
                add_input (self);
        }
}

static void
update_region_label (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;
        const gchar *region;
        gchar *name;

        if (priv->region == NULL || priv->region[0] == '\0')
                region = priv->language;
        else
                region = priv->region;

        name = gnome_get_country_from_locale (region, region);
        gtk_label_set_label (GTK_LABEL (priv->formats_label), name);
        g_free (name);
}

static void
update_region_from_setting (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;

        g_free (priv->region);
        priv->region = g_settings_get_string (priv->locale_settings, KEY_REGION);
        update_region_label (self);
}


static void
update_layouts_label (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;

        gtk_label_set_label (GTK_LABEL (priv->layouts_label),
                             priv->login? priv->system_layout_name : priv->layout_name);
}

static void
update_language_label (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        const gchar *language;
        gchar *name;

        if (priv->login)
                language = priv->system_language;
        else
                language = priv->language;
        if (language)
                name = gnome_get_language_from_locale (language, language);
        else
                name = g_strdup (C_("Language", "None"));
        gtk_label_set_label (GTK_LABEL (priv->language_label), name);
        g_free (name);

        /* Formats will change too if not explicitly set. */
        update_region_label (self);
}

static void
update_language_from_user (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        const gchar *language;

        if (act_user_is_loaded (priv->user))
                language = act_user_get_language (priv->user);
        else
                language = "en_US.utf-8";

        g_free (priv->language);
        priv->language = g_strdup (language);
        update_language_label (self);
}

static void
setup_language_section (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GtkWidget *widget;

        priv->user = act_user_manager_get_user_by_id (priv->user_manager, getuid ());
        g_signal_connect_swapped (priv->user, "notify::language",
                                  G_CALLBACK (update_language_from_user), self);
        g_signal_connect_swapped (priv->user, "notify::is-loaded",
                                  G_CALLBACK (update_language_from_user), self);

        priv->locale_settings = g_settings_new (GNOME_SYSTEM_LOCALE_DIR);
        g_signal_connect_swapped (priv->locale_settings, "changed::" KEY_REGION,
                                  G_CALLBACK (update_region_from_setting), self);

        priv->language_section = WID ("language_section");
        priv->language_row = GTK_LIST_BOX_ROW (WID ("language_row"));
        priv->language_label = WID ("language_label");
        priv->formats_row = GTK_LIST_BOX_ROW (WID ("formats_row"));
        priv->formats_label = WID ("formats_label");
        priv->layouts_row = GTK_LIST_BOX_ROW (WID ("layouts_row"));
        priv->layouts_label = WID ("layouts_label");

        widget = WID ("language_list");
        gtk_list_box_set_selection_mode (GTK_LIST_BOX (widget),
                                         GTK_SELECTION_NONE);
        gtk_list_box_set_header_func (GTK_LIST_BOX (widget),
                                      update_header_func,
                                      NULL, NULL);
        g_signal_connect_swapped (widget, "row-activated",
                                  G_CALLBACK (activate_language_row), self);

        update_language_from_user (self);
        update_region_from_setting (self);
}

#ifdef HAVE_IBUS
static void
update_input_chooser (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;
        GtkWidget *chooser;

        chooser = g_object_get_data (G_OBJECT (self), "input-chooser");
        if (!chooser)
                return;

        cc_input_chooser_set_ibus_engines (chooser, priv->ibus_engines);
}

static void
fetch_ibus_engines_result (GObject       *object,
                           GAsyncResult  *result,
                           CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;
        GList *list, *l;
        GError *error;

        error = NULL;
        list = ibus_bus_list_engines_async_finish (priv->ibus, result, &error);
        g_clear_object (&priv->ibus_cancellable);
        if (!list && error) {
                g_warning ("Couldn't finish IBus request: %s", error->message);
                g_error_free (error);
                return;
        }

        /* Maps engine ids to engine description objects */
        priv->ibus_engines = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

        for (l = list; l; l = l->next) {
                IBusEngineDesc *engine = l->data;
                const gchar *engine_id = ibus_engine_desc_get_name (engine);

                if (g_str_has_prefix (engine_id, "xkb:"))
                        g_object_unref (engine);
                else
                        g_hash_table_replace (priv->ibus_engines, (gpointer)engine_id, engine);
        }
        g_list_free (list);

        update_input_chooser (self);
}

static void
fetch_ibus_engines (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;

        priv->ibus_cancellable = g_cancellable_new ();

        ibus_bus_list_engines_async (priv->ibus,
                                     -1,
                                     priv->ibus_cancellable,
                                     (GAsyncReadyCallback)fetch_ibus_engines_result,
                                     self);

  /* We've got everything we needed, don't want to be called again. */
  g_signal_handlers_disconnect_by_func (priv->ibus, fetch_ibus_engines, self);
}

static void
maybe_start_ibus (void)
{
        /* IBus doesn't export API in the session bus. The only thing
         * we have there is a well known name which we can use as a
         * sure-fire way to activate it.
         */
        g_bus_unwatch_name (g_bus_watch_name (G_BUS_TYPE_SESSION,
                                              IBUS_SERVICE_IBUS,
                                              G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                              NULL,
                                              NULL,
                                              NULL,
                                              NULL));
}
#endif

static void
input_sources_changed (GSettings     *settings,
                       const gchar   *key,
                       CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;
        GVariant *sources;
        guint current;
        gchar *id;

        sources = g_settings_get_value (priv->input_settings, KEY_INPUT_SOURCES);
        current = g_settings_get_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE);
        g_free (priv->layout_id);
        g_free (priv->layout_name);
        g_variant_get_child (sources, current, "(&s&s)", &priv->layout_type, &id);
        priv->layout_id = g_strdup (id);
        if (g_str_equal (priv->layout_type, INPUT_SOURCE_TYPE_XKB)) {
                const gchar *name = NULL;
                gnome_xkb_info_get_layout_info (priv->xkb_info, priv->layout_id, &name, NULL, NULL, NULL);
                if (name)
                        priv->layout_name = g_strdup (name);
                else
                        priv->layout_name = g_strdup ("");
#ifdef HAVE_IBUS
        } else if (g_str_equal (priv->layout_type, INPUT_SOURCE_TYPE_IBUS)) {
                IBusEngineDesc *engine_desc = NULL;
                if (priv->ibus_engines)
                  engine_desc = g_hash_table_lookup (priv->ibus_engines, priv->layout_id);
                if (engine_desc)
                        priv->layout_name = engine_get_display_name (engine_desc);
                else
                        priv->layout_name = g_strdup ("");
#endif
        } else {
                priv->layout_name = g_strdup ("");
        }

        update_layouts_label (self);
}

static void
set_input_settings (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GVariantBuilder builder;

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
        g_variant_builder_add (&builder, "(ss)", priv->layout_type, priv->layout_id);
        g_settings_set_value (priv->input_settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));
        g_settings_set_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE, 0);
        g_settings_apply (priv->input_settings);
}

static void set_localed_input (CcRegionPanel *self);

static void
update_input (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;

        if (priv->login) {
                set_localed_input (self);
        } else {
                set_input_settings (self);
                if (priv->login_auto_apply)
                        set_localed_input (self);
        }
}

static void
apologize_for_no_ibus_login (CcRegionPanel *self)
{
        GtkWidget *dialog;
        GtkWidget *toplevel;
        GtkWidget *image;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));

        dialog = gtk_message_dialog_new (GTK_WINDOW (toplevel),
                                         GTK_DIALOG_MODAL,
                                         GTK_MESSAGE_OTHER,
                                         GTK_BUTTONS_OK,
                                         _("Sorry"));
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                  "%s", _("Input methods can't be used on the login screen"));
        image = gtk_image_new_from_icon_name ("face-sad-symbolic",
                                              GTK_ICON_SIZE_DIALOG);
        gtk_widget_show (image);
        gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dialog), image);

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
}

static void
input_response (GtkWidget *chooser, gint response_id, gpointer data)
{
	CcRegionPanel *self = data;
        CcRegionPanelPrivate *priv = self->priv;
        gchar *type;
        gchar *id;
        gchar *name;
        GDesktopAppInfo *app_info = NULL;

        if (response_id == GTK_RESPONSE_OK) {
                if (cc_input_chooser_get_selected (chooser, &type, &id, &name)) {
                        if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                                g_free (type);
                                type = INPUT_SOURCE_TYPE_IBUS;
                        } else {
                                g_free (type);
                                type = INPUT_SOURCE_TYPE_XKB;
                        }

                        if (priv->login && g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                                apologize_for_no_ibus_login (self);
                                priv->system_layout_type = INPUT_SOURCE_TYPE_IBUS;
                        } else {
                                if (priv->login) {
                                        priv->system_layout_type = type;
                                        priv->system_layout_id = g_strdup (id);
                                        priv->system_layout_name = g_strdup (name);
                                } else {
                                        priv->layout_type = type;
                                        priv->layout_id = g_strdup (id);
                                        priv->layout_name = g_strdup (name);
                                }
                                update_input (self);
                        }
                        g_free (id);
                        g_free (name);
                        g_clear_object (&app_info);
                }
        }
        gtk_widget_destroy (chooser);
        g_object_set_data (G_OBJECT (self), "input-chooser", NULL);
}

static void
show_input_chooser (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GtkWidget *chooser;
        GtkWidget *toplevel;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
        chooser = cc_input_chooser_new (GTK_WINDOW (toplevel),
                                        priv->xkb_info,
#ifdef HAVE_IBUS
                                        priv->ibus_engines
#else
                                        NULL
#endif
                );
        g_signal_connect (chooser, "response",
                          G_CALLBACK (input_response), self);
        gtk_window_present (GTK_WINDOW (chooser));

        g_object_set_data (G_OBJECT (self), "input-chooser", chooser);
}

static void
add_input (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;

        if (!priv->login) {
                show_input_chooser (self);
        } else if (g_permission_get_allowed (priv->permission)) {
                show_input_chooser (self);
        } else if (g_permission_get_can_acquire (priv->permission)) {
                priv->op = ADD_INPUT;
                g_permission_acquire_async (priv->permission,
                                            NULL,
                                            permission_acquired,
                                            self);
        }
}

static void
setup_input_section (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;

        priv->input_settings = g_settings_new (GNOME_DESKTOP_INPUT_SOURCES_DIR);
        g_settings_delay (priv->input_settings);

        priv->xkb_info = gnome_xkb_info_new ();

#ifdef HAVE_IBUS
        ibus_init ();
        if (!priv->ibus) {
                priv->ibus = ibus_bus_new_async ();
                if (ibus_bus_is_connected (priv->ibus))
                        fetch_ibus_engines (self);
                else
                        g_signal_connect_swapped (priv->ibus, "connected",
                                                  G_CALLBACK (fetch_ibus_engines), self);
        }
        maybe_start_ibus ();
#endif

        g_signal_connect (priv->input_settings, "changed::" KEY_INPUT_SOURCES,
                          G_CALLBACK (input_sources_changed), self);

        input_sources_changed (priv->input_settings, KEY_INPUT_SOURCES, self);
}

static void
on_localed_properties_changed (GDBusProxy     *proxy,
                               GVariant       *changed_properties,
                               const gchar   **invalidated_properties,
                               CcRegionPanel  *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GVariant *v;
        const gchar *s;
        gchar **layouts = NULL;
        gchar **variants = NULL;

        v = g_dbus_proxy_get_cached_property (proxy, "Locale");
        if (v) {
                const gchar **strv;
                gsize len;
                gint i;
                const gchar *lang, *messages, *time;

                strv = g_variant_get_strv (v, &len);

                lang = messages = time = NULL;
                for (i = 0; strv[i]; i++) {
                        if (g_str_has_prefix (strv[i], "LANG=")) {
                                lang = strv[i] + strlen ("LANG=");
                        } else if (g_str_has_prefix (strv[i], "LC_MESSAGES=")) {
                                messages = strv[i] + strlen ("LC_MESSAGES=");
                        } else if (g_str_has_prefix (strv[i], "LC_TIME=")) {
                                time = strv[i] + strlen ("LC_TIME=");
                        }
                }
                if (!lang) {
                        lang = "";
                }
                if (!messages) {
                        messages = lang;
                }
                if (!time) {
                        time = lang;
                }
                g_free (priv->system_language);
                priv->system_language = g_strdup (messages);
                g_free (priv->system_region);
                priv->system_region = g_strdup (time);
                g_variant_unref (v);

                update_language_label (self);
        }

        v = g_dbus_proxy_get_cached_property (priv->localed, "X11Layout");
        if (v) {
                s = g_variant_get_string (v, NULL);
                layouts = g_strsplit (s, ",", -1);
                g_variant_unref (v);
        }

        v = g_dbus_proxy_get_cached_property (priv->localed, "X11Variant");
        if (v) {
                s = g_variant_get_string (v, NULL);
                if (s && *s)
                        variants = g_strsplit (s, ",", -1);
                g_variant_unref (v);
        }

        g_free (priv->system_layout_id);
        g_free (priv->system_layout_name);
        if (layouts && layouts[0] && layouts[0][0]) {
          const gchar *name;
          gchar *id;
          if (variants && variants[0] && variants[0][0])
            id = g_strdup_printf("%s+%s", layouts[0], variants[0]);
          else
            id = g_strdup (layouts[0]);

          gnome_xkb_info_get_layout_info (priv->xkb_info, id, &name, NULL, NULL, NULL);
          priv->system_layout_type = INPUT_SOURCE_TYPE_XKB;
          priv->system_layout_id = id;
          priv->system_layout_name = name ? g_strdup (name) : g_strdup (id);
        } else {
          priv->system_layout_type = "none";
          priv->system_layout_id = g_strdup ("none");
          priv->system_layout_name = g_strdup (_("No input source selected"));
        }

        g_strfreev (variants);
        g_strfreev (layouts);

        update_layouts_label (self);
}

static void
set_localed_locale (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GVariantBuilder *b;
        gchar *s;

        b = g_variant_builder_new (G_VARIANT_TYPE ("as"));
        s = g_strconcat ("LANG=", priv->system_language, NULL);
        g_variant_builder_add (b, "s", s);
        g_free (s);

        if (g_strcmp0 (priv->system_language, priv->system_region) != 0) {
                s = g_strconcat ("LC_TIME=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_NUMERIC=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_MONETARY=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_MEASUREMENT=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
                s = g_strconcat ("LC_PAPER=", priv->system_region, NULL);
                g_variant_builder_add (b, "s", s);
                g_free (s);
        }
        g_dbus_proxy_call (priv->localed,
                           "SetLocale",
                           g_variant_new ("(asb)", b, TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);
        g_variant_builder_unref (b);
}

static void
set_localed_input (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GString *layouts;
        GString *variants;
        const gchar *l, *v;

        layouts = g_string_new ("");
        variants = g_string_new ("");

        if (!g_str_equal (priv->system_layout_type, INPUT_SOURCE_TYPE_IBUS)) {
          if (gnome_xkb_info_get_layout_info (priv->xkb_info, priv->system_layout_id, NULL, NULL, &l, &v)) {
            g_string_append (layouts, l);
            g_string_append (variants, v);
          }
        }

        g_dbus_proxy_call (priv->localed,
                           "SetX11Keyboard",
                           g_variant_new ("(ssssbb)", layouts->str, "", variants->str, "", TRUE, TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);

        g_string_free (layouts, TRUE);
        g_string_free (variants, TRUE);
}

static void
localed_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        CcRegionPanel *self = data;
        CcRegionPanelPrivate *priv;
        GDBusProxy *proxy;
        GError *error = NULL;

        proxy = g_dbus_proxy_new_finish (res, &error);

        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact localed: %s\n", error->message);
                g_error_free (error);
                return;
        }

        priv = self->priv;
        priv->localed = proxy;

        g_signal_connect (priv->localed, "g-properties-changed",
                          G_CALLBACK (on_localed_properties_changed), self);
        on_localed_properties_changed (priv->localed, NULL, NULL, self);
}

static void
login_changed (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        gboolean can_acquire;

        priv->login = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->login_language_button));
        gtk_widget_set_visible (GTK_WIDGET (priv->formats_row), !priv->login);
        gtk_widget_set_visible (priv->login_label, priv->login);

        can_acquire = priv->permission &&
                (g_permission_get_allowed (priv->permission) ||
                 g_permission_get_can_acquire (priv->permission));
        /* FIXME: insensitive doesn't look quite right for this */
        gtk_widget_set_sensitive (priv->language_section, !priv->login || can_acquire);

        update_language_label (self);
        update_layouts_label (self);
}

static void
set_login_button_visibility (CcRegionPanel *self)
{
        CcRegionPanelPrivate *priv = self->priv;
        gboolean has_multiple_users;
        gboolean loaded;

        g_object_get (priv->user_manager, "is-loaded", &loaded, NULL);
        if (!loaded)
          return;

        g_object_get (priv->user_manager, "has-multiple-users", &has_multiple_users, NULL);

        priv->login_auto_apply = !has_multiple_users && g_permission_get_allowed (priv->permission);
        gtk_widget_set_visible (priv->hbox_selector, !priv->login_auto_apply);

        g_signal_handlers_disconnect_by_func (priv->user_manager, set_login_button_visibility, self);
}

static void
setup_login_button (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv = self->priv;
        GDBusConnection *bus;
        gboolean loaded;

        priv->permission = polkit_permission_new_sync ("org.freedesktop.locale1.set-locale", NULL, NULL, NULL);
        bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
        g_dbus_proxy_new (bus,
                          G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                          NULL,
                          "org.freedesktop.locale1",
                          "/org/freedesktop/locale1",
                          "org.freedesktop.locale1",
                          priv->cancellable,
                          (GAsyncReadyCallback) localed_proxy_ready,
                          self);
        g_object_unref (bus);

        priv->login_label = WID ("login-label");
        priv->hbox_selector = WID ("hbox-selector");
        priv->session_language_button = WID ("session-language-button");
        priv->login_language_button = WID ("login-language-button");

        g_signal_connect_swapped (priv->login_language_button, "notify::active",
                                  G_CALLBACK (login_changed), self);

        g_object_get (priv->user_manager, "is-loaded", &loaded, NULL);
        if (loaded)
                set_login_button_visibility (self);
        else
                g_signal_connect_swapped (priv->user_manager, "notify::is-loaded",
                                          G_CALLBACK (set_login_button_visibility), self);
}

static void
session_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        CcRegionPanel *self = data;
        GDBusProxy *proxy;
        GError *error = NULL;

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);

        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact gnome-session: %s\n", error->message);
                g_error_free (error);
                return;
        }

        self->priv->session = proxy;
}

static void
cc_region_panel_init (CcRegionPanel *self)
{
	CcRegionPanelPrivate *priv;
	GError *error = NULL;

	priv = self->priv = REGION_PANEL_PRIVATE (self);
        g_resources_register (cc_region_get_resource ());

	priv->builder = gtk_builder_new ();

	gtk_builder_add_from_resource (priv->builder,
                                       "/org/gnome/control-center/region/region.ui",
                                       &error);
	if (error != NULL) {
		g_warning ("Error loading UI file: %s", error->message);
		g_error_free (error);
		return;
	}

        priv->user_manager = act_user_manager_get_default ();

        priv->cancellable = g_cancellable_new ();

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  NULL,
                                  "org.gnome.SessionManager",
                                  "/org/gnome/SessionManager",
                                  "org.gnome.SessionManager",
                                  priv->cancellable,
                                  session_proxy_ready,
                                  self);

        setup_login_button (self);
        setup_language_section (self);
        setup_input_section (self);

        priv->overlay = GTK_WIDGET (gtk_builder_get_object (priv->builder, "overlay"));
	gtk_container_add (GTK_CONTAINER (self), priv->overlay);
}
