/* um-app-permissions.h
 *
 * Copyright 2018 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#pragma once

#include <act/act.h>
#include <gtk/gtk.h>
#include <shell/cc-panel.h>

G_BEGIN_DECLS

#define UM_TYPE_APP_PERMISSIONS (um_app_permissions_get_type())

G_DECLARE_FINAL_TYPE (UmAppPermissions, um_app_permissions, UM, APP_PERMISSIONS, GtkGrid)

ActUser* um_app_permissions_get_user (UmAppPermissions *self);
void     um_app_permissions_set_user (UmAppPermissions *self,
                                      ActUser          *user);

GPermission *um_app_permissions_get_permission (UmAppPermissions *self);
void         um_app_permissions_set_permission (UmAppPermissions *self,
                                                GPermission      *permission);

G_END_DECLS