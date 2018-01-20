/* cc-updates-panel.c
 *
 * Copyright © 2018 Endless, Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Georges Basile Stavracas Neto <georges@endlessm.com>
 */

#include "cc-tariff-editor.h"
#include "cc-updates-panel.h"
#include "cc-updates-resources.h"

#include <glib/gi18n.h>
#include <libmogwai-tariff/tariff-loader.h>
#include <NetworkManager.h>

#define NM_SETTING_ALLOW_DOWNLOADS_WHEN_METERED "connection.allow-downloads-when-metered"
#define SYSTEM_TARIFF_FILENAME                  "system-tariff"

struct _CcUpdatesPanel
{
  CcPanel             parent;

  GtkWidget          *automatic_updates_container;
  GtkWidget          *automatic_updates_switch;
  GtkWidget          *metered_data_label;
  GtkWidget          *network_name_label;
  GtkWidget          *network_settings_label;
  GtkWidget          *network_status_icon;
  GtkWidget          *scheduled_updates_switch;
  CcTariffEditor     *tariff_editor;

  /* Network Manager */
  NMClient           *nm_client;
  NMDevice           *current_device;

  /* Signal handlers */
  guint               changed_id;
  guint               save_tariff_timeout_id;

  GCancellable       *cancellable;
};


static void          on_automatic_updates_switch_changed_cb      (GtkSwitch      *sw,
                                                                  GParamSpec     *pspec,
                                                                  CcUpdatesPanel *self);

static void          on_network_changed_cb                       (CcUpdatesPanel *self);

static void          on_network_changes_commited_cb              (GObject        *source,
                                                                  GAsyncResult   *result,
                                                                  gpointer        user_data);

static void          on_scheduled_updates_switch_changed_cb      (GtkSwitch      *sw,
                                                                  GParamSpec     *pspec,
                                                                  CcUpdatesPanel *self);

static void          on_tariff_changed_cb                        (CcTariffEditor *tariff_editor,
                                                                  CcUpdatesPanel *self);

static void          on_tariff_file_deleted_cb                   (GFile          *file,
                                                                  GAsyncResult   *result,
                                                                  CcUpdatesPanel *self);


G_DEFINE_TYPE (CcUpdatesPanel, cc_updates_panel, CC_TYPE_PANEL)

enum
{
  PROP_PARAMETERS = 1,
  N_PROPS
};


/*
 * Auxiliary methods
 */

static void
set_automatic_updates_setting (NMConnection   *connection,
                               gboolean        enabled)
{
  NMSettingUser *setting_user;
  g_autoptr(GError) error = NULL;

  g_debug ("Setting "NM_SETTING_ALLOW_DOWNLOADS_WHEN_METERED" to %d", enabled);

  setting_user = NM_SETTING_USER (nm_connection_get_setting (connection, NM_TYPE_SETTING_USER));

  nm_setting_user_set_data (setting_user,
                            NM_SETTING_ALLOW_DOWNLOADS_WHEN_METERED,
                            enabled ? "1" : "0",
                            &error);

  if (error)
    {
      g_warning ("Error storing "NM_SETTING_ALLOW_DOWNLOADS_WHEN_METERED": %s", error->message);
      return;
    }

  nm_remote_connection_commit_changes_async (NM_REMOTE_CONNECTION (connection),
                                             TRUE, /* save to disk */
                                             NULL,
                                             on_network_changes_commited_cb,
                                             NULL);
}

static void
get_active_connection_and_device (CcUpdatesPanel  *self,
                                  NMDevice       **out_device,
                                  NMConnection   **out_connection,
                                  NMAccessPoint  **out_ap)
{
  NMActiveConnection *active_connection = NULL;
  NMConnection *connection = NULL;
  const GPtrArray *active_devices = NULL;
  NMAccessPoint *ap = NULL;
  NMDevice *active_device = NULL;

  active_connection = nm_client_get_primary_connection (self->nm_client);

  /* If no primary connection is already present, try and use the connecting one */
  if (!active_connection)
    active_connection = nm_client_get_activating_connection (self->nm_client);

  if (active_connection)
    {
      connection = NM_CONNECTION (nm_active_connection_get_connection (active_connection));
      active_devices = nm_active_connection_get_devices (active_connection);

      if (active_devices && active_devices->len > 0)
        {
          /* This array is guaranteed to have only one element */
          active_device = g_ptr_array_index (active_devices, 0);

          if (NM_IS_DEVICE_WIFI (active_device))
            ap = nm_device_wifi_get_active_access_point (NM_DEVICE_WIFI (active_device));
        }
    }

  if (out_device)
    *out_device = active_device;

  if (out_connection)
    *out_connection = connection;

  if (out_ap)
    *out_ap = ap;
}

static void
ensure_setting_user (CcUpdatesPanel *self,
                     NMConnection   *connection)
{
  NMSettingUser *setting_user;

  if (!connection)
    return;

  setting_user = NM_SETTING_USER (nm_connection_get_setting (connection, NM_TYPE_SETTING_USER));

  if (!setting_user)
    {
      g_autoptr(GError) error = NULL;
      NMSettingConnection *setting;
      NMMetered metered;
      gboolean enabled;

      g_debug ("Creating a new custom config file for the current connection…");

      /* Add a new NMSettingUser to this connection */
      setting_user = NM_SETTING_USER (nm_setting_user_new ());
      nm_connection_add_setting (connection, NM_SETTING (setting_user));

      /* The default value depends on the metered state of the connection */
      setting = nm_connection_get_setting_connection (connection);
      metered = nm_setting_connection_get_metered (setting);
      enabled = metered != NM_METERED_YES && metered != NM_METERED_GUESS_YES;

      set_automatic_updates_setting (connection, enabled);

      g_signal_handlers_block_by_func (self->automatic_updates_switch,
                                       on_automatic_updates_switch_changed_cb,
                                       self);

      gtk_switch_set_active (GTK_SWITCH (self->automatic_updates_switch), enabled);

      g_signal_handlers_unblock_by_func (self->automatic_updates_switch,
                                         on_automatic_updates_switch_changed_cb,
                                         self);

      if (error)
        g_warning ("Error creating custom config for connection: %s", error->message);
    }
}

static const gchar *
get_wifi_icon_from_strength (NMAccessPoint *ap)
{
  guint8 strength;

  if (!ap)
    return "network-wireless-signal-none-symbolic";

  strength = nm_access_point_get_strength (ap);

  if (strength < 20)
    return "network-wireless-signal-none-symbolic";
  else if (strength < 40)
    return "network-wireless-signal-weak-symbolic";
  else if (strength < 50)
    return "network-wireless-signal-ok-symbolic";
  else if (strength < 80)
    return "network-wireless-signal-good-symbolic";
  else
    return "network-wireless-signal-excellent-symbolic";
}

static const gchar *
get_network_status_icon_name (CcUpdatesPanel *self,
                              NMDevice       *device,
                              NMAccessPoint  *ap)
{
  if (NM_IS_DEVICE_WIFI (device))
    {
      switch (nm_client_get_state (self->nm_client))
        {
        case NM_STATE_UNKNOWN:
        case NM_STATE_ASLEEP:
        case NM_STATE_DISCONNECTING:
          return "network-wireless-offline-symbolic";

        case NM_STATE_DISCONNECTED:
          return "network-wireless-offline-symbolic";

        case NM_STATE_CONNECTING:
          return "network-wireless-acquiring-symbolic";

        case NM_STATE_CONNECTED_LOCAL:
        case NM_STATE_CONNECTED_SITE:
          return "network-wireless-no-route-symbolic";

        case NM_STATE_CONNECTED_GLOBAL:
          return get_wifi_icon_from_strength (ap);
        }
    }
  else
    {
      switch (nm_client_get_state (self->nm_client))
        {
        case NM_STATE_UNKNOWN:
        case NM_STATE_ASLEEP:
        case NM_STATE_DISCONNECTING:
          return "network-wired-disconnected-symbolic";

        case NM_STATE_DISCONNECTED:
          return "network-wired-offline-symbolic";

        case NM_STATE_CONNECTING:
          return "network-wired-acquiring-symbolic";

        case NM_STATE_CONNECTED_LOCAL:
        case NM_STATE_CONNECTED_SITE:
          return "network-wired-no-route-symbolic";

        case NM_STATE_CONNECTED_GLOBAL:
          return "network-wired-symbolic";
        }
    }

  return "network-wireless-offline-symbolic";
}

static void
update_active_network (CcUpdatesPanel *self)
{
  NMAccessPoint *ap;
  NMConnection *connection;
  NMDevice *device;
  const gchar *icon_name;

  get_active_connection_and_device (self, &device, &connection, &ap);

  /* Setup the new device */
  if (g_set_object (&self->current_device, device) && device)
    {
      /* Cleanup previous device handler */
      if (self->changed_id > 0)
        {
          g_source_remove (self->changed_id);
          self->changed_id = 0;
        }

      self->changed_id = g_signal_connect_swapped (device,
                                                   "state-changed",
                                                   G_CALLBACK (on_network_changed_cb),
                                                   self);
    }

  /* Icon */
  icon_name = get_network_status_icon_name (self, device, ap);
  gtk_image_set_from_icon_name (GTK_IMAGE (self->network_status_icon), icon_name, 4);

  /* Name */
  if (ap)
    {
      GBytes *ssid = nm_access_point_get_ssid (ap);
      g_autofree gchar *title = nm_utils_ssid_to_utf8 (g_bytes_get_data (ssid, NULL), g_bytes_get_size (ssid));

      gtk_label_set_label (GTK_LABEL (self->network_name_label), title);
    }
  else
    {
      if (connection)
        gtk_label_set_label (GTK_LABEL (self->network_name_label), _("Connected"));
      else
        gtk_label_set_label (GTK_LABEL (self->network_name_label), _("No active connection"));
    }

  /* Metered status */
  gtk_widget_set_visible (self->metered_data_label, connection != NULL);

  if (connection)
    {
      NMSettingConnection *setting;
      NMMetered metered;

      setting = nm_connection_get_setting_connection (connection);
      metered = nm_setting_connection_get_metered (setting);

      if (metered == NM_METERED_YES || metered == NM_METERED_GUESS_YES)
        gtk_label_set_label (GTK_LABEL (self->metered_data_label), _("Limited data plan connection"));
      else
        gtk_label_set_label (GTK_LABEL (self->metered_data_label), _("Unlimited data plan connection"));
    }

  /* Automatic Updates switch */
  gtk_widget_set_sensitive (self->automatic_updates_container, connection != NULL);

  ensure_setting_user (self, connection);

  if (connection)
    {
      NMSettingUser *setting_user;
      const gchar *value;

      setting_user = NM_SETTING_USER (nm_connection_get_setting (connection, NM_TYPE_SETTING_USER));
      value = nm_setting_user_get_data (setting_user, NM_SETTING_ALLOW_DOWNLOADS_WHEN_METERED);

      g_signal_handlers_block_by_func (self->automatic_updates_switch,
                                       on_automatic_updates_switch_changed_cb,
                                       self);

      gtk_switch_set_active (GTK_SWITCH (self->automatic_updates_switch), g_strcmp0 (value, "1") == 0);

      g_signal_handlers_unblock_by_func (self->automatic_updates_switch,
                                         on_automatic_updates_switch_changed_cb,
                                         self);
    }
}

static void
load_tariff (CcUpdatesPanel *self)
{
  g_autoptr(MwtTariffLoader) tariff_loader = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *tariff_path = NULL;
  gchar *contents = NULL;
  gsize size = 0;

  g_debug ("Loading tariff");

  tariff_path = g_build_filename (g_get_user_config_dir (), SYSTEM_TARIFF_FILENAME, NULL);
  g_file_get_contents (tariff_path, &contents, &size, &error);
  if (error)
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Error reading tariff file: %s", error->message);
      return;
    }

  bytes = g_bytes_new_take (contents, size);
  tariff_loader = mwt_tariff_loader_new ();
  mwt_tariff_loader_load_from_bytes (tariff_loader, bytes, &error);
  if (error)
    {
      g_warning ("Error loading tariff: %s", error->message);
      return;
    }

  g_signal_handlers_block_by_func (self->tariff_editor, on_tariff_changed_cb, self);
  g_signal_handlers_block_by_func (self->scheduled_updates_switch, on_scheduled_updates_switch_changed_cb, self);

  cc_tariff_editor_load_tariff (self->tariff_editor,
                                mwt_tariff_loader_get_tariff (tariff_loader),
                                &error);

  if (error)
    g_warning ("Error loading tariff: %s", error->message);
  else
    gtk_switch_set_active (GTK_SWITCH (self->scheduled_updates_switch), TRUE);

  g_signal_handlers_unblock_by_func (self->scheduled_updates_switch, on_scheduled_updates_switch_changed_cb, self);
  g_signal_handlers_unblock_by_func (self->tariff_editor, on_tariff_changed_cb, self);
}

static void
save_tariff (CcUpdatesPanel *self)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *tariff_path = NULL;
  GBytes *tariff_bytes;

  tariff_path = g_build_filename (g_get_user_config_dir (), SYSTEM_TARIFF_FILENAME, NULL);
  tariff_bytes = cc_tariff_editor_get_tariff_as_bytes (self->tariff_editor);

  if (!tariff_bytes || !gtk_switch_get_active (GTK_SWITCH (self->scheduled_updates_switch)))
    {
      g_autoptr(GFile) file = g_file_new_for_path (tariff_path);

      g_debug ("Deleting tariff");

      g_file_delete_async (file,
                           G_PRIORITY_LOW,
                           NULL,
                           (GAsyncReadyCallback) on_tariff_file_deleted_cb,
                           &error);
      return;
    }

  g_debug ("Saving tariff");

  g_file_set_contents (tariff_path,
                       g_bytes_get_data (tariff_bytes, NULL),
                       g_bytes_get_size (tariff_bytes),
                       &error);

  if (error)
    g_critical ("Error saving tariff file: %s", error->message);
}

static gboolean
save_tariff_cb (gpointer user_data)
{
  CcUpdatesPanel *self = CC_UPDATES_PANEL (user_data);

  save_tariff (self);
  self->save_tariff_timeout_id = 0;

  return G_SOURCE_REMOVE;
}

static void
schedule_save_tariff (CcUpdatesPanel *self)
{
  g_debug ("Scheduling save tariff");

  if (self->save_tariff_timeout_id > 0)
    g_source_remove (self->save_tariff_timeout_id);

  self->save_tariff_timeout_id = g_timeout_add_seconds (2, save_tariff_cb, self);
}


/*
 * Callbacks
 */

static void
on_automatic_updates_switch_changed_cb (GtkSwitch      *sw,
                                        GParamSpec     *pspec,
                                        CcUpdatesPanel *self)
{
  NMConnection *connection;
  gboolean active;

  active = gtk_switch_get_active (sw);
  get_active_connection_and_device (self, NULL, &connection, NULL);

  set_automatic_updates_setting (connection, active);

  /* Disable scheduled updates if automatic updates are disabled too */
  if (!active)
    gtk_switch_set_active (GTK_SWITCH (self->scheduled_updates_switch), FALSE);
}

static gboolean
on_change_network_link_activated_cb (GtkLabel       *label,
                                     gchar          *uri,
                                     CcUpdatesPanel *self)
{
  g_autoptr(GError) error = NULL;
  NMDevice *device;
  CcShell *shell;

  shell = cc_panel_get_shell (CC_PANEL (self));

  get_active_connection_and_device (self, &device, NULL, NULL);

  if (!device || !NM_IS_DEVICE_WIFI (device))
    cc_shell_set_active_panel_from_id (shell, "network", NULL, &error);
  else
    cc_shell_set_active_panel_from_id (shell, "wifi", NULL, &error);

  if (error)
    {
      g_warning ("Error activating panel: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

static void
on_network_changed_cb (CcUpdatesPanel *self)
{
  g_debug ("NetworkManager changed state");

  update_active_network (self);
}

static void
on_network_changes_commited_cb (GObject      *source,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  g_autoptr(GError) error = NULL;

  nm_remote_connection_commit_changes_finish (NM_REMOTE_CONNECTION (source), result, &error);

  if (error)
    g_warning ("Error storing "NM_SETTING_ALLOW_DOWNLOADS_WHEN_METERED": %s", error->message);
}

static void
on_scheduled_updates_switch_changed_cb (GtkSwitch      *sw,
                                        GParamSpec     *pspec,
                                        CcUpdatesPanel *self)
{
  g_debug ("Scheduled Updates changed state");

  schedule_save_tariff (self);
}

static void
on_tariff_changed_cb (CcTariffEditor *tariff_editor,
                      CcUpdatesPanel *self)
{
  g_debug ("The saved tariff changed");

  schedule_save_tariff (self);
}

static void
on_tariff_file_deleted_cb (GFile          *file,
                           GAsyncResult   *result,
                           CcUpdatesPanel *self)
{
  g_autoptr(GError) error = NULL;

  g_file_delete_finish (file, result, &error);

  if (error && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    g_critical ("Error deleting tariff file: %s", error->message);
}


/*
 * GObject overrides
 */

static void
cc_updates_panel_dispose (GObject *object)
{
  CcUpdatesPanel *self = (CcUpdatesPanel *)object;

  if (self->save_tariff_timeout_id > 0)
    {
      save_tariff (self);
      g_source_remove (self->save_tariff_timeout_id);
      self->save_tariff_timeout_id = 0;
    }

  G_OBJECT_CLASS (cc_updates_panel_parent_class)->dispose (object);
}

static void
cc_updates_panel_finalize (GObject *object)
{
  CcUpdatesPanel *self = (CcUpdatesPanel *)object;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->current_device);
  g_clear_object (&self->nm_client);

  G_OBJECT_CLASS (cc_updates_panel_parent_class)->finalize (object);
}

static void
cc_updates_panel_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  switch (prop_id)
    {
    case PROP_PARAMETERS:
      /* Nothing to do */
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cc_updates_panel_class_init (CcUpdatesPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = cc_updates_panel_dispose;
  object_class->finalize = cc_updates_panel_finalize;
  object_class->set_property = cc_updates_panel_set_property;

  g_object_class_override_property (object_class, PROP_PARAMETERS, "parameters");

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/control-center/updates/cc-updates-panel.ui");

  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, automatic_updates_container);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, automatic_updates_switch);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, metered_data_label);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, network_name_label);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, network_settings_label);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, network_status_icon);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, scheduled_updates_switch);
  gtk_widget_class_bind_template_child (widget_class, CcUpdatesPanel, tariff_editor);

  gtk_widget_class_bind_template_callback (widget_class, on_automatic_updates_switch_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_change_network_link_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_scheduled_updates_switch_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_tariff_changed_cb);

  g_type_ensure (CC_TYPE_TARIFF_EDITOR);
}

static void
cc_updates_panel_init (CcUpdatesPanel *self)
{
  g_autofree gchar *settings_text = NULL;

  g_resources_register (cc_updates_get_resource ());

  gtk_widget_init_template (GTK_WIDGET (self));

  /* "Change Network Settings" label */
  settings_text = g_strdup_printf ("<a href=\"\">%s</a>", _("Change Network Settings…"));
  gtk_label_set_markup (GTK_LABEL (self->network_settings_label), settings_text);

  /* Load any saved tariff */
  load_tariff (self);

  /* Setup network manager */
  self->nm_client = nm_client_new (NULL, NULL);

  update_active_network (self);

  g_signal_connect_swapped (self->nm_client, "notify", G_CALLBACK (on_network_changed_cb), self);
}
