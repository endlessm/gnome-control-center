/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Mohammed Sadiq <sadiq@sadiqpk.org>
 * Copyright (C) 2010 Red Hat, Inc
 * Copyright (C) 2008 William Jon McCann <jmccann@redhat.com>
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
 */

#include <config.h>

#include "cc-info-panel.h"
#include "cc-info-resources.h"
#include "eos-updater-generated.h"
#include "info-cleanup.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <gio/gdesktopappinfo.h>

#include <glibtop/fsusage.h>
#include <glibtop/mountlist.h>
#include <glibtop/mem.h>
#include <glibtop/sysinfo.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "gsd-disk-space-helper.h"

#include "cc-info-overview-panel.h"

typedef enum {
  EOS_UPDATER_STATE_NONE = 0,
  EOS_UPDATER_STATE_READY,
  EOS_UPDATER_STATE_ERROR,
  EOS_UPDATER_STATE_POLLING,
  EOS_UPDATER_STATE_UPDATE_AVAILABLE,
  EOS_UPDATER_STATE_FETCHING,
  EOS_UPDATER_STATE_UPDATE_READY,
  EOS_UPDATER_STATE_APPLYING_UPDATE,
  EOS_UPDATER_STATE_UPDATE_APPLIED,
  EOS_UPDATER_N_STATES,
} EosUpdaterState;

#define EOS_UPDATER_ERROR_LIVE_BOOT_STR "com.endlessm.Updater.Error.LiveBoot"
#define EOS_UPDATER_ERROR_NON_OSTREE_STR "com.endlessm.Updater.Error.NotOstreeSystem"

typedef struct {
  /* Will be one or 2 GPU name strings, or "Unknown" */
  char *hardware_string;
} GraphicsData;

typedef struct
{
  GtkWidget      *system_image;
  GtkWidget      *version_label;
  GtkWidget      *name_entry;
  GtkWidget      *memory_label;
  GtkWidget      *processor_label;
  GtkWidget      *os_name_label;
  GtkWidget      *os_type_label;
  GtkWidget      *disk_label;
  GtkWidget      *graphics_label;
  GtkWidget      *virt_type_label;

  GtkWidget      *os_updates_box;
  GtkWidget      *os_updates_label;
  GtkWidget      *os_updates_spinner;

  /* Virtualisation labels */
  GtkWidget      *label8;
  GtkWidget      *grid1;
  GtkWidget      *label18;

  char           *gnome_version;
  char           *gnome_distributor;
  char           *gnome_date;

  GCancellable   *cancellable;

  /* Free space */
  GList          *primary_mounts;
  guint64         total_bytes;

  GraphicsData   *graphics_data;

  EosUpdater     *updater_proxy;
  GDBusProxy     *session_proxy;
  GCancellable   *updater_cancellable;
  gboolean        updater_activated;
} CcInfoOverviewPanelPrivate;

struct _CcInfoOverviewPanel
{
 CcPanel parent_instance;

  /*< private >*/
 CcInfoOverviewPanelPrivate *priv;
};

static void get_primary_disc_info_start     (CcInfoOverviewPanel *self);
static void sync_state_from_updater         (CcInfoOverviewPanel *self,
                                             gboolean             is_initial_state);
static void sync_initial_state_from_updater (CcInfoOverviewPanel *self);
static gboolean updates_link_activated      (GtkLabel            *label,
                                             gchar               *uri,
                                             CcInfoOverviewPanel *self);

typedef struct
{
  char *major;
  char *minor;
  char *micro;
  char *distributor;
  char *date;
  char **current;
} VersionData;


G_DEFINE_TYPE_WITH_PRIVATE (CcInfoOverviewPanel, cc_info_overview_panel, CC_TYPE_PANEL)

static gboolean
is_updater_state_spinning (EosUpdaterState state)
{
  switch (state)
    {
    case EOS_UPDATER_STATE_POLLING:
    case EOS_UPDATER_STATE_FETCHING:
    case EOS_UPDATER_STATE_APPLYING_UPDATE:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
is_updater_state_interactive (CcInfoOverviewPanel *self,
                              EosUpdaterState      state)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  switch (state)
    {
    case EOS_UPDATER_STATE_READY:
    case EOS_UPDATER_STATE_ERROR:
    case EOS_UPDATER_STATE_UPDATE_AVAILABLE:
    case EOS_UPDATER_STATE_UPDATE_READY:
      return (!priv->updater_activated);
    case EOS_UPDATER_STATE_UPDATE_APPLIED:
      return TRUE;
    default:
      return FALSE;
    }
}

static const gchar *
get_message_for_updater_state (CcInfoOverviewPanel *self,
                               EosUpdaterState      state)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  switch (state)
    {
    case EOS_UPDATER_STATE_NONE:
    case EOS_UPDATER_STATE_READY:
      if (priv->updater_activated)
        return _("No updates available");
      else
        return _("Check for updates now");
    case EOS_UPDATER_STATE_ERROR:
      return _("Update failed");
    case EOS_UPDATER_STATE_POLLING:
      return _("Checking for updates…");
    case EOS_UPDATER_STATE_UPDATE_AVAILABLE:
    case EOS_UPDATER_STATE_UPDATE_READY:
      return _("Install updates now");
    case EOS_UPDATER_STATE_FETCHING:
    case EOS_UPDATER_STATE_APPLYING_UPDATE:
      return _("Installing updates…");
    case EOS_UPDATER_STATE_UPDATE_APPLIED:
      return _("Restart to complete update");
    default:
      return NULL;
    }
}

static void
updates_apply_completed (GObject      *object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  EosUpdater *proxy = (EosUpdater *) object;
  g_autoptr(GError) error = NULL;

  eos_updater_call_apply_finish (proxy, res, &error);
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      CcInfoOverviewPanel *self = user_data;
      g_warning ("Failed to call Apply() on EOS updater: %s", error->message);
      sync_state_from_updater (self, FALSE);
    }
}

static void
updates_fetch_completed (GObject      *object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  EosUpdater *proxy = (EosUpdater *) object;
  g_autoptr(GError) error = NULL;

  eos_updater_call_fetch_finish (proxy, res, &error);
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      CcInfoOverviewPanel *self = user_data;
      g_warning ("Failed to call Fetch() on EOS updater: %s", error->message);
      sync_state_from_updater (self, FALSE);
    }
}

static void
updates_poll_completed (GObject      *object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  EosUpdater *proxy = (EosUpdater *) object;
  g_autoptr(GError) error = NULL;

  eos_updater_call_poll_finish (proxy, res, &error);
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      CcInfoOverviewPanel *self = user_data;
      g_warning ("Failed to call Poll() on EOS updater: %s", error->message);
      sync_state_from_updater (self, FALSE);
    }
}

static gboolean
updates_link_activated (GtkLabel            *label,
                        gchar               *uri,
                        CcInfoOverviewPanel *self)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  EosUpdaterState state;

  state = eos_updater_get_state (priv->updater_proxy);
  g_assert (is_updater_state_interactive (self, state));
  priv->updater_activated = TRUE;

  switch (state)
    {
    case EOS_UPDATER_STATE_ERROR:
    case EOS_UPDATER_STATE_READY:
      eos_updater_call_poll (priv->updater_proxy,
                             priv->updater_cancellable,
                             updates_poll_completed, self);
      break;
    case EOS_UPDATER_STATE_UPDATE_AVAILABLE:
      eos_updater_call_fetch (priv->updater_proxy,
                              priv->updater_cancellable,
                              updates_fetch_completed, self);
      break;
    case EOS_UPDATER_STATE_UPDATE_READY:
      eos_updater_call_apply (priv->updater_proxy,
                              priv->updater_cancellable,
                              updates_apply_completed, self);
      break;
    case EOS_UPDATER_STATE_UPDATE_APPLIED:
      g_dbus_proxy_call (priv->session_proxy,
                         "Reboot",
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1, NULL, NULL, NULL);
      break;
    default:
      g_assert_not_reached ();
    }

  return TRUE;
}

static void
updates_maybe_do_automatic_step (CcInfoOverviewPanel *self)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  EosUpdaterState state;

  if (!priv->updater_activated)
    return;

  state = eos_updater_get_state (priv->updater_proxy);
  switch (state)
    {
    case EOS_UPDATER_STATE_UPDATE_AVAILABLE:
      eos_updater_call_fetch (priv->updater_proxy,
                              priv->updater_cancellable,
                              updates_fetch_completed, self);
      break;
    case EOS_UPDATER_STATE_UPDATE_READY:
      eos_updater_call_apply (priv->updater_proxy,
                              priv->updater_cancellable,
                              updates_apply_completed, self);
      break;
    default:
      break;
    }
}

static void
sync_state_from_updater (CcInfoOverviewPanel *self,
                         gboolean             is_initial_state)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  EosUpdaterState state;
  gboolean is_live_boot, is_non_ostree;
  gboolean state_spinning, state_interactive;
  const gchar *message, *error_name;
  g_autofree gchar *markup = NULL;

  state = eos_updater_get_state (priv->updater_proxy);
  error_name = eos_updater_get_error_name (priv->updater_proxy);
  is_live_boot = state == EOS_UPDATER_STATE_ERROR &&
    g_strcmp0 (error_name, EOS_UPDATER_ERROR_LIVE_BOOT_STR) == 0;
  is_non_ostree = state == EOS_UPDATER_STATE_ERROR &&
    g_strcmp0 (error_name, EOS_UPDATER_ERROR_NON_OSTREE_STR) == 0;

  /* Attempt to clear the error by pretending to be ready, which will
   * trigger a poll
   */
  if (state == EOS_UPDATER_STATE_ERROR && is_initial_state)
    state = EOS_UPDATER_STATE_READY;

  state_spinning = is_updater_state_spinning (state);
  state_interactive = is_updater_state_interactive (self, state);
  message = get_message_for_updater_state (self, state);

  gtk_widget_set_visible (priv->os_updates_spinner, state_spinning);
  g_object_set (priv->os_updates_spinner, "active", state_spinning, NULL);

  gtk_widget_set_visible (priv->os_updates_label, !is_live_boot && !is_non_ostree);
  if (state_interactive)
    markup = g_strdup_printf ("<a href='updates-link'>%s</a>", message);
  else
    markup = g_strdup (message);

  gtk_label_set_markup (GTK_LABEL (priv->os_updates_label), markup);

  updates_maybe_do_automatic_step (self);
}

static void
updater_state_changed (EosUpdater          *proxy,
                       GParamSpec          *pspec,
                       CcInfoOverviewPanel *self)
{
  sync_state_from_updater (self, FALSE);
}

static void
sync_initial_state_from_updater (CcInfoOverviewPanel *self)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);

  priv->updater_cancellable = g_cancellable_new ();

  sync_state_from_updater (self, TRUE);

  g_signal_connect (priv->updater_proxy, "notify::state", G_CALLBACK (updater_state_changed), self);
}

static void
info_overview_panel_setup_eos_updater (CcInfoOverviewPanel *self)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  g_autoptr(GError) error = NULL;

  priv->updater_proxy = eos_updater_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                            G_DBUS_PROXY_FLAGS_NONE,
                                                            "com.endlessm.Updater",
                                                            "/com/endlessm/Updater",
                                                            NULL,
                                                            &error);

  if (error)
    {
      g_critical ("Unable to get a proxy to the EOS updater: %s. Updates will not be available.",
                  error->message);
    }

  priv->session_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       NULL,
                                                       "org.gnome.SessionManager",
                                                       "/org/gnome/SessionManager",
                                                       "org.gnome.SessionManager",
                                                       NULL,
                                                       &error);

  if (error)
    {
      g_critical ("Unable to get a proxy to gnome-session: %s. Updates will not be available.",
                  error->message);
    }

  if (!priv->updater_proxy || !priv->session_proxy)
    {
      gtk_widget_set_visible (priv->os_updates_box, FALSE);
    }
  else
    {
      g_signal_connect (priv->os_updates_label, "activate-link", G_CALLBACK (updates_link_activated), self);
      sync_initial_state_from_updater (self);
    }
}

static gboolean
on_attribution_label_link (GtkLabel            *label,
                           gchar               *uri,
                           CcInfoOverviewPanel *self)
{
  g_autoptr(GError) error = NULL;
  const gchar * const * languages;
  const gchar * const * data_dirs;
  const gchar *language;
  g_autofree gchar *pdf_uri = NULL;
  g_autofree gchar *path = NULL;
  gint i, j;
  gboolean found = FALSE;

  if (g_strcmp0 (uri, "attribution-link") != 0)
    return FALSE;

  data_dirs = g_get_system_data_dirs ();
  languages = g_get_language_names ();
  path = NULL;

  for (i = 0; languages[i] != NULL; i++)
    {
      language = languages[i];

      for (j = 0; data_dirs[j] != NULL; j++)
        {
          path = g_build_filename (data_dirs[j],
                                   "eos-license-service",
                                   "terms",
                                   language,
                                   "Endless-Terms-of-Use.pdf",
                                   NULL);

          if (g_file_test (path, G_FILE_TEST_EXISTS))
            {
              found = TRUE;
              break;
            }

          g_free (path);
          path = NULL;
        }

      if (found)
        break;
    }

  if (!found)
    {
      g_warning ("Unable to find terms and conditions PDF on the system");
      return TRUE;
    }

  pdf_uri = g_filename_to_uri (path, NULL, &error);

  if (error)
    {
      g_warning ("Unable to construct terms and conditions PDF uri: %s", error->message);
      return TRUE;
    }

  gtk_show_uri (NULL, pdf_uri, gtk_get_current_event_time (), &error);

  if (error)
    g_warning ("Unable to display terms and conditions PDF: %s", error->message);

  return TRUE;
}

static void
version_start_element_handler (GMarkupParseContext      *ctx,
                               const char               *element_name,
                               const char              **attr_names,
                               const char              **attr_values,
                               gpointer                  user_data,
                               GError                  **error)
{
  VersionData *data = user_data;
  if (g_str_equal (element_name, "platform"))
    data->current = &data->major;
  else if (g_str_equal (element_name, "minor"))
    data->current = &data->minor;
  else if (g_str_equal (element_name, "micro"))
    data->current = &data->micro;
  else if (g_str_equal (element_name, "distributor"))
    data->current = &data->distributor;
  else if (g_str_equal (element_name, "date"))
    data->current = &data->date;
  else
    data->current = NULL;
}

static void
version_end_element_handler (GMarkupParseContext      *ctx,
                             const char               *element_name,
                             gpointer                  user_data,
                             GError                  **error)
{
  VersionData *data = user_data;
  data->current = NULL;
}

static void
version_text_handler (GMarkupParseContext *ctx,
                      const char          *text,
                      gsize                text_len,
                      gpointer             user_data,
                      GError             **error)
{
  VersionData *data = user_data;
  if (data->current != NULL)
    *data->current = g_strstrip (g_strdup (text));
}

static gboolean
load_gnome_version (char **version,
                    char **distributor,
                    char **date)
{
  GMarkupParser version_parser = {
    version_start_element_handler,
    version_end_element_handler,
    version_text_handler,
    NULL,
    NULL,
  };
  GError              *error;
  GMarkupParseContext *ctx;
  char                *contents;
  gsize                length;
  VersionData         *data;
  gboolean             ret;

  ret = FALSE;

  error = NULL;
  if (!g_file_get_contents (DATADIR "/gnome/gnome-version.xml",
                            &contents,
                            &length,
                            &error))
    return FALSE;

  data = g_new0 (VersionData, 1);
  ctx = g_markup_parse_context_new (&version_parser, 0, data, NULL);

  if (!g_markup_parse_context_parse (ctx, contents, length, &error))
    {
      g_warning ("Invalid version file: '%s'", error->message);
    }
  else
    {
      if (version != NULL)
        *version = g_strdup_printf ("%s.%s.%s", data->major, data->minor, data->micro);
      if (distributor != NULL)
        *distributor = g_strdup (data->distributor);
      if (date != NULL)
        *date = g_strdup (data->date);

      ret = TRUE;
    }

  g_markup_parse_context_free (ctx);
  g_free (data->major);
  g_free (data->minor);
  g_free (data->micro);
  g_free (data->distributor);
  g_free (data->date);
  g_free (data);
  g_free (contents);

  return ret;
};

static void
graphics_data_free (GraphicsData *gdata)
{
  g_free (gdata->hardware_string);
  g_slice_free (GraphicsData, gdata);
}

static char *
get_renderer_from_session (void)
{
  GDBusProxy *session_proxy;
  GVariant *renderer_variant;
  char *renderer;
  GError *error = NULL;

  session_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 NULL,
                                                 "org.gnome.SessionManager",
                                                 "/org/gnome/SessionManager",
                                                 "org.gnome.SessionManager",
                                                 NULL, &error);
  if (error != NULL)
    {
      g_warning ("Unable to connect to create a proxy for org.gnome.SessionManager: %s",
                 error->message);
      g_error_free (error);
      return NULL;
    }

  renderer_variant = g_dbus_proxy_get_cached_property (session_proxy, "Renderer");
  g_object_unref (session_proxy);

  if (!renderer_variant)
    {
      g_warning ("Unable to retrieve org.gnome.SessionManager.Renderer property");
      return NULL;
    }

  renderer = info_cleanup (g_variant_get_string (renderer_variant, NULL));
  g_variant_unref (renderer_variant);

  return renderer;
}

static char *
get_renderer_from_helper (gboolean discrete_gpu)
{
  int status;
  char *argv[] = { GNOME_SESSION_DIR "/gnome-session-check-accelerated", NULL };
  char **envp = NULL;
  char *renderer = NULL;
  char *ret = NULL;
  GError *error = NULL;

  if (discrete_gpu)
    {
      envp = g_get_environ ();
      envp = g_environ_setenv (envp, "DRI_PRIME", "1", TRUE);
    }

  if (!g_spawn_sync (NULL, (char **) argv, envp, 0, NULL, NULL, &renderer, NULL, &status, &error))
    {
      g_debug ("Failed to get %s GPU: %s",
               discrete_gpu ? "discrete" : "integrated",
               error->message);
      g_error_free (error);
      goto out;
    }

  if (!g_spawn_check_exit_status (status, NULL))
    goto out;

  if (renderer == NULL || *renderer == '\0')
    goto out;

  ret = info_cleanup (renderer);

out:
  g_free (renderer);
  g_strfreev (envp);
  return ret;
}

static gboolean
has_dual_gpu (void)
{
  GDBusProxy *switcheroo_proxy;
  GVariant *dualgpu_variant;
  gboolean ret;
  GError *error = NULL;

  switcheroo_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    NULL,
                                                    "net.hadess.SwitcherooControl",
                                                    "/net/hadess/SwitcherooControl",
                                                    "net.hadess.SwitcherooControl",
                                                    NULL, &error);
  if (switcheroo_proxy == NULL)
    {
      g_debug ("Unable to connect to create a proxy for net.hadess.SwitcherooControl: %s",
               error->message);
      g_error_free (error);
      return FALSE;
    }

  dualgpu_variant = g_dbus_proxy_get_cached_property (switcheroo_proxy, "HasDualGpu");
  g_object_unref (switcheroo_proxy);

  if (!dualgpu_variant)
    {
      g_debug ("Unable to retrieve net.hadess.SwitcherooControl.HasDualGpu property, the daemon is likely not running");
      return FALSE;
    }

  ret = g_variant_get_boolean (dualgpu_variant);
  g_variant_unref (dualgpu_variant);

  if (ret)
    g_debug ("Dual-GPU machine detected");

  return ret;
}

static GraphicsData *
get_graphics_data (void)
{
  GraphicsData *result;
  GdkDisplay *display;

  result = g_slice_new0 (GraphicsData);

  display = gdk_display_get_default ();

#if defined(GDK_WINDOWING_X11) || defined(GDK_WINDOWING_WAYLAND)
  gboolean x11_or_wayland = FALSE;
#ifdef GDK_WINDOWING_X11
  x11_or_wayland = GDK_IS_X11_DISPLAY (display);
#endif
#ifdef GDK_WINDOWING_WAYLAND
  x11_or_wayland = x11_or_wayland || GDK_IS_WAYLAND_DISPLAY (display);
#endif

  if (x11_or_wayland)
    {
      char *discrete_renderer = NULL;
      char *renderer;

      renderer = get_renderer_from_session ();
      if (!renderer)
        renderer = get_renderer_from_helper (FALSE);
      if (has_dual_gpu ())
        discrete_renderer = get_renderer_from_helper (TRUE);
      if (!discrete_renderer)
        result->hardware_string = g_strdup (renderer);
      else
        result->hardware_string = g_strdup_printf ("%s / %s",
                                                   renderer,
                                                   discrete_renderer);
      g_free (renderer);
      g_free (discrete_renderer);
    }
#endif

  if (!result->hardware_string)
    result->hardware_string = g_strdup (_("Unknown"));

  return result;
}

static GHashTable*
get_os_info (void)
{
  GHashTable *hashtable;
  gchar *buffer;

  hashtable = NULL;

  if (g_file_get_contents ("/etc/os-release", &buffer, NULL, NULL))
    {
      gchar **lines;
      gint i;

      lines = g_strsplit (buffer, "\n", -1);

      for (i = 0; lines[i] != NULL; i++)
        {
          gchar *delimiter, *key, *value;

          /* Initialize the hash table if needed */
          if (!hashtable)
            hashtable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

          delimiter = strstr (lines[i], "=");
          value = NULL;
          key = NULL;

          if (delimiter != NULL)
            {
              gint size;

              key = g_strndup (lines[i], delimiter - lines[i]);

              /* Jump the '=' */
              delimiter += strlen ("=");

              /* Eventually jump the ' " ' character */
              if (g_str_has_prefix (delimiter, "\""))
                delimiter += strlen ("\"");

              size = strlen (delimiter);

              /* Don't consider the last ' " ' too */
              if (g_str_has_suffix (delimiter, "\""))
                size -= strlen ("\"");

              value = g_strndup (delimiter, size);

              g_hash_table_insert (hashtable, key, value);
            }
        }

      g_strfreev (lines);
      g_free (buffer);
    }

  return hashtable;
}

static char *
get_os_name (void)
{
  GHashTable *os_info;
  gchar *name, *version_id, *pretty_name, *build_id;
  gchar *result = NULL;
  g_autofree gchar *name_version = NULL;

  os_info = get_os_info ();

  if (!os_info)
    return NULL;

  name = g_hash_table_lookup (os_info, "NAME");
  version_id = g_hash_table_lookup (os_info, "VERSION_ID");
  pretty_name = g_hash_table_lookup (os_info, "PRETTY_NAME");
  build_id = g_hash_table_lookup (os_info, "BUILD_ID");

  if (pretty_name)
    name_version = g_strdup (pretty_name);
  else if (name && version_id)
    name_version = g_strdup_printf ("%s %s", name, version_id);
  else
    name_version = g_strdup (_("Unknown"));

  if (build_id)
    {
      /* translators: This is the name of the OS, followed by the build ID, for
       * example:
       * "Fedora 25 (Workstation Edition); Build ID: xyz" or
       * "Ubuntu 16.04 LTS; Build ID: jki" */
      result = g_strdup_printf (_("%s; Build ID: %s"), name_version, build_id);
    }
  else
    {
      result = g_strdup (name_version);
    }

  g_clear_pointer (&os_info, g_hash_table_destroy);

  return result;
}

static char *
get_os_type (void)
{
  if (GLIB_SIZEOF_VOID_P == 8)
    /* translators: This is the type of architecture for the OS */
    return g_strdup_printf (_("64-bit"));
  else
    /* translators: This is the type of architecture for the OS */
    return g_strdup_printf (_("32-bit"));
}

static void
query_done (GFile               *file,
            GAsyncResult        *res,
            CcInfoOverviewPanel *self)
{
  CcInfoOverviewPanelPrivate *priv;
  GFileInfo *info;
  GError *error = NULL;

  info = g_file_query_filesystem_info_finish (file, res, &error);
  if (info != NULL)
    {
      priv = cc_info_overview_panel_get_instance_private (self);
      priv->total_bytes += g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      g_object_unref (info);
    }
  else
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_error_free (error);
          return;
        }
      else
        {
          char *path;
          path = g_file_get_path (file);
          g_warning ("Failed to get filesystem free space for '%s': %s", path, error->message);
          g_free (path);
          g_error_free (error);
        }
    }

  /* And onto the next element */
  get_primary_disc_info_start (self);
}

static void
get_primary_disc_info_start (CcInfoOverviewPanel *self)
{
  GUnixMountEntry *mount;
  GFile *file;
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);

  if (priv->primary_mounts == NULL)
    {
      char *size;

      size = g_format_size (priv->total_bytes);
      gtk_label_set_text (GTK_LABEL (priv->disk_label), size);
      g_free (size);

      return;
    }

  mount = priv->primary_mounts->data;
  priv->primary_mounts = g_list_remove (priv->primary_mounts, mount);
  file = g_file_new_for_path (g_unix_mount_get_mount_path (mount));
  g_unix_mount_free (mount);

  g_file_query_filesystem_info_async (file,
                                      G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                      0,
                                      priv->cancellable,
                                      (GAsyncReadyCallback) query_done,
                                      self);
  g_object_unref (file);
}

static void
get_primary_disc_info (CcInfoOverviewPanel *self)
{
  GList *points;
  GList *p;
  GHashTable *hash;
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);

  hash = g_hash_table_new (g_str_hash, g_str_equal);
  points = g_unix_mount_points_get (NULL);

  /* If we do not have /etc/fstab around, try /etc/mtab */
  if (points == NULL)
    points = g_unix_mounts_get (NULL);

  for (p = points; p != NULL; p = p->next)
    {
      GUnixMountEntry *mount = p->data;
      const char *mount_path;
      const char *device_path;

      mount_path = g_unix_mount_get_mount_path (mount);
      device_path = g_unix_mount_get_device_path (mount);

      /* Do not count multiple mounts with same device_path, because it is
       * probably something like btrfs subvolume. Use only the first one in
       * order to count the real size. */
      if (gsd_should_ignore_unix_mount (mount) ||
          gsd_is_removable_mount (mount) ||
          g_str_has_prefix (mount_path, "/media/") ||
          g_str_has_prefix (mount_path, g_get_home_dir ()) ||
          g_hash_table_lookup (hash, device_path) != NULL)
        {
          g_unix_mount_free (mount);
          continue;
        }

      priv->primary_mounts = g_list_prepend (priv->primary_mounts, mount);
      g_hash_table_insert (hash, (gpointer) device_path, (gpointer) device_path);
    }
  g_list_free (points);
  g_hash_table_destroy (hash);

  priv->cancellable = g_cancellable_new ();
  get_primary_disc_info_start (self);
}

static char *
get_cpu_info (const glibtop_sysinfo *info)
{
  GHashTable    *counts;
  GString       *cpu;
  char          *ret;
  GHashTableIter iter;
  gpointer       key, value;
  int            i;
  int            j;

  counts = g_hash_table_new (g_str_hash, g_str_equal);

  /* count duplicates */
  for (i = 0; i != info->ncpu; ++i)
    {
      const char * const keys[] = { "model name", "cpu", "Processor" };
      char *model;
      int  *count;

      model = NULL;

      for (j = 0; model == NULL && j != G_N_ELEMENTS (keys); ++j)
        {
          model = g_hash_table_lookup (info->cpuinfo[i].values,
                                       keys[j]);
        }

      if (model == NULL)
          continue;

      count = g_hash_table_lookup (counts, model);
      if (count == NULL)
        g_hash_table_insert (counts, model, GINT_TO_POINTER (1));
      else
        g_hash_table_replace (counts, model, GINT_TO_POINTER (GPOINTER_TO_INT (count) + 1));
    }

  cpu = g_string_new (NULL);
  g_hash_table_iter_init (&iter, counts);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      char *cleanedup;
      int   count;

      count = GPOINTER_TO_INT (value);
      cleanedup = info_cleanup ((const char *) key);
      if (count > 1)
        g_string_append_printf (cpu, "%s \303\227 %d ", cleanedup, count);
      else
        g_string_append_printf (cpu, "%s ", cleanedup);
      g_free (cleanedup);
    }

  g_hash_table_destroy (counts);

  ret = g_string_free (cpu, FALSE);

  return ret;
}

static void
move_one_up (GtkWidget *grid,
             GtkWidget *child)
{
  int top_attach;

  gtk_container_child_get (GTK_CONTAINER (grid),
                           child,
                           "top-attach", &top_attach,
                           NULL);
  gtk_container_child_set (GTK_CONTAINER (grid),
                           child,
                           "top-attach", top_attach - 1,
                           NULL);
}

static struct {
  const char *id;
  const char *display;
} const virt_tech[] = {
  { "kvm", "KVM" },
  { "qemu", "QEmu" },
  { "vmware", "VMware" },
  { "microsoft", "Microsoft" },
  { "oracle", "Oracle" },
  { "xen", "Xen" },
  { "bochs", "Bochs" },
  { "chroot", "chroot" },
  { "openvz", "OpenVZ" },
  { "lxc", "LXC" },
  { "lxc-libvirt", "LXC (libvirt)" },
  { "systemd-nspawn", "systemd (nspawn)" }
};

static void
set_virtualization_label (CcInfoOverviewPanel *self,
                          const char          *virt)
{
  const char *display_name;
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);
  guint i;

  if (virt == NULL || *virt == '\0')
  {
    gtk_widget_hide (priv->virt_type_label);
    gtk_widget_hide (priv->label18);
    move_one_up (priv->grid1, priv->label8);
    move_one_up (priv->grid1, priv->disk_label);
    return;
  }

  display_name = NULL;
  for (i = 0; i < G_N_ELEMENTS (virt_tech); i++)
    {
      if (g_str_equal (virt_tech[i].id, virt))
        {
          display_name = _(virt_tech[i].display);
          break;
        }
    }

  gtk_label_set_text (GTK_LABEL (priv->virt_type_label), display_name ? display_name : virt);
}

static void
info_overview_panel_setup_virt (CcInfoOverviewPanel *self)
{
  GError *error = NULL;
  GDBusProxy *systemd_proxy;
  GVariant *variant;
  GVariant *inner;
  char *str;

  str = NULL;

  systemd_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 NULL,
                                                 "org.freedesktop.systemd1",
                                                 "/org/freedesktop/systemd1",
                                                 "org.freedesktop.systemd1",
                                                 NULL,
                                                 &error);

  if (systemd_proxy == NULL)
    {
      g_debug ("systemd not available, bailing: %s", error->message);
      g_error_free (error);
      goto bail;
    }

  variant = g_dbus_proxy_call_sync (systemd_proxy,
                                    "org.freedesktop.DBus.Properties.Get",
                                    g_variant_new ("(ss)", "org.freedesktop.systemd1.Manager", "Virtualization"),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    NULL,
                                    &error);
  if (variant == NULL)
    {
      g_debug ("Failed to get property '%s': %s", "Virtualization", error->message);
      g_error_free (error);
      g_object_unref (systemd_proxy);
      goto bail;
    }

  g_variant_get (variant, "(v)", &inner);
  str = g_variant_dup_string (inner, NULL);
  g_variant_unref (variant);

  g_object_unref (systemd_proxy);

bail:
  set_virtualization_label (self, str);
  g_free (str);
}

static void
info_overview_panel_setup_overview (CcInfoOverviewPanel *self)
{
  gboolean    res;
  glibtop_mem mem;
  const glibtop_sysinfo *info;
  char       *text;
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);

  res = load_gnome_version (&priv->gnome_version,
                            &priv->gnome_distributor,
                            &priv->gnome_date);
  if (res)
    {
      text = g_strdup_printf (_("Version %s"), priv->gnome_version);
      gtk_label_set_text (GTK_LABEL (priv->version_label), text);
      g_free (text);
    }

  glibtop_get_mem (&mem);
  text = g_format_size_full (mem.total, G_FORMAT_SIZE_IEC_UNITS);
  gtk_label_set_text (GTK_LABEL (priv->memory_label), text ? text : "");
  g_free (text);

  info = glibtop_get_sysinfo ();

  text = get_cpu_info (info);
  gtk_label_set_markup (GTK_LABEL (priv->processor_label), text ? text : "");
  g_free (text);

  text = get_os_type ();
  gtk_label_set_text (GTK_LABEL (priv->os_type_label), text ? text : "");
  g_free (text);

  text = get_os_name ();
  gtk_label_set_text (GTK_LABEL (priv->os_name_label), text ? text : "");
  g_free (text);

  get_primary_disc_info (self);

  gtk_label_set_markup (GTK_LABEL (priv->graphics_label), priv->graphics_data->hardware_string);
}

static void
cc_info_overview_panel_dispose (GObject *object)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (CC_INFO_OVERVIEW_PANEL (object));

  g_clear_pointer (&priv->graphics_data, graphics_data_free);

  G_OBJECT_CLASS (cc_info_overview_panel_parent_class)->dispose (object);
}

static void
cc_info_overview_panel_finalize (GObject *object)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (CC_INFO_OVERVIEW_PANEL (object));

  if (priv->cancellable)
    {
      g_cancellable_cancel (priv->cancellable);
      g_clear_object (&priv->cancellable);
    }

  if (priv->primary_mounts)
    g_list_free_full (priv->primary_mounts, (GDestroyNotify) g_unix_mount_free);

  g_free (priv->gnome_version);
  g_free (priv->gnome_date);
  g_free (priv->gnome_distributor);

  if (priv->updater_cancellable)
    {
      g_cancellable_cancel (priv->updater_cancellable);
      g_clear_object (&priv->updater_cancellable);
    }

  g_clear_object (&priv->updater_proxy);
  g_clear_object (&priv->session_proxy);

  G_OBJECT_CLASS (cc_info_overview_panel_parent_class)->finalize (object);
}

static void
cc_info_overview_panel_class_init (CcInfoOverviewPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = cc_info_overview_panel_finalize;
  object_class->dispose = cc_info_overview_panel_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/control-center/info/info-overview.ui");

  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, system_image);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, version_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, name_entry);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, memory_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, processor_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, os_name_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, os_type_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, os_updates_box);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, os_updates_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, os_updates_spinner);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, disk_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, graphics_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, virt_type_label);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, label8);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, grid1);
  gtk_widget_class_bind_template_child_private (widget_class, CcInfoOverviewPanel, label18);
  gtk_widget_class_bind_template_callback (widget_class, on_attribution_label_link);
}

static void
cc_info_overview_panel_init (CcInfoOverviewPanel *self)
{
  CcInfoOverviewPanelPrivate *priv = cc_info_overview_panel_get_instance_private (self);

  gtk_widget_init_template (GTK_WIDGET (self));

  priv->graphics_data = get_graphics_data ();

  info_overview_panel_setup_overview (self);
  info_overview_panel_setup_virt (self);
  info_overview_panel_setup_eos_updater (self);
}

GtkWidget *
cc_info_overview_panel_new (void)
{
  return g_object_new (CC_TYPE_INFO_OVERVIEW_PANEL,
                       NULL);
}
