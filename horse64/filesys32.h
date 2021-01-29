// Copyright (c) 2020-2021, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef HORSE64_FILESYS32_H_
#define HORSE64_FILESYS32_H_

#include <stdint.h>
#include <stdio.h>

#include "widechar.h"

int filesys32_TargetExists(
    const h64wchar *path, int64_t pathlen, int *result
);

void filesys32_FreeFolderList(
    h64wchar **list, int64_t *listlen
);

enum {
    FS32_CHDIRERR_SUCCESS = 0,
    FS32_CHDIRERR_NOPERMISSION = -1,
    FS32_CHDIRERR_TARGETNOTADIRECTORY = -2,
    FS32_CHDIRERR_OUTOFMEMORY = -3,
    FS32_CHDIRERR_OTHERERROR = -4
};

int filesys32_ChangeDirectory(
    h64wchar *path, int64_t pathlen
);

enum {
    FS32_MKDIRERR_SUCCESS = 0,
    FS32_MKDIRERR_OUTOFMEMORY = -1,
    FS32_MKDIRERR_NOPERMISSION = -2,
    FS32_MKDIRERR_TARGETALREADYEXISTS = -3,
    FS32_MKDIRERR_OUTOFFDS = -4,
    FS32_MKDIRERR_PARENTSDONTEXIST = -5,
    FS32_MKDIRERR_INVALIDNAME = -6,
    FS32_MKDIRERR_OTHERERROR = -7
};

int filesys32_CreateDirectory(
    h64wchar *path, int64_t pathlen, int user_readable_only
);

enum {
    FS32_REMOVEDIR_SUCCESS = 0,
    FS32_REMOVEDIR_OUTOFMEMORY = -1,
    FS32_REMOVEDIR_NOPERMISSION = -2,
    FS32_REMOVEDIR_NOSUCHTARGET = -3,
    FS32_REMOVEDIR_OUTOFFDS = -4,
    FS32_REMOVEDIR_DIRISBUSY = -5,
    FS32_REMOVEDIR_NOTADIR = -6,
    FS32_REMOVEDIR_NONEMPTYDIRECTORY = -7,
    FS32_REMOVEDIR_OTHERERROR = -8
};

int filesys32_RemoveFolderRecursively(
    const h64wchar *path, int64_t pathlen, int *error
);

enum {
    FS32_REMOVEERR_SUCCESS = 0,
    FS32_REMOVEERR_OUTOFMEMORY = -1,
    FS32_REMOVEERR_NOPERMISSION = -2,
    FS32_REMOVEERR_NOSUCHTARGET = -3,
    FS32_REMOVEERR_NONEMPTYDIRECTORY = -4,
    FS32_REMOVEERR_OUTOFFDS = -5,
    FS32_REMOVEERR_DIRISBUSY = -6,
    FS32_REMOVEERR_OTHERERROR = -6
};

int filesys32_RemoveFileOrEmptyDir(
    const h64wchar *path, int64_t pathlen, int *error
);

enum {
    FS32_LISTFOLDERERR_SUCCESS = 0,
    FS32_LISTFOLDERERR_OUTOFMEMORY = -1,
    FS32_LISTFOLDERERR_NOPERMISSION = -2,
    FS32_LISTFOLDERERR_TARGETNOTDIRECTORY = -3,
    FS32_LISTFOLDERERR_OUTOFFDS = -4,
    FS32_LISTFOLDERERR_SYMLINKSWEREEXCLUDED = -5,
    FS32_LISTFOLDERERR_NOSUCHTARGET = -6,
    FS32_LISTFOLDERERR_OTHERERROR = -7
};

int filesys32_ListFolderEx(
    const h64wchar *path, int64_t pathlen,
    h64wchar ***contents, int64_t **contentslen,
    int returnFullPath, int allowsymlink, int *error
);

enum {
    FS32_CONTENTASSTR_SUCCESS = 0,
    FS32_CONTENTASSTR_OUTOFMEMORY = -1,
    FS32_CONTENTASSTR_NOPERMISSION = -2,
    FS32_CONTENTASSTR_TARGETNOTAFILE = -3,
    FS32_CONTENTASSTR_OUTOFFDS = -4,
    FS32_CONTENTASSTR_INVALIDFILENAME = -5,
    FS32_CONTENTASSTR_IOERROR = -6,
    FS32_CONTENTASSTR_OTHERERROR = -7
};

char *filesys23_ContentsAsStr(
    const h64wchar *path, int64_t pathlen, int *error
);

int filesys32_ListFolder(
    const h64wchar *path, int64_t pathlen,
    h64wchar ***contents, int64_t **contentslen,
    int returnFullPath, int *error
);

h64wchar *filesys32_RemoveDoubleSlashes(
    const h64wchar *path, int64_t pathlen,
    int couldbewinpath, int64_t *out_len
);

h64wchar *filesys32_NormalizeEx(
    const h64wchar *path, int64_t pathlen,
    int couldbewinpath,
    int64_t *out_len
);

h64wchar *filesys32_Normalize(
    const h64wchar *path, int64_t pathlen,
    int64_t *out_len
);

h64wchar *filesys32_ToAbsolutePath(
    const h64wchar *path, int64_t pathlen,
    int64_t *out_len    
);

int filesys32_WinApiInsensitiveCompare(
    const h64wchar *path1_o, int64_t path1len_o,
    const h64wchar *path2_o, int64_t path2len_o,
    int *wasoom
);

int filesys32_PathCompare(
    const h64wchar *p1, int64_t p1len,
    const h64wchar *p2, int64_t p2len
);

h64wchar *filesys32_Join(
    const h64wchar *path1, int64_t path1len,
    const h64wchar *path2, int64_t path2len,
    int64_t *out_len
);

int filesys32_IsAbsolutePath(const h64wchar *path, int64_t pathlen);

h64wchar *filesys32_GetCurrentDirectory(int64_t *out_len);

FILE *filesys32_OpenFromPath(
    const h64wchar *path, int64_t pathlen,
    const char *mode
);  // sets errno in case of errors (even on winows!!)

FILE *filesys32_TempFile(
    int subfolder,
    const h64wchar *prefix, int64_t prefixlen,
    const h64wchar *suffix, int64_t suffixlen,
    h64wchar **folder_path, int64_t* folder_path_len,
    h64wchar **path, int64_t *path_len
);

#endif  // HORSE64_FILESYS32_H_