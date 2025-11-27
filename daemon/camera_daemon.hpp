/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * camera_daemon.hpp - High level controller for the rpicam daemon.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/video_options.hpp"
#include "daemon/json_utils.hpp"
#include "daemon/simple_http.hpp"
#include "image/image.hpp"

class DngOutput; // forward declaration

namespace rpicam
{

struct MetadataSettings
{
        std::string make;
        std::string model;
        std::string unique_model;
        std::string software;
        std::string artist;
        std::string copyright;
};

struct CameraSettings
{
        double fps = 24.0;
        double shutter_us = 0.0;
        double analogue_gain = 1.0;
        bool auto_exposure = true;
        std::string output_dir = "/ssd/RAW";
        std::string mode;
        MetadataSettings metadata;
};

enum class SessionMode
{
        None,
        Still,
        Video
};

struct SessionState
{
        SessionMode mode = SessionMode::None;
        bool active = false;
        std::string last_error;
};

struct CaptureSummary
{
        std::string type;
        std::vector<std::string> frames;
        std::string directory;
};

struct CameraModeInfo
{
        unsigned int width = 0;
        unsigned int height = 0;
        std::string format;
        unsigned int bit_depth = 0;
        double max_fps = 0.0;
};

struct CameraProbeInfo
{
        std::string id;
        std::string model;
        std::string location;
        std::vector<CameraModeInfo> modes;
};

struct HardwareInfo
{
        std::string board_model;
        std::string board_revision;
        std::string os_name;
        std::string kernel;
        std::vector<CameraProbeInfo> cameras;
        std::string error;
};

class CameraDaemon
{
public:
        CameraDaemon();

        void start(uint16_t port);
        void stop();

        SessionState getState() const;
        CameraSettings getSettings() const;
        std::string getLastCameraModel() const;

        bool updateSettings(JsonObject const &values, std::string &error_message);
        bool updateMetadata(JsonObject const &values, std::string &error_message);
        void setPreviewPipeline(const std::string &pipeline);
        void setPreviewClientPipeline(const std::string &pipeline, bool explicit_value);
        std::string previewPipeline() const;
        std::string previewClientPipeline() const;
        bool previewClientPipelineExplicit() const;
        void setShmSocket(const std::string &socket);
        std::string shmSocket() const;
        bool startSession(SessionMode mode, const std::string &video_path, std::string &error_message);
        bool stopSession(std::string &error_message);

private:
        struct CaptureResult
        {
                bool success = false;
                std::vector<std::string> frames;
                std::string error;
        };

        std::string buildStatusJson() const;
        std::string buildSettingsJson(CameraSettings const &settings, std::string const &camera_model) const;
        std::string buildMetadataJson(CameraSettings const &settings, std::string const &camera_model) const;
        static std::string modeToString(SessionMode mode);
        static std::string buildCaptureJson(CaptureSummary const &capture);
        std::string buildHardwareJson() const;
        std::string buildHardwareJsonLocked(HardwareInfo const &info) const;

        CaptureResult runCineDngCapture(CameraSettings const &settings, bool single_shot,
                                        std::atomic<bool> *stop_flag);
        bool capturePreviewSnapshot(CameraSettings const &settings, std::vector<uint8_t> &jpeg,
                                    std::string &error);
        static bool ensureOutputDirectory(std::string const &path, std::string &error_message);
        static void applySettingsToOptions(CameraSettings const &settings, VideoOptions &options,
                                           bool request_raw);
        static std::string makeCaptureDirectory(const std::string &base, const std::string &prefix,
                                                std::string &error_message);
        bool applyMetadataPatch(JsonObject const &values, MetadataSettings &target, std::string &error_message,
                                bool &any) const;
        ImageMetadata resolveMetadataForSensor(std::string const &camera_model, MetadataSettings const &base,
                                               MetadataSettings const *override_settings) const;
        void probeHardwareInfo();
        void applyDetectedMetadataLocked(HardwareInfo const &info);

        void registerRoutes();
        // Unified background camera loop
        void cameraLoop();
        void startCameraLoop();
        void stopCameraLoop();
        // Per-client MJPEG streaming writer
        void streamClientLoop(int client_fd);

        mutable std::mutex mutex_;
        SimpleHttpServer server_;
        CameraSettings settings_;
        SessionState session_;
        CaptureSummary last_capture_;
        std::string last_camera_model_;
        HardwareInfo hardware_info_;
        std::string preview_pipeline_;
        std::string preview_client_pipeline_;
        bool preview_client_pipeline_explicit_ = false;
        std::string shm_socket_;

        // Background camera thread and control flags
        std::thread camera_thread_;
        std::atomic<bool> camera_stop_{false};
        std::atomic<bool> camera_reconfigure_{false};
        std::mutex camera_guard_;

        // Latest preview frame (JPEG)
        std::mutex preview_mutex_;
        std::condition_variable preview_cv_;
        std::vector<uint8_t> latest_jpeg_;
        uint64_t preview_seq_ = 0;
        std::atomic<bool> preview_enabled_{false}; // generate preview frames when true
        std::atomic<int> preview_clients_{0};

        // Capture state shared with camera loop
        std::mutex capture_mutex_;
        std::shared_ptr<DngOutput> active_output_;
        bool video_recording_ = false;
        bool still_pending_ = false;
        std::vector<std::string> still_result_;
        std::condition_variable still_cv_;
        std::string video_sequence_path_;
        MetadataSettings still_metadata_override_;
        bool still_metadata_override_pending_ = false;
};

} // namespace rpicam
