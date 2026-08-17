#include "ChatTranslator.h"

#include <PluginUtils.h>
#include <RestClient.h>

#include <fasttext.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct MyMemoryResponseData {
    std::string translatedText;
};

struct MyMemoryResponse {
    MyMemoryResponseData responseData;
};

template <>
struct glz::meta<MyMemoryResponseData> {
    using T = MyMemoryResponseData;
    static constexpr auto value = object("translatedText", &T::translatedText);
};

template <>
struct glz::meta<MyMemoryResponse> {
    using T = MyMemoryResponse;
    static constexpr auto value = object("responseData", &T::responseData);
};

namespace {

    struct CapturedMessage {
        uint64_t id;
        std::wstring visible;
        std::wstring translation;
        std::wstring translation_error;
        GW::Chat::Channel channel;
        FILETIME timestamp;
        bool translation_pending = false;
    };

    struct PendingMessage {
        PluginUtils::EncString text;
        GW::Chat::Channel channel;
        FILETIME timestamp;

        PendingMessage(const wchar_t* encoded, const GW::Chat::Channel channel, const FILETIME& timestamp)
            : text(encoded), channel(channel), timestamp(timestamp) {}
    };

    struct TranslationRequest {
        AsyncRestClient client;
        uint64_t message_id;
        std::wstring cache_key;
    };

    std::vector<CapturedMessage> messages;
    std::vector<std::unique_ptr<PendingMessage>> pending_messages;
    std::vector<std::unique_ptr<TranslationRequest>> translation_requests;
    std::unordered_set<const GW::Chat::ChatMessage*> known_messages;
    std::unordered_map<std::wstring, std::wstring> translation_cache;
    std::unique_ptr<fasttext::FastText> language_detector;
    std::string language_detector_error;

    constexpr std::array source_languages{
        std::pair{"English", "en"},
        std::pair{"French", "fr"},
        std::pair{"German", "de"},
        std::pair{"Italian", "it"},
        std::pair{"Spanish", "es"},
        std::pair{"Polish", "pl"},
        std::pair{"Russian", "ru"},
        std::pair{"Japanese", "ja"},
        std::pair{"Korean", "ko"},
        std::pair{"Traditional Chinese", "zh-TW"},
    };

    constexpr std::array chat_channels{
        GW::Chat::Channel::CHANNEL_ALL,
        GW::Chat::Channel::CHANNEL_ALLIES,
        GW::Chat::Channel::CHANNEL_GROUP,
        GW::Chat::Channel::CHANNEL_GUILD,
        GW::Chat::Channel::CHANNEL_ALLIANCE,
        GW::Chat::Channel::CHANNEL_TRADE,
        GW::Chat::Channel::CHANNEL_WHISPER,
        GW::Chat::Channel::CHANNEL_EMOTE,
        GW::Chat::Channel::CHANNEL_GLOBAL,
    };

    bool mymemory_enabled = true;
    bool show_original_messages = true;
    bool show_translations = true;
    int source_language_index = 0;
    std::array<bool, GW::Chat::Channel::CHANNEL_COUNT> translate_channels = [] {
        std::array<bool, GW::Chat::Channel::CHANNEL_COUNT> channels{};
        for (const auto channel : chat_channels) channels[static_cast<size_t>(channel)] = true;
        return channels;
    }();
    bool initialized = false;
    uint64_t next_message_id = 1;

    constexpr size_t max_messages = 200;
    constexpr size_t max_cached_translations = 500;

    const wchar_t* ChannelName(const GW::Chat::Channel channel)
    {
        switch (channel) {
            case GW::Chat::Channel::CHANNEL_ALLIANCE: return L"Alliance";
            case GW::Chat::Channel::CHANNEL_ALLIES: return L"Allies";
            case GW::Chat::Channel::CHANNEL_ALL: return L"All";
            case GW::Chat::Channel::CHANNEL_EMOTE: return L"Emote";
            case GW::Chat::Channel::CHANNEL_GUILD: return L"Guild";
            case GW::Chat::Channel::CHANNEL_GLOBAL: return L"Other";
            case GW::Chat::Channel::CHANNEL_GROUP: return L"Team";
            case GW::Chat::Channel::CHANNEL_TRADE: return L"Trade";
            case GW::Chat::Channel::CHANNEL_WHISPER: return L"Whisper";
            default: return L"Other";
        }
    }

    bool IsChannelSelected(const GW::Chat::Channel channel)
    {
        const auto index = static_cast<int>(channel);
        return index >= 0 && index < GW::Chat::Channel::CHANNEL_COUNT && translate_channels[static_cast<size_t>(index)];
    }

    const char* LanguageName(const GW::Constants::Language language)
    {
        switch (language) {
            case GW::Constants::Language::English: return "English";
            case GW::Constants::Language::Korean: return "Korean";
            case GW::Constants::Language::French: return "French";
            case GW::Constants::Language::German: return "German";
            case GW::Constants::Language::Italian: return "Italian";
            case GW::Constants::Language::Spanish: return "Spanish";
            case GW::Constants::Language::TraditionalChinese: return "Traditional Chinese";
            case GW::Constants::Language::Japanese: return "Japanese";
            case GW::Constants::Language::Polish: return "Polish";
            case GW::Constants::Language::Russian: return "Russian";
            case GW::Constants::Language::BorkBorkBork: return "Bork Bork Bork";
            default: return "Unknown";
        }
    }

    const char* MyMemoryTargetLanguage(const GW::Constants::Language language)
    {
        switch (language) {
            case GW::Constants::Language::English: return "en";
            case GW::Constants::Language::Korean: return "ko";
            case GW::Constants::Language::French: return "fr";
            case GW::Constants::Language::German: return "de";
            case GW::Constants::Language::Italian: return "it";
            case GW::Constants::Language::Spanish: return "es";
            case GW::Constants::Language::TraditionalChinese: return "zh-TW";
            case GW::Constants::Language::Japanese: return "ja";
            case GW::Constants::Language::Polish: return "pl";
            case GW::Constants::Language::Russian: return "ru";
            default: return nullptr;
        }
    }

    void LoadLanguageDetector()
    {
        if (!plugin_handle) {
            language_detector_error = "plugin handle unavailable";
            return;
        }
        const auto resource = FindResourceW(plugin_handle, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
        if (!resource) {
            language_detector_error = "model resource unavailable";
            return;
        }

        const auto resource_size = SizeofResource(plugin_handle, resource);
        const auto resource_data = LockResource(LoadResource(plugin_handle, resource));
        if (!resource_data || resource_size == 0) {
            language_detector_error = "model resource could not be loaded";
            return;
        }

        try {
            std::string model(static_cast<const char*>(resource_data), resource_size);
            std::istringstream stream(model);
            stream.seekg(sizeof(int32_t) * 2);
            auto detector = std::make_unique<fasttext::FastText>();
            detector->loadModel(stream);
            language_detector = std::move(detector);
        }
        catch (const std::exception& error) {
            language_detector_error = error.what();
        }
    }

    std::string DetectSourceLanguage(const std::wstring& text)
    {
        if (!language_detector) return "en";

        std::istringstream stream(PluginUtils::WStringToString(text));
        std::vector<std::pair<fasttext::real, std::string>> predictions;
        if (!language_detector->predictLine(stream, predictions, 1, 0.0f) || predictions.empty()) return "en";

        constexpr std::string_view prefix = "__label__";
        const auto& label = predictions.front().second;
        if (!label.starts_with(prefix)) return "en";
        const auto language = label.substr(prefix.size());
        if (language == "zh") return "zh-TW";
        for (const auto& [_, code] : source_languages) {
            if (language == code) return language;
        }
        return "en";
    }

    std::string SourceLanguage(const std::wstring& text)
    {
        if (source_language_index == 0) return DetectSourceLanguage(text);
        return source_languages[static_cast<size_t>(source_language_index - 1)].second;
    }

    CapturedMessage* FindMessage(const uint64_t id)
    {
        for (auto& message : messages) {
            if (message.id == id) return &message;
        }
        return nullptr;
    }

    std::wstring_view MessageBody(const std::wstring& decoded)
    {
        const auto separator = decoded.find(L": ");
        if (separator == std::wstring::npos || separator + 2 >= decoded.size()) return decoded;
        return std::wstring_view(decoded).substr(separator + 2);
    }

    std::wstring_view Trim(const std::wstring_view text)
    {
        const auto first = std::find_if_not(text.begin(), text.end(), [](const wchar_t character) { return iswspace(character); });
        const auto last = std::find_if_not(text.rbegin(), text.rend(), [](const wchar_t character) { return iswspace(character); }).base();
        return first >= last ? std::wstring_view{} : std::wstring_view(text.data() + (first - text.begin()), static_cast<size_t>(last - first));
    }

    bool IsUsefulTranslation(const std::wstring_view translation, const std::wstring_view original)
    {
        const auto trimmed_translation = Trim(translation);
        const auto trimmed_original = Trim(original);
        if (trimmed_translation.empty()) return false;

        constexpr std::wstring_view rejected_response = L"testvalue";
        const auto equals_ignore_case = [](const std::wstring_view left, const std::wstring_view right) {
            return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](const wchar_t a, const wchar_t b) {
                return towlower(a) == towlower(b);
            });
        };
        return !equals_ignore_case(trimmed_translation, trimmed_original) && !equals_ignore_case(trimmed_translation, rejected_response);
    }

    void ClearHistory()
    {
        for (const auto& request : translation_requests) request->client.Abort();
        messages.clear();
        pending_messages.clear();
        translation_requests.clear();
        translation_cache.clear();
    }

    void QueueTranslation(CapturedMessage& message, const std::wstring& decoded)
    {
        if (!mymemory_enabled) return;

        const auto target_language = MyMemoryTargetLanguage(GW::UI::GetTextLanguage());
        if (!target_language) return;

        const auto body = MessageBody(decoded);
        const auto source_language = SourceLanguage(std::wstring(body));
        if (source_language == target_language) return;

        const auto cache_key = PluginUtils::StringToWString(source_language) + L"\n" + PluginUtils::StringToWString(target_language) + L"\n" + std::wstring(body);
        if (const auto cache = translation_cache.find(cache_key); cache != translation_cache.end()) {
            message.translation = cache->second;
            return;
        }

        auto text = PluginUtils::WStringToString(std::wstring(body));
        if (text.size() > 500) {
            text.resize(500);
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        }
        std::string escaped_text;
        if (!EscapeUrl(escaped_text, text.c_str())) return;

        auto request = std::make_unique<TranslationRequest>();
        request->message_id = message.id;
        request->cache_key = cache_key;
        const auto url = "https://api.mymemory.translated.net/get?q=" + escaped_text + "&langpair=" + source_language + "%7C" + target_language + "&mt=1";
        request->client.SetUrl(url.c_str());
        request->client.SetUserAgent("GWToolboxpp ChatTranslator/1.0");
        request->client.SetFollowLocation(true);
        request->client.SetTimeoutSec(30);
        request->client.ExecuteAsync();
        message.translation_pending = true;
        translation_requests.push_back(std::move(request));
    }

    void FlushTranslationRequests()
    {
        for (auto request = translation_requests.begin(); request != translation_requests.end();) {
            if ((*request)->client.IsPending()) {
                ++request;
                continue;
            }

            if (auto* message = FindMessage((*request)->message_id)) {
                message->translation_pending = false;
                MyMemoryResponse response;
                if ((*request)->client.IsSuccessful() && !glz::read<glz::opts{.error_on_unknown_keys = false}>(response, (*request)->client.GetContent()) && !response.responseData.translatedText.empty()) {
                    const auto translation = PluginUtils::StringToWString(response.responseData.translatedText);
                    if (IsUsefulTranslation(translation, MessageBody(message->visible))) {
                        message->translation = translation;
                        if (translation_cache.size() >= max_cached_translations) translation_cache.clear();
                        translation_cache[(*request)->cache_key] = message->translation;
                    }
                }
                else {
                    message->translation_error = L"No se pudo traducir (HTTP " + std::to_wstring((*request)->client.GetStatusCode()) + L").";
                }
            }
            request = translation_requests.erase(request);
        }
    }

    std::wstring FormatTimestamp(const FILETIME& ft)
    {
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
            return L"[??:??]";
        }

        wchar_t buffer[16]{};
        swprintf_s(buffer, L"[%02u:%02u]", static_cast<unsigned>(local.wHour), static_cast<unsigned>(local.wMinute));
        return buffer;
    }

    void FlushPendingMessages()
    {
        for (auto message = pending_messages.begin(); message != pending_messages.end();) {
            const auto& decoded = (*message)->text.wstring();
            if (decoded.empty()) {
                ++message;
                continue;
            }

            if (!IsChannelSelected((*message)->channel)) {
                message = pending_messages.erase(message);
                continue;
            }

            const auto visible = FormatTimestamp((*message)->timestamp) + L" [" + ChannelName((*message)->channel) + L"] " + decoded;
            messages.push_back({next_message_id++, visible, {}, {}, (*message)->channel, (*message)->timestamp});
            if (messages.size() > max_messages) messages.erase(messages.begin());
            QueueTranslation(messages.back(), decoded);
            message = pending_messages.erase(message);
        }
    }
} // namespace

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static ChatTranslator instance;
    return &instance;
}

void ChatTranslator::Initialize(ImGuiContext* ctx, ImGuiAllocFns fns, HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, fns, toolbox_dll);
    ClearHistory();
    known_messages.clear();
    translation_cache.clear();
    LoadLanguageDetector();
    initialized = false;
}

void ChatTranslator::SignalTerminate()
{
    ToolboxUIPlugin::SignalTerminate();
    for (const auto& request : translation_requests) request->client.Abort();
    messages.clear();
    pending_messages.clear();
    translation_requests.clear();
    known_messages.clear();
    translation_cache.clear();
    language_detector.reset();
    language_detector_error.clear();
}

void ChatTranslator::LoadSettings(const wchar_t* folder)
{
    ToolboxUIPlugin::LoadSettings(folder);
    LoadSetting("mymemory_enabled", mymemory_enabled);
    LoadSetting("show_original_messages", show_original_messages);
    LoadSetting("show_translations", show_translations);
    LoadSetting("source_language_index", source_language_index);
    LoadSetting("translate_channels", translate_channels);
    source_language_index = std::clamp(source_language_index, 0, static_cast<int>(source_languages.size()));
}

void ChatTranslator::SaveSettings(const wchar_t* folder)
{
    SaveSetting("mymemory_enabled", mymemory_enabled);
    SaveSetting("show_original_messages", show_original_messages);
    SaveSetting("show_translations", show_translations);
    SaveSetting("source_language_index", source_language_index);
    SaveSetting("translate_channels", translate_channels);
    ToolboxUIPlugin::SaveSettings(folder);
}

void ChatTranslator::Update(float delay)
{
    ToolboxUIPlugin::Update(delay);
    FlushPendingMessages();
    FlushTranslationRequests();

    const auto chat = GW::Chat::GetChatLog();
    if (!chat) return;

    if (!initialized) {
        for (size_t i = 0; i < GW::Chat::CHAT_LOG_LENGTH; ++i) {
            if (const auto message = chat->messages[i]) known_messages.insert(message);
        }
        initialized = true;
        return;
    }

    for (size_t i = 0; i < GW::Chat::CHAT_LOG_LENGTH; ++i) {
        const auto message = chat->messages[i];
        if (!message || known_messages.contains(message)) continue;

        known_messages.insert(message);
        pending_messages.push_back(std::make_unique<PendingMessage>(message->message, static_cast<GW::Chat::Channel>(message->channel), message->timestamp));
    }
}

void ChatTranslator::Draw(IDirect3DDevice9* pDevice)
{
    UNREFERENCED_PARAMETER(pDevice);
    if (!GetVisiblePtr() || !*GetVisiblePtr()) return;

    ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags())) {
        ImGui::Text("Destino: %s", LanguageName(GW::UI::GetTextLanguage()));
        ImGui::Separator();
        if (messages.empty()) {
            ImGui::TextDisabled("Aun no hay mensajes en los canales seleccionados.");
        } else {
            for (const auto& message : messages) {
                if (show_original_messages) {
                    const auto visible = PluginUtils::WStringToString(message.visible);
                    ImGui::TextWrapped("%s", visible.c_str());
                }
                if (show_translations && !message.translation.empty()) {
                    const auto translation = PluginUtils::WStringToString(message.translation);
                    ImGui::TextWrapped("  -> %s", translation.c_str());
                }
                else if (show_translations && message.translation_pending) {
                    ImGui::TextDisabled("  -> Traduciendo...");
                }
                else if (show_translations && !message.translation_error.empty()) {
                    const auto error = PluginUtils::WStringToString(message.translation_error);
                    ImGui::TextDisabled("  -> %s", error.c_str());
                }
            }
        }
    }
    ImGui::End();
}

void ChatTranslator::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();
    ImGui::Text("Idioma de destino: %s", LanguageName(GW::UI::GetTextLanguage()));
    ImGui::Separator();
    ImGui::Checkbox("Activar traduccion", &mymemory_enabled);
    ImGui::Checkbox("Mostrar mensaje original", &show_original_messages);
    ImGui::Checkbox("Mostrar traduccion", &show_translations);
    if (ImGui::Button("Limpiar historial")) ClearHistory();
    ImGui::Combo("Idioma de origen", &source_language_index, [](void*, const int index) {
        return index == 0 ? "Detectar automaticamente" : source_languages[static_cast<size_t>(index - 1)].first;
    }, nullptr, static_cast<int>(source_languages.size()) + 1);
    ImGui::Separator();
    ImGui::Text("Canales a traducir");
    for (size_t i = 0; i < chat_channels.size(); ++i) {
        const auto channel = chat_channels[i];
        const auto label = PluginUtils::WStringToString(ChannelName(channel));
        ImGui::Checkbox(label.c_str(), &translate_channels[static_cast<size_t>(channel)]);
        if (i % 2 == 0 && i + 1 < chat_channels.size()) ImGui::SameLine(180.0f);
    }
}
