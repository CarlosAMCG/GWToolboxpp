#pragma once

#include <ToolboxUIPlugin.h>
#include <IconsFontAwesome5.h>

class ChatTranslator : public ToolboxUIPlugin {
public:
    ChatTranslator() = default;
    ~ChatTranslator() override = default;

    const char* Name() const override { return "Chat Translator"; }

    const char* Icon() const override { return ICON_FA_LANGUAGE; }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;

    void SignalTerminate() override;

    void LoadSettings(const wchar_t* folder) override;

    void SaveSettings(const wchar_t* folder) override;

    void Update(float delay) override;

    void Draw(IDirect3DDevice9* device) override;

    void DrawSettings() override;
};
