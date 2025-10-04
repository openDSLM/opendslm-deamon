/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2024
 *
 * dng_output.cpp - Write raw frames as a CinemaDNG sequence.
 */

#include "dng_output.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

#include <libcamera/base/span.h>

#include "core/logging.hpp"
#include "image/image.hpp"

namespace
{

std::string derive_pattern(const std::string &output)
{
        if (output.empty())
                return "frame-%08u.dng";

        if (output.find('%') != std::string::npos)
                return output;

        struct stat st = {};
        if (stat(output.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        {
                std::string sep = output.back() == '/' ? "" : "/";
                return output + sep + "frame-%08u.dng";
        }

        auto dot = output.rfind('.');
        if (dot != std::string::npos)
                return output.substr(0, dot) + "-%08u" + output.substr(dot);

        return output + "-%08u.dng";
}

} // namespace

DngOutput::DngOutput(VideoOptions const *options, StreamInfo const &info, std::string camera_model)
        : Output(options), info_(info), camera_model_(std::move(camera_model)),
          filename_pattern_(derive_pattern(options->output))
{
        if (options->output == "-")
                throw std::runtime_error("CinemaDNG output does not support writing to stdout");

        initialiseFrameIndex();
}

void DngOutput::MetadataReady(libcamera::ControlList &metadata)
{
        {
                std::lock_guard<std::mutex> lock(metadata_mutex_);
                metadata_queue_.push(metadata);
        }
        metadata_cv_.notify_one();

        Output::MetadataReady(metadata);
}

libcamera::ControlList DngOutput::waitForMetadata()
{
        std::unique_lock<std::mutex> lock(metadata_mutex_);
        metadata_cv_.wait(lock, [this]() { return !metadata_queue_.empty(); });
        libcamera::ControlList metadata = metadata_queue_.front();
        metadata_queue_.pop();
        return metadata;
}

std::string DngOutput::nextFilename()
{
        std::lock_guard<std::mutex> lock(file_mutex_);
        std::string filename = composeFilename(frame_index_);

        frame_index_++;
        if (options_->wrap)
                frame_index_ = frame_index_ % options_->wrap;

        return filename;
}

std::string DngOutput::composeFilename(unsigned int index) const
{
        std::array<char, 512> filename {};
        int n = snprintf(filename.data(), filename.size(), filename_pattern_.c_str(), index);
        if (n < 0 || n >= static_cast<int>(filename.size()))
                throw std::runtime_error("failed to compose CinemaDNG filename");

        return std::string(filename.data(), n);
}

void DngOutput::initialiseFrameIndex()
{
        std::filesystem::path sample_path(composeFilename(0));
        std::filesystem::path directory = sample_path.has_parent_path() ? sample_path.parent_path()
                                                                       : std::filesystem::path(".");

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || ec)
                return;
        if (!std::filesystem::is_directory(directory, ec) || ec)
                return;

        std::filesystem::path pattern_path(filename_pattern_);
        std::string file_pattern = pattern_path.filename().string();

        bool parsed_pattern = false;
        std::string prefix;
        std::string suffix;
        char conversion = '\0';

        size_t percent = file_pattern.find('%');
        if (percent != std::string::npos)
        {
                size_t conv_pos = file_pattern.find_first_of("diuoxX", percent);
                if (conv_pos != std::string::npos)
                {
                        conversion = file_pattern[conv_pos];
                        ++conv_pos;
                        prefix = file_pattern.substr(0, percent);
                        suffix = file_pattern.substr(conv_pos);
                        parsed_pattern = true;
                }
        }

        unsigned int max_index = 0;
        bool found = false;

        if (parsed_pattern)
        {
                int base = 10;
                if (conversion == 'x' || conversion == 'X')
                        base = 16;
                else if (conversion == 'o')
                        base = 8;

                auto is_valid_digit = [base](char ch) {
                        unsigned char uch = static_cast<unsigned char>(ch);
                        if (base == 10)
                                return std::isdigit(uch) != 0;
                        if (base == 8)
                                return ch >= '0' && ch <= '7';
                        if (base == 16)
                                return std::isxdigit(uch) != 0;
                        return false;
                };

                std::filesystem::directory_iterator it(directory, ec);
                std::filesystem::directory_iterator end;
                for (; !ec && it != end; it.increment(ec))
                {
                        std::error_code file_ec;
                        if (!it->is_regular_file(file_ec) || file_ec)
                                continue;

                        std::string candidate = it->path().filename().string();
                        if (candidate.size() < prefix.size() + suffix.size())
                                continue;
                        if (candidate.compare(0, prefix.size(), prefix) != 0)
                                continue;
                        if (!suffix.empty() &&
                            candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) != 0)
                                continue;

                        std::string number = candidate.substr(prefix.size(),
                                                              candidate.size() - prefix.size() - suffix.size());
                        if (number.empty())
                                continue;
                        if (!std::all_of(number.begin(), number.end(), is_valid_digit))
                                continue;

                        unsigned long long value = 0;
                        try
                        {
                                value = std::stoull(number, nullptr, base);
                        }
                        catch (std::exception const &)
                        {
                                continue;
                        }

                        if (value > std::numeric_limits<unsigned int>::max())
                                continue;

                        if (!found || value > max_index)
                        {
                                max_index = static_cast<unsigned int>(value);
                                found = true;
                        }
                }
        }

        if (found)
        {
                if (max_index == std::numeric_limits<unsigned int>::max())
                        frame_index_ = max_index;
                else
                        frame_index_ = max_index + 1;
                return;
        }

        std::error_code exists_ec;
        unsigned int candidate = 0;
        while (candidate < std::numeric_limits<unsigned int>::max())
        {
                exists_ec.clear();
                std::filesystem::path probe(composeFilename(candidate));
                if (!std::filesystem::exists(probe, exists_ec) || exists_ec)
                        break;
                ++candidate;
        }
        frame_index_ = candidate;
}

void DngOutput::outputBuffer(void *mem, size_t size, int64_t, uint32_t)
{
        std::vector<libcamera::Span<uint8_t>> spans;
        spans.emplace_back(static_cast<uint8_t *>(mem), size);

        libcamera::ControlList metadata = waitForMetadata();
        std::string filename = nextFilename();

        LOG(2, "Writing CinemaDNG frame to " << filename);
        dng_save(spans, info_, metadata, filename, camera_model_, nullptr);

        if (frame_written_callback_)
                frame_written_callback_(filename);
}
