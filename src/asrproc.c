/* Nemotron 3.5 streaming ASR backend for the LiveCaptions fork. */
#include "asrproc.h"
#include "caption-result.h"
#include "history.h"
#include "line-gen.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "translation.h"

#include <nemo_speech/asr.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NEMOTRON_SAMPLE_RATE 16000
#define AUDIO_QUEUE_LIMIT 32
#define AUDIO_CONTROL_ENDPOINT ((size_t)-1)
#define AUDIO_CONTROL_STOP 0

typedef struct {
    size_t count;
    short samples[];
} AudioChunk;

struct asr_thread_i {
    nemo_speech_asr_recognizer *recognizer;
    nemo_speech_asr_stream *stream;
    GAsyncQueue *audio_queue;
    GThread *worker;
    GThread *line_watchdog;
    GMutex state_mutex;
    GMutex text_mutex;

    struct line_generator line;
    size_t layout_counter;
    time_t last_silence_time;

    gboolean ending;
    gboolean paused;
    gboolean errored;
    gboolean text_stream_active;
    gboolean translation_enabled;
    char *translation_model;
    char *translation_target_language;

    LiveCaptionsWindow *window;
    TranslationWorker *translator;
    char *last_result_text;
    CaptionResultType last_result_type;
    char *last_final_text;
    char *last_translation_text;
    CaptionResultType last_translation_type;
};

static gboolean main_thread_update_label(gpointer userdata) {
    asr_thread thread = userdata;
    if(thread->window == NULL || thread->paused) return G_SOURCE_REMOVE;

    g_mutex_lock(&thread->text_mutex);
    line_generator_set_text(&thread->line, thread->window->label);
    if(thread->text_stream_active) {
        LiveCaptionsApplication *app = LIVECAPTIONS_APPLICATION(
            gtk_window_get_application(GTK_WINDOW(thread->window)));
        livecaptions_application_stream_text(app,
            line_generator_get_plaintext(&thread->line));
    }
    g_mutex_unlock(&thread->text_mutex);
    return G_SOURCE_REMOVE;
}

static gpointer line_watchdog_main(gpointer userdata) {
    asr_thread thread = userdata;
    for(;;) {
        g_usleep(G_USEC_PER_SEC);
        g_mutex_lock(&thread->state_mutex);
        gboolean ending = thread->ending;
        g_mutex_unlock(&thread->state_mutex);
        if(ending) break;

        g_mutex_lock(&thread->text_mutex);
        if(thread->last_silence_time != 0 &&
           difftime(time(NULL), thread->last_silence_time) >= 6.0) {
            thread->last_silence_time = 0;
            for(int i = 1; i < AC_LINE_COUNT; i++)
                line_generator_break(&thread->line);
            save_silence_to_history();
            g_mutex_unlock(&thread->text_mutex);
            g_idle_add(main_thread_update_label, thread);
        } else {
            g_mutex_unlock(&thread->text_mutex);
        }
    }
    return NULL;
}

static void set_error(asr_thread thread, const char *where) {
    const char *detail = nemo_speech_asr_last_error();
    g_printerr("LiveCaptions Nemotron ASR (%s): %s\n", where,
               detail && *detail ? detail : "unknown error");
    g_mutex_lock(&thread->state_mutex);
    thread->errored = TRUE;
    g_mutex_unlock(&thread->state_mutex);
}

static gboolean text_ends_sentence(const char *text) {
    if(text == NULL || *text == '\0') return FALSE;
    const char *last = g_utf8_find_prev_char(text, text + strlen(text));
    gunichar c = last ? g_utf8_get_char(last) : 0;
    return c == '.' || c == '!' || c == '?' || c == 0x3002 ||
           c == 0xff01 || c == 0xff1f;
}

static float safe_confidence(float confidence) {
    return confidence >= 0.0f && confidence <= 1.0f ? confidence : 1.0f;
}

static CaptionResult *caption_result_from_nemo(const nemo_speech_asr_result *nemo) {
    if(nemo_speech_asr_result_alternative_count(nemo) == 0) return NULL;

    const char *transcript = nemo_speech_asr_result_transcript(nemo, 0);
    g_autofree char *clean = g_strdup(transcript ? transcript : "");
    g_strstrip(clean);

    const char *language = NULL;
    if(nemo_speech_asr_result_language_count(nemo, 0) > 0)
        language = nemo_speech_asr_result_language_code(nemo, 0, 0);

    size_t count = nemo_speech_asr_result_word_count(nemo, 0);
    CaptionUnit *units = NULL;
    if(count > 0) {
        units = g_new0(CaptionUnit, count);
        for(size_t i = 0; i < count; i++) {
            const char *word = nemo_speech_asr_result_word_text(nemo, 0, i);
            units[i].text = i == 0 ? g_strdup(word ? word : "")
                                    : g_strconcat(" ", word ? word : "", NULL);
            units[i].confidence = safe_confidence(
                nemo_speech_asr_result_word_confidence(nemo, 0, i));
            units[i].start_time =
                nemo_speech_asr_result_word_start_time(nemo, 0, i) / 1000.0;
            units[i].end_time =
                nemo_speech_asr_result_word_end_time(nemo, 0, i) / 1000.0;
            units[i].flags = CAPTION_UNIT_WORD_START;
            if(text_ends_sentence(word))
                units[i].flags |= CAPTION_UNIT_SENTENCE_END;
        }
    } else if(*clean) {
        g_auto(GStrv) words = g_strsplit_set(clean, " \t\r\n", -1);
        for(size_t i = 0; words[i] != NULL; i++)
            if(*words[i]) count++;
        units = g_new0(CaptionUnit, count);
        size_t out = 0;
        for(size_t i = 0; words[i] != NULL; i++) {
            if(!*words[i]) continue;
            units[out].text = out == 0 ? g_strdup(words[i])
                                        : g_strconcat(" ", words[i], NULL);
            units[out].confidence = 1.0f;
            units[out].flags = CAPTION_UNIT_WORD_START;
            if(text_ends_sentence(words[i]))
                units[out].flags |= CAPTION_UNIT_SENTENCE_END;
            out++;
        }
    }

    CaptionResultType type = nemo_speech_asr_result_is_final(nemo)
        ? CAPTION_RESULT_FINAL : CAPTION_RESULT_PARTIAL;
    CaptionResult *result = caption_result_new(type, clean, language, units, count);
    for(size_t i = 0; i < count; i++) g_free(units[i].text);
    g_free(units);
    return result;
}

static void process_caption_result(asr_thread thread, const CaptionResult *result) {
    if(result == NULL || result->unit_count == 0 || thread->window == NULL) return;

    g_mutex_lock(&thread->state_mutex);
    gboolean paused = thread->paused;
    g_mutex_unlock(&thread->state_mutex);
    if(paused) return;

    if(g_strcmp0(thread->last_result_text, result->text) == 0 &&
       thread->last_result_type == result->type)
        return;
    g_free(thread->last_result_text);
    thread->last_result_text = g_strdup(result->text);
    thread->last_result_type = result->type;

    g_mutex_lock(&thread->text_mutex);
    if(thread->layout_counter != thread->window->font_layout_counter ||
       thread->line.layout == NULL) {
        if(thread->line.layout != NULL) g_object_unref(thread->line.layout);
        thread->line.layout = pango_layout_copy(thread->window->font_layout);
        thread->line.max_text_width = thread->window->max_text_width;
        thread->layout_counter = thread->window->font_layout_counter;
    }
    if(result->language && *result->language)
        line_generator_set_language(&thread->line, result->language);
    line_generator_update(&thread->line, result->unit_count, result->units);

    if(result->type == CAPTION_RESULT_FINAL) {
        if(g_strcmp0(thread->last_final_text, result->text) != 0) {
            line_generator_finalize(&thread->line);
            commit_units_to_current_history(result->units, result->unit_count);
            g_free(thread->last_final_text);
            thread->last_final_text = g_strdup(result->text);
            thread->last_silence_time = time(NULL);
        }
    } else {
        g_clear_pointer(&thread->last_final_text, g_free);
        thread->last_silence_time = 0;
    }
    g_mutex_unlock(&thread->text_mutex);
    g_idle_add(main_thread_update_label, thread);

    if(thread->translator &&
       (g_strcmp0(thread->last_translation_text, result->text) != 0 ||
        thread->last_translation_type != result->type)) {
        g_free(thread->last_translation_text);
        thread->last_translation_text = g_strdup(result->text);
        thread->last_translation_type = result->type;
        translation_worker_submit(thread->translator, result->text,
                                  result->type == CAPTION_RESULT_FINAL);
    }
}

static gboolean drain_results(asr_thread thread) {
    for(;;) {
        nemo_speech_asr_result *nemo = NULL;
        nemo_speech_asr_status status = nemo_speech_asr_stream_next(thread->stream, &nemo);
        if(status != NEMO_SPEECH_ASR_OK) {
            set_error(thread, "stream_next");
            return FALSE;
        }
        if(nemo == NULL) return TRUE;
        CaptionResult *result = caption_result_from_nemo(nemo);
        process_caption_result(thread, result);
        caption_result_free(result);
        nemo_speech_asr_result_destroy(nemo);
    }
}

static gboolean create_stream(asr_thread thread) {
    if(thread->stream) {
        nemo_speech_asr_stream_close(thread->stream);
        thread->stream = NULL;
    }

    nemo_speech_asr_recognition_options options = nemo_speech_asr_recognition_options_default();
    options.language_code = "auto"; /* model-based language detection */
    options.interim_results = true;
    options.enable_word_time_offsets = true;
    options.enable_automatic_punctuation = true;

    nemo_speech_asr_status status = nemo_speech_asr_streaming_recognize(
        thread->recognizer, &options, &thread->stream);
    if(status != NEMO_SPEECH_ASR_OK) {
        set_error(thread, "streaming_recognize");
        return FALSE;
    }
    return TRUE;
}

static gpointer asr_worker_main(gpointer userdata) {
    asr_thread thread = userdata;

    while(TRUE) {
        AudioChunk *chunk = g_async_queue_pop(thread->audio_queue);
        if(chunk == NULL) continue;

        if(chunk->count == AUDIO_CONTROL_STOP) {
            g_free(chunk);
            break;
        }

        if(chunk->count == AUDIO_CONTROL_ENDPOINT) {
            g_free(chunk);
            if(thread->stream) {
                nemo_speech_asr_status status = nemo_speech_asr_stream_force_endpoint(thread->stream);
                if(status != NEMO_SPEECH_ASR_OK)
                    set_error(thread, "force_endpoint");
                else
                    drain_results(thread);
            }
            continue;
        }

        g_mutex_lock(&thread->state_mutex);
        gboolean paused = thread->paused;
        gboolean ending = thread->ending;
        g_mutex_unlock(&thread->state_mutex);
        if(ending) {
            g_free(chunk);
            break;
        }
        if(paused || thread->stream == NULL) {
            g_free(chunk);
            continue;
        }

        float *samples = g_new(float, chunk->count);
        for(size_t i = 0; i < chunk->count; i++)
            samples[i] = (float)chunk->samples[i] / 32768.0f;

        nemo_speech_asr_status status = nemo_speech_asr_stream_push_f32(
            thread->stream, samples, chunk->count, NEMOTRON_SAMPLE_RATE);
        g_free(samples);
        g_free(chunk);

        if(status != NEMO_SPEECH_ASR_OK) {
            set_error(thread, "stream_push_f32");
            continue;
        }
        drain_results(thread);
    }

    if(thread->stream) {
        nemo_speech_asr_stream_finish(thread->stream);
        drain_results(thread);
    }
    return NULL;
}

asr_thread create_asr_thread(G_GNUC_UNUSED const char *model_path) {
    const char *nemotron_model = g_getenv("LIVECAPTIONS_NEMOTRON_MODEL");
    if(nemotron_model == NULL || *nemotron_model == '\0') {
        g_printerr("LIVECAPTIONS_NEMOTRON_MODEL is not set.\n");
        return NULL;
    }

    asr_thread thread = g_new0(struct asr_thread_i, 1);
    g_mutex_init(&thread->state_mutex);
    g_mutex_init(&thread->text_mutex);
    thread->audio_queue = g_async_queue_new();
    thread->translation_enabled = FALSE;
    thread->translation_model = g_strdup("translategemma:4b");
    thread->translation_target_language = g_strdup("Chinese");
    line_generator_init(&thread->line);

    nemo_speech_asr_backend_config backend = {0};
    backend.size = sizeof(backend);
    backend.gpu = -1; /* CPU default; override is intentionally not automatic. */

    const char *gpu_env = g_getenv("LIVECAPTIONS_NEMOTRON_GPU");
    if(gpu_env && *gpu_env)
        backend.gpu = (int32_t)g_ascii_strtoll(gpu_env, NULL, 10);

    nemo_speech_asr_model_config model = {0};
    model.size = sizeof(model);
    model.path = nemotron_model;
    model.name = "Nemotron 3.5 ASR Streaming 0.6B";

    nemo_speech_asr_streaming_config streaming = {0};
    streaming.size = sizeof(streaming);
    streaming.chunk_size = 0.16f;
    streaming.ctc_left_padding = 0.0f;
    streaming.ctc_right_padding = 0.0f;
    streaming.rnnt_right_context = 1; /* ~160 ms algorithmic right context. */

    const char *right_context = g_getenv("LIVECAPTIONS_NEMOTRON_RIGHT_CONTEXT");
    if(right_context && *right_context)
        streaming.rnnt_right_context = (int32_t)g_ascii_strtoll(right_context, NULL, 10);

    nemo_speech_asr_endpointing_config endpointing = {0};
    endpointing.size = sizeof(endpointing);
    endpointing.enable = true;
    endpointing.vad_based = false;
    endpointing.stop_history_eou_ms = 600;

    nemo_speech_asr_recognizer_config config = {0};
    config.size = sizeof(config);
    config.backend = &backend;
    config.model = &model;
    config.streaming = &streaming;
    config.endpointing = &endpointing;

    nemo_speech_asr_status status = nemo_speech_asr_create(&config, &thread->recognizer);
    if(status != NEMO_SPEECH_ASR_OK || thread->recognizer == NULL) {
        set_error(thread, "create");
        g_async_queue_unref(thread->audio_queue);
        g_mutex_clear(&thread->text_mutex);
        g_mutex_clear(&thread->state_mutex);
        g_free(thread);
        return NULL;
    }

    if(!create_stream(thread)) {
        nemo_speech_asr_destroy(thread->recognizer);
        g_async_queue_unref(thread->audio_queue);
        g_mutex_clear(&thread->text_mutex);
        g_mutex_clear(&thread->state_mutex);
        g_free(thread);
        return NULL;
    }

    thread->worker = g_thread_new("lcap-nemotron", asr_worker_main, thread);
    thread->line_watchdog = g_thread_new("lcap-line-watchdog", line_watchdog_main, thread);
    return thread;
}

bool asr_thread_is_errored(asr_thread thread) {
    if(thread == NULL) return true;
    g_mutex_lock(&thread->state_mutex);
    gboolean errored = thread->errored;
    g_mutex_unlock(&thread->state_mutex);
    return errored;
}

void asr_thread_set_main_window(asr_thread thread, LiveCaptionsWindow *window) {
    if(thread == NULL) return;
    if(thread->translator) {
        translation_worker_free(thread->translator);
        thread->translator = NULL;
    }
    thread->window = window;
    if(window)
        thread->translator = translation_worker_new(window->translation_label,
                                                     thread->translation_enabled,
                                                     thread->translation_model,
                                                     thread->translation_target_language);
}

void asr_thread_set_translation_configuration(asr_thread thread,
                                              const char *model,
                                              const char *target_language) {
    if(thread == NULL) return;
    g_mutex_lock(&thread->state_mutex);
    g_free(thread->translation_model);
    g_free(thread->translation_target_language);
    thread->translation_model = g_strdup(model && *model ? model : "translategemma:4b");
    thread->translation_target_language = g_strdup(target_language && *target_language ? target_language : "Chinese");
    g_mutex_unlock(&thread->state_mutex);
    if(thread->translator)
        translation_worker_set_configuration(thread->translator,
                                             thread->translation_model,
                                             thread->translation_target_language);
}

void asr_thread_set_translation_enabled(asr_thread thread, bool enabled) {
    if(thread == NULL) return;
    g_mutex_lock(&thread->state_mutex);
    thread->translation_enabled = enabled;
    g_mutex_unlock(&thread->state_mutex);
    if(thread->translator)
        translation_worker_set_enabled(thread->translator, enabled);
}

void asr_thread_enqueue_audio(asr_thread thread, short *data, size_t num_shorts) {
    if(thread == NULL || data == NULL || num_shorts == 0) return;

    g_mutex_lock(&thread->state_mutex);
    gboolean skip = thread->ending || thread->paused;
    g_mutex_unlock(&thread->state_mutex);
    if(skip) return;

    while(g_async_queue_length(thread->audio_queue) > AUDIO_QUEUE_LIMIT) {
        AudioChunk *old = g_async_queue_try_pop(thread->audio_queue);
        if(old == NULL) break;
        g_free(old);
    }

    AudioChunk *chunk = g_malloc(sizeof(AudioChunk) + num_shorts * sizeof(short));
    chunk->count = num_shorts;
    memcpy(chunk->samples, data, num_shorts * sizeof(short));
    g_async_queue_push(thread->audio_queue, chunk);
}

void asr_thread_pause(asr_thread thread, bool pause) {
    if(thread == NULL) return;
    g_mutex_lock(&thread->state_mutex);
    thread->paused = pause;
    g_mutex_unlock(&thread->state_mutex);
}

void asr_thread_set_text_stream_active(asr_thread thread, bool active) {
    if(thread == NULL) return;
    g_mutex_lock(&thread->state_mutex);
    thread->text_stream_active = active;
    g_mutex_unlock(&thread->state_mutex);
}

int asr_thread_samplerate(G_GNUC_UNUSED asr_thread thread) {
    return NEMOTRON_SAMPLE_RATE;
}

void asr_thread_flush(asr_thread thread) {
    if(thread == NULL) return;
    AudioChunk *control = g_malloc0(sizeof(AudioChunk));
    control->count = AUDIO_CONTROL_ENDPOINT;
    g_async_queue_push(thread->audio_queue, control);
}

void free_asr_thread(asr_thread thread) {
    if(thread == NULL) return;

    g_mutex_lock(&thread->state_mutex);
    thread->ending = TRUE;
    g_mutex_unlock(&thread->state_mutex);

    AudioChunk *sentinel = g_malloc0(sizeof(AudioChunk));
    sentinel->count = AUDIO_CONTROL_STOP;
    g_async_queue_push(thread->audio_queue, sentinel);

    if(thread->worker) g_thread_join(thread->worker);
    if(thread->line_watchdog) g_thread_join(thread->line_watchdog);

    if(thread->translator) translation_worker_free(thread->translator);
    g_clear_pointer(&thread->translation_model, g_free);
    g_clear_pointer(&thread->translation_target_language, g_free);
    if(thread->stream) nemo_speech_asr_stream_close(thread->stream);
    if(thread->recognizer) nemo_speech_asr_destroy(thread->recognizer);
    if(thread->line.layout) g_object_unref(thread->line.layout);

    AudioChunk *left;
    while((left = g_async_queue_try_pop(thread->audio_queue)) != NULL) g_free(left);
    g_async_queue_unref(thread->audio_queue);
    g_clear_pointer(&thread->last_result_text, g_free);
    g_clear_pointer(&thread->last_final_text, g_free);
    g_clear_pointer(&thread->last_translation_text, g_free);
    g_mutex_clear(&thread->text_mutex);
    g_mutex_clear(&thread->state_mutex);
    g_free(thread);
}
