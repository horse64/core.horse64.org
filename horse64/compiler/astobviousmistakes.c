// Copyright (c) 2020, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

/// This module checks a few programmer mistakes that would not per se
/// prevent a program from compiling and running, but cause a runtime error
/// in a likely unintended way.
/// For example, it enforces that using "new" on a value identified by
/// horsec as clearly not a class at compile time is always wrapped by
/// an .is_a() check to make sure it was intentional.

#include "compileconfig.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "compiler/ast.h"
#include "compiler/asthelpers.h"
#include "compiler/astobviousmistakes.h"
#include "compiler/astparser.h"
#include "compiler/asttransform.h"
#include "compiler/compileproject.h"
#include "nonlocale.h"


int _resolver_IsPossiblyGuardedInvalidType(
        h64expression *expr
        ) {
    h64expression *child = NULL;
    while (expr) {
        if (child != NULL &&
                expr->type == H64EXPRTYPE_IF_STMT) {
            int got_is_a = guarded_by_is_a(expr);
            if (got_is_a)
                return 1;
        }
        if (expr->type == H64EXPRTYPE_FUNCDEF_STMT)
            return 0;
        child = expr;
        expr = expr->parent;
    }
    return 0;
}

int _astobviousmistakes_cb_CheckObviousErrors_visit_out(
        h64expression *expr, ATTR_UNUSED h64expression *parent,
        void *ud
        ) {
    asttransforminfo *rinfo = (asttransforminfo *)ud; 

    // Check for "continue" and "break" statements in wrong positions:
    if (isinvalidcontinuebreak(expr)) {
        char buf[512];
        h64snprintf(
            buf, sizeof(buf) - 1,
            "unexpected %s statement outside of any loop",
            (expr->type == H64EXPRTYPE_BREAK_STMT ?
             "break" : "continue")
        );
        if (!result_AddMessage(
                &rinfo->ast->resultmsg,
                H64MSG_ERROR, buf,
                rinfo->ast->fileuri, rinfo->ast->fileurilen,
                expr->line,
                expr->column
                )) {
            rinfo->hadoutofmemory = 1;
            return 0;
        }
    }
    // Check for attribute by identifier (.) done with wrong pairs:
    if (expr->type == H64EXPRTYPE_BINARYOP &&
            expr->op.optype == H64OP_ATTRIBUTEBYIDENTIFIER) {
        if (!expr->op.value2 ||
                expr->op.value2->type != H64EXPRTYPE_IDENTIFIERREF) {
            char buf[512];
            h64snprintf(
                buf, sizeof(buf) - 1,
                "cannot use access by identifier '.' followed by "
                "something else than identifier"
            );
            if (!result_AddMessage(
                    &rinfo->ast->resultmsg,
                    H64MSG_ERROR, buf,
                    rinfo->ast->fileuri, rinfo->ast->fileurilen,
                    (expr->op.value2 ? expr->op.value2->line : expr->line),
                    (expr->op.value2 ? expr->op.value2->column : expr->column)
                    )) {
                rinfo->hadoutofmemory = 1;
                return 0;
            }
        }
    }
    // Check for invalid calls to class types without "new":
    if (expr->type == H64EXPRTYPE_CALL &&
            (parent == NULL ||
             parent->type != H64EXPRTYPE_UNARYOP ||
             parent->op.optype != H64OP_NEW)) {
        if (expr->inlinecall.value->storage.set) {
            if (expr->inlinecall.value->storage.ref.type ==
                    H64STORETYPE_GLOBALCLASSSLOT &&
                    !_resolver_IsPossiblyGuardedInvalidType(expr)) {
                char buf[512];
                h64snprintf(
                    buf, sizeof(buf) - 1,
                    "calling a class type will cause TypeError, "
                    "use \"new\", or "
                    "put it in if statement with "
                    ".is_a() if intended for API compat"
                );
                if (!result_AddMessage(
                        &rinfo->ast->resultmsg,
                        H64MSG_ERROR, buf,
                        rinfo->ast->fileuri, rinfo->ast->fileurilen,
                        expr->line,
                        expr->column
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        }
    }
    // Check for attribute by identifier with unknown identifiers:
    if (expr->type == H64EXPRTYPE_IDENTIFIERREF &&
            parent != NULL &&
            parent->type == H64EXPRTYPE_BINARYOP &&
            parent->op.value2 == expr &&
            parent->op.optype == H64OP_ATTRIBUTEBYIDENTIFIER && (
              parent->op.value1->type != H64EXPRTYPE_IDENTIFIERREF ||
              strcmp(parent->op.value1->identifierref.value, "self") != 0
              // ^ self.X stuff that will likely error is handled earlier
              // where the scope is resolved, so we just care about
              // any other type of attribute access here.
            ) &&
            !parent->op.value2->storage.set  // must be unresolved still
            ) {
        int64_t idx = h64debugsymbols_AttributeNameToAttributeNameId(
            rinfo->pr->program->symbols,
            expr->identifierref.value, 0, 0
        );
        if (idx < 0) {
            int guarded = (
                guarded_by_is_a_or_has_attr(expr)
            );
            if (!guarded) {
                char buf[256];
                snprintf(buf, sizeof(buf) - 1,
                    "unknown identifier \"%s\" "
                    "will cause AttributeError, put it "
                    "in if statement with "
                    "has_attr() or .is_a() if intended for API "
                    "compat",
                    expr->identifierref.value
                );
                if (!result_AddMessage(
                        &rinfo->ast->resultmsg,
                        H64MSG_WARNING, buf,
                        rinfo->ast->fileuri, rinfo->ast->fileurilen,
                        expr->line,
                        expr->column
                        )) {
                    rinfo->hadoutofmemory = 1;
                    return 0;
                }
            }
        }
    }
    return 1;
}

int astobviousmistakes_CheckAST(
        h64compileproject *pr, h64ast *ast
        ) {
    // Assign storage for all local variables and parameters:
    int transformresult = asttransform_Apply(
        pr, ast,
        NULL,
        &_astobviousmistakes_cb_CheckObviousErrors_visit_out,
        NULL
    );
    if (!transformresult)
        return 0;

    return 1;
}


