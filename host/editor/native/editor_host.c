/*
 * Offline host harness for the effect editor.
 *
 * This program is host-only tooling. It links the real firmware
 * `effect_runtime` and `program_runtime` sources together with a
 * generated registry/program configuration, so the editor previews the
 * same code the pedal runs instead of a re-implementation.
 *
 * Host-only conveniences that firmware must never use (malloc, stdio,
 * clock_gettime) live in this file. They are outside the audio callback
 * under test: the measured region contains only `effect_chain_process`.
 *
 * Modes:
 *   --catalog   describe the registry as JSON on stdout
 *   --verify    validate registry/catalog/library/program, JSON on stdout
 *   --stream    retain effect state while processing length-framed chunks
 *   (default)   read float32 frames on stdin, write processed float32 on
 *               stdout, and write the JSON report on stderr
 */

/* clock_gettime is POSIX; the firmware sources stay strict ISO C. */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "effect_runtime.h"
#include "program_runtime.h"

#ifndef EDITOR_ARENA_BYTES
#define EDITOR_ARENA_BYTES (256U * 1024U)
#endif

#ifndef EDITOR_INSTANCE_CAPACITY
#define EDITOR_INSTANCE_CAPACITY 32U
#endif

#define EDITOR_MAX_OVERRIDES 256
#define EDITOR_READ_CHUNK (64U * 1024U)
#define EDITOR_STREAM_MAGIC UINT32_C(0x45564c31)
#define EDITOR_STREAM_MAX_BYTES (4U * 1024U * 1024U)

extern const effect_registry_t editor_registry;
extern const program_descriptor_t editor_program;
extern const program_catalog_t editor_catalog;
extern const program_library_t editor_library;

typedef struct parameter_override {
    size_t node;
    uint32_t parameter_id;
    float value;
    uint16_t status;
} parameter_override_t;

static effect_instance_t instances[EDITOR_INSTANCE_CAPACITY];
static _Alignas(EFFECT_RUNTIME_MAX_CONTEXT_ALIGNMENT)
    uint8_t arena[EDITOR_ARENA_BYTES];
static parameter_override_t overrides[EDITOR_MAX_OVERRIDES];
static size_t override_count;

static void print_json_string(FILE *stream, const char *text)
{
    fputc('"', stream);
    if (text != NULL) {
        for (const char *cursor = text; *cursor != '\0'; ++cursor) {
            unsigned char character = (unsigned char)*cursor;

            if (character == '"' || character == '\\') {
                fprintf(stream, "\\%c", character);
            } else if (character < 0x20U) {
                fprintf(stream, "\\u%04x", character);
            } else {
                fputc((int)character, stream);
            }
        }
    }
    fputc('"', stream);
}

static void print_json_float(FILE *stream, float value)
{
    /* JSON has no NaN or Infinity; report them as null. */
    if (value != value || value > 3.0e38F || value < -3.0e38F) {
        fputs("null", stream);
    } else {
        fprintf(stream, "%.9g", (double)value);
    }
}

static int value_is_finite(float value)
{
    return value == value && value < 3.0e38F && value > -3.0e38F;
}

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return UINT64_C(0);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int parse_override(const char *text)
{
    unsigned long node = 0UL;
    unsigned long parameter = 0UL;
    double value = 0.0;

    if (override_count >= (size_t)EDITOR_MAX_OVERRIDES) {
        return -1;
    }
    if (sscanf(text, "%lu:%lu:%lf", &node, &parameter, &value) != 3) {
        return -1;
    }
    overrides[override_count].node = (size_t)node;
    overrides[override_count].parameter_id = (uint32_t)parameter;
    overrides[override_count].value = (float)value;
    overrides[override_count].status = EFFECT_RUNTIME_OK;
    ++override_count;
    return 0;
}

static void print_catalog(void)
{
    const effect_registry_t *registry = &editor_registry;

    printf("{\"registry_status\":%u,\"effects\":[",
           (unsigned)effect_registry_validate(registry));
    for (size_t index = 0U; index < registry->count; ++index) {
        const effect_descriptor_t *effect = registry->effects[index];

        if (index != 0U) {
            fputc(',', stdout);
        }
        if (effect == NULL) {
            fputs("null", stdout);
            continue;
        }
        fputs("{\"name\":", stdout);
        print_json_string(stdout, effect->name);
        printf(",\"vendor_id\":%lu,\"effect_id\":%lu",
               (unsigned long)effect->key.vendor_id,
               (unsigned long)effect->key.effect_id);
        printf(",\"abi_version\":%u,\"context_size\":%lu",
               (unsigned)effect->abi_version,
               (unsigned long)effect->context_size);
        printf(",\"context_alignment\":%lu",
               (unsigned long)effect->context_alignment);
        printf(",\"has_initialize\":%d,\"has_reset\":%d",
               effect->initialize != NULL,
               effect->reset != NULL);
        fputs(",\"parameters\":[", stdout);
        for (uint16_t parameter = UINT16_C(0);
             parameter < effect->parameter_count;
             ++parameter) {
            const effect_parameter_descriptor_t *descriptor =
                &effect->parameters[parameter];

            if (parameter != UINT16_C(0)) {
                fputc(',', stdout);
            }
            printf("{\"parameter_id\":%lu,\"name\":",
                   (unsigned long)descriptor->parameter_id);
            print_json_string(stdout, descriptor->name);
            fputs(",\"unit\":", stdout);
            print_json_string(stdout, descriptor->unit);
            fputs(",\"minimum\":", stdout);
            print_json_float(stdout, descriptor->minimum);
            fputs(",\"maximum\":", stdout);
            print_json_float(stdout, descriptor->maximum);
            fputs(",\"default_value\":", stdout);
            print_json_float(stdout, descriptor->default_value);
            fputc('}', stdout);
        }
        fputs("]}", stdout);
    }
    fputs("]}\n", stdout);
}

static void print_chain_nodes(FILE *stream, const effect_chain_t *chain)
{
    fputs("\"nodes\":[", stream);
    for (size_t index = 0U; index < chain->count; ++index) {
        const effect_descriptor_t *effect =
            chain->instances[index].descriptor;

        if (index != 0U) {
            fputc(',', stream);
        }
        if (effect == NULL) {
            fputs("null", stream);
            continue;
        }
        fputs("{\"name\":", stream);
        print_json_string(stream, effect->name);
        fprintf(stream,
                ",\"vendor_id\":%lu,\"effect_id\":%lu"
                ",\"context_size\":%lu}",
                (unsigned long)effect->key.vendor_id,
                (unsigned long)effect->key.effect_id,
                (unsigned long)effect->context_size);
    }
    fputc(']', stream);
}

static void print_overrides(FILE *stream)
{
    fputs("\"parameter_results\":[", stream);
    for (size_t index = 0U; index < override_count; ++index) {
        if (index != 0U) {
            fputc(',', stream);
        }
        fprintf(stream,
                "{\"node\":%lu,\"parameter_id\":%lu,\"status\":%u"
                ",\"value\":",
                (unsigned long)overrides[index].node,
                (unsigned long)overrides[index].parameter_id,
                (unsigned)overrides[index].status);
        print_json_float(stream, overrides[index].value);
        fputc('}', stream);
    }
    fputc(']', stream);
}

static uint16_t apply_overrides(effect_chain_t *chain)
{
    uint16_t first_failure = EFFECT_RUNTIME_OK;

    for (size_t index = 0U; index < override_count; ++index) {
        uint16_t status = effect_chain_set_parameter(
            chain,
            overrides[index].node,
            overrides[index].parameter_id,
            overrides[index].value);

        overrides[index].status = status;
        if (status != EFFECT_RUNTIME_OK &&
            first_failure == EFFECT_RUNTIME_OK) {
            first_failure = status;
        }
    }
    return first_failure;
}

static float *read_stdin_frames(size_t *sample_count)
{
    size_t capacity = EDITOR_READ_CHUNK;
    size_t used = 0U;
    unsigned char *buffer = (unsigned char *)malloc(capacity);

    *sample_count = 0U;
    if (buffer == NULL) {
        return NULL;
    }
    for (;;) {
        size_t read_bytes;

        if (used == capacity) {
            unsigned char *grown;

            capacity *= 2U;
            grown = (unsigned char *)realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
        }
        read_bytes =
            fread(&buffer[used], 1U, capacity - used, stdin);
        used += read_bytes;
        if (read_bytes == 0U) {
            break;
        }
    }
    *sample_count = used / sizeof(float);
    return (float *)buffer;
}

static int stream_read_exact(void *buffer, size_t bytes)
{
    unsigned char *destination = (unsigned char *)buffer;
    size_t completed = 0U;

    while (completed < bytes) {
        size_t count = fread(
            &destination[completed], 1U, bytes - completed, stdin);

        if (count == 0U) return -1;
        completed += count;
    }
    return 0;
}

static uint16_t stream_process_samples(
    effect_chain_t *chain,
    float *interleaved,
    size_t frame_count,
    uint32_t block_frames,
    uint8_t channel_count,
    float channel_storage[EFFECT_RUNTIME_MAX_CHANNELS][4096],
    effect_audio_block_t *block)
{
    for (size_t offset = 0U; offset < frame_count;) {
        size_t frames = frame_count - offset;

        if (frames > (size_t)block_frames) {
            frames = (size_t)block_frames;
        }
        for (uint8_t channel = UINT8_C(0);
             channel < channel_count;
             ++channel) {
            block->channels[channel] = channel_storage[channel];
            for (size_t frame = 0U; frame < frames; ++frame) {
                float sample = interleaved[
                    (offset + frame) * (size_t)channel_count +
                    (size_t)channel];

                channel_storage[channel][frame] =
                    value_is_finite(sample) ? sample : 0.0F;
            }
        }
        block->frame_count = (uint32_t)frames;
        block->channel_count = channel_count;
        {
            const uint16_t status = effect_chain_process(chain, block);

            if (status != EFFECT_RUNTIME_OK) return status;
        }
        for (uint8_t channel = UINT8_C(0);
             channel < channel_count;
             ++channel) {
            for (size_t frame = 0U; frame < frames; ++frame) {
                float sample = channel_storage[channel][frame];

                if (!value_is_finite(sample)) sample = 0.0F;
                interleaved[
                    (offset + frame) * (size_t)channel_count +
                    (size_t)channel] = sample;
            }
        }
        offset += frames;
    }
    return EFFECT_RUNTIME_OK;
}

static int run_stream(
    effect_chain_t *chain,
    uint16_t ready_status,
    uint32_t block_frames,
    uint8_t channel_count,
    float channel_storage[EFFECT_RUNTIME_MAX_CHANNELS][4096],
    effect_audio_block_t *block)
{
    const uint32_t ready[4] = {
        EDITOR_STREAM_MAGIC,
        (uint32_t)ready_status,
        (uint32_t)channel_count,
        block_frames,
    };
    float *payload = NULL;
    size_t capacity = 0U;

    if (fwrite(ready, sizeof(ready), 1U, stdout) != 1U) return 4;
    fflush(stdout);
    if (ready_status != EFFECT_RUNTIME_OK) return 5;

    for (;;) {
        uint32_t payload_bytes;
        size_t frame_count;
        uint16_t status;

        if (fread(&payload_bytes, sizeof(payload_bytes), 1U, stdin) != 1U) {
            break;
        }
        if (payload_bytes == UINT32_C(0)) break;
        if (payload_bytes > EDITOR_STREAM_MAX_BYTES ||
            payload_bytes %
                (sizeof(float) * (size_t)channel_count) != 0U) {
            free(payload);
            return 6;
        }
        if ((size_t)payload_bytes > capacity) {
            float *replacement = (float *)realloc(payload, payload_bytes);

            if (replacement == NULL) {
                free(payload);
                return 7;
            }
            payload = replacement;
            capacity = (size_t)payload_bytes;
        }
        if (stream_read_exact(payload, (size_t)payload_bytes) != 0) {
            free(payload);
            return 8;
        }
        frame_count = (size_t)payload_bytes /
            (sizeof(float) * (size_t)channel_count);
        status = stream_process_samples(
            chain,
            payload,
            frame_count,
            block_frames,
            channel_count,
            channel_storage,
            block);
        if (status != EFFECT_RUNTIME_OK) {
            free(payload);
            return 9;
        }
        if (fwrite(&payload_bytes, sizeof(payload_bytes), 1U, stdout) != 1U ||
            fwrite(payload, 1U, (size_t)payload_bytes, stdout) !=
                (size_t)payload_bytes) {
            free(payload);
            return 10;
        }
        fflush(stdout);
    }
    free(payload);
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t sample_rate = UINT32_C(48000);
    uint32_t block_frames = UINT32_C(64);
    uint8_t channel_count = UINT8_C(2);
    int catalog_mode = 0;
    int verify_mode = 0;
    int stream_mode = 0;
    uint16_t registry_status;
    uint16_t catalog_status;
    uint16_t library_status;
    uint16_t chain_status;
    uint16_t prepare_status;
    uint16_t parameter_status = EFFECT_RUNTIME_OK;
    uint16_t process_status = EFFECT_RUNTIME_OK;
    effect_chain_t chain;
    float *interleaved = NULL;
    size_t sample_count = 0U;
    size_t frame_count = 0U;
    float channel_storage[EFFECT_RUNTIME_MAX_CHANNELS][4096];
    effect_audio_block_t block;
    uint64_t worst_block_ns = UINT64_C(0);
    uint64_t total_block_ns = UINT64_C(0);
    uint64_t block_iterations = UINT64_C(0);
    uint64_t nonfinite_samples = UINT64_C(0);
    double input_square_sum = 0.0;
    double output_square_sum = 0.0;
    float input_peak = 0.0F;
    float output_peak = 0.0F;
    FILE *report = stderr;

    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];

        if (strcmp(argument, "--catalog") == 0) {
            catalog_mode = 1;
        } else if (strcmp(argument, "--verify") == 0) {
            verify_mode = 1;
        } else if (strcmp(argument, "--stream") == 0) {
            stream_mode = 1;
        } else if (strcmp(argument, "--sample-rate") == 0 &&
                   index + 1 < argc) {
            sample_rate = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argument, "--block-frames") == 0 &&
                   index + 1 < argc) {
            block_frames = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argument, "--channels") == 0 &&
                   index + 1 < argc) {
            channel_count = (uint8_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argument, "--set") == 0 &&
                   index + 1 < argc) {
            if (parse_override(argv[++index]) != 0) {
                fputs("{\"error\":\"bad-override\"}\n", stderr);
                return 2;
            }
        } else {
            fputs("{\"error\":\"bad-argument\"}\n", stderr);
            return 2;
        }
    }

    if (catalog_mode) {
        print_catalog();
        return 0;
    }
    if (verify_mode) {
        report = stdout;
    }
    if (channel_count == UINT8_C(0) ||
        channel_count > EFFECT_RUNTIME_MAX_CHANNELS ||
        block_frames == UINT32_C(0) ||
        block_frames > (uint32_t)(sizeof(channel_storage[0]) /
                                  sizeof(float)) ||
        sample_rate == UINT32_C(0)) {
        fputs("{\"error\":\"bad-format\"}\n", report);
        return 2;
    }

    registry_status = effect_registry_validate(&editor_registry);
    catalog_status =
        program_catalog_validate(&editor_catalog, &editor_registry);
    library_status =
        program_library_validate(&editor_library, &editor_registry);
    chain_status = effect_chain_initialize(
        &chain,
        &editor_registry,
        instances,
        (size_t)EDITOR_INSTANCE_CAPACITY,
        arena,
        sizeof(arena),
        sample_rate,
        block_frames);
    prepare_status = EFFECT_RUNTIME_OK;
    if (chain_status == EFFECT_RUNTIME_OK) {
        prepare_status = program_prepare(&chain, &editor_program);
        if (prepare_status == PROGRAM_RUNTIME_OK) {
            parameter_status = apply_overrides(&chain);
        }
    } else {
        chain.count = 0U;
        chain.arena_used = 0U;
        chain.instances = instances;
    }

    if (stream_mode) {
        uint16_t ready_status = registry_status;

        if (ready_status == EFFECT_RUNTIME_OK) ready_status = catalog_status;
        if (ready_status == EFFECT_RUNTIME_OK) ready_status = library_status;
        if (ready_status == EFFECT_RUNTIME_OK) ready_status = chain_status;
        if (ready_status == EFFECT_RUNTIME_OK) ready_status = prepare_status;
        if (ready_status == EFFECT_RUNTIME_OK) ready_status = parameter_status;
        return run_stream(
            &chain,
            ready_status,
            block_frames,
            channel_count,
            channel_storage,
            &block);
    }

    if (!verify_mode &&
        registry_status == EFFECT_RUNTIME_OK &&
        prepare_status == PROGRAM_RUNTIME_OK &&
        chain_status == EFFECT_RUNTIME_OK) {
        interleaved = read_stdin_frames(&sample_count);
        if (interleaved == NULL && sample_count != 0U) {
            fputs("{\"error\":\"out-of-memory\"}\n", report);
            return 3;
        }
        frame_count = sample_count / (size_t)channel_count;

        for (size_t offset = 0U; offset < frame_count;) {
            size_t frames = frame_count - offset;

            if (frames > (size_t)block_frames) {
                frames = (size_t)block_frames;
            }
            for (uint8_t channel = UINT8_C(0);
                 channel < channel_count;
                 ++channel) {
                block.channels[channel] = channel_storage[channel];
                for (size_t frame = 0U; frame < frames; ++frame) {
                    float sample = interleaved
                        [(offset + frame) * channel_count + channel];

                    channel_storage[channel][frame] = sample;
                    if (value_is_finite(sample)) {
                        float magnitude =
                            sample < 0.0F ? -sample : sample;

                        input_square_sum +=
                            (double)sample * (double)sample;
                        if (magnitude > input_peak) {
                            input_peak = magnitude;
                        }
                    }
                }
            }
            block.frame_count = (uint32_t)frames;
            block.channel_count = channel_count;

            {
                uint64_t started = monotonic_nanoseconds();
                uint16_t status = effect_chain_process(&chain, &block);
                uint64_t elapsed = monotonic_nanoseconds() - started;

                if (elapsed > worst_block_ns) {
                    worst_block_ns = elapsed;
                }
                total_block_ns += elapsed;
                ++block_iterations;
                if (status != EFFECT_RUNTIME_OK) {
                    process_status = status;
                    break;
                }
            }

            for (uint8_t channel = UINT8_C(0);
                 channel < channel_count;
                 ++channel) {
                for (size_t frame = 0U; frame < frames; ++frame) {
                    float sample = channel_storage[channel][frame];

                    if (!value_is_finite(sample)) {
                        ++nonfinite_samples;
                        sample = 0.0F;
                    } else {
                        float magnitude =
                            sample < 0.0F ? -sample : sample;

                        output_square_sum +=
                            (double)sample * (double)sample;
                        if (magnitude > output_peak) {
                            output_peak = magnitude;
                        }
                    }
                    interleaved
                        [(offset + frame) * channel_count + channel] =
                            sample;
                }
            }
            offset += frames;
        }

        if (frame_count != 0U) {
            fwrite(
                interleaved,
                sizeof(float),
                frame_count * (size_t)channel_count,
                stdout);
        }
        fflush(stdout);
    }

    fprintf(report,
            "{\"registry_status\":%u,\"catalog_status\":%u"
            ",\"library_status\":%u,\"chain_status\":%u"
            ",\"prepare_status\":%u,\"parameter_status\":%u"
            ",\"process_status\":%u,",
            (unsigned)registry_status,
            (unsigned)catalog_status,
            (unsigned)library_status,
            (unsigned)chain_status,
            (unsigned)prepare_status,
            (unsigned)parameter_status,
            (unsigned)process_status);
    print_chain_nodes(report, &chain);
    fputc(',', report);
    print_overrides(report);
    fprintf(report,
            ",\"sample_rate\":%lu,\"block_frames\":%lu"
            ",\"channels\":%u,\"frames\":%lu,\"blocks\":%llu"
            ",\"worst_block_ns\":%llu,\"mean_block_ns\":%llu"
            ",\"deadline_ns\":%llu,\"nonfinite_samples\":%llu"
            ",\"arena_size\":%lu,\"arena_used\":%lu"
            ",\"instance_capacity\":%lu,\"instance_count\":%lu",
            (unsigned long)sample_rate,
            (unsigned long)block_frames,
            (unsigned)channel_count,
            (unsigned long)frame_count,
            (unsigned long long)block_iterations,
            (unsigned long long)worst_block_ns,
            (unsigned long long)(block_iterations == UINT64_C(0)
                                     ? UINT64_C(0)
                                     : total_block_ns /
                                           block_iterations),
            (unsigned long long)((uint64_t)block_frames *
                                 UINT64_C(1000000000) /
                                 (uint64_t)sample_rate),
            (unsigned long long)nonfinite_samples,
            (unsigned long)sizeof(arena),
            (unsigned long)chain.arena_used,
            (unsigned long)EDITOR_INSTANCE_CAPACITY,
            (unsigned long)chain.count);
    fputs(",\"peak_input\":", report);
    print_json_float(report, input_peak);
    fputs(",\"peak_output\":", report);
    print_json_float(report, output_peak);
    fputs(",\"rms_input\":", report);
    print_json_float(
        report,
        frame_count == 0U
            ? 0.0F
            : (float)sqrt(input_square_sum /
                          (double)(frame_count * channel_count)));
    fputs(",\"rms_output\":", report);
    print_json_float(
        report,
        frame_count == 0U
            ? 0.0F
            : (float)sqrt(output_square_sum /
                          (double)(frame_count * channel_count)));
    fputs("}\n", report);
    fflush(report);

    free(interleaved);
    return 0;
}
