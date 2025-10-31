#ifndef SD_AUDIO_PLAYER_H
#define SD_AUDIO_PLAYER_H

#include <array>
#include <string>
#include <vector>
#include <chrono>

#include <esp_timer.h>

#include "audio_service.h"
#include "display.h"

struct SdAudioTrack {
    std::string path;
    std::string title;
    size_t size_bytes = 0;
};

class SdAudioPlayer {
public:
    SdAudioPlayer();
    ~SdAudioPlayer();

    void Initialize(const std::string& mount_point, AudioService* audio_service, Display* display);

    std::vector<SdAudioTrack> ScanTracks(const std::string& subdir = "") const;

    bool Play(const std::string& path);
    void Stop();
    bool IsPlaying() const { return playing_; }
    const std::string& current_track() const { return current_track_path_; }
    const std::string& current_title() const { return current_track_title_; }
    const std::string& mount_point() const { return mount_point_; }
    void OnPlaybackFrame(const std::vector<int16_t>& pcm);

private:
    static constexpr size_t kSpectrumBands = 8;

    AudioService* audio_service_ = nullptr;
    Display* display_ = nullptr;
    std::string mount_point_;
    std::string current_track_path_;
    std::string current_track_title_;
    bool playing_ = false;
    esp_timer_handle_t monitor_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_frame_time_;

    void HandlePlaybackFinished();
    void EnsureMonitorTimer();
    void StartMonitor();
    void StopMonitor();

    static void MonitorTimerThunk(void* arg);
};

#endif // SD_AUDIO_PLAYER_H
