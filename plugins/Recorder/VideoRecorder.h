#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

class VideoRecorder {
public:
    ~VideoRecorder();

    bool Start(std::filesystem::path output_path,
               uint32_t fps,
               uint32_t bitrate_mbps,
               bool capture_output_audio,
               bool capture_microphone,
               float output_volume,
               float microphone_volume);
    void SetFinalPath(std::filesystem::path output_path);
    void CaptureFrame(IDirect3DDevice9* device);
    void Stop();
    bool SavePending();
    bool DiscardPending();

    [[nodiscard]] bool IsRecording() const;
    [[nodiscard]] bool HasPendingRecording() const;
    [[nodiscard]] std::filesystem::path PendingPath() const;
    [[nodiscard]] std::string Status() const;
    [[nodiscard]] std::chrono::steady_clock::duration RecordingDuration() const;

public:
    struct Frame {
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t timestamp_100ns = 0;
        std::vector<uint8_t> pixels;
    };

private:
    bool EnsureCaptureSurfaces(IDirect3DDevice9* device, uint32_t width, uint32_t height);
    void ReleaseCaptureSurfaces();
    void WorkerEntry();
    void SetStatus(std::string value);

    mutable std::mutex mutex;
    std::condition_variable queue_changed;
    std::deque<Frame> frames;
    std::thread worker;
    std::filesystem::path initial_path;
    std::filesystem::path final_path;
    std::filesystem::path pending_source;
    std::filesystem::path pending_destination;
    std::string status = "Idle";
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point next_capture_at{};
    uint32_t fps = 30;
    uint32_t bitrate_mbps = 8;
    bool capture_output_audio = true;
    bool capture_microphone = true;
    float output_volume = 1.f;
    float microphone_volume = 1.f;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    IDirect3DSurface9* render_surface = nullptr;
    IDirect3DSurface9* system_surface = nullptr;
    bool stopping = false;
    bool recording = false;
};
