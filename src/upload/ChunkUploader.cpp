#include "ChunkUploader.hpp"

#include <curl/curl.h>

#include <chrono>
#include <filesystem>
#include <format>

namespace uvc {
namespace {

constexpr int kUploadMaxAttempts = 3;
constexpr std::chrono::seconds kUploadTimeout{30};

size_t discard_body(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/) {
    return size * nmemb;
}

}  // namespace

ChunkUploader::~ChunkUploader() {
    thread_.request_stop();
    cv_.notify_all();
}

void ChunkUploader::set_endpoint(std::string url) {
    std::lock_guard lock(mutex_);
    endpoint_ = std::move(url);
    aborted_ = false;
}

void ChunkUploader::set_delete_after_upload(bool enabled) {
    std::lock_guard lock(mutex_);
    delete_after_upload_ = enabled;
}

void ChunkUploader::abort() {
    aborted_ = true;
    cv_.notify_all();
}

bool ChunkUploader::enabled() const {
    std::lock_guard lock(mutex_);
    return !endpoint_.empty();
}

void ChunkUploader::enqueue(ChunkInfo chunk) {
    chunk.upload = UploadState::queued;
    {
        std::lock_guard lock(mutex_);
        if (endpoint_.empty() || aborted_) {
            return;
        }
        queue_.push(std::move(chunk));
        ensure_worker();
    }
    cv_.notify_one();
}

void ChunkUploader::on_status(StatusHandler handler) {
    std::lock_guard lock(mutex_);
    status_handler_ = std::move(handler);
}

void ChunkUploader::ensure_worker() {
    if (!thread_.joinable()) {
        thread_ = std::jthread([this](std::stop_token stop) { worker(stop); });
    }
}

// Send finished files one-by-one so the REST API receives them in recording order.
void ChunkUploader::worker(std::stop_token stop) {
    while (!stop.stop_requested()) {
        ChunkInfo chunk;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested() || queue_.empty()) {
                continue;
            }
            chunk = std::move(queue_.front());
            queue_.pop();
        }

        chunk.upload = UploadState::uploading;
        StatusHandler handler;
        {
            std::lock_guard lock(mutex_);
            handler = status_handler_;
        }
        if (handler) {
            handler(chunk);
        }

        if (aborted_ || stop.stop_requested()) {
            continue;
        }

        std::string error;
        bool ok = false;
        for (int attempt = 1; attempt <= kUploadMaxAttempts && !stop.stop_requested() && !aborted_;
             ++attempt) {
            error.clear();
            ok = upload_once(chunk, error);
            if (ok) {
                break;
            }
            if (attempt < kUploadMaxAttempts && !aborted_) {
                const auto backoff = std::chrono::seconds{1 << (attempt - 1)};
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, stop, backoff, [this] { return aborted_.load(); });
            }
        }

        if (aborted_ && !ok) {
            continue;
        }

        chunk.error = ok ? std::string{} : error;
        chunk.upload = ok ? UploadState::uploaded : UploadState::failed;
        if (!ok) {
            aborted_ = true;
        }
        {
            std::lock_guard lock(mutex_);
            handler = status_handler_;
        }
        if (handler) {
            handler(chunk);
        }
        if (ok) {
            bool remove_local = false;
            {
                std::lock_guard lock(mutex_);
                remove_local = delete_after_upload_;
            }
            if (remove_local) {
                std::error_code ec;
                std::filesystem::remove(chunk.path, ec);
            }
        }
    }
}

bool ChunkUploader::upload_once(const ChunkInfo& chunk, std::string& error) const {
    std::string endpoint;
    {
        std::lock_guard lock(mutex_);
        endpoint = endpoint_;
    }
    if (endpoint.empty()) {
        error = "Upload endpoint is empty";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }

    curl_mime* mime = curl_mime_init(curl);
    auto add_field = [mime](const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    };

    add_field("session_id", chunk.session_id);
    add_field("sequence", std::to_string(chunk.sequence));
    add_field("filename", chunk.path.filename().string());

    curl_mimepart* file_part = curl_mime_addpart(mime);
    curl_mime_name(file_part, "file");
    curl_mime_filedata(file_part, chunk.path.c_str());
    curl_mime_filename(file_part, chunk.path.filename().c_str());
    curl_mime_type(file_part, "video/mp4");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(kUploadTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);

    const CURLcode result = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = (result == CURLE_GOT_NOTHING) ? "server returned nothing"
                                              : curl_easy_strerror(result);
        return false;
    }
    if (http_status < 200 || http_status >= 300) {
        error = std::format("HTTP {}", http_status);
        return false;
    }
    return true;
}

}  // namespace uvc
