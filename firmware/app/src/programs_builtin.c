#include "programs_builtin.h"

#include "effects_basic.h"

static const program_parameter_value_t clean_gain[] = {
    { EFFECT_GAIN_PARAMETER_GAIN, 1.0F },
};

static const program_node_descriptor_t clean_nodes[] = {
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_GAIN_ID,
        },
        clean_gain,
        sizeof(clean_gain) / sizeof(clean_gain[0]),
    },
};

static const program_parameter_value_t boost_gain[] = {
    { EFFECT_GAIN_PARAMETER_GAIN, 2.0F },
};

static const program_node_descriptor_t boost_nodes[] = {
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_GAIN_ID,
        },
        boost_gain,
        sizeof(boost_gain) / sizeof(boost_gain[0]),
    },
};

static const program_parameter_value_t edge_parameters[] = {
    { EFFECT_SOFT_CLIP_PARAMETER_DRIVE, 2.0F },
    { EFFECT_SOFT_CLIP_PARAMETER_MIX, 1.0F },
};

static const program_node_descriptor_t edge_nodes[] = {
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_SOFT_CLIP_ID,
        },
        edge_parameters,
        sizeof(edge_parameters) /
            sizeof(edge_parameters[0]),
    },
};

static const program_parameter_value_t crunch_parameters[] = {
    { EFFECT_SOFT_CLIP_PARAMETER_DRIVE, 6.0F },
    { EFFECT_SOFT_CLIP_PARAMETER_LEVEL, 1.0F },
};

static const program_node_descriptor_t crunch_nodes[] = {
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_SOFT_CLIP_ID,
        },
        crunch_parameters,
        sizeof(crunch_parameters) /
            sizeof(crunch_parameters[0]),
    },
};

static const program_parameter_value_t
boosted_crunch_gain[] = {
    { EFFECT_GAIN_PARAMETER_GAIN, 1.5F },
};

static const program_parameter_value_t
boosted_crunch_clip[] = {
    { EFFECT_SOFT_CLIP_PARAMETER_DRIVE, 8.0F },
    { EFFECT_SOFT_CLIP_PARAMETER_LEVEL, 0.9F },
};

static const program_node_descriptor_t
boosted_crunch_nodes[] = {
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_GAIN_ID,
        },
        boosted_crunch_gain,
        sizeof(boosted_crunch_gain) /
            sizeof(boosted_crunch_gain[0]),
    },
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_SOFT_CLIP_ID,
        },
        boosted_crunch_clip,
        sizeof(boosted_crunch_clip) /
            sizeof(boosted_crunch_clip[0]),
    },
};

static const program_parameter_value_t blend_drive[] = {
    { EFFECT_SOFT_CLIP_PARAMETER_DRIVE, 12.0F },
    { EFFECT_SOFT_CLIP_PARAMETER_MIX, 0.55F },
};

static const program_node_descriptor_t blend_drive_nodes[] = {
    {
        {
            EFFECT_VENDOR_OPEN,
            EFFECT_OPEN_BASIC_SOFT_CLIP_ID,
        },
        blend_drive,
        sizeof(blend_drive) / sizeof(blend_drive[0]),
    },
};

static const program_descriptor_t clean_program = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_CLEAN_ID,
    },
    .name = "Clean",
    .nodes = clean_nodes,
    .node_count =
        sizeof(clean_nodes) / sizeof(clean_nodes[0]),
};

static const program_descriptor_t boost_program = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_BOOST_ID,
    },
    .name = "Clean Boost",
    .nodes = boost_nodes,
    .node_count =
        sizeof(boost_nodes) / sizeof(boost_nodes[0]),
};

static const program_descriptor_t edge_program = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_EDGE_ID,
    },
    .name = "Edge",
    .nodes = edge_nodes,
    .node_count =
        sizeof(edge_nodes) / sizeof(edge_nodes[0]),
};

static const program_descriptor_t crunch_program = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_CRUNCH_ID,
    },
    .name = "Crunch",
    .nodes = crunch_nodes,
    .node_count =
        sizeof(crunch_nodes) / sizeof(crunch_nodes[0]),
};

static const program_descriptor_t boosted_crunch_program = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_BOOSTED_CRUNCH_ID,
    },
    .name = "Boosted Crunch",
    .nodes = boosted_crunch_nodes,
    .node_count =
        sizeof(boosted_crunch_nodes) /
        sizeof(boosted_crunch_nodes[0]),
};

static const program_descriptor_t blend_drive_program = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_BLEND_DRIVE_ID,
    },
    .name = "Blend Drive",
    .nodes = blend_drive_nodes,
    .node_count =
        sizeof(blend_drive_nodes) /
        sizeof(blend_drive_nodes[0]),
};

static const program_descriptor_t *const
builtin_programs[] = {
    &clean_program,
    &boost_program,
    &edge_program,
    &crunch_program,
    &boosted_crunch_program,
    &blend_drive_program,
};

const program_catalog_t ncr2_builtin_program_catalog = {
    .programs = builtin_programs,
    .count =
        sizeof(builtin_programs) /
        sizeof(builtin_programs[0]),
};

static const program_key_t starter_programs[] = {
    { EFFECT_VENDOR_OPEN, PROGRAM_OPEN_CLEAN_ID },
    { EFFECT_VENDOR_OPEN, PROGRAM_OPEN_BOOST_ID },
    { EFFECT_VENDOR_OPEN, PROGRAM_OPEN_EDGE_ID },
    { EFFECT_VENDOR_OPEN, PROGRAM_OPEN_CRUNCH_ID },
    {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_BOOSTED_CRUNCH_ID,
    },
    {
        EFFECT_VENDOR_OPEN,
        PROGRAM_OPEN_BLEND_DRIVE_ID,
    },
};

const program_bank_descriptor_t ncr2_starter_program_bank = {
    .key = {
        EFFECT_VENDOR_OPEN,
        PROGRAM_BANK_OPEN_STARTER_ID,
    },
    .name = "Open Starter",
    .programs = starter_programs,
    .program_count =
        sizeof(starter_programs) /
        sizeof(starter_programs[0]),
};

static const program_bank_descriptor_t *const
builtin_banks[] = {
    &ncr2_starter_program_bank,
};

const program_library_t ncr2_builtin_program_library = {
    .catalog = &ncr2_builtin_program_catalog,
    .banks = builtin_banks,
    .bank_count =
        sizeof(builtin_banks) / sizeof(builtin_banks[0]),
};
