#ifdef GSTREAMER_PRESENT
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * gstreamer_preview.cpp - Feed preview frames into a GStreamer pipeline.
 */

#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <glib.h>

#include "core/logging.hpp"
#include "core/options.hpp"

#include "preview.hpp"

namespace
{

constexpr float kDefaultPreviewFps = 30.0f;

void start_glib_pump()
{
        static std::once_flag pump_flag;
        static std::thread pump_thread;
        std::call_once(pump_flag, [] {
                pump_thread = std::thread([] {
                        while (true)
                                g_main_context_iteration(nullptr, TRUE);
                });
                pump_thread.detach();
        });
}

class GstPreview : public Preview
{
public:
        explicit GstPreview(Options const *options);
        ~GstPreview() override;

        void Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info) override;
        void Reset() override;
        bool Quit() override { return false; }
        void MaxImageSize(unsigned int &w, unsigned int &h) const override
        {
                w = 0;
                h = 0;
        }

private:
        void configureCaps(StreamInfo const &info);
        void drainBus();
        void ensurePlaying();

        GstElement *pipeline_ = nullptr;
        GstAppSrc *appsrc_ = nullptr;
        GstBus *bus_ = nullptr;
        GstCaps *caps_ = nullptr;
        GstVideoInfo video_info_ {};
        StreamInfo current_info_ {};
        bool have_info_ = false;
        bool needs_playing_ = true;
        GstClockTime timestamp_ = 0;
        GstClockTime frame_duration_ = GST_CLOCK_TIME_NONE;
        std::string pipeline_desc_;
};

void init_gstreamer()
{
        static std::once_flag init_flag;
        std::call_once(init_flag, [] {
                GError *error = nullptr;
                if (!gst_init_check(nullptr, nullptr, &error))
                {
                        std::string message = "Failed to initialise GStreamer";
                        if (error)
                        {
                                message += ": ";
                                message += error->message ? error->message : "unknown error";
                                g_error_free(error);
                        }
                        throw std::runtime_error(message);
                }

                start_glib_pump();
        });
}

GstPreview::GstPreview(Options const *options) : Preview(options)
{
        init_gstreamer();

        if (!options_->preview_gstreamer.size())
                throw std::runtime_error("GStreamer preview requested without a pipeline description");

        std::ostringstream desc;
        desc << "appsrc name=rpicam_src is-live=true format=time do-timestamp=true ! "
             << options_->preview_gstreamer;
        pipeline_desc_ = desc.str();

        GError *error = nullptr;
        pipeline_ = gst_parse_launch(pipeline_desc_.c_str(), &error);
        if (!pipeline_)
        {
                std::string message = "Failed to create GStreamer pipeline";
                if (error)
                {
                        message += ": ";
                        message += error->message ? error->message : "unknown error";
                        g_error_free(error);
                }
                throw std::runtime_error(message);
        }

        GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline_), "rpicam_src");
        if (!src)
        {
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
                throw std::runtime_error("GStreamer pipeline is missing appsrc named 'rpicam_src'");
        }
        appsrc_ = GST_APP_SRC(src);

        bus_ = gst_element_get_bus(pipeline_);

        gst_app_src_set_stream_type(appsrc_, GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_max_bytes(appsrc_, 0); // unlimited buffer, rely on downstream for flow control
        g_object_set(G_OBJECT(appsrc_),
                     "is-live", TRUE,
                     "format", GST_FORMAT_TIME,
                     "do-timestamp", TRUE,
                     NULL);

        float fps = options_->framerate.value_or(kDefaultPreviewFps);
        if (fps <= 0.0f)
                fps = kDefaultPreviewFps;
        frame_duration_ = fps > 0.0f ? static_cast<GstClockTime>(GST_SECOND / fps) : GST_CLOCK_TIME_NONE;

        if (gst_element_set_state(pipeline_, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE)
                throw std::runtime_error("Failed to set GStreamer pipeline to READY state");
}

GstPreview::~GstPreview()
{
        if (pipeline_)
        {
                gst_app_src_end_of_stream(appsrc_);
                drainBus();
                gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (caps_)
                gst_caps_unref(caps_);
        if (bus_)
                gst_object_unref(bus_);
        if (appsrc_)
                gst_object_unref(appsrc_);
        if (pipeline_)
                gst_object_unref(pipeline_);
}

void GstPreview::configureCaps(StreamInfo const &info)
{
        if (have_info_ && info.width == current_info_.width && info.height == current_info_.height
            && info.stride == current_info_.stride)
                return;

        current_info_ = info;
        have_info_ = true;

        unsigned int stride = info.stride ? info.stride : info.width;
        if (stride % 2)
                stride += 1; // ensure chroma planes remain aligned

        gst_video_info_set_format(&video_info_, GST_VIDEO_FORMAT_I420, info.width, info.height);
        video_info_.stride[0] = stride;
        video_info_.stride[1] = stride / 2;
        video_info_.stride[2] = stride / 2;
        video_info_.offset[0] = 0;
        video_info_.offset[1] = video_info_.stride[0] * info.height;
        video_info_.offset[2] = video_info_.offset[1] + video_info_.stride[1] * (info.height / 2);
        video_info_.size = video_info_.offset[2] + video_info_.stride[2] * (info.height / 2);

        if (caps_)
        {
                gst_caps_unref(caps_);
                caps_ = nullptr;
        }
        caps_ = gst_video_info_to_caps(&video_info_);
        gst_app_src_set_caps(appsrc_, caps_);
}

void GstPreview::ensurePlaying()
{
        if (!needs_playing_)
                return;

        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
                throw std::runtime_error("Failed to start GStreamer pipeline");

        needs_playing_ = false;
        timestamp_ = 0;
}

void GstPreview::drainBus()
{
        if (!bus_)
                return;

        while (GstMessage *msg = gst_bus_pop(bus_))
        {
                switch (GST_MESSAGE_TYPE(msg))
                {
                case GST_MESSAGE_ERROR:
                {
                        GError *err = nullptr;
                        gchar *debug = nullptr;
                        gst_message_parse_error(msg, &err, &debug);
                        LOG_ERROR("GStreamer error: " << (err && err->message ? err->message : "unknown"));
                        if (debug)
                                LOG(2, "GStreamer debug info: " << debug);
                        if (err)
                                g_error_free(err);
                        g_free(debug);
                        needs_playing_ = true;
                        break;
                }
                case GST_MESSAGE_WARNING:
                {
                        GError *err = nullptr;
                        gchar *debug = nullptr;
                        gst_message_parse_warning(msg, &err, &debug);
                        LOG(1, "GStreamer warning: " << (err && err->message ? err->message : "unknown"));
                        if (debug)
                                LOG(2, "GStreamer warning debug: " << debug);
                        if (err)
                                g_error_free(err);
                        g_free(debug);
                        break;
                }
                case GST_MESSAGE_EOS:
                        LOG(1, "GStreamer pipeline signalled EOS");
                        needs_playing_ = true;
                        break;
                default:
                        break;
                }
                gst_message_unref(msg);
        }

}

void GstPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{
        if (!pipeline_ || !appsrc_)
                return;

        configureCaps(info);
        ensurePlaying();

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, span.size(), nullptr);
        if (!buffer)
                throw std::runtime_error("Failed to allocate GStreamer buffer");

        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
                gst_buffer_unref(buffer);
                throw std::runtime_error("Failed to map GStreamer buffer");
        }

        if (map.size < span.size())
        {
                gst_buffer_unmap(buffer, &map);
                gst_buffer_unref(buffer);
                throw std::runtime_error("GStreamer buffer mapping smaller than frame size");
        }

        std::memcpy(map.data, span.data(), span.size());
        gst_buffer_unmap(buffer, &map);

        if (frame_duration_ != GST_CLOCK_TIME_NONE)
        {
                GST_BUFFER_PTS(buffer) = timestamp_;
                GST_BUFFER_DTS(buffer) = timestamp_;
                GST_BUFFER_DURATION(buffer) = frame_duration_;
                timestamp_ += frame_duration_;
        }
        else
        {
                GST_BUFFER_PTS(buffer) = timestamp_;
                GST_BUFFER_DTS(buffer) = timestamp_;
        }

        GstFlowReturn flow = gst_app_src_push_buffer(appsrc_, buffer);
        if (flow != GST_FLOW_OK)
        {
                if (flow == GST_FLOW_FLUSHING)
                        LOG(1, "GStreamer pipeline flushing, dropping preview frame");
                else
                        LOG_ERROR("Failed to push preview frame to GStreamer pipeline: flow=" << flow);
                needs_playing_ = true;
        }

        drainBus();

        if (done_callback_)
                done_callback_(fd);
}

void GstPreview::Reset()
{
        if (!pipeline_ || !appsrc_)
                return;

        gst_app_src_end_of_stream(appsrc_);
        drainBus();
        gst_element_set_state(pipeline_, GST_STATE_READY);
        needs_playing_ = true;
        timestamp_ = 0;
}

} // namespace

Preview *make_gstreamer_preview(Options const *options)
{
        return new GstPreview(options);
}

#endif // GSTREAMER_PRESENT
