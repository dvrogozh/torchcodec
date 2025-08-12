#include <unistd.h>

#include <level_zero/ze_api.h>
#include <va/va_drmcommon.h>

#include <ATen/DLConvertor.h>
#include <c10/xpu/XPUStream.h>

#include "src/torchcodec/_core/XpuDeviceInterface.h"
#include "src/torchcodec/_core/FFMPEGCommon.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
}

namespace facebook::torchcodec {
namespace {

static bool g_xpu =
    registerDeviceInterface(torch::kXPU, [](const torch::Device& device) {
      return new XpuDeviceInterface(device);
    });

const int MAX_XPU_GPUS = 128;
// Set to -1 to have an infinitely sized cache. Set it to 0 to disable caching.
// Set to a positive number to have a cache of that size.
const int MAX_CONTEXTS_PER_GPU_IN_CACHE = -1;
std::vector<AVBufferRef*> g_cached_hw_device_ctxs[MAX_XPU_GPUS];
std::mutex g_cached_hw_device_mutexes[MAX_XPU_GPUS];

torch::DeviceIndex getFFMPEGCompatibleDeviceIndex(const torch::Device& device) {
  torch::DeviceIndex deviceIndex = device.index();
  deviceIndex = std::max<at::DeviceIndex>(deviceIndex, 0);
  TORCH_CHECK(deviceIndex >= 0, "Device index out of range");
  // For single GPU- machines libtorch returns -1 for the device index. So for
  // that case we set the device index to 0.
  return deviceIndex;
}

void addToCacheIfCacheHasCapacity(
    const torch::Device& device,
    AVBufferRef* hwContext) {
  torch::DeviceIndex deviceIndex = getFFMPEGCompatibleDeviceIndex(device);
  if (static_cast<int>(deviceIndex) >= MAX_XPU_GPUS) {
    return;
  }
  std::scoped_lock lock(g_cached_hw_device_mutexes[deviceIndex]);
  if (MAX_CONTEXTS_PER_GPU_IN_CACHE >= 0 &&
      g_cached_hw_device_ctxs[deviceIndex].size() >=
          MAX_CONTEXTS_PER_GPU_IN_CACHE) {
    return;
  }
  g_cached_hw_device_ctxs[deviceIndex].push_back(av_buffer_ref(hwContext));
}

AVBufferRef* getFromCache(const torch::Device& device) {
  torch::DeviceIndex deviceIndex = getFFMPEGCompatibleDeviceIndex(device);
  if (static_cast<int>(deviceIndex) >= MAX_XPU_GPUS) {
    return nullptr;
  }
  std::scoped_lock lock(g_cached_hw_device_mutexes[deviceIndex]);
  if (g_cached_hw_device_ctxs[deviceIndex].size() > 0) {
    AVBufferRef* hw_device_ctx = g_cached_hw_device_ctxs[deviceIndex].back();
    g_cached_hw_device_ctxs[deviceIndex].pop_back();
    return hw_device_ctx;
  }
  return nullptr;
}

AVBufferRef* getVaapiContext(const torch::Device& device) {
  enum AVHWDeviceType type = av_hwdevice_find_type_by_name("vaapi");
  TORCH_CHECK(type != AV_HWDEVICE_TYPE_NONE, "Failed to find vaapi device");
  torch::DeviceIndex nonNegativeDeviceIndex =
      getFFMPEGCompatibleDeviceIndex(device);

  AVBufferRef* hw_device_ctx = getFromCache(device);
  if (hw_device_ctx != nullptr) {
    return hw_device_ctx;
  }

  std::string renderD = "/dev/dri/renderD128";

  sycl::device syclDevice = c10::xpu::get_raw_device(nonNegativeDeviceIndex);
  if (syclDevice.has(sycl::aspect::ext_intel_pci_address)) {
    auto BDF =
        syclDevice.get_info<sycl::ext::intel::info::device::pci_address>();
    renderD = "/dev/dri/by-path/pci-" + BDF + "-render";
  }

  int err =
      av_hwdevice_ctx_create(&hw_device_ctx, type, renderD.c_str(), nullptr, 0);
  if (err < 0) {
    TORCH_CHECK(
        false,
        "Failed to create specified HW device: ",
        getFFMPEGErrorStringFromErrorCode(err));
  }
  return hw_device_ctx;
}

} // namespace

bool XpuDeviceInterface::DecodedFrameContext::operator==(
    const XpuDeviceInterface::DecodedFrameContext& other) {
  return decodedWidth == other.decodedWidth &&
      decodedHeight == other.decodedHeight &&
      decodedFormat == other.decodedFormat &&
      expectedWidth == other.expectedWidth &&
      expectedHeight == other.expectedHeight;
}

bool XpuDeviceInterface::DecodedFrameContext::operator!=(
    const XpuDeviceInterface::DecodedFrameContext& other) {
  return !(*this == other);
}

XpuDeviceInterface::XpuDeviceInterface(const torch::Device& device)
    : DeviceInterface(device) {
  TORCH_CHECK(g_xpu, "XpuDeviceInterface was not registered!");
  TORCH_CHECK(
      device_.type() == torch::kXPU, "Unsupported device: ", device_.str());
}

XpuDeviceInterface::~XpuDeviceInterface() {
  if (ctx_) {
    addToCacheIfCacheHasCapacity(device_, ctx_);
    av_buffer_unref(&ctx_);
  }
}

VADisplay getVaDisplayFromAV(AVFrame* avFrame) {
  AVHWFramesContext* hwfc = (AVHWFramesContext*)avFrame->hw_frames_ctx->data;
  AVHWDeviceContext* hwdc = hwfc->device_ctx;
  AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwdc->hwctx;
  return vactx->display;
}

void XpuDeviceInterface::initializeContext(AVCodecContext* codecContext) {
  TORCH_CHECK(!ctx_, "FFmpeg HW device context already initialized");

  // It is important for pytorch itself to create the xpu context. If ffmpeg
  // creates the context it may not be compatible with pytorch.
  // This is a dummy tensor to initialize the xpu context.
  torch::Tensor dummyTensorForXpuInitialization = torch::empty(
      {1}, torch::TensorOptions().dtype(torch::kUInt8).device(device_));
  ctx_ = getVaapiContext(device_);
  codecContext->hw_device_ctx = av_buffer_ref(ctx_);
  return;
}

struct xpuManagerCtx {
  UniqueAVFrame avFrame;
  ze_context_handle_t zeCtx;
};

void deleter(DLManagedTensor* self) {
  std::unique_ptr<DLManagedTensor> tensor(self);
  std::unique_ptr<xpuManagerCtx> context((xpuManagerCtx*)self->manager_ctx);
  zeMemFree(context->zeCtx, self->dl_tensor.data);
}

torch::Tensor AVFrameToTensor(const torch::Device& device, AVFrame* frame) {
  VADRMPRIMESurfaceDescriptor desc{};

  VAStatus sts = vaExportSurfaceHandle(
      getVaDisplayFromAV(frame),
      (VASurfaceID)(uintptr_t)frame->data[3],
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_READ_ONLY,
      &desc);
  TORCH_CHECK(
      sts == VA_STATUS_SUCCESS,
      "vaExportSurfaceHandle failed: ",
      vaErrorStr(sts));

  TORCH_CHECK(desc.num_objects == 1, "Expected 1 fd, got ", desc.num_objects);
  TORCH_CHECK(desc.num_layers == 1, "Expected 1 layer, got ", desc.num_layers);
  TORCH_CHECK(
      desc.layers[0].num_planes == 1,
      "Expected 1 plane, got ",
      desc.num_layers);

  std::unique_ptr<xpuManagerCtx> context = std::make_unique<xpuManagerCtx>();
  ze_device_handle_t ze_device{};
  sycl::queue queue = c10::xpu::getCurrentXPUStream(device.index());

  queue
      .submit([&](sycl::handler& cgh) {
        cgh.host_task([&](const sycl::interop_handle& ih) {
          context->zeCtx =
              ih.get_native_context<sycl::backend::ext_oneapi_level_zero>();
          ze_device =
              ih.get_native_device<sycl::backend::ext_oneapi_level_zero>();
        });
      })
      .wait();

  ze_external_memory_import_fd_t import_fd_desc{};
  import_fd_desc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMPORT_FD;
  import_fd_desc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
  import_fd_desc.fd = desc.objects[0].fd;

  ze_device_mem_alloc_desc_t alloc_desc{};
  alloc_desc.pNext = &import_fd_desc;
  void* usm_ptr = nullptr;

  ze_result_t res = zeMemAllocDevice(
      context->zeCtx, &alloc_desc, desc.objects[0].size, 0, ze_device, &usm_ptr);
  TORCH_CHECK(
      res == ZE_RESULT_SUCCESS, "Failed to import fd=", desc.objects[0].fd);

  close(desc.objects[0].fd);

  std::unique_ptr<DLManagedTensor> dl_dst = std::make_unique<DLManagedTensor>();
  int64_t shape[3] = {desc.height, desc.width, 4};

  dl_dst->manager_ctx = context.release();
  dl_dst->deleter = deleter;
  dl_dst->dl_tensor.data = usm_ptr;
  dl_dst->dl_tensor.device.device_type = kDLOneAPI;
  dl_dst->dl_tensor.device.device_id = device.index();
  dl_dst->dl_tensor.ndim = 3;
  dl_dst->dl_tensor.dtype.code = kDLUInt;
  dl_dst->dl_tensor.dtype.bits = 8;
  dl_dst->dl_tensor.dtype.lanes = 1;
  dl_dst->dl_tensor.shape = shape;
  dl_dst->dl_tensor.strides = nullptr;
  dl_dst->dl_tensor.byte_offset = desc.layers[0].offset[0];

  auto dst = at::fromDLPack(dl_dst.release());

  return dst;
}

VADisplay getVaDisplayFromAV(UniqueAVFrame& avFrame) {
  AVHWFramesContext* hwfc = (AVHWFramesContext*)avFrame->hw_frames_ctx->data;
  AVHWDeviceContext* hwdc = hwfc->device_ctx;
  AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwdc->hwctx;
  return vactx->display;
}

torch::Tensor XpuDeviceInterface::convertAVFrameToTensorUsingFilterGraph(
    const UniqueAVFrame& avFrame) {
  int status = av_buffersrc_write_frame(
      filterGraphContext_.sourceContext, avFrame.get());
  TORCH_CHECK(
      status >= AVSUCCESS, "Failed to add frame to buffer source context");

  UniqueAVFrame filteredAVFrame(av_frame_alloc());
  status = av_buffersink_get_frame(
      filterGraphContext_.sinkContext, filteredAVFrame.get());
  printf(">>> DVROGOZH: aaa: 1\n");
  TORCH_CHECK_EQ(filteredAVFrame->format, AV_PIX_FMT_VAAPI);
  printf(">>> DVROGOZH: aaa: 2\n");

  AVFrame* filteredAVFramePtr = filteredAVFrame.release();
  return AVFrameToTensor(device_, filteredAVFramePtr);
}

void XpuDeviceInterface::convertAVFrameToFrameOutput(
    const VideoStreamOptions& videoStreamOptions,
    [[maybe_unused]] const AVRational& timeBase,
    UniqueAVFrame& avFrame,
    FrameOutput& frameOutput,
    std::optional<torch::Tensor> preAllocatedOutputTensor) {
  // TODO: consider to copy handling of CPU frame from CUDA
  // TODO: consider to copy NV12 format check from CUDA
  TORCH_CHECK(
      avFrame->format == AV_PIX_FMT_VAAPI,
      "Expected format to be AV_PIX_FMT_VAAPI, got " +
          std::string(av_get_pix_fmt_name((AVPixelFormat)avFrame->format)));
  auto frameDims =
      getHeightAndWidthFromOptionsOrAVFrame(videoStreamOptions, avFrame);
  int height = frameDims.height;
  int width = frameDims.width;
  torch::Tensor& dst = frameOutput.data;
  if (preAllocatedOutputTensor.has_value()) {
    dst = preAllocatedOutputTensor.value();
    auto shape = dst.sizes();
    TORCH_CHECK(
        (shape.size() == 3) && (shape[0] == height) && (shape[1] == width) &&
            (shape[2] == 3),
        "Expected tensor of shape ",
        height,
        "x",
        width,
        "x3, got ",
        shape);
  } else {
    dst = allocateEmptyHWCTensor(height, width, device_);
  }

  auto start = std::chrono::high_resolution_clock::now();
  // We need to compare the current frame context with our previous frame
  // context. If they are different, then we need to re-create our colorspace
  // conversion objects. We create our colorspace conversion objects late so
  // that we don't have to depend on the unreliable metadata in the header.
  // And we sometimes re-create them because it's possible for frame
  // resolution to change mid-stream. Finally, we want to reuse the colorspace
  // conversion objects as much as possible for performance reasons.
  enum AVPixelFormat frameFormat =
      static_cast<enum AVPixelFormat>(avFrame->format);
  //AVHWFramesContext* hwfc = (AVHWFramesContext*)avFrame->hw_frames_ctx->data;
  auto frameContext = DecodedFrameContext{
      avFrame->width,
      avFrame->height,
      frameFormat,
      avFrame->sample_aspect_ratio,
      width,
      height,
      avFrame->hw_frames_ctx};
 
  if (!filterGraphContext_.filterGraph || prevFrameContext_ != frameContext) {
      createFilterGraph(frameContext, videoStreamOptions, timeBase);
      prevFrameContext_ = frameContext;
  }
    
  // We convert input to the RGBX color format with VAAPI getting WxHx4
  // tensor on the output.
  torch::Tensor dst_rgb4 = convertAVFrameToTensorUsingFilterGraph(avFrame);
  dst.copy_(dst_rgb4.narrow(2, 0, 3));

  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::micro> duration = end - start;
  VLOG(9) << "NPP Conversion of frame height=" << height << " width=" << width
          << " took: " << duration.count() << "us" << std::endl;
}

void XpuDeviceInterface::createFilterGraph(
    const DecodedFrameContext& frameContext,
    [[maybe_unused]] const VideoStreamOptions& videoStreamOptions,
    const AVRational& timeBase) {
  filterGraphContext_.filterGraph.reset(avfilter_graph_alloc());
  TORCH_CHECK(filterGraphContext_.filterGraph.get() != nullptr);

  /*if (videoStreamOptions.ffmpegThreadCount.has_value()) {
    filterGraphContext_.filterGraph->nb_threads =
        videoStreamOptions.ffmpegThreadCount.value();
  }*/

  /*int status = avfilter_graph_set_hw_device(filterGraphContext_.filterGraph, ctx_);
  TORCH_CHECK(
      status >= 0,
      "Failed to set hw device for filter graph: ",
      getFFMPEGErrorStringFromErrorCode(status));  */

  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");

  std::stringstream filterArgs;
  filterArgs << "video_size=" << frameContext.decodedWidth << "x"
             << frameContext.decodedHeight;
  filterArgs << ":pix_fmt=" << frameContext.decodedFormat;
  filterArgs << ":time_base=" << timeBase.num << "/" << timeBase.den;
  filterArgs << ":pixel_aspect=" << frameContext.decodedAspectRatio.num << "/"
             << frameContext.decodedAspectRatio.den;

  printf(">>> %s, AV_PIX_FMT_VAAPI=%d\n", filterArgs.str().c_str(), AV_PIX_FMT_VAAPI);

  int status = avfilter_graph_create_filter(
      &filterGraphContext_.sourceContext,
      buffersrc,
      "in",
      filterArgs.str().c_str(),
      nullptr,
      filterGraphContext_.filterGraph.get());
  TORCH_CHECK(
      status >= 0,
      "Failed to create filter graph: ",
      filterArgs.str(),
      ": ",
      getFFMPEGErrorStringFromErrorCode(status));

  printf(">>> &filterGraphContext_.sourceContext=%p\n",
	(void*)filterGraphContext_.sourceContext);
  //printf(">>> &filterGraphContext_.sourceContext=%p\n",
  //      filterGraphContext_.sourceContext);

  AVBufferSrcParameters* params = av_buffersrc_parameters_alloc();
  params->format = frameContext.decodedFormat;
  params->width = frameContext.decodedWidth;
  params->height = frameContext.decodedHeight;
  params->sample_aspect_ratio = frameContext.decodedAspectRatio;
  params->time_base = timeBase;
  params->hw_frames_ctx = av_buffer_ref(frameContext.hw_frames_ctx);  //av_buffer_ref(ctx_);
  status = av_buffersrc_parameters_set(filterGraphContext_.sourceContext, params);
  //auto hw_ctx = av_buffer_ref(ctx_);
  //status = av_opt_set_bin(filterGraphContext_.sourceContext, "hw_device_ctx", (uint8_t*)&hw_ctx, sizeof(hw_ctx), AV_OPT_SEARCH_CHILDREN);
   TORCH_CHECK(
      status >= 0, "failed av_buffersrc_parameters_set");

  status = avfilter_graph_create_filter(
      &filterGraphContext_.sinkContext,
      buffersink,
      "out",
      nullptr,
      nullptr,
      filterGraphContext_.filterGraph.get());
  TORCH_CHECK(
      status >= 0,
      "Failed to create filter graph: ",
      getFFMPEGErrorStringFromErrorCode(status));

  //enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGB24, AV_PIX_FMT_NONE};
  enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_VAAPI, AV_PIX_FMT_NONE};

  status = av_opt_set_int_list(
      filterGraphContext_.sinkContext,
      "pix_fmts",
      pix_fmts,
      AV_PIX_FMT_NONE,
      AV_OPT_SEARCH_CHILDREN);
  TORCH_CHECK(
      status >= 0,
      "Failed to set output pixel formats: ",
      getFFMPEGErrorStringFromErrorCode(status));

  UniqueAVFilterInOut outputs(avfilter_inout_alloc());
  UniqueAVFilterInOut inputs(avfilter_inout_alloc());

  //filterGraphContext_.sourceContext->hw_device_ctx = ctx_;
  //filterGraphContext_.sinkContext->hw_device_ctx = ctx_;

  outputs->name = av_strdup("in");
  outputs->filter_ctx = filterGraphContext_.sourceContext;
  outputs->pad_idx = 0;
  outputs->next = nullptr;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = filterGraphContext_.sinkContext;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  std::stringstream description;
  description << "scale_vaapi=" << frameContext.expectedWidth << ":"
              << frameContext.expectedHeight;
  description << ":format=rgba"; //:out_color_matrix=bt709:out_range=tv";

  AVFilterInOut* outputsTmp = outputs.release();
  AVFilterInOut* inputsTmp = inputs.release();
  status = avfilter_graph_parse_ptr(
      filterGraphContext_.filterGraph.get(),
      description.str().c_str(),
      &inputsTmp,
      &outputsTmp,
      nullptr);
  outputs.reset(outputsTmp);
  inputs.reset(inputsTmp);
  TORCH_CHECK(
      status >= 0,
      "Failed to parse filter description: ",
      getFFMPEGErrorStringFromErrorCode(status));

  status =
      avfilter_graph_config(filterGraphContext_.filterGraph.get(), nullptr);
  TORCH_CHECK(
      status >= 0,
      "Failed to configure filter graph: ",
      getFFMPEGErrorStringFromErrorCode(status));
}

// inspired by https://github.com/FFmpeg/FFmpeg/commit/ad67ea9
// we have to do this because of an FFmpeg bug where hardware decoding is not
// appropriately set, so we just go off and find the matching codec for the CUDA
// device
std::optional<const AVCodec*> XpuDeviceInterface::findCodec(
    const AVCodecID& codecId) {
  void* i = nullptr;
  const AVCodec* codec = nullptr;
  while ((codec = av_codec_iterate(&i)) != nullptr) {
    if (codec->id != codecId || !av_codec_is_decoder(codec)) {
      continue;
    }

    const AVCodecHWConfig* config = nullptr;
    for (int j = 0; (config = avcodec_get_hw_config(codec, j)) != nullptr;
         ++j) {
      if (config->device_type == AV_HWDEVICE_TYPE_VAAPI) {
        return codec;
      }
    }
  }

  return std::nullopt;
}

} // namespace facebook::torchcodec
