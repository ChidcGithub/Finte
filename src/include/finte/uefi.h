/* Minimal UEFI definitions used by Finte's own loader and kernel console. */
#ifndef FINTE_UEFI_H
#define FINTE_UEFI_H

#include <stdint.h>
#include <stddef.h>

#if defined(_MSC_VER) || defined(__aarch64__)
#define EFIAPI
#else
#define EFIAPI __attribute__((ms_abi))
#endif

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;
typedef int64_t INTN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_PHYSICAL_ADDRESS VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_CONVENTIONAL_MEMORY 7u
#define EFI_ALLOCATE_ANY_PAGES 0u
#define EFI_ALLOCATE_MAX_ADDRESS 1u
#define EFI_ALLOCATE_ADDRESS 2u
#define EFI_LOADER_CODE 1u
#define EFI_LOADER_DATA 2u

typedef struct {
    uint16_t ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef uint64_t EFI_LBA;

typedef struct efi_block_io_media {
    uint32_t MediaId;
    uint8_t RemovableMedia;
    uint8_t MediaPresent;
    uint8_t LogicalPartition;
    uint8_t ReadOnly;
    uint8_t WriteCaching;
    uint8_t Pad[3];
    uint32_t BlockSize;
    uint32_t IoAlign;
    EFI_LBA LastBlock;
    EFI_LBA LowestAlignedLba;
    uint32_t LogicalBlocksPerPhysicalBlock;
    uint32_t OptimalTransferLengthGranularity;
    EFI_LBA HighestLba;
    uint32_t OptimalTransferLength;
} EFI_BLOCK_IO_MEDIA;

typedef struct efi_block_io_protocol EFI_BLOCK_IO_PROTOCOL;
typedef EFI_STATUS (EFIAPI *efi_block_io_read_t)(
    EFI_BLOCK_IO_PROTOCOL *, uint32_t, EFI_LBA, UINTN, void *);
typedef EFI_STATUS (EFIAPI *efi_block_io_write_t)(
    EFI_BLOCK_IO_PROTOCOL *, uint32_t, EFI_LBA, UINTN, const void *);

struct efi_block_io_protocol {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    void *Reset;
    efi_block_io_read_t ReadBlocks;
    efi_block_io_write_t WriteBlocks;
    void *FlushBlocks;
};

typedef EFI_STATUS (EFIAPI *efi_locate_handle_buffer_t)(
    UINTN SearchType, EFI_GUID *Protocol, void *SearchKey,
    UINTN *NoHandles, EFI_HANDLE **Buffer);

typedef struct efi_simple_text_output_protocol efi_simple_text_output_protocol_t;
typedef EFI_STATUS (EFIAPI *efi_text_string_t)(efi_simple_text_output_protocol_t *,
                                               CHAR16 *);
typedef EFI_STATUS (EFIAPI *efi_text_clear_t)(efi_simple_text_output_protocol_t *);

struct efi_simple_text_output_protocol {
    void *Reset;
    efi_text_string_t OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    efi_text_clear_t ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

typedef struct efi_simple_text_input_protocol efi_simple_text_input_protocol_t;
typedef EFI_STATUS (EFIAPI *efi_input_read_key_t)(
    efi_simple_text_input_protocol_t *, EFI_INPUT_KEY *);

struct efi_simple_text_input_protocol {
    void *Reset;
    efi_input_read_key_t ReadKeyStroke;
    void *WaitForKey;
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(uint32_t Type, uint32_t MemoryType,
                                        UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    void *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize,
                                       EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                       UINTN *MapKey, UINTN *DescriptorSize,
                                       uint32_t *DescriptorVersion);
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                         void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle,
                                           UINTN MapKey);
    void *GetNextMonotonicCount;
    void *Stall;
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t Code,
                                           UINTN DataSize, CHAR16 *Data);
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    efi_locate_handle_buffer_t LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration,
                                         void **Interface);
} EFI_BOOT_SERVICES;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t Pad;
    EFI_HANDLE ConsoleInHandle;
    efi_simple_text_input_protocol_t *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    efi_simple_text_output_protocol_t *ConOut;
    EFI_HANDLE StandardErrorHandle;
    efi_simple_text_output_protocol_t *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* UEFI loaded-image protocol (DeviceHandle is the handle we need). */
typedef struct {
    uint32_t Revision;
    uint32_t Pad;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    uint32_t Pad2;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct efi_file_protocol efi_file_protocol_t;
typedef struct efi_file_protocol EFI_FILE_PROTOCOL;
typedef EFI_STATUS (EFIAPI *efi_file_open_t)(efi_file_protocol_t *This,
                                              efi_file_protocol_t **NewHandle,
                                              CHAR16 *FileName,
                                              uint64_t OpenMode,
                                              uint64_t Attributes);
typedef EFI_STATUS (EFIAPI *efi_file_close_t)(efi_file_protocol_t *);
typedef EFI_STATUS (EFIAPI *efi_file_read_t)(efi_file_protocol_t *, UINTN *, void *);

struct efi_file_protocol {
    uint64_t Revision;
    efi_file_open_t Open;
    efi_file_close_t Close;
    void *Delete;
    efi_file_read_t Read;
    void *Write;
    void *GetPosition;
    void *SetPosition;
    void *GetInfo;
    void *SetInfo;
    void *Flush;
};

typedef struct {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *This, efi_file_protocol_t **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* EFI status helpers. */
static inline int efi_ok(EFI_STATUS s)
{
    return (int64_t)s >= 0;
}

#define EFI_FILE_MODE_READ 0x0000000000000001ull

#endif /* FINTE_UEFI_H */
