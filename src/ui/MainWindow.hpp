#pragma once

#include "capture/ChunkRecorder.hpp"
#include "device/DeviceEnumerator.hpp"
#include "upload/ChunkUploader.hpp"

#include <gtk/gtk.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace uvc {

class MainWindow {
public:
    explicit MainWindow(GtkApplication* app);
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    ~MainWindow();

    void present();

private:
    void build_ui(GtkApplication* app);
    void refresh_devices();
    void set_status(const std::string& text);
    void update_controls();
    void on_start();
    void on_stop();
    void on_browse();
    void on_chunk_completed(ChunkInfo chunk);
    void on_upload_status(const ChunkInfo& chunk);
    void on_recorder_state(RecorderState state);
    void on_window_destroy();
    void clear_scratch_dir();
    [[nodiscard]] std::string trimmed_upload_url() const;
    static gboolean on_idle_upload(gpointer data);

    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};
    DeviceEnumerator enumerator_;
    ChunkRecorder recorder_;
    ChunkUploader uploader_;
    std::vector<VideoDevice> devices_;

    GtkWidget* window_{nullptr};
    GtkWidget* device_dropdown_{nullptr};
    GtkWidget* start_button_{nullptr};
    GtkWidget* stop_button_{nullptr};
    GtkWidget* refresh_button_{nullptr};
    GtkWidget* output_entry_{nullptr};
    GtkWidget* browse_button_{nullptr};
    GtkWidget* upload_entry_{nullptr};
    GtkWidget* status_label_{nullptr};

    guint device_poll_id_{0};
    bool widgets_alive_{true};
    std::string sticky_status_;
    std::filesystem::path scratch_dir_;
    bool upload_only_{false};
};

}  // namespace uvc
