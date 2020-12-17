// Copyright (c) 2020, ellie/@ell1e & Horse64 Team (see AUTHORS.md),
// also see LICENSE.md file.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef HORSE64_VMSUSPENDTYPEENUM_H_
#define HORSE64_VMSUSPENDTYPEENUM_H_

typedef enum suspendtype {
    SUSPENDTYPE_UNINITIALIZED = 0,
    SUSPENDTYPE_NONE = 1,
    SUSPENDTYPE_FIXEDTIME = 2,
    SUSPENDTYPE_WAITFORSOCKET,
    SUSPENDTYPE_WAITFORPROCESSTERMINATION,
    SUSPENDTYPE_ASYNCCALLSCHEDULED,
    SUSPENDTYPE_ASYNCSYSJOBWAIT,
    SUSPENDTYPE_SOCKWAIT_WRITABLEORERROR,
    SUSPENDTYPE_SOCKWAIT_READABLEORERROR,
    SUSPENDTYPE_DONE,
    SUSPENDTYPE_TOTALCOUNT
} suspendtype;

#endif  // HORSE64_VMSUSPENDTYPEENUM_H_