#pragma once

#include <ToolboxUIPlugin.h>
#include <IconsFontAwesome5.h>

class ObsRecorder final : public ToolboxUIPlugin {
public:
    const char* Name() const override { return "OBS Recorder"; }
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
    class ObsClient;

    bool BeginMapRecording();
    void EndMapRecording();

    std::unique_ptr<ObsClient> obs;
    bool automatic_recording = true;
    bool restrict_to_selected_maps = true;
    bool show_indicator = true;
    uint32_t warning_minutes = 120;
    std::string obs_host = "127.0.0.1";
    uint32_t obs_port = 4455;
    std::string obs_password;
    bool was_recordable = false;
    bool pending_map_name = false;
    bool recording_requested = false;
    bool duration_warning_shown = false;
    uint32_t new_map_id = 0;
    std::vector<uint32_t> available_map_ids{307, 266, 72, 34, 474};
    std::vector<uint32_t> enabled_map_ids{307, 266, 72, 34, 474};
    uint32_t recording_map_id = 0;
    std::wstring recording_character;
    wchar_t encoded_map_name[8]{};
    wchar_t decoded_map_name[128]{};
};
