#include "ChunkRecorder.hpp"

#include <chrono>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

namespace uvc {
namespace {

constexpr std::chrono::seconds kEosFinalizeTimeout{3};
constexpr int kHdMinWidth = 1280;
constexpr int kHdVideoBitrate = 2'000'000;
constexpr int kSdVideoBitrate = 1'000'000;
constexpr int kAacBitrate = 64'000;

[[nodiscard]] const char* source_caps(const CaptureFormat& format) {
    if (format.fourcc == "MJPG") {
        return "image/jpeg";
    }
    if (format.fourcc == "YUYV") {
        return "video/x-raw,format=YUY2";
    }
    if (format.fourcc == "NV12") {
        return "video/x-raw,format=NV12";
    }
    if (format.fourcc == "GREY") {
        return "video/x-raw,format=GRAY8";
    }
    return "video/x-raw";
}

[[nodiscard]] const char* decoder_chain(const CaptureFormat& format) {
    if (format.fourcc == "MJPG") {
        return "jpegdec ! videoconvert";
    }
    return "videoconvert";
}

[[nodiscard]] bool has_element(const char* factory_name) {
    GstElementFactory* factory = gst_element_factory_find(factory_name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

[[nodiscard]] std::optional<std::string> missing_elements(const CaptureFormat& format) {
    std::vector<const char*> required{"v4l2src",      "videoconvert", "openh264enc", "h264parse",
                                      "splitmuxsink", "audiotestsrc", "audioconvert", "aacparse",
                                      "queue"};
    if (format.fourcc == "MJPG") {
        required.push_back("jpegdec");
    }
    required.push_back(has_element("fdkaacenc") ? "fdkaacenc" : "avenc_aac");

    std::string missing;
    for (const char* name : required) {
        if (!has_element(name)) {
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += name;
        }
    }
    if (missing.empty()) {
        return std::nullopt;
    }
    return missing;
}

}  // namespace

ChunkRecorder::ChunkRecorder() = default;

ChunkRecorder::~ChunkRecorder() {
    alive_.store(false);
    if (stop_timeout_id_ != 0) {
        g_source_remove(stop_timeout_id_);
        stop_timeout_id_ = 0;
    }
    shutdown_pipeline();
}

void ChunkRecorder::set_device(VideoDevice device) {
    device_ = std::move(device);
}

void ChunkRecorder::set_output_root(std::filesystem::path directory) {
    output_root_ = std::move(directory);
}

bool ChunkRecorder::is_recording() const noexcept {
    const auto state = state_.load();
    return state == RecorderState::starting || state == RecorderState::recording
           || state == RecorderState::stopping;
}

RecorderState ChunkRecorder::state() const noexcept {
    return state_.load();
}

const std::filesystem::path& ChunkRecorder::session_directory() const noexcept {
    return session_dir_;
}

void ChunkRecorder::on_chunk_completed(ChunkHandler handler) {
    chunk_handler_ = std::move(handler);
}

void ChunkRecorder::on_error(ErrorHandler handler) {
    error_handler_ = std::move(handler);
}

void ChunkRecorder::on_state_changed(StateHandler handler) {
    state_handler_ = std::move(handler);
}

void ChunkRecorder::set_state(RecorderState state) {
    state_.store(state);
    if (state_handler_) {
        state_handler_(state);
    }
}

std::filesystem::path ChunkRecorder::chunk_path(std::uint32_t sequence) const {
    return session_dir_ / std::format("chunk_{:05d}.mp4", sequence);
}

gchar* ChunkRecorder::on_format_location(GstElement*, guint fragment_id, gpointer user_data) {
    auto* self = static_cast<ChunkRecorder*>(user_data);
    const auto path = self->chunk_path(fragment_id);
    std::lock_guard lock(self->mutex_);
    self->current_sequence_ = fragment_id;
    self->current_path_ = path;
    return g_strdup(path.c_str());
}

gboolean ChunkRecorder::on_bus_watch(GstBus*, GstMessage* message, gpointer user_data) {
    auto* self = static_cast<ChunkRecorder*>(user_data);
    if (!self->alive_.load()) {
        return TRUE;
    }
    self->handle_bus_message(message);
    return TRUE;
}

gboolean ChunkRecorder::on_stop_timeout(gpointer user_data) {
    auto* self = static_cast<ChunkRecorder*>(user_data);
    self->stop_timeout_id_ = 0;
    if (self->state_.load() == RecorderState::stopping) {
        self->shutdown_pipeline();
        self->set_state(RecorderState::idle);
    }
    return FALSE;
}

// Live capture graph: camera video + silent AAC, rotated every 3 seconds by
// splitmuxsink. Cuts happen on encoder keyframes so consecutive files neither
// overlap nor drop frames. config-interval=-1 repeats SPS/PPS so each file plays alone.
std::string ChunkRecorder::build_pipeline_description() const {
    const auto& format = device_.preferred;
    const int gop = std::max(1, static_cast<int>(std::lround(format.fps() * kChunkDuration.count())));
    const int bitrate = format.width >= kHdMinWidth ? kHdVideoBitrate : kSdVideoBitrate;
    const std::string aac_element =
        has_element("fdkaacenc") ? std::format("fdkaacenc bitrate={}", kAacBitrate)
                                 : std::format("avenc_aac bitrate={}", kAacBitrate);

    std::string caps = source_caps(format);
    caps += std::format(",width={},height={}", format.width, format.height);
    if (format.fps_numerator > 0 && format.fps_denominator > 0) {
        caps += std::format(",framerate={}/{}", format.fps_numerator, format.fps_denominator);
    }

    return std::format(
        "v4l2src name=cam do-timestamp=true io-mode=mmap"
        " ! {}"
        " ! {}"
        " ! video/x-raw,format=I420"
        " ! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0"
        " ! openh264enc gop-size={} bitrate={} complexity=low enable-frame-skip=false"
        " ! video/x-h264,profile=baseline"
        " ! h264parse config-interval=-1"
        " ! splitmuxsink name=mux max-size-time={} send-keyframe-requests=true"
        " audiotestsrc name=silence wave=silence is-live=true do-timestamp=true"
        " ! audio/x-raw,format=S16LE,rate=48000,channels=1"
        " ! audioconvert"
        " ! {}"
        " ! aacparse"
        " ! mux.audio_0",
        caps, decoder_chain(format), gop, bitrate, kChunkDurationNs, aac_element);
}

std::optional<std::string> ChunkRecorder::start() {
    if (is_recording()) {
        return "Recorder is already running";
    }
    if (device_.path.empty()) {
        return "No UVC device selected";
    }
    if (output_root_.empty()) {
        return "Output directory is not set";
    }
    if (auto missing = missing_elements(device_.preferred)) {
        return "Missing GStreamer elements: " + *missing;
    }

    std::error_code ec;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto local = std::chrono::current_zone()->to_local(now);
    session_id_ = std::format("{:%Y-%m-%d_%H-%M-%S}", local);
    session_dir_ = output_root_ / session_id_;
    std::filesystem::create_directories(session_dir_, ec);
    if (ec) {
        return "Cannot create session directory: " + ec.message();
    }

    {
        std::lock_guard lock(mutex_);
        completed_sequences_.clear();
        current_sequence_.reset();
        current_path_.clear();
    }

    GError* error = nullptr;
    GstElement* parsed = gst_parse_launch(build_pipeline_description().c_str(), &error);
    if (!parsed) {
        std::string message = error ? error->message : "gst_parse_launch failed";
        if (error) {
            g_error_free(error);
        }
        return message;
    }
    pipeline_.reset(parsed);

    GstElement* camera = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "cam");
    GstElement* mux = gst_bin_get_by_name(GST_BIN(pipeline_.get()), "mux");
    if (!camera || !mux) {
        if (camera) {
            gst_object_unref(camera);
        }
        if (mux) {
            gst_object_unref(mux);
        }
        pipeline_.reset();
        return "Pipeline is missing required named elements";
    }

    g_object_set(camera, "device", device_.path.c_str(), nullptr);
    g_signal_connect(mux, "format-location", G_CALLBACK(on_format_location), this);
    gst_object_unref(camera);
    gst_object_unref(mux);

    GstBus* bus = gst_element_get_bus(pipeline_.get());
    bus_watch_id_ = gst_bus_add_watch(bus, on_bus_watch, this);
    gst_object_unref(bus);

    set_state(RecorderState::starting);
    const GstStateChangeReturn ret = gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        shutdown_pipeline();
        set_state(RecorderState::idle);
        return "Failed to set the capture pipeline to PLAYING";
    }
    if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL) {
        set_state(RecorderState::recording);
    }
    return std::nullopt;
}

void ChunkRecorder::stop() {
    if (!pipeline_ || state_.load() == RecorderState::idle) {
        return;
    }
    if (state_.load() == RecorderState::stopping) {
        return;
    }
    set_state(RecorderState::stopping);
    send_eos_to_sources();
    if (stop_timeout_id_ == 0) {
        stop_timeout_id_ =
            g_timeout_add_seconds(static_cast<guint>(kEosFinalizeTimeout.count()), on_stop_timeout, this);
    }
}

void ChunkRecorder::send_eos_to_sources() {
    if (!pipeline_) {
        return;
    }
    // Live sources ignore a pipeline-level EOS. Push EOS into each source so
    // splitmuxsink can close the last fragment and write a valid MP4 header.
    for (const char* name : {"cam", "silence"}) {
        if (GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline_.get()), name)) {
            gst_element_send_event(source, gst_event_new_eos());
            gst_object_unref(source);
        }
    }
}

void ChunkRecorder::shutdown_pipeline() {
    if (bus_watch_id_ != 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
    if (pipeline_) {
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        pipeline_.reset();
    }

    std::optional<std::uint32_t> leftover_sequence;
    std::filesystem::path leftover_path;
    {
        std::lock_guard lock(mutex_);
        leftover_sequence = current_sequence_;
        leftover_path = current_path_;
        current_sequence_.reset();
        current_path_.clear();
    }
    if (leftover_sequence) {
        complete_chunk(*leftover_sequence, leftover_path);
    }
}

void ChunkRecorder::complete_chunk(std::uint32_t sequence, std::filesystem::path path) {
    {
        std::lock_guard lock(mutex_);
        if (!completed_sequences_.insert(sequence).second) {
            return;
        }
    }
    if (path.empty()) {
        path = chunk_path(sequence);
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) {
        return;
    }
    ChunkInfo info{
        .sequence = sequence,
        .path = std::move(path),
        .session_id = session_id_,
        .size_bytes = size,
        .upload = UploadState::idle,
        .error = {},
    };
    if (chunk_handler_) {
        chunk_handler_(std::move(info));
    }
}

void ChunkRecorder::handle_fragment_closed(const GstStructure* structure) {
    if (!structure) {
        return;
    }
    guint fragment_id = 0;
    const gchar* location = gst_structure_get_string(structure, "location");
    gst_structure_get_uint(structure, "fragment-id", &fragment_id);
    std::filesystem::path path = location ? std::filesystem::path(location) : chunk_path(fragment_id);
    complete_chunk(fragment_id, std::move(path));
}

void ChunkRecorder::handle_bus_message(GstMessage* message) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            std::string text = error ? error->message : "Unknown GStreamer error";
            g_clear_error(&error);
            g_free(debug);
            shutdown_pipeline();
            set_state(RecorderState::idle);
            if (error_handler_) {
                error_handler_(std::move(text));
            }
            break;
        }
        case GST_MESSAGE_EOS:
            if (stop_timeout_id_ != 0) {
                g_source_remove(stop_timeout_id_);
                stop_timeout_id_ = 0;
            }
            shutdown_pipeline();
            set_state(RecorderState::idle);
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline_.get())) {
                GstState old_state = GST_STATE_NULL;
                GstState new_state = GST_STATE_NULL;
                gst_message_parse_state_changed(message, &old_state, &new_state, nullptr);
                if (new_state == GST_STATE_PLAYING && state_.load() == RecorderState::starting) {
                    set_state(RecorderState::recording);
                }
            }
            break;
        case GST_MESSAGE_ELEMENT: {
            const GstStructure* structure = gst_message_get_structure(message);
            if (structure && gst_structure_has_name(structure, "splitmuxsink-fragment-closed")) {
                handle_fragment_closed(structure);
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace uvc
