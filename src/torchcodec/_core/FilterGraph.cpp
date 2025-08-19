// Copyright (c) Meta Platforms, Inc. and affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include "src/torchcodec/_core/FilterGraph.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace facebook::torchcodec {

bool FiltersContext::operator==(
    const FiltersContext& other) {
  return decodedWidth == other.decodedWidth &&
      decodedHeight == other.decodedHeight &&
      decodedFormat == other.decodedFormat &&
      filters == other.filters &&
      expectedFormat == other.expectedFormat &&
      timeBase == other.timeBase &&
      hwFramesCtx.get() == other.hwFramesCtx.get();
}

bool FiltersContext::operator!=(
    const FiltersContext& other) {
  return !(*this == other);
}

FilterGraph::FilterGraph(
    const FiltersContext& filtersContext,
    const VideoStreamOptions& videoStreamOptions) {
  filterGraph_.reset(avfilter_graph_alloc());
  TORCH_CHECK(filterGraph_.get() != nullptr);

  if (videoStreamOptions.ffmpegThreadCount.has_value()) {
    filterGraph_->nb_threads =
        videoStreamOptions.ffmpegThreadCount.value();
  }

  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");

  std::stringstream filterArgs;
  filterArgs << "video_size=" << filtersContext.decodedWidth << "x"
             << filtersContext.decodedHeight;
  filterArgs << ":pix_fmt=" << filtersContext.decodedFormat;
  filterArgs << ":time_base=" << filtersContext.timeBase.num << "/"
	     << filtersContext.timeBase.den;
  filterArgs << ":pixel_aspect=" << filtersContext.decodedAspectRatio.num << "/"
             << filtersContext.decodedAspectRatio.den;

  int status = avfilter_graph_create_filter(
      &sourceContext_,
      buffersrc,
      "in",
      filterArgs.str().c_str(),
      nullptr,
      filterGraph_.get());
  TORCH_CHECK(
      status >= 0,
      "Failed to create filter graph: ",
      filterArgs.str(),
      ": ",
      getFFMPEGErrorStringFromErrorCode(status));

  if (hwFramesCtx) {
    AVBufferSrcParameters* params = av_buffersrc_parameters_alloc();
    params->format = filtersContext.decodedFormat;
    params->width = filtersContext.decodedWidth;
    params->height = filtersContext.decodedHeight;
    params->sample_aspect_ratio = filtersContext.decodedAspectRatio;
    params->time_base = filtersContext.timeBase;
    params->hw_frames_ctx = av_buffer_ref(filtersContext.hwFramesCtx);
    status = av_buffersrc_parameters_set(filterGraphContext_.sourceContext, params);
    //auto hw_ctx = av_buffer_ref(ctx_);
    //status = av_opt_set_bin(filterGraphContext_.sourceContext, "hw_device_ctx", (uint8_t*)&hw_ctx, sizeof(hw_ctx), AV_OPT_SEARCH_CHILDREN);
    TORCH_CHECK(
        status >= 0, "failed av_buffersrc_parameters_set");
  }

  status = avfilter_graph_create_filter(
      &sinkContext_,
      buffersink,
      "out",
      nullptr,
      nullptr,
      filterGraph_.get());
  TORCH_CHECK(
      status >= 0,
      "Failed to create filter graph: ",
      getFFMPEGErrorStringFromErrorCode(status));

  enum AVPixelFormat pix_fmts[] = {filtersContext.expectedFormat, AV_PIX_FMT_NONE};

  status = av_opt_set_int_list(
      sinkContext_,
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

  outputs->name = av_strdup("in");
  outputs->filter_ctx = sourceContext_;
  outputs->pad_idx = 0;
  outputs->next = nullptr;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = sinkContext_;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  AVFilterInOut* outputsTmp = outputs.release();
  AVFilterInOut* inputsTmp = inputs.release();
  status = avfilter_graph_parse_ptr(
      filterGraph_.get(),
      filtersContext.c_str(),
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
      avfilter_graph_config(filterGraph_.get(), nullptr);
  TORCH_CHECK(
      status >= 0,
      "Failed to configure filter graph: ",
      getFFMPEGErrorStringFromErrorCode(status));
}

UniqueAVFrame FilterGraph::convert(const UniqueAVFrame& avFrame) {
  int status = av_buffersrc_write_frame(sourceContext_, avFrame.get());
  TORCH_CHECK(
      status >= AVSUCCESS, "Failed to add frame to buffer source context");

  UniqueAVFrame filteredAVFrame(av_frame_alloc());
  status = av_buffersink_get_frame(
      sinkContext_, filteredAVFrame.get());
  TORCH_CHECK(
      status >= AVSUCCESS, "Failed to get frame from buffer sink context");

  return filteredAVFrame;
}

} // namespace facebook::torchcodec
