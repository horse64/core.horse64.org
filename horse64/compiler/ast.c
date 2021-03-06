// Copyright (c) 2020, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#include "compileconfig.h"

#if defined(_WIN32) || defined(_WIN64)
#include <malloc.h>
#else
#include <alloca.h>
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "compiler/ast.h"
#include "compiler/globallimits.h"
#include "compiler/lexer.h"
#include "compiler/operator.h"
#include "compiler/varstorage.h"
#include "nonlocale.h"


h64scope *ast_GetScope(
        h64expression *child_expr, h64scope *global_scope
        ) {
    h64expression *expr = child_expr->parent;
    if (!expr)
        return global_scope;
    switch (expr->type) {
    case H64EXPRTYPE_FUNCDEF_STMT:
    case H64EXPRTYPE_INLINEFUNCDEF:
        return &expr->funcdef.scope;
    case H64EXPRTYPE_CLASSDEF_STMT:
        return &expr->classdef.scope;
    case H64EXPRTYPE_FOR_STMT:
        return &expr->forstmt.scope;
    case H64EXPRTYPE_IF_STMT: ;
        struct h64ifstmt *curr_clause = &expr->ifstmt;
        while (curr_clause) {
            if (curr_clause->conditional == child_expr)
                return &curr_clause->scope;
            int i = 0;
            while (i < curr_clause->stmt_count) {
                if (curr_clause->stmt[i] == child_expr) {
                    return &curr_clause->scope;
                }
                i++;
            }
            curr_clause = curr_clause->followup_clause;
        }
        return NULL;  // shouldn't be hit on well-formed AST
    case H64EXPRTYPE_WHILE_STMT:
        return &expr->whilestmt.scope;
    case H64EXPRTYPE_DO_STMT: ;
        int i = 0;
        while (i < expr->dostmt.dostmt_count) {
            if (expr->dostmt.dostmt[i] == child_expr)
                return &expr->dostmt.doscope;
            i++;
        }
        i = 0;
        while (i < expr->dostmt.errors_count) {
            if (expr->dostmt.errors[i] == child_expr)
                return ast_GetScope(expr, global_scope);
            i++;
        }
        i = 0;
        while (i < expr->dostmt.rescuestmt_count) {
            if (expr->dostmt.rescuestmt[i] == child_expr)
                return &expr->dostmt.rescuescope;
            i++;
        }
        i = 0;
        while (i < expr->dostmt.finallystmt_count) {
            if (expr->dostmt.finallystmt[i] == child_expr)
                return &expr->dostmt.finallyscope;
            i++;
        }
        return NULL;  // shouldn't be hit on the well-formed AST
    case H64EXPRTYPE_WITH_STMT:
        return &expr->withstmt.scope;
    default:
        break;
    }
    return ast_GetScope(expr, global_scope);
}

void ast_ClearFunctionArgsWithoutFunc(
        h64funcargs *fargs, h64scope *scope, int freeargs
        ) {
    int i = 0;
    while (i < fargs->arg_count) {
        if (fargs->arg_name[i]) {
            if (scope &&
                    // check scope wasn't already cleared:
                    scope->name_to_declaration_map != NULL
                    ) {
                scope_RemoveItem(
                    scope, fargs->arg_name[i]
                );
            }
            free(fargs->arg_name[i]);
        }
        if (fargs->arg_value[i] && freeargs)
            ast_FreeExpression(fargs->arg_value[i]);
        i++;
    }
    free(fargs->arg_name);
    fargs->arg_name = NULL;
    free(fargs->arg_value);
    fargs->arg_value = NULL;
    fargs->arg_count = 0;
}

void ast_ClearFunctionArgs(
        h64funcargs *fargs, h64expression *func, int freeargs
        ) {
    assert(!func ||
        func->type == H64EXPRTYPE_FUNCDEF_STMT ||
        func->type == H64EXPRTYPE_INLINEFUNCDEF ||
        func->type == H64EXPRTYPE_CALL ||
        func->type == H64EXPRTYPE_CALL_STMT);
    if (func && func->type != H64EXPRTYPE_CALL &&
                    func->type != H64EXPRTYPE_CALL_STMT) {
        return ast_ClearFunctionArgsWithoutFunc(
            fargs, &func->funcdef.scope, freeargs
        );
    }
    return ast_ClearFunctionArgsWithoutFunc(fargs, NULL, freeargs);
}

int ast_VisitExpression(
        h64expression *expr, h64expression *parent,
        int (*visit_in)(
            h64expression *expr, h64expression *parent, void *ud
        ),
        int (*visit_out)(
            h64expression *expr, h64expression *parent, void *ud
        ),
        int (*cancel_visit_descend_callback)(
            h64expression *expr, void *ud
        ),
        void *ud
        ) {
    if (!expr)
        return 1;

    if (visit_in) {
        if (!visit_in(expr, parent, ud))
            return 0;
        if (cancel_visit_descend_callback != NULL &&
                cancel_visit_descend_callback(expr, ud)) {
            if (!visit_out(expr, parent, ud))
                return 0;
            return 1;
        }
    }

    int i = 0;
    switch (expr->type) {
    case H64EXPRTYPE_INVALID:
    case H64EXPRTYPE_IDENTIFIERREF:
    case H64EXPRTYPE_CONTINUE_STMT:
    case H64EXPRTYPE_BREAK_STMT:
    case H64EXPRTYPE_LITERAL:
        break;
    case H64EXPRTYPE_VARDEF_STMT:
        if (expr->vardef.value)
            if (!ast_VisitExpression(
                    expr->vardef.value, expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
        break;
    case H64EXPRTYPE_FUNCDEF_STMT:
    case H64EXPRTYPE_INLINEFUNCDEF:
        i = 0;
        while (i < expr->funcdef.arguments.arg_count) {
            if (!ast_VisitExpression(
                    expr->funcdef.arguments.arg_value[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        i = 0;
        while (i < expr->funcdef.stmt_count) {
            if (!ast_VisitExpression(
                    expr->funcdef.stmt[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_CALL_STMT:
        if (expr->callstmt.call)
            if (!ast_VisitExpression(
                    expr->callstmt.call, expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
        break;
    case H64EXPRTYPE_CLASSDEF_STMT:
        if (!ast_VisitExpression(
                expr->classdef.baseclass_ref, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        i = 0;
        while (i < expr->classdef.vardef_count) {
            if (!ast_VisitExpression(
                    expr->classdef.vardef[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        i = 0;
        while (i < expr->classdef.funcdef_count) {
            if (!ast_VisitExpression(
                    expr->classdef.funcdef[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_IF_STMT: ;
        struct h64ifstmt *current_clause = &expr->ifstmt;
        while (current_clause) {
            struct h64ifstmt *next_clause = (
                current_clause->followup_clause
            );

            if (!ast_VisitExpression(
                    current_clause->conditional, expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i = 0;
            while (i < current_clause->stmt_count) {
                if (!ast_VisitExpression(
                        current_clause->stmt[i], expr, visit_in, visit_out,
                        cancel_visit_descend_callback, ud
                        ))
                    return 0;
                i++;
            }
            current_clause = next_clause;
        }
        break;
    case H64EXPRTYPE_WHILE_STMT:
        if (!ast_VisitExpression(
                expr->whilestmt.conditional, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        i = 0;
        while (i < expr->whilestmt.stmt_count) {
            if (!ast_VisitExpression(
                    expr->whilestmt.stmt[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_FOR_STMT:
        if (!ast_VisitExpression(
                expr->forstmt.iterated_container, expr,
                visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        i = 0;
        while (i < expr->forstmt.stmt_count) {
            if (!ast_VisitExpression(
                    expr->forstmt.stmt[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_IMPORT_STMT:
        break;
    case H64EXPRTYPE_RAISE_STMT:
        if (!ast_VisitExpression(
                expr->raisestmt.raised_expression, expr,
                visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        break;
    case H64EXPRTYPE_RETURN_STMT:
        if (!ast_VisitExpression(
                expr->returnstmt.returned_expression, expr,
                visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        break;
    case H64EXPRTYPE_DO_STMT:
        i = 0;
        while (i < expr->dostmt.dostmt_count) {
            if (!ast_VisitExpression(
                    expr->dostmt.dostmt[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        i = 0;
        while (i < expr->dostmt.errors_count) {
            if (!ast_VisitExpression(
                    expr->dostmt.errors[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        i = 0;
        while (i < expr->dostmt.rescuestmt_count) {
            if (!ast_VisitExpression(
                    expr->dostmt.rescuestmt[i], expr, visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        i = 0;
        while (i < expr->dostmt.finallystmt_count) {
            if (!ast_VisitExpression(
                    expr->dostmt.finallystmt[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_WITH_STMT:
        i = 0;
        while (i < expr->withstmt.withclause_count) {
            if (!ast_VisitExpression(
                    expr->withstmt.withclause[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        i = 0;
        while (i < expr->withstmt.stmt_count) {
            if (!ast_VisitExpression(
                    expr->withstmt.stmt[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_AWAIT_STMT:
        if (!ast_VisitExpression(
                expr->awaitstmt.awaitedvalue, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        break;
    case H64EXPRTYPE_ASSIGN_STMT:
        if (!ast_VisitExpression(
                expr->assignstmt.lvalue, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        if (!ast_VisitExpression(
                expr->assignstmt.rvalue, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        break;
    case H64EXPRTYPE_BINARYOP:
        if (!ast_VisitExpression(
                expr->op.value1, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        if (!ast_VisitExpression(
                expr->op.value2, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        break;
    case H64EXPRTYPE_UNARYOP:
        if (!ast_VisitExpression(
                expr->op.value1, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        break;
    case H64EXPRTYPE_CALL:
        if (!ast_VisitExpression(
                expr->inlinecall.value, expr, visit_in, visit_out,
                cancel_visit_descend_callback, ud
                ))
            return 0;
        i = 0;
        while (i < expr->inlinecall.arguments.arg_count) {
            if (!ast_VisitExpression(
                    expr->inlinecall.arguments.arg_value[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_LIST:
        i = 0;
        while (i < expr->constructorlist.entry_count) {
            if (!ast_VisitExpression(
                    expr->constructorlist.entry[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_SET:
        i = 0;
        while (i < expr->constructorset.entry_count) {
            if (!ast_VisitExpression(
                    expr->constructorset.entry[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_MAP:
        i = 0;
        while (i < expr->constructormap.entry_count) {
            if (!ast_VisitExpression(
                    expr->constructormap.key[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            if (!ast_VisitExpression(
                    expr->constructormap.value[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_VECTOR:
        i = 0;
        while (i < expr->constructorvector.entry_count) {
            if (!ast_VisitExpression(
                    expr->constructorvector.entry[i], expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
            i++;
        }
        break;
    case H64EXPRTYPE_WITH_CLAUSE:
        if (expr->withclause.withitem_value) {
            if (!ast_VisitExpression(
                    expr->withclause.withitem_value, expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
        }
        break;
    case H64EXPRTYPE_GIVEN:
        if (expr->given.condition) {
            if (!ast_VisitExpression(
                    expr->given.condition, expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
        }
        if (expr->given.valueyes) {
            if (!ast_VisitExpression(
                    expr->given.valueyes, expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
        }
        if (expr->given.valueno) {
            if (!ast_VisitExpression(
                    expr->given.valueno, expr,
                    visit_in, visit_out,
                    cancel_visit_descend_callback, ud
                    ))
                return 0;
        }
        break;
    default:
        h64fprintf(stderr, "horsec: warning: internal issue, "
            "unhandled expression in ast_VisitExpression(): "
            "type=%d, LIKELY BREAKAGE AHEAD.\n", expr->type);
    }

    if (visit_out) {
        if (!visit_out(expr, parent, ud))
            return 0;
    }

    return 1;
}

static int _mark_destroyed_cb(
        h64expression *expr,
        ATTR_UNUSED h64expression *parent,
        ATTR_UNUSED void *ud
        ) {
    expr->destroyed = 1;
    ast_FreeExprNonpoolMembers(expr);
    return 1;
}

void ast_MarkExprDestroyed(h64expression *expr) {
    if (!expr)
        return;
    int result = ast_VisitExpression(
        expr, NULL, NULL, &_mark_destroyed_cb, NULL, NULL
    );
    assert(result != 0);
    expr->destroyed = 1;
}

void ast_FreeExprNonpoolMembers(
        h64expression *expr
        ) {
    // Free resources not covered by pool allocator:
    if (expr->knownvalue.type == KNOWNVALUETYPE_KNOWNSTR) {
        free(expr->knownvalue.knownstr);
        expr->knownvalue.knownstr = NULL;
    }
    switch (expr->type) {
    case H64EXPRTYPE_INVALID: {
        // nothing to do
        break;
    }
    case H64EXPRTYPE_VARDEF_STMT: {
        free(expr->vardef.identifier);
        expr->vardef.identifier = NULL;
        break;
    }
    case H64EXPRTYPE_FUNCDEF_STMT:
    case H64EXPRTYPE_INLINEFUNCDEF: {
        scope_FreeData(&expr->funcdef.scope);
        free(expr->funcdef.name);
        expr->funcdef.name = NULL;
        ast_ClearFunctionArgs(&expr->funcdef.arguments, expr, 0);
        free(expr->funcdef.stmt);
        expr->funcdef.stmt = NULL;
        expr->funcdef.stmt_count = 0;
        varstorage_FreeExtraInfo(expr->funcdef._storageinfo);
        expr->funcdef._storageinfo = NULL;
        break;
    }
    case H64EXPRTYPE_CALL_STMT: {
        break;
    }
    case H64EXPRTYPE_CLASSDEF_STMT: {
        scope_FreeData(&expr->classdef.scope);
        free(expr->classdef.name);
        expr->classdef.name = NULL;
        free(expr->classdef.vardef);
        expr->classdef.vardef = NULL;
        expr->classdef.vardef_count = 0;
        free(expr->classdef.funcdef);
        expr->classdef.funcdef = NULL;
        expr->classdef.funcdef_count = 0;
        break;
    }
    case H64EXPRTYPE_IF_STMT: {
        struct h64ifstmt *current_clause = &expr->ifstmt;
        int isfirst = 1;
        while (current_clause) {
            struct h64ifstmt *next_clause = (
                current_clause->followup_clause
            );
            scope_FreeData(&current_clause->scope);
            free(current_clause->stmt);
            current_clause->stmt = NULL;
            current_clause->followup_clause = NULL;
            current_clause->stmt_count = 0;
            if (isfirst) {
                isfirst = 0;
            } else {
                free(current_clause);
            }
            current_clause = next_clause;
        }
        break;
    }
    case H64EXPRTYPE_WHILE_STMT: {
        scope_FreeData(&expr->whilestmt.scope);
        free(expr->whilestmt.stmt);
        expr->whilestmt.stmt = NULL;
        expr->whilestmt.stmt_count = 0;
        break;
    }
    case H64EXPRTYPE_FOR_STMT: {
        scope_FreeData(&expr->forstmt.scope);
        free(expr->forstmt.iterator_identifier);
        expr->forstmt.iterator_identifier = NULL;
        free(expr->forstmt.stmt);
        expr->forstmt.stmt = NULL;
        expr->forstmt.stmt_count = 0;
        break;
    }
    case H64EXPRTYPE_IMPORT_STMT: {
        int i = 0;
        while (i < expr->importstmt.import_elements_count) {
            free(expr->importstmt.import_elements[i]);
            i++;
        }
        free(expr->importstmt.import_elements);
        expr->importstmt.import_elements = NULL;
        expr->importstmt.import_elements_count = 0;
        free(expr->importstmt.import_as);
        expr->importstmt.import_as = NULL;
        free(expr->importstmt.source_library);
        expr->importstmt.source_library = NULL;
        break;
    }
    case H64EXPRTYPE_WITH_STMT: {
        free(expr->withstmt.withclause);
        expr->withstmt.withclause = NULL;
        free(expr->withstmt.stmt);
        expr->withstmt.stmt = NULL;
        break;
    }
    case H64EXPRTYPE_RAISE_STMT: {
        break;
    }
    case H64EXPRTYPE_RETURN_STMT: {
        break;
    }
    case H64EXPRTYPE_DO_STMT: {
        scope_FreeData(&expr->dostmt.doscope);
        free(expr->dostmt.dostmt);
        expr->dostmt.dostmt = NULL;
        expr->dostmt.dostmt_count = 0;
        free(expr->dostmt.errors);
        expr->dostmt.errors = NULL;
        expr->dostmt.errors_count = 0;
        free(expr->dostmt.error_name);
        expr->dostmt.error_name = NULL;
        scope_FreeData(&expr->dostmt.rescuescope);
        free(expr->dostmt.rescuestmt);
        expr->dostmt.rescuestmt = NULL;
        expr->dostmt.rescuestmt_count = 0;
        scope_FreeData(&expr->dostmt.finallyscope);
        free(expr->dostmt.finallystmt);
        expr->dostmt.finallystmt = NULL;
        expr->dostmt.finallystmt_count = 0;
        break;
    }
    case H64EXPRTYPE_AWAIT_STMT: {
        break;
    };
    case H64EXPRTYPE_ASSIGN_STMT: {
        break;
    }
    case H64EXPRTYPE_BREAK_STMT: {
        break;
    }
    case H64EXPRTYPE_CONTINUE_STMT: {
        break;
    }
    case H64EXPRTYPE_LITERAL: {
        if (expr->literal.type == H64TK_CONSTANT_STRING) {
            free(expr->literal.str_value);
            expr->literal.str_value = NULL;
        } else if (expr->literal.type == H64TK_CONSTANT_BYTES) {
            free(expr->literal.str_value);
            expr->literal.str_value = NULL;
        }
        break;
    }
    case H64EXPRTYPE_IDENTIFIERREF: {
        free(expr->identifierref.value);
        expr->identifierref.value = NULL;
        break;
    }
    case H64EXPRTYPE_BINARYOP: {
        break;
    }
    case H64EXPRTYPE_UNARYOP: {
        break;
    }
    case H64EXPRTYPE_CALL: {
        ast_ClearFunctionArgs(
            &expr->inlinecall.arguments, expr, 0
        );
        break;
    }
    case H64EXPRTYPE_LIST: {
        free(expr->constructorlist.entry);
        expr->constructorlist.entry = NULL;
        expr->constructorlist.entry_count = 0;
        break;
    }
    case H64EXPRTYPE_SET: {
        free(expr->constructorset.entry);
        expr->constructorset.entry_count = 0;
        break;
    }
    case H64EXPRTYPE_MAP: {
        free(expr->constructormap.key);
        expr->constructormap.key = NULL;
        free(expr->constructormap.value);
        expr->constructormap.value = NULL;
        expr->constructormap.entry_count = 0;
        break;
    }
    case H64EXPRTYPE_VECTOR: {
        free(expr->constructorvector.entry);
        expr->constructorvector.entry = NULL;
        expr->constructorvector.entry_count = 0;
        break;
    }
    case H64EXPRTYPE_WITH_CLAUSE: {
        free(expr->withclause.withitem_identifier);
        expr->withclause.withitem_identifier = NULL;
        break;
    }
    case H64EXPRTYPE_GIVEN: {
        break;
    }
    default: {
        h64fprintf(stderr, "horsec: warning: internal issue, "
            "unhandled expression in "
            "ast_FreeExprNonpoolMembers(): "
            "type=%d, LIKELY MEMORY LEAK.\n", expr->type
        );
    }
    }
}

struct _collectfreelist {
    int free_expr_count;
    h64expression **free_expr;
    int free_expr_alloc, free_expr_onstack;
};

static int _collect_free_expr_cb(
        h64expression *expr,
        ATTR_UNUSED h64expression *parent, void *ud
        ) {
    struct _collectfreelist *list = (
        (struct _collectfreelist *)ud
    );
    if (list->free_expr_count + 1 > list->free_expr_alloc) {
        h64expression **new_list = NULL;;
        if (!list->free_expr_onstack) {
            new_list = realloc(
                list->free_expr, sizeof(*new_list) *
                (list->free_expr_alloc * 2)
            );
        } else {
            new_list = malloc(
                sizeof(*new_list) *
                (list->free_expr_alloc * 2)
            );
        }
        if (!new_list) {
            h64fprintf(stderr,
                "horsec: warning: oom collecting expr children free list, "
                "will likely leak..."
            );
            return 1;
        }
        list->free_expr = new_list;
        list->free_expr_alloc *= 2;
        list->free_expr_onstack = 0;
    }
    assert(list->free_expr_count + 1 <= list->free_expr_alloc);
    list->free_expr[
        list->free_expr_count
    ] = expr;
    list->free_expr_count++;
    return 1;
}

void ast_FreeExpression(h64expression *expr) {
    if (!expr)
        return;

    char buflist[512 * sizeof(void*)];  // around 4KB
    struct _collectfreelist list = {0};
    list.free_expr = (h64expression **)buflist;
    list.free_expr_onstack = 1;
    list.free_expr_alloc = 512;
    int result = ast_VisitExpression(
        expr, NULL, NULL, &_collect_free_expr_cb, NULL, &list
    );
    ast_MarkExprDestroyed(expr);
    assert(result != 0);
    int i = 0;
    while (i < list.free_expr_count) {
        free(list.free_expr[i]);
        i++;
    }
    if (!list.free_expr_onstack)
        free(list.free_expr);
}

static char _h64exprname_invalid[] = "H64EXPRTYPE_INVALID";
static char _h64exprname_vardef_stmt[] = "H64EXPRTYPE_VARDEF_STMT";
static char _h64exprname_funcdef_stmt[] = "H64EXPRTYPE_FUNCDEF_STMT";
static char _h64exprname_call_stmt[] = "H64EXPRTYPE_CALL_STMT";
static char _h64exprname_classdef_stmt[] = "H64EXPRTYPE_CLASSDEF_STMT";
static char _h64exprname_if_stmt[] = "H64EXPRTYPE_IF_STMT";
static char _h64exprname_while_stmt[] = "H64EXPRTYPE_WHILE_STMT";
static char _h64exprname_for_stmt[] = "H64EXPRTYPE_FOR_STMT";
static char _h64exprname_import_stmt[] = "H64EXPRTYPE_IMPORT_STMT";
static char _h64exprname_raise_stmt[] = "H64EXPRTYPE_RAISE_STMT";
static char _h64exprname_return_stmt[] = "H64EXPRTYPE_RETURN_STMT";
static char _h64exprname_do_stmt[] = "H64EXPRTYPE_DO_STMT";
static char _h64exprname_with_stmt[] = "H64EXPRTYPE_WITH_STMT";
static char _h64exprname_break_stmt[] = "H64EXPRTYPE_BREAK_STMT";
static char _h64exprname_continue_stmt[] = "H64EXPRTYPE_CONTINUE_STMT";
static char _h64exprname_await_stmt[] = "H64EXPRTYPE_AWAIT_STMT";
static char _h64exprname_assign_stmt[] = "H64EXPRTYPE_ASSIGN_STMT";
static char _h64exprname_literal[] = "H64EXPRTYPE_LITERAL";
static char _h64exprname_identifierref[] = "H64EXPRTYPE_IDENTIFIERREF";
static char _h64exprname_inlinefuncdef[] = "H64EXPRTYPE_INLINEFUNCDEF";
static char _h64exprname_unaryop[] = "H64EXPRTYPE_UNARYOP";
static char _h64exprname_binaryop[] = "H64EXPRTYPE_BINARYOP";
static char _h64exprname_call[] = "H64EXPRTYPE_CALL";
static char _h64exprname_list[] = "H64EXPRTYPE_LIST";
static char _h64exprname_set[] = "H64EXPRTYPE_SET";
static char _h64exprname_map[] = "H64EXPRTYPE_MAP";
static char _h64exprname_vector[] = "H64EXPRTYPE_VECTOR";
static char _h64exprname_with_clause[] = "H64EXPRTYPE_WITH_CLAUSE";
static char _h64exprname_given[] = "H64EXPRTYPE_GIVEN";

const char *ast_ExpressionTypeToStr(h64expressiontype type) {
    if (type == H64EXPRTYPE_INVALID || type <= 0)
        return _h64exprname_invalid;
    switch (type) {
    case H64EXPRTYPE_VARDEF_STMT:
        return _h64exprname_vardef_stmt;
    case H64EXPRTYPE_FUNCDEF_STMT:
        return _h64exprname_funcdef_stmt;
    case H64EXPRTYPE_CALL_STMT:
        return _h64exprname_call_stmt;
    case H64EXPRTYPE_CLASSDEF_STMT:
        return _h64exprname_classdef_stmt;
    case H64EXPRTYPE_IF_STMT:
        return _h64exprname_if_stmt;
    case H64EXPRTYPE_WHILE_STMT:
        return _h64exprname_while_stmt;
    case H64EXPRTYPE_FOR_STMT:
        return _h64exprname_for_stmt;
    case H64EXPRTYPE_IMPORT_STMT:
        return _h64exprname_import_stmt;
    case H64EXPRTYPE_RAISE_STMT:
        return _h64exprname_raise_stmt;
    case H64EXPRTYPE_RETURN_STMT:
        return _h64exprname_return_stmt;
    case H64EXPRTYPE_DO_STMT:
        return _h64exprname_do_stmt;
    case H64EXPRTYPE_WITH_STMT:
        return _h64exprname_with_stmt;
    case H64EXPRTYPE_BREAK_STMT:
        return _h64exprname_break_stmt;
    case H64EXPRTYPE_CONTINUE_STMT:
        return _h64exprname_continue_stmt;
    case H64EXPRTYPE_AWAIT_STMT:
        return _h64exprname_await_stmt;
    case H64EXPRTYPE_ASSIGN_STMT:
        return _h64exprname_assign_stmt;
    case H64EXPRTYPE_LITERAL:
        return _h64exprname_literal;
    case H64EXPRTYPE_IDENTIFIERREF:
        return _h64exprname_identifierref;
    case H64EXPRTYPE_INLINEFUNCDEF:
        return _h64exprname_inlinefuncdef;
    case H64EXPRTYPE_UNARYOP:
        return _h64exprname_unaryop;
    case H64EXPRTYPE_BINARYOP:
        return _h64exprname_binaryop;
    case H64EXPRTYPE_CALL:
        return _h64exprname_call;
    case H64EXPRTYPE_LIST:
        return _h64exprname_list;
    case H64EXPRTYPE_SET:
        return _h64exprname_set;
    case H64EXPRTYPE_MAP:
        return _h64exprname_map;
    case H64EXPRTYPE_VECTOR:
        return _h64exprname_vector;
    case H64EXPRTYPE_WITH_CLAUSE:
        return _h64exprname_with_clause;
    case H64EXPRTYPE_GIVEN:
        return _h64exprname_given;
    default:
        return NULL;
    }
    return NULL;
}

char *ast_ExpressionToJSONStr(
        h64expression *e, const h64wchar *fileuri,
        int64_t fileurilen
        ) {
    jsonvalue *v = ast_ExpressionToJSON(e, fileuri, fileurilen);
    if (!v)
        return NULL;
    char *s = json_Dump(v);
    json_Free(v);
    return s;
}

jsonvalue *ast_FuncArgsToJSON(
        h64funcargs *fargs, const h64wchar *fileuri,
        int64_t fileurilen
        ) {
    jsonvalue *v = json_List();
    int fail = 0;
    int i = 0;
    while (i < fargs->arg_count) {
        jsonvalue *arg = json_Dict();
        if (!arg) {
            fail = 1;
            break;
        }
        if (fargs->arg_name &&
                fargs->arg_name[i] && strlen(fargs->arg_name[i]) > 0) {
            if (!json_SetDictStr(arg, "name", fargs->arg_name[i])) {
                fail = 1;
                json_Free(arg);
                break;
            }
        } else {
            if (!json_SetDictNull(arg, "name")) {
                fail = 1;
                json_Free(arg);
                break;
            }
        }
        if (fargs->arg_value && fargs->arg_value[i]) {
            jsonvalue *innerv = ast_ExpressionToJSON(
                fargs->arg_value[i], fileuri, fileurilen
            );
            if (!innerv) {
                json_Free(arg);
                fail = 1;
                break;
            } else if (!json_SetDict(arg, "value", innerv)) {
                fail = 1;
                json_Free(innerv);
                json_Free(arg);
                break;
            }
        } else {
            if (!json_SetDictNull(arg, "value")) {
                fail = 1;
                json_Free(arg);
                break;
            }
        }
        if (!json_AddToList(v, arg)) {
            fail = 1;
            json_Free(arg);
            break;
        }
        i++;
    }
    if (fail) {
        json_Free(v);
        return NULL;
    }
    return v;
}

jsonvalue *ast_ExpressionToJSON(
        h64expression *e, const h64wchar *fileuri,
        int64_t fileurilen
        ) {
    if (!e)
        return NULL;
    int fail = 0;
    jsonvalue *v = json_Dict();
    if (!v)
        return NULL;
    const char *typestrconst = ast_ExpressionTypeToStr(e->type);
    char *typestr = typestrconst ? strdup(typestrconst) : NULL;
    if (typestr) {
        if (!json_SetDictStr(v, "type", typestr)) {
            fail = 1;
        }
        free(typestr);
    } else {
        h64fprintf(stderr, "horsec: error: internal error, "
            "fail of handling expression type %d in "
            "ast_ExpressionTypeToStr\n",
            e->type);
        fail = 1;
    }
    if (e->tokenindex >= 0) {
        if (!json_SetDictInt(v, "tokenindex", e->tokenindex))
            fail = 1;
    }
    if (e->line >= 0) {
        if (!json_SetDictInt(v, "line", e->line)) {
            fail = 1;
        } else if (e->column >= 0) {
            if (!json_SetDictInt(v, "column", e->column)) {
                fail = 1;
            }
        }
    }
    if (e->type == H64EXPRTYPE_VARDEF_STMT) {
        if (e->vardef.identifier &&
                !json_SetDictStr(v, "name", e->vardef.identifier))
            fail = 1;
        jsonvalue *storagejson = varstorage_StorageAsJSON(e);
        json_SetDict(v, "storage", storagejson);
        jsonvalue *attributes = json_List();
        if (e->vardef.is_deprecated) {
            if (!json_AddToListStr(attributes, "deprecated"))
                fail = 1;
        }
        if (e->vardef.is_const) {
            if (!json_AddToListStr(attributes, "const"))
                fail = 1;
        }
        if (e->vardef.is_protected) {
            if (!json_AddToListStr(attributes, "protect"))
                fail = 1;
        }
        if (!json_SetDict(v, "attributes", attributes)) {
            fail = 1;
            json_Free(attributes);
        }
        if (e->vardef.value) {
            jsonvalue *val = ast_ExpressionToJSON(
                e->vardef.value, fileuri, fileurilen
            );
            if (!val) {
                fail = 1;
            } else {
                if (!json_SetDict(v, "value", val)) {
                    json_Free(val);
                    fail = 1;
                }
            }
        }
    } else if (e->type == H64EXPRTYPE_IF_STMT) {
        struct h64ifstmt *current_clause = &e->ifstmt;

        int i = -1;;

        while (current_clause) {
            i++;
            jsonvalue *scopeval = scope_ScopeToJSON(&current_clause->scope);
            char name_scope[64];
            char name_statements[64];
            char name_condition[64] = {0};
            if (i <= 0) {
                memcpy(name_scope, "if-scope", strlen("if-scope") + 1);
                memcpy(name_statements, "if-statements",
                       strlen("if-statements") + 1);
                memcpy(name_condition, "if-conditional",
                       strlen("if-statements") + 1);
            } else {
                if (current_clause->conditional) {
                    snprintf(name_scope, sizeof(name_scope),
                             "elseif-%d-scope", i);
                    snprintf(name_statements, sizeof(name_statements),
                             "elseif-%d-statements", i);
                    snprintf(name_condition, sizeof(name_condition),
                             "elseif-%d-conditional", i);
                } else {
                    memcpy(name_scope, "else-scope", strlen("else-scope") + 1);
                    memcpy(name_statements, "else-statements",
                           strlen("else-statements") + 1);
                }
            }
            if (current_clause->conditional) {
                jsonvalue *conditionval = ast_ExpressionToJSON(
                    current_clause->conditional, fileuri, fileurilen
                );
                if (!json_SetDict(v, name_condition, conditionval)) {
                    json_Free(conditionval);
                    fail = 1;
                }
            }
            if (!json_SetDict(v, name_scope, scopeval)) {
                fail = 1;
                json_Free(scopeval);
            }
            jsonvalue *cblock = json_List();
            int i = 0;
            while (i < current_clause->stmt_count) {
                jsonvalue *stmtjson = ast_ExpressionToJSON(
                    current_clause->stmt[i], fileuri, fileurilen
                );
                if (!json_AddToList(cblock, stmtjson)) {
                    json_Free(stmtjson);
                    fail = 1;
                    break;
                }
                i++;
            }
            if (!json_SetDict(v, name_statements, cblock)) {
                json_Free(cblock);
                fail = 1;
            }

            current_clause = current_clause->followup_clause;
        }
    } else if (e->type == H64EXPRTYPE_GIVEN) {
        jsonvalue *conditionval = ast_ExpressionToJSON(
            e->given.condition, fileuri, fileurilen
        );
        if (!json_SetDict(v, "condition", conditionval)) {
            json_Free(conditionval);
            fail = 1;
        }
        jsonvalue *valueyes = ast_ExpressionToJSON(
            e->given.valueyes, fileuri, fileurilen
        );
        if (!json_SetDict(v, "if-yes-value", valueyes)) {
            json_Free(conditionval);
            fail = 1;
        }
        jsonvalue *valueno = ast_ExpressionToJSON(
            e->given.valueno, fileuri, fileurilen
        );
        if (!json_SetDict(v, "if-no-value", valueno)) {
            json_Free(conditionval);
            fail = 1;
        }
    } else if (e->type == H64EXPRTYPE_WHILE_STMT) {
        jsonvalue *scopeval = scope_ScopeToJSON(&e->whilestmt.scope);
        if (!json_SetDict(v, "scope", scopeval)) {
            fail = 1;
            json_Free(scopeval);
        }
        jsonvalue *cblock = json_List();
        int i = 0;
        while (i < e->whilestmt.stmt_count) {
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->whilestmt.stmt[i], fileuri, fileurilen
            );
            if (!json_AddToList(cblock, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "statements", cblock)) {
            json_Free(cblock);
            fail = 1;
        }
        jsonvalue *conditionval = ast_ExpressionToJSON(
            e->whilestmt.conditional, fileuri, fileurilen
        );
        if (!json_SetDict(v, "condition", conditionval)) {
            json_Free(conditionval);
            fail = 1;
        }
    } else if (e->type == H64EXPRTYPE_FOR_STMT) {
        if (!json_SetDictStr(
                v, "iterator-identifier",
                e->forstmt.iterator_identifier)) {
            fail = 1;
        }
        jsonvalue *iterate_container = ast_ExpressionToJSON(
            e->forstmt.iterated_container, fileuri, fileurilen
        );
        if (!json_SetDict(v, "iterated-container", iterate_container)) {
            json_Free(iterate_container);
            fail = 1;
        }
        jsonvalue *scopeval = scope_ScopeToJSON(&e->forstmt.scope);
        if (!json_SetDict(v, "scope", scopeval)) {
            fail = 1;
            json_Free(scopeval);
        }
        jsonvalue *cblock = json_List();
        int i = 0;
        while (i < e->forstmt.stmt_count) {
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->forstmt.stmt[i], fileuri, fileurilen
            );
            if (!json_AddToList(cblock, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "statements", cblock)) {
            json_Free(cblock);
            fail = 1;
        }
    } else if (e->type == H64EXPRTYPE_DO_STMT) {
        jsonvalue *dosection = json_List();
        int i = 0;
        while (i < e->dostmt.dostmt_count) {
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->dostmt.dostmt[i], fileuri, fileurilen
            );
            if (!json_AddToList(dosection, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "do-statements", dosection)) {
            json_Free(dosection);
            fail = 1;
        }
        jsonvalue *doscopeval = scope_ScopeToJSON(&e->dostmt.doscope);
        if (!json_SetDict(v, "do-scope", doscopeval)) {
            fail = 1;
            json_Free(doscopeval);
        }
        jsonvalue *rescuetypes = json_List();
        i = 0;
        while (i < e->dostmt.errors_count) {
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->dostmt.errors[i], fileuri, fileurilen
            );
            if (!json_AddToList(rescuetypes, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "errors", rescuetypes)) {
            json_Free(rescuetypes);
            fail = 1;
        }
        if (e->dostmt.error_name) {
            if (!json_SetDictStr(
                    v, "error-name", e->dostmt.error_name))
                fail = 1;
        } else {
            if (!json_SetDictNull(v, "error-name"))
                fail = 1;
        }
        jsonvalue *rescuesection = json_List();
        i = 0;
        while (i < e->dostmt.rescuestmt_count) {
            assert(e->dostmt.rescuestmt[i] != NULL);
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->dostmt.rescuestmt[i], fileuri, fileurilen
            );
            if (!json_AddToList(rescuesection, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "rescue-statements", rescuesection)) {
            json_Free(rescuesection);
            fail = 1;
        }
        jsonvalue *rescuescopeval = scope_ScopeToJSON(
            &e->dostmt.rescuescope
        );
        if (!json_SetDict(v, "rescue-scope", rescuescopeval)) {
            fail = 1;
            json_Free(rescuescopeval);
        }
        jsonvalue *finallysection = json_List();
        i = 0;
        while (i < e->dostmt.finallystmt_count) {
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->dostmt.finallystmt[i], fileuri, fileurilen
            );
            if (!json_AddToList(finallysection, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        jsonvalue *finallyscopeval = scope_ScopeToJSON(
            &e->dostmt.finallyscope
        );
        if (!json_SetDict(v, "finally-statements", finallysection)) {
            json_Free(finallysection);
            fail = 1;
        }
        if (!json_SetDict(v, "finally-scope", finallyscopeval)) {
            fail = 1;
            json_Free(finallyscopeval);
        }
    } else if (e->type == H64EXPRTYPE_CLASSDEF_STMT) {
        jsonvalue *scopeval = scope_ScopeToJSON(&e->classdef.scope);
        if (!json_SetDict(v, "scope", scopeval)) {
            fail = 1;
            json_Free(scopeval);
        }
        if (e->classdef.name &&
                !json_SetDictStr(v, "name", e->classdef.name))
            fail = 1;
        jsonvalue *storagejson = varstorage_StorageAsJSON(e);
        json_SetDict(v, "storage", storagejson);
        jsonvalue *attributes = json_List();
        if (e->classdef.is_noparallel) {
            if (!json_AddToListStr(attributes, "noparallel"))
                fail = 1;
        }
        if (e->classdef.is_deprecated) {
            if (!json_AddToListStr(attributes, "deprecated"))
                fail = 1;
        }
        if (!json_SetDict(v, "attributes", attributes)) {
            fail = 1;
            json_Free(attributes);
        }
        jsonvalue *vardefs = json_List();
        int i = 0;
        while (i < e->classdef.vardef_count) {
            jsonvalue *varjson = ast_ExpressionToJSON(
                e->classdef.vardef[i], fileuri, fileurilen
            );
            if (!json_AddToList(vardefs, varjson)) {
                json_Free(varjson);
                fail = 1;
                break;
            }
            i++;
        }
        jsonvalue *funcdefs = json_List();
        i = 0;
        while (i < e->classdef.funcdef_count) {
            jsonvalue *funcjson = ast_ExpressionToJSON(
                e->classdef.funcdef[i], fileuri, fileurilen
            );
            if (!json_AddToList(funcdefs, funcjson)) {
                json_Free(funcjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "variables", vardefs)) {
            fail = 1;
            json_Free(vardefs);
        }
        if (!json_SetDict(v, "functions", funcdefs)) {
            fail = 1;
            json_Free(funcdefs);
        }
    } else if (e->type == H64EXPRTYPE_MAP) {
        jsonvalue *keys = json_List();
        jsonvalue *values = json_List();
        int i = 0;
        while (i < e->constructormap.entry_count) {
            jsonvalue *exprjson = ast_ExpressionToJSON(
                e->constructormap.key[i], fileuri, fileurilen
            );
            if (!json_AddToList(keys, exprjson)) {
                json_Free(exprjson);
                fail = 1;
                break;
            }
            exprjson = ast_ExpressionToJSON(
                e->constructormap.value[i], fileuri, fileurilen
            );
            if (!json_AddToList(values, exprjson)) {
                json_Free(exprjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "keys", keys)) {
            fail = 1;
            json_Free(keys);
        }
        if (!json_SetDict(v, "values", values)) {
            fail = 1;
            json_Free(values);
        }
    } else if (e->type == H64EXPRTYPE_AWAIT_STMT) {
        jsonvalue *awaitedjson = ast_ExpressionToJSON(
            e->awaitstmt.awaitedvalue, fileuri, fileurilen
        );
        if (!json_SetDict(v, "awaited", awaitedjson)) {
            fail = 1;
            json_Free(awaitedjson);
        }
    } else if (e->type == H64EXPRTYPE_ASSIGN_STMT) {
        jsonvalue *lvaluejson = ast_ExpressionToJSON(
            e->assignstmt.lvalue, fileuri, fileurilen
        );
        jsonvalue *rvaluejson = ast_ExpressionToJSON(
            e->assignstmt.rvalue, fileuri, fileurilen
        );
        if (!json_SetDict(v, "lvalue", lvaluejson)) {
            fail = 1;
            json_Free(lvaluejson);
        }
        if (!json_SetDict(v, "rvalue", rvaluejson)) {
            fail = 1;
            json_Free(rvaluejson);
        }
    } else if (e->type == H64EXPRTYPE_LIST) {
        jsonvalue *contents = json_List();
        int i = 0;
        while (i < e->constructorlist.entry_count) {
            jsonvalue *exprjson = ast_ExpressionToJSON(
                e->constructorlist.entry[i], fileuri, fileurilen
            );
            if (!json_AddToList(contents, exprjson)) {
                json_Free(exprjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "contents", contents)) {
            fail = 1;
            json_Free(contents);
        }
    } else if (e->type == H64EXPRTYPE_VECTOR) {
        jsonvalue *contents = json_List();
        int i = 0;
        while (i < e->constructorvector.entry_count) {
            jsonvalue *exprjson = ast_ExpressionToJSON(
                e->constructorvector.entry[i], fileuri, fileurilen
            );
            if (!json_AddToList(contents, exprjson)) {
                json_Free(exprjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "contents", contents)) {
            fail = 1;
            json_Free(contents);
        }
    } else if (e->type == H64EXPRTYPE_SET) {
        jsonvalue *contents = json_List();
        int i = 0;
        while (i < e->constructorset.entry_count) {
            jsonvalue *exprjson = ast_ExpressionToJSON(
                e->constructorset.entry[i], fileuri, fileurilen
            );
            if (!json_AddToList(contents, exprjson)) {
                json_Free(exprjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "contents", contents)) {
            fail = 1;
            json_Free(contents);
        }
    } else if (e->type == H64EXPRTYPE_FUNCDEF_STMT ||
            e->type == H64EXPRTYPE_INLINEFUNCDEF) {
        if (e->funcdef.name && e->type != H64EXPRTYPE_INLINEFUNCDEF &&
                !json_SetDictStr(v, "name", e->funcdef.name))
            fail = 1;
        if (e->funcdef.bytecode_func_id >= 0) {
            if (!json_SetDictInt(v, "bytecode-func-id",
                    e->funcdef.bytecode_func_id))
                fail = 1;
        }
        jsonvalue *storagejson = varstorage_StorageAsJSON(e);
        json_SetDict(v, "storage", storagejson);
        jsonvalue *attributes = json_List();
        if (e->funcdef.is_parallel) {
            if (!json_AddToListStr(attributes, "parallel"))
                fail = 1;
        }
        if (e->funcdef.is_noparallel) {
            if (!json_AddToListStr(attributes, "noparallel"))
                fail = 1;
        }
        if (e->funcdef.is_deprecated) {
            if (!json_AddToListStr(attributes, "deprecated"))
                fail = 1;
        }
        if (!json_SetDict(v, "attributes", attributes)) {
            fail = 1;
            json_Free(attributes);
        }
        jsonvalue *value2 = ast_FuncArgsToJSON(
            &e->funcdef.arguments, fileuri, fileurilen
        );
        if (!value2) {
            fail = 1;
        } else {
            if (!json_SetDict(v, "arguments", value2)) {
                json_Free(value2);
                fail = 1;
            }
        }
        jsonvalue *scopeval = scope_ScopeToJSON(&e->funcdef.scope);
        if (!json_SetDict(v, "scope", scopeval)) {
            fail = 1;
            json_Free(scopeval);
        }
        jsonvalue *statements = json_List();
        int i = 0;
        while (i < e->funcdef.stmt_count) {
            jsonvalue *stmtjson = ast_ExpressionToJSON(
                e->funcdef.stmt[i], fileuri, fileurilen
            );
            if (!json_AddToList(statements, stmtjson)) {
                json_Free(stmtjson);
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "statements", statements)) {
            fail = 1;
            json_Free(statements);
        }
    } else if (e->type == H64EXPRTYPE_CONTINUE_STMT ||
            e->type == H64EXPRTYPE_BREAK_STMT) {
        // No extra info to set.
    } else if (e->type == H64EXPRTYPE_LITERAL) {
        if (e->literal.type == H64TK_CONSTANT_INT) {
            if (!json_SetDictInt(v, "value", e->literal.int_value))
                fail = 1;
        } else if (e->literal.type == H64TK_CONSTANT_FLOAT) {
            if (!json_SetDictFloat(v, "value", e->literal.float_value))
                fail = 1;
        } else if (e->literal.type == H64TK_CONSTANT_BOOL) {
            if (!json_SetDictBool(v, "value", (int)e->literal.int_value))
                fail = 1;
        } else if (e->literal.type == H64TK_CONSTANT_NONE) {
            if (!json_SetDictNull(v, "value"))
                fail = 1;
        } else if (e->literal.type == H64TK_CONSTANT_STRING) {
            if (!json_SetDictStr(v, "value", e->literal.str_value))
                fail = 1;
        } else if (e->literal.type == H64TK_CONSTANT_BYTES) {
            if (!json_SetDictStr(v, "value", e->literal.str_value))
                fail = 1;
        }
    } else if (e->type == H64EXPRTYPE_IMPORT_STMT) {
        jsonvalue *list = json_List();
        int i = 0;
        while (i < e->importstmt.import_elements_count) {
            if (!json_AddToListStr(list,
                    e->importstmt.import_elements[i])) {
                fail = 1;
                break;
            }
            i++;
        }
        if (!json_SetDict(v, "import_path", list)) {
            json_Free(list);
            fail = 1;
        }
        if (e->importstmt.source_library &&
                !json_SetDictStr(v, "source_library",
                    e->importstmt.source_library))
            fail = 1;
        else if (!e->importstmt.source_library &&
                !json_SetDictNull(v, "source_library"))
            fail = 1;
        if (e->importstmt.import_as &&
                !json_SetDictStr(v, "import_as",
                    e->importstmt.import_as))
            fail = 1;
    } else if (e->type == H64EXPRTYPE_RAISE_STMT) {
        if (e->raisestmt.raised_expression) {
            jsonvalue *value = ast_ExpressionToJSON(
                e->raisestmt.raised_expression, fileuri, fileurilen
            );
            if (!json_SetDict(v, "raised_value", value)) {
                json_Free(value);
                fail = 1;
            }
        } else {
            if (!json_SetDictNull(v, "raised_value"))
                fail = 1;
        }
    } else if (e->type == H64EXPRTYPE_RETURN_STMT) {
        if (e->returnstmt.returned_expression) {
            jsonvalue *value = ast_ExpressionToJSON(
                e->returnstmt.returned_expression, fileuri, fileurilen
            );
            if (!json_SetDict(v, "returned_value", value)) {
                json_Free(value);
                fail = 1;
            }
        } else {
            if (!json_SetDictNull(v, "returned_value"))
                fail = 1;
        }
    } else if (e->type == H64EXPRTYPE_BINARYOP) {
        jsonvalue *value1 = ast_ExpressionToJSON(
            e->op.value1, fileuri, fileurilen
        );
        if (!json_SetDictStr(v, "operator",
                operator_OpTypeToStr(e->op.optype)))
            fail = 1;
        if (!value1) {
            fail = 1;
        } else {
            if (!json_SetDict(v, "operand1", value1)) {
                json_Free(value1);
                fail = 1;
            }
        }
        jsonvalue *value2 = ast_ExpressionToJSON(
            e->op.value2, fileuri, fileurilen
        );
        if (!value2) {
            fail = 1;
        } else {
            if (!json_SetDict(v, "operand2", value2)) {
                json_Free(value2);
                fail = 1;
            }
        }
    } else if (e->type == H64EXPRTYPE_IDENTIFIERREF) {
        if (e->identifierref.value &&
                !json_SetDictStr(v, "value", e->identifierref.value))
            fail = 1;
    } else if (e->type == H64EXPRTYPE_UNARYOP) {
        if (!json_SetDictStr(v, "operator",
                operator_OpTypeToStr(e->op.optype)))
            fail = 1;
        jsonvalue *value1 = ast_ExpressionToJSON(
            e->op.value1, fileuri, fileurilen
        );
        if (!value1) {
            fail = 1;
        } else {
            if (!json_SetDict(v, "operand", value1)) {
                json_Free(value1);
                fail = 1;
            }
        }
    } else if (e->type == H64EXPRTYPE_CALL ||
            e->type == H64EXPRTYPE_CALL_STMT) {
        h64expression *innere = (
            e->type == H64EXPRTYPE_CALL ? e :
            e->callstmt.call
        );
        if (innere->inlinecall.value != NULL) {
            jsonvalue *value1 = ast_ExpressionToJSON(
                innere->inlinecall.value, fileuri, fileurilen
            );
            if (!value1) {
                fail = 1;
            } else {
                if (!json_SetDict(v, "callee", value1)) {
                    json_Free(value1);
                    fail = 1;
                }
            }
        }
        jsonvalue *value2 = ast_FuncArgsToJSON(
            &innere->inlinecall.arguments, fileuri, fileurilen
        );
        if (!value2) {
            fail = 1;
        } else {
            if (!json_SetDict(v, "arguments", value2)) {
                json_Free(value2);
                fail = 1;
            }
        }
        if (!json_SetDictBool(v, "is_async", innere->inlinecall.is_async)) {
            fail = 1;
        }
    }
    if (fileuri) {
        char *fileuri_u8 = AS_U8(fileuri, fileurilen);
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
