/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (C) 2016–2018 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Original code written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

/* Finished page {{{1 */

#define PAGE_ID "finished"

#include "config.h"
#include "finished-resources.h"
#include "gis-finished-page.h"
#include "gis-store.h"

#include <gtk/gtkx.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <errno.h>
#include <attr/xattr.h>

#include <udisks/udisks.h>

struct _GisFinishedPagePrivate {
  gint led_state;
};
typedef struct _GisFinishedPagePrivate GisFinishedPagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GisFinishedPage, gis_finished_page, GIS_TYPE_PAGE);

#define OBJ(type,name) ((type)gtk_builder_get_object(GIS_PAGE(page)->builder,(name)))
#define WID(name) OBJ(GtkWidget*,name)

/* See endlessm/gnome-shell/js/gdm/authPrompt.js */
#define CUSTOMER_SUPPORT_FILENAME "vendor-customer-support.ini"
#define CUSTOMER_SUPPORT_GROUP_NAME "Customer Support"
#define CUSTOMER_SUPPORT_KEY_EMAIL "Email"
#define CUSTOMER_SUPPORT_EMAIL_FALLBACK "support@endlessm.com"

static const gchar * const customer_support_paths[] = {
  LOCALSTATEDIR "/lib/eos-image-defaults/" CUSTOMER_SUPPORT_FILENAME,
  DATADIR "/gnome-shell/" CUSTOMER_SUPPORT_FILENAME,
  NULL
};

static gchar *
get_customer_support_email (void)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new ();
  const gchar * const *path_iter;
  g_autofree gchar *support_email = NULL;
  g_autoptr(GError) error = NULL;

  for (path_iter = customer_support_paths; *path_iter != NULL; path_iter++)
    {
      if (!g_key_file_load_from_file (key_file, *path_iter, G_KEY_FILE_NONE,
                                      &error))
        {
          if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning ("%s: %s", *path_iter, error->message);

          g_clear_error (&error);
          continue;
        }

      support_email = g_key_file_get_locale_string (key_file,
                                                    CUSTOMER_SUPPORT_GROUP_NAME,
                                                    CUSTOMER_SUPPORT_KEY_EMAIL,
                                                    NULL, &error);
      if (support_email == NULL)
        {
          g_warning ("[%s] %s key not found in %s: %s",
                     CUSTOMER_SUPPORT_GROUP_NAME, CUSTOMER_SUPPORT_KEY_EMAIL,
                     *path_iter, error->message);
          g_clear_error (&error);
        }
      else
        {
          return g_steal_pointer (&support_email);
        }
    }

  g_warning ("[%s] %s key not found in any %s, using default",
             CUSTOMER_SUPPORT_GROUP_NAME, CUSTOMER_SUPPORT_KEY_EMAIL,
             CUSTOMER_SUPPORT_FILENAME);
  return g_strdup (CUSTOMER_SUPPORT_EMAIL_FALLBACK);
}

static void
reboot_cb (GtkButton *button, GisFinishedPage *page)
{
  g_spawn_command_line_sync ("/usr/bin/systemctl poweroff", NULL, NULL, NULL, NULL);
  g_application_quit(G_APPLICATION (GIS_PAGE (page)->driver));
}

static gboolean
toggle_leds (GisPage *page)
{
  XKeyboardControl values;
  GisFinishedPage *summary = GIS_FINISHED_PAGE (page);
  GisFinishedPagePrivate *priv = gis_finished_page_get_instance_private (summary);

  values.led_mode = priv->led_state;
  XChangeKeyboardControl(GDK_DISPLAY_XDISPLAY(gtk_widget_get_display(GTK_WIDGET(page))), KBLedMode, &values);
  priv->led_state = priv->led_state == 1 ? 0 : 1;
  return TRUE;
}

#define EOS_IMAGE_VERSION_XATTR "user.eos-image-version"
#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

static gchar *
get_image_version (const gchar *path)
{
  ssize_t attrsize;
  gchar *value;

  g_return_val_if_fail (path != NULL, NULL);

  attrsize = getxattr (path, EOS_IMAGE_VERSION_XATTR, NULL, 0);
  if (attrsize < 0) {
    g_message ("Error examining " EOS_IMAGE_VERSION_XATTR " on %s: %s",
               path, g_strerror (errno));
    return NULL;
  }

  value = g_malloc (attrsize + 1);
  value[attrsize] = 0;

  attrsize = getxattr (path, EOS_IMAGE_VERSION_XATTR, value,
                       attrsize);
  if (attrsize < 0) {
    g_warning ("Error reading " EOS_IMAGE_VERSION_XATTR " on %s: %s",
               path, g_strerror (errno));
    g_free (value);
    return NULL;
  }

  return value;
}

static gboolean
is_eosdvd (void)
{
  g_autofree gchar *image_version = get_image_version (EOS_IMAGE_VERSION_PATH);

  if (image_version == NULL)
    image_version = get_image_version (EOS_IMAGE_VERSION_ALT_PATH);

  return image_version != NULL &&
    g_str_has_prefix (image_version, "eosdvd-");
}

static void
gis_finished_page_shown (GisPage *page)
{
  GError *error = gis_store_get_error();

  gis_driver_save_data (GIS_PAGE (page)->driver);

  if (error != NULL)
    {
      GisAssistant *assistant = gis_driver_get_assistant (page->driver);
      GtkLabel *error_heading_label = OBJ (GtkLabel*, "error_heading_label");
      GtkLabel *error_label = OBJ (GtkLabel*, "error_label");

      if (error->domain == GIS_UNATTENDED_ERROR)
        gtk_label_set_text (error_heading_label,
                            _("Oops, something is wrong with your unattended installation configuration."));
      /* Otherwise, leave the default message (indicating a problem with the
       * image file). TODO: handle other domains that indicate problems
       * elsewhere.
       */

      gtk_label_set_text (error_label, error->message);
      gtk_widget_show (WID ("error_box"));
      gtk_widget_hide (WID ("success_box"));
      gis_assistant_locale_changed (assistant);

      if (gis_store_is_unattended())
        {
          g_timeout_add_seconds (1, (GSourceFunc)toggle_leds, page);
        }
    }
  else
    {
      gboolean optical = is_eosdvd ();

      gtk_widget_set_visible (WID ("removelabel_usb"), !optical);
      gtk_widget_set_visible (WID ("removelabel_dvd"),  optical);
    }
}

static void
gis_finished_page_constructed (GObject *object)
{
  GisFinishedPage *page = GIS_FINISHED_PAGE (object);

  G_OBJECT_CLASS (gis_finished_page_parent_class)->constructed (object);

  gtk_container_add (GTK_CONTAINER (page), WID ("finished-page"));

  gis_page_set_complete (GIS_PAGE (page), TRUE);

  g_signal_connect (OBJ (GtkButton *, "restart_button"), "clicked", G_CALLBACK(reboot_cb), page);

  gtk_widget_show (GTK_WIDGET (page));
}

static void
gis_finished_page_locale_changed (GisPage *page)
{
  g_autofree gchar *support_email = NULL;
  g_autofree gchar *support_email_markup = NULL;
  g_autofree gchar *support_markup = NULL;

  if (gis_store_get_error() == NULL)
    {
      gis_page_set_title (page, _("Reformatting Finished"));
    }
  else
    {
      gis_page_set_title (page, "");
    }

  support_email = get_customer_support_email ();
  support_email_markup = g_strdup_printf ("<a href=\"mailto:%1$s\">%1$s</a>",
                                          support_email);
  /* Translators: the %s is the customer support email address */
  support_markup = g_strdup_printf (_("Please contact %s or join the <a href=\"https://community.endlessos.com/\">Endless Community</a> to troubleshoot."),
                                    support_email_markup);
  gtk_label_set_markup (OBJ (GtkLabel *, "support_label"), support_markup);
}

static void
gis_finished_page_class_init (GisFinishedPageClass *klass)
{
  GisPageClass *page_class = GIS_PAGE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  page_class->page_id = PAGE_ID;
  page_class->locale_changed = gis_finished_page_locale_changed;
  page_class->shown = gis_finished_page_shown;
  object_class->constructed = gis_finished_page_constructed;
}

static void
gis_finished_page_init (GisFinishedPage *page)
{
  g_resources_register (finished_get_resource ());
}

void
gis_prepare_finished_page (GisDriver *driver)
{
  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_FINISHED_PAGE,
                                     "driver", driver,
                                     NULL));
}
