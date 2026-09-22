#include "DeviceEnumerator.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace uvc {
namespace {

constexpr int kTargetFps = 30;
constexpr int kMinAcceptableFps = 15;
constexpr double kFpsMatchTolerance = 0.5;
constexpr int kMaxCaptureWidth = 1280;
constexpr int kMaxCaptureHeight = 720;
constexpr std::array<int, 4> kPreferredFps{kTargetFps, 25, 24, kMinAcceptableFps};
constexpr std::array<std::string_view, 3> kPixelPrefs{"MJPG", "YUYV", "NV12"};

[[nodiscard]] std::string v4l2_fixed_string(const char* data, std::size_t max_length) {
    return std::string(data, strnlen(data, max_length));
}

[[nodiscard]] double frame_interval_to_fps(const v4l2_fract& period) {
    if (period.numerator == 0 || period.denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(period.denominator) / static_cast<double>(period.numerator);
}

[[nodiscard]] int video_node_number(const std::string& path) {
    const auto name = std::filesystem::path(path).filename().string();
    if (!name.starts_with("video")) {
        return 0;
    }
    try {
        return std::stoi(name.substr(5));
    } catch (...) {
        return 0;
    }
}

[[nodiscard]] std::string fourcc_to_string(__u32 fourcc) {
    const char chars[] = {static_cast<char>(fourcc & 0xff),
                          static_cast<char>((fourcc >> 8) & 0xff),
                          static_cast<char>((fourcc >> 16) & 0xff),
                          static_cast<char>((fourcc >> 24) & 0xff)};
    return std::string(chars, 4);
}

[[nodiscard]] bool is_capture_caps(__u32 caps) noexcept {
    return (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) != 0;
}

class Fd {
public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_{-1};
};

[[nodiscard]] std::pair<int, int> pick_fps(int fd, __u32 pixel_format, int width, int height) {
    std::vector<std::pair<int, int>> discrete;
    for (int index = 0; index < 64; ++index) {
        v4l2_frmivalenum interval{};
        interval.index = static_cast<__u32>(index);
        interval.pixel_format = pixel_format;
        interval.width = static_cast<__u32>(width);
        interval.height = static_cast<__u32>(height);
        if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) {
            break;
        }
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (interval.discrete.numerator > 0) {
                discrete.emplace_back(static_cast<int>(interval.discrete.denominator),
                                      static_cast<int>(interval.discrete.numerator));
            }
        } else if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE
                   || interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
            const auto min_fps = frame_interval_to_fps(interval.stepwise.max);
            const auto max_fps = frame_interval_to_fps(interval.stepwise.min);
            if (min_fps <= kTargetFps && kTargetFps <= max_fps) {
                return {kTargetFps, 1};
            }
            return {static_cast<int>(interval.stepwise.min.denominator),
                    static_cast<int>(interval.stepwise.min.numerator)};
        }
    }

    for (const int wanted : kPreferredFps) {
        const auto found = std::ranges::find_if(discrete, [wanted](const auto& fps) {
            if (fps.second == 0) {
                return false;
            }
            const double current = static_cast<double>(fps.first) / fps.second;
            return std::abs(current - wanted) < kFpsMatchTolerance;
        });
        if (found != discrete.end()) {
            return *found;
        }
    }
    if (!discrete.empty()) {
        return *std::ranges::max_element(discrete, std::less{}, [](const auto& fps) {
            return fps.second == 0 ? 0.0 : static_cast<double>(fps.first) / fps.second;
        });
    }
    return {0, 1};
}

}  // namespace

std::vector<VideoDevice> DeviceEnumerator::list_uvc_capture_devices() const {
    std::vector<VideoDevice> devices;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        if (!entry.is_character_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.starts_with("video")) {
            continue;
        }
        if (auto device = inspect_device(entry.path())) {
            devices.push_back(std::move(*device));
        }
    }
    std::ranges::sort(devices, std::less{}, [](const VideoDevice& device) {
        return video_node_number(device.path);
    });
    return devices;
}

std::optional<VideoDevice> DeviceEnumerator::inspect_device(const std::filesystem::path& path) {
    // QUERYCAP/ENUM_FMT only need read. RDWR fails if the user is not in group
    // video; fall back to it for the few drivers that reject O_RDONLY.
    int raw = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (raw < 0) {
        raw = open(path.c_str(), O_RDWR | O_NONBLOCK);
    }
    Fd fd{raw};
    if (!fd) {
        return std::nullopt;
    }

    v4l2_capability capability{};
    if (ioctl(fd.get(), VIDIOC_QUERYCAP, &capability) < 0) {
        return std::nullopt;
    }

    const std::string driver =
        v4l2_fixed_string(reinterpret_cast<const char*>(capability.driver), sizeof(capability.driver));
    if (!driver.starts_with("uvcvideo")) {
        return std::nullopt;
    }

    const __u32 caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                           ? capability.device_caps
                           : capability.capabilities;
    if (!is_capture_caps(caps)) {
        return std::nullopt;
    }

    auto formats = enumerate_formats(fd.get());
    if (formats.empty()) {
        return std::nullopt;
    }

    return VideoDevice{
        .path = path.string(),
        .card_name = v4l2_fixed_string(reinterpret_cast<const char*>(capability.card),
                                       sizeof(capability.card)),
        .driver = driver,
        .preferred = pick_preferred(formats),
    };
}

std::vector<CaptureFormat> DeviceEnumerator::enumerate_formats(int fd) {
    std::vector<CaptureFormat> formats;
    for (int index = 0; index < 32; ++index) {
        v4l2_fmtdesc description{};
        description.index = static_cast<__u32>(index);
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &description) < 0) {
            break;
        }
        const auto fourcc = fourcc_to_string(description.pixelformat);
        for (int size_index = 0; size_index < 64; ++size_index) {
            v4l2_frmsizeenum size{};
            size.index = static_cast<__u32>(size_index);
            size.pixel_format = description.pixelformat;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
                break;
            }
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                const auto fps = pick_fps(fd, description.pixelformat,
                                          static_cast<int>(size.discrete.width),
                                          static_cast<int>(size.discrete.height));
                formats.push_back(CaptureFormat{
                    .fourcc = fourcc,
                    .width = static_cast<int>(size.discrete.width),
                    .height = static_cast<int>(size.discrete.height),
                    .fps_numerator = fps.first,
                    .fps_denominator = fps.second,
                });
            } else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE
                       || size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                const int width = static_cast<int>(size.stepwise.max_width);
                const int height = static_cast<int>(size.stepwise.max_height);
                const auto fps = pick_fps(fd, description.pixelformat, width, height);
                formats.push_back(CaptureFormat{
                    .fourcc = fourcc,
                    .width = width,
                    .height = height,
                    .fps_numerator = fps.first,
                    .fps_denominator = fps.second,
                });
                break;
            }
        }
    }
    return formats;
}

// Prefer MJPG, then the largest mode the camera actually lists, but not above
// 1280x720 so openh264 can keep up in real time. If every mode is larger, take
// the smallest of that pixel format.
CaptureFormat DeviceEnumerator::pick_preferred(const std::vector<CaptureFormat>& formats) {
    auto rank = [](const CaptureFormat& format) {
        const bool fps_ok = format.fps() >= kMinAcceptableFps;
        const bool within_cap =
            format.width <= kMaxCaptureWidth && format.height <= kMaxCaptureHeight;
        const int area = format.width * format.height;
        return std::tuple{fps_ok, within_cap, within_cap ? area : -area};
    };
    auto best_of = [&](const std::vector<CaptureFormat>& group) -> CaptureFormat {
        return *std::ranges::max_element(group, std::less{}, rank);
    };

    for (const auto pixel : kPixelPrefs) {
        std::vector<CaptureFormat> group;
        std::ranges::copy_if(formats, std::back_inserter(group),
                             [&](const CaptureFormat& format) { return format.fourcc == pixel; });
        if (!group.empty()) {
            return best_of(group);
        }
    }
    return best_of(formats);
}

}  // namespace uvc
