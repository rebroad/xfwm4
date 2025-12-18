#include "virtual_display.h"

#include <gio/gio.h>
#include <stdlib.h>

#define VDM_DBUS_NAME  "org.xfce.Xfwm.VirtualDisplay"
#define VDM_DBUS_PATH  "/org/xfce/Xfwm/VirtualDisplayManager"
#define VDM_DBUS_IFACE "org.xfce.Xfwm.VirtualDisplayManager"

static GDBusNodeInfo   *vdm_introspection = NULL;
static GDBusConnection *vdm_connection    = NULL;
static guint            vdm_reg_id        = 0;
static guint            vdm_name_id       = 0;

static GHashTable      *vdm_displays      = NULL;
static guint            vdm_next_id       = 1;

static const gchar vdm_introspection_xml[] =
"<node>\n"
"  <interface name=\"org.xfce.Xfwm.VirtualDisplayManager\">\n"
"\n"
"    <!-- Create a virtual display and return its ID -->\n"
"    <method name=\"CreateVirtualDisplay\">\n"
"      <arg name=\"width\"   type=\"u\" direction=\"in\"/>\n"
"      <arg name=\"height\"  type=\"u\" direction=\"in\"/>\n"
"      <arg name=\"refresh\" type=\"u\" direction=\"in\"/>\n"
"      <!-- e.g. \"desktop\", \"workspace:0\", \"monitor:HDMI-0\" -->\n"
"      <arg name=\"source\"  type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"id\"      type=\"u\" direction=\"out\"/>\n"
"    </method>\n"
"\n"
"    <!-- Destroy a previously created virtual display -->\n"
"    <method name=\"DestroyVirtualDisplay\">\n"
"      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
"    </method>\n"
"\n"
"    <!-- Get a handle that Breezy can capture from -->\n"
"    <method name=\"GetVirtualDisplaySurface\">\n"
"      <arg name=\"id\"          type=\"u\" direction=\"in\"/>\n"
"      <!-- For now: X11 window ID or pixmap ID as uint32 -->\n"
"      <arg name=\"surfaceId\"   type=\"u\" direction=\"out\"/>\n"
"      <!-- e.g. \"window\", \"pixmap\", \"shm\" -->\n"
"      <arg name=\"surfaceType\" type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"\n"
"    <!-- Change which desktop/workspace/monitor is mirrored -->\n"
"    <method name=\"SetVirtualDisplaySource\">\n"
"      <arg name=\"id\"     type=\"u\" direction=\"in\"/>\n"
"      <arg name=\"source\" type=\"s\" direction=\"in\"/>\n"
"    </method>\n"
"\n"
"    <!-- Lifecycle signals -->\n"
"    <signal name=\"VirtualDisplayCreated\">\n"
"      <arg name=\"id\" type=\"u\"/>\n"
"    </signal>\n"
"\n"
"    <signal name=\"VirtualDisplayDestroyed\">\n"
"      <arg name=\"id\" type=\"u\"/>\n"
"    </signal>\n"
"\n"
"  </interface>\n"
"</node>\n";

static void
virtual_display_free (VirtualDisplay *vd)
{
    if (!vd)
        return;
    g_free (vd->source);
    g_free (vd->surface_type);
    g_free (vd);
}

static void
vdm_handle_method_call (GDBusConnection       *connection,
                        const gchar           *sender,
                        const gchar           *object_path,
                        const gchar           *interface_name,
                        const gchar           *method_name,
                        GVariant              *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0 (method_name, "CreateVirtualDisplay") == 0)
    {
        guint width, height, refresh;
        const gchar *source;
        g_variant_get (parameters, "(uuus)", &width, &height, &refresh, &source);

        VirtualDisplay *vd = g_new0 (VirtualDisplay, 1);
        vd->id           = vdm_next_id++;
        vd->width        = width;
        vd->height       = height;
        vd->refresh      = refresh;
        vd->source       = g_strdup (source);
        vd->surface_id   = 0; /* Will be wired to a real surface later */
        vd->surface_type = g_strdup ("none");

        g_hash_table_insert (vdm_displays, GUINT_TO_POINTER (vd->id), vd);

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", vd->id));

        g_dbus_connection_emit_signal (connection,
                                       NULL,
                                       VDM_DBUS_PATH,
                                       VDM_DBUS_IFACE,
                                       "VirtualDisplayCreated",
                                       g_variant_new ("(u)", vd->id),
                                       NULL);
    }
    else if (g_strcmp0 (method_name, "DestroyVirtualDisplay") == 0)
    {
        guint id;
        g_variant_get (parameters, "(u)", &id);

        if (g_hash_table_remove (vdm_displays, GUINT_TO_POINTER (id)))
        {
            g_dbus_method_invocation_return_value (invocation, NULL);
            g_dbus_connection_emit_signal (connection,
                                           NULL,
                                           VDM_DBUS_PATH,
                                           VDM_DBUS_IFACE,
                                           "VirtualDisplayDestroyed",
                                           g_variant_new ("(u)", id),
                                           NULL);
        }
        else
        {
            g_dbus_method_invocation_return_dbus_error (invocation,
                "org.xfce.Xfwm.VirtualDisplay.Error.NotFound",
                "Virtual display not found");
        }
    }
    else if (g_strcmp0 (method_name, "GetVirtualDisplaySurface") == 0)
    {
        guint id;
        g_variant_get (parameters, "(u)", &id);

        VirtualDisplay *vd = g_hash_table_lookup (vdm_displays, GUINT_TO_POINTER (id));
        if (!vd)
        {
            g_dbus_method_invocation_return_dbus_error (invocation,
                "org.xfce.Xfwm.VirtualDisplay.Error.NotFound",
                "Virtual display not found");
            return;
        }

        g_dbus_method_invocation_return_value (invocation,
            g_variant_new ("(us)", vd->surface_id, vd->surface_type ? vd->surface_type : "none"));
    }
    else if (g_strcmp0 (method_name, "SetVirtualDisplaySource") == 0)
    {
        guint id;
        const gchar *source;
        g_variant_get (parameters, "(us)", &id, &source);

        VirtualDisplay *vd = g_hash_table_lookup (vdm_displays, GUINT_TO_POINTER (id));
        if (!vd)
        {
            g_dbus_method_invocation_return_dbus_error (invocation,
                "org.xfce.Xfwm.VirtualDisplay.Error.NotFound",
                "Virtual display not found");
            return;
        }

        g_free (vd->source);
        vd->source = g_strdup (source);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else
    {
        g_dbus_method_invocation_return_dbus_error (invocation,
            "org.freedesktop.DBus.Error.NotSupported",
            "Unknown method");
    }
}

static const GDBusInterfaceVTable vdm_vtable = {
    vdm_handle_method_call,
    NULL,
    NULL
};

static void
vdm_on_bus_acquired (GDBusConnection *connection,
                     const gchar     *name,
                     gpointer         user_data)
{
    (void)name;
    (void)user_data;

    vdm_connection = g_object_ref (connection);

    guint id = g_dbus_connection_register_object (connection,
                                                  VDM_DBUS_PATH,
                                                  vdm_introspection->interfaces[0],
                                                  &vdm_vtable,
                                                  NULL,
                                                  NULL,
                                                  NULL);
    if (id == 0)
    {
        g_warning ("Failed to register VirtualDisplayManager D-Bus object");
    }
    else
    {
        vdm_reg_id = id;
    }
}

static void
vdm_on_name_acquired (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
    (void)connection;
    (void)user_data;
    g_message ("VirtualDisplayManager acquired D-Bus name %s", name);
}

static void
vdm_on_name_lost (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
    (void)connection;
    (void)user_data;
    g_warning ("VirtualDisplayManager lost D-Bus name %s", name);
}

void
virtual_display_manager_init (void)
{
    if (vdm_introspection != NULL)
        return;

    vdm_introspection = g_dbus_node_info_new_for_xml (vdm_introspection_xml, NULL);
    if (!vdm_introspection)
    {
        g_warning ("Failed to parse VirtualDisplayManager introspection data");
        return;
    }

    vdm_displays = g_hash_table_new_full (g_direct_hash,
                                          g_direct_equal,
                                          NULL,
                                          (GDestroyNotify)virtual_display_free);

    vdm_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  VDM_DBUS_NAME,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  vdm_on_bus_acquired,
                                  vdm_on_name_acquired,
                                  vdm_on_name_lost,
                                  NULL,
                                  NULL);
}

void
virtual_display_manager_shutdown (void)
{
    if (vdm_name_id != 0)
    {
        g_bus_unown_name (vdm_name_id);
        vdm_name_id = 0;
    }

    if (vdm_connection && vdm_reg_id != 0)
    {
        g_dbus_connection_unregister_object (vdm_connection, vdm_reg_id);
        vdm_reg_id = 0;
    }

    if (vdm_connection)
    {
        g_object_unref (vdm_connection);
        vdm_connection = NULL;
    }

    if (vdm_introspection)
    {
        g_dbus_node_info_unref (vdm_introspection);
        vdm_introspection = NULL;
    }

    if (vdm_displays)
    {
        g_hash_table_destroy (vdm_displays);
        vdm_displays = NULL;
    }
}


