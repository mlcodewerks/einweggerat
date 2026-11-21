/* Project-supplied MemoryModulePP, hardened for Windows x86/x64 ZIP cores.
 * See MemoryModulePP-README.md for local changes and supported behavior. */
#ifdef _WIN32
#include <windows.h>
#include <winnt.h>
#include <stddef.h>
#include <tchar.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef DEBUG_OUTPUT
#include <stdio.h>
#endif

#if _MSC_VER
// Disable warning about data -> function pointer conversion
#pragma warning(disable : 4055)
// C4244: conversion from 'uintptr_t' to 'DWORD', possible loss of data.
#pragma warning(error : 4244)
// C4267: conversion from 'size_t' to 'int', possible loss of data.
#pragma warning(error : 4267)

#define inline __inline
#endif

#ifndef IMAGE_SIZEOF_BASE_RELOCATION
// Vista SDKs no longer define IMAGE_SIZEOF_BASE_RELOCATION!?
#define IMAGE_SIZEOF_BASE_RELOCATION (sizeof(IMAGE_BASE_RELOCATION))
#endif

#ifdef _WIN64
#define HOST_MACHINE IMAGE_FILE_MACHINE_AMD64
#define HOST_OPTIONAL_MAGIC IMAGE_NT_OPTIONAL_HDR64_MAGIC
#else
#define HOST_MACHINE IMAGE_FILE_MACHINE_I386
#define HOST_OPTIONAL_MAGIC IMAGE_NT_OPTIONAL_HDR32_MAGIC
#endif

#define MEMORYMODULEPP_IMPLEMENTATION
#include "MemoryModulePP.h"

static inline uintptr_t
AlignValueDown(uintptr_t value, uintptr_t alignment)
{
	return value & ~(alignment - 1);
}

static inline LPVOID
AlignAddressDown(LPVOID address, uintptr_t alignment)
{
	return (LPVOID)AlignValueDown((uintptr_t)address, alignment);
}

static inline size_t
AlignValueUp(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static inline void *
OffsetPointer(void *data, ptrdiff_t offset)
{
	return (void *)((uintptr_t)data + offset);
}

static inline void
OutputLastError(const char *msg)
{
#ifndef DEBUG_OUTPUT
	UNREFERENCED_PARAMETER(msg);
#else
	DWORD error = GetLastError();
	char detail[512];
	OutputDebugStringA(msg);
	if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, error, 0, detail, sizeof(detail), NULL)) OutputDebugStringA(detail);
	SetLastError(error);
#endif
}

static BOOL
CheckSize(size_t size, size_t expected)
{
	if (size < expected)
	{
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}

	return TRUE;
}

static BOOL
CopySections(const unsigned char *data, size_t size, PIMAGE_NT_HEADERS old_headers, PMEMORYMODULE module)
{
	UNREFERENCED_PARAMETER(old_headers);
	int i;
	unsigned char *codeBase = module->codeBase;
	unsigned char *dest;
	PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
	for (i = 0; i < module->headers->FileHeader.NumberOfSections; i++, section++)
	{
		SIZE_T section_size = section->Misc.VirtualSize;
		if (section_size < section->SizeOfRawData) section_size = section->SizeOfRawData;
		if (!section_size) continue;
		if (section->VirtualAddress > module->headers->OptionalHeader.SizeOfImage ||
			section_size > module->headers->OptionalHeader.SizeOfImage - section->VirtualAddress ||
			section->PointerToRawData > size || section->SizeOfRawData > size - section->PointerToRawData) {
			SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
		}
		dest = (unsigned char *)module->alloc(codeBase + section->VirtualAddress,
											  section_size,
											  MEM_COMMIT,
											  PAGE_READWRITE,
											  module->userdata);
		if (dest == NULL)
			return FALSE;
		
		dest = codeBase + section->VirtualAddress;
		memset(dest, 0, section_size);
		if (section->SizeOfRawData)
			memcpy(dest, data + section->PointerToRawData, section->SizeOfRawData);
	}
	return TRUE;
}

// Protection flags for memory pages (Executable, Readable, Writeable)
static int ProtectionFlags[2][2][2] = {
	{
		// not executable
		{PAGE_NOACCESS, PAGE_WRITECOPY},
		{PAGE_READONLY, PAGE_READWRITE},
	},
	{
		// executable
		{PAGE_EXECUTE, PAGE_EXECUTE_WRITECOPY},
		{PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE},
	},
};

static BOOL
FinalizeSection(PMEMORYMODULE module, PSECTIONFINALIZEDATA sectionData)
{
	DWORD protect, oldProtect;
	BOOL executable;
	BOOL readable;
	BOOL writeable;
	if (sectionData->size == 0)
		return TRUE;
	/* Keep metadata and TLS templates alive until unload. */

	// determine protection flags based on characteristics
	executable = (sectionData->characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
	readable = (sectionData->characteristics & IMAGE_SCN_MEM_READ) != 0;
	writeable = (sectionData->characteristics & IMAGE_SCN_MEM_WRITE) != 0;
	protect = ProtectionFlags[executable][readable][writeable];
	if (sectionData->characteristics & IMAGE_SCN_MEM_NOT_CACHED)
		protect |= PAGE_NOCACHE;
	// change memory access flags
	if (VirtualProtect(sectionData->address, sectionData->size, protect, &oldProtect) == 0)
	{
		OutputLastError("Error protecting memory page");
		return FALSE;
	}

	return TRUE;
}

static BOOL
FinalizeSections(PMEMORYMODULE module)
{
	SIZE_T count = AlignValueUp(module->headers->OptionalHeader.SizeOfImage, module->pageSize) / module->pageSize;
	DWORD *flags = calloc(count, sizeof(DWORD));
	if (!flags) { SetLastError(ERROR_OUTOFMEMORY); return FALSE; }
	for (SIZE_T p = 0; p < AlignValueUp(module->headers->OptionalHeader.SizeOfHeaders, module->pageSize) / module->pageSize; ++p)
		flags[p] = IMAGE_SCN_MEM_READ;
	PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
	for (unsigned i = 0; i < module->headers->FileHeader.NumberOfSections; ++i, ++section) {
		SIZE_T size = section->Misc.VirtualSize > section->SizeOfRawData ? section->Misc.VirtualSize : section->SizeOfRawData;
		if (!size) continue;
		SIZE_T first = section->VirtualAddress / module->pageSize;
		SIZE_T end = AlignValueUp(section->VirtualAddress + size, module->pageSize) / module->pageSize;
		for (SIZE_T p = first; p < end; ++p) flags[p] |= section->Characteristics;
	}
	BOOL ok = TRUE;
	for (SIZE_T p = 0; p < count;) {
		SIZE_T end = p + 1;
		while (end < count && flags[end] == flags[p]) ++end;
		SECTIONFINALIZEDATA data = { module->codeBase + p * module->pageSize, (end - p) * module->pageSize, flags[p], FALSE };
		if (!FinalizeSection(module, &data)) { ok = FALSE; break; }
		p = end;
	}
	free(flags);
	return ok;
}

static BOOL
PerformBaseRelocation(PMEMORYMODULE module, ptrdiff_t delta)
{
	unsigned char *codeBase = module->codeBase;
	PIMAGE_BASE_RELOCATION relocation;

	PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_BASERELOC);
	if (directory->Size == 0)
	{
		return (delta == 0);
	}

	relocation = (PIMAGE_BASE_RELOCATION)(codeBase + directory->VirtualAddress);
	SIZE_T remaining = directory->Size;
	for (; remaining;)
	{
		if (remaining < sizeof(IMAGE_BASE_RELOCATION) || relocation->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
			relocation->SizeOfBlock > remaining || (relocation->SizeOfBlock & 1)) {
			SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
		}
		DWORD i;
		unsigned char *dest = codeBase + relocation->VirtualAddress;
		unsigned short *relInfo = (unsigned short *)OffsetPointer(relocation, IMAGE_SIZEOF_BASE_RELOCATION);
		for (i = 0; i < ((relocation->SizeOfBlock - IMAGE_SIZEOF_BASE_RELOCATION) / 2); i++, relInfo++)
		{
			// the upper 4 bits define the type of relocation
			int type = *relInfo >> 12;
			// the lower 12 bits define the offset
			int offset = *relInfo & 0xfff;
			SIZE_T width = type == IMAGE_REL_BASED_ABSOLUTE ? 0 : type == IMAGE_REL_BASED_DIR64 ? 8 : type == IMAGE_REL_BASED_HIGHLOW ? 4 : 2;
			SIZE_T rva = (SIZE_T)relocation->VirtualAddress + offset;
			if (rva > module->headers->OptionalHeader.SizeOfImage || width > module->headers->OptionalHeader.SizeOfImage - rva) {
				SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
			}

			switch (type)
			{
			case IMAGE_REL_BASED_ABSOLUTE:
				// skip relocation
				break;
			case IMAGE_REL_BASED_HIGH:
			{
				*(WORD *)((PBYTE)dest + offset) += HIWORD(delta);
			}
			break;

			case IMAGE_REL_BASED_LOW:
			{
				*(WORD *)((PBYTE)dest + offset) += LOWORD(delta);
			}
			break;

			case IMAGE_REL_BASED_HIGHLOW:
				// change complete 32 bit address
				{
					DWORD *patchAddrHL = (DWORD *)(dest + offset);
					*patchAddrHL += (DWORD)delta;
				}
				break;

#ifdef _WIN64
			case IMAGE_REL_BASED_DIR64:
			{
				ULONGLONG *patchAddr64 = (ULONGLONG *)(dest + offset);
				*patchAddr64 += (ULONGLONG)delta;
			}
			break;
#endif

			default:
				SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
			}
		}

		// advance to next relocation block
		remaining -= relocation->SizeOfBlock;
		relocation = (PIMAGE_BASE_RELOCATION)OffsetPointer(relocation, relocation->SizeOfBlock);
	}
	return TRUE;
}

/* Direct in-memory TLS implementation. */
/* In-process static TLS registration, Windows x86/x64. Local code: Unlicense.
 * No proxy images, disk files, patched system code, or fabricated TLS indices.
 * Native TLS registration is private: resolve and validate both operations
 * before making any change, and reject unsupported ntdll implementations.
 */
#include "MemoryModulePP.h"
#include <winternl.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef NTSTATUS (NTAPI *LockLoader)(ULONG, ULONG *, ULONG_PTR *);
typedef NTSTATUS (NTAPI *UnlockLoader)(ULONG, ULONG_PTR);
#ifdef _WIN64
#define TLS_CALL NTAPI
#else
#define TLS_CALL __attribute__((fastcall))
#endif
typedef NTSTATUS (TLS_CALL *HandleTls)(void *);
typedef NTSTATUS (TLS_CALL *ReleaseTls)(void *, void **);
typedef ULONG (NTAPI *StatusError)(NTSTATUS);

typedef struct TLS_STATE {
    struct TLS_STATE *next, *previous;
    PMEMORYMODULE module;
    BOOL registered;
    void *template;
    /* Stable LDR_DATA_TABLE_ENTRY prefix, with room for current extensions.
     * This record is owned here and is never inserted in the PEB's lists. */
    ULONGLONG entry[128];
} TLS_STATE;

typedef struct {
    BYTE *base;
    DWORD size;
#ifdef _WIN64
    RUNTIME_FUNCTION *functions;
#endif
    DWORD count;
    BYTE *text;
    DWORD textSize;
    BYTE *rdata;
    DWORD rdataSize;
} NATIVE_IMAGE;

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static LockLoader lockLoader;
static UnlockLoader unlockLoader;
static HandleTls handleTls;
static ReleaseTls releaseTls;
static StatusError statusError;
static TLS_STATE *first, *last;
static volatile LONG ready;
#ifndef _WIN64
typedef void (__attribute__((fastcall)) *InsertSeh32)(void *, ULONG);
typedef void (__attribute__((fastcall)) *RemoveSeh32)(void *);
typedef struct { DWORD handlers, base, size, count; } SEH_ENTRY32;
typedef struct { DWORD count, capacity, epoch, overflow; SEH_ENTRY32 entries[1]; } SEH_CACHE32;
static InsertSeh32 insertSeh32;
static RemoveSeh32 removeSeh32;
static SEH_CACHE32 *sehCache32;
#endif

static BOOL Range(const NATIVE_IMAGE *image, SIZE_T rva, SIZE_T size) {
    return rva <= image->size && size <= image->size - rva;
}
#ifdef _WIN64
static RUNTIME_FUNCTION *FunctionAt(const NATIVE_IMAGE *image, DWORD rva) {
    DWORD lo = 0, hi = image->count;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        RUNTIME_FUNCTION *function = image->functions + mid;
        if (rva < function->BeginAddress) hi = mid;
        else if (rva >= function->EndAddress) lo = mid + 1;
        else return function;
    }
    return NULL;
}
static BOOL ExecutableFunction(const NATIVE_IMAGE *image, DWORD rva) {
    RUNTIME_FUNCTION *f = FunctionAt(image, rva);
    return f && f->BeginAddress == rva &&
        rva >= (DWORD)(image->text - image->base) &&
        f->EndAddress <= (DWORD)(image->text - image->base) + image->textSize;
}

/* Resolve a function by its own diagnostic filter and actual SEH scope table.
 * Do not guess an entry point from padding, a version offset, or a prologue
 * common to many unrelated functions. The owning scope must be unambiguous. */
static DWORD DiagnosticFunction(const NATIVE_IMAGE *image, const char *name) {
    DWORD candidate = 0;
    SIZE_T length = strlen(name) + 1;
    DWORD start = (DWORD)(image->rdata - image->base);
    DWORD end = start + image->rdataSize;
    for (DWORD s = start; s + length <= end; ++s) {
        if (memcmp(image->base + s, name, length)) continue;
        for (DWORD x = 0; x + 7 <= image->textSize; ++x) {
            BYTE *instruction = image->text + x;
            if ((instruction[0] != 0x48 && instruction[0] != 0x4C) ||
                instruction[1] != 0x8D || (instruction[2] & 0xC7) != 0x05) continue;
            int32_t displacement;
            memcpy(&displacement, instruction + 3, sizeof(displacement));
            if ((intptr_t)(instruction - image->base) + 7 + displacement != s) continue;
            RUNTIME_FUNCTION *filter = FunctionAt(image, (DWORD)(instruction - image->base));
            if (!filter) return 0;
            for (DWORD i = 0; i < image->count; ++i) {
                RUNTIME_FUNCTION *parent = image->functions + i;
                DWORD unwind = parent->UnwindData;
                if (!Range(image, unwind, 4)) continue;
                BYTE *info = image->base + unwind;
                unsigned flags = info[0] >> 3;
                if (!(flags & 3) || (flags & 4) || (info[0] & 7) > 2) continue;
                SIZE_T handler = (SIZE_T)unwind + ((4 + (SIZE_T)info[2] * 2 + 3) & ~(SIZE_T)3);
                if (!Range(image, handler, 8)) continue;
                DWORD count;
                memcpy(&count, image->base + handler + 4, 4);
                if (!count || count > 128 || !Range(image, handler + 8, (SIZE_T)count * 16)) continue;
                const DWORD *scopes = (const DWORD *)(image->base + handler + 8);
                for (DWORD j = 0; j < count; ++j, scopes += 4) {
                    if (scopes[2] != filter->BeginAddress || scopes[0] < parent->BeginAddress ||
                        scopes[0] >= scopes[1] || scopes[1] > parent->EndAddress ||
                        scopes[3] < parent->BeginAddress || scopes[3] >= parent->EndAddress) continue;
                    if (candidate && candidate != parent->BeginAddress) return 0;
                    candidate = parent->BeginAddress;
                }
            }
        }
    }
    return candidate;
}

static BOOL Calls(const NATIVE_IMAGE *image, const RUNTIME_FUNCTION *function, DWORD target) {
    for (DWORD p = function->BeginAddress; p + 5 <= function->EndAddress; ++p) {
        if (image->base[p] != 0xE8) continue;
        int32_t offset;
        memcpy(&offset, image->base + p + 1, 4);
        if ((intptr_t)p + 5 + offset == target) return TRUE;
    }
    return FALSE;
}
static BOOL SharesReference(const NATIVE_IMAGE *image, const RUNTIME_FUNCTION *a, const RUNTIME_FUNCTION *b) {
    /* Both TLS operations take the same native TLS lock and look up entries
     * through the same routine. Verify a shared RIP-relative lock reference. */
    for (DWORD x = a->BeginAddress; x + 7 <= a->EndAddress; ++x) {
        const BYTE *p = image->base + x;
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D) continue;
        int32_t d; memcpy(&d, p + 3, 4);
        intptr_t target = (intptr_t)x + 7 + d;
        if (target < 0 || !Range(image, (SIZE_T)target, sizeof(void *))) continue;
        for (DWORD y = b->BeginAddress; y + 7 <= b->EndAddress; ++y) {
            const BYTE *q = image->base + y;
            int32_t e; memcpy(&e, q + 3, 4);
            if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D && (intptr_t)y + 7 + e == target) return TRUE;
        }
    }
    return FALSE;
}
static DWORD ReleaseFunction(const NATIVE_IMAGE *image, DWORD handle) {
    /* Known x64 release ABI (rcx = LDR entry, rdx = optional returned entry).
     * The prefix alone is insufficient: require a true unwind boundary, the
     * registration routine's TLS lock, and their shared entry lookup call. */
    static const BYTE prefix[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,
        0x48,0x8B,0xFA,0x48,0x8B,0xD9,0x48,0x85,0xD2,0x75,0x0C};
    RUNTIME_FUNCTION *registration = FunctionAt(image, handle);
    DWORD result = 0;
    for (DWORD x = 0; x + sizeof(prefix) <= image->textSize; ++x) {
        if (memcmp(image->text + x, prefix, sizeof(prefix))) continue;
        DWORD rva = (DWORD)(image->text + x - image->base);
        if (!ExecutableFunction(image, rva)) continue;
        RUNTIME_FUNCTION *candidate = FunctionAt(image, rva);
        if (!SharesReference(image, registration, candidate)) continue;
        unsigned calls = 0;
        DWORD lookup = 0;
        for (DWORD p = candidate->BeginAddress; p + 5 <= candidate->EndAddress; ++p) {
            if (image->base[p] != 0xE8) continue;
            int32_t d; memcpy(&d, image->base + p + 1, 4);
            intptr_t target = (intptr_t)p + 5 + d;
            if (target < 0 || target > MAXDWORD || !ExecutableFunction(image, (DWORD)target)) continue;
            if (++calls == 2) { lookup = (DWORD)target; break; }
        }
        if (!lookup || !Calls(image, registration, lookup)) continue;
        if (result) return 0;
        result = rva;
    }
    return result;
}
#else
/* The x86 compiler places the diagnostic filter in an SEH4 scope table.
 * Resolve its owning push-locals/push-scope/call-prolog sequence, checking
 * actual absolute references rather than subtracting Windows-build offsets. */
static DWORD FunctionEnd32(const NATIVE_IMAGE *image, DWORD start, DWORD limit) {
    DWORD end = (DWORD)(image->text - image->base) + image->textSize;
    if (limit > end - start) limit = end - start;
    for (DWORD p = start + 16; p + 4 <= start + limit; ++p)
        if (!memcmp(image->base + p, "\xCC\xCC\xCC\xCC", 4)) return p;
    return 0;
}
static BOOL Text32(const NATIVE_IMAGE *image, uintptr_t address) {
    return address >= (uintptr_t)image->text && address - (uintptr_t)image->text < image->textSize;
}
static BOOL Calls32(const NATIVE_IMAGE *image, DWORD start, DWORD end, DWORD target) {
    for (DWORD p = start; p + 5 <= end; ++p) {
        if (image->base[p] != 0xE8) continue;
        int32_t offset; memcpy(&offset, image->base + p + 1, 4);
        if (p + 5 + offset == target) return TRUE;
    }
    return FALSE;
}
static DWORD DiagnosticFunction(const NATIVE_IMAGE *image, const char *name) {
    DWORD result = 0;
    SIZE_T length = strlen(name) + 1;
    for (DWORD s = 0; s + length <= image->rdataSize; ++s) {
        if (memcmp(image->rdata + s, name, length)) continue;
        uintptr_t string = (uintptr_t)(image->rdata + s);
        for (DWORD x = 0; x + 5 <= image->textSize; ++x) {
            BYTE *reference = image->text + x;
            if ((reference[0] != 0xBA && reference[0] != 0x68) || *(DWORD *)(reference + 1) != string) continue;
            for (DWORD f = 20; f + 8 <= image->rdataSize; f += 4) {
                uintptr_t filter = *(DWORD *)(image->rdata + f);
                if (filter > (uintptr_t)reference || (uintptr_t)reference - filter > 64 ||
                    !Text32(image, filter) || !Text32(image, *(DWORD *)(image->rdata + f + 4))) continue;
                for (DWORD row = 0; row < 16 && f >= 20 + row * 12; ++row) {
                    DWORD *table = (DWORD *)(image->rdata + f - 20 - row * 12);
                    if ((LONG)table[0] >= 0 || (LONG)table[2] >= 0 || (LONG)*(DWORD *)(image->rdata + f - 4) < -2) continue;
                    for (DWORD p = 5; p + 10 <= image->textSize; ++p) {
                        BYTE *push = image->text + p;
                        if (push[0] != 0x68 || *(DWORD *)(push + 1) != (uintptr_t)table || push[5] != 0xE8) continue;
                        BYTE *start = push[-2] == 0x6A ? push - 2 : push[-5] == 0x68 ? push - 5 : NULL;
                        if (!start || start[-1] != 0xCC) continue;
                        DWORD rva = (DWORD)(start - image->base), end = FunctionEnd32(image, rva, 0x3000);
                        if (!end || filter < (uintptr_t)start || filter >= (uintptr_t)(image->base + end)) continue;
                        if (result && result != rva) return 0;
                        result = rva;
                    }
                }
            }
        }
    }
    return result;
}
static DWORD ReleaseFunction(const NATIVE_IMAGE *image, DWORD handle) {
    DWORD handleEnd = FunctionEnd32(image, handle, 0x3000), result = 0;
    if (!handleEnd) return 0;
    static const BYTE prefix[] = {0x8B,0xFF,0x53,0x56,0x57,0x8B,0xFA,0x8B,0xF1,0xBB};
    for (DWORD x = 1; x + 20 <= image->textSize; ++x) {
        BYTE *p = image->text + x;
        if (p[-1] != 0xCC || memcmp(p, prefix, sizeof(prefix))) continue;
        DWORD start = (DWORD)(p - image->base), end = FunctionEnd32(image, start, 0x400);
        if (!end || !Calls32(image, handle, handleEnd, start)) continue;
        uintptr_t lock = *(DWORD *)(p + 10);
        if (lock < (uintptr_t)image->base || !Range(image, lock - (uintptr_t)image->base, sizeof(void *))) continue;
        BOOL shared = FALSE;
        for (DWORD y = handle; y + 5 <= handleEnd; ++y)
            if (image->base[y] == 0x68 && *(DWORD *)(image->base + y + 1) == lock) shared = TRUE;
        if (!shared) continue;
        if (result) return 0;
        result = start;
    }
    return result;
}
static DWORD Relative32(const NATIVE_IMAGE *image, DWORD instruction) {
    int32_t offset;
    memcpy(&offset, image->base + instruction + 1, 4);
    return instruction + 5 + offset;
}
static BOOL Contains32(const NATIVE_IMAGE *image, DWORD start, DWORD end, DWORD value) {
    for (DWORD i = start; i + 4 <= end; ++i)
        if (*(DWORD *)(image->base + i) == value) return TRUE;
    return FALSE;
}
static BOOL InitializeSeh32(const NATIVE_IMAGE *image) {
    /* Validate the native insert wrapper, its lock/unlock pair, protection
     * operation and table insertion helper. Do not write ntdll's cache or
     * weaken SafeSEH checks: the native routines own those operations. */
    static const BYTE prefix[] = {0x8B,0xFF,0x55,0x8B,0xEC,0x83,0xEC,0x0C,
        0x53,0x56,0x57,0x8D,0x45,0xF8,0x8B,0xFA};
    DWORD insertion = 0, removal = 0;
    SEH_CACHE32 *cache = NULL;
    for (DWORD p = 1; p + sizeof(prefix) <= image->textSize; ++p) {
        BYTE *code = image->text + p;
        if (code[-1] != 0xCC || memcmp(code, prefix, sizeof(prefix))) continue;
        DWORD start = (DWORD)(code - image->base), end = FunctionEnd32(image, start, 0x200);
        if (!end) continue;
        DWORD calls[16], count = 0;
        for (DWORD x = start; x + 5 <= end && count < 16; ++x) {
            if (image->base[x] != 0xE8) continue;
            DWORD target = Relative32(image, x);
            if (Text32(image, (uintptr_t)(image->base + target))) calls[count++] = target;
        }
        if (count != 6 || calls[2] != calls[4]) continue;
        BYTE *lock = image->base + calls[1], *unlock = image->base + calls[5];
        BYTE *helper = image->base + calls[3];
        if (!Range(image, calls[1], 15) || !Range(image, calls[5], 15) || !Range(image, calls[3], 10) ||
            memcmp(lock, "\x8B\xFF\x51\x68", 4) || memcmp(unlock, "\x8B\xFF\x51\x68", 4) ||
            *(DWORD *)(lock + 4) != *(DWORD *)(unlock + 4) || lock[8] != 0xE8 || unlock[8] != 0xE8 ||
            memcmp(helper, "\x8B\xFF\x55\x8B\xEC\xA1", 6)) continue;
        DWORD table = *(DWORD *)(helper + 6);
        if (table < (uintptr_t)image->base || !Range(image, table - (uintptr_t)image->base, sizeof(SEH_CACHE32))) continue;
        SEH_CACHE32 *candidate = (SEH_CACHE32 *)(uintptr_t)table;
        if (!candidate->count || candidate->capacity < candidate->count || candidate->capacity > 0x10000 ||
            !Range(image, table - (uintptr_t)image->base, 16 + (SIZE_T)candidate->capacity * sizeof(SEH_ENTRY32)) ||
            candidate->entries[0].base != (uintptr_t)image->base || candidate->entries[0].size != image->size) continue;
        DWORD leave = calls[5], enter = Relative32(image, calls[1] + 8);
        DWORD lockAddress = *(DWORD *)(lock + 4);
        for (DWORD x = 1; x + 28 <= image->textSize; ++x) {
            BYTE *r = image->text + x;
            if (r[-1] != 0xCC || memcmp(r, "\x8B\xFF\x56\x68", 4) || *(DWORD *)(r + 4) != lockAddress ||
                memcmp(r + 8, "\x8B\xF1\xE8", 3) || memcmp(r + 15, "\x8B\xD6\xE8", 3) ||
                r[22] != 0x5E || r[23] != 0xE9) continue;
            DWORD rva = (DWORD)(r - image->base);
            if (Relative32(image, rva + 10) != enter || Relative32(image, rva + 23) != leave) continue;
            DWORD removeHelper = Relative32(image, rva + 17);
            if (!Text32(image, (uintptr_t)(image->base + removeHelper))) continue;
            DWORD removeEnd = FunctionEnd32(image, removeHelper, 0x200);
            if (!removeEnd || !Contains32(image, removeHelper, removeEnd, table) ||
                !Calls32(image, removeHelper, removeEnd, calls[2])) continue;
            if (insertion || removal) return FALSE;
            insertion = start; removal = rva; cache = candidate;
        }
    }
    if (!insertion || !removal) return FALSE;
    insertSeh32 = (InsertSeh32)(image->base + insertion);
    removeSeh32 = (RemoveSeh32)(image->base + removal);
    sehCache32 = cache;
    return TRUE;
}
static BOOL RegisterSeh32(PMEMORYMODULE module) {
    if (sehCache32->count >= sehCache32->capacity) { SetLastError(ERROR_OUTOFMEMORY); return FALSE; }
    insertSeh32(module->codeBase, module->headers->OptionalHeader.SizeOfImage);
    for (DWORD i = 0; i < sehCache32->count; ++i) {
        if (sehCache32->entries[i].base == (uintptr_t)module->codeBase) {
            module->functionTable = sehCache32;
            return TRUE;
        }
    }
    SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
}
#endif

static BOOL CALLBACK Initialize(PINIT_ONCE init, PVOID parameter, PVOID *context) {
    (void)init; (void)parameter; (void)context;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NATIVE_IMAGE image = {0};
    image.base = (BYTE *)ntdll;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image.base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(image.base + dos->e_lfanew);
    image.size = nt->OptionalHeader.SizeOfImage;
#ifdef _WIN64
    IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!Range(&image, directory.VirtualAddress, directory.Size) || directory.Size % sizeof(RUNTIME_FUNCTION)) return TRUE;
    image.functions = (RUNTIME_FUNCTION *)(image.base + directory.VirtualAddress);
    image.count = directory.Size / sizeof(RUNTIME_FUNCTION);
#endif
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!memcmp(section[i].Name, ".text\0", 6) && Range(&image, section[i].VirtualAddress, section[i].Misc.VirtualSize)) {
            image.text = image.base + section[i].VirtualAddress;
            image.textSize = section[i].Misc.VirtualSize;
        }
        if (!memcmp(section[i].Name, ".rdata\0", 7) && Range(&image, section[i].VirtualAddress, section[i].Misc.VirtualSize)) {
            image.rdata = image.base + section[i].VirtualAddress;
            image.rdataSize = section[i].Misc.VirtualSize;
        }
    }
#ifndef _WIN64
    /* WOW64 ntdll can merge read-only constants/scope tables into .text. */
    if (!image.rdata) { image.rdata = image.text; image.rdataSize = image.textSize; }
#endif
    if (!image.text || !image.rdata) return TRUE;
    DWORD handle = DiagnosticFunction(&image, "LdrpHandleTlsData");
    if (!handle) return TRUE;
#ifdef _WIN64
    if (!ExecutableFunction(&image, handle)) return TRUE;
#endif
    DWORD release = ReleaseFunction(&image, handle);
    if (!release) return TRUE;
    handleTls = (HandleTls)(image.base + handle);
    releaseTls = (ReleaseTls)(image.base + release);
    lockLoader = (LockLoader)GetProcAddress(ntdll, "LdrLockLoaderLock");
    unlockLoader = (UnlockLoader)GetProcAddress(ntdll, "LdrUnlockLoaderLock");
    statusError = (StatusError)GetProcAddress(ntdll, "RtlNtStatusToDosError");
#ifndef _WIN64
    if (!InitializeSeh32(&image)) return TRUE;
#endif
    if (lockLoader && unlockLoader && statusError) InterlockedExchange(&ready, 1);
    return TRUE;
}
static void CallCallbacks(PMEMORYMODULE module, DWORD reason) {
    IMAGE_DATA_DIRECTORY *dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!dir->VirtualAddress) return;
    IMAGE_TLS_DIRECTORY *tls = (IMAGE_TLS_DIRECTORY *)(module->codeBase + dir->VirtualAddress);
    PIMAGE_TLS_CALLBACK *callbacks = (PIMAGE_TLS_CALLBACK *)tls->AddressOfCallBacks;
    if (callbacks) for (; *callbacks; ++callbacks) (*callbacks)(module->codeBase, reason, NULL);
}
static BOOL CallEntry(PMEMORYMODULE module, DWORD reason) {
    DWORD entry = module->headers->OptionalHeader.AddressOfEntryPoint;
    return !entry || ((DllEntryProc)(module->codeBase + entry))((HINSTANCE)module->codeBase, reason, NULL);
}

/* This callback is part of the executable's existing native TLS directory.
 * Windows has allocated every registered static block before it is called.
 * The native loader lock serializes it against module attach/detach below. */
static void NTAPI ThreadNotify(PVOID instance, DWORD reason, PVOID reserved) {
    (void)instance; (void)reserved;
    if (!InterlockedCompareExchange(&ready, 0, 0)) return;
    if (reason == DLL_THREAD_ATTACH) {
        for (TLS_STATE *state = first; state; state = state->next) {
            if (!state->module->initialized) continue;
            CallCallbacks(state->module, reason);
            CallEntry(state->module, reason);
        }
    } else if (reason == DLL_THREAD_DETACH) {
        for (TLS_STATE *state = last; state; state = state->previous) {
            if (!state->module->initialized) continue;
            CallCallbacks(state->module, reason);
            CallEntry(state->module, reason);
        }
    }
}
__attribute__((section(".CRT$XLB"), used)) static PIMAGE_TLS_CALLBACK threadNotify = ThreadNotify;

BOOL MemoryInitializeTLS(PMEMORYMODULE module) {
    if (!InitOnceExecuteOnce(&once, Initialize, NULL, NULL) || !InterlockedCompareExchange(&ready, 0, 0)) {
        SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
    }
    TLS_STATE *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) { SetLastError(ERROR_OUTOFMEMORY); return FALSE; }
    state->module = module;
    BYTE *entry = (BYTE *)state->entry;
#ifdef _WIN64
    *(void **)(entry + 0x30) = module->codeBase;
    *(void **)(entry + 0x38) = module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint;
    *(DWORD *)(entry + 0x40) = module->headers->OptionalHeader.SizeOfImage;
    UNICODE_STRING name = {22, 24, L"memory-core"};
    memcpy(entry + 0x48, &name, sizeof(name));
    memcpy(entry + 0x58, &name, sizeof(name));
    *(DWORD *)(entry + 0x10C) = 4; /* LoadReasonDynamicLoad */
#else
    *(void **)(entry + 0x18) = module->codeBase;
    *(void **)(entry + 0x1C) = module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint;
    *(DWORD *)(entry + 0x20) = module->headers->OptionalHeader.SizeOfImage;
    UNICODE_STRING name = {22, 24, L"memory-core"};
    memcpy(entry + 0x24, &name, sizeof(name));
    memcpy(entry + 0x2C, &name, sizeof(name));
    *(DWORD *)(entry + 0x94) = 4;
#endif
    ULONG_PTR cookie;
    NTSTATUS status = lockLoader(0, NULL, &cookie);
    if (status < 0) { HeapFree(GetProcessHeap(), 0, state); SetLastError(statusError(status)); return FALSE; }
    IMAGE_DATA_DIRECTORY *dir = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_TLS);
    if (dir->VirtualAddress) {
        IMAGE_TLS_DIRECTORY *tls = (IMAGE_TLS_DIRECTORY *)(module->codeBase + dir->VirtualAddress);
        IMAGE_TLS_DIRECTORY saved = *tls;
        SIZE_T raw = (SIZE_T)(saved.EndAddressOfRawData - saved.StartAddressOfRawData);
        /* Some native TLS paths ignore SizeOfZeroFill. Give them a complete
         * initialized template, retaining its allocation until deregistration.
         * Restore the image directory before any core callback executes. */
        if (saved.SizeOfZeroFill) {
            SIZE_T size = raw + saved.SizeOfZeroFill;
            if (size < raw) status = (NTSTATUS)0xC000007B;
            else state->template = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!state->template && status >= 0) status = (NTSTATUS)0xC0000017;
            if (status >= 0) {
                if (raw) memcpy(state->template, (void *)saved.StartAddressOfRawData, raw);
                DWORD old;
                if (!VirtualProtect(tls, sizeof(*tls), PAGE_READWRITE, &old)) status = (NTSTATUS)0xC0000001;
                else {
                    tls->StartAddressOfRawData = (uintptr_t)state->template;
                    tls->EndAddressOfRawData = tls->StartAddressOfRawData + size;
                    tls->SizeOfZeroFill = 0;
                    status = handleTls(entry);
                    *tls = saved;
                    DWORD ignored;
                    VirtualProtect(tls, sizeof(*tls), old, &ignored);
                }
            }
        } else status = handleTls(entry);
        if (status >= 0) state->registered = TRUE;
    }
#ifndef _WIN64
    if (status >= 0 && !RegisterSeh32(module)) {
        status = (NTSTATUS)0xC00000BB;
        if (state->registered) { releaseTls(state->entry, NULL); state->registered = FALSE; }
    }
#endif
    if (status < 0) {
        unlockLoader(0, cookie);
        if (state->template) VirtualFree(state->template, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, state);
        SetLastError(statusError(status)); return FALSE;
    }
    module->tlsState = state;
    state->previous = last;
    if (last) last->next = state; else first = state;
    last = state;
    CallCallbacks(module, DLL_PROCESS_ATTACH);
    module->tlsAttached = TRUE;
    module->initialized = TRUE;
    BOOL success = CallEntry(module, DLL_PROCESS_ATTACH);
    unlockLoader(0, cookie);
    if (!success) SetLastError(ERROR_DLL_INIT_FAILED);
    return success;
}

void MemoryReleaseTLS(PMEMORYMODULE module) {
    TLS_STATE *state = module->tlsState;
    if (!state) return;
    ULONG_PTR cookie;
    if (lockLoader(0, NULL, &cookie) < 0) return;
    if (module->tlsAttached) {
        module->tlsAttached = FALSE;
        CallCallbacks(module, DLL_PROCESS_DETACH);
    }
    if (module->initialized) {
        module->initialized = FALSE;
        CallEntry(module, DLL_PROCESS_DETACH);
    }
    if (state->previous) state->previous->next = state->next; else first = state->next;
    if (state->next) state->next->previous = state->previous; else last = state->previous;
    if (state->registered) releaseTls(state->entry, NULL);
#ifndef _WIN64
    if (module->functionTable) {
        removeSeh32(module->codeBase);
        module->functionTable = NULL;
    }
#endif
    module->tlsState = NULL;
    unlockLoader(0, cookie);
    if (state->template) VirtualFree(state->template, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, state);
}


static BOOL
InsertExceptionTable(PMEMORYMODULE module)
{

#if defined(_WIN64)

	PIMAGE_DATA_DIRECTORY pDataTable =
		&module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];

	PRUNTIME_FUNCTION pFuncTable =
		(PRUNTIME_FUNCTION)((ULONG_PTR)module->codeBase + pDataTable->VirtualAddress);

	if (pDataTable->Size)
	{
		if (pDataTable->Size % sizeof(RUNTIME_FUNCTION) || !RtlAddFunctionTable(pFuncTable, pDataTable->Size / sizeof(RUNTIME_FUNCTION), (DWORD64)module->codeBase)) {
			SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
		}
		module->functionTable = pFuncTable;
	}

#endif
	return TRUE;
}

LPVOID MemoryDefaultAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect, void *userdata)
{
	UNREFERENCED_PARAMETER(userdata);
	return VirtualAlloc(address, size, allocationType, protect);
}

BOOL MemoryDefaultFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType, void *userdata)
{
	UNREFERENCED_PARAMETER(userdata);
	return VirtualFree(lpAddress, dwSize, dwFreeType);
}

HCUSTOMMODULE MemoryDefaultLoadLibrary(LPCSTR filename, void *userdata)
{
	HMODULE result;
	UNREFERENCED_PARAMETER(userdata);
	result = LoadLibraryA(filename);
	if (result == NULL)
	{
		return NULL;
	}

	return (HCUSTOMMODULE)result;
}

FARPROC MemoryDefaultGetProcAddress(HCUSTOMMODULE module, LPCSTR name, void *userdata)
{
	UNREFERENCED_PARAMETER(userdata);
	return (FARPROC)GetProcAddress((HMODULE)module, name);
}

void MemoryDefaultFreeLibrary(HCUSTOMMODULE module, void *userdata)
{
	UNREFERENCED_PARAMETER(userdata);
	FreeLibrary((HMODULE)module);
}

PMEMORYMODULE MemoryLoadLibrary(const void *data, size_t size)
{
	return MemoryLoadLibraryEx(data, size, MemoryDefaultAlloc, MemoryDefaultFree, MemoryDefaultLoadLibrary, MemoryDefaultGetProcAddress, MemoryDefaultFreeLibrary, NULL);
}

PMEMORYMODULE MemoryLoadLibraryEx(const void *data, size_t size,
								  CustomAllocFunc allocMemory,
								  CustomFreeFunc freeMemory,
								  CustomLoadLibraryFunc loadLibrary,
								  CustomGetProcAddressFunc getProcAddress,
								  CustomFreeLibraryFunc freeLibrary,
								  void *userdata)
{
	PMEMORYMODULE result = NULL;
	PIMAGE_DOS_HEADER dos_header;
	PIMAGE_NT_HEADERS old_header;
	unsigned char *code, *headers;
	ptrdiff_t locationDelta;
	SYSTEM_INFO sysInfo;
	PIMAGE_SECTION_HEADER section;
	DWORD i;
	size_t lastSectionEnd = 0;
	size_t alignedImageSize;

	if (!data || !allocMemory || !freeMemory || !loadLibrary || !getProcAddress || !freeLibrary) {
		SetLastError(ERROR_INVALID_PARAMETER); return NULL;
	}
	if (!CheckSize(size, sizeof(IMAGE_DOS_HEADER)))
	{
		return NULL;
	}
	dos_header = (PIMAGE_DOS_HEADER)data;
	if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
	{
		SetLastError(ERROR_BAD_EXE_FORMAT);
		return NULL;
	}

	if (dos_header->e_lfanew < 0 || !CheckSize(size, (size_t)dos_header->e_lfanew + sizeof(IMAGE_NT_HEADERS)))
	{
		return NULL;
	}
	old_header = (PIMAGE_NT_HEADERS) & ((const unsigned char *)(data))[dos_header->e_lfanew];
	if (old_header->Signature != IMAGE_NT_SIGNATURE)
	{
		SetLastError(ERROR_BAD_EXE_FORMAT);
		return NULL;
	}

	if (old_header->FileHeader.Machine != HOST_MACHINE || old_header->OptionalHeader.Magic != HOST_OPTIONAL_MAGIC ||
		old_header->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER) ||
		old_header->OptionalHeader.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
		!(old_header->FileHeader.Characteristics & IMAGE_FILE_DLL))
	{
		SetLastError(ERROR_BAD_EXE_FORMAT);
		return NULL;
	}

	if (!old_header->OptionalHeader.SectionAlignment ||
		(old_header->OptionalHeader.SectionAlignment & (old_header->OptionalHeader.SectionAlignment - 1)) ||
		!old_header->OptionalHeader.FileAlignment ||
		(old_header->OptionalHeader.FileAlignment & (old_header->OptionalHeader.FileAlignment - 1)) ||
		old_header->OptionalHeader.FileAlignment > old_header->OptionalHeader.SectionAlignment)
	{
		// PE alignment values must be nonzero powers of two.
		SetLastError(ERROR_BAD_EXE_FORMAT);
		return NULL;
	}

	section = IMAGE_FIRST_SECTION(old_header);
	if (!CheckSize(size, (size_t)((const BYTE *)section - (const BYTE *)data) +
		old_header->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER)) ||
		old_header->OptionalHeader.SizeOfHeaders > old_header->OptionalHeader.SizeOfImage ||
		old_header->OptionalHeader.SizeOfHeaders < (size_t)((const BYTE *)section - (const BYTE *)data) + old_header->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))
		return NULL;
	for (i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++i) {
		IMAGE_DATA_DIRECTORY d = old_header->OptionalHeader.DataDirectory[i];
		if (i == IMAGE_DIRECTORY_ENTRY_SECURITY) continue; /* file offset, not RVA */
		if (d.VirtualAddress > old_header->OptionalHeader.SizeOfImage || d.Size > old_header->OptionalHeader.SizeOfImage - d.VirtualAddress) {
			SetLastError(ERROR_BAD_EXE_FORMAT); return NULL;
		}
	}
	for (i = 0; i < old_header->FileHeader.NumberOfSections; i++, section++)
	{
		size_t sectionSize = section->Misc.VirtualSize > section->SizeOfRawData ? section->Misc.VirtualSize : section->SizeOfRawData;
		size_t endOfSection = (size_t)section->VirtualAddress + sectionSize;
		if (endOfSection > lastSectionEnd)
		{
			lastSectionEnd = endOfSection;
		}
	}

	GetNativeSystemInfo(&sysInfo);
	alignedImageSize = AlignValueUp(old_header->OptionalHeader.SizeOfImage, sysInfo.dwPageSize);
	if (!alignedImageSize || lastSectionEnd > alignedImageSize || old_header->OptionalHeader.AddressOfEntryPoint >= alignedImageSize)
	{
		SetLastError(ERROR_BAD_EXE_FORMAT);
		return NULL;
	}

	// reserve memory for image of library
	// XXX: is it correct to commit the complete memory region at once?
	//      calling DllEntry raises an exception if we don't...
	code = (unsigned char *)allocMemory((LPVOID)(old_header->OptionalHeader.ImageBase),
										alignedImageSize,
										MEM_RESERVE | MEM_COMMIT,
										PAGE_READWRITE,
										userdata);

	if (code == NULL)
	{
		// try to allocate memory at arbitrary position
		code = (unsigned char *)allocMemory(NULL,
											alignedImageSize,
											MEM_RESERVE | MEM_COMMIT,
											PAGE_READWRITE,
											userdata);
		if (code == NULL)
		{
			SetLastError(ERROR_OUTOFMEMORY);
			return NULL;
		}
	}

	result = (PMEMORYMODULE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MEMORYMODULE));
	if (result == NULL)
	{
		freeMemory(code, 0, MEM_RELEASE, userdata);
		SetLastError(ERROR_OUTOFMEMORY);
		return NULL;
	}

	result->codeBase = code;
	result->isDLL = (old_header->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
	result->alloc = allocMemory;
	result->free = freeMemory;
	result->loadLibrary = loadLibrary;
	result->getProcAddress = getProcAddress;
	result->freeLibrary = freeLibrary;
	result->userdata = userdata;
	result->pageSize = sysInfo.dwPageSize;

	if (!CheckSize(size, old_header->OptionalHeader.SizeOfHeaders))
	{
		goto error;
	}

	// commit memory for headers
	headers = (unsigned char *)allocMemory(code,
										   old_header->OptionalHeader.SizeOfHeaders,
										   MEM_COMMIT,
										   PAGE_READWRITE,
										   userdata);

	// copy PE header to code
	if (!headers) goto error;
	memcpy(headers, dos_header, old_header->OptionalHeader.SizeOfHeaders);
	result->headers = (PIMAGE_NT_HEADERS) & ((const unsigned char *)(headers))[dos_header->e_lfanew];

	// update position
	result->headers->OptionalHeader.ImageBase = (uintptr_t)code;

	// copy sections from DLL file block to new memory location
	if (!CopySections((const unsigned char *)data, size, old_header, result))
	{
		goto error;
	}

	// adjust base address of imported data
	locationDelta = (ptrdiff_t)(result->headers->OptionalHeader.ImageBase - old_header->OptionalHeader.ImageBase);
	if (locationDelta != 0)
	{
		// if not exist reloc table?
		if (result->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0)
		{
			goto error;
		}
		result->isRelocated = PerformBaseRelocation(result, locationDelta);
		if (!result->isRelocated) goto error;
	}
	else
	{
		result->isRelocated = TRUE;
	}

	// load required dlls and adjust function table of imports
	if (!BuildImportTable(result))
	{
		goto error;
	}

	if (!BuildDelayedImportTable(result))
	{
		goto error;
	}

	// mark memory pages depending on section headers and release
	if (!ValidateRuntimeData(result)) goto error;
	// sections that are marked as "discardable"
	if (!FinalizeSections(result))
	{
		goto error;
	}

	// Unwind information must exist before any core code (including TLS) runs.
	FlushInstructionCache(GetCurrentProcess(), result->codeBase, result->headers->OptionalHeader.SizeOfImage);
	if (!InsertExceptionTable(result) || !MemoryInitializeTLS(result))
	{
		goto error;
	}

	return (PMEMORYMODULE)result;

error:
	// cleanup
	{ DWORD errorCode = GetLastError(); MemoryFreeLibrary(result); SetLastError(errorCode); }
	return NULL;
}

static int _compare(const void *a, const void *b)
{
	const struct ExportNameEntry *p1 = (const struct ExportNameEntry *)a;
	const struct ExportNameEntry *p2 = (const struct ExportNameEntry *)b;
	return strcmp(p1->name, p2->name);
}

static int _find(const void *a, const void *b)
{
	LPCSTR *name = (LPCSTR *)a;
	const struct ExportNameEntry *p = (const struct ExportNameEntry *)b;
	return strcmp(*name, p->name);
}

FARPROC MemoryGetProcAddress(PMEMORYMODULE mod, LPCSTR name)
{
	if (!mod || !name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
	PMEMORYMODULE module = (PMEMORYMODULE)mod;
	unsigned char *codeBase = module->codeBase;
	DWORD idx = 0;
	PIMAGE_EXPORT_DIRECTORY exports;
	PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_EXPORT);
	if (directory->Size < sizeof(IMAGE_EXPORT_DIRECTORY) || !ImageRange(module, directory->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
	{
		// no export table found
		SetLastError(ERROR_PROC_NOT_FOUND);
		return NULL;
	}

	exports = (PIMAGE_EXPORT_DIRECTORY)(codeBase + directory->VirtualAddress);
	SIZE_T imageSize = module->headers->OptionalHeader.SizeOfImage;
	if (exports->NumberOfFunctions > imageSize / sizeof(DWORD) || exports->NumberOfNames > imageSize / sizeof(DWORD)) {
		SetLastError(ERROR_BAD_EXE_FORMAT); return NULL;
	}
	if (!ImageRange(module, exports->AddressOfFunctions, (SIZE_T)exports->NumberOfFunctions * sizeof(DWORD)) ||
		!ImageRange(module, exports->AddressOfNames, (SIZE_T)exports->NumberOfNames * sizeof(DWORD)) ||
		!ImageRange(module, exports->AddressOfNameOrdinals, (SIZE_T)exports->NumberOfNames * sizeof(WORD))) return NULL;
	if (exports->NumberOfFunctions == 0)
	{
		// DLL doesn't export anything
		SetLastError(ERROR_PROC_NOT_FOUND);
		return NULL;
	}

	if ((uintptr_t)name <= 0xFFFF)
	{
		// load function by ordinal value
		if (LOWORD(name) < exports->Base)
		{
			SetLastError(ERROR_PROC_NOT_FOUND);
			return NULL;
		}

		idx = LOWORD(name) - exports->Base;
	}
	else if (!exports->NumberOfNames)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return NULL;
	}
	else
	{
		const struct ExportNameEntry *found;

		// Lazily build name table and sort it by names
		if (!module->nameExportsTable)
		{
			DWORD i;
			DWORD *nameRef = (DWORD *)(codeBase + exports->AddressOfNames);
			WORD *ordinal = (WORD *)(codeBase + exports->AddressOfNameOrdinals);
			struct ExportNameEntry *entry = (struct ExportNameEntry *)malloc(exports->NumberOfNames * sizeof(struct ExportNameEntry));
			module->nameExportsTable = entry;
			if (!entry)
			{
				SetLastError(ERROR_OUTOFMEMORY);
				return NULL;
			}
			for (i = 0; i < exports->NumberOfNames; i++, nameRef++, ordinal++, entry++)
			{
				entry->name = ImageString(module, *nameRef);
				if (!entry->name || *ordinal >= exports->NumberOfFunctions) {
					free(module->nameExportsTable); module->nameExportsTable = NULL;
					SetLastError(ERROR_BAD_EXE_FORMAT); return NULL;
				}
				entry->idx = *ordinal;
			}
			qsort(module->nameExportsTable,
				  exports->NumberOfNames,
				  sizeof(struct ExportNameEntry), _compare);
		}

		// search function name in list of exported names with binary search
		found = (const struct ExportNameEntry *)bsearch(&name,
														module->nameExportsTable,
														exports->NumberOfNames,
														sizeof(struct ExportNameEntry), _find);
		if (!found)
		{
			// exported symbol not found
			SetLastError(ERROR_PROC_NOT_FOUND);
			return NULL;
		}

		idx = found->idx;
	}

	if (idx >= exports->NumberOfFunctions)
	{
		// name <-> ordinal number don't match
		SetLastError(ERROR_PROC_NOT_FOUND);
		return NULL;
	}

	// AddressOfFunctions contains the RVAs to the "real" functions
	DWORD rva = *(DWORD *)(codeBase + exports->AddressOfFunctions + (idx * 4));
	if (!rva) { SetLastError(ERROR_PROC_NOT_FOUND); return NULL; }
	if (!ImageRange(module, rva, 1)) return NULL;
	if (rva >= directory->VirtualAddress && rva - directory->VirtualAddress < directory->Size) {
		const char *forward = ImageString(module, rva);
		if (!forward || module->forwardDepth >= 32) { SetLastError(ERROR_BAD_EXE_FORMAT); return NULL; }
		const char *dot = strrchr(forward, '.');
		if (!dot || dot == forward || dot - forward > 250) { SetLastError(ERROR_BAD_EXE_FORMAT); return NULL; }
		char dll[256]; memcpy(dll, forward, dot - forward); strcpy(dll + (dot - forward), ".dll");
		HCUSTOMMODULE dependency = module->loadLibrary(dll, module->userdata);
		if (!dependency) return NULL;
		HCUSTOMMODULE *list = realloc(module->modules, (module->numModules + 1) * sizeof(HCUSTOMMODULE));
		if (!list) { module->freeLibrary(dependency, module->userdata); SetLastError(ERROR_OUTOFMEMORY); return NULL; }
		module->modules = list; module->modules[module->numModules++] = dependency;
		LPCSTR target = dot + 1;
		if (*target == '#') {
			char *end;
			unsigned long ordinal = strtoul(target + 1, &end, 10);
			if (end == target + 1 || *end || ordinal > 65535) { SetLastError(ERROR_BAD_EXE_FORMAT); return NULL; }
			target = (LPCSTR)(uintptr_t)ordinal;
		}
		++module->forwardDepth;
		FARPROC proc = module->getProcAddress(dependency, target, module->userdata);
		--module->forwardDepth;
		return proc;
	}
	return (FARPROC)(LPVOID)(codeBase + rva);
}

void MemoryFreeLibrary(PMEMORYMODULE mod)
{
	PMEMORYMODULE module = (PMEMORYMODULE)mod;

	if (module == NULL)
	{
		return;
	}
	MemoryReleaseTLS(module);
#ifdef _WIN64
	if (module->functionTable) RtlDeleteFunctionTable(module->functionTable);
#endif

	free(module->nameExportsTable);
	if (module->modules != NULL)
	{
		// free previously opened libraries
		int i;
		for (i = 0; i < module->numModules; i++)
		{
			if (module->modules[i] != NULL)
			{
				module->freeLibrary(module->modules[i], module->userdata);
			}
		}

		free(module->modules);
	}

	if (module->codeBase != NULL)
	{
		// release memory of library
		module->free(module->codeBase, 0, MEM_RELEASE, module->userdata);
	}

	HeapFree(GetProcessHeap(), 0, module);
}

int MemoryCallEntryPoint(PMEMORYMODULE mod)
{
	PMEMORYMODULE module = (PMEMORYMODULE)mod;

	if (module == NULL || module->isDLL || module->exeEntry == NULL || !module->isRelocated)
	{
		return -1;
	}

	return module->exeEntry();
}

#define DEFAULT_LANGUAGE MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)

HMEMORYRSRC MemoryFindResource(PMEMORYMODULE module, LPCTSTR name, LPCTSTR type)
{
	return MemoryFindResourceEx(module, name, type, DEFAULT_LANGUAGE);
}

static PIMAGE_RESOURCE_DIRECTORY_ENTRY _MemorySearchResourceEntry(
	void *root,
	PIMAGE_RESOURCE_DIRECTORY resources,
	LPCTSTR key)
{
	PIMAGE_RESOURCE_DIRECTORY_ENTRY entries = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(resources + 1);
	PIMAGE_RESOURCE_DIRECTORY_ENTRY result = NULL;
	DWORD start;
	DWORD end;
	DWORD middle;

	if (!IS_INTRESOURCE(key) && key[0] == TEXT('#'))
	{
		// special case: resource id given as string
		TCHAR *endpos = NULL;
		long int tmpkey = (WORD)_tcstol((TCHAR *)&key[1], &endpos, 10);
		if (tmpkey <= 0xffff && lstrlen(endpos) == 0)
		{
			key = MAKEINTRESOURCE(tmpkey);
		}
	}

	// entries are stored as ordered list of named entries,
	// followed by an ordered list of id entries - we can do
	// a binary search to find faster...
	if (IS_INTRESOURCE(key))
	{
		WORD check = (WORD)(uintptr_t)key;
		start = resources->NumberOfNamedEntries;
		end = start + resources->NumberOfIdEntries;

		while (end > start)
		{
			WORD entryName;
			middle = (start + end) >> 1;
			entryName = (WORD)entries[middle].Name;
			if (check < entryName)
			{
				end = (end != middle ? middle : middle - 1);
			}
			else if (check > entryName)
			{
				start = (start != middle ? middle : middle + 1);
			}
			else
			{
				result = &entries[middle];
				break;
			}
		}
	}
	else
	{
		LPCWSTR searchKey;
		size_t searchKeyLen = _tcslen(key);
#if defined(UNICODE)
		searchKey = key;
#else
		// Resource names are always stored using 16bit characters, need to
		// convert string we search for.
#define MAX_LOCAL_KEY_LENGTH 2048
		// In most cases resource names are short, so optimize for that by
		// using a pre-allocated array.
		wchar_t _searchKeySpace[MAX_LOCAL_KEY_LENGTH + 1];
		LPWSTR _searchKey;
		if (searchKeyLen > MAX_LOCAL_KEY_LENGTH)
		{
			size_t _searchKeySize = (searchKeyLen + 1) * sizeof(wchar_t);
			_searchKey = (LPWSTR)malloc(_searchKeySize);
			if (_searchKey == NULL)
			{
				SetLastError(ERROR_OUTOFMEMORY);
				return NULL;
			}
		}
		else
		{
			_searchKey = &_searchKeySpace[0];
		}

		mbstowcs(_searchKey, key, searchKeyLen);
		_searchKey[searchKeyLen] = 0;
		searchKey = _searchKey;
#endif
		start = 0;
		end = resources->NumberOfNamedEntries;
		while (end > start)
		{
			int cmp;
			PIMAGE_RESOURCE_DIR_STRING_U resourceString;
			middle = (start + end) >> 1;
			resourceString = (PIMAGE_RESOURCE_DIR_STRING_U)OffsetPointer(root, entries[middle].Name & 0x7FFFFFFF);
			cmp = _wcsnicmp(searchKey, resourceString->NameString, resourceString->Length);
			if (cmp == 0)
			{
				// Handle partial match
				if (searchKeyLen > resourceString->Length)
				{
					cmp = 1;
				}
				else if (searchKeyLen < resourceString->Length)
				{
					cmp = -1;
				}
			}
			if (cmp < 0)
			{
				end = (middle != end ? middle : middle - 1);
			}
			else if (cmp > 0)
			{
				start = (middle != start ? middle : middle + 1);
			}
			else
			{
				result = &entries[middle];
				break;
			}
		}
#if !defined(UNICODE)
		if (searchKeyLen > MAX_LOCAL_KEY_LENGTH)
		{
			free(_searchKey);
		}
#undef MAX_LOCAL_KEY_LENGTH
#endif
	}

	return result;
}

HMEMORYRSRC MemoryFindResourceEx(PMEMORYMODULE module, LPCTSTR name, LPCTSTR type, WORD language)
{
	unsigned char *codeBase = ((PMEMORYMODULE)module)->codeBase;
	PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY((PMEMORYMODULE)module, IMAGE_DIRECTORY_ENTRY_RESOURCE);
	PIMAGE_RESOURCE_DIRECTORY rootResources;
	PIMAGE_RESOURCE_DIRECTORY nameResources;
	PIMAGE_RESOURCE_DIRECTORY typeResources;
	PIMAGE_RESOURCE_DIRECTORY_ENTRY foundType;
	PIMAGE_RESOURCE_DIRECTORY_ENTRY foundName;
	PIMAGE_RESOURCE_DIRECTORY_ENTRY foundLanguage;
	if (directory->Size == 0)
	{
		// no resource table found
		SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NULL;
	}

	if (language == DEFAULT_LANGUAGE)
	{
		// use language from current thread
		language = LANGIDFROMLCID(GetThreadLocale());
	}

	// resources are stored as three-level tree
	// - first node is the type
	// - second node is the name
	// - third node is the language
	rootResources = (PIMAGE_RESOURCE_DIRECTORY)(codeBase + directory->VirtualAddress);
	foundType = _MemorySearchResourceEntry(rootResources, rootResources, type);
	if (foundType == NULL)
	{
		SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
		return NULL;
	}

	typeResources = (PIMAGE_RESOURCE_DIRECTORY)(codeBase + directory->VirtualAddress + (foundType->OffsetToData & 0x7fffffff));
	foundName = _MemorySearchResourceEntry(rootResources, typeResources, name);
	if (foundName == NULL)
	{
		SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}

	nameResources = (PIMAGE_RESOURCE_DIRECTORY)(codeBase + directory->VirtualAddress + (foundName->OffsetToData & 0x7fffffff));
	foundLanguage = _MemorySearchResourceEntry(rootResources, nameResources, (LPCTSTR)(uintptr_t)language);
	if (foundLanguage == NULL)
	{
		// requested language not found, use first available
		if (nameResources->NumberOfIdEntries == 0)
		{
			SetLastError(ERROR_RESOURCE_LANG_NOT_FOUND);
			return NULL;
		}

		foundLanguage = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(nameResources + 1);
	}

	return (codeBase + directory->VirtualAddress + (foundLanguage->OffsetToData & 0x7fffffff));
}

DWORD MemorySizeofResource(PMEMORYMODULE module, HMEMORYRSRC resource)
{
	PIMAGE_RESOURCE_DATA_ENTRY entry;
	UNREFERENCED_PARAMETER(module);
	entry = (PIMAGE_RESOURCE_DATA_ENTRY)resource;
	if (entry == NULL)
	{
		return 0;
	}

	return entry->Size;
}

LPVOID MemoryLoadResource(PMEMORYMODULE module, HMEMORYRSRC resource)
{
	unsigned char *codeBase = ((PMEMORYMODULE)module)->codeBase;
	PIMAGE_RESOURCE_DATA_ENTRY entry = (PIMAGE_RESOURCE_DATA_ENTRY)resource;
	if (entry == NULL)
	{
		return NULL;
	}

	return codeBase + entry->OffsetToData;
}

int MemoryLoadString(PMEMORYMODULE module, UINT id, LPTSTR buffer, int maxsize)
{
	return MemoryLoadStringEx(module, id, buffer, maxsize, DEFAULT_LANGUAGE);
}

int MemoryLoadStringEx(PMEMORYMODULE module, UINT id, LPTSTR buffer, int maxsize, WORD language)
{
	HMEMORYRSRC resource;
	PIMAGE_RESOURCE_DIR_STRING_U data;
	DWORD size;
	if (maxsize == 0)
	{
		return 0;
	}

	resource = MemoryFindResourceEx(module, MAKEINTRESOURCE((id >> 4) + 1), RT_STRING, language);
	if (resource == NULL)
	{
		buffer[0] = 0;
		return 0;
	}

	data = (PIMAGE_RESOURCE_DIR_STRING_U)MemoryLoadResource(module, resource);
	id = id & 0x0f;
	while (id--)
	{
		data = (PIMAGE_RESOURCE_DIR_STRING_U)OffsetPointer(data, (data->Length + 1) * sizeof(WCHAR));
	}
	if (data->Length == 0)
	{
		SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
		buffer[0] = 0;
		return 0;
	}

	size = data->Length;
	if (size >= (DWORD)maxsize)
	{
		size = maxsize;
	}
	else
	{
		buffer[size] = 0;
	}
#if defined(UNICODE)
	wcsncpy(buffer, data->NameString, size);
#else
	wcstombs(buffer, data->NameString, size);
#endif
	return size;
}

#ifdef TESTSUITE
#include <stdio.h>

#ifndef PRIxPTR
#ifdef _WIN64
#define PRIxPTR "I64x"
#else
#define PRIxPTR "x"
#endif
#endif

static const uintptr_t AlignValueDownTests[][3] = {
	{16, 16, 16},
	{17, 16, 16},
	{32, 16, 32},
	{33, 16, 32},
#ifdef _WIN64
	{0x12345678abcd1000, 0x1000, 0x12345678abcd1000},
	{0x12345678abcd101f, 0x1000, 0x12345678abcd1000},
#endif
	{0, 0, 0},
};

static const uintptr_t AlignValueUpTests[][3] = {
	{16, 16, 16},
	{17, 16, 32},
	{32, 16, 32},
	{33, 16, 48},
#ifdef _WIN64
	{0x12345678abcd1000, 0x1000, 0x12345678abcd1000},
	{0x12345678abcd101f, 0x1000, 0x12345678abcd2000},
#endif
	{0, 0, 0},
};

BOOL MemoryModuleTestsuite()
{
	BOOL success = TRUE;
	size_t idx;
	for (idx = 0; AlignValueDownTests[idx][0]; ++idx)
	{
		const uintptr_t *tests = AlignValueDownTests[idx];
		uintptr_t value = AlignValueDown(tests[0], tests[1]);
		if (value != tests[2])
		{
			printf("AlignValueDown failed for 0x%" PRIxPTR "/0x%" PRIxPTR ": expected 0x%" PRIxPTR ", got 0x%" PRIxPTR "\n",
				   tests[0], tests[1], tests[2], value);
			success = FALSE;
		}
	}
	for (idx = 0; AlignValueDownTests[idx][0]; ++idx)
	{
		const uintptr_t *tests = AlignValueUpTests[idx];
		uintptr_t value = AlignValueUp(tests[0], tests[1]);
		if (value != tests[2])
		{
			printf("AlignValueUp failed for 0x%" PRIxPTR "/0x%" PRIxPTR ": expected 0x%" PRIxPTR ", got 0x%" PRIxPTR "\n",
				   tests[0], tests[1], tests[2], value);
			success = FALSE;
		}
	}
	if (success)
	{
		printf("OK\n");
	}
	return success;
}
#endif

#endif
