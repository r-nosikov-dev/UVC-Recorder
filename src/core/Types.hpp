#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace uvc {

inline constexpr std::chrono::seconds kChunkDuration{3};
inline constexpr auto kChunkDurationNs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(kChunkDuration).count());

enum class RecorderState { idle, starting, recording, stopping };

enum class UploadState { idle, queued, uploading, uploaded, failed };

struct CaptureFormat {
    std::string fourcc;
    int width{0};
    int height{0};
    int fps_numerator{0};
    int fps_denominator{1};

    [[nodiscard]] double fps() const noexcept {
        return fps_denominator == 0 ? 0.0
                                    : static_cast<double>(fps_numerator) / fps_denominator;
    }

    [[nodiscard]] std::string describe() const {
        if (fps_numerator > 0) {
            return std::to_string(width) + "x" + std::to_string(height) + " " + fourcc + " @"
                   + std::to_string(static_cast<int>(fps() + 0.5)) + "fps";
        }
        return std::to_string(width) + "x" + std::to_string(height) + " " + fourcc;
    }
};

struct VideoDevice {
    std::string path;
    std::string card_name;
    std::string driver;
    CaptureFormat preferred;

    [[nodiscard]] std::string label() const {
        return card_name + " (" + path + ", " + preferred.describe() + ")";
    }
};

struct ChunkInfo {
    std::uint32_t sequence{0};
    std::filesystem::path path;
    std::string session_id;
    std::uintmax_t size_bytes{0};
    UploadState upload{UploadState::idle};
    std::string error;
};

}  // namespace uvc
