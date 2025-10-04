/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * preview_assist_stage.cpp - Common helpers for preview assist post-processing stages.
 */

#include "post_processing_stages/assist/preview_assist_stage.hpp"

#include <stdexcept>

#include <libcamera/formats.h>

#include "core/buffer_sync.hpp"
#include "core/rpicam_app.hpp"

PreviewAssistStage::PreviewAssistStage(RPiCamApp *app) : PostProcessingStage(app) {}

void PreviewAssistStage::Configure()
{
        stream_ = app_->GetMainStream();
        if (!stream_ || stream_->configuration().pixelFormat != libcamera::formats::YUV420)
                throw std::runtime_error("PreviewAssistStage: only YUV420 format supported");

        stream_info_ = app_->GetStreamInfo(stream_);

        ConfigureAssist();
}

void PreviewAssistStage::ConfigureAssist() {}

bool PreviewAssistStage::Process(CompletedRequestPtr &completed_request)
{
        BufferWriteSync write_sync(app_, completed_request->buffers[stream_]);
        auto const &planes = write_sync.Get();
        if (planes.empty())
                return false;

        uint8_t *ptr = static_cast<uint8_t *>(planes[0].data());
        cv::Mat luma(stream_info_.height, stream_info_.width, CV_8UC1, ptr, stream_info_.stride);

        ApplyAssist(completed_request, luma, write_sync);

        return false;
}

libcamera::Span<uint8_t> PreviewAssistStage::Plane(BufferWriteSync &write_sync, unsigned int index) const
{
        auto const &planes = write_sync.Get();
        if (index >= planes.size())
                return {};
        return planes[index];
}

unsigned int PreviewAssistStage::PlaneStride(libcamera::Span<uint8_t> plane, unsigned int rows) const
{
        if (!rows)
                return 0;
        return static_cast<unsigned int>(plane.size() / rows);
}

