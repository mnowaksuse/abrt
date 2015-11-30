/*
  Copyright (C) 2015  ABRT team

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "abrt_problems2_session_node.h"
#include "abrt_problems2_service.h"

#include "libabrt.h"

#include <assert.h>

static PolkitAuthority *s_pk_authority;

PolkitAuthority *abrt_p2_session_class_set_polkit_authority(PolkitAuthority *pk_authority)
{
    if (s_pk_authority != NULL)
    {
        log_warning("Session: polkit Authority already initialized");

        /*
         * Introduce something like this to libreport
        if (g_verbose > 3)
            abort();
        */
        return s_pk_authority;
    }

    s_pk_authority = pk_authority;

    return s_pk_authority;
}

PolkitAuthority *abrt_p2_session_class_polkit_authority(void)
{
    if (s_pk_authority == NULL)
    {
        log_debug("Session: Polkit Authority not-yet initialized");
    }

    return s_pk_authority;
}

PolkitAuthority *abrt_p2_session_class_release_polkit_authority(void)
{
    PolkitAuthority *pk_authority = s_pk_authority;
    s_pk_authority = NULL;
    return pk_authority;
}

typedef struct
{
    char   *p2s_caller;
    uid_t   p2s_uid;
    int     p2s_state;
    time_t  p2s_stamp;
    struct check_auth_cb_params *p2s_auth_rq;
} AbrtP2SessionPrivate;

enum
{
    ABRT_P2_SESSION_STATE_INIT,
    ABRT_P2_SESSION_STATE_PENDING,
    ABRT_P2_SESSION_STATE_AUTH,
};

struct _AbrtP2Session
{
    GObject parent_instance;
    AbrtP2SessionPrivate *pv;

   void (*authorization_changed)(AbrtP2Session *session, gint32 status);
};

G_DEFINE_TYPE_WITH_PRIVATE(AbrtP2Session, abrt_p2_session, G_TYPE_OBJECT)

struct check_auth_cb_params
{
    AbrtP2Session *session;
    GDBusConnection *connection;
    GCancellable *cancellable;
};

enum {
    SN_AUTHORIZATION_CHANGED,
    SN_LAST_SIGNAL
} SignalNumber;

static guint s_signals[SN_LAST_SIGNAL] = { 0 };

static void abrt_p2_session_finalize(GObject *gobject)
{
    AbrtP2SessionPrivate *pv = abrt_p2_session_get_instance_private(ABRT_P2_SESSION(gobject));
    free(pv->p2s_caller);
}

static void abrt_p2_session_class_init(AbrtP2SessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = abrt_p2_session_finalize;

    s_signals[SN_AUTHORIZATION_CHANGED] = g_signal_new ("authorization-changed",
                             G_TYPE_FROM_CLASS (klass),
                             G_SIGNAL_RUN_LAST,
                             G_STRUCT_OFFSET(struct _AbrtP2Session, authorization_changed),
                             /*accumulator*/NULL, /*accu_data*/NULL,
                             g_cclosure_marshal_VOID__VOID,
                             G_TYPE_NONE,
                             /*n_params*/1,
                             G_TYPE_INT);
}

static void abrt_p2_session_init(AbrtP2Session *self)
{
    self->pv = abrt_p2_session_get_instance_private(self);
}

static void emit_authorization_changed(AbrtP2Session *session, gint32 status)
{
    g_signal_emit(session, s_signals[SN_AUTHORIZATION_CHANGED], 0, status);
}

static void change_state(AbrtP2Session *session, int new_state)
{
    if (session->pv->p2s_state == new_state)
        return;

    int value = -1;
    int old_state = session->pv->p2s_state;
    session->pv->p2s_state = new_state;

    if      (old_state == ABRT_P2_SESSION_STATE_INIT    && new_state == ABRT_P2_SESSION_STATE_PENDING)
    {
        log_debug("Authorization request is pending");
        value = 1;
    }
    else if (old_state == ABRT_P2_SESSION_STATE_PENDING && new_state == ABRT_P2_SESSION_STATE_AUTH)
    {
        log_debug("Authorization has been acquired");
        value = 0;
    }
    else if (old_state == ABRT_P2_SESSION_STATE_AUTH    && new_state == ABRT_P2_SESSION_STATE_INIT)
    {
        log_debug("Authorization request has been lost");
        value = 2;
    }
    else if (old_state == ABRT_P2_SESSION_STATE_PENDING && new_state == ABRT_P2_SESSION_STATE_INIT)
    {
        log_debug("Authorization request has failed");
        value = 3;
    }
    else
        goto forgotten_state;

    emit_authorization_changed(session, value);
    return;

forgotten_state:
    error_msg("BUG: unsupported state, current : %d, new : %d", session->pv->p2s_state, new_state);
}

static void authorization_request_destroy(AbrtP2Session *session)
{
    g_object_unref(session->pv->p2s_auth_rq->cancellable);
    session->pv->p2s_auth_rq->cancellable = (void *)0xDEADBEEF;

    free(session->pv->p2s_auth_rq);
    session->pv->p2s_auth_rq = NULL;
}

static void check_authorization_callback(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    PolkitAuthorizationResult *result = NULL;
    result = polkit_authority_check_authorization_finish(POLKIT_AUTHORITY(source), res, &error);

    int new_state = ABRT_P2_SESSION_STATE_INIT;
    if (result == NULL)
    {
       error_msg("Polkit authorization failed: %s", error->message);
       g_error_free(error);
    }
    else if (polkit_authorization_result_get_is_authorized(result))
        new_state = ABRT_P2_SESSION_STATE_AUTH;
    else
        log_debug("Not authorized");

    g_object_unref(result);

    struct check_auth_cb_params *params = (struct check_auth_cb_params *)user_data;
    change_state(params->session, new_state);

    /* Invalidates args/params !!! */
    authorization_request_destroy(params->session);
}

static void authorization_request_initialize(AbrtP2Session *session, GVariant *parameters)
{
    struct check_auth_cb_params *auth_rq = xmalloc(sizeof(*auth_rq));
    auth_rq->session = session;
    auth_rq->cancellable = g_cancellable_new();
    session->pv->p2s_auth_rq = auth_rq;
    change_state(session, ABRT_P2_SESSION_STATE_PENDING);

    /* http://www.freedesktop.org/software/polkit/docs/latest/polkit-apps.html
     */
    PolkitSubject *subject = polkit_system_bus_name_new(session->pv->p2s_caller);
    PolkitDetails *details = NULL;
    if (parameters != NULL)
    {
        GVariant *message = g_variant_lookup_value(parameters, "message", G_VARIANT_TYPE_STRING);
        if (message != NULL)
        {
            details = polkit_details_new();
            polkit_details_insert(details, "polkit.message", g_variant_get_string(message, NULL));
        }
    }

    polkit_authority_check_authorization(abrt_p2_session_class_polkit_authority(),
                subject,
                "org.freedesktop.problems.getall",
                details,
                POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                auth_rq->cancellable,
                check_authorization_callback,
                auth_rq);

}

AbrtP2Session *abrt_p2_session_new(char *caller, uid_t uid)
{
    AbrtP2Session *node = g_object_new(TYPE_ABRT_P2_SESSION, NULL);
    node->pv->p2s_caller = caller;
    node->pv->p2s_uid = uid;

    if (node->pv->p2s_uid == 0)
        node->pv->p2s_state = ABRT_P2_SESSION_STATE_AUTH;
    else
        node->pv->p2s_state = ABRT_P2_SESSION_STATE_INIT;

    node->pv->p2s_stamp = time(NULL);

    return node;
}

gint32 abrt_p2_session_authorize(AbrtP2Session *session, GVariant *parameters)
{
    switch(session->pv->p2s_state)
    {
        case ABRT_P2_SESSION_STATE_INIT:
            authorization_request_initialize(session, parameters);
            return 1;

        case ABRT_P2_SESSION_STATE_PENDING:
            return 2;

        case ABRT_P2_SESSION_STATE_AUTH:
            return 0;

        default:
            error_msg("BUG: %s: forgotten state -> %d", __func__, session->pv->p2s_state);
            return -1;
    }

}

void abrt_p2_session_close(AbrtP2Session *session)
{
    switch(session->pv->p2s_state)
    {
        case ABRT_P2_SESSION_STATE_AUTH:
            change_state(session, ABRT_P2_SESSION_STATE_INIT);
            break;

        case ABRT_P2_SESSION_STATE_PENDING:
            {
                g_cancellable_cancel(session->pv->p2s_auth_rq->cancellable);
                authorization_request_destroy(session);
                change_state(session, ABRT_P2_SESSION_STATE_INIT);
            }
            break;

        case ABRT_P2_SESSION_STATE_INIT:
            /* pass */
            break;
    }
}

uid_t abrt_p2_session_uid(AbrtP2Session *session)
{
    return session->pv->p2s_uid;
}

const char *abrt_p2_session_caller(AbrtP2Session *session)
{
    return session->pv->p2s_caller;
}

int abrt_p2_session_is_authorized(AbrtP2Session *session)
{
    return session->pv->p2s_state == ABRT_P2_SESSION_STATE_AUTH;
}

int abrt_p2_session_check_sanity(AbrtP2Session *session,
            const char *caller,
            uid_t caller_uid,
            GError **error)
{
    if (strcmp(session->pv->p2s_caller, caller) == 0 && session->pv->p2s_uid == caller_uid)
        /* the session node is sane */
        return 0;

    log_warning("Problems2 Session object does not belong to UID %d", caller_uid);

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
            "Your Problems2 Session is broken. Check system logs for more details.");
    return -1;
}
