#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STUDY_DECK_COUNT 8
#define STUDY_UNIT_COUNT 8
#define STUDY_DECK_WORDS 64

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
} study_deck_t;

extern const study_deck_t STUDY_DECKS[STUDY_DECK_COUNT];
