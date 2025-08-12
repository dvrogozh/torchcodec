// Copyright (c) Meta Platforms, Inc. and affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "src/torchcodec/_core/DeviceInterface.h"

namespace facebook::torchcodec {

class XpuDeviceInterface : public DeviceInterface {
 public:
  XpuDeviceInterface(const torch::Device& device);

  virtual ~XpuDeviceInterface();

  std::optional<const AVCodec*> findCodec(const AVCodecID& codecId) override;

  void initializeContext(AVCodecContext* codecContext) override;

  void convertAVFrameToFrameOutput(
      const VideoStreamOptions& videoStreamOptions,
      const AVRational& timeBase,
      UniqueAVFrame& avFrame,
      FrameOutput& frameOutput,
      std::optional<torch::Tensor> preAllocatedOutputTensor =
          std::nullopt) override;

 private:
   struct FilterGraphContext {
    UniqueAVFilterGraph filterGraph;
    AVFilterContext* sourceContext = nullptr;
    AVFilterContext* sinkContext = nullptr;
  };

  struct DecodedFrameContext {
    int decodedWidth;
    int decodedHeight;
    AVPixelFormat decodedFormat;
    AVRational decodedAspectRatio;
    int expectedWidth;
    int expectedHeight;
    AVBufferRef* hw_frames_ctx;
    bool operator==(const DecodedFrameContext&);
    bool operator!=(const DecodedFrameContext&);
  };

  void createFilterGraph(
      const DecodedFrameContext& frameContext,
      const VideoStreamOptions& videoStreamOptions,
      const AVRational& timeBase);

  torch::Tensor convertAVFrameToTensorUsingFilterGraph(
      const UniqueAVFrame& avFrame);

  AVBufferRef* ctx_ = nullptr;


  FilterGraphContext filterGraphContext_;

  // Used to know whether a new FilterGraphContext should
  // be created before decoding a new frame.
  DecodedFrameContext prevFrameContext_;
};

} // namespace facebook::torchcodec
