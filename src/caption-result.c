#include "caption-result.h"

#include <glib.h>
#include <string.h>

CaptionResult *caption_result_new(CaptionResultType type,
                                  const char *text,
                                  const char *language,
                                  const CaptionUnit *units,
                                  size_t unit_count)
{
    CaptionResult *result = g_new0(CaptionResult, 1);
    result->type = type;
    result->text = g_strdup(text ? text : "");
    result->language = g_strdup(language);
    result->unit_count = unit_count;

    if(unit_count > 0) {
        result->units = g_new0(CaptionUnit, unit_count);
        for(size_t i = 0; i < unit_count; i++) {
            result->units[i] = units[i];
            result->units[i].text = g_strdup(units[i].text ? units[i].text : "");
        }
    }

    return result;
}

CaptionResult *caption_result_copy(const CaptionResult *result)
{
    if(result == NULL) return NULL;
    return caption_result_new(result->type, result->text, result->language,
                              result->units, result->unit_count);
}

void caption_result_free(CaptionResult *result)
{
    if(result == NULL) return;
    for(size_t i = 0; i < result->unit_count; i++)
        g_free(result->units[i].text);
    g_free(result->units);
    g_free(result->text);
    g_free(result->language);
    g_free(result);
}
