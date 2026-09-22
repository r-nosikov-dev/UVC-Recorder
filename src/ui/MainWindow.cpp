#include "MainWindow.hpp"

#include "core/GLibPtr.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <string>
#include <unistd.h>
#include <utility>

namespace uvc {
namespace {

constexpr std::chrono::seconds kDevicePollInterval{3};

struct UploadIdle {
    std::shared_ptr<std::atomic<bool>> alive;
    MainWindow* window;
    ChunkInfo chunk;
};

[[nodiscard]] std::filesystem::path default_output_dir() {
    const char* home = g_get_home_dir();
    if (!home) {
        return std::filesystem::current_path() / "recordings";
    }
    return std::filesystem::path(home) / "Videos" / "uvc-recorder";
}

[[nodiscard]] std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

[[nodiscard]] std::filesystem::path make_scratch_dir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "uvc-recorder-XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) {
        return {};
    }
    return tmpl;
}

}  // namespace

MainWindow::MainWindow(GtkApplication* app) {
    build_ui(app);
    recorder_.on_chunk_completed([this](ChunkInfo chunk) { on_chunk_completed(std::move(chunk)); });
    recorder_.on_error([this](std::string message) {
        set_status("Error: " + message);
        update_controls();
    });
    recorder_.on_state_changed([this](RecorderState state) { on_recorder_state(state); });
    uploader_.on_status([this](const ChunkInfo& chunk) {
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_idle_upload,
                        new UploadIdle{alive_, this, chunk},
                        +[](gpointer data) { delete static_cast<UploadIdle*>(data); });
    });
    refresh_devices();
    device_poll_id_ = g_timeout_add_seconds(
        static_cast<guint>(kDevicePollInterval.count()),
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<MainWindow*>(data);
            if (!self->widgets_alive_) {
                return FALSE;
            }
            if (!self->recorder_.is_recording()) {
                self->refresh_devices();
            }
            return TRUE;
        },
        this);
}

MainWindow::~MainWindow() {
    on_window_destroy();
}

void MainWindow::present() {
    if (window_ != nullptr) {
        gtk_window_present(GTK_WINDOW(window_));
    }
}

void MainWindow::build_ui(GtkApplication* app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "UVC Recorder");
    gtk_window_set_default_size(GTK_WINDOW(window_), 620, -1);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    GtkWidget* device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), device_row);
    gtk_box_append(GTK_BOX(device_row), gtk_label_new("Device"));
    device_dropdown_ = gtk_drop_down_new(nullptr, nullptr);
    gtk_widget_set_hexpand(device_dropdown_, TRUE);
    gtk_box_append(GTK_BOX(device_row), device_dropdown_);
    refresh_button_ = gtk_button_new_with_label("Refresh");
    gtk_box_append(GTK_BOX(device_row), refresh_button_);

    GtkWidget* output_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), output_row);
    gtk_box_append(GTK_BOX(output_row), gtk_label_new("Save to"));
    output_entry_ = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(output_entry_), default_output_dir().string().c_str());
    gtk_widget_set_hexpand(output_entry_, TRUE);
    gtk_box_append(GTK_BOX(output_row), output_entry_);
    browse_button_ = gtk_button_new_with_label("Browse");
    gtk_box_append(GTK_BOX(output_row), browse_button_);

    GtkWidget* upload_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), upload_row);
    gtk_box_append(GTK_BOX(upload_row), gtk_label_new("Upload URL"));
    upload_entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(upload_entry_, TRUE);
    gtk_box_append(GTK_BOX(upload_row), upload_entry_);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), buttons);
    start_button_ = gtk_button_new_with_label("Start");
    stop_button_ = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(stop_button_, FALSE);
    gtk_box_append(GTK_BOX(buttons), start_button_);
    gtk_box_append(GTK_BOX(buttons), stop_button_);

    status_label_ = gtk_label_new("");
    gtk_widget_set_halign(status_label_, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(status_label_), TRUE);
    gtk_widget_set_visible(status_label_, FALSE);
    gtk_box_append(GTK_BOX(root), status_label_);

    g_signal_connect(start_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<MainWindow*>(data)->on_start();
                     }),
                     this);
    g_signal_connect(stop_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<MainWindow*>(data)->on_stop();
                     }),
                     this);
    g_signal_connect(refresh_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<MainWindow*>(data)->refresh_devices();
                     }),
                     this);
    g_signal_connect(browse_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<MainWindow*>(data)->on_browse();
                     }),
                     this);
    g_signal_connect(upload_entry_, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
                         static_cast<MainWindow*>(data)->update_controls();
                     }),
                     this);
    g_signal_connect(window_, "close-request",
                     G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
                         auto* self = static_cast<MainWindow*>(data);
                         if (self->recorder_.is_recording()) {
                             self->recorder_.stop();
                         }
                         return FALSE;
                     }),
                     this);
    g_signal_connect(window_, "destroy",
                     G_CALLBACK(+[](GtkWidget*, gpointer data) {
                         static_cast<MainWindow*>(data)->on_window_destroy();
                     }),
                     this);
}

void MainWindow::on_window_destroy() {
    if (!widgets_alive_) {
        return;
    }
    widgets_alive_ = false;
    alive_->store(false);
    recorder_.on_chunk_completed({});
    recorder_.on_error({});
    recorder_.on_state_changed({});
    uploader_.on_status({});
    if (device_poll_id_ != 0) {
        g_source_remove(device_poll_id_);
        device_poll_id_ = 0;
    }
    if (recorder_.is_recording()) {
        recorder_.stop();
    }
    window_ = nullptr;
    device_dropdown_ = nullptr;
    start_button_ = nullptr;
    stop_button_ = nullptr;
    refresh_button_ = nullptr;
    output_entry_ = nullptr;
    browse_button_ = nullptr;
    upload_entry_ = nullptr;
    status_label_ = nullptr;
    clear_scratch_dir();
}

void MainWindow::refresh_devices() {
    if (!widgets_alive_ || device_dropdown_ == nullptr) {
        return;
    }
    const guint previous = gtk_drop_down_get_selected(GTK_DROP_DOWN(device_dropdown_));
    std::string previous_path;
    if (previous != GTK_INVALID_LIST_POSITION && previous < devices_.size()) {
        previous_path = devices_[previous].path;
    }

    devices_ = enumerator_.list_uvc_capture_devices();
    GtkStringList* names = gtk_string_list_new(nullptr);
    guint selected = GTK_INVALID_LIST_POSITION;
    for (guint index = 0; index < devices_.size(); ++index) {
        gtk_string_list_append(names, devices_[index].label().c_str());
        if (!previous_path.empty() && devices_[index].path == previous_path) {
            selected = index;
        }
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(device_dropdown_), G_LIST_MODEL(names));
    g_object_unref(names);
    if (selected != GTK_INVALID_LIST_POSITION) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(device_dropdown_), selected);
    } else if (!devices_.empty()) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(device_dropdown_), 0);
    }
    update_controls();
    if (devices_.empty()) {
        set_status("No UVC capture device found");
    }
}

void MainWindow::set_status(const std::string& text) {
    if (!widgets_alive_ || status_label_ == nullptr) {
        return;
    }
    gtk_label_set_text(GTK_LABEL(status_label_), text.c_str());
    gtk_widget_set_visible(status_label_, !text.empty());
}

void MainWindow::update_controls() {
    if (!widgets_alive_ || start_button_ == nullptr) {
        return;
    }
    const bool recording = recorder_.is_recording();
    gtk_widget_set_sensitive(start_button_, !recording && !devices_.empty());
    gtk_widget_set_sensitive(stop_button_, recording && recorder_.state() != RecorderState::stopping);
    gtk_widget_set_sensitive(device_dropdown_, !recording);
    gtk_widget_set_sensitive(refresh_button_, !recording);
    const bool has_url = !trimmed_upload_url().empty();
    gtk_widget_set_sensitive(output_entry_, !recording && !has_url);
    if (browse_button_ != nullptr) {
        gtk_widget_set_sensitive(browse_button_, !recording && !has_url);
    }
    gtk_widget_set_sensitive(upload_entry_, !recording);
}

void MainWindow::on_start() {
    if (!widgets_alive_ || device_dropdown_ == nullptr) {
        return;
    }
    const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(device_dropdown_));
    if (selected == GTK_INVALID_LIST_POSITION || selected >= devices_.size()) {
        set_status("Select a UVC device first");
        return;
    }

    sticky_status_.clear();
    clear_scratch_dir();
    recorder_.set_device(devices_[selected]);

    const std::string url = trimmed_upload_url();
    upload_only_ = !url.empty();
    if (upload_only_) {
        scratch_dir_ = make_scratch_dir();
        if (scratch_dir_.empty()) {
            set_status("Cannot create a temp directory for upload-only recording");
            return;
        }
        recorder_.set_output_root(scratch_dir_);
        uploader_.set_endpoint(url);
        uploader_.set_delete_after_upload(true);
    } else {
        recorder_.set_output_root(gtk_editable_get_text(GTK_EDITABLE(output_entry_)));
        uploader_.set_endpoint({});
        uploader_.set_delete_after_upload(false);
    }

    if (auto error = recorder_.start()) {
        set_status("Failed to start: " + *error);
        update_controls();
        return;
    }
    update_controls();
    if (upload_only_) {
        set_status("Recording to URL");
    } else {
        set_status("Recording to " + recorder_.session_directory().string());
    }
}

void MainWindow::on_stop() {
    recorder_.stop();
    update_controls();
    set_status("Stopping");
}

void MainWindow::on_browse() {
    if (!widgets_alive_ || window_ == nullptr) {
        return;
    }
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose output folder");
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            auto* self = static_cast<MainWindow*>(data);
            GError* error = nullptr;
            GFile* file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
            if (file) {
                GCharPtr path{g_file_get_path(file)};
                if (path && self->widgets_alive_ && self->output_entry_ != nullptr) {
                    gtk_editable_set_text(GTK_EDITABLE(self->output_entry_), path.get());
                }
                g_object_unref(file);
            }
            g_clear_error(&error);
            g_object_unref(source);
        },
        this);
}

void MainWindow::on_chunk_completed(ChunkInfo chunk) {
    if (uploader_.enabled()) {
        set_status(std::format("Uploading {}", chunk.path.filename().string()));
        uploader_.enqueue(std::move(chunk));
        return;
    }
    set_status(std::format("Wrote {}", chunk.path.string()));
}

void MainWindow::on_upload_status(const ChunkInfo& chunk) {
    if (chunk.upload == UploadState::failed) {
        sticky_status_ = "Server disconnected";
        if (!chunk.error.empty()) {
            sticky_status_ += " (" + chunk.error + ")";
        }
        set_status(sticky_status_);
        uploader_.abort();
        if (recorder_.is_recording()) {
            recorder_.stop();
        }
        update_controls();
        return;
    }
    if (chunk.upload == UploadState::uploaded) {
        set_status(std::format("Uploaded {}", chunk.path.filename().string()));
    }
}

void MainWindow::on_recorder_state(RecorderState state) {
    update_controls();
    if (state == RecorderState::idle) {
        set_status(sticky_status_);
        if (upload_only_ && !sticky_status_.empty()) {
            clear_scratch_dir();
        }
    }
}

void MainWindow::clear_scratch_dir() {
    if (scratch_dir_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(scratch_dir_, ec);
    scratch_dir_.clear();
}

std::string MainWindow::trimmed_upload_url() const {
    if (!widgets_alive_ || upload_entry_ == nullptr) {
        return {};
    }
    const char* text = gtk_editable_get_text(GTK_EDITABLE(upload_entry_));
    return trim_copy(text ? text : "");
}

gboolean MainWindow::on_idle_upload(gpointer data) {
    auto* job = static_cast<UploadIdle*>(data);
    if (job->alive->load()) {
        job->window->on_upload_status(job->chunk);
    }
    return G_SOURCE_REMOVE;
}

}  // namespace uvc
