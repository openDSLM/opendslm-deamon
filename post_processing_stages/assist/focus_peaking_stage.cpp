/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * focus_peaking_stage.cpp - Highlight in-focus edges for preview assists.
 */

#include "post_processing_stages/assist/preview_assist_stage.hpp"

#include <algorithm>

#include <boost/property_tree/ptree.hpp>

#include <opencv2/imgproc.hpp>

#include "core/buffer_sync.hpp"

namespace
{

constexpr char kStageName[] = "focus_peaking";

class FocusPeakingStage : public PreviewAssistStage
{
public:
        explicit FocusPeakingStage(RPiCamApp *app) : PreviewAssistStage(app) {}

        char const *Name() const override { return kStageName; }

        void Read(boost::property_tree::ptree const &params) override
        {
                threshold_ = params.get<int>("threshold", threshold_);
                auto_threshold_ = params.get<bool>("auto_threshold", auto_threshold_);
                auto_multiplier_ = params.get<double>("auto_multiplier", auto_multiplier_);
                highlight_value_ = params.get<int>("highlight_value", highlight_value_);
                tint_edges_ = params.get<bool>("tint_edges", tint_edges_);
                highlight_u_ = params.get<int>("highlight_u", highlight_u_);
                highlight_v_ = params.get<int>("highlight_v", highlight_v_);
                blur_size_ = params.get<int>("blur_size", blur_size_);
                dilate_iterations_ = params.get<int>("dilate_iterations", dilate_iterations_);
        }

protected:
        void ConfigureAssist() override
        {
                highlight_value_ = std::clamp(highlight_value_, 0, 255);
                highlight_u_ = std::clamp(highlight_u_, 0, 255);
                highlight_v_ = std::clamp(highlight_v_, 0, 255);
                threshold_ = std::clamp(threshold_, 0, 255);

                if (blur_size_ < 1)
                        blur_size_ = 1;
                if (blur_size_ % 2 == 0)
                        blur_size_ += 1;

                if (dilate_iterations_ < 0)
                        dilate_iterations_ = 0;
        }

        void ApplyAssist(CompletedRequestPtr &, cv::Mat &luma, BufferWriteSync &write_sync) override
        {
                cv::Mat working;
                if (blur_size_ > 1)
                        cv::GaussianBlur(luma, working, cv::Size(blur_size_, blur_size_), 0);
                else
                        working = luma;

                cv::Mat grad_x;
                cv::Mat grad_y;
                cv::Sobel(working, grad_x, CV_16S, 1, 0, 3);
                cv::Sobel(working, grad_y, CV_16S, 0, 1, 3);

                cv::Mat abs_x;
                cv::Mat abs_y;
                cv::convertScaleAbs(grad_x, abs_x);
                cv::convertScaleAbs(grad_y, abs_y);

                cv::Mat magnitude;
                cv::addWeighted(abs_x, 0.5, abs_y, 0.5, 0, magnitude);

                double threshold_value = static_cast<double>(threshold_);
                if (auto_threshold_)
                {
                        cv::Scalar mean_val = cv::mean(magnitude);
                        threshold_value = mean_val[0] * auto_multiplier_;
                        threshold_value = std::clamp(threshold_value, 0.0, 255.0);
                }

                cv::Mat mask;
                cv::threshold(magnitude, mask, threshold_value, 255, cv::THRESH_BINARY);

                if (dilate_iterations_ > 0)
                        cv::dilate(mask, mask, cv::Mat(), cv::Point(-1, -1), dilate_iterations_);

                luma.setTo(highlight_value_, mask);

                if (!tint_edges_)
                        return;

                auto u_plane = Plane(write_sync, 1);
                auto v_plane = Plane(write_sync, 2);
                if (u_plane.empty() || v_plane.empty())
                        return;

                unsigned int chroma_height = stream_info_.height / 2;
                unsigned int chroma_width = stream_info_.width / 2;
                if (!chroma_height || !chroma_width)
                        return;

                unsigned int u_stride = PlaneStride(u_plane, chroma_height);
                unsigned int v_stride = PlaneStride(v_plane, chroma_height);

                uint8_t *u_data = static_cast<uint8_t *>(u_plane.data());
                uint8_t *v_data = static_cast<uint8_t *>(v_plane.data());

                for (unsigned int y = 0; y < chroma_height; ++y)
                {
                        uint8_t *u_row = u_data + y * u_stride;
                        uint8_t *v_row = v_data + y * v_stride;

                        unsigned int src_y0 = std::min(y * 2, stream_info_.height - 1);
                        unsigned int src_y1 = std::min(y * 2 + 1, stream_info_.height - 1);
                        const uint8_t *mask_row0 = mask.ptr<uint8_t>(src_y0);
                        const uint8_t *mask_row1 = mask.ptr<uint8_t>(src_y1);

                        for (unsigned int x = 0; x < chroma_width; ++x)
                        {
                                unsigned int src_x0 = std::min(x * 2, stream_info_.width - 1);
                                unsigned int src_x1 = std::min(x * 2 + 1, stream_info_.width - 1);

                                bool highlight = mask_row0[src_x0] || mask_row0[src_x1] || mask_row1[src_x0]
                                                 || mask_row1[src_x1];
                                if (highlight)
                                {
                                        u_row[x] = highlight_u_;
                                        v_row[x] = highlight_v_;
                                }
                        }
                }
        }

private:
        int threshold_ = 40;
        bool auto_threshold_ = true;
        double auto_multiplier_ = 1.8;
        int highlight_value_ = 255;
        bool tint_edges_ = true;
        int highlight_u_ = 90;
        int highlight_v_ = 240;
        int blur_size_ = 3;
        int dilate_iterations_ = 1;
};

PostProcessingStage *Create(RPiCamApp *app)
{
        return new FocusPeakingStage(app);
}

RegisterStage reg(kStageName, &Create);

} // namespace

