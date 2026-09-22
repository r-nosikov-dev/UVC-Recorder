#pragma once

#include "core/GLibPtr.hpp"
#include "core/Types.hpp"

#include <gst/gst.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace uvc {

class ChunkRecorder {
public:
    using ChunkHandler = std::function<void(ChunkInfo)>;
    using ErrorHandler = std::function<void(std::string)>;
    using StateHandler = std::function<void(RecorderState)>;

    ChunkRecorder();
    ChunkRecorder(const ChunkRecorder&) = delete;
    ChunkRecorder& operator=(const ChunkRecorder&) = delete;
    ~ChunkRecorder();

    void set_device(VideoDevice device);
    void set_output_root(std::filesystem::path directory);

    [[nodiscard]] std::optional<std::string> start();
    void stop();

    [[nodiscard]] bool is_recording() const noexcept;
    [[nodiscard]] RecorderState state() const noexcept;
    [[nodiscard]] const std::filesystem::path& session_directory() const noexcept;

    void on_chunk_completed(ChunkHandler handler);
    void on_error(ErrorHandler handler);
    void on_state_changed(StateHandler handler);

private:
    [[nodiscard]] std::string build_pipeline_description() const;
    void send_eos_to_sources();
    [[nodiscard]] std::filesystem::path chunk_path(std::uint32_t sequence) const;
    void set_state(RecorderState state);
    void handle_bus_message(GstMessage* message);
    void handle_fragment_closed(const GstStructure* structure);
    void complete_chunk(std::uint32_t sequence, std::filesystem::path path);
    void shutdown_pipeline();
    static gchar* on_format_location(GstElement* splitmux, guint fragment_id, gpointer user_data);
    static gboolean on_bus_watch(GstBus* bus, GstMessage* message, gpointer user_data);
    static gboolean on_stop_timeout(gpointer user_data);

    VideoDevice device_{};
    std::filesystem::path output_root_;
    std::filesystem::path session_dir_;
    std::string session_id_;

    GstPtr<GstElement> pipeline_;
    guint bus_watch_id_{0};
    guint stop_timeout_id_{0};

    std::atomic<RecorderState> state_{RecorderState::idle};
    std::atomic<bool> alive_{true};

    std::mutex mutex_;
    std::unordered_set<std::uint32_t> completed_sequences_;
    std::optional<std::uint32_t> current_sequence_;
    std::filesystem::path current_path_;

    ChunkHandler chunk_handler_;
    ErrorHandler error_handler_;
    StateHandler state_handler_;
};

}  // namespace uvc
