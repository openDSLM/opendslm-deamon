/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * preview_assist_stage.hpp - Common helpers for preview assist post-processing stages.
 */

#pragma once

#include <algorithm>

#include <libcamera/base/span.h>
#include <libcamera/stream.h>

#include <opencv2/core.hpp>

#include "core/stream_info.hpp"

#include "post_processing_stages/post_processing_stage.hpp"

class BufferWriteSync;

class PreviewAssistStage : public PostProcessingStage
{
public:
        explicit PreviewAssistStage(RPiCamApp *app);

        void Configure() override;
        bool Process(CompletedRequestPtr &completed_request) override;

protected:
        using Stream = libcamera::Stream;

        virtual void ConfigureAssist();
        virtual void ApplyAssist(CompletedRequestPtr &completed_request, cv::Mat &luma,
                                 BufferWriteSync &write_sync) = 0;

        Stream *stream_ = nullptr;
        StreamInfo stream_info_ {};

        libcamera::Span<uint8_t> Plane(BufferWriteSync &write_sync, unsigned int index) const;
        unsigned int PlaneStride(libcamera::Span<uint8_t> plane, unsigned int rows) const;
};

