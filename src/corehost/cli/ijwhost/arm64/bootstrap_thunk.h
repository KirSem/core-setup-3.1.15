// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#ifndef IJW_BOOTSTRAP_THUNK_H
#define IJW_BOOTSTRAP_THUNK_H

#if !defined(_TARGET_ARM64_)
#error "This file should only be included on arm64 builds."
#endif

#include "pal.h"
#include "corhdr.h"

extern "C" void start_runtime_thunk_stub();

#include <pshpack1.h>
//=================================================================================
class bootstrap_thunk
{
private:
    std::uint32_t         m_rgCode[4];
    std::uintptr_t        m_pBootstrapCode;

    pal::dll_t       m_dll;            // pal::dll_t of this module
    std::uintptr_t       *m_slot;             // VTable slot for this thunk
    std::uint32_t           m_token;            // Token for this thunk

public:
    // Get thunk from the address that the thunk code provided
    static bootstrap_thunk *get_thunk_from_cookie(std::uintptr_t cookie);

    // Get thunk from the thunk code entry point address
    static bootstrap_thunk *get_thunk_from_entrypoint(std::uintptr_t entryAddr);

    // Initializes the thunk to point to pThunkInitFcn that will load the
    // runtime and perform the real thunk initialization.
    void initialize(std::uintptr_t pThunkInitFcn,
                    pal::dll_t dll,
                    std::uint32_t token,
                    std::uintptr_t *pSlot);

    // Returns the slot address of the vtable entry for this thunk
    std::uintptr_t *get_slot_address();

    // Returns the pal::dll_t for this thunk's module
    pal::dll_t get_dll_handle();

    // Returns the token of this thunk
    std::uint32_t get_token();

    std::uintptr_t get_entrypoint();
};
#include <poppack.h>

#endif