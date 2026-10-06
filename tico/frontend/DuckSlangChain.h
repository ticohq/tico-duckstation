/// @file DuckSlangChain.h
/// @brief RetroArch slang presets (the shaders tico's other cores use) run on
/// DuckStation's GPUDevice, so they work on deko3D and Vulkan alike.
///
/// The game's display area is copied into a source image by a built-in pass,
/// every pass of the preset renders into its own target, and the last one
/// draws into the screen (or the given target) at the game's rectangle, in
/// place of DuckStation's display pass. Parsing and reflection are TicoSlang's,
/// as in the other cores; each pass is rewritten to DuckStation's fixed slots
/// (one uniform buffer, eight textures) first.
#pragma once

#include "TicoSlang.h"

#include "common/gsvector.h"

#include <memory>
#include <string>
#include <vector>

class GPUTexture;

class DuckSlangChain
{
public:
  DuckSlangChain();
  ~DuckSlangChain();

  /// Loads a .slangp; false (and the previous preset kept) on failure. An
  /// empty path unloads the preset: the chain is then inactive.
  bool LoadPreset(const std::string& path, std::string& error);
  bool IsActive() const;
  const std::string& PresetPath() const { return m_preset_path; }

  /// The active preset's parameters, with their current values.
  const std::vector<TicoSlang::Parameter>& Parameters() const;
  void SetParameter(const std::string& id, float value);
  void ResetParameters();

  /// Bilinear filtering for passes that do not set filter_linear.
  void SetSmooth(bool smooth) { m_smooth = smooth; }

  enum class Result
  {
    NotApplied, // nothing drawn: DuckStation's display pass draws instead
    Skipped,    // the frame is not presented (BeginPresent declined it)
    Drawn,
  };

  /// Runs the passes over `source` (`source_rect` is the picture in it) and
  /// draws the last into `final_target` at `final_rect`, or into the screen
  /// (BeginPresent, which this calls) when it is null. `final_rect` is in the
  /// device's coordinates: flipped already on a lower-left origin device.
  Result Apply(GPUTexture* source, const GSVector4i source_rect, GPUTexture* final_target,
               const GSVector4i final_rect, float aspect, double fps);

  /// Frees everything the GPU device owns; the preset loads again on demand.
  void ReleaseGPUResources();

  struct Runtime;

private:
  std::unique_ptr<Runtime> m_runtime;
  std::string m_preset_path;
  bool m_smooth = false;
  u32 m_frame_count = 0;
};
