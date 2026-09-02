#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STUDY_DECK_COUNT 8
#define STUDY_UNIT_COUNT 8
#define STUDY_STANDARD_DECK_WORDS 64
#define STUDY_DECK_WORDS_MAX     127
#define STUDY_UNIT_WORDS_MAX      31

typedef struct {
    const char *word;
    const char *meaning;
    uint8_t unit;
} study_word_t;

typedef struct {
    const char *progress_key;
    uint8_t grade;
    bool upper;
    const char *revision;
    const char *unit_titles[STUDY_UNIT_COUNT];
    const study_word_t *words;
    uint16_t word_count;
} study_deck_t;

extern const study_deck_t STUDY_DECKS[STUDY_DECK_COUNT];
