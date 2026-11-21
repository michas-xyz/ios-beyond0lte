// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read the XNU payload from an Android v1 BOOT partition through firmware UFS.
#include "BootPayload.h"
#include "BootMetadata.h"
#include "BootDevicePath.h"
#include "BootMenuEfi.h"
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>

#define PAYLOAD_LIMIT (128U * 1024U * 1024U)
#define PAYLOAD_HEADER_SIZE 320U
#define B2L_BOUNCE_PAGES 16
#define B2L_BOUNCE_BYTES (B2L_BOUNCE_PAGES * 4096)
STATIC UINT32 mBootUfsLun = MAX_UINT32;

#pragma pack(1)
typedef struct {
  UINT64 Signature;          // "EFI PART" = 0x5452415020494645ULL
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 HeaderCRC32;
  UINT32 Reserved;
  UINT64 MyLBA;
  UINT64 AlternateLBA;
  UINT64 FirstUsableLBA;
  UINT64 LastUsableLBA;
  UINT8  DiskGUID[16];
  UINT64 PartitionEntryLBA;
  UINT32 NumberOfPartitionEntries;
  UINT32 SizeOfPartitionEntry;
  UINT32 PartitionEntryArrayCRC32;
} B2L_GPT_HEADER;

typedef struct {
  UINT8  PartitionTypeGUID[16];
  UINT8  UniquePartitionGUID[16];
  UINT64 StartingLBA;
  UINT64 EndingLBA;
  UINT64 Attributes;
  CHAR16 PartitionName[36];
} B2L_GPT_ENTRY;
#pragma pack()

EFI_STATUS B2lGetBootUfsLun(UINT32 *Lun) {
  if (Lun == NULL) return EFI_INVALID_PARAMETER;
  if (mBootUfsLun > 7) return EFI_NOT_FOUND;
  *Lun = mBootUfsLun;
  return EFI_SUCCESS;
}

STATIC UINT32 Le32(CONST UINT8 *P) {
  return (UINT32)P[0] | (UINT32)P[1] << 8 | (UINT32)P[2] << 16 | (UINT32)P[3] << 24;
}
STATIC UINT64 Le64(CONST UINT8 *P) {
  return (UINT64)Le32(P) | (UINT64)Le32(P + 4) << 32;
}

// Safely read arbitrary partition offsets and lengths using a pre-allocated 64 KiB bounce buffer
// and pure block-aligned BlockIo->ReadBlocks transfers.
STATIC EFI_STATUS B2lReadAligned(
  IN EFI_BLOCK_IO_PROTOCOL *Block,
  IN UINT32 MediaId,
  IN UINT64 FileOffset,
  IN UINTN Size,
  IN VOID *BounceBuffer,
  OUT VOID *Dest
) {
  if (Block == NULL || Dest == NULL || Size == 0 || BounceBuffer == NULL) return EFI_INVALID_PARAMETER;

  UINT32 BlockSize = Block->Media->BlockSize;
  if (BlockSize == 0 || (BlockSize & (BlockSize - 1)) != 0) BlockSize = 4096;

  UINT64 CurrentOffset = FileOffset;
  UINTN Remaining = Size;
  UINT8 *CurrentDest = (UINT8 *)Dest;

  while (Remaining > 0) {
    UINT64 BlockAlignedOffset = CurrentOffset & ~((UINT64)BlockSize - 1);
    UINTN OffsetInBlock = (UINTN)(CurrentOffset - BlockAlignedOffset);
    UINTN SpaceInBounce = B2L_BOUNCE_BYTES - OffsetInBlock;
    UINTN BytesToCopy = (Remaining < SpaceInBounce) ? Remaining : SpaceInBounce;

    UINTN BytesToRead = (OffsetInBlock + BytesToCopy + BlockSize - 1) & ~((UINTN)BlockSize - 1);
    if (BytesToRead > B2L_BOUNCE_BYTES) BytesToRead = B2L_BOUNCE_BYTES;

    EFI_LBA Lba = BlockAlignedOffset / BlockSize;
    EFI_STATUS Status = Block->ReadBlocks(Block, MediaId, Lba, BytesToRead, BounceBuffer);
    if (EFI_ERROR(Status)) {
      Print(L"B2L: Block read fail @ LBA 0x%lx (size %u): %r\r\n", Lba, (UINT32)BytesToRead, Status);
      return Status;
    }

    CopyMem(CurrentDest, (UINT8 *)BounceBuffer + OffsetInBlock, BytesToCopy);
    CurrentDest += BytesToCopy;
    CurrentOffset += BytesToCopy;
    Remaining -= BytesToCopy;
  }

  return EFI_SUCCESS;
}

// Read data via DiskIo if available, falling back to BlockIo aligned bounce buffer
STATIC EFI_STATUS B2lReadData(
  IN EFI_BLOCK_IO_PROTOCOL *Block,
  IN EFI_DISK_IO_PROTOCOL *Disk OPTIONAL,
  IN UINT32 MediaId,
  IN UINT64 ByteOffset,
  IN UINTN Size,
  IN VOID *BounceBuffer,
  OUT VOID *Dest
) {
  if (Disk != NULL) {
    UINTN Remaining = Size;
    UINT64 CurrentOffset = ByteOffset;
    UINT8 *CurrentDest = (UINT8 *)Dest;
    EFI_STATUS Status = EFI_SUCCESS;
    while (Remaining > 0) {
      UINTN Chunk = (Remaining > B2L_BOUNCE_BYTES) ? B2L_BOUNCE_BYTES : Remaining;
      Status = Disk->ReadDisk(Disk, MediaId, CurrentOffset, Chunk, CurrentDest);
      if (EFI_ERROR(Status)) break;
      CurrentOffset += Chunk;
      CurrentDest += Chunk;
      Remaining -= Chunk;
    }
    if (!EFI_ERROR(Status)) return EFI_SUCCESS;
  }
  return B2lReadAligned(Block, MediaId, ByteOffset, Size, BounceBuffer, Dest);
}

// Try reading and verifying Android header and B2LXNU02 trailer from a candidate device/offset
STATIC EFI_STATUS B2lTryLoad(
  IN EFI_BLOCK_IO_PROTOCOL *Block,
  IN EFI_DISK_IO_PROTOCOL *Disk OPTIONAL,
  IN UINT32 MediaId,
  IN UINT64 BaseOffset,
  IN UINT64 Capacity,
  IN VOID *BounceBuffer,
  OUT VOID **Kernel,
  OUT UINTN *KernelSize,
  OUT UINT8 Seed[B2L_BOOT_SEED_SIZE],
  OUT UINT8 WifiMac[6]
) {
  UINT8 Header[48], Payload[PAYLOAD_HEADER_SIZE];
  UINT32 Page;
  UINT64 Offset, KernelOffset, Length;
  VOID *Buffer;
  EFI_STATUS Status;

  if (Capacity < sizeof(Header)) return EFI_BAD_BUFFER_SIZE;

  Status = B2lReadData(Block, Disk, MediaId, BaseOffset, sizeof(Header), BounceBuffer, Header);
  if (EFI_ERROR(Status)) return Status;
  if (CompareMem(Header, "ANDROID!", 8) != 0) return EFI_NOT_FOUND;

  Page = Le32(Header + 36);
  if (Le32(Header + 40) != 1 || Page < 2048 || Page > 65536 || (Page & (Page - 1)) != 0 ||
      Le32(Header + 16) != 0 || Le32(Header + 24) != 0) {
    return EFI_NOT_FOUND;
  }

  Offset = Page + (((UINT64)Le32(Header + 8) + Page - 1) & ~((UINT64)Page - 1));
  if (Offset > Capacity || sizeof(Payload) > Capacity - Offset) {
    B2lSetErrorStage("Trailer offset beyond partition");
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = B2lReadData(Block, Disk, MediaId, BaseOffset + Offset, sizeof(Payload), BounceBuffer, Payload);
  if (EFI_ERROR(Status)) {
    B2lSetErrorStage("Trailer read error");
    return Status;
  }

  if (CompareMem(Payload, "B2LXNU01", 8) == 0) {
    Print(L"B2L: legacy BOOT payload has only 64 seed bytes; repackage B2LXNU02 with 256 bytes.\r\n");
    B2lSetErrorStage("Legacy B2LXNU01 payload");
    return EFI_COMPROMISED_DATA;
  }
  if (CompareMem(Payload, "B2LXNU02", 8) != 0) {
    return EFI_NOT_FOUND;
  }

  KernelOffset = Le64(Payload + 24);
  Length = Le64(Payload + 16);
  if (Le32(Payload + 8) != 2 || Le32(Payload + 12) != PAYLOAD_HEADER_SIZE ||
      KernelOffset != PAYLOAD_HEADER_SIZE || Length < 4096 || Length > PAYLOAD_LIMIT ||
      KernelOffset > Capacity - Offset || Length > Capacity - Offset - KernelOffset) {
    B2lSetErrorStage("Invalid payload geometry");
    return EFI_COMPROMISED_DATA;
  }

  UINT8 CandidateMac[B2L_WIFI_MAC_BYTES];
  if (b2l_boot_wifi_decode(Payload + 32 + B2L_BOOT_SEED_SIZE, CandidateMac) < 0) {
    B2lSetErrorStage("Invalid WiFi MAC decode");
    return EFI_COMPROMISED_DATA;
  }

  Buffer = AllocatePool((UINTN)Length);
  if (Buffer == NULL) {
    B2lSetErrorStage("Kernel pool OOM");
    return EFI_OUT_OF_RESOURCES;
  }

  B2lSetErrorStage("Reading XNU Mach-O");
  Status = B2lReadData(Block, Disk, MediaId, BaseOffset + Offset + KernelOffset, (UINTN)Length, BounceBuffer, Buffer);
  if (EFI_ERROR(Status)) {
    FreePool(Buffer);
    B2lSetErrorStage("Kernel payload read failed");
    return Status;
  }

  if (Le32(Buffer) != 0xfeedfacf || Le32((UINT8 *)Buffer + 4) != 0x0100000c) {
    Print(L"B2L: Bad Mach-O magic: 0x%08x 0x%08x\r\n", Le32(Buffer), Le32((UINT8 *)Buffer + 4));
    FreePool(Buffer);
    B2lSetErrorStage("Bad Mach-O magic");
    return EFI_COMPROMISED_DATA;
  }

  CopyMem(Seed, Payload + 32, B2L_BOOT_SEED_SIZE);
  CopyMem(WifiMac, CandidateMac, B2L_WIFI_MAC_BYTES);
  *Kernel = Buffer;
  *KernelSize = (UINTN)Length;
  Print(L"B2L: read %u kernel bytes from BOOT trailer payload\r\n", (UINT32)Length);
  return EFI_SUCCESS;
}

// Recursively connect all hardware controllers so UfsDxe and PartitionDxe bind to storage
STATIC VOID B2lConnectAll(VOID) {
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_STATUS Status;

  Status = gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return;

  for (UINTN Index = 0; Index < Count; Index++) {
    (VOID)gBS->ConnectController(Handles[Index], NULL, NULL, TRUE);
  }

  FreePool(Handles);
}

// Search physical RAM for an in-memory B2LXNU02 payload staged by S-Boot
STATIC EFI_STATUS B2lTryLoadFromRam(
  OUT VOID **Kernel,
  OUT UINTN *KernelSize,
  OUT UINT8 Seed[B2L_BOOT_SEED_SIZE],
  OUT UINT8 WifiMac[6]
) {
  EFI_STATUS Status;
  UINTN MapSize = 0, MapKey, DescSize;
  UINT32 DescVersion;
  EFI_MEMORY_DESCRIPTOR *MemMap = NULL;

  Status = gBS->GetMemoryMap(&MapSize, MemMap, &MapKey, &DescSize, &DescVersion);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    MapSize += 8192;
    MemMap = AllocatePool(MapSize);
    if (MemMap == NULL) return EFI_OUT_OF_RESOURCES;
    Status = gBS->GetMemoryMap(&MapSize, MemMap, &MapKey, &DescSize, &DescVersion);
  }
  if (EFI_ERROR(Status) || MemMap == NULL) {
    if (MemMap != NULL) FreePool(MemMap);
    return EFI_NOT_FOUND;
  }

  UINTN NumEntries = MapSize / DescSize;
  for (UINTN i = 0; i < NumEntries; i++) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemMap + i * DescSize);
    if (Desc->Type != EfiConventionalMemory &&
        Desc->Type != EfiLoaderData &&
        Desc->Type != EfiBootServicesData) {
      continue;
    }

    UINT64 Base = Desc->PhysicalStart;
    UINT64 RegionSize = Desc->NumberOfPages * 4096;
    if (RegionSize < PAYLOAD_HEADER_SIZE + 4096) continue;

    for (UINT64 Off = 0; Off + PAYLOAD_HEADER_SIZE + 4096 <= RegionSize; Off += 512) {
      UINT8 *Candidate = (UINT8 *)(UINTN)(Base + Off);
      if (CompareMem(Candidate, "B2LXNU02", 8) != 0) continue;

      UINT64 KernelOffset = Le64(Candidate + 24);
      UINT64 Length = Le64(Candidate + 16);
      if (Le32(Candidate + 8) != 2 || Le32(Candidate + 12) != PAYLOAD_HEADER_SIZE ||
          KernelOffset != PAYLOAD_HEADER_SIZE || Length < 4096 || Length > PAYLOAD_LIMIT ||
          Off + KernelOffset + Length > RegionSize) {
        continue;
      }

      UINT8 *MachO = Candidate + KernelOffset;
      if (Le32(MachO) != 0xfeedfacf || Le32(MachO + 4) != 0x0100000c) {
        continue;
      }

      UINT8 CandidateMac[B2L_WIFI_MAC_BYTES];
      if (b2l_boot_wifi_decode(Candidate + 32 + B2L_BOOT_SEED_SIZE, CandidateMac) < 0) {
        continue;
      }

      VOID *Buffer = AllocatePool((UINTN)Length);
      if (Buffer == NULL) {
        FreePool(MemMap);
        return EFI_OUT_OF_RESOURCES;
      }
      CopyMem(Buffer, MachO, (UINTN)Length);
      CopyMem(Seed, Candidate + 32, B2L_BOOT_SEED_SIZE);
      CopyMem(WifiMac, CandidateMac, B2L_WIFI_MAC_BYTES);
      *Kernel = Buffer;
      *KernelSize = (UINTN)Length;
      FreePool(MemMap);
      Print(L"B2L: found and loaded XNU from RAM at 0x%lx (%u bytes)\r\n", Base + Off, (UINT32)Length);
      return EFI_SUCCESS;
    }
  }

  FreePool(MemMap);
  return EFI_NOT_FOUND;
}

EFI_STATUS B2lLoadBootPayload(VOID **Kernel, UINTN *KernelSize, UINT8 Seed[B2L_BOOT_SEED_SIZE], UINT8 WifiMac[6]) {
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_STATUS Status;
  if (Kernel == NULL || KernelSize == NULL || Seed == NULL || WifiMac == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(WifiMac, B2L_WIFI_MAC_BYTES);
  *Kernel = NULL;
  *KernelSize = 0;
  mBootUfsLun = MAX_UINT32;

  // Connect all controllers first so UfsDxe and PartitionDxe bind to UFS host hardware
  B2lConnectAll();

  // Probe connected BlockIo handles to detect the boot UFS LUN
  EFI_HANDLE *BlockHandles = NULL;
  UINTN BlockCount = 0;
  if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &BlockCount, &BlockHandles))) {
    for (UINTN i = 0; i < BlockCount; i++) {
      EFI_DEVICE_PATH_PROTOCOL *Path;
      if (!EFI_ERROR(gBS->HandleProtocol(BlockHandles[i], &gEfiDevicePathProtocolGuid, (VOID **)&Path))) {
        UINT32 Lun;
        if (b2l_boot_ufs_lun(Path, GetDevicePathSize(Path), &Lun) == 0) {
          mBootUfsLun = Lun;
          break;
        }
      }
    }
    FreePool(BlockHandles);
  }
  if (mBootUfsLun > 7) mBootUfsLun = 0; // Default to LUN 0 (sda) where GPT partitions live

  // First try loading XNU payload directly from physical RAM if staged by S-Boot
  B2lSetErrorStage("Probing RAM for XNU");
  Status = B2lTryLoadFromRam(Kernel, KernelSize, Seed, WifiMac);
  if (!EFI_ERROR(Status)) {
    return EFI_SUCCESS;
  }

  // Allocate 64 KiB bounce buffer below 4 GiB for safe 32-bit DMA alignment
  EFI_PHYSICAL_ADDRESS BounceAddress = 0xFFFFFFFF;
  Status = gBS->AllocatePages(AllocateMaxAddress, EfiBootServicesData, B2L_BOUNCE_PAGES, &BounceAddress);
  if (EFI_ERROR(Status)) {
    Status = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, B2L_BOUNCE_PAGES, &BounceAddress);
    if (EFI_ERROR(Status)) {
      B2lSetErrorStage("Bounce buffer alloc failed");
      return Status;
    }
  }
  VOID *BounceBuffer = (VOID *)(UINTN)BounceAddress;

  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) {
    gBS->FreePages(BounceAddress, B2L_BOUNCE_PAGES);
    B2lSetErrorStage("Locate BlockIo failed");
    return Status;
  }

  // Phase 1: Try all child partition handles and direct block devices
  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_BLOCK_IO_PROTOCOL *Block;
    EFI_DISK_IO_PROTOCOL *Disk = NULL;
    EFI_PARTITION_INFO_PROTOCOL *Partition;
    UINT32 MediaId;
    UINT64 Capacity;

    if (EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block)) ||
        !Block->Media || !Block->Media->MediaPresent) continue;

    (VOID)gBS->ConnectController(Handles[Index], NULL, NULL, TRUE);
    (VOID)gBS->HandleProtocol(Handles[Index], &gEfiDiskIoProtocolGuid, (VOID **)&Disk);

    // If partition info protocol is available, verify it is not an unrelated partition
    if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Partition))) {
      if (Partition->Type == PARTITION_TYPE_GPT) {
        CONST CHAR16 *Name = Partition->Info.Gpt.PartitionName;
        BOOLEAN MightBeBoot = ((Name[0] == L'B' || Name[0] == L'b') &&
                               (Name[1] == L'O' || Name[1] == L'o') &&
                               (Name[2] == L'O' || Name[2] == L'o') &&
                               (Name[3] == L'T' || Name[3] == L't'));
        if (!MightBeBoot && Name[0] != L'\0') continue;
      }
    }

    if (Block->Media->BlockSize == 0 || Block->Media->LastBlock == MAX_UINT64 ||
        Block->Media->LastBlock + 1 > MAX_UINT64 / Block->Media->BlockSize) continue;
    Capacity = (Block->Media->LastBlock + 1) * Block->Media->BlockSize;
    MediaId = Block->Media->MediaId;

    Status = B2lTryLoad(Block, Disk, MediaId, 0, Capacity, BounceBuffer, Kernel, KernelSize, Seed, WifiMac);
    if (!EFI_ERROR(Status)) {
      EFI_DEVICE_PATH_PROTOCOL *Path;
      if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path))) {
        UINTN PathSize = GetDevicePathSize(Path);
        UINT32 Lun;
        if (b2l_boot_ufs_lun(Path, PathSize, &Lun) == 0) mBootUfsLun = Lun;
      }
      gBS->FreePages(BounceAddress, B2L_BOUNCE_PAGES);
      FreePool(Handles);
      return EFI_SUCCESS;
    }
  }

  // Phase 2: If no child partition matched, scan GPT on raw LUNs (e.g. LUN 0 sda)
  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_BLOCK_IO_PROTOCOL *Block;
    EFI_DISK_IO_PROTOCOL *Disk = NULL;
    UINT32 MediaId, BlockSize;

    if (EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block)) ||
        !Block->Media || !Block->Media->MediaPresent) continue;

    (VOID)gBS->HandleProtocol(Handles[Index], &gEfiDiskIoProtocolGuid, (VOID **)&Disk);
    BlockSize = Block->Media->BlockSize;
    if (BlockSize == 0 || (BlockSize & (BlockSize - 1)) != 0) BlockSize = 4096;
    MediaId = Block->Media->MediaId;

    // Read LBA 1 (GPT Header)
    EFI_STATUS GptStatus = B2lReadAligned(Block, MediaId, (UINT64)BlockSize, BlockSize, BounceBuffer, BounceBuffer);
    if (EFI_ERROR(GptStatus)) continue;

    B2L_GPT_HEADER *Gpt = (B2L_GPT_HEADER *)BounceBuffer;
    if (Gpt->Signature != 0x5452415020494645ULL) continue; // "EFI PART"

    UINT64 EntryLba = Gpt->PartitionEntryLBA;
    UINT32 NumEntries = Gpt->NumberOfPartitionEntries;
    UINT32 EntrySize = Gpt->SizeOfPartitionEntry;
    if (EntryLba < 2 || NumEntries == 0 || NumEntries > 256 || EntrySize < sizeof(B2L_GPT_ENTRY)) continue;

    // Read partition entries array
    UINTN EntriesBytes = (UINTN)NumEntries * EntrySize;
    VOID *EntriesBuf = AllocatePool(EntriesBytes);
    if (EntriesBuf == NULL) continue;

    GptStatus = B2lReadAligned(Block, MediaId, EntryLba * BlockSize, EntriesBytes, BounceBuffer, EntriesBuf);
    if (!EFI_ERROR(GptStatus)) {
      for (UINT32 e = 0; e < NumEntries; e++) {
        B2L_GPT_ENTRY *Entry = (B2L_GPT_ENTRY *)((UINT8 *)EntriesBuf + e * EntrySize);
        if (Entry->StartingLBA == 0 || Entry->EndingLBA < Entry->StartingLBA) continue;
        CONST CHAR16 *Name = Entry->PartitionName;
        BOOLEAN IsBoot = ((Name[0] == L'B' || Name[0] == L'b') &&
                          (Name[1] == L'O' || Name[1] == L'o') &&
                          (Name[2] == L'O' || Name[2] == L'o') &&
                          (Name[3] == L'T' || Name[3] == L't') &&
                          Name[4] == L'\0');
        if (!IsBoot) continue;

        UINT64 PartOffset = Entry->StartingLBA * BlockSize;
        UINT64 PartCap = (Entry->EndingLBA - Entry->StartingLBA + 1) * BlockSize;

        Status = B2lTryLoad(Block, Disk, MediaId, PartOffset, PartCap, BounceBuffer, Kernel, KernelSize, Seed, WifiMac);
        if (!EFI_ERROR(Status)) {
          FreePool(EntriesBuf);
          EFI_DEVICE_PATH_PROTOCOL *Path;
          if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path))) {
            UINTN PathSize = GetDevicePathSize(Path);
            UINT32 Lun;
            if (b2l_boot_ufs_lun(Path, PathSize, &Lun) == 0) mBootUfsLun = Lun;
          }
          if (mBootUfsLun > 7) mBootUfsLun = 0; // Default to LUN 0 for raw sda
          gBS->FreePages(BounceAddress, B2L_BOUNCE_PAGES);
          FreePool(Handles);
          return EFI_SUCCESS;
        }
      }
    }
    FreePool(EntriesBuf);
  }

  gBS->FreePages(BounceAddress, B2L_BOUNCE_PAGES);
  FreePool(Handles);

  CHAR8 StageBuf[128];
  AsciiSPrint(StageBuf, sizeof(StageBuf), "BOOT not found (%u handles)", (UINT32)Count);
  B2lSetErrorStage(StageBuf);
  return EFI_NOT_FOUND;
}
