/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * rpicam_daemon.cpp - HTTP daemon backing future DSLR-style UI workflows.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <string>
#include <thread>

#include "daemon/camera_daemon.hpp"

namespace
{

std::atomic<bool> keep_running{true};

void signalHandler(int)
{
        keep_running = false;
}

struct CommandLineOptions
{
        uint16_t port = 8400;
        std::string preview_pipeline;
};

CommandLineOptions parseCommandLine(int argc, char *argv[])
{
        CommandLineOptions options;
        for (int i = 1; i < argc; ++i)
        {
                std::string arg = argv[i];
                if ((arg == "--port" || arg == "-p") && i + 1 < argc)
                {
                        int value = std::atoi(argv[++i]);
                        if (value <= 0 || value > 65535)
                                throw std::runtime_error("Port must be between 1 and 65535");
                        options.port = static_cast<uint16_t>(value);
                }
                else if (arg.rfind("--preview-gstreamer=", 0) == 0)
                {
                        options.preview_pipeline = arg.substr(std::strlen("--preview-gstreamer="));
                        if (options.preview_pipeline.empty())
                                throw std::runtime_error("--preview-gstreamer requires a non-empty pipeline");
                }
                else if (arg == "--preview-gstreamer")
                {
                        if (i + 1 >= argc)
                                throw std::runtime_error("--preview-gstreamer expects a pipeline description");
                        options.preview_pipeline = argv[++i];
                }
                else if (arg == "--help" || arg == "-h")
                {
                        std::cout << "Usage: rpicam-daemon [--port <port>] [--preview-gstreamer <pipeline>]\n";
                        std::cout << "       pipeline describes the downstream elements for an appsrc named rpicam_src." << std::endl;
                        std::exit(0);
                }
                else
                        throw std::runtime_error("Unknown argument: " + arg);
        }
        return options;
}

} // namespace

int main(int argc, char *argv[])
{
        try
        {
                CommandLineOptions cli = parseCommandLine(argc, argv);

#ifndef GSTREAMER_PRESENT
                if (!cli.preview_pipeline.empty())
                        throw std::runtime_error("GStreamer preview requested but binary was built without GStreamer support");
#endif

                rpicam::CameraDaemon daemon;
                daemon.setPreviewPipeline(cli.preview_pipeline);
                daemon.start(cli.port);

                std::signal(SIGINT, signalHandler);
                std::signal(SIGTERM, signalHandler);

                std::cout << "rpicam-daemon listening on port " << cli.port << std::endl;
                if (!cli.preview_pipeline.empty())
                        std::cout << "Preview pipeline: " << cli.preview_pipeline << std::endl;
                std::cout << "Press Ctrl+C to stop." << std::endl;

                while (keep_running)
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));

                daemon.stop();
        }
        catch (std::exception const &ex)
        {
                std::cerr << "ERROR: " << ex.what() << std::endl;
                return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
}

