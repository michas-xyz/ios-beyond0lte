# Standalone XNU loader applications for the existing beyond2lte UEFI.
[Defines]
  PLATFORM_NAME                  = B2LXnu
  PLATFORM_GUID                  = E8783571-975C-40DA-847D-2B9AB2598171
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/B2LXnu
  SUPPORTED_ARCHITECTURES         = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  # Board display profile (uefi/XnuLoader/BoardDisplay.h); the defaults keep
  # the S10+ 1:1 panel at scale 3. scripts/build-efi.sh overrides them.
  DEFINE B2L_DISPLAY_SCALE       = 3
  DEFINE B2L_FB_LOGICAL_W        = 0
  DEFINE B2L_FB_LOGICAL_H        = 0

[LibraryClasses]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf

[Components]
  B2LXnuPkg/BootSurvey/BootSurvey.inf
  B2LXnuPkg/XnuLoader/XnuLoader.inf

[BuildOptions]
  *_*_*_CC_FLAGS = -DB2L_DISPLAY_SCALE=$(B2L_DISPLAY_SCALE) -DB2L_FB_LOGICAL_W=$(B2L_FB_LOGICAL_W) -DB2L_FB_LOGICAL_H=$(B2L_FB_LOGICAL_H) -Wno-error=unused-but-set-variable -Wno-error=unused-variable -Wno-error
