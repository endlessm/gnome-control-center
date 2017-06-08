/*
 * Copyright (C) 2007, 2008  Red Hat, Inc.
 * Copyright (C) 2013 Intel, Inc.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "cc-display-panel.h"

#include <gtk/gtk.h>
#include "scrollarea.h"
#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-rr.h>
#include <libgnome-desktop/gnome-rr-config.h>
#include <libgnome-desktop/gnome-bg.h>
#include <glib/gi18n.h>
#include <stdlib.h>
#include <gdesktop-enums.h>
#include <math.h>

#include "shell/list-box-helper.h"
#include <libupower-glib/upower.h>

CC_PANEL_REGISTER (CcDisplayPanel, cc_display_panel)

#define DISPLAY_PANEL_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), CC_TYPE_DISPLAY_PANEL, CcDisplayPanelPrivate))

#define WID(s) GTK_WIDGET (gtk_builder_get_object (self->priv->builder, s))

#define TOP_BAR_HEIGHT 5

/* The minimum supported size for the panel, see:
 * http://live.gnome.org/Design/SystemSettings */
#define MINIMUM_WIDTH 740
#define MINIMUM_HEIGHT 530


#define DISPLAY_PREVIEW_SETUP_HEIGHT 140
#define DISPLAY_PREVIEW_LIST_HEIGHT  55

enum
{
  DISPLAY_MODE_PRIMARY,
  DISPLAY_MODE_SECONDARY,
  /* DISPLAY_MODE_PRESENTATION, */
  DISPLAY_MODE_MIRROR,
  DISPLAY_MODE_OFF
};

struct _CcDisplayPanelPrivate
{
  GnomeRRScreen     *screen;
  GnomeRRConfig     *current_configuration;
  GnomeRROutputInfo *current_output;

  GnomeBG *background;
  GnomeDesktopThumbnailFactory *thumbnail_factory;

  guint           focus_id;
  guint           screen_changed_handler_id;

  GtkWidget *displays_listbox;
  GtkWidget *arrange_button;
  GtkWidget *res_combo;
  GtkWidget *freq_combo;
  GHashTable *res_freqs;
  GtkWidget *scaling_switch;
  GtkWidget *rotate_left_button;
  GtkWidget *upside_down_button;
  GtkWidget *rotate_right_button;
  GtkWidget *dialog;
  GtkWidget *config_grid;

  UpClient *up_client;
  gboolean lid_is_closed;

  GDBusProxy *shell_proxy;
  GCancellable *shell_cancellable;

  guint       sensor_watch_id;
  GDBusProxy *iio_sensor_proxy;
  gboolean    has_accelerometer;
};

typedef struct
{
  int grab_x;
  int grab_y;
  int output_x;
  int output_y;
} GrabInfo;

static GHashTable *output_ids;

gint
cc_display_panel_get_output_id (GnomeRROutputInfo *output)
{
  if (output_ids)
    return GPOINTER_TO_INT (g_hash_table_lookup (output_ids, output));
  else
    return 0;
}

static void
monitor_labeler_show (CcDisplayPanel *self)
{
  CcDisplayPanelPrivate *priv = self->priv;
  GnomeRROutputInfo **infos, **info;
  GnomeRROutput *output;
  GVariantBuilder builder;
  gint number;

  if (!priv->shell_proxy)
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_open (&builder, G_VARIANT_TYPE_ARRAY);

  infos = gnome_rr_config_get_outputs (priv->current_configuration);
  for (info = infos; *info; info++)
    {
      number = cc_display_panel_get_output_id (*info);
      if (number == 0)
        continue;

      output = gnome_rr_screen_get_output_by_name (priv->screen,
                                                   gnome_rr_output_info_get_name (*info));
      g_variant_builder_add (&builder, "{uv}",
                             gnome_rr_output_get_id (output),
                             g_variant_new_int32 (number));
    }

  g_variant_builder_close (&builder);

  g_dbus_proxy_call (priv->shell_proxy,
                     "ShowMonitorLabels",
                     g_variant_builder_end (&builder),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1, NULL, NULL, NULL);
}

static void
monitor_labeler_hide (CcDisplayPanel *self)
{
  CcDisplayPanelPrivate *priv = self->priv;

  if (!priv->shell_proxy)
    return;

  g_dbus_proxy_call (priv->shell_proxy,
                     "HideMonitorLabels",
                     NULL, G_DBUS_CALL_FLAGS_NONE,
                     -1, NULL, NULL, NULL);
}

static void
ensure_monitor_labels (CcDisplayPanel *self)
{
  GList *windows, *w;

  windows = gtk_window_list_toplevels ();

  for (w = windows; w; w = w->next)
    {
      if (gtk_window_has_toplevel_focus (GTK_WINDOW (w->data)))
        {
          monitor_labeler_show (self);
          break;
        }
    }

  if (!w)
    monitor_labeler_hide (self);

  g_list_free (windows);
}

static void
cc_display_panel_dispose (GObject *object)
{
  CcDisplayPanelPrivate *priv = CC_DISPLAY_PANEL (object)->priv;
  CcShell *shell;
  GtkWidget *toplevel;

  if (priv->sensor_watch_id > 0)
    {
      g_bus_unwatch_name (priv->sensor_watch_id);
      priv->sensor_watch_id = 0;
    }

  g_clear_object (&priv->iio_sensor_proxy);

  if (output_ids)
    {
      g_hash_table_destroy (output_ids);
      output_ids = NULL;
    }

  if (priv->focus_id)
    {
      shell = cc_panel_get_shell (CC_PANEL (object));
      toplevel = cc_shell_get_toplevel (shell);
      if (toplevel != NULL)
        g_signal_handler_disconnect (G_OBJECT (toplevel),
                                     priv->focus_id);
      priv->focus_id = 0;
      monitor_labeler_hide (CC_DISPLAY_PANEL (object));
    }

  if (priv->screen_changed_handler_id)
    {
      g_signal_handler_disconnect (priv->screen, priv->screen_changed_handler_id);
      priv->screen_changed_handler_id = 0;
    }

  g_clear_object (&priv->screen);
  g_clear_object (&priv->up_client);
  g_clear_object (&priv->background);
  g_clear_object (&priv->thumbnail_factory);

  if (priv->dialog)
    {
      gtk_widget_destroy (priv->dialog);
      priv->dialog = NULL;
    }

  g_cancellable_cancel (priv->shell_cancellable);
  g_clear_object (&priv->shell_cancellable);
  g_clear_object (&priv->shell_proxy);

  G_OBJECT_CLASS (cc_display_panel_parent_class)->dispose (object);
}

static const char *
cc_display_panel_get_help_uri (CcPanel *panel)
{
  return "help:gnome-help/hardware#displays";
}

static void
cc_display_panel_class_init (CcDisplayPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CcPanelClass *panel_class = CC_PANEL_CLASS (klass);

  g_type_class_add_private (klass, sizeof (CcDisplayPanelPrivate));

  panel_class->get_help_uri = cc_display_panel_get_help_uri;

  object_class->dispose = cc_display_panel_dispose;
}

static gboolean
should_show_resolution (gint output_width,
                        gint output_height,
                        gint width,
                        gint height)
{
  if (width >= MIN (output_width, MINIMUM_WIDTH) &&
      height >= MIN (output_height, MINIMUM_HEIGHT))
    {
      return TRUE;
    }
  return FALSE;
}

static void
apply_rotation_to_geometry (GnomeRROutputInfo *output, int *w, int *h)
{
  GnomeRRRotation rotation;

  rotation = gnome_rr_output_info_get_rotation (output);
  if ((rotation & GNOME_RR_ROTATION_90) || (rotation & GNOME_RR_ROTATION_270))
    {
      int tmp;
      tmp = *h;
      *h = *w;
      *w = tmp;
    }
}

static void
get_geometry (GnomeRROutputInfo *output, int *x, int *y, int *w, int *h)
{
  if (gnome_rr_output_info_is_active (output))
    {
      gnome_rr_output_info_get_geometry (output, x, y, w, h);
    }
  else
    {
      gnome_rr_output_info_get_geometry (output, x, y, NULL, NULL);
      *h = gnome_rr_output_info_get_preferred_height (output);
      *w = gnome_rr_output_info_get_preferred_width (output);
    }

  apply_rotation_to_geometry (output, w, h);
}

static void
on_viewport_changed (FooScrollArea *scroll_area,
                     GdkRectangle  *old_viewport,
                     GdkRectangle  *new_viewport)
{
  foo_scroll_area_set_size (scroll_area,
                            new_viewport->width,
                            new_viewport->height);

  foo_scroll_area_invalidate (scroll_area);
}

static void
paint_output (CcDisplayPanel    *panel,
              cairo_t           *cr,
              GnomeRRConfig     *configuration,
              GnomeRROutputInfo *output,
              gint               num,
              gint               allocated_width,
              gint               allocated_height)
{
  GdkPixbuf *pixbuf;
  gint x, y, width, height;

  get_geometry (output, NULL, NULL, &width, &height);

  x = y = 0;

  /* scale to fit allocation */
  if (width / (double) height < allocated_width / (double) allocated_height)
    {
      width = allocated_height * (width / (double) height);
      height = allocated_height;
    }
  else
    {
      height = allocated_width * (height / (double) width);
      width = allocated_width;
    }


  x = (allocated_width / 2.0) - (width / 2.0);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_rectangle (cr, x, y, width, height);
  cairo_fill (cr);

  if (gnome_rr_output_info_is_active (output))
    {
      pixbuf = gnome_bg_create_thumbnail (panel->priv->background,
                                          panel->priv->thumbnail_factory,
                                          gdk_screen_get_default (), width, height);
    }
  else
    pixbuf = NULL;

  if (gnome_rr_output_info_get_primary (output)
      || gnome_rr_config_get_clone (configuration))
    height -= TOP_BAR_HEIGHT;

  if (pixbuf)
    gdk_cairo_set_source_pixbuf (cr, pixbuf, x + 1, y + 1);
  else
    cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  cairo_rectangle (cr, x + 1, y + 1, width - 2, height - 2);
  cairo_fill (cr);

  g_clear_object (&pixbuf);

  if (num > 0)
    {
      PangoLayout *layout;
      gchar *number_str;
      gdouble r = 3, r2 = r / 2.0, x1, y1, x2, y2;
      PangoRectangle extents;
      gdouble max_extent;

      number_str = g_strdup_printf ("<small>%d</small>", num);
      layout = gtk_widget_create_pango_layout (GTK_WIDGET (panel), "");
      pango_layout_set_markup (layout, number_str, -1);
      pango_layout_get_extents (layout, NULL, &extents);
      g_free (number_str);

      cairo_set_source_rgba (cr, 0, 0, 0, 0.75);
      max_extent = MAX ((extents.width - extents.x)/ PANGO_SCALE,
                        (extents.height - extents.y) / PANGO_SCALE);

      x += 5;
      y += 5;
      x1 = x;
      x2 = x1 + max_extent + 1;
      y1 = y;
      y2 = y1 + max_extent + 1;
      cairo_move_to    (cr, x1 + r, y1);
      cairo_line_to    (cr, x2 - r, y1);
      cairo_curve_to   (cr, x2 - r2, y1, x2, y1 + r2, x2, y1 + r);
      cairo_line_to    (cr, x2, y2 - r);
      cairo_curve_to   (cr, x2, y2 - r2, x2 - r2, y2, x2 - r, y2);
      cairo_line_to    (cr, x1 + r, y2);
      cairo_curve_to   (cr, x1 + r2, y2, x1, y2 - r2, x1, y2 - r);
      cairo_line_to    (cr, x1, y1 + r);
      cairo_curve_to   (cr, x1, y1 + r2, x1 + r2, y1, x1 + r, y1);
      cairo_fill (cr);

      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_move_to (cr,
                     x + (max_extent / 2.0) - ((extents.width / PANGO_SCALE) / 2.0),
                     y + (max_extent / 2.0) - ((extents.height / PANGO_SCALE) / 2.0));
      pango_cairo_show_layout (cr, layout);
      cairo_fill (cr);
      g_object_unref (layout);
    }
}

static gboolean
display_preview_draw (GtkWidget      *widget,
                      cairo_t        *cr,
                      CcDisplayPanel *panel)
{
  GnomeRROutputInfo *output;
  GnomeRRConfig *config;
  gint num, width, height;

  output = g_object_get_data (G_OBJECT (widget), "output");
  config = g_object_get_data (G_OBJECT (widget), "config");
  num = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "number"));

  width = gtk_widget_get_allocated_width (widget);
  height = gtk_widget_get_allocated_height (widget);

  paint_output (panel, cr, config, output, num, width, height);

  return TRUE;
}

static GtkWidget*
display_preview_new (CcDisplayPanel    *panel,
                     GnomeRROutputInfo *output,
                     GnomeRRConfig     *config,
                     gint               num,
                     gint               base_height)
{
  GtkWidget *area;
  gint width, height;

  get_geometry (output, NULL, NULL, &width, &height);

  area = gtk_drawing_area_new ();
  g_signal_connect (area, "draw", G_CALLBACK (display_preview_draw), panel);

  gtk_widget_set_size_request (area, base_height * (width / (gdouble) height), base_height);

  gtk_widget_set_valign (area, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (area, GTK_ALIGN_CENTER);

  g_object_set_data (G_OBJECT (area), "output", output);
  g_object_set_data (G_OBJECT (area), "config", config);
  g_object_set_data (G_OBJECT (area), "number", GINT_TO_POINTER (num));

  return area;
}

static void
on_screen_changed (CcDisplayPanel *panel)
{
  GnomeRRConfig *current;
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRROutputInfo **outputs;
  gint i, num_connected_outputs = 0, number = 0;
  gboolean clone, combined = FALSE;
  GtkSizeGroup *sizegroup;
  GList *sorted_outputs = NULL, *l;

  if (priv->dialog)
    gtk_dialog_response (GTK_DIALOG (priv->dialog), GTK_RESPONSE_NONE);

  gnome_rr_screen_refresh (priv->screen, NULL);

  current = gnome_rr_config_new_current (priv->screen, NULL);
  gnome_rr_config_ensure_primary (current);

  gtk_container_foreach (GTK_CONTAINER (priv->displays_listbox),
                         (GtkCallback) gtk_widget_destroy, NULL);

  if (priv->current_configuration)
    g_object_unref (priv->current_configuration);

  priv->current_configuration = current;

  clone = gnome_rr_config_get_clone (current);

  outputs = gnome_rr_config_get_outputs (current);

  g_hash_table_remove_all (output_ids);

  /* count the number of active and connected outputs */
  for (i = 0; outputs[i]; i++)
    {
      GnomeRROutput *output;

      if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	continue;

      output = gnome_rr_screen_get_output_by_name (priv->screen,
                                                   gnome_rr_output_info_get_name (outputs[i]));

      /* ensure the built in display is first in the list */
      if (gnome_rr_output_is_builtin_display (output))
        sorted_outputs = g_list_prepend (sorted_outputs, outputs[i]);
      else
        sorted_outputs = g_list_append (sorted_outputs, outputs[i]);

      num_connected_outputs++;
    }

  sizegroup = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  for (l = sorted_outputs; l; l = g_list_next (l))
    {
      GtkWidget *row, *item, *preview, *label;
      gboolean primary, active;
      const gchar *status;
      gboolean display_closed = FALSE;
      GnomeRROutput *output;
      GnomeRROutputInfo *output_info;

      output_info = l->data;

      output = gnome_rr_screen_get_output_by_name (priv->screen,
                                                   gnome_rr_output_info_get_name (output_info));

      if (priv->lid_is_closed)
        display_closed = gnome_rr_output_is_builtin_display (output);

      row = gtk_list_box_row_new ();
      item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_container_set_border_width (GTK_CONTAINER (item), 12);

      preview = display_preview_new (panel, output_info, current, ++number,
                                     DISPLAY_PREVIEW_LIST_HEIGHT);
      gtk_size_group_add_widget (sizegroup, preview);

      if (display_closed)
        gtk_widget_set_sensitive (row, FALSE);

      g_hash_table_insert (output_ids, output_info, GINT_TO_POINTER (number));

      gtk_container_add (GTK_CONTAINER (item), preview);

      label = gtk_label_new (gnome_rr_output_info_get_display_name (output_info));
      gtk_container_add (GTK_CONTAINER (item), label);

      primary = gnome_rr_output_info_get_primary (output_info);
      active = gnome_rr_output_info_is_active (output_info);

      if (num_connected_outputs > 1)
        {
          if (display_closed)
            status = _("Lid Closed");
          else if (clone)
            /* translators: "Mirrored" describes when both displays show the same view */
            status = _("Mirrored");
          else if (primary)
            status = _("Primary");
          else if (!active)
            status = _("Off");
          else
            {
              status = _("Secondary");
              combined = TRUE;
            }

          label = gtk_label_new (status);
          gtk_widget_set_hexpand (label, TRUE);
          gtk_widget_set_halign (label, GTK_ALIGN_END);
          gtk_container_add (GTK_CONTAINER (item), label);
        }

      g_object_set_data (G_OBJECT (row), "gnome-rr-output", output_info);
      gtk_container_add (GTK_CONTAINER (row), item);
      gtk_container_add (GTK_CONTAINER (priv->displays_listbox), row);
      gtk_widget_show_all (row);
    }

  g_list_free (sorted_outputs);

  if (combined)
    gtk_widget_show (priv->arrange_button);
  else
    gtk_widget_hide (priv->arrange_button);

  ensure_monitor_labels (panel);
}


static void
realign_outputs_after_resolution_change (CcDisplayPanel *self, GnomeRROutputInfo *output_that_changed, int old_width, int old_height, GnomeRRRotation old_rotation)
{
  /* We find the outputs that were below or to the right of the output that
   * changed, and realign them; we also do that for outputs that shared the
   * right/bottom edges with the output that changed.  The outputs that are
   * above or to the left of that output don't need to change.
   */

  int i;
  int old_right_edge, old_bottom_edge;
  int dx, dy;
  int x, y, width, height;
  GnomeRROutputInfo **outputs;
  GnomeRRRotation rotation;

  g_assert (self->priv->current_configuration != NULL);

  gnome_rr_output_info_get_geometry (output_that_changed, &x, &y, &width, &height);
  rotation = gnome_rr_output_info_get_rotation (output_that_changed);

  if (width == old_width && height == old_height && rotation == old_rotation)
    {
      g_debug ("Not realigning outputs, configuration is the same for %s", gnome_rr_output_info_get_name (output_that_changed));
      return;
    }

  g_debug ("Realigning outputs, configuration changed for %s", gnome_rr_output_info_get_name (output_that_changed));

  /* Apply rotation to the geometry of the newly changed output,
   * as well as to its old configuration */
  apply_rotation_to_geometry (output_that_changed, &width, &height);
  if ((old_rotation & GNOME_RR_ROTATION_90) || (old_rotation & GNOME_RR_ROTATION_270))
    {
      int tmp;
      tmp = old_height;
      old_height = old_width;
      old_width = tmp;
    }

  old_right_edge = x + old_width;
  old_bottom_edge = y + old_height;

  dx = width - old_width;
  dy = height - old_height;

  outputs = gnome_rr_config_get_outputs (self->priv->current_configuration);

  for (i = 0; outputs[i] != NULL; i++)
    {
      int output_x, output_y;
      int output_width, output_height;

      if (outputs[i] == output_that_changed || !gnome_rr_output_info_is_connected (outputs[i]))
        continue;

      if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	continue;

      gnome_rr_output_info_get_geometry (outputs[i], &output_x, &output_y, &output_width, &output_height);

      if (output_x >= old_right_edge)
         output_x += dx;
      else if (output_x + output_width == old_right_edge)
         output_x = x + width - output_width;

      if (output_y >= old_bottom_edge)
         output_y += dy;
      else if (output_y + output_height == old_bottom_edge)
         output_y = y + height - output_height;

      g_debug ("Setting geometry for %s: %dx%d+%d+%d", gnome_rr_output_info_get_name (outputs[i]), output_width, output_height, output_x, output_y);
      gnome_rr_output_info_set_geometry (outputs[i], output_x, output_y, output_width, output_height);
    }
}

static void
lay_out_outputs_horizontally (CcDisplayPanel *self)
{
  int i;
  int x;
  GnomeRROutputInfo **outputs;

  /* Lay out all the monitors horizontally when "mirror screens" is turned
   * off, to avoid having all of them overlapped initially.  We put the
   * outputs turned off on the right-hand side.
   */

  x = 0;

  /* First pass, all "on" outputs */
  outputs = gnome_rr_config_get_outputs (self->priv->current_configuration);

  for (i = 0; outputs[i]; ++i)
    {
      int width, height;
      if (gnome_rr_output_info_is_connected (outputs[i]) && gnome_rr_output_info_is_active (outputs[i]))
        {
          gnome_rr_output_info_get_geometry (outputs[i], NULL, NULL, &width, &height);
          gnome_rr_output_info_set_geometry (outputs[i], x, 0, width, height);
          x += width;
        }
    }

  /* Second pass, all the black screens */

    for (i = 0; outputs[i]; ++i)
    {
      int width, height;
      if (!(gnome_rr_output_info_is_connected (outputs[i]) && gnome_rr_output_info_is_active (outputs[i])))
        {
          gnome_rr_output_info_get_geometry (outputs[i], NULL, NULL, &width, &height);
          gnome_rr_output_info_set_geometry (outputs[i], x, 0, width, height);
          x += width;
        }
    }

}



#define SPACE 15
#define MARGIN  15

static GList *
list_connected_outputs (CcDisplayPanel *self, int *total_w, int *total_h)
{
  int i, dummy;
  GList *result = NULL;
  GnomeRROutputInfo **outputs;

  if (!total_w)
    total_w = &dummy;
  if (!total_h)
    total_h = &dummy;

  *total_w = 0;
  *total_h = 0;

  outputs = gnome_rr_config_get_outputs (self->priv->current_configuration);
  for (i = 0; outputs[i] != NULL; ++i)
    {
      if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	continue;

      if (gnome_rr_output_info_is_connected (outputs[i]))
	{
	  int w, h;

	  result = g_list_prepend (result, outputs[i]);

	  get_geometry (outputs[i], NULL, NULL, &w, &h);

          *total_w += w;
          *total_h += h;
        }
    }

  return g_list_reverse (result);
}

static int
get_n_connected (CcDisplayPanel *self)
{
  GList *connected_outputs = list_connected_outputs (self, NULL, NULL);
  int n = g_list_length (connected_outputs);

  g_list_free (connected_outputs);

  return n;
}

static double
compute_scale (CcDisplayPanel *self, FooScrollArea *area)
{
  int available_w, available_h;
  int total_w, total_h;
  int n_monitors;
  GdkRectangle viewport;
  GList *connected_outputs;

  foo_scroll_area_get_viewport (area, &viewport);

  connected_outputs = list_connected_outputs (self, &total_w, &total_h);

  n_monitors = g_list_length (connected_outputs);

  g_list_free (connected_outputs);

  available_w = viewport.width - 2 * MARGIN - (n_monitors - 1) * SPACE;
  available_h = viewport.height - 2 * MARGIN - (n_monitors - 1) * SPACE;

  return MIN ((double)available_w / total_w, (double)available_h / total_h);
}

typedef struct Edge
{
  GnomeRROutputInfo *output;
  int x1, y1;
  int x2, y2;
} Edge;

typedef struct Snap
{
  Edge *snapper;              /* Edge that should be snapped */
  Edge *snappee;
  int dy, dx;
} Snap;

static void
add_edge (GnomeRROutputInfo *output, int x1, int y1, int x2, int y2, GArray *edges)
{
  Edge e;

  e.x1 = x1;
  e.x2 = x2;
  e.y1 = y1;
  e.y2 = y2;
  e.output = output;

  g_array_append_val (edges, e);
}

static void
list_edges_for_output (GnomeRROutputInfo *output, GArray *edges)
{
  int x, y, w, h;

  get_geometry (output, &x, &y, &w, &h);

  /* Top, Bottom, Left, Right */
  add_edge (output, x, y, x + w, y, edges);
  add_edge (output, x, y + h, x + w, y + h, edges);
  add_edge (output, x, y, x, y + h, edges);
  add_edge (output, x + w, y, x + w, y + h, edges);
}

static void
list_edges (GnomeRRConfig *config, GArray *edges)
{
  int i;
  GnomeRROutputInfo **outputs = gnome_rr_config_get_outputs (config);

  for (i = 0; outputs[i]; ++i)
    {
      if (gnome_rr_output_info_is_connected (outputs[i]))
	{
	  if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	    continue;
	  list_edges_for_output (outputs[i], edges);
	}
    }
}

static gboolean
overlap (int s1, int e1, int s2, int e2)
{
  return (!(e1 < s2 || s1 >= e2));
}

static gboolean
horizontal_overlap (Edge *snapper, Edge *snappee)
{
  if (snapper->y1 != snapper->y2 || snappee->y1 != snappee->y2)
    return FALSE;

  return overlap (snapper->x1, snapper->x2, snappee->x1, snappee->x2);
}

static gboolean
vertical_overlap (Edge *snapper, Edge *snappee)
{
  if (snapper->x1 != snapper->x2 || snappee->x1 != snappee->x2)
    return FALSE;

  return overlap (snapper->y1, snapper->y2, snappee->y1, snappee->y2);
}

static void
add_snap (GArray *snaps, Snap snap)
{
  if (ABS (snap.dx) <= 200 || ABS (snap.dy) <= 200)
    g_array_append_val (snaps, snap);
}

static void
add_edge_snaps (Edge *snapper, Edge *snappee, GArray *snaps)
{
  Snap snap;

  snap.snapper = snapper;
  snap.snappee = snappee;

  if (horizontal_overlap (snapper, snappee))
    {
      snap.dx = 0;
      snap.dy = snappee->y1 - snapper->y1;

      add_snap (snaps, snap);
    }
  else if (vertical_overlap (snapper, snappee))
    {
      snap.dy = 0;
      snap.dx = snappee->x1 - snapper->x1;

      add_snap (snaps, snap);
    }

  /* Corner snaps */
  /* 1->1 */
  snap.dx = snappee->x1 - snapper->x1;
  snap.dy = snappee->y1 - snapper->y1;

  add_snap (snaps, snap);

  /* 1->2 */
  snap.dx = snappee->x2 - snapper->x1;
  snap.dy = snappee->y2 - snapper->y1;

  add_snap (snaps, snap);

  /* 2->2 */
  snap.dx = snappee->x2 - snapper->x2;
  snap.dy = snappee->y2 - snapper->y2;

  add_snap (snaps, snap);

  /* 2->1 */
  snap.dx = snappee->x1 - snapper->x2;
  snap.dy = snappee->y1 - snapper->y2;

  add_snap (snaps, snap);
}

static void
list_snaps (GnomeRROutputInfo *output, GArray *edges, GArray *snaps)
{
  int i;

  for (i = 0; i < edges->len; ++i)
    {
      Edge *output_edge = &(g_array_index (edges, Edge, i));

      if (output_edge->output == output)
        {
          int j;

          for (j = 0; j < edges->len; ++j)
            {
              Edge *edge = &(g_array_index (edges, Edge, j));

              if (edge->output != output)
                add_edge_snaps (output_edge, edge, snaps);
            }
        }
    }
}

#if 0
static void
print_edge (Edge *edge)
{
  g_debug ("(%d %d %d %d)", edge->x1, edge->y1, edge->x2, edge->y2);
}
#endif

static gboolean
corner_on_edge (int x, int y, Edge *e)
{
  if (x == e->x1 && x == e->x2 && y >= e->y1 && y <= e->y2)
    return TRUE;

  if (y == e->y1 && y == e->y2 && x >= e->x1 && x <= e->x2)
    return TRUE;

  return FALSE;
}

static gboolean
edges_align (Edge *e1, Edge *e2)
{
  if (corner_on_edge (e1->x1, e1->y1, e2))
    return TRUE;

  if (corner_on_edge (e2->x1, e2->y1, e1))
    return TRUE;

  return FALSE;
}

static gboolean
output_is_aligned (GnomeRROutputInfo *output, GArray *edges)
{
  gboolean result = FALSE;
  int i;

  for (i = 0; i < edges->len; ++i)
    {
      Edge *output_edge = &(g_array_index (edges, Edge, i));

      if (output_edge->output == output)
        {
          int j;

          for (j = 0; j < edges->len; ++j)
            {
              Edge *edge = &(g_array_index (edges, Edge, j));

              /* We are aligned if an output edge matches
               * an edge of another output
               */
              if (edge->output != output_edge->output)
                {
                  if (edges_align (output_edge, edge))
                    {
                      result = TRUE;
                      goto done;
                    }
                }
            }
        }
    }
 done:

  return result;
}

static void
get_output_rect (GnomeRROutputInfo *output, GdkRectangle *rect)
{
  get_geometry (output, &rect->x, &rect->y, &rect->width, &rect->height);
}

static gboolean
output_overlaps (GnomeRROutputInfo *output, GnomeRRConfig *config)
{
  int i;
  GdkRectangle output_rect;
  GnomeRROutputInfo **outputs;

  g_assert (output != NULL);

  get_output_rect (output, &output_rect);

  outputs = gnome_rr_config_get_outputs (config);
  for (i = 0; outputs[i]; ++i)
    {
      if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	continue;
      if (outputs[i] != output && gnome_rr_output_info_is_connected (outputs[i]))
	{
	  GdkRectangle other_rect;

	  get_output_rect (outputs[i], &other_rect);
	  if (gdk_rectangle_intersect (&output_rect, &other_rect, NULL))
	    return TRUE;
	}
    }

  return FALSE;
}

static gboolean
gnome_rr_config_is_aligned (GnomeRRConfig *config, GArray *edges)
{
  int i;
  gboolean result = TRUE;
  GnomeRROutputInfo **outputs;

  outputs = gnome_rr_config_get_outputs (config);
  for (i = 0; outputs[i]; ++i)
    {
      if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	continue;
      if (gnome_rr_output_info_is_connected (outputs[i]))
	{
	  if (!output_is_aligned (outputs[i], edges))
	    return FALSE;

	  if (output_overlaps (outputs[i], config))
	    return FALSE;
	}
    }

  return result;
}

static gboolean
is_corner_snap (const Snap *s)
{
  return s->dx != 0 && s->dy != 0;
}

static int
compare_snaps (gconstpointer v1, gconstpointer v2)
{
  const Snap *s1 = v1;
  const Snap *s2 = v2;
  int sv1 = MAX (ABS (s1->dx), ABS (s1->dy));
  int sv2 = MAX (ABS (s2->dx), ABS (s2->dy));
  int d;

  d = sv1 - sv2;

  /* This snapping algorithm is good enough for rock'n'roll, but
   * this is probably a better:
   *
   *    First do a horizontal/vertical snap, then
   *    with the new coordinates from that snap,
   *    do a corner snap.
   *
   * Right now, it's confusing that corner snapping
   * depends on the distance in an axis that you can't actually see.
   *
   */
  if (d == 0)
    {
      if (is_corner_snap (s1) && !is_corner_snap (s2))
        return -1;
      else if (is_corner_snap (s2) && !is_corner_snap (s1))
        return 1;
      else
        return 0;
    }
  else
    {
      return d;
    }
}

/* Sets a mouse cursor for a widget's window.  As a hack, you can pass
 * GDK_BLANK_CURSOR to mean "set the cursor to NULL" (i.e. reset the widget's
 * window's cursor to its default).
 */
static void
set_cursor (GtkWidget *widget, GdkCursorType type)
{
  GdkCursor *cursor;
  GdkWindow *window;

  if (type == GDK_BLANK_CURSOR)
    cursor = NULL;
  else
    cursor = gdk_cursor_new_for_display (gtk_widget_get_display (widget), type);

  window = gtk_widget_get_window (widget);

  if (window)
    gdk_window_set_cursor (window, cursor);

  if (cursor)
    g_object_unref (cursor);
}

static void
grab_weak_ref_notify (gpointer  area,
                      GObject  *object)
{
  foo_scroll_area_end_grab (area, NULL);
}

static GnomeRROutputInfo *
find_output (GnomeRROutputInfo **outputs,
             const gchar        *name)
{
  int i;

  for (i = 0; outputs[i]; i++)
    {
      if (g_str_equal (gnome_rr_output_info_get_name (outputs[i]), name))
        return outputs[i];
    }
  return NULL;
}

static void
update_apply_button (CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  gboolean config_equal;
  GnomeRRConfig *current_configuration;

  if (!gnome_rr_config_applicable (priv->current_configuration,
                                   priv->screen, NULL))
    {
      gtk_dialog_set_response_sensitive (GTK_DIALOG (priv->dialog), GTK_RESPONSE_ACCEPT, FALSE);
      return;
    }

  current_configuration = gnome_rr_config_new_current (priv->screen, NULL);

  /* this checks if the same modes will be set on the outputs */
  config_equal = gnome_rr_config_equal (priv->current_configuration,
                                        current_configuration);

  if (config_equal)
    {
      /* check if clone state has changed */
      if (gnome_rr_config_get_clone (priv->current_configuration)
          != gnome_rr_config_get_clone (current_configuration))
        {
          config_equal = FALSE;
        }
      else
        {
          GnomeRROutputInfo **new_outputs, **current_outputs;
          int i;

          /* check if primary display has changed */
          new_outputs = gnome_rr_config_get_outputs (priv->current_configuration);
          current_outputs = gnome_rr_config_get_outputs (current_configuration);

          for (i = 0; new_outputs[i]; i++)
            {
              GnomeRROutputInfo *output;

              output = find_output (current_outputs,
                                    gnome_rr_output_info_get_name (new_outputs[i]));

              if (!output)
                {
                  config_equal = FALSE;
                  break;
                }

              if (gnome_rr_output_info_get_primary (new_outputs[i])
                  != gnome_rr_output_info_get_primary (output))
                {
                  config_equal = FALSE;
                  break;
                }
            }
        }
    }

  g_object_unref (current_configuration);

  gtk_dialog_set_response_sensitive (GTK_DIALOG (priv->dialog), GTK_RESPONSE_ACCEPT, !config_equal);
}

static void
on_output_event (FooScrollArea *area,
                 FooScrollAreaEvent *event,
                 gpointer data)
{
  GnomeRROutputInfo *output = data;
  CcDisplayPanel *self = g_object_get_data (G_OBJECT (area), "panel");

  if (event->type == FOO_DRAG_HOVER)
    {
      return;
    }
  if (event->type == FOO_DROP)
    {
      /* Activate new primary? */
      return;
    }

  /* If the mouse is inside the outputs, set the cursor to "you can move me".  See
   * on_canvas_event() for where we reset the cursor to the default if it
   * exits the outputs' area.
   */
  if (!gnome_rr_config_get_clone (self->priv->current_configuration) && get_n_connected (self) > 1)
    set_cursor (GTK_WIDGET (area), GDK_FLEUR);

  if (event->type == FOO_BUTTON_PRESS)
    {
      GrabInfo *info;

      self->priv->current_output = output;


      if (!gnome_rr_config_get_clone (self->priv->current_configuration) && get_n_connected (self) > 1)
	{
	  int output_x, output_y;
	  gnome_rr_output_info_get_geometry (output, &output_x, &output_y, NULL, NULL);

	  foo_scroll_area_begin_grab (area, on_output_event, data);
	  g_object_weak_ref (data, grab_weak_ref_notify, area);

	  info = g_new0 (GrabInfo, 1);
	  info->grab_x = event->x;
	  info->grab_y = event->y;
	  info->output_x = output_x;
	  info->output_y = output_y;

	  g_object_set_data (G_OBJECT (output), "grab-info", info);
	}
      foo_scroll_area_invalidate (area);
    }
  else
    {
      if (foo_scroll_area_is_grabbed (area))
	{
	  GrabInfo *info = g_object_get_data (G_OBJECT (output), "grab-info");
	  double scale = compute_scale (self, area);
	  int old_x, old_y;
	  int width, height;
	  int new_x, new_y;
	  int i;
	  GArray *edges, *snaps, *new_edges;

	  gnome_rr_output_info_get_geometry (output, &old_x, &old_y, &width, &height);
	  new_x = info->output_x + (event->x - info->grab_x) / scale;
	  new_y = info->output_y + (event->y - info->grab_y) / scale;

	  gnome_rr_output_info_set_geometry (output, new_x, new_y, width, height);

	  edges = g_array_new (TRUE, TRUE, sizeof (Edge));
	  snaps = g_array_new (TRUE, TRUE, sizeof (Snap));
	  new_edges = g_array_new (TRUE, TRUE, sizeof (Edge));

	  list_edges (self->priv->current_configuration, edges);
	  list_snaps (output, edges, snaps);

	  g_array_sort (snaps, compare_snaps);

	  gnome_rr_output_info_set_geometry (output, old_x, old_y, width, height);

	  for (i = 0; i < snaps->len; ++i)
	    {
	      Snap *snap = &(g_array_index (snaps, Snap, i));
	      GArray *new_edges = g_array_new (TRUE, TRUE, sizeof (Edge));

	      gnome_rr_output_info_set_geometry (output, new_x + snap->dx, new_y + snap->dy, width, height);

	      g_array_set_size (new_edges, 0);
	      list_edges (self->priv->current_configuration, new_edges);

	      if (gnome_rr_config_is_aligned (self->priv->current_configuration, new_edges))
		{
		  g_array_free (new_edges, TRUE);
		  break;
		}
	      else
		{
		  gnome_rr_output_info_set_geometry (output, info->output_x, info->output_y, width, height);
		}
	    }

	  g_array_free (new_edges, TRUE);
	  g_array_free (snaps, TRUE);
	  g_array_free (edges, TRUE);

	  if (event->type == FOO_BUTTON_RELEASE)
	    {
	      foo_scroll_area_end_grab (area, event);

	      g_free (g_object_get_data (G_OBJECT (output), "grab-info"));
	      g_object_set_data (G_OBJECT (output), "grab-info", NULL);
	      g_object_weak_unref (data, grab_weak_ref_notify, area);
              update_apply_button (self);

#if 0
              g_debug ("new position: %d %d %d %d", output->x, output->y, output->width, output->height);
#endif
            }

          foo_scroll_area_invalidate (area);
        }
    }
}

static void
on_canvas_event (FooScrollArea *area,
                 FooScrollAreaEvent *event,
                 gpointer data)
{
  /* If the mouse exits the outputs, reset the cursor to the default.  See
   * on_output_event() for where we set the cursor to the movement cursor if
   * it is over one of the outputs.
   */
  set_cursor (GTK_WIDGET (area), GDK_BLANK_CURSOR);
}

static void
paint_background (FooScrollArea *area,
                  cairo_t       *cr)
{
  GdkRectangle viewport;

  foo_scroll_area_get_viewport (area, &viewport);

  cairo_set_source_rgba (cr, 0, 0, 0, 0.4);
  cairo_rectangle (cr,
                   viewport.x, viewport.y,
                   viewport.width, viewport.height);
  foo_scroll_area_add_input_from_fill (area, cr, on_canvas_event, NULL);
  cairo_fill (cr);
}

static void
on_area_paint (FooScrollArea  *area,
               cairo_t        *cr,
               gpointer        data)
{
  CcDisplayPanel *self = data;
  GList *connected_outputs = NULL;
  GList *list;

  paint_background (area, cr);

  if (!self->priv->current_configuration)
    return;

  connected_outputs = list_connected_outputs (self, NULL, NULL);

  for (list = connected_outputs; list != NULL; list = list->next)
    {
      int w, h;
      double scale = compute_scale (self, area);
      gint x, y;
      int output_x, output_y;
      int total_w, total_h;
      GList *connected_outputs;
      GnomeRROutputInfo *output = list->data;
      GdkRectangle viewport;

      cairo_save (cr);

      connected_outputs = list_connected_outputs (self, &total_w, &total_h);
      g_list_free (connected_outputs);

      foo_scroll_area_get_viewport (area, &viewport);
      get_geometry (output, &output_x, &output_y, &w, &h);

      viewport.height -= 2 * MARGIN;
      viewport.width -= 2 * MARGIN;

      x = output_x * scale + MARGIN + (viewport.width - total_w * scale) / 2.0;
      y = output_y * scale + MARGIN + (viewport.height - total_h * scale) / 2.0;


      cairo_set_source_rgba (cr, 0, 0, 0, 0);
      cairo_rectangle (cr, x, y, w * scale + 0.5, h * scale + 0.5);
      foo_scroll_area_add_input_from_fill (area, cr, on_output_event, output);
      cairo_fill (cr);

      cairo_translate (cr, x, y);
      paint_output (self, cr, self->priv->current_configuration, output,
                    cc_display_panel_get_output_id (output),
                    w * scale, h * scale);

      cairo_restore (cr);

      if (gnome_rr_config_get_clone (self->priv->current_configuration))
        break;
    }
}

static void
compute_virtual_size_for_configuration (GnomeRRConfig *config, int *ret_width, int *ret_height)
{
  int i;
  int width, height;
  int output_x, output_y, output_width, output_height;
  GnomeRROutputInfo **outputs;

  width = height = 0;

  outputs = gnome_rr_config_get_outputs (config);
  for (i = 0; outputs[i] != NULL; i++)
    {

      if (!gnome_rr_output_info_is_primary_tile (outputs[i]))
	continue;

      if (gnome_rr_output_info_is_active (outputs[i]))
	{
	  gnome_rr_output_info_get_geometry (outputs[i], &output_x, &output_y, &output_width, &output_height);
	  width = MAX (width, output_x + output_width);
	  height = MAX (height, output_y + output_height);
	}
    }

  *ret_width = width;
  *ret_height = height;
}

static void
check_required_virtual_size (CcDisplayPanel *self)
{
  int req_width, req_height;
  int min_width, max_width;
  int min_height, max_height;

  compute_virtual_size_for_configuration (self->priv->current_configuration, &req_width, &req_height);

  gnome_rr_screen_get_ranges (self->priv->screen, &min_width, &max_width, &min_height, &max_height);

#if 0
  g_debug ("X Server supports:");
  g_debug ("min_width = %d, max_width = %d", min_width, max_width);
  g_debug ("min_height = %d, max_height = %d", min_height, max_height);

  g_debug ("Requesting size of %dx%d", req_width, req_height);
#endif

  if (!(min_width <= req_width && req_width <= max_width
        && min_height <= req_height && req_height <= max_height))
    {
      /* FIXME: present a useful dialog, maybe even before the user tries to Apply */
#if 0
      g_debug ("Your X server needs a larger Virtual size!");
#endif
    }
}

static void
apply_current_configuration (CcDisplayPanel *self)
{
  GError *error = NULL;

  gnome_rr_config_sanitize (self->priv->current_configuration);
  gnome_rr_config_ensure_primary (self->priv->current_configuration);

  check_required_virtual_size (self);

  gnome_rr_config_apply_persistent (self->priv->current_configuration,
                                    self->priv->screen, &error);

  /* re-read the configuration */
  on_screen_changed (self);

  if (error)
    {
      g_warning ("Error applying configuration: %s", error->message);
      g_clear_error (&error);
    }
}

static void
dialog_toplevel_focus_changed (GtkWindow      *window,
                               GParamSpec     *pspec,
                               CcDisplayPanel *self)
{
  ensure_monitor_labels (self);
}

static void
show_arrange_displays_dialog (GtkButton      *button,
                              CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GtkWidget *content_area, *area, *vbox, *label;
  gint response;

  /* Title of displays dialog when multiple monitors are present. */
  priv->dialog = gtk_dialog_new_with_buttons (_("Arrange Combined Displays"),
                                              GTK_WINDOW (cc_shell_get_toplevel (cc_panel_get_shell (CC_PANEL (panel)))),
                                              GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                              _("_Cancel"), GTK_RESPONSE_CANCEL,
                                              _("_Apply"), GTK_RESPONSE_ACCEPT,
                                              NULL);
  g_signal_connect (priv->dialog, "notify::has-toplevel-focus",
                    G_CALLBACK (dialog_toplevel_focus_changed), panel);
  gtk_dialog_set_default_response (GTK_DIALOG (priv->dialog), GTK_RESPONSE_ACCEPT);
  gtk_dialog_set_response_sensitive (GTK_DIALOG (priv->dialog), GTK_RESPONSE_ACCEPT, FALSE);

  content_area = gtk_dialog_get_content_area (GTK_DIALOG (priv->dialog));

  area = (GtkWidget *) foo_scroll_area_new ();
  g_object_set_data (G_OBJECT (area), "panel", panel);

  foo_scroll_area_set_min_size (FOO_SCROLL_AREA (area), 520, 290);
  gtk_widget_set_margin_end (area, 12);
  gtk_widget_set_margin_start (area, 12);
  gtk_widget_set_size_request (area, 520, 290);
  g_signal_connect (area, "paint",
                    G_CALLBACK (on_area_paint), panel);
  g_signal_connect (area, "viewport_changed",
                    G_CALLBACK (on_viewport_changed), panel);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (vbox), area);

  label = gtk_label_new (_("Drag displays to rearrange them"));
  gtk_widget_set_margin_top (label, 12);
  gtk_widget_set_margin_bottom (label, 12);
  gtk_container_add (GTK_CONTAINER (vbox), label);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), GTK_STYLE_CLASS_DIM_LABEL);

  gtk_widget_show_all (vbox);
  gtk_container_add (GTK_CONTAINER (content_area), vbox);

  response = gtk_dialog_run (GTK_DIALOG (priv->dialog));
  if (response == GTK_RESPONSE_ACCEPT)
    apply_current_configuration (panel);
  else if (response != GTK_RESPONSE_NONE)
    {
      /* re-read the previous configuration */
      on_screen_changed (panel);
    }

  gtk_widget_destroy (priv->dialog);
  priv->dialog = NULL;
}

static const gchar *
make_aspect_string (gint width,
                    gint height)
{
  int ratio;
  const gchar *aspect = NULL;

    /* We use a number of Unicode characters below:
     * ∶ is U+2236 RATIO
     *   is U+2009 THIN SPACE,
     * × is U+00D7 MULTIPLICATION SIGN
     */
  if (width && height) {
    if (width > height)
      ratio = width * 10 / height;
    else
      ratio = height * 10 / width;

    switch (ratio) {
    case 13:
      aspect = "4∶3";
      break;
    case 16:
      aspect = "16∶10";
      break;
    case 17:
      aspect = "16∶9";
      break;
    case 23:
      aspect = "21∶9";
      break;
    case 12:
      aspect = "5∶4";
      break;
      /* This catches 1.5625 as well (1600x1024) when maybe it shouldn't. */
    case 15:
      aspect = "3∶2";
      break;
    case 18:
      aspect = "9∶5";
      break;
    case 10:
      aspect = "1∶1";
      break;
    }
  }

  return aspect;
}

static GtkWidget *
list_box_item (const gchar *title,
               const gchar *subtitle)
{
  GtkWidget *item, *label, *row;

  row = gtk_list_box_row_new ();

  item = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_set_border_width (GTK_CONTAINER (item), 12);

  label = gtk_label_new (title);
  gtk_container_add (GTK_CONTAINER (item), label);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);

  label = gtk_label_new (subtitle);
  gtk_container_add (GTK_CONTAINER (item), label);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), GTK_STYLE_CLASS_DIM_LABEL);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);

  gtk_container_add (GTK_CONTAINER (row), item);

  return row;
}

static gboolean
is_atsc_duplicate_freq (GnomeRRMode *mode,
                        GnomeRRMode *next_mode)
{
  double freq, next_freq;
  gboolean ret;

  if (next_mode == NULL)
    return FALSE;

  freq = gnome_rr_mode_get_freq_f (mode);
  next_freq = gnome_rr_mode_get_freq_f (next_mode);

  ret = fabs (freq - (next_freq / 1000.0 * 1001.0)) < 0.01;

  if (ret)
    g_debug ("Next frequency %f is the NTSC variant of %f",
             next_freq, freq);

  return ret;
}

static int
sort_frequencies (gconstpointer a, gconstpointer b)
{
  GnomeRRMode *mode_a = (GnomeRRMode *) a;
  GnomeRRMode *mode_b = (GnomeRRMode *) b;

  /* Highest to lowest */
  if (gnome_rr_mode_get_freq_f (mode_a) > gnome_rr_mode_get_freq_f (mode_b))
    return -1;
  if (gnome_rr_mode_get_freq_f (mode_a) < gnome_rr_mode_get_freq_f (mode_b))
    return 1;
  return 0;
}

static void
setup_frequency_combo_box (CcDisplayPanel *panel,
                           GnomeRRMode    *resolution_mode)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRROutput *current_output;
  GnomeRRMode *current_mode;
  GtkTreeModel *model;
  GtkTreeIter iter;
  const gchar *res;
  GSList *l, *frequencies;
  guint i;
  gboolean prev_dup;

  current_output = gnome_rr_screen_get_output_by_name (priv->screen,
                                                       gnome_rr_output_info_get_name (priv->current_output));
  current_mode = gnome_rr_output_get_current_mode (current_output);

  model = gtk_combo_box_get_model (GTK_COMBO_BOX (priv->freq_combo));
  gtk_list_store_clear (GTK_LIST_STORE (model));

  i = 0;
  res = gnome_rr_mode_get_name (resolution_mode);
  frequencies = g_slist_copy (g_hash_table_lookup (priv->res_freqs, res));
  frequencies = g_slist_sort (frequencies, sort_frequencies);
  prev_dup = FALSE;

  /* Look for 59.94Hz, and if it exists, remove the 60Hz option
   * in favour of this NTSC/ATSC frequency.
   * 60Hz is a "PC" frequency, 59.94Hz is a holdover
   * from NTSC:
   * https://en.wikipedia.org/wiki/NTSC#Lines_and_refresh_rate
   *
   * We also want to handle this for ~30Hz and ~120Hz */
  for (l = frequencies; l != NULL; l = l->next)
    {
      GnomeRRMode *mode = l->data;
      GnomeRRMode *next_mode;
      gchar *freq;
      gboolean dup;

      if (l->next != NULL)
        next_mode = l->next->data;
      else
        next_mode = NULL;

      dup = is_atsc_duplicate_freq (mode, next_mode);
      if (dup && mode != current_mode)
        {
          prev_dup = TRUE;
          continue;
        }

      if (prev_dup)
        {
          /* translators: example string is "60 Hz (NTSC)"
           * NTSC is https://en.wikipedia.org/wiki/NTSC */
          freq = g_strdup_printf (_("%d Hz (NTSC)"),
                                  (int) (roundf (gnome_rr_mode_get_freq_f (mode))));
        }
      else
        {
          /* translators: example string is "60 Hz" */
          freq = g_strdup_printf (_("%d Hz"), gnome_rr_mode_get_freq (mode));
        }

      gtk_list_store_insert_with_values (GTK_LIST_STORE (model), &iter,
                                         -1, 0, freq, 1, mode, -1);
      g_free (freq);

      if (mode == current_mode)
        gtk_combo_box_set_active_iter (GTK_COMBO_BOX (priv->freq_combo), &iter);

      prev_dup = dup;
      i++;
    }

  g_slist_free (frequencies);

  if (i < 2)
    {
      gtk_widget_hide (priv->freq_combo);
      return;
    }

  gtk_widget_show (priv->freq_combo);
  if (gtk_combo_box_get_active (GTK_COMBO_BOX (priv->freq_combo)) == -1)
    gtk_combo_box_set_active (GTK_COMBO_BOX (priv->freq_combo), 0);
}

static void
free_mode_list (gpointer key,
                gpointer value,
                gpointer data)
{
  g_slist_free (value);
}

static void
clear_res_freqs (CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  if (priv->res_freqs)
    {
      g_hash_table_foreach (priv->res_freqs, free_mode_list, NULL);
      g_hash_table_destroy (priv->res_freqs);
      priv->res_freqs = NULL;
    }
}

static void
setup_resolution_combo_box (CcDisplayPanel  *panel,
                            GnomeRRMode    **modes,
                            GnomeRRMode     *current_mode)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GtkTreeModel *res_model;
  gint i;

  clear_res_freqs (panel);
  priv->res_freqs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  res_model = gtk_combo_box_get_model (GTK_COMBO_BOX (priv->res_combo));
  gtk_list_store_clear (GTK_LIST_STORE (res_model));

  for (i = 0; modes[i] != NULL; i++)
    {
      GSList *l;
      const gchar *res;
      gint output_width, output_height, mode_width, mode_height;

      if (!current_mode)
        current_mode = modes[i];

      mode_width = gnome_rr_mode_get_width (modes[i]);
      mode_height = gnome_rr_mode_get_height (modes[i]);

      output_width = gnome_rr_output_info_get_preferred_width (priv->current_output);
      output_height = gnome_rr_output_info_get_preferred_height (priv->current_output);

      if (!should_show_resolution (output_width, output_height, mode_width,
                                   mode_height))
        continue;

      res = gnome_rr_mode_get_name (modes[i]);

      if ((l = g_hash_table_lookup (priv->res_freqs, res)) == NULL)
        {
          GtkTreeIter iter;

          gtk_list_store_insert_with_values (GTK_LIST_STORE (res_model), &iter,
                                             -1, 0, res, 1, modes[i], -1);

          /* select the current mode in the combo box */
          if (gnome_rr_mode_get_width (modes[i]) == gnome_rr_mode_get_width (current_mode)
              && gnome_rr_mode_get_height (modes[i]) == gnome_rr_mode_get_height (current_mode)
              && gnome_rr_mode_get_is_interlaced (modes[i]) == gnome_rr_mode_get_is_interlaced (current_mode))
            {
              gtk_combo_box_set_active_iter (GTK_COMBO_BOX (priv->res_combo),
                                             &iter);
            }
        }

      l = g_slist_append (l, modes[i]);
      g_hash_table_replace (priv->res_freqs, g_strdup (res), l);
    }

  /* ensure a resolution is selected by default */
  if (gtk_combo_box_get_active (GTK_COMBO_BOX (priv->res_combo)) == -1)
    gtk_combo_box_set_active (GTK_COMBO_BOX (priv->res_combo), 0);

  setup_frequency_combo_box (panel, current_mode);
}


static void
setup_listbox_row_activated (GtkListBox     *list_box,
                             GtkListBoxRow  *row,
                             CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRRMode **modes;
  gint index;
  GnomeRROutput *output;

  if (!row)
    return;

  index = gtk_list_box_row_get_index (row);

  output = gnome_rr_screen_get_output_by_name (priv->screen,
                                               gnome_rr_output_info_get_name (priv->current_output));
  gtk_widget_set_sensitive (priv->config_grid, index != DISPLAY_MODE_OFF);
  gnome_rr_output_info_set_active (priv->current_output,
                                   (index != DISPLAY_MODE_OFF));

  if (index == DISPLAY_MODE_MIRROR)
    {
      modes = gnome_rr_screen_list_clone_modes (priv->screen);
      gnome_rr_config_set_clone (priv->current_configuration, TRUE);
    }
  else
    {
      gnome_rr_output_info_set_primary (priv->current_output,
                                        (index == DISPLAY_MODE_PRIMARY));
      gnome_rr_config_set_clone (priv->current_configuration, FALSE);

      modes = gnome_rr_output_list_modes (output);
    }

  setup_resolution_combo_box (panel, modes,
                              gnome_rr_output_get_current_mode (output));
  update_apply_button (panel);
}

static void
rotate_left_clicked (GtkButton      *button,
                     CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRRRotation rotation;
  gboolean active;

  active = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));

  if (active)
    {
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rotate_right_button), FALSE);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->upside_down_button), FALSE);
      rotation = GNOME_RR_ROTATION_90;
    }
  else
    {
      rotation = GNOME_RR_ROTATION_0;
    }

  gnome_rr_output_info_set_rotation (priv->current_output, rotation);
  update_apply_button (panel);
}

static void
upside_down_clicked (GtkButton      *button,
                     CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRRRotation rotation;
  gboolean active;

  active = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));

  if (active)
    {
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rotate_left_button), FALSE);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rotate_right_button), FALSE);
      rotation = GNOME_RR_ROTATION_180;
    }
  else
    {
      rotation = GNOME_RR_ROTATION_0;
    }

  gnome_rr_output_info_set_rotation (priv->current_output, rotation);
  update_apply_button (panel);
}

static void
rotate_right_clicked (GtkButton      *button,
                      CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRRRotation rotation;
  gboolean active;

  active = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));

  if (active)
    {
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rotate_left_button), FALSE);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->upside_down_button), FALSE);
      rotation = GNOME_RR_ROTATION_270;
    }
  else
    {
      rotation = GNOME_RR_ROTATION_0;
    }

  gnome_rr_output_info_set_rotation (priv->current_output, rotation);
  update_apply_button (panel);
}

static const double known_diagonals[] = {
    12.1,
    13.3,
    15.6
};

static char *
diagonal_to_str (double d)
{
    int i;

    for (i = 0; i < G_N_ELEMENTS (known_diagonals); i++)
    {
        double delta;

        delta = fabs(known_diagonals[i] - d);
        if (delta < 0.1)
            return g_strdup_printf ("%0.1lf\"", known_diagonals[i]);
    }

    return g_strdup_printf ("%d\"", (int) (d + 0.5));
}

static char *
make_display_size_string (int width_mm,
                          int height_mm)
{
  char *inches = NULL;

  if (width_mm > 0 && height_mm > 0)
    {
      double d = sqrt (width_mm * width_mm + height_mm * height_mm);

      inches = diagonal_to_str (d / 25.4);
    }

  return inches;
}

static void
freq_combo_changed (GtkComboBox    *combo,
                    CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GnomeRRMode *mode;

  model = gtk_combo_box_get_model (combo);

  if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo), &iter))
    {
      gtk_tree_model_get (GTK_TREE_MODEL (model), &iter, 1, &mode, -1);
      if (mode)
        {
          gnome_rr_output_info_set_refresh_rate (priv->current_output,
                                                 gnome_rr_mode_get_freq (mode));
          update_apply_button (panel);
        }
    }
}

static void
res_combo_changed (GtkComboBox    *combo,
                   CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GtkTreeModel *res_model;
  GtkTreeIter iter;
  GnomeRRMode *mode;
  gint x, y, width, height, i;
  GnomeRROutputInfo **outputs;

  res_model = gtk_combo_box_get_model (combo);

  if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo), &iter))
    {
      gtk_tree_model_get (GTK_TREE_MODEL (res_model), &iter, 1, &mode, -1);

      gnome_rr_output_info_get_geometry (priv->current_output, &x, &y, NULL, NULL);

      width = gnome_rr_mode_get_width (mode);
      height = gnome_rr_mode_get_height (mode);

      if (gnome_rr_config_get_clone (priv->current_configuration))
        {
          outputs = gnome_rr_config_get_outputs (priv->current_configuration);
          for (i = 0; outputs[i]; i++)
            gnome_rr_output_info_set_geometry (outputs[i], x, y, width, height);
        }
      else
        {
          gnome_rr_output_info_set_geometry (priv->current_output, x, y, width, height);
        }

      update_apply_button (panel);

      setup_frequency_combo_box (panel, mode);
    }
}

static void
sanity_check_rotation (GnomeRROutputInfo *output)
{
  GnomeRRRotation rotation;

  rotation = gnome_rr_output_info_get_rotation (output);

  /* other options such as reflection are not supported */
  rotation &= (GNOME_RR_ROTATION_0 | GNOME_RR_ROTATION_90
               | GNOME_RR_ROTATION_180 | GNOME_RR_ROTATION_270);
  if (rotation == 0)
    rotation = GNOME_RR_ROTATION_0;
  gnome_rr_output_info_set_rotation (output, rotation);
}

static gboolean
should_show_rotation (CcDisplayPanel *panel,
                      GnomeRROutput  *output)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  gboolean supports_rotation;

  supports_rotation = gnome_rr_output_info_supports_rotation (priv->current_output,
                                                              GNOME_RR_ROTATION_90 |
                                                              GNOME_RR_ROTATION_180 |
                                                              GNOME_RR_ROTATION_270);

  /* Doesn't support rotation at all */
  if (!supports_rotation)
    return FALSE;

  /* We can always rotate displays that aren't builtin */
  if (!gnome_rr_output_is_builtin_display (output))
    return TRUE;

  /* Only offer rotation if there's no accelerometer */
  return !priv->has_accelerometer;
}

static void
underscan_switch_toggled (CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  gboolean value;

  value = gtk_switch_get_active (GTK_SWITCH (priv->scaling_switch));
  gnome_rr_output_info_set_underscanning (priv->current_output, value);
  update_apply_button (panel);
}

static void
show_setup_dialog (CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GtkWidget *listbox = NULL, *content_area, *item, *box, *frame, *preview;
  GtkWidget *label, *rotate_box;
  gint i, width_mm, height_mm, old_width, old_height, grid_pos;
  GnomeRROutput *output;
  gchar *str;
  gboolean clone, was_clone, primary, was_primary, active;
  GtkListStore *res_model, *freq_model;
  GtkCellRenderer *renderer;
  GnomeRRRotation rotation;
  gboolean show_rotation;
  gint response, num_active_outputs = 0;
  GnomeRROutputInfo **outputs;

  outputs = gnome_rr_config_get_outputs (priv->current_configuration);

  /* count the number of active */
  for (i = 0; outputs[i]; i++)
    if (gnome_rr_output_info_is_active (outputs[i]))
      num_active_outputs++;

  output = gnome_rr_screen_get_output_by_name (priv->screen,
                                               gnome_rr_output_info_get_name (priv->current_output));

  priv->dialog = gtk_dialog_new_with_buttons (gnome_rr_output_info_get_display_name (priv->current_output),
                                              GTK_WINDOW (cc_shell_get_toplevel (cc_panel_get_shell (CC_PANEL (panel)))),
                                              GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                              _("_Cancel"), GTK_RESPONSE_CANCEL,
                                              _("_Apply"), GTK_RESPONSE_ACCEPT,
                                              NULL);
  g_signal_connect (priv->dialog, "notify::has-toplevel-focus",
                    G_CALLBACK (dialog_toplevel_focus_changed), panel);
  gtk_dialog_set_default_response (GTK_DIALOG (priv->dialog), GTK_RESPONSE_ACCEPT);
  gtk_dialog_set_response_sensitive (GTK_DIALOG (priv->dialog), GTK_RESPONSE_ACCEPT, FALSE);
  gtk_window_set_resizable (GTK_WINDOW (priv->dialog), FALSE);

  content_area = gtk_dialog_get_content_area (GTK_DIALOG (priv->dialog));

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_container_add (GTK_CONTAINER (content_area), box);

  /* configuration grid */
  grid_pos = 0;
  priv->config_grid = gtk_grid_new ();
  gtk_widget_set_margin_start (priv->config_grid, 36);
  gtk_widget_set_margin_end (priv->config_grid, 36);
  gtk_widget_set_margin_bottom (priv->config_grid, 6);
  gtk_grid_set_column_spacing (GTK_GRID (priv->config_grid), 12);
  gtk_grid_set_row_spacing (GTK_GRID (priv->config_grid), 12);

  /* preview */
  preview = display_preview_new (panel, priv->current_output,
                                 priv->current_configuration,
                                 cc_display_panel_get_output_id (priv->current_output),
                                 DISPLAY_PREVIEW_SETUP_HEIGHT);
  gtk_grid_attach (GTK_GRID (priv->config_grid), preview, 0, grid_pos, 2, 1);
  grid_pos++;

  /* rotation */
  show_rotation = should_show_rotation (panel, output);
  rotation = gnome_rr_output_info_get_rotation (priv->current_output);

  if (show_rotation)
    {
      rotate_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_margin_bottom (rotate_box, 12);
      gtk_style_context_add_class (gtk_widget_get_style_context (rotate_box),
                                   GTK_STYLE_CLASS_LINKED);
      gtk_grid_attach (GTK_GRID (priv->config_grid), rotate_box, 0, grid_pos, 2, 1);
      gtk_widget_set_halign (rotate_box, GTK_ALIGN_CENTER);
      grid_pos++;

      if (gnome_rr_output_info_supports_rotation (priv->current_output,
                                                  GNOME_RR_ROTATION_90))
        {
          priv->rotate_left_button = gtk_toggle_button_new ();
          gtk_widget_set_tooltip_text (priv->rotate_left_button, _("Rotate counterclockwise by 90\xc2\xb0"));
          if (rotation == GNOME_RR_ROTATION_90)
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rotate_left_button), TRUE);
          g_signal_connect (priv->rotate_left_button, "clicked",
                            G_CALLBACK (rotate_left_clicked), panel);
          g_signal_connect_swapped (priv->rotate_left_button, "clicked",
                                    G_CALLBACK (gtk_widget_queue_draw), preview);
          gtk_container_add (GTK_CONTAINER (priv->rotate_left_button),
                             gtk_image_new_from_icon_name ("object-rotate-left-symbolic",
                                                           GTK_ICON_SIZE_BUTTON));
          gtk_widget_set_halign (priv->rotate_left_button, GTK_ALIGN_END);
          gtk_container_add (GTK_CONTAINER (rotate_box), priv->rotate_left_button);
        }

      if (gnome_rr_output_info_supports_rotation (priv->current_output,
                                                  GNOME_RR_ROTATION_180))
        {
          priv->upside_down_button = gtk_toggle_button_new ();
          gtk_widget_set_tooltip_text (priv->upside_down_button, _("Rotate by 180\xc2\xb0"));
          if (rotation == GNOME_RR_ROTATION_180)
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->upside_down_button), TRUE);
          g_signal_connect (priv->upside_down_button, "clicked",
                            G_CALLBACK (upside_down_clicked), panel);
          g_signal_connect_swapped (priv->upside_down_button, "clicked",
                                    G_CALLBACK (gtk_widget_queue_draw), preview);
          gtk_container_add (GTK_CONTAINER (priv->upside_down_button),
                             gtk_image_new_from_icon_name ("object-flip-vertical-symbolic",
                                                           GTK_ICON_SIZE_BUTTON));
          gtk_widget_set_halign (priv->upside_down_button, GTK_ALIGN_FILL);
          gtk_container_add (GTK_CONTAINER (rotate_box), priv->upside_down_button);
        }

      if (gnome_rr_output_info_supports_rotation (priv->current_output,
                                                  GNOME_RR_ROTATION_270))
        {
          priv->rotate_right_button = gtk_toggle_button_new ();
          gtk_widget_set_tooltip_text (priv->rotate_right_button, _("Rotate clockwise by 90\xc2\xb0"));
          if (rotation == GNOME_RR_ROTATION_270)
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rotate_right_button), TRUE);
          g_signal_connect (priv->rotate_right_button, "clicked",
                            G_CALLBACK (rotate_right_clicked), panel);
          g_signal_connect_swapped (priv->rotate_right_button, "clicked",
                                    G_CALLBACK (gtk_widget_queue_draw), preview);
          gtk_container_add (GTK_CONTAINER (priv->rotate_right_button),
                             gtk_image_new_from_icon_name ("object-rotate-right-symbolic",
                                                           GTK_ICON_SIZE_BUTTON));
          gtk_widget_set_halign (priv->rotate_right_button, GTK_ALIGN_START);
          gtk_container_add (GTK_CONTAINER (rotate_box), priv->rotate_right_button);
        }
    }

  /* size */
  gnome_rr_output_get_physical_size (output, &width_mm, &height_mm);
  str = make_display_size_string (width_mm, height_mm);

  if (str != NULL)
    {
      label = gtk_label_new (_("Size"));
      gtk_style_context_add_class (gtk_widget_get_style_context (label),
                                   GTK_STYLE_CLASS_DIM_LABEL);
      gtk_grid_attach (GTK_GRID (priv->config_grid), label, 0, grid_pos, 1, 1);
      gtk_widget_set_halign (label, GTK_ALIGN_END);

      label = gtk_label_new (str);
      gtk_grid_attach (GTK_GRID (priv->config_grid), label, 1, grid_pos, 1, 1);
      gtk_widget_set_halign (label, GTK_ALIGN_START);
      g_free (str);

      grid_pos++;
    }

  /* aspect ratio */
  label = gtk_label_new (_("Aspect Ratio"));
  gtk_style_context_add_class (gtk_widget_get_style_context (label),
                               GTK_STYLE_CLASS_DIM_LABEL);
  gtk_grid_attach (GTK_GRID (priv->config_grid), label, 0, grid_pos, 1, 1);
  gtk_widget_set_halign (label, GTK_ALIGN_END);
  label = gtk_label_new (make_aspect_string (gnome_rr_output_info_get_preferred_width (priv->current_output),
                                             gnome_rr_output_info_get_preferred_height (priv->current_output)));
  gtk_grid_attach (GTK_GRID (priv->config_grid), label, 1, grid_pos, 1, 1);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  grid_pos++;

  /* resolution combo box */
  res_model = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_POINTER);
  priv->res_combo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (res_model));
  g_object_unref (res_model);
  renderer = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (priv->res_combo), renderer, TRUE);
  gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (priv->res_combo), renderer, "text", 0);
  g_signal_connect (priv->res_combo, "changed", G_CALLBACK (res_combo_changed),
                    panel);
  g_signal_connect_swapped (priv->res_combo, "changed",
                            G_CALLBACK (gtk_widget_queue_draw), preview);

  label = gtk_label_new (_("Resolution"));
  gtk_style_context_add_class (gtk_widget_get_style_context (label),
                               GTK_STYLE_CLASS_DIM_LABEL);
  gtk_grid_attach (GTK_GRID (priv->config_grid), label, 0, grid_pos, 1, 1);
  gtk_grid_attach (GTK_GRID (priv->config_grid), priv->res_combo, 1, grid_pos, 1, 1);
  grid_pos++;

  gtk_widget_set_halign (label, GTK_ALIGN_END);
  gtk_widget_set_halign (priv->res_combo, GTK_ALIGN_START);

  /* overscan */
  if (!gnome_rr_output_is_builtin_display (output) &&
      gnome_rr_output_supports_underscanning (output))
    {
      priv->scaling_switch = gtk_switch_new ();
      gtk_switch_set_active (GTK_SWITCH (priv->scaling_switch),
                             gnome_rr_output_info_get_underscanning (priv->current_output));
      g_signal_connect_swapped (G_OBJECT (priv->scaling_switch), "notify::active",
                                G_CALLBACK (underscan_switch_toggled), panel);

      label = gtk_label_new (_("Adjust for TV"));
      gtk_style_context_add_class (gtk_widget_get_style_context (label),
                                   GTK_STYLE_CLASS_DIM_LABEL);
      gtk_grid_attach (GTK_GRID (priv->config_grid), label, 0, grid_pos, 1, 1);
      gtk_grid_attach (GTK_GRID (priv->config_grid), priv->scaling_switch, 1, grid_pos, 1, 1);
      grid_pos++;

      gtk_widget_set_halign (label, GTK_ALIGN_END);
      gtk_widget_set_halign (priv->scaling_switch, GTK_ALIGN_START);
    }

  /* frequency combo box */
  freq_model = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_POINTER);
  priv->freq_combo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (freq_model));
  g_object_unref (freq_model);
  renderer = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (priv->freq_combo), renderer, TRUE);
  gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (priv->freq_combo), renderer, "text", 0);
  g_signal_connect (priv->freq_combo, "changed", G_CALLBACK (freq_combo_changed),
                    panel);
  gtk_grid_attach (GTK_GRID (priv->config_grid), priv->freq_combo, 1, grid_pos, 1, 1);
  gtk_widget_set_halign (priv->freq_combo, GTK_ALIGN_START);
  gtk_widget_set_no_show_all (priv->freq_combo, TRUE);

  label = gtk_label_new (_("Refresh Rate"));
  gtk_style_context_add_class (gtk_widget_get_style_context (label),
                               GTK_STYLE_CLASS_DIM_LABEL);
  gtk_grid_attach (GTK_GRID (priv->config_grid), label, 0, grid_pos, 1, 1);
  gtk_widget_set_halign (label, GTK_ALIGN_END);
  gtk_widget_set_no_show_all (label, TRUE);
  g_object_bind_property (priv->freq_combo, "visible",
                          label, "visible", G_BINDING_BIDIRECTIONAL);
  grid_pos++;

  was_clone = clone = gnome_rr_config_get_clone (priv->current_configuration);
  was_primary = primary = gnome_rr_output_info_get_primary (priv->current_output);
  active = gnome_rr_output_info_is_active (priv->current_output);

  if (num_active_outputs > 1 || !active)
    {
      frame = gtk_frame_new (NULL);
      gtk_container_add (GTK_CONTAINER (box), frame);
      gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);

      listbox = gtk_list_box_new ();
      gtk_container_add (GTK_CONTAINER (frame), listbox);
      gtk_list_box_set_header_func (GTK_LIST_BOX (listbox),
                                    cc_list_box_update_header_func,
                                    NULL, NULL);
      g_signal_connect (listbox, "row-selected",
                        G_CALLBACK (setup_listbox_row_activated), panel);
      g_signal_connect_swapped (listbox, "row-selected",
                                G_CALLBACK (gtk_widget_queue_draw), preview);
      gtk_widget_show (listbox);

      item = list_box_item (_("Primary"),
                            _("Show the top bar and Activities Overview on this display"));
      gtk_container_add (GTK_CONTAINER (listbox), item);
      if (primary)
        gtk_list_box_select_row (GTK_LIST_BOX (listbox),
                                 GTK_LIST_BOX_ROW (item));

      item = list_box_item (_("Secondary Display"),
                            _("Join this display with another to create an extra workspace"));
      gtk_container_add (GTK_CONTAINER (listbox), item);
      if (!primary && !clone)
        gtk_list_box_select_row (GTK_LIST_BOX (listbox),
                                 GTK_LIST_BOX_ROW (item));

#if 0
      item = list_box_item (_("Presentation"),
                            _("Show slideshows and media only"));
      gtk_container_add (GTK_CONTAINER (listbox), item);
#endif

      /* translators: "Mirror" describes when both displays show the same view */
      item = list_box_item (_("Mirror"),
                            _("Show your existing view on both displays"));
      gtk_container_add (GTK_CONTAINER (listbox), item);
      if (clone && active)
        gtk_list_box_select_row (GTK_LIST_BOX (listbox),
                                 GTK_LIST_BOX_ROW (item));

      item = list_box_item (_("Turn Off"),
                            _("Don't use this display"));
      gtk_container_add (GTK_CONTAINER (listbox), item);

      if (!active)
        gtk_list_box_select_row (GTK_LIST_BOX (listbox),
                                 GTK_LIST_BOX_ROW (item));
    }
  else
    {
      GnomeRRMode **modes;

      modes = gnome_rr_output_list_modes (output);
      setup_resolution_combo_box (panel, modes,
                                  gnome_rr_output_get_current_mode (output));
    }

  content_area = gtk_dialog_get_content_area (GTK_DIALOG (priv->dialog));
  gtk_container_add (GTK_CONTAINER (box), priv->config_grid);
  gtk_widget_show_all (box);

  gnome_rr_output_info_get_geometry (priv->current_output, NULL, NULL,
                                     &old_width, &old_height);

  response = gtk_dialog_run (GTK_DIALOG (priv->dialog));
  if (response == GTK_RESPONSE_ACCEPT)
    {
      GtkListBoxRow *row;
      gboolean active = TRUE;

      if (g_hash_table_size (output_ids) > 1)
        {
          gint new_width, new_height;
          gboolean primary_chosen = FALSE;

          gnome_rr_output_info_get_geometry (priv->current_output, NULL, NULL,
                                             &new_width, &new_height);

          row = gtk_list_box_get_selected_row (GTK_LIST_BOX (listbox));

          switch (gtk_list_box_row_get_index (row))
            {
            case DISPLAY_MODE_PRIMARY:
              primary = TRUE;
              clone = FALSE;
              break;

#if 0
            case DISPLAY_MODE_PRESENTATION:
              gnome_rr_config_set_clone (priv->current_configuration, FALSE);
              primary = FALSE;
              clone = FALSE;
              break;
#endif

            case DISPLAY_MODE_MIRROR:
              clone = TRUE;
              break;

            case DISPLAY_MODE_SECONDARY:
              primary = FALSE;
              clone = FALSE;
              break;

            case DISPLAY_MODE_OFF:
              clone = FALSE;
              active = FALSE;
              break;
            }

          gnome_rr_output_info_set_active (priv->current_output, active);
          gnome_rr_output_info_set_primary (priv->current_output, primary);
          gnome_rr_config_set_clone (priv->current_configuration, clone);

          for (i = 0; outputs[i]; i++)
            {
              if (!gnome_rr_output_info_is_active (outputs[i]))
                continue;

              if (clone)
                {
                  /* set all active outputs to the same size and position when
                   * cloning */
                  gnome_rr_output_info_set_geometry (outputs[i], 0, 0,
                                                     new_width, new_height);
                }
              else if (outputs[i] != priv->current_output)
                {
                  /* ensure no other outputs are primary if this output is now
                   * primary, or find another output to set as primary if this
                   * output is no longer primary */
                  if (primary)
                    {
                      gnome_rr_output_info_set_primary (outputs[i], FALSE);
                    }
                  else if (!primary_chosen && gnome_rr_output_info_is_primary_tile (outputs[i]))
                    {
                      gnome_rr_output_info_set_primary (outputs[i], TRUE);
                      primary_chosen = TRUE;
                    }
                }
            }

          sanity_check_rotation (priv->current_output);

          /* if the display was previously in clone mode, ensure the outputs
           * are arranged correctly */
          if ((was_clone && !clone))
            lay_out_outputs_horizontally (panel);

          if (!clone)
            realign_outputs_after_resolution_change (panel,
                                                     priv->current_output,
                                                     old_width, old_height, rotation);
        }
      else
        {
          /* check rotation */
          sanity_check_rotation (priv->current_output);
        }

      apply_current_configuration (panel);
    }
  else if (response != GTK_RESPONSE_NONE)
    {
      /* changes cancelled, so re-read the current configuration */
      on_screen_changed (panel);
    }

  priv->rotate_left_button = NULL;
  priv->rotate_right_button = NULL;
  priv->res_combo = NULL;
  priv->freq_combo = NULL;
  clear_res_freqs (panel);
  gtk_widget_destroy (priv->dialog);
  priv->dialog = NULL;
}

static void
cc_display_panel_box_row_activated (GtkListBox     *listbox,
                                    GtkWidget      *row,
                                    CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  GnomeRROutputInfo *output_info;

  gtk_list_box_select_row (listbox, NULL);

  output_info = g_object_get_data (G_OBJECT (row), "gnome-rr-output");

  if (!output_info)
    return;

  priv->current_output = output_info;

  show_setup_dialog (panel);
}

static void
mapped_cb (CcDisplayPanel *panel)
{
  CcDisplayPanelPrivate *priv = panel->priv;
  CcShell *shell;
  GtkWidget *toplevel;

  shell = cc_panel_get_shell (CC_PANEL (panel));
  toplevel = cc_shell_get_toplevel (shell);
  if (toplevel)
    priv->focus_id = g_signal_connect (toplevel, "notify::has-toplevel-focus",
                                       G_CALLBACK (dialog_toplevel_focus_changed), panel);
}

static void
cc_display_panel_up_client_changed (UpClient       *client,
                                    GParamSpec     *pspec,
                                    CcDisplayPanel *self)
{
  CcDisplayPanelPrivate *priv = self->priv;
  gboolean lid_is_closed;

  lid_is_closed = up_client_get_lid_is_closed (client);

  if (lid_is_closed != priv->lid_is_closed)
    {
      priv->lid_is_closed = lid_is_closed;

      on_screen_changed (self);
    }
}

static void
shell_proxy_ready (GObject        *source,
                   GAsyncResult   *res,
                   CcDisplayPanel *self)
{
  GDBusProxy *proxy;
  GError *error = NULL;

  proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to contact gnome-shell: %s", error->message);
      g_error_free (error);
      return;
    }

  self->priv->shell_proxy = proxy;

  ensure_monitor_labels (self);
}

static void
update_has_accel (CcDisplayPanel *self)
{
  GVariant *v;

  if (self->priv->iio_sensor_proxy == NULL)
    {
      g_debug ("Has no accelerometer");
      self->priv->has_accelerometer = FALSE;
      return;
    }

  v = g_dbus_proxy_get_cached_property (self->priv->iio_sensor_proxy, "HasAccelerometer");
  if (v)
    {
      self->priv->has_accelerometer = g_variant_get_boolean (v);
      g_variant_unref (v);
    }
  else
    {
      self->priv->has_accelerometer = FALSE;
    }

  g_debug ("Has %saccelerometer", self->priv->has_accelerometer ? "" : "no ");
}

static void
sensor_proxy_properties_changed_cb (GDBusProxy     *proxy,
                                    GVariant       *changed_properties,
                                    GStrv           invalidated_properties,
                                    CcDisplayPanel *self)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, changed_properties);

  if (g_variant_dict_contains (&dict, "HasAccelerometer"))
    update_has_accel (self);
}

static void
sensor_proxy_appeared_cb (GDBusConnection *connection,
                          const gchar     *name,
                          const gchar     *name_owner,
                          gpointer         user_data)
{
  CcDisplayPanel *self = user_data;

  g_debug ("SensorProxy appeared");

  self->priv->iio_sensor_proxy = g_dbus_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        NULL,
                                                        "net.hadess.SensorProxy",
                                                        "/net/hadess/SensorProxy",
                                                        "net.hadess.SensorProxy",
                                                        NULL,
                                                        NULL);
  g_return_if_fail (self->priv->iio_sensor_proxy);

  g_signal_connect (self->priv->iio_sensor_proxy, "g-properties-changed",
                    G_CALLBACK (sensor_proxy_properties_changed_cb), self);
  update_has_accel (self);
}

static void
sensor_proxy_vanished_cb (GDBusConnection *connection,
                          const gchar     *name,
                          gpointer         user_data)
{
  CcDisplayPanel *self = user_data;

  g_debug ("SensorProxy vanished");

  g_clear_object (&self->priv->iio_sensor_proxy);
  update_has_accel (self);
}

static void
cc_display_panel_init (CcDisplayPanel *self)
{
  CcDisplayPanelPrivate *priv;
  GtkWidget *frame, *vbox;
  GError *error = NULL;
  GSettings *settings;

  priv = self->priv = DISPLAY_PANEL_PRIVATE (self);

  settings = g_settings_new ("org.gnome.desktop.background");
  priv->background = gnome_bg_new ();
  gnome_bg_load_from_preferences (priv->background, settings);
  g_object_unref (settings);

  priv->thumbnail_factory = gnome_desktop_thumbnail_factory_new (GNOME_DESKTOP_THUMBNAIL_SIZE_NORMAL);


  priv->screen = gnome_rr_screen_new (gdk_screen_get_default (), &error);
  if (!priv->screen)
    {
      gtk_container_add (GTK_CONTAINER (self),
                         gtk_label_new (_("Could not get screen information")));
      g_error_free (error);

      gtk_widget_show_all (GTK_WIDGET (self));

      return;
    }

  output_ids = g_hash_table_new (g_direct_hash, g_direct_equal);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_add (GTK_CONTAINER (self), vbox);

  frame = gtk_frame_new (NULL);
  gtk_widget_set_margin_start (vbox, 134);
  gtk_widget_set_margin_end (vbox, 134);
  gtk_widget_set_margin_top (vbox, 22);
  gtk_widget_set_margin_bottom (vbox, 22);
  gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (vbox), frame);

  priv->displays_listbox = gtk_list_box_new ();
  gtk_list_box_set_header_func (GTK_LIST_BOX (priv->displays_listbox),
                                cc_list_box_update_header_func, NULL,
                                NULL);
  g_signal_connect (priv->displays_listbox, "row-activated",
                    G_CALLBACK (cc_display_panel_box_row_activated),
                    self);
  gtk_container_add (GTK_CONTAINER (frame), priv->displays_listbox);


  priv->arrange_button = gtk_button_new_with_mnemonic (_("_Arrange Combined Displays"));
  g_signal_connect (priv->arrange_button, "clicked",
                    G_CALLBACK (show_arrange_displays_dialog), self);
  gtk_widget_set_halign (priv->arrange_button, GTK_ALIGN_CENTER);

  gtk_container_add (GTK_CONTAINER (vbox), priv->arrange_button);
  gtk_widget_show_all (vbox);

  on_screen_changed (self);
  priv->screen_changed_handler_id = g_signal_connect_swapped (priv->screen,
                                                              "changed",
                                                              G_CALLBACK (on_screen_changed),
                                                              self);

  self->priv->up_client = up_client_new ();
  if (up_client_get_lid_is_present (self->priv->up_client))
    {
      /* Connect to the "changed" signal to track changes to "lid-is-closed"
       * property. Connecting to "notify::lid-is-closed" would be preferable,
       * but currently doesn't work as expected:
       * https://bugs.freedesktop.org/show_bug.cgi?id=43001
       */

      g_signal_connect (self->priv->up_client, "notify::lid-is-closed",
                        G_CALLBACK (cc_display_panel_up_client_changed), self);
      cc_display_panel_up_client_changed (self->priv->up_client, NULL, self);
    }
  else
    g_clear_object (&self->priv->up_client);

  g_signal_connect (self, "map", G_CALLBACK (mapped_cb), NULL);

  self->priv->shell_cancellable = g_cancellable_new ();
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                            G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS |
                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                            NULL,
                            "org.gnome.Shell",
                            "/org/gnome/Shell",
                            "org.gnome.Shell",
                            self->priv->shell_cancellable,
                            (GAsyncReadyCallback) shell_proxy_ready,
                            self);

  priv->sensor_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                            "net.hadess.SensorProxy",
                                            G_BUS_NAME_WATCHER_FLAGS_NONE,
                                            sensor_proxy_appeared_cb,
                                            sensor_proxy_vanished_cb,
                                            self,
                                            NULL);
}
