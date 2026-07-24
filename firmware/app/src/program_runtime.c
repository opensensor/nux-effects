#include "program_runtime.h"

static uint16_t program_descriptor_validate(
    const program_descriptor_t *program,
    const effect_registry_t *effects)
{
    if (program == NULL ||
        program->name == NULL ||
        (program->node_count != 0U &&
         program->nodes == NULL)) {
        return PROGRAM_RUNTIME_INVALID_DESCRIPTOR;
    }

    for (size_t node_index = 0U;
         node_index < program->node_count;
         ++node_index) {
        const program_node_descriptor_t *node =
            &program->nodes[node_index];
        const effect_descriptor_t *effect =
            effect_registry_find(effects, node->effect);

        if (effect == NULL) {
            return PROGRAM_RUNTIME_EFFECT_NOT_FOUND;
        }
        if ((node->parameter_count != 0U &&
             node->parameters == NULL) ||
            node->parameter_count >
                (size_t)effect->parameter_count) {
            return PROGRAM_RUNTIME_INVALID_DESCRIPTOR;
        }
        for (size_t parameter = 0U;
             parameter < node->parameter_count;
             ++parameter) {
            const effect_parameter_descriptor_t *schema =
                effect_parameter_find(
                    effect,
                    node->parameters[parameter].parameter_id);

            if (schema == NULL ||
                node->parameters[parameter].value !=
                    node->parameters[parameter].value ||
                node->parameters[parameter].value <
                    schema->minimum ||
                node->parameters[parameter].value >
                    schema->maximum) {
                return PROGRAM_RUNTIME_INVALID_DESCRIPTOR;
            }
            for (size_t previous = 0U;
                 previous < parameter;
                 ++previous) {
                if (node->parameters[parameter].parameter_id ==
                    node->parameters[previous].parameter_id) {
                    return
                        PROGRAM_RUNTIME_INVALID_DESCRIPTOR;
                }
            }
        }
    }
    return PROGRAM_RUNTIME_OK;
}

static const program_bank_descriptor_t *bank_at(
    const program_library_t *library,
    size_t bank_index)
{
    if (library == NULL ||
        library->banks == NULL ||
        bank_index >= library->bank_count) {
        return NULL;
    }
    return library->banks[bank_index];
}

int program_key_equal(program_key_t left, program_key_t right)
{
    return left.vendor_id == right.vendor_id &&
           left.program_id == right.program_id;
}

int program_bank_key_equal(
    program_bank_key_t left,
    program_bank_key_t right)
{
    return left.vendor_id == right.vendor_id &&
           left.bank_id == right.bank_id;
}

uint16_t program_catalog_validate(
    const program_catalog_t *catalog,
    const effect_registry_t *effects)
{
    if (catalog == NULL ||
        effects == NULL ||
        (catalog->count != 0U &&
         catalog->programs == NULL) ||
        effect_registry_validate(effects) !=
            EFFECT_RUNTIME_OK) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }

    for (size_t index = 0U;
         index < catalog->count;
         ++index) {
        uint16_t descriptor_status =
            program_descriptor_validate(
                catalog->programs[index],
                effects);

        if (descriptor_status != PROGRAM_RUNTIME_OK) {
            return descriptor_status;
        }
        for (size_t previous = 0U;
             previous < index;
             ++previous) {
            if (program_key_equal(
                    catalog->programs[index]->key,
                    catalog->programs[previous]->key)) {
                return PROGRAM_RUNTIME_DUPLICATE_PROGRAM;
            }
        }
    }
    return PROGRAM_RUNTIME_OK;
}

const program_descriptor_t *program_catalog_find(
    const program_catalog_t *catalog,
    program_key_t key)
{
    if (catalog == NULL || catalog->programs == NULL) {
        return NULL;
    }

    for (size_t index = 0U;
         index < catalog->count;
         ++index) {
        if (catalog->programs[index] != NULL &&
            program_key_equal(
                catalog->programs[index]->key,
                key)) {
            return catalog->programs[index];
        }
    }
    return NULL;
}

uint16_t program_library_validate(
    const program_library_t *library,
    const effect_registry_t *effects)
{
    uint16_t status;

    if (library == NULL ||
        library->catalog == NULL ||
        library->catalog->count == 0U ||
        library->bank_count == 0U ||
        library->banks == NULL) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    status =
        program_catalog_validate(library->catalog, effects);
    if (status != PROGRAM_RUNTIME_OK) {
        return status;
    }

    for (size_t index = 0U;
         index < library->bank_count;
         ++index) {
        const program_bank_descriptor_t *bank =
            library->banks[index];

        if (bank == NULL ||
            bank->name == NULL ||
            bank->programs == NULL ||
            bank->program_count == 0U) {
            return PROGRAM_RUNTIME_INVALID_DESCRIPTOR;
        }
        for (size_t program = 0U;
             program < bank->program_count;
             ++program) {
            if (program_catalog_find(
                    library->catalog,
                    bank->programs[program]) == NULL) {
                return PROGRAM_RUNTIME_PROGRAM_NOT_FOUND;
            }
            for (size_t previous = 0U;
                 previous < program;
                 ++previous) {
                if (program_key_equal(
                        bank->programs[program],
                        bank->programs[previous])) {
                    return
                        PROGRAM_RUNTIME_INVALID_DESCRIPTOR;
                }
            }
        }
        for (size_t previous = 0U;
             previous < index;
             ++previous) {
            if (program_bank_key_equal(
                    bank->key,
                    library->banks[previous]->key)) {
                return PROGRAM_RUNTIME_DUPLICATE_BANK;
            }
        }
    }
    return PROGRAM_RUNTIME_OK;
}

const program_bank_descriptor_t *program_library_find_bank(
    const program_library_t *library,
    program_bank_key_t key)
{
    if (library == NULL || library->banks == NULL) {
        return NULL;
    }

    for (size_t index = 0U;
         index < library->bank_count;
         ++index) {
        if (library->banks[index] != NULL &&
            program_bank_key_equal(
                library->banks[index]->key,
                key)) {
            return library->banks[index];
        }
    }
    return NULL;
}

uint16_t program_prepare(
    effect_chain_t *inactive_chain,
    const program_descriptor_t *program)
{
    if (inactive_chain == NULL ||
        inactive_chain->registry == NULL ||
        program == NULL) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    if (inactive_chain->count != 0U ||
        inactive_chain->arena_used != 0U) {
        return PROGRAM_RUNTIME_CHAIN_NOT_EMPTY;
    }
    {
        uint16_t descriptor_status =
            program_descriptor_validate(
                program,
                inactive_chain->registry);

        if (descriptor_status != PROGRAM_RUNTIME_OK) {
            return descriptor_status;
        }
    }

    for (size_t node = 0U;
         node < program->node_count;
         ++node) {
        size_t instance_index;
        uint16_t effect_status =
            effect_chain_add(
                inactive_chain,
                program->nodes[node].effect,
                &instance_index);

        if (effect_status != EFFECT_RUNTIME_OK) {
            effect_chain_clear(inactive_chain);
            return PROGRAM_RUNTIME_CHAIN_ERROR;
        }

        for (size_t parameter = 0U;
             parameter <
                 program->nodes[node].parameter_count;
             ++parameter) {
            const program_parameter_value_t *value =
                &program->nodes[node].parameters[parameter];

            effect_status =
                effect_chain_set_parameter(
                    inactive_chain,
                    instance_index,
                    value->parameter_id,
                    value->value);
            if (effect_status != EFFECT_RUNTIME_OK) {
                effect_chain_clear(inactive_chain);
                return PROGRAM_RUNTIME_CHAIN_ERROR;
            }
        }
    }
    return PROGRAM_RUNTIME_OK;
}

uint16_t program_cursor_initialize(
    program_cursor_t *cursor,
    const program_library_t *library,
    size_t bank_index,
    size_t program_index)
{
    const program_bank_descriptor_t *bank =
        bank_at(library, bank_index);

    if (cursor == NULL ||
        bank == NULL ||
        library->catalog == NULL ||
        program_index >= bank->program_count) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    cursor->library = library;
    cursor->bank_index = bank_index;
    cursor->program_index = program_index;
    return PROGRAM_RUNTIME_OK;
}

const program_descriptor_t *program_cursor_current(
    const program_cursor_t *cursor)
{
    const program_bank_descriptor_t *bank;

    if (cursor == NULL) {
        return NULL;
    }
    bank = bank_at(cursor->library, cursor->bank_index);
    if (bank == NULL ||
        cursor->library->catalog == NULL ||
        cursor->program_index >= bank->program_count) {
        return NULL;
    }
    return program_catalog_find(
        cursor->library->catalog,
        bank->programs[cursor->program_index]);
}

uint16_t program_cursor_next_program(program_cursor_t *cursor)
{
    const program_bank_descriptor_t *bank;

    if (cursor == NULL) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    bank = bank_at(cursor->library, cursor->bank_index);
    if (bank == NULL ||
        bank->program_count == 0U ||
        cursor->program_index >= bank->program_count) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    ++cursor->program_index;
    if (cursor->program_index == bank->program_count) {
        cursor->program_index = 0U;
    }
    return PROGRAM_RUNTIME_OK;
}

uint16_t program_cursor_previous_program(
    program_cursor_t *cursor)
{
    const program_bank_descriptor_t *bank;

    if (cursor == NULL) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    bank = bank_at(cursor->library, cursor->bank_index);
    if (bank == NULL ||
        bank->program_count == 0U ||
        cursor->program_index >= bank->program_count) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    if (cursor->program_index == 0U) {
        cursor->program_index = bank->program_count - 1U;
    } else {
        --cursor->program_index;
    }
    return PROGRAM_RUNTIME_OK;
}

uint16_t program_cursor_next_bank(program_cursor_t *cursor)
{
    if (cursor == NULL ||
        cursor->library == NULL ||
        cursor->library->bank_count == 0U ||
        cursor->bank_index >=
            cursor->library->bank_count) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    ++cursor->bank_index;
    if (cursor->bank_index ==
        cursor->library->bank_count) {
        cursor->bank_index = 0U;
    }
    cursor->program_index = 0U;
    return PROGRAM_RUNTIME_OK;
}

uint16_t program_cursor_previous_bank(
    program_cursor_t *cursor)
{
    if (cursor == NULL ||
        cursor->library == NULL ||
        cursor->library->bank_count == 0U ||
        cursor->bank_index >=
            cursor->library->bank_count) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }
    if (cursor->bank_index == 0U) {
        cursor->bank_index =
            cursor->library->bank_count - 1U;
    } else {
        --cursor->bank_index;
    }
    cursor->program_index = 0U;
    return PROGRAM_RUNTIME_OK;
}

uint16_t program_cursor_select_bank(
    program_cursor_t *cursor,
    program_bank_key_t bank_key,
    size_t program_index)
{
    if (cursor == NULL ||
        cursor->library == NULL ||
        cursor->library->banks == NULL) {
        return PROGRAM_RUNTIME_INVALID_ARGUMENT;
    }

    for (size_t index = 0U;
         index < cursor->library->bank_count;
         ++index) {
        const program_bank_descriptor_t *bank =
            cursor->library->banks[index];

        if (bank != NULL &&
            program_bank_key_equal(bank->key, bank_key)) {
            if (program_index >= bank->program_count) {
                return PROGRAM_RUNTIME_INVALID_ARGUMENT;
            }
            cursor->bank_index = index;
            cursor->program_index = program_index;
            return PROGRAM_RUNTIME_OK;
        }
    }
    return PROGRAM_RUNTIME_BANK_NOT_FOUND;
}
