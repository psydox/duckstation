// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "opengl_device.h"
#include "opengl_pipeline.h"
#include "opengl_stream_buffer.h"
#include "opengl_texture.h"
#include "postprocessing_shadergen.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <array>
#include <tuple>

Log_SetChannel(OpenGLDevice);

OpenGLDevice::OpenGLDevice()
{
  // Something which won't be matched..
  std::memset(&m_last_rasterization_state, 0xFF, sizeof(m_last_rasterization_state));
  std::memset(&m_last_depth_state, 0xFF, sizeof(m_last_depth_state));
  std::memset(&m_last_blend_state, 0xFF, sizeof(m_last_blend_state));
}

OpenGLDevice::~OpenGLDevice()
{
  Assert(!m_gl_context);
}

void OpenGLDevice::BindUpdateTextureUnit()
{
  GetInstance().SetActiveTexture(UPDATE_TEXTURE_UNIT - GL_TEXTURE0);
}

RenderAPI OpenGLDevice::GetRenderAPI() const
{
  return m_gl_context->IsGLES() ? RenderAPI::OpenGLES : RenderAPI::OpenGL;
}

std::unique_ptr<GPUTexture> OpenGLDevice::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                        GPUTexture::Type type, GPUTexture::Format format,
                                                        const void* data, u32 data_stride, bool dynamic /* = false */)
{
  std::unique_ptr<OpenGLTexture> tex(std::make_unique<OpenGLTexture>());
  if (!tex->Create(width, height, layers, levels, samples, format, data, data_stride))
    tex.reset();

  return tex;
}

bool OpenGLDevice::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                   u32 out_data_stride)
{
  OpenGLTexture* T = static_cast<OpenGLTexture*>(texture);

  GLint alignment;
  if (out_data_stride & 1)
    alignment = 1;
  else if (out_data_stride & 2)
    alignment = 2;
  else
    alignment = 4;

  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH, out_data_stride / T->GetPixelSize());

  const auto [gl_internal_format, gl_format, gl_type] = OpenGLTexture::GetPixelFormatMapping(T->GetFormat());
  const u32 layer = 0;
  const u32 level = 0;

  if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image)
  {
    glGetTextureSubImage(T->GetGLId(), level, x, y, layer, width, height, 1, gl_format, gl_type,
                         height * out_data_stride, out_data);
  }
  else
  {
    if (T->GetLayers() > 0)
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, T->GetGLId(), level, layer);
    else
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, T->GetGLId(), level);

    DebugAssert(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glReadPixels(x, y, width, height, gl_format, gl_type, out_data);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  }

  return true;
}

bool OpenGLDevice::SupportsTextureFormat(GPUTexture::Format format) const
{
  const auto [gl_internal_format, gl_format, gl_type] = OpenGLTexture::GetPixelFormatMapping(format);
  return (gl_internal_format != static_cast<GLenum>(0));
}

void OpenGLDevice::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                     GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                     u32 height)
{
  OpenGLTexture* D = static_cast<OpenGLTexture*>(dst);
  OpenGLTexture* S = static_cast<OpenGLTexture*>(src);
  CommitClear(D);
  CommitClear(S);

  const GLuint sid = S->GetGLId();
  const GLuint did = D->GetGLId();
  if (GLAD_GL_VERSION_4_3 || GLAD_GL_ARB_copy_image)
  {
    glCopyImageSubData(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer, did, GL_TEXTURE_2D, dst_level, dst_x,
                       dst_y, dst_layer, width, height, 1);
  }
  else if (GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer, did, GL_TEXTURE_2D, dst_level, dst_x,
                          dst_y, dst_layer, width, height, 1);
  }
  else if (GLAD_GL_OES_copy_image)
  {
    glCopyImageSubDataOES(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer, did, GL_TEXTURE_2D, dst_level, dst_x,
                          dst_y, dst_layer, width, height, 1);
  }
  else
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_write_fbo);
    if (D->IsTextureArray())
      glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, did, dst_level, dst_layer);
    else
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, did, dst_level);
    if (S->IsTextureArray())
      glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sid, src_level, src_layer);
    else
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sid, src_level);

    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(src_x, src_y, src_x + width, src_y + width, dst_x, dst_y, dst_x + width, dst_y + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_framebuffer ? m_current_framebuffer->GetGLId() : 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }
}

void OpenGLDevice::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                        GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                        u32 height)
{
  OpenGLTexture* D = static_cast<OpenGLTexture*>(dst);
  OpenGLTexture* S = static_cast<OpenGLTexture*>(src);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_write_fbo);
  if (D->IsTextureArray())
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, D->GetGLId(), dst_level, dst_layer);
  else
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, D->GetGLId(), dst_level);
  if (S->IsTextureArray())
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, S->GetGLId(), src_level, src_layer);
  else
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, S->GetGLId(), src_level);

  CommitClear(S);
  if (width == D->GetMipWidth(dst_level) && height == D->GetMipHeight(dst_level))
  {
    D->SetState(GPUTexture::State::Dirty);
    if (glInvalidateFramebuffer)
    {
      const GLenum attachment = GL_COLOR_ATTACHMENT0;
      glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
    }
  }
  else
  {
    CommitClear(D);
  }

  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(src_x, src_y, src_x + width, src_y + width, dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glEnable(GL_SCISSOR_TEST);

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_framebuffer ? m_current_framebuffer->GetGLId() : 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void OpenGLDevice::PushDebugGroup(const char* fmt, ...)
{
#ifdef _DEBUG
  if (!glPushDebugGroup)
    return;

  std::va_list ap;
  va_start(ap, fmt);
  const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
  va_end(ap);
  if (!buf.empty())
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(buf.size()), buf.c_str());
#endif
}

void OpenGLDevice::PopDebugGroup()
{
#ifdef _DEBUG
  if (!glPopDebugGroup)
    return;

  glPopDebugGroup();
#endif
}

void OpenGLDevice::InsertDebugMessage(const char* fmt, ...)
{
#ifdef _DEBUG
  if (!glDebugMessageInsert)
    return;

  std::va_list ap;
  va_start(ap, fmt);
  const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
  va_end(ap);
  if (!buf.empty())
  {
    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 0, GL_DEBUG_SEVERITY_NOTIFICATION,
                         static_cast<GLsizei>(buf.size()), buf.c_str());
  }
#endif
}

void OpenGLDevice::SetVSync(bool enabled)
{
  if (m_vsync_enabled == enabled)
    return;

  m_vsync_enabled = enabled;
  SetSwapInterval();
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_ErrorPrint(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrint(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool OpenGLDevice::HasSurface() const
{
  return m_window_info.type != WindowInfo::Type::Surfaceless;
}

bool OpenGLDevice::CreateDevice(const std::string_view& adapter, bool debug_device)
{
  m_gl_context = GL::Context::Create(m_window_info);
  if (!m_gl_context)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    m_gl_context.reset();
    return false;
  }

  // Is this needed?
  m_window_info = m_gl_context->GetWindowInfo();

#if 0
  // TODO: add these checks
  const bool opengl_is_available = ((g_host_display->GetRenderAPI() == RenderAPI::OpenGL &&
    (GLAD_GL_VERSION_3_0 || GLAD_GL_ARB_uniform_buffer_object)) ||
    (g_host_display->GetRenderAPI() == RenderAPI::OpenGLES && GLAD_GL_ES_VERSION_3_1));
  if (!opengl_is_available)
  {
    Host::AddOSDMessage(Host::TranslateStdString("OSDMessage",
      "OpenGL renderer unavailable, your driver or hardware is not "
      "recent enough. OpenGL 3.1 or OpenGL ES 3.1 is required."),
      20.0f);
    return nullptr;
  }
#endif

  OpenGLTexture::s_use_pbo_for_uploads = true;
  if (m_gl_context->IsGLES())
  {
    // Adreno seems to corrupt textures through PBOs... and Mali is slow.
    const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    if (std::strstr(gl_vendor, "Qualcomm") || std::strstr(gl_vendor, "ARM") || std::strstr(gl_vendor, "Broadcom"))
      OpenGLTexture::s_use_pbo_for_uploads = false;
  }

  Log_VerbosePrintf("Using PBO for uploads: %s", OpenGLTexture::s_use_pbo_for_uploads ? "yes" : "no");

  if (debug_device && GLAD_GL_KHR_debug)
  {
    if (m_gl_context->IsGLES())
      glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    else
      glDebugMessageCallback(GLDebugCallback, nullptr);

    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }
  else
  {
    // Nail the function pointers so that we don't waste time calling them.
    glPushDebugGroup = nullptr;
    glPopDebugGroup = nullptr;
    glDebugMessageInsert = nullptr;
    glObjectLabel = nullptr;
  }

  if (!CheckFeatures())
    return false;

  if (!CreateBuffers())
    return false;

  return true;
}

bool OpenGLDevice::CheckFeatures()
{
  const bool is_gles = m_gl_context->IsGLES();

  GLint max_texture_size = 1024;
  GLint max_samples = 1;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  m_max_texture_size = std::max(1024u, static_cast<u32>(max_texture_size));
  m_max_multisamples = std::max(1u, static_cast<u32>(max_samples));

  GLint max_dual_source_draw_buffers = 0;
  glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &max_dual_source_draw_buffers);
  m_features.dual_source_blend =
    (max_dual_source_draw_buffers > 0) &&
    (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended || GLAD_GL_EXT_blend_func_extended);

#ifdef __APPLE__
  // Partial texture buffer uploads appear to be broken in macOS's OpenGL driver.
  m_features.supports_texture_buffers = false;
#else
  m_features.supports_texture_buffers = (GLAD_GL_VERSION_3_1 || GLAD_GL_ES_VERSION_3_2);

  // And Samsung's ANGLE/GLES driver?
  if (std::strstr(reinterpret_cast<const char*>(glGetString(GL_RENDERER)), "ANGLE"))
    m_features.supports_texture_buffers = false;
#endif

  if (!m_features.supports_texture_buffers)
  {
    // Try SSBOs.
    GLint max_fragment_storage_blocks = 0;
    GLint64 max_ssbo_size = 0;
    if (GLAD_GL_VERSION_4_3 || GLAD_GL_ES_VERSION_3_1 || GLAD_GL_ARB_shader_storage_buffer_object)
    {
      glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &max_fragment_storage_blocks);
      glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
    }

    Log_InfoPrintf("Max fragment shader storage blocks: %d", max_fragment_storage_blocks);
    Log_InfoPrintf("Max shader storage buffer size: %" PRId64, max_ssbo_size);
    m_features.texture_buffers_emulated_with_ssbo =
      (max_fragment_storage_blocks > 0 && max_ssbo_size >= static_cast<GLint64>(1024 * 512 * sizeof(u16)));
    if (m_features.texture_buffers_emulated_with_ssbo)
    {
      // TODO: SSBOs should be clamped to max size.
      Log_InfoPrintf("Using shader storage buffers for VRAM writes.");
    }
    else
    {
      Log_WarningPrintf("Both texture buffers and SSBOs are not supported.");
      return false;
    }
  }

  m_features.per_sample_shading = GLAD_GL_VERSION_4_0 || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_ARB_sample_shading;

  // adaptive smoothing would require texture views, which aren't in GLES.
  m_features.mipmapped_render_targets = false;

  // noperspective is not supported in GLSL ES.
  m_features.noperspective_interpolation = !is_gles;
  return true;
}

void OpenGLDevice::DestroyDevice()
{
  if (!m_gl_context)
    return;

  DestroyBuffers();

  m_gl_context->DoneCurrent();
  m_gl_context.reset();
}

bool OpenGLDevice::UpdateWindow()
{
  Assert(m_gl_context);

  DestroySurface();

  if (!AcquireWindow(false))
    return false;

  if (!m_gl_context->ChangeSurface(m_window_info))
  {
    Log_ErrorPrintf("Failed to change surface");
    return false;
  }

  m_window_info = m_gl_context->GetWindowInfo();

  if (m_window_info.type != WindowInfo::Type::Surfaceless)
  {
    // reset vsync rate, since it (usually) gets lost
    SetSwapInterval();
    // TODO RenderBlankFrame();
  }

  return true;
}

void OpenGLDevice::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  m_window_info.surface_scale = new_window_scale;
  if (m_window_info.surface_width == static_cast<u32>(new_window_width) &&
      m_window_info.surface_height == static_cast<u32>(new_window_height))
  {
    return;
  }

  m_gl_context->ResizeSurface(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_info = m_gl_context->GetWindowInfo();
}

void OpenGLDevice::SetSwapInterval()
{
  if (m_window_info.type == WindowInfo::Type::Surfaceless)
    return;

  // Window framebuffer has to be bound to call SetSwapInterval.
  const s32 interval = m_vsync_enabled ? 1 : 0;
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

  if (!m_gl_context->SetSwapInterval(interval))
    Log_WarningPrintf("Failed to set swap interval to %d", interval);

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

GPUDevice::AdapterAndModeList OpenGLDevice::GetAdapterAndModeList()
{
  AdapterAndModeList aml;

  if (m_gl_context)
  {
    for (const GL::Context::FullscreenModeInfo& fmi : m_gl_context->EnumerateFullscreenModes())
    {
      aml.fullscreen_modes.push_back(GetFullscreenModeString(fmi.width, fmi.height, fmi.refresh_rate));
    }
  }

  return aml;
}

void OpenGLDevice::DestroySurface()
{
  if (!m_gl_context)
    return;

  m_window_info.SetSurfaceless();
  if (!m_gl_context->ChangeSurface(m_window_info))
    Log_ErrorPrintf("Failed to switch to surfaceless");
}

std::string OpenGLDevice::GetShaderCacheBaseName(const std::string_view& type, bool debug) const
{
  return fmt::format("opengl_{}{}", type, debug ? "_debug" : "");
}

bool OpenGLDevice::CreateBuffers()
{
  if (!(m_vertex_buffer = OpenGLStreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE)) ||
      !(m_index_buffer = OpenGLStreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE)) ||
      !(m_uniform_buffer = OpenGLStreamBuffer::Create(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE)))
  {
    Log_ErrorPrintf("Failed to create one or more device buffers.");
    return false;
  }

  GL_OBJECT_NAME(m_vertex_buffer, "Device Vertex Buffer");
  GL_OBJECT_NAME(m_index_buffer, "Device Index Buffer");
  GL_OBJECT_NAME(m_uniform_buffer, "Device Uniform Buffer");

  // TODO NOTE If we don't have GLES3.1, then SV_VertexID isn't defined when no VBOs are active.
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));

  // TODO: buggy pbo
  if (true)
  {
    if (!(m_texture_stream_buffer = OpenGLStreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, TEXTURE_STREAM_BUFFER_SIZE)))
    {
      Log_ErrorPrintf("Failed to create texture stream buffer");
      return false;
    }

    // Need to unbind otherwise normal uploads will fail.
    m_texture_stream_buffer->Unbind();

    GL_OBJECT_NAME(m_texture_stream_buffer, "Device Texture Stream Buffer");
  }

  GLuint fbos[2];
  glGetError();
  glGenFramebuffers(static_cast<GLsizei>(std::size(fbos)), fbos);
  if (const GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    Log_ErrorPrintf("Failed to create framebuffers: %u", err);
    return false;
  }
  m_read_fbo = fbos[0];
  m_write_fbo = fbos[1];

  // Read FBO gets left bound.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);

  return true;
}

void OpenGLDevice::DestroyBuffers()
{
  if (m_write_fbo != 0)
    glDeleteFramebuffers(1, &m_write_fbo);
  if (m_read_fbo != 0)
    glDeleteFramebuffers(1, &m_read_fbo);
  m_texture_stream_buffer.reset();
  m_uniform_buffer.reset();
  m_index_buffer.reset();
  m_vertex_buffer.reset();
}

bool OpenGLDevice::BeginPresent(bool skip_present)
{
  if (skip_present || m_window_info.type == WindowInfo::Type::Surfaceless)
  {
    if (!skip_present)
      glFlush();
    return false;
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);

  m_current_framebuffer = nullptr;
  return true;
}

void OpenGLDevice::EndPresent()
{
  DebugAssert(!m_current_framebuffer);

  if (m_gpu_timing_enabled)
    PopTimestampQuery();

  m_gl_context->SwapBuffers();

  if (m_gpu_timing_enabled)
    KickTimestampQuery();
}

#if 0

void OpenGLDevice::RenderDisplay(s32 left, s32 bottom, s32 width, s32 height, OpenGLTexture* texture,
                                 s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                 s32 texture_view_height, bool linear_filter)
{
  glViewport(left, bottom, width, height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  texture->Bind();

  const bool linear = IsUsingLinearFiltering();

  if (!m_use_gles2_draw_path)
  {
    const float position_adjust = linear ? 0.5f : 0.0f;
    const float size_adjust = linear ? 1.0f : 0.0f;
    const float flip_adjust = (texture_view_height < 0) ? -1.0f : 1.0f;
    m_display_program.Uniform4f(
      0, (static_cast<float>(texture_view_x) + position_adjust) / static_cast<float>(texture->GetWidth()),
      (static_cast<float>(texture_view_y) + (position_adjust * flip_adjust)) / static_cast<float>(texture->GetHeight()),
      (static_cast<float>(texture_view_width) - size_adjust) / static_cast<float>(texture->GetWidth()),
      (static_cast<float>(texture_view_height) - (size_adjust * flip_adjust)) /
        static_cast<float>(texture->GetHeight()));
    glBindSampler(0, linear_filter ? m_display_linear_sampler : m_display_nearest_sampler);
    glBindVertexArray(m_display_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
  }
  else
  {
    texture->SetLinearFilter(linear_filter);

    DrawFullscreenQuadES2(m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                          m_display_texture_view_height, texture->GetWidth(), texture->GetHeight());
  }
}
#endif

void OpenGLDevice::CreateTimestampQueries()
{
  const bool gles = m_gl_context->IsGLES();
  const auto GenQueries = gles ? glGenQueriesEXT : glGenQueries;

  GenQueries(static_cast<u32>(m_timestamp_queries.size()), m_timestamp_queries.data());
  KickTimestampQuery();
}

void OpenGLDevice::DestroyTimestampQueries()
{
  if (m_timestamp_queries[0] == 0)
    return;

  const bool gles = m_gl_context->IsGLES();
  const auto DeleteQueries = gles ? glDeleteQueriesEXT : glDeleteQueries;

  if (m_timestamp_query_started)
  {
    const auto EndQuery = gles ? glEndQueryEXT : glEndQuery;
    EndQuery(GL_TIME_ELAPSED);
  }

  DeleteQueries(static_cast<u32>(m_timestamp_queries.size()), m_timestamp_queries.data());
  m_timestamp_queries.fill(0);
  m_read_timestamp_query = 0;
  m_write_timestamp_query = 0;
  m_waiting_timestamp_queries = 0;
  m_timestamp_query_started = false;
}

void OpenGLDevice::PopTimestampQuery()
{
  const bool gles = m_gl_context->IsGLES();

  if (gles)
  {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint)
    {
      Log_VerbosePrintf("GPU timing disjoint, resetting.");
      if (m_timestamp_query_started)
        glEndQueryEXT(GL_TIME_ELAPSED);

      m_read_timestamp_query = 0;
      m_write_timestamp_query = 0;
      m_waiting_timestamp_queries = 0;
      m_timestamp_query_started = false;
    }
  }

  while (m_waiting_timestamp_queries > 0)
  {
    const auto GetQueryObjectiv = gles ? glGetQueryObjectivEXT : glGetQueryObjectiv;
    const auto GetQueryObjectui64v = gles ? glGetQueryObjectui64vEXT : glGetQueryObjectui64v;

    GLint available = 0;
    GetQueryObjectiv(m_timestamp_queries[m_read_timestamp_query], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available)
      break;

    u64 result = 0;
    GetQueryObjectui64v(m_timestamp_queries[m_read_timestamp_query], GL_QUERY_RESULT, &result);
    m_accumulated_gpu_time += static_cast<float>(static_cast<double>(result) / 1000000.0);
    m_read_timestamp_query = (m_read_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_waiting_timestamp_queries--;
  }

  if (m_timestamp_query_started)
  {
    const auto EndQuery = gles ? glEndQueryEXT : glEndQuery;
    EndQuery(GL_TIME_ELAPSED);

    m_write_timestamp_query = (m_write_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_timestamp_query_started = false;
    m_waiting_timestamp_queries++;
  }
}

void OpenGLDevice::KickTimestampQuery()
{
  if (m_timestamp_query_started || m_waiting_timestamp_queries == NUM_TIMESTAMP_QUERIES)
    return;

  const bool gles = m_gl_context->IsGLES();
  const auto BeginQuery = gles ? glBeginQueryEXT : glBeginQuery;

  BeginQuery(GL_TIME_ELAPSED, m_timestamp_queries[m_write_timestamp_query]);
  m_timestamp_query_started = true;
}

bool OpenGLDevice::SetGPUTimingEnabled(bool enabled)
{
  if (m_gpu_timing_enabled == enabled)
    return true;

  if (enabled && m_gl_context->IsGLES() &&
      (!GLAD_GL_EXT_disjoint_timer_query || !glGetQueryObjectivEXT || !glGetQueryObjectui64vEXT))
  {
    return false;
  }

  m_gpu_timing_enabled = enabled;
  if (m_gpu_timing_enabled)
    CreateTimestampQueries();
  else
    DestroyTimestampQueries();

  return true;
}

float OpenGLDevice::GetAndResetAccumulatedGPUTime()
{
  const float value = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return value;
}

void OpenGLDevice::SetActiveTexture(u32 slot)
{
  if (m_last_texture_unit != slot)
  {
    m_last_texture_unit = slot;
    glActiveTexture(GL_TEXTURE0 + slot);
  }
}

void OpenGLDevice::UnbindTexture(GLuint id)
{
  for (u32 slot = 0; slot < MAX_TEXTURE_SAMPLERS; slot++)
  {
    auto& ss = m_last_samplers[slot];
    if (ss.first == id)
    {
      ss.first = 0;

      const GLenum unit = GL_TEXTURE0 + slot;
      if (m_last_texture_unit != unit)
      {
        m_last_texture_unit = unit;
        glActiveTexture(unit);
      }

      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
}

void OpenGLDevice::UnbindSampler(GLuint id)
{
  for (u32 slot = 0; slot < MAX_TEXTURE_SAMPLERS; slot++)
  {
    auto& ss = m_last_samplers[slot];
    if (ss.second == id)
    {
      ss.second = 0;
      glBindSampler(slot, 0);
    }
  }
}

void OpenGLDevice::UnbindFramebuffer(const OpenGLFramebuffer* fb)
{
  if (m_current_framebuffer == fb)
  {
    m_current_framebuffer = nullptr;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  }
}

void OpenGLDevice::UnbindPipeline(const OpenGLPipeline* pl)
{
  if (m_current_pipeline == pl)
  {
    m_current_pipeline = nullptr;
    glUseProgram(0);
  }
}

void OpenGLDevice::PreDrawCheck()
{
  DebugAssert(m_current_pipeline);
  if (m_current_framebuffer)
    CommitClear(m_current_framebuffer);
}

void OpenGLDevice::Draw(u32 vertex_count, u32 base_vertex)
{
  PreDrawCheck();
  glDrawArrays(m_current_pipeline->GetTopology(), base_vertex, vertex_count);
}

void OpenGLDevice::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  PreDrawCheck();

  const void* indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(base_index) * sizeof(u16));
  glDrawElementsBaseVertex(m_current_pipeline->GetTopology(), index_count, GL_UNSIGNED_SHORT, indices, base_vertex);
}

void OpenGLDevice::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                                   u32* map_base_vertex)
{
  const auto res = m_vertex_buffer->Map(vertex_size, vertex_size * vertex_count);
  *map_ptr = res.pointer;
  *map_space = res.space_aligned;
  *map_base_vertex = res.index_aligned;
}

void OpenGLDevice::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  m_vertex_buffer->Unmap(vertex_size * vertex_count);
}

void OpenGLDevice::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
{
  const auto res = m_index_buffer->Map(sizeof(DrawIndex), sizeof(DrawIndex) * index_count);
  *map_ptr = static_cast<DrawIndex*>(res.pointer);
  *map_space = res.space_aligned;
  *map_base_index = res.index_aligned;
}

void OpenGLDevice::UnmapIndexBuffer(u32 used_index_count)
{
  m_index_buffer->Unmap(sizeof(DrawIndex) * used_index_count);
}

void OpenGLDevice::PushUniformBuffer(const void* data, u32 data_size)
{
  const auto res = m_uniform_buffer->Map(m_uniform_buffer_alignment, data_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_buffer->Unmap(data_size);
  glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_uniform_buffer->GetGLBufferId(), res.buffer_offset, data_size);
}

void* OpenGLDevice::MapUniformBuffer(u32 size)
{
  const auto res = m_uniform_buffer->Map(m_uniform_buffer_alignment, size);
  return res.pointer;
}

void OpenGLDevice::UnmapUniformBuffer(u32 size)
{
  const u32 pos = m_uniform_buffer->Unmap(size);
  glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_uniform_buffer->GetGLBufferId(), pos, size);
}

void OpenGLDevice::SetFramebuffer(GPUFramebuffer* fb)
{
  if (m_current_framebuffer == fb)
    return;

  // TODO: maybe move clear check here? gets rid of the per-draw overhead
  m_current_framebuffer = static_cast<OpenGLFramebuffer*>(fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_framebuffer ? m_current_framebuffer->GetGLId() : 0);
}

void OpenGLDevice::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  DebugAssert(slot < MAX_TEXTURE_SAMPLERS);
  auto& sslot = m_last_samplers[slot];

  const OpenGLTexture* T = static_cast<const OpenGLTexture*>(texture);
  const GLuint Tid = T ? T->GetGLId() : 0;
  if (sslot.first != Tid)
  {
    sslot.first = Tid;

    SetActiveTexture(slot);
    glBindTexture(T ? T->GetGLTarget() : GL_TEXTURE_2D, T ? T->GetGLId() : 0);
  }

  const GLuint Sid = sampler ? static_cast<const OpenGLSampler*>(sampler)->GetID() : 0;
  if (sslot.second != Sid)
  {
    sslot.second = Sid;
    glBindSampler(slot, Sid);
  }
}

void OpenGLDevice::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  const OpenGLTextureBuffer* B = static_cast<const OpenGLTextureBuffer*>(buffer);
  if (!m_features.texture_buffers_emulated_with_ssbo)
  {
    const GLuint Tid = B ? B->GetTextureId() : 0;
    if (m_last_samplers[slot].first != Tid)
    {
      m_last_samplers[slot].first = Tid;
      SetActiveTexture(slot);
      glBindTexture(GL_TEXTURE_BUFFER, Tid);
    }
  }
  else
  {
    // TODO: cache
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, B ? B->GetBuffer()->GetGLBufferId() : 0);
  }
}

void OpenGLDevice::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  // TODO: cache this
  // TODO: lower-left origin flip for window fb?
  glViewport(x, y, width, height);
}

void OpenGLDevice::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  // TODO: cache this
  // TODO: lower-left origin flip for window fb?
  glScissor(x, y, width, height);
}