add_library(core
  AchievementManager.cpp
  AchievementManager.h
  ActionReplay.cpp
  ActionReplay.h
  ARDecrypt.cpp
  ARDecrypt.h
  Boot/AncastTypes.h
  Boot/Boot_BS2Emu.cpp
  Boot/Boot_WiiWAD.cpp
  Boot/Boot.cpp
  Boot/Boot.h
  Boot/DolReader.cpp
  Boot/DolReader.h
  Boot/ElfReader.cpp
  Boot/ElfReader.h
  Boot/ElfTypes.h
  BootManager.cpp
  BootManager.h
  CheatCodes.h
  CheatGeneration.cpp
  CheatGeneration.h
  CheatSearch.cpp
  CheatSearch.h
  CommonTitles.h
  Config/AchievementSettings.cpp
  Config/AchievementSettings.h
  Config/DefaultLocale.cpp
  Config/DefaultLocale.h
  Config/FreeLookSettings.cpp
  Config/FreeLookSettings.h
  Config/GraphicsSettings.cpp
  Config/GraphicsSettings.h
  Config/MainSettings.cpp
  Config/MainSettings.h
  Config/NetplaySettings.cpp
  Config/NetplaySettings.h
  Config/SessionSettings.cpp
  Config/SessionSettings.h
  Config/SYSCONFSettings.cpp
  Config/SYSCONFSettings.h
  Config/UISettings.cpp
  Config/UISettings.h
  Config/WiimoteSettings.cpp
  Config/WiimoteSettings.h
  ConfigLoaders/BaseConfigLoader.cpp
  ConfigLoaders/BaseConfigLoader.h
  ConfigLoaders/GameConfigLoader.cpp
  ConfigLoaders/GameConfigLoader.h
  ConfigLoaders/IsSettingSaveable.cpp
  ConfigLoaders/IsSettingSaveable.h
  ConfigLoaders/MovieConfigLoader.cpp
  ConfigLoaders/MovieConfigLoader.h
  ConfigLoaders/NetPlayConfigLoader.cpp
  ConfigLoaders/NetPlayConfigLoader.h
  ConfigManager.cpp
  ConfigManager.h
  Core.cpp
  Core.h
  CoreTiming.cpp
  CoreTiming.h
  CPUThreadConfigCallback.cpp
  CPUThreadConfigCallback.h
  Debugger/BranchWatch.cpp
  Debugger/BranchWatch.h
  Debugger/CodeTrace.cpp
  Debugger/CodeTrace.h
  Debugger/DebugInterface.h
  Debugger/Debugger_SymbolMap.cpp
  Debugger/Debugger_SymbolMap.h
  Debugger/Dump.cpp
  Debugger/Dump.h
  Debugger/OSThread.cpp
  Debugger/OSThread.h
  Debugger/PPCDebugInterface.cpp
  Debugger/PPCDebugInterface.h
  Debugger/RSO.cpp
  Debugger/RSO.h
  DolphinAnalytics.cpp
  DolphinAnalytics.h
  DSP/DSPAccelerator.cpp
  DSP/DSPAccelerator.h
  DSP/DSPAnalyzer.cpp
  DSP/DSPAnalyzer.h
  DSP/DSPAssembler.cpp
  DSP/DSPAssembler.h
  DSP/DSPCaptureLogger.cpp
  DSP/DSPCaptureLogger.h
  DSP/DSPCodeUtil.cpp
  DSP/DSPCodeUtil.h
  DSP/DSPCommon.h
  DSP/DSPCore.cpp
  DSP/DSPCore.h
  DSP/DSPDisassembler.cpp
  DSP/DSPDisassembler.h
  DSP/DSPHWInterface.cpp
  DSP/DSPMemoryMap.cpp
  DSP/DSPStacks.cpp
  DSP/DSPTables.cpp
  DSP/DSPTables.h
  DSP/Interpreter/DSPIntArithmetic.cpp
  DSP/Interpreter/DSPIntBranch.cpp
  DSP/Interpreter/DSPIntCCUtil.h
  DSP/Interpreter/DSPInterpreter.cpp
  DSP/Interpreter/DSPInterpreter.h
  DSP/Interpreter/DSPIntExtOps.cpp
  DSP/Interpreter/DSPIntLoadStore.cpp
  DSP/Interpreter/DSPIntMisc.cpp
  DSP/Interpreter/DSPIntMultiplier.cpp
  DSP/Interpreter/DSPIntTables.cpp
  DSP/Interpreter/DSPIntTables.h
  DSP/Interpreter/DSPIntUtil.h
  DSP/Jit/DSPEmitterBase.cpp
  DSP/Jit/DSPEmitterBase.h
  DSP/LabelMap.cpp
  DSP/LabelMap.h
  DSPEmulator.cpp
  DSPEmulator.h
  FifoPlayer/FifoDataFile.cpp
  FifoPlayer/FifoDataFile.h
  FifoPlayer/FifoPlayer.cpp
  FifoPlayer/FifoPlayer.h
  FifoPlayer/FifoRecorder.cpp
  FifoPlayer/FifoRecorder.h
  FreeLookManager.cpp
  FreeLookManager.h
  GeckoCode.cpp
  GeckoCode.h
  GeckoCodeConfig.cpp
  GeckoCodeConfig.h
  HLE/HLE_Misc.cpp
  HLE/HLE_Misc.h
  HLE/HLE_OS.cpp
  HLE/HLE_OS.h
  HLE/HLE_VarArgs.cpp
  HLE/HLE_VarArgs.h
  HLE/HLE.cpp
  HLE/HLE.h
  Host.h
  HotkeyManager.cpp
  HotkeyManager.h
  HW/AddressSpace.cpp
  HW/AddressSpace.h
  HW/AudioInterface.cpp
  HW/AudioInterface.h
  HW/CPU.cpp
  HW/CPU.h
  HW/DSP.cpp
  HW/DSP.h
  HW/DSPHLE/DSPHLE.cpp
  HW/DSPHLE/DSPHLE.h
  HW/DSPHLE/MailHandler.cpp
  HW/DSPHLE/MailHandler.h
  HW/DSPHLE/UCodes/ASnd.cpp
  HW/DSPHLE/UCodes/ASnd.h
  HW/DSPHLE/UCodes/AESnd.cpp
  HW/DSPHLE/UCodes/AESnd.h
  HW/DSPHLE/UCodes/AX.cpp
  HW/DSPHLE/UCodes/AX.h
  HW/DSPHLE/UCodes/AXStructs.h
  HW/DSPHLE/UCodes/AXVoice.h
  HW/DSPHLE/UCodes/AXWii.cpp
  HW/DSPHLE/UCodes/AXWii.h
  HW/DSPHLE/UCodes/CARD.cpp
  HW/DSPHLE/UCodes/CARD.h
  HW/DSPHLE/UCodes/GBA.cpp
  HW/DSPHLE/UCodes/GBA.h
  HW/DSPHLE/UCodes/INIT.cpp
  HW/DSPHLE/UCodes/INIT.h
  HW/DSPHLE/UCodes/ROM.cpp
  HW/DSPHLE/UCodes/ROM.h
  HW/DSPHLE/UCodes/UCodes.cpp
  HW/DSPHLE/UCodes/UCodes.h
  HW/DSPHLE/UCodes/Zelda.cpp
  HW/DSPHLE/UCodes/Zelda.h
  HW/DSPLLE/DSPHost.cpp
  HW/DSPLLE/DSPLLE.cpp
  HW/DSPLLE/DSPLLE.h
  HW/DSPLLE/DSPSymbols.cpp
  HW/DSPLLE/DSPSymbols.h
  HW/DVD/DVDInterface.cpp
  HW/DVD/DVDInterface.h
  HW/DVD/AMMediaboard.cpp
  HW/DVD/AMMediaboard.h
  HW/DVD/DVDMath.cpp
  HW/DVD/DVDMath.h
  HW/DVD/DVDThread.cpp
  HW/DVD/DVDThread.h
  HW/DVD/FileMonitor.cpp
  HW/DVD/FileMonitor.h
  HW/EXI/BBA/TAPServerConnection.cpp
  HW/EXI/BBA/TAPServerBBA.cpp
  HW/EXI/BBA/XLINK_KAI_BBA.cpp
  HW/EXI/BBA/BuiltIn.cpp
  HW/EXI/BBA/BuiltIn.h
  HW/EXI/Modem/TAPServerModem.cpp
  HW/EXI/EXI_Channel.cpp
  HW/EXI/EXI_Channel.h
  HW/EXI/EXI_Device.cpp
  HW/EXI/EXI_Device.h
  HW/EXI/EXI_DeviceAD16.cpp
  HW/EXI/EXI_DeviceAD16.h
  HW/EXI/EXI_DeviceAGP.cpp
  HW/EXI/EXI_DeviceAGP.h
  HW/EXI/EXI_DeviceBaseboard.cpp
  HW/EXI/EXI_DeviceBaseboard.h
  HW/EXI/EXI_DeviceDummy.cpp
  HW/EXI/EXI_DeviceDummy.h
  HW/EXI/EXI_DeviceEthernet.cpp
  HW/EXI/EXI_DeviceEthernet.h
  HW/EXI/EXI_DeviceGecko.cpp
  HW/EXI/EXI_DeviceGecko.h
  HW/EXI/EXI_DeviceIPL.cpp
  HW/EXI/EXI_DeviceIPL.h
  HW/EXI/EXI_DeviceMemoryCard.cpp
  HW/EXI/EXI_DeviceMemoryCard.h
  HW/EXI/EXI_DeviceModem.cpp
  HW/EXI/EXI_DeviceModem.h
  HW/EXI/EXI.cpp
  HW/EXI/EXI.h
  HW/GBAPad.cpp
  HW/GBAPad.h
  HW/GBAPadEmu.cpp
  HW/GBAPadEmu.h
  HW/GCKeyboard.cpp
  HW/GCKeyboard.h
  HW/GCKeyboardEmu.cpp
  HW/GCKeyboardEmu.h
  HW/GCMemcard/GCIFile.cpp
  HW/GCMemcard/GCIFile.h
  HW/GCMemcard/GCMemcard.cpp
  HW/GCMemcard/GCMemcard.h
  HW/GCMemcard/GCMemcardBase.h
  HW/GCMemcard/GCMemcardDirectory.cpp
  HW/GCMemcard/GCMemcardDirectory.h
  HW/GCMemcard/GCMemcardRaw.cpp
  HW/GCMemcard/GCMemcardRaw.h
  HW/GCMemcard/GCMemcardUtils.cpp
  HW/GCMemcard/GCMemcardUtils.h
  HW/GCPad.cpp
  HW/GCPad.h
  HW/GCPadEmu.cpp
  HW/GCPadEmu.h
  HW/GPFifo.cpp
  HW/GPFifo.h
  HW/HSP/HSP.cpp
  HW/HSP/HSP.h
  HW/HSP/HSP_Device.cpp
  HW/HSP/HSP_Device.h
  HW/HSP/HSP_DeviceARAMExpansion.cpp
  HW/HSP/HSP_DeviceARAMExpansion.h
  HW/HSP/HSP_DeviceGBPlayer.cpp
  HW/HSP/HSP_DeviceGBPlayer.h
  HW/HSP/HSP_DeviceNull.cpp
  HW/HSP/HSP_DeviceNull.h
  HW/HW.cpp
  HW/HW.h
  HW/MagCard/C1231BR.cpp
  HW/MagCard/C1231BR.h
  HW/MagCard/C1231LR.cpp
  HW/MagCard/C1231LR.h
  HW/MagCard/MagneticCardReader.cpp
  HW/MagCard/MagneticCardReader.h
  HW/Memmap.cpp
  HW/Memmap.h
  HW/MemoryInterface.cpp
  HW/MemoryInterface.h
  HW/MMIO.cpp
  HW/MMIO.h
  HW/ProcessorInterface.cpp
  HW/ProcessorInterface.h
  HW/SI/SI_Device.cpp
  HW/SI/SI_Device.h
  HW/SI/SI_DeviceDanceMat.cpp
  HW/SI/SI_DeviceDanceMat.h
  HW/SI/SI_DeviceGBA.cpp
  HW/SI/SI_DeviceGBA.h
  HW/SI/SI_DeviceGCAdapter.cpp
  HW/SI/SI_DeviceGCAdapter.h
  HW/SI/SI_DeviceGCController.cpp
  HW/SI/SI_DeviceGCController.h
  HW/SI/SI_DeviceGCSteeringWheel.cpp
  HW/SI/SI_DeviceGCSteeringWheel.h
  HW/SI/SI_DeviceKeyboard.cpp
  HW/SI/SI_DeviceKeyboard.h
  HW/SI/SI_DeviceAMBaseboard.cpp
  HW/SI/SI_DeviceAMBaseboard.h
  HW/SI/SI_DeviceNull.cpp
  HW/SI/SI_DeviceNull.h
  HW/SI/SI.cpp
  HW/SI/SI.h
  HW/Sram.cpp
  HW/Sram.h
  HW/StreamADPCM.cpp
  HW/StreamADPCM.h
  HW/SystemTimers.cpp
  HW/SystemTimers.h
  HW/Triforce/DeckReader.cpp
  HW/Triforce/DeckReader.h
  HW/Triforce/FZeroAX.cpp
  HW/Triforce/FZeroAX.h
  HW/Triforce/ICCardReader.cpp
  HW/Triforce/ICCardReader.h
  HW/Triforce/IOPorts.cpp
  HW/Triforce/IOPorts.h
  HW/Triforce/JVSIO.cpp
  HW/Triforce/JVSIO.h
  HW/Triforce/MarioKartGP.cpp
  HW/Triforce/MarioKartGP.h
  HW/Triforce/SerialDevice.cpp
  HW/Triforce/SerialDevice.h
  HW/Triforce/Touchscreen.cpp
  HW/Triforce/Touchscreen.h
  HW/VideoInterface.cpp
  HW/VideoInterface.h
  HW/WII_IPC.cpp
  HW/WII_IPC.h
  HW/Wiimote.cpp
  HW/Wiimote.h
  HW/WiimoteCommon/DataReport.cpp
  HW/WiimoteCommon/DataReport.h
  HW/WiimoteCommon/WiimoteConstants.h
  HW/WiimoteCommon/WiimoteHid.h
  HW/WiimoteCommon/WiimoteReport.h
  HW/WiimoteEmu/Camera.cpp
  HW/WiimoteEmu/Camera.h
  HW/WiimoteEmu/DesiredWiimoteState.cpp
  HW/WiimoteEmu/DesiredWiimoteState.h
  HW/WiimoteEmu/Dynamics.cpp
  HW/WiimoteEmu/Dynamics.h
  HW/WiimoteEmu/EmuSubroutines.cpp
  HW/WiimoteEmu/Encryption.cpp
  HW/WiimoteEmu/Encryption.h
  HW/WiimoteEmu/Extension/Classic.cpp
  HW/WiimoteEmu/Extension/Classic.h
  HW/WiimoteEmu/Extension/DesiredExtensionState.h
  HW/WiimoteEmu/Extension/DrawsomeTablet.cpp
  HW/WiimoteEmu/Extension/DrawsomeTablet.h
  HW/WiimoteEmu/Extension/Drums.cpp
  HW/WiimoteEmu/Extension/Drums.h
  HW/WiimoteEmu/Extension/Extension.cpp
  HW/WiimoteEmu/Extension/Extension.h
  HW/WiimoteEmu/Extension/Guitar.cpp
  HW/WiimoteEmu/Extension/Guitar.h
  HW/WiimoteEmu/Extension/Nunchuk.cpp
  HW/WiimoteEmu/Extension/Nunchuk.h
  HW/WiimoteEmu/Extension/Shinkansen.cpp
  HW/WiimoteEmu/Extension/Shinkansen.h
  HW/WiimoteEmu/Extension/TaTaCon.cpp
  HW/WiimoteEmu/Extension/TaTaCon.h
  HW/WiimoteEmu/Extension/Turntable.cpp
  HW/WiimoteEmu/Extension/Turntable.h
  HW/WiimoteEmu/Extension/UDrawTablet.cpp
  HW/WiimoteEmu/Extension/UDrawTablet.h
  HW/WiimoteEmu/ExtensionPort.cpp
  HW/WiimoteEmu/ExtensionPort.h
  HW/WiimoteEmu/I2CBus.cpp
  HW/WiimoteEmu/I2CBus.h
  HW/WiimoteEmu/MotionPlus.cpp
  HW/WiimoteEmu/MotionPlus.h
  HW/WiimoteEmu/Speaker.cpp
  HW/WiimoteEmu/Speaker.h
  HW/WiimoteEmu/WiimoteEmu.cpp
  HW/WiimoteEmu/WiimoteEmu.h
  HW/WiimoteReal/WiimoteReal.cpp
  HW/WiimoteReal/WiimoteReal.h
  HW/WiiSave.cpp
  HW/WiiSave.h
  HW/WiiSaveStructs.h
  IOS/Device.cpp
  IOS/Device.h
  IOS/DeviceStub.cpp
  IOS/DeviceStub.h
  IOS/Crypto/AesDevice.cpp
  IOS/Crypto/AesDevice.h
  IOS/Crypto/Sha.cpp
  IOS/Crypto/Sha.h
  IOS/DI/DI.cpp
  IOS/DI/DI.h
  IOS/DolphinDevice.cpp
  IOS/DolphinDevice.h
  IOS/ES/ES.cpp
  IOS/ES/ES.h
  IOS/ES/Formats.cpp
  IOS/ES/Formats.h
  IOS/ES/Identity.cpp
  IOS/ES/NandUtils.cpp
  IOS/ES/TitleContents.cpp
  IOS/ES/TitleInformation.cpp
  IOS/ES/TitleManagement.cpp
  IOS/ES/Views.cpp
  IOS/FS/FileSystem.h
  IOS/FS/FileSystemCommon.cpp
  IOS/FS/FileSystemProxy.cpp
  IOS/FS/FileSystemProxy.h
  IOS/FS/HostBackend/File.cpp
  IOS/FS/HostBackend/FS.cpp
  IOS/FS/HostBackend/FS.h
  IOS/FS/HostBackend/Journal.cpp
  IOS/IOS.cpp
  IOS/IOS.h
  IOS/IOSC.cpp
  IOS/IOSC.h
  IOS/MIOS.cpp
  IOS/MIOS.h
  IOS/Network/ICMP.h
  IOS/Network/ICMPLin.cpp
  IOS/Network/IP/Top.cpp
  IOS/Network/IP/Top.h
  IOS/Network/KD/NetKDRequest.cpp
  IOS/Network/KD/NetKDRequest.h
  IOS/Network/KD/NetKDTime.cpp
  IOS/Network/KD/NetKDTime.h
  IOS/Network/KD/NWC24Config.cpp
  IOS/Network/KD/NWC24Config.h
  IOS/Network/KD/NWC24DL.cpp
  IOS/Network/KD/NWC24DL.h
  IOS/Network/KD/VFF/VFFUtil.cpp
  IOS/Network/KD/VFF/VFFUtil.h
  IOS/Network/KD/WC24File.h
  IOS/Network/KD/Mail/MailCommon.h
  IOS/Network/KD/Mail/WC24FriendList.cpp
  IOS/Network/KD/Mail/WC24FriendList.h
  IOS/Network/KD/Mail/WC24Send.cpp
  IOS/Network/KD/Mail/WC24Send.h
  IOS/Network/MACUtils.cpp
  IOS/Network/MACUtils.h
  IOS/Network/NCD/Manage.cpp
  IOS/Network/NCD/Manage.h
  IOS/Network/NCD/WiiNetConfig.cpp
  IOS/Network/NCD/WiiNetConfig.h
  IOS/Network/Socket.cpp
  IOS/Network/Socket.h
  IOS/Network/SSL.cpp
  IOS/Network/SSL.h
  IOS/Network/WD/Command.cpp
  IOS/Network/WD/Command.h
  IOS/SDIO/SDIOSlot0.cpp
  IOS/SDIO/SDIOSlot0.h
  IOS/STM/STM.cpp
  IOS/STM/STM.h
  IOS/USB/Bluetooth/BTBase.cpp
  IOS/USB/Bluetooth/BTBase.h
  IOS/USB/Bluetooth/BTEmu.cpp
  IOS/USB/Bluetooth/BTEmu.h
  IOS/USB/Bluetooth/BTStub.cpp
  IOS/USB/Bluetooth/BTStub.h
  IOS/USB/Bluetooth/hci.h
  IOS/USB/Bluetooth/l2cap.h
  IOS/USB/Bluetooth/WiimoteDevice.cpp
  IOS/USB/Bluetooth/WiimoteDevice.h
  IOS/USB/Bluetooth/WiimoteHIDAttr.cpp
  IOS/USB/Bluetooth/WiimoteHIDAttr.h
  IOS/USB/Common.cpp
  IOS/USB/Common.h
  IOS/USB/Emulated/Infinity.cpp
  IOS/USB/Emulated/Infinity.h
  IOS/USB/Emulated/Microphone.cpp
  IOS/USB/Emulated/Microphone.h
  IOS/USB/Emulated/Skylanders/Skylander.cpp
  IOS/USB/Emulated/Skylanders/Skylander.h
  IOS/USB/Emulated/Skylanders/SkylanderCrypto.cpp
  IOS/USB/Emulated/Skylanders/SkylanderCrypto.h
  IOS/USB/Emulated/Skylanders/SkylanderFigure.cpp
  IOS/USB/Emulated/Skylanders/SkylanderFigure.h
  IOS/USB/Emulated/WiiSpeak.cpp
  IOS/USB/Emulated/WiiSpeak.h
  IOS/USB/Emulated/LogitechMic.cpp
  IOS/USB/Emulated/LogitechMic.h
  IOS/USB/Host.cpp
  IOS/USB/Host.h
  IOS/USB/OH0/OH0.cpp
  IOS/USB/OH0/OH0.h
  IOS/USB/OH0/OH0Device.cpp
  IOS/USB/OH0/OH0Device.h
  IOS/USB/USB_HID/HIDv4.cpp
  IOS/USB/USB_HID/HIDv4.h
  IOS/USB/USB_HID/HIDv5.cpp
  IOS/USB/USB_HID/HIDv5.h
  IOS/USB/USB_KBD.cpp
  IOS/USB/USB_KBD.h
  IOS/USB/USB_VEN/VEN.cpp
  IOS/USB/USB_VEN/VEN.h
  IOS/USB/USBScanner.cpp
  IOS/USB/USBScanner.h
  IOS/USB/USBV0.cpp
  IOS/USB/USBV0.h
  IOS/USB/USBV4.cpp
  IOS/USB/USBV4.h
  IOS/USB/USBV5.cpp
  IOS/USB/USBV5.h
  IOS/VersionInfo.cpp
  IOS/VersionInfo.h
  IOS/WFS/WFSI.cpp
  IOS/WFS/WFSI.h
  IOS/WFS/WFSSRV.cpp
  IOS/WFS/WFSSRV.h
  LibusbUtils.cpp
  LibusbUtils.h
  MemTools.cpp
  MemTools.h
  Movie.cpp
  Movie.h
  NetPlayClient.cpp
  NetPlayClient.h
  NetPlayCommon.cpp
  NetPlayCommon.h
  NetPlayServer.cpp
  NetPlayServer.h
  Orca/Activity.cpp
  Orca/Activity.h
  Orca/BootLogs.cpp
  Orca/BootLogs.h
  Orca/BootMemory.cpp
  Orca/BootMemory.h
  Orca/Branding.cpp
  Orca/Branding.h
  Orca/Bridge.cpp
  Orca/Bridge.h
  Orca/CrashNote.cpp
  Orca/CrashNote.h
  Orca/Disc.cpp
  Orca/Disc.h
  Orca/DiscVerify.cpp
  Orca/DiscVerify.h
  Orca/Engine/Display.h
  Orca/Engine/GuestMemory.cpp
  Orca/Engine/GuestMemory.h
  Orca/Engine/MatchTypes.cpp
  Orca/Engine/MatchTypes.h
  Orca/Engine/Module.cpp
  Orca/Engine/Module.h
  Orca/Engine/Regions.cpp
  Orca/Engine/Regions.h
  Orca/Engine/Ruleset.cpp
  Orca/Engine/Ruleset.h
  Orca/Engine/Schema.cpp
  Orca/Engine/Schema.h
  Orca/Engine/StrictJson.cpp
  Orca/Engine/StrictJson.h
  Orca/GpuLost.cpp
  Orca/GpuLost.h
  Orca/JitWarm.cpp
  Orca/JitWarm.h
  Orca/Launch.cpp
  Orca/Launch.h
  Orca/LocalStore.cpp
  Orca/LocalStore.h
  Orca/Bundle.cpp
  Orca/Bundle.h
  Orca/Mod.cpp
  Orca/Mod.h
  Orca/Music.cpp
  Orca/Music.h
  Orca/PanicDiag.cpp
  Orca/PanicDiag.h
  Orca/Perf.cpp
  Orca/Perf.h
  Orca/Policy/CharOrder.cpp
  Orca/Policy/CharOrder.h
  Orca/Policy/CssRules.h
  Orca/Policy/Queue.cpp
  Orca/Policy/Queue.h
  Orca/Profile.cpp
  Orca/Profile.h
  Orca/Sandbox.cpp
  Orca/Sandbox.h
  Orca/Status.cpp
  Orca/Status.h
  Orca/ThreadPriority.cpp
  Orca/Session/DirectLink.cpp
  Orca/Session/DirectLink.h
  Orca/Session/Events.cpp
  Orca/Session/Events.h
  Orca/Session/Keyframe.cpp
  Orca/Session/Keyframe.h
  Orca/Session/Online.cpp
  Orca/Session/Online.h
  Orca/Session/PadCodec.h
  Orca/Session/Session.cpp
  Orca/Session/Session.h
  Orca/Session/YouGameRoom.cpp
  Orca/Session/YouGameRoom.h
  Orca/Games/Brawl/BrawlStages.cpp
  Orca/Games/Brawl/BrawlGame.cpp
  Orca/Games/Brawl/BrawlGame.h
  Orca/Games/Brawl/BrawlModule.cpp
  Orca/Games/Brawl/BrawlModule.h
  Orca/Games/Generic/GenericModule.cpp
  Orca/Games/Generic/GenericModule.h
  Orca/Games/Brawl/BrawlStages.h
  Orca/Games/Brawl/CharOrder.cpp
  Orca/Games/Brawl/CharOrder.h
  Orca/UX/Chat.cpp
  Orca/UX/Chat.h
  Orca/Games/Brawl/CssTitle.cpp
  Orca/Games/Brawl/CssTitle.h
  Orca/UX/Controllers.cpp
  Orca/UX/Controllers.h
  Orca/UX/ControllerSource.cpp
  Orca/UX/ControllerSource.h
  Orca/Games/Brawl/FreeSpace.h
  Orca/UX/GameFont.cpp
  Orca/UX/GameFont.h
  Orca/UX/GamePatches.cpp
  Orca/UX/GamePatches.h
  Orca/UX/Kit.cpp
  Orca/UX/Kit.h
  Orca/UX/MenuEvents.cpp
  Orca/UX/MenuEvents.h
  Orca/Games/Brawl/SetBlock.cpp
  Orca/Games/Brawl/SetBlock.h
  Orca/Games/Brawl/MatchBlock.cpp
  Orca/Games/Brawl/MatchBlock.h
  Orca/Games/Brawl/MenuText.cpp
  Orca/Games/Brawl/MenuText.h
  Orca/Games/Brawl/NameTags.cpp
  Orca/Games/Brawl/NameTags.h
  Orca/Games/Brawl/NativeText.cpp
  Orca/Games/Brawl/NativeText.h
  Orca/Games/Brawl/OnlineMenu.cpp
  Orca/Games/Brawl/OnlineMenu.h
  Orca/Games/Brawl/OnlineRules.cpp
  Orca/Games/Brawl/OnlineRules.h
  Orca/Games/Brawl/OnlineSeats.cpp
  Orca/Games/Brawl/OnlineSeats.h
  Orca/UX/Overlay.cpp
  Orca/UX/Overlay.h
  Orca/UX/Probe.cpp
  Orca/UX/Probe.h
  Orca/UX/QueueApp.cpp
  Orca/UX/QueueApp.h
  Orca/Games/Brawl/Queue.cpp
  Orca/Games/Brawl/Queue.h
  Orca/Games/Brawl/RankedSet.cpp
  Orca/Games/Brawl/RankedSet.h
  Orca/Games/Brawl/PPlus/RankedPPlus.cpp
  Orca/Games/Brawl/PPlus/RankedPPlus.h
  Orca/UX/RankedSteps.cpp
  Orca/UX/RankedSteps.h
  Orca/Games/Brawl/Relabel.cpp
  Orca/Games/Brawl/Relabel.h
  Orca/Games/Brawl/Results.cpp
  Orca/Games/Brawl/Results.h
  Orca/Games/Brawl/Rulesets.cpp
  Orca/Games/Brawl/Rulesets.h
  Orca/UX/Rumble.cpp
  Orca/UX/Rumble.h
  Orca/UX/SetEnd.cpp
  Orca/UX/SetEnd.h
  Orca/UX/Search.cpp
  Orca/UX/Search.h
  Orca/Games/Brawl/StageCursors.cpp
  Orca/Games/Brawl/StageCursors.h
  Orca/UX/UX.cpp
  Orca/UX/UX.h
  Orca/UX/Widgets.cpp
  Orca/UX/Widgets.h
  Orca/UX/YgCard.cpp
  Orca/UX/YgCard.h
  Orca/UX/OrbCombo.cpp
  Orca/UX/OrbCombo.h
  Orca/UX/OverlayKey.cpp
  Orca/UX/OverlayKey.h
  Orca/UX/YgOrb.cpp
  Orca/UX/YgOrb.h
  Orca/UX/YgOrbDraw.cpp
  Orca/UX/YgOrbDraw.h
  NetworkCaptureLogger.cpp
  NetworkCaptureLogger.h
  PatchEngine.cpp
  PatchEngine.h
  PowerPC/BreakPoints.cpp
  PowerPC/BreakPoints.h
  PowerPC/CachedInterpreter/CachedInterpreter_Disassembler.cpp
  PowerPC/CachedInterpreter/CachedInterpreter.cpp
  PowerPC/CachedInterpreter/CachedInterpreter.h
  PowerPC/CachedInterpreter/CachedInterpreterBlockCache.cpp
  PowerPC/CachedInterpreter/CachedInterpreterBlockCache.h
  PowerPC/CachedInterpreter/CachedInterpreterEmitter.cpp
  PowerPC/CachedInterpreter/CachedInterpreterEmitter.h
  PowerPC/ConditionRegister.cpp
  PowerPC/ConditionRegister.h
  PowerPC/Expression.cpp
  PowerPC/Expression.h
  PowerPC/Interpreter/ExceptionUtils.h
  PowerPC/Interpreter/Interpreter_Branch.cpp
  PowerPC/Interpreter/Interpreter_FloatingPoint.cpp
  PowerPC/Interpreter/Interpreter_FPUtils.h
  PowerPC/Interpreter/Interpreter_Integer.cpp
  PowerPC/Interpreter/Interpreter_LoadStore.cpp
  PowerPC/Interpreter/Interpreter_LoadStorePaired.cpp
  PowerPC/Interpreter/Interpreter_Paired.cpp
  PowerPC/Interpreter/Interpreter_SystemRegisters.cpp
  PowerPC/Interpreter/Interpreter_Tables.cpp
  PowerPC/Interpreter/Interpreter.cpp
  PowerPC/Interpreter/Interpreter.h
  PowerPC/JitCommon/ConstantPropagation.cpp
  PowerPC/JitCommon/ConstantPropagation.h
  PowerPC/JitCommon/DivUtils.cpp
  PowerPC/JitCommon/DivUtils.h
  PowerPC/JitCommon/JitAsmCommon.cpp
  PowerPC/JitCommon/JitAsmCommon.h
  PowerPC/JitCommon/JitBase.cpp
  PowerPC/JitCommon/JitBase.h
  PowerPC/JitCommon/JitCache.cpp
  PowerPC/JitCommon/JitCache.h
  PowerPC/JitInterface.cpp
  PowerPC/JitInterface.h
  PowerPC/GDBStub.cpp
  PowerPC/GDBStub.h
  PowerPC/MMU.cpp
  PowerPC/MMU.h
  PowerPC/PowerPC.cpp
  PowerPC/PowerPC.h
  PowerPC/PPCAnalyst.cpp
  PowerPC/PPCAnalyst.h
  PowerPC/PPCCache.cpp
  PowerPC/PPCCache.h
  PowerPC/PPCSymbolDB.cpp
  PowerPC/PPCSymbolDB.h
  PowerPC/PPCTables.cpp
  PowerPC/PPCTables.h
  PowerPC/SignatureDB/CSVSignatureDB.cpp
  PowerPC/SignatureDB/CSVSignatureDB.h
  PowerPC/SignatureDB/DSYSignatureDB.cpp
  PowerPC/SignatureDB/DSYSignatureDB.h
  PowerPC/SignatureDB/MEGASignatureDB.cpp
  PowerPC/SignatureDB/MEGASignatureDB.h
  PowerPC/SignatureDB/SignatureDB.cpp
  PowerPC/SignatureDB/SignatureDB.h
  Rollback/Cow.cpp
  Rollback/Cow.h
  Rollback/Diag.cpp
  Rollback/Diag.h
  Rollback/Harness.cpp
  Rollback/Harness.h
  Rollback/InputGate.cpp
  Rollback/InputGate.h
  Rollback/Loopback.cpp
  Rollback/Loopback.h
  Rollback/OnlineMatch.cpp
  Rollback/OnlineMatch.h
  Rollback/ReplayStream.cpp
  Rollback/ReplayStream.h
  Rollback/Rollback.cpp
  Rollback/Rollback.h
  Rollback/SessionPort.cpp
  Rollback/SessionPort.h
  Rollback/UndoLog.cpp
  Rollback/UndoLog.h
  State.cpp
  State.h
  SyncIdentifier.h
  SysConf.cpp
  SysConf.h
  System.cpp
  System.h
  TimePlayed.cpp
  TimePlayed.h
  TitleDatabase.cpp
  TitleDatabase.h
  USBUtils.cpp
  USBUtils.h
  WC24PatchEngine.cpp
  WC24PatchEngine.h
  WiiRoot.cpp
  WiiRoot.h
  WiiUtils.cpp
  WiiUtils.h
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/AchievementApprovedHash.h
  COMMAND ${CMAKE_COMMAND}
          -DJSON_FILE=${CMAKE_SOURCE_DIR}/Data/Sys/ApprovedInis.json
          -DTEMPLATE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/AchievementApprovedHash.h.in
          -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/AchievementApprovedHash.h
          -P ${CMAKE_SOURCE_DIR}/CMake/GenerateAchievementHash.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/Data/Sys/ApprovedInis.json
          ${CMAKE_CURRENT_SOURCE_DIR}/AchievementApprovedHash.h.in
  COMMENT "Generating AchievementApprovedHash.h"
)

target_sources(core PRIVATE AchievementApprovedHash.h)

if(_M_X86_64)
  target_sources(core PRIVATE
    DSP/Jit/x64/DSPEmitter.cpp
    DSP/Jit/x64/DSPEmitter.h
    DSP/Jit/x64/DSPJitArithmetic.cpp
    DSP/Jit/x64/DSPJitBranch.cpp
    DSP/Jit/x64/DSPJitCCUtil.cpp
    DSP/Jit/x64/DSPJitExtOps.cpp
    DSP/Jit/x64/DSPJitLoadStore.cpp
    DSP/Jit/x64/DSPJitMisc.cpp
    DSP/Jit/x64/DSPJitMultiplier.cpp
    DSP/Jit/x64/DSPJitRegCache.cpp
    DSP/Jit/x64/DSPJitRegCache.h
    DSP/Jit/x64/DSPJitTables.cpp
    DSP/Jit/x64/DSPJitTables.h
    DSP/Jit/x64/DSPJitUtil.cpp
    PowerPC/Jit64/Jit.cpp
    PowerPC/Jit64/Jit.h
    PowerPC/Jit64/Jit64_Tables.cpp
    PowerPC/Jit64/Jit_Branch.cpp
    PowerPC/Jit64/Jit_FloatingPoint.cpp
    PowerPC/Jit64/Jit_Integer.cpp
    PowerPC/Jit64/Jit_LoadStore.cpp
    PowerPC/Jit64/Jit_LoadStoreFloating.cpp
    PowerPC/Jit64/Jit_LoadStorePaired.cpp
    PowerPC/Jit64/Jit_Paired.cpp
    PowerPC/Jit64/Jit_SystemRegisters.cpp
    PowerPC/Jit64/JitAsm.cpp
    PowerPC/Jit64/JitAsm.h
    PowerPC/Jit64/RegCache/CachedReg.h
    PowerPC/Jit64/RegCache/FPURegCache.cpp
    PowerPC/Jit64/RegCache/FPURegCache.h
    PowerPC/Jit64/RegCache/GPRRegCache.cpp
    PowerPC/Jit64/RegCache/GPRRegCache.h
    PowerPC/Jit64/RegCache/JitRegCache.cpp
    PowerPC/Jit64/RegCache/JitRegCache.h
    PowerPC/Jit64/RegCache/RCMode.h
    PowerPC/Jit64Common/BlockCache.cpp
    PowerPC/Jit64Common/BlockCache.h
    PowerPC/Jit64Common/ConstantPool.cpp
    PowerPC/Jit64Common/ConstantPool.h
    PowerPC/Jit64Common/EmuCodeBlock.cpp
    PowerPC/Jit64Common/EmuCodeBlock.h
    PowerPC/Jit64Common/FarCodeCache.cpp
    PowerPC/Jit64Common/FarCodeCache.h
    PowerPC/Jit64Common/Jit64AsmCommon.cpp
    PowerPC/Jit64Common/Jit64AsmCommon.h
    PowerPC/Jit64Common/Jit64Constants.h
    PowerPC/Jit64Common/Jit64PowerPCState.h
    PowerPC/Jit64Common/TrampolineCache.cpp
    PowerPC/Jit64Common/TrampolineCache.h
    PowerPC/Jit64Common/TrampolineInfo.h
  )
elseif(_M_ARM_64)
  target_sources(core PRIVATE
    PowerPC/JitArm64/Jit.cpp
    PowerPC/JitArm64/Jit.h
    PowerPC/JitArm64/JitAsm.cpp
    PowerPC/JitArm64/JitArm64Cache.cpp
    PowerPC/JitArm64/JitArm64_BackPatch.cpp
    PowerPC/JitArm64/JitArm64_Branch.cpp
    PowerPC/JitArm64/JitArm64_FloatingPoint.cpp
    PowerPC/JitArm64/JitArm64_Integer.cpp
    PowerPC/JitArm64/JitArm64_LoadStore.cpp
    PowerPC/JitArm64/JitArm64_LoadStoreFloating.cpp
    PowerPC/JitArm64/JitArm64_LoadStorePaired.cpp
    PowerPC/JitArm64/JitArm64_Paired.cpp
    PowerPC/JitArm64/JitArm64_RegCache.cpp
    PowerPC/JitArm64/JitArm64_RegCache.h
    PowerPC/JitArm64/JitArm64_SystemRegisters.cpp
    PowerPC/JitArm64/Jit_Util.cpp
    PowerPC/JitArm64/Jit_Util.h
    PowerPC/JitArm64/JitArm64_Tables.cpp
    PowerPC/JitArmCommon/BackPatch.h
  )
endif()

target_link_libraries(core
PUBLIC
  audiocommon
  common
  discio
  enet::enet
  expr
  inputcommon
  MbedTLS::mbedtls
  pugixml
  sfml-network
  videonull
  videoogl
  videosoftware

PRIVATE
  CURL::libcurl
  ed25519
  imgui
  LibJuice::LibJuice
  zstd::zstd
  FatFs
  fmt::fmt
  LZO::LZO
  LZ4::LZ4
  ZLIB::ZLIB
)

# The Ed25519 key a flat bundle's mod.sig must be by (Core/Orca/Bundle.cpp): 64 lowercase hex digits.
# Unset, the build trusts no key and every flat bundle is refused (legacy only). The bundle-flat TEST
# key is trusted only with ORCA_BUNDLE_TRUST_TEST_KEY=ON (tests and gates); release scripts refuse it.
set(ORCA_BUNDLE_PUBLIC_KEY "" CACHE STRING
    "Ed25519 public key (64 lowercase hex digits) that signs Orca flat bundles; empty: none")
option(ORCA_BUNDLE_TRUST_TEST_KEY "Also trust the bundle-flat TEST key (tests only, never a release)" OFF)
set(ORCA_BUNDLE_DEFINITIONS "")
if(ORCA_BUNDLE_PUBLIC_KEY)
  if(NOT ORCA_BUNDLE_PUBLIC_KEY MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "ORCA_BUNDLE_PUBLIC_KEY must be 64 lowercase hex digits")
  endif()
  string(LENGTH "${ORCA_BUNDLE_PUBLIC_KEY}" ORCA_BUNDLE_PUBLIC_KEY_LENGTH)
  if(NOT ORCA_BUNDLE_PUBLIC_KEY_LENGTH EQUAL 64)
    message(FATAL_ERROR "ORCA_BUNDLE_PUBLIC_KEY must be 64 lowercase hex digits")
  endif()
  list(APPEND ORCA_BUNDLE_DEFINITIONS "ORCA_BUNDLE_PUBLIC_KEY=\"${ORCA_BUNDLE_PUBLIC_KEY}\"")
  message(STATUS "Orca flat bundles: trusting key ${ORCA_BUNDLE_PUBLIC_KEY}")
endif()
if(ORCA_BUNDLE_TRUST_TEST_KEY)
  list(APPEND ORCA_BUNDLE_DEFINITIONS ORCA_BUNDLE_TRUST_TEST_KEY)
  message(WARNING "Orca flat bundles: trusting the TEST key (ORCA_BUNDLE_TRUST_TEST_KEY): never a release")
endif()
if(NOT ORCA_BUNDLE_DEFINITIONS)
  message(STATUS "Orca flat bundles: no key (set ORCA_BUNDLE_PUBLIC_KEY): every bundle is refused")
endif()
# Read by Source/UnitTests/Core (OrcaBundle.PinnedKey checks the build trusts exactly these).
set(ORCA_BUNDLE_DEFINITIONS "${ORCA_BUNDLE_DEFINITIONS}" CACHE INTERNAL "")
set_source_files_properties(Orca/Bundle.cpp PROPERTIES COMPILE_DEFINITIONS "${ORCA_BUNDLE_DEFINITIONS}")
# The running version a mod.json's engine.minOrca is checked against (Mod::EngineVersion).
set_source_files_properties(Orca/Mod.cpp PROPERTIES COMPILE_DEFINITIONS
                            "ORCA_ENGINE_VERSION=\"${ORCA_VERSION}\"")
# Orca data compiled in byte for byte (Engine/Ruleset.cpp, Games/Brawl/Rulesets.cpp): the ruleset
# schema and the builtin module's rulesets, each written as a list of char literals that a source
# file includes. Changing one of them re-runs CMake.
set(ORCA_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/OrcaEmbed")
foreach(orca_embed_src
    "${CMAKE_SOURCE_DIR}/Tools/orca/mods/ruleset.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/Orca/Games/Brawl/Rulesets/brawl-supernova-2025.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/Orca/Games/Brawl/Rulesets/pplus-2024.json")
  get_filename_component(orca_embed_name "${orca_embed_src}" NAME)
  file(READ "${orca_embed_src}" orca_embed_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," orca_embed_list "${orca_embed_hex}")
  file(CONFIGURE OUTPUT "${ORCA_EMBED_DIR}/${orca_embed_name}.inc" CONTENT "${orca_embed_list}\n" @ONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orca_embed_src}")
endforeach()
target_include_directories(core PRIVATE "${ORCA_EMBED_DIR}")
if(ORCA_BUNDLE_TRUST_TEST_KEY)
  set_source_files_properties(Orca/UX/UX.cpp PROPERTIES COMPILE_DEFINITIONS ORCA_BUNDLE_TRUST_TEST_KEY)
endif()
# The SD image's SHA-256 (Bundle.cpp) uses CNG on Windows.
if(WIN32)
  target_link_libraries(core PRIVATE bcrypt)
endif()

if(LIBUDEV_FOUND)
  target_include_directories(core PRIVATE ${LIBUDEV_INCLUDE_DIR})
  target_link_libraries(core PRIVATE ${LIBUDEV_LIBRARY})
endif()

if (APPLE)
  target_link_libraries(core
  PRIVATE
    ${CORESERV_LIBRARY}
    ${IOB_LIBRARY}
    ${IOK_LIBRARY}
    resolv
  )
elseif (ANDROID)
  target_link_libraries(core
  PRIVATE
    androidcommon
  )
endif()

if(ENABLE_CUBEB)
  target_link_libraries(core PUBLIC cubeb)
  target_sources(core PRIVATE
    HW/EXI/EXI_DeviceMic.cpp
    HW/EXI/EXI_DeviceMic.h
  )
endif()

if(TARGET LibUSB::LibUSB)
  target_link_libraries(core PUBLIC LibUSB::LibUSB)
  target_sources(core PRIVATE
    IOS/USB/LibusbDevice.cpp
    IOS/USB/LibusbDevice.h
    IOS/USB/Bluetooth/BTReal.cpp
    IOS/USB/Bluetooth/BTReal.h
    IOS/USB/Bluetooth/LibUSBBluetoothAdapter.cpp
    IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h
    IOS/USB/Bluetooth/RealtekFirmwareLoader.cpp
    IOS/USB/Bluetooth/RealtekFirmwareLoader.h
  )
endif()

if(ENABLE_VULKAN)
  target_link_libraries(core PUBLIC videovulkan)
endif()

if(APPLE)
  target_link_libraries(core PUBLIC videometal)
endif()

if(USE_MGBA)
  target_sources(core PRIVATE
    HW/GBACore.cpp
    HW/GBACore.h
    HW/SI/SI_DeviceGBAEmu.cpp
    HW/SI/SI_DeviceGBAEmu.h
  )
  target_link_libraries(core PUBLIC mGBA::mgba)
  target_compile_definitions(core PUBLIC -DHAS_LIBMGBA)
endif()

if(WIN32)
  target_sources(core PRIVATE
    HW/EXI/BBA/TAP_Win32.cpp
    HW/EXI/BBA/TAP_Win32.h
    HW/WiimoteReal/IOWin.cpp
    HW/WiimoteReal/IOWin.h
  )
  target_link_libraries(core PUBLIC
    videod3d
    videod3d12
    setupapi.lib
    iphlpapi.lib
  )
  target_compile_definitions(core PRIVATE "-D_WINSOCK_DEPRECATED_NO_WARNINGS")
elseif(APPLE)
  target_sources(core PRIVATE
    HW/EXI/BBA/TAP_Apple.cpp
  )
  target_link_libraries(core PUBLIC ${IOB_LIBRARY})
elseif(UNIX)
  target_sources(core PRIVATE
    HW/EXI/BBA/TAP_Unix.cpp
  )
  if(ANDROID)
    target_sources(core PRIVATE
      HW/WiimoteReal/IOAndroid.cpp
      HW/WiimoteReal/IOAndroid.h
    )
  endif()
endif()

# Bluez doesn't support all the communication modes on FreeBSD, so only using it on Linux
if(LINUX)
  option(ENABLE_BLUEZ "Enables bluetooth support" ON)
  if(ENABLE_BLUEZ)
    find_package(BlueZ)
    if(BlueZ_FOUND)
      message(STATUS "BlueZ found, enabling bluetooth support")
      target_sources(core PRIVATE
        HW/WiimoteReal/IOLinux.cpp
        HW/WiimoteReal/IOLinux.h
      )
      target_link_libraries(core PUBLIC BlueZ::BlueZ)
      target_compile_definitions(core PRIVATE -DHAVE_BLUEZ=1)
    else()
      message(STATUS "BlueZ NOT found, disabling bluetooth support")
    endif()
  else()
    message(STATUS "BlueZ explicitly disabled, disabling bluetooth support")
  endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  # OpenBSD doesn't allow memory to be both writable and executable by default.
  # The JIT currently needs this.
  target_link_options(core PUBLIC -Wl,-zwxneeded)
endif()

if(TARGET Hidapi::Hidapi)
  target_sources(core PRIVATE
    HW/WiimoteReal/IOhidapi.cpp
    HW/WiimoteReal/IOhidapi.h
  )
  target_link_libraries(core PUBLIC Hidapi::Hidapi)
  target_compile_definitions(core PRIVATE -DHAVE_HIDAPI=1)
endif()

if(UNIX)
  target_sources(core PRIVATE
    MemoryWatcher.cpp
    MemoryWatcher.h
  )
endif()

if(TARGET cpp-ipc::ipc)
  target_sources(core PRIVATE HW/EXI/BBA/IPC.cpp)
  target_link_libraries(core PRIVATE cpp-ipc::ipc)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(core PRIVATE use_pch)
endif()

# Orca and Rollback work out values every peer must agree on (the input gate's steering and
# presses, what the hooks write to emulated memory) on different hosts and compilers, so no fused
# multiply-adds there: Apple clang contracts a * b + c into one by default on ARM64, which rounds
# once where x86-64 rounds twice. That code keeps to integers anyway (ORCA.md "Determinism across hosts");
# this is the belt to those braces. MSVC needs nothing: /fp:precise (CMakeLists.txt) never
# contracts in VS 2022 and later, and the x86-64 baseline (no /arch:AVX2) has no FMA to emit.
if(NOT MSVC)
  get_target_property(ORCA_DETERMINISTIC_SOURCES core SOURCES)
  list(FILTER ORCA_DETERMINISTIC_SOURCES INCLUDE REGEX "^(Orca|Rollback)/.*\\.cpp$")
  set_property(SOURCE ${ORCA_DETERMINISTIC_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
  # The replay codec and the threads that pack and unpack replays catch their own size and
  # allocation failures (try/catch, as MSVC's /EHsc builds them); the rest of a non-MSVC build has
  # -fno-exceptions. The boot boundary must unwind its scope guards, and the cache allocator
  # must throw rather than abort when its vector growth runs out of memory.
  set_property(SOURCE Orca/Session/Keyframe.cpp Rollback/OnlineMatch.cpp
               Core.cpp Orca/BootMemory.cpp PowerPC/PPCCache.cpp APPEND PROPERTY
               COMPILE_OPTIONS -fexceptions)
endif()

if(USE_RETRO_ACHIEVEMENTS)
  target_link_libraries(core PUBLIC rcheevos)
  target_compile_definitions(core PUBLIC -DUSE_RETRO_ACHIEVEMENTS)
  target_compile_definitions(core PUBLIC -DRC_CLIENT_SUPPORTS_HASH)
  if(RC_CLIENT_SUPPORTS_RAINTEGRATION)
    target_compile_definitions(core PUBLIC -DRC_CLIENT_SUPPORTS_RAINTEGRATION)
  endif()
endif()

# Orca rollback harness hashes RAM with xxHash
target_link_libraries(core PRIVATE xxhash::xxhash)
