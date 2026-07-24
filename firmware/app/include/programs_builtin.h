#ifndef NCR2_PROGRAMS_BUILTIN_H
#define NCR2_PROGRAMS_BUILTIN_H

#include "program_runtime.h"

#define PROGRAM_OPEN_CLEAN_ID UINT32_C(1)
#define PROGRAM_OPEN_BOOST_ID UINT32_C(2)
#define PROGRAM_OPEN_EDGE_ID UINT32_C(3)
#define PROGRAM_OPEN_CRUNCH_ID UINT32_C(4)
#define PROGRAM_OPEN_BOOSTED_CRUNCH_ID UINT32_C(5)
#define PROGRAM_OPEN_BLEND_DRIVE_ID UINT32_C(6)

#define PROGRAM_BANK_OPEN_STARTER_ID UINT32_C(1)

extern const program_catalog_t ncr2_builtin_program_catalog;
extern const program_bank_descriptor_t ncr2_starter_program_bank;
extern const program_library_t ncr2_builtin_program_library;

#endif
