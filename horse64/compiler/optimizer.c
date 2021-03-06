// Copyright (c) 2020-2021, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/ast.h"
#include "compiler/astparser.h"
#include "compiler/asttransform.h"
#include "compiler/compileproject.h"
#include "compiler/optimizer.h"

int _resolvercallback_PreevaluateConstants_visit_out(
        h64expression *expr, h64expression *parent, void *ud
        ) {
    return 1;
}

int optimizer_PreevaluateConstants(
        h64compileproject *pr, h64ast *ast
        ) {
    int result = asttransform_Apply(
        pr, ast, NULL,
        &_resolvercallback_PreevaluateConstants_visit_out,
        NULL
    );
    if (!result)
        return 0;
    return 1;
}
