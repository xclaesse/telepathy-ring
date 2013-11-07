/*
 * Copyright (C) 2012 Jolla Ltd.
 * Contact: John Brooks <john.brooks@jollamobile.com>
 *
 * Based on Empathy ubuntu-online-accounts:
 * Copyright (C) 2012 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "mcp-account-manager-ring.h"
#include <string.h>

#define PLUGIN_NAME "ring-account"
#define PLUGIN_PRIORITY (MCP_ACCOUNT_STORAGE_PLUGIN_PRIO_READONLY)
#define PLUGIN_DESCRIPTION "Provide account for telepathy-ring"
#define PLUGIN_PROVIDER "im.telepathy.Account.Storage.Ring"

static void account_storage_iface_init (McpAccountStorageIface *iface);

G_DEFINE_TYPE_WITH_CODE (McpAccountManagerRing, mcp_account_manager_ring,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (MCP_TYPE_ACCOUNT_STORAGE,
        account_storage_iface_init))

struct _McpAccountManagerRingPrivate
{
  McpAccountManager *am;
  GDBusProxy *manager_proxy;
  GCancellable *cancellable;
  /* string -> GHashTable<static string, string> */
  GHashTable *modems;
  /* Queue of DelayedSignalData */
  GQueue *pending_signals;

  gboolean ready;
};

typedef enum
{
  DELAYED_CREATE,
  DELAYED_DELETE,
} DelayedSignal;

typedef struct
{
  DelayedSignal signal;
  gchar *path;
} DelayedSignalData;

static void
remove_modem (McpAccountManagerRing *self,
    const gchar *path)
{
  GHashTableIter iter;
  gpointer key, value;

  if (!self->priv->ready)
    {
      DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

      data->signal = DELAYED_DELETE;
      data->path = g_strdup (path);

      g_queue_push_tail (self->priv->pending_signals, data);
      return;
    }

  g_debug ("Removing modem %s\n", path);

  g_hash_table_iter_init (&iter, self->priv->modems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GHashTable *params = value;

      if (g_str_equal (g_hash_table_lookup (params, "param-modem"), path))
        {
          g_signal_emit_by_name (self, "deleted", key);
          g_hash_table_iter_remove (&iter);
          break;
        }
    }
}

static void
add_modem (McpAccountManagerRing *self,
    const gchar *path)
{
  gchar *account_name;
  GHashTable *params;

  if (!self->priv->ready)
    {
      DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

      data->signal = DELAYED_CREATE;
      data->path = g_strdup (path);

      g_queue_push_tail (self->priv->pending_signals, data);
      return;
    }

  g_debug ("Adding modem %s\n", path);

  account_name = mcp_account_manager_get_unique_name (self->priv->am,
      "ring", "tel", "account");

#define PARAM(key, value) g_hash_table_insert (params, key, g_strdup (value));
  params = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, g_free);
  PARAM ("manager", "ring");
  PARAM ("protocol", "tel");
  PARAM ("DisplayName", "Cellular");
  PARAM ("Enabled", "true");
  PARAM ("ConnectAutomatically", "true");
  PARAM ("always_dispatch", "true");
  PARAM ("param-modem", path);
  PARAM ("org.freedesktop.Telepathy.Account.Interface.Addressing.URISchemes",
      "tel;");
#undef PARAM

  g_hash_table_insert (self->priv->modems, account_name, params);
  g_signal_emit_by_name (self, "created", account_name);
}

static void
manager_proxy_signal_cb (GDBusProxy *proxy,
    gchar *sender_name,
    gchar *signal_name,
    GVariant *parameters,
    McpAccountManagerRing *self)
{
  const gchar *path;

  if (g_str_equal (signal_name, "ModemAdded"))
    {
      g_variant_get (parameters, "(&o@a{sv})", &path, NULL);
      add_modem (self, path);
    }
  else if (g_str_equal (signal_name, "ModemRemoved"))
    {
      g_variant_get (parameters, "(&o)", &path);
      remove_modem (self, path);
    }
}

static void
got_modems_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  McpAccountManagerRing *self = user_data;
  GVariant *modems;
  GVariantIter *iter;
  const gchar *path;
  GError *error = NULL;

  modems = g_dbus_proxy_call_finish ((GDBusProxy *) source, result, &error);
  if (modems == NULL)
    {
      g_debug ("Failed to get ofono modems: %s", error->message);
      g_clear_error (&error);
      return;
    }

  g_variant_get (modems, "(a(oa{sv}))", &iter);
  while (g_variant_iter_loop (iter, "(&o@a{sv})", &path, NULL))
    add_modem (self, path);
  g_variant_iter_free (iter);
  g_variant_unref (modems);
}

static void
got_manager_proxy_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  McpAccountManagerRing *self = user_data;
  GDBusProxy *proxy;
  GError *error = NULL;

  /* It does not use self->priv->proxy_manager here because self could have been
   * disposed, in which case we'll get a cancelled error. */
  proxy = g_dbus_proxy_new_finish (result, &error);
  if (proxy == NULL)
    {
      g_debug ("Failed to get ofono manager proxy: %s", error->message);
      g_clear_error (&error);
      return;
    }
  self->priv->manager_proxy = proxy;

  g_dbus_proxy_call (self->priv->manager_proxy, "GetModems", NULL,
      G_DBUS_CALL_FLAGS_NONE, -1, self->priv->cancellable,
      got_modems_cb, self);

  g_signal_connect_object (self->priv->manager_proxy, "g-signal",
      G_CALLBACK (manager_proxy_signal_cb), self, 0);
}

static void
got_system_bus_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  McpAccountManagerRing *self = user_data;
  GDBusConnection *connection;
  GError *error = NULL;

  connection = g_bus_get_finish (result, &error);
  if (connection == NULL)
    {
      g_debug ("Failed to get system bus: %s", error->message);
      g_clear_error (&error);
      return;
    }

  g_dbus_proxy_new (connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
      "org.ofono", "/", "org.ofono.Manager", self->priv->cancellable,
      got_manager_proxy_cb, self);

  g_object_unref (connection);
}

static void
mcp_account_manager_ring_dispose (GObject *object)
{
  McpAccountManagerRing *self = (McpAccountManagerRing*) object;

  /* Cancel any pending operation */
  if (self->priv->cancellable != NULL)
    g_cancellable_cancel (self->priv->cancellable);

  g_clear_object (&self->priv->am);
  g_clear_object (&self->priv->manager_proxy);
  g_clear_object (&self->priv->cancellable);
  g_clear_pointer (&self->priv->modems, g_hash_table_unref);

  G_OBJECT_CLASS (mcp_account_manager_ring_parent_class)->dispose (object);
}

static void
mcp_account_manager_ring_init (McpAccountManagerRing *self)
{
  g_debug ("MC Ring account plugin initialized");

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MCP_TYPE_ACCOUNT_MANAGER_RING,
      McpAccountManagerRingPrivate);

  self->priv->modems = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) g_hash_table_unref);
  self->priv->pending_signals = g_queue_new ();

  self->priv->cancellable = g_cancellable_new ();
  g_bus_get (G_BUS_TYPE_SYSTEM, self->priv->cancellable,
      got_system_bus_cb, self);
}

static void
mcp_account_manager_ring_class_init (McpAccountManagerRingClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = mcp_account_manager_ring_dispose;

  g_type_class_add_private (gobject_class,
      sizeof (McpAccountManagerRingPrivate));
}

static GList *
account_manager_ring_list (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  McpAccountManagerRing *self = (McpAccountManagerRing*) storage;
  GList *accounts = NULL;
  GHashTableIter iter;
  gpointer key;

  g_debug ("%s", G_STRFUNC);

  g_hash_table_iter_init (&iter, self->priv->modems);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    accounts = g_list_prepend (accounts, g_strdup (key));

  return accounts;
}

static gboolean
account_manager_ring_get (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key)
{
  McpAccountManagerRing *self = (McpAccountManagerRing*) storage;
  GHashTable *params;

  params = g_hash_table_lookup (self->priv->modems, account_name);
  if (params == NULL)
    return FALSE;

  if (key == NULL)
    {
      GHashTableIter iter;
      gpointer itkey, value;

      g_hash_table_iter_init (&iter, params);
      while (g_hash_table_iter_next (&iter, &itkey, &value))
        {
          g_debug  ("%s: %s, %s %s", G_STRFUNC, account_name,
              (gchar *) itkey, (gchar *) value);
          mcp_account_manager_set_value (am, account_name, itkey, value);
        }
    }
  else
    {
      const gchar *value = g_hash_table_lookup (params, key);

      g_debug ("%s: %s, %s %s", G_STRFUNC, account_name,
          (gchar *) key, (gchar *) value);
      mcp_account_manager_set_value (am, account_name, key, value);
    }

  return TRUE;
}

static void
account_manager_ring_ready (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  McpAccountManagerRing *self = (McpAccountManagerRing *) storage;
  DelayedSignalData *data;

  if (self->priv->ready)
    return;

  g_debug ("%s", G_STRFUNC);

  self->priv->ready = TRUE;
  self->priv->am = g_object_ref (G_OBJECT (am));

  while ((data = g_queue_pop_head (self->priv->pending_signals)) != NULL)
    {
      switch (data->signal)
        {
          case DELAYED_CREATE:
            add_modem (self, data->path);
            break;
          case DELAYED_DELETE:
            remove_modem (self, data->path);
            break;
          default:
            g_assert_not_reached ();
        }

      g_free (data->path);
      g_slice_free (DelayedSignalData, data);
    }

  g_queue_free (self->priv->pending_signals);
  self->priv->pending_signals = NULL;
}

static guint
account_manager_ring_get_restrictions (const McpAccountStorage *storage,
    const gchar *account_name)
{
  return TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_PARAMETERS |
      TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_ENABLED |
      TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_PRESENCE |
      TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_SERVICE;
}

static void
account_storage_iface_init (McpAccountStorageIface *iface)
{
  iface->name = PLUGIN_NAME;
  iface->desc = PLUGIN_DESCRIPTION;
  iface->priority = PLUGIN_PRIORITY;
  iface->provider = PLUGIN_PROVIDER;

  iface->get = account_manager_ring_get;
  iface->list = account_manager_ring_list;
  iface->ready = account_manager_ring_ready;
  iface->get_restrictions = account_manager_ring_get_restrictions;
}

McpAccountManagerRing *
mcp_account_manager_ring_new (void)
{
  return g_object_new (MCP_TYPE_ACCOUNT_MANAGER_RING, NULL);
}
