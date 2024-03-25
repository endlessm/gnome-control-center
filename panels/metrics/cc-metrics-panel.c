/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2020 Endless Mobile
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Umang Jain <umang@endlessm.com>
 */

#include "cc-metrics-panel.h"
#include "cc-metrics-resources.h"
#include "cc-util.h"
#include "shell/cc-application.h"

#include <glib/gi18n.h>
#include <polkit/polkit.h>

struct _CcMetricsPanel
{
  CcPanel        parent_instance;

  GtkStack      *stack;
  GtkWidget     *enable_metrics_switch;

  AdwStatusPage      *metrics_disabled_page;
  AdwPreferencesPage *preferences_page;

  gboolean       changing_state;

  GDBusProxy    *metrics_proxy;
};

CC_PANEL_REGISTER (CcMetricsPanel, cc_metrics_panel)

static void
on_metrics_proxy_set_enabled_finish (GObject      *source_object,
                                     GAsyncResult *res,
                                     gpointer      user_data)
{
  CcMetricsPanel *self = CC_METRICS_PANEL (user_data);
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;

  results = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                      res,
                                      &error);
  if (results == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to enable/disable metrics: %s", error->message);

      return;
    }

  self->changing_state = FALSE;
}

static gboolean
on_metrics_switch_state_set (GtkSwitch *widget,
                             gboolean   new_state,
                             gpointer   user_data)
{
  CcMetricsPanel *self = CC_METRICS_PANEL (user_data);

  if (self->changing_state)
    return TRUE;

  self->changing_state = TRUE;
  g_dbus_proxy_call (self->metrics_proxy,
                     "SetEnabled",
                     g_variant_new ("(b)", new_state),
                     G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                     -1,
                     cc_panel_get_cancellable (CC_PANEL (self)),
                     on_metrics_proxy_set_enabled_finish,
                     self);
  return TRUE;
}

static void
cc_metrics_panel_enable_sync (CcMetricsPanel *self)
{
  g_autoptr(GVariant) value = NULL;
  gboolean metrics_enabled;

  value = g_dbus_proxy_get_cached_property (self->metrics_proxy, "Enabled");
  metrics_enabled = g_variant_get_boolean (value);
  g_clear_pointer (&value, g_variant_unref);

  g_signal_handlers_block_by_func (G_OBJECT (self->enable_metrics_switch),
                                   on_metrics_switch_state_set,
                                   self);

  gtk_switch_set_state (GTK_SWITCH (self->enable_metrics_switch), metrics_enabled);
  gtk_switch_set_active (GTK_SWITCH (self->enable_metrics_switch), metrics_enabled);

  g_signal_handlers_unblock_by_func (G_OBJECT (self->enable_metrics_switch),
                                     on_metrics_switch_state_set,
                                     self);
}

static void
on_metrics_proxy_properties_changed (GDBusProxy     *proxy,
                                     GVariant       *changed_properties,
                                     GStrv           invalidated_properties,
                                     CcMetricsPanel *self)
{
  cc_metrics_panel_enable_sync (self);
}

static gboolean
on_attribution_label_link (GtkLinkButton  *link_button,
                           CcMetricsPanel *self)
{
  CcShell *shell;
  GtkWindow *toplevel;

  shell = cc_panel_get_shell (CC_PANEL (self));
  toplevel = GTK_WINDOW (cc_shell_get_toplevel (shell));
  return cc_util_show_endless_terms_of_use (toplevel);
}

static gboolean
metrics_enabled_to_visible_child (GBinding     *binding,
                                  const GValue *from,
                                  GValue       *to,
                                  gpointer      user_data)
{
  CcMetricsPanel *self = CC_METRICS_PANEL (user_data);

  if (g_value_get_boolean (from))
    g_value_set_object (to, self->preferences_page);
  else
    g_value_set_object (to, self->metrics_disabled_page);
  return TRUE;
}

static void
cc_metrics_panel_dispose (GObject *object)
{
  CcMetricsPanel *self = CC_METRICS_PANEL (object);

  if (self->metrics_proxy)
    {
      g_signal_handlers_disconnect_by_func (self->metrics_proxy,
                                            on_metrics_proxy_properties_changed,
                                            self);
      g_clear_object (&self->metrics_proxy);
    }

  G_OBJECT_CLASS (cc_metrics_panel_parent_class)->dispose (object);
}

static void
cc_metrics_panel_constructed (GObject *object)
{
  CcMetricsPanel *self = CC_METRICS_PANEL (object);
  g_autoptr(GError) error = NULL;
  gboolean metrics_can_change = FALSE;
  GtkWidget *box;
  g_autoptr(GPermission) permission = NULL;
  g_autoptr(GVariant) value = NULL;

  G_OBJECT_CLASS (cc_metrics_panel_parent_class)->constructed (object);

  self->metrics_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       NULL,
                                                       "com.endlessm.Metrics",
                                                       "/com/endlessm/Metrics",
                                                       "com.endlessm.Metrics.EventRecorderServer",
                                                       NULL, &error);
  if (error != NULL)
    {
      g_warning ("Unable to create a D-Bus proxy for the metrics daemon: %s", error->message);
    }
  else
    {
      g_signal_connect (self->metrics_proxy, "g-properties-changed",
                        G_CALLBACK (on_metrics_proxy_properties_changed), self);
    }

  permission = polkit_permission_new_sync ("com.endlessm.Metrics.SetEnabled",
                                           NULL, NULL, NULL);
  if (permission)
    metrics_can_change = g_permission_get_allowed (permission);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_visible (box, true);

  gtk_widget_set_sensitive (GTK_WIDGET (self->enable_metrics_switch), metrics_can_change);

  cc_metrics_panel_enable_sync (self);

  g_object_bind_property_full (self->enable_metrics_switch,
                               "state",
                               self->stack,
                               "visible-child",
                               G_BINDING_SYNC_CREATE,
                               metrics_enabled_to_visible_child,
                               NULL,
                               self,
                               NULL);
}

static void
cc_metrics_panel_class_init (CcMetricsPanelClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  oclass->constructed = cc_metrics_panel_constructed;
  oclass->dispose = cc_metrics_panel_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/control-center/metrics/cc-metrics-panel.ui");

  gtk_widget_class_bind_template_child (widget_class, CcMetricsPanel, stack);
  gtk_widget_class_bind_template_child (widget_class, CcMetricsPanel, enable_metrics_switch);
  gtk_widget_class_bind_template_child (widget_class, CcMetricsPanel, metrics_disabled_page);
  gtk_widget_class_bind_template_child (widget_class, CcMetricsPanel, preferences_page);

  gtk_widget_class_bind_template_callback (widget_class, on_attribution_label_link);
  gtk_widget_class_bind_template_callback (widget_class, on_metrics_switch_state_set);
}

static void
cc_metrics_panel_init (CcMetricsPanel *self)
{
  g_resources_register (cc_metrics_get_resource ());

  gtk_widget_init_template (GTK_WIDGET (self));
}
