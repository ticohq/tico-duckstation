// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"

#include "deko3d_memory_heap.h"

#include <memory>

#include <deko3d.hpp>

class Deko3DDevice;

struct Deko3DInternalShader
{
  Deko3DInternalShader(const dk::Shader& shader, Deko3DMemoryHeap::Allocation memory);
  ~Deko3DInternalShader();

  dk::Shader shader;
  Deko3DMemoryHeap::Allocation memory;
};

class Deko3DShader final : public GPUShader
{
  friend Deko3DDevice;

public:
  ALWAYS_INLINE std::shared_ptr<Deko3DInternalShader> GetInternalShader() { return m_internal_shader; }

  void SetDebugName(std::string_view name)
  { /* not really implementable */
  }

private:
  Deko3DShader(GPUShaderStage stage, dk::Shader shader, Deko3DMemoryHeap::Allocation memory);

  std::shared_ptr<Deko3DInternalShader> m_internal_shader;
};

class Deko3DPipeline final : public GPUPipeline
{
  friend Deko3DDevice;

public:
  ~Deko3DPipeline() override;

  void SetDebugName(std::string_view name) override;

  ALWAYS_INLINE const RasterizationState& GetRasterizationState() const { return m_rasterization_state; }
  ALWAYS_INLINE const DepthState& GetDepthState() const { return m_depth_state; }
  ALWAYS_INLINE const BlendState& GetBlendState() const { return m_blend_state; }

  DkPrimitive GetTopology() const { return m_topology; }
  Layout GetLayout() const { return m_layout; }

private:
  Deko3DPipeline(Layout layout, const RasterizationState& rs, const DepthState& ds, const BlendState& bs,
                 DkPrimitive topology, u32 num_attribs,
                 const std::array<DkVtxAttribState, VertexAttribute::MaxAttributes>& attribs, u32 stride,
                 std::shared_ptr<Deko3DInternalShader> vtx_shader, std::shared_ptr<Deko3DInternalShader> frg_shader,
                 std::shared_ptr<Deko3DInternalShader> geom_shader);

  Layout m_layout;
  BlendState m_blend_state;
  RasterizationState m_rasterization_state;
  DepthState m_depth_state;
  DkPrimitive m_topology;

  std::shared_ptr<Deko3DInternalShader> m_vertex_shader, m_fragment_shader, m_geometry_shader;

  std::array<DkVtxAttribState, VertexAttribute::MaxAttributes> m_attributes;
  u8 m_num_attributes, m_stride;
};
