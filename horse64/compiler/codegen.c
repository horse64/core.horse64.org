// Copyright (c) 2020, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#include "bytecode.h"
#include "compileconfig.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "compiler/ast.h"
#include "compiler/asthelpers.h"
#include "compiler/astparser.h"
#include "compiler/asttransform.h"
#include "compiler/codegen.h"
#include "compiler/compileproject.h"
#include "compiler/lexer.h"
#include "compiler/main.h"
#include "compiler/varstorage.h"
#include "corelib/errors.h"
#include "hash.h"
#include "itemsort.h"
#include "nonlocale.h"
#include "valuecontentstruct.h"
#include "widechar.h"


typedef struct asttransformcodegenextra {
    int loop_nesting_depth, loop_nesting_alloc;
    int64_t *loop_start_jumpid;
    int64_t *loop_end_jumpid;
} asttransformcodegenextra;

//#define DEBUG_CODEGEN_INSTADD

static void get_assign_lvalue_storage(
        h64expression *expr,
        storageref **out_storageref
        ) {
    assert(expr->type == H64EXPRTYPE_ASSIGN_STMT);
    if (expr->assignstmt.lvalue->type ==
            H64EXPRTYPE_BINARYOP &&
            expr->assignstmt.lvalue->op.optype ==
                H64OP_ATTRIBUTEBYIDENTIFIER &&
            expr->assignstmt.lvalue->op.value2->storage.set) {
        *out_storageref = &(
            expr->assignstmt.lvalue->op.value2->storage.ref
        );
    } else {
        if (expr->assignstmt.lvalue->storage.set) {
            *out_storageref = &expr->assignstmt.lvalue->storage.ref;
        } else {
            assert(
                expr->assignstmt.lvalue->type == H64EXPRTYPE_BINARYOP &&
                (expr->assignstmt.lvalue->op.optype ==
                     H64OP_ATTRIBUTEBYIDENTIFIER ||
                 expr->assignstmt.lvalue->op.optype ==
                     H64OP_INDEXBYEXPR)
            );
            *out_storageref = NULL;
        }
    }
}

static int is_in_extends_arg(h64expression *expr) {
    h64expression *child = expr;
    h64expression *parent = expr->parent;
    while (parent) {
        if (parent->type == H64EXPRTYPE_CLASSDEF_STMT)
            return (
                child == parent->classdef.baseclass_ref
            );
        child = parent;
        parent = parent->parent;
    }
    return 0;
}

static int _newtemp_ex(h64expression *func, int deletepastline) {
    int i = 0;
    while (i < func->funcdef._storageinfo->codegen.extra_temps_count) {
        if (!func->funcdef._storageinfo->codegen.extra_temps_used[i]) {
            func->funcdef._storageinfo->codegen.extra_temps_used[i] = 1;
            func->funcdef._storageinfo->codegen.
                extra_temps_deletepastline[i] = (deletepastline != 0);
            return func->funcdef._storageinfo->
                lowest_guaranteed_free_temp + i;
        }
        i++;
    }
    int *new_used = realloc(
        func->funcdef._storageinfo->codegen.extra_temps_used,
        sizeof(*func->funcdef._storageinfo->codegen.extra_temps_used) * (
        func->funcdef._storageinfo->codegen.extra_temps_count + 1)
    );
    if (!new_used)
        return -1;
    func->funcdef._storageinfo->codegen.extra_temps_used = new_used;
    int *new_deletepastline = realloc(
        func->funcdef._storageinfo->codegen.extra_temps_deletepastline,
        sizeof(*func->funcdef._storageinfo->
               codegen.extra_temps_deletepastline) * (
        func->funcdef._storageinfo->codegen.extra_temps_count + 1)
    );
    if (!new_deletepastline)
        return -1;
    func->funcdef._storageinfo->codegen.extra_temps_deletepastline = (
        new_deletepastline
    );
    func->funcdef._storageinfo->codegen.extra_temps_used[
        func->funcdef._storageinfo->codegen.extra_temps_count
    ] = 1;
    func->funcdef._storageinfo->codegen.extra_temps_deletepastline[
        func->funcdef._storageinfo->codegen.extra_temps_count
    ] = (deletepastline != 0);
    func->funcdef._storageinfo->codegen.extra_temps_count++;
    if (func->funcdef._storageinfo->codegen.extra_temps_count >
            func->funcdef._storageinfo->
            codegen.max_extra_stack) {
        func->funcdef._storageinfo->codegen.max_extra_stack = (
            func->funcdef._storageinfo->codegen.extra_temps_count
        );
    }
    return func->funcdef._storageinfo->lowest_guaranteed_free_temp + (
        func->funcdef._storageinfo->codegen.extra_temps_count - 1
    );
}

int newmultilinetemp(h64expression *func) {
    return _newtemp_ex(func, 0);
}

void free1linetemps(h64expression *func) {
    assert(func != NULL && (
        func->type == H64EXPRTYPE_FUNCDEF_STMT ||
        func->type == H64EXPRTYPE_INLINEFUNCDEF)
    );
    int i = 0;
    while (i < func->funcdef._storageinfo->codegen.extra_temps_count) {
        if (func->funcdef._storageinfo->codegen.extra_temps_used[i] &&
                func->funcdef._storageinfo->codegen.
                    extra_temps_deletepastline[i]) {
            func->funcdef._storageinfo->codegen.extra_temps_used[i] = 0;
        }
        i++;
    }
}

int funccurrentstacktop(h64expression *func) {
    int _top = func->funcdef._storageinfo->lowest_guaranteed_free_temp;
    int i = 0;
    while (i < func->funcdef._storageinfo->codegen.extra_temps_count) {
        if (func->funcdef._storageinfo->codegen.extra_temps_used[i]) {
            _top = (func->funcdef._storageinfo->
                    lowest_guaranteed_free_temp + i + 1);
        }
        i++;
    }
    return _top;
}

void freemultilinetemp(
        h64expression *func, int temp
        ) {
    temp -= func->funcdef._storageinfo->lowest_guaranteed_free_temp;
    assert(temp >= 0 && temp <
           func->funcdef._storageinfo->codegen.extra_temps_count);
    assert(func->funcdef._storageinfo->codegen.extra_temps_used[temp]);
    assert(func->funcdef._storageinfo->codegen.
               extra_temps_deletepastline[temp] == 0);
    func->funcdef._storageinfo->codegen.extra_temps_used[temp] = 0;
}

int new1linetemp(
        h64expression *func, h64expression *expr, int ismainitem
        ) {
    if (ismainitem) {
        // Use temporary 'mandated' by parent if any:
        storageref *parent_store = NULL;
        if (expr && expr->parent &&
                expr->parent->type == H64EXPRTYPE_ASSIGN_STMT &&
                expr->parent->assignstmt.assignop == H64OP_ASSIGN) {
            get_assign_lvalue_storage(expr->parent, &parent_store);
        } else if (expr && expr->parent &&
                expr->parent->type == H64EXPRTYPE_VARDEF_STMT) {
            assert(expr->parent->storage.set);
            if (expr->parent->storage.ref.type == H64STORETYPE_STACKSLOT)
                return (int)expr->parent->storage.ref.id;
        }
        if (parent_store && parent_store->type ==
                H64STORETYPE_STACKSLOT)
            return parent_store->id;

        // If a binary or unary operator, see if we can reuse child storage:
        if (expr && (expr->type == H64EXPRTYPE_BINARYOP ||
                     expr->type == H64EXPRTYPE_UNARYOP)) {
            assert(expr->op.value1 != NULL);
            if (expr->op.value1->storage.eval_temp_id >=
                    func->funcdef._storageinfo->
                    lowest_guaranteed_free_temp) {
                return expr->op.value1->storage.eval_temp_id;
            }
            if (expr->type == H64EXPRTYPE_BINARYOP) {
                assert(expr->op.value2 != NULL);
                if (expr->op.value2->storage.eval_temp_id >=
                        func->funcdef._storageinfo->
                            lowest_guaranteed_free_temp) {
                    return expr->op.value2->storage.eval_temp_id;
                }
            }
        }
    }

    // Get new free temporary:
    assert(func->funcdef._storageinfo != NULL);
    return _newtemp_ex(func, 1);
}

int appendinstbyfuncid(
        h64program *p,
        int id,
        h64expression *correspondingexpr,
        // ^ FIXME: extract & attach debug info from this, like location
        void *ptr
        ) {
    assert(id >= 0 && id < p->func_count);
    assert(!p->func[id].iscfunc);
    assert(((h64instructionany *)ptr)->type != H64INST_INVALID);
    size_t len = h64program_PtrToInstructionSize(ptr);
    #if !defined(NDEBUG) && defined(DEBUG_CODEGEN_INSTADD)
    h64fprintf(
        stderr,
        "horsec: debug: inst appended to: "
        "f%" PRId64 " offset %" PRId64 " inst_type:%s "
        "inst_size:%d\n",
        (int64_t)id, (int64_t)p->func[id].instructions_bytes,
        bytecode_InstructionTypeToStr(
            ((h64instructionany *)ptr)->type
        ),
        (int)len
    );
    #endif
    char *instructionsnew = realloc(
        p->func[id].instructions,
        sizeof(*p->func[id].instructions) *
        (p->func[id].instructions_bytes + len)
    );
    if (!instructionsnew) {
        return 0;
    }
    p->func[id].instructions = instructionsnew;
    assert(p->func[id].instructions != NULL);
    memcpy(
        p->func[id].instructions + p->func[id].instructions_bytes,
        ptr, len
    );
    p->func[id].instructions_bytes += len;
    assert(p->func[id].instructions_bytes >= 0);
    return 1;
}

int appendinst(
        h64program *p,
        h64expression *func,
        h64expression *correspondingexpr,
        void *ptr
        ) {
    assert(p != NULL);
    assert(func != NULL && (func->type == H64EXPRTYPE_FUNCDEF_STMT ||
           func->type == H64EXPRTYPE_INLINEFUNCDEF));
    int id = func->funcdef.bytecode_func_id;
    return appendinstbyfuncid(p, id, correspondingexpr, ptr);
}

void codegen_CalculateFinalFuncStack(
        h64program *program, h64expression *expr) {
    assert(expr != NULL && program != NULL);
    if (expr->type != H64EXPRTYPE_FUNCDEF_STMT)
        return;
    // Determine final amount of temporaries/stack slots used:
    h64funcsymbol *fsymbol = h64debugsymbols_GetFuncSymbolById(
        program->symbols, expr->funcdef.bytecode_func_id
    );
    assert(fsymbol != NULL);
    expr->funcdef._storageinfo->lowest_guaranteed_free_temp +=
        expr->funcdef._storageinfo->codegen.max_extra_stack;
    fsymbol->closure_bound_count =
        expr->funcdef._storageinfo->closureboundvars_count;
    fsymbol->stack_temporaries_count =
        (expr->funcdef._storageinfo->lowest_guaranteed_free_temp -
         fsymbol->closure_bound_count -
         fsymbol->arg_count -
         (fsymbol->has_self_arg ? 1 : 0));
    program->func[expr->funcdef.bytecode_func_id].
        inner_stack_size = fsymbol->stack_temporaries_count;
    program->func[expr->funcdef.bytecode_func_id].
        input_stack_size = (
            fsymbol->closure_bound_count +
            fsymbol->arg_count +
            (fsymbol->has_self_arg ? 1 : 0)
        );
}

h64expression *_fakeclassinitfunc(
        asttransforminfo *rinfo, h64expression *classexpr
        ) {
    assert(classexpr != NULL &&
           classexpr->type == H64EXPRTYPE_CLASSDEF_STMT);
    classid_t classidx = classexpr->classdef.bytecode_class_id;
    assert(
        classidx >= 0 &&
        classidx < rinfo->pr->program->classes_count
    );
    assert(
        rinfo->pr->program->classes[classidx].hasvarinitfunc
    );

    // Make sure the map for registering it by class exists:
    if (!rinfo->pr->_tempclassesfakeinitfunc_map) {
        rinfo->pr->_tempclassesfakeinitfunc_map = (
            hash_NewBytesMap(1024)
        );
        if (!rinfo->pr->_tempclassesfakeinitfunc_map) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
    }

    // If we got an entry already, return it:
    uintptr_t queryresult = 0;
    if (hash_BytesMapGet(
            rinfo->pr->_tempclassesfakeinitfunc_map,
            (const char *)&classidx, sizeof(classidx),
            &queryresult)) {
        assert(queryresult != 0);
        return (h64expression *)(void *)queryresult;
    }

    // Allocate new faked func expression and return it:
    h64expression *fakefunc = malloc(sizeof(*fakefunc));
    if (!fakefunc) {
        rinfo->hadoutofmemory = 1;
        return 0;
    }
    memset(fakefunc, 0, sizeof(*fakefunc));
    fakefunc->storage.eval_temp_id = -1;
    fakefunc->type = (
        H64EXPRTYPE_FUNCDEF_STMT
    );
    fakefunc->funcdef.name = strdup("$$clsinit");
    if (!fakefunc->funcdef.name) {
        oom:
        free(fakefunc->funcdef._storageinfo);
        free(fakefunc->funcdef.name);
        free(fakefunc);
        return NULL;
    }
    fakefunc->funcdef.bytecode_func_id = -1;
    fakefunc->funcdef._storageinfo = malloc(
        sizeof(*fakefunc->funcdef._storageinfo)
    );
    if (!fakefunc->funcdef._storageinfo)
        goto oom;
    memset(
        fakefunc->funcdef._storageinfo, 0,
        sizeof(*fakefunc->funcdef._storageinfo)
    );
    fakefunc->funcdef._storageinfo->closure_with_self = 1;
    fakefunc->funcdef._storageinfo->lowest_guaranteed_free_temp = 1;
    if (!hash_BytesMapSet(
            rinfo->pr->_tempclassesfakeinitfunc_map,
            (const char *)&classidx, sizeof(classidx),
            (uintptr_t)fakefunc
            ))
        goto oom;
    fakefunc->funcdef.bytecode_func_id = (
        rinfo->pr->program->classes[classidx].varinitfuncidx
    );
    fakefunc->storage.set = 1;
    fakefunc->storage.ref.type = (
        H64STORETYPE_GLOBALFUNCSLOT
    );
    fakefunc->storage.ref.id = (
        rinfo->pr->program->classes[classidx].varinitfuncidx
    );
    return fakefunc;
}

h64expression *_fakeglobalinitfunc(asttransforminfo *rinfo) {
    if (rinfo->pr->_tempglobalfakeinitfunc)
        return rinfo->pr->_tempglobalfakeinitfunc;
    rinfo->pr->_tempglobalfakeinitfunc = malloc(
        sizeof(*rinfo->pr->_tempglobalfakeinitfunc)
    );
    if (!rinfo->pr->_tempglobalfakeinitfunc)
        return NULL;
    memset(rinfo->pr->_tempglobalfakeinitfunc, 0,
           sizeof(*rinfo->pr->_tempglobalfakeinitfunc));
    rinfo->pr->_tempglobalfakeinitfunc->storage.eval_temp_id = -1;
    rinfo->pr->_tempglobalfakeinitfunc->type = (
        H64EXPRTYPE_FUNCDEF_STMT
    );
    rinfo->pr->_tempglobalfakeinitfunc->funcdef.name = strdup(
        "$$globalinit"
    );
    if (!rinfo->pr->_tempglobalfakeinitfunc->funcdef.name) {
        oom:
        free(rinfo->pr->_tempglobalfakeinitfunc->funcdef._storageinfo);
        free(rinfo->pr->_tempglobalfakeinitfunc->funcdef.name);
        free(rinfo->pr->_tempglobalfakeinitfunc);
        rinfo->pr->_tempglobalfakeinitfunc = NULL;
        return NULL;
    }
    rinfo->pr->_tempglobalfakeinitfunc->funcdef.bytecode_func_id = -1;
    rinfo->pr->_tempglobalfakeinitfunc->funcdef._storageinfo = (
        malloc(sizeof(
            *rinfo->pr->_tempglobalfakeinitfunc->funcdef._storageinfo
        ))
    );
    if (!rinfo->pr->_tempglobalfakeinitfunc->funcdef._storageinfo)
        goto oom;
    memset(
        rinfo->pr->_tempglobalfakeinitfunc->funcdef._storageinfo, 0,
        sizeof(*rinfo->pr->_tempglobalfakeinitfunc->funcdef._storageinfo)
    );
    int bytecode_id = h64program_RegisterHorse64Function(
        rinfo->pr->program, "$$globalinit",
        rinfo->pr->program->symbols->fileuri[
            rinfo->pr->program->symbols->mainfileuri_index
        ],
        rinfo->pr->program->symbols->fileurilen[
            rinfo->pr->program->symbols->mainfileuri_index
        ],
        0, NULL,
        rinfo->pr->program->symbols->mainfile_module_path,
        "", -1
    );
    if (bytecode_id < 0)
        goto oom;
    rinfo->pr->program->func[bytecode_id].is_threadable = 0;
    rinfo->pr->_tempglobalfakeinitfunc->
        funcdef.bytecode_func_id = bytecode_id;
    rinfo->pr->program->globalinit_func_index = bytecode_id;
    rinfo->pr->_tempglobalfakeinitfunc->storage.set = 1;
    rinfo->pr->_tempglobalfakeinitfunc->storage.ref.type = (
        H64STORETYPE_GLOBALFUNCSLOT
    );
    rinfo->pr->_tempglobalfakeinitfunc->storage.ref.id = (
        bytecode_id
    );
    return rinfo->pr->_tempglobalfakeinitfunc;
}

struct _jumpinfo {
    int jumpid;
    int64_t offset;
};

static int _resolve_jumpid_to_jumpoffset(
        h64compileproject *prj,
        int jumpid, int64_t offset,
        struct _jumpinfo *jump_info,
        int jump_table_fill,
        int *out_oom,
        int16_t *out_jumpoffset
        ) {
    int64_t jumptargetoffset = -1;
    int z = 0;
    while (z < jump_table_fill) {
        if (jump_info[z].jumpid == jumpid) {
            jumptargetoffset = jump_info[z].offset;
            break;
        }
        z++;
    }
    if (jumptargetoffset < 0) {
        // Shouldn't happen, unless there is a bug
        if (out_oom) *out_oom = 0;
        return 0;
    }
    jumptargetoffset -= offset;
    if (jumptargetoffset == 0) {
        prj->resultmsg->success = 0;
        char buf[256];
        snprintf(buf, sizeof(buf) - 1, "internal error: "
            "found jump instruction in func at "
            "instruction pos %" PRId64 " "
            "that has invalid zero relative offset - "
            "codegen bug?",
            (int64_t)offset
        );
        if (!result_AddMessage(
                prj->resultmsg,
                H64MSG_ERROR, buf,
                NULL, 0, -1, -1
                )) {
            if (out_oom) *out_oom = 1;
            return 0;
        }
        if (out_oom) *out_oom = 0;
        return 0;
    }
    if (jumptargetoffset > 65535 || jumptargetoffset < -65535) {
        prj->resultmsg->success = 0;
        char buf[256];
        snprintf(buf, sizeof(buf) - 1,
            "found jump instruction in func at "
            "instruction pos %" PRId64 " "
            "that exceeds 16bit int range, this is not supported",
            (int64_t)offset
        );
        if (!result_AddMessage(
                prj->resultmsg,
                H64MSG_ERROR, buf,
                NULL, 0, -1, -1
                )) {
            if (out_oom) *out_oom = 1;
            return 0;
        }
        if (out_oom) *out_oom = 0;
        return 0;
    }
    if (out_jumpoffset) *out_jumpoffset = jumptargetoffset;
    return 1;
}

static h64instruction_callsettop *_settop_inst(
        asttransforminfo *rinfo, h64expression *func,
        int64_t offset
        ) {
    return (h64instruction_callsettop *)(
        rinfo->pr->program->func[
            func->funcdef.bytecode_func_id
        ].instructions + offset
    );
}

typedef struct kwargsortinfo {
    int64_t kwnameindex;
    int callargno;
} kwargsortinfo;

static int _compare_kw_args(void *item1, void *item2) {
    kwargsortinfo *kwinfo1 = item1;
    kwargsortinfo *kwinfo2 = item2;
    if (kwinfo1->kwnameindex < kwinfo2->kwnameindex)
        return -1;
    else if (kwinfo1->kwnameindex > kwinfo2->kwnameindex)
        return 1;
    return 0;
}

static int _codegen_call_to(
        asttransforminfo *rinfo, h64expression *func,
        h64expression *callexpr,
        int calledexprstoragetemp, int resulttemp,
        int ignoreifnone
        ) {
    assert(callexpr->type == H64EXPRTYPE_CALL);
    int _argtemp = funccurrentstacktop(func);
    int posargcount = 0;
    int expandlastposarg = 0;
    int kwargcount = 0;
    int _reachedkwargs = 0;
    h64instruction_callsettop inst_callsettop = {0};
    inst_callsettop.type = H64INST_CALLSETTOP;
    inst_callsettop.topto = _argtemp;
    if (!appendinst(
            rinfo->pr->program, func, callexpr, &inst_callsettop
            )) {
        rinfo->hadoutofmemory = 1;
        return 0;
    }
    int64_t callsettop_offset = (
        rinfo->pr->program->func[
            func->funcdef.bytecode_func_id
        ].instructions_bytes -
        sizeof(h64instruction_callsettop)
    );
    // Pre-iteration: collect kw arg indexes, and sort them:
    kwargsortinfo _arg_indexes_buf[32];
    kwargsortinfo *arg_kwsortinfo = _arg_indexes_buf;
    int alloc_heap = 0;
    {
        int alloc_size = 32;
        if (callexpr->inlinecall.arguments.arg_count > alloc_size) {
            arg_kwsortinfo = malloc(
                sizeof(*arg_kwsortinfo) * (
                    callexpr->inlinecall.arguments.arg_count
                )
            );
            if (!arg_kwsortinfo) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            alloc_heap = 1;
        }
        int kwargs_start_slot = -1;
        int i = 0;
        while (i < callexpr->inlinecall.arguments.arg_count) {
            assert(callexpr->inlinecall.arguments.arg_name != NULL);
            #ifndef NDEBUG
            if (callexpr->inlinecall.arguments.arg_value == NULL) {
                h64printf(
                    "horsec: error: internal error: "
                    "invalid call expression with arg count > 0, "
                    "but arg_value array is NULL\n"
                );
                char *s = ast_ExpressionToJSONStr(callexpr, NULL, 0);
                h64printf(
                    "horsec: error: internal error: "
                    "expr is: %s\n", s
                );
                free(s);
            }
            #endif
            arg_kwsortinfo[i].callargno = i;
            if (!callexpr->inlinecall.arguments.arg_name[i]) {
                arg_kwsortinfo[i].kwnameindex = -1;
            } else {
                if (kwargs_start_slot < 0)
                    kwargs_start_slot = i;
                int64_t kwnameidx = (
                h64debugsymbols_AttributeNameToAttributeNameId(
                    rinfo->pr->program->symbols,
                    callexpr->inlinecall.arguments.arg_name[i],
                    0, 0
                ));
                if (kwnameidx < 0) {
                    char buf[256];
                    snprintf(buf, sizeof(buf) - 1,
                        "unknown keyword argument \"%s\" "
                        "will cause runtime error with this function",
                        callexpr->inlinecall.arguments.arg_name[i]
                    );
                    if (!result_AddMessage(
                            &rinfo->ast->resultmsg,
                            H64MSG_WARNING, buf,
                            rinfo->ast->fileuri, rinfo->ast->fileurilen,
                            callexpr->line, callexpr->column
                            )) {
                        rinfo->hadoutofmemory = 1;
                        if (alloc_heap)
                            free(arg_kwsortinfo);
                        return 0;
                    }
                    // Unknown keyword arg, so hardcode an error:
                    const char errmsg[] = (
                        "called func does not recognize all passed "
                        "keyword arguments"
                    );
                    h64wchar *msg = NULL;
                    int64_t msglen = 0;
                    msg = utf8_to_utf32(
                        errmsg, strlen(errmsg), NULL, NULL, &msglen
                    );
                    if (!msg) {
                        rinfo->hadoutofmemory = 1;
                        if (alloc_heap)
                            free(arg_kwsortinfo);
                        return 0;
                    }
                    int temp2 = new1linetemp(
                        func, callexpr, 0
                    );
                    h64instruction_setconst inst_str = {0};
                    inst_str.type = H64INST_SETCONST;
                    inst_str.slot = temp2;
                    inst_str.content.type = H64VALTYPE_CONSTPREALLOCSTR;
                    inst_str.content.constpreallocstr_len = msglen;
                    inst_str.content.constpreallocstr_value = msg;
                    if (!appendinst(
                            rinfo->pr->program, func,
                            callexpr->inlinecall.arguments.arg_value[i],
                            &inst_str
                            )) {
                        rinfo->hadoutofmemory = 1;
                        free(msg);
                        if (alloc_heap)
                            free(arg_kwsortinfo);
                        return 0;
                    }
                    h64instruction_raise inst_raise = {0};
                    inst_raise.type = H64INST_RAISE;
                    inst_raise.error_class_id = H64STDERROR_ARGUMENTERROR;
                    inst_raise.sloterrormsgobj = temp2;
                    if (!appendinst(
                            rinfo->pr->program, func,
                            callexpr->inlinecall.arguments.arg_value[i],
                            &inst_raise
                            )) {
                        rinfo->hadoutofmemory = 1;
                        if (alloc_heap)
                            free(arg_kwsortinfo);
                        return 0;
                    }
                    return 1;
                }
                arg_kwsortinfo[i].kwnameindex = kwnameidx;
            }
            i++;
        }
        // Ok we collected it all, now sort it;
        if (kwargs_start_slot >= 0) {
            int kwargs_count = (
                callexpr->inlinecall.arguments.arg_count -
                kwargs_start_slot
            );
            assert(kwargs_count > 0);
            int oom = 0; int unsortable = 0;
            int sortresult = itemsort_Do(
                &arg_kwsortinfo[kwargs_start_slot],
                kwargs_count * sizeof(
                    arg_kwsortinfo[kwargs_start_slot]
                ), sizeof(
                    arg_kwsortinfo[kwargs_start_slot]
                ), &_compare_kw_args, &oom, &unsortable
            );
            if (!sortresult) {
                rinfo->hadoutofmemory = 1;
                if (alloc_heap)
                    free(arg_kwsortinfo);
                return 0;
            }
            assert(sortresult != 0);
        }
    }
    // Now that kw args are sorted, emit in-order arguments:
    int i = 0;
    while (i < callexpr->inlinecall.arguments.arg_count) {
        if (arg_kwsortinfo[i].kwnameindex >= 0)
            _reachedkwargs = 1;
        if (_reachedkwargs) {
            kwargcount++;
            int64_t kwnameidx = arg_kwsortinfo[i].kwnameindex;
            assert(kwnameidx >= 0);
            h64instruction_setconst inst_setconst = {0};
            inst_setconst.type = H64INST_SETCONST;
            inst_setconst.slot = _argtemp;
            inst_setconst.content.type = H64VALTYPE_INT64;
            inst_setconst.content.int_value = kwnameidx;
            if (!appendinst(
                    rinfo->pr->program, func, callexpr,
                    &inst_setconst)) {
                rinfo->hadoutofmemory = 1;
                if (alloc_heap)
                    free(arg_kwsortinfo);
                return 0;
            }
            _argtemp++;
            _settop_inst(rinfo, func, callsettop_offset)->topto++;
            h64instruction_valuecopy inst_vc = {0};
            inst_vc.type = H64INST_VALUECOPY;
            inst_vc.slotto = _argtemp;
            inst_vc.slotfrom = (
                callexpr->inlinecall.arguments.arg_value[
                    arg_kwsortinfo[i].callargno
                ]->storage.eval_temp_id);
            assert(inst_vc.slotto >= 0);
            assert(inst_vc.slotfrom >= 0);
            _argtemp++;
            _settop_inst(rinfo, func, callsettop_offset)->topto++;
            if (!appendinst(
                    rinfo->pr->program, func, callexpr,
                    &inst_vc)) {
                rinfo->hadoutofmemory = 1;
                if (alloc_heap)
                    free(arg_kwsortinfo);
                return 0;
            }
        } else {
            posargcount++;
            h64instruction_valuecopy inst_vc = {0};
            inst_vc.type = H64INST_VALUECOPY;
            inst_vc.slotto = _argtemp;
            inst_vc.slotfrom = (
                callexpr->inlinecall.arguments.arg_value[i]->
                    storage.eval_temp_id);
            assert(inst_vc.slotto >= 0);
            assert(inst_vc.slotfrom >= 0);
            _argtemp++;
            _settop_inst(rinfo, func, callsettop_offset)->topto++;
            if (!appendinst(
                    rinfo->pr->program, func, callexpr,
                    &inst_vc)) {
                rinfo->hadoutofmemory = 1;
                if (alloc_heap)
                    free(arg_kwsortinfo);
                return 0;
            }
            if (callexpr->inlinecall.expand_last_posarg) {
                expandlastposarg = 1;
            }
        }
        i++;
    }
    if (alloc_heap)
        free(arg_kwsortinfo);
    arg_kwsortinfo = NULL;
    // Ok, now we got arguments done so do actual call:
    int maxslotsused = _argtemp - (
        func->funcdef._storageinfo->lowest_guaranteed_free_temp
    );
    if (maxslotsused > func->funcdef._storageinfo->
            codegen.max_extra_stack)
        func->funcdef._storageinfo->codegen.max_extra_stack = (
            maxslotsused
        );
    int temp = resulttemp;  // may be -1
    if (ignoreifnone) {
        h64instruction_callignoreifnone inst_call = {0};
        inst_call.type = H64INST_CALLIGNOREIFNONE;
        inst_call.returnto = temp;
        inst_call.slotcalledfrom = calledexprstoragetemp;
        inst_call.flags = 0 | (
            expandlastposarg ? CALLFLAG_UNPACKLASTPOSARG : 0
        ) | (callexpr->inlinecall.is_async ? CALLFLAG_ASYNC : 0);
        inst_call.posargs = posargcount;
        inst_call.kwargs = kwargcount;
        if (!appendinst(
                rinfo->pr->program, func, callexpr, &inst_call
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
    } else {
        h64instruction_call inst_call = {0};
        inst_call.type = H64INST_CALL;
        inst_call.returnto = temp;
        inst_call.slotcalledfrom = calledexprstoragetemp;
        inst_call.posargs = posargcount;
        inst_call.kwargs = kwargcount;
        inst_call.flags = 0 | (
            expandlastposarg ? CALLFLAG_UNPACKLASTPOSARG : 0
        ) | (callexpr->inlinecall.is_async ? CALLFLAG_ASYNC : 0);
        if (!appendinst(
                rinfo->pr->program, func, callexpr, &inst_call
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
    }
    callexpr->storage.eval_temp_id = temp;  // allowed to be -1!
    return 1;
}

int codegen_FinalBytecodeTransform(
        h64compileproject *prj
        ) {
    int haveerrors = 0;
    {
        int i = 0;
        while (i < prj->resultmsg->message_count) {
            if (prj->resultmsg->message[i].type == H64MSG_ERROR) {
                haveerrors = 1;
            }
            i++;
        }
    }
    if (!prj->resultmsg->success || haveerrors)
        return 1;

    int jump_table_alloc = 0;
    int jump_table_fill = 0;
    struct _jumpinfo *jump_info = NULL;
    h64program *pr = prj->program;

    int i = 0;
    while (i < pr->func_count) {  // Giant loop to resolve all the jumps!
        if (pr->func[i].iscfunc) {
            i++;
            continue;
        }
        jump_table_fill = 0;

        assert(pr->func[i].instructions != NULL ||
               pr->func[i].instructions_bytes == 0);

        // Remove jumptarget instructions while extracting offsets:
        int64_t k = 0;
        while (k < pr->func[i].instructions_bytes) {
            h64instructionany *inst = (
                (h64instructionany *)((char*)pr->func[i].instructions + k)
            );
            assert(inst->type != H64INST_INVALID);
            if (inst->type == H64INST_JUMPTARGET) {
                if (jump_table_fill + 1 > jump_table_alloc) {
                    struct _jumpinfo *new_jump_info = realloc(
                        jump_info,
                        sizeof(*jump_info) *
                            ((jump_table_fill + 1) * 2 + 10)
                    );
                    if (!new_jump_info) {
                        free(jump_info);
                        return 0;
                    }
                    jump_table_alloc = (
                        (jump_table_fill + 1) * 2 + 10
                    );
                    jump_info = new_jump_info;
                }
                memset(
                    &jump_info[jump_table_fill], 0,
                    sizeof(*jump_info)
                );
                jump_info[jump_table_fill].offset = k;
                assert(k >= 0);
                jump_info[jump_table_fill].jumpid = (
                    ((h64instruction_jumptarget *)inst)->jumpid
                );
                assert(k + (int)sizeof(h64instruction_jumptarget) <=
                       pr->func[i].instructions_bytes);
                memmove(
                    ((char*)pr->func[i].instructions) + k,
                    ((char*)pr->func[i].instructions) + k +
                        sizeof(h64instruction_jumptarget),
                    ((int64_t)pr->func[i].instructions_bytes) - (
                        k + sizeof(h64instruction_jumptarget)
                    )
                );
                pr->func[i].instructions_bytes -=
                    sizeof(h64instruction_jumptarget);
                jump_table_fill++;
                continue;
            }
            k += (int64_t)h64program_PtrToInstructionSize((char*)inst);
        }

        // Rewrite jumps to the actual offsets:
        k = 0;
        while (k < pr->func[i].instructions_bytes) {
            h64instructionany *inst = (
                (h64instructionany *)((char*)pr->func[i].instructions + k)
            );

            int32_t jumpid = -1;
            int32_t jumpid2 = -1;

            switch (inst->type) {
            case H64INST_CONDJUMP: {
                h64instruction_condjump *cjump = (
                    (h64instruction_condjump *)inst
                );
                jumpid = cjump->jumpbytesoffset;
                break;
            }
            case H64INST_CONDJUMPEX: {
                h64instruction_condjumpex *cjump = (
                    (h64instruction_condjumpex *)inst
                );
                jumpid = cjump->jumpbytesoffset;
                break;
            }
            case H64INST_JUMP: {
                h64instruction_jump *jump = (
                    (h64instruction_jump *)inst
                );
                jumpid = jump->jumpbytesoffset;
                break;
            }
            case H64INST_HASATTRJUMP: {
                h64instruction_hasattrjump *hajump = (
                    (h64instruction_hasattrjump *)inst
                );
                jumpid = hajump->jumpbytesoffset;
                break;
            }
            case H64INST_PUSHRESCUEFRAME: {
                h64instruction_pushrescueframe *catchjump = (
                    (h64instruction_pushrescueframe *)inst
                );
                if ((catchjump->mode & RESCUEMODE_JUMPONRESCUE) != 0) {
                    jumpid = catchjump->jumponrescue;
                    assert(jumpid >= 0);
                }
                if ((catchjump->mode & RESCUEMODE_JUMPONFINALLY) != 0) {
                    jumpid2 = catchjump->jumponfinally;
                    assert(jumpid2 >= 0);
                }
                break;
            }
            case H64INST_ITERATE: {
                h64instruction_iterate *iterate = (
                    (h64instruction_iterate *)inst
                );
                jumpid = iterate->jumponend;
                break;
            }
            default: {
                k += (int64_t)h64program_PtrToInstructionSize((char*)inst);
                continue;
            }
            }
            assert(jumpid >= 0 || jumpid2 >= 0);

            // FIXME: use a faster algorithm here, maybe hash table?
            if (jumpid >= 0) {
                int hadoom = 0;
                int16_t offset = 0;
                int resolveworked = _resolve_jumpid_to_jumpoffset(
                    prj, jumpid, k, jump_info, jump_table_fill,
                    &hadoom, &offset
                );
                if (!resolveworked) {
                    free(jump_info);
                    if (prj->resultmsg->success && !hadoom) {
                        h64fprintf(
                            stderr, "horsec: error: internal error in "
                            "codegen jump translation: failed to resolve "
                            "jump %" PRId64 " to target offset for jump at "
                            "instruction offset %" PRId64
                            " in func %" PRId64 " BUT NO ERROR"
                            "\n",
                            (int64_t)jumpid, (int64_t)k,
                            (int64_t)i
                        );
                    }
                    prj->resultmsg->success = 0;
                    return 0;
                }

                switch (inst->type) {
                case H64INST_CONDJUMP: {
                    h64instruction_condjump *cjump = (
                        (h64instruction_condjump *)inst
                    );
                    cjump->jumpbytesoffset = offset;
                    break;
                }
                case H64INST_CONDJUMPEX: {
                    h64instruction_condjumpex *cjump = (
                        (h64instruction_condjumpex *)inst
                    );
                    cjump->jumpbytesoffset = offset;
                    break;
                }
                case H64INST_JUMP: {
                    h64instruction_jump *jump = (
                        (h64instruction_jump *)inst
                    );
                    jump->jumpbytesoffset = offset;
                    break;
                }
                case H64INST_HASATTRJUMP: {
                    h64instruction_hasattrjump *hajump = (
                        (h64instruction_hasattrjump *)inst
                    );
                    hajump->jumpbytesoffset = offset;
                    break;
                }
                case H64INST_PUSHRESCUEFRAME: {
                    h64instruction_pushrescueframe *catchjump = (
                        (h64instruction_pushrescueframe *)inst
                    );
                    catchjump->jumponrescue = offset;
                    break;
                }
                case H64INST_ITERATE: {
                    h64instruction_iterate *iterate = (
                        (h64instruction_iterate *)inst
                    );
                    iterate->jumponend = offset;
                    break;
                }
                default:
                    h64fprintf(
                        stderr, "horsec: error: internal error in "
                        "codegen jump translation: unhandled jump type\n"
                    );
                    free(jump_info);
                    return 0;
                }
            }
            if (jumpid2 >= 0) {
                int hadoom = 0;
                int16_t offset = 0;
                int resolveworked = _resolve_jumpid_to_jumpoffset(
                    prj, jumpid2, k, jump_info, jump_table_fill,
                    &hadoom, &offset
                );
                if (!resolveworked) {
                    free(jump_info);
                    if (prj->resultmsg->success && !hadoom) {
                        h64fprintf(
                            stderr, "horsec: error: internal error in "
                            "codegen jump translation: failed to resolve "
                            "jump %" PRId64 " to target offset for jump at "
                            "instruction offset %" PRId64
                            " in func %" PRId64 " BUT NO ERROR"
                            "\n",
                            (int64_t)jumpid2, (int64_t)k,
                            (int64_t)i
                        );
                    }
                    prj->resultmsg->success = 0;
                    return 0;
                }

                switch (inst->type) {
                case H64INST_PUSHRESCUEFRAME: {
                    h64instruction_pushrescueframe *catchjump = (
                        (h64instruction_pushrescueframe *)inst
                    );
                    catchjump->jumponfinally = offset;
                    break;
                }
                default:
                    h64fprintf(
                        stderr, "horsec: error: internal error in "
                        "codegen jump translation: unhandled jump type\n"
                    );
                    free(jump_info);
                    return 0;
                }
            }
            k += (int64_t)h64program_PtrToInstructionSize((char*)inst);
        }
        i++;
    }
    int i2 = 0;
    while (i2 < pr->func_count) {
        if (pr->func[i2].iscfunc) {
            i2++;
            continue;
        }
        jump_table_fill = 0;

        int func_ends_in_return = 0;
        int64_t k = 0;
        while (k < pr->func[i2].instructions_bytes) {
            h64instructionany *inst = (
                (h64instructionany *)((char*)pr->func[i2].instructions + k)
            );
            size_t instsize = (
                h64program_PtrToInstructionSize((char*)inst)
            );
            if (k + (int)instsize >= pr->func[i2].instructions_bytes &&
                    inst->type == H64INST_RETURNVALUE) {
                func_ends_in_return = 1;
            }
            k += (int64_t)instsize;
        }
        if (!func_ends_in_return) {
            // Add return to the end:
            if (pr->func[i2].inner_stack_size <= 0)
                pr->func[i2].inner_stack_size = 1;
            h64instruction_setconst inst_setnone = {0};
            inst_setnone.type = H64INST_SETCONST;
            inst_setnone.slot = 0;
            inst_setnone.content.type = H64VALTYPE_NONE;
            if (!appendinstbyfuncid(pr, i2, NULL, &inst_setnone)) {
                return 0;
            }
            h64instruction_returnvalue inst_return = {0};
            inst_return.type = H64INST_RETURNVALUE;
            inst_return.returnslotfrom = 0;
            if (!appendinstbyfuncid(pr, i2, NULL, &inst_return)) {
                return 0;
            }
        }

        i2++;
    }
    free(jump_info);
    return 1;
}

int _codegencallback_DoCodegen_visit_out(
        h64expression *expr, ATTR_UNUSED h64expression *parent, void *ud
        ) {
    asttransforminfo *rinfo = (asttransforminfo *)ud;
    ATTR_UNUSED asttransformcodegenextra *extra = rinfo->userdata;
    codegen_CalculateFinalFuncStack(rinfo->pr->program, expr);

    // FIRST, before anything else: ignore "none" literals entirely that
    // do nothing (= that are used for class var attr assigns)
    if (expr->type == H64EXPRTYPE_LITERAL &&
            expr->literal.type == H64TK_CONSTANT_NONE &&
            parent != NULL &&
            parent->type == H64EXPRTYPE_VARDEF_STMT &&
            surroundingclass(parent, 0) != NULL &&
            parent->vardef.value == expr) {
        // Must ignore this entirely and bail out NOW,
        // or it will fail to get the "func" scope right below.
        return 1;
    }

    // Determine func scope:
    h64expression *func = surroundingfunc(expr);
    if (!func) {
        h64expression *sclass = surroundingclass(expr, 0);
        if (sclass != NULL) {
            // It's inside a class, but outside a func. All expressions
            // that evaluate here need to happen in $$clsinit.
            if (expr->type == H64EXPRTYPE_FUNCDEF_STMT ||
                    rinfo->pr->program->classes[
                        sclass->classdef.bytecode_class_id
                    ].varattr_count == 0
                    ) {
                // This is unrelated to var initialization, and/or
                // we have no var attributes anyway.
                // Since this means no $$clsinit func, don't attempt
                // to get it.
                func = NULL;
            } else if (expr->type == H64EXPRTYPE_VARDEF_STMT &&
                    (expr->vardef.value == NULL ||
                     (expr->vardef.value->type == H64EXPRTYPE_LITERAL &&
                      expr->vardef.value->literal.type ==
                          H64TK_CONSTANT_NONE))) {
                // While it is a vardef, it has no initialization code.
                // So codegen shouldn't yield anything.
                func = NULL;
            } else {
                func = _fakeclassinitfunc(rinfo, sclass);
                if (!func) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        } else {
            // Inside global space. -> any expression initializing something
            // needs to go into the global init func.
            func = _fakeglobalinitfunc(rinfo);
            if (!func) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
    }
    if (expr->type == H64EXPRTYPE_LIST ||
            expr->type == H64EXPRTYPE_SET) {
        int isset = (expr->type == H64EXPRTYPE_SET);
        int listtmp = new1linetemp(
            func, expr, 1
        );
        if (listtmp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        if (!isset) {
            h64instruction_newlist inst = {0};
            inst.type = H64INST_NEWLIST;
            inst.slotto = listtmp;
            if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            h64instruction_newset inst = {0};
            inst.type = H64INST_NEWSET;
            inst.slotto = listtmp;
            if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        int64_t entry_count = (
            isset ? expr->constructorset.entry_count :
            expr->constructorlist.entry_count
        );
        int64_t add_name_idx =
            h64debugsymbols_AttributeNameToAttributeNameId(
                rinfo->pr->program->symbols, "add", 1, 0
            );
        if (entry_count > 0) {
            int addfunctemp = new1linetemp(
                func, expr, 0
            );
            assert(addfunctemp >= func->funcdef._storageinfo->
                   lowest_guaranteed_free_temp);
            if (addfunctemp < 0) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            h64instruction_getattributebyname instgetattr = {0};
            instgetattr.type = H64INST_GETATTRIBUTEBYNAME;
            instgetattr.slotto = addfunctemp;
            instgetattr.objslotfrom = listtmp;
            instgetattr.nameidx = add_name_idx;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &instgetattr
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            int argsfloor = funccurrentstacktop(func);
            int i = 0;
            while (i < entry_count) {
                int item_slot = (
                    (isset ? expr->constructorset.entry[i]->
                        storage.eval_temp_id :
                        expr->constructorlist.entry[i]->
                        storage.eval_temp_id)
                );
                assert(item_slot >= 0);
                h64instruction_valuecopy instvcopy = {0};
                instvcopy.type = H64INST_VALUECOPY;
                instvcopy.slotto = argsfloor;
                instvcopy.slotfrom = item_slot;
                if (!appendinst(rinfo->pr->program, func, expr,
                                &instvcopy)) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                h64instruction_callsettop inststop = {0};
                inststop.type = H64INST_CALLSETTOP;
                inststop.topto = argsfloor + 1;
                if (!appendinst(rinfo->pr->program, func, expr,
                                &inststop)) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                h64instruction_call instcall = {0};
                instcall.type = H64INST_CALL;
                instcall.returnto = argsfloor;
                instcall.slotcalledfrom = addfunctemp;
                instcall.posargs = 1;
                instcall.kwargs = 0;
                instcall.flags = 0;
                if (!appendinst(rinfo->pr->program, func, expr,
                                &instcall)) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                i++;
            }
            if ((argsfloor + 1) - func->funcdef._storageinfo->
                    lowest_guaranteed_free_temp >
                    func->funcdef._storageinfo->
                    codegen.max_extra_stack) {
                func->funcdef._storageinfo->codegen.max_extra_stack = (
                    (argsfloor + 1) - func->funcdef._storageinfo->
                    lowest_guaranteed_free_temp
                );
            }
        }
        expr->storage.eval_temp_id = listtmp;
    } else if (expr->type == H64EXPRTYPE_AWAIT_STMT) {
        assert(expr->awaitstmt.awaitedvalue->storage.eval_temp_id >= 0);
        h64instruction_awaititem inst = {0};
        inst.type = H64INST_AWAITITEM;
        inst.objslotawait = (
            expr->awaitstmt.awaitedvalue->storage.eval_temp_id
        );
        if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
    } else if (expr->type == H64EXPRTYPE_VECTOR ||
               expr->type == H64EXPRTYPE_MAP) {
        int ismap = (expr->type == H64EXPRTYPE_MAP);
        int vectortmp = new1linetemp(
            func, expr, 1
        );
        if (vectortmp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        if (ismap) {
            h64instruction_newmap inst = {0};
            inst.type = H64INST_NEWMAP;
            inst.slotto = vectortmp;
            if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            h64instruction_newvector inst = {0};
            inst.type = H64INST_NEWVECTOR;
            inst.slotto = vectortmp;
            if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        int64_t entry_count = (
            ismap ? expr->constructormap.entry_count :
            expr->constructorvector.entry_count
        );
        int keytmp = -1;
        if (ismap) {
            keytmp = new1linetemp(
                func, expr, 0
            );
            if (keytmp < 0) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        int i = 0;
        while (i < entry_count) {
            int item_slot = (
                (ismap ? expr->constructormap.value[i]->
                    storage.eval_temp_id :
                    expr->constructorvector.entry[i]->
                    storage.eval_temp_id)
            );
            assert(item_slot >= 0);
            int key_slot = (
                (ismap ? expr->constructormap.key[i]->
                    storage.eval_temp_id :
                    keytmp)
            );
            assert(key_slot >= 0);
            if (!ismap) {
                h64instruction_setconst instsc = {0};
                instsc.type = H64INST_SETCONST;
                instsc.slot = key_slot;
                instsc.content.type = H64VALTYPE_INT64;
                instsc.content.int_value = i;
            }
            h64instruction_setbyindexexpr instbyindexexpr = {0};
            instbyindexexpr.type = H64INST_SETBYINDEXEXPR;
            instbyindexexpr.slotobjto = vectortmp;
            instbyindexexpr.slotindexto = key_slot;
            instbyindexexpr.slotvaluefrom = item_slot;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &instbyindexexpr
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            i++;
        }
        expr->storage.eval_temp_id = vectortmp;
    } else if (expr->type == H64EXPRTYPE_LITERAL) {
        int temp = new1linetemp(func, expr, 1);
        if (temp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        h64instruction_setconst inst = {0};
        inst.type = H64INST_SETCONST;
        inst.slot = temp;
        memset(&inst.content, 0, sizeof(inst.content));
        if (expr->literal.type == H64TK_CONSTANT_INT) {
            inst.content.type = H64VALTYPE_INT64;
            inst.content.int_value = expr->literal.int_value;
        } else if (expr->literal.type == H64TK_CONSTANT_FLOAT) {
            inst.content.type = H64VALTYPE_FLOAT64;
            inst.content.float_value = expr->literal.float_value;
        } else if (expr->literal.type == H64TK_CONSTANT_BOOL) {
            inst.content.type = H64VALTYPE_BOOL;
            inst.content.int_value = expr->literal.int_value;
        } else if (expr->literal.type == H64TK_CONSTANT_NONE) {
            inst.content.type = H64VALTYPE_NONE;
        } else if (expr->literal.type == H64TK_CONSTANT_BYTES) {
            inst.content.type = H64VALTYPE_SHORTBYTES;
            uint64_t len = expr->literal.str_value_len;
            if (strlen(expr->literal.str_value) <
                    VALUECONTENT_SHORTBYTESLEN) {
                memcpy(
                    inst.content.shortbytes_value,
                    expr->literal.str_value, len
                );
                inst.content.type = H64VALTYPE_SHORTBYTES;
                inst.content.shortbytes_len = len;
            } else {
                inst.content.type = H64VALTYPE_CONSTPREALLOCBYTES;
                inst.content.constpreallocbytes_value = malloc(len);
                if (!inst.content.constpreallocbytes_value) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                inst.content.constpreallocbytes_len = len;
                memcpy(
                    inst.content.constpreallocbytes_value,
                    expr->literal.str_value, len
                );
            }
        } else if (expr->literal.type == H64TK_CONSTANT_STRING) {
            inst.content.type = H64VALTYPE_SHORTSTR;
            assert(expr->literal.str_value != NULL);
            int64_t out_len = 0;
            int abortinvalid = 0;
            int abortoom = 0;
            h64wchar *result = utf8_to_utf32_ex(
                expr->literal.str_value,
                expr->literal.str_value_len,
                NULL, 0,
                NULL, NULL, &out_len, 1, 0,
                &abortinvalid, &abortoom
            );
            if (!result) {
                if (abortoom) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                char buf[256];
                snprintf(buf, sizeof(buf) - 1,
                    "internal error: utf8 to utf32 "
                    "conversion unexpectedly failed"
                );
                if (!result_AddMessage(
                        &rinfo->ast->resultmsg,
                        H64MSG_ERROR, buf,
                        rinfo->ast->fileuri, rinfo->ast->fileurilen,
                        expr->line, expr->column
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                return 1;
            }
            assert(!abortinvalid);
            assert(!abortoom);
            if (out_len <= VALUECONTENT_SHORTSTRLEN) {
                memcpy(
                    inst.content.shortstr_value,
                    result, out_len * sizeof(*result)
                );
                inst.content.type = H64VALTYPE_SHORTSTR;
                inst.content.shortstr_len = out_len;
            } else {
                inst.content.type = H64VALTYPE_CONSTPREALLOCSTR;
                inst.content.constpreallocstr_value = malloc(
                    out_len * sizeof(*result)
                );
                if (!inst.content.constpreallocstr_value) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                inst.content.constpreallocstr_len = out_len;
                memcpy(
                    inst.content.constpreallocstr_value,
                    result, out_len * sizeof(*result)
                );
            }
            free(result);
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf) - 1,
                "internal error: unhandled literal type %d",
                (int)expr->literal.type
            );
            if (!result_AddMessage(
                    &rinfo->ast->resultmsg,
                    H64MSG_ERROR, buf,
                    rinfo->ast->fileuri, rinfo->ast->fileurilen,
                    expr->line, expr->column
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            return 1;
        }
        if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
            if (inst.content.type == H64VALTYPE_CONSTPREALLOCSTR)
                free(inst.content.constpreallocstr_value);
            else if (inst.content.type == H64VALTYPE_CONSTPREALLOCBYTES)
                free(inst.content.constpreallocbytes_value);
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        expr->storage.eval_temp_id = temp;
    } else if (expr->type == H64EXPRTYPE_WHILE_STMT ||
            expr->type == H64EXPRTYPE_DO_STMT ||
            expr->type == H64EXPRTYPE_FUNCDEF_STMT ||
            expr->type == H64EXPRTYPE_IF_STMT ||
            expr->type == H64EXPRTYPE_FOR_STMT ||
            expr->type == H64EXPRTYPE_WITH_STMT ||
            expr->type == H64EXPRTYPE_RAISE_STMT ||
            expr->type == H64EXPRTYPE_BREAK_STMT ||
            expr->type == H64EXPRTYPE_CONTINUE_STMT ||
            expr->type == H64EXPRTYPE_GIVEN ||
            (expr->type == H64EXPRTYPE_UNARYOP &&
             expr->op.optype == H64OP_NEW)) {
        // Already handled in visit_in
    } else if (expr->type == H64EXPRTYPE_BINARYOP &&
            expr->op.optype == H64OP_ATTRIBUTEBYIDENTIFIER &&
            (expr->parent == NULL ||
             expr->parent->type != H64EXPRTYPE_ASSIGN_STMT ||
             expr->parent->assignstmt.lvalue != expr) &&
            !expr->op.value2->storage.set
            ) {
        if (is_in_extends_arg(expr)) {
            // Nothing to do if in 'extends' clause, since that
            // has all been resolved already by varstorage handling.
            return 1;
        }
        // Regular get by member that needs to be evaluated at runtime:
        assert(expr->op.value2->type == H64EXPRTYPE_IDENTIFIERREF);
        int64_t idx = h64debugsymbols_AttributeNameToAttributeNameId(
            rinfo->pr->program->symbols,
            expr->op.value2->identifierref.value, 0, 0
        );
        int temp = new1linetemp(func, expr, 1);
        if (temp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        if (idx < 0) {
            const char errmsg[] = (
                "given attribute not present on this value"
            );
            h64wchar *msg = NULL;
            int64_t msglen = 0;
            msg = utf8_to_utf32(
                errmsg, strlen(errmsg), NULL, NULL, &msglen
            );
            if (!msg) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            int temp2 = new1linetemp(func, expr, 0);
            h64instruction_setconst inst_str = {0};
            inst_str.type = H64INST_SETCONST;
            inst_str.slot = temp2;
            inst_str.content.type = H64VALTYPE_CONSTPREALLOCSTR;
            inst_str.content.constpreallocstr_len = msglen;
            inst_str.content.constpreallocstr_value = msg;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_str
                    )) {
                rinfo->hadoutofmemory = 1;
                free(msg);
                return 0;
            }
            h64instruction_raise inst_raise = {0};
            inst_raise.type = H64INST_RAISE;
            inst_raise.error_class_id = H64STDERROR_ATTRIBUTEERROR;
            inst_raise.sloterrormsgobj = temp2;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_raise
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            h64instruction_getattributebyname inst_getattr = {0};
            inst_getattr.type = H64INST_GETATTRIBUTEBYNAME;
            inst_getattr.slotto = temp;
            inst_getattr.objslotfrom = expr->op.value1->storage.eval_temp_id;
            inst_getattr.nameidx = idx;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_getattr
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        expr->storage.eval_temp_id = temp;
    } else if (expr->type == H64EXPRTYPE_BINARYOP) {
        // Other binary op instances that aren't get by member,
        // unless it doesn't need to be handled anyway:
        if (expr->op.optype == H64OP_ATTRIBUTEBYIDENTIFIER) {
            if (expr->storage.set &&
                    expr->storage.eval_temp_id < 0 &&
                    (!expr->op.value1->storage.set ||
                     expr->op.value1->storage.ref.type !=
                         H64STORETYPE_STACKSLOT) &&
                    expr->op.value2->storage.eval_temp_id >= 0) {
                // Might be a pre-resolved global module access,
                // given operand 2 apparently has been processed.
                assert(expr->op.value2->storage.eval_temp_id >= 0);
                expr->storage.eval_temp_id = (
                    expr->op.value2->storage.eval_temp_id
                );
            }
            assert(
                (expr->storage.set &&
                 expr->storage.eval_temp_id >= 0) || (
                expr->parent != NULL &&
                expr->parent->type == H64EXPRTYPE_ASSIGN_STMT &&
                expr->parent->assignstmt.lvalue == expr
                )
            );
            return 1;  // bail out, handled by parent assign statement
        }
        if (expr->op.optype == H64OP_INDEXBYEXPR) {
            if (expr->parent != NULL &&
                    expr->parent->type == H64EXPRTYPE_ASSIGN_STMT &&
                    expr->parent->assignstmt.lvalue == expr) {
                return 1;  // Similarly to getbymember, this is
                           // handled by parent assign statement
            }
        }
        if (expr->op.optype == H64OP_BOOLCOND_AND ||
                expr->op.optype == H64OP_BOOLCOND_OR) {
            // Handled on visit_in, for early left-hand bail out
            // special code.
            return 1;
        }
        int temp = new1linetemp(func, expr, 1);
        if (temp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        h64instruction_binop inst_binop = {0};
        inst_binop.type = H64INST_BINOP;
        inst_binop.optype = expr->op.optype;
        inst_binop.slotto = temp;
        inst_binop.arg1slotfrom = expr->op.value1->storage.eval_temp_id;
        inst_binop.arg2slotfrom = expr->op.value2->storage.eval_temp_id;
        assert(inst_binop.arg1slotfrom >= 0);
        assert(inst_binop.arg2slotfrom >= 0);
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_binop
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        expr->storage.eval_temp_id = temp;
    } else if (expr->type == H64EXPRTYPE_UNARYOP &&
            expr->op.optype != H64OP_NEW) {
        int temp = new1linetemp(func, expr, 1);
        if (temp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        h64instruction_unop inst_unop = {0};
        inst_unop.type = H64INST_UNOP;
        inst_unop.optype = expr->op.optype;
        inst_unop.slotto = temp;
        inst_unop.argslotfrom = expr->op.value1->storage.eval_temp_id;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_unop
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        expr->storage.eval_temp_id = temp;
    } else if (expr->type == H64EXPRTYPE_CALL) {
        if (expr->inlinecall.value->type == H64EXPRTYPE_IDENTIFIERREF &&
                expr->inlinecall.value->storage.set &&
                expr->inlinecall.value->storage.ref.type ==
                    H64STORETYPE_GLOBALFUNCSLOT &&
                expr->inlinecall.value->storage.ref.id ==
                    rinfo->pr->program->has_attr_func_idx) {
            // Already handled in visit in.
            return 1;
        }
        int calledexprstoragetemp = (
            expr->inlinecall.value->storage.eval_temp_id
        );
        int temp = new1linetemp(func, expr, 1);
        if (temp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        if (!_codegen_call_to(
                rinfo, func, expr, calledexprstoragetemp, temp, 0
                )) {
            return 0;
        }

        expr->storage.eval_temp_id = temp;
    } else if (expr->type == H64EXPRTYPE_CLASSDEF_STMT ||
            expr->type == H64EXPRTYPE_CALL_STMT ||
            expr->type == H64EXPRTYPE_IMPORT_STMT) {
        // Nothing to do with those!
    } else if (expr->type == H64EXPRTYPE_IDENTIFIERREF ||
            expr->type == H64EXPRTYPE_WITH_CLAUSE) {
        if (expr->type == H64EXPRTYPE_IDENTIFIERREF) {
            // Special cases where we'll not handle it here:
            if (is_in_extends_arg(expr)) {
                // Nothing to do if in 'extends' clause, since that
                // has all been resolved already by varstorage handling.
                return 1;
            }
            if (expr->parent != NULL && (
                    (expr->parent->type == H64EXPRTYPE_ASSIGN_STMT &&
                     expr->parent->assignstmt.lvalue == expr) ||
                    (expr->parent->type == H64EXPRTYPE_BINARYOP &&
                     expr->parent->op.optype == H64OP_ATTRIBUTEBYIDENTIFIER &&
                     expr->parent->op.value2 == expr &&
                     !expr->storage.set &&
                     expr->parent->parent->type == H64EXPRTYPE_ASSIGN_STMT &&
                     expr->parent == expr->parent->parent->assignstmt.lvalue)
                    )) {
                // This identifier is assigned to, will be handled elsewhere
                return 1;
            } else if (expr->parent != NULL &&
                    expr->parent->type == H64EXPRTYPE_BINARYOP &&
                    expr->parent->op.optype == H64OP_ATTRIBUTEBYIDENTIFIER &&
                    expr->parent->op.value2 == expr &&
                    !expr->storage.set
                    ) {
                // A runtime-resolved get by identifier, handled elsewhere
                return 1;
            }
            if (expr->identifierref.resolved_to_expr &&
                    expr->identifierref.resolved_to_expr->type ==
                    H64EXPRTYPE_IMPORT_STMT)
                return 1;  // nothing to do with those
        }
        assert(expr->storage.set);
        if (expr->storage.ref.type == H64STORETYPE_STACKSLOT) {
            expr->storage.eval_temp_id = expr->storage.ref.id;
        } else {
            int temp = new1linetemp(func, expr, 1);
            if (temp < 0) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            expr->storage.eval_temp_id = temp;
            if (expr->storage.ref.type == H64STORETYPE_GLOBALVARSLOT) {
                h64instruction_getglobal inst_getglobal = {0};
                inst_getglobal.type = H64INST_GETGLOBAL;
                inst_getglobal.slotto = temp;
                inst_getglobal.globalfrom = expr->storage.ref.id;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_getglobal
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            } else if (expr->storage.ref.type ==
                    H64STORETYPE_GLOBALFUNCSLOT) {
                #ifndef NDEBUG
                if (expr->storage.ref.id < 0) {
                    char *s = ast_ExpressionToJSONStr(
                        expr, rinfo->ast->fileuri,
                        rinfo->ast->fileurilen
                    );
                    h64fprintf(
                        stderr, "horsec: error: invalid expr "
                        "with func storage with negative id: "
                        "%s -> id %" PRId64 "\n",
                        s, (int64_t)expr->storage.ref.id
                    );
                    free(s);
                }
                #endif
                assert(expr->storage.ref.id >= 0);
                h64instruction_getfunc inst_getfunc = {0};
                inst_getfunc.type = H64INST_GETFUNC;
                inst_getfunc.slotto = temp;
                inst_getfunc.funcfrom = expr->storage.ref.id;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_getfunc
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            } else if (expr->storage.ref.type ==
                    H64STORETYPE_GLOBALCLASSSLOT) {
                h64instruction_getclass inst_getclass = {0};
                inst_getclass.type = H64INST_GETCLASS;
                inst_getclass.slotto = temp;
                inst_getclass.classfrom = expr->storage.ref.id;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_getclass
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf) - 1,
                    "internal error: unhandled storage type %d",
                    (int)expr->storage.ref.type
                );
                if (!result_AddMessage(
                        &rinfo->ast->resultmsg,
                        H64MSG_ERROR, buf,
                        rinfo->ast->fileuri, rinfo->ast->fileurilen,
                        expr->line, expr->column
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                return 1;
            }
        }
        if (expr->type == H64EXPRTYPE_WITH_CLAUSE) {
            assert(expr->withclause.withitem_value != NULL);
            if (expr->withclause.withitem_value->storage.
                    eval_temp_id != expr->storage.eval_temp_id) {
                h64instruction_valuecopy vcopy = {0};
                vcopy.type = H64INST_VALUECOPY;
                vcopy.slotfrom = (
                    expr->withclause.withitem_value->storage.
                    eval_temp_id
                );
                vcopy.slotto = expr->storage.eval_temp_id;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &vcopy
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        }
    } else if (expr->type == H64EXPRTYPE_VARDEF_STMT &&
               (expr->vardef.value == NULL ||
                (func == NULL && surroundingclass(expr, 0) != NULL &&
                 expr->vardef.value->type == H64EXPRTYPE_LITERAL &&
                 expr->vardef.value->literal.type ==
                     H64TK_CONSTANT_NONE))) {
        // Empty variable definition or none definition for
        // class attr, so nothing to do.
        return 1;
    } else if (expr->type == H64EXPRTYPE_RETURN_STMT) {
        int returntemp = -1;
        if (expr->returnstmt.returned_expression) {
            returntemp = expr->returnstmt.returned_expression->
                storage.eval_temp_id;
            assert(returntemp >= 0);
        } else {
            returntemp = new1linetemp(func, expr, 1);
            if (returntemp < 0) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            h64instruction_setconst inst_setconst = {0};
            inst_setconst.type = H64INST_SETCONST;
            inst_setconst.content.type = H64VALTYPE_NONE;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_setconst
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        h64instruction_returnvalue inst_returnvalue = {0};
        inst_returnvalue.type = H64INST_RETURNVALUE;
        inst_returnvalue.returnslotfrom = returntemp;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_returnvalue
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
    } else if (expr->type == H64EXPRTYPE_VARDEF_STMT ||
            expr->type == H64EXPRTYPE_ASSIGN_STMT) {
        // Assigning directly to a variable (rather than a member,
        // map value, or the like)
        assert(func != NULL);
        int assignfromtemporary = -1;
        storageref *str = NULL;
        int complexsetter_tmp = -1;
        if (expr->type == H64EXPRTYPE_VARDEF_STMT) {
            assert(expr->storage.set);
            str = &expr->storage.ref;
            if (expr->vardef.value != NULL) {
                assert(expr->vardef.value->storage.eval_temp_id >= 0);
                assignfromtemporary = expr->vardef.value->
                    storage.eval_temp_id;
            } else {
                assignfromtemporary = new1linetemp(func, expr, 1);
                if (assignfromtemporary < 0) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                h64instruction_setconst inst = {0};
                inst.type = H64INST_SETCONST;
                inst.slot = assignfromtemporary;
                inst.content.type = H64VALTYPE_NONE;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        } else if (expr->type == H64EXPRTYPE_ASSIGN_STMT) {
            get_assign_lvalue_storage(
                expr, &str
            );  // Get the storage info of our assignment target
            storageref _complexsetter_buf = {0};
            int iscomplexassign = 0;
            if (str == NULL) {  // No storage, must be complex assign:
                iscomplexassign = 1;
                assert(
                    expr->assignstmt.lvalue->type ==
                        H64EXPRTYPE_BINARYOP && (
                    expr->assignstmt.lvalue->op.optype ==
                        H64OP_ATTRIBUTEBYIDENTIFIER ||
                    expr->assignstmt.lvalue->op.optype == H64OP_INDEXBYEXPR
                    )
                );
                // This assigns to a member or indexed thing,
                // e.g. a[b] = c  or  a.b = c.
                //
                // -> We need an extra temporary to hold the in-between
                // value for this case, since there is no real target
                // storage ready. (Since we'll assign it with a special
                // setbymember/setbyindexexpr instruction, rather than
                // by copying directly into the target.)
                assert(!expr->assignstmt.lvalue->storage.set);
                assert(expr->assignstmt.lvalue->storage.eval_temp_id < 0);
                complexsetter_tmp = new1linetemp(func, expr, 0);
                if (complexsetter_tmp < 0) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                _complexsetter_buf.type = H64STORETYPE_STACKSLOT;
                _complexsetter_buf.id = complexsetter_tmp;
                str = &_complexsetter_buf;
            }
            assert(str != NULL);
            assignfromtemporary = (
                expr->assignstmt.rvalue->storage.eval_temp_id
            );
            if (expr->assignstmt.assignop != H64OP_ASSIGN) {
                // This assign op does some sort of arithmetic!
                int oldvaluetemp = -1;
                if (str->type == H64STORETYPE_GLOBALVARSLOT) {
                    oldvaluetemp = new1linetemp(func, expr, 0);
                    if (oldvaluetemp < 0) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                    h64instruction_getglobal inst = {0};
                    inst.type = H64INST_GETGLOBAL;
                    inst.globalfrom = str->id;
                    inst.slotto = oldvaluetemp;
                    if (!appendinst(
                            rinfo->pr->program, func, expr, &inst
                            )) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                } else if (str->type == H64STORETYPE_VARATTRSLOT) {
                    oldvaluetemp = new1linetemp(func, expr, 0);
                    if (oldvaluetemp < 0) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                    assert(surroundingclass(expr, 1) != NULL);
                    assert(func->funcdef._storageinfo->closure_with_self != 0);
                    h64instruction_getattributebyidx inst = {0};
                    inst.type = H64INST_GETATTRIBUTEBYIDX;
                    // 'self' is always on top of all "regular" args:
                    inst.objslotfrom = (
                        func->funcdef.arguments.arg_count
                    );
                    inst.varattrfrom = (attridx_t)str->id;
                    inst.slotto = oldvaluetemp;
                    if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                } else if (!iscomplexassign) {
                    if (str->type != H64STORETYPE_STACKSLOT) {
                        errorinvalidassign:
                        if (str->type == H64STORETYPE_GLOBALCLASSSLOT) {
                            int64_t classrefid = str->id;
                            h64classsymbol *csymbol = (
                                h64debugsymbols_GetClassSymbolById(
                                    rinfo->pr->program->symbols, classrefid
                                )
                            );
                            h64modulesymbols *msymbol = (
                                h64debugsymbols_GetModuleSymbolsByClassId(
                                    rinfo->pr->program->symbols, classrefid
                                )
                            );
                            char buf[512];
                            snprintf(
                                buf, sizeof(buf) - 1,
                                "unexpected assign to global class "
                                "definition, can not assign to class "
                                "%s.%s%s%s",
                                msymbol->module_path, csymbol->name,
                                (msymbol->library_name != NULL ?
                                " from " : ""),
                                (msymbol->library_name != NULL ?
                                msymbol->library_name : "")
                            );
                            if (!result_AddMessage(
                                    &rinfo->ast->resultmsg,
                                    H64MSG_ERROR, buf,
                                    rinfo->ast->fileuri,
                                    rinfo->ast->fileurilen,
                                    expr->line, expr->column
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                            rinfo->hadunexpectederror = 1;
                            return 0;
                        } else  if (str->type == H64STORETYPE_GLOBALFUNCSLOT) {
                            funcid_t funcrefid = str->id;
                            h64funcsymbol *fsymbol = (
                                h64debugsymbols_GetFuncSymbolById(
                                    rinfo->pr->program->symbols, funcrefid
                                )
                            );
                            h64modulesymbols *msymbol = (
                                h64debugsymbols_GetModuleSymbolsByFuncId(
                                    rinfo->pr->program->symbols, funcrefid
                                )
                            );
                            char buf[512];
                            snprintf(
                                buf, sizeof(buf) - 1,
                                "unexpected assign to global func "
                                "definition, can not assign to func "
                                "%s.%s%s%s",
                                msymbol->module_path, fsymbol->name,
                                (msymbol->library_name != NULL ?
                                " from " : ""),
                                (msymbol->library_name != NULL ?
                                msymbol->library_name : "")
                            );
                            if (!result_AddMessage(
                                    &rinfo->ast->resultmsg,
                                    H64MSG_ERROR, buf,
                                    rinfo->ast->fileuri,
                                    rinfo->ast->fileurilen,
                                    expr->line, expr->column
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                            rinfo->hadunexpectederror = 1;
                            return 0;
                        } else {
                            char buf[512];
                            snprintf(
                                buf, sizeof(buf) - 1,
                                "unexpected assign to unassignable item, "
                                "can not assign to storage type %d (=%s)",
                                (int)str->type,
                                storage_StorageTypeToStr(str->type)
                            );
                            if (!result_AddMessage(
                                    &rinfo->ast->resultmsg,
                                    H64MSG_ERROR, buf,
                                    rinfo->ast->fileuri,
                                    rinfo->ast->fileurilen,
                                    expr->line, expr->column
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                            rinfo->hadunexpectederror = 1;
                            return 0;
                        }
                    }
                    assert(str->type == H64STORETYPE_STACKSLOT);
                    oldvaluetemp = str->id;
                } else {
                    // We actually need to get this the complex way:
                    oldvaluetemp = new1linetemp(func, expr, 0);
                    assert(expr->assignstmt.lvalue->type ==
                           H64EXPRTYPE_BINARYOP);
                    if (expr->assignstmt.lvalue->op.optype ==
                            H64OP_ATTRIBUTEBYIDENTIFIER) {
                        assert(
                            expr->assignstmt.lvalue->op.value2->type ==
                            H64EXPRTYPE_IDENTIFIERREF
                        );
                        int64_t nameid = (
                            h64debugsymbols_AttributeNameToAttributeNameId(
                                rinfo->pr->program->symbols,
                                expr->assignstmt.lvalue->op.value2->
                                    identifierref.value, 0, 0
                            ));
                        if (nameid >= 0) {
                            h64instruction_getattributebyname inst = {0};
                            inst.type = H64INST_GETATTRIBUTEBYNAME;
                            inst.objslotfrom = (
                                expr->assignstmt.lvalue->op.value1->
                                    storage.eval_temp_id);
                            inst.nameidx = nameid;
                            inst.slotto = oldvaluetemp;
                            if (!appendinst(
                                    rinfo->pr->program, func, expr,
                                    &inst
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                        } else {
                            if (!guarded_by_is_a_or_has_attr(expr)) {
                                char buf[256];
                                snprintf(buf, sizeof(buf) - 1,
                                    "unknown attribute \"%s\" "
                                    "will cause AttributeError, put it "
                                    "in if statement with "
                                    "has_attr() or .is_a() if "
                                    "intended for API compat",
                                    expr->assignstmt.lvalue->op.value2->
                                        identifierref.value
                                );
                                if (!result_AddMessage(
                                        &rinfo->ast->resultmsg,
                                        H64MSG_WARNING, buf,
                                        rinfo->ast->fileuri,
                                        rinfo->ast->fileurilen,
                                        expr->assignstmt.lvalue->
                                            op.value2->line,
                                        expr->assignstmt.lvalue->
                                            op.value2->column
                                        )) {
                                    rinfo->hadoutofmemory = 1;
                                    return 0;
                                }
                            }
                            // Unknown attribute, so hardcode an error:
                            const char errmsg[] = (
                                "given attribute not present on this value"
                            );
                            h64wchar *msg = NULL;
                            int64_t msglen = 0;
                            msg = utf8_to_utf32(
                                errmsg, strlen(errmsg), NULL, NULL, &msglen
                            );
                            if (!msg) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                            int temp2 = new1linetemp(
                                func, expr->assignstmt.lvalue, 0
                            );
                            h64instruction_setconst inst_str = {0};
                            inst_str.type = H64INST_SETCONST;
                            inst_str.slot = temp2;
                            inst_str.content.type = (
                                H64VALTYPE_CONSTPREALLOCSTR
                            );
                            inst_str.content.constpreallocstr_len = msglen;
                            inst_str.content.constpreallocstr_value = msg;
                            if (!appendinst(
                                    rinfo->pr->program, func,
                                    expr->assignstmt.lvalue,
                                    &inst_str
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                free(msg);
                                return 0;
                            }
                            h64instruction_raise inst_raise = {0};
                            inst_raise.type = H64INST_RAISE;
                            inst_raise.error_class_id = (
                                H64STDERROR_ATTRIBUTEERROR
                            );
                            inst_raise.sloterrormsgobj = temp2;
                            if (!appendinst(
                                    rinfo->pr->program, func,
                                    expr->assignstmt.lvalue,
                                    &inst_raise
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                        }
                    } else {
                        assert(expr->assignstmt.lvalue->op.optype ==
                               H64OP_INDEXBYEXPR);
                        h64instruction_binop inst = {0};
                        inst.type = H64INST_BINOP;
                        inst.optype = H64OP_INDEXBYEXPR;
                        inst.arg1slotfrom = (
                            expr->assignstmt.lvalue->op.value1->
                                storage.eval_temp_id);
                        inst.arg2slotfrom = (
                            expr->assignstmt.lvalue->op.value2->
                                storage.eval_temp_id);
                        inst.slotto = oldvaluetemp;
                        if (!appendinst(
                                rinfo->pr->program, func, expr,
                                &inst
                                )) {
                            rinfo->hadoutofmemory = 1;
                            return 0;
                        }
                    }
                }
                int mathop = operator_AssignOpToMathOp(
                    expr->assignstmt.assignop
                );
                assert(mathop != H64OP_INVALID);
                h64instruction_binop inst_assignmath = {0};
                inst_assignmath.type = H64INST_BINOP;
                inst_assignmath.optype = mathop;
                inst_assignmath.arg1slotfrom = oldvaluetemp;
                inst_assignmath.arg2slotfrom = assignfromtemporary;
                inst_assignmath.slotto = oldvaluetemp;
                if (!appendinst(
                        rinfo->pr->program, func, expr,
                        &inst_assignmath
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                assignfromtemporary = oldvaluetemp;
            }
        }
        assert(assignfromtemporary >= 0);
        if (str->type == H64STORETYPE_GLOBALVARSLOT) {
            h64instruction_setglobal inst = {0};
            inst.type = H64INST_SETGLOBAL;
            inst.globalto = str->id;
            inst.slotfrom = assignfromtemporary;
            if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else if (str->type == H64STORETYPE_VARATTRSLOT) {
            assert(surroundingclass(expr, 1) != NULL);
            assert(func->funcdef._storageinfo->closure_with_self != 0);
            h64instruction_setbyattributeidx inst = {0};
            inst.type = H64INST_SETBYATTRIBUTEIDX;
            // 'self' is always on top of all "regular" args:
            inst.slotobjto = (
                func->funcdef.arguments.arg_count
            );
            inst.varattrto = (attridx_t)str->id;
            inst.slotvaluefrom = assignfromtemporary;
            if (!appendinst(rinfo->pr->program, func, expr, &inst)) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else if (str->type != H64STORETYPE_STACKSLOT) {
            // We cannot assign to this, show error.
            goto errorinvalidassign;
        } else if (assignfromtemporary != str->id ||
                complexsetter_tmp >= 0) {
            assert(str->type == H64STORETYPE_STACKSLOT);
            if (complexsetter_tmp >= 0) {
                // This assigns to a member or indexed thing,
                // e.g. a[b] = c  or  a.b = c
                //
                // (Please note this excludes cases where a.b refers
                // to an import since those have storage 'flattened'
                // and set directly by the scope resolver. Therefore,
                // this code path is only cases where a.b needs to be
                // a true dynamic runtime getmember.)
                assert(expr->assignstmt.lvalue->type ==
                       H64EXPRTYPE_BINARYOP);
                if (expr->assignstmt.lvalue->op.optype ==
                        H64OP_ATTRIBUTEBYIDENTIFIER) {
                    h64instruction_setbyattributename inst = {0};
                    inst.type = H64INST_SETBYATTRIBUTENAME;
                    assert(expr->assignstmt.lvalue->
                           op.value1->storage.eval_temp_id >= 0);
                    inst.slotobjto = (
                        expr->assignstmt.lvalue->op.value1->
                        storage.eval_temp_id
                    );
                    assert(expr->assignstmt.lvalue->
                           op.value2->storage.eval_temp_id < 0);
                    assert(expr->assignstmt.lvalue->op.value2->type ==
                           H64EXPRTYPE_IDENTIFIERREF);
                    int64_t nameidx = (
                        h64debugsymbols_AttributeNameToAttributeNameId(
                            rinfo->pr->program->symbols,
                            expr->assignstmt.lvalue->op.value2->
                                identifierref.value, 0, 0
                        )
                    );
                    if (nameidx < 0) {
                        if (!guarded_by_is_a_or_has_attr(expr)) {
                            char buf[256];
                            snprintf(buf, sizeof(buf) - 1,
                                "unknown attribute \"%s\" "
                                "will cause AttributeError, put it "
                                "in if statement with "
                                "has_attr() or .is_a() if "
                                "intended for API compat",
                                expr->assignstmt.lvalue->op.value2->
                                    identifierref.value
                            );
                            if (!result_AddMessage(
                                    &rinfo->ast->resultmsg,
                                    H64MSG_WARNING, buf,
                                    rinfo->ast->fileuri,
                                    rinfo->ast->fileurilen,
                                    expr->assignstmt.lvalue->
                                        op.value2->line,
                                    expr->assignstmt.lvalue->
                                        op.value2->column
                                    )) {
                                rinfo->hadoutofmemory = 1;
                                return 0;
                            }
                        }
                        // Unknown attribute, so hardcode an error:
                        const char errmsg[] = (
                            "given attribute not present on this value"
                        );
                        h64wchar *msg = NULL;
                        int64_t msglen = 0;
                        msg = utf8_to_utf32(
                            errmsg, strlen(errmsg), NULL, NULL, &msglen
                        );
                        if (!msg) {
                            rinfo->hadoutofmemory = 1;
                            return 0;
                        }
                        int temp2 = new1linetemp(
                            func, expr->assignstmt.lvalue, 0
                        );
                        h64instruction_setconst inst_str = {0};
                        inst_str.type = H64INST_SETCONST;
                        inst_str.slot = temp2;
                        inst_str.content.type = (
                            H64VALTYPE_CONSTPREALLOCSTR
                        );
                        inst_str.content.constpreallocstr_len = msglen;
                        inst_str.content.constpreallocstr_value = msg;
                        if (!appendinst(
                                rinfo->pr->program, func,
                                expr->assignstmt.lvalue,
                                &inst_str
                                )) {
                            rinfo->hadoutofmemory = 1;
                            free(msg);
                            return 0;
                        }
                        h64instruction_raise inst_raise = {0};
                        inst_raise.type = H64INST_RAISE;
                        inst_raise.error_class_id = (
                            H64STDERROR_ATTRIBUTEERROR
                        );
                        inst_raise.sloterrormsgobj = temp2;
                        if (!appendinst(
                                rinfo->pr->program, func,
                                expr->assignstmt.lvalue,
                                &inst_raise
                                )) {
                            rinfo->hadoutofmemory = 1;
                            return 0;
                        }
                    } else {
                        inst.nameidx = nameidx;
                        inst.slotvaluefrom = assignfromtemporary;
                        if (!appendinst(
                                rinfo->pr->program, func, expr,
                                &inst
                                )) {
                            rinfo->hadoutofmemory = 1;
                            return 0;
                        }
                    }
                } else {
                    assert(expr->assignstmt.lvalue->op.optype ==
                           H64OP_INDEXBYEXPR);
                    h64instruction_setbyindexexpr inst = {0};
                    inst.type = H64INST_SETBYINDEXEXPR;
                    assert(expr->assignstmt.lvalue->
                           op.value1->storage.eval_temp_id >= 0);
                    inst.slotobjto = (
                        expr->assignstmt.lvalue->op.value1->
                        storage.eval_temp_id
                    );
                    assert(expr->assignstmt.lvalue->
                           op.value2->storage.eval_temp_id >= 0);
                    inst.slotindexto = (
                        expr->assignstmt.lvalue->op.value2->
                        storage.eval_temp_id
                    );
                    inst.slotvaluefrom = assignfromtemporary;
                    if (!appendinst(
                            rinfo->pr->program, func, expr, &inst
                            )) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                }
            } else {
                // Simple assignment of form a = b
                h64instruction_valuecopy inst = {0};
                inst.type = H64INST_VALUECOPY;
                inst.slotto = str->id;
                inst.slotfrom = assignfromtemporary;
                if (!appendinst(rinfo->pr->program, func, expr,
                                &inst)) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        }
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf) - 1,
            "internal error: unhandled expr type %d (=%s)",
            (int)expr->type,
            ast_ExpressionTypeToStr(expr->type)
        );
        if (!result_AddMessage(
                &rinfo->ast->resultmsg,
                H64MSG_ERROR, buf,
                rinfo->ast->fileuri,
                rinfo->ast->fileurilen,
                expr->line, expr->column
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        return 1;
    }

    if (IS_STMT(expr->type) && func != NULL)
        free1linetemps(func);

    return 1;
}

static int _enforce_dostmt_limit_in_func(
        asttransforminfo *rinfo, h64expression *func
        ) {
    if (func->funcdef._storageinfo->dostmts_used + 1 >=
            INT16_MAX - 1) {
        rinfo->hadunexpectederror = 1;
        if (!result_AddMessage(
                rinfo->pr->resultmsg,
                H64MSG_ERROR, "exceeded maximum of "
                "do or with statements in one function",
                NULL, 0, -1, -1
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        return 0;
    }
    return 1;
}

int _codegencallback_DoCodegen_visit_in(
        h64expression *expr, ATTR_UNUSED h64expression *parent, void *ud
        ) {
    asttransforminfo *rinfo = (asttransforminfo *)ud;
    ATTR_UNUSED asttransformcodegenextra *extra = rinfo->userdata;

    h64expression *func = surroundingfunc(expr);
    if (!func) {
        h64expression *sclass = surroundingclass(expr, 0);
        if (sclass != NULL && expr->type != H64EXPRTYPE_FUNCDEF_STMT) {
            if (!isvardefstmtassignvalue(expr) ||
                    (expr->type == H64EXPRTYPE_LITERAL &&
                     expr->literal.type == H64TK_CONSTANT_NONE))
                return 1;  // ignore this for now
            func = _fakeclassinitfunc(rinfo, sclass);
            if (!func) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else if (sclass == NULL && expr->type != H64EXPRTYPE_FUNCDEF_STMT) {
            func = _fakeglobalinitfunc(rinfo);
            if (!func && expr->type != H64EXPRTYPE_FUNCDEF_STMT) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
    }

    if (IS_STMT(expr->type) && func)
        free1linetemps(func);

    if (expr->type == H64EXPRTYPE_WHILE_STMT) {
        rinfo->dont_descend_visitation = 1;
        int32_t jumpid_start = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;
        int32_t jumpid_end = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        // Make sure inner visited stuff has the loop
        // information, for break/continue etc.
        if (extra->loop_nesting_depth + 1 >
                extra->loop_nesting_alloc) {
            int new_alloc = (
                extra->loop_nesting_depth + 16
            );
            if (new_alloc < extra->loop_nesting_alloc * 2)
                new_alloc = extra->loop_nesting_alloc * 2;
            int64_t *new_start_ids = realloc(
                extra->loop_start_jumpid,
                sizeof(*extra->loop_start_jumpid) * new_alloc
            );
            if (!new_start_ids) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            extra->loop_start_jumpid = new_start_ids;
            int64_t *new_end_ids = realloc(
                extra->loop_end_jumpid,
                sizeof(*extra->loop_end_jumpid) * new_alloc
            );
            if (!new_end_ids) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            extra->loop_end_jumpid = new_end_ids;
            extra->loop_nesting_alloc = new_alloc;
        }
        extra->loop_start_jumpid[
            extra->loop_nesting_depth
        ] = jumpid_start;
        extra->loop_end_jumpid[
            extra->loop_nesting_depth
        ] = jumpid_end;
        extra->loop_nesting_depth++;

        // Ok, now start codegen for loop and its contents:
        h64instruction_jumptarget inst_jumptarget = {0};
        inst_jumptarget.type = H64INST_JUMPTARGET;
        inst_jumptarget.jumpid = jumpid_start;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumptarget
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        rinfo->dont_descend_visitation = 0;
        int result = ast_VisitExpression(
            expr->whilestmt.conditional, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;

        h64instruction_condjump inst_condjump = {0};
        inst_condjump.type = H64INST_CONDJUMP;
        inst_condjump.conditionalslot = (
            expr->whilestmt.conditional->storage.eval_temp_id
        );
        inst_condjump.jumpbytesoffset = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_condjump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        int i = 0;
        while (i < expr->whilestmt.stmt_count) {
            rinfo->dont_descend_visitation = 0;
            result = ast_VisitExpression(
                expr->whilestmt.stmt[i], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;
            i++;
        }

        h64instruction_jump inst_jump = {0};
        inst_jump.type = H64INST_JUMP;
        inst_jump.jumpbytesoffset = jumpid_start;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        h64instruction_jumptarget inst_jumptargetend = {0};
        inst_jumptargetend.type = H64INST_JUMPTARGET;
        inst_jumptargetend.jumpid = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr,
                &inst_jumptargetend
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        rinfo->dont_descend_visitation = 1;
        extra->loop_nesting_depth--;  // leaving the loop.
        assert(extra->loop_nesting_depth >= 0);
        return 1;
    } else if (expr->type == H64EXPRTYPE_BREAK_STMT) {
        assert(extra->loop_nesting_depth > 0);
        h64instruction_jump inst_jump = {0};
        inst_jump.type = H64INST_JUMP;
        inst_jump.jumpbytesoffset = (
            extra->loop_end_jumpid[extra->loop_nesting_depth - 1]
        );
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        return 1;
    } else if (expr->type == H64EXPRTYPE_CONTINUE_STMT) {
        assert(extra->loop_nesting_depth > 0);
        h64instruction_jump inst_jump = {0};
        inst_jump.type = H64INST_JUMP;
        inst_jump.jumpbytesoffset = (
            extra->loop_start_jumpid[extra->loop_nesting_depth - 1]
        );
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        return 1;
    } else if (expr->type == H64EXPRTYPE_BINARYOP &&
            (expr->op.optype == H64OP_BOOLCOND_AND ||
             expr->op.optype == H64OP_BOOLCOND_OR)) {
        rinfo->dont_descend_visitation = 1;

        int target_tmp = new1linetemp(func, expr, 1);
        int32_t jumpid_regulareval = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;
        int32_t jumpid_pasteval = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        rinfo->dont_descend_visitation = 0;
        int result = ast_VisitExpression(
            expr->op.value1, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;
        int arg1tmp = expr->op.value1->storage.eval_temp_id;
        assert(arg1tmp >= 0);

        if (expr->op.optype == H64OP_BOOLCOND_AND) {
            // If first arg is 'yes', resume with regular eval:
            h64instruction_condjumpex inst_cjump = {0};
            inst_cjump.type = H64INST_CONDJUMPEX;
            inst_cjump.flags |= (
                CONDJUMPEX_FLAG_JUMPONTRUE
            );
            inst_cjump.conditionalslot = arg1tmp;
            inst_cjump.jumpbytesoffset = jumpid_regulareval;
            if (!appendinst(
                    rinfo->pr->program,
                    func, expr, &inst_cjump
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            // If first arg is NOT 'yes', bail early:
            h64instruction_setconst inst_setfalse = {0};
            inst_setfalse.type = H64INST_SETCONST;
            inst_setfalse.content.type = H64VALTYPE_BOOL;
            inst_setfalse.content.int_value = 0;
            inst_setfalse.slot = target_tmp;
            if (!appendinst(
                    rinfo->pr->program,
                    func, expr, &inst_setfalse
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            // Jump past regular eval since we already returned false:
            h64instruction_jump inst_jump = {0};
            inst_jump.type = H64INST_JUMP;
            inst_jump.jumpbytesoffset = jumpid_pasteval;
            if (!appendinst(
                    rinfo->pr->program,
                    func, expr, &inst_jump
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            assert(expr->op.optype == H64OP_BOOLCOND_OR);
            // If first arg is 'no', resume with regular eval:
            h64instruction_condjumpex inst_cjump = {0};
            inst_cjump.type = H64INST_CONDJUMPEX;
            inst_cjump.conditionalslot = arg1tmp;
            inst_cjump.jumpbytesoffset = jumpid_regulareval;
            if (!appendinst(
                    rinfo->pr->program,
                    func, expr, &inst_cjump
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            // If first arg is NOT 'no', bail early:
            h64instruction_setconst inst_settrue = {0};
            inst_settrue.type = H64INST_SETCONST;
            inst_settrue.content.type = H64VALTYPE_BOOL;
            inst_settrue.content.int_value = 1;
            inst_settrue.slot = target_tmp;
            if (!appendinst(
                    rinfo->pr->program,
                    func, expr, &inst_settrue
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            // Jump past regular eval since we already returned false:
            h64instruction_jump inst_jump = {0};
            inst_jump.type = H64INST_JUMP;
            inst_jump.jumpbytesoffset = jumpid_pasteval;
            if (!appendinst(
                    rinfo->pr->program,
                    func, expr, &inst_jump
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        h64instruction_jumptarget inst_regulareval = {0};
        inst_regulareval.type = H64INST_JUMPTARGET;
        inst_regulareval.jumpid = jumpid_regulareval;
        if (!appendinst(
                rinfo->pr->program,
                func, expr, &inst_regulareval
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        rinfo->dont_descend_visitation = 0;
        result = ast_VisitExpression(
            expr->op.value2, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;
        int arg2tmp = expr->op.value2->storage.eval_temp_id;

        h64instruction_binop inst_binop = {0};
        inst_binop.type = H64INST_BINOP;
        inst_binop.optype = expr->op.optype;
        inst_binop.slotto = target_tmp;
        inst_binop.arg1slotfrom = arg1tmp;
        inst_binop.arg2slotfrom = arg2tmp;
        if (!appendinst(
                rinfo->pr->program,
                func, expr, &inst_binop
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        h64instruction_jumptarget inst_pasteval = {0};
        inst_pasteval.type = H64INST_JUMPTARGET;
        inst_pasteval.jumpid = jumpid_pasteval;
        if (!appendinst(
                rinfo->pr->program,
                func, expr, &inst_pasteval
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        rinfo->dont_descend_visitation = 1;
        expr->storage.eval_temp_id = target_tmp;
        return 1;
    } else if (expr->type == H64EXPRTYPE_RAISE_STMT) {
        rinfo->dont_descend_visitation = 1;

        // Check the raised thing is a `new Exception(string)` item:
        if (expr->raisestmt.raised_expression->type !=
                H64EXPRTYPE_UNARYOP ||
                expr->raisestmt.raised_expression->op.optype !=
                H64OP_NEW ||
                expr->raisestmt.raised_expression->op.value1->type !=
                H64EXPRTYPE_CALL) {
            result_AddMessage(
                rinfo->pr->resultmsg, H64MSG_ERROR,
                "unexpected raised expression, expected "
                "a 'new' instantiation of an error class",
                rinfo->ast->fileuri, rinfo->ast->fileurilen,
                expr->line, expr->column
            );
            rinfo->hadunexpectederror = 1;
            return 0;
        }
        if (expr->raisestmt.raised_expression->op.value1->inlinecall.
                arguments.arg_count != 1 || (
                expr->raisestmt.raised_expression->op.value1->inlinecall.
                arguments.arg_name != NULL &&
                expr->raisestmt.raised_expression->op.value1->inlinecall.
                arguments.arg_name[0] != NULL)) {
            result_AddMessage(
                rinfo->pr->resultmsg, H64MSG_ERROR,
                "unexpected number of arguments to error object, "
                "expected single positional argument",
                rinfo->ast->fileuri, rinfo->ast->fileurilen,
                expr->line, expr->column
            );
            rinfo->hadunexpectederror = 1;
            return 0;
        }

        // See if we can tell what error class this is by looking at it:
        classid_t error_class_id = -1;
        if (expr->raisestmt.raised_expression->op.value1->inlinecall.
                value->storage.set &&
                expr->raisestmt.raised_expression->op.value1->inlinecall.
                value->storage.ref.type == H64STORETYPE_GLOBALCLASSSLOT) {
            error_class_id = (
                expr->raisestmt.raised_expression->op.value1->inlinecall.
                value->storage.ref.id
            );
            assert(error_class_id >= 0);
        }

        // Visit raised element and the string argument to generate code:
        rinfo->dont_descend_visitation = 0;
        int result = 1;
        if (error_class_id < 0)  // -> we don't know what to raise already
            result = ast_VisitExpression(
                expr->raisestmt.raised_expression->op.value1->inlinecall.
                    value, expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;
        rinfo->dont_descend_visitation = 0;
        result = ast_VisitExpression(
            expr->raisestmt.raised_expression->op.value1->inlinecall.
                arguments.arg_value[0], expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;

        // Generate raise instruction:
        int error_instance_tmp = -1;
        if (error_class_id < 0)  // -> we visited this for code gen
            error_instance_tmp = (
                expr->raisestmt.raised_expression->op.value1->inlinecall.
                value->storage.eval_temp_id
            );
        int str_arg_tmp = (
            expr->raisestmt.raised_expression->op.value1->inlinecall.
                arguments.arg_value[0]->storage.eval_temp_id
        );
        if ((error_instance_tmp < 0 && error_class_id < 0) ||
                str_arg_tmp < 0) {
            assert(rinfo->hadunexpectederror || rinfo->hadoutofmemory);
            return 0;
        } else if (error_class_id < 0) {
            // We don't know the exact class at compile time, so
            // raise by using runtime ref:
            h64instruction_raisebyref inst_raisebyref = {0};
            inst_raisebyref.type = H64INST_RAISEBYREF;
            inst_raisebyref.sloterrormsgobj = str_arg_tmp;
            inst_raisebyref.sloterrorclassrefobj = error_instance_tmp;
            if (!appendinst(
                    rinfo->pr->program, func, expr,
                    &inst_raisebyref
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            // We DO know the exact class, so hard code it (faster):
            h64instruction_raise inst_raise = {0};
            inst_raise.type = H64INST_RAISE;
            inst_raise.sloterrormsgobj = str_arg_tmp;
            inst_raise.error_class_id = error_class_id;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_raise
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        free1linetemps(func);
        rinfo->dont_descend_visitation = 1;
        return 1;
    } else if (expr->type == H64EXPRTYPE_FUNCDEF_STMT) {
        rinfo->dont_descend_visitation = 1;

        funcid_t func_id = expr->funcdef.bytecode_func_id;

        // Handling of keyword arguments:
        int argtmp = 0;
        int i = 0;
        while (i < expr->funcdef.arguments.arg_count) {
            if (expr->funcdef.arguments.arg_value[i] != NULL) {
                assert(i + 1 >= expr->funcdef.arguments.arg_count ||
                       expr->funcdef.arguments.arg_value[i + 1] != NULL);
                int jump_past_id = (
                    expr->funcdef._storageinfo->jump_targets_used
                );
                expr->funcdef._storageinfo->jump_targets_used++;
                // ^ IMPORTANT, again expr instead of func since this
                // code is generated INTO the expr funcdef.

                int operand2tmp = new1linetemp(
                    expr, expr->funcdef.arguments.arg_value[i], 0
                    // ^ expr as func again, we're gen'ing INTO a funcdef expr
                );
                if (operand2tmp < 0) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }

                h64instruction_setconst inst_sconst = {0};
                inst_sconst.type = H64INST_SETCONST;
                inst_sconst.content.type = H64VALTYPE_UNSPECIFIED_KWARG;
                inst_sconst.slot = operand2tmp;
                if (!appendinst(
                        rinfo->pr->program,
                        expr, expr->funcdef.arguments.arg_value[i],
                        // ^ expr as func again, see explanation above.
                        &inst_sconst
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }

                h64instruction_binop inst_binop = {0};
                inst_binop.type = H64INST_BINOP;
                inst_binop.optype = H64OP_CMP_EQUAL;
                inst_binop.slotto = operand2tmp;
                inst_binop.arg1slotfrom = argtmp;
                inst_binop.arg2slotfrom = operand2tmp;
                if (!appendinst(
                        rinfo->pr->program,
                        expr, expr->funcdef.arguments.arg_value[i],
                        // ^ expr as func again, see explanation above.
                        &inst_binop
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }

                h64instruction_condjump cjump = {0};
                cjump.type = H64INST_CONDJUMP;
                cjump.conditionalslot = operand2tmp;
                cjump.jumpbytesoffset = jump_past_id;
                if (!appendinst(
                        rinfo->pr->program,
                        expr, expr->funcdef.arguments.arg_value[i],
                        // ^ expr as func again, see explanation above.
                        &cjump
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }

                free1linetemps(expr);  // expr as func.

                rinfo->dont_descend_visitation = 0;
                int result = ast_VisitExpression(
                    expr->funcdef.arguments.arg_value[i], expr,
                    &_codegencallback_DoCodegen_visit_in,
                    &_codegencallback_DoCodegen_visit_out,
                    _asttransform_cancel_visit_descend_callback,
                    rinfo
                );
                rinfo->dont_descend_visitation = 1;
                if (!result)
                    return 0;
                assert(expr->funcdef.arguments.arg_value[i]->
                    storage.eval_temp_id >= 0);

                if (expr->funcdef.arguments.arg_value[i]->
                        storage.eval_temp_id != argtmp) {
                    h64instruction_valuecopy vc = {0};
                    vc.type = H64INST_VALUECOPY;
                    vc.slotto = argtmp;
                    vc.slotfrom = (
                        expr->funcdef.arguments.arg_value[i]->
                            storage.eval_temp_id);
                    if (!appendinst(
                            rinfo->pr->program,
                            expr, expr->funcdef.arguments.arg_value[i],
                            // ^ expr as func again, see explanation above.
                            &vc
                            )) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                }

                free1linetemps(expr);  // expr as func.
                h64instruction_jumptarget jumpt = {0};
                jumpt.type = H64INST_JUMPTARGET;
                jumpt.jumpid = jump_past_id;
                if (!appendinst(
                        rinfo->pr->program,
                        expr, expr->funcdef.arguments.arg_value[i],
                        // ^ expr as func again, see explanation above.
                        &jumpt
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
            argtmp++;
            i++;
        }
        free1linetemps(expr);  // expr as func.

        i = 0;
        while (i < expr->funcdef.stmt_count) {
            rinfo->dont_descend_visitation = 0;
            int result = ast_VisitExpression(
                expr->funcdef.stmt[i], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;
            free1linetemps(expr);  // expr as func.
            i++;
        }

        free1linetemps(expr);  // expr as func.
        rinfo->dont_descend_visitation = 1;
        return 1;
    } else if (expr->type == H64EXPRTYPE_UNARYOP &&
               expr->op.optype == H64OP_NEW) {
        rinfo->dont_descend_visitation = 1;

        // This should have been enforced by the parser:
        assert(expr->op.value1->type == H64EXPRTYPE_CALL);

        // Visit all arguments of constructor call:
        int i = 0;
        while (i < expr->op.value1->inlinecall.arguments.arg_count) {
            rinfo->dont_descend_visitation = 0;
            if (!ast_VisitExpression(
                    expr->op.value1->inlinecall.arguments.arg_value[i],
                    expr,
                    &_codegencallback_DoCodegen_visit_in,
                    &_codegencallback_DoCodegen_visit_out,
                    _asttransform_cancel_visit_descend_callback,
                    rinfo
                    )) {
                rinfo->dont_descend_visitation = 1;
                return 0;
            }
            rinfo->dont_descend_visitation = 1;
            i++;
        }

        int objslot = -1;
        if (expr->op.value1->inlinecall.value->type !=
                    H64EXPRTYPE_IDENTIFIERREF ||
                expr->op.value1->inlinecall.value->storage.set ||
                expr->op.value1->inlinecall.value->storage.ref.type !=
                    H64STORETYPE_GLOBALCLASSSLOT) {
            // Not mapping to a class type we can obviously identify
            // at compile time. -> must obtain this at runtime.

            // Visit called object (= constructed type) to get
            // temporary:
            rinfo->dont_descend_visitation = 0;
            int result = ast_VisitExpression(
                expr->op.value1->inlinecall.value, expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;

            // The temporary cannot be a final variable, since if
            // the constructor errors that would leave us with
            // an invalid incomplete object possibly still accessible
            // by "rescue" code accessing that variable:
            objslot = expr->op.value1->inlinecall.value->
                storage.eval_temp_id;
            if (objslot < 0) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            assert(objslot >= 0);
            if (objslot < func->funcdef._storageinfo->
                    lowest_guaranteed_free_temp) {
                // This is a fixed variable or argument.
                objslot = new1linetemp(func, expr, 0);
                assert(objslot >= func->funcdef._storageinfo->
                       lowest_guaranteed_free_temp);
            }

            // Convert it to object instance:
            h64instruction_newinstancebyref inst_newinstbyref = {0};
            inst_newinstbyref.type = H64INST_NEWINSTANCEBYREF;
            inst_newinstbyref.slotto = objslot;
            inst_newinstbyref.classtypeslotfrom = (
                expr->op.value1->inlinecall.value->storage.eval_temp_id
            );
            if (!appendinst(
                    rinfo->pr->program, func, expr,
                    &inst_newinstbyref
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            // Apparently we already know the class id at compile time,
            // so instantiate it directly:

            // Slot to hold resulting object instance:
            objslot = new1linetemp(func, expr, 0);
            if (objslot < 0) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            assert(objslot >= 0);
            assert(
                objslot >= func->funcdef._storageinfo->
                lowest_guaranteed_free_temp
            );  // must not be variable

            // Create object instance directly from given class:
            h64instruction_newinstance inst_newinst = {0};
            inst_newinst.type = H64INST_NEWINSTANCE;
            inst_newinst.slotto = objslot;
            inst_newinst.classidcreatefrom = ((int64_t)
                expr->op.value1->inlinecall.value->storage.ref.id
            );
            if (!appendinst(
                    rinfo->pr->program, func, expr,
                    &inst_newinst
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
        // Prepare unused temporary for constructor call:
        int temp = new1linetemp(func, expr, 0);
        if (temp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        // Place constructor in unused result temporary:
        h64instruction_getconstructor inst_getconstr = {0};
        inst_getconstr.type = H64INST_GETCONSTRUCTOR;
        inst_getconstr.slotto = temp;
        inst_getconstr.objslotfrom = objslot;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_getconstr
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        // Generate call to actual constructor:
        assert(func != NULL && expr != NULL);
        if (!_codegen_call_to(
                rinfo, func, expr->op.value1, temp, temp, 1
                )) {
            return 0;
        }
        // Move object to result:
        int resulttemp = new1linetemp(func, expr, 1);
        if (resulttemp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        if (objslot != resulttemp) {
            h64instruction_valuecopy inst_vc = {0};
            inst_vc.type = H64INST_VALUECOPY;
            inst_vc.slotto = resulttemp;
            inst_vc.slotfrom = objslot;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_vc
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }

        rinfo->dont_descend_visitation = 1;
        expr->storage.eval_temp_id = resulttemp;
        return 1;
    } else if (expr->type == H64EXPRTYPE_WITH_STMT) {
        rinfo->dont_descend_visitation = 1;

        int32_t jumpid_finally = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        // First, set all the temporaries of the "with'ed" values to none:
        assert(expr->withstmt.withclause_count >= 1);
        int32_t i = 0;
        while (i < expr->withstmt.withclause_count) {
            assert(
                expr->withstmt.withclause[i]->
                storage.eval_temp_id >= 0 || (
                expr->withstmt.withclause[i]->storage.set &&
                expr->withstmt.withclause[i]->storage.ref.type ==
                    H64STORETYPE_STACKSLOT)
            );
            h64instruction_setconst inst_setconst = {0};
            inst_setconst.type = H64INST_SETCONST;
            inst_setconst.slot = (
                expr->withstmt.withclause[i]->storage.eval_temp_id >= 0 ?
                expr->withstmt.withclause[i]->storage.eval_temp_id :
                expr->withstmt.withclause[i]->storage.ref.id
            );
            memset(
                &inst_setconst.content, 0, sizeof(inst_setconst.content)
            );
            inst_setconst.content.type = H64VALTYPE_NONE;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_setconst
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            i++;
        }

        // Ok, before setting any of the true values,
        // set up the error catch frame in case the init already errors:
        if (!_enforce_dostmt_limit_in_func(rinfo, func))
            return 0;
        int16_t dostmtid = (
            func->funcdef._storageinfo->dostmts_used
        );
        func->funcdef._storageinfo->dostmts_used++;
        h64instruction_pushrescueframe inst_pushframe = {0};
        inst_pushframe.type = H64INST_PUSHRESCUEFRAME;
        inst_pushframe.sloterrorto = -1;
        inst_pushframe.jumponrescue = -1;
        inst_pushframe.jumponfinally = jumpid_finally;
        inst_pushframe.mode = RESCUEMODE_JUMPONFINALLY;
        inst_pushframe.frameid = dostmtid;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_pushframe
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        h64instruction_addrescuetype addctype = {0};
        addctype.type = H64INST_ADDRESCUETYPE;
        addctype.frameid = dostmtid;
        addctype.classid = (classid_t)H64STDERROR_ERROR;
        if (!appendinst(
                rinfo->pr->program, func, expr, &addctype
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        // Visit with'ed values to generate their code:
        i = 0;
        while (i < expr->withstmt.withclause_count) {
            rinfo->dont_descend_visitation = 0;
            int result = ast_VisitExpression(
                expr->withstmt.withclause[i], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;
            i++;
        }

        // Inner code contents:
        i = 0;
        while (i < expr->withstmt.stmt_count) {
            rinfo->dont_descend_visitation = 0;
            int result = ast_VisitExpression(
                expr->withstmt.stmt[i], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;
            free1linetemps(func);
            i++;
        }

        // NOTE: jumptofinally is needed even if finally block follows
        // immediately, such that horsevm knows that the finally
        // was already triggered. (Important if another error were
        // to happen.)
        h64instruction_jumptofinally inst_jumptofinally = {0};
        inst_jumptofinally.type = H64INST_JUMPTOFINALLY;
        inst_jumptofinally.frameid = dostmtid;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumptofinally
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        // Start of finally block:
        h64instruction_jumptarget inst_jumpfinally = {0};
        inst_jumpfinally.type = H64INST_JUMPTARGET;
        inst_jumpfinally.jumpid = jumpid_finally;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumpfinally
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        // Call .close() on all objects that have that property.
        // However, we need to wrap them all with tiny do/finally clauses
        // such that even when one of them fails, the others still
        // attempt to run afterwards.
        int16_t *_withclause_rescueframeid = (
            malloc(sizeof(*_withclause_rescueframeid) *
                   expr->withstmt.withclause_count)
        );
        if (!_withclause_rescueframeid) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        int32_t *_withclause_jumpfinallyid = (
            malloc(sizeof(*_withclause_jumpfinallyid) *
                   expr->withstmt.withclause_count)
        );
        if (!_withclause_jumpfinallyid) {
            free(_withclause_rescueframeid);
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        memset(
            _withclause_rescueframeid, 0,
            sizeof(*_withclause_rescueframeid) *
                expr->withstmt.withclause_count
        );
        memset(
            _withclause_jumpfinallyid, 0,
            sizeof(*_withclause_jumpfinallyid) *
                expr->withstmt.withclause_count
        );
        // Add nested do {first.close()} finally {second.close() ...}
        i = 0;
        while (i < expr->withstmt.withclause_count) {
            int gotfinally = 0;
            if (i + 1 < expr->withstmt.withclause_count) {
                if (!_enforce_dostmt_limit_in_func(rinfo, func)) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    return 0;
                }
                gotfinally = 1;
                _withclause_rescueframeid[i] = (
                    func->funcdef._storageinfo->dostmts_used
                );
                func->funcdef._storageinfo->dostmts_used++;
                _withclause_jumpfinallyid[i] = (
                    func->funcdef._storageinfo->jump_targets_used
                );
                func->funcdef._storageinfo->jump_targets_used++;
                h64instruction_pushrescueframe inst_pushframe2 = {0};
                inst_pushframe2.type = H64INST_PUSHRESCUEFRAME;
                inst_pushframe2.sloterrorto = -1;
                inst_pushframe2.jumponrescue = -1;
                inst_pushframe2.jumponfinally = (
                    _withclause_jumpfinallyid[i]
                );
                inst_pushframe2.frameid = (
                    _withclause_rescueframeid[i]
                );
                inst_pushframe2.mode = RESCUEMODE_JUMPONFINALLY;
                if (!appendinst(
                        rinfo->pr->program, func, expr,
                        &inst_pushframe2
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                h64instruction_addrescuetype addctype2 = {0};
                addctype2.type = H64INST_ADDRESCUETYPE;
                addctype2.frameid = (
                    _withclause_rescueframeid[i]
                );
                addctype2.classid = (classid_t)H64STDERROR_ERROR;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &addctype2
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
            int32_t jump_past_hasattr_id = (
                func->funcdef._storageinfo->jump_targets_used
            );
            func->funcdef._storageinfo->jump_targets_used++;
            // Check if value has .close() attribute, and call it:
            int64_t closeidx = (
                h64debugsymbols_AttributeNameToAttributeNameId(
                    rinfo->pr->program->symbols, "close", 0, 0
                )
            );
            if (closeidx >= 0) {
                h64instruction_hasattrjump hasattrcheck = {0};
                hasattrcheck.type = H64INST_HASATTRJUMP;
                hasattrcheck.jumpbytesoffset = (
                    jump_past_hasattr_id
                );
                hasattrcheck.nameidxcheck = closeidx;
                hasattrcheck.slotvaluecheck = (
                    expr->withstmt.withclause[i]->storage.eval_temp_id
                );
                if (!appendinst(
                        rinfo->pr->program, func, expr, &hasattrcheck
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                // It has .close() attribute, so get & call it:
                int16_t slotid = new1linetemp(
                    func, NULL, 0
                );
                h64instruction_getattributebyname abyname = {0};
                abyname.type = H64INST_GETATTRIBUTEBYNAME;
                abyname.objslotfrom = (
                    expr->withstmt.withclause[i]->storage.eval_temp_id
                );
                abyname.slotto = slotid;
                abyname.nameidx = closeidx;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &abyname
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                h64instruction_call callclose = {0};
                callclose.type = H64INST_CALL;
                callclose.slotcalledfrom = slotid;
                callclose.flags = 0;
                callclose.kwargs = 0;
                callclose.posargs = 0;
                callclose.returnto = slotid;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &callclose
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                free1linetemps(func);
            } else {
                h64instruction_jump skipattrcheck = {0};
                skipattrcheck.type = H64INST_JUMP;
                skipattrcheck.jumpbytesoffset = (
                    jump_past_hasattr_id
                );
                if (!appendinst(
                        rinfo->pr->program, func, expr, &skipattrcheck
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
            h64instruction_jumptarget pastchecktarget = {0};
            pastchecktarget.type = H64INST_JUMPTARGET;
            pastchecktarget.jumpid = jump_past_hasattr_id;
            if (!appendinst(
                    rinfo->pr->program, func, expr,
                    &pastchecktarget
                    )) {
                free(_withclause_rescueframeid);
                free(_withclause_jumpfinallyid);
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            if (gotfinally) {
                h64instruction_jumptofinally nowtofinally = {0};
                nowtofinally.type = H64INST_JUMPTOFINALLY;
                nowtofinally.frameid = (
                    _withclause_rescueframeid[i]
                );
                if (!appendinst(
                        rinfo->pr->program, func, expr, &nowtofinally
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                h64instruction_jumptarget finallytarget = {0};
                finallytarget.type = H64INST_JUMPTARGET;
                finallytarget.jumpid = (
                    _withclause_jumpfinallyid[i]
                );
                if (!appendinst(
                        rinfo->pr->program, func, expr, &finallytarget
                        )) {
                    free(_withclause_rescueframeid);
                    free(_withclause_jumpfinallyid);
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
            i++;
        }
        // Pop all the catch frames again in reverse, at the end:
        i = expr->withstmt.withclause_count - 1;
        while (i >= 0) {
            int gotfinally = 0;
            if (i + 1 < expr->withstmt.withclause_count)
                gotfinally = 1;
            if (gotfinally) {
                h64instruction_poprescueframe inst_popcatch = {0};
                inst_popcatch.type = H64INST_POPRESCUEFRAME;
                inst_popcatch.frameid = (
                    _withclause_rescueframeid[i]
                );
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_popcatch
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
            i--;
        }

        free(_withclause_rescueframeid);
        _withclause_rescueframeid = NULL;
        free(_withclause_jumpfinallyid);
        _withclause_jumpfinallyid = NULL;

        // End of entire block here.
        h64instruction_poprescueframe inst_popcatch = {0};
        inst_popcatch.type = H64INST_POPRESCUEFRAME;
        inst_popcatch.frameid = dostmtid;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_popcatch
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        free1linetemps(func);
        rinfo->dont_descend_visitation = 1;
        return 1;
    } else if (expr->type == H64EXPRTYPE_DO_STMT) {
        rinfo->dont_descend_visitation = 1;

        int32_t jumpid_catch = -1;
        int32_t jumpid_finally = -1;
        int32_t jumpid_end = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        if (!_enforce_dostmt_limit_in_func(rinfo, func))
            return 0;
        int16_t dostmtid = (
            func->funcdef._storageinfo->dostmts_used
        );
        func->funcdef._storageinfo->dostmts_used++;

        h64instruction_pushrescueframe inst_pushframe = {0};
        inst_pushframe.type = H64INST_PUSHRESCUEFRAME;
        inst_pushframe.sloterrorto = -1;
        inst_pushframe.jumponrescue = -1;
        inst_pushframe.jumponfinally = -1;
        inst_pushframe.frameid = dostmtid;
        if (expr->dostmt.errors_count > 0) {
            assert(!expr->storage.set ||
                   expr->storage.ref.type ==
                   H64STORETYPE_STACKSLOT);
            int error_tmp = -1;
            if (expr->storage.set) {
                error_tmp = expr->storage.ref.id;
            }
            inst_pushframe.sloterrorto = error_tmp;
            inst_pushframe.mode |= RESCUEMODE_JUMPONRESCUE;
            jumpid_catch = (
                func->funcdef._storageinfo->jump_targets_used
            );
            func->funcdef._storageinfo->jump_targets_used++;
            inst_pushframe.jumponrescue = jumpid_catch;
            assert(inst_pushframe.jumponrescue >= 0);
        }
        if (expr->dostmt.has_finally_block) {
            inst_pushframe.mode |= RESCUEMODE_JUMPONFINALLY;
            jumpid_finally = (
                func->funcdef._storageinfo->jump_targets_used
            );
            func->funcdef._storageinfo->jump_targets_used++;
            inst_pushframe.jumponfinally = jumpid_finally;
        }
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_pushframe
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        int error_reuse_tmp = -1;
        int i = 0;
        while (i < expr->dostmt.errors_count) {
            assert(expr->dostmt.errors[i]->storage.set);
            int error_tmp = -1;
            if (expr->dostmt.errors[i]->storage.ref.type ==
                    H64STORETYPE_STACKSLOT) {
                error_tmp = (int)(
                    expr->dostmt.errors[i]->storage.ref.id
                );
            } else if (expr->dostmt.errors[i]->storage.ref.type ==
                       H64STORETYPE_GLOBALCLASSSLOT) {
                h64instruction_addrescuetype addctype = {0};
                addctype.type = H64INST_ADDRESCUETYPE;
                addctype.frameid = dostmtid;
                addctype.classid = (
                    expr->dostmt.errors[i]->
                        storage.ref.id
                );
                if (!appendinst(
                        rinfo->pr->program, func, expr, &addctype
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
                i++;
                continue;
            } else {
                assert(expr->dostmt.errors[i]->storage.ref.type ==
                       H64STORETYPE_GLOBALVARSLOT);
                if (error_reuse_tmp < 0) {
                    error_reuse_tmp = new1linetemp(
                        func, expr, 0
                    );
                    if (error_reuse_tmp < 0) {
                        rinfo->hadoutofmemory = 1;
                        return 0;
                    }
                }
                error_tmp = error_reuse_tmp;
                h64instruction_getglobal inst_getglobal = {0};
                inst_getglobal.type = H64INST_GETGLOBAL;
                inst_getglobal.slotto = error_tmp;
                inst_getglobal.globalfrom = expr->dostmt.errors[i]->
                    storage.ref.id;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_getglobal
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
            assert(error_tmp >= 0);
            h64instruction_addrescuetypebyref addctyperef = {0};
            addctyperef.type = H64INST_ADDRESCUETYPEBYREF;
            addctyperef.slotfrom = error_tmp;
            addctyperef.frameid = dostmtid;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &addctyperef
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            i++;
        }

        i = 0;
        while (i < expr->dostmt.dostmt_count) {
            rinfo->dont_descend_visitation = 0;
            int result = ast_VisitExpression(
                expr->dostmt.dostmt[i], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;
            free1linetemps(func);
            i++;
        }
        if ((inst_pushframe.mode & RESCUEMODE_JUMPONFINALLY) == 0) {
            h64instruction_poprescueframe inst_popcatch = {0};
            inst_popcatch.type = H64INST_POPRESCUEFRAME;
            inst_popcatch.frameid = dostmtid;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_popcatch
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            if ((inst_pushframe.mode & RESCUEMODE_JUMPONRESCUE) != 0) {
                h64instruction_jump inst_jump = {0};
                inst_jump.type = H64INST_JUMP;
                inst_jump.jumpbytesoffset = jumpid_end;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_jump
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        } else {
            // NOTE: this is needed even when the finally block follows
            // immediately, such that horsevm knows that the finally
            // was already triggered. (Important if another error were
            // to happen.)
            h64instruction_jumptofinally inst_jumptofinally = {0};
            inst_jumptofinally.type = H64INST_JUMPTOFINALLY;
            inst_jumptofinally.frameid = dostmtid;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_jumptofinally
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }

        if ((inst_pushframe.mode & RESCUEMODE_JUMPONRESCUE) != 0) {
            h64instruction_jumptarget inst_jumpcatch = {0};
            inst_jumpcatch.type = H64INST_JUMPTARGET;
            inst_jumpcatch.jumpid = jumpid_catch;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_jumpcatch
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            i = 0;
            while (i < expr->dostmt.rescuestmt_count) {
                rinfo->dont_descend_visitation = 0;
                int result = ast_VisitExpression(
                    expr->dostmt.rescuestmt[i], expr,
                    &_codegencallback_DoCodegen_visit_in,
                    &_codegencallback_DoCodegen_visit_out,
                    _asttransform_cancel_visit_descend_callback,
                    rinfo
                );
                rinfo->dont_descend_visitation = 1;
                if (!result)
                    return 0;
                free1linetemps(func);
                i++;
            }
            if ((inst_pushframe.mode & RESCUEMODE_JUMPONFINALLY) == 0) {
                // No finally follows, so we need to clean up the
                // error frame here.
                h64instruction_poprescueframe inst_popcatch = {0};
                inst_popcatch.type = H64INST_POPRESCUEFRAME;
                inst_popcatch.frameid = dostmtid;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_popcatch
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            } else {
                // NOTE: needed despite finally being right after, SEE ABOVE.
                h64instruction_jumptofinally inst_jumptofinally = {0};
                inst_jumptofinally.type = H64INST_JUMPTOFINALLY;
                inst_jumptofinally.frameid = dostmtid;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_jumptofinally
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        }

        if ((inst_pushframe.mode & RESCUEMODE_JUMPONFINALLY) != 0) {
            h64instruction_jumptarget inst_jumpfinally = {0};
            inst_jumpfinally.type = H64INST_JUMPTARGET;
            inst_jumpfinally.jumpid = jumpid_finally;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_jumpfinally
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            i = 0;
            while (i < expr->dostmt.finallystmt_count) {
                rinfo->dont_descend_visitation = 0;
                int result = ast_VisitExpression(
                    expr->dostmt.finallystmt[i], expr,
                    &_codegencallback_DoCodegen_visit_in,
                    &_codegencallback_DoCodegen_visit_out,
                    _asttransform_cancel_visit_descend_callback,
                    rinfo
                );
                rinfo->dont_descend_visitation = 1;
                if (!result)
                    return 0;
                free1linetemps(func);
                i++;
            }
            h64instruction_poprescueframe inst_popcatch = {0};
            inst_popcatch.type = H64INST_POPRESCUEFRAME;
            inst_popcatch.frameid = dostmtid;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_popcatch
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }

        h64instruction_jumptarget inst_jumpend = {0};
        inst_jumpend.type = H64INST_JUMPTARGET;
        inst_jumpend.jumpid = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumpend
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        free1linetemps(func);
        rinfo->dont_descend_visitation = 1;
        return 1;
    } else if (expr->type == H64EXPRTYPE_CALL &&
            expr->inlinecall.value->type == H64EXPRTYPE_IDENTIFIERREF &&
            expr->inlinecall.value->storage.set &&
            expr->inlinecall.value->storage.ref.type ==
                H64STORETYPE_GLOBALFUNCSLOT &&
            expr->inlinecall.value->storage.ref.id ==
                rinfo->pr->program->has_attr_func_idx) {
        rinfo->dont_descend_visitation = 1;

        int resulttmp = new1linetemp(
            func, expr, 1
        );
        assert(resulttmp >= 0);

        if (expr->inlinecall.arguments.arg_count != 2 || (
                expr->inlinecall.arguments.arg_name != NULL && (
                expr->inlinecall.arguments.arg_name[0] != NULL ||
                expr->inlinecall.arguments.arg_name[1] != NULL))) {
            result_AddMessage(
                rinfo->pr->resultmsg, H64MSG_ERROR,
                "unexpected call to has_attr() with not "
                "exactly two positional arguments",
                rinfo->ast->fileuri, rinfo->ast->fileurilen,
                expr->line, expr->column
            );
            rinfo->hadunexpectederror = 1;
            return 0;
        }
        if (expr->inlinecall.arguments.arg_value[1]->type !=
                H64EXPRTYPE_LITERAL ||
                expr->inlinecall.arguments.arg_value[1]->
                    literal.type != H64TK_CONSTANT_STRING
                ) {
            result_AddMessage(
                rinfo->pr->resultmsg, H64MSG_ERROR,
                "unexpected call to has_attr() with non-trivial "
                "attribute argument. must be plain string literal "
                "since has_attr() is not a normal function",
                rinfo->ast->fileuri, rinfo->ast->fileurilen,
                expr->line, expr->column
            );
            rinfo->hadunexpectederror = 1;
            return 0;
        }
        int64_t nameidx = -1;
        if ((int)strlen(expr->inlinecall.arguments.arg_value[1]->
                literal.str_value) ==
                expr->inlinecall.arguments.arg_value[1]->
                literal.str_value_len) {  // no null byte
            nameidx = (
                h64debugsymbols_AttributeNameToAttributeNameId(
                    rinfo->pr->program->symbols,
                    expr->inlinecall.arguments.arg_value[1]->
                        literal.str_value,
                    isbuiltinattrname(
                        expr->inlinecall.arguments.arg_value[1]->
                        literal.str_value), 0
                )
            );
        }
        if (nameidx < 0) {
            h64instruction_setconst inst_setconst = {0};
            inst_setconst.type = H64INST_SETCONST;
            inst_setconst.slot = resulttmp;
            inst_setconst.content.type = H64VALTYPE_BOOL;
            inst_setconst.content.int_value = 0;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_setconst
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        } else {
            rinfo->dont_descend_visitation = 0;
            expr->inlinecall.arguments.arg_value[0]->
                storage.eval_temp_id = -1;
            int result = ast_VisitExpression(
                expr->inlinecall.arguments.arg_value[0], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            if (!result) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            rinfo->dont_descend_visitation = 1;
            if (expr->inlinecall.arguments.arg_value[0]->
                    storage.eval_temp_id < 0) {
                rinfo->hadunexpectederror = 1;
                return 0;
            }
            assert(
                expr->inlinecall.arguments.arg_value[0]->
                storage.eval_temp_id != resulttmp
            );
            h64instruction_setconst inst_setconst = {0};
            inst_setconst.type = H64INST_SETCONST;
            inst_setconst.slot = resulttmp;
            inst_setconst.content.type = H64VALTYPE_BOOL;
            inst_setconst.content.int_value = 0;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_setconst
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            int32_t jumpid_pastset = (
                func->funcdef._storageinfo->jump_targets_used
            );
            func->funcdef._storageinfo->jump_targets_used++;
            
            h64instruction_hasattrjump inst_haj = {0};
            inst_haj.type = H64INST_HASATTRJUMP;
            inst_haj.jumpbytesoffset = jumpid_pastset;
            inst_haj.nameidxcheck = nameidx;
            inst_haj.slotvaluecheck = (
                expr->inlinecall.arguments.arg_value[0]->
                storage.eval_temp_id
            );
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_haj
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            h64instruction_setconst inst_setconst2 = {0};
            inst_setconst2.type = H64INST_SETCONST;
            inst_setconst2.slot = resulttmp;
            inst_setconst2.content.type = H64VALTYPE_BOOL;
            inst_setconst2.content.int_value = 1;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_setconst2
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }

            h64instruction_jumptarget inst_jumpppastset = {0};
            inst_jumpppastset.type = H64INST_JUMPTARGET;
            inst_jumpppastset.jumpid = jumpid_pastset;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_jumpppastset
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }

        rinfo->dont_descend_visitation = 1;
        expr->storage.eval_temp_id = resulttmp;
        return 1;
    } else if (expr->type == H64EXPRTYPE_FOR_STMT) {
        rinfo->dont_descend_visitation = 1;
        int32_t jumpid_start = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;
        int32_t jumpid_end = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        // Make sure inner visited stuff has the loop
        // information, for break/continue etc.
        if (extra->loop_nesting_depth + 1 >
                extra->loop_nesting_alloc) {
            int new_alloc = (
                extra->loop_nesting_depth + 16
            );
            if (new_alloc < extra->loop_nesting_alloc * 2)
                new_alloc = extra->loop_nesting_alloc * 2;
            int64_t *new_start_ids = realloc(
                extra->loop_start_jumpid,
                sizeof(*extra->loop_start_jumpid) * new_alloc
            );
            if (!new_start_ids) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            extra->loop_start_jumpid = new_start_ids;
            int64_t *new_end_ids = realloc(
                extra->loop_end_jumpid,
                sizeof(*extra->loop_end_jumpid) * new_alloc
            );
            if (!new_end_ids) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            extra->loop_end_jumpid = new_end_ids;
            extra->loop_nesting_alloc = new_alloc;
        }
        extra->loop_start_jumpid[
            extra->loop_nesting_depth
        ] = jumpid_start;
        extra->loop_end_jumpid[
            extra->loop_nesting_depth
        ] = jumpid_end;
        extra->loop_nesting_depth++;

        int itertemp = newmultilinetemp(func);
        if (itertemp < 0) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        // Visit container value to get the slot of where it's stored:
        rinfo->dont_descend_visitation = 0;
        assert(expr->forstmt.iterated_container->
               storage.eval_temp_id <= 0);
        expr->forstmt.iterated_container->storage.eval_temp_id = -1;
        int result = ast_VisitExpression(
            expr->forstmt.iterated_container, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        if (!result) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
        rinfo->dont_descend_visitation = 1;
        int containertemp = -1;
        assert(expr->forstmt.iterated_container->
                   storage.eval_temp_id >= 0);
        containertemp = (
            expr->forstmt.iterated_container->storage.eval_temp_id
        );

        h64instruction_newiterator inst_newiter = {0};
        inst_newiter.type = H64INST_NEWITERATOR;
        inst_newiter.slotiteratorto = itertemp;
        inst_newiter.slotcontainerfrom = containertemp;

        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_newiter
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        h64instruction_jumptarget inst_jumpstart = {0};
        inst_jumpstart.type = H64INST_JUMPTARGET;
        inst_jumpstart.jumpid = jumpid_start;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumpstart
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        assert(
            expr->storage.set &&
            expr->storage.ref.type == H64STORETYPE_STACKSLOT
        );
        h64instruction_iterate inst_iterate = {0};
        inst_iterate.type = H64INST_ITERATE;
        inst_iterate.slotvalueto = expr->storage.ref.id;
        inst_iterate.slotiteratorfrom = itertemp;
        inst_iterate.jumponend = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_iterate
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        int i = 0;
        while (i < expr->forstmt.stmt_count) {
            rinfo->dont_descend_visitation = 0;
            int result = ast_VisitExpression(
                expr->forstmt.stmt[i], expr,
                &_codegencallback_DoCodegen_visit_in,
                &_codegencallback_DoCodegen_visit_out,
                _asttransform_cancel_visit_descend_callback,
                rinfo
            );
            rinfo->dont_descend_visitation = 1;
            if (!result)
                return 0;
            i++;
        }

        h64instruction_jump inst_jump = {0};
        inst_jump.type = H64INST_JUMP;
        inst_jump.jumpbytesoffset = jumpid_start;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        h64instruction_jumptarget inst_jumpend = {0};
        inst_jumpend.type = H64INST_JUMPTARGET;
        inst_jumpend.jumpid = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumpend
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        freemultilinetemp(func, itertemp);
        rinfo->dont_descend_visitation = 1;
        extra->loop_nesting_depth--;  // leaving the loop
        assert(extra->loop_nesting_depth >= 0);
        free1linetemps(func);
        return 1;
    } else if (expr->type == H64EXPRTYPE_GIVEN) {
        rinfo->dont_descend_visitation = 1;

        int tmp_result = new1linetemp(
            func, expr, 1
        );

        int32_t jumpid_false = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        rinfo->dont_descend_visitation = 1;
        int32_t jumpid_end = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        rinfo->dont_descend_visitation = 0;
        int result = ast_VisitExpression(
            expr->given.condition, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;

        h64instruction_condjump inst_condjump = {0};
        inst_condjump.type = H64INST_CONDJUMP;
        inst_condjump.conditionalslot = (
            expr->given.condition->storage.eval_temp_id
        );
        inst_condjump.jumpbytesoffset = (
            jumpid_false
        );
        assert(inst_condjump.jumpbytesoffset >= 0);
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_condjump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        rinfo->dont_descend_visitation = 0;
        result = ast_VisitExpression(
            expr->given.valueyes, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;

        if (expr->given.valueyes->storage.eval_temp_id != tmp_result) {
            h64instruction_valuecopy inst_vc = {0};
            inst_vc.type = H64INST_VALUECOPY;
            inst_vc.slotfrom = expr->given.valueyes->storage.eval_temp_id;
            inst_vc.slotto = tmp_result;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_vc
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }

        h64instruction_jump inst_jump = {0};
        inst_jump.type = H64INST_JUMP;
        inst_jump.jumpbytesoffset = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jump
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        h64instruction_jumptarget inst_jumptarget = {0};
        inst_jumptarget.type = H64INST_JUMPTARGET;
        inst_jumptarget.jumpid = jumpid_false;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumptarget
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        rinfo->dont_descend_visitation = 0;
        result = ast_VisitExpression(
            expr->given.valueno, expr,
            &_codegencallback_DoCodegen_visit_in,
            &_codegencallback_DoCodegen_visit_out,
            _asttransform_cancel_visit_descend_callback,
            rinfo
        );
        rinfo->dont_descend_visitation = 1;
        if (!result)
            return 0;

        if (expr->given.valueno->storage.eval_temp_id != tmp_result) {
            h64instruction_valuecopy inst_vc = {0};
            inst_vc.type = H64INST_VALUECOPY;
            inst_vc.slotfrom = expr->given.valueno->storage.eval_temp_id;
            inst_vc.slotto = tmp_result;
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_vc
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }

        h64instruction_jumptarget inst_jumptarget2 = {0};
        inst_jumptarget2.type = H64INST_JUMPTARGET;
        inst_jumptarget2.jumpid = jumpid_end;
        if (!appendinst(
                rinfo->pr->program, func, expr, &inst_jumptarget2
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }

        rinfo->dont_descend_visitation = 1;
        expr->storage.eval_temp_id = tmp_result;
        return 1;
    } else if (expr->type == H64EXPRTYPE_IF_STMT) {
        rinfo->dont_descend_visitation = 1;
        int32_t jumpid_end = (
            func->funcdef._storageinfo->jump_targets_used
        );
        func->funcdef._storageinfo->jump_targets_used++;

        struct h64ifstmt *current_clause = &expr->ifstmt;
        assert(current_clause->conditional != NULL);
        while (current_clause != NULL) {
            int32_t jumpid_nextclause = -1;
            if (current_clause->followup_clause) {
                jumpid_nextclause = (
                    func->funcdef._storageinfo->jump_targets_used
                );
                func->funcdef._storageinfo->jump_targets_used++;
            }

            assert(
                !current_clause->conditional ||
                current_clause->conditional->parent == expr
            );
            if (current_clause->conditional) {
                rinfo->dont_descend_visitation = 0;
                int result = ast_VisitExpression(
                    current_clause->conditional, expr,
                    &_codegencallback_DoCodegen_visit_in,
                    &_codegencallback_DoCodegen_visit_out,
                    _asttransform_cancel_visit_descend_callback,
                    rinfo
                );
                rinfo->dont_descend_visitation = 1;
                if (!result)
                    return 0;

                h64instruction_condjump inst_condjump = {0};
                inst_condjump.type = H64INST_CONDJUMP;
                inst_condjump.conditionalslot = (
                    current_clause->conditional->storage.eval_temp_id
                );
                inst_condjump.jumpbytesoffset = (
                    current_clause->followup_clause != NULL ?
                    jumpid_nextclause : jumpid_end
                );
                assert(inst_condjump.jumpbytesoffset >= 0);
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_condjump
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }

            int i = 0;
            while (i < current_clause->stmt_count) {
                rinfo->dont_descend_visitation = 0;
                int result = ast_VisitExpression(
                    current_clause->stmt[i], expr,
                    &_codegencallback_DoCodegen_visit_in,
                    &_codegencallback_DoCodegen_visit_out,
                    _asttransform_cancel_visit_descend_callback,
                    rinfo
                );
                rinfo->dont_descend_visitation = 1;
                if (!result)
                    return 0;
                i++;
            }

            if (current_clause->followup_clause != NULL) {
                h64instruction_jump inst_jump = {0};
                inst_jump.type = H64INST_JUMP;
                inst_jump.jumpbytesoffset = jumpid_end;
                if (!appendinst(
                        rinfo->pr->program, func, expr, &inst_jump
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }

            h64instruction_jumptarget inst_jumptarget = {0};
            inst_jumptarget.type = H64INST_JUMPTARGET;
            if (current_clause->followup_clause == NULL) {
                inst_jumptarget.jumpid = jumpid_end;
            } else {
                inst_jumptarget.jumpid = jumpid_nextclause;
            }
            if (!appendinst(
                    rinfo->pr->program, func, expr, &inst_jumptarget
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
            rinfo->dont_descend_visitation = 1;
            current_clause = current_clause->followup_clause;
        }
        rinfo->dont_descend_visitation = 1;
        free1linetemps(func);
        return 1;
    }

    return 1;
}

static int _codegen_calc_tempclassfakeinitfuncstack_cb(
        ATTR_UNUSED hashmap *map, const char *bytes,
        uint64_t byteslen, uint64_t number,
        void *userdata
        ) {
    struct asttransforminfo *fiterinfo = (
        (struct asttransforminfo *)userdata
    );
    classid_t classidx;
    assert(byteslen == sizeof(classidx));
    memcpy(&classidx, bytes, byteslen);
    h64expression *func = (h64expression *)(uintptr_t)number;
    assert(func != NULL);
    assert(func->type == H64EXPRTYPE_FUNCDEF_STMT);
    assert(fiterinfo->pr->program != NULL);
    codegen_CalculateFinalFuncStack(
        fiterinfo->pr->program, func
    );
    return 1;
}

int codegen_GenerateBytecodeForFile(
        h64compileproject *project, h64misccompileroptions *miscoptions,
        h64ast *resolved_ast
        ) {
    if (!project || !resolved_ast)
        return 0;

    if (miscoptions->compiler_stage_debug) {
        h64fprintf(
            stderr, "horsec: debug: codegen_GenerateBytecodeForFile "
                "start on %s (pr->resultmsg.success: %d)\n",
            resolved_ast->fileuri, project->resultmsg->success
        );
    }

    // Do actual codegen step:
    asttransformcodegenextra extra = {0};
    int transformresult = asttransform_Apply(
        project, resolved_ast,
        &_codegencallback_DoCodegen_visit_in,
        &_codegencallback_DoCodegen_visit_out,
        &extra
    );
    free(extra.loop_start_jumpid);
    extra.loop_start_jumpid = 0;
    free(extra.loop_end_jumpid);
    extra.loop_end_jumpid = 0;
    extra.loop_nesting_alloc = 0;
    if (!transformresult)
        return 0;
    // Ensure final stack is calculated for "made-up" func expressions:
    {
        asttransforminfo rinfo = {0};
        rinfo.pr = project;
        rinfo.ast = resolved_ast;
        rinfo.userdata = &extra;
        if (project->_tempglobalfakeinitfunc) {
            codegen_CalculateFinalFuncStack(
                project->program, project->_tempglobalfakeinitfunc
            );
        }
        if (project->_tempclassesfakeinitfunc_map) {
            int iterresult = hash_BytesMapIterate(
                project->_tempclassesfakeinitfunc_map,
                &_codegen_calc_tempclassfakeinitfuncstack_cb,
                &rinfo
            );
            free(extra.loop_start_jumpid);
            extra.loop_start_jumpid = 0;
            free(extra.loop_end_jumpid);
            extra.loop_end_jumpid = 0;
            extra.loop_nesting_alloc = 0;
            if (!iterresult || rinfo.hadoutofmemory ||
                    rinfo.hadunexpectederror) {
                if (!result_AddMessage(
                        project->resultmsg,
                        H64MSG_ERROR, "unexpected _codegen_calc_"
                        "tempclassfakeinitfuncstack_cb iteration "
                        "failure",
                        NULL, 0, -1, -1
                        )) {
                    // Nothing we can do
                }
                return 0;
            }
        }
    }

    if (miscoptions->compiler_stage_debug) {
        h64fprintf(
            stderr, "horsec: debug: codegen_GenerateBytecodeForFile "
                "completed on %s (pr->resultmsg.success: %d)\n",
            resolved_ast->fileuri, project->resultmsg->success
        );
    }

    return 1;
}
