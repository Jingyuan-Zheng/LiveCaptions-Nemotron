/* Internal, backend-neutral recognition result used by LiveCaptions. */
#pragma once

#include <stddef.h>

typedef enum {
    CAPTION_UNIT_WORD_START = 1u << 0,
    CAPTION_UNIT_SENTENCE_END = 1u << 1
} CaptionUnitFlags;

typedef struct {
    char *text;
    float confidence;
    double start_time;
    double end_time;
    unsigned int flags;
} CaptionUnit;

typedef enum {
    CAPTION_RESULT_PARTIAL,
    CAPTION_RESULT_FINAL
} CaptionResultType;

typedef struct {
    CaptionResultType type;
    char *text;
    char *language;
    CaptionUnit *units;
    size_t unit_count;
} CaptionResult;

CaptionResult *caption_result_new(CaptionResultType type,
                                  const char *text,
                                  const char *language,
                                  const CaptionUnit *units,
                                  size_t unit_count);
CaptionResult *caption_result_copy(const CaptionResult *result);
void caption_result_free(CaptionResult *result);
