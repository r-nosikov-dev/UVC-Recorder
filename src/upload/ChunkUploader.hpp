#pragma once

#include "core/Types.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>

namespace uvc {

class ChunkUploader {
public:
    using StatusHandler = std::function<void(const ChunkInfo&)>;

    ChunkUploader() = default;
    ChunkUploader(const ChunkUploader&) = delete;
    ChunkUploader& operator=(const ChunkUploader&) = delete;
    ~ChunkUploader();

    void set_endpoint(std::string url);
    void set_delete_after_upload(bool enabled);
    [[nodiscard]] bool enabled() const;
    void abort();

    void enqueue(ChunkInfo chunk);
    void on_status(StatusHandler handler);

private:
    void ensure_worker();
    void worker(std::stop_token stop);
    [[nodiscard]] bool upload_once(const ChunkInfo& chunk, std::string& error) const;

    std::string endpoint_;
    StatusHandler status_handler_;
    std::atomic<bool> aborted_{false};
    bool delete_after_upload_{false};

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::queue<ChunkInfo> queue_;
    std::jthread thread_;
};

}  // namespace uvc
