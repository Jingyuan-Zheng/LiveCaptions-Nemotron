#pragma once
#include <gtk/gtk.h>
typedef struct _TranslationWorker TranslationWorker;
TranslationWorker *translation_worker_new(GtkLabel *label, gboolean enabled,
                                          const char *model, const char *target_language);
void translation_worker_set_enabled(TranslationWorker *worker, gboolean enabled);
void translation_worker_set_configuration(TranslationWorker *worker,
                                          const char *model, const char *target_language);
void translation_worker_submit(TranslationWorker *worker, const char *text,
                               gboolean is_final);
void translation_worker_free(TranslationWorker *worker);
