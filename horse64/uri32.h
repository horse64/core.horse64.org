// Copyright (c) 2020, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef HORSE64_URI32_H_
#define HORSE64_URI32_H_

#include "widechar.h"

typedef struct uri32info {
    h64wchar *protocol;
    int64_t protocollen;
    h64wchar *host;
    int64_t hostlen;
    int16_t port;
    h64wchar *path;
    int64_t pathlen;
} uri32info;

uri32info *uri32_ParseEx(
    const h64wchar *uri, int64_t urilen,
    const char *default_remote_protocol
);

uri32info *uri32_Parse(
    const h64wchar *uri, int64_t urilen
);

h64wchar *uri32_Normalize(
    const h64wchar *uri, int64_t urilen,
    int absolutefilepaths, int64_t *out_len
);

void uri32_Free(uri32info *uri);

h64wchar *uri32_Dump(
    const uri32info *uri, int64_t *out_len
);

int uri32_Compare(
    const h64wchar *uri1str, int64_t uri1len,
    const h64wchar *uri2str, int64_t uri2len,
    int converttoabsolutefilepaths,
    int assumecasesensitivefilepaths, int *result
);

#endif  // HORSE64_URI32_H_
