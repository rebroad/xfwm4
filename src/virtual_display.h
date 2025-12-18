#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include <glib.h>

typedef struct _VirtualDisplay VirtualDisplay;

struct _VirtualDisplay
{
    guint id;
    guint width;
    guint height;
    guint refresh;
    gchar *source; /* e.g. "desktop", "workspace:0", "monitor:HDMI-0" */
    guint surface_id;   /* XID: window or pixmap, 0 for now */
    gchar *surface_type; /* "window", "pixmap", "shm" etc. */
};

void virtual_display_manager_init (void);
void virtual_display_manager_shutdown (void);

#endif /* VIRTUAL_DISPLAY_H */


