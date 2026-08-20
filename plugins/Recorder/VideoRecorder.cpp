#include "VideoRecorder.h"

#include <algorithm>
#include <audioclient.h>
#include <cstring>
#include <deque>
#include <d3d9.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <span>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {
    constexpr size_t max_queued_frames = 3;
    constexpr uint32_t audio_sample_rate = 48'000;
    constexpr uint32_t audio_channels = 2;
    constexpr uint32_t audio_chunk_frames = 480;

    class WasapiSource {
    public:
        bool Initialize(const EDataFlow flow)
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return false;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) return false;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio_client))) return false;

            WAVEFORMATEXTENSIBLE format{};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = audio_channels;
            format.Format.nSamplesPerSec = audio_sample_rate;
            format.Format.wBitsPerSample = 32;
            format.Format.nBlockAlign = audio_channels * sizeof(float);
            format.Format.nAvgBytesPerSec = audio_sample_rate * format.Format.nBlockAlign;
            format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            auto flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            if (flow == eRender) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
            if (FAILED(audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10'000'000, 0, &format.Format, nullptr))) return false;
            if (FAILED(audio_client->GetService(IID_PPV_ARGS(&capture_client)))) return false;
            return SUCCEEDED(audio_client->Start());
        }

        void Drain(std::deque<float>& samples)
        {
            if (!capture_client) return;
            UINT32 packet_frames = 0;
            while (SUCCEEDED(capture_client->GetNextPacketSize(&packet_frames)) && packet_frames) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT32 frames = 0;
                if (FAILED(capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
                const auto sample_count = static_cast<size_t>(frames) * audio_channels;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    samples.insert(samples.end(), sample_count, 0.f);
                }
                else {
                    const auto input = reinterpret_cast<const float*>(data);
                    samples.insert(samples.end(), input, input + sample_count);
                }
                capture_client->ReleaseBuffer(frames);
            }
        }

        void Stop()
        {
            if (audio_client) audio_client->Stop();
            capture_client.Reset();
            audio_client.Reset();
        }

        [[nodiscard]] bool IsReady() const { return capture_client != nullptr; }

    private:
        ComPtr<IAudioClient> audio_client;
        ComPtr<IAudioCaptureClient> capture_client;
    };

    bool ConfigureWriter(const std::filesystem::path& path,
                         const uint32_t width,
                         const uint32_t height,
                         const uint32_t fps,
                         const uint32_t bitrate_mbps,
                         const bool include_audio,
                         ComPtr<IMFSinkWriter>& writer,
                         DWORD& video_stream_index,
                         DWORD& audio_stream_index)
    {
        ComPtr<IMFAttributes> attributes;
        if (FAILED(MFCreateAttributes(&attributes, 2))) return false;
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer))) return false;

        ComPtr<IMFMediaType> output_type;
        if (FAILED(MFCreateMediaType(&output_type))) return false;
        output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_mbps * 1'000'000);
        output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(writer->AddStream(output_type.Get(), &video_stream_index))) return false;

        ComPtr<IMFMediaType> input_type;
        if (FAILED(MFCreateMediaType(&input_type))) return false;
        input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(writer->SetInputMediaType(video_stream_index, input_type.Get(), nullptr))) return false;

        if (include_audio) {
            ComPtr<IMFMediaType> audio_output;
            if (FAILED(MFCreateMediaType(&audio_output))) return false;
            audio_output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            audio_output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            audio_output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio_channels);
            audio_output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio_sample_rate);
            audio_output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24'000);
            audio_output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            audio_output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            audio_output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            if (FAILED(writer->AddStream(audio_output.Get(), &audio_stream_index))) return false;

            ComPtr<IMFMediaType> audio_input;
            if (FAILED(MFCreateMediaType(&audio_input))) return false;
            audio_input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            audio_input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            audio_input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio_channels);
            audio_input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio_sample_rate);
            audio_input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            audio_input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audio_channels * sizeof(float));
            audio_input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio_sample_rate * audio_channels * sizeof(float));
            if (FAILED(writer->SetInputMediaType(audio_stream_index, audio_input.Get(), nullptr))) return false;
        }
        return SUCCEEDED(writer->BeginWriting());
    }

    bool WriteFrame(IMFSinkWriter* writer, const DWORD stream_index, const VideoRecorder::Frame& frame, const uint32_t fps)
    {
        ComPtr<IMFMediaBuffer> buffer;
        const auto buffer_size = static_cast<DWORD>(frame.pixels.size());
        if (FAILED(MFCreateMemoryBuffer(buffer_size, &buffer))) return false;
        BYTE* destination = nullptr;
        if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) return false;
        memcpy(destination, frame.pixels.data(), frame.pixels.size());
        buffer->Unlock();
        buffer->SetCurrentLength(buffer_size);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) return false;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(frame.timestamp_100ns);
        sample->SetSampleDuration(10'000'000ll / fps);
        return SUCCEEDED(writer->WriteSample(stream_index, sample.Get()));
    }

    bool WriteAudio(IMFSinkWriter* writer,
                    const DWORD stream_index,
                    const std::span<const float> samples,
                    const int64_t timestamp_100ns)
    {
        ComPtr<IMFMediaBuffer> buffer;
        const auto buffer_size = static_cast<DWORD>(samples.size_bytes());
        if (FAILED(MFCreateMemoryBuffer(buffer_size, &buffer))) return false;
        BYTE* destination = nullptr;
        if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) return false;
        memcpy(destination, samples.data(), samples.size_bytes());
        buffer->Unlock();
        buffer->SetCurrentLength(buffer_size);
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) return false;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(timestamp_100ns);
        sample->SetSampleDuration(static_cast<int64_t>(audio_chunk_frames) * 10'000'000 / audio_sample_rate);
        return SUCCEEDED(writer->WriteSample(stream_index, sample.Get()));
    }
}

VideoRecorder::~VideoRecorder()
{
    Stop();
    ReleaseCaptureSurfaces();
}

bool VideoRecorder::Start(std::filesystem::path output_path,
                                const uint32_t requested_fps,
                                const uint32_t requested_bitrate_mbps,
                                const bool should_capture_output_audio,
                                const bool should_capture_microphone,
                                const float requested_output_volume,
                                const float requested_microphone_volume)
{
    Stop();
    {
        const std::scoped_lock lock(mutex);
        if (!pending_source.empty()) {
            status = "Choose whether to save or discard the previous recording";
            return false;
        }
    }
    std::error_code error;
    std::filesystem::create_directories(output_path.parent_path(), error);
    if (error) {
        SetStatus("Could not create the recording directory: " + error.message());
        return false;
    }
    {
        const std::scoped_lock lock(mutex);
        final_path = std::move(output_path);
        initial_path = final_path.parent_path() / (final_path.stem().wstring() + L".partial" + final_path.extension().wstring());
        for (uint32_t suffix = 2; std::filesystem::exists(initial_path, error); ++suffix) {
            initial_path = final_path.parent_path()
                         / std::format(L"{}.partial_{}{}", final_path.stem().wstring(), suffix, final_path.extension().wstring());
        }
        fps = std::clamp(requested_fps, 1u, 60u);
        bitrate_mbps = std::clamp(requested_bitrate_mbps, 1u, 100u);
        capture_output_audio = should_capture_output_audio;
        capture_microphone = should_capture_microphone;
        output_volume = std::clamp(requested_output_volume, 0.f, 2.f);
        microphone_volume = std::clamp(requested_microphone_volume, 0.f, 2.f);
        frames.clear();
        stopping = false;
        recording = true;
        started_at = std::chrono::steady_clock::now();
        next_capture_at = started_at;
        status = "Waiting for the first video frame";
    }
    worker = std::thread([this] { WorkerEntry(); });
    return true;
}

void VideoRecorder::SetFinalPath(std::filesystem::path output_path)
{
    const std::scoped_lock lock(mutex);
    final_path = std::move(output_path);
}

void VideoRecorder::CaptureFrame(IDirect3DDevice9* device)
{
    if (!device || !IsRecording()) return;
    const auto now = std::chrono::steady_clock::now();
    {
        const std::scoped_lock lock(mutex);
        if (now < next_capture_at || frames.size() >= max_queued_frames) return;
        next_capture_at = now + std::chrono::milliseconds(1000 / fps);
    }

    ComPtr<IDirect3DSurface9> back_buffer;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer))) return;
    D3DSURFACE_DESC description{};
    if (FAILED(back_buffer->GetDesc(&description))) return;
    if (!EnsureCaptureSurfaces(device, description.Width, description.Height)) return;
    if (FAILED(device->StretchRect(back_buffer.Get(), nullptr, render_surface, nullptr, D3DTEXF_NONE))) return;
    if (FAILED(device->GetRenderTargetData(render_surface, system_surface))) return;

    D3DLOCKED_RECT locked{};
    if (FAILED(system_surface->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return;
    Frame frame;
    frame.width = description.Width;
    frame.height = description.Height;
    frame.timestamp_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_at).count() / 100;
    frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4);
    const auto row_bytes = static_cast<size_t>(frame.width) * 4;
    for (uint32_t row = 0; row < frame.height; ++row) {
        const auto destination_row = frame.height - row - 1;
        memcpy(frame.pixels.data() + destination_row * row_bytes,
               static_cast<const uint8_t*>(locked.pBits) + row * locked.Pitch,
               row_bytes);
    }
    system_surface->UnlockRect();
    {
        const std::scoped_lock lock(mutex);
        if (!stopping && frames.size() < max_queued_frames) frames.push_back(std::move(frame));
    }
    queue_changed.notify_one();
}

void VideoRecorder::Stop()
{
    {
        const std::scoped_lock lock(mutex);
        if (!recording && !worker.joinable()) return;
        stopping = true;
    }
    queue_changed.notify_all();
    if (worker.joinable()) worker.join();
    ReleaseCaptureSurfaces();
}

bool VideoRecorder::SavePending()
{
    std::filesystem::path source;
    std::filesystem::path destination;
    {
        const std::scoped_lock lock(mutex);
        if (pending_source.empty()) return false;
        source = pending_source;
        destination = pending_destination;
    }
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    const std::scoped_lock lock(mutex);
    if (error) {
        status = "Could not save the video: " + error.message();
        return false;
    }
    pending_source.clear();
    pending_destination.clear();
    status = "Video saved";
    return true;
}

bool VideoRecorder::DiscardPending()
{
    std::filesystem::path source;
    {
        const std::scoped_lock lock(mutex);
        if (pending_source.empty()) return false;
        source = pending_source;
    }
    std::error_code error;
    std::filesystem::remove(source, error);
    const std::scoped_lock lock(mutex);
    if (error) {
        status = "Could not discard the video: " + error.message();
        return false;
    }
    pending_source.clear();
    pending_destination.clear();
    status = "Recording discarded";
    return true;
}

bool VideoRecorder::IsRecording() const
{
    const std::scoped_lock lock(mutex);
    return recording;
}

bool VideoRecorder::HasPendingRecording() const
{
    const std::scoped_lock lock(mutex);
    return !pending_source.empty();
}

std::filesystem::path VideoRecorder::PendingPath() const
{
    const std::scoped_lock lock(mutex);
    return pending_destination;
}

std::string VideoRecorder::Status() const
{
    const std::scoped_lock lock(mutex);
    return status;
}

std::chrono::steady_clock::duration VideoRecorder::RecordingDuration() const
{
    const std::scoped_lock lock(mutex);
    return recording ? std::chrono::steady_clock::now() - started_at : std::chrono::steady_clock::duration::zero();
}

bool VideoRecorder::EnsureCaptureSurfaces(IDirect3DDevice9* device, const uint32_t width, const uint32_t height)
{
    if (render_surface && system_surface && surface_width == width && surface_height == height) return true;
    ReleaseCaptureSurfaces();
    if (FAILED(device->CreateRenderTarget(width, height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &render_surface, nullptr))) {
        SetStatus("DirectX could not create the capture surface");
        return false;
    }
    if (FAILED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &system_surface, nullptr))) {
        ReleaseCaptureSurfaces();
        SetStatus("DirectX could not create the readback surface");
        return false;
    }
    surface_width = width;
    surface_height = height;
    return true;
}

void VideoRecorder::ReleaseCaptureSurfaces()
{
    if (render_surface) render_surface->Release();
    if (system_surface) system_surface->Release();
    render_surface = nullptr;
    system_surface = nullptr;
    surface_width = 0;
    surface_height = 0;
}

void VideoRecorder::WorkerEntry()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto mf_started = SUCCEEDED(MFStartup(MF_VERSION));
    WasapiSource output_source;
    WasapiSource microphone_source;
    const auto output_ready = capture_output_audio && output_source.Initialize(eRender);
    const auto microphone_ready = capture_microphone && microphone_source.Initialize(eCapture);
    const auto include_audio = output_ready || microphone_ready;
    std::deque<float> output_samples;
    std::deque<float> microphone_samples;
    ComPtr<IMFSinkWriter> writer;
    DWORD video_stream_index = 0;
    DWORD audio_stream_index = 0;
    int64_t audio_frames_written = 0;
    bool writer_ready = false;
    bool failed = !mf_started;
    while (!failed) {
        Frame frame;
        bool has_video_frame = false;
        bool should_stop = false;
        {
            std::unique_lock lock(mutex);
            queue_changed.wait_for(lock, 10ms, [this] { return stopping || !frames.empty(); });
            if (!frames.empty()) {
                frame = std::move(frames.front());
                frames.pop_front();
                has_video_frame = true;
            }
            should_stop = stopping && frames.empty();
        }

        if (output_ready) output_source.Drain(output_samples);
        if (microphone_ready) microphone_source.Drain(microphone_samples);

        if (has_video_frame && !writer_ready) {
            writer_ready = ConfigureWriter(initial_path,
                                           frame.width,
                                           frame.height,
                                           fps,
                                           bitrate_mbps,
                                           include_audio,
                                           writer,
                                           video_stream_index,
                                           audio_stream_index);
            if (!writer_ready) {
                failed = true;
                break;
            }
            SetStatus(include_audio ? "Recording H.264 video and AAC audio" : "Recording H.264 video (audio unavailable)");
        }
        if (has_video_frame && writer_ready && !WriteFrame(writer.Get(), video_stream_index, frame, fps)) failed = true;

        const auto chunk_samples = static_cast<size_t>(audio_chunk_frames) * audio_channels;
        while (!failed && writer_ready && include_audio
               && (!output_ready || output_samples.size() >= chunk_samples)
               && (!microphone_ready || microphone_samples.size() >= chunk_samples)) {
            std::vector<float> mixed(chunk_samples, 0.f);
            for (size_t i = 0; i < chunk_samples; ++i) {
                if (output_ready) {
                    mixed[i] += output_samples.front() * output_volume;
                    output_samples.pop_front();
                }
                if (microphone_ready) {
                    mixed[i] += microphone_samples.front() * microphone_volume;
                    microphone_samples.pop_front();
                }
                mixed[i] = std::clamp(mixed[i], -1.f, 1.f);
            }
            const auto timestamp = audio_frames_written * 10'000'000 / audio_sample_rate;
            if (!WriteAudio(writer.Get(), audio_stream_index, mixed, timestamp)) failed = true;
            audio_frames_written += audio_chunk_frames;
        }
        if (should_stop) break;
    }
    output_source.Stop();
    microphone_source.Stop();
    if (writer_ready) writer->Finalize();
    writer.Reset();
    if (mf_started) MFShutdown();
    CoUninitialize();

    std::filesystem::path source;
    std::filesystem::path destination;
    {
        const std::scoped_lock lock(mutex);
        source = initial_path;
        destination = final_path;
    }
    {
        const std::scoped_lock lock(mutex);
        recording = false;
        if (failed) status = "Video encoder failed";
        else if (!writer_ready) status = "No video frames were captured";
        else {
            pending_source = std::move(source);
            pending_destination = std::move(destination);
            status = "Awaiting save confirmation";
        }
    }
}

void VideoRecorder::SetStatus(std::string value)
{
    const std::scoped_lock lock(mutex);
    status = std::move(value);
}
