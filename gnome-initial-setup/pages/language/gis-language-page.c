/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat
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
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 *     Michael Wood <michael.g.wood@intel.com>
 *
 * Based on gnome-control-center cc-region-panel.c
 */

/* Language page {{{1 */

#define PAGE_ID "language"

#define OSRELEASE_FILE      "/etc/os-release"
#define SERIAL_VERSION_FILE "/sys/devices/virtual/dmi/id/product_serial"

#include "config.h"
#include "language-resources.h"
#include "cc-language-chooser.h"
#include "gis-language-page.h"

#include <act/act-user-manager.h>
#include <polkit/polkit.h>
#include <locale.h>
#include <gtk/gtk.h>
#include <zint.h>

struct _GisLanguagePagePrivate
{
  GtkWidget *language_chooser;
  GDBusProxy *localed;
  GPermission *permission;
  const gchar *new_locale_id;

  GCancellable *cancellable;

  GtkAccelGroup *accel_group;
};
typedef struct _GisLanguagePagePrivate GisLanguagePagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GisLanguagePage, gis_language_page, GIS_TYPE_PAGE);

#define OBJ(type,name) ((type)gtk_builder_get_object(GIS_PAGE (page)->builder,(name)))
#define WID(name) OBJ(GtkWidget*,name)

static void
set_localed_locale (GisLanguagePage *self)
{
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (self);
  GVariantBuilder *b;
  gchar *s;

  if (!priv->localed)
    return;

  b = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  s = g_strconcat ("LANG=", priv->new_locale_id, NULL);
  g_variant_builder_add (b, "s", s);
  g_free (s);

  g_dbus_proxy_call (priv->localed,
                     "SetLocale",
                     g_variant_new ("(asb)", b, TRUE),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1, NULL, NULL, NULL);
  g_variant_builder_unref (b);
}

static void
change_locale_permission_acquired (GObject      *source,
                                   GAsyncResult *res,
                                   gpointer      data)
{
  GisLanguagePage *page = GIS_LANGUAGE_PAGE (data);
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (page);
  GError *error = NULL;
  gboolean allowed;

  allowed = g_permission_acquire_finish (priv->permission, res, &error);
  if (error) {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to acquire permission: %s\n", error->message);
      g_error_free (error);
      return;
  }

  if (allowed)
    set_localed_locale (page);
}

static void
user_loaded (GObject    *object,
             GParamSpec *pspec,
             gpointer    user_data)
{
  gchar *new_locale_id = user_data;

  act_user_set_language (ACT_USER (object), new_locale_id);

  g_free (new_locale_id);
}

static gboolean
_selection_done (gpointer driver)
{
  gis_assistant_next_page (gis_driver_get_assistant (GIS_DRIVER (driver)));

  return FALSE;
}

static void
set_language (GisLanguagePage *page)
{
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (page);
  ActUser *user;
  GisDriver *driver;

  priv->new_locale_id = cc_language_chooser_get_language (CC_LANGUAGE_CHOOSER (priv->language_chooser));
  driver = GIS_PAGE (page)->driver;

  setlocale (LC_MESSAGES, priv->new_locale_id);
  gis_driver_locale_changed (driver);

  if (gis_driver_get_mode (driver) == GIS_DRIVER_MODE_NEW_USER) {
      if (g_permission_get_allowed (priv->permission)) {
          set_localed_locale (page);
      }
      else if (g_permission_get_can_acquire (priv->permission)) {
          g_permission_acquire_async (priv->permission,
                                      NULL,
                                      change_locale_permission_acquired,
                                      page);
      }
  }
  user = act_user_manager_get_user (act_user_manager_get_default (),
                                    g_get_user_name ());
  if (act_user_is_loaded (user))
    act_user_set_language (user, priv->new_locale_id);
  else
    g_signal_connect (user,
                      "notify::is-loaded",
                      G_CALLBACK (user_loaded),
                      g_strdup (priv->new_locale_id));

  gis_driver_set_user_language (driver, priv->new_locale_id);
}

static void
language_activated (CcLanguageChooser *chooser,
                    gchar             *language,
                    GisLanguagePage   *page)
{
  set_language (page);
}

static void
language_changed (CcLanguageChooser *chooser,
                  GParamSpec        *pspec,
                  GisLanguagePage   *page)
{
  set_language (page);
}

static void
ensure_localed_proxy (GisLanguagePage *page)
{
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (page);
  GDBusConnection *bus;
  GError *error = NULL;

  priv->permission = polkit_permission_new_sync ("org.freedesktop.locale1.set-locale", NULL, NULL, NULL);

  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  priv->localed = g_dbus_proxy_new_sync (bus,
                                         G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                                         NULL,
                                         "org.freedesktop.locale1",
                                         "/org/freedesktop/locale1",
                                         "org.freedesktop.locale1",
                                         priv->cancellable,
                                         &error);
  g_object_unref (bus);

  if (error != NULL) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to contact localed: %s\n", error->message);
    g_error_free (error);
  }
}

static gchar *
create_serial_barcode (const gchar *serial)
{
  gchar *savefile;
  struct zint_symbol *barcode;

  savefile = g_build_filename (g_get_user_cache_dir (), "product_serial.png", NULL);

  barcode = ZBarcode_Create();
  strncpy ((char *) barcode->outfile, savefile, 4096);
  if (ZBarcode_Encode_and_Print (barcode, (gchar *) serial, 0, 0)) {
    g_warning ("Error while generating barcode: %s", barcode->errtxt);
  }
  ZBarcode_Delete (barcode);

  return savefile;
}

static gchar *
get_serial_version (void)
{
  GError *error = NULL;
  gchar *serial = NULL;

  g_file_get_contents (SERIAL_VERSION_FILE, &serial, NULL, &error);

  if (error) {
    g_warning ("Error when reading " SERIAL_VERSION_FILE ": %s", error->message);
    g_error_free (error);
    return NULL;
  }

  return serial;
}

static gchar *
get_software_version (void)
{
  GDataInputStream *datastream;
  GError *error = NULL;
  GFile *osrelease_file = NULL;
  GFileInputStream *filestream;
  GString *software_version;
  gchar *line;
  gchar *name = NULL;
  gchar *version = NULL;
  gchar *version_string = NULL;

  osrelease_file = g_file_new_for_path (OSRELEASE_FILE);
  filestream = g_file_read (osrelease_file, NULL, &error);
  if (error) {
    goto bailout;
  }

  datastream = g_data_input_stream_new (G_INPUT_STREAM (filestream));

  while ((!name || !version) &&
         (line = g_data_input_stream_read_line (datastream, NULL, NULL, &error))) {
    if (g_str_has_prefix (line, "NAME=")) {
      name = line;
    } else if (g_str_has_prefix (line, "VERSION=")) {
      version = line;
    } else {
      g_free (line);
    }
  }

  if (error) {
    goto bailout;
  }

  software_version = g_string_new ("");

  if (name) {
    g_string_append (software_version, name + strlen ("NAME=\""));
    g_string_erase (software_version, software_version->len - 1, 1);
  }

  if (version) {
    if (name) {
      g_string_append_c (software_version, ' ');
    }
    g_string_append (software_version, version + strlen ("VERSION=\""));
    g_string_erase (software_version, software_version->len - 1, 1);
  }

  version_string = g_string_free (software_version, FALSE);

 bailout:
  g_free (name);
  g_free (version);

  if (error) {
    g_warning ("Error reading " OSRELEASE_FILE ": %s", error->message);
    g_error_free (error);
  }

  g_clear_object (&datastream);
  g_clear_object (&filestream);
  g_clear_object (&osrelease_file);

  if (version_string) {
    return version_string;
  } else {
    return g_strdup ("");
  }
}

static void
system_poweroff (gpointer data)
{
  GDBusConnection *bus;
  GError *error = NULL;
  GPermission *permission;

  permission = polkit_permission_new_sync ("org.freedesktop.login1.power-off", NULL, NULL, &error);
  if (error) {
    g_warning ("Failed getting permission to power off: %s", error->message);
    g_error_free (error);
    return;
  }

  if (!g_permission_get_allowed (permission)) {
    g_warning ("Not allowed to power off");
    g_object_unref (permission);
    return;
  }

  g_object_unref (permission);

  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (error) {
    g_warning ("Failed to get system bus: %s", error->message);
    g_error_free (error);
    return;
  }

  g_dbus_connection_call (bus,
                          "org.freedesktop.login1",
                          "/org/freedesktop/login1",
                          "org.freedesktop.login1.Manager",
                          "PowerOff",
                          g_variant_new ("(b)", FALSE),
                          NULL, 0, G_MAXINT, NULL, NULL, NULL);

  g_object_unref (bus);
}

static void
show_factory_dialog (GisLanguagePage *page)
{
  GtkButton *poweroff_button;
  GtkDialog *factory_dialog;
  GtkImage *serial_image;
  GtkLabel *serial_label;
  GtkLabel *version_label;
  gchar *barcode;
  gchar *serial;
  gchar *version;

  factory_dialog = OBJ (GtkDialog *, "factory-dialog");
  version_label = OBJ (GtkLabel *, "software-version");
  serial_label = OBJ (GtkLabel *, "serial-text");
  serial_image = OBJ (GtkImage *, "serial-barcode");
  poweroff_button = OBJ (GtkButton *, "poweroff-button");

  version = get_software_version ();
  gtk_label_set_text (version_label, version);

  serial = get_serial_version ();
  if (serial) {
    gtk_label_set_text (serial_label, serial);

    barcode = create_serial_barcode (serial);
    gtk_image_set_from_file (serial_image, barcode);
  } else {
    gtk_widget_set_visible (GTK_WIDGET (serial_label), FALSE);
    gtk_widget_set_visible (GTK_WIDGET (serial_image), FALSE);
  }

  g_signal_connect_swapped (poweroff_button, "clicked",
                            G_CALLBACK (system_poweroff), NULL);

  gtk_window_set_transient_for (GTK_WINDOW (factory_dialog),
                                GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (page))));
  gtk_window_set_modal (GTK_WINDOW (factory_dialog), TRUE);
  gtk_window_present (GTK_WINDOW (factory_dialog));
  g_signal_connect (factory_dialog, "delete-event",
                    G_CALLBACK (gtk_widget_hide_on_delete), NULL);

  if (serial) {
    g_remove (barcode);
    g_free (barcode);
    g_free (serial);
  }

  g_free (version);
}

static void
gis_language_page_constructed (GObject *object)
{
  GisLanguagePage *page = GIS_LANGUAGE_PAGE (object);
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (page);
  GisDriver *driver = GIS_PAGE (page)->driver;
  GClosure *closure;
  const gchar *lang_override;

  g_type_ensure (CC_TYPE_LANGUAGE_CHOOSER);

  G_OBJECT_CLASS (gis_language_page_parent_class)->constructed (object);

  gtk_container_add (GTK_CONTAINER (page), WID ("language-page"));

  priv->language_chooser = WID ("language-chooser");

  /* Set initial language override */
  lang_override = gis_driver_get_language_override (driver);
  if (lang_override)
    cc_language_chooser_set_language (CC_LANGUAGE_CHOOSER (priv->language_chooser), lang_override);

  /* Now connect to language chooser changes */
  g_signal_connect (priv->language_chooser, "notify::language",
                    G_CALLBACK (language_changed), page);
  g_signal_connect (priv->language_chooser, "language-activated",
                    G_CALLBACK (language_activated), page);

  /* If we're in new user mode then we're manipulating system settings */
  if (gis_driver_get_mode (driver) == GIS_DRIVER_MODE_NEW_USER)
    ensure_localed_proxy (page);

  /* Propagate initial language setting to localed/AccountsService */
  set_language (page);

  /* Use ctrl+f to show factory dialog */
  priv->accel_group = gtk_accel_group_new ();
  closure = g_cclosure_new_swap (G_CALLBACK (show_factory_dialog), page, NULL);
  gtk_accel_group_connect (priv->accel_group, GDK_KEY_f, GDK_CONTROL_MASK, 0, closure);
  g_closure_unref (closure);

  gis_page_set_complete (GIS_PAGE (page), TRUE);
  gtk_widget_show (GTK_WIDGET (page));
}

static void
gis_language_page_locale_changed (GisPage *page)
{
  gis_page_set_title (GIS_PAGE (page), _("Welcome"));
}

static void
gis_language_page_dispose (GObject *object)
{
  GisLanguagePage *page = GIS_LANGUAGE_PAGE (object);
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (page);

  g_clear_object (&priv->permission);
  g_clear_object (&priv->localed);
  g_clear_object (&priv->cancellable);
  g_clear_object (&priv->accel_group);
}

static GtkAccelGroup *
gis_language_page_get_accel_group (GisPage *page)
{
  GisLanguagePage *language_page = GIS_LANGUAGE_PAGE (page);
  GisLanguagePagePrivate *priv = gis_language_page_get_instance_private (language_page);

  return priv->accel_group;
}

static void
gis_language_page_class_init (GisLanguagePageClass *klass)
{
  GisPageClass *page_class = GIS_PAGE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  page_class->page_id = PAGE_ID;
  page_class->locale_changed = gis_language_page_locale_changed;
  page_class->get_accel_group = gis_language_page_get_accel_group;
  object_class->constructed = gis_language_page_constructed;
  object_class->dispose = gis_language_page_dispose;
}

static void
gis_language_page_init (GisLanguagePage *page)
{
  g_resources_register (language_get_resource ());
}

void
gis_prepare_language_page (GisDriver *driver)
{
  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_LANGUAGE_PAGE,
                                     "driver", driver,
                                     NULL));
}
