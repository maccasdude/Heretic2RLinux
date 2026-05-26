//
// DllMain.c
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//
// On Windows H2Common is a DLL with a DllMain that runs the resource
// manager constructor/destructor. On Linux we replace that with GCC
// __attribute__((constructor)) / __attribute__((destructor)) which run
// at .so load/unload time and have the same lifecycle.
//

#include "ResourceManager.h"
#include "SinglyLinkedList.h"

extern ResourceManager_t sllist_nodes_mgr;

#if defined(_WIN32) || defined(WIN32)

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            ResMngr_Con(&sllist_nodes_mgr, SLL_NODE_SIZE, SLL_NODE_BLOCK_SIZE);
            break;

        case DLL_PROCESS_DETACH:
            ResMngr_Des(&sllist_nodes_mgr);
            break;

        default:
            break;
    }

    return TRUE;
}

#else // Linux / POSIX

static void __attribute__((constructor)) H2Common_OnLoad(void)
{
    ResMngr_Con(&sllist_nodes_mgr, SLL_NODE_SIZE, SLL_NODE_BLOCK_SIZE);
}

static void __attribute__((destructor))  H2Common_OnUnload(void)
{
    ResMngr_Des(&sllist_nodes_mgr);
}

#endif
