#include "translation.h"

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define DEFAULT_OLLAMA_URL "http://127.0.0.1:11434"
#define DEFAULT_OLLAMA_MODEL "translategemma:4b"
#define COALESCE_USEC (120 * 1000)

typedef struct {
    GtkLabel *label;
    char *text;
} UiUpdate;

struct _TranslationWorker {
    GThread *thread;
    GMutex mutex;
    GCond cond;
    gboolean stopping;
    gboolean enabled;
    gboolean warmed;
    gboolean warm_requested;
    guint64 revision;
    gboolean active_final;

    char *pending_text;
    char *pending_final_text;
    char *last_submitted_text;
    gboolean last_submitted_final;
    GCancellable *active_request;
    char *committed_translation;
    char *visible_live_translation;

    GtkLabel *label;
    char *url;
    char *model;
    char *target_language;
};

static gboolean apply_translation_on_main(gpointer userdata) {
    UiUpdate *update = userdata;
    gtk_label_set_text(update->label, update->text ? update->text : "");
    g_object_unref(update->label);
    g_free(update->text);
    g_free(update);
    return G_SOURCE_REMOVE;
}

static void queue_label_update(TranslationWorker *worker, const char *text) {
    UiUpdate *update = g_new0(UiUpdate, 1);
    update->label = g_object_ref(worker->label);
    update->text = g_strdup(text ? text : "");
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, apply_translation_on_main, update, NULL);
}

static void queue_translation_progress(TranslationWorker *worker,
                                       const char *text,
                                       guint64 revision,
                                       gboolean force) {
    char *display = NULL;

    g_mutex_lock(&worker->mutex);
    gboolean current = worker->enabled && !worker->stopping &&
        revision == worker->revision;
    if(current) {
        gsize old_length = worker->visible_live_translation
            ? g_utf8_strlen(worker->visible_live_translation, -1) : 0;
        gsize new_length = text ? g_utf8_strlen(text, -1) : 0;

        /* Keep the previous live tail until its replacement has caught up.
         * This prevents every restarted partial translation from collapsing
         * to its first token and visibly flashing. */
        if(force || new_length >= old_length) {
            g_free(worker->visible_live_translation);
            worker->visible_live_translation = g_strdup(text ? text : "");
        }

        if(worker->committed_translation && *worker->committed_translation &&
           worker->visible_live_translation && *worker->visible_live_translation) {
            display = g_strconcat(worker->committed_translation, "\n",
                                  worker->visible_live_translation, NULL);
        } else if(worker->visible_live_translation && *worker->visible_live_translation) {
            display = g_strdup(worker->visible_live_translation);
        } else {
            display = g_strdup(worker->committed_translation
                ? worker->committed_translation : "");
        }
    }
    g_mutex_unlock(&worker->mutex);

    if(display != NULL) {
        queue_label_update(worker, display);
        g_free(display);
    }
}

static void commit_translation(TranslationWorker *worker,
                               const char *text,
                               guint64 revision) {
    char *display = NULL;

    g_mutex_lock(&worker->mutex);
    if(worker->enabled && !worker->stopping && revision == worker->revision) {
        g_free(worker->committed_translation);
        worker->committed_translation = g_strdup(text ? text : "");
        g_clear_pointer(&worker->visible_live_translation, g_free);
        display = g_strdup(worker->committed_translation);
    }
    g_mutex_unlock(&worker->mutex);

    if(display != NULL) {
        queue_label_update(worker, display);
        g_free(display);
    }
}

static char *build_generate_url(const char *base) {
    if(g_str_has_suffix(base, "/api/generate")) return g_strdup(base);
    if(g_str_has_suffix(base, "/")) return g_strconcat(base, "api/generate", NULL);
    return g_strconcat(base, "/api/generate", NULL);
}

static char *ollama_translate(SoupSession *session,
                              TranslationWorker *worker,
                              const char *source,
                              GCancellable *cancellable,
                              gboolean update_live,
                              guint64 revision,
                              GError **error) {
    g_mutex_lock(&worker->mutex);
    g_autofree char *model = g_strdup(worker->model);
    g_autofree char *target_language = g_strdup(worker->target_language);
    g_mutex_unlock(&worker->mutex);
    g_autofree char *prompt = g_strdup_printf(
        "Translate the following text into %s. Preserve names, numbers and meaning. Output only the translation:\n%s",
        target_language, source);

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);
    json_builder_set_member_name(builder, "prompt");
    json_builder_add_string_value(builder, prompt);
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "keep_alive");
    json_builder_add_string_value(builder, "5m");
    json_builder_set_member_name(builder, "options");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "temperature");
    json_builder_add_double_value(builder, 0.0);
    json_builder_set_member_name(builder, "num_predict");
    json_builder_add_int_value(builder, 96);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    g_autofree char *request_json = json_generator_to_data(generator, NULL);

    SoupMessage *message = soup_message_new("POST", worker->url);
    GBytes *request_bytes = g_bytes_new(request_json, strlen(request_json));
    soup_message_set_request_body_from_bytes(message, "application/json", request_bytes);
    g_bytes_unref(request_bytes);

    GInputStream *response_stream = soup_session_send(session, message, cancellable, error);
    guint status = soup_message_get_status(message);

    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);

    if(response_stream == NULL) {
        g_object_unref(message);
        return NULL;
    }
    if(status < 200 || status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Ollama returned HTTP %u", status);
        g_object_unref(response_stream);
        g_object_unref(message);
        return NULL;
    }

    GDataInputStream *data_stream = g_data_input_stream_new(response_stream);
    GString *translated = g_string_new(NULL);
    gboolean failed = FALSE;

    while(TRUE) {
        gsize line_length = 0;
        g_autofree char *line = g_data_input_stream_read_line(
            data_stream, &line_length, cancellable, error);
        if(line == NULL) {
            if(error != NULL && *error != NULL) failed = TRUE;
            break;
        }
        if(line_length == 0) continue;

        JsonParser *parser = json_parser_new();
        if(!json_parser_load_from_data(parser, line, line_length, error)) {
            g_object_unref(parser);
            failed = TRUE;
            break;
        }
        JsonNode *response_root = json_parser_get_root(parser);
        if(!JSON_NODE_HOLDS_OBJECT(response_root)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Invalid Ollama streaming JSON response");
            g_object_unref(parser);
            failed = TRUE;
            break;
        }

        JsonObject *object = json_node_get_object(response_root);
        const char *chunk = json_object_get_string_member_with_default(
            object, "response", "");
        if(chunk[0] != '\0') {
            g_string_append(translated, chunk);
            if(update_live) {
                g_mutex_lock(&worker->mutex);
                gboolean current = worker->enabled && !worker->stopping &&
                    revision == worker->revision;
                g_mutex_unlock(&worker->mutex);
                if(current)
                    queue_translation_progress(worker, translated->str,
                                               revision, FALSE);
            }
        }
        gboolean done = json_object_get_boolean_member_with_default(
            object, "done", FALSE);
        g_object_unref(parser);
        if(done) break;
    }

    g_object_unref(data_stream);
    g_object_unref(response_stream);
    g_object_unref(message);
    if(failed) {
        g_string_free(translated, TRUE);
        return NULL;
    }

    char *result = g_string_free(translated, FALSE);
    g_strstrip(result);
    return result;
}

static gpointer translation_thread_main(gpointer userdata) {
    TranslationWorker *worker = userdata;
    SoupSession *session = soup_session_new();
    g_object_set(session, "timeout", 10u, NULL);

    while(TRUE) {
        g_mutex_lock(&worker->mutex);
        while(!worker->stopping &&
              (!worker->enabled ||
               (worker->warmed && worker->pending_text == NULL &&
                worker->pending_final_text == NULL)))
            g_cond_wait(&worker->cond, &worker->mutex);

        if(worker->stopping) {
            g_mutex_unlock(&worker->mutex);
            break;
        }

        gboolean need_warm = worker->enabled && !worker->warmed;
        worker->warm_requested = FALSE;
        g_mutex_unlock(&worker->mutex);

        if(need_warm) {
            g_autoptr(GCancellable) cancellable = g_cancellable_new();
            g_mutex_lock(&worker->mutex);
            g_set_object(&worker->active_request, cancellable);
            g_mutex_unlock(&worker->mutex);
            GError *warm_error = NULL;
            char *warm = ollama_translate(session, worker, "Hello.", cancellable,
                                          FALSE, 0, &warm_error);
            g_free(warm);
            if(warm_error && !g_error_matches(warm_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_printerr("LiveCaptions translation warm-up: %s\n", warm_error->message);
            }
            g_clear_error(&warm_error);
            g_mutex_lock(&worker->mutex);
            if(worker->active_request == cancellable)
                g_clear_object(&worker->active_request);
            worker->active_final = FALSE;
            if(worker->enabled) worker->warmed = TRUE;
            g_mutex_unlock(&worker->mutex);
        }

        g_mutex_lock(&worker->mutex);
        if(worker->stopping) {
            g_mutex_unlock(&worker->mutex);
            break;
        }
        if(!worker->enabled ||
           (worker->pending_text == NULL && worker->pending_final_text == NULL)) {
            g_mutex_unlock(&worker->mutex);
            continue;
        }

        if(worker->pending_final_text == NULL) {
            gint64 deadline = g_get_monotonic_time() + COALESCE_USEC;
            while(!worker->stopping && worker->enabled &&
                  worker->pending_final_text == NULL &&
                  g_get_monotonic_time() < deadline)
                g_cond_wait_until(&worker->cond, &worker->mutex, deadline);
        }

        if(worker->stopping) {
            g_mutex_unlock(&worker->mutex);
            break;
        }
        if(!worker->enabled ||
           (worker->pending_text == NULL && worker->pending_final_text == NULL)) {
            g_mutex_unlock(&worker->mutex);
            continue;
        }

        gboolean is_final = worker->pending_final_text != NULL;
        char *text = is_final ? worker->pending_final_text : worker->pending_text;
        if(is_final)
            worker->pending_final_text = NULL;
        else
            worker->pending_text = NULL;
        guint64 revision = worker->revision;
        g_autoptr(GCancellable) cancellable = g_cancellable_new();
        g_set_object(&worker->active_request, cancellable);
        worker->active_final = is_final;
        g_mutex_unlock(&worker->mutex);

        g_strstrip(text);
        if(text[0] != '\0') {
            GError *error = NULL;
            char *translated = ollama_translate(session, worker, text, cancellable,
                                                TRUE, revision, &error);
            if(translated != NULL) {
                /* Do not let a slow request overwrite the caption that was
                 * submitted after it.  The pending newest caption will be
                 * translated by the next worker iteration. */
                g_mutex_lock(&worker->mutex);
                gboolean apply = worker->enabled && !worker->stopping &&
                    revision == worker->revision;
                g_mutex_unlock(&worker->mutex);
                if(apply) {
                    if(is_final)
                        commit_translation(worker, translated, revision);
                    else
                        queue_translation_progress(worker, translated,
                                                   revision, TRUE);
                }
                g_free(translated);
            } else if(error != NULL &&
                      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_printerr("LiveCaptions translation: %s\n", error->message);
                g_usleep(500 * 1000);
            }
            g_clear_error(&error);
        }
        g_mutex_lock(&worker->mutex);
        if(worker->active_request == cancellable)
            g_clear_object(&worker->active_request);
        worker->active_final = FALSE;
        g_mutex_unlock(&worker->mutex);
        g_free(text);
    }

    g_object_unref(session);
    return NULL;
}

TranslationWorker *translation_worker_new(GtkLabel *label, gboolean enabled,
                                          const char *model, const char *target_language) {
    g_return_val_if_fail(GTK_IS_LABEL(label), NULL);

    TranslationWorker *worker = g_new0(TranslationWorker, 1);
    g_mutex_init(&worker->mutex);
    g_cond_init(&worker->cond);
    worker->label = g_object_ref(label);
    worker->enabled = enabled;
    worker->warm_requested = enabled;

    const char *base = g_getenv("LIVECAPTIONS_OLLAMA_URL");
    worker->url = build_generate_url(base && *base ? base : DEFAULT_OLLAMA_URL);
    worker->model = g_strdup(model && *model ? model : DEFAULT_OLLAMA_MODEL);
    worker->target_language = g_strdup(target_language && *target_language ? target_language : "Chinese");

    worker->thread = g_thread_new("lcap-translation", translation_thread_main, worker);
    return worker;
}

void translation_worker_set_configuration(TranslationWorker *worker,
                                          const char *model, const char *target_language) {
    if(worker == NULL) return;
    g_mutex_lock(&worker->mutex);
    gboolean model_changed = g_strcmp0(worker->model, model) != 0;
    worker->revision++;
    if(worker->active_request != NULL)
        g_cancellable_cancel(worker->active_request);
    g_free(worker->model);
    g_free(worker->target_language);
    worker->model = g_strdup(model && *model ? model : DEFAULT_OLLAMA_MODEL);
    worker->target_language = g_strdup(target_language && *target_language ? target_language : "Chinese");
    g_clear_pointer(&worker->pending_text, g_free);
    g_clear_pointer(&worker->pending_final_text, g_free);
    g_clear_pointer(&worker->committed_translation, g_free);
    g_clear_pointer(&worker->visible_live_translation, g_free);
    if(model_changed) worker->warmed = FALSE;
    g_cond_signal(&worker->cond);
    g_mutex_unlock(&worker->mutex);
}

void translation_worker_set_enabled(TranslationWorker *worker, gboolean enabled) {
    if(worker == NULL) return;

    g_mutex_lock(&worker->mutex);
    gboolean was_enabled = worker->enabled;
    worker->revision++;
    if(worker->active_request != NULL)
        g_cancellable_cancel(worker->active_request);
    worker->enabled = enabled;
    g_clear_pointer(&worker->pending_text, g_free);
    g_clear_pointer(&worker->pending_final_text, g_free);
    g_clear_pointer(&worker->last_submitted_text, g_free);
    g_clear_pointer(&worker->committed_translation, g_free);
    g_clear_pointer(&worker->visible_live_translation, g_free);
    if(enabled && !was_enabled && !worker->warmed)
        worker->warm_requested = TRUE;
    g_cond_signal(&worker->cond);
    g_mutex_unlock(&worker->mutex);

    if(!enabled) queue_label_update(worker, "");
}

void translation_worker_submit(TranslationWorker *worker, const char *text,
                               gboolean is_final) {
    if(worker == NULL || text == NULL) return;

    g_mutex_lock(&worker->mutex);
    if(worker->stopping || !worker->enabled) {
        g_mutex_unlock(&worker->mutex);
        return;
    }

    if(worker->last_submitted_text != NULL &&
       g_strcmp0(worker->last_submitted_text, text) == 0 &&
       worker->last_submitted_final == is_final) {
        g_mutex_unlock(&worker->mutex);
        return;
    }

    g_free(worker->last_submitted_text);
    worker->last_submitted_text = g_strdup(text);
    worker->last_submitted_final = is_final;
    if(worker->active_request != NULL && !worker->active_final) {
        worker->revision++;
        g_cancellable_cancel(worker->active_request);
    }
    if(is_final) {
        g_free(worker->pending_final_text);
        worker->pending_final_text = g_strdup(text);
        g_clear_pointer(&worker->pending_text, g_free);
    } else {
        g_free(worker->pending_text);
        worker->pending_text = g_strdup(text);
    }
    g_cond_signal(&worker->cond);
    g_mutex_unlock(&worker->mutex);
}

void translation_worker_free(TranslationWorker *worker) {
    if(worker == NULL) return;

    g_mutex_lock(&worker->mutex);
    worker->stopping = TRUE;
    worker->revision++;
    if(worker->active_request != NULL)
        g_cancellable_cancel(worker->active_request);
    g_cond_signal(&worker->cond);
    g_mutex_unlock(&worker->mutex);

    if(worker->thread != NULL) g_thread_join(worker->thread);

    g_clear_pointer(&worker->pending_text, g_free);
    g_clear_pointer(&worker->pending_final_text, g_free);
    g_clear_pointer(&worker->last_submitted_text, g_free);
    g_clear_pointer(&worker->committed_translation, g_free);
    g_clear_pointer(&worker->visible_live_translation, g_free);
    g_clear_object(&worker->active_request);
    g_clear_pointer(&worker->url, g_free);
    g_clear_pointer(&worker->model, g_free);
    g_clear_pointer(&worker->target_language, g_free);
    g_clear_object(&worker->label);
    g_cond_clear(&worker->cond);
    g_mutex_clear(&worker->mutex);
    g_free(worker);
}
