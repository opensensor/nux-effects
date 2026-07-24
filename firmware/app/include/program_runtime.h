#ifndef NCR2_PROGRAM_RUNTIME_H
#define NCR2_PROGRAM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "effect_runtime.h"

typedef struct program_key {
    uint32_t vendor_id;
    uint32_t program_id;
} program_key_t;

typedef struct program_bank_key {
    uint32_t vendor_id;
    uint32_t bank_id;
} program_bank_key_t;

typedef struct program_parameter_value {
    uint32_t parameter_id;
    float value;
} program_parameter_value_t;

typedef struct program_node_descriptor {
    effect_key_t effect;
    const program_parameter_value_t *parameters;
    size_t parameter_count;
} program_node_descriptor_t;

/*
 * A program is an ordered effect chain today. The descriptor deliberately
 * uses stable effect identities so storage, editors, and future graph
 * compilers do not depend on factory slot numbers or C array positions.
 */
typedef struct program_descriptor {
    program_key_t key;
    const char *name;
    const program_node_descriptor_t *nodes;
    size_t node_count;
} program_descriptor_t;

typedef struct program_catalog {
    const program_descriptor_t *const *programs;
    size_t count;
} program_catalog_t;

typedef struct program_bank_descriptor {
    program_bank_key_t key;
    const char *name;
    const program_key_t *programs;
    size_t program_count;
} program_bank_descriptor_t;

typedef struct program_library {
    const program_catalog_t *catalog;
    const program_bank_descriptor_t *const *banks;
    size_t bank_count;
} program_library_t;

typedef struct program_cursor {
    const program_library_t *library;
    size_t bank_index;
    size_t program_index;
} program_cursor_t;

enum program_runtime_status {
    PROGRAM_RUNTIME_OK = 0,
    PROGRAM_RUNTIME_INVALID_ARGUMENT = 1,
    PROGRAM_RUNTIME_INVALID_DESCRIPTOR = 2,
    PROGRAM_RUNTIME_DUPLICATE_PROGRAM = 3,
    PROGRAM_RUNTIME_DUPLICATE_BANK = 4,
    PROGRAM_RUNTIME_EFFECT_NOT_FOUND = 5,
    PROGRAM_RUNTIME_PROGRAM_NOT_FOUND = 6,
    PROGRAM_RUNTIME_BANK_NOT_FOUND = 7,
    PROGRAM_RUNTIME_CHAIN_NOT_EMPTY = 8,
    PROGRAM_RUNTIME_CHAIN_ERROR = 9,
};

int program_key_equal(program_key_t left, program_key_t right);
int program_bank_key_equal(
    program_bank_key_t left,
    program_bank_key_t right);
uint16_t program_catalog_validate(
    const program_catalog_t *catalog,
    const effect_registry_t *effects);
const program_descriptor_t *program_catalog_find(
    const program_catalog_t *catalog,
    program_key_t key);
uint16_t program_library_validate(
    const program_library_t *library,
    const effect_registry_t *effects);
const program_bank_descriptor_t *program_library_find_bank(
    const program_library_t *library,
    program_bank_key_t key);

/*
 * Prepare into an empty, inactive chain. On failure the chain is cleared,
 * so a partially initialized program can never become active.
 */
uint16_t program_prepare(
    effect_chain_t *inactive_chain,
    const program_descriptor_t *program);

uint16_t program_cursor_initialize(
    program_cursor_t *cursor,
    const program_library_t *library,
    size_t bank_index,
    size_t program_index);
const program_descriptor_t *program_cursor_current(
    const program_cursor_t *cursor);
uint16_t program_cursor_next_program(program_cursor_t *cursor);
uint16_t program_cursor_previous_program(program_cursor_t *cursor);
uint16_t program_cursor_next_bank(program_cursor_t *cursor);
uint16_t program_cursor_previous_bank(program_cursor_t *cursor);
uint16_t program_cursor_select_bank(
    program_cursor_t *cursor,
    program_bank_key_t bank,
    size_t program_index);

#endif
