#include "js.h"
#include "setup.h"
#include "util.h"
#include "uzbl-core.h"

#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <string.h>

struct _UzblEM {
    JSGlobalContextRef js_ctx;

    GMainContext *em_ctx;
    GMainLoop    *em_loop;
    GThread      *em_thread;
};

/* =========================== PUBLIC API =========================== */

static void
init_js_em_api (JSGlobalContextRef context, JSObjectRef obj);
static void
em_load_initial_script (const gchar *script);
static gboolean
control_em (GIOChannel *gio, GIOCondition condition, gpointer data);
static gpointer
run_em (gpointer data);

GIOChannel *
uzbl_em_init (const gchar *path)
{
    if (uzbl.em) {
        return NULL;
    }

    const gchar *script = path;
    path = strchr (path, '/');
    if (!path) {
        return NULL;
    }

    int sockfd[2];
    if (socketpair (AF_UNIX, SOCK_STREAM, 0, sockfd) < 0) {
        return NULL;
    }

    struct sockaddr_un local;
    local.sun_family = AF_UNIX;
    strncpy (local.sun_path, path + strlen (UZBL_EM_PREFIX), 108);

    GIOChannel *chan = NULL;
    if (!connect (sockfd[0], (struct sockaddr *)&local, sizeof (local))) {
        chan = g_io_channel_unix_new (sockfd[0]);
    } else {
        close (sockfd[0]);
        close (sockfd[1]);
        return NULL;
    }
    GIOChannel *local_chan = NULL;
    if (!connect (sockfd[1], (struct sockaddr *)&local, sizeof (local))) {
        local_chan = g_io_channel_unix_new (sockfd[1]);
    } else {
        g_io_channel_unref (chan);
        close (sockfd[1]);
        return NULL;
    }

    uzbl.em = g_malloc (sizeof (UzblEM));

    JSContextGroupRef group = JSContextGetGroup (uzbl.state.jscontext);
    uzbl.em->js_ctx = JSGlobalContextCreateInGroup (group, NULL);

    JSObjectRef uzbl_global = JSContextGetGlobalObject (uzbl.state.jscontext);
    JSObjectRef em_global = JSContextGetGlobalObject (uzbl.em->js_ctx);

    uzbl_js_set (uzbl.em->js_ctx,
        em_global, "uzbl", uzbl_global,
        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete);

    init_js_em_api (uzbl.em->js_ctx, em_global);

    em_load_initial_script (script);

    uzbl.em->em_ctx = g_main_context_new ();
    GSource *source = g_io_create_watch (local_chan, G_IO_IN | G_IO_HUP);
    g_source_set_name (source, "Uzbl event manager listener");
    g_source_set_callback (source, (GSourceFunc)control_em, NULL, NULL);
    g_source_attach (source, uzbl.em->em_ctx);
    g_source_unref (source);
    g_io_channel_unref (local_chan);

    uzbl.em->em_thread = g_thread_new ("uzbl-em", run_em, NULL);

    return chan;
}

void
uzbl_em_free ()
{
    if (!uzbl.em) {
        return;
    }

    JSGlobalContextRelease (uzbl.em->js_ctx);
    g_thread_unref (uzbl.em->em_thread);

    g_free (uzbl.em);
    uzbl.em = NULL;
}

/* ===================== HELPER IMPLEMENTATIONS ===================== */

typedef struct {
    const gchar *name;
    const gchar *class_name;
    JSObjectCallAsFunctionCallback callback;
} UzblEMAPI;

static const UzblEMAPI
builtin_em_api[];

void
init_js_em_api (JSGlobalContextRef context, JSObjectRef obj)
{
    JSObjectRef em_obj = JSObjectMake (context, NULL, NULL);

    const UzblEMAPI *api = builtin_em_api;
    while (api->name) {
        const JSClassDefinition
        api_class_def = {
            0,                     // version
            kJSClassAttributeNone, // attributes
            api->class_name,       // class name
            NULL,                  // parent class
            NULL,                  // static values
            NULL,                  // static functions
            NULL,                  // initialize
            NULL,                  // finalize
            NULL,                  // has property
            NULL,                  // get property
            NULL,                  // set property
            NULL,                  // delete property
            NULL,                  // get property names
            api->callback,         // call as function
            NULL,                  // call as contructor
            NULL,                  // has instance
            NULL                   // convert to type
        };

        JSClassRef api_class = JSClassCreate (&api_class_def);
        JSObjectRef api_obj = JSObjectMake (context, api_class, NULL);
        JSClassRelease (api_class);

        JSStringRef name = JSStringCreateWithUTF8CString (api->name);
        JSValueRef name_val = JSValueMakeString(context, name);

        uzbl_js_set (context,
            api_obj, "name", name_val,
            kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete);
        uzbl_js_set (context,
            api_obj, api->name, api_obj,
            kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete);

        ++api;
    }

    uzbl_js_set (context,
        obj, "em", em_obj,
        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete);
}

static void
em_load_initial_script (const gchar *script)
{
    /* TODO: Implement. */
}

static gboolean
control_em (GIOChannel *gio, GIOCondition condition, gpointer data)
{
    UZBL_UNUSED (condition);
    UZBL_UNUSED (data);

    gchar *ctl_line = NULL;
    GIOStatus ret;

    ret = g_io_channel_read_line (gio, &ctl_line, NULL, NULL, NULL);
    if ((ret == G_IO_STATUS_ERROR) || (ret == G_IO_STATUS_EOF)) {
        return FALSE;
    }

    JSObjectRef input_call = uzbl_js_object (uzbl.em->js_ctx, "input");

    if (JSValueGetType (uzbl.em->js_ctx, input_call) == kJSTypeUndefined) {
        uzbl_debug ("failed to get entry point for internal EM");
        return FALSE;
    }

    JSStringRef input_str = JSStringCreateWithUTF8CString (ctl_line);
    JSValueRef input = JSValueMakeString (uzbl.em->js_ctx, input_str);
    JSValueRef exc;
    JSValueRef args[1] = { input };
    JSValueRef js_ret = JSObjectCallAsFunction (uzbl.em->js_ctx, input_call, NULL, 1, args, &exc);
    JSStringRelease (input_str);
    JSValueUnprotect (uzbl.em->js_ctx, input);

    if (!js_ret) {
        if (exc) {
            gchar *exc_text = uzbl_js_to_string (uzbl.em->js_ctx, exc);
            uzbl_debug ("entry point for internal EM threw an exception: %s", exc_text);
            g_free (exc_text);
            JSValueUnprotect (uzbl.em->js_ctx, exc);
        } else {
            uzbl_debug ("entry point for internal EM is not a function");
            return FALSE;
        }
    }

    return TRUE;
}

gpointer
run_em (gpointer data)
{
    UZBL_UNUSED (data);

    uzbl.em->em_loop = g_main_loop_new (uzbl.em->em_ctx, FALSE);
    g_main_loop_run (uzbl.em->em_loop);
    g_main_loop_unref (uzbl.em->em_loop);
    uzbl.em->em_loop = NULL;

    g_main_context_unref (uzbl.em->em_ctx);
    uzbl.em->em_ctx = NULL;

    return NULL;
}

#define DECLARE_API(name) \
    static JSValueRef     \
    em_##name (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);

DECLARE_API (load);
DECLARE_API (reply);

static const UzblEMAPI
builtin_em_api[] = {
    { "load",  "LoadEMAPI",  em_load  },
    { "reply", "ReplyEMAPI", em_reply },
    { NULL }
};

#define IMPLEMENT_API(name) \
    JSValueRef              \
    em_##name (JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)

IMPLEMENT_API (load)
{
    /* TODO: Implement. */
}

IMPLEMENT_API (reply)
{
    /* TODO: Implement. */
}
