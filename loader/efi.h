/* The parts of the UEFI specification Banana Boot uses (no EDK2/gnu-efi). */
#ifndef LOADER_EFI_H
#define LOADER_EFI_H

#include "common.h"

#define EFIAPI __attribute__((ms_abi))
typedef unsigned long long UINTN;
typedef UINTN EFI_STATUS;
typedef void* EFI_HANDLE;
typedef void* EFI_EVENT;
typedef unsigned short CHAR16;
typedef u64 EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS            0
#define EFI_BUFFER_TOO_SMALL   (0x8000000000000000ull | 5)
#define EFI_ERROR(s)           ((long long)(s) < 0)

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } EFI_GUID;

typedef struct { u64 Signature; u32 Revision, HeaderSize, CRC32, Reserved; } EFI_TABLE_HEADER;

typedef struct { u16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT*, u8);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT*, EFI_INPUT_KEY*);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT;

typedef struct EFI_SIMPLE_TEXT_OUTPUT {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT*, u8);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT*, const CHAR16*);
    void* TestString;
    void* QueryMode;
    void* SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT*, UINTN);
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT*);
    EFI_STATUS (EFIAPI *SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT*, UINTN, UINTN);
    EFI_STATUS (EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT*, u8);
    void* Mode;
} EFI_SIMPLE_TEXT_OUTPUT;

typedef struct {
    u32 Type, Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    u64 VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* memory types */
enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
       EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
       EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
       EfiPalCode, EfiPersistentMemory };
/* AllocatePages */
enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void* RaiseTPL;
    void* RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(UINTN Type, UINTN MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS* Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN* Size, EFI_MEMORY_DESCRIPTOR* Map, UINTN* MapKey, UINTN* DescSize, u32* DescVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(UINTN PoolType, UINTN Size, void** Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void*);
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, const EFI_GUID*, void**);
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN MapKey);
    void* GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, u64, UINTN, const CHAR16*);
    void* ConnectController;
    void* DisconnectController;
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;
    void* ProtocolsPerHandle;
    void* LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID*, void* Registration, void** Interface);
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID VendorGuid; void* VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16* FirmwareVendor;
    u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT* ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT* StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE* ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    u32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    void* FilePath;
    void* Reserved;
    u32 LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    u64 ImageSize;
    u32 ImageCodeType, ImageDataType;
    void* Unload;
} EFI_LOADED_IMAGE;

typedef struct EFI_FILE {
    u64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE*, struct EFI_FILE**, const CHAR16*, u64 Mode, u64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE*);
    void* Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE*, UINTN*, void*);
    void* Write;
    void* GetPosition;
    void* SetPosition;
    EFI_STATUS (EFIAPI *GetInfo)(struct EFI_FILE*, const EFI_GUID*, UINTN*, void*);
} EFI_FILE;

typedef struct EFI_SIMPLE_FILE_SYSTEM {
    u64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM*, EFI_FILE**);
} EFI_SIMPLE_FILE_SYSTEM;

typedef struct {
    u64 Size, FileSize, PhysicalSize;
    /* times, attribute, name follow */
} EFI_FILE_INFO;

typedef struct {
    u32 Version, HorizontalResolution, VerticalResolution;
    u32 PixelFormat;              /* 0 RGBX, 1 BGRX, 2 bit mask, 3 blt only */
    u32 RedMask, GreenMask, BlueMask, ReservedMask;
    u32 PixelsPerScanLine;
} EFI_GOP_MODE_INFO;

typedef struct {
    u32 MaxMode, Mode;
    EFI_GOP_MODE_INFO* Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GOP_MODE;

typedef struct EFI_GOP {
    void* QueryMode;
    void* SetMode;
    void* Blt;
    EFI_GOP_MODE* Mode;
} EFI_GOP;

#endif
