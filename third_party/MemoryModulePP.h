/*
* modify by xjun
*/

#ifndef __MEMORY_MODULE_HEADER
#define __MEMORY_MODULE_HEADER

#include <windows.h>

typedef void *HMEMORYRSRC;

typedef void *HCUSTOMMODULE;

#ifdef __cplusplus
extern "C" {
#endif

typedef LPVOID (*CustomAllocFunc)(LPVOID, SIZE_T, DWORD, DWORD, void*);
typedef BOOL (*CustomFreeFunc)(LPVOID, SIZE_T, DWORD, void*);
typedef HCUSTOMMODULE (*CustomLoadLibraryFunc)(LPCSTR, void *);
typedef FARPROC (*CustomGetProcAddressFunc)(HCUSTOMMODULE, LPCSTR, void *);
typedef void (*CustomFreeLibraryFunc)(HCUSTOMMODULE, void *);

struct ExportNameEntry {
	LPCSTR name;
	WORD idx;
};

typedef BOOL(WINAPI *DllEntryProc)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);
typedef int (WINAPI *ExeEntryProc)(void);

typedef struct {
	PIMAGE_NT_HEADERS headers;
	unsigned char *codeBase;
	HCUSTOMMODULE *modules;
	int numModules;
	BOOL initialized;
	BOOL isDLL;
	BOOL isRelocated;
	CustomAllocFunc alloc;
	CustomFreeFunc free;
	CustomLoadLibraryFunc loadLibrary;
	CustomGetProcAddressFunc getProcAddress;
	CustomFreeLibraryFunc freeLibrary;
	struct ExportNameEntry *nameExportsTable;
	void *userdata;
	ExeEntryProc exeEntry;
	DWORD pageSize;
	void *functionTable;
	BOOL tlsAttached;
	void *tlsState;
	unsigned forwardDepth;
} MEMORYMODULE, *PMEMORYMODULE;

BOOL MemoryInitializeTLS(PMEMORYMODULE);
void MemoryReleaseTLS(PMEMORYMODULE);

typedef struct {
	LPVOID address;
	SIZE_T size;
	DWORD characteristics;
	BOOL last;
} SECTIONFINALIZEDATA, *PSECTIONFINALIZEDATA;

#define GET_HEADER_DICTIONARY(module, idx)  &(module)->headers->OptionalHeader.DataDirectory[idx]

/**
 * Load EXE/DLL from memory location with the given size.
 *
 * All dependencies are resolved using default LoadLibrary/GetProcAddress
 * calls through the Windows API.
 */
PMEMORYMODULE MemoryLoadLibrary(const void *, size_t);

/**
 * Load EXE/DLL from memory location with the given size using custom dependency
 * resolvers.
 *
 * Dependencies will be resolved using passed callback methods.
 */
PMEMORYMODULE MemoryLoadLibraryEx(const void *, size_t,
    CustomAllocFunc,
    CustomFreeFunc,
    CustomLoadLibraryFunc,
    CustomGetProcAddressFunc,
    CustomFreeLibraryFunc,
    void *);

/**
 * Get address of exported method. Supports loading both by name and by
 * ordinal value.
 */
FARPROC MemoryGetProcAddress(PMEMORYMODULE, LPCSTR);

/**
 * Free previously loaded EXE/DLL.
 */
void MemoryFreeLibrary(PMEMORYMODULE);

/**
 * Execute entry point (EXE only). The entry point can only be executed
 * if the EXE has been loaded to the correct base address or it could
 * be relocated (i.e. relocation information have not been stripped by
 * the linker).
 *
 * Important: calling this function will not return, i.e. once the loaded
 * EXE finished running, the process will terminate.
 *
 * Returns a negative value if the entry point could not be executed.
 */
int MemoryCallEntryPoint(PMEMORYMODULE);

/**
 * Find the location of a resource with the specified type and name.
 */
HMEMORYRSRC MemoryFindResource(PMEMORYMODULE, LPCTSTR, LPCTSTR);

/**
 * Find the location of a resource with the specified type, name and language.
 */
HMEMORYRSRC MemoryFindResourceEx(PMEMORYMODULE, LPCTSTR, LPCTSTR, WORD);

/**
 * Get the size of the resource in bytes.
 */
DWORD MemorySizeofResource(PMEMORYMODULE, HMEMORYRSRC);

/**
 * Get a pointer to the contents of the resource.
 */
LPVOID MemoryLoadResource(PMEMORYMODULE, HMEMORYRSRC);

/**
 * Load a string resource.
 */
int MemoryLoadString(PMEMORYMODULE, UINT, LPTSTR, int);

/**
 * Load a string resource with a given language.
 */
int MemoryLoadStringEx(PMEMORYMODULE, UINT, LPTSTR, int, WORD);

/**
* Default implementation of CustomAllocFunc that calls VirtualAlloc
* internally to allocate memory for a library
*
* This is the default as used by MemoryLoadLibrary.
*/
LPVOID MemoryDefaultAlloc(LPVOID, SIZE_T, DWORD, DWORD, void *);

/**
* Default implementation of CustomFreeFunc that calls VirtualFree
* internally to free the memory used by a library
*
* This is the default as used by MemoryLoadLibrary.
*/
BOOL MemoryDefaultFree(LPVOID, SIZE_T, DWORD, void *);

/**
 * Default implementation of CustomLoadLibraryFunc that calls LoadLibraryA
 * internally to load an additional libary.
 *
 * This is the default as used by MemoryLoadLibrary.
 */
HCUSTOMMODULE MemoryDefaultLoadLibrary(LPCSTR, void *);

/**
 * Default implementation of CustomGetProcAddressFunc that calls GetProcAddress
 * internally to get the address of an exported function.
 *
 * This is the default as used by MemoryLoadLibrary.
 */
FARPROC MemoryDefaultGetProcAddress(HCUSTOMMODULE, LPCSTR, void *);

/**
 * Default implementation of CustomFreeLibraryFunc that calls FreeLibrary
 * internally to release an additional libary.
 *
 * This is the default as used by MemoryLoadLibrary.
 */
void MemoryDefaultFreeLibrary(HCUSTOMMODULE, void *);

/* Private implementation helpers; ordinary users see declarations only. */
#ifdef MEMORYMODULEPP_IMPLEMENTATION
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
/* Validated eager import resolution. Original adaptation: Unlicense. */
static BOOL ImageRange(PMEMORYMODULE module, SIZE_T rva, SIZE_T size) {
    SIZE_T imageSize = module->headers->OptionalHeader.SizeOfImage;
    if (rva > imageSize || size > imageSize - rva) {
        SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
    }
    return TRUE;
}
static const char *ImageString(PMEMORYMODULE module, SIZE_T rva) {
    if (!ImageRange(module, rva, 1) || !memchr(module->codeBase + rva, 0, module->headers->OptionalHeader.SizeOfImage - rva)) {
        SetLastError(ERROR_BAD_EXE_FORMAT); return NULL;
    }
    return (const char *)(module->codeBase + rva);
}
static HCUSTOMMODULE ImportDependency(PMEMORYMODULE module, DWORD nameRva) {
    const char *name = ImageString(module, nameRva);
    if (!name || !*name) return NULL;
    HCUSTOMMODULE handle = module->loadLibrary(name, module->userdata);
    if (!handle) { SetLastError(ERROR_MOD_NOT_FOUND); return NULL; }
    HCUSTOMMODULE *list = realloc(module->modules, (module->numModules + 1) * sizeof(HCUSTOMMODULE));
    if (!list) { module->freeLibrary(handle, module->userdata); SetLastError(ERROR_OUTOFMEMORY); return NULL; }
    module->modules = list;
    module->modules[module->numModules++] = handle;
    return handle;
}
static BOOL ResolveThunks(PMEMORYMODULE module, HCUSTOMMODULE handle, DWORD names, DWORD addresses, BOOL absoluteNames) {
    if (!names || !addresses) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
    for (SIZE_T n = names, a = addresses;; n += sizeof(uintptr_t), a += sizeof(uintptr_t)) {
        if (!ImageRange(module, n, sizeof(uintptr_t)) || !ImageRange(module, a, sizeof(FARPROC))) return FALSE;
        uintptr_t thunk = *(uintptr_t *)(module->codeBase + n);
        if (!thunk) return TRUE;
#ifndef _WIN64
        if (absoluteNames && thunk >= (uintptr_t)module->codeBase &&
            thunk - (uintptr_t)module->codeBase < module->headers->OptionalHeader.SizeOfImage)
            thunk -= (uintptr_t)module->codeBase;
#else
        (void)absoluteNames;
#endif
        const char *name;
        if (IMAGE_SNAP_BY_ORDINAL(thunk)) {
            if (thunk & ~(IMAGE_ORDINAL_FLAG | (uintptr_t)0xFFFF)) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
            name = (LPCSTR)IMAGE_ORDINAL(thunk);
        }
        else {
            /* Bound IAT addresses are never RVAs. Always prefer the original
             * name table; stripped bound-only images cannot be safely rebound. */
            if (!ImageRange(module, thunk, sizeof(WORD) + 1)) return FALSE;
            name = ImageString(module, thunk + sizeof(WORD));
            if (!name) return FALSE;
        }
        FARPROC proc = module->getProcAddress(handle, name, module->userdata);
        if (!proc) { SetLastError(ERROR_PROC_NOT_FOUND); return FALSE; }
        *(FARPROC *)(module->codeBase + a) = proc;
    }
}
static BOOL BuildImportTable(PMEMORYMODULE module) {
    PIMAGE_DATA_DIRECTORY dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!dir->Size) return TRUE;
    for (SIZE_T offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir->Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)(module->codeBase + dir->VirtualAddress + offset);
        if (!desc->Name) return TRUE;
        if (desc->TimeDateStamp && !desc->OriginalFirstThunk) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
        HCUSTOMMODULE dependency = ImportDependency(module, desc->Name);
        if (!dependency || !ResolveThunks(module, dependency, desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk, desc->FirstThunk, FALSE)) return FALSE;
    }
    SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
}
static BOOL BuildDelayedImportTable(PMEMORYMODULE module) {
    PIMAGE_DATA_DIRECTORY dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (!dir->Size) return TRUE;
    for (SIZE_T offset = 0; offset + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= dir->Size; offset += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
        PIMAGE_DELAYLOAD_DESCRIPTOR desc = (PIMAGE_DELAYLOAD_DESCRIPTOR)(module->codeBase + dir->VirtualAddress + offset);
        if (!desc->DllNameRVA) return TRUE;
        IMAGE_DELAYLOAD_DESCRIPTOR normalized = *desc;
        BOOL legacy = desc->Attributes.AllAttributes == 0;
#ifndef _WIN64
        if (legacy) {
            DWORD *values = (DWORD *)&normalized;
            for (unsigned field = 1; field <= 6; ++field) {
                if (!values[field]) continue;
                SIZE_T rva = values[field] - (uintptr_t)module->codeBase;
                if (!ImageRange(module, rva, 1)) return FALSE;
                values[field] = (DWORD)rva;
            }
            normalized.Attributes.AllAttributes = 1;
        }
#endif
        desc = &normalized;
        /* Rebind delay imports from the INT; cached bound addresses can be
         * stale. PE32 additionally supports legacy absolute-VA descriptors. */
        if (desc->Attributes.AllAttributes != 1 || !desc->ImportNameTableRVA) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
        HCUSTOMMODULE dependency = ImportDependency(module, desc->DllNameRVA);
        if (!dependency || !ResolveThunks(module, dependency, desc->ImportNameTableRVA, desc->ImportAddressTableRVA, legacy)) return FALSE;
        if (desc->ModuleHandleRVA) {
            if (!ImageRange(module, desc->ModuleHandleRVA, sizeof(HMODULE))) return FALSE;
            /* Eager dependencies stay owned by the graph. Keep the native
             * delay-unload helper from freeing a reference owned by us. */
            *(HMODULE *)(module->codeBase + desc->ModuleHandleRVA) = NULL;
        }
    }
    SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
}
static BOOL ValidateRuntimeData(PMEMORYMODULE module) {
    PIMAGE_DATA_DIRECTORY dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_TLS);
    if (dir->VirtualAddress) {
        if (dir->Size < sizeof(IMAGE_TLS_DIRECTORY)) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
        PIMAGE_TLS_DIRECTORY tls = (PIMAGE_TLS_DIRECTORY)(module->codeBase + dir->VirtualAddress);
        uintptr_t base = (uintptr_t)module->codeBase;
        if (tls->EndAddressOfRawData < tls->StartAddressOfRawData ||
            !ImageRange(module, tls->StartAddressOfRawData - base, tls->EndAddressOfRawData - tls->StartAddressOfRawData) ||
            !ImageRange(module, tls->AddressOfIndex - base, sizeof(DWORD))) return FALSE;
        if (tls->AddressOfCallBacks) {
            SIZE_T cursor = tls->AddressOfCallBacks - base;
            for (;;) {
                if (!ImageRange(module, cursor, sizeof(uintptr_t))) return FALSE;
                uintptr_t callback = *(uintptr_t *)(module->codeBase + cursor);
                if (!callback) break;
                if (!ImageRange(module, callback - base, 1)) return FALSE;
                cursor += sizeof(uintptr_t);
            }
        }
    }
#ifdef _WIN64
    dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (dir->Size % sizeof(RUNTIME_FUNCTION)) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
    PRUNTIME_FUNCTION functions = (PRUNTIME_FUNCTION)(module->codeBase + dir->VirtualAddress);
    DWORD previous = 0;
    for (SIZE_T i = 0; i < dir->Size / sizeof(RUNTIME_FUNCTION); ++i) {
        /* Linkers can retain zero-length entries for discarded COMDATs. */
        if (functions[i].BeginAddress < previous || functions[i].EndAddress < functions[i].BeginAddress ||
            !ImageRange(module, functions[i].BeginAddress, functions[i].EndAddress - functions[i].BeginAddress) ||
            !ImageRange(module, functions[i].UnwindData & ~3U, 4)) {
            SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
        }
        previous = functions[i].BeginAddress;
    }
#else
    dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
    if (dir->VirtualAddress && dir->Size) {
        if (!ImageRange(module, dir->VirtualAddress, sizeof(DWORD))) return FALSE;
        const IMAGE_LOAD_CONFIG_DIRECTORY32 *config = (const IMAGE_LOAD_CONFIG_DIRECTORY32 *)(module->codeBase + dir->VirtualAddress);
        SIZE_T needed = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SEHandlerCount) + sizeof(DWORD);
        if (config->Size >= needed) {
            if (!ImageRange(module, dir->VirtualAddress, needed)) return FALSE;
            DWORD count = config->SEHandlerCount;
            if (count) {
                SIZE_T rva = config->SEHandlerTable - (uintptr_t)module->codeBase;
                if (count > module->headers->OptionalHeader.SizeOfImage / sizeof(DWORD)) {
                    SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
                }
                if (!ImageRange(module, rva, (SIZE_T)count * sizeof(DWORD))) return FALSE;
                const DWORD *handlers = (const DWORD *)(module->codeBase + rva);
                for (DWORD i = 0; i < count; ++i) {
                    if ((i && handlers[i] <= handlers[i - 1]) || !ImageRange(module, handlers[i], 1)) {
                        SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
                    }
                }
            }
        }
    }
#endif
    return TRUE;
}

#endif

#ifdef __cplusplus
}
#endif

#endif  // __MEMORY_MODULE_HEADER
