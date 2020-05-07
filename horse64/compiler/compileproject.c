
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/ast.h"
#include "compiler/astparser.h"
#include "compiler/codemodule.h"
#include "compiler/compileproject.h"
#include "filesys.h"
#include "hash.h"
#include "secrandom.h"
#include "uri.h"
#include "vfs.h"


h64compileproject *compileproject_New(
        const char *basefolderuri
        ) {
    if (!basefolderuri)
        return NULL;

    h64compileproject *pr = malloc(sizeof(*pr));
    if (!pr)
        return NULL;
    memset(pr, 0, sizeof(*pr));
    warningconfig_Init(&pr->warnconfig);

    uriinfo *uinfo = uri_ParseEx(basefolderuri, "https");
    if (!uinfo || !uinfo->path || !uinfo->protocol ||
            strcasecmp(uinfo->protocol, "file") != 0) {
        uri_Free(uinfo);
        free(pr);
        return NULL;
    }

    char *s = filesys_ToAbsolutePath(uinfo->path);
    uri_Free(uinfo); uinfo = NULL;
    if (!s) {
        free(pr);
        return NULL;
    }
    pr->basefolder = filesys_Normalize(s);
    free(s);
    if (!pr->basefolder) {
        free(pr);
        return NULL;
    }

    if (!secrandom_GetBytes(
            pr->hashsecret, sizeof(*pr->hashsecret)
            )) {
        free(pr->basefolder);
        free(pr);
        return NULL;
    }
    pr->astfilemap = hash_NewStringMap(32);
    if (!pr->astfilemap) {
        free(pr->basefolder);
        free(pr);
        return NULL;
    }

    return pr;
}

char *compileproject_ToProjectRelPath(
        h64compileproject *pr, const char *fileuri
        ) {
    if (!fileuri || !pr || !pr->basefolder)
        return NULL;

    uriinfo *uinfo = uri_ParseEx(fileuri, "https");
    if (!uinfo || !uinfo->path || !uinfo->protocol ||
            strcasecmp(uinfo->protocol, "file") != 0) {
        uri_Free(uinfo);
        return NULL;
    }

    char *s = filesys_ToAbsolutePath(uinfo->path);
    uri_Free(uinfo); uinfo = NULL;
    if (!s) return NULL;

    char *result = filesys_TurnIntoPathRelativeTo(
        s, pr->basefolder
    );
    free(s); s = NULL;
    if (!result) return NULL;

    char *resultnormalized = filesys_Normalize(result);
    free(result);
    if (!resultnormalized) return NULL;

    return resultnormalized;
}

int compileproject_GetAST(
        h64compileproject *pr, const char *fileuri,
        h64ast *out_ast, char **error
        ) {
    char *relfilepath = compileproject_ToProjectRelPath(
        pr, fileuri
    );
    if (!relfilepath) {
        *error = strdup(
            "cannot get AST of file outside of project root"
        );
        return 0;
    }
 
    uint64_t entry;
    if (hash_StringMapGet(
            pr->astfilemap, relfilepath, &entry
            ) && entry > 0) {
        h64ast *resultptr = (h64ast*)(uintptr_t)entry;
        if (resultptr->stmt_count > 0) {
            free(relfilepath);
            memcpy(out_ast, resultptr, sizeof(*resultptr));
            return 1;
        }
        hash_StringMapUnset(
            pr->astfilemap, relfilepath
        );
        result_FreeContents(&resultptr->resultmsg);
        ast_FreeContents(resultptr);
        free(resultptr);
    }

    char *absfilepath = filesys_Join(
        pr->basefolder, relfilepath
    );
    if (!absfilepath) {
        free(relfilepath);
        *error = strdup("alloc fail");
        return 0;
    }

    h64ast result = codemodule_GetASTUncached(
        pr, absfilepath, &pr->warnconfig
    );
    free(absfilepath); absfilepath = NULL;
    h64ast *resultalloc = malloc(sizeof(*resultalloc));
    if (!resultalloc) {
        ast_FreeContents(&result);
        free(relfilepath);
        *error = strdup("alloc fail");
        return 0;
    }
    memcpy(resultalloc, &result, sizeof(result));

    if (!hash_StringMapSet(
            pr->astfilemap, relfilepath, (uintptr_t)resultalloc
            )) {
        ast_FreeContents(&result);
        free(relfilepath);
        free(resultalloc);
        *error = strdup("alloc fail");
        return 0;
    }
    memcpy(out_ast, &result, sizeof(result));
    free(relfilepath);
    return 1;
}

int _compileproject_astfreecallback(
        __attribute__((unused)) hashmap *map,
        __attribute__((unused)) const char *key, uint64_t number,
        __attribute__((unused)) void *userdata
        ) {
    h64ast *resultptr = (h64ast*)(uintptr_t)number;
    if (resultptr) {
        result_FreeContents(&resultptr->resultmsg);
        ast_FreeContents(resultptr);
        free(resultptr);
    }
    return 1;
}

void compileproject_Free(h64compileproject *pr) {
    if (!pr) return;

    free(pr->basefolder);

    if (pr->astfilemap) {
        hash_StringMapIterate(
            pr->astfilemap, &_compileproject_astfreecallback, NULL
        );
        hash_FreeMap(pr->astfilemap);
    }
    free(pr);
}

char *compileproject_GetFileSubProjectPath(
        h64compileproject *pr, const char *sourcefileuri
        ) {
    return NULL;  // FIXME  IMPLEMENT PROPERLY
}

char *compileproject_ResolveImport(
        h64compileproject *pr,
        const char *sourcefileuri,
        const char **import_elements, int import_elements_count,
        const char *library_source,
        int *outofmemory
        ) {
    if (!pr || !pr->basefolder || !sourcefileuri)
        return NULL;
    char *import_relpath = NULL;
    int import_relpath_len = 0;
    int i = 0;
    while (i < import_elements_count) {
        if (i + 1 < import_elements_count)
            import_relpath_len++; // dir separator
        import_relpath_len += strlen(import_elements[i]);
        i++;
    }
    import_relpath_len += strlen(".h64");
    import_relpath = malloc(import_relpath_len + 1);
    if (!import_relpath) {
        if (outofmemory) *outofmemory = 1;
        return NULL;
    }
    {
        char *p = import_relpath;
        i = 0;
        while (i < import_elements_count) {
            memcpy(p, import_elements[i], strlen(import_elements[i]));
            p += strlen(import_elements[i]);
            if (i + 1 < import_elements_count) {
                #if defined(_WIN32) || defined(_WIN64)
                *p = '\\';
                #else
                *p = '/';
                #endif
                p++;
            }
            i++;
        }
        memcpy(p, ".h64", strlen(".h64") + 1);
    }
    assert(import_relpath_len == (int)strlen(import_relpath));
    if (library_source) {
        // FIXME !!! IMPLEMENT PROPERLY
        if (outofmemory) *outofmemory = 1;
        return NULL;
    }

    // Not a library, do local project folder search:
    char *projectpath = compileproject_GetFileSubProjectPath(
        pr, sourcefileuri
    );
    if (!projectpath) {
        free(import_relpath);
        if (outofmemory) *outofmemory = 1;
        return NULL;
    }
    char *relfilepath = compileproject_ToProjectRelPath(
        pr, sourcefileuri
    );
    if (!relfilepath) {
        free(projectpath);
        free(import_relpath);
        if (outofmemory) *outofmemory = 1;
        return NULL;
    }
    char *relfolderpath = filesys_Dirname(relfilepath);
    free(relfilepath);
    relfilepath = NULL;
    if (!relfolderpath) {
        free(projectpath);
        free(import_relpath);
        if (outofmemory) *outofmemory = 1;
        return NULL;
    }
    while (strlen(relfolderpath) && (
            relfolderpath[strlen(relfolderpath) - 1] == '/'
            #if defined(_WIN32) || defined(_WIN64)
            || relfolderpath[strlen(relfolderpath) - 1] == '\\'
            #endif
            )) {
        relfolderpath[strlen(relfolderpath) - 1] = '\0';
    }
    char **subdir_components = NULL;
    int subdir_components_count = 0;
    char currentcomponent[512] = {0};
    int currentcomponentlen = 0;
    i = 0;
    while (1) {
        if (relfolderpath[i] == '\0' || (
                relfolderpath[i] == '/'
                #if defined(_WIN32) || defined(_WIN64)
                relfolderpath[i] == '\\'
                #endif
                )) {
            if (currentcomponentlen > 0) {
                currentcomponent[currentcomponentlen] = '\0';
                char *s = strdup(currentcomponent);
                if (!s) {
                    subdircheckoom: ;
                    int k = 0;
                    while (k < subdir_components_count) {
                        free(subdir_components[k]);
                        k++;
                    }
                    free(subdir_components);
                    free(projectpath);
                    free(relfolderpath);
                    free(import_relpath);
                    if (outofmemory) *outofmemory = 1;
                    return NULL;
                }
                char **new_components = realloc(
                    subdir_components,
                    sizeof(*new_components) * (
                    subdir_components_count + 1
                    ));
                if (!new_components) {
                    free(s);
                    goto subdircheckoom;
                }
                subdir_components = new_components;
                subdir_components[subdir_components_count] = s;
                subdir_components_count++;
            }
            if (relfolderpath[i] == '\0')
                break;
            currentcomponentlen = 0;
            i++;
            continue;
        }
        if (currentcomponentlen + 1 < (int)sizeof(currentcomponent)) {
            currentcomponent[currentcomponentlen] = (
                relfolderpath[i]
            );
            currentcomponentlen++;
        }
        i++;
    }
    free(relfolderpath); relfolderpath = NULL;
    char *result = NULL;
    int k = subdir_components_count;
    while (k >= 0) {
        int subdirs_len = 0;
        i = 0;
        while (i < k) {
            if (i + 1 < k)
                subdirs_len++;  // dir sep
            subdirs_len += strlen(subdir_components[k]);
            i++;
        }
        char *checkpath_rel = malloc(
            subdirs_len + 1 + import_relpath_len + 1
        );
        if (!checkpath_rel)
            goto subdircheckoom;
        char *p = checkpath_rel;
        i = 0;
        while (i < k) {
            memcpy(p, subdir_components[k],
                   strlen(subdir_components[k]));
            if (i + 1 < k) {
                #if defined(_WIN32) || defined(_WIN64)
                *p = '\\';
                #else
                *p = '/';
                #endif
                p++;
            }
            i++;
        }
        #if defined(_WIN32) || defined(_WIN64)
        *p = '\\';
        #else
        *p = '/';
        #endif
        p++;
        memcpy(p, import_relpath, import_relpath_len + 1);
        assert((int)strlen(checkpath_rel) ==
               subdirs_len + 1 + import_relpath_len + 1);
        char *checkpath_abs = filesys_Join(
            projectpath, checkpath_rel
        );
        if (!checkpath_abs)
            goto subdircheckoom;
        int _exists_result = 0;
        if (!vfs_ExistsEx(checkpath_abs, checkpath_rel,
                &_exists_result, 0)) {
            free(checkpath_abs);
            free(checkpath_rel);
            goto subdircheckoom;
        }
        if (_exists_result) {
            int _directory_result = 0;
            if (!vfs_IsDirectoryEx(checkpath_abs, checkpath_rel,
                    &_directory_result, 0)) {
                free(checkpath_abs);
                free(checkpath_rel);
                goto subdircheckoom;
            }
            if (!_directory_result) {
                // Match.
                result = checkpath_abs;
                free(checkpath_rel);
                break;
            }
        }
        free(checkpath_abs);
        free(checkpath_rel);
        k--;
    }
    k = 0;
    while (k < subdir_components_count) {
        free(subdir_components[k]);
        k++;
    }
    free(subdir_components);
    free(projectpath);
    free(relfolderpath);
    free(import_relpath);
    if (outofmemory) *outofmemory = 0;
    return result;
}

char *compileproject_FolderGuess(
        const char *fileuri, int cwd_fallback_if_appropriate,
        char **error
        ) {
    assert(fileuri != NULL);
    uriinfo *uinfo = uri_ParseEx(fileuri, "https");
    if (!uinfo || !uinfo->path || !uinfo->protocol ||
            strcasecmp(uinfo->protocol, "file") != 0) {
        uri_Free(uinfo);
        *error = strdup("failed to parse URI, invalid syntax "
            "or not file protocol");
        return NULL;
    }
    char *s = filesys_ToAbsolutePath(uinfo->path);
    char *full_path = (s ? strdup(s) : (char*)NULL);
    uri_Free(uinfo); uinfo = NULL;
    if (!s) {
        *error = strdup("allocation failure, out of memory?");
        return NULL;
    }
    if (!filesys_FileExists(s) || filesys_IsDirectory(s)) {
        free(s);
        free(full_path);
        *error = strdup("path not referring to an existing file, "
            "or lacking permission to access");
        return NULL;
    }

    while (1) {
        // Go up one folder:
        {
            char *snew = filesys_ParentdirOfItem(s);
            if (!snew) {
                free(s);
                free(full_path);
                *error = strdup("alloc failure");
                return NULL;
            }
            while (strlen(snew) > 0 && (snew[strlen(snew) - 1] == '/'
                    #if defined(_WIN32) || defined(_WIN64)
                    || snew[strlen(snew) - 1] == '\\'
                    #endif
                    ))
                snew[strlen(snew) - 1] = '\0';
            if (strcmp(s, snew) == 0) {
                free(s); free(snew);
                s = NULL;
                break;
            }
            free(s);
            s = snew;
        }

        // Check for .git:
        {
            char *git_path = filesys_Join(s, ".git");
            if (!git_path) {
                free(s);
                free(full_path);
                *error = strdup("alloc failure");
                return NULL;
            }
            if (filesys_FileExists(git_path)) {
                free(git_path);
                free(full_path);
                return s;
            }
            free(git_path);
        }

        // Check for horse_modules:
        {
            char *mods_path = filesys_Join(s, "horse_modules");
            if (!mods_path) {
                free(s);
                free(full_path);
                *error = strdup("alloc failure");
                return NULL;
            }
            if (filesys_FileExists(mods_path)) {
                free(mods_path);
                free(full_path);
                return s;
            }
            free(mods_path);
        }
    }

    // Check if we can fall back to the current directory:
    if (cwd_fallback_if_appropriate) {
        char *cwd = filesys_GetCurrentDirectory();
        if (!cwd) {
            free(full_path);
            *error = strdup("alloc failure");
            return NULL;
        }
        if (filesys_FolderContainsPath(cwd, full_path)) {    
            char *result = filesys_Normalize(cwd);
            free(full_path);
            free(cwd);
            if (!result)
                *error = strdup("alloc failure");
            return result;
        }
        free(cwd);
    }

    free(full_path);
    *error = strdup("failed to find project folder");
    return NULL;
}
