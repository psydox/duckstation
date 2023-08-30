// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "opengl_pipeline.h"
#include "opengl_device.h"
#include "opengl_stream_buffer.h"
#include "shadergen.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/hash_combine.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include "zstd.h"
#include "zstd_errors.h"

#include <cerrno>

Log_SetChannel(OpenGLPipeline);

struct PipelineDiskCacheFooter
{
  u32 version;
  u32 num_programs;
  char driver_vendor[128];
  char driver_renderer[128];
  char driver_version[128];
};
static_assert(sizeof(PipelineDiskCacheFooter) == (sizeof(u32) * 2 + 128 * 3));

struct PipelineDiskCacheIndexEntry
{
  OpenGLPipeline::ProgramCacheKey key;
  u32 format;
  u32 offset;
  u32 uncompressed_size;
  u32 compressed_size;
};
static_assert(sizeof(PipelineDiskCacheIndexEntry) == 128); // No padding

static unsigned s_next_bad_shader_id = 1;

static GLenum GetGLShaderType(GPUShaderStage stage)
{
  static constexpr std::array<GLenum, static_cast<u32>(GPUShaderStage::MaxCount)> mapping = {{
    GL_VERTEX_SHADER,   // Vertex
    GL_FRAGMENT_SHADER, // Fragment
    GL_COMPUTE_SHADER,  // Compute
  }};

  return mapping[static_cast<u32>(stage)];
}

static void FillFooter(PipelineDiskCacheFooter* footer, u32 version)
{
  footer->version = version;
  footer->num_programs = 0;
  StringUtil::Strlcpy(footer->driver_vendor, reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                      std::size(footer->driver_vendor));
  StringUtil::Strlcpy(footer->driver_renderer, reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                      std::size(footer->driver_renderer));
  StringUtil::Strlcpy(footer->driver_version, reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                      std::size(footer->driver_version));
}

OpenGLShader::OpenGLShader(GPUShaderStage stage, const GPUShaderCache::CacheIndexKey& key, std::string source)
  : GPUShader(stage), m_key(key), m_source(std::move(source))
{
}

OpenGLShader::~OpenGLShader()
{
  if (m_id.has_value())
    glDeleteShader(m_id.value());
}

void OpenGLShader::SetDebugName(const std::string_view& name)
{
#ifdef _DEBUG
  if (glObjectLabel)
  {
    if (m_id.has_value())
    {
      m_debug_name = {};
      glObjectLabel(GL_SHADER, m_id.value(), static_cast<GLsizei>(name.length()),
                    static_cast<const GLchar*>(name.data()));
    }
    else
    {
      m_debug_name = name;
    }
  }
#endif
}

bool OpenGLShader::Compile()
{
  if (m_compile_tried)
    return m_id.has_value();

  m_compile_tried = true;

  glGetError();

  GLuint shader = glCreateShader(GetGLShaderType(m_stage));
  if (GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    Log_ErrorPrintf("glCreateShader() failed: %u", err);
    return false;
  }

  const GLchar* string = m_source.data();
  const GLint length = static_cast<GLint>(m_source.length());
  glShaderSource(shader, 1, &string, &length);
  glCompileShader(shader);

  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  GLint info_log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetShaderInfoLog(shader, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Shader compiled with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Shader failed to compile:\n%s", info_log.c_str());

      auto fp = FileSystem::OpenManagedCFile(
        GPUDevice::GetShaderDumpPath(fmt::format("bad_shader_{}.txt", s_next_bad_shader_id++)).c_str(), "wb");
      if (fp)
      {
        std::fwrite(m_source.data(), m_source.size(), 1, fp.get());
        std::fprintf(fp.get(), "\n\nCompile %s shader failed\n", GPUShader::GetStageName(m_stage));
        std::fwrite(info_log.c_str(), info_log_length, 1, fp.get());
      }

      glDeleteShader(shader);
      return false;
    }
  }

  m_id = shader;

#ifdef _DEBUG
  if (glObjectLabel && !m_debug_name.empty())
  {
    glObjectLabel(GL_SHADER, shader, static_cast<GLsizei>(m_debug_name.length()),
                  static_cast<const GLchar*>(m_debug_name.data()));
    m_debug_name = {};
  }
#endif

  return true;
}

std::unique_ptr<GPUShader> OpenGLDevice::CreateShaderFromBinary(GPUShaderStage stage, gsl::span<const u8> data)
{
  // Not supported.. except spir-v maybe? but no point really...
  return {};
}

std::unique_ptr<GPUShader> OpenGLDevice::CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                                const char* entry_point,
                                                                DynamicHeapArray<u8>* out_binary)
{
  if (std::strcmp(entry_point, "main") != 0)
  {
    Log_ErrorPrintf("Entry point must be 'main', but got '%s' instead.", entry_point);
    return {};
  }

  return std::unique_ptr<GPUShader>(
    new OpenGLShader(stage, GPUShaderCache::GetCacheKey(stage, source, entry_point), std::string(source)));
}

//////////////////////////////////////////////////////////////////////////

bool OpenGLPipeline::VertexArrayCacheKey::operator==(const VertexArrayCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}

bool OpenGLPipeline::VertexArrayCacheKey::operator!=(const VertexArrayCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

size_t OpenGLPipeline::VertexArrayCacheKeyHash::operator()(const VertexArrayCacheKey& k) const
{
  std::size_t h = 0;
  hash_combine(h, k.num_vertex_attributes, k.vertex_attribute_stride);
  for (const VertexAttribute& va : k.vertex_attributes)
    hash_combine(h, va.key);
  return h;
}

bool OpenGLPipeline::ProgramCacheKey::operator==(const ProgramCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}

bool OpenGLPipeline::ProgramCacheKey::operator!=(const ProgramCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

size_t OpenGLPipeline::ProgramCacheKeyHash::operator()(const ProgramCacheKey& k) const
{
  // TODO: maybe use xxhash here...
  std::size_t h = 0;
  hash_combine(h, k.vs_key.entry_point_low, k.vs_key.entry_point_high, k.vs_key.source_hash_low,
               k.vs_key.source_hash_high, k.vs_key.source_length, k.vs_key.shader_type);
  hash_combine(h, k.fs_key.entry_point_low, k.fs_key.entry_point_high, k.fs_key.source_hash_low,
               k.fs_key.source_hash_high, k.fs_key.source_length, k.fs_key.shader_type);
  hash_combine(h, k.va_key.num_vertex_attributes, k.va_key.vertex_attribute_stride);
  for (const VertexAttribute& va : k.va_key.vertex_attributes)
    hash_combine(h, va.key);
  return h;
}

OpenGLPipeline::ProgramCacheKey OpenGLPipeline::GetProgramCacheKey(const GraphicsConfig& plconfig)
{
  Assert(plconfig.input_layout.vertex_attributes.size() <= MAX_VERTEX_ATTRIBUTES);

  ProgramCacheKey ret;
  ret.vs_key = static_cast<const OpenGLShader*>(plconfig.vertex_shader)->GetKey();
  ret.fs_key = static_cast<const OpenGLShader*>(plconfig.fragment_shader)->GetKey();

  std::memset(ret.va_key.vertex_attributes, 0, sizeof(ret.va_key.vertex_attributes));
  ret.va_key.vertex_attribute_stride = 0;
  ret.va_key.num_vertex_attributes = static_cast<u32>(plconfig.input_layout.vertex_attributes.size());

  if (ret.va_key.num_vertex_attributes > 0)
  {
    std::memcpy(ret.va_key.vertex_attributes, plconfig.input_layout.vertex_attributes.data(),
                sizeof(VertexAttribute) * ret.va_key.num_vertex_attributes);
    ret.va_key.vertex_attribute_stride = plconfig.input_layout.vertex_stride;
  }

  return ret;
}

GLuint OpenGLDevice::LookupProgramCache(const OpenGLPipeline::ProgramCacheKey& key,
                                        const GPUPipeline::GraphicsConfig& plconfig)
{
  auto it = m_program_cache.find(key);
  if (it != m_program_cache.end() && it->second.program_id == 0 && it->second.file_uncompressed_size > 0)
  {
    it->second.program_id = CreateProgramFromPipelineCache(it->second, plconfig);
    if (it->second.program_id == 0)
    {
      Log_ErrorPrintf("Failed to create program from binary.");
      m_program_cache.erase(it);
      it = m_program_cache.end();
      DiscardPipelineCache();
    }
  }

  if (it != m_program_cache.end())
  {
    if (it->second.program_id != 0)
      it->second.reference_count++;

    return it->second.program_id;
  }

  OpenGLPipeline::ProgramCacheItem item;
  item.program_id = CompileProgram(plconfig);
  item.reference_count = 0;
  item.file_format = 0;
  item.file_offset = 0;
  item.file_uncompressed_size = 0;
  item.file_compressed_size = 0;
  if (item.program_id != 0)
  {
    AddToPipelineCache(&item);
    item.reference_count++;
  }

  // Insert into cache even if we failed, so we don't compile it again, but don't increment reference count.
  m_program_cache.emplace(key, item);
  return item.program_id;
}

GLuint OpenGLDevice::CompileProgram(const GPUPipeline::GraphicsConfig& plconfig)
{
  OpenGLShader* vertex_shader = static_cast<OpenGLShader*>(plconfig.vertex_shader);
  OpenGLShader* fragment_shader = static_cast<OpenGLShader*>(plconfig.fragment_shader);
  if (!vertex_shader || !fragment_shader || !vertex_shader->Compile() || !fragment_shader->Compile())
  {
    Log_ErrorPrintf("Failed to compile shaders.");
    return 0;
  }

  glGetError();
  const GLuint program_id = glCreateProgram();
  if (glGetError() != GL_NO_ERROR)
  {
    Log_ErrorPrintf("Failed to create program object.");
    return 0;
  }

  if (m_pipeline_disk_cache_file)
    glProgramParameteri(program_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

  Assert(plconfig.vertex_shader && plconfig.fragment_shader);
  glAttachShader(program_id, vertex_shader->GetGLId());
  glAttachShader(program_id, fragment_shader->GetGLId());

  if (!ShaderGen::UseGLSLBindingLayout())
  {
    static constexpr std::array<const char*, static_cast<u8>(GPUPipeline::VertexAttribute::Semantic::MaxCount)>
      semantic_vars = {{
        "a_pos", // Position
        "a_tex", // TexCoord
        "a_col", // Color
      }};

    for (u32 i = 0; i < static_cast<u32>(plconfig.input_layout.vertex_attributes.size()); i++)
    {
      const GPUPipeline::VertexAttribute& va = plconfig.input_layout.vertex_attributes[i];
      if (va.semantic == GPUPipeline::VertexAttribute::Semantic::Position && va.semantic_index == 0)
      {
        glBindAttribLocation(program_id, i, "a_pos");
      }
      else
      {
        glBindAttribLocation(
          program_id, i,
          TinyString::FromFmt("{}{}", semantic_vars[static_cast<u8>(va.semantic.GetValue())], va.semantic_index));
      }
    }

    glBindFragDataLocation(program_id, 0, "o_col0");

    if (m_features.dual_source_blend)
    {
      if (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended)
        glBindFragDataLocationIndexed(program_id, 1, 0, "o_col1");
      else if (GLAD_GL_EXT_blend_func_extended)
        glBindFragDataLocationIndexedEXT(program_id, 1, 0, "o_col1");
    }
  }

  glLinkProgram(program_id);

  GLint status = GL_FALSE;
  glGetProgramiv(program_id, GL_LINK_STATUS, &status);

  GLint info_log_length = 0;
  glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetProgramInfoLog(program_id, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Program linked with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Program failed to link:\n%s", info_log.c_str());
      glDeleteProgram(program_id);
      return 0;
    }
  }

  PostLinkProgram(plconfig, program_id);

  return program_id;
}

void OpenGLDevice::PostLinkProgram(const GPUPipeline::GraphicsConfig& plconfig, GLuint program_id)
{
  if (!ShaderGen::UseGLSLBindingLayout())
  {
    GLint location = glGetUniformBlockIndex(program_id, "UBOBlock");
    if (location >= 0)
      glUniformBlockBinding(program_id, location, 1);

    glUseProgram(program_id);

    // Texture buffer is zero here, so we have to bump it.
    const u32 num_textures = std::max<u32>(GetActiveTexturesForLayout(plconfig.layout), 1);
    for (u32 i = 0; i < num_textures; i++)
    {
      location = glGetUniformLocation(program_id, TinyString::FromFmt("samp{}", i));
      if (location >= 0)
        glUniform1i(location, i);
    }

    glUseProgram(m_last_program);
  }
}

void OpenGLDevice::UnrefProgram(const OpenGLPipeline::ProgramCacheKey& key)
{
  auto it = m_program_cache.find(key);
  Assert(it != m_program_cache.end() && it->second.program_id != 0 && it->second.reference_count > 0);

  if ((--it->second.reference_count) > 0)
    return;

  if (m_last_program == it->second.program_id)
  {
    m_last_program = 0;
    glUseProgram(0);
  }

  glDeleteProgram(it->second.program_id);
  it->second.program_id = 0;
}

GLuint OpenGLDevice::LookupVAOCache(const OpenGLPipeline::VertexArrayCacheKey& key)
{
  auto it = m_vao_cache.find(key);
  if (it != m_vao_cache.end())
  {
    it->second.reference_count++;
    return it->second.vao_id;
  }

  OpenGLPipeline::VertexArrayCacheItem item;
  item.vao_id =
    CreateVAO(gsl::span<const GPUPipeline::VertexAttribute>(key.vertex_attributes, key.num_vertex_attributes),
              key.vertex_attribute_stride);
  if (item.vao_id == 0)
    return 0;

  item.reference_count = 1;
  m_vao_cache.emplace(key, item);
  return item.vao_id;
}

GLuint OpenGLDevice::CreateVAO(gsl::span<const GPUPipeline::VertexAttribute> attributes, u32 stride)
{
  glGetError();
  GLuint vao;
  glGenVertexArrays(1, &vao);
  if (const GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    Log_ErrorPrintf("Failed to create vertex array object: %u", vao);
    return 0;
  }

  glBindVertexArray(vao);
  m_vertex_buffer->Bind();
  m_index_buffer->Bind();

  struct VAMapping
  {
    GLenum type;
    GLboolean normalized;
    GLboolean integer;
  };
  static constexpr const std::array<VAMapping, static_cast<u8>(GPUPipeline::VertexAttribute::Type::MaxCount)>
    format_mapping = {{
      {GL_FLOAT, GL_FALSE, GL_FALSE},         // Float
      {GL_UNSIGNED_BYTE, GL_FALSE, GL_TRUE},  // UInt8
      {GL_BYTE, GL_FALSE, GL_TRUE},           // SInt8
      {GL_UNSIGNED_BYTE, GL_TRUE, GL_FALSE},  // UNorm8
      {GL_UNSIGNED_SHORT, GL_FALSE, GL_TRUE}, // UInt16
      {GL_SHORT, GL_FALSE, GL_TRUE},          // SInt16
      {GL_UNSIGNED_SHORT, GL_TRUE, GL_FALSE}, // UNorm16
      {GL_UNSIGNED_INT, GL_FALSE, GL_TRUE},   // UInt32
      {GL_INT, GL_FALSE, GL_TRUE},            // SInt32
    }};

  for (u32 i = 0; i < static_cast<u32>(attributes.size()); i++)
  {
    const GPUPipeline::VertexAttribute& va = attributes[i];
    const VAMapping& m = format_mapping[static_cast<u8>(va.type.GetValue())];
    const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(va.offset.GetValue()));
    glEnableVertexAttribArray(i);
    if (m.integer)
      glVertexAttribIPointer(i, va.components, m.type, stride, ptr);
    else
      glVertexAttribPointer(i, va.components, m.type, m.normalized, stride, ptr);
  }

  glBindVertexArray(m_last_vao);

  return vao;
}

void OpenGLDevice::UnrefVAO(const OpenGLPipeline::VertexArrayCacheKey& key)
{
  auto it = m_vao_cache.find(key);
  Assert(it != m_vao_cache.end() && it->second.reference_count > 0);

  if ((--it->second.reference_count) > 0)
    return;

  if (m_last_vao == it->second.vao_id)
  {
    m_last_vao = 0;
    glBindVertexArray(0);
  }

  glDeleteVertexArrays(1, &it->second.vao_id);
  m_vao_cache.erase(it);
}

OpenGLPipeline::OpenGLPipeline(const ProgramCacheKey& key, GLuint program, GLuint vao, const RasterizationState& rs,
                               const DepthState& ds, const BlendState& bs, GLenum topology)
  : m_key(key), m_program(program), m_vao(vao), m_blend_state(bs), m_rasterization_state(rs), m_depth_state(ds),
    m_topology(topology)
{
}

OpenGLPipeline::~OpenGLPipeline()
{
  OpenGLDevice& dev = OpenGLDevice::GetInstance();
  dev.UnbindPipeline(this);
  dev.UnrefProgram(m_key);
  dev.UnrefVAO(m_key.va_key);
}

void OpenGLPipeline::SetDebugName(const std::string_view& name)
{
#ifdef _DEBUG
  if (glObjectLabel)
    glObjectLabel(GL_PROGRAM, m_program, static_cast<u32>(name.length()), name.data());
#endif
}

std::unique_ptr<GPUPipeline> OpenGLDevice::CreatePipeline(const GPUPipeline::GraphicsConfig& config)
{
  const OpenGLPipeline::ProgramCacheKey pkey = OpenGLPipeline::GetProgramCacheKey(config);

  const GLuint program_id = LookupProgramCache(pkey, config);
  if (program_id == 0)
    return {};

  const GLuint vao_id = LookupVAOCache(pkey.va_key);
  if (vao_id == 0)
  {
    UnrefProgram(pkey);
    return {};
  }

  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives = {{
    GL_POINTS,         // Points
    GL_LINES,          // Lines
    GL_TRIANGLES,      // Triangles
    GL_TRIANGLE_STRIP, // TriangleStrips
  }};

  return std::unique_ptr<GPUPipeline>(new OpenGLPipeline(pkey, program_id, vao_id, config.rasterization, config.depth,
                                                         config.blend, primitives[static_cast<u8>(config.primitive)]));
}

ALWAYS_INLINE static void ApplyRasterizationState(const GPUPipeline::RasterizationState& rs)
{
  if (rs.cull_mode == GPUPipeline::CullMode::None)
  {
    glDisable(GL_CULL_FACE);
  }
  else
  {
    glEnable(GL_CULL_FACE);
    glCullFace((rs.cull_mode == GPUPipeline::CullMode::Front) ? GL_FRONT : GL_BACK);
  }

  // TODO: always enabled, should be done at init time
  glEnable(GL_SCISSOR_TEST);
}

ALWAYS_INLINE static void ApplyDepthState(const GPUPipeline::DepthState& ds)
{
  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)> func_mapping = {{
    GL_NEVER,   // Never
    GL_ALWAYS,  // Always
    GL_LESS,    // Less
    GL_LEQUAL,  // LessEqual
    GL_GREATER, // Greater
    GL_GEQUAL,  // GreaterEqual
    GL_EQUAL,   // Equal
  }};

  (ds.depth_test != GPUPipeline::DepthFunc::Never) ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
  glDepthFunc(func_mapping[static_cast<u8>(ds.depth_test.GetValue())]);
  glDepthMask(ds.depth_write);
}

ALWAYS_INLINE static void ApplyBlendState(const GPUPipeline::BlendState& bs)
{
  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
    GL_ZERO,                     // Zero
    GL_ONE,                      // One
    GL_SRC_COLOR,                // SrcColor
    GL_ONE_MINUS_SRC_COLOR,      // InvSrcColor
    GL_DST_COLOR,                // DstColor
    GL_ONE_MINUS_DST_COLOR,      // InvDstColor
    GL_SRC_ALPHA,                // SrcAlpha
    GL_ONE_MINUS_SRC_ALPHA,      // InvSrcAlpha
    GL_SRC1_ALPHA,               // SrcAlpha1
    GL_ONE_MINUS_SRC1_ALPHA,     // InvSrcAlpha1
    GL_DST_ALPHA,                // DstAlpha
    GL_ONE_MINUS_DST_ALPHA,      // InvDstAlpha
    GL_CONSTANT_COLOR,           // ConstantColor
    GL_ONE_MINUS_CONSTANT_COLOR, // InvConstantColor
  }};

  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
    GL_FUNC_ADD,              // Add
    GL_FUNC_SUBTRACT,         // Subtract
    GL_FUNC_REVERSE_SUBTRACT, // ReverseSubtract
    GL_MIN,                   // Min
    GL_MAX,                   // Max
  }};

  // TODO: driver bugs
  // TODO: rdoc and look for redundant calls

  bs.enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);

  if (bs.enable)
  {
    glBlendFuncSeparate(blend_mapping[static_cast<u8>(bs.src_blend.GetValue())],
                        blend_mapping[static_cast<u8>(bs.dst_blend.GetValue())],
                        blend_mapping[static_cast<u8>(bs.src_alpha_blend.GetValue())],
                        blend_mapping[static_cast<u8>(bs.dst_alpha_blend.GetValue())]);
    glBlendEquationSeparate(op_mapping[static_cast<u8>(bs.blend_op.GetValue())],
                            op_mapping[static_cast<u8>(bs.alpha_blend_op.GetValue())]);

    // TODO: cache this to avoid calls?
    glBlendColor(bs.GetConstantRed(), bs.GetConstantGreen(), bs.GetConstantBlue(), bs.GetConstantAlpha());
  }

  glColorMask(bs.write_r, bs.write_g, bs.write_b, bs.write_a);
}

void OpenGLDevice::SetPipeline(GPUPipeline* pipeline)
{
  if (m_current_pipeline == pipeline)
    return;

  OpenGLPipeline* const P = static_cast<OpenGLPipeline*>(pipeline);
  m_current_pipeline = P;

  if (m_last_rasterization_state != P->GetRasterizationState())
  {
    m_last_rasterization_state = P->GetRasterizationState();
    ApplyRasterizationState(m_last_rasterization_state);
  }
  if (m_last_depth_state != P->GetDepthState())
  {
    m_last_depth_state = P->GetDepthState();
    ApplyDepthState(m_last_depth_state);
  }
  if (m_last_blend_state != P->GetBlendState())
  {
    m_last_blend_state = P->GetBlendState();
    ApplyBlendState(m_last_blend_state);
  }
  if (m_last_vao != P->GetVAO())
  {
    m_last_vao = P->GetVAO();
    glBindVertexArray(m_last_vao);
  }
  if (m_last_program != P->GetProgram())
  {
    m_last_program = P->GetProgram();
    glUseProgram(m_last_program);
  }
}

bool OpenGLDevice::ReadPipelineCache(const std::string& filename)
{
  DebugAssert(!m_pipeline_disk_cache_file);

  m_pipeline_disk_cache_file = FileSystem::OpenCFile(filename.c_str(), "r+b");
  m_pipeline_disk_cache_filename = filename;

  if (!m_pipeline_disk_cache_file)
  {
    // Multiple instances running? Ignore.
    if (errno == EACCES)
    {
      m_pipeline_disk_cache_filename = {};
      return true;
    }

    // If it doesn't exist, we're going to create it.
    if (errno != ENOENT)
    {
      Log_WarningPrintf("Failed to open shader cache: %d", errno);
      m_pipeline_disk_cache_filename = {};
      return false;
    }

    Log_WarningPrintf("Disk cache does not exist, creating.");
    return DiscardPipelineCache();
  }

  // Read footer.
  const s64 size = FileSystem::FSize64(m_pipeline_disk_cache_file);
  if (size < sizeof(PipelineDiskCacheFooter) || size >= static_cast<s64>(std::numeric_limits<u32>::max()))
    return DiscardPipelineCache();

  PipelineDiskCacheFooter file_footer;
  if (FileSystem::FSeek64(m_pipeline_disk_cache_file, size - sizeof(PipelineDiskCacheFooter), SEEK_SET) != 0 ||
      std::fread(&file_footer, sizeof(file_footer), 1, m_pipeline_disk_cache_file) != 1)
  {
    Log_ErrorPrintf("Failed to read disk cache footer.");
    return DiscardPipelineCache();
  }

  PipelineDiskCacheFooter expected_footer;
  FillFooter(&expected_footer, m_shader_cache.GetVersion());

  if (file_footer.version != expected_footer.version ||
      std::strncmp(file_footer.driver_vendor, expected_footer.driver_vendor, std::size(file_footer.driver_vendor)) !=
        0 ||
      std::strncmp(file_footer.driver_renderer, expected_footer.driver_renderer,
                   std::size(file_footer.driver_renderer)) != 0 ||
      std::strncmp(file_footer.driver_version, expected_footer.driver_version, std::size(file_footer.driver_version)) !=
        0)
  {
    Log_ErrorPrintf("Disk cache does not match expected driver/version.");
    return DiscardPipelineCache();
  }

  m_pipeline_disk_cache_data_end = static_cast<u32>(size) - sizeof(PipelineDiskCacheFooter) -
                                   (sizeof(PipelineDiskCacheIndexEntry) * file_footer.num_programs);
  if (m_pipeline_disk_cache_data_end < 0 ||
      FileSystem::FSeek64(m_pipeline_disk_cache_file, m_pipeline_disk_cache_data_end, SEEK_SET) != 0)
  {
    Log_ErrorPrintf("Failed to seek to start of index entries.");
    return DiscardPipelineCache();
  }

  // Read entries.
  for (u32 i = 0; i < file_footer.num_programs; i++)
  {
    PipelineDiskCacheIndexEntry entry;
    if (std::fread(&entry, sizeof(entry), 1, m_pipeline_disk_cache_file) != 1 ||
        (static_cast<s64>(entry.offset) + static_cast<s64>(entry.compressed_size)) >= size)
    {
      Log_ErrorPrintf("Failed to read disk cache entry.");
      return DiscardPipelineCache();
    }

    if (m_program_cache.find(entry.key) != m_program_cache.end())
    {
      Log_ErrorPrintf("Duplicate program in disk cache.");
      return DiscardPipelineCache();
    }

    OpenGLPipeline::ProgramCacheItem pitem;
    pitem.program_id = 0;
    pitem.reference_count = 0;
    pitem.file_format = entry.format;
    pitem.file_offset = entry.offset;
    pitem.file_uncompressed_size = entry.uncompressed_size;
    pitem.file_compressed_size = entry.compressed_size;
    m_program_cache.emplace(entry.key, pitem);
  }

  Log_VerbosePrintf("Read %zu programs from disk cache.", m_program_cache.size());
  return true;
}

bool OpenGLDevice::GetPipelineCacheData(DynamicHeapArray<u8>* data)
{
  // Self-managed.
  return false;
}

GLuint OpenGLDevice::CreateProgramFromPipelineCache(const OpenGLPipeline::ProgramCacheItem& it,
                                                    const GPUPipeline::GraphicsConfig& plconfig)
{
  DynamicHeapArray<u8> data(it.file_uncompressed_size);
  DynamicHeapArray<u8> compressed_data(it.file_compressed_size);

  if (FileSystem::FSeek64(m_pipeline_disk_cache_file, it.file_offset, SEEK_SET) != 0 ||
      std::fread(compressed_data.data(), it.file_compressed_size, 1, m_pipeline_disk_cache_file) != 1)
  {
    Log_ErrorPrintf("Failed to read program from disk cache.");
    return 0;
  }

  const size_t decompress_result =
    ZSTD_decompress(data.data(), data.size(), compressed_data.data(), compressed_data.size());
  if (ZSTD_isError(decompress_result))
  {
    Log_ErrorPrintf("Failed to decompress program from disk cache: %s", ZSTD_getErrorName(decompress_result));
    return 0;
  }
  compressed_data.deallocate();

  glGetError();
  GLuint prog = glCreateProgram();
  if (const GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    Log_ErrorPrintf("Failed to create program object: %u", err);
    return 0;
  }

  glProgramBinary(prog, it.file_format, data.data(), it.file_uncompressed_size);

  GLint link_status;
  glGetProgramiv(prog, GL_LINK_STATUS, &link_status);
  if (link_status != GL_TRUE)
  {
    Log_ErrorPrintf("Failed to create GL program from binary: status %d, discarding cache.", link_status);
    glDeleteProgram(prog);
    return 0;
  }

  PostLinkProgram(plconfig, prog);

  return prog;
}

void OpenGLDevice::AddToPipelineCache(OpenGLPipeline::ProgramCacheItem* it)
{
  DebugAssert(it->program_id != 0 && it->file_uncompressed_size == 0);
  DebugAssert(m_pipeline_disk_cache_file);

  GLint binary_size = 0;
  glGetProgramiv(it->program_id, GL_PROGRAM_BINARY_LENGTH, &binary_size);
  if (binary_size == 0)
  {
    Log_WarningPrint("glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) returned 0");
    return;
  }

  GLenum format = 0;
  DynamicHeapArray<u8> uncompressed_data(binary_size);
  glGetProgramBinary(it->program_id, binary_size, &binary_size, &format, uncompressed_data.data());
  if (binary_size == 0)
  {
    Log_WarningPrint("glGetProgramBinary() failed");
    return;
  }
  else if (static_cast<size_t>(binary_size) != uncompressed_data.size())
  {
    Log_WarningPrintf("Size changed from %zu to %d after glGetProgramBinary()", uncompressed_data.size(), binary_size);
  }

  DynamicHeapArray<u8> compressed_data(ZSTD_compressBound(binary_size));
  const size_t compress_result =
    ZSTD_compress(compressed_data.data(), compressed_data.size(), uncompressed_data.data(), binary_size, 0);
  if (ZSTD_isError(compress_result))
  {
    Log_ErrorPrintf("Failed to compress program: %s", ZSTD_getErrorName(compress_result));
    return;
  }

  Log_DevPrintf("Program binary retrieved and compressed, %zu -> %zu bytes, format %u",
                static_cast<size_t>(binary_size), compress_result, format);

  if (FileSystem::FSeek64(m_pipeline_disk_cache_file, m_pipeline_disk_cache_data_end, SEEK_SET) != 0 ||
      std::fwrite(compressed_data.data(), compress_result, 1, m_pipeline_disk_cache_file) != 1)
  {
    Log_ErrorPrintf("Failed to write binary to disk cache.");
  }

  it->file_format = format;
  it->file_offset = m_pipeline_disk_cache_data_end;
  it->file_uncompressed_size = static_cast<u32>(binary_size);
  it->file_compressed_size = static_cast<u32>(compress_result);
  m_pipeline_disk_cache_data_end += static_cast<u32>(compress_result);
  m_pipeline_disk_cache_changed = true;
}

bool OpenGLDevice::DiscardPipelineCache()
{
  // Remove any other disk cache entries which haven't been loaded.
  for (auto it = m_program_cache.begin(); it != m_program_cache.end();)
  {
    if (it->second.program_id != 0)
    {
      it->second.file_format = 0;
      it->second.file_offset = 0;
      it->second.file_uncompressed_size = 0;
      it->second.file_compressed_size = 0;
      ++it;
      continue;
    }

    it = m_program_cache.erase(it);
  }

  if (m_pipeline_disk_cache_file)
    std::fclose(m_pipeline_disk_cache_file);

  m_pipeline_disk_cache_data_end = 0;
  m_pipeline_disk_cache_file = FileSystem::OpenCFile(m_pipeline_disk_cache_filename.c_str(), "w+b");
  if (!m_pipeline_disk_cache_file)
  {
    Log_ErrorPrintf("Failed to reopen pipeline cache: %d", errno);
    m_pipeline_disk_cache_filename = {};
    return false;
  }

  return true;
}

void OpenGLDevice::ClosePipelineCache()
{
  const ScopedGuard file_closer = [this]() {
    std::fclose(m_pipeline_disk_cache_file);
    m_pipeline_disk_cache_file = nullptr;
  };

  if (!m_pipeline_disk_cache_changed)
  {
    Log_VerbosePrintf("Not updating pipeline cache because it has not changed.");
    return;
  }

  if (FileSystem::FSeek64(m_pipeline_disk_cache_file, m_pipeline_disk_cache_data_end, SEEK_SET) != 0)
  {
    Log_ErrorPrintf("Failed to seek to data end.");
    return;
  }

  u32 count = 0;

  for (const auto& it : m_program_cache)
  {
    if (it.second.file_uncompressed_size == 0)
      continue;

    PipelineDiskCacheIndexEntry entry;
    std::memcpy(&entry.key, &it.first, sizeof(entry.key));
    entry.format = it.second.file_format;
    entry.offset = it.second.file_offset;
    entry.compressed_size = it.second.file_compressed_size;
    entry.uncompressed_size = it.second.file_uncompressed_size;

    if (std::fwrite(&entry, sizeof(entry), 1, m_pipeline_disk_cache_file) != 1)
    {
      Log_ErrorPrintf("Failed to write index entry.");
      return;
    }

    count++;
  }

  PipelineDiskCacheFooter footer;
  FillFooter(&footer, m_shader_cache.GetVersion());
  footer.num_programs = count;

  if (std::fwrite(&footer, sizeof(footer), 1, m_pipeline_disk_cache_file) != 1)
    Log_ErrorPrintf("Failed to write footer.");
}