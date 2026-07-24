#include "recovery_engine.h"

#include <stddef.h>

#include "crc32.h"
#include "ncr2_flash_layout.h"
#include "pedal_image.h"
#include "sha256.h"

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = UINT8_C(0);
    }
}

static void bytes_copy(void *destination, const void *source, size_t size)
{
    uint8_t *output = (uint8_t *)destination;
    const uint8_t *input = (const uint8_t *)source;

    for (size_t index = 0U; index < size; ++index) {
        output[index] = input[index];
    }
}

static int bytes_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *left_bytes = (const uint8_t *)left;
    const uint8_t *right_bytes = (const uint8_t *)right;
    uint8_t difference = UINT8_C(0);

    for (size_t index = 0U; index < size; ++index) {
        difference |=
            (uint8_t)(left_bytes[index] ^ right_bytes[index]);
    }
    return difference == UINT8_C(0);
}

static uint8_t packet_slot(const recovery_packet_t *packet)
{
    return (uint8_t)(packet->flags & RECOVERY_FLAG_SLOT_MASK);
}

static int command_uses_session(uint8_t command)
{
    return command != RECOVERY_COMMAND_GET_INFO &&
           command != RECOVERY_COMMAND_BEGIN_IMAGE;
}

static int command_allows_zero_length(uint8_t command)
{
    return command != RECOVERY_COMMAND_WRITE_CHUNK &&
           command != RECOVERY_COMMAND_READ_CHUNK;
}

static void make_response(const recovery_packet_t *request,
                          recovery_packet_t *response,
                          uint16_t status)
{
    bytes_zero(response, sizeof(*response));
    response->magic = RECOVERY_PACKET_MAGIC;
    response->version = RECOVERY_PROTOCOL_VERSION;
    response->command = request->command;
    response->flags = request->flags;
    response->session = request->session;
    response->sequence = request->sequence;
    response->offset = request->offset;
    response->status = status;
}

static uint16_t validate_common(const recovery_packet_t *request)
{
    uint16_t status =
        recovery_packet_validate(request, 0U, 0U, 0);

    if (status != RECOVERY_STATUS_OK) {
        return status;
    }
    if ((request->flags & ~RECOVERY_FLAG_ALLOWED_MASK) != UINT16_C(0)) {
        return RECOVERY_STATUS_BAD_FLAGS;
    }
    if (request->status != RECOVERY_STATUS_OK) {
        return RECOVERY_STATUS_INVALID_STATE;
    }
    if (command_allows_zero_length(request->command) != 0) {
        if (request->length != UINT16_C(0)) {
            return RECOVERY_STATUS_BAD_LENGTH;
        }
    } else if (request->length == UINT16_C(0)) {
        return RECOVERY_STATUS_BAD_LENGTH;
    }
    return RECOVERY_STATUS_OK;
}

static int backend_is_complete(const recovery_backend_t *backend)
{
    return backend->read != NULL &&
           backend->erase != NULL &&
           backend->program != NULL &&
           backend->store_boot_state != NULL;
}

static uint32_t next_session(recovery_engine_t *engine)
{
    uint32_t value = engine->session_seed;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    value += UINT32_C(0x9E3779B9);
    if (value == UINT32_C(0)) {
        value = UINT32_C(1);
    }
    engine->session_seed = value;
    return value;
}

static uint16_t resolve_target_range(const recovery_engine_t *engine,
                                     const recovery_packet_t *request,
                                     uint32_t *address)
{
    if (packet_slot(request) != engine->target_slot) {
        return RECOVERY_STATUS_BAD_SLOT;
    }
    return recovery_resolve_range(
        engine->target_slot,
        request->offset,
        request->length,
        address);
}

static int manifest_header_is_valid(
    const pedal_image_manifest_t *manifest)
{
    const uint32_t payload_capacity =
        NCR2_APPLICATION_SLOT_SIZE - NCR2_APPLICATION_MANIFEST_SIZE;
    const uint32_t expected_crc =
        crc32_compute(
            manifest,
            offsetof(pedal_image_manifest_t, header_crc32));

    return manifest->magic == PEDAL_IMAGE_MAGIC &&
           manifest->format_version == PEDAL_IMAGE_FORMAT_VERSION &&
           manifest->header_size == NCR2_APPLICATION_MANIFEST_SIZE &&
           manifest->board_id == PEDAL_IMAGE_BOARD_NCR2 &&
           manifest->load_address == NCR2_APPLICATION_LOAD_ADDRESS &&
           manifest->vector_offset == UINT32_C(0) &&
           manifest->image_size >= UINT32_C(8) &&
           manifest->image_size <= payload_capacity &&
           manifest->header_crc32 == expected_crc;
}

static int manifest_vector_is_valid(
    const pedal_image_manifest_t *manifest,
    const uint32_t vectors[2])
{
    const uint32_t initial_stack = vectors[0];
    const uint32_t reset_handler = vectors[1];
    const uint32_t reset_address =
        reset_handler & ~UINT32_C(1);
    const uint32_t image_end =
        manifest->load_address + manifest->image_size;

    return initial_stack >= NCR2_DTCM_START &&
           initial_stack <= NCR2_DTCM_END &&
           (initial_stack & UINT32_C(7)) == UINT32_C(0) &&
           (reset_handler & UINT32_C(1)) != UINT32_C(0) &&
           reset_address >= manifest->load_address &&
           reset_address < image_end;
}

static uint16_t validate_stored_image(recovery_engine_t *engine)
{
    pedal_image_manifest_t manifest;
    sha256_context_t sha;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t buffer[RECOVERY_PAYLOAD_SIZE];
    uint32_t vectors[2];
    uint32_t slot_address;
    uint32_t payload_address;
    uint32_t remaining;

    if (recovery_resolve_range(
            engine->target_slot,
            0U,
            (uint32_t)sizeof(manifest),
            &slot_address) != RECOVERY_STATUS_OK ||
        engine->backend.read(
            engine->backend.context,
            slot_address,
            &manifest,
            (uint32_t)sizeof(manifest)) != 0) {
        return RECOVERY_STATUS_BACKEND_ERROR;
    }
    if (!manifest_header_is_valid(&manifest)) {
        return RECOVERY_STATUS_IMAGE_INVALID;
    }
    if (engine->next_write_offset <
        NCR2_APPLICATION_MANIFEST_SIZE + manifest.image_size) {
        return RECOVERY_STATUS_IMAGE_INVALID;
    }

    payload_address =
        slot_address + NCR2_APPLICATION_MANIFEST_SIZE;
    if (engine->backend.read(
            engine->backend.context,
            payload_address,
            vectors,
            (uint32_t)sizeof(vectors)) != 0) {
        return RECOVERY_STATUS_BACKEND_ERROR;
    }
    if (!manifest_vector_is_valid(&manifest, vectors)) {
        return RECOVERY_STATUS_IMAGE_INVALID;
    }

    sha256_init(&sha);
    remaining = manifest.image_size;
    while (remaining != UINT32_C(0)) {
        uint32_t chunk = remaining;
        if (chunk > RECOVERY_PAYLOAD_SIZE) {
            chunk = RECOVERY_PAYLOAD_SIZE;
        }
        if (engine->backend.read(
                engine->backend.context,
                payload_address,
                buffer,
                chunk) != 0) {
            return RECOVERY_STATUS_BACKEND_ERROR;
        }
        sha256_update(&sha, buffer, chunk);
        payload_address += chunk;
        remaining -= chunk;
    }
    sha256_final(&sha, digest);
    if (!bytes_equal(
            digest,
            manifest.image_sha256,
            sizeof(digest))) {
        return RECOVERY_STATUS_IMAGE_INVALID;
    }
    return RECOVERY_STATUS_OK;
}

static uint16_t handle_get_info(recovery_engine_t *engine,
                                recovery_packet_t *response)
{
    recovery_info_t info;

    info.flash_size = NCR2_FLASH_SIZE;
    info.slot_size = NCR2_APPLICATION_SLOT_SIZE;
    info.slot_a_offset = NCR2_APPLICATION_A_OFFSET;
    info.slot_b_offset = NCR2_APPLICATION_B_OFFSET;
    info.manifest_size = NCR2_APPLICATION_MANIFEST_SIZE;
    info.confirmed_slot = engine->boot_state.confirmed_slot;
    info.pending_slot = engine->boot_state.pending_slot;
    info.selected_slot =
        boot_state_selected_slot(&engine->boot_state);
    info.update_phase = engine->phase;
    info.capabilities =
        RECOVERY_CAPABILITY_AB_SLOTS |
        RECOVERY_CAPABILITY_SHA256 |
        RECOVERY_CAPABILITY_READBACK |
        RECOVERY_CAPABILITY_RETRY_CACHE;
    info.max_chunk_size = RECOVERY_PAYLOAD_SIZE;

    response->length = (uint16_t)sizeof(info);
    bytes_copy(response->payload, &info, sizeof(info));
    return RECOVERY_STATUS_OK;
}

static uint16_t handle_begin(recovery_engine_t *engine,
                             const recovery_packet_t *request,
                             recovery_packet_t *response)
{
    const uint8_t target = packet_slot(request);

    if (target == engine->boot_state.confirmed_slot ||
        target == engine->active_slot) {
        return RECOVERY_STATUS_ACTIVE_SLOT;
    }
    engine->session = next_session(engine);
    engine->expected_sequence = UINT32_C(1);
    engine->next_write_offset = UINT32_C(0);
    engine->target_slot = target;
    engine->phase = RECOVERY_PHASE_BEGUN;
    engine->has_previous = UINT8_C(0);
    response->session = engine->session;
    return RECOVERY_STATUS_OK;
}

static uint16_t handle_erase(recovery_engine_t *engine)
{
    uint32_t slot_address;

    if (engine->phase != RECOVERY_PHASE_BEGUN &&
        engine->phase != RECOVERY_PHASE_ERASED) {
        return RECOVERY_STATUS_INVALID_STATE;
    }
    if (recovery_resolve_range(
            engine->target_slot,
            0U,
            NCR2_APPLICATION_SLOT_SIZE,
            &slot_address) != RECOVERY_STATUS_OK) {
        return RECOVERY_STATUS_BAD_SLOT;
    }
    if (engine->backend.erase(
            engine->backend.context,
            slot_address,
            NCR2_APPLICATION_SLOT_SIZE) != 0) {
        return RECOVERY_STATUS_BACKEND_ERROR;
    }
    engine->next_write_offset = UINT32_C(0);
    engine->phase = RECOVERY_PHASE_ERASED;
    return RECOVERY_STATUS_OK;
}

static uint16_t handle_write(recovery_engine_t *engine,
                             const recovery_packet_t *request)
{
    uint32_t address;
    uint16_t status;

    if (engine->phase != RECOVERY_PHASE_ERASED &&
        engine->phase != RECOVERY_PHASE_WRITING) {
        return RECOVERY_STATUS_INVALID_STATE;
    }
    if (request->offset != engine->next_write_offset) {
        return RECOVERY_STATUS_WRITE_ORDER;
    }
    status = resolve_target_range(engine, request, &address);
    if (status != RECOVERY_STATUS_OK) {
        return status;
    }
    if (engine->backend.program(
            engine->backend.context,
            address,
            request->payload,
            request->length) != 0) {
        return RECOVERY_STATUS_BACKEND_ERROR;
    }
    engine->next_write_offset += request->length;
    engine->phase = RECOVERY_PHASE_WRITING;
    return RECOVERY_STATUS_OK;
}

static uint16_t handle_read(recovery_engine_t *engine,
                            const recovery_packet_t *request,
                            recovery_packet_t *response)
{
    uint32_t address;
    uint16_t status;

    if (engine->phase < RECOVERY_PHASE_ERASED) {
        return RECOVERY_STATUS_INVALID_STATE;
    }
    status = resolve_target_range(engine, request, &address);
    if (status != RECOVERY_STATUS_OK) {
        return status;
    }
    if (engine->backend.read(
            engine->backend.context,
            address,
            response->payload,
            request->length) != 0) {
        return RECOVERY_STATUS_BACKEND_ERROR;
    }
    response->length = request->length;
    return RECOVERY_STATUS_OK;
}

static uint16_t handle_finalize(recovery_engine_t *engine)
{
    uint16_t status;

    if (engine->phase != RECOVERY_PHASE_WRITING) {
        return RECOVERY_STATUS_INVALID_STATE;
    }
    status = validate_stored_image(engine);
    if (status == RECOVERY_STATUS_OK) {
        engine->phase = RECOVERY_PHASE_FINALIZED;
    }
    return status;
}

static uint16_t handle_set_pending(recovery_engine_t *engine)
{
    boot_state_t next_state;

    if (engine->phase != RECOVERY_PHASE_FINALIZED) {
        return RECOVERY_STATUS_NOT_FINALIZED;
    }
    next_state = engine->boot_state;
    boot_state_begin_update(&next_state, engine->target_slot);
    if (next_state.pending_slot != engine->target_slot) {
        return RECOVERY_STATUS_INVALID_STATE;
    }
    if (engine->backend.store_boot_state(
            engine->backend.context,
            &next_state) != 0) {
        return RECOVERY_STATUS_BACKEND_ERROR;
    }
    engine->boot_state = next_state;
    engine->phase = RECOVERY_PHASE_PENDING;
    return RECOVERY_STATUS_OK;
}

static uint16_t dispatch_command(recovery_engine_t *engine,
                                 const recovery_packet_t *request,
                                 recovery_packet_t *response)
{
    switch (request->command) {
    case RECOVERY_COMMAND_GET_INFO:
        return handle_get_info(engine, response);
    case RECOVERY_COMMAND_BEGIN_IMAGE:
        return handle_begin(engine, request, response);
    case RECOVERY_COMMAND_ERASE_SLOT:
        return handle_erase(engine);
    case RECOVERY_COMMAND_WRITE_CHUNK:
        return handle_write(engine, request);
    case RECOVERY_COMMAND_READ_CHUNK:
        return handle_read(engine, request, response);
    case RECOVERY_COMMAND_FINALIZE_IMAGE:
        return handle_finalize(engine);
    case RECOVERY_COMMAND_SET_PENDING:
        return handle_set_pending(engine);
    case RECOVERY_COMMAND_REBOOT:
        if (engine->phase != RECOVERY_PHASE_PENDING) {
            return RECOVERY_STATUS_INVALID_STATE;
        }
        if (engine->backend.request_reboot != NULL) {
            engine->backend.request_reboot(engine->backend.context);
        }
        return RECOVERY_STATUS_OK;
    case RECOVERY_COMMAND_GET_LOG:
        return RECOVERY_STATUS_OK;
    default:
        return RECOVERY_STATUS_BAD_COMMAND;
    }
}

void recovery_engine_init(recovery_engine_t *engine,
                          const recovery_backend_t *backend,
                          const boot_state_t *boot_state,
                          uint8_t active_slot,
                          uint32_t session_seed)
{
    bytes_zero(engine, sizeof(*engine));
    if (backend != NULL) {
        engine->backend = *backend;
    }
    if (boot_state != NULL) {
        engine->boot_state = *boot_state;
    } else {
        boot_state_default(&engine->boot_state);
    }
    engine->active_slot = active_slot;
    engine->target_slot = BOOT_SLOT_NONE;
    engine->phase = RECOVERY_PHASE_IDLE;
    engine->session_seed =
        session_seed == UINT32_C(0)
            ? UINT32_C(0x4E584658)
            : session_seed;
}

void recovery_engine_process(recovery_engine_t *engine,
                             const recovery_packet_t *request,
                             recovery_packet_t *response)
{
    uint16_t status;
    int session_command;

    make_response(request, response, RECOVERY_STATUS_OK);
    status = validate_common(request);
    if (status != RECOVERY_STATUS_OK) {
        response->status = status;
        recovery_packet_finalize(response);
        return;
    }
    if (!backend_is_complete(&engine->backend)) {
        response->status = RECOVERY_STATUS_BACKEND_ERROR;
        recovery_packet_finalize(response);
        return;
    }

    if (engine->has_previous != UINT8_C(0) &&
        bytes_equal(
            request,
            &engine->previous_request,
            sizeof(*request))) {
        *response = engine->previous_response;
        return;
    }

    session_command = command_uses_session(request->command);
    if (session_command != 0) {
        if (engine->phase == RECOVERY_PHASE_IDLE) {
            status = RECOVERY_STATUS_INVALID_STATE;
        } else if (request->session != engine->session) {
            status = RECOVERY_STATUS_BAD_SESSION;
        } else if (request->sequence != engine->expected_sequence) {
            status = RECOVERY_STATUS_BAD_SEQUENCE;
        } else if (packet_slot(request) != engine->target_slot) {
            status = RECOVERY_STATUS_BAD_SLOT;
        } else {
            status = RECOVERY_STATUS_OK;
        }
        if (status != RECOVERY_STATUS_OK) {
            response->status = status;
            recovery_packet_finalize(response);
            return;
        }
    } else if (request->session != UINT32_C(0) ||
               request->sequence != UINT32_C(0)) {
        response->status = RECOVERY_STATUS_INVALID_STATE;
        recovery_packet_finalize(response);
        return;
    }

    status = dispatch_command(engine, request, response);
    response->status = status;
    recovery_packet_finalize(response);

    if (status == RECOVERY_STATUS_OK) {
        if (session_command != 0) {
            ++engine->expected_sequence;
        }
        engine->previous_request = *request;
        engine->previous_response = *response;
        engine->has_previous = UINT8_C(1);
    }
}
