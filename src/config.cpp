#include "config.h"

Config g_cfg;
Lang g_lang = Lang::Zh;
int g_logEnglish = 0;

static std::wstring IniPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L'\\')) + L"\\sbparry.ini";
}

static int GetInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"sbparry", key, def, IniPath().c_str()); }
static void SetInt(const wchar_t* key, int v) {
    WritePrivateProfileStringW(L"sbparry", key, std::to_wstring(v).c_str(), IniPath().c_str());
}

static const wchar_t* kDevices[] = {L"auto", L"keyboard", L"xinput", L"dualsense"};
static const wchar_t* kSources[] = {L"auto", L"native", L"ue4ss"};

// 自动：跟随游戏语言（%LOCALAPPDATA%\SB\Saved\Config\WindowsNoEditor\GameUserSettings.ini 的 Language=zh-Hans），
// 读不到再看系统界面语言 / 区域
static Lang DetectLanguage() {
    wchar_t path[MAX_PATH], lang[32] = L"";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) {
        wcscat_s(path, L"\\SB\\Saved\\Config\\WindowsNoEditor\\GameUserSettings.ini");
        FILE* f = nullptr;
        if (!_wfopen_s(&f, path, L"rb") && f) {
            char line[256];
            while (fgets(line, sizeof(line), f))
                if (!_strnicmp(line, "Language=", 9)) MultiByteToWideChar(CP_UTF8, 0, line + 9, -1, lang, 32);
            fclose(f);
        }
    }
    if (lang[0]) return !_wcsnicmp(lang, L"zh", 2) ? Lang::Zh : Lang::En;
    bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE || PRIMARYLANGID(GetSystemDefaultLangID()) == LANG_CHINESE;
    return zh ? Lang::Zh : Lang::En;
}

static void ApplyLanguage() {
    if (g_cfg.language == 1) g_lang = Lang::Zh;
    else if (g_cfg.language == 2) g_lang = Lang::En;
    else g_lang = DetectLanguage();
}

void LoadConfig() {
    Config d;
    g_cfg.language = std::clamp(GetInt(L"language", d.language), 0, 2);
    g_cfg.barMode = std::clamp(GetInt(L"barMode", d.barMode), 0, 1);
    g_cfg.barVisible = GetInt(L"barVisible", d.barVisible) != 0;
    g_cfg.panelVisible = GetInt(L"panelVisible", d.panelVisible) != 0;
    g_cfg.panelCorner = GetInt(L"panelCorner", d.panelCorner) & 3;
    g_cfg.autoParry = GetInt(L"autoParry", d.autoParry) != 0;
    g_cfg.autoChance = GetInt(L"autoChance", d.autoChance) != 0;
    g_cfg.autoQte = GetInt(L"autoQte", d.autoQte) != 0;
    g_cfg.autoAimMs = std::clamp(GetInt(L"autoAimMs", d.autoAimMs), -100, 100);
    g_cfg.debugLog = GetInt(L"debugLog", d.debugLog) != 0;
    g_cfg.uiScale = std::clamp(GetInt(L"uiScale", d.uiScale), 50, 300);
    wchar_t dev[32];
    GetPrivateProfileStringW(L"sbparry", L"autoDevice", L"auto", dev, 32, IniPath().c_str());
    g_cfg.autoDevice = InputDevice::Auto;
    for (int i = 0; i < 4; i++)
        if (!_wcsicmp(dev, kDevices[i])) g_cfg.autoDevice = (InputDevice)i;
    GetPrivateProfileStringW(L"sbparry", L"dataSource", L"auto", dev, 32, IniPath().c_str());
    g_cfg.dataSource = DataSource::Auto;
    for (int i = 0; i < 3; i++)
        if (!_wcsicmp(dev, kSources[i])) g_cfg.dataSource = (DataSource)i;
    ApplyLanguage();
    SaveConfig(); // 第一次运行时生成带全部选项的 ini，方便手改
}

void SaveConfig() {
    SetInt(L"language", g_cfg.language);
    SetInt(L"barMode", g_cfg.barMode);
    SetInt(L"barVisible", g_cfg.barVisible);
    SetInt(L"panelVisible", g_cfg.panelVisible);
    SetInt(L"panelCorner", g_cfg.panelCorner);
    SetInt(L"autoParry", g_cfg.autoParry);
    SetInt(L"autoChance", g_cfg.autoChance);
    SetInt(L"autoQte", g_cfg.autoQte);
    WritePrivateProfileStringW(L"sbparry", L"autoDevice", kDevices[(int)g_cfg.autoDevice], IniPath().c_str());
    WritePrivateProfileStringW(L"sbparry", L"dataSource", kSources[(int)g_cfg.dataSource], IniPath().c_str());
    SetInt(L"autoAimMs", g_cfg.autoAimMs);
    SetInt(L"debugLog", g_cfg.debugLog);
    SetInt(L"uiScale", g_cfg.uiScale);
    ApplyLanguage();
}
