#pragma once

#include "core/Types.hpp"

#include <optional>
#include <vector>

namespace uvc {

class DeviceEnumerator {
public:
    // Keep UVC capture nodes only. Metadata-only /dev/video* siblings are skipped.
    [[nodiscard]] std::vector<VideoDevice> list_uvc_capture_devices() const;

private:
    [[nodiscard]] static std::optional<VideoDevice> inspect_device(const std::filesystem::path& path);
    [[nodiscard]] static std::vector<CaptureFormat> enumerate_formats(int fd);
    [[nodiscard]] static CaptureFormat pick_preferred(const std::vector<CaptureFormat>& formats);
};

}  // namespace uvc
