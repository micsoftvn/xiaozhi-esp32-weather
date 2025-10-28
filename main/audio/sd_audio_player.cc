#include "sd_audio_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <esp_log.h>

#include "application.h"
#include "board.h"

namespace {
constexpr char TAG[] = "SdAudioPlayer";
constexpr size_t kReadChunk = 4096;
constexpr uint64_t kMonitorPeriodUs = 250000;
constexpr size_t kSpectrumBands = 8;

bool HasOggExtension(const std::string& name) {
    auto pos = name.find_last_of('.');
    if (pos == std::string::npos) {
        return false;
    }
    auto ext = name.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == "ogg" || ext == "oga";
}

std::string ExtractTitle(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    std::string filename = (pos == std::string::npos) ? path : path.substr(pos + 1);
    auto dot = filename.find_last_of('.');
    if (dot != std::string::npos) {
        filename = filename.substr(0, dot);
    }
    return filename;
}
} // namespace

SdAudioPlayer::SdAudioPlayer() = default;

SdAudioPlayer::~SdAudioPlayer() {
    StopMonitor();
    if (monitor_timer_) {
        esp_timer_delete(monitor_timer_);
        monitor_timer_ = nullptr;
    }
}

void SdAudioPlayer::Initialize(const std::string& mount_point, AudioService* audio_service, Display* display) {
    mount_point_ = mount_point;
    audio_service_ = audio_service;
    display_ = display;
    EnsureMonitorTimer();
}

std::vector<SdAudioTrack> SdAudioPlayer::ScanTracks(const std::string& subdir) const {
    std::vector<SdAudioTrack> tracks;
    std::string base = mount_point_;
    if (!subdir.empty()) {
        if (base.back() != '/') {
            base += "/";
        }
        base += subdir;
    }

    DIR* dir = opendir(base.c_str());
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", base.c_str());
        return tracks;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string full_path = base;
        if (full_path.back() != '/') {
            full_path += "/";
        }
        full_path += entry->d_name;

        if (entry->d_type == DT_DIR) {
            auto nested = ScanTracks(full_path.substr(mount_point_.size() + 1));
            tracks.insert(tracks.end(), nested.begin(), nested.end());
            continue;
        }

        if (!HasOggExtension(entry->d_name)) {
            continue;
        }

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            continue;
        }

        SdAudioTrack track;
        track.path = full_path;
        track.title = ExtractTitle(full_path);
        track.size_bytes = static_cast<size_t>(st.st_size);
        tracks.push_back(std::move(track));
    }

    closedir(dir);
    std::sort(tracks.begin(), tracks.end(), [](const SdAudioTrack& a, const SdAudioTrack& b) {
        return a.title < b.title;
    });
    return tracks;
}

bool SdAudioPlayer::Play(const std::string& path) {
    if (audio_service_ == nullptr || display_ == nullptr) {
        ESP_LOGE(TAG, "AudioService or Display not set");
        return false;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open %s", path.c_str());
        return false;
    }

    std::vector<uint8_t> buffer;
    buffer.reserve(64 * 1024);
    uint8_t chunk[kReadChunk];
    size_t read_bytes = 0;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        buffer.insert(buffer.end(), chunk, chunk + read_bytes);
    }
    fclose(file);

    if (buffer.empty()) {
        ESP_LOGW(TAG, "Track %s is empty", path.c_str());
        return false;
    }

    audio_service_->ResetDecoder();

    current_track_path_ = path;
    current_track_title_ = ExtractTitle(path);
    playing_ = true;
    last_frame_time_ = std::chrono::steady_clock::now();

    display_->ShowAudioPlayer(current_track_title_);
    StartMonitor();

    std::string_view sound(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    audio_service_->PlaySound(sound);
    return true;
}

void SdAudioPlayer::Stop() {
    if (!playing_) {
        return;
    }
    audio_service_->ResetDecoder();
    HandlePlaybackFinished();
}

void SdAudioPlayer::OnPlaybackFrame(const std::vector<int16_t>& pcm) {
    if (!playing_ || pcm.empty()) {
        return;
    }

    last_frame_time_ = std::chrono::steady_clock::now();

    std::array<uint8_t, kSpectrumBands> bars{};
    size_t samples_per_band = pcm.size() / kSpectrumBands;
    if (samples_per_band == 0) {
        samples_per_band = pcm.size();
    }

    for (size_t band = 0; band < kSpectrumBands; ++band) {
        size_t start = band * samples_per_band;
        size_t end = std::min(start + samples_per_band, pcm.size());
        if (start >= end) {
            break;
        }
        uint64_t accum = 0;
        for (size_t i = start; i < end; ++i) {
            accum += std::abs(pcm[i]);
        }
        uint32_t avg = static_cast<uint32_t>(accum / (end - start));
        float normalized = static_cast<float>(avg) / 32768.0f;
        if (normalized > 1.0f) {
            normalized = 1.0f;
        }
        bars[band] = static_cast<uint8_t>(normalized * 100.0f);
    }

    display_->UpdateAudioSpectrum(bars);
}

void SdAudioPlayer::HandlePlaybackFinished() {
    playing_ = false;
    StopMonitor();
    current_track_path_.clear();
    current_track_title_.clear();
    display_->HideAudioPlayer();
}

void SdAudioPlayer::EnsureMonitorTimer() {
    if (monitor_timer_) {
        return;
    }
    esp_timer_create_args_t args = {
        .callback = &SdAudioPlayer::MonitorTimerThunk,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sd_audio_monitor",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &monitor_timer_));
}

void SdAudioPlayer::StartMonitor() {
    EnsureMonitorTimer();
    if (monitor_timer_) {
        esp_timer_start_periodic(monitor_timer_, kMonitorPeriodUs);
    }
}

void SdAudioPlayer::StopMonitor() {
    if (monitor_timer_) {
        esp_timer_stop(monitor_timer_);
    }
}

void SdAudioPlayer::MonitorTimerThunk(void* arg) {
    auto* self = static_cast<SdAudioPlayer*>(arg);
    if (!self->playing_) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - self->last_frame_time_).count();
    if (elapsed > 1500 && self->audio_service_ && self->audio_service_->IsIdle()) {
        Application::GetInstance().Schedule([self]() {
            self->HandlePlaybackFinished();
        });
    }
}
