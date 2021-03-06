// Copyright (c) 2020-2021, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#include "compileconfig.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/globallimits.h"
#include "compiler/lexer.h"
#include "compiler/operator.h"
#include "compiler/result.h"
#include "nonlocale.h"
#include "json.h"
#include "uri32.h"
#include "vfs.h"
#include "widechar.h"


static char INT64_MAX_STR[128];
static char INT64_MIN_STR[128];

static __attribute__((constructor)) void _set_maxmin_strs() {
    snprintf(
        INT64_MAX_STR, sizeof(INT64_MAX_STR) - 1,
        "%" PRId64, INT64_MAX
    );
    INT64_MAX_STR[sizeof(INT64_MAX_STR) - 1] = 0;
    snprintf(
        INT64_MIN_STR, sizeof(INT64_MIN_STR) - 1,
        "%" PRId64, INT64_MIN
    );
    INT64_MIN_STR[sizeof(INT64_MIN_STR) - 1] = 0;
}


static char _literaloverflowerror[] = (
    "unexpected number range overflow when parsing literal"
);


static int _tokenalloc(
        h64tokenizedfile *result,
        int *allocsize
        ) {
    int new_size = (*allocsize) * 2;
    if (new_size < 16) new_size = 16;
    h64token *newtokens = realloc(
        result->token, sizeof(*newtokens) * new_size
    );
    if (!newtokens)
        return 0;
    result->token = newtokens;
    int i = (*allocsize);
    while (i < new_size) {
        result->token[i].type = H64TK_INVALID;
        result->token[i].line = -1;
        result->token[i].column = -1;
        i++;
    }
    *allocsize = new_size;
    return 1;
}

static int is_identifier_char(uint8_t c) {
    if (c == '_')
        return 1;
    else if (c >= 'a' && c <= 'z')
        return 1;
    else if (c >= 'A' && c <= 'Z')
        return 1;
    else if (c > 127)
        return 1;  // possibly invalid utf8. but we have no other use anyway.
    return 0;
}

static int is_identifier_resume_char(uint8_t c) {
    return (
        is_identifier_char(c) || (c >= '0' && c <= '9')
    );
}

char *lexer_ParseStringLiteral(
        const char *literal,
        const h64wchar *fileuri, int64_t fileurilen,
        int line, int column,
        int isbinary,
        h64result *result,
        h64compilewarnconfig *wconfig,
        int *out_len
        ) {
    char *p = strdup(
        literal + 1 + (isbinary ? 1 : 0)
    );
    #ifndef NDEBUG
    int plen = strlen(p);
    #endif
    if (!p)
        return NULL;
    int k = 0;
    int i = 1 + (isbinary ? 1 : 0);
    while (i < (int)strlen(literal) - 1) {
        int charlen = 1;
        if (!isbinary)
            charlen = utf8_char_len((uint8_t*)&literal[i]);
        assert(charlen > 0);
        if (literal[i] != '\\') {
            if (literal[i] == '\n' || literal[i] == '\r') {
                p[k] = '\n';  // Translate all line breaks to \n
                line++;
                column = 1;
                k++;
                i++;
                if (p[k - 1] == '\r' &&
                        i < (int)strlen(literal) - 1 &&
                        literal[i] == '\n') {
                    // Treat windows \r\n as a single char.
                    i++;
                }
                continue;
            }
            column++;
            int j = 0;
            while (j < charlen) {
                #ifndef NDEBUG
                assert(k < plen);
                #endif
                p[k] = literal[i];
                k++;
                i++;
                j++;
            }
            continue;
        } else if (i + 1 < (int)strlen(literal) - 1) {
            column++;
            i++;
            if (literal[i] == 'n') {
                p[k] = '\n'; k++;
            } else if (literal[i] == 'r') {
                p[k] = '\r'; k++;
            } else if (literal[i] == 't') {
                p[k] = '\t'; k++;
            } else if (literal[i] == '\\') {
                p[k] = '\\'; k++;
            } else if (literal[i] == '"') {
                p[k] = '"'; k++;
            } else if (literal[i] == '\'') {
                p[k] = '\''; k++;
            } else if (literal[i] == 'u') {
                // Unicode literal, up to \uNNNNNNNN with
                // the value being hex. (Unsigned 32bit int.)
                int _columnlen = 0;
                i++;
                _columnlen++;
                char numdigits[9] = "";
                while (i < (int)strlen(literal) - 1 &&
                        ((literal[i] >= '0' &&
                          literal[i] <= '9') ||
                         (literal[i] >= 'a' &&
                          literal[i] <= 'f') ||
                         (literal[i] >= 'A' &&
                          literal[i] <= 'F')) &&
                        strlen(numdigits) < 8) {
                    numdigits[strlen(numdigits) + 1] = '\0';
                    numdigits[strlen(numdigits)] = literal[i];
                    i++;
                    _columnlen++;
                }
                if (strlen(numdigits) < 4) {
                    char buf[512];
                    snprintf(buf, sizeof(buf) - 1,
                        "invalid escape \"\\u\" not followed "
                        "by hex number of at least "
                        "4 digits was ignored "
                        "[-Wunrecognized-escape-sequences]");
                    if (!result_AddMessage(
                            result,
                            H64MSG_WARNING, buf,
                            fileuri, fileurilen, line, column
                            )) {
                        free(p);
                        return NULL;
                    }
                } else {
                    int64_t number = (int64_t)strtoll(numdigits, NULL, 16);
                    assert(number >= 0 && number <= UINT32_MAX);
                    char utf8buf[10] = "";
                    int utf8buflen;
                    int u8result = write_codepoint_as_utf8(
                        number, 0, 0, utf8buf, 9, &utf8buflen
                    );
                    if (!u8result || utf8buflen <= 0 ||
                            utf8buflen >= 10) {
                        char buf[512];
                        snprintf(buf, sizeof(buf) - 1,
                            "invalid escape \"\\u\" not followed "
                            "by a valid unicode code point "
                            "[-Wunrecognized-escape-sequences]");
                        if (!result_AddMessage(
                                result,
                                H64MSG_WARNING, buf,
                                fileuri, fileurilen,
                                line, column
                                )) {
                            free(p);
                            return NULL;
                        }
                    } else {
                        int i2 = 0;
                        while (i2 < utf8buflen) {
                            #ifndef NDEBUG
                            assert(k < plen);
                            #endif
                            p[k] = utf8buf[i2];
                            k++;
                            i2++;
                        }
                    }
                }
                column += _columnlen;
                continue;
            } else if (literal[i] == 'x') {
                // Binary byte literal, up to \xNN with value
                // being hex. (Unsigned 8bit int.)
                int _columnextralen = 0;
                char hexnum[3] = "";
                if (i + 1 < (int)strlen(literal) - 1 &&
                        ((literal[i + 1] >= '0' &&
                          literal[i + 1] <= '9') ||
                         (literal[i + 1] >= 'a' &&
                          literal[i + 1] <= 'f') ||
                         (literal[i + 1] >= 'A' &&
                          literal[i + 1] <= 'F'))) {
                    hexnum[1] = '\0';
                    hexnum[0] = literal[i + 1];
                    i++;
                    _columnextralen++;
                    if (i + 1 < (int)strlen(literal) - 1 &&
                             ((literal[i + 1] >= '0' &&
                               literal[i + 1] <= '9') ||
                              (literal[i + 1] >= 'a' &&
                               literal[i + 1] <= 'f') ||
                              (literal[i + 1] >= 'A' &&
                               literal[i + 1] <= 'F'))) {
                         hexnum[2] = '\0';
                         hexnum[1] = literal[i + 1];
                         i++;
                         _columnextralen++;
                     }
                }
                if (strlen(hexnum) == 0) {
                    char buf[512];
                    snprintf(buf, sizeof(buf) - 1,
                        "invalid escape \"\\x\" not followed "
                        "by hex number was ignored "
                        "[-Wunrecognized-escape-sequences]");
                    if (!result_AddMessage(
                            result,
                            H64MSG_WARNING, buf,
                            fileuri, fileurilen,
                            line, column
                            )) {
                        free(p);
                        return NULL;
                    }
                } else {
                    int number = (int)strtol(hexnum, 0, 16);
                    assert(number >= 0 && number < 256);
                    #ifndef NDEBUG
                    assert(k < plen);
                    #endif
                    p[k] = number;
                    k++;
                }
                column += _columnextralen;
            } else {
                if (result && wconfig &&
                        wconfig->warn_unrecognized_escape_sequences) {
                    char s[16];
                    snprintf(s, 15, "byte %d", literal[i]);
                    s[15] = '\0';
                    if (literal[i] >= 32 && literal[i] < 127 &&
                            literal[i] != '\'') {
                        snprintf(s, 15, "'%c'", literal[i]);
                        s[15] = '\0';
                    }
                    char buf[512];
                    snprintf(buf, sizeof(buf) - 1,
                        "unrecognized escape sequence '\\' followed "
                        "by %s was ignored "
                        "[-Wunrecognized-escape-sequences]", s);
                    if (!result_AddMessage(
                            result,
                            H64MSG_WARNING, buf,
                            fileuri, fileurilen,
                            line, column
                            )) {
                        free(p);
                        return NULL;
                    }
                }
                p[k] = '\\'; k++;
            }
            i++;
            column++;
            continue;
        } else {
            #ifndef NDEBUG
            assert(k < plen);
            #endif
            p[k] = '\\';
            k++;
            i++;
            column++;
            continue;
        }
        i++;
    }
    #ifndef NDEBUG
    assert(k <= plen);
    #endif
    p[k] = '\0';
    *out_len = k;
    return p;
}


h64tokenizedfile lexer_ParseFromFile(
        const uri32info *fileuri, h64compilewarnconfig *wconfig
        ) {
    h64tokenizedfile result;
    memset(&result, 0, sizeof(result));
    result.resultmsg.success = 1;

    int64_t fileuri_slen = 0;
    h64wchar *fileuri_s = uri32_Dump(
        fileuri, &fileuri_slen
    );
    if (!fileuri_s) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "out of memory converting URI",
            NULL, 0
        );
        free(fileuri_s);
        return result;
    }

    if (h64casecmp_u32u8(fileuri->protocol,
                fileuri->protocollen, "file") != 0 &&
            h64casecmp_u32u8(fileuri->protocol,
                fileuri->protocollen, "vfs") != 0) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "URI protocol unsupported",
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }
    int vfsflags = (
        h64casecmp_u32u8(fileuri->protocol,
            fileuri->protocollen, "file") == 0 ?
        VFSFLAG_NO_VIRTUALPAK_ACCESS :
        VFSFLAG_NO_REALDISK_ACCESS
    );
    int _vfs_exists = 0;
    if (!vfs_ExistsU32(
            fileuri->path, fileuri->pathlen,
            &_vfs_exists, vfsflags)) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "vfs_Exists() failed, out of memory?",
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }
    if (!_vfs_exists) {
        char *fileuri_s_u8 = AS_U8(
            fileuri_s, fileuri_slen
        );
        if (!fileuri_s_u8) {
            result_ErrorNoLoc(
                &result.resultmsg,
                "string conversion alloc fail",
                fileuri_s, fileuri_slen
            );
            free(fileuri_s);
            return result;
        }
        int bufferlen = (
            strlen("no such file: ") + strlen(fileuri_s_u8) + 1
        );
        char *buffer = malloc(bufferlen);
        if (!buffer) {
            result.resultmsg.success = 0;
            free(fileuri_s);
            free(fileuri_s_u8);
            return result;
        }
        snprintf(buffer, bufferlen,
                 "no such file: %s", fileuri_s_u8);
        result_ErrorNoLoc(
            &result.resultmsg,
            buffer,
            NULL, 0
        );
        free(buffer);
        assert(result.resultmsg.message_count == 1);
        assert(result.resultmsg.message[0].message);
        assert(strlen(result.resultmsg.message[0].message) > 0);
        free(fileuri_s);
        free(fileuri_s_u8);
        return result;
    }

    int _vfs_isdir = 0;
    if (!vfs_IsDirectoryU32(fileuri->path,
            fileuri->pathlen, &_vfs_isdir, vfsflags)) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "vfs_IsDirectory() failed, out of memory?",
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }
    if (_vfs_isdir) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "path points to directory instead of file",
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }

    uint64_t size = 0;
    if (!vfs_SizeU32(fileuri->path,
            fileuri->pathlen, &size, vfsflags)) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "vfs_Size() failed, lack of permission or i/o error",
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }
    if (size > H64LIMIT_SOURCEFILESIZE) {
        char buf[512];
        snprintf(buf, sizeof(buf) - 1,
            "file exceeds source file size limit of %" PRId64
            " bytes", (int64_t)H64LIMIT_SOURCEFILESIZE
        );
        result_ErrorNoLoc(
            &result.resultmsg,
            buf,
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }
    char *buffer = malloc(size);
    if (!buffer) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "failed to allocate token file buffer",
            fileuri_s, fileuri_slen
        );
        free(fileuri_s);
        return result;
    }
    if (!vfs_GetBytesU32(fileuri->path,
            fileuri->pathlen, 0, size, buffer, vfsflags)) {
        result_ErrorNoLoc(
            &result.resultmsg,
            "failed to read file, lack of permission or i/o error",
            fileuri_s, fileuri_slen
        );
        free(buffer);
        free(fileuri_s);
        return result;
    }

    int post_identifier_is_likely_func = 0;
    int tokenallocsize = 0;
    int64_t line = 1;
    int64_t column = 1;
    int i = 0;
    while (i < (int)size) {
        uint8_t c = ((uint8_t*)buffer)[i];
        if (c == '\r' || c == '\n') {
            i++;
            line++;
            column = 1;
            if (c == '\r' && i < (int)size && buffer[i] == '\n')
                i++;
            continue;
        }

        if (c == '\0') {
            result_AddMessage(
                &result.resultmsg,
                H64MSG_ERROR,
                "invalid binary value 0x0, "
                "you must escape zero bytes with \\0",
                fileuri_s, fileuri_slen, line, column
            );
            column++;
            i++;
            continue;
        }

        // Whitespace and comments:
        if (c == ' ' || c == '\t') {
            column++;
            i++;
            continue;
        }
        if (c == '#') {
            i++;
            column++;
            while (i < (int)size && buffer[i] != '\r' &&
                    buffer[i] != '\n') {
                i++;
                column++;
            }
            continue;
        }

        // Make sure we have space for the next token:
        if (result.token_count >= tokenallocsize &&
                !_tokenalloc(
                &result, &tokenallocsize
                )) {
            result_ErrorNoLoc(
                &result.resultmsg,
                "failed to allocate token, out of memory?",
                fileuri_s, fileuri_slen
            );
            free(buffer);
            free(fileuri_s);
            return result;
        }
        result.token[result.token_count].line = line;
        result.token[result.token_count].column = column;

        // Separating commas:
        if (c == ',') {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_COMMA;
            result.token_count++;
            i++;
            column++;
            continue; 
        }

        // We need to know later if a unary op is allowed here:
        int could_be_unary_op = 1;
        if (result.token_count > 0) {
            could_be_unary_op = 0;
            h64token *prevtok = &result.token[result.token_count - 1];
            int prevtype = result.token[result.token_count - 1].type;
            if (prevtype == H64TK_BRACKET && (
                    prevtok->char_value == '{' ||
                    prevtok->char_value == '(' ||
                    prevtok->char_value == '['))
                could_be_unary_op = 1;
            else if (prevtype == H64TK_UNOPSYMBOL ||
                    prevtype == H64TK_COMMA ||
                    prevtype == H64TK_BINOPSYMBOL ||
                    prevtype == H64TK_INLINEFUNC ||
                    prevtype == H64TK_MAPARROW ||
                    prevtype == H64TK_COLON ||
                    (prevtype == H64TK_KEYWORD &&
                     (strcmp(prevtok->str_value, "return") == 0 ||
                      strcmp(prevtok->str_value, "if") == 0 ||
                      strcmp(prevtok->str_value, "async") == 0 ||
                      strcmp(prevtok->str_value, "await") == 0 ||
                      strcmp(prevtok->str_value, "elseif") == 0 ||
                      strcmp(prevtok->str_value, "while") == 0 ||
                      strcmp(prevtok->str_value, "for") == 0 ||
                      strcmp(prevtok->str_value, "except") == 0 ||
                      strcmp(prevtok->str_value, "unpack") == 0 ||
                      strcmp(prevtok->str_value, "then") == 0)))
                could_be_unary_op = 1;
        }

        // Constants/literals:
        if (c == '"' || c == '\'' ||
                (c == 'b' && i < (int)size &&
                 (buffer[i + 1] == '"' || buffer[i + 1] == '\''))) {
            // This is a string or bytes literal.
            post_identifier_is_likely_func = 0;
            int startcolumn = column;
            int startline = line;
            int isbinary = (c == 'b');
            unsigned char startc = (isbinary ? buffer[i + 1] : c);
            i++;
            column++;

            char *strbuf = malloc(32);
            if (!strbuf) {
                 result_ErrorNoLoc(
                    &result.resultmsg,
                    "failed to allocate literal, "
                    "out of memory?",
                    fileuri_s, fileuri_slen
                );
                free(buffer);
                free(fileuri_s);
                return result;
            }
            int hadinvaliderror = 0;
            int stralloc = 32;
            int strbuflen = 0;

            strbuf[strbuflen] = c;
            strbuflen++;
            if (isbinary) {
                strbuf[strbuflen] = buffer[i];
                startc = buffer[i];
                strbuflen++;
                i++;
                column++;
            }

            int escaped = 0;
            while (1) {
                if (i >= (int)size) {
                    char buf[512];
                    snprintf(buf, sizeof(buf) - 1,
                        "unexpected end of file, "
                        "expected terminating \"%c\" for %s literal "
                        "starting in line %d, column %d",
                        startc, (isbinary ? "bytes" : "string"),
                        startline, startcolumn);
                    result_AddMessage(
                        &result.resultmsg,
                        H64MSG_ERROR, buf,
                        fileuri_s, fileuri_slen,
                        line, column
                    );
                    hadinvaliderror = 1;
                    break;
                }
                if (strbuflen + 9 >= stralloc) {
                    stralloc *= 2;
                    if (stralloc < strbuflen + 6)
                        stralloc = strbuflen + 6;
                    if (stralloc < 8)
                        stralloc = 8;
                    char *newstrbuf = realloc(
                        strbuf, stralloc
                    );
                    if (!newstrbuf) {
                        if (strbuf) free(strbuf);
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to allocate literal, "
                            "out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                    strbuf = newstrbuf;
                }
                c = ((uint8_t*)buffer)[i];
                if (c == '\0') {
                    hadinvaliderror = 1;
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR,
                            "invalid binary value 0x0, "
                            "you must escape zero bytes with \\0",
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        if (strbuf) free(strbuf);
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to allocate error, "
                            "out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                }
                if (!isbinary &&
                        !is_valid_utf8_char(
                        (uint8_t*)buffer + i, size - i)
                        ) {
                    hadinvaliderror = 1;
                    char buf[256];
                    snprintf(buf, sizeof(buf) - 1,
                        "invalid binary value 0x%x, "
                        "source code must be valid utf-8",
                        (int)c
                    );
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR, buf,
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        if (strbuf) free(strbuf);
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to allocate error, "
                            "out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                    escaped = 0;
                    i++;
                    continue;
                } else if (isbinary && ((uint8_t*)buffer)[i] > 127) {
                    char buf[256];
                    snprintf(buf, sizeof(buf) - 1,
                        "invalid character 0x%x, "
                        "non-ASCII values in bytes literal must "
                        "be escaped",
                        (int)c
                    );
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR, buf,
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        if (strbuf) free(strbuf);
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to allocate error, "
                            "out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                    escaped = 0;
                    i++;
                    continue;
                }
                int charlen = utf8_char_len((uint8_t*)&buffer[i]);
                int k = 0;
                while (k < charlen) {
                    c = ((uint8_t*)buffer)[i];
                    strbuf[strbuflen] = c;
                    strbuflen++;
                    i++;
                    k++;
                }
                if (charlen == 1 && c == '\r') {
                    c = '\n';
                    if (i + 1 < (int)size &&
                            buffer[i + 1] == '\n')
                        i++;
                }
                if (charlen == 1 && !escaped) {
                    if (c == '\\') {
                        escaped = 1;
                        column++;
                        continue;
                    } else if (c == startc) {
                        column++;
                        break;
                    }
                } else {
                    escaped = 0;
                }
                if (c == '\n') {
                    line++;
                    column = 1;
                } else {
                    column++;
                }
            }
            strbuf[strbuflen] = '\0';

            if (!hadinvaliderror) {
                int out_len = -1;
                char *unescaped = lexer_ParseStringLiteral(
                    strbuf, fileuri_s, fileuri_slen,
                    startline, startcolumn,
                    isbinary,
                    &result.resultmsg, wconfig, &out_len
                );
                free(strbuf);
                strbuf = NULL;
                if (!unescaped) {
                    result_ErrorNoLoc(
                        &result.resultmsg,
                        "failed to allocate literal, "
                        "out of memory?",
                        fileuri_s, fileuri_slen
                    );
                    free(buffer);
                    free(fileuri_s);
                    return result;
                }
                assert(out_len >= 0);
                if (!isbinary) {
                    result.token[result.token_count].type = (
                        H64TK_CONSTANT_STRING
                    );
                } else {
                    result.token[result.token_count].type = (
                        H64TK_CONSTANT_BYTES
                    );
                }
                result.token[result.token_count].str_value = unescaped;
                result.token[result.token_count].str_value_len = out_len;
            } else {
                if (strbuf) free(strbuf);
                strbuf = NULL;
                result.token[result.token_count].type = H64TK_INVALID;
            }
            result.token_count++;
            // Special: if we produced two string or two byte tokens
            // in a row, merge them:
            if (likely(result.token_count >= 2)) {
                if (unlikely((
                        result.token[result.token_count - 1].type ==
                            H64TK_CONSTANT_STRING &&
                        result.token[result.token_count - 2].type ==
                            H64TK_CONSTANT_STRING)) || (
                        result.token[result.token_count - 1].type ==
                            H64TK_CONSTANT_BYTES &&
                        result.token[result.token_count - 2].type ==
                            H64TK_CONSTANT_BYTES)) {
                    char *new_str_value = malloc(
                        result.token[result.token_count - 1].str_value_len +
                        result.token[result.token_count - 2].str_value_len +
                        1
                    );
                    if (!new_str_value) {
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to allocate literal, "
                            "out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                    memcpy(
                        new_str_value,
                        result.token[result.token_count - 2].str_value,
                        result.token[result.token_count - 2].str_value_len
                    );
                    memcpy(
                        new_str_value +
                        result.token[result.token_count - 2].str_value_len,
                        result.token[result.token_count - 1].str_value,
                        result.token[result.token_count - 1].str_value_len
                    );
                    new_str_value[
                        result.token[result.token_count - 1].str_value_len +
                        result.token[result.token_count - 2].str_value_len
                    ] = '\0';
                    free(result.token[result.token_count - 1].str_value);
                    free(result.token[result.token_count - 2].str_value);
                    result.token[result.token_count - 2].str_value = (
                        new_str_value
                    );
                    result.token[result.token_count - 2].str_value_len += (
                        result.token[result.token_count - 1].str_value_len
                    );
                    result.token_count--;
                }
            }
            continue;
        } else if (c >= '0' && c <= '9') {
            // This is a number literal
            post_identifier_is_likely_func = 0;
            int64_t startline = line;
            int64_t startcolumn = column;
            char *numbuf = malloc(8);
            int hadoverflowerror = 0;
            int numbufalloc = 8;
            int numbuflen = 0;
            if (c == '-') {
                i++;
                column++;
                numbuf[numbuflen] = '-';
                numbuflen++;
                while (i < (int)size && (
                        buffer[i] == ' ' || buffer[i] == '\t' ||
                        buffer[i] == '\r' || buffer[i] == '\n')) {
                    if (buffer[i] == '\r' || buffer[i] == '\n') {
                        line++;
                        column = 1;
                        if (buffer[i] == '\r' &&
                                i + 1 < (int)size && buffer[i + 1] == '\n')
                            i++;
                    } else {
                        column++;
                    }
                    i++;
                }
            }
            int nodigitotherthanzero = 1;
            int lastwasdigit = 0;
            int sawdot = 0;
            int sawxorb = 0;
            int ishex = 0;
            int isbinary = 0;
            while (i < (int)size &&
                    ((buffer[i] >= '0' && buffer[i] <= '9' && !isbinary) ||
                     (buffer[i] == '0' || buffer[i] == '1') ||
                     (buffer[i] >= 'a' && buffer[i] <= 'f' && ishex) ||
                     (buffer[i] >= 'A' && buffer[i] <= 'F' && ishex) ||
                    (buffer[i] == '.' && !sawdot && lastwasdigit &&
                     !sawxorb && i + 1 < (int)size &&
                     buffer[i + 1] >= '0' && buffer[i + 1] <= '9') ||
                    (buffer[i] == 'b' && lastwasdigit &&
                     nodigitotherthanzero && !sawdot) ||
                    (buffer[i] == 'x' && lastwasdigit &&
                     nodigitotherthanzero && !sawdot))) {
                c = buffer[i];
                if (c >= '0' && c <= '9') {
                    lastwasdigit = 1;
                    if (c != '0')
                        nodigitotherthanzero = 0;
                } else {
                    lastwasdigit = 0;
                    if (c == '.') {
                        sawdot = 1;
                    } else if (c == 'x' || c == 'b') {
                        sawxorb = 1;
                        if (c == 'x') ishex = 1;
                        else if (c == 'b') isbinary = 1;
                    }
                }
                if (numbuflen + 1 >= numbufalloc) {
                    numbufalloc *= 2;
                    char *numbufnew = realloc(
                        numbuf, numbufalloc
                    );
                    if (!numbufnew) {
                        free(numbuf);
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to allocate literal, "
                            "out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                    numbuf = numbufnew;
                }
                numbuf[numbuflen] = c;
                numbuflen++;
                column++;
                i++;
            }
            if (numbuflen <= 0 ||
                    ((numbuf[numbuflen - 1] < '0' ||
                      numbuf[numbuflen - 1] > '9') &&
                     (numbuf[numbuflen - 1] < 'a' ||
                      numbuf[numbuflen - 1] > 'f') &&
                     (numbuf[numbuflen - 1] < 'A' ||
                      numbuf[numbuflen - 1] > 'F'))) {
                result.token[result.token_count].type = H64TK_INVALID;
                result.token_count++;
                numbuf[numbuflen] = '\0';
                char buf[512];
                snprintf(
                    buf, sizeof(buf) - 1,
                    "unexpected end of literal, "
                    "expected digit to finish off number "
                    "literal starting in line %" PRId64
                    ", column %" PRId64,
                    startline, startcolumn
                    );
                free(numbuf);
                if (!result_AddMessage(
                        &result.resultmsg,
                        H64MSG_ERROR, buf,
                        fileuri_s, fileuri_slen,
                        line, column
                        )) {
                    result_ErrorNoLoc(
                        &result.resultmsg,
                        "failed to add result message, out of memory?",
                        fileuri_s, fileuri_slen
                    );
                    free(buffer);
                    free(fileuri_s);
                    return result;
                }
                continue;
            }
            if (sawdot) {  // Trim unnecessary fractional zeros
                while (numbuflen > 2 &&
                        numbuf[numbuflen - 1] == '0' &&
                        (numbuf[numbuflen - 2] >= '0' &&
                         numbuf[numbuflen - 2] <= '9')) {
                    numbuflen--;
                }
                if (numbuflen > 2 &&
                        numbuf[numbuflen - 1] == '0' &&
                        numbuf[numbuflen - 2] == '.') {
                    numbuflen -= 2;
                    sawdot = 0;
                }
            }
            numbuf[numbuflen] = '\0';
            if (sawdot) {
                assert(!sawxorb);
                double value = h64atof(numbuf);
                result.token[result.token_count].type = H64TK_CONSTANT_FLOAT;
                result.token[result.token_count].float_value = value;
                if (!hadoverflowerror && (
                        value >= (double)INT64_MAX ||
                        // ^ reminder: double rounds to INT64_MAX + 1
                        value < (double)INT64_MIN)) {
                    hadoverflowerror = 1;
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR, _literaloverflowerror,
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to add result message, out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        free(numbuf);
                        return result;
                    }
                }
            } else if (sawxorb) {
                assert(numbuflen >= 3);
                assert(numbuf[0] == '0' && (
                       numbuf[1] == 'x' || numbuf[1] == 'b'));
                const char *p = (numbuf + 2);
                assert(strlen(p) > 0);
                int64_t value = h64strtoll(p, NULL, (
                    numbuf[1] == 'x' ? 16 : 2
                ));
                result.token[result.token_count].type = H64TK_CONSTANT_INT;
                result.token[result.token_count].int_value = value;
            } else {
                // Plain decimal system integer.
                assert(!isbinary && !ishex);
                if (!hadoverflowerror &&
                        strlen(numbuf) > strlen(INT64_MAX_STR) &&
                        strlen(numbuf) > strlen(INT64_MIN_STR)) {  // overflow
                    hadoverflowerror = 1;
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR, _literaloverflowerror,
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to add result message, out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        free(numbuf);
                        return result;
                    }
                }
                // Parse & set integer value:
                int64_t value = h64atoll(numbuf);
                result.token[result.token_count].type = H64TK_CONSTANT_INT;
                result.token[result.token_count].int_value = value;
                // Make sure we didn't overflow:
                char num_convertback[128];
                snprintf(
                    num_convertback, sizeof(num_convertback),
                    "%" PRId64, value
                );
                num_convertback[sizeof(num_convertback) - 1] = 0;
                if (!hadoverflowerror &&
                        strcmp(num_convertback, numbuf) != 0) {  // overflow
                    hadoverflowerror = 1;
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR, _literaloverflowerror,
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to add result message, out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        free(numbuf);
                        return result;
                    }
                }
            }
            result.token_count++;
            free(numbuf);
            numbuf = NULL;
            if (i < (int)size && is_identifier_char(buffer[i])) {
                char printc[32];
                unsigned int len = utf8_char_len((uint8_t*)buffer + i);
                if (len > size - i)
                    len = size - i;
                if (len > sizeof(printc) - 1)
                    len = sizeof(printc) - 1;
                memcpy(printc, buffer + i, len);
                printc[len] = '\0';
                char buf[512];
                snprintf(
                    buf, sizeof(buf) - 1,
                    "unexpected lack of separation before character "
                    "\"%s\", "
                    "expected whitespace, bracket, comma, "
                    "operator, or other separator "
                    "after number literal "
                    "starting in line %" PRId64 ", column %" PRId64,
                    printc, startline, startcolumn);
                if (!result_AddMessage(
                        &result.resultmsg,
                        H64MSG_ERROR, buf,
                        fileuri_s, fileuri_slen,
                        line, column
                        )) {
                    result_ErrorNoLoc(
                        &result.resultmsg,
                        "failed to add result message, out of memory?",
                        fileuri_s, fileuri_slen
                    );
                    free(buffer);
                    free(fileuri_s);
                    return result;
                }
            }
            continue;
        } else if (c == 'y' &&
                i + strlen("yes") - 1 < (unsigned int)size &&
                buffer[i + 1] == 'e' &&
                buffer[i + 2] == 's' && (
                i + strlen("yes") >= (unsigned int)size ||
                !is_identifier_resume_char(buffer[i + strlen("yes")]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_CONSTANT_BOOL;
            result.token[result.token_count].int_value = 1;
            result.token_count++;
            i += strlen("yes");
            column += strlen("yes");
            continue;
        } else if (c == 'n' &&
                i + strlen("no") - 1 < (unsigned int)size &&
                buffer[i + 1] == 'o' && (
                i + strlen("no") >= (unsigned int)size ||
                !is_identifier_resume_char(buffer[i + strlen("no")]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_CONSTANT_BOOL;
            result.token[result.token_count].int_value = 0;
            result.token_count++;
            i += strlen("no");
            column += strlen("no");
            continue;
        } else if (c == 'n' &&
                i + strlen("none") - 1 < (unsigned int)size &&
                buffer[i + 1] == 'o' &&
                buffer[i + 2] == 'n' &&
                buffer[i + 3] == 'e' && (
                i + strlen("none") >= (unsigned int)size ||
                !is_identifier_resume_char(buffer[i + strlen("none")]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_CONSTANT_NONE;
            result.token_count++;
            i += strlen("none");
            column += strlen("none");
            continue;
        }

        // Brackets:
        if ((c == '(' && (could_be_unary_op ||
                post_identifier_is_likely_func)) || c == ')' ||
                (c == '[' && could_be_unary_op) || c == ']' ||
                c == '{' || c == '}') {
            result.token[result.token_count].type = H64TK_BRACKET;
            result.token[result.token_count].char_value = c;
            if (!could_be_unary_op && c == '[') {
                result.token[result.token_count].type = H64TK_BINOPSYMBOL;
                result.token[result.token_count].int_value = (
                    H64OP_INDEXBYEXPR
                );
            }
            post_identifier_is_likely_func = 0;
            result.token_count++;
            i++;
            column++;
            continue;
        }

        // Colon for vectors:
        if (c == ':') {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_COLON;
            result.token[result.token_count].char_value = c;
            result.token_count++;
            i++;
            column++;
            continue;
        }

        // Arrow for maps:
        if (c == '-' && i + 1 < (int)size && buffer[i + 1] == '>') {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_MAPARROW;
            result.token_count++;
            i += 2;
            column += 2;
            continue;
        }

        // Arithmetic-style operators and -> dict assign:
        if (c == '+' || c == '%' || c == '|' || c == '&' ||
                c == '^' || c == '.' ||
                (c == '!' && i + 1 < (int)size && buffer[i + 1] == '=') ||
                c == '/' || c == '<' || c == '>' || c == '*' ||
                (c == '-' && (i + 1 >= (int)size ||
                 buffer[i + 1] != '>')) ||
                (c == '=' && (i + 1 >= (int)size ||
                 buffer[i + 1] != '>')) ||
                ((c == '(' || c == '[') && !could_be_unary_op)) {
            post_identifier_is_likely_func = 0;
            int tokentype = H64TK_BINOPSYMBOL;
            int optype = H64OP_INVALID;

            switch (c) {
            case '(':
                optype = H64OP_CALL;
                break;
            case '[':
                optype = H64OP_INDEXBYEXPR;
                break;
            case '.':
                optype = H64OP_ATTRIBUTEBYIDENTIFIER;
                break;
            case '=':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_CMP_EQUAL;
                    i++;
                    column++;
                } else {
                    optype = H64OP_ASSIGN;
                }
                break;
            case '!':
                assert(i + 1 < (int)size && buffer[i + 1] == '=');
                optype = H64OP_CMP_NOTEQUAL;
                i++;
                column++;
                break;
            case '>':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_CMP_LARGEROREQUAL;
                    i++;
                    column++;
                } else if (i + 1 < (int)size && buffer[i + 1] == '>') {
                    if (i + 2 < (int)size && buffer[i + 2] == '=') {
                        optype = H64OP_ASSIGNMATH_BINSHIFTRIGHT;
                        i++;
                        column++;
                    } else {
                        optype = H64OP_MATH_BINSHIFTRIGHT;
                        i++;
                        column++;
                    }
                } else {
                    optype = H64OP_CMP_LARGER;
                }
                break;
            case '<':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_CMP_SMALLEROREQUAL;
                    i++;
                    column++;
                } else if (i + 1 < (int)size && buffer[i + 1] == '<') {
                    if (i + 2 < (int)size && buffer[i + 2] == '=') {
                        optype = H64OP_ASSIGNMATH_BINSHIFTLEFT;
                        i++;
                        column++;
                    } else {
                        optype = H64OP_MATH_BINSHIFTLEFT;
                        i++;
                        column++;
                    }
                } else {
                    optype = H64OP_CMP_SMALLER;
                }
                break;
            case '/':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_DIVIDE;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_DIVIDE;
                }
                break;
            case '*':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_MULTIPLY;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_MULTIPLY;
                }
                break;
            case '-':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_SUBSTRACT;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_SUBSTRACT;
                    if (could_be_unary_op)
                        tokentype = H64TK_UNOPSYMBOL;
                }
                break;
            case '+':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_ADD;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_ADD;
                }
                break;
            case '%':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_MODULO;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_MODULO;
                }
                break;
            case '|':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_BINOR;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_BINOR;
                }
                break;
            case '~':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_BINNOT;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_BINNOT;
                }
                break;
            case '^':
                if (i + 1 < (int)size && buffer[i + 1] == '=') {
                    optype = H64OP_ASSIGNMATH_BINXOR;
                    i++;
                    column++;
                } else {
                    optype = H64OP_MATH_BINXOR;
                }
                break;
            }
            assert(optype != 0);
            result.token[result.token_count].type = tokentype;
            result.token[result.token_count].int_value = optype;
            result.token_count++;
            if (IS_ASSIGN_OP(optype) && IS_UNWANTED_ASSIGN_OP(optype)) {
                char buf[512];
                snprintf(
                    buf, sizeof(buf) - 1,
                    "unexpected unavailable assignment math operator "
                    "\"%s\", "
                    "this syntax shortcut is only allowed for "
                    "\"+=\", \"-=\", \"*=\", and \"/=\"",
                    operator_OpPrintedAsStr(optype));
                if (!result_AddMessage(
                        &result.resultmsg,
                        H64MSG_ERROR, buf,
                        fileuri_s, fileuri_slen, line,
                        column + 1 - strlen(operator_OpPrintedAsStr(optype))
                        )) {
                    result_ErrorNoLoc(
                        &result.resultmsg,
                        "failed to add result message, out of memory?",
                        fileuri_s, fileuri_slen
                    );
                    free(buffer);
                    free(fileuri_s);
                    return result;
                }
            }
            i++;
            column++;
            continue;
        }
        // => inline func:
        if (c == '=' && i + 1 < (int)size &&
                 buffer[i + 1] == '>') {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_INLINEFUNC;
            result.token_count++;
            i += 2;
            column += 2;
            continue;
        }
        // "and" operator:
        if (c == 'a' && i + 1 < (int)size && buffer[i + 1] == 'n' &&
                i + 2 < (int)size && buffer[i + 2] == 'd' &&
                (i + 3 >= (int)size ||
                 !is_identifier_resume_char(buffer[i + 3]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_BINOPSYMBOL;
            result.token[result.token_count].int_value = H64OP_BOOLCOND_AND;
            result.token_count++;
            i += strlen("and");
            column += strlen("and");
            continue;
        }
        // "new" operator:
        if (c == 'n' && i + 1 < (int)size && buffer[i + 1] == 'e' &&
                i + 2 < (int)size && buffer[i + 2] == 'w' &&
                (i + 3 >= (int)size ||
                 !is_identifier_resume_char(buffer[i + 3]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_UNOPSYMBOL;
            result.token[result.token_count].int_value = H64OP_NEW;
            result.token_count++;
            i += strlen("new");
            column += strlen("new");
            continue;
        }
        // "or" operator:
        if (c == 'o' && i + 1 < (int)size && buffer[i + 1] == 'r' &&
                (i + 2 >= (int)size ||
                 !is_identifier_resume_char(buffer[i + 2]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_BINOPSYMBOL;
            result.token[result.token_count].int_value = H64OP_BOOLCOND_OR;
            result.token_count++;
            i += strlen("or");
            column += strlen("or");
            continue;
        }
        // "not" operator:
        if (c == 'n' && i + 1 < (int)size && buffer[i + 1] == 'o' &&
                i + 2 < (int)size && buffer[i + 2] == 't' &&
                (i + 3 >= (int)size ||
                 !is_identifier_resume_char(buffer[i + 3]))) {
            post_identifier_is_likely_func = 0;
            result.token[result.token_count].type = H64TK_UNOPSYMBOL;
            result.token[result.token_count].int_value = H64OP_BOOLCOND_NOT;
            result.token_count++;
            i += strlen("not");
            column += strlen("not");
            continue;
        }

        // Parse identifier and keywords:
        if (is_identifier_char(c)) {
            int cancontaindots = (
                result.token_count > 0 &&
                result.token[result.token_count - 1].type ==
                    H64TK_KEYWORD &&
                strcmp(result.token[result.token_count - 1].str_value,
                       "from") == 0
            );

            int64_t columnstart = column;
            int hadlimiterror = 0;
            int hadinvalidcharerror = 0;
            int totalchars = 0;
            char identifierbuf[
                H64LIMIT_IDENTIFIERLEN * 4 + 1
            ];  // multiplied by 4 since utf-8
            int firstchar = 1;
            int ilen = 0;
            while (i < (int)size && (is_identifier_char(c) ||
                    (!firstchar && c >= '0' && c <= '9') ||
                    (cancontaindots && c == '.'))) {
                firstchar = 0;
                unsigned int charlen = utf8_char_len((uint8_t*)buffer + i);
                if (charlen > size - i)
                    charlen = size - i;
                if (c > 127 && !is_valid_utf8_char(
                        (uint8_t*)(buffer + i), size - i
                        )) {
                    charlen = 1;
                    hadinvalidcharerror = 1;
                    char buf[256];
                    snprintf(buf, sizeof(buf) - 1,
                        "invalid binary value 0x%x, "
                        "source code must be valid utf-8",
                        (int)c
                    );
                    if (!result_AddMessage(
                            &result.resultmsg,
                            H64MSG_ERROR, buf,
                            fileuri_s, fileuri_slen,
                            line, column
                            )) {
                        result_ErrorNoLoc(
                            &result.resultmsg,
                            "failed to add result "
                            "message, out of memory?",
                            fileuri_s, fileuri_slen
                        );
                        free(buffer);
                        free(fileuri_s);
                        return result;
                    }
                }
                assert(charlen > 0);
                int k = 0;
                while (k < (int)charlen) {
                    if (ilen < H64LIMIT_IDENTIFIERLEN * 4 &&
                            totalchars < H64LIMIT_IDENTIFIERLEN) {
                        identifierbuf[ilen] = buffer[i + k];
                        ilen++;
                    } else if (!hadlimiterror) {
                        hadlimiterror = 1;
                        char buf[256];
                        snprintf(buf, sizeof(buf) - 1,
                            "invalid identifier exceeds maximum length "
                            "of %d characters",
                            (int)H64LIMIT_IDENTIFIERLEN
                        );
                        if (!result_AddMessage(
                                &result.resultmsg,
                                H64MSG_ERROR, buf,
                                fileuri_s, fileuri_slen,
                                line, columnstart
                                )) {
                            result_ErrorNoLoc(
                                &result.resultmsg,
                                "failed to add result message, "
                                "out of memory?",
                                fileuri_s, fileuri_slen
                            );
                            free(buffer);
                            free(fileuri_s);
                            return result;
                        }
                    }
                    k++;
                }
                totalchars++;
                column++;
                i += charlen;
                if (i < (int)size)
                    c = ((uint8_t*)buffer)[i];
            }
            identifierbuf[ilen] = '\0';
            result.token[result.token_count].type = H64TK_IDENTIFIER;
            result.token[result.token_count].str_value = strdup(
                (!hadlimiterror && !hadinvalidcharerror ?
                 identifierbuf : "##INVALID##")
            );
            if (!result.token[result.token_count].str_value) {
                result_ErrorNoLoc(
                    &result.resultmsg,
                    "failed to allocate identifier, "
                    "out of memory?",
                    fileuri_s, fileuri_slen
                );
                free(buffer);
                free(fileuri_s);
                return result;
            }
            int k = 0;
            while (h64keywords[k]) {
                if (strcmp(
                        result.token[result.token_count].str_value,
                        h64keywords[k]) == 0) {
                    result.token[result.token_count].type = H64TK_KEYWORD;
                    if (strcmp(result.token[result.token_count].str_value,
                               "func") == 0) {
                        post_identifier_is_likely_func = 1;
                    } else {
                        post_identifier_is_likely_func = 0;
                    }
                    break;
                }
                k++;
            }
            result.token_count++;
            continue;
        }

        // Report unexpected character:
        char buf[256];
        snprintf(buf, sizeof(buf) - 1,
            "unexpected binary value 0x%x, "
            "expected any valid token instead",
            (int)c
        );
        if (c >= 32 && c <= 126 && c != '\'') {
            snprintf(buf, sizeof(buf) - 1,
                "unexpected character \"%c\", "
                "expected any valid token instead",
                (int)c
            );
        }
        if (!result_AddMessage(
                &result.resultmsg,
                H64MSG_ERROR, buf, fileuri_s, fileuri_slen,
                line, column
                )) {
            result_ErrorNoLoc(
                &result.resultmsg,
                "failed to add result message, "
                "out of memory?",
                fileuri_s, fileuri_slen
            );
            free(buffer);
            free(fileuri_s);
            return result;
        }
        i++;
        column++;
    }

    int returninganyerror = 0;
    i = 0;
    while (i < result.resultmsg.message_count) {
        if (result.resultmsg.message[i].type == H64MSG_ERROR) {
            returninganyerror = 1;
            break;
        }
        i++;
    }
    if (!result.resultmsg.fileuri)
        result.resultmsg.fileuri = strdupu32(
            fileuri_s, fileuri_slen,
            &result.resultmsg.fileurilen
        );
    if (returninganyerror)
        result.resultmsg.success = 0;
    free(buffer);
    free(fileuri_s);

    #ifndef NDEBUG
    if (result.resultmsg.success) {
        int k = 0;
        while (k < result.token_count) {
            assert(result.token[k].type != H64TK_INVALID);
            k++;
        }
    }
    #endif
    return result;
}

void lexer_ClearToken(h64token *t) {
    if (t->type == H64TK_IDENTIFIER ||
            t->type == H64TK_KEYWORD ||
            t->type == H64TK_CONSTANT_STRING ||
            t->type == H64TK_CONSTANT_BYTES) {
        free(t->str_value);
    }
}

void lexer_FreeFileTokens(h64tokenizedfile *tfile) {
    int i = 0;
    while (i < tfile->token_count) {
        lexer_ClearToken(&tfile->token[i]);
        i++;
    }
    if (tfile->token)
        free(tfile->token);
    tfile->token = NULL;
    tfile->token_count = 0;
}

static char _h64tkname_invalid[] = "H64TK_INVALID";
static char _h64tkname_identifier[] = "H64TK_IDENTIFIER";
static char _h64tkname_bracket[] = "H64TK_BRACKET";
static char _h64tkname_comma[] = "H64TK_COMMA";
static char _h64tkname_colon[] = "H64TK_COLON";
static char _h64tkname_keyword[] = "H64TK_KEYWORD";
static char _h64tkname_constant_int[] = "H64TK_CONSTANT_INT";
static char _h64tkname_constant_float[] = "H64TK_CONSTANT_FLOAT";
static char _h64tkname_constant_bool[] = "H64TK_CONSTANT_BOOL";
static char _h64tkname_constant_none[] = "H64TK_CONSTANT_NULL";
static char _h64tkname_constant_string[] = "H64TK_CONSTANT_STRING";
static char _h64tkname_constant_bytes[] = "H64TK_CONSTANT_BYTES";
static char _h64tkname_binopsymbol[] = "H64TK_BINOPSYMBOL";
static char _h64tkname_unopsymbol[] = "H64TK_UNOPSYMBOL";
static char _h64tkname_inlinefunc[] = "H64TK_INLINEFUNC";
static char _h64tkname_maparrow[] = "H64TK_MAPARROW";

const char *lexer_TokenTypeToStr(h64tokentype type) {
    if (type == H64TK_INVALID) {
        return _h64tkname_invalid;
    } else if (type == H64TK_IDENTIFIER) {
        return _h64tkname_identifier;
    } else if (type == H64TK_BRACKET) {
        return _h64tkname_bracket;
    } else if (type == H64TK_COMMA) {
        return _h64tkname_comma;
    } else if (type == H64TK_COLON) {
        return _h64tkname_colon;
    } else if (type == H64TK_KEYWORD) {
        return _h64tkname_keyword;
    } else if (type == H64TK_CONSTANT_INT) {
        return _h64tkname_constant_int;
    } else if (type == H64TK_CONSTANT_FLOAT) {
        return _h64tkname_constant_float;
    } else if (type == H64TK_CONSTANT_BOOL) {
        return _h64tkname_constant_bool;
    } else if (type == H64TK_CONSTANT_NONE) {
        return _h64tkname_constant_none;
    } else if (type == H64TK_CONSTANT_STRING) {
        return _h64tkname_constant_string;
    } else if (type == H64TK_CONSTANT_BYTES) {
        return _h64tkname_constant_bytes;
    } else if (type == H64TK_BINOPSYMBOL) {
        return _h64tkname_binopsymbol;
    } else if (type == H64TK_UNOPSYMBOL) {
        return _h64tkname_unopsymbol;
    } else if (type == H64TK_INLINEFUNC) {
        return _h64tkname_inlinefunc;
    } else if (type == H64TK_MAPARROW) {
        return _h64tkname_maparrow;
    }
    return NULL;
}

char *lexer_TokenToJSONStr(
        h64token *t, const h64wchar *fileuri, int64_t fileurilen
        ) {
    jsonvalue *v = lexer_TokenToJSON(t, fileuri, fileurilen);
    if (!v)
        return NULL;

    char *result = json_Dump(v);
    json_Free(v);
    return result;
}

jsonvalue *lexer_TokenToJSON(
        h64token *t, const h64wchar *fileuri, int64_t fileurilen
        ) {
    int fail = 0;
    jsonvalue *v = json_Dict();
    char *typestr = strdup(lexer_TokenTypeToStr(t->type));
    if (typestr) {
        if (!json_SetDictStr(v, "type", typestr)) {
            fail = 1;
        }
        free(typestr);
    } else {
        h64fprintf(stderr, "horsec: error: internal error, "
            "fail of handling token type %d in lexer_TokenTypeToStr\n",
            t->type);
        fail = 1;
    }
    if (t->line >= 0) {
        if (!json_SetDictInt(v, "line", t->line)) {
            fail = 1;
        } else if (t->column >= 0) {
            if (!json_SetDictInt(v, "column", t->column)) {
                fail = 1;
            }
        }
    }
    if (t->type == H64TK_CONSTANT_STRING) {
        if (!json_SetDictStr(v, "value", t->str_value))
            fail = 1;
    } else if (t->type == H64TK_CONSTANT_BOOL) {
        if (!json_SetDictBool(v, "value", t->int_value != 0))
            fail = 1;
    } else if (t->type == H64TK_CONSTANT_INT) {
        if (!json_SetDictInt(v, "value", t->int_value))
            fail = 1;
    } else if (t->type == H64TK_CONSTANT_FLOAT) {
        if (!json_SetDictFloat(v, "value", t->float_value))
            fail = 1;
    } else if (t->type == H64TK_IDENTIFIER ||
            t->type == H64TK_KEYWORD) {
        if (!json_SetDictStr(v, "value", t->str_value))
            fail = 1;
    } else if (t->type == H64TK_BRACKET) {
        char br[2];
        br[0] = t->char_value;
        br[1] = '\0';
        if (!json_SetDictStr(v, "value", br))
            fail = 1;
    } else if (t->type == H64TK_BINOPSYMBOL ||
            t->type == H64TK_UNOPSYMBOL) {
        const char *opname = operator_OpTypeToStr(t->int_value);
        if (!opname)
            fail = 1;
        else if (!json_SetDictStr(v, "value", opname))
            fail = 1;
    }
    if (fileuri) {
        char *fileuri_u8 = AS_U8(
            fileuri, fileurilen
        );
        if (!fileuri_u8) {
            fail = 1;
        } else {
            if (!json_SetDictStr(v, "file-uri", fileuri_u8))
                fail = 1;
            free(fileuri_u8);
        }
    }
    if (fail) {
        json_Free(v);
        return NULL;
    }
    return v;
}

void lexer_DebugPrintTokens(h64token *t, int count) {
    h64printf("horsec: debug: tokens:");
    int i = 0;
    while (i < count) {
        h64printf(" %s", lexer_TokenTypeToStr(t[i].type));
        if (t[i].type == H64TK_CONSTANT_INT) {
            h64printf("(%" PRId64 ")", t[i].int_value);
        } else if (t[i].type == H64TK_CONSTANT_STRING) {
            h64printf("(\"%s\")", t[i].str_value);
        } else if (t[i].type == H64TK_IDENTIFIER) {
            h64printf("(%s)", t[i].str_value);
        } else if (t[i].type == H64TK_BINOPSYMBOL) {
            h64printf("(\"%s\")", operator_OpPrintedAsStr(t[i].int_value));
        }
        i++;
    }
    h64printf("\n");
}
