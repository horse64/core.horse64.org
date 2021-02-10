// Copyright (c) 2020, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#include "compileconfig.h"

#include <stdlib.h>
#include <string.h>

#include "compiler/warningconfig.h"
#include "widechar.h"


void warningconfig_Init(h64compilewarnconfig *wconfig) {
    memset(wconfig, 0, sizeof(*wconfig));
    wconfig->warn_shadowing_direct_locals = 1;
    wconfig->warn_shadowing_parent_func_locals = 0;
    wconfig->warn_shadowing_globals = 0;
    wconfig->warn_unrecognized_escape_sequences = 1;
}

int warningconfig_ProcessOptionU32(
        h64compilewarnconfig *wconfig, const h64wchar *option,
        int64_t optionlen
        ) {
    char *conv = AS_U8(option, optionlen);
    if (!conv)
        return 0;
    int result = warningconfig_ProcessOption(wconfig, conv);
    free(conv);
    return result;
}

int warningconfig_ProcessOption(
        h64compilewarnconfig *wconfig, const char *option
        ) {
    if (!option || strlen(option) < strlen("-W") ||
            option[0] != '-' || option[1] != 'W')
        return 0;

    int enable_warning = 1;
    char warning_name[64];
    unsigned int copylen = strlen(option + 2) + 1;
    if (copylen > sizeof(warning_name)) copylen = sizeof(warning_name);
    memcpy(warning_name, option + 2, copylen);
    warning_name[sizeof(warning_name) - 1] = '\0';
    if (strlen(warning_name) > strlen("no-") &&
            memcmp(warning_name, "no-", strlen("no-")) == 0) {
        enable_warning = 0;
        memmove(warning_name, warning_name + strlen("no-"),
                sizeof(warning_name) - strlen("no-"));
    }

    if (strcmp(warning_name, "shadowing-direct-locals") == 0) {
        wconfig->warn_shadowing_direct_locals = enable_warning;
        return 1;
    } else if (strcmp(warning_name, "shadowing-parent-func-locals") == 0) {
        wconfig->warn_shadowing_parent_func_locals = enable_warning;
        return 1;
    } else if (strcmp(warning_name, "shadowing-globals") == 0) {
        wconfig->warn_shadowing_globals = enable_warning;
        return 1;
    } else if (strcmp(warning_name, "shadowing-all") == 0) {
        wconfig->warn_shadowing_direct_locals = enable_warning;
        wconfig->warn_shadowing_parent_func_locals = enable_warning;
        wconfig->warn_shadowing_globals = enable_warning;
        return 1;
    } else if (strcmp(warning_name, "unrecognized-escape-sequences") == 0) {
        wconfig->warn_unrecognized_escape_sequences = enable_warning;
        return 1;
    } else if (strcmp(warning_name, "all") == 0) {
        wconfig->warn_shadowing_direct_locals = enable_warning;
        wconfig->warn_shadowing_parent_func_locals = enable_warning;
        wconfig->warn_shadowing_globals = enable_warning;
        wconfig->warn_unrecognized_escape_sequences = enable_warning;
        return 1;
    }
    return 0;
}
