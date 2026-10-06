#include "switch_controller_interface.h"
#include "common/file_system.h"
#include "common/assert.h"
#include "common/log.h"
#include "fmt/printf.h"
Log_SetChannel(SwitchControllerInterface);

SwitchControllerInterface::SwitchControllerInterface() = default;

SwitchControllerInterface::~SwitchControllerInterface() = default;

ControllerInterface::Backend SwitchControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::Switch;
}

bool SwitchControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  if (!ControllerInterface::Initialize(host_interface))
    return false;

  padConfigureInput(2, HidNpadStyleSet_NpadStandard);
  padInitialize(&m_controllers[0].pad, HidNpadIdType_Handheld, HidNpadIdType_No1);
  padInitialize(&m_controllers[1].pad, HidNpadIdType_No2);

  return true;
}

void SwitchControllerInterface::Shutdown()
{
  ControllerInterface::Shutdown();
}

std::string SwitchControllerInterface::GetGameControllerDBFileName() const
{
  // prefer the userdir copy
  std::string filename(m_host_interface->GetUserDirectoryRelativePath("gamecontrollerdb.txt"));
  if (FileSystem::FileExists(filename.c_str()))
    return filename;

  return {};
}

void SwitchControllerInterface::PollEvents()
{
  for (int i = 0; i < 2; i++)
  {
    padUpdate(&m_controllers[i].pad);

    bool connected = padIsConnected(&m_controllers[i].pad);
    if (connected != m_controllers[i].connected)
    {
      if (connected)
      {
        INFO_LOG("{}", fmt::sprintf("Controller %d connected", i));
        OnControllerConnected(i);
      }
      else
      {
        INFO_LOG("{}", fmt::sprintf("Controller %d disconnected", i));
        OnControllerDisconnected(i);
      }
      m_controllers[i].connected = connected;
    }

    if (connected)
    {
      // remove the pseudo derived from analog sticks since duckstation has it's own analog -> digital
      const u64 button_mask = ~(u64)(HidNpadButton_StickLLeft
        | HidNpadButton_StickLUp
        | HidNpadButton_StickLRight
        | HidNpadButton_StickLDown
        | HidNpadButton_StickRLeft
        | HidNpadButton_StickRUp
        | HidNpadButton_StickRRight
        | HidNpadButton_StickRDown);
      HandleButtons(i, padGetButtonsDown(&m_controllers[i].pad) & button_mask, true);
      HandleButtons(i, padGetButtonsUp(&m_controllers[i].pad) & button_mask, false);

      for (int j = 0; j < 2; j++)
      {
        HidAnalogStickState state = padGetStickPos(&m_controllers[i].pad, j);
        HandleAxis(i, j*2+0, static_cast<float>(state.x) / JOYSTICK_MAX);
        HandleAxis(i, j*2+1, -static_cast<float>(state.y) / JOYSTICK_MAX);
      }
    }
  }
}

void SwitchControllerInterface::HandleButtons(int controller_id, u64 mask, bool pressed)
{
  while (mask)
  {
    unsigned button = __builtin_ctzll(mask);
    mask &= ~(1ULL << button);

    DEBUG_LOG("{}", fmt::sprintf("controller %d button %d %s", controller_id, button,
                pressed ? "pressed" : "released"));

    ControllerData& controller = m_controllers[controller_id];

    static constexpr std::array<FrontendCommon::ControllerNavigationButton, MAX_NUM_BUTTONS>
      nav_button_mapping = {{
        FrontendCommon::ControllerNavigationButton::Activate,      // HidNpadButton_A
        FrontendCommon::ControllerNavigationButton::Cancel,        // HidNpadButton_B
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_X
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_Y
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_StickL
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_StickR
        FrontendCommon::ControllerNavigationButton::LeftShoulder,  // HidNpadButton_L
        FrontendCommon::ControllerNavigationButton::RightShoulder, // HidNpadButton_R
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_ZL
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_ZR
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_Plus
        FrontendCommon::ControllerNavigationButton::Count,         // HidNpadButton_Minus
        FrontendCommon::ControllerNavigationButton::DPadLeft,      // HidNpadButton_DpadLeft
        FrontendCommon::ControllerNavigationButton::DPadUp,        // HidNpadButton_DpadUp
        FrontendCommon::ControllerNavigationButton::DPadRight,     // HidNpadButton_DpadRight
        FrontendCommon::ControllerNavigationButton::DPadDown,      // HidNpadButton_DpadDown
      }};

    if (DoEventHook(Hook::Type::Button, controller_id, button, pressed ? 1.0f : 0.0f))
      continue;

    if (button < nav_button_mapping.size() &&
      nav_button_mapping[button] != FrontendCommon::ControllerNavigationButton::Count)
    {
      m_host_interface->SetControllerNavigationButtonState(nav_button_mapping[button], pressed);
    }

    if (m_host_interface->IsControllerNavigationActive())
    {
      // UI consumed the event
      continue;
    }

    if (button >= MAX_NUM_BUTTONS)
      continue;

    const ButtonCallback& cb = controller.button_mapping[button];
    if (cb)
    {
      cb(pressed);
      continue;
    }

    const AxisCallback& axis_cb = controller.button_axis_mapping[button];
    if (axis_cb)
    {
      axis_cb(pressed ? 1.0f : -1.0f);
      continue;
    }
  }
}

void SwitchControllerInterface::HandleAxis(int controller_id, int axis_number, float value)
{
  DEBUG_LOG("{}", fmt::sprintf("controller %d axis %d %f", controller_id, axis_number, value));

  if (DoEventHook(Hook::Type::Axis, controller_id, axis_number, value, true))
    return;

  ControllerData& controller = m_controllers[controller_id];

  bool processed = false;

  const AxisCallback& cb = controller.axis_mapping[axis_number][AxisSide::Full];
  if (cb)
  {
    cb(value);
    processed = true;
  }

  if (value > 0.0f)
  {
    const AxisCallback& hcb = controller.axis_mapping[axis_number][AxisSide::Positive];
    if (hcb)
    {
      hcb(value);
      processed = true;
    }
  }
  else if (value < 0.0f)
  {
    const AxisCallback& hcb = controller.axis_mapping[axis_number][AxisSide::Negative];
    if (hcb)
    {
      hcb(value);
      processed = true;
    }
  }

  if (processed)
    return;

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= m_controllers[controller_id].deadzone);
  const bool positive = (value >= 0.0f);
  const ButtonCallback& other_button_cb = controller.axis_button_mapping[axis_number][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb = controller.axis_button_mapping[axis_number][BoolToUInt8(positive)];
  if (button_cb)
  {
    button_cb(outside_deadzone);
    if (other_button_cb)
      other_button_cb(false);
    return;
  }
  else if (other_button_cb)
  {
    other_button_cb(false);
    return;
  }
  else
  {
    return;
  }

}

void SwitchControllerInterface::ClearBindings()
{
  for (auto& it : m_controllers)
  {
    it.axis_mapping.fill({});
    it.button_mapping.fill({});
    it.axis_button_mapping.fill({});
    it.button_axis_mapping.fill({});
  }
}

bool SwitchControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                AxisCallback callback)
{
  if (controller_index < 0 || controller_index >= MAX_CONTROLLERS)
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXES)
    return false;

  m_controllers[controller_index].axis_mapping[axis_number][axis_side] = std::move(callback);
  return true;
}

bool SwitchControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  if (controller_index < 0 || controller_index >= MAX_CONTROLLERS)
    return false;

  if (button_number < 0 || button_number >= MAX_NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_mapping[button_number] = std::move(callback);
  return true;
}

bool SwitchControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                        ButtonCallback callback)
{
  if (controller_index < 0 || controller_index >= MAX_CONTROLLERS)
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXES)
    return false;

  m_controllers[controller_index].axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool SwitchControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                       std::string_view hat_position, ButtonCallback callback)
{
  return false;
}

bool SwitchControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number, AxisCallback callback)
{
  if (controller_index < 0 || controller_index >= MAX_CONTROLLERS)
    return false;

  if (button_number < 0 || button_number >= MAX_NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_axis_mapping[button_number] = std::move(callback);
  return true;
}

u32 SwitchControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  return 0;
}

void SwitchControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors)
{}

bool SwitchControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  if (controller_index < 0 || controller_index >= MAX_CONTROLLERS)
    return false;

  m_controllers[controller_index].deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  INFO_LOG("{}", fmt::sprintf("Controller %d deadzone size set to %f", controller_index, m_controllers[controller_index].deadzone));
  return true;
}
