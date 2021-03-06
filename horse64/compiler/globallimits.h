// Copyright (c) 2020-2021, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef HORSE64_COMPILER_GLOBALLIMITS_H_
#define HORSE64_COMPILER_GLOBALLIMITS_H_

#include <limits.h>
#include <stdint.h>

#define H64LIMIT_SOURCEFILESIZE (10 * 1024 * 1024)
#define H64LIMIT_IDENTIFIERLEN (256)
#define H64LIMIT_MAXPARSERECURSION 64
#define H64LIMIT_IMPORTCHAINLEN 32
#define H64LIMIT_MAXCLASSES ((INT32_MAX / 2) - 2)
#define H64LIMIT_MAXFUNCS ((INT32_MAX / 2) - 2)
#define H64LIMIT_MAXGLOBALVAR ((INT32_MAX / 2) - 2)
#define H64LIMIT_MAX_CLASS_VARATTRS ((INT16_MAX / 4) - 2)
#define H64LIMIT_MAX_CLASS_FUNCATTRS ((INT16_MAX / 4) - 2)

#define CFUNC_ASYNCDATA_DEFAULTITEMSIZE 64
#define ASYNCSYSJOB_WORKER_COUNT 6

typedef int16_t attridx_t;
typedef int32_t classid_t;
typedef int32_t funcid_t;
typedef int32_t globalvarid_t;

#endif  // HORSE64_COMPILER_GLOBALLIMITS_H_
