/// @file DuckSlangChain.cpp
/// @brief RetroArch slang presets on DuckStation's GPUDevice. See DuckSlangChain.h.
///
/// Semantics follow RetroArch's filter chain, as tico's Vulkan chain does: a
/// pass's filter/wrap settings describe how it samples its Source, so
/// PassOutputN is sampled with the settings of pass N+1 and Original with pass
/// 0's; the last pass defaults to viewport scale, and an explicitly scaled last
/// pass gets a stock pass after it so the result still lands at viewport size.
/// Mipmapped inputs are sampled from their first level.

#include "DuckSlangChain.h"
#include "TicoLogger.h"

#include "util/gpu_device.h"
#include "util/image.h"

#include "common/error.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#define CHAIN_TAG "SHADER"

namespace
{

const char* kStockShader = R"(#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main()
{
    FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0);
}
)";

struct Vertex
{
  float x, y, z, w;
  float u, v;
};

// Position 0..1 to clip space; the flipped one puts y = 0 at the top of a
// lower-left origin device's screen.
constexpr float kMvp[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};
constexpr float kMvpFlipped[16] = {2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};

struct PassGPU
{
  TicoSlang::Pass cfg;
  TicoSlang::Reflection refl;
  std::string vertex; // rewritten to DuckStation's slots
  std::string fragment;
  GPUTexture::Format format = GPUTexture::Format::RGBA8;
  std::unique_ptr<GPUShader> vs;
  std::unique_ptr<GPUShader> fs;
  std::unique_ptr<GPUPipeline> pipeline;
  GPUTexture::Format pipelineFormat = GPUTexture::Format::Unknown;
  std::vector<u8> ubo;

  std::unique_ptr<GPUTexture> output;
  std::unique_ptr<GPUTexture> feedback; // the previous frame's output, when something reads it
  bool hasFeedback = false;
  u32 outW = 0;
  u32 outH = 0;
};

struct Lut
{
  std::string name;
  std::string path;
  bool linear = false;
  TicoSlang::WrapMode wrap = TicoSlang::WrapMode::ClampToBorder;
  std::unique_ptr<GPUTexture> tex;
};

GPUTexture::Format ParseFormat(const std::string& s)
{
  static const struct
  {
    const char* name;
    GPUTexture::Format fmt;
  } kFormats[] = {
    {"R8_UNORM", GPUTexture::Format::R8},
    {"R8G8_UNORM", GPUTexture::Format::RG8},
    {"R8G8B8A8_UNORM", GPUTexture::Format::RGBA8},
    {"R8G8B8A8_SRGB", GPUTexture::Format::RGBA16F}, // see PassFormat
    {"A2B10G10R10_UNORM_PACK32", GPUTexture::Format::RGB10A2},
    {"R16_UINT", GPUTexture::Format::R16U},
    {"R16_SINT", GPUTexture::Format::R16I},
    {"R16_SFLOAT", GPUTexture::Format::R16F},
    {"R16G16_SFLOAT", GPUTexture::Format::RG16F},
    {"R16G16B16A16_SFLOAT", GPUTexture::Format::RGBA16F},
    {"R32_UINT", GPUTexture::Format::R32U},
    {"R32_SINT", GPUTexture::Format::R32I},
    {"R32_SFLOAT", GPUTexture::Format::R32F},
    {"R32G32_SFLOAT", GPUTexture::Format::RG32F},
    {"R32G32B32A32_SFLOAT", GPUTexture::Format::RGBA32F},
  };
  for (const auto& f : kFormats)
    if (s == f.name)
      return f.fmt;
  return GPUTexture::Format::Unknown;
}

GPUTexture::Format PassFormat(const TicoSlang::Pass& cfg)
{
  GPUTexture::Format fmt = ParseFormat(cfg.source.format);
  // no sRGB targets in GPUDevice: a pass that wants one keeps its linear
  // values in half floats instead, without the banding 8 bits would give them
  if (fmt == GPUTexture::Format::Unknown)
    fmt = (cfg.floatFramebuffer || cfg.srgbFramebuffer) ? GPUTexture::Format::RGBA16F : GPUTexture::Format::RGBA8;
  return g_gpu_device->SupportsTextureFormat(fmt) ? fmt : GPUTexture::Format::RGBA8;
}

bool StartsWith(const std::string& s, const char* prefix, std::string& rest)
{
  const size_t n = std::strlen(prefix);
  if (s.compare(0, n, prefix) != 0)
    return false;
  rest = s.substr(n);
  return true;
}

bool ParseIndex(const std::string& s, int& out)
{
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
    return false;
  out = std::atoi(s.c_str());
  return true;
}

/// Rewrites a pass to DuckStation's slots and reflects it.
bool PreparePass(PassGPU& p, std::string& error)
{
  TicoSlang::ShaderSource src = p.cfg.source;
  if (!TicoSlang::RemapToFixedSlots(src, GPUDevice::MAX_TEXTURE_SAMPLERS, error))
    return false;
  TicoSlang::CompiledPass compiled;
  if (!TicoSlang::Compile(src, compiled, error))
    return false;
  p.refl = compiled.reflection;
  p.vertex = std::move(src.vertex);
  p.fragment = std::move(src.fragment);
  p.ubo.assign(std::max<u32>(16, (p.refl.uboSize + 15) & ~15u), 0);
  return true;
}

bool CreatePipeline(PassGPU& p, GPUTexture::Format format, std::string& error)
{
  Error err;
  if (!p.vs && !(p.vs = g_gpu_device->CreateShader(GPUShaderStage::Vertex, GPUShaderLanguage::GLSLVK, p.vertex, &err)))
  {
    error = "vertex shader: " + err.GetDescription();
    return false;
  }
  if (!p.fs &&
      !(p.fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, GPUShaderLanguage::GLSLVK, p.fragment, &err)))
  {
    error = "fragment shader: " + err.GetDescription();
    return false;
  }

  static constexpr GPUPipeline::VertexAttribute kAttributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 4, offsetof(Vertex, x)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, offsetof(Vertex, u)),
  };

  GPUPipeline::GraphicsConfig config = {};
  config.layout = GPUPipeline::Layout::MultiTextureAndUBO;
  config.primitive = GPUPipeline::Primitive::TriangleStrips;
  config.input_layout.vertex_attributes = kAttributes;
  config.input_layout.vertex_stride = sizeof(Vertex);
  config.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  config.depth = GPUPipeline::DepthState::GetNoTestsState();
  config.blend = GPUPipeline::BlendState::GetNoBlendingState();
  config.vertex_shader = p.vs.get();
  config.geometry_shader = nullptr;
  config.fragment_shader = p.fs.get();
  config.SetTargetFormats(format);
  config.samples = 1;
  config.per_sample_shading = false;
  config.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  p.pipeline = g_gpu_device->CreatePipeline(config, &err);
  if (!p.pipeline)
  {
    error = "pipeline: " + err.GetDescription();
    return false;
  }
  p.pipelineFormat = format;
  return true;
}

bool EnsureTarget(std::unique_ptr<GPUTexture>& t, u32 w, u32 h, GPUTexture::Format format)
{
  if (t && t->GetWidth() == w && t->GetHeight() == h && t->GetFormat() == format)
    return true;
  if (t)
    g_gpu_device->RecycleTexture(std::move(t));
  t = g_gpu_device->FetchTexture(w, h, 1, 1, 1, GPUTexture::Type::RenderTarget, format);
  if (!t)
    return false;
  // cleared now (binding commits the clear), not in the middle of the
  // presentation, where the first sampling of it would otherwise clear it
  g_gpu_device->ClearRenderTarget(t.get(), 0);
  g_gpu_device->SetTextureSampler(0, t.get(), nullptr);
  t->MakeReadyForSampling();
  return true;
}

void Release(std::unique_ptr<GPUTexture>& t)
{
  if (t)
    g_gpu_device->RecycleTexture(std::move(t));
}

/// A texture a shader can read, with the sampler settings that apply to it.
struct TexRef
{
  GPUTexture* tex = nullptr;
  bool linear = false;
  TicoSlang::WrapMode wrap = TicoSlang::WrapMode::ClampToBorder;
};

// `smooth` is the filter for a pass that does not choose one (RetroArch's
// video_smooth).
TexRef FromPassSettings(GPUTexture* tex, const TicoSlang::Pass& settings, bool smooth)
{
  return {tex, settings.filterSet ? settings.filterLinear : smooth, settings.wrap};
}

} // namespace

//==============================================================================
// Runtime: one loaded preset
//==============================================================================

struct DuckSlangChain::Runtime
{
  PassGPU copy; // built-in: the game's display area into `source`
  std::vector<PassGPU> passes;
  std::vector<Lut> luts;
  std::vector<TicoSlang::Parameter> parameters;
  u32 historyDepth = 0; // previous source frames any pass reads

  bool gpuReady = false;
  std::unique_ptr<GPUTexture> source;
  std::vector<std::unique_ptr<GPUTexture>> history;
  std::unique_ptr<GPUTexture> dummy;
  std::map<u32, std::unique_ptr<GPUSampler>> samplers;

  GPUSampler* Sampler(bool linear, TicoSlang::WrapMode wrap)
  {
    std::unique_ptr<GPUSampler>& s = samplers[(static_cast<u32>(wrap) << 1) | (linear ? 1u : 0u)];
    if (!s)
    {
      GPUSampler::Config config = linear ? GPUSampler::GetLinearConfig() : GPUSampler::GetNearestConfig();
      GPUSampler::AddressMode mode;
      switch (wrap)
      {
        case TicoSlang::WrapMode::ClampToEdge: mode = GPUSampler::AddressMode::ClampToEdge; break;
        case TicoSlang::WrapMode::Repeat: mode = GPUSampler::AddressMode::Repeat; break;
        case TicoSlang::WrapMode::MirroredRepeat: mode = GPUSampler::AddressMode::MirrorRepeat; break;
        default: mode = GPUSampler::AddressMode::ClampToBorder; break;
      }
      config.address_u = mode;
      config.address_v = mode;
      config.address_w = mode;
      config.border_color = 0; // transparent black, as RetroArch
      s = g_gpu_device->CreateSampler(config);
    }
    return s.get();
  }

  void ReleaseGPU()
  {
    auto releasePass = [](PassGPU& p) {
      p.pipeline.reset();
      p.pipelineFormat = GPUTexture::Format::Unknown;
      p.vs.reset();
      p.fs.reset();
      Release(p.output);
      Release(p.feedback);
    };
    releasePass(copy);
    for (PassGPU& p : passes)
      releasePass(p);
    for (Lut& l : luts)
      l.tex.reset();
    Release(source);
    for (std::unique_ptr<GPUTexture>& h : history)
      Release(h);
    history.clear();
    dummy.reset();
    samplers.clear();
    gpuReady = false;
  }

  bool BuildGPU(std::string& error)
  {
    if (gpuReady)
      return true;
    if (!CreatePipeline(copy, GPUTexture::Format::RGBA8, error))
    {
      error = "built-in pass: " + error;
      return false;
    }
    // the last pass's pipeline follows its target's format, in Apply
    for (size_t i = 0; i + 1 < passes.size(); i++)
    {
      if (!CreatePipeline(passes[i], passes[i].format, error))
      {
        error = passes[i].cfg.path + ": " + error;
        return false;
      }
    }
    for (Lut& l : luts)
    {
      RGBA8Image image;
      if (!image.LoadFromFile(l.path.c_str()))
      {
        error = "Cannot load texture " + l.path;
        return false;
      }
      l.tex = g_gpu_device->CreateTexture(image.GetWidth(), image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
                                          GPUTexture::Format::RGBA8, image.GetPixels(), image.GetPitch());
      if (!l.tex)
      {
        error = "Texture allocation failed for " + l.path;
        return false;
      }
    }
    static constexpr u32 kBlack = 0;
    dummy = g_gpu_device->CreateTexture(1, 1, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8, &kBlack,
                                        sizeof(kBlack));
    if (!dummy)
    {
      error = "Texture allocation failed";
      return false;
    }
    gpuReady = true;
    return true;
  }
};

//==============================================================================
// DuckSlangChain
//==============================================================================

DuckSlangChain::DuckSlangChain() = default;

DuckSlangChain::~DuckSlangChain()
{
  ReleaseGPUResources();
}

bool DuckSlangChain::IsActive() const
{
  return m_runtime != nullptr;
}

bool DuckSlangChain::LoadPreset(const std::string& path, std::string& error)
{
  if (path.empty())
  {
    ReleaseGPUResources();
    m_runtime.reset();
    m_preset_path.clear();
    return true;
  }

  TicoSlang::Preset preset;
  if (!TicoSlang::LoadPreset(path, preset, error))
    return false;
  // An explicitly scaled last pass renders offscreen; RetroArch then adds a
  // stock pass so the image still reaches viewport size.
  const TicoSlang::Pass& last = preset.passes.back();
  const bool viewportLast =
    (last.scaleTypeX == TicoSlang::ScaleType::Unset && last.scaleTypeY == TicoSlang::ScaleType::Unset) ||
    (last.scaleTypeX == TicoSlang::ScaleType::Viewport && last.scaleTypeY == TicoSlang::ScaleType::Viewport &&
     last.scaleX == 1.0f && last.scaleY == 1.0f);
  TicoSlang::Preset stock;
  if (!TicoSlang::PresetFromSource("stock", kStockShader, stock, error))
    return false;
  if (!viewportLast)
    preset.passes.push_back(stock.passes[0]);

  auto rt = std::make_unique<Runtime>();
  rt->parameters = preset.parameters;
  rt->copy.cfg = stock.passes[0];
  if (!PreparePass(rt->copy, error))
  {
    error = "built-in pass: " + error;
    return false;
  }
  for (const TicoSlang::Pass& cfg : preset.passes)
  {
    PassGPU p;
    p.cfg = cfg;
    if (!PreparePass(p, error))
    {
      error = cfg.path + ": " + error;
      return false;
    }
    p.format = PassFormat(cfg);
    rt->passes.push_back(std::move(p));
  }
  for (const TicoSlang::Texture& t : preset.textures)
  {
    Lut l;
    l.name = t.name;
    l.path = t.path;
    l.linear = t.linear;
    l.wrap = t.wrap;
    rt->luts.push_back(std::move(l));
  }

  // Work out which outputs need history or feedback copies.
  for (PassGPU& p : rt->passes)
  {
    for (const TicoSlang::SamplerBinding& s : p.refl.samplers)
    {
      std::string rest;
      int n = 0;
      if (StartsWith(s.name, "OriginalHistory", rest) && ParseIndex(rest, n))
        rt->historyDepth = std::max<u32>(rt->historyDepth, n);
      else if (StartsWith(s.name, "PassFeedback", rest) && ParseIndex(rest, n) && n < (int)rt->passes.size())
        rt->passes[n].hasFeedback = true;
      else
      {
        for (PassGPU& q : rt->passes)
          if (!q.cfg.alias.empty() && s.name == q.cfg.alias + "Feedback")
            q.hasFeedback = true;
      }
    }
  }

  // Shaders, pipelines and textures now, so a broken preset is reported here.
  if (!rt->BuildGPU(error) || !CreatePipeline(rt->passes.back(), g_gpu_device->GetWindowFormat(), error))
  {
    rt->ReleaseGPU();
    return false;
  }

  ReleaseGPUResources();
  m_runtime = std::move(rt);
  m_preset_path = path;
  LOG_INFO(CHAIN_TAG, "Loaded %s: %zu pass(es), %zu texture(s), %zu parameter(s)", path.c_str(),
           m_runtime->passes.size(), m_runtime->luts.size(), m_runtime->parameters.size());
  return true;
}

const std::vector<TicoSlang::Parameter>& DuckSlangChain::Parameters() const
{
  static const std::vector<TicoSlang::Parameter> kNone;
  return m_runtime ? m_runtime->parameters : kNone;
}

void DuckSlangChain::SetParameter(const std::string& id, float value)
{
  if (!m_runtime)
    return;
  for (TicoSlang::Parameter& p : m_runtime->parameters)
    if (p.id == id)
      p.value = std::clamp(value, p.minimum, p.maximum);
}

void DuckSlangChain::ResetParameters()
{
  if (!m_runtime)
    return;
  for (TicoSlang::Parameter& p : m_runtime->parameters)
    p.value = p.initial;
}

void DuckSlangChain::ReleaseGPUResources()
{
  if (m_runtime)
    m_runtime->ReleaseGPU();
}

DuckSlangChain::Result DuckSlangChain::Apply(GPUTexture* source, const GSVector4i source_rect,
                                             GPUTexture* final_target, const GSVector4i final_rect, float aspect,
                                             double fps)
{
  if (!m_runtime || !source || source_rect.rempty() || final_rect.rempty())
    return Result::NotApplied;

  Runtime& rt = *m_runtime;
  std::string error;
  if (!rt.BuildGPU(error))
  {
    // a device that cannot run it: back to DuckStation's display
    LOG_ERROR(CHAIN_TAG, "%s", error.c_str());
    rt.ReleaseGPU();
    m_runtime.reset();
    return Result::NotApplied;
  }

  const GPUTexture::Format final_format = final_target ? final_target->GetFormat() : g_gpu_device->GetWindowFormat();
  PassGPU& lastPass = rt.passes.back();
  if (lastPass.pipelineFormat != final_format)
  {
    lastPass.pipeline.reset();
    if (!CreatePipeline(lastPass, final_format, error))
    {
      LOG_ERROR(CHAIN_TAG, "%s: %s", lastPass.cfg.path.c_str(), error.c_str());
      return Result::NotApplied;
    }
  }

  const u32 srcW = static_cast<u32>(source_rect.width());
  const u32 srcH = static_cast<u32>(source_rect.height());
  const u32 viewportW = static_cast<u32>(final_rect.width());
  const u32 viewportH = static_cast<u32>(final_rect.height());

  // History: the ring holds the frames before the current one, which the
  // source still holds.
  if (rt.history.size() != rt.historyDepth || (rt.source && (rt.source->GetWidth() != srcW ||
                                                             rt.source->GetHeight() != srcH)))
  {
    for (std::unique_ptr<GPUTexture>& h : rt.history)
      Release(h);
    rt.history.clear();
    for (u32 i = 0; i < rt.historyDepth; i++)
    {
      rt.history.emplace_back();
      if (!EnsureTarget(rt.history.back(), srcW, srcH, GPUTexture::Format::RGBA8))
        return Result::NotApplied;
    }
  }
  else if (!rt.history.empty() && rt.source)
  {
    std::rotate(rt.history.rbegin(), rt.history.rbegin() + 1, rt.history.rend());
    g_gpu_device->CopyTextureRegion(rt.history[0].get(), 0, 0, 0, 0, rt.source.get(), 0, 0, 0, 0, srcW, srcH);
    rt.history[0]->MakeReadyForSampling();
  }

  // Last frame's outputs become this frame's feedback.
  for (PassGPU& p : rt.passes)
    if (p.hasFeedback)
      std::swap(p.output, p.feedback);

  // Size every target; the last pass draws straight into the final target.
  u32 prevW = srcW, prevH = srcH;
  for (size_t i = 0; i < rt.passes.size(); i++)
  {
    PassGPU& p = rt.passes[i];
    const bool last = i + 1 == rt.passes.size();
    TicoSlang::ScaleType sx = p.cfg.scaleTypeX, sy = p.cfg.scaleTypeY;
    if (sx == TicoSlang::ScaleType::Unset && sy == TicoSlang::ScaleType::Unset)
      sx = sy = last ? TicoSlang::ScaleType::Viewport : TicoSlang::ScaleType::Source;
    auto axis = [](TicoSlang::ScaleType t, float scale, u32 prev, u32 vp) -> u32 {
      switch (t)
      {
        case TicoSlang::ScaleType::Viewport: return static_cast<u32>(std::lround(vp * scale));
        case TicoSlang::ScaleType::Absolute: return static_cast<u32>(std::lround(scale));
        default: return static_cast<u32>(std::lround(prev * scale));
      }
    };
    u32 w = std::clamp<u32>(axis(sx, p.cfg.scaleX, prevW, viewportW), 1, 8192);
    u32 h = std::clamp<u32>(axis(sy, p.cfg.scaleY, prevH, viewportH), 1, 8192);
    if (last)
    {
      w = viewportW;
      h = viewportH;
    }
    p.outW = w;
    p.outH = h;
    prevW = w;
    prevH = h;

    if (!last && !EnsureTarget(p.output, w, h, p.format))
      return Result::NotApplied;
    if (p.hasFeedback && !EnsureTarget(p.feedback, w, h, last ? final_format : p.format))
      return Result::NotApplied;
  }

  // Resolve a texture name for pass `index` (RetroArch semantics).
  auto settingsAfter = [&](size_t k) -> const TicoSlang::Pass& {
    return k + 1 < rt.passes.size() ? rt.passes[k + 1].cfg : rt.passes[k].cfg;
  };
  auto resolve = [&](const std::string& name, size_t index) -> TexRef {
    std::string rest;
    int n = 0;
    if (name == "Original")
      return FromPassSettings(rt.source.get(), rt.passes[0].cfg, m_smooth);
    if (name == "Source")
      return FromPassSettings(index == 0 ? rt.source.get() : rt.passes[index - 1].output.get(), rt.passes[index].cfg,
                              m_smooth);
    if (StartsWith(name, "OriginalHistory", rest) && ParseIndex(rest, n))
    {
      GPUTexture* tex = n == 0                            ? rt.source.get() :
                        (size_t)n <= rt.history.size() ? rt.history[n - 1].get() :
                                                            nullptr;
      return FromPassSettings(tex, rt.passes[0].cfg, m_smooth);
    }
    if (StartsWith(name, "PassOutput", rest) && ParseIndex(rest, n) && (size_t)n < index)
      return FromPassSettings(rt.passes[n].output.get(), settingsAfter(n), m_smooth);
    if (StartsWith(name, "PassFeedback", rest) && ParseIndex(rest, n) && (size_t)n < rt.passes.size() &&
        rt.passes[n].hasFeedback)
      return FromPassSettings(rt.passes[n].feedback.get(), settingsAfter(n), m_smooth);
    if (StartsWith(name, "User", rest) && ParseIndex(rest, n) && (size_t)n < rt.luts.size())
      return {rt.luts[n].tex.get(), rt.luts[n].linear, rt.luts[n].wrap};
    for (size_t k = 0; k < rt.passes.size(); k++)
    {
      const std::string& alias = rt.passes[k].cfg.alias;
      if (alias.empty())
        continue;
      if (name == alias && k < index)
        return FromPassSettings(rt.passes[k].output.get(), settingsAfter(k), m_smooth);
      if (name == alias + "Feedback" && rt.passes[k].hasFeedback)
        return FromPassSettings(rt.passes[k].feedback.get(), settingsAfter(k), m_smooth);
    }
    for (const Lut& l : rt.luts)
      if (name == l.name)
        return {l.tex.get(), l.linear, l.wrap};
    return {};
  };
  auto sizeOf = [&](const std::string& name, size_t index, float out[4]) -> bool {
    std::string rest, tex;
    if (name == "OutputSize")
    {
      const PassGPU& p = rt.passes[index];
      out[0] = (float)p.outW, out[1] = (float)p.outH;
    }
    else if (name == "FinalViewportSize")
      out[0] = (float)viewportW, out[1] = (float)viewportH;
    else
    {
      if (StartsWith(name, "OriginalHistorySize", rest))
        tex = "OriginalHistory" + rest;
      else if (StartsWith(name, "PassOutputSize", rest))
        tex = "PassOutput" + rest;
      else if (StartsWith(name, "PassFeedbackSize", rest))
        tex = "PassFeedback" + rest;
      else if (StartsWith(name, "UserSize", rest))
        tex = "User" + rest;
      else if (name.size() > 4 && name.compare(name.size() - 4, 4, "Size") == 0)
        tex = name.substr(0, name.size() - 4);
      else
        return false;
      const TexRef r = resolve(tex, index);
      if (!r.tex)
        return false;
      out[0] = (float)r.tex->GetWidth(), out[1] = (float)r.tex->GetHeight();
    }
    out[2] = 1.0f / out[0];
    out[3] = 1.0f / out[1];
    return true;
  };

  auto draw = [&](PassGPU& p, float u0, float v0, float u1, float v1) {
    const Vertex quad[4] = {
      {0, 0, 0, 1, u0, v0},
      {1, 0, 0, 1, u1, v0},
      {0, 1, 0, 1, u0, v1},
      {1, 1, 0, 1, u1, v1},
    };
    u32 base_vertex;
    g_gpu_device->UploadVertexBuffer(quad, sizeof(Vertex), 4, &base_vertex);
    g_gpu_device->UploadUniformBuffer(p.ubo.data(), static_cast<u32>(p.ubo.size()));
    g_gpu_device->Draw(4, base_vertex);
  };

  // No pass's target may stay bound as a texture.
  auto unbindAll = [&]() {
    for (u32 s = 0; s < GPUDevice::MAX_TEXTURE_SAMPLERS; s++)
      g_gpu_device->SetTextureSampler(s, rt.dummy.get(), rt.Sampler(false, TicoSlang::WrapMode::ClampToEdge));
  };

  auto setMvp = [](PassGPU& p, const float* mvp) {
    std::fill(p.ubo.begin(), p.ubo.end(), 0);
    for (const TicoSlang::UniformMember& m : p.refl.members)
      if (m.name == "MVP" && m.size >= 64 && m.offset + 64 <= p.ubo.size())
        std::memcpy(p.ubo.data() + m.offset, mvp, 64);
  };

  // The built-in pass: the game's display area into the source image.
  if (!EnsureTarget(rt.source, srcW, srcH, GPUTexture::Format::RGBA8))
    return Result::NotApplied;
  source->MakeReadyForSampling();
  unbindAll();
  g_gpu_device->SetRenderTarget(rt.source.get());
  g_gpu_device->SetPipeline(rt.copy.pipeline.get());
  for (const TicoSlang::SamplerBinding& s : rt.copy.refl.samplers)
    g_gpu_device->SetTextureSampler(s.binding, source, rt.Sampler(false, TicoSlang::WrapMode::ClampToEdge));
  g_gpu_device->SetViewportAndScissor(0, 0, srcW, srcH);
  setMvp(rt.copy, kMvp);
  {
    const float rcpW = 1.0f / static_cast<float>(source->GetWidth());
    const float rcpH = 1.0f / static_cast<float>(source->GetHeight());
    draw(rt.copy, source_rect.left * rcpW, source_rect.top * rcpH, source_rect.right * rcpW,
         source_rect.bottom * rcpH);
  }
  rt.source->MakeReadyForSampling();

  const bool flipFinal = g_gpu_device->UsesLowerLeftOrigin();
  for (size_t i = 0; i < rt.passes.size(); i++)
  {
    PassGPU& p = rt.passes[i];
    const bool last = i + 1 == rt.passes.size();

    // Uniforms.
    setMvp(p, last && flipFinal ? kMvpFlipped : kMvp);
    for (const TicoSlang::UniformMember& m : p.refl.members)
    {
      if (m.offset + m.size > p.ubo.size() || m.name == "MVP")
        continue;
      u8* dst = p.ubo.data() + m.offset;
      float vec[4];
      if (m.name == "FrameCount" && m.size >= 4)
      {
        const u32 v = p.cfg.frameCountMod ? m_frame_count % p.cfg.frameCountMod : m_frame_count;
        std::memcpy(dst, &v, 4);
      }
      else if (m.name == "FrameDirection" && m.size >= 4)
      {
        const s32 v = 1;
        std::memcpy(dst, &v, 4);
      }
      else if ((m.name == "TotalSubFrames" || m.name == "CurrentSubFrame") && m.size >= 4)
      {
        const u32 v = 1;
        std::memcpy(dst, &v, 4);
      }
      else if (m.name == "FrameTimeDelta" && m.size >= 4)
      {
        const u32 v = fps > 0 ? static_cast<u32>(1000000.0 / fps) : 16667;
        std::memcpy(dst, &v, 4);
      }
      else if ((m.name == "OriginalFPS" || m.name == "CoreFPS") && m.size >= 4)
      {
        const float v = static_cast<float>(fps);
        std::memcpy(dst, &v, 4);
      }
      else if ((m.name == "OriginalAspect" || m.name == "OriginalAspectRotated") && m.size >= 4)
        std::memcpy(dst, &aspect, 4);
      else if (m.size >= 16 && sizeOf(m.name, i, vec))
        std::memcpy(dst, vec, 16);
      else if (m.size == 4)
      {
        for (const TicoSlang::Parameter& param : rt.parameters)
          if (param.id == m.name)
            std::memcpy(dst, &param.value, 4);
      }
    }

    // Target.
    unbindAll();
    if (!last)
      g_gpu_device->SetRenderTarget(p.output.get());
    else if (final_target)
      g_gpu_device->SetRenderTarget(final_target);
    else if (!g_gpu_device->BeginPresent(false))
      return Result::Skipped;

    // Textures.
    g_gpu_device->SetPipeline(p.pipeline.get());
    for (const TicoSlang::SamplerBinding& s : p.refl.samplers)
    {
      const TexRef r = resolve(s.name, i);
      if (r.tex)
        g_gpu_device->SetTextureSampler(s.binding, r.tex, rt.Sampler(r.linear, r.wrap));
    }

    if (last)
      g_gpu_device->SetViewportAndScissor(final_rect);
    else
      g_gpu_device->SetViewportAndScissor(0, 0, p.outW, p.outH);
    draw(p, 0.0f, 0.0f, 1.0f, 1.0f);

    if (!last)
      p.output->MakeReadyForSampling();
  }

  m_frame_count++;
  return Result::Drawn;
}
