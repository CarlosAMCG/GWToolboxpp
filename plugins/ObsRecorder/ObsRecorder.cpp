#include "ObsRecorder.h"

#include <bcrypt.h>
#include <easywsclient.hpp>
#include <glaze/glaze.hpp>
#include <optional>
#include <span>

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <PluginUtils.h>

namespace ObsRecorderDetail {
    using namespace std::chrono_literals;

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

    struct ObsAuthentication {
        std::string challenge;
        std::string salt;
    };

    struct ObsRequestStatus {
        bool result = false;
        int code = 0;
        std::string comment;
    };

    struct ObsResponseData {
        std::string outputPath;
    };

    struct ObsData {
        int rpcVersion = 1;
        std::optional<ObsAuthentication> authentication;
        std::string requestType;
        std::string requestId;
        std::optional<ObsRequestStatus> requestStatus;
        std::optional<ObsResponseData> responseData;
    };

    struct ObsMessage {
        int op = -1;
        ObsData d;
    };

    constexpr auto json_options = glz::opts{.error_on_unknown_keys = false};

    std::string Base64(const std::span<const uint8_t> bytes)
    {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve((bytes.size() + 2) / 3 * 4);
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const auto remaining = bytes.size() - i;
            const auto value = static_cast<uint32_t>(bytes[i]) << 16
                             | (remaining > 1 ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0)
                             | (remaining > 2 ? bytes[i + 2] : 0);
            result.push_back(alphabet[(value >> 18) & 0x3f]);
            result.push_back(alphabet[(value >> 12) & 0x3f]);
            result.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=');
            result.push_back(remaining > 2 ? alphabet[value & 0x3f] : '=');
        }
        return result;
    }

    std::array<uint8_t, 32> Sha256(const std::string_view input)
    {
        std::array<uint8_t, 32> digest{};
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD object_size = 0;
        DWORD copied = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0) < 0) {
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            return digest;
        }
        std::vector<uint8_t> object(object_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0);
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        }
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return digest;
    }

    std::string MakeAuthentication(const std::string& password, const ObsAuthentication& auth)
    {
        const auto secret_hash = Sha256(password + auth.salt);
        const auto secret = Base64(secret_hash);
        return Base64(Sha256(secret + auth.challenge));
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
}

using namespace ObsRecorderDetail;

class ObsRecorder::ObsClient {
public:
    ~ObsClient() { Stop(); }

    void Start(const std::string& host, const uint32_t port, const std::string& connection_password)
    {
        Stop();
        {
            const std::scoped_lock lock(mutex);
            url = std::format("ws://{}:{}", host, port);
            password = connection_password;
            stopping = false;
            status = "Connecting to OBS...";
        }
        worker = std::thread([this] { Run(); });
    }

    void Stop()
    {
        {
            const std::scoped_lock lock(mutex);
            stopping = true;
            wants_recording = false;
        }
        if (worker.joinable()) worker.join();
    }

    void SetRecording(const bool desired, std::wstring stem = {})
    {
        const std::scoped_lock lock(mutex);
        wants_recording = desired;
        if (!stem.empty()) {
            desired_stem = std::move(stem);
            if (recording) active_stem = desired_stem;
        }
    }

    [[nodiscard]] bool IsRecording() const
    {
        const std::scoped_lock lock(mutex);
        return recording;
    }

    [[nodiscard]] std::string Status() const
    {
        const std::scoped_lock lock(mutex);
        return status;
    }

    [[nodiscard]] std::chrono::steady_clock::duration RecordingDuration() const
    {
        const std::scoped_lock lock(mutex);
        return recording ? std::chrono::steady_clock::now() - started_at : std::chrono::steady_clock::duration::zero();
    }

private:
    void SetStatus(std::string value)
    {
        const std::scoped_lock lock(mutex);
        status = std::move(value);
    }

    void HandleMessage(easywsclient::WebSocket* socket, const std::string& raw)
    {
        ObsMessage message;
        if (glz::read<json_options>(message, raw)) return;
        if (message.op == 0) {
            const auto authentication = message.d.authentication
                ? std::format(",\"authentication\":\"{}\"", MakeAuthentication(password, *message.d.authentication))
                : std::string{};
            socket->send(std::format("{{\"op\":1,\"d\":{{\"rpcVersion\":1{}}}}}", authentication));
        }
        else if (message.op == 2) {
            identified = true;
            SetStatus("Connected to OBS");
        }
        else if (message.op == 7 && message.d.requestStatus) {
            const auto success = message.d.requestStatus->result;
            if (message.d.requestId == "start") {
                start_pending = false;
                if (success) {
                    const std::scoped_lock lock(mutex);
                    recording = true;
                    active_stem = desired_stem;
                    started_at = std::chrono::steady_clock::now();
                    status = "Recording";
                }
                else {
                    SetStatus("OBS could not start recording: " + message.d.requestStatus->comment);
                }
            }
            else if (message.d.requestId == "stop") {
                stop_pending = false;
                {
                    const std::scoped_lock lock(mutex);
                    recording = false;
                    status = success ? "Recording saved" : "OBS could not stop recording: " + message.d.requestStatus->comment;
                }
                if (success && message.d.responseData && !message.d.responseData->outputPath.empty()) {
                    RenameRecording(message.d.responseData->outputPath);
                }
            }
        }
    }

    void RenameRecording(const std::string& output_path)
    {
        const std::filesystem::path source = PluginUtils::StringToWString(output_path);
        std::error_code error;
        auto destination = source.parent_path() / (active_stem + source.extension().wstring());
        for (uint32_t suffix = 2; std::filesystem::exists(destination, error); ++suffix) {
            destination = source.parent_path() / std::format(L"{}_{}{}", active_stem, suffix, source.extension().wstring());
        }
        for (uint32_t attempt = 0; attempt < 50; ++attempt) {
            error.clear();
            std::filesystem::rename(source, destination, error);
            if (!error) {
                SetStatus("Recording saved and renamed");
                return;
            }
            std::this_thread::sleep_for(100ms);
        }
        SetStatus(std::format("Recording saved, but rename failed: {}", error.message()));
    }

    void Reconcile(easywsclient::WebSocket* socket)
    {
        bool desired;
        bool current;
        {
            const std::scoped_lock lock(mutex);
            desired = wants_recording;
            current = recording;
        }
        if (desired && !current && !start_pending) {
            socket->send(R"({"op":6,"d":{"requestType":"StartRecord","requestId":"start"}})");
            start_pending = true;
        }
        else if (!desired && current && !stop_pending) {
            socket->send(R"({"op":6,"d":{"requestType":"StopRecord","requestId":"stop"}})");
            stop_pending = true;
        }
    }

    void Run()
    {
        WSADATA winsock{};
        WSAStartup(MAKEWORD(2, 2), &winsock);
        while (true) {
            {
                const std::scoped_lock lock(mutex);
                if (stopping) break;
            }
            auto* socket = easywsclient::WebSocket::from_url(url);
            if (!socket) {
                SetStatus("OBS is unavailable; retrying...");
                std::this_thread::sleep_for(3s);
                continue;
            }
            identified = false;
            auto shutdown_started = std::chrono::steady_clock::time_point{};
            while (socket->getReadyState() != easywsclient::WebSocket::CLOSED) {
                socket->poll(100);
                socket->dispatch([this, socket](const std::string& raw) { HandleMessage(socket, raw); });
                if (identified) Reconcile(socket);
                const std::scoped_lock lock(mutex);
                if (stopping) {
                    if (shutdown_started == std::chrono::steady_clock::time_point{}) {
                        shutdown_started = std::chrono::steady_clock::now();
                    }
                    if (recording && !stop_pending) {
                        socket->send(R"({"op":6,"d":{"requestType":"StopRecord","requestId":"stop"}})");
                        stop_pending = true;
                    }
                    if (!recording || std::chrono::steady_clock::now() - shutdown_started > 3s) socket->close();
                }
            }
            delete socket;
            {
                const std::scoped_lock lock(mutex);
                if (stopping) break;
                recording = false;
                start_pending = false;
                stop_pending = false;
                status = "OBS connection lost; retrying...";
            }
        }
        WSACleanup();
    }

    mutable std::mutex mutex;
    std::thread worker;
    std::string url;
    std::string password;
    std::string status = "Not connected";
    std::wstring desired_stem;
    std::wstring active_stem;
    std::chrono::steady_clock::time_point started_at{};
    bool stopping = false;
    bool wants_recording = false;
    bool recording = false;
    bool identified = false;
    bool start_pending = false;
    bool stop_pending = false;
};

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static ObsRecorder instance;
    return &instance;
}

bool* ObsRecorder::GetVisiblePtr()
{
    const auto window = ImGui::GetCurrentContext() ? ImGui::GetCurrentWindowRead() : nullptr;
    if (window && strcmp(window->Name, "Settings") == 0) return nullptr;
    return &show_indicator;
}

void ObsRecorder::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    obs = std::make_unique<ObsClient>();
}

void ObsRecorder::SignalTerminate()
{
    ToolboxPlugin::SignalTerminate();
    if (obs) obs->Stop();
}

bool ObsRecorder::CanTerminate()
{
    return true;
}

void ObsRecorder::Terminate()
{
    obs.reset();
    ToolboxPlugin::Terminate();
}

bool ObsRecorder::BeginMapRecording()
{
    const auto map = GW::Map::GetMapInfo(GW::Map::GetMapID());
    const auto player_name = GW::PlayerMgr::GetPlayerName(GW::PlayerMgr::GetPlayerNumber());
    if (!map || !map->name_id || !player_name || !*player_name) return false;
    recording_map_id = std::to_underlying(GW::Map::GetMapID());
    recording_character = player_name;
    recording_requested = true;
    duration_warning_shown = false;
    obs->SetRecording(true, RecordingStem(recording_character, L"Map", recording_map_id));
    encoded_map_name[0] = 0;
    decoded_map_name[0] = 0;
    if (!GW::UI::UInt32ToEncStr(map->name_id, encoded_map_name, _countof(encoded_map_name))) return true;
    GW::UI::AsyncDecodeStr(encoded_map_name, decoded_map_name, _countof(decoded_map_name));
    pending_map_name = true;
    return true;
}

void ObsRecorder::EndMapRecording()
{
    pending_map_name = false;
    recording_requested = false;
    duration_warning_shown = false;
    if (obs) obs->SetRecording(false);
}

void ObsRecorder::Update(float)
{
    const auto current_map_id = static_cast<uint32_t>(std::to_underlying(GW::Map::GetMapID()));
    const auto map_selected = !restrict_to_selected_maps || std::ranges::contains(enabled_map_ids, current_map_id);
    const auto recordable = automatic_recording
        && map_selected
        && GW::Map::GetIsMapLoaded()
        && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable
        && !GW::Map::GetIsObserving();
    if (recordable && !was_recordable && !recording_requested) was_recordable = BeginMapRecording();
    if (!recordable && was_recordable) EndMapRecording();
    if (!recordable) was_recordable = false;
    if (pending_map_name && decoded_map_name[0]) {
        pending_map_name = false;
        obs->SetRecording(true, RecordingStem(recording_character, decoded_map_name, recording_map_id));
    }
    if (obs && obs->IsRecording() && warning_minutes && !duration_warning_shown
        && obs->RecordingDuration() >= std::chrono::minutes(warning_minutes)) {
        duration_warning_shown = true;
        GW::Chat::WriteChat(GW::Chat::Channel::CHANNEL_WARNING,
                            std::format(L"OBS has been recording for more than {} minutes.", warning_minutes).c_str());
    }
}

void ObsRecorder::Draw(IDirect3DDevice9*)
{
    if (!show_indicator || !obs || !obs->IsRecording()) return;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(obs->RecordingDuration()).count();
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("OBS recording indicator", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        const auto warning = warning_minutes && seconds >= static_cast<int64_t>(warning_minutes) * 60;
        ImGui::TextColored(warning ? ImVec4(1.f, .65f, 0.f, 1.f) : ImVec4(1.f, .2f, .2f, 1.f),
                           "REC  %02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    }
    ImGui::End();
}

void ObsRecorder::DrawSettings()
{
    ImGui::TextDisabled("OBS Recorder 1.0.0");
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
    ImGui::SetNextItemWidth(140.f);
    ImGui::InputScalar("Long recording warning (minutes)", ImGuiDataType_U32, &warning_minutes);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("OBS CONNECTION");
    std::array<char, 256> host_buffer{};
    std::array<char, 256> password_buffer{};
    strncpy_s(host_buffer.data(), host_buffer.size(), obs_host.c_str(), _TRUNCATE);
    strncpy_s(password_buffer.data(), password_buffer.size(), obs_password.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::InputText("Host", host_buffer.data(), host_buffer.size())) obs_host = host_buffer.data();
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputScalar("Port", ImGuiDataType_U32, &obs_port);
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::InputText("Password", password_buffer.data(), password_buffer.size(), ImGuiInputTextFlags_Password)) {
        obs_password = password_buffer.data();
    }
    ImGui::TextDisabled("Reload the plugin after changing connection settings.");
    if (obs) {
        const auto connected = obs->Status() == "Connected to OBS" || obs->Status() == "Recording";
        ImGui::TextColored(connected ? ImVec4(.35f, .85f, .45f, 1.f) : ImVec4(1.f, .65f, .2f, 1.f),
                           "Status: %s", obs->Status().c_str());
    }
}

void ObsRecorder::LoadSettings(const wchar_t* folder)
{
    ToolboxPlugin::LoadSettings(folder);
    LoadSetting("automatic_recording", automatic_recording);
    LoadSetting("restrict_to_selected_maps", restrict_to_selected_maps);
    LoadSetting("available_map_ids", available_map_ids);
    LoadSetting("enabled_map_ids", enabled_map_ids);
    LoadSetting("show_indicator", show_indicator);
    LoadSetting("warning_minutes", warning_minutes);
    LoadSetting("obs_host", obs_host);
    LoadSetting("obs_port", obs_port);
    LoadSetting("obs_password", obs_password);
    if (obs) obs->Start(obs_host, obs_port, obs_password);
}

void ObsRecorder::SaveSettings(const wchar_t* folder)
{
    SaveSetting("automatic_recording", automatic_recording);
    SaveSetting("restrict_to_selected_maps", restrict_to_selected_maps);
    SaveSetting("available_map_ids", available_map_ids);
    SaveSetting("enabled_map_ids", enabled_map_ids);
    SaveSetting("show_indicator", show_indicator);
    SaveSetting("warning_minutes", warning_minutes);
    SaveSetting("obs_host", obs_host);
    SaveSetting("obs_port", obs_port);
    SaveSetting("obs_password", obs_password);
    ToolboxPlugin::SaveSettings(folder);
}
