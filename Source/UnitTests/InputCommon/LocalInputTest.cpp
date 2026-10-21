// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"
#include "Core/IOS/USB/USB_KBD.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/LocalInput.h"
#include "UICommon/UICommon.h"

namespace IOS::HLE
{
class USBKeyboardTestPeer
{
public:
  static void QueueKey(USB_KBD& keyboard)
  {
    keyboard.m_message_queue.emplace(USB_KBD::MessageType::Event, 0x02,
                                     USB_KBD::PressedKeyData{0x04});
  }
  static bool HasMessages(const USB_KBD& keyboard) { return !keyboard.m_message_queue.empty(); }
};

#if defined(__LIBUSB__)
class BluetoothRealDeviceTestPeer
{
public:
  static void QueueHCI(BluetoothRealDevice& bluetooth, const IOCtlVRequest& request)
  {
    bluetooth.m_hci_endpoint =
        std::make_unique<USB::V0IntrMessage>(bluetooth.GetEmulationKernel(), request);
  }
};
#endif
}  // namespace IOS::HLE

namespace
{
class LocalInputEnv
{
public:
  explicit LocalInputEnv(const char* value)
  {
    if (const char* old = std::getenv("ORCA_NO_LOCAL_INPUT"))
      m_old = old;
    Set(value);
  }
  ~LocalInputEnv() { Set(m_old ? m_old->c_str() : nullptr); }

private:
  static void Set(const char* value)
  {
#ifdef _WIN32
    _putenv_s("ORCA_NO_LOCAL_INPUT", value ? value : "");
#else
    if (value)
      setenv("ORCA_NO_LOCAL_INPUT", value, 1);
    else
      unsetenv("ORCA_NO_LOCAL_INPUT");
#endif
  }

  std::optional<std::string> m_old;
};

class MemoryConfig final : public Config::ConfigLayerLoader
{
public:
  MemoryConfig() : ConfigLayerLoader(Config::LayerType::Base) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

class LocalInputDevices : public testing::Test
{
protected:
  void SetUp() override
  {
    m_old_user = File::GetUserPath(D_USER_IDX);
    m_directory = File::CreateTempDir();
    ASSERT_FALSE(m_directory.empty());
    UICommon::SetUserDirectory(m_directory);
    Config::Init();
    Config::AddLayer(std::make_unique<MemoryConfig>());
    SConfig::Init();
    m_system.Initialize();
    m_old_wii = m_system.IsWii();
    m_system.SetIsWii(true);
    m_system.GetMemory().Init();
    ASSERT_FALSE(Core::WiiRootIsInitialized());
    Core::InitializeWiiRoot(false);
    m_wii_root = true;
    Config::SetBase(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, true);
    // IOS4 has no background network service; no device is opened by construction.
    m_ios = std::make_unique<IOS::HLE::EmulationKernel>(m_system, 0x0000000100000004);
  }
  void TearDown() override
  {
    m_ios.reset();
    if (m_wii_root)
      Core::ShutdownWiiRoot();
    m_system.GetMemory().Shutdown();
    m_system.SetIsWii(m_old_wii);
    SConfig::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, m_old_user);
    File::DeleteDirRecursively(m_directory);
  }

#if defined(__LIBUSB__)
  std::vector<u8> ActivePassthroughState(IOS::HLE::BluetoothRealDevice& bluetooth)
  {
    const LocalInputEnv allowed(nullptr);
    IOS::HLE::OpenRequest open(m_system, 0x1000);
    // Activate the emulated device state without opening a physical adapter.
    bluetooth.Device::Open(open);
    auto& memory = m_system.GetMemory();
    memory.Write_U32(IOS::HLE::IPC_CMD_IOCTLV, 0x1000);
    memory.Write_U32(IOS::HLE::USB::IOCTLV_USBV0_INTRMSG, 0x100c);
    memory.Write_U32(2, 0x1010);
    memory.Write_U32(1, 0x1014);
    memory.Write_U32(0x1100, 0x1018);
    memory.Write_U32(0x1200, 0x1100);
    memory.Write_U32(1, 0x1104);
    memory.Write_U32(0x1202, 0x1108);
    memory.Write_U32(2, 0x110c);
    memory.Write_U32(0x1300, 0x1110);
    memory.Write_U32(256, 0x1114);
    memory.Write_U8(0x81, 0x1200);
    memory.Write_U16(256, 0x1202);
    IOS::HLE::IOCtlVRequest interrupt(m_system, 0x1000);
    IOS::HLE::BluetoothRealDeviceTestPeer::QueueHCI(bluetooth, interrupt);
    std::vector<u8> state(256);
    u8* cursor = state.data();
    PointerWrap write(&cursor, state.size(), PointerWrap::Mode::Write);
    bluetooth.DoState(write);
    EXPECT_TRUE(write.IsWriteMode());
    state.resize(cursor - state.data());
    bluetooth.Device::Close(0);
    return state;
  }
#endif

  Core::System& m_system = Core::System::GetInstance();
  std::unique_ptr<IOS::HLE::EmulationKernel> m_ios;
  std::string m_directory, m_old_user;
  bool m_old_wii = false;
  bool m_wii_root = false;
};

class FakeUSBKeyboard final : public IOS::HLE::USB_KBD
{
public:
  using USB_KBD::USB_KBD;
  mutable int polls = 0;

private:
  bool IsKeyPressed(int) const override
  {
    ++polls;
    return false;
  }
};
}  // namespace

TEST_F(LocalInputDevices, GuestKeyboardSkipsPollingAndQueuedDelivery)
{
  FakeUSBKeyboard keyboard(*m_ios, "/dev/usb/kbd");
  Config::SetBase(Config::MAIN_WII_KEYBOARD, true);
  ASSERT_FALSE(Core::WantsDeterminism());
  IOS::HLE::OpenRequest open(m_system, 0x1000);
  ASSERT_EQ(keyboard.Open(open)->return_value, IOS::HLE::IPC_SUCCESS);
  IOS::HLE::IOCtlRequest request(m_system, 0x1000);
  request.buffer_out = 0x2000;
  request.buffer_out_size = 16;

  const bool old_gate = ControlReference::GetInputGate();
  ControlReference::SetInputGate(true);
  {
    const LocalInputEnv allowed(nullptr);
    keyboard.Update();
    EXPECT_GT(keyboard.polls, 0);
    IOS::HLE::USBKeyboardTestPeer::QueueKey(keyboard);
    keyboard.IOCtl(request);
    EXPECT_EQ(m_system.GetMemory().Read_U8(request.buffer_out + 8), 0x02);
    EXPECT_EQ(m_system.GetMemory().Read_U8(request.buffer_out + 10), 0x04);
  }
  keyboard.polls = 0;
  const LocalInputEnv disabled("1");
  IOS::HLE::USBKeyboardTestPeer::QueueKey(keyboard);
  keyboard.Update();
  EXPECT_EQ(keyboard.polls, 0);
  EXPECT_FALSE(IOS::HLE::USBKeyboardTestPeer::HasMessages(keyboard));
  IOS::HLE::USBKeyboardTestPeer::QueueKey(keyboard);
  m_system.GetMemory().Write_U32(0xabababab, request.buffer_out + 8);
  keyboard.IOCtl(request);
  EXPECT_EQ(m_system.GetMemory().Read_U32(request.buffer_out + 8), 0xabababab);
  EXPECT_FALSE(IOS::HLE::USBKeyboardTestPeer::HasMessages(keyboard));
  ControlReference::SetInputGate(old_gate);
}

#if defined(__LIBUSB__)
TEST_F(LocalInputDevices, PassthroughStateCannotActivateWithoutLocalInput)
{
  const LocalInputEnv disabled("1");
  IOS::HLE::BluetoothRealDevice bluetooth(*m_ios, "/dev/usb/oh1/57e/305");
  IOS::HLE::OpenRequest open(m_system, 0x1000);
  ASSERT_EQ(bluetooth.Open(open)->return_value, IOS::HLE::IPC_ENOENT);

  // The serialized device is active and has an outstanding HCI interrupt request.
  auto state = ActivePassthroughState(bluetooth);
  u8* cursor = state.data();
  PointerWrap read(&cursor, state.size(), PointerWrap::Mode::Read);
  bluetooth.DoState(read);
  ASSERT_TRUE(read.IsVerifyMode());
  EXPECT_EQ(cursor, state.data() + sizeof(bool));
  EXPECT_FALSE(bluetooth.IsOpened());
  bluetooth.Update();
  IOS::HLE::IOCtlVRequest control(m_system, 0x1000);
  control.request = IOS::HLE::USB::IOCTLV_USBV0_CTRLMSG;
  control.in_vectors = {{0x1400, 1}, {0x1401, 1}, {0x1402, 2}, {0x1404, 2}, {0x1406, 2}};
  control.io_vectors = {{0x1500, 3}};
  EXPECT_EQ(bluetooth.IOCtlV(control)->return_value, IOS::HLE::IPC_ENOENT);
  EXPECT_EQ(bluetooth.Close(0)->return_value, IOS::HLE::IPC_SUCCESS);
  EXPECT_FALSE(bluetooth.IsOpened());
}

TEST_F(LocalInputDevices, PassthroughStateBytesAreUnchangedWithLocalInputEnabled)
{
  const LocalInputEnv allowed(nullptr);
  IOS::HLE::BluetoothRealDevice bluetooth(*m_ios, "/dev/usb/oh1/57e/305");
  auto state = ActivePassthroughState(bluetooth);
  u8* cursor = state.data();
  PointerWrap read(&cursor, state.size(), PointerWrap::Mode::Read);
  bluetooth.DoState(read);
  EXPECT_TRUE(read.IsReadMode());
  EXPECT_TRUE(bluetooth.IsOpened());
  EXPECT_EQ(cursor, state.data() + state.size());
  std::vector<u8> saved(state.size());
  cursor = saved.data();
  PointerWrap write(&cursor, saved.size(), PointerWrap::Mode::Write);
  bluetooth.DoState(write);
  EXPECT_EQ(saved, state);
  bluetooth.Close(0);
}
#endif

TEST(LocalInput, OnlyExactOneDisablesLocalInput)
{
  for (const char* value : {static_cast<const char*>(nullptr), "", "0", "true", "01", "1 "})
  {
    const LocalInputEnv env(value);
    EXPECT_FALSE(InputCommon::LocalInputDisabled());
  }
  const LocalInputEnv env("1");
  EXPECT_TRUE(InputCommon::LocalInputDisabled());
}

TEST(LocalInput, NoBackendsOrDevicesAfterInitializeAndRefresh)
{
  const LocalInputEnv env("1");
  ControllerInterface controllers;
  controllers.Initialize({});
  EXPECT_TRUE(controllers.IsInit());
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  EXPECT_TRUE(controllers.GetAllDevices().empty());
  controllers.Initialize({});
  controllers.RefreshDevices();
  controllers.ChangeWindow(nullptr);
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  EXPECT_TRUE(controllers.GetAllDevices().empty());
  controllers.Shutdown();
  controllers.Initialize({});
  EXPECT_EQ(controllers.GetInputBackendCount(), 0u);
  controllers.Shutdown();
}

TEST(LocalInput, AdapterInitSkipsNativeContextAndConfigCallbacks)
{
  const LocalInputEnv env("1");
  ASSERT_FALSE(GCAdapter::IsInitialized());
  // No Config/System initialization: disabled Init must return before either is consulted.
  GCAdapter::Init();
  GCAdapter::Init();
  EXPECT_FALSE(GCAdapter::IsInitialized());
  GCAdapter::Shutdown();
  EXPECT_FALSE(GCAdapter::IsInitialized());
}

TEST(LocalInput, WiimoteInitializationSkipsScanningAndWait)
{
  const LocalInputEnv env("1");
  // Even the state-load path must return without reading config or starting a scanner.
  WiimoteReal::Initialize(Wiimote::InitializeMode::DO_WAIT_FOR_WIIMOTES);
  WiimoteReal::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
}
