// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu.h"
#include "gpu_types.h"

#include "common/rectangle.h"
#include "common/types.h"

#include <algorithm>
#include <array>

namespace GPU_SW_Rasterizer {

// this is actually (31 * 255) >> 4) == 494, but to simplify addressing we use the next power of two (512)
static constexpr u32 DITHER_LUT_SIZE = 512;
using DitherLUT = std::array<std::array<std::array<u8, DITHER_LUT_SIZE>, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
extern const DitherLUT g_dither_lut;

extern Common::Rectangle<u32> g_drawing_area;

using DrawRectangleFunction = void (*)(const GPUBackendDrawSpriteCommand* cmd);
typedef const DrawRectangleFunction DrawRectangleFunctionTable[2][2][2];

using DrawTriangleFunction = void (*)(const GPUBackendDrawCommand* cmd,
                                      const GPUBackendDrawPolygonCommand::Vertex* v0,
                                      const GPUBackendDrawPolygonCommand::Vertex* v1,
                                      const GPUBackendDrawPolygonCommand::Vertex* v2);
typedef const DrawTriangleFunction DrawTriangleFunctionTable[2][2][2][2][2];

using DrawLineFunction = void (*)(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0,
                                  const GPUBackendDrawLineCommand::Vertex* p1);
typedef const DrawLineFunction DrawLineFunctionTable[2][2][2];

// Current implementation, selected at runtime.
extern const DrawRectangleFunctionTable* SelectedDrawRectangleFunctions;
extern const DrawTriangleFunctionTable* SelectedDrawTriangleFunctions;
extern const DrawLineFunctionTable* SelectedDrawLineFunctions;

extern void SelectImplementation();

ALWAYS_INLINE static DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable,
                                                          bool dithering_enable)
{
  return (*SelectedDrawLineFunctions)[u8(shading_enable)][u8(transparency_enable)][u8(dithering_enable)];
}

ALWAYS_INLINE static DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                                    bool transparency_enable)
{
  return (*SelectedDrawRectangleFunctions)[u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)];
}

ALWAYS_INLINE static DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable,
                                                                  bool raw_texture_enable, bool transparency_enable,
                                                                  bool dithering_enable)
{
  return (*SelectedDrawTriangleFunctions)[u8(shading_enable)][u8(texture_enable)][u8(raw_texture_enable)]
                                         [u8(transparency_enable)][u8(dithering_enable)];
}

// Have to define the symbols globally, because clang won't include them otherwise.
#if 0 // #if defined(CPU_ARCH_SSE)
#define ALTERNATIVE_RASTERIZER_LIST()                                                                                  \
  DECLARE_ALTERNATIVE_RASTERIZER(SSE4)                                                                                 \
  DECLARE_ALTERNATIVE_RASTERIZER(AVX2)
#else
#define ALTERNATIVE_RASTERIZER_LIST()
#endif

#define DECLARE_ALTERNATIVE_RASTERIZER(isa)                                                                            \
  namespace isa {                                                                                                      \
  extern const DrawRectangleFunctionTable DrawRectangleFunctions;                                                      \
  extern const DrawTriangleFunctionTable DrawTriangleFunctions;                                                        \
  extern const DrawLineFunctionTable DrawLineFunctions;                                                                \
  }
ALTERNATIVE_RASTERIZER_LIST()
#undef DECLARE_ALTERNATIVE_RASTERIZER

} // namespace GPU_SW_Rasterizer