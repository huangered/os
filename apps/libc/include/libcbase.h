/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    libcbase.h

Abstract:

    This header contains definitions for the Minoca C Library.

Author:

    Evan Green 4-Mar-2013

--*/

#ifndef _LIBCBASE_H
#define _LIBCBASE_H

//
// ------------------------------------------------------------------- Includes
//

//
// ---------------------------------------------------------------- Definitions
//

#ifdef __cplusplus

extern "C" {

#endif

//
// This library is mostly POSIX compliant. These need to be defined with the
// inclusion of any POSIX header.
//

#define POSIX
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

//
// Wide characters (wchar_t) are 32-bits, meaning that the short identifier of
// every character in the Unicode required set has the same value when stored
// in a wchar_t. The Minoca C Library supports this compatibility as of the
// 4th edition of the ISO/IEC 10646 standard.
//

#define __STDC_ISO_10646__ 201409L

//
// Error out if the architecture is unknown.
//

#if !defined(__i386) && !defined(__arm__) && !defined(__amd64)

#error No known architecture was defined.

#endif

//
// Define some compiler-specific attributes.
//

#define __PACKED __attribute__((__packed__))
#define __NO_RETURN __attribute__((__noreturn__))
#define __ALIGNED(_Alignment) __attribute__((aligned(_Alignment)))
#define __ALIGNED16 __ALIGNED(16)
#define __THREAD __thread

#ifdef __ELF__

#define __DLLIMPORT __attribute__ ((visibility ("default")))
#define __DLLEXPORT __attribute__ ((visibility ("default")))
#define __DLLPROTECTED __attribute__ ((visibility ("protected")))

#define __HIDDEN __attribute__ ((visibility ("hidden")))
#define __CONSTRUCTOR __attribute__ ((constructor))
#define __DESTRUCTOR __attribute__ ((destructor))

#else

#define __DLLIMPORT __declspec(dllimport)
#define __DLLEXPORT __declspec(dllexport)
#define __DLLPROTECTED __declspec(dllexport)

#define __HIDDEN
#define __CONSTRUCTOR
#define __DESTRUCTOR

#endif

//
// Define all C API functions to be imports unless otherwise specified.
//

#ifndef LIBC_API

#define LIBC_API __DLLIMPORT

#endif

//
// ------------------------------------------------------ Data Type Definitions
//

//
// -------------------------------------------------------------------- Globals
//

//
// -------------------------------------------------------- Function Prototypes
//

LIBC_API
void
ClInitialize (
    void *Environment
    );

/*++

Routine Description:

    This routine initializes the Minoca C library. This routine is normally
    called by statically linked assembly within a program, and unless developing
    outside the usual paradigm should not need to call this routine directly.

Arguments:

    Environment - Supplies a pointer to the environment information to be passed
        on to the OS Base library.

Return Value:

    None.

--*/

#ifdef __cplusplus

}

#endif
#endif
