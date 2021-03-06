// Copyright (c) 2020-2021, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#include "compileconfig.h"

#define _FILE_OFFSET_BITS 64
#ifndef __USE_LARGEFILE64
#define __USE_LARGEFILE64 1
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#define _LARGEFILE_SOURCE
#if defined(_WIN32) || defined(_WIN64)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko64
#define ftell64 ftello64
#endif

#include "physfs.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archiver.h"
#include "filesys.h"
#include "filesys32.h"
#include "nonlocale.h"
#include "vfs.h"
#include "vfspak.h"
#include "vfspartialfileio.h"


static uint64_t _pakcounter = 0;

int vfs_AddPakEx(
        const h64wchar *path, int64_t pathlen,
        uint64_t start_offset, uint64_t max_len,
        int ignore_extension) {
    // Test if path looks non-bogus:
    h64wchar _h64pak[] = {'.', 'h', '6', '4', 'p', 'a', 'k'};
    if (!path || pathlen < (int64_t)strlen(".h64pak") ||
            (!ignore_extension &&
             memcmp(path + pathlen - strlen(".h64pak"),
                _h64pak, sizeof(*_h64pak) * strlen(".h64pak")) != 0))
        return 0;

    // See if target is a file:
    int _fileexists = 0;
    int _isdir = 0;
    if (!filesys32_TargetExists(path, pathlen, &_fileexists) ||
            !filesys32_IsDirectory(path, pathlen, &_isdir)) {
        #if defined(DEBUG_VFS) && !defined(NDEBUG)
        h64printf("horse64/vfs.c: debug: "
            "I/O error looking at resource pack: %s\n", path);
        #endif
        return 0;
    }
    if (!_fileexists || _isdir)
        return 0;

    // Ok, attempt to add:
    #if defined(DEBUG_VFS) && !defined(NDEBUG)
    h64printf("horse64/vfs.c: debug: "
           "adding resource pack: %s\n", path);
    #endif
    int innererr = 0;
    FILE *f = filesys32_OpenFromPath(path, pathlen, "rb", &innererr);
    if (!f) {
        return 0;
    }
    PHYSFS_Io *io = (PHYSFS_Io *)(
        _PhysFS_Io_partialFileReadOnlyStruct(f, start_offset, max_len)
    );
    if (!io) {
        fclose(f);
        return 0;
    }
    fclose(f);
    f = NULL;
    char pakname[128];
    snprintf(
        pakname, sizeof(pakname) - 1, "m%" PRIu64 ".zip",
        _pakcounter
    );
    _pakcounter++;
    if (!PHYSFS_mountIo(io, pakname, "/", 1)) {
        io->destroy(io);
        return 0;
    }

    return 1;
}

int vfs_AddPakStdioEx(
        FILE *origf, uint64_t start_offset, uint64_t max_len
        ) {
    // Ok, attempt to add:
    #if defined(DEBUG_VFS) && !defined(NDEBUG)
    h64printf("horse64/vfs.c: debug: "
           "adding resource pack: FILE %p\n", origf);
    #endif
    FILE *f = _dupfhandle(origf, "rb");
    if (!f) {
        return 0;
    }
    if (fseek64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    PHYSFS_Io *io = (PHYSFS_Io *)(
        _PhysFS_Io_partialFileReadOnlyStruct(f, start_offset, max_len)
    );
    if (!io) {
        fclose(f);
        return 0;
    }
    fclose(f);
    f = NULL;
    char pakname[128];
    snprintf(
        pakname, sizeof(pakname) - 1, "m%" PRIu64 ".zip",
        _pakcounter
    );
    _pakcounter++;
    if (!PHYSFS_mountIo(io, pakname, "/", 1)) {
        io->destroy(io);
        return 0;
    }

    return 1;
}

int vfs_AddPak(const h64wchar *path, int64_t pathlen) {
    return vfs_AddPakEx(path, pathlen, 0, 0, 0);
}


int vfs_GetEmbbeddedPakInfoByStdioFile(
        FILE *f, embeddedvfspakinfo **einfo
        ) {
    VFSFILE *vfsf = vfs_ownThisFD(f, "rb");
    if (!vfsf) {
        *einfo = NULL;
        return 0;
    }
    int result = vfs_GetEmbbeddedPakInfoByVFSFile(
        vfsf, einfo
    );
    vfs_DetachFD(vfsf);
    vfs_fclose(vfsf);
    return result;
}

int _vfs_GetEmbbeddedPakInfoByVFSFile_Do(
        VFSFILE *f, int64_t end_offset, embeddedvfspakinfo **einfo
        ) {
    #if defined(DEBUG_VFS) && !defined(NDEBUG)
    h64printf("horse64/vfs.c: debug: "
           "checking for pack with end_offset %" PRId64 " "
           "in: VFSILE %p\n", end_offset, f);
    #endif
    if (!vfs_fseektoend(f)) {
        *einfo = NULL;
        return 0;
    }
    int64_t file_len = vfs_ftell(f);
    if (file_len <= 0) {
        *einfo = NULL;
        return 0;
    }
    #if defined(DEBUG_VFS) && !defined(NDEBUG)
    h64printf("horse64/vfs.c: debug: "
           "have size %" PRId64 " "
           "for file: VFSILE %p\n", file_len, f);
    #endif
    uint64_t end_check = file_len - end_offset;
    char magic_pakappend[] = (
        "\x00\xFF\x00H64PAKAPPEND_V1\x00\xFF\x00"
    );
    int magiclen = strlen("\x01\xFF\x01H64PAKAPPEND_V1\x01\xFF\x01");
    if (end_check < magiclen + sizeof(uint64_t) * 2) {
        *einfo = NULL;
        return 1;  // no pak append header, so no pak to find.
    }
    if (vfs_fseek(f,
            end_check - magiclen - sizeof(uint64_t) * 2
            ) < 0) {
        *einfo = NULL;
        return 0;
    }
    char comparedata[256] = {0};
    if (vfs_fread(comparedata,
            magiclen + sizeof(uint64_t) * 2,
            1, f) != 1) {
        *einfo = NULL;
        return 0;
    }
    if (memcmp(comparedata + sizeof(uint64_t) * 2, magic_pakappend,
            magiclen) != 0) {
        *einfo = NULL;
        return 1;  // no pak append header, so no pak to find.
    }
    uint64_t pak_start, pak_end;
    memcpy(&pak_start, comparedata, sizeof(pak_start));
    memcpy(&pak_end, comparedata + sizeof(pak_start), sizeof(pak_end));
    if (pak_end != end_check - magiclen -
            sizeof(uint64_t) * 2 ||
            pak_start >= pak_end || end_check - pak_start <= 0) {
        *einfo = NULL;
        return 1;  // wrong pak offsets, so no working pak appended
    }
    
    *einfo = malloc(sizeof(**einfo));
    if (!*einfo) {
        return 0;
    }
    memset(*einfo, 0, sizeof(**einfo));
    (*einfo)->data_start_offset = pak_start;
    (*einfo)->data_end_offset = pak_end;
    (*einfo)->full_with_header_start_offset = pak_start;
    (*einfo)->full_with_header_end_offset = (
        pak_end + magiclen + sizeof(uint64_t) * 2
    );
    embeddedvfspakinfo *next_einfo = NULL;
    if (!_vfs_GetEmbbeddedPakInfoByVFSFile_Do(
            f, file_len - pak_start, &next_einfo
            )) {
        free(*einfo);
        *einfo = NULL;
        return 0;
    }
    (*einfo)->next = next_einfo;
    return 1;
}

int vfs_GetEmbbeddedPakInfoByVFSFile(
        VFSFILE *f, embeddedvfspakinfo **einfo
        ) {
    return _vfs_GetEmbbeddedPakInfoByVFSFile_Do(
        f, 0, einfo
    );
}

int vfs_GetEmbbeddedPakInfo(
        const h64wchar *path, int64_t pathlen,
        embeddedvfspakinfo **einfo
        ) {
    VFSFILE *f = vfs_fopen_u32(
        path, pathlen, "rb",
        VFSFLAG_NO_VIRTUALPAK_ACCESS
    );
    if (!f) {
        *einfo = NULL;
        return 0;
    }
    int result = vfs_GetEmbbeddedPakInfoByVFSFile(
        f, einfo
    );
    vfs_fclose(f);
    return result;
}

int vfs_HasEmbbededPakThatContainsFilePath(
        embeddedvfspakinfo *einfo,
        const h64wchar *binary_path,
        int64_t binary_path_len,
        const char *file_path, int *out_result
        ) {
    h64archive *a = archive_FromFilePathSlice(
        binary_path, binary_path_len,
        einfo->data_start_offset,
        einfo->data_end_offset - einfo->data_start_offset,
        0, 0, H64ARCHIVE_TYPE_AUTODETECT
    );
    if (!a) {
        return 0;
    }
    int existsasfolder = 0;
    int64_t idx = -1;
    if (!h64archive_GetEntryIndex(a, file_path, &idx,
            &existsasfolder)) {
        h64archive_Close(a);
        return 0;
    }
    h64archive_Close(a);
    *out_result = (idx >= 0 || existsasfolder);
    return 1;
}

int vfs_HasEmbbededPakThatContainsFilePath_Stdio(
        embeddedvfspakinfo *einfo,
        FILE *binary_file,
        const char *file_path, int *out_result
        ) {
    h64archive *a = archive_FromFileHandleSlice(
        binary_file, einfo->data_start_offset,
        einfo->data_end_offset - einfo->data_start_offset,
        H64ARCHIVE_TYPE_AUTODETECT, 0
    );
    if (!a) {
        return 0;
    }
    int existsasfolder = 0;
    int64_t idx = -1;
    if (!h64archive_GetEntryIndex(a, file_path, &idx,
            &existsasfolder)) {
        h64archive_Close(a);
        return 0;
    }
    h64archive_Close(a);
    *out_result = (idx >= 0 || existsasfolder);
    return 1;
}

void vfs_FreeEmbeddedPakInfo(embeddedvfspakinfo *einfo) {
    if (!einfo)
        return;
    vfs_FreeEmbeddedPakInfo(einfo->next);
    free(einfo);
}

int vfs_AddPaksEmbeddedInBinary(
        FILE *binhandle
        ) {
    embeddedvfspakinfo *einfo = NULL;
    int result = (
        vfs_GetEmbbeddedPakInfoByStdioFile(binhandle, &einfo)
    );
    if (!result)
        return 0;
    embeddedvfspakinfo *einfo_orig = einfo;
    while (einfo) {
        if (!vfs_AddPakStdioEx(
                binhandle, einfo->data_start_offset,
                einfo->data_end_offset - einfo->data_start_offset
                )) {
            vfs_FreeEmbeddedPakInfo(einfo_orig);
            return 0;
        }
        einfo = einfo->next;
    }
    vfs_FreeEmbeddedPakInfo(einfo_orig);
    return 1;
}