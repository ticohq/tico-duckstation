#pragma once
#include "input_source.h"
#include <array>
#include <functional>
#include <mutex>
#include <switch.h>
#include <vector>

class SettingsInterface;

class SwitchInputSource final : public InputSource
{
public:
  SwitchInputSource();
  ~SwitchInputSource();

  bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void Shutdown() override;
  bool ReloadDevices() override;

  void PollEvents() override;
  std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
  std::vector<InputBindingKey> EnumerateMotors() override;
  bool GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  std::optional<InputBindingKey> ParseKeyString(const std::string_view& device,
                                                const std::string_view& binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key) override;

  enum : u32
  {
    NUM_CONTROLLERS = 4,
    NUM_BUTTONS = 28,
    NUM_AXIS = 4,
    NUM_VIBRATION_HANDLES = 6,
  };

  struct ControllerData
  {
    HidVibrationDeviceHandle vibration_handles[NUM_VIBRATION_HANDLES];
    bool vibration_handle_valid[NUM_VIBRATION_HANDLES] = {};

    PadState pad_state;
    bool connected = false;
    u64 buttons = 0;
  };

private:
  using ControllerDataArray = std::array<ControllerData, NUM_CONTROLLERS>;

  ControllerDataArray m_controllers;

  void UpdateState(u32 controller);
};
