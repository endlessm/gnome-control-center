/* um-app-permissions.c
 *
 * Copyright 2018 Endless, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <libeos-parental-controls/app-filter.h>
#include <flatpak.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <strings.h>

#include "gs-content-rating.h"

#include "um-app-permissions.h"

struct _UmAppPermissions
{
  GtkGrid     parent_instance;

  GMenu      *age_menu;
  GtkSwitch  *allow_system_installation_switch;
  GtkSwitch  *allow_user_installation_switch;
  GtkListBox *listbox;
  GtkButton  *restriction_button;
  GtkPopover *restriction_popover;

  FlatpakInstallation *system_installation; /* (owned) */
  FlatpakInstallation *user_installation; /* (owned) */

  GSimpleActionGroup *action_group; /* (owned) */

  ActUser    *user; /* (owned) */

  GHashTable *blacklisted_apps; /* (owned) */
  GListStore *apps; /* (owned) */

  GCancellable *cancellable; /* (owned) */
  EpcAppFilter *filter; /* (owned) */
  guint         selected_age;

  guint         blacklist_apps_source_id;
};

static gboolean blacklist_apps_cb (gpointer data);

static gint compare_app_info_cb (gconstpointer a,
                                 gconstpointer b,
                                 gpointer      user_data);

static void on_allow_installation_switch_active_changed_cb (GtkSwitch        *s,
                                                            GParamSpec       *pspec,
                                                            UmAppPermissions *self);

static void on_set_age_action_activated (GSimpleAction *action,
                                         GVariant      *param,
                                         gpointer       user_data);

G_DEFINE_TYPE (UmAppPermissions, um_app_permissions, GTK_TYPE_GRID)

enum
{
  PROP_USER = 1,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

static const GActionEntry actions[] = {
  { "set-age", on_set_age_action_activated, "u" }
};

/* FIXME: Factor this out and rely on code from libappstream-glib or gnome-software
 * to do it. See: https://phabricator.endlessm.com/T24986 */
static const gchar * const oars_categories[] =
{
  "violence-cartoon",
  "violence-fantasy",
  "violence-realistic",
  "violence-bloodshed",
  "violence-sexual",
  "violence-desecration",
  "violence-slavery",
  "violence-worship",
  "drugs-alcohol",
  "drugs-narcotics",
  "drugs-tobacco",
  "sex-nudity",
  "sex-themes",
  "sex-homosexuality",
  "sex-prostitution",
  "sex-adultery",
  "sex-appearance",
  "language-profanity",
  "language-humor",
  "language-discrimination",
  "social-chat",
  "social-info",
  "social-audio",
  "social-location",
  "social-contacts",
  "money-purchasing",
  "money-gambling",
  NULL
};

/* Auxiliary methods */

static void
reload_apps (UmAppPermissions *self)
{
  GList *iter, *apps;

  apps = g_app_info_get_all ();

  g_list_store_remove_all (self->apps);

  for (iter = apps; iter; iter = iter->next)
    {
      GAppInfo *app;
      const gchar *app_name;

      app = iter->data;
      app_name = g_app_info_get_name (app);

      if (!G_IS_DESKTOP_APP_INFO (app) ||
          !g_app_info_should_show (app) ||
          app_name[0] == '\0' ||
          /* Endless' link apps have the "eos-link" prefix, and should be ignored too */
          g_str_has_prefix (g_app_info_get_id (app), "eos-link"))
        {
          continue;
        }

      g_debug ("Processing app '%s' (%s)", g_app_info_get_id (app), g_app_info_get_executable (app));

      g_list_store_insert_sorted (self->apps,
                                  app,
                                  compare_app_info_cb,
                                  self);
    }

  g_list_free_full (apps, g_object_unref);
}

static GsContentRatingSystem
get_content_rating_system (ActUser *user)
{
  const gchar *suffixes[] = {
    ".UTF-8",
    ".utf8",
    "@",
    NULL,
  };
  const gchar *user_language;
  gchar *str;
  gsize i;

  user_language = act_user_get_language (user);

  /* Remove the encoding suffixes */
  for (i = 0; suffixes[i] != NULL; i++)
    {
      str = g_strstr_len (user_language, -1, suffixes[i]);
      if (str != NULL)
        *str = '\0';
    }

  return gs_utils_content_rating_system_from_locale (user_language);
}

static void
schedule_update_blacklisted_apps (UmAppPermissions *self)
{
  if (self->blacklist_apps_source_id > 0)
    return;

  /* Use a timeout to batch multiple quick changes into a single
   * update. 1 second is an arbitrary sufficiently small number */
  self->blacklist_apps_source_id = g_timeout_add_seconds (1, blacklist_apps_cb, self);
}

static void
update_app_filter (UmAppPermissions *self)
{
  g_autoptr(GError) error = NULL;

  /* FIXME: make it asynchronous */
  g_clear_pointer (&self->filter, epc_app_filter_unref);
  self->filter = epc_get_app_filter (NULL,
                                     act_user_get_uid (self->user),
                                     FALSE,
                                     self->cancellable,
                                     &error);

  if (error)
    {
      g_warning ("Error retrieving app filter for user '%s': %s",
                  act_user_get_user_name (self->user),
                  error->message);
      return;
    }

  g_debug ("Retrieved new app filter for user '%s'", act_user_get_user_name (self->user));
}

static void
update_categories_from_language (UmAppPermissions *self)
{
  GsContentRatingSystem rating_system;
  const gchar * const * entries;
  const gchar *rating_system_str;
  const guint *ages;
  gsize i;

  rating_system = get_content_rating_system (self->user);
  rating_system_str = gs_content_rating_system_to_str (rating_system);

  g_debug ("Using rating system %s", rating_system_str);

  entries = gs_utils_content_rating_get_values (rating_system);
  ages = gs_utils_content_rating_get_ages (rating_system);

  /* Fill in the age menu */
  g_menu_remove_all (self->age_menu);

  for (i = 0; entries[i] != NULL; i++)
    {
      g_autofree gchar *action = g_strdup_printf ("permissions.set-age(uint32 %u)", ages[i]);

      g_menu_append (self->age_menu, entries[i], action);
    }
}

/* Returns a human-readable but untranslated string, not suitable
 * to be shown in any UI */
static const gchar*
oars_value_to_string (EpcAppFilterOarsValue oars_value)
{
  switch (oars_value)
    {
    case EPC_APP_FILTER_OARS_VALUE_UNKNOWN:
      return "unknown";
    case EPC_APP_FILTER_OARS_VALUE_NONE:
      return "none";
    case EPC_APP_FILTER_OARS_VALUE_MILD:
      return "mild";
    case EPC_APP_FILTER_OARS_VALUE_MODERATE:
      return "moderate";
    case EPC_APP_FILTER_OARS_VALUE_INTENSE:
      return "intense";
    }
  return "";
}

static void
update_oars_level (UmAppPermissions *self)
{
  GsContentRatingSystem rating_system;
  const gchar *rating_age_category;
  guint maximum_age;
  gsize i;

  g_assert (self->filter != NULL);

  maximum_age = 0;

  for (i = 0; oars_categories[i] != NULL; i++)
    {
      EpcAppFilterOarsValue oars_value;
      guint age;

      oars_value = epc_app_filter_get_oars_value (self->filter, oars_categories[i]);
      age = as_content_rating_id_value_to_csm_age (oars_categories[i], oars_value);

      g_debug ("OARS value for '%s': %s", oars_categories[i], oars_value_to_string (oars_value));

      if (age > maximum_age)
        maximum_age = age;
    }

  g_debug ("Effective age for this user: %u", maximum_age);

  rating_system = get_content_rating_system (self->user);
  rating_age_category = gs_utils_content_rating_age_to_str (rating_system, maximum_age);

  gtk_button_set_label (self->restriction_button, rating_age_category);
}

static void
update_allow_app_installation (UmAppPermissions *self)
{
  gboolean allow_system_installation;
  gboolean allow_user_installation;

  allow_system_installation = epc_app_filter_is_system_installation_allowed (self->filter);
  allow_user_installation = epc_app_filter_is_user_installation_allowed (self->filter);

  g_signal_handlers_block_by_func (self->allow_system_installation_switch,
                                   on_allow_installation_switch_active_changed_cb,
                                   self);

  g_signal_handlers_block_by_func (self->allow_user_installation_switch,
                                   on_allow_installation_switch_active_changed_cb,
                                   self);

  gtk_switch_set_active (self->allow_system_installation_switch, allow_system_installation);
  gtk_switch_set_active (self->allow_user_installation_switch, allow_user_installation);

  g_debug ("Allow system installation: %s", allow_system_installation ? "yes" : "no");
  g_debug ("Allow user installation: %s", allow_user_installation ? "yes" : "no");

  g_signal_handlers_unblock_by_func (self->allow_system_installation_switch,
                                     on_allow_installation_switch_active_changed_cb,
                                     self);

  g_signal_handlers_unblock_by_func (self->allow_user_installation_switch,
                                     on_allow_installation_switch_active_changed_cb,
                                     self);
}

static void
setup_parental_control_settings (UmAppPermissions *self)
{
  gtk_widget_set_visible (GTK_WIDGET (self), self->filter != NULL);

  if (!self->filter)
    return;

  g_hash_table_remove_all (self->blacklisted_apps);

  update_oars_level (self);
  update_categories_from_language (self);
  update_allow_app_installation (self);
  reload_apps (self);
}

static gchar*
get_flatpak_ref_for_app_id (UmAppPermissions *self,
                            const gchar      *flatpak_id)
{
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (self->system_installation != NULL);
  g_assert (self->user_installation != NULL);

  ref = flatpak_installation_get_current_installed_app (self->user_installation,
                                                        flatpak_id,
                                                        self->cancellable,
                                                        &error);

  if (error)
    {
      if (!g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
        g_warning ("Error searching for Flatpak ref: %s", error->message);
      return NULL;
    }

  if (!ref || !flatpak_installed_ref_get_is_current (ref))
    {
      ref = flatpak_installation_get_current_installed_app (self->system_installation,
                                                            flatpak_id,
                                                            self->cancellable,
                                                            &error);
      if (error)
        {
          if (!g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
            g_warning ("Error searching for Flatpak ref: %s", error->message);
          return NULL;
        }
    }

  return flatpak_ref_format_ref (FLATPAK_REF (ref));
}

/* Callbacks */

static gboolean
blacklist_apps_cb (gpointer data)
{
  g_auto(EpcAppFilterBuilder) builder = EPC_APP_FILTER_BUILDER_INIT ();
  g_autoptr(EpcAppFilter) new_filter = NULL;
  g_autoptr(GError) error = NULL;
  UmAppPermissions *self = data;
  GDesktopAppInfo *app;
  GHashTableIter iter;
  gboolean allow_system_installation;
  gboolean allow_user_installation;
  gsize i;

  self->blacklist_apps_source_id = 0;

  g_debug ("Building parental controls settings…");

  /* Blacklist */

  g_debug ("\t → Blacklisting apps");

  g_hash_table_iter_init (&iter, self->blacklisted_apps);
  while (g_hash_table_iter_next (&iter, (gpointer) &app, NULL))
    {
      g_autofree gchar *flatpak_id = NULL;

      flatpak_id = g_desktop_app_info_get_string (app, "X-Flatpak");

      if (flatpak_id)
        {
          g_autofree gchar *flatpak_ref = get_flatpak_ref_for_app_id (self, flatpak_id);

          g_debug ("\t\t → Blacklisting Flatpak ref: %s", flatpak_ref);
          epc_app_filter_builder_blacklist_flatpak_ref (&builder, flatpak_ref);
        }
      else
        {
          const gchar *executable = g_app_info_get_executable (G_APP_INFO (app));
          g_autofree gchar *path = g_find_program_in_path (executable);

          g_debug ("\t\t → Blacklisting path: %s", path);
          epc_app_filter_builder_blacklist_path (&builder, path);
        }
    }

  /* Maturity level */

  g_debug ("\t → Maturity level");

  for (i = 0; oars_categories[i] != NULL; i++)
    {
      EpcAppFilterOarsValue oars_value;
      const gchar *oars_category;

      oars_category = oars_categories[i];
      oars_value = as_content_rating_id_csm_age_to_value (oars_category, self->selected_age);

      g_debug ("\t\t → %s: %s", oars_category, oars_value_to_string (oars_value));

      epc_app_filter_builder_set_oars_value (&builder, oars_category, oars_value);
    }

  /* App Installation */
  allow_system_installation = gtk_switch_get_active (self->allow_system_installation_switch);
  allow_user_installation = gtk_switch_get_active (self->allow_user_installation_switch);

  g_debug ("\t → %s system installation", allow_system_installation ? "Enabling" : "Disabling");
  g_debug ("\t → %s user installation", allow_user_installation ? "Enabling" : "Disabling");

  epc_app_filter_builder_set_allow_user_installation (&builder, allow_user_installation);
  epc_app_filter_builder_set_allow_system_installation (&builder, allow_system_installation);

  new_filter = epc_app_filter_builder_end (&builder);

  /* FIXME: should become asynchronous */
  epc_set_app_filter (NULL,
                      act_user_get_uid (self->user),
                      new_filter,
                      TRUE,
                      self->cancellable,
                      &error);

  if (error)
    {
      g_warning ("Error updating app filter: %s", error->message);
      setup_parental_control_settings (self);
    }

  return G_SOURCE_REMOVE;
}

static void
on_allow_installation_switch_active_changed_cb (GtkSwitch        *s,
                                                GParamSpec       *pspec,
                                                UmAppPermissions *self)
{
  schedule_update_blacklisted_apps (self);
}

static void
on_switch_active_changed_cb (GtkSwitch        *s,
                             GParamSpec       *pspec,
                             UmAppPermissions *self)
{
  GAppInfo *app;
  gboolean allowed;

  app = g_object_get_data (G_OBJECT (s), "GAppInfo");
  allowed = gtk_switch_get_active (s);

  if (allowed)
    {
      gboolean removed;

      g_debug ("Removing '%s' from blacklisted apps", g_app_info_get_id (app));

      removed = g_hash_table_remove (self->blacklisted_apps, app);
      g_assert (removed);
    }
  else
    {
      gboolean added;

      g_debug ("Blacklisting '%s'", g_app_info_get_id (app));

      added = g_hash_table_add (self->blacklisted_apps, g_object_ref (app));
      g_assert (added);
    }

  schedule_update_blacklisted_apps (self);
}

static GtkWidget *
create_row_for_app_cb (gpointer item,
                       gpointer user_data)
{
  g_autoptr(GIcon) icon = NULL;
  UmAppPermissions *self;
  GtkWidget *box, *w;
  GAppInfo *app;
  gboolean allowed;
  const gchar *app_name;
  gint size;

  self = UM_APP_PERMISSIONS (user_data);
  app = item;
  app_name = g_app_info_get_name (app);

  g_assert (G_IS_DESKTOP_APP_INFO (app));

  icon = g_app_info_get_icon (app);
  if (icon == NULL)
    icon = g_themed_icon_new ("application-x-executable");
  else
    g_object_ref (icon);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  gtk_widget_set_margin_end (box, 12);

  /* Icon */
  w = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DIALOG);
  gtk_icon_size_lookup (GTK_ICON_SIZE_DND, &size, NULL);
  gtk_image_set_pixel_size (GTK_IMAGE (w), size);
  gtk_container_add (GTK_CONTAINER (box), w);

  /* App name label */
  w = g_object_new (GTK_TYPE_LABEL,
                    "label", app_name,
                    "hexpand", TRUE,
                    "xalign", 0.0,
                    NULL);
  gtk_container_add (GTK_CONTAINER (box), w);

  /* Switch */
  w = gtk_switch_new ();
  gtk_container_add (GTK_CONTAINER (box), w);

  gtk_widget_show_all (box);

  /* Fetch status from AccountService */
  allowed = epc_app_filter_is_appinfo_allowed (self->filter, app);

  gtk_switch_set_active (GTK_SWITCH (w), allowed);
  g_object_set_data_full (G_OBJECT (w), "GAppInfo", g_object_ref (app), g_object_unref);

  if (allowed)
    g_hash_table_remove (self->blacklisted_apps, app);
  else if (!allowed)
    g_hash_table_add (self->blacklisted_apps, g_object_ref (app));

  g_signal_connect (w, "notify::active", G_CALLBACK (on_switch_active_changed_cb), self);

  return box;
}

static gint
compare_app_info_cb (gconstpointer a,
                     gconstpointer b,
                     gpointer      user_data)
{
  GAppInfo *app_a = (GAppInfo*) a;
  GAppInfo *app_b = (GAppInfo*) b;

  return g_utf8_collate (g_app_info_get_display_name (app_a),
                         g_app_info_get_display_name (app_b));
}

static void
on_set_age_action_activated (GSimpleAction *action,
                             GVariant      *param,
                             gpointer       user_data)
{
  GsContentRatingSystem rating_system;
  UmAppPermissions *self;
  const gchar * const * entries;
  const guint *ages;
  guint age;
  guint i;

  self = UM_APP_PERMISSIONS (user_data);
  age = g_variant_get_uint32 (param);

  rating_system = get_content_rating_system (self->user);
  entries = gs_utils_content_rating_get_values (rating_system);
  ages = gs_utils_content_rating_get_ages (rating_system);

  /* Update the button */
  for (i = 0; entries[i] != NULL; i++)
    {
      if (ages[i] == age)
        {
          gtk_button_set_label (self->restriction_button, entries[i]);
          break;
        }
    }

  g_assert (entries[i] != NULL);

  g_debug ("Selected age: %u", age);

  self->selected_age = age;

  schedule_update_blacklisted_apps (self);
}

/* GObject overrides */

static void
um_app_permissions_finalize (GObject *object)
{
  UmAppPermissions *self = (UmAppPermissions *)object;

  g_assert (self->blacklist_apps_source_id == 0);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->action_group);
  g_clear_object (&self->apps);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->system_installation);
  g_clear_object (&self->user);
  g_clear_object (&self->user_installation);

  g_clear_pointer (&self->blacklisted_apps, g_hash_table_unref);
  g_clear_pointer (&self->filter, epc_app_filter_unref);

  G_OBJECT_CLASS (um_app_permissions_parent_class)->finalize (object);
}


static void
um_app_permissions_dispose (GObject *object)
{
  UmAppPermissions *self = (UmAppPermissions *)object;

  if (self->blacklist_apps_source_id > 0)
    {
      blacklist_apps_cb (self);
      g_source_remove (self->blacklist_apps_source_id);
      self->blacklist_apps_source_id = 0;
    }

  G_OBJECT_CLASS (um_app_permissions_parent_class)->dispose (object);
}

static void
um_app_permissions_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  UmAppPermissions *self = UM_APP_PERMISSIONS (object);

  switch (prop_id)
    {
    case PROP_USER:
      g_value_set_object (value, self->user);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
um_app_permissions_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  UmAppPermissions *self = UM_APP_PERMISSIONS (object);

  switch (prop_id)
    {
    case PROP_USER:
      um_app_permissions_set_user (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
um_app_permissions_class_init (UmAppPermissionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = um_app_permissions_finalize;
  object_class->dispose = um_app_permissions_dispose;
  object_class->get_property = um_app_permissions_get_property;
  object_class->set_property = um_app_permissions_set_property;

  properties[PROP_USER] = g_param_spec_object ("user",
                                               "User",
                                               "User",
                                               ACT_TYPE_USER,
                                               G_PARAM_READWRITE |
                                               G_PARAM_STATIC_STRINGS |
                                               G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/control-center/user-accounts/app-permissions.ui");

  gtk_widget_class_bind_template_child (widget_class, UmAppPermissions, age_menu);
  gtk_widget_class_bind_template_child (widget_class, UmAppPermissions, allow_system_installation_switch);
  gtk_widget_class_bind_template_child (widget_class, UmAppPermissions, allow_user_installation_switch);
  gtk_widget_class_bind_template_child (widget_class, UmAppPermissions, restriction_button);
  gtk_widget_class_bind_template_child (widget_class, UmAppPermissions, restriction_popover);
  gtk_widget_class_bind_template_child (widget_class, UmAppPermissions, listbox);

  gtk_widget_class_bind_template_callback (widget_class, on_allow_installation_switch_active_changed_cb);
}

static void
um_app_permissions_init (UmAppPermissions *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->system_installation = flatpak_installation_new_system (NULL, NULL);
  self->user_installation = flatpak_installation_new_user (NULL, NULL);

  self->action_group = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->action_group),
                                   actions,
                                   G_N_ELEMENTS (actions),
                                   self);

  gtk_widget_insert_action_group (GTK_WIDGET (self),
                                  "permissions",
                                  G_ACTION_GROUP (self->action_group));

  gtk_popover_bind_model (self->restriction_popover, G_MENU_MODEL (self->age_menu), NULL);
  self->blacklisted_apps = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);

  self->cancellable = g_cancellable_new ();
  self->apps = g_list_store_new (G_TYPE_APP_INFO);

  gtk_list_box_bind_model (self->listbox,
                           G_LIST_MODEL (self->apps),
                           create_row_for_app_cb,
                           self,
                           NULL);
}

ActUser*
um_app_permissions_get_user (UmAppPermissions *self)
{
  g_return_val_if_fail (UM_IS_APP_PERMISSIONS (self), NULL);

  return self->user;
}

void
um_app_permissions_set_user (UmAppPermissions *self,
                             ActUser          *user)
{
  g_return_if_fail (UM_IS_APP_PERMISSIONS (self));
  g_return_if_fail (ACT_IS_USER (user));

  if (g_set_object (&self->user, user))
    {
      update_app_filter (self);
      setup_parental_control_settings (self);

      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_USER]);
    }
}
