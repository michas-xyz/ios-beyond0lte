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

#define PAYLOAD_LIMIT (128U * 1024U * 1024U)
#define PAYLOAD_HEADER_SIZE 320U
#define B2L_BOUNCE_PAGES 16
#define B2L_BOUNCE_BYTES (B2L_BOUNCE_PAGES * 4096)
STATIC UINT32 mBootUfsLun = MAX_UINT32;

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

// Safely read arbitrary partition offsets and lengths using a page-aligned 64 KiB bounce buffer
// and pure block-aligned BlockIo->ReadBlocks transfers. This guarantees full compatibility
// with any UFS host controller DMA alignment (IoAlign) and hardware PRDT limits.
STATIC EFI_STATUS B2lReadPartitionAligned(
  IN EFI_BLOCK_IO_PROTOCOL *Block,
  IN UINT32 MediaId,
  IN UINT64 FileOffset,
  IN UINTN Size,
  OUT VOID *Dest
) {
  if (Block == NULL || Dest == NULL || Size == 0) return EFI_INVALID_PARAMETER;

  EFI_PHYSICAL_ADDRESS BounceAddress = 0;
  EFI_STATUS Status = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, B2L_BOUNCE_PAGES, &BounceAddress);
  if (EFI_ERROR(Status)) return EFI_OUT_OF_RESOURCES;
  VOID *BounceBuffer = (VOID *)(UINTN)BounceAddress;

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
    Status = Block->ReadBlocks(Block, MediaId, Lba, BytesToRead, BounceBuffer);
    if (EFI_ERROR(Status)) {
      Print(L"B2L: Block read fail @ LBA 0x%lx (size %u): %r\r\n", Lba, (UINT32)BytesToRead, Status);
      break;
    }

    CopyMem(CurrentDest, (UINT8 *)BounceBuffer + OffsetInBlock, BytesToCopy);
    CurrentDest += BytesToCopy;
    CurrentOffset += BytesToCopy;
    Remaining -= BytesToCopy;
  }

  gBS->FreePages(BounceAddress, B2L_BOUNCE_PAGES);
  return Status;
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
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  Status = EFI_NOT_FOUND;
  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_BLOCK_IO_PROTOCOL *Block;
    EFI_PARTITION_INFO_PROTOCOL *Partition;
    UINT8 Header[48], Payload[PAYLOAD_HEADER_SIZE];
    UINT32 Page, MediaId;
    UINT64 Offset, KernelOffset, Length, Capacity;
    VOID *Buffer;
    EFI_STATUS ReadStatus;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block)) ||
        !Block->Media->MediaPresent) continue;
    // If partition info protocol is available, verify it is not an unrelated partition
    if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Partition))) {
      if (Partition->Type == PARTITION_TYPE_GPT) {
        CONST CHAR16 *Name = Partition->Info.Gpt.PartitionName;
        // Accept BOOT, boot, Boot, boot_a, etc. Reject explicitly named other partitions (system, userdata, cache, etc.)
        BOOLEAN MightBeBoot = ((Name[0] == L'B' || Name[0] == L'b') &&
                               (Name[1] == L'O' || Name[1] == L'o') &&
                               (Name[2] == L'O' || Name[2] == L'o') &&
                               (Name[3] == L'T' || Name[3] == L't'));
        if (!MightBeBoot && Name[0] != L'\0') continue;
      }
    }
    // Reject overflow before converting LastBlock into a byte capacity.
    if (Block->Media->BlockSize == 0 || Block->Media->LastBlock == MAX_UINT64 ||
        Block->Media->LastBlock + 1 > MAX_UINT64 / Block->Media->BlockSize) continue;
    Capacity = (Block->Media->LastBlock + 1) * Block->Media->BlockSize;
    MediaId = Block->Media->MediaId;
    ReadStatus = B2lReadPartitionAligned(Block, MediaId, 0, sizeof(Header), Header);
    if (EFI_ERROR(ReadStatus) || CompareMem(Header, "ANDROID!", 8) != 0) continue;
    Page = Le32(Header + 36);
    // First recognize only a well-bounded Android header before probing payload.
    if (Le32(Header + 40) != 1 || Page < 2048 || Page > 65536 || (Page & (Page - 1)) != 0 ||
        Le32(Header + 16) != 0 || Le32(Header + 24) != 0) continue;
    Offset = Page + (((UINT64)Le32(Header + 8) + Page - 1) & ~((UINT64)Page - 1));
    if (Offset > Capacity || sizeof(Payload) > Capacity - Offset) continue;
    ReadStatus = B2lReadPartitionAligned(Block, MediaId, Offset, sizeof(Payload), Payload);
    if (EFI_ERROR(ReadStatus)) continue;
    if (CompareMem(Payload, "B2LXNU01", 8) == 0) {
      Print(L"B2L: legacy BOOT payload has only 64 seed bytes; repackage B2LXNU02 with 256 bytes.\r\n");
      Status = EFI_COMPROMISED_DATA;
      B2lSetErrorStage("Legacy B2LXNU01 payload");
      break;
    }
    if (CompareMem(Payload, "B2LXNU02", 8) != 0) continue;
    // Once our magic is present, corruption must not silently select another OS.
    Status = EFI_COMPROMISED_DATA;
    KernelOffset = Le64(Payload + 24);
    Length = Le64(Payload + 16);
    if (Le32(Payload + 8) != 2 || Le32(Payload + 12) != PAYLOAD_HEADER_SIZE ||
        KernelOffset != PAYLOAD_HEADER_SIZE || Length < 4096 || Length > PAYLOAD_LIMIT ||
        KernelOffset > Capacity - Offset || Length > Capacity - Offset - KernelOffset) {
      B2lSetErrorStage("Invalid payload geometry");
      break;
    }
    // Decode only after recognizing and bounding our complete BOOT header.
    UINT8 CandidateMac[B2L_WIFI_MAC_BYTES];
    if (b2l_boot_wifi_decode(Payload + 32 + B2L_BOOT_SEED_SIZE, CandidateMac) < 0) {
      B2lSetErrorStage("Invalid WiFi MAC decode");
      break;
    }
    Buffer = AllocatePool((UINTN)Length);
    if (Buffer == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      B2lSetErrorStage("Kernel pool OOM");
      break;
    }

    ReadStatus = B2lReadPartitionAligned(Block, MediaId, Offset + KernelOffset, (UINTN)Length, Buffer);
    if (EFI_ERROR(ReadStatus)) {
      Print(L"B2L: Kernel payload read failed: %r\r\n", ReadStatus);
      FreePool(Buffer);
      Status = ReadStatus;
      B2lSetErrorStage("Kernel BlockIo read failed");
      break;
    }

    if (Le32(Buffer) != 0xfeedfacf || Le32((UINT8 *)Buffer + 4) != 0x0100000c) {
      Print(L"B2L: Bad Mach-O magic: 0x%08x 0x%08x\r\n", Le32(Buffer), Le32((UINT8 *)Buffer + 4));
      FreePool(Buffer);
      B2lSetErrorStage("Bad Mach-O magic");
      break;
    }
    CopyMem(Seed, Payload + 32, B2L_BOOT_SEED_SIZE);
    CopyMem(WifiMac, CandidateMac, B2L_WIFI_MAC_BYTES);
    *Kernel = Buffer;
    *KernelSize = (UINTN)Length;
    EFI_DEVICE_PATH_PROTOCOL *Path;
    if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path))) {
      UINTN PathSize = GetDevicePathSize(Path);
      UINT32 Lun;
      if (b2l_boot_ufs_lun(Path, PathSize, &Lun) == 0) mBootUfsLun = Lun;
    }
    Print(L"B2L: read %u kernel bytes from BOOT trailer payload\r\n", (UINT32)Length);
    Status = EFI_SUCCESS;
    break;
  }
  FreePool(Handles);
  return Status;
}
