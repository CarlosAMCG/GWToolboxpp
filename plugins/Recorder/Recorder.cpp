#include "Recorder.h"
#include "VideoRecorder.h"

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <MinHook.h>

#include <PluginUtils.h>

namespace RecorderDetail {
    using namespace std::chrono_literals;

    using PresentCallback = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    PresentCallback present_target = nullptr;
    PresentCallback present_original = nullptr;
    Recorder* active_recorder = nullptr;

    HRESULT __stdcall OnPresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                const HWND window, const RGNDATA* dirty_region)
    {
        if (active_recorder) active_recorder->CapturePresentedFrame(device);
        return present_original(device, source, destination, window, dirty_region);
    }

    void InstallPresentHook(IDirect3DDevice9* device)
    {
        if (present_target || !device) return;
        const auto vtable = *reinterpret_cast<void***>(device);
        present_target = reinterpret_cast<PresentCallback>(vtable[17]);
        MH_Initialize();
        if (MH_CreateHook(reinterpret_cast<void*>(present_target), OnPresent,
                          reinterpret_cast<void**>(&present_original)) != MH_OK) {
            present_target = nullptr;
            present_original = nullptr;
            return;
        }
        MH_EnableHook(reinterpret_cast<void*>(present_target));
    }

    void RemovePresentHook()
    {
        active_recorder = nullptr;
        if (!present_target) return;
        MH_DisableHook(reinterpret_cast<void*>(present_target));
        MH_RemoveHook(reinterpret_cast<void*>(present_target));
        present_target = nullptr;
        present_original = nullptr;
    }

    constexpr std::array default_maps{
        std::pair{307u, "Deep"},
        std::pair{266u, "Urgoz"},
        std::pair{72u, "UW"},
        std::pair{34u, "FoW"},
        std::pair{474u, "DoA"},
    };

    const char* DefaultMapLabel(const uint32_t map_id)
    {
        const auto found = std::ranges::find(default_maps, map_id, &decltype(default_maps)::value_type::first);
        return found == default_maps.end() ? nullptr : found->second;
    }

    std::wstring SanitizeFilePart(std::wstring value)
    {
        constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
        for (auto& c : value) {
            if (invalid.contains(c) || c < 32) c = L'_';
        }
        while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) value.pop_back();
        return value.empty() ? L"Unknown" : value;
    }

    std::wstring RecordingStem(const std::wstring& character, const std::wstring& map_name, const uint32_t map_id)
    {
        const auto now = std::chrono::system_clock::now();
        const auto timestamp = std::format(L"{:%Y-%m-%d_%H-%M-%S}", now);
        auto zone = SanitizeFilePart(map_name);
        if (zone == L"Map") zone = std::format(L"Map-{}", map_id);
        return std::format(L"{}__{}__{}", zone, SanitizeFilePart(character), timestamp);
    }

    std::filesystem::path DefaultOutputDirectory()
    {
        PWSTR videos_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos_path))) {
            const auto result = std::filesystem::path(videos_path) / L"GWToolbox Recordings";
            CoTaskMemFree(videos_path);
            return result;
        }
        return std::filesystem::current_path() / L"GWToolbox Recordings";
    }

    std::filesystem::path VideoPath(const std::string& configured_directory, const std::wstring& stem)
    {
        const auto directory = configured_directory.empty()
            ? DefaultOutputDirectory()
            : std::filesystem::path(PluginUtils::StringToWString(configured_directory));
        auto path = directory / (stem + L".mp4");
        std::error_code error;
        for (uint32_t suffix = 2; std::filesystem::exists(path, error); ++suffix) {
            path = directory / std::format(L"{}_{}.mp4", stem, suffix);
        }
        return path;
    }
}

using namespace RecorderDetail;

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static Recorder instance;
    return &instance;
}

bool* Recorder::GetVisiblePtr()
{
    const auto window = ImGui::GetCurrentContext() ? ImGui::GetCurrentWindowRead() : nullptr;
    if (window && strcmp(window->Name, "Settings") == 0) return nullptr;
    return &render_enabled;
}

bool Recorder::DrawTabButton(const bool show_icon, const bool show_text, const bool center_align_text)
{
    ImGui::PushStyleColor(ImGuiCol_Button,
                          recorder_window_visible ? ImGui::GetStyle().Colors[ImGuiCol_Button] : ImVec4(0, 0, 0, 0));
    const auto position = ImGui::GetCursorScreenPos();
    const auto text_size = ImGui::CalcTextSize(Name());
    const auto width = ImGui::GetContentRegionAvail().x;
    const auto icon_size = show_icon ? ImGui::GetTextLineHeightWithSpacing() : 0.f;
    const auto text_x = center_align_text
        ? position.x + icon_size + (width - icon_size - text_size.x) / 2
        : position.x + icon_size + ImGui::GetStyle().ItemSpacing.x;
    const auto clicked = ImGui::Button("##recorder_main_bar", ImVec2(width, ImGui::GetTextLineHeightWithSpacing()));
    if (show_icon) {
        ImGui::GetWindowDrawList()->AddText(ImVec2(position.x, position.y + ImGui::GetStyle().ItemSpacing.y / 2),
                                            ImGui::GetColorU32(ImGuiCol_Text), Icon());
    }
    if (show_text) {
        ImGui::GetWindowDrawList()->AddText(ImVec2(text_x, position.y + ImGui::GetStyle().ItemSpacing.y / 2),
                                            ImGui::GetColorU32(ImGuiCol_Text), Name());
    }
    ImGui::PopStyleColor();
    if (clicked) recorder_window_visible = !recorder_window_visible;
    return clicked;
}

void Recorder::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    recorder = std::make_unique<VideoRecorder>();
    active_recorder = this;
}

void Recorder::SignalTerminate()
{
    ToolboxPlugin::SignalTerminate();
    RemovePresentHook();
    if (recorder) {
        recorder->Stop();
        if (recorder->HasPendingRecording()) recorder->SavePending();
    }
}

bool Recorder::CanTerminate()
{
    return true;
}

void Recorder::Terminate()
{
    recorder.reset();
    ToolboxPlugin::Terminate();
}

bool Recorder::BeginMapRecording()
{
    if (!recorder || !GW::Map::GetIsMapLoaded() || recorder->IsRecording() || recorder->HasPendingRecording()) return false;
    const auto map = GW::Map::GetMapInfo(GW::Map::GetMapID());
    const auto player_name = GW::PlayerMgr::GetPlayerName(GW::PlayerMgr::GetPlayerNumber());
    if (!map || !map->name_id || !player_name || !*player_name) return false;
    recording_map_id = std::to_underlying(GW::Map::GetMapID());
    recording_character = player_name;
    recording_requested = true;
    duration_warning_shown = false;
    recording_stem = RecordingStem(recording_character, L"Map", recording_map_id);
    if (!recorder->Start(VideoPath(output_directory, recording_stem),
                                fps,
                                bitrate_mbps,
                                capture_output_audio,
                                capture_microphone,
                                output_audio_volume,
                                microphone_volume)) {
        recording_requested = false;
        return false;
    }
    encoded_map_name[0] = 0;
    decoded_map_name[0] = 0;
    if (!GW::UI::UInt32ToEncStr(map->name_id, encoded_map_name, _countof(encoded_map_name))) return true;
    GW::UI::AsyncDecodeStr(encoded_map_name, decoded_map_name, _countof(decoded_map_name));
    pending_map_name = true;
    return true;
}

void Recorder::EndMapRecording()
{
    manual_recording = false;
    pending_map_name = false;
    recording_requested = false;
    duration_warning_shown = false;
    if (recorder) {
        recorder->Stop();
        if (!confirm_before_saving && recorder->HasPendingRecording()) recorder->SavePending();
    }
}

void Recorder::DrawSaveConfirmation()
{
    if (!recorder || !recorder->HasPendingRecording()) return;
    ImGui::OpenPopup("Save recording?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    if (!ImGui::BeginPopupModal("Save recording?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const auto filename = PluginUtils::WStringToString(recorder->PendingPath().filename().wstring());
    ImGui::TextUnformatted("The recording has finished.");
    ImGui::Spacing();
    ImGui::Text("Save %s?", filename.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Save", ImVec2(120.f, 0.f))) {
        if (recorder->SavePending()) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(120.f, 0.f))) {
        if (recorder->DiscardPending()) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Recorder::DrawRecorderWindow()
{
    if (!recorder_window_visible) return;
    ImGui::SetNextWindowSize(ImVec2(280.f, 0.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Name(), &recorder_window_visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto is_recording = recorder && recorder->IsRecording();
        ImGui::Checkbox("Record selected areas automatically", &automatic_recording);
        ImGui::TextDisabled("Uses the area selection configured in Recorder settings.");
        ImGui::Separator();
        ImGui::Text("Status: %s", recorder ? recorder->Status().c_str() : "Unavailable");
        if (is_recording) {
            ImGui::Text("Mode: %s", manual_recording ? "Manual" : "Automatic");
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(recorder->RecordingDuration()).count();
            ImGui::Text("REC  %02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
            if (ImGui::Button("Stop recording")) EndMapRecording();
        }
        else if (recorder && !recorder->HasPendingRecording() && GW::Map::GetIsMapLoaded()) {
            if (ImGui::Button("Start recording") && BeginMapRecording()) {
                manual_recording = true;
                was_recordable = true;
            }
        }
        else if (recorder && recorder->HasPendingRecording()) {
            ImGui::TextDisabled("Choose whether to save or discard the previous recording.");
        }
    }
    ImGui::End();
}

void Recorder::Update(float)
{
    const auto current_map_id = static_cast<uint32_t>(std::to_underlying(GW::Map::GetMapID()));
    const auto map_selected = !restrict_to_selected_maps || std::ranges::contains(enabled_map_ids, current_map_id);
    const auto recordable = automatic_recording
        && map_selected
        && GW::Map::GetIsMapLoaded()
        && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable
        && !GW::Map::GetIsObserving();
    if (manual_recording) {
        if (!GW::Map::GetIsMapLoaded() || current_map_id != recording_map_id) {
            EndMapRecording();
            was_recordable = false;
        }
    }
    else {
        if (recordable && !was_recordable && !recording_requested) was_recordable = BeginMapRecording();
        if (!recordable && was_recordable) EndMapRecording();
        if (!recordable) was_recordable = false;
    }
    if (pending_map_name && decoded_map_name[0]) {
        pending_map_name = false;
        recording_stem = RecordingStem(recording_character, decoded_map_name, recording_map_id);
        recorder->SetFinalPath(VideoPath(output_directory, recording_stem));
    }
    const auto is_recording = recorder && recorder->IsRecording();
    const auto duration = recorder->RecordingDuration();
    if (is_recording && warning_minutes && !duration_warning_shown
        && duration >= std::chrono::minutes(warning_minutes)) {
        duration_warning_shown = true;
        GW::Chat::WriteChat(GW::Chat::Channel::CHANNEL_WARNING,
                            std::format(L"Recorder has been running for more than {} minutes.", warning_minutes).c_str());
    }
}

void Recorder::Draw(IDirect3DDevice9* device)
{
    InstallPresentHook(device);
    if (confirm_before_saving) DrawSaveConfirmation();
    DrawRecorderWindow();
    const auto is_recording = recorder && recorder->IsRecording();
    if (!show_indicator || !is_recording) return;
    const auto duration = recorder->RecordingDuration();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("Recorder indicator", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        const auto warning = warning_minutes && seconds >= static_cast<int64_t>(warning_minutes) * 60;
        const auto label = std::format("REC  {:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
        const auto position = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##drag_recorder_indicator", ImGui::CalcTextSize(label.c_str()));
        ImGui::GetWindowDrawList()->AddText(position,
                                            ImGui::ColorConvertFloat4ToU32(warning
                                                ? ImVec4(1.f, .65f, 0.f, 1.f)
                                                : ImVec4(1.f, .2f, .2f, 1.f)),
                                            label.c_str());
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const auto& delta = ImGui::GetIO().MouseDelta;
            const auto window_position = ImGui::GetWindowPos();
            ImGui::SetWindowPos(ImVec2(window_position.x + delta.x, window_position.y + delta.y));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) EndMapRecording();
    }
    ImGui::End();
}

void Recorder::CapturePresentedFrame(IDirect3DDevice9* device)
{
    if (recorder) recorder->CaptureFrame(device);
}

void Recorder::DrawSettings()
{
    ImGui::TextDisabled("Recorder 1.1.0 Preview");
    if (recorder && recorder->IsRecording()) {
        if (ImGui::Button("Stop recording")) EndMapRecording();
    }
    else if (recorder && !recorder->HasPendingRecording() && GW::Map::GetIsMapLoaded()) {
        if (ImGui::Button("Start recording") && BeginMapRecording()) {
            manual_recording = true;
            was_recordable = true;
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("AUTOMATIC RECORDING");
    ImGui::Checkbox("Record explorable areas automatically", &automatic_recording);
    ImGui::Checkbox("Only record selected map IDs", &restrict_to_selected_maps);
    if (restrict_to_selected_maps) {
        ImGui::Indent();
        for (const auto map_id : available_map_ids) {
            ImGui::PushID(static_cast<int>(map_id));
            auto enabled = std::ranges::contains(enabled_map_ids, map_id);
            const auto preset_label = DefaultMapLabel(map_id);
            const auto checkbox_label = preset_label ? preset_label : std::format("Custom map {}", map_id);
            if (ImGui::Checkbox(checkbox_label.c_str(), &enabled)) {
                if (enabled) {
                    enabled_map_ids.push_back(map_id);
                }
                else {
                    std::erase(enabled_map_ids, map_id);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("ID %u", map_id);
            if (!preset_label) {
                ImGui::SameLine();
            }
            if (!preset_label && ImGui::SmallButton("Remove")) {
                std::erase(enabled_map_ids, map_id);
                std::erase(available_map_ids, map_id);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("ADD ANOTHER AREA");
        ImGui::SetNextItemWidth(140.f);
        ImGui::InputScalar("New map ID", ImGuiDataType_U32, &new_map_id);
        ImGui::SameLine();
        if (ImGui::Button("Add") && new_map_id && !std::ranges::contains(available_map_ids, new_map_id)) {
            available_map_ids.push_back(new_map_id);
            enabled_map_ids.push_back(new_map_id);
            new_map_id = 0;
        }
        const auto current_map_id = static_cast<uint32_t>(std::to_underlying(GW::Map::GetMapID()));
        if (current_map_id && !std::ranges::contains(available_map_ids, current_map_id)
            && ImGui::Button(std::format("Add current map ({})", current_map_id).c_str())) {
            available_map_ids.push_back(current_map_id);
            enabled_map_ids.push_back(current_map_id);
        }
        ImGui::Unindent();
    }
    ImGui::Spacing();
    ImGui::Checkbox("Show REC indicator", &show_indicator);
    ImGui::Checkbox("Show in main window", &show_in_main_bar);
    ImGui::Checkbox("Ask before saving completed recordings", &confirm_before_saving);
    ImGui::SetNextItemWidth(140.f);
    ImGui::InputScalar("Long recording warning (minutes)", ImGuiDataType_U32, &warning_minutes);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("VIDEO");
    std::array<char, 512> directory_buffer{};
    strncpy_s(directory_buffer.data(), directory_buffer.size(), output_directory.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(360.f);
    if (ImGui::InputText("Output directory", directory_buffer.data(), directory_buffer.size())) {
        output_directory = directory_buffer.data();
    }
    ImGui::TextDisabled("Leave empty to use Videos\\GWToolbox Recordings.");
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputScalar("Frames per second", ImGuiDataType_U32, &fps);
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputScalar("Bitrate (Mbps)", ImGuiDataType_U32, &bitrate_mbps);
    ImGui::Spacing();
    ImGui::TextDisabled("AUDIO");
    ImGui::Checkbox("Capture headphones / system output", &capture_output_audio);
    if (capture_output_audio) {
        ImGui::SetNextItemWidth(220.f);
        ImGui::SliderFloat("Output volume", &output_audio_volume, 0.f, 2.f, "%.2fx", ImGuiSliderFlags_None);
    }
    ImGui::Checkbox("Capture microphone", &capture_microphone);
    if (capture_microphone) {
        ImGui::SetNextItemWidth(220.f);
        ImGui::SliderFloat("Microphone volume", &microphone_volume, 0.f, 2.f, "%.2fx", ImGuiSliderFlags_None);
    }
    ImGui::TextDisabled("Uses the default Windows output and input devices.");
    if (recorder) ImGui::Text("Status: %s", recorder->Status().c_str());
}

void Recorder::LoadSettings(const wchar_t* folder)
{
    ToolboxPlugin::LoadSettings(folder);
    LoadSetting("automatic_recording", automatic_recording);
    LoadSetting("restrict_to_selected_maps", restrict_to_selected_maps);
    LoadSetting("available_map_ids", available_map_ids);
    LoadSetting("enabled_map_ids", enabled_map_ids);
    LoadSetting("show_indicator", show_indicator);
    LoadSetting("show_in_main_bar", show_in_main_bar);
    LoadSetting("recorder_window_visible", recorder_window_visible);
    LoadSetting("confirm_before_saving", confirm_before_saving);
    LoadSetting("fps", fps);
    LoadSetting("bitrate_mbps", bitrate_mbps);
    LoadSetting("output_directory", output_directory);
    LoadSetting("capture_output_audio", capture_output_audio);
    LoadSetting("capture_microphone", capture_microphone);
    LoadSetting("output_audio_volume", output_audio_volume);
    LoadSetting("microphone_volume", microphone_volume);
    LoadSetting("warning_minutes", warning_minutes);
}

void Recorder::SaveSettings(const wchar_t* folder)
{
    SaveSetting("automatic_recording", automatic_recording);
    SaveSetting("restrict_to_selected_maps", restrict_to_selected_maps);
    SaveSetting("available_map_ids", available_map_ids);
    SaveSetting("enabled_map_ids", enabled_map_ids);
    SaveSetting("show_indicator", show_indicator);
    SaveSetting("show_in_main_bar", show_in_main_bar);
    SaveSetting("recorder_window_visible", recorder_window_visible);
    SaveSetting("confirm_before_saving", confirm_before_saving);
    SaveSetting("fps", fps);
    SaveSetting("bitrate_mbps", bitrate_mbps);
    SaveSetting("output_directory", output_directory);
    SaveSetting("capture_output_audio", capture_output_audio);
    SaveSetting("capture_microphone", capture_microphone);
    SaveSetting("output_audio_volume", output_audio_volume);
    SaveSetting("microphone_volume", microphone_volume);
    SaveSetting("warning_minutes", warning_minutes);
    ToolboxPlugin::SaveSettings(folder);
}
