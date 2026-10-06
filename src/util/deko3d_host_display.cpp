#include "deko3d_host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/deko3d/context.h"
#include "common/deko3d/shader_cache.h"
#include "common/deko3d/util.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/settings.h"
#include "core/shader_cache_version.h"
#include "frontend-common/imgui_impl_deko3d.h"
#include "imgui.h"
#include "imgui_impl_deko3d.h"
#include "postprocessing_shadergen.h"
#include <alloca.h>
#include <array>
#include <switch.h>
#include <tuple>
#include "fmt/printf.h"
Log_SetChannel(Deko3DHostDisplay);

Deko3DHostDisplay::Deko3DHostDisplay() = default;

Deko3DHostDisplay::~Deko3DHostDisplay()
{
  if (!g_deko3d_context)
    return;

  g_deko3d_context->WaitGPUIdle();

  DestroyResources();
  DestroySurface();

  Deko3D::Context::Destroy();
}

RenderAPI Deko3DHostDisplay::GetRenderAPI() const
{
  return RenderAPI::Deko3D;
}

void* Deko3DHostDisplay::GetDevice() const
{
  return nullptr;
}

void* Deko3DHostDisplay::GetContext() const
{
  return nullptr;
}

bool Deko3DHostDisplay::HasDevice() const
{
  return g_deko3d_context != nullptr;
}

bool Deko3DHostDisplay::HasSurface() const
{
  return g_deko3d_context != nullptr;
}

bool Deko3DHostDisplay::CreateDevice(const WindowInfo& wi, bool vsync)
{
  WindowInfo local_wi(wi);
  if (!Deko3D::Context::Create(&local_wi))
  {
    ERROR_LOG("{}", fmt::sprintf("Failed to create deko3D context"));
    m_window_info = {};
    return false;
  }
  m_swap_chain = Deko3D::SwapChain::Create(local_wi);
  if (!m_swap_chain)
  {
    ERROR_LOG("{}", fmt::sprintf("Failed to create swapchain"));
  }

  m_window_info = m_swap_chain ? m_swap_chain->GetWindowInfo() : local_wi;

#ifdef NDEBUG
  const bool debug = false;
#else
  const bool debug = true;
#endif

  Deko3D::ShaderCache::Create(EmuFolders::Cache, SHADER_CACHE_VERSION, debug);

  return true;
}

bool Deko3DHostDisplay::SetupDevice()
{
  if (!CreateResources())
    return false;

  return true;
}

bool Deko3DHostDisplay::MakeCurrent()
{
  return true;
}

bool Deko3DHostDisplay::DoneCurrent()
{
  return true;
}

bool Deko3DHostDisplay::ChangeWindow(const WindowInfo& new_wi)
{
  return false;
}

void Deko3DHostDisplay::ResizeWindow(s32 new_window_width, s32 new_window_height) {
  g_deko3d_context->WaitGPUIdle();

  m_window_info.surface_width = new_window_width;
  m_window_info.surface_height = new_window_height;

  m_swap_chain.reset();
  m_swap_chain = Deko3D::SwapChain::Create(m_window_info);
}

bool Deko3DHostDisplay::SupportsFullscreen() const
{
  return false;
}

bool Deko3DHostDisplay::IsFullscreen()
{
  return false;
}

bool Deko3DHostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
{
  return false;
}

HostDisplay::AdapterAndModeList Deko3DHostDisplay::GetAdapterAndModeList()
{
  return {};
}

void Deko3DHostDisplay::DestroySurface()
{
  m_window_info = {};
  g_deko3d_context->WaitGPUIdle();
  m_swap_chain.reset();
}

bool Deko3DHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  if (config.empty())
  {
    m_post_processing_input_texture.Destroy();
    DestroyPostProcessingStages(true);
    m_post_processing_chain.ClearStages();
    return true;
  }

  if (!m_post_processing_chain.CreateFromString(config))
    return false;

  DestroyPostProcessingStages(true);

  FrontendCommon::PostProcessingShaderGen shadergen(RenderAPI::Deko3D, true);

  for (u32 i = 0; i < m_post_processing_chain.GetStageCount(); i++)
  {
    const FrontendCommon::PostProcessingShader& shader = m_post_processing_chain.GetShaderStage(i);

    // cheating, but I'm currently too lazy to implement non push uniforms
    if (shader.GetUniformsSize() > m_uniform_buffer.size)
      return false;

    const std::string vs = shadergen.GeneratePostProcessingVertexShader(shader);
    const std::string ps = shadergen.GeneratePostProcessingFragmentShader(shader);

    PostProcessingStage stage;
    stage.uniforms_size = shader.GetUniformsSize();

    if (!g_deko3d_shader_cache->GetVertexShader(vs, stage.vertex_shader, stage.vertex_shader_memory))
    {
      INFO_LOG("{}", fmt::sprintf("Failed to compile post-processing program, disabling."));
      DestroyPostProcessingStages();
      m_post_processing_chain.ClearStages();
      return false;
    }
    if (!g_deko3d_shader_cache->GetFragmentShader(ps, stage.fragment_shader, stage.fragment_shader_memory))
    {
      INFO_LOG("{}", fmt::sprintf("Failed to compile post-processing program, disabling."));
      DestroyPostProcessingStages();
      m_post_processing_chain.ClearStages();
      return false;
    }

    m_post_processing_stages.push_back(std::move(stage));
  }

  m_post_processing_timer.Reset();

  g_deko3d_context->DeferedFree(&g_deko3d_context->GetGeneralHeap(), m_descriptor_buffer);
  m_descriptor_buffer = g_deko3d_context->GetGeneralHeap().Alloc(
    sizeof(dk::ImageDescriptor) * (m_post_processing_chain.GetStageCount() + 1), DK_IMAGE_DESCRIPTOR_ALIGNMENT);

  m_post_processing_descriptors_dirty = true;

  return true;
}

static constexpr std::array<DkImageFormat, static_cast<u32>(GPUTexture::Format::Count)> s_display_pixel_format_mapping =
  {{DkImageFormat_None, DkImageFormat_RGBA8_Unorm, DkImageFormat_BGRA8_Unorm, DkImageFormat_BGR565_Unorm,
    DkImageFormat_BGR5A1_Unorm, DkImageFormat_R8_Unorm, DkImageFormat_Z16}};

std::unique_ptr<GPUTexture> Deko3DHostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                             GPUTexture::Format format, const void* data,
                                                             u32 data_stride, bool dynamic)
{
  const DkImageFormat dk_format = s_display_pixel_format_mapping[static_cast<u32>(format)];
  if (dk_format == DkImageFormat_None)
    return {};

  std::unique_ptr<Deko3D::Texture> texture(std::make_unique<Deko3D::Texture>());
  if (!texture->Create(width, height, levels, layers, dk_format, static_cast<DkMsMode>(__builtin_ctz(samples)),
                       (layers > 1) ? DkImageType_2DArray : DkImageType_2D, 0))
  {
    return {};
  }

  if (data)
  {
    texture->Update(0, 0, width, height, 0, 0, data, data_stride);
  }

  return texture;
}

bool Deko3DHostDisplay::UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                                      u32 data_stride)
{
  return static_cast<Deko3D::Texture*>(texture)->Update(x, y, width, height, 0, 0, data, data_stride);
}

bool Deko3DHostDisplay::BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer,
                                           u32* out_pitch)
{
  return static_cast<Deko3D::Texture*>(texture)->BeginUpdate(width, height, out_buffer, out_pitch);
}

void Deko3DHostDisplay::EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height)
{
  static_cast<Deko3D::Texture*>(texture)->EndUpdate(x, y, width, height, 0, 0);
}

void Deko3DHostDisplay::CheckStagingBufferSize(u32 required_size)
{
  if (m_readback_buffer.size >= required_size)
    return;

  Deko3D::MemoryHeap& heap = g_deko3d_context->GetGeneralHeap();
  // no synchronisation necessary, because there's always a GPU/CPU sync
  // when using this buffer
  if (m_readback_buffer.size > 0)
    heap.Free(m_readback_buffer);
  m_readback_buffer = heap.Alloc(required_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
}

bool Deko3DHostDisplay::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                        u32 out_data_stride)
{
  Deko3D::Texture* tex = static_cast<Deko3D::Texture*>(texture);
  const u32 pitch = tex->CalcUpdatePitch(width);
  const u32 size = pitch * height;
  CheckStagingBufferSize(size);

  dk::CmdBuf cmdbuf = g_deko3d_context->GetCmdBuf();

  dk::ImageView srcView{tex->GetImage()};
  if (texture->GetFormat() == GPUTexture::Format::D16)
    srcView.setFormat(DkImageFormat_R16_Uint);

  Deko3D::MemoryHeap& heap = g_deko3d_context->GetGeneralHeap();

  // do the copy
  cmdbuf.copyImageToBuffer(srcView, {x, y, 0, width, height, 1}, {heap.GpuAddr(m_readback_buffer), 0, 0});
  cmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);

  g_deko3d_context->ExecuteCommandBuffer(true);

  StringUtil::StrideMemCpy(out_data, out_data_stride, heap.CpuAddr<void>(m_readback_buffer), pitch,
                           std::min(pitch, out_data_stride), height);
  return true;
}

bool Deko3DHostDisplay::SupportsTextureFormat(GPUTexture::Format format) const
{
  return format == GPUTexture::Format::RGBA8 || format == GPUTexture::Format::RGB565 ||
         format == GPUTexture::Format::RGBA5551;
}

void Deko3DHostDisplay::SetVSync(bool enabled) {}

bool Deko3DHostDisplay::Render(bool skip_present)
{
  if (ShouldSkipDisplayingFrame() || !m_swap_chain)
  {
    if (ImGui::GetCurrentContext())
      ImGui::Render();

    return false;
  }

  int imageSlot = m_swap_chain->AcquireImage();

  dk::CmdBuf cmdbuf = g_deko3d_context->GetCmdBuf();

  const Deko3D::Texture* final_fb = &m_swap_chain->GetImage(imageSlot);
  dk::ImageView colorTargetView{final_fb->GetImage()};
  cmdbuf.bindRenderTargets({&colorTargetView});
  cmdbuf.setScissors(0, {{0, 0, m_window_info.surface_width, m_window_info.surface_height}});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 1.f);

  RenderDisplay(final_fb);

  if (ImGui::GetCurrentContext())
    RenderImGui();

  g_deko3d_context->SubmitCommandBuffer(&m_swap_chain->GetCurrentAcquireFence(), false);
  g_deko3d_context->MoveToNextCommandBuffer();
  m_swap_chain->PresentImage(imageSlot);

  return true;
}

void Deko3DHostDisplay::RenderImGui()
{
  ImGui::Render();
  ImGui_ImplDeko3D_RenderDrawData(ImGui::GetDrawData(), g_deko3d_context->GetCmdBuf());
}

bool Deko3DHostDisplay::CheckPostProcessingRenderTargets(dk::CmdBuf cmdbuf, u32 target_width, u32 target_height)
{
  DebugAssert(!m_post_processing_stages.empty());

  if (m_post_processing_input_texture.GetWidth() != target_width ||
      m_post_processing_input_texture.GetHeight() != target_height)
  {
    if (!m_post_processing_input_texture.Create(target_width, target_height, 1, 1, DkImageFormat_RGBA8_Unorm,
                                                DkMsMode_1x, DkImageType_2D, DkImageFlags_UsageRender))
    {
      return false;
    }

    m_post_processing_descriptors_dirty = true;
  }

  const u32 target_count = (static_cast<u32>(m_post_processing_stages.size()) - 1);
  for (u32 i = 0; i < target_count; i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    if (pps.output_texture.GetWidth() != target_width || pps.output_texture.GetHeight() != target_height)
    {
      if (!pps.output_texture.Create(target_width, target_height, 1, 1, DkImageFormat_RGBA8_Unorm, DkMsMode_1x,
                                     DkImageType_2D, DkImageFlags_UsageRender))
      {
        return false;
      }
    }

    m_post_processing_descriptors_dirty = true;
  }

  return true;
}

void Deko3DHostDisplay::ApplyPostProcessingChain(const Deko3D::Texture* final_target, s32 final_left, s32 final_top,
                                                 s32 final_width, s32 final_height, const Deko3D::Texture* texture,
                                                 s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                                 s32 texture_view_height, u32 target_width, u32 target_height)
{
  dk::CmdBuf cmdbuf = g_deko3d_context->GetCmdBuf();
  if (!CheckPostProcessingRenderTargets(cmdbuf, target_width, target_height))
  {
    RenderDisplay(final_left, target_height - final_top - final_height, final_width, final_height, texture,
                  texture_view_x, texture_view_y, texture_view_width, texture_view_height, IsUsingLinearFiltering());
    return;
  }

  // downsample/upsample - use same viewport for remainder
  dk::ImageView post_processing_input{m_post_processing_input_texture.GetImage()};

  if (m_post_processing_descriptors_dirty)
  {
    u32 data_size = sizeof(dk::ImageDescriptor) * m_post_processing_chain.GetStageCount();
    dk::ImageDescriptor* descriptors = reinterpret_cast<dk::ImageDescriptor*>(alloca(data_size));

    descriptors[0].initialize(post_processing_input);
    for (u32 i = 0; i < m_post_processing_chain.GetStageCount() - 1; i++)
    {
      dk::ImageView view{m_post_processing_stages[i].output_texture.GetImage()};
      descriptors[i + 1].initialize(view);
    }

    cmdbuf.pushData(g_deko3d_context->GetGeneralHeap().GpuAddr(m_descriptor_buffer) + sizeof(dk::ImageDescriptor),
                    descriptors, data_size);

    m_post_processing_descriptors_dirty = false;
    // barrier and cache flush not necessary, because render display always does this for us
  }

  cmdbuf.bindRenderTargets({&post_processing_input});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 0, 0, 0, 0);

  RenderDisplay(final_left, target_height - final_top - final_height, final_width, final_height, texture,
                texture_view_x, texture_view_y, texture_view_width, texture_view_height, IsUsingLinearFiltering());

  const s32 orig_texture_width = texture_view_width;
  const s32 orig_texture_height = texture_view_height;
  texture = &m_post_processing_input_texture;
  texture_view_x = final_left;
  texture_view_y = final_top;
  texture_view_width = final_width;
  texture_view_height = final_height;

  const u32 final_stage = static_cast<u32>(m_post_processing_stages.size()) - 1u;
  for (u32 i = 0; i < static_cast<u32>(m_post_processing_stages.size()); i++)
  {
    cmdbuf.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);

    PostProcessingStage& pps = m_post_processing_stages[i];
    dk::ImageView rt{((i == final_stage) ? final_target : &pps.output_texture)->GetImage()};
    cmdbuf.bindRenderTargets({&rt});
    if (i != final_stage)
    {
      // for inbetween textures always clear the entire texture
      // (the last texture is the final fb which has already been cleared completely
      // before drawing the GUI)
      // there might be leftovers from previous frames which had a different scissor rect
      cmdbuf.setScissors(0, {{0, 0, pps.output_texture.GetWidth(), pps.output_texture.GetHeight()}});
      cmdbuf.clearColor(0, DkColorMask_RGBA, 0, 0, 0, 0);
    }
    cmdbuf.setScissors(cmdbuf, {{static_cast<u32>(final_left), static_cast<u32>(final_top),
                                 static_cast<u32>(final_width), static_cast<u32>(final_height)}});

    cmdbuf.bindShaders(DkStageFlag_Vertex | DkStageFlag_Fragment, {&pps.vertex_shader, &pps.fragment_shader});
    cmdbuf.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(i + 1, 0)});

    u8* uniforms = reinterpret_cast<u8*>(alloca(pps.uniforms_size));
    m_post_processing_chain.GetShaderStage(i).FillUniformBuffer(
      uniforms, texture->GetWidth(), texture->GetHeight(), texture_view_x, texture_view_y, texture_view_width,
      texture_view_height, GetWindowWidth(), GetWindowHeight(), orig_texture_width, orig_texture_height,
      static_cast<float>(m_post_processing_timer.GetTimeSeconds()));

    cmdbuf.pushConstants(g_deko3d_context->GetGeneralHeap().GpuAddr(m_uniform_buffer), m_uniform_buffer.size, 0,
                         pps.uniforms_size, uniforms);

    cmdbuf.draw(DkPrimitive_Triangles, 3, 1, 0, 0);

    if (i != final_stage)
      texture = &pps.output_texture;
  }

  cmdbuf.barrier(DkBarrier_Fragments, 0);
  cmdbuf.bindRenderTargets({&post_processing_input});
  cmdbuf.discardColor(0);
  for (u32 i = 0; i < static_cast<u32>(m_post_processing_stages.size() - 1); i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    dk::ImageView rt{((i == final_stage) ? final_target : &pps.output_texture)->GetImage()};
    cmdbuf.bindRenderTargets({&rt});
    cmdbuf.discardColor(0);
  }

  dk::ImageView final_target_view{final_target->GetImage()};
  cmdbuf.bindRenderTargets({&final_target_view});
}

void Deko3DHostDisplay::RenderDisplay(const Deko3D::Texture* final_target)
{
  if (!HasDisplayTexture())
  {
    return;
  }

  dk::CmdBuf cmdbuf = g_deko3d_context->GetCmdBuf();

  cmdbuf.bindVtxAttribState({});
  cmdbuf.bindColorState(dk::ColorState{});
  cmdbuf.bindColorWriteState(dk::ColorWriteState{}.setMask(0, DkColorMask_RGBA));
  cmdbuf.bindDepthStencilState(dk::DepthStencilState{}.setDepthWriteEnable(false).setDepthTestEnable(false));
  cmdbuf.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));

  Deko3D::MemoryHeap& heap = g_deko3d_context->GetGeneralHeap();
  cmdbuf.bindSamplerDescriptorSet(heap.GpuAddr(m_sampler_buffer), SAMPLERS_COUNT);
  cmdbuf.bindImageDescriptorSet(heap.GpuAddr(m_descriptor_buffer), 1 + m_post_processing_chain.GetStageCount());

  cmdbuf.bindUniformBuffer(DkStage_Vertex, 1, heap.GpuAddr(m_uniform_buffer), m_uniform_buffer.size);
  cmdbuf.bindUniformBuffer(DkStage_Fragment, 1, heap.GpuAddr(m_uniform_buffer), m_uniform_buffer.size);

  const auto [left, top, width, height] = CalculateDrawRect(final_target->GetWidth(), final_target->GetHeight());

  if (!m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(final_target, left, final_target->GetHeight() - top - height, width, height,
                             static_cast<Deko3D::Texture*>(m_display_texture), m_display_texture_view_x,
                             m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                             final_target->GetWidth(), final_target->GetHeight());
    return;
  }

  RenderDisplay(left, top, width, height, static_cast<Deko3D::Texture*>(m_display_texture), m_display_texture_view_x,
                m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                IsUsingLinearFiltering());
}

void Deko3DHostDisplay::RenderDisplay(s32 left, s32 top, s32 width, s32 height, const Deko3D::Texture* texture,
                                      s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                      s32 texture_view_height, bool linear_filter)
{
  dk::CmdBuf cmdbuf = g_deko3d_context->GetCmdBuf();

  const float position_adjust = linear_filter ? 0.5f : 0.0f;
  const float size_adjust = linear_filter ? 1.0f : 0.0f;
  const UniformBuffer pc{
    (static_cast<float>(texture_view_x) + position_adjust) / static_cast<float>(texture->GetWidth()),
    (static_cast<float>(texture_view_y) + position_adjust) / static_cast<float>(texture->GetHeight()),
    (static_cast<float>(texture_view_width) - size_adjust) / static_cast<float>(texture->GetWidth()),
    (static_cast<float>(texture_view_height) - size_adjust) / static_cast<float>(texture->GetHeight())};

  cmdbuf.barrier(DkBarrier_Primitives, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Image);

  dk::ImageDescriptor descriptor;
  dk::ImageView view{texture->GetImage()};

  descriptor.initialize(view);
  cmdbuf.pushData(g_deko3d_context->GetGeneralHeap().GpuAddr(m_descriptor_buffer), &descriptor, sizeof(descriptor));
  cmdbuf.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(0, (u32)linear_filter)});

  cmdbuf.bindShaders(DkStageFlag_Vertex | DkStageFlag_Fragment, {&m_vertex_shader, &m_display_fragment_shader});

  cmdbuf.pushConstants(g_deko3d_context->GetGeneralHeap().GpuAddr(m_uniform_buffer), m_uniform_buffer.size, 0,
                       sizeof(pc), &pc);
  Deko3D::Util::SetViewportAndScissor(cmdbuf, left, top, width, height);
  cmdbuf.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
}

bool Deko3DHostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                         GPUTexture::Format* out_format)
{
  Deko3D::Texture texture;
  if (!texture.Create(width, height, 1, 1, DkImageFormat_RGBA8_Unorm, DkMsMode_1x, DkImageType_2D,
                      DkImageFlags_UsageRender))
  {
    return false;
  }

  dk::CmdBuf cmdbuf = g_deko3d_context->GetCmdBuf();
  dk::ImageView textureView{texture.GetImage()};
  cmdbuf.bindRenderTargets({&textureView});
  cmdbuf.setScissors(0, {{0, 0, width, height}});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 0, 0, 0, 0);

  RenderDisplay(&texture);

  out_pixels->resize(width * height);
  *out_format = GPUTexture::Format::RGBA8;
  *out_stride = width * 4;

  DownloadTexture(&texture, 0, 0, width, height, out_pixels->data(), *out_stride);

  return true;
}

bool Deko3DHostDisplay::CreateResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
#version 450 core

layout(std140, binding = 1) uniform PushConstants {
  uniform vec4 u_src_rect;
};

layout(location = 0) out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

  static constexpr char display_fragment_shader_src[] = R"(
#version 450 core

layout(binding = 0) uniform sampler2D samp0;

layout(location = 0) in vec2 v_tex0;
layout(location = 0) out vec4 o_col0;

void main()
{
  o_col0 = vec4(texture(samp0, v_tex0).rgb, 1.0);
}
)";

  if (!g_deko3d_shader_cache->GetVertexShader(fullscreen_quad_vertex_shader, m_vertex_shader, m_vertex_shader_memory))
    return false;
  if (!g_deko3d_shader_cache->GetFragmentShader(display_fragment_shader_src, m_display_fragment_shader,
                                                m_display_fragment_shader_memory))
    return false;

  m_uniform_buffer = g_deko3d_context->GetGeneralHeap().Alloc(1024, DK_UNIFORM_BUF_ALIGNMENT);
  m_descriptor_buffer =
    g_deko3d_context->GetGeneralHeap().Alloc(sizeof(dk::ImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
  m_sampler_buffer = g_deko3d_context->GetGeneralHeap().Alloc(sizeof(dk::SamplerDescriptor) * SAMPLERS_COUNT,
                                                              DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
  dk::SamplerDescriptor* samplers = g_deko3d_context->GetGeneralHeap().CpuAddr<dk::SamplerDescriptor>(m_sampler_buffer);
  samplers[SAMPLER_NEAREST].initialize(dk::Sampler{}.setWrapMode(DkWrapMode_ClampToBorder, DkWrapMode_ClampToBorder));
  samplers[SAMPLER_LINEAR].initialize(dk::Sampler{}
                                        .setWrapMode(DkWrapMode_ClampToBorder, DkWrapMode_ClampToBorder)
                                        .setFilter(DkFilter_Linear, DkFilter_Linear));

  return true;
}

void Deko3DHostDisplay::DestroyResources()
{
  if (m_vertex_shader_memory.size > 0)
  {
    g_deko3d_context->GetShaderHeap().Free(m_vertex_shader_memory);
    m_vertex_shader_memory = {};
  }
  if (m_display_fragment_shader_memory.size > 0)
  {
    g_deko3d_context->GetShaderHeap().Free(m_display_fragment_shader_memory);
    m_display_fragment_shader_memory = {};
  }

  g_deko3d_context->GetGeneralHeap().Free(m_uniform_buffer);

  m_display_pixels_texture.Destroy(false);
  m_post_processing_input_texture.Destroy(false);

  DestroyPostProcessingStages(false);
}

void Deko3DHostDisplay::DestroyPostProcessingStages(bool defer)
{
  for (size_t i = 0; i < m_post_processing_stages.size(); i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];

    Deko3D::MemoryHeap& shader_heap = g_deko3d_context->GetShaderHeap();
    if (defer)
    {
      g_deko3d_context->DeferedFree(&shader_heap, pps.vertex_shader_memory);
      g_deko3d_context->DeferedFree(&shader_heap, pps.fragment_shader_memory);
    }
    else
    {
      shader_heap.Free(pps.vertex_shader_memory);
      shader_heap.Free(pps.fragment_shader_memory);
    }

    pps.output_texture.Destroy(defer);
  }

  m_post_processing_stages.clear();
}

bool Deko3DHostDisplay::CreateImGuiContext()
{
  ImGui_ImplDeko3D_InitInfo vii = {};
  vii.Device = g_deko3d_context->GetDevice();
  vii.Queue = g_deko3d_context->GetQueue();
  vii.MinImageCount = Deko3D::SwapChain::NumSwapchainEntries;
  vii.ImageCount = Deko3D::SwapChain::NumSwapchainEntries;

  return ImGui_ImplDeko3D_Init(&vii);
}

void Deko3DHostDisplay::DestroyImGuiContext()
{
  g_deko3d_context->WaitGPUIdle();
  ImGui_ImplDeko3D_Shutdown();
}

bool Deko3DHostDisplay::UpdateImGuiFontTexture()
{
  g_deko3d_context->ExecuteCommandBuffer(true);
  ImGui_ImplDeko3D_DestroyFontUploadObjects();
  return ImGui_ImplDeko3D_CreateFontsTexture(g_deko3d_context->GetCmdBuf());
}
