/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright 2009-2010  Red Hat, Inc,
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
 * Written by: Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "um-user-panel.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <polkit/polkit.h>
#include <act/act.h>
#include <libgd/gd-notification.h>
#include <cairo-gobject.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-languages.h>

#include "um-user-image.h"
#include "um-cell-renderer-user-image.h"

#include "um-account-dialog.h"
#include "cc-language-chooser.h"
#include "um-password-dialog.h"
#include "um-photo-dialog.h"
#include "um-fingerprint-dialog.h"
#include "um-utils.h"
#include "um-resources.h"
#include "um-history-dialog.h"

#include "cc-common-language.h"
#include "cc-util.h"

#include "um-realm-manager.h"

#define USER_ACCOUNTS_PERMISSION "org.gnome.controlcenter.user-accounts.administration"

CC_PANEL_REGISTER (CcUserPanel, cc_user_panel)

#define UM_USER_PANEL_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), UM_TYPE_USER_PANEL, CcUserPanelPrivate))

struct _CcUserPanelPrivate {
        ActUserManager *um;
        GCancellable  *cancellable;
        GtkBuilder *builder;
        GtkWidget *notification;
        GSettings *login_screen_settings;

        GtkWidget *headerbar_buttons;
        GtkWidget *main_box;
        GPermission *permission;
        GtkWidget *language_chooser;

        UmPasswordDialog *password_dialog;
        UmPhotoDialog *photo_dialog;
        UmHistoryDialog *history_dialog;

        gint other_accounts;
        GtkTreeIter *other_iter;

        UmAccountDialog *account_dialog;
};

static GtkWidget *
get_widget (CcUserPanelPrivate *d, const char *name)
{
        return (GtkWidget *)gtk_builder_get_object (d->builder, name);
}

#define PAGE_LOCK "_lock"
#define PAGE_ADDUSER "_adduser"

enum {
        USER_COL,
        NAME_COL,
        USER_ROW_COL,
        TITLE_COL,
        HEADING_ROW_COL,
        SORT_KEY_COL,
        NUM_USER_LIST_COLS
};

static void show_restart_notification (CcUserPanelPrivate *d, const gchar *locale);

typedef struct {
        CcUserPanel *self;
        GCancellable *cancellable;
        gchar *login;
} AsyncDeleteData;

static void
async_delete_data_free (AsyncDeleteData *data)
{
        g_object_unref (data->self);
        g_object_unref (data->cancellable);
        g_free (data->login);
        g_slice_free (AsyncDeleteData, data);
}

static void
show_error_dialog (CcUserPanelPrivate *d,
                   const gchar *message,
                   GError *error)
{
        GtkWidget *dialog;

        dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR,
                                         GTK_MESSAGE_ERROR,
                                         GTK_BUTTONS_CLOSE,
                                         "%s", message);

        if (error != NULL) {
                g_dbus_error_strip_remote_error (error);
                gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                          "%s", error->message);
        }

        g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);
        gtk_window_present (GTK_WINDOW (dialog));
}

static ActUser *
get_selected_user (CcUserPanelPrivate *d)
{
        GtkTreeView *tv;
        GtkTreeIter iter;
        GtkTreeSelection *selection;
        GtkTreeModel *model;
        ActUser *user;

        tv = (GtkTreeView *)get_widget (d, "list-treeview");
        selection = gtk_tree_view_get_selection (tv);

        if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
                gtk_tree_model_get (model, &iter, USER_COL, &user, -1);
                return user;
        }

        return NULL;
}

static const gchar *
get_real_or_user_name (ActUser *user)
{
  const gchar *name;

  name = act_user_get_real_name (user);
  if (name == NULL)
    name = act_user_get_user_name (user);

  return name;
}

static char *
get_name_col_str (ActUser *user)
{
        return g_markup_printf_escaped ("<b>%s</b>\n<small>%s</small>",
                                        get_real_or_user_name (user),
                                        act_user_get_user_name (user));
}

static void show_user (ActUser *user, CcUserPanelPrivate *d);

static void
user_added (ActUserManager *um, ActUser *user, CcUserPanelPrivate *d)
{
        GtkWidget *widget;
        GtkTreeModel *model;
        GtkListStore *store;
        GtkTreeIter iter;
        GtkTreeIter dummy;
        gchar *text, *title;
        GtkTreeSelection *selection;
        gint sort_key;

        if (act_user_is_system_account (user)) {
                return;
        }

        g_debug ("user added: %d %s\n", act_user_get_uid (user), get_real_or_user_name (user));
        widget = get_widget (d, "list-treeview");
        model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));
        store = GTK_LIST_STORE (model);
        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));

        text = get_name_col_str (user);

        if (act_user_get_uid (user) == getuid ()) {
                sort_key = 1;
        }
        else {
                d->other_accounts++;
                sort_key = 3;
        }
        gtk_list_store_append (store, &iter);

        gtk_list_store_set (store, &iter,
                            USER_COL, user,
                            NAME_COL, text,
                            USER_ROW_COL, TRUE,
                            TITLE_COL, NULL,
                            HEADING_ROW_COL, FALSE,
                            SORT_KEY_COL, sort_key,
                            -1);
        g_free (text);

        if (sort_key == 1 &&
            !gtk_tree_selection_get_selected (selection, &model, &dummy)) {
                gtk_tree_selection_select_iter (selection, &iter);
        }

        /* Show heading for other accounts if new one have been added. */
        if (d->other_accounts == 1 && sort_key == 3) {
                title = g_strdup_printf ("<small><span foreground=\"#555555\">%s</span></small>", _("Other Accounts"));
                gtk_list_store_append (store, &iter);
                gtk_list_store_set (store, &iter,
                                    TITLE_COL, title,
                                    HEADING_ROW_COL, TRUE,
                                    SORT_KEY_COL, 2,
                                    -1);
                d->other_iter = gtk_tree_iter_copy (&iter);
                g_free (title);
        }
}

static void
get_previous_user_row (GtkTreeModel *model,
                       GtkTreeIter  *iter,
                       GtkTreeIter  *prev)
{
        GtkTreePath *path;
        ActUser *user;

        path = gtk_tree_model_get_path (model, iter);
        while (gtk_tree_path_prev (path)) {
                gtk_tree_model_get_iter (model, prev, path);
                gtk_tree_model_get (model, prev, USER_COL, &user, -1);
                if (user) {
                        g_object_unref (user);
                        break;
                }
        }
        gtk_tree_path_free (path);
}

static gboolean
get_next_user_row (GtkTreeModel *model,
                   GtkTreeIter  *iter,
                   GtkTreeIter  *next)
{
        ActUser *user;

        *next = *iter;
        while (gtk_tree_model_iter_next (model, next)) {
                gtk_tree_model_get (model, next, USER_COL, &user, -1);
                if (user) {
                        g_object_unref (user);
                        return TRUE;
                }
        }

        return FALSE;
}

static void
user_removed (ActUserManager *um, ActUser *user, CcUserPanelPrivate *d)
{
        GtkTreeView *tv;
        GtkTreeModel *model;
        GtkTreeSelection *selection;
        GtkListStore *store;
        GtkTreeIter iter, next;
        ActUser *u;
        gint key;

        g_debug ("user removed: %s\n", act_user_get_user_name (user));
        tv = (GtkTreeView *)get_widget (d, "list-treeview");
        selection = gtk_tree_view_get_selection (tv);
        model = gtk_tree_view_get_model (tv);
        store = GTK_LIST_STORE (model);
        if (gtk_tree_model_get_iter_first (model, &iter)) {
                do {
                        gtk_tree_model_get (model, &iter, USER_COL, &u, SORT_KEY_COL, &key, -1);

                        if (u != NULL) {
                                if (act_user_get_uid (user) == act_user_get_uid (u)) {
                                        if (!get_next_user_row (model, &iter, &next))
                                                get_previous_user_row (model, &iter, &next);
                                        if (key == 3) {
                                                d->other_accounts--;
                                        }
                                        gtk_list_store_remove (store, &iter);
                                        gtk_tree_selection_select_iter (selection, &next);
                                        g_object_unref (u);
                                        break;
                                }
                                g_object_unref (u);
                        }
                } while (gtk_tree_model_iter_next (model, &iter));
        }

        /* Hide heading for other accounts if last one have been removed. */
        if (d->other_iter != NULL && d->other_accounts == 0 && key == 3) {
                gtk_list_store_remove (store, d->other_iter);
                gtk_tree_iter_free (d->other_iter);
                d->other_iter = NULL;
        }
}

static void
user_changed (ActUserManager *um, ActUser *user, CcUserPanelPrivate *d)
{
        GtkTreeView *tv;
        GtkTreeSelection *selection;
        GtkTreeModel *model;
        GtkTreeIter iter;
        ActUser *current;
        char *text;

        tv = (GtkTreeView *)get_widget (d, "list-treeview");
        model = gtk_tree_view_get_model (tv);
        selection = gtk_tree_view_get_selection (tv);

        g_assert (gtk_tree_model_get_iter_first (model, &iter));
        do {
                gtk_tree_model_get (model, &iter, USER_COL, &current, -1);
                if (current == user) {
                        text = get_name_col_str (user);

                        gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                                            USER_COL, user,
                                            NAME_COL, text,
                                            -1);
                        g_free (text);
                        g_object_unref (current);

                        break;
                }
                if (current)
                        g_object_unref (current);

        } while (gtk_tree_model_iter_next (model, &iter));

        if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
                gtk_tree_model_get (model, &iter, USER_COL, &current, -1);

                if (current == user) {
                        show_user (user, d);
                }
                if (current)
                        g_object_unref (current);
        }
}

static void
select_created_user (GObject *object,
                     GAsyncResult *result,
                     gpointer user_data)
{
        CcUserPanelPrivate *d = user_data;
        UmAccountDialog *dialog;
        GtkTreeView *tv;
        GtkTreeModel *model;
        GtkTreeSelection *selection;
        GtkTreeIter iter;
        ActUser *current;
        GtkTreePath *path;
        ActUser *user;
        uid_t user_uid;

        dialog = UM_ACCOUNT_DIALOG (object);
        user = um_account_dialog_finish (dialog, result);
        gtk_widget_destroy (GTK_WIDGET (dialog));
        d->account_dialog = NULL;

        if (user == NULL)
                return;

        tv = (GtkTreeView *)get_widget (d, "list-treeview");
        model = gtk_tree_view_get_model (tv);
        selection = gtk_tree_view_get_selection (tv);
        user_uid = act_user_get_uid (user);

        g_assert (gtk_tree_model_get_iter_first (model, &iter));
        do {
                gtk_tree_model_get (model, &iter, USER_COL, &current, -1);
                if (current) {
                        if (user_uid == act_user_get_uid (current)) {
                                path = gtk_tree_model_get_path (model, &iter);
                                gtk_tree_view_scroll_to_cell (tv, path, NULL, FALSE, 0.0, 0.0);
                                gtk_tree_selection_select_path (selection, path);
                                gtk_tree_path_free (path);
                                g_object_unref (current);
                                break;
                        }
                        g_object_unref (current);
                }
        } while (gtk_tree_model_iter_next (model, &iter));

        g_object_unref (user);
}

static void
add_user (GtkButton *button, CcUserPanelPrivate *d)
{
        d->account_dialog = um_account_dialog_new ();
        um_account_dialog_show (d->account_dialog, GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                d->permission, select_created_user, d);
}

static void
delete_user_done (ActUserManager    *manager,
                  GAsyncResult      *res,
                  CcUserPanelPrivate *d)
{
        GError *error;

        error = NULL;
        if (!act_user_manager_delete_user_finish (manager, res, &error)) {
                if (!g_error_matches (error, ACT_USER_MANAGER_ERROR,
                                      ACT_USER_MANAGER_ERROR_PERMISSION_DENIED))
                        show_error_dialog (d, _("Failed to delete user"), error);

                g_error_free (error);
        }
}

static void
delete_user_response (GtkWidget         *dialog,
                      gint               response_id,
                      CcUserPanelPrivate *d)
{
        ActUser *user;
        gboolean remove_files;

        gtk_widget_destroy (dialog);

        if (response_id == GTK_RESPONSE_CANCEL) {
                return;
        }
        else if (response_id == GTK_RESPONSE_NO) {
                remove_files = TRUE;
        }
        else {
                remove_files = FALSE;
        }

        user = get_selected_user (d);

        /* remove autologin */
        if (act_user_get_automatic_login (user)) {
                act_user_set_automatic_login (user, FALSE);
        }

        act_user_manager_delete_user_async (d->um,
                                            user,
                                            remove_files,
                                            NULL,
                                            (GAsyncReadyCallback)delete_user_done,
                                            d);

        g_object_unref (user);
}

static void
enterprise_user_revoked (GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
        AsyncDeleteData *data = user_data;
        CcUserPanelPrivate *d = data->self->priv;
        UmRealmCommon *common = UM_REALM_COMMON (source);
        GError *error = NULL;

        if (g_cancellable_is_cancelled (data->cancellable)) {
                async_delete_data_free (data);
                return;
        }

        um_realm_common_call_change_login_policy_finish (common, result, &error);
        if (error != NULL) {
                show_error_dialog (d, _("Failed to revoke remotely managed user"), error);
                g_error_free (error);
        }

        async_delete_data_free (data);
}

static UmRealmCommon *
find_matching_realm (UmRealmManager *realm_manager, const gchar *login)
{
        UmRealmCommon *common = NULL;
        GList *realms, *l;

        realms = um_realm_manager_get_realms (realm_manager);
        for (l = realms; l != NULL; l = g_list_next (l)) {
                const gchar * const *permitted_logins;
                gint i;

                common = um_realm_object_get_common (l->data);
                if (common == NULL)
                        continue;

                permitted_logins = um_realm_common_get_permitted_logins (common);
                for (i = 0; permitted_logins[i] != NULL; i++) {
                        if (g_strcmp0 (permitted_logins[i], login) == 0)
                                break;
                }

                if (permitted_logins[i] != NULL)
                        break;

                g_clear_object (&common);
        }
        g_list_free_full (realms, g_object_unref);

        return common;
}

static void
realm_manager_found (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
        AsyncDeleteData *data = user_data;
        CcUserPanelPrivate *d = data->self->priv;
        UmRealmCommon *common;
        UmRealmManager *realm_manager;
        const gchar *add[1];
        const gchar *remove[2];
        GVariant *options;
        GError *error = NULL;

        if (g_cancellable_is_cancelled (data->cancellable)) {
                async_delete_data_free (data);
                return;
        }

        realm_manager = um_realm_manager_new_finish (result, &error);
        if (error != NULL) {
                show_error_dialog (d, _("Failed to revoke remotely managed user"), error);
                g_error_free (error);
                async_delete_data_free (data);
                return;
        }

        /* Find matching realm */
        common = find_matching_realm (realm_manager, data->login);
        if (common == NULL) {
                /* The realm was probably left */
                async_delete_data_free (data);
                return;
        }

        /* Remove the user from permitted logins */
        g_debug ("Denying future login for: %s", data->login);

        add[0] = NULL;
        remove[0] = data->login;
        remove[1] = NULL;

        options = g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);
        um_realm_common_call_change_login_policy (common, "",
                                                  add, remove, options,
                                                  data->cancellable,
                                                  enterprise_user_revoked,
                                                  data);

        g_object_unref (common);
}

static void
enterprise_user_uncached (GObject           *source,
                          GAsyncResult      *res,
                          gpointer           user_data)
{
        AsyncDeleteData *data = user_data;
        CcUserPanelPrivate *d = data->self->priv;
        ActUserManager *manager = ACT_USER_MANAGER (source);
        GError *error = NULL;

        if (g_cancellable_is_cancelled (data->cancellable)) {
                async_delete_data_free (data);
                return;
        }

        act_user_manager_uncache_user_finish (manager, res, &error);
        if (error == NULL) {
                /* Find realm manager */
                um_realm_manager_new (d->cancellable, realm_manager_found, data);
        }
        else {
                show_error_dialog (d, _("Failed to revoke remotely managed user"), error);
                g_error_free (error);
                async_delete_data_free (data);
        }
}

static void
delete_enterprise_user_response (GtkWidget          *dialog,
                                 gint                response_id,
                                 gpointer            user_data)
{
        CcUserPanel *self = UM_USER_PANEL (user_data);
        CcUserPanelPrivate *d = self->priv;
        AsyncDeleteData *data;
        ActUser *user;

        gtk_widget_destroy (dialog);

        if (response_id != GTK_RESPONSE_ACCEPT) {
                return;
        }

        user = get_selected_user (self->priv);

        data = g_slice_new (AsyncDeleteData);
        data->self = g_object_ref (self);
        data->cancellable = g_object_ref (d->cancellable);
        data->login = g_strdup (act_user_get_user_name (user));

        g_object_unref (user);

        /* Uncache the user account from the accountsservice */
        g_debug ("Uncaching remote user: %s", data->login);

        act_user_manager_uncache_user_async (d->um, data->login,
                                             data->cancellable,
                                             enterprise_user_uncached,
                                             data);
}

static void
delete_user (GtkButton *button, CcUserPanel *self)
{
        CcUserPanelPrivate *d = self->priv;
        ActUser *user;
        GtkWidget *dialog;

        user = get_selected_user (d);
        if (user == NULL) {
                return;
        }
        else if (act_user_get_uid (user) == getuid ()) {
                dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                                 0,
                                                 GTK_MESSAGE_INFO,
                                                 GTK_BUTTONS_CLOSE,
                                                 _("You cannot delete your own account."));
                g_signal_connect (dialog, "response",
                                  G_CALLBACK (gtk_widget_destroy), NULL);
        }
        else if (act_user_is_logged_in_anywhere (user)) {
                dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                                 0,
                                                 GTK_MESSAGE_INFO,
                                                 GTK_BUTTONS_CLOSE,
                                                 _("%s is still logged in"),
                                                get_real_or_user_name (user));

                gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                          _("Deleting a user while they are logged in can leave the system in an inconsistent state."));
                g_signal_connect (dialog, "response",
                                  G_CALLBACK (gtk_widget_destroy), NULL);
        }
        else if (act_user_is_local_account (user)) {
                dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                                 0,
                                                 GTK_MESSAGE_QUESTION,
                                                 GTK_BUTTONS_NONE,
                                                 _("Do you want to keep %s's files?"),
                                                get_real_or_user_name (user));

                gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                          _("It is possible to keep the home directory, mail spool and temporary files around when deleting a user account."));

                gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                                        _("_Delete Files"), GTK_RESPONSE_NO,
                                        _("_Keep Files"), GTK_RESPONSE_YES,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        NULL);

                gtk_window_set_icon_name (GTK_WINDOW (dialog), "system-users");

                g_signal_connect (dialog, "response",
                                  G_CALLBACK (delete_user_response), d);
        }
        else {
                dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                                 0,
                                                 GTK_MESSAGE_QUESTION,
                                                 GTK_BUTTONS_NONE,
                                                 _("Are you sure you want to revoke remotely managed %s's account?"),
                                                 get_real_or_user_name (user));

                gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                                        _("_Delete"), GTK_RESPONSE_ACCEPT,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        NULL);

                gtk_window_set_icon_name (GTK_WINDOW (dialog), "system-users");

                g_signal_connect (dialog, "response",
                                  G_CALLBACK (delete_enterprise_user_response), self);
        }

        g_signal_connect (dialog, "close",
                          G_CALLBACK (gtk_widget_destroy), NULL);

        gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

        gtk_window_present (GTK_WINDOW (dialog));

        g_object_unref (user);
}

static const gchar *
get_invisible_text (void)
{
   GtkWidget *entry;
   gunichar invisible_char;
   static gchar invisible_text[40];
   gchar *p;
   gint i;

   entry = gtk_entry_new ();
   invisible_char = gtk_entry_get_invisible_char (GTK_ENTRY (entry));
   if (invisible_char == 0)
     invisible_char = 0x2022;

   g_object_ref_sink (entry);
   g_object_unref (entry);

   /* five bullets */
   p = invisible_text;
   for (i = 0; i < 5; i++)
     p += g_unichar_to_utf8 (invisible_char, p);
   *p = 0;

   return invisible_text;
}

static const gchar *
get_password_mode_text (ActUser *user)
{
        const gchar *text;

        if (act_user_get_locked (user)) {
                text = C_("Password mode", "Account disabled");
        }
        else {
                switch (act_user_get_password_mode (user)) {
                case ACT_USER_PASSWORD_MODE_REGULAR:
                        text = get_invisible_text ();
                        break;
                case ACT_USER_PASSWORD_MODE_SET_AT_LOGIN:
                        text = C_("Password mode", "To be set at next login");
                        break;
                case ACT_USER_PASSWORD_MODE_NONE:
                        text = C_("Password mode", "None");
                        break;
                default:
                        g_assert_not_reached ();
                }
        }

        return text;
}

static void
autologin_changed (GObject            *object,
                   GParamSpec         *pspec,
                   CcUserPanelPrivate *d)
{
        gboolean active;
        ActUser *user;

        active = gtk_switch_get_active (GTK_SWITCH (object));
        user = get_selected_user (d);

        if (active != act_user_get_automatic_login (user)) {
                act_user_set_automatic_login (user, active);
                if (act_user_get_automatic_login (user)) {
                        GSList *list;
                        GSList *l;
                        list = act_user_manager_list_users (d->um);
                        for (l = list; l != NULL; l = l->next) {
                                ActUser *u = l->data;
                                if (act_user_get_uid (u) != act_user_get_uid (user)) {
                                        act_user_set_automatic_login (user, FALSE);
                                }
                        }
                        g_slist_free (list);
                }
        }

        g_object_unref (user);
}

static gchar *
get_login_time_text (ActUser *user)
{
        gchar *text, *date_str, *time_str;
        GDateTime *date_time;
        gint64 time;

        time = act_user_get_login_time (user);
        if (act_user_is_logged_in (user)) {
                text = g_strdup (_("Logged in"));
        }
        else if (time > 0) {
                date_time = g_date_time_new_from_unix_local (time);
                date_str = cc_util_get_smart_date (date_time);
                /* Translators: This is a time format string in the style of "22:58".
                   It indicates a login time which follows a date. */
                time_str = g_date_time_format (date_time, C_("login date-time", "%k:%M"));

                /* Translators: This indicates a login date-time.
                   The first %s is a date, and the second %s a time. */
                text = g_strdup_printf(C_("login date-time", "%s, %s"), date_str, time_str);

                g_date_time_unref (date_time);
                g_free (date_str);
                g_free (time_str);
        }
        else {
                text = g_strdup ("—");
        }

        return text;
}

static gboolean
get_autologin_possible (ActUser *user)
{
        gboolean locked;
        gboolean set_password_at_login;

        locked = act_user_get_locked (user);
        set_password_at_login = (act_user_get_password_mode (user) == ACT_USER_PASSWORD_MODE_SET_AT_LOGIN);

        return !(locked || set_password_at_login);
}

static void on_permission_changed (GPermission *permission, GParamSpec *pspec, gpointer data);

static void
show_user (ActUser *user, CcUserPanelPrivate *d)
{
        GtkWidget *image;
        GtkWidget *label;
        gchar *lang, *text, *name;
        GtkWidget *widget;
        gboolean show, enable;
        ActUser *current;

        image = get_widget (d, "user-icon-image");
        um_user_image_set_user (UM_USER_IMAGE (image), user);
        image = get_widget (d, "user-icon-image2");
        um_user_image_set_user (UM_USER_IMAGE (image), user);

        um_photo_dialog_set_user (d->photo_dialog, user);

        widget = get_widget (d, "full-name-entry");
        gtk_entry_set_text (GTK_ENTRY (widget), act_user_get_real_name (user));
        gtk_widget_set_tooltip_text (widget, act_user_get_user_name (user));

        widget = get_widget (d, act_user_get_account_type (user) ? "account-type-admin" : "account-type-standard");
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);

        /* Do not show the "Account Type" option when there's a single user account. */
        show = (d->other_accounts != 0);
        gtk_widget_set_visible (get_widget (d, "account-type-label"), show);
        gtk_widget_set_visible (get_widget (d, "account-type-box"), show);

        widget = get_widget (d, "account-password-button-label");
        gtk_label_set_label (GTK_LABEL (widget), get_password_mode_text (user));
        enable = act_user_is_local_account (user);
        gtk_widget_set_sensitive (widget, enable);

        widget = get_widget (d, "autologin-switch");
        g_signal_handlers_block_by_func (widget, autologin_changed, d);
        gtk_switch_set_active (GTK_SWITCH (widget), act_user_get_automatic_login (user));
        g_signal_handlers_unblock_by_func (widget, autologin_changed, d);
        gtk_widget_set_sensitive (widget, get_autologin_possible (user));

        widget = get_widget (d, "account-language-button-label");

        name = NULL;
        lang = g_strdup (act_user_get_language (user));
        if ((!lang || *lang == '\0') && act_user_get_uid (user) == getuid ()) {
                lang = cc_common_language_get_current_language ();
                act_user_set_language (user, lang);
        }

        if (lang && *lang != '\0') {
                name = gnome_get_language_from_locale (lang, NULL);
        } else {
                name = g_strdup ("—");
        }

        gtk_label_set_label (GTK_LABEL (widget), name);
        g_free (lang);
        g_free (name);

        /* Fingerprint: show when self, local, enabled, and possible */
        widget = get_widget (d, "account-fingerprint-button");
        label = get_widget (d, "account-fingerprint-label");
        show = (act_user_get_uid (user) == getuid() &&
                act_user_is_local_account (user) &&
                (d->login_screen_settings &&
                 g_settings_get_boolean (d->login_screen_settings, "enable-fingerprint-authentication")) &&
                set_fingerprint_label (widget));
        gtk_widget_set_visible (label, show);
        gtk_widget_set_visible (widget, show);

        /* Autologin: show when local account */
        widget = get_widget (d, "autologin-box");
        label = get_widget (d, "autologin-label");
        show = act_user_is_local_account (user);
        gtk_widget_set_visible (widget, show);
        gtk_widget_set_visible (label, show);

        /* Language: do not show for current user */
        widget = get_widget (d, "account-language-button");
        label = get_widget (d, "language-label");
        show = act_user_get_uid (user) != getuid();
        gtk_widget_set_visible (widget, show);
        gtk_widget_set_visible (label, show);

        /* Last login: show when administrator or current user */
        widget = get_widget (d, "last-login-button");
        label = get_widget (d, "last-login-button-label");

        current = act_user_manager_get_user_by_id (d->um, getuid ());
        show = act_user_get_uid (user) == getuid () ||
               act_user_get_account_type (current) == ACT_USER_ACCOUNT_TYPE_ADMINISTRATOR;
        if (show) {
                text = get_login_time_text (user);
                gtk_label_set_label (GTK_LABEL (label), text);
                g_free (text);
        }
        label = get_widget (d, "last-login-label");
        gtk_widget_set_visible (widget, show);
        gtk_widget_set_visible (label, show);

        enable = act_user_get_login_history (user) != NULL;
        gtk_widget_set_sensitive (widget, enable);

        if (d->permission != NULL)
                on_permission_changed (d->permission, NULL, d);
}

static void
selected_user_changed (GtkTreeSelection *selection, CcUserPanelPrivate *d)
{
        GtkTreeModel *model;
        GtkTreeIter iter;
        ActUser *user;

        if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
                gtk_tree_model_get (model, &iter, USER_COL, &user, -1);
                show_user (user, d);
                gtk_widget_set_sensitive (get_widget (d, "main-user-vbox"), TRUE);
                g_object_unref (user);
        } else {
                gtk_widget_set_sensitive (get_widget (d, "main-user-vbox"), FALSE);
        }
}

static void
change_name_done (GtkWidget          *entry,
                  CcUserPanelPrivate *d)
{
        const gchar *text;
        ActUser *user;

        user = get_selected_user (d);

        text = gtk_entry_get_text (GTK_ENTRY (entry));
        if (g_strcmp0 (text, act_user_get_real_name (user)) != 0 &&
            is_valid_name (text)) {
                act_user_set_real_name (user, text);
        }

        g_object_unref (user);
}

static void
change_name_focus_out (GtkWidget          *entry,
                       GdkEvent           *event,
                       CcUserPanelPrivate *d)
{
        change_name_done (entry, d);
}

static void
account_type_changed (GtkToggleButton    *button,
                      CcUserPanelPrivate *d)
{
        ActUser *user;
        gint account_type;
        gboolean self_selected;

        user = get_selected_user (d);
        self_selected = act_user_get_uid (user) == geteuid ();

        account_type = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)) ?  ACT_USER_ACCOUNT_TYPE_STANDARD : ACT_USER_ACCOUNT_TYPE_ADMINISTRATOR;

        if (account_type != act_user_get_account_type (user)) {
                act_user_set_account_type (user, account_type);

                if (self_selected)
                        show_restart_notification (d, NULL);
        }

        g_object_unref (user);
}

static void
restart_now (CcUserPanelPrivate *d)
{
        GDBusConnection *bus;

        gd_notification_dismiss (GD_NOTIFICATION (d->notification));

        bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
        g_dbus_connection_call (bus,
                                "org.gnome.SessionManager",
                                "/org/gnome/SessionManager",
                                "org.gnome.SessionManager",
                                "Logout",
                                g_variant_new ("(u)", 0),
                                NULL, 0, G_MAXINT,
                                NULL, NULL, NULL);
        g_object_unref (bus);
}

static void
show_restart_notification (CcUserPanelPrivate *d, const gchar *locale)
{
        GtkWidget *box;
        GtkWidget *label;
        GtkWidget *button;
        gchar *current_locale;

        if (d->notification)
                return;

        if (locale) {
                current_locale = g_strdup (setlocale (LC_MESSAGES, NULL));
                setlocale (LC_MESSAGES, locale);
        }

        d->notification = gd_notification_new ();
        g_object_add_weak_pointer (G_OBJECT (d->notification), (gpointer *)&d->notification);
        box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start (box, 6);
        gtk_widget_set_margin_end (box, 6);
        gtk_widget_set_margin_top (box, 6);
        gtk_widget_set_margin_bottom (box, 6);
        label = gtk_label_new (_("Your session needs to be restarted for changes to take effect"));
        gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (label), 30);
        g_object_set (G_OBJECT (label), "xalign", 0, NULL);
        button = gtk_button_new_with_label (_("Restart Now"));
        gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
        g_signal_connect_swapped (button, "clicked", G_CALLBACK (restart_now), d);
        gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
        gtk_widget_show_all (box);

        gtk_container_add (GTK_CONTAINER (d->notification), box);
        gtk_overlay_add_overlay (GTK_OVERLAY (get_widget (d, "overlay")), d->notification);
        gtk_widget_show (d->notification);

        if (locale) {
                setlocale (LC_MESSAGES, current_locale);
                g_free (current_locale);
        }
}

static void
language_response (GtkDialog         *dialog,
                   gint               response_id,
                   CcUserPanelPrivate *d)
{
        GtkWidget *button;
        ActUser *user;
        const gchar *lang, *account_language;
        gchar *current_language;
        gchar *name = NULL;
        gboolean self_selected;

        if (response_id != GTK_RESPONSE_OK) {
                gtk_widget_hide (GTK_WIDGET (dialog));
                return;
        }

        user = get_selected_user (d);
        account_language = act_user_get_language (user);
        self_selected = act_user_get_uid (user) == geteuid ();

        lang = cc_language_chooser_get_language (GTK_WIDGET (dialog));
        if (lang) {
                if (g_strcmp0 (lang, account_language) != 0) {
                        act_user_set_language (user, lang);

                        /* Do not show the notification if the locale is already used. */
                        current_language = gnome_normalize_locale (setlocale (LC_MESSAGES, NULL));
                        if (self_selected && g_strcmp0 (lang, current_language) != 0)
                                show_restart_notification (d, lang);
                        g_free (current_language);
                }

                button = get_widget (d, "account-language-button-label");
                name = gnome_get_language_from_locale (lang, NULL);
                gtk_label_set_label (GTK_LABEL (button), name);
                g_free (name);
        }

        g_object_unref (user);

        gtk_widget_hide (GTK_WIDGET (dialog));
}

static void
change_language (GtkButton *button,
                 CcUserPanelPrivate *d)
{
        const gchar *current_language;
        ActUser *user;

        user = get_selected_user (d);
        current_language = act_user_get_language (user);

        if (d->language_chooser) {
		cc_language_chooser_clear_filter (d->language_chooser);
                cc_language_chooser_set_language (d->language_chooser, NULL);
        }
        else {
                d->language_chooser = cc_language_chooser_new (gtk_widget_get_toplevel (d->main_box));

                g_signal_connect (d->language_chooser, "response",
                                  G_CALLBACK (language_response), d);
                g_signal_connect (d->language_chooser, "delete-event",
                                  G_CALLBACK (gtk_widget_hide_on_delete), NULL);

                gdk_window_set_cursor (gtk_widget_get_window (gtk_widget_get_toplevel (d->main_box)), NULL);
        }

        if (current_language && *current_language != '\0')
                cc_language_chooser_set_language (d->language_chooser, current_language);
        gtk_window_present (GTK_WINDOW (d->language_chooser));

        g_object_unref (user);
}

static void
change_password (GtkButton *button, CcUserPanelPrivate *d)
{
        ActUser *user;

        user = get_selected_user (d);

        um_password_dialog_set_user (d->password_dialog, user);
        um_password_dialog_show (d->password_dialog,
                                  GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)));

        g_object_unref (user);
}

static void
change_fingerprint (GtkButton *button, CcUserPanelPrivate *d)
{
        GtkWidget *widget;
        ActUser *user;

        user = get_selected_user (d);

        g_assert (g_strcmp0 (g_get_user_name (), act_user_get_user_name (user)) == 0);

        widget = get_widget (d, "account-fingerprint-button");
        fingerprint_button_clicked (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)), widget, user);

        g_object_unref (user);
}

static void
show_history (GtkButton *button, CcUserPanelPrivate *d)
{
        ActUser *user;

        user = get_selected_user (d);

        um_history_dialog_set_user (d->history_dialog, user);
        um_history_dialog_show (d->history_dialog, GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)));

        g_object_unref (user);
}

static gint
sort_users (GtkTreeModel *model,
            GtkTreeIter  *a,
            GtkTreeIter  *b,
            gpointer      data)
{
        ActUser *ua, *ub;
        gint sa, sb;
        gint result;

        gtk_tree_model_get (model, a, USER_COL, &ua, SORT_KEY_COL, &sa, -1);
        gtk_tree_model_get (model, b, USER_COL, &ub, SORT_KEY_COL, &sb, -1);

        if (sa < sb) {
                result = -1;
        }
        else if (sa > sb) {
                result = 1;
        }
        else {
                result = act_user_collate (ua, ub);
        }

        if (ua) {
                g_object_unref (ua);
        }
        if (ub) {
                g_object_unref (ub);
        }

        return result;
}

static gboolean
dont_select_headings (GtkTreeSelection *selection,
                      GtkTreeModel     *model,
                      GtkTreePath      *path,
                      gboolean          selected,
                      gpointer          data)
{
        GtkTreeIter iter;
        gboolean is_user;

        gtk_tree_model_get_iter (model, &iter, path);
        gtk_tree_model_get (model, &iter, USER_ROW_COL, &is_user, -1);

        return is_user;
}

static void
users_loaded (ActUserManager     *manager,
              GParamSpec         *pspec,
              CcUserPanelPrivate *d)
{
        GSList *list, *l;
        ActUser *user;
        GtkWidget *dialog;

        if (act_user_manager_no_service (d->um)) {
                dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (d->main_box)),
                                                 GTK_DIALOG_MODAL,
                                                 GTK_MESSAGE_OTHER,
                                                 GTK_BUTTONS_CLOSE,
                                                 _("Failed to contact the accounts service"));
                gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                          _("Please make sure that the AccountService is installed and enabled."));
                g_signal_connect_swapped (dialog, "response",
                                          G_CALLBACK (gtk_widget_destroy),
                                          dialog);
                gtk_widget_show (dialog);

                gtk_widget_set_sensitive (d->main_box, FALSE);
        }

        list = act_user_manager_list_users (d->um);
        g_debug ("Got %d users\n", g_slist_length (list));

        g_signal_connect (d->um, "user-changed", G_CALLBACK (user_changed), d);
        g_signal_connect (d->um, "user-is-logged-in-changed", G_CALLBACK (user_changed), d);

        for (l = list; l; l = l->next) {
                user = l->data;
                g_debug ("adding user %s\n", get_real_or_user_name (user));
                user_added (d->um, user, d);
        }
        show_user (list->data, d);
        g_slist_free (list);

        g_signal_connect (d->um, "user-added", G_CALLBACK (user_added), d);
        g_signal_connect (d->um, "user-removed", G_CALLBACK (user_removed), d);
}

static void
add_unlock_tooltip (GtkWidget *button)
{
        gchar *names[3];
        GIcon *icon;

        names[0] = "changes-allow-symbolic";
        names[1] = "changes-allow";
        names[2] = NULL;
        icon = (GIcon *)g_themed_icon_new_from_names (names, -1);
        /* Translator comments:
         * We split the line in 2 here to "make it look good", as there's
         * no good way to do this in GTK+ for tooltips. See:
         * https://bugzilla.gnome.org/show_bug.cgi?id=657168 */
        setup_tooltip_with_embedded_icon (button,
                                          _("To make changes,\nclick the * icon first"),
                                          "*",
                                          icon);
        g_object_unref (icon);
        g_signal_connect (button, "button-release-event",
                           G_CALLBACK (show_tooltip_now), NULL);
}

static void
remove_unlock_tooltip (GtkWidget *button)
{
        setup_tooltip_with_embedded_icon (button, NULL, NULL, NULL);
        g_signal_handlers_disconnect_by_func (button,
                                              G_CALLBACK (show_tooltip_now), NULL);
}

static void
on_permission_changed (GPermission *permission,
                       GParamSpec  *pspec,
                       gpointer     data)
{
        CcUserPanelPrivate *d = data;
        gboolean is_authorized;
        gboolean is_shared_account;
        gboolean self_selected;
        ActUser *user;
        GtkWidget *widget;

        user = get_selected_user (d);
        if (!user) {
                return;
        }

        is_authorized = g_permission_get_allowed (G_PERMISSION (d->permission));
        is_shared_account = g_strcmp0 (act_user_get_user_name (user), "shared") == 0;
        self_selected = act_user_get_uid (user) == geteuid ();

        gtk_stack_set_visible_child_name (GTK_STACK (d->headerbar_buttons), is_authorized ? PAGE_ADDUSER : PAGE_LOCK);

        widget = get_widget (d, "add-user-toolbutton");
        gtk_widget_set_sensitive (widget, is_authorized);
        if (is_authorized) {
                setup_tooltip_with_embedded_icon (widget, _("Create a user account"), NULL, NULL);
        }
        else {
                gchar *names[3];
                GIcon *icon;

                names[0] = "changes-allow-symbolic";
                names[1] = "changes-allow";
                names[2] = NULL;
                icon = (GIcon *)g_themed_icon_new_from_names (names, -1);
                setup_tooltip_with_embedded_icon (widget,
                                                  _("To create a user account,\nclick the * icon first"),
                                                  "*",
                                                  icon);
                g_object_unref (icon);
        }

        widget = get_widget (d, "remove-user-toolbutton");
        gtk_widget_set_sensitive (widget, is_authorized && !self_selected
                                  && !would_demote_only_admin (user));
        if (is_authorized) {
                setup_tooltip_with_embedded_icon (widget, _("Delete the selected user account"), NULL, NULL);
        }
        else {
                gchar *names[3];
                GIcon *icon;

                names[0] = "changes-allow-symbolic";
                names[1] = "changes-allow";
                names[2] = NULL;
                icon = (GIcon *)g_themed_icon_new_from_names (names, -1);

                setup_tooltip_with_embedded_icon (widget,
                                                  _("To delete the selected user account,\nclick the * icon first"),
                                                  "*",
                                                  icon);
                g_object_unref (icon);
        }

        if (!act_user_is_local_account (user)) {
                gtk_widget_set_sensitive (get_widget (d, "account-type-box"), FALSE);
                remove_unlock_tooltip (get_widget (d, "account-type-box"));
                gtk_widget_set_sensitive (GTK_WIDGET (get_widget (d, "autologin-switch")), FALSE);
                remove_unlock_tooltip (get_widget (d, "autologin-switch"));

        } else if (is_authorized && act_user_is_local_account (user)) {
                if (would_demote_only_admin (user)) {
                        gtk_widget_set_sensitive (get_widget (d, "account-type-box"), FALSE);
                } else {
                        gtk_widget_set_sensitive (get_widget (d, "account-type-box"), TRUE);
                }
                remove_unlock_tooltip (get_widget (d, "account-type-box"));

                gtk_widget_set_sensitive (GTK_WIDGET (get_widget (d, "autologin-switch")), get_autologin_possible (user));
                remove_unlock_tooltip (get_widget (d, "autologin-switch"));
        }
        else {
                gtk_widget_set_sensitive (get_widget (d, "account-type-box"), FALSE);
                if (would_demote_only_admin (user)) {
                        remove_unlock_tooltip (get_widget (d, "account-type-box"));
                } else {
                        add_unlock_tooltip (get_widget (d, "account-type-box"));
                }
                gtk_widget_set_sensitive (GTK_WIDGET (get_widget (d, "autologin-switch")), FALSE);
                add_unlock_tooltip (get_widget (d, "autologin-switch"));
        }

        /* The full name entry: insensitive if remote or not authorized and not
           self, or is Shared Account */
        widget = get_widget (d, "full-name-entry");
        if (!act_user_is_local_account (user)) {
                gtk_widget_set_sensitive (widget, FALSE);
                remove_unlock_tooltip (widget);
        } else if (!is_shared_account && (is_authorized || self_selected)) {
                gtk_widget_set_sensitive (widget, TRUE);
                remove_unlock_tooltip (widget);

        } else {
                gtk_widget_set_sensitive (widget, FALSE);
                add_unlock_tooltip (widget);
        }

        if (is_authorized || self_selected) {
                gtk_widget_show (get_widget (d, "user-icon-button"));
                gtk_widget_hide (get_widget (d, "user-icon-image"));

                gtk_widget_set_sensitive (get_widget (d, "account-language-button"), TRUE);
                remove_unlock_tooltip (get_widget (d, "account-language-button"));

                gtk_widget_set_sensitive (get_widget (d, "account-password-button"), TRUE);
                remove_unlock_tooltip (get_widget (d, "account-password-button"));

                gtk_widget_set_sensitive (get_widget (d, "account-fingerprint-button"), TRUE);
                remove_unlock_tooltip (get_widget (d, "account-fingerprint-button"));
        }
        else {
                gtk_widget_hide (get_widget (d, "user-icon-button"));
                gtk_widget_show (get_widget (d, "user-icon-image"));

                gtk_widget_set_sensitive (get_widget (d, "account-language-button"), FALSE);
                add_unlock_tooltip (get_widget (d, "account-language-button"));

                gtk_widget_set_sensitive (get_widget (d, "account-password-button"), FALSE);
                add_unlock_tooltip (get_widget (d, "account-password-button"));

                gtk_widget_set_sensitive (get_widget (d, "account-fingerprint-button"), FALSE);
                add_unlock_tooltip (get_widget (d, "account-fingerprint-button"));
        }

        um_password_dialog_set_user (d->password_dialog, user);

        g_object_unref (user);
}

static gboolean
match_user (GtkTreeModel *model,
            gint          column,
            const gchar  *key,
            GtkTreeIter  *iter,
            gpointer      search_data)
{
        ActUser *user;
        const gchar *name;
        gchar *normalized_key = NULL;
        gchar *normalized_name = NULL;
        gchar *case_normalized_key = NULL;
        gchar *case_normalized_name = NULL;
        gchar *p;
        gboolean result = TRUE;
        gint i;

        gtk_tree_model_get (model, iter, USER_COL, &user, -1);

        if (!user) {
                goto out;
        }

        normalized_key = g_utf8_normalize (key, -1, G_NORMALIZE_ALL);
        if (!normalized_key) {
                goto out;
        }

        case_normalized_key = g_utf8_casefold (normalized_key, -1);

        for (i = 0; i < 2; i++) {
                if (i == 0) {
                        name = act_user_get_real_name (user);
                }
                else {
                        name = act_user_get_user_name (user);
                }
                g_free (normalized_name);
                normalized_name = g_utf8_normalize (name, -1, G_NORMALIZE_ALL);
                if (normalized_name) {
                        g_free (case_normalized_name);
                        case_normalized_name = g_utf8_casefold (normalized_name, -1);
                        p = strstr (case_normalized_name, case_normalized_key);

                        /* poor man's \b */
                        if (p == case_normalized_name || (p && p[-1] == ' ')) {
                                result = FALSE;
                                break;
                        }
                }
        }

 out:
        if (user) {
                g_object_unref (user);
        }
        g_free (normalized_key);
        g_free (case_normalized_key);
        g_free (normalized_name);
        g_free (case_normalized_name);

        return result;
}

static void
setup_main_window (CcUserPanel *self)
{
        CcUserPanelPrivate *d = self->priv;
        GtkWidget *userlist;
        GtkTreeModel *model;
        GtkListStore *store;
        GtkTreeViewColumn *column;
        GtkCellRenderer *cell;
        GtkTreeSelection *selection;
        GtkWidget *button;
        GtkTreeIter iter;
        gint expander_size;
        gchar *title;
        GIcon *icon;
        GError *error = NULL;
        gchar *names[3];
        gboolean loaded;

        userlist = get_widget (d, "list-treeview");
        store = gtk_list_store_new (NUM_USER_LIST_COLS,
                                    ACT_TYPE_USER,
                                    G_TYPE_STRING,
                                    G_TYPE_BOOLEAN,
                                    G_TYPE_STRING,
                                    G_TYPE_BOOLEAN,
                                    G_TYPE_INT);
        model = (GtkTreeModel *)store;
        gtk_tree_sortable_set_default_sort_func (GTK_TREE_SORTABLE (model), sort_users, NULL, NULL);
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (model), GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
        gtk_tree_view_set_model (GTK_TREE_VIEW (userlist), model);
        gtk_tree_view_set_search_column (GTK_TREE_VIEW (userlist), USER_COL);
        gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (userlist),
                                             match_user, NULL, NULL);
        g_object_unref (model);

        gtk_widget_style_get (userlist, "expander-size", &expander_size, NULL);
        gtk_tree_view_set_level_indentation (GTK_TREE_VIEW (userlist), - (expander_size + 6));

        title = g_strdup_printf ("<small><span foreground=\"#555555\">%s</span></small>", _("My Account"));
        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter,
                            TITLE_COL, title,
                            HEADING_ROW_COL, TRUE,
                            SORT_KEY_COL, 0,
                            -1);
        g_free (title);

        d->other_accounts = 0;
        d->other_iter = NULL;

        column = gtk_tree_view_column_new ();
        cell = um_cell_renderer_user_image_new (userlist);
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (column), cell, FALSE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (column), cell, "user", USER_COL);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (column), cell, "visible", USER_ROW_COL);
        cell = gtk_cell_renderer_text_new ();
        g_object_set (cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (column), cell, TRUE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (column), cell, "markup", NAME_COL);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (column), cell, "visible", USER_ROW_COL);
        cell = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (column), cell, TRUE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (column), cell, "markup", TITLE_COL);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (column), cell, "visible", HEADING_ROW_COL);

        gtk_tree_view_append_column (GTK_TREE_VIEW (userlist), column);

        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (userlist));
        gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);
        g_signal_connect (selection, "changed", G_CALLBACK (selected_user_changed), d);
        gtk_tree_selection_set_select_function (selection, dont_select_headings, NULL, NULL);

        gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (get_widget (d, "list-scrolledwindow")), 300);
        gtk_widget_set_size_request (get_widget (d, "list-scrolledwindow"), 200, -1);

        button = get_widget (d, "add-user-toolbutton");
        g_signal_connect (button, "clicked", G_CALLBACK (add_user), d);

        button = get_widget (d, "remove-user-toolbutton");
        g_signal_connect (button, "clicked", G_CALLBACK (delete_user), self);

        button = get_widget (d, "user-icon-image");
        add_unlock_tooltip (button);

        button = get_widget (d, "full-name-entry");
        g_signal_connect (button, "activate", G_CALLBACK (change_name_done), d);
        g_signal_connect (button, "focus-out-event", G_CALLBACK (change_name_focus_out), d);

        button = get_widget (d, "account-type-standard");
        g_signal_connect (button, "toggled", G_CALLBACK (account_type_changed), d);

        button = get_widget (d, "account-password-button");
        g_signal_connect (button, "clicked", G_CALLBACK (change_password), d);

        button = get_widget (d, "account-language-button");
        g_signal_connect (button, "clicked", G_CALLBACK (change_language), d);

        button = get_widget (d, "autologin-switch");
        g_signal_connect (button, "notify::active", G_CALLBACK (autologin_changed), d);

        button = get_widget (d, "account-fingerprint-button");
        g_signal_connect (button, "clicked",
                          G_CALLBACK (change_fingerprint), d);

        button = get_widget (d, "last-login-button");
        g_signal_connect (button, "clicked",
                          G_CALLBACK (show_history), d);

        d->permission = (GPermission *)polkit_permission_new_sync (USER_ACCOUNTS_PERMISSION, NULL, NULL, &error);
        if (d->permission != NULL) {
                g_signal_connect (d->permission, "notify",
                                  G_CALLBACK (on_permission_changed), d);
                on_permission_changed (d->permission, NULL, d);
        } else {
                g_warning ("Cannot create '%s' permission: %s", USER_ACCOUNTS_PERMISSION, error->message);
                g_error_free (error);
        }

        button = get_widget (d, "add-user-toolbutton");
        names[0] = "changes-allow-symbolic";
        names[1] = "changes-allow";
        names[2] = NULL;
        icon = (GIcon *)g_themed_icon_new_from_names (names, -1);
        setup_tooltip_with_embedded_icon (button,
                                          _("To create a user account,\nclick the * icon first"),
                                          "*",
                                          icon);
        button = get_widget (d, "remove-user-toolbutton");
        setup_tooltip_with_embedded_icon (button,
                                          _("To delete the selected user account,\nclick the * icon first"),
                                          "*",
                                          icon);
        g_object_unref (icon);

        g_object_get (d->um, "is-loaded", &loaded, NULL);
        if (loaded)
                users_loaded (d->um, NULL, d);
        else
                g_signal_connect (d->um, "notify::is-loaded", G_CALLBACK (users_loaded), d);
}

static GSettings *
settings_or_null (const gchar *schema)
{
        GSettingsSchemaSource *source = NULL;
        gchar **non_relocatable = NULL;
        gchar **relocatable = NULL;
        GSettings *settings = NULL;

        source = g_settings_schema_source_get_default ();
        if (!source)
                return NULL;

        g_settings_schema_source_list_schemas (source, TRUE, &non_relocatable, &relocatable);

        if (g_strv_contains ((const gchar * const *)non_relocatable, schema) ||
            g_strv_contains ((const gchar * const *)relocatable, schema))
                settings = g_settings_new (schema);

        g_strfreev (non_relocatable);
        g_strfreev (relocatable);
        return settings;
}

static void
cc_user_panel_constructed (GObject *object)
{
        CcUserPanelPrivate *d;
        CcUserPanel *self = UM_USER_PANEL (object);
        GtkWidget *button;
        CcShell *shell;

        G_OBJECT_CLASS (cc_user_panel_parent_class)->constructed (object);
        d = self->priv;

        shell = cc_panel_get_shell (CC_PANEL (self));
        cc_shell_embed_widget_in_header (shell, d->headerbar_buttons);

        button = get_widget (d, "lock-button");
        gtk_lock_button_set_permission (GTK_LOCK_BUTTON (button), d->permission);

        shell = cc_panel_get_shell (CC_PANEL (object));

        /* Add scrollbars when screen is too small */
        if (cc_shell_is_small_screen (shell)) {
                GtkWidget *main_user_vbox, *hbox2, *sw;

                sw = gtk_scrolled_window_new (NULL, NULL);
                gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (sw), 400);

                main_user_vbox = get_widget (self->priv, "main-user-vbox");
                hbox2 = get_widget (self->priv, "hbox2");
                gtk_container_remove (GTK_CONTAINER (hbox2), main_user_vbox);
                gtk_container_add (GTK_CONTAINER (sw), main_user_vbox);
                gtk_container_add (GTK_CONTAINER (hbox2), sw);

                gtk_widget_show (sw);
        }
}

static void
cc_user_panel_init (CcUserPanel *self)
{
        CcUserPanelPrivate *d;
        GError *error;
        volatile GType type G_GNUC_UNUSED;
        GtkWidget *button;

        d = self->priv = UM_USER_PANEL_PRIVATE (self);
        g_resources_register (um_get_resource ());

        /* register types that the builder might need */
        type = um_user_image_get_type ();
        type = um_cell_renderer_user_image_get_type ();

        gtk_widget_set_size_request (GTK_WIDGET (self), -1, 350);

        d->builder = gtk_builder_new ();
        d->um = act_user_manager_get_default ();
        d->cancellable = g_cancellable_new ();

        error = NULL;
        if (!gtk_builder_add_from_resource (d->builder,
                                            "/org/gnome/control-center/user-accounts/user-accounts-dialog.ui",
                                            &error)) {
                g_error ("%s", error->message);
                g_error_free (error);
                return;
        }

        d->headerbar_buttons = get_widget (d, "headerbar-buttons");
        d->login_screen_settings = settings_or_null ("org.gnome.login-screen");

        d->password_dialog = um_password_dialog_new ();
        button = get_widget (d, "user-icon-button");
        d->photo_dialog = um_photo_dialog_new (button);
        d->main_box = get_widget (d, "accounts-vbox");
        gtk_container_add (GTK_CONTAINER (self), get_widget (d, "overlay"));
        d->history_dialog = um_history_dialog_new ();
        setup_main_window (self);
}

static void
cc_user_panel_dispose (GObject *object)
{
        CcUserPanelPrivate *priv = UM_USER_PANEL (object)->priv;

        g_cancellable_cancel (priv->cancellable);
        g_clear_object (&priv->cancellable);

        g_clear_object (&priv->login_screen_settings);

        if (priv->um) {
                g_signal_handlers_disconnect_by_data (priv->um, priv);
                priv->um = NULL;
        }
        if (priv->builder) {
                g_object_unref (priv->builder);
                priv->builder = NULL;
        }
        if (priv->password_dialog) {
                um_password_dialog_free (priv->password_dialog);
                priv->password_dialog = NULL;
        }
        if (priv->photo_dialog) {
                um_photo_dialog_free (priv->photo_dialog);
                priv->photo_dialog = NULL;
        }
        if (priv->history_dialog) {
                um_history_dialog_free (priv->history_dialog);
                priv->history_dialog = NULL;
        }
        if (priv->account_dialog) {
                gtk_dialog_response (GTK_DIALOG (priv->account_dialog), GTK_RESPONSE_DELETE_EVENT);
                priv->account_dialog = NULL;
        }
        if (priv->language_chooser) {
                gtk_widget_destroy (priv->language_chooser);
                priv->language_chooser = NULL;
        }
        if (priv->permission) {
                g_object_unref (priv->permission);
                priv->permission = NULL;
        }
        if (priv->other_iter) {
                gtk_tree_iter_free (priv->other_iter);
                priv->other_iter = NULL;
        }
        G_OBJECT_CLASS (cc_user_panel_parent_class)->dispose (object);
}

static const char *
cc_user_panel_get_help_uri (CcPanel *panel)
{
	return "help:gnome-help/security-and-privacy#user";
}

static void
cc_user_panel_class_init (CcUserPanelClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);
        CcPanelClass *panel_class = CC_PANEL_CLASS (klass);

        object_class->dispose = cc_user_panel_dispose;
        object_class->constructed = cc_user_panel_constructed;

        panel_class->get_help_uri = cc_user_panel_get_help_uri;

        g_type_class_add_private (klass, sizeof (CcUserPanelPrivate));
}
