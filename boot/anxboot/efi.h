/*
 * efi.h — Minimal UEFI 2.x type and protocol definitions for anxboot.
 *
 * Hand-written subset of <Uefi.h> and <Protocol/...> sufficient to:
 *   - exit a UEFI image cleanly
 *   - print to ConOut
 *   - locate the file system on the boot device
 *   - read a file from the ESP
 *   - get a memory map and call ExitBootServices
 *
 * No external dependencies (no GNU EFI, no edk2). The goal is full
 * source-level auditability — every type and constant here is one we
 * can read.
 */

#ifndef ANXBOOT_EFI_H
#define ANXBOOT_EFI_H

#ifdef __SIZEOF_LONG__
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;
typedef long long      int64_t;
typedef int            int32_t;
typedef short          int16_t;
typedef signed char    int8_t;
typedef uint64_t       uintptr_t;
typedef int64_t        intptr_t;
typedef uint64_t       size_t;
#endif

typedef uint8_t  BOOLEAN;
typedef int64_t  INTN;
typedef uint64_t UINTN;
typedef int8_t   INT8;
typedef uint8_t  UINT8;
typedef int16_t  INT16;
typedef uint16_t UINT16;
typedef int32_t  INT32;
typedef uint32_t UINT32;
typedef int64_t  INT64;
typedef uint64_t UINT64;
typedef uint16_t CHAR16;	/* UEFI strings are UCS-2 / UTF-16. */
typedef char     CHAR8;
typedef void *   EFI_HANDLE;
typedef void *   EFI_EVENT;
typedef UINTN    EFI_STATUS;
typedef UINT64   EFI_PHYSICAL_ADDRESS;
typedef UINT64   EFI_VIRTUAL_ADDRESS;

#define TRUE  1
#define FALSE 0

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS               0x0000000000000000ULL
#define EFI_LOAD_ERROR            0x8000000000000001ULL
#define EFI_INVALID_PARAMETER     0x8000000000000002ULL
#define EFI_UNSUPPORTED           0x8000000000000003ULL
#define EFI_BAD_BUFFER_SIZE       0x8000000000000004ULL
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ULL
#define EFI_NOT_READY             0x8000000000000006ULL
#define EFI_DEVICE_ERROR          0x8000000000000007ULL
#define EFI_OUT_OF_RESOURCES      0x8000000000000009ULL
#define EFI_NOT_FOUND             0x800000000000000EULL
#define EFI_END_OF_FILE           0x8000000000000016ULL
#define EFI_ERROR(s)              (((INTN)(s)) < 0)

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8  Data4[8];
} EFI_GUID;

/* --- Time --- */
typedef struct {
	UINT16 Year;
	UINT8  Month;
	UINT8  Day;
	UINT8  Hour;
	UINT8  Minute;
	UINT8  Second;
	UINT8  Pad1;
	UINT32 Nanosecond;
	INT16  TimeZone;
	UINT8  Daylight;
	UINT8  Pad2;
} EFI_TIME;

/* --- Table headers --- */
typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

/* --- Memory map --- */
typedef enum {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef struct {
	UINT32 Type;
	UINT32 Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS  VirtualStart;
	UINT64 NumberOfPages;
	UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_PAGE_SIZE 4096
#define EFI_SIZE_TO_PAGES(x) (((x) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)

/* --- SimpleTextOutput (ConOut) --- */
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
	struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
	struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
	struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_TEXT_RESET        Reset;
	EFI_TEXT_STRING       OutputString;
	EFI_TEXT_STRING       TestString;
	void                 *QueryMode;
	void                 *SetMode;
	void                 *SetAttribute;
	EFI_TEXT_CLEAR_SCREEN ClearScreen;
	void                 *SetCursorPosition;
	void                 *EnableCursor;
	void                 *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* --- Boot Services --- */
typedef struct EFI_BOOT_SERVICES {
	EFI_TABLE_HEADER Hdr;

	/* Task Priority */
	void *RaiseTPL;
	void *RestoreTPL;

	/* Memory Services */
	EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type,
		EFI_MEMORY_TYPE MemType, UINTN Pages,
		EFI_PHYSICAL_ADDRESS *Memory);
	EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
	EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize,
		EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
		UINTN *DescriptorSize, UINT32 *DescriptorVersion);
	EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType,
		UINTN Size, void **Buffer);
	EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

	/* Event & Timer */
	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;

	/* Protocol Handler — old */
	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle,
		EFI_GUID *Protocol, void **Interface);
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;

	/* Image Services */
	void *InstallConfigurationTable;
	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

	/* Misc */
	void *GetNextMonotonicCount;
	void *Stall;
	void *SetWatchdogTimer;

	/* Driver Services */
	void *ConnectController;
	void *DisconnectController;

	/* Open/Close Protocol Services */
	EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
		void **Interface, EFI_HANDLE AgentHandle,
		EFI_HANDLE ControllerHandle, UINT32 Attributes);
	void *CloseProtocol;
	void *OpenProtocolInformation;

	/* Library Services */
	void *ProtocolsPerHandle;
	EFI_STATUS (EFIAPI *LocateHandleBuffer)(UINT32 SearchType,
		EFI_GUID *Protocol, void *SearchKey, UINTN *NoHandles,
		EFI_HANDLE **Buffer);
	EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration,
		void **Interface);
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;

	/* CRC */
	void *CalculateCrc32;

	/* Misc */
	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

#define EFI_OPEN_PROTOCOL_GET_PROTOCOL  0x00000002
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x00000001

/* --- System Table --- */
typedef struct EFI_SYSTEM_TABLE {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* --- File Protocol --- */
struct EFI_FILE_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(struct EFI_FILE_PROTOCOL *This,
	struct EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode,
	UINT64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(struct EFI_FILE_PROTOCOL *This,
	UINTN *BufferSize, void *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GETPOSITION)(struct EFI_FILE_PROTOCOL *This,
	UINT64 *Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SETPOSITION)(struct EFI_FILE_PROTOCOL *This,
	UINT64 Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GETINFO)(struct EFI_FILE_PROTOCOL *This,
	EFI_GUID *InformationType, UINTN *BufferSize, void *Buffer);

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

typedef struct EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_FILE_OPEN  Open;
	EFI_FILE_CLOSE Close;
	void          *Delete;
	EFI_FILE_READ  Read;
	void          *Write;
	EFI_FILE_GETPOSITION GetPosition;
	EFI_FILE_SETPOSITION SetPosition;
	EFI_FILE_GETINFO     GetInfo;
	void                *SetInfo;
	void                *Flush;
} EFI_FILE_PROTOCOL;

/* --- Simple File System Protocol --- */
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
	struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* GUIDs we use. */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define EFI_FILE_INFO_GUID \
	{0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}

/* EFI_LOADED_IMAGE_PROTOCOL — partial. We need DeviceHandle to find the ESP. */
typedef struct EFI_LOADED_IMAGE_PROTOCOL {
	UINT32      Revision;
	EFI_HANDLE  ParentHandle;
	void       *SystemTable;
	EFI_HANDLE  DeviceHandle;	/* the partition we booted from */
	void       *FilePath;
	void       *Reserved;
	UINT32      LoadOptionsSize;
	void       *LoadOptions;
	void       *ImageBase;
	UINT64      ImageSize;
	EFI_MEMORY_TYPE ImageCodeType;
	EFI_MEMORY_TYPE ImageDataType;
	void       *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* EFI_FILE_INFO — header subset of GetInfo response. */
typedef struct EFI_FILE_INFO {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	EFI_TIME CreateTime;
	EFI_TIME LastAccessTime;
	EFI_TIME ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[1];	/* variable-length */
} EFI_FILE_INFO;

#endif /* ANXBOOT_EFI_H */
