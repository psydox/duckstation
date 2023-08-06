// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"
#include "metal_stream_buffer.h"
#include "postprocessing_chain.h"

#include "common/rectangle.h"
#include "common/timer.h"
#include "common/window_info.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>

#ifndef __OBJC__
#error This file needs to be compiled with Objective C++.
#endif

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

class MetalDevice;
class MetalFramebuffer;
class MetalPipeline;
class MetalTexture;

class MetalSampler final : public GPUSampler
{
  friend MetalDevice;

public:
  ~MetalSampler() override;

	ALWAYS_INLINE id<MTLSamplerState> GetSamplerState() const { return m_ss; }

  void SetDebugName(const std::string_view& name) override;

private:
  MetalSampler(id<MTLSamplerState> ss);

	id<MTLSamplerState> m_ss;
};

class MetalShader final : public GPUShader
{
  friend MetalDevice;

public:
  ~MetalShader() override;

	ALWAYS_INLINE id<MTLLibrary> GetLibrary() const { return m_library; }
	ALWAYS_INLINE id<MTLFunction> GetFunction() const { return m_function; }

  void SetDebugName(const std::string_view& name) override;

private:
  MetalShader(GPUShaderStage stage, id<MTLLibrary> library, id<MTLFunction> function);

	id<MTLLibrary> m_library;
	id<MTLFunction> m_function;
};

class MetalPipeline final : public GPUPipeline
{
  friend MetalDevice;

public:
  ~MetalPipeline() override;
	
	ALWAYS_INLINE id<MTLRenderPipelineState> GetPipelineState() const { return m_pipeline; }
	ALWAYS_INLINE id<MTLDepthStencilState> GetDepthState() const { return m_depth; }
	ALWAYS_INLINE MTLCullMode GetCullMode() const { return m_cull_mode; }
	ALWAYS_INLINE MTLPrimitiveType GetPrimitive() const { return m_primitive; }

  void SetDebugName(const std::string_view& name) override;

private:
  MetalPipeline(id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth, MTLCullMode cull_mode, MTLPrimitiveType primitive);
	
	id<MTLRenderPipelineState> m_pipeline;
	id<MTLDepthStencilState> m_depth;
	MTLCullMode m_cull_mode;
	MTLPrimitiveType m_primitive;
};

class MetalTexture final : public GPUTexture
{
  friend MetalDevice;

public:
  ~MetalTexture();

	ALWAYS_INLINE id<MTLTexture> GetMTLTexture() const { return m_texture; }

  bool Create(id<MTLDevice> device, u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type,
              Format format, const void* initial_data = nullptr, u32 initial_data_stride = 0);
  void Destroy();

  bool IsValid() const override;

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;

  void SetDebugName(const std::string_view& name) override;

private:
	MetalTexture(id<MTLTexture> texture, u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type,
							 Format format);
	
	id<MTLTexture> m_texture;

	u16 m_map_x = 0;
	u16 m_map_y = 0;
  u16 m_map_width = 0;
	u16 m_map_height = 0;
	u8 m_map_layer = 0;
	u8 m_map_level = 0;
};

#if 0
class MetalTextureBuffer final : public GPUTextureBuffer
{
public:
  MetalTextureBuffer(Format format, u32 size_in_elements);
  ~MetalTextureBuffer() override;

  ALWAYS_INLINE IMetalBuffer* GetBuffer() const { return m_buffer.GetD3DBuffer(); }
  ALWAYS_INLINE IMetalShaderResourceView* GetSRV() const { return m_srv.Get(); }
  ALWAYS_INLINE IMetalShaderResourceView* const* GetSRVArray() const { return m_srv.GetAddressOf(); }

  bool CreateBuffer(IMetalDevice* device);

  // Inherited via GPUTextureBuffer
  virtual void* Map(u32 required_elements) override;
  virtual void Unmap(u32 used_elements) override;

private:
  MetalStreamBuffer m_buffer;
  Microsoft::WRL::ComPtr<IMetalShaderResourceView> m_srv;
};
#endif

class MetalFramebuffer final : public GPUFramebuffer
{
	friend MetalDevice;

public:
	~MetalFramebuffer() override;
	
	MTLRenderPassDescriptor* GetDescriptor() const;

	void SetDebugName(const std::string_view& name) override;

private:
	MetalFramebuffer(GPUTexture* rt, GPUTexture* ds, u32 width, u32 height, id<MTLTexture> rt_tex, id<MTLTexture> ds_tex,
									 MTLRenderPassDescriptor* descriptor);

	id<MTLTexture> m_rt_tex;
	id<MTLTexture> m_ds_tex;
	MTLRenderPassDescriptor* m_descriptor;
};

class MetalDevice final : public GPUDevice
{
public:
  ALWAYS_INLINE static MetalDevice& GetInstance() { return *static_cast<MetalDevice*>(g_host_display.get()); }
  ALWAYS_INLINE static id<MTLDevice> GetMTLDevice() { return GetInstance().m_device; }
	ALWAYS_INLINE static u64 GetCurrentFenceCounter() { return GetInstance().m_current_fence_counter; }
	ALWAYS_INLINE static u64 GetCompletedFenceCounter() { return GetInstance().m_completed_fence_counter; }

  MetalDevice();
  ~MetalDevice();

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;

  bool CreateDevice(const WindowInfo& wi, bool vsync) override;
  bool SetupDevice() override;

  bool MakeCurrent() override;
  bool DoneCurrent() override;

  bool ChangeWindow(const WindowInfo& new_wi) override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height) override;
  bool SupportsFullscreen() const override;
  bool IsFullscreen() override;
  bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) override;
  AdapterAndModeList GetAdapterAndModeList() override;
  void DestroySurface() override;

  std::string GetShaderCacheBaseName(const std::string_view& type, bool debug) const override;

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format,
                                            const void* data = nullptr, u32 data_stride = 0,
                                            bool dynamic = false) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements) override;

  bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;
  bool SupportsTextureFormat(GPUTexture::Format format) const override;
  void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                         u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;
  void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                            u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;

  std::unique_ptr<GPUFramebuffer> CreateFramebuffer(GPUTexture* rt = nullptr, u32 rt_layer = 0, u32 rt_level = 0,
                                                    GPUTexture* ds = nullptr, u32 ds_layer = 0,
                                                    u32 ds_level = 0) override;

  std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, gsl::span<const u8> data) override;
  std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                    std::vector<u8>* out_binary = nullptr) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config) override;

  void PushDebugGroup(const char* fmt, ...) override;
  void PopDebugGroup() override;
  void InsertDebugMessage(const char* fmt, ...) override;

  void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                       u32* map_base_vertex) override;
  void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) override;
  void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) override;
  void UnmapIndexBuffer(u32 used_index_count) override;
  void PushUniformBuffer(const void* data, u32 data_size) override;
  void* MapUniformBuffer(u32 size) override;
  void UnmapUniformBuffer(u32 size) override;
  void SetFramebuffer(GPUFramebuffer* fb) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(s32 x, s32 y, s32 width, s32 height) override;
  void SetScissor(s32 x, s32 y, s32 width, s32 height) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;

  bool GetHostRefreshRate(float* refresh_rate) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void SetVSync(bool enabled) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent() override;

	void WaitForFenceCounter(u64 counter);
	
	ALWAYS_INLINE MetalStreamBuffer& GetTextureStreamBuffer() { return m_texture_upload_buffer; }
	id<MTLBlitCommandEncoder> GetTextureUploadEncoder(bool is_inline);
	
	void SubmitCommandBuffer();
	void SubmitCommandBufferAndRestartRenderPass(const char* reason);
	
  void UnbindFramebuffer(MetalFramebuffer* fb);
  void UnbindPipeline(MetalPipeline* pl);
  void UnbindTexture(MetalTexture* tex);

  static AdapterAndModeList StaticGetAdapterAndModeList();

private:
  static constexpr u32 VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 INDEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_ALIGNMENT = 256;
	static constexpr u32 TEXTURE_STREAM_BUFFER_SIZE = 32/*16*/ * 1024 * 1024; // TODO reduce after separate allocations
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;
	
	using DepthStateMap = std::unordered_map<u8, id<MTLDepthStencilState>>;

	ALWAYS_INLINE NSView* GetWindowView() const { return (__bridge NSView*)m_window_info.window_handle; }
	
  void SetFeatures();
	
	std::unique_ptr<GPUShader> CreateShaderFromMSL(GPUShaderStage stage, const std::string_view& source, const std::string_view& entry_point);
	
	id<MTLDepthStencilState> GetDepthState(const GPUPipeline::DepthState& ds);
	
	void CreateCommandBuffer();
	void CommandBufferCompleted(u64 fence_counter);
	
	ALWAYS_INLINE bool InRenderPass() const { return (m_render_encoder != nil); }
	ALWAYS_INLINE bool IsInlineUploading() const { return (m_inline_upload_encoder != nil); }
	void BeginRenderPass();
	void EndRenderPass();
	void EndInlineUploading();
	void EndAnyEncoding();

  void PreDrawCheck();
	void SetInitialEncoderState();
	void SetUniformBufferInRenderEncoder();
	void SetViewportInRenderEncoder();
	void SetScissorInRenderEncoder();

  //bool CheckStagingBufferSize(u32 width, u32 height, DXGI_FORMAT format);
  //void DestroyStagingBuffer();

	bool CreateLayer();
	void DestroyLayer();

  bool CreateBuffers();
  void DestroyBuffers();

  bool CreateTimestampQueries();
  void DestroyTimestampQueries();
  void PopTimestampQuery();
  void KickTimestampQuery();

	id<MTLDevice> m_device;
	id<MTLCommandQueue> m_queue;
	
	CAMetalLayer* m_layer = nil;
	id<MTLDrawable> m_layer_drawable = nil;
	MTLRenderPassDescriptor* m_layer_pass_desc = nil;
	
	std::mutex m_fence_mutex;
	u64 m_current_fence_counter = 0;
	std::atomic<u64> m_completed_fence_counter{0};
	
	DepthStateMap m_depth_states;

//  ComPtr<IMetalTexture2D> m_readback_staging_texture;
//  DXGI_FORMAT m_readback_staging_texture_format = DXGI_FORMAT_UNKNOWN;
//  u32 m_readback_staging_texture_width = 0;
//  u32 m_readback_staging_texture_height = 0;

  MetalStreamBuffer m_vertex_buffer;
  MetalStreamBuffer m_index_buffer;
  MetalStreamBuffer m_uniform_buffer;
	MetalStreamBuffer m_texture_upload_buffer;
	
	id<MTLCommandBuffer> m_upload_cmdbuf = nil;
	id<MTLBlitCommandEncoder> m_upload_encoder = nil;
	id<MTLBlitCommandEncoder> m_inline_upload_encoder = nil;
	
	id<MTLCommandBuffer> m_render_cmdbuf = nil;
	id<MTLRenderCommandEncoder> m_render_encoder = nil;

  MetalFramebuffer* m_current_framebuffer = nullptr;

	MetalPipeline* m_current_pipeline = nullptr;
	id<MTLDepthStencilState> m_current_depth_state = nil;
	MTLCullMode m_current_cull_mode = MTLCullModeNone;
	u32 m_current_uniform_buffer_position = 0;

	std::array<id<MTLTexture>, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
	std::array<id<MTLSamplerState>, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};
	Common::Rectangle<s32> m_current_viewport = {};
	Common::Rectangle<s32> m_current_scissor = {};
	
	bool m_vsync_enabled = false;

//  std::array<std::array<ComPtr<IMetalQuery>, 3>, NUM_TIMESTAMP_QUERIES> m_timestamp_queries = {};
//  u8 m_read_timestamp_query = 0;
//  u8 m_write_timestamp_query = 0;
//  u8 m_waiting_timestamp_queries = 0;
//  bool m_timestamp_query_started = false;
//  float m_accumulated_gpu_time = 0.0f;
};