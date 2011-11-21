/*
 * gnome-keyring
 *
 * Copyright (C) 2011 Stefan Walter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Stef Walter <stef@thewalter.net>
 */

#include "config.h"

#include "gcr-dbus-constants.h"
#include "gcr-dbus-generated.h"
#define DEBUG_FLAG GCR_DEBUG_PROMPT
#include "gcr-debug.h"
#include "gcr-enum-types-base.h"
#include "gcr-internal.h"
#include "gcr-library.h"
#include "gcr-prompt.h"
#include "gcr-secret-exchange.h"
#include "gcr-system-prompter.h"
#include "gcr-system-prompt.h"

#include "egg/egg-error.h"

#include <string.h>

/**
 * SECTION:gcr-system-prompter
 * @title: GcrSystemPrompter
 * @short_description: XXX
 *
 * XXXX
 */

/**
 * GcrSystemPrompter:
 *
 * XXX
 */

/**
 * GcrSystemPrompterClass:
 *
 * The class for #GcrSystemPrompter.
 */

enum {
	PROP_0,
	PROP_MODE,
	PROP_PROMPT_TYPE
};

struct _GcrSystemPrompterPrivate {
	GcrSystemPrompterMode mode;
	GType prompt_type;

	guint prompter_registered;
	GDBusConnection *connection;

	GHashTable *pending;          /* callback path (string) -> sender (string) */
	GHashTable *active;           /* callback path (string) -> active (ActivePrompt) */
};

G_DEFINE_TYPE (GcrSystemPrompter, gcr_system_prompter, G_TYPE_OBJECT);

typedef struct {
	gint refs;
	gchar *callback_path;
	gchar *callback_name;
	GcrSystemPrompter *prompter;
	GCancellable *cancellable;
	GcrPrompt *prompt;
	gboolean ready;
	guint notify_sig;
	GHashTable *changed;
	GcrSecretExchange *exchange;
	gboolean received;
} ActivePrompt;

static void    prompt_send_ready               (ActivePrompt *active,
                                                const gchar *response,
                                                const gchar *secret);

static void    prompt_possibly_ready           (GcrSystemPrompter *self,
                                                const gchar *callback);

static void    prompt_stop_prompting           (ActivePrompt *active,
                                                gboolean send_done_message,
                                                gboolean wait_for_reply);

static ActivePrompt *
active_prompt_ref (ActivePrompt *active)
{
	g_atomic_int_inc (&active->refs);
	return active;
}

static void
on_prompt_notify (GObject *object,
                  GParamSpec *param,
                  gpointer user_data)
{
	ActivePrompt *active = user_data;
	gpointer key = (gpointer)g_intern_string (param->name);
	g_hash_table_replace (active->changed, key, key);
}

static void
active_prompt_unref (gpointer data)
{
	ActivePrompt *active = data;

	if (g_atomic_int_dec_and_test (&active->refs)) {
		g_free (active->callback_path);
		g_free (active->callback_name);
		g_object_unref (active->prompter);
		g_object_unref (active->cancellable);
		g_signal_handlers_disconnect_by_func (active->prompt, on_prompt_notify, active);
		g_object_unref (active->prompt);
		g_hash_table_destroy (active->changed);
		if (active->exchange)
			g_object_unref (active->exchange);
		g_slice_free (ActivePrompt, active);
	}
}

static GcrSecretExchange *
active_prompt_get_secret_exchange (ActivePrompt *active)
{
	if (active->exchange == NULL)
		active->exchange = gcr_secret_exchange_new (NULL);
	return active->exchange;
}

static ActivePrompt *
active_prompt_create (GcrSystemPrompter *self,
                      const gchar *callback)
{
	ActivePrompt *active;

	active = g_slice_new0 (ActivePrompt);
	if (!g_hash_table_lookup_extended (self->pv->pending, callback,
	                                   (gpointer *)&active->callback_path,
	                                   (gpointer *)&active->callback_name))
		g_return_val_if_reached (NULL);
	if (!g_hash_table_steal (self->pv->pending, callback))
		g_return_val_if_reached (NULL);

	active->refs = 1;
	active->prompter = g_object_ref (self);
	active->cancellable = g_cancellable_new ();
	active->prompt = g_object_new (self->pv->prompt_type, NULL);
	active->notify_sig = g_signal_connect (active->prompt, "notify", G_CALLBACK (on_prompt_notify), active);
	active->changed = g_hash_table_new (g_direct_hash, g_direct_equal);

	/* Insert us into the active hash table */
	g_hash_table_replace (self->pv->active, active->callback_path, active);
	return active;
}

static void
gcr_system_prompter_init (GcrSystemPrompter *self)
{
	self->pv = G_TYPE_INSTANCE_GET_PRIVATE (self, GCR_TYPE_SYSTEM_PROMPTER,
	                                        GcrSystemPrompterPrivate);
	self->pv->pending = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	self->pv->active = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, active_prompt_unref);
}

static void
gcr_system_prompter_set_property (GObject *obj,
                                  guint prop_id,
                                  const GValue *value,
                                  GParamSpec *pspec)
{
	GcrSystemPrompter *self = GCR_SYSTEM_PROMPTER (obj);

	switch (prop_id) {
	case PROP_MODE:
		self->pv->mode = g_value_get_enum (value);
		break;
	case PROP_PROMPT_TYPE:
		self->pv->prompt_type = g_value_get_gtype (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
gcr_system_prompter_get_property (GObject *obj,
                                  guint prop_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
	GcrSystemPrompter *self = GCR_SYSTEM_PROMPTER (obj);

	switch (prop_id) {
	case PROP_MODE:
		g_value_set_enum (value, gcr_system_prompter_get_mode (self));
		break;
	case PROP_PROMPT_TYPE:
		g_value_set_gtype (value, gcr_system_prompter_get_prompt_type (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
gcr_system_prompter_dispose (GObject *obj)
{
	GcrSystemPrompter *self = GCR_SYSTEM_PROMPTER (obj);

	_gcr_debug ("disposing prompter");

	if (self->pv->prompter_registered)
		gcr_system_prompter_unregister (self, FALSE);

	g_hash_table_remove_all (self->pv->active);
	g_hash_table_remove_all (self->pv->pending);

	G_OBJECT_CLASS (gcr_system_prompter_parent_class)->dispose (obj);
}

static void
gcr_system_prompter_finalize (GObject *obj)
{
	GcrSystemPrompter *self = GCR_SYSTEM_PROMPTER (obj);

	_gcr_debug ("finalizing prompter");

	g_assert (self->pv->connection == NULL);
	g_assert (self->pv->prompter_registered == 0);

	g_hash_table_destroy (self->pv->active);
	g_hash_table_destroy (self->pv->pending);

	G_OBJECT_CLASS (gcr_system_prompter_parent_class)->finalize (obj);
}

static void
gcr_system_prompter_class_init (GcrSystemPrompterClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->get_property = gcr_system_prompter_get_property;
	gobject_class->set_property = gcr_system_prompter_set_property;
	gobject_class->dispose = gcr_system_prompter_dispose;
	gobject_class->finalize = gcr_system_prompter_finalize;

	g_type_class_add_private (gobject_class, sizeof (GcrSystemPrompterPrivate));

	g_object_class_install_property (gobject_class, PROP_MODE,
	              g_param_spec_enum ("mode", "Mode", "Prompting mode",
	                                 GCR_TYPE_SYSTEM_PROMPTER_MODE, GCR_SYSTEM_PROMPTER_SINGLE,
	                                 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_PROMPT_TYPE,
	             g_param_spec_gtype ("prompt-type", "Prompt GType", "GObject type of prompts",
	                                 GCR_TYPE_PROMPT,
	                                 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static GVariantBuilder *
prompt_build_properties (GcrPrompt *prompt,
                         GHashTable *changed)
{
	GObject *obj = G_OBJECT (prompt);
	GVariantBuilder *builder;
	const gchar *property_name;
	GParamSpec *pspec;
	GHashTableIter iter;
	const GVariantType *type;
	GVariant *variant;
	GValue value;

	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_hash_table_iter_init (&iter, changed);
	while (g_hash_table_iter_next (&iter, (gpointer *)&property_name, NULL)) {

		/* Make sure this property is on the prompt interface */
		pspec = g_object_interface_find_property (GCR_PROMPT_GET_INTERFACE (obj),
		                                          property_name);
		if (pspec == NULL)
			continue;

		memset (&value, 0, sizeof (GValue));
		g_value_init (&value, pspec->value_type);
		g_object_get_property (obj, property_name, &value);

		switch (pspec->value_type) {
		case G_TYPE_STRING:
			type = G_VARIANT_TYPE ("s");
			break;
		case G_TYPE_INT:
			type = G_VARIANT_TYPE ("i");
			break;
		case G_TYPE_BOOLEAN:
			type = G_VARIANT_TYPE ("b");
			break;
		default:
			g_critical ("encountered unsupported property type on GcrPrompt: %s",
			            g_type_name (pspec->value_type));
			continue;
		}

		variant = g_dbus_gvalue_to_gvariant (&value, type);
		g_variant_builder_add (builder, "{sv}", property_name,
		                       g_variant_new_variant (variant));
		g_value_unset (&value);
		g_variant_unref (variant);
	}
	g_hash_table_remove_all (changed);
	return builder;
}

static void
prompt_stop_prompting (ActivePrompt *active,
                       gboolean send_done_message,
                       gboolean wait_for_reply)
{
	GcrSystemPrompter *self = g_object_ref (active->prompter);
	GVariant *retval;

	if (!active->ready)
		g_cancellable_cancel (active->cancellable);

	if (send_done_message && wait_for_reply) {
		retval = g_dbus_connection_call_sync (self->pv->connection,
		                                      active->callback_name,
		                                      active->callback_path,
		                                      GCR_DBUS_CALLBACK_INTERFACE,
		                                      GCR_DBUS_CALLBACK_METHOD_DONE,
		                                      g_variant_new ("()"),
		                                      G_VARIANT_TYPE ("()"),
		                                      G_DBUS_CALL_FLAGS_NO_AUTO_START,
		                                      -1, NULL, NULL);
		if (retval)
			g_variant_unref (retval);
	} else if (send_done_message) {
		g_dbus_connection_call (self->pv->connection,
		                        active->callback_name,
		                        active->callback_path,
		                        GCR_DBUS_CALLBACK_INTERFACE,
		                        GCR_DBUS_CALLBACK_METHOD_DONE,
		                        g_variant_new ("()"),
		                        G_VARIANT_TYPE ("()"),
		                        G_DBUS_CALL_FLAGS_NO_AUTO_START,
		                        -1, NULL, NULL, NULL);
	}

	g_object_run_dispose (G_OBJECT (active->prompt));
	if (!g_hash_table_remove (self->pv->active, active->callback_path))
		g_assert_not_reached ();

	g_object_unref (self);
}

static void
on_prompt_ready_complete (GObject *source,
                          GAsyncResult *result,
                          gpointer user_data)
{
	ActivePrompt *active = user_data;
	GcrSystemPrompter *self = g_object_ref (active->prompter);
	GError *error = NULL;
	GVariant *retval;

	active->ready = TRUE;
	retval = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);

	/* The ready call failed,  */
	if (error != NULL) {
		if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
			_gcr_debug ("prompt disappeared or does not exist: %s: %s",
			            active->callback_name, active->callback_path);
		else
			g_message ("received an error from the prompt callback: %s", error->message);
		g_error_free (error);
		prompt_stop_prompting (active, FALSE, FALSE);

		/* Another new prompt may be ready to go active? */
		prompt_possibly_ready (self, NULL);
	}

	if (retval != NULL)
		g_variant_unref (retval);

	active_prompt_unref (active);
	g_object_unref (self);
}

static void
prompt_send_ready (ActivePrompt *active,
                   const gchar *response,
                   const gchar *secret)
{
	GcrSystemPrompter *self;
	GVariantBuilder *builder;
	GcrSecretExchange *exchange;
	gchar *sent;

	exchange = active_prompt_get_secret_exchange (active);
	if (!active->received) {
		g_return_if_fail (secret == NULL);
		sent = gcr_secret_exchange_begin (exchange);
	} else {
		sent = gcr_secret_exchange_send (exchange, secret, -1);
	}

	self = active->prompter;
	builder = prompt_build_properties (active->prompt, active->changed);

	g_dbus_connection_call (self->pv->connection,
	                        active->callback_name,
	                        active->callback_path,
	                        GCR_DBUS_CALLBACK_INTERFACE,
	                        GCR_DBUS_CALLBACK_METHOD_READY,
	                        g_variant_new ("(sa{sv}s)", response, builder, sent),
	                        G_VARIANT_TYPE ("()"),
	                        G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                        -1, active->cancellable,
	                        on_prompt_ready_complete,
	                        active_prompt_ref (active));

	g_variant_builder_unref (builder);
	g_free (sent);
}

static void
prompt_possibly_ready (GcrSystemPrompter *self,
                       const gchar *callback)
{
	ActivePrompt *active;
	GHashTableIter iter;

	if (callback == NULL) {
		g_hash_table_iter_init (&iter, self->pv->pending);
		if (!g_hash_table_iter_next (&iter, (gpointer *)&callback, NULL))
			return;
		g_assert (callback != NULL);
	}

	active = g_hash_table_lookup (self->pv->active, callback);

	/* Only one prompt at a time, and only one active */
	if (active == NULL) {
		if (self->pv->mode == GCR_SYSTEM_PROMPTER_SINGLE &&
		    g_hash_table_size (self->pv->active) > 0)
			return;

		active = active_prompt_create (self, callback);
	}

	prompt_send_ready (active, GCR_DBUS_PROMPT_REPLY_YES, NULL);
}

static void
prompt_update_properties (GcrPrompt *prompt,
                          GVariantIter *iter)
{
	GObject *obj = G_OBJECT (prompt);
	gchar *property_name;
	GVariant *variant;
	GValue value;

	g_object_freeze_notify (obj);
	while (g_variant_iter_loop (iter, "{&sv}", &property_name, &variant)) {
		memset (&value, 0, sizeof (GValue));
		g_dbus_gvariant_to_gvalue (variant, &value);
		g_object_set_property (obj, property_name, &value);
		g_value_unset (&value);
	}
	g_object_thaw_notify (obj);
}

static GVariant *
prompter_get_property (GDBusConnection *connection,
                       const gchar *sender,
                       const gchar *object_path,
                       const gchar *interface_name,
                       const gchar *property_name,
                       GError **error,
                       gpointer user_data)
{
	g_return_val_if_reached (NULL);
}

static gboolean
prompter_set_property (GDBusConnection *connection,
                       const gchar *sender,
                       const gchar *object_path,
                       const gchar *interface_name,
                       const gchar *property_name,
                       GVariant *value,
                       GError **error,
                       gpointer user_data)
{
	g_return_val_if_reached (FALSE);
}

static void
prompter_method_begin_prompting (GcrSystemPrompter *self,
                                 GDBusMethodInvocation *invocation,
                                 GVariant *parameters)
{
	gchar *callback;
	const gchar *sender;

	g_variant_get (parameters, "(o)", &callback);

	if (g_hash_table_lookup (self->pv->pending, callback) ||
	    g_hash_table_lookup (self->pv->active, callback)) {
		g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
		                                               "Already begun prompting for this prompt callback");
		return;
	}

	sender = g_dbus_method_invocation_get_sender (invocation);
	g_hash_table_insert (self->pv->pending, g_strdup (callback), g_strdup (sender));

	g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
	prompt_possibly_ready (self, callback);
	g_free (callback);
}

static void
on_prompt_password (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	ActivePrompt *active = user_data;
	const gchar *reply;
	GError *error = NULL;
	const gchar *response;

	g_assert (active->ready == FALSE);

	reply = gcr_prompt_password_finish (GCR_PROMPT (source), result, &error);
	if (error != NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("prompting failed: %s", error->message);
		g_clear_error (&error);
	}

	if (reply == NULL)
		response = "no";
	else
		response = "yes";

	prompt_send_ready (active, response, reply);
	active_prompt_unref (active);
}

static void
on_prompt_confirm (GObject *source,
                   GAsyncResult *result,
                   gpointer user_data)
{
	ActivePrompt *active = user_data;
	GcrPromptReply reply;
	GError *error = NULL;
	const gchar *response;

	g_assert (active->ready == FALSE);

	reply = gcr_prompt_confirm_finish (GCR_PROMPT (source), result, &error);
	if (error != NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("prompting failed: %s", error->message);
		g_clear_error (&error);
	}

	switch (reply) {
	case GCR_PROMPT_REPLY_OK:
		response = GCR_DBUS_PROMPT_REPLY_YES;
		break;
	case GCR_PROMPT_REPLY_CANCEL:
		response = GCR_DBUS_PROMPT_REPLY_NO;
		break;
	}

	prompt_send_ready (active, response, NULL);
	active_prompt_unref (active);
}

static void
prompter_method_perform_prompt (GcrSystemPrompter *self,
                                GDBusMethodInvocation *invocation,
                                GVariant *parameters)
{
	GcrSecretExchange *exchange;
	GError *error = NULL;
	ActivePrompt *active;
	const gchar *callback;
	const gchar *type;
	GVariantIter *iter;
	const gchar *received;

	g_variant_get (parameters, "(&o&sa{sv}&s)",
	               &callback, &type, &iter, &received);

	active = g_hash_table_lookup (self->pv->active, callback);
	if (active == NULL) {
		error = g_error_new (G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
		                     "Not begun prompting for this prompt callback");

	} else if (!g_str_equal (active->callback_name, g_dbus_method_invocation_get_sender (invocation))) {
		error = g_error_new (G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
		                     "This prompt is not owned by this application");

	} else if (!active->ready) {
		error = g_error_new (G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
		                     "Already performing a prompt for this prompt callback");
	}

	if (error != NULL) {
		g_dbus_method_invocation_take_error (invocation, error);
		g_variant_iter_free (iter);
		return;
	}

	g_assert (active != NULL);
	prompt_update_properties (active->prompt, iter);
	g_variant_iter_free (iter);

	exchange = active_prompt_get_secret_exchange (active);
	if (!gcr_secret_exchange_receive (exchange, received)) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
		                                       "Invalid secret exchange received");
		return;
	}

	active->received = TRUE;

	if (g_strcmp0 (type, GCR_DBUS_PROMPT_TYPE_CONFIRM) == 0) {
		active->ready = FALSE;
		gcr_prompt_confirm_async (active->prompt, active->cancellable,
		                          on_prompt_confirm, active_prompt_ref (active));

	} else if (g_strcmp0 (type, GCR_DBUS_PROMPT_TYPE_PASSWORD) == 0) {
		active->ready = FALSE;
		gcr_prompt_password_async (active->prompt, active->cancellable,
		                           on_prompt_password, active_prompt_ref (active));

	} else {
		g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
		                                               "Invalid type argument");
		return;
	}

	g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
prompter_method_stop_prompting (GcrSystemPrompter *self,
                                GDBusMethodInvocation *invocation,
                                GVariant *parameters)
{
	const gchar *callback;
	ActivePrompt *active;

	g_variant_get (parameters, "(&o)", &callback);

	active = g_hash_table_lookup (self->pv->active, callback);
	if (active != NULL)
		prompt_stop_prompting (active, TRUE, FALSE);

	g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
	prompt_possibly_ready (self, NULL);
}

static void
prompter_method_call (GDBusConnection *connection,
                      const gchar *sender,
                      const gchar *object_path,
                      const gchar *interface_name,
                      const gchar *method_name,
                      GVariant *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer user_data)
{
	GcrSystemPrompter *self = GCR_SYSTEM_PROMPTER (user_data);

	g_return_if_fail (method_name != NULL);

	if (g_str_equal (method_name, GCR_DBUS_PROMPTER_METHOD_BEGIN)) {
		prompter_method_begin_prompting (self, invocation, parameters);

	} else if (g_str_equal (method_name, GCR_DBUS_PROMPTER_METHOD_PERFORM)) {
		prompter_method_perform_prompt (self, invocation, parameters);

	} else if (g_str_equal (method_name, GCR_DBUS_PROMPTER_METHOD_STOP)) {
		prompter_method_stop_prompting (self, invocation, parameters);

	} else {
		g_return_if_reached ();
	}
}

static GDBusInterfaceVTable prompter_dbus_vtable = {
	prompter_method_call,
	prompter_get_property,
	prompter_set_property,
};

void
gcr_system_prompter_register (GcrSystemPrompter *self,
                              GDBusConnection *connection)
{
	GError *error = NULL;

	g_return_if_fail (GCR_IS_SYSTEM_PROMPTER (self));
	g_return_if_fail (G_DBUS_CONNECTION (connection));
	g_return_if_fail (self->pv->prompter_registered == 0);
	g_return_if_fail (self->pv->connection == NULL);

	_gcr_debug ("registering prompter");

	self->pv->connection = g_object_ref (connection);

	self->pv->prompter_registered = g_dbus_connection_register_object (connection,
	                                                                   GCR_DBUS_PROMPTER_OBJECT_PATH,
	                                                                   _gcr_dbus_prompter_interface_info (),
	                                                                   &prompter_dbus_vtable,
	                                                                   self, NULL, &error);
	if (error != NULL) {
		g_warning ("error registering prompter %s", egg_error_message (error));
		g_clear_error (&error);
	}
}

void
gcr_system_prompter_unregister (GcrSystemPrompter *self,
                                gboolean wait)
{
	ActivePrompt *active;
	GVariantBuilder builder;
	const gchar *sender;
	GVariant *retval;
	GList *paths;
	GList *l;

	g_return_if_fail (GCR_IS_SYSTEM_PROMPTER (self));
	g_return_if_fail (self->pv->prompter_registered != 0);

	_gcr_debug ("unregistering prompter");

	paths = g_hash_table_get_keys (self->pv->active);
	for (l = paths; l != NULL; l = g_list_next (l)) {
		active = g_hash_table_lookup (self->pv->active, l->data);
		prompt_stop_prompting (active, TRUE, wait);
	}
	g_assert (g_hash_table_size (self->pv->active) == 0);
	g_list_free (paths);

	paths = g_hash_table_get_keys (self->pv->pending);
	for (l = paths; l != NULL; l = g_list_next (l)) {
		sender = g_hash_table_lookup (self->pv->pending, l->data);
		g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

		if (wait) {
			retval = g_dbus_connection_call_sync (self->pv->connection,
			                                      sender, l->data,
			                                      GCR_DBUS_CALLBACK_INTERFACE,
			                                      GCR_DBUS_CALLBACK_METHOD_DONE,
			                                      g_variant_new ("()"),
			                                      G_VARIANT_TYPE ("()"),
			                                      G_DBUS_CALL_FLAGS_NO_AUTO_START,
			                                      -1, NULL, NULL);
			if (retval)
				g_variant_unref (retval);

		} else {
			g_dbus_connection_call (self->pv->connection,
			                        sender, l->data,
			                        GCR_DBUS_CALLBACK_INTERFACE,
			                        GCR_DBUS_CALLBACK_METHOD_DONE,
			                        g_variant_new ("()"),
			                        G_VARIANT_TYPE ("()"),
			                        G_DBUS_CALL_FLAGS_NO_AUTO_START,
			                        -1, NULL, NULL, NULL);
		}

		if (!g_hash_table_remove (self->pv->pending, l->data))
			g_assert_not_reached ();
	}
	g_assert (g_hash_table_size (self->pv->pending) == 0);
	g_list_free (paths);

	if (!g_dbus_connection_unregister_object (self->pv->connection, self->pv->prompter_registered))
		g_assert_not_reached ();
	self->pv->prompter_registered = 0;

	g_clear_object (&self->pv->connection);
}

/**
 * gcr_system_prompter_new:
 *
 * Create a new system prompter service. This prompter won't do anything unless
 * you connect to its signals and show appropriate prompts.
 *
 * Returns: (transfer full): a new prompter service
 */
GcrSystemPrompter *
gcr_system_prompter_new (GcrSystemPrompterMode mode,
                         GType prompt_type)
{
	return g_object_new (GCR_TYPE_SYSTEM_PROMPTER,
	                     "mode", mode,
	                     "prompt-type", prompt_type,
	                     NULL);
}

GcrSystemPrompterMode
gcr_system_prompter_get_mode (GcrSystemPrompter *self)
{
	g_return_val_if_fail (GCR_IS_SYSTEM_PROMPTER (self), GCR_SYSTEM_PROMPTER_SINGLE);
	return self->pv->mode;
}

GType
gcr_system_prompter_get_prompt_type (GcrSystemPrompter *self)
{
	g_return_val_if_fail (GCR_IS_SYSTEM_PROMPTER (self), 0);
	return self->pv->prompt_type;
}
