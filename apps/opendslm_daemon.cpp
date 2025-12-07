/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * opendslm_daemon.cpp - HTTP daemon backing future DSLR-style UI workflows.
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

constexpr const char *kDefaultShmSocket = "/tmp/opendslm-preview.sock";

std::string gstQuote(const std::string &value)
{
        std::string quoted;
        quoted.reserve(value.size() + 2);
        quoted.push_back('"');
        for (char c : value)
        {
                if (c == '\\' || c == '"')
                        quoted.push_back('\\');
                quoted.push_back(c);
        }
        quoted.push_back('"');
        return quoted;
}

std::string buildShmPreviewPipeline(const std::string &socket_path)
{
        std::string quoted = gstQuote(socket_path);
        const char *extra_queue_env = std::getenv("ODS_PREVIEW_EXTRA_QUEUE");
        bool add_extra_queue = true;
        if (extra_queue_env && (std::strcmp(extra_queue_env, "0") == 0 || std::strcmp(extra_queue_env, "false") == 0))
                add_extra_queue = false;

        std::string pipeline = "queue max-size-buffers=2 leaky=downstream ! videoconvert ! ";
        if (add_extra_queue)
                pipeline += "queue max-size-buffers=4 leaky=downstream ! ";

        pipeline += "video/x-raw,format=RGBA ! shmsink wait-for-connection=false sync=false socket-path=";
        pipeline += quoted;
        return pipeline;
}

std::string buildShmClientPipeline(const std::string &socket_path)
{
        std::string quoted = gstQuote(socket_path);
        return "shmsrc socket-path=" + quoted
               + " is-live=true do-timestamp=true ! queue max-size-buffers=2 leaky=downstream ! "
                 "video/x-raw,format=RGBA ! gtk4paintablesink";
}

struct CommandLineOptions
{
        uint16_t port = 8400;
        std::string preview_pipeline;
        std::string preview_client_pipeline;
        std::string shm_socket = kDefaultShmSocket;
        bool preview_enabled = true;
        bool preview_pipeline_explicit = false;
        bool preview_client_pipeline_explicit = false;
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
                else if (arg.rfind("--preview-gstreamer-client=", 0) == 0)
                {
                        options.preview_client_pipeline = arg.substr(std::strlen("--preview-gstreamer-client="));
                        options.preview_client_pipeline_explicit = true;
                }
                else if (arg == "--preview-gstreamer-client")
                {
                        if (i + 1 >= argc)
                                throw std::runtime_error("--preview-gstreamer-client expects a pipeline description");
                        options.preview_client_pipeline = argv[++i];
                        options.preview_client_pipeline_explicit = true;
                }
                else if (arg.rfind("--preview-gstreamer-socket=", 0) == 0)
                {
                        options.shm_socket = arg.substr(std::strlen("--preview-gstreamer-socket="));
                        if (options.shm_socket.empty())
                                throw std::runtime_error("--preview-gstreamer-socket requires a non-empty path");
                }
                else if (arg == "--preview-gstreamer-socket")
                {
                        if (i + 1 >= argc)
                                throw std::runtime_error("--preview-gstreamer-socket expects a socket path");
                        options.shm_socket = argv[++i];
                        if (options.shm_socket.empty())
                                throw std::runtime_error("--preview-gstreamer-socket requires a non-empty path");
                }
                else if (arg.rfind("--preview-gstreamer=", 0) == 0)
                {
                        options.preview_pipeline = arg.substr(std::strlen("--preview-gstreamer="));
                        if (options.preview_pipeline == "none" || options.preview_pipeline == "off")
                        {
                                options.preview_enabled = false;
                                options.preview_pipeline.clear();
                        }
                        else if (options.preview_pipeline.empty())
                                throw std::runtime_error("--preview-gstreamer requires a non-empty pipeline");
                        options.preview_pipeline_explicit = true;
                }
                else if (arg == "--preview-gstreamer")
                {
                        if (i + 1 >= argc)
                                throw std::runtime_error("--preview-gstreamer expects a pipeline description");
                        options.preview_pipeline = argv[++i];
                        if (options.preview_pipeline == "none" || options.preview_pipeline == "off")
                        {
                                options.preview_enabled = false;
                                options.preview_pipeline.clear();
                        }
                        else if (options.preview_pipeline.empty())
                                throw std::runtime_error("--preview-gstreamer requires a non-empty pipeline");
                        options.preview_pipeline_explicit = true;
                }
                else if (arg == "--no-preview-gstreamer")
                {
                        options.preview_enabled = false;
                        options.preview_pipeline.clear();
                        options.preview_pipeline_explicit = true;
                }
                else if (arg == "--help" || arg == "-h")
                {
                        std::cout << "Usage: opendslm-daemon [--port <port>] [preview options]\n";
                        std::cout << "Preview options:\n";
                        std::cout << "  --preview-gstreamer <pipeline>        Use a custom GStreamer pipeline (downstream of appsrc).\n";
                        std::cout << "  --preview-gstreamer=none|off          Disable the GStreamer preview backend.\n";
                        std::cout << "  --preview-gstreamer-client <pipeline> Advertise a client-side pipeline in /status.\n";
                        std::cout << "  --preview-gstreamer-socket <path>     Change the shared-memory socket path (default: "
                                  << kDefaultShmSocket << ").\n";
                        std::cout << "  --no-preview-gstreamer                Disable the GStreamer preview backend." << std::endl;
                        std::cout << "When no custom pipeline is supplied the daemon publishes preview frames through shmsink\n"
                                     "and advertises a matching shmsrc client pipeline." << std::endl;
                        std::exit(0);
                }
                else
                        throw std::runtime_error("Unknown argument: " + arg);
        }

        if (!options.preview_pipeline_explicit)
        {
                if (options.preview_enabled)
                {
                        options.preview_pipeline = buildShmPreviewPipeline(options.shm_socket);
                        if (options.preview_client_pipeline.empty())
                                options.preview_client_pipeline = buildShmClientPipeline(options.shm_socket);
                }
                else
                {
                        options.preview_pipeline.clear();
                        options.preview_client_pipeline.clear();
                }
        }
        else
        {
                if (!options.preview_enabled)
                        options.preview_client_pipeline.clear();
                else if (options.preview_client_pipeline.empty()
                         && options.preview_pipeline.find("shmsink") != std::string::npos)
                        options.preview_client_pipeline = buildShmClientPipeline(options.shm_socket);
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
                daemon.setPreviewClientPipeline(cli.preview_client_pipeline, cli.preview_client_pipeline_explicit);
                daemon.setShmSocket(cli.shm_socket);
                daemon.start(cli.port);

                std::signal(SIGINT, signalHandler);
                std::signal(SIGTERM, signalHandler);

                std::cout << "opendslm-daemon listening on port " << cli.port << std::endl;
                if (!cli.preview_pipeline.empty())
                        std::cout << "Preview pipeline: " << cli.preview_pipeline << std::endl;
                if (!cli.preview_client_pipeline.empty())
                        std::cout << "Client preview pipeline: " << cli.preview_client_pipeline << std::endl;
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
