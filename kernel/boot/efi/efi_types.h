/*
 * efi_types.h — Minimal UEFI type definitions.
 *
 * Just enough to call Boot Services, get GOP framebuffer,
 * locate ACPI tables, and exit boot services.
 * No external UEFI SDK required.
 */

#ifndef ANX_EFI_TYPES_H
#define ANX_EFI_TYPES_H

typedef unsigned char		UINT8;
typedef unsigned short		UINT16;
typedef unsigned int		UINT32;
typedef unsigned long long	UINT64;
typedef long long		INT64;
typedef UINT64			UINTN;
typedef UINT16			CHAR16;
typedef void			VOID;
typedef UINT8			BOOLEAN;
typedef UINTN			EFI_STATUS;
typedef VOID *			EFI_HANDLE;
typedef VOID *			EFI_EVENT;
typedef UINT64			EFI_PHYSICAL_ADDRESS;
typedef UINT64			EFI_VIRTUAL_ADDRESS;

#define TRUE	1
#define FALSE	0
#define NULL	((VOID *)0)
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI	__attribute__((ms_abi))

/* Status codes */
#define EFI_SUCCESS		0
#define EFI_BUFFER_TOO_SMALL	5

/* GUIDs */
typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8  Data4[8];
} EFI_GUID;

/* Memory descriptor */
typedef struct {
	UINT32			Type;
	EFI_PHYSICAL_ADDRESS	PhysicalStart;
	EFI_VIRTUAL_ADDRESS	VirtualStart;
	UINT64			NumberOfPages;
	UINT64			Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* GOP pixel format */
typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	UINT32	RedMask;
	UINT32	GreenMask;
	UINT32	BlueMask;
	UINT32	ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	UINT32				Version;
	UINT32				HorizontalResolution;
	UINT32				VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT	PixelFormat;
	EFI_PIXEL_BITMASK		PixelInformation;
	UINT32				PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32					MaxMode;
	UINT32					Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION	*Info;
	UINTN					SizeOfInfo;
	EFI_PHYSICAL_ADDRESS			FrameBufferBase;
	UINTN					FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* Forward declarations */
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct _EFI_BOOT_SERVICES;
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Simple Text Output */
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
	struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
	CHAR16 *String
);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	VOID		*Reset;
	EFI_TEXT_STRING	OutputString;
	/* ... more fields we don't need */
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* GOP Protocol */
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
	struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
	UINT32 ModeNumber,
	UINTN *SizeOfInfo,
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
	struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
	UINT32 ModeNumber
);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE	QueryMode;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE	SetMode;
	VOID					*Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE	*Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Boot Services (partial) */
typedef enum {
	AllHandles,
	ByRegisterNotify,
	ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
	EFI_HANDLE Handle,
	EFI_GUID *Protocol,
	VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
	EFI_LOCATE_SEARCH_TYPE SearchType,
	EFI_GUID *Protocol,
	VOID *SearchKey,
	UINTN *NoHandles,
	EFI_HANDLE **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
	EFI_GUID *Protocol,
	VOID *Registration,
	VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
	UINTN *MemoryMapSize,
	EFI_MEMORY_DESCRIPTOR *MemoryMap,
	UINTN *MapKey,
	UINTN *DescriptorSize,
	UINT32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
	EFI_HANDLE ImageHandle,
	UINTN MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
	UINT32 PoolType,
	UINTN Size,
	VOID **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
	VOID *Buffer
);

typedef struct _EFI_BOOT_SERVICES {
	/* Header (24 bytes) */
	UINT64	Hdr[3];

	/* Task priority (2 entries) */
	VOID	*RaiseTPL;
	VOID	*RestoreTPL;

	/* Memory (5 entries) */
	VOID		*AllocatePages;
	VOID		*FreePages;
	EFI_GET_MEMORY_MAP	GetMemoryMap;
	EFI_ALLOCATE_POOL	AllocatePool;
	EFI_FREE_POOL		FreePool;

	/* Events (6 entries) */
	VOID	*CreateEvent;
	VOID	*SetTimer;
	VOID	*WaitForEvent;
	VOID	*SignalEvent;
	VOID	*CloseEvent;
	VOID	*CheckEvent;

	/* Protocol handlers (6 entries) */
	VOID			*InstallProtocolInterface;
	VOID			*ReinstallProtocolInterface;
	VOID			*UninstallProtocolInterface;
	EFI_HANDLE_PROTOCOL	HandleProtocol;
	VOID			*Reserved;
	VOID			*RegisterProtocolNotify;

	/* Search (3 entries) */
	VOID	*LocateHandle;
	VOID	*LocateDevicePath;
	VOID	*InstallConfigurationTable;

	/* Image (5 entries) */
	VOID	*LoadImage;
	VOID	*StartImage;
	VOID	*Exit;
	VOID	*UnloadImage;
	EFI_EXIT_BOOT_SERVICES	ExitBootServices;

	/* Misc (2 entries) */
	VOID	*GetNextMonotonicCount;
	VOID	*Stall;

	/* Watchdog (1 entry) */
	VOID	*SetWatchdogTimer;

	/* Driver support (2 entries) */
	VOID	*ConnectController;
	VOID	*DisconnectController;

	/* Open/close protocol (3 entries) */
	VOID	*OpenProtocol;
	VOID	*CloseProtocol;
	VOID	*OpenProtocolInformation;

	/* Library (3 entries) */
	VOID			*ProtocolsPerHandle;
	EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
	EFI_LOCATE_PROTOCOL	LocateProtocol;
} EFI_BOOT_SERVICES;

/* Configuration Table */
typedef struct {
	EFI_GUID	VendorGuid;
	VOID		*VendorTable;
} EFI_CONFIGURATION_TABLE;

/* System Table */
typedef struct {
	/* Header (24 bytes) */
	UINT64				Hdr[3];

	CHAR16				*FirmwareVendor;
	UINT32				FirmwareRevision;

	EFI_HANDLE			ConsoleInHandle;
	VOID				*ConIn;
	EFI_HANDLE			ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL	*ConOut;
	EFI_HANDLE			ConsoleErrorHandle;
	VOID				*StdErr;
	VOID				*RuntimeServices;
	EFI_BOOT_SERVICES		*BootServices;
	UINTN				NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE		*ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Well-known GUIDs */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{0x9042a9de, 0x23dc, 0x4a38, \
	 {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}

#define EFI_ACPI_20_TABLE_GUID \
	{0x8868e871, 0xe4f1, 0x11d3, \
	 {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}

#endif /* ANX_EFI_TYPES_H */
