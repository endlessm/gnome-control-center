/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <config.h>

#include <gtk/gtk.h>
#include <stdlib.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>

#include <GL/gl.h>
#include <GL/glx.h>
#endif

#ifdef GDK_WINDOWING_X11
static char *
get_gl_renderer (void)
{
  Display *display;
  int attributes[] = {
    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
    GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    None
  };
  int nconfigs;
  int major, minor;
  Window window;
  GLXFBConfig *config;
  GLXWindow glxwin;
  GLXContext context;
  XSetWindowAttributes win_attributes;
  XVisualInfo *visualInfo;
  char *renderer;

  gdk_error_trap_push ();

  display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());

  glXQueryVersion (display, &major, &minor);
  config = glXChooseFBConfig (display, DefaultScreen (display),
                              attributes, &nconfigs);
  if (config == NULL) {
    g_warning ("Failed to get OpenGL configuration");

    gdk_error_trap_pop_ignored ();
    return NULL;
  }
  visualInfo = glXGetVisualFromFBConfig (display, *config);
  win_attributes.colormap = XCreateColormap (display, DefaultRootWindow(display),
                                        visualInfo->visual, AllocNone );

  window = XCreateWindow (display, DefaultRootWindow (display),
                                0, 0, /* x, y */
                                1, 1, /* width, height */
                                0,   /* border_width */
                                visualInfo->depth, InputOutput,
                                visualInfo->visual, CWColormap, &win_attributes);
  glxwin = glXCreateWindow (display, *config, window, NULL);

  context = glXCreateNewContext (display, *config, GLX_RGBA_TYPE,
                                 NULL, TRUE);
  XFree (config);

  glXMakeContextCurrent (display, glxwin, glxwin, context);
  renderer = (char *) glGetString (GL_RENDERER);
  g_debug ("Got GL_RENDERER: '%s'", renderer);

  glXMakeContextCurrent (display, None, None, NULL);
  glXDestroyContext (display, context);
  glXDestroyWindow (display, glxwin);
  XDestroyWindow (display, window);
  XFree (visualInfo);

  if (gdk_error_trap_pop () != Success) {
    g_warning ("Failed to get OpenGL driver info");
    return NULL;
  }

  return renderer;
}
#endif /* GDK_WINDOWING_X11 */

int
main (int argc,
      char **argv)
{
  char *renderer = NULL;

  gtk_init (&argc, &argv);

#ifdef GDK_WINDOWING_X11
  renderer = get_gl_renderer ();
#endif

  if (renderer != NULL)
    {
      g_print ("%s\n", renderer);
      return EXIT_SUCCESS;
    }

  return EXIT_FAILURE;
}
