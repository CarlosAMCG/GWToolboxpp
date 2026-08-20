#pragma once

#include <ToolboxUIPlugin.h>
#include <IconsFontAwesome5.h>

class Recorder final : public ToolboxUIPlugin {
public:
    const char* Name() const override { return "Recorder"; }
    const char* Icon() const override { return ICON_FA_VIDEO; }
    bool HasSettings() const override { return true; }
    bool* GetVisiblePtr() override;

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void SignalTerminate() override;
    bool CanTerminate() override;
    void Terminate() override;
    void Update(float delta) override;
    void Draw(IDirect3DDevice9*) override;
    void DrawSettings() override;
    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;

private:
    bool BeginMapRecording();
    void EndMapRecording();
    void DrawSaveConfirmation();

    std::unique_ptr<class VideoRecorder> recorder;
    bool automatic_recording = true;
    bool restrict_to_selected_maps = true;
    bool show_indicator = true;
    bool confirm_before_saving = true;
    bool render_enabled = true;
    uint32_t fps = 30;
    uint32_t bitrate_mbps = 8;
    bool capture_output_audio = true;
    bool capture_microphone = true;
    float output_audio_volume = 1.f;
    float microphone_volume = 1.f;
    uint32_t warning_minutes = 120;
    std::string output_directory;
    bool was_recordable = false;
    bool pending_map_name = false;
    bool recording_requested = false;
    bool duration_warning_shown = false;
    uint32_t new_map_id = 0;
    std::vector<uint32_t> available_map_ids{307, 266, 72, 34, 474};
    std::vector<uint32_t> enabled_map_ids{307, 266, 72, 34, 474};
    uint32_t recording_map_id = 0;
    std::wstring recording_character;
    std::wstring recording_stem;
    wchar_t encoded_map_name[8]{};
    wchar_t decoded_map_name[128]{};
};
