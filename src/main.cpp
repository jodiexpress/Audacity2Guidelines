#include <Geode/Geode.hpp>
#include <Geode/modify/CreateGuidelinesLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <windows.h>
#include <commdlg.h>

using namespace geode::prelude;

static float labelToColor(const std::string& label) {
    std::string lower = label;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "orange" || lower == "o") return 1.0f;
    if (lower == "yellow" || lower == "y") return 0.9f;
    if (lower == "green"  || lower == "g") return 0.5f;
    return 1.0f;
}

static std::string parseAudacityFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::string result, line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) fields.push_back(field);
        if (fields.empty()) continue;
        try {
            double startTime = std::stod(fields[0]);
            std::string label = fields.size() >= 3 ? fields[2] : "";
            float color = labelToColor(label);
            result += std::to_string(startTime) + "~" + std::to_string(color) + "~";
        } catch (...) {}
    }
    return result;
}

struct BPMResult {
    std::string guidelines;
    std::string audacityTxt;
};

static BPMResult generateBPMGuidelines(double bpm, double start, double end, std::vector<int> divisors, double offsetMs) {
    BPMResult result;
    if (bpm <= 0 || start >= end || divisors.empty()) return result;

    double beatDuration = 60.0 / bpm;
    double offsetSec = offsetMs / 1000.0;

    struct Point { double time; float color; std::string label; };
    std::vector<Point> points;

    for (int divisor : divisors) {
        if (divisor <= 0) continue;
        double step = beatDuration / divisor;
        double t = start;
        float color = 1.0f;
        std::string label = "orange";
        if (divisor == 2) { color = 0.9f; label = "yellow"; }
        if (divisor >= 4) { color = 0.5f; label = "green"; }
        while (t <= end + 0.0001) {
            double shifted = t + offsetSec;
            if (shifted >= 0) points.push_back({ shifted, color, label });
            t += step;
        }
    }

    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.time < b.time;
    });

    std::stringstream gdSS, audacitySS;
    audacitySS << std::fixed << std::setprecision(6);
    for (const auto& p : points) {
        gdSS << std::fixed << std::setprecision(6) << p.time << "~" << p.color << "~";
        audacitySS << p.time << "\t" << p.time << "\t" << p.label << "\n";
    }
    result.guidelines = gdSS.str();
    result.audacityTxt = audacitySS.str();
    return result;
}

static std::string saveFileDialog() {
    OPENFILENAMEA ofn;
    char szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Audacity Labels\0*.txt\0All Files\0*.*\0";
    ofn.lpstrDefExt = "txt";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return std::string(szFile);
    return "";
}

class BPMPopup : public geode::Popup {
public:
    LevelSettingsObject* m_settings;
    TextInput* m_bpmInput;
    TextInput* m_startInput;
    TextInput* m_endInput;
    TextInput* m_divisorsInput;
    TextInput* m_offsetInput;

    static BPMPopup* create(LevelSettingsObject* settings) {
        auto ret = new BPMPopup();
        if (ret && ret->initPopup(settings)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool initPopup(LevelSettingsObject* settings) {
        if (!geode::Popup::init(280.f, 300.f)) return false;
        m_settings = settings;

        this->setTitle("Generate BPM Guidelines");

        auto menu = CCMenu::create();
        menu->setPosition(this->m_mainLayer->getContentSize() / 2);
        this->m_mainLayer->addChild(menu);

        float y = 80;
        float gap = 35;

        auto addRow = [&](const char* labelStr, TextInput*& input, const char* savedKey, const char* placeholder, const char* filter) {
            auto contentSize = this->m_mainLayer->getContentSize();
            auto lbl = CCLabelBMFont::create(labelStr, "bigFont.fnt");
            lbl->setScale(0.4f);
            lbl->setAnchorPoint({1, 0.5f});
            lbl->setPosition(contentSize.width / 2 - 10, contentSize.height / 2 + y);
            this->m_mainLayer->addChild(lbl);
            input = TextInput::create(110.f, placeholder, "chatFont.fnt");
            input->setFilter(filter);
            auto saved = Mod::get()->getSavedValue<std::string>(savedKey, "");
            if (!saved.empty()) input->setString(saved);
            input->setPosition({55, y});
            menu->addChild(input);
            y -= gap;
        };

        addRow("BPM",         m_bpmInput,     "bpm_val",      "120",  "0123456789.");
        addRow("Start (sec)", m_startInput,   "start_val",    "0.0",  "0123456789.");
        addRow("End (sec)",   m_endInput,     "end_val",      "60.0", "0123456789.");
        addRow("Divisors",    m_divisorsInput,"divisors_val", "1 4",  "0123456789 ");
        addRow("Offset (ms)", m_offsetInput,  "offset_val",  "0",    "0123456789.-");

        auto genBtn = CCMenuItemExt::createSpriteExtra(
            ButtonSprite::create("Generate", "goldFont.fnt", "GJ_button_01.png"),
            [this](CCMenuItemSpriteExtra*) { onGenerate(); }
        );
        genBtn->setPosition({0, y - 8});
        menu->addChild(genBtn);

        auto helpBtn = CCMenuItemExt::createSpriteExtra(
            CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"),
            [](CCMenuItemSpriteExtra*) { showHelp(); }
        );
        helpBtn->setScale(0.7f);
        helpBtn->setPosition({120, 130});
        menu->addChild(helpBtn);

        if (!Mod::get()->getSavedValue<bool>("tutorial_shown")) {
            Mod::get()->setSavedValue("tutorial_shown", true);
            this->scheduleOnce(schedule_selector(BPMPopup::showTutorialDelayed), 0.3f);
        }

        return true;
    }

    static void showHelp() {
        FLAlertLayer::create(
            "How to use",
            "BPM - your song tempo\n"
            "Start - start time in seconds\n"
            "End - end time in seconds\n"
            "Divisors - beat subdivisions:\n"
            "  1 = every beat (orange)\n"
            "  2 = half beats (yellow)\n"
            "  4 = quarter beats (green)\n"
            "Offset - shift in milliseconds\n\n"
            "Example: \"1 4\" generates beats\n"
            "and quarter beats together.\n\n"
            "Saved .txt opens in Audacity\n"
            "as a label track!",
            "OK"
        )->show();
    }

    void showTutorialDelayed(float) { showHelp(); }

    void onGenerate() {
        try {
            double bpm    = std::stod(m_bpmInput->getString());
            double start  = std::stod(m_startInput->getString());
            double end    = std::stod(m_endInput->getString());
            double offset = 0.0;
            try { offset = std::stod(m_offsetInput->getString()); } catch (...) {}

            Mod::get()->setSavedValue("bpm_val",      std::string(m_bpmInput->getString()));
            Mod::get()->setSavedValue("start_val",    std::string(m_startInput->getString()));
            Mod::get()->setSavedValue("end_val",      std::string(m_endInput->getString()));
            Mod::get()->setSavedValue("divisors_val", std::string(m_divisorsInput->getString()));
            Mod::get()->setSavedValue("offset_val",   std::string(m_offsetInput->getString()));

            std::vector<int> divisors;
            std::stringstream ss(m_divisorsInput->getString());
            std::string token;
            while (ss >> token) {
                try { divisors.push_back(std::stoi(token)); } catch (...) {}
            }
            if (divisors.empty()) divisors = {1};

            auto result = generateBPMGuidelines(bpm, start, end, divisors, offset);
            if (result.guidelines.empty()) {
                Notification::create("Invalid parameters!", NotificationIcon::Error)->show();
                return;
            }

            // мёржим с существующими гайдлайнами
            auto existing = std::string(m_settings->m_guidelineString);
            m_settings->m_guidelineString = existing + result.guidelines;
            m_settings->m_guidelinesUpdated = true;
            if (auto* editor = LevelEditorLayer::get()) editor->levelSettingsUpdated();

            auto savePath = saveFileDialog();
            if (!savePath.empty()) {
                std::ofstream outFile(savePath);
                if (outFile.is_open()) {
                    outFile << result.audacityTxt;
                    outFile.close();
                    Notification::create("Saved & imported!", NotificationIcon::Success)->show();
                } else {
                    Notification::create("Could not save file!", NotificationIcon::Error)->show();
                }
            } else {
                Notification::create("BPM guidelines generated!", NotificationIcon::Success)->show();
            }

            this->onClose(nullptr);
        } catch (...) {
            Notification::create("Invalid input!", NotificationIcon::Error)->show();
        }
    }
};

static std::atomic<bool> s_picking = false;

class $modify(MyCreateGuidelinesLayer, CreateGuidelinesLayer) {
    bool init(CustomSongDelegate* p0, AudioGuidelinesType p1) {
        if (!CreateGuidelinesLayer::init(p0, p1)) return false;

        auto btnAudacity = CCMenuItemExt::createSpriteExtra(
            ButtonSprite::create("Import Audacity", "bigFont.fnt", "GJ_button_01.png", 0.3f),
            [this](CCMenuItemSpriteExtra*) { handleImportClick(); }
        );
        btnAudacity->setPosition({-145, -80});
        m_buttonMenu->addChild(btnAudacity);

        auto btnBPM = CCMenuItemExt::createSpriteExtra(
            ButtonSprite::create("Generate BPM", "bigFont.fnt", "GJ_button_02.png", 0.3f),
            [this](CCMenuItemSpriteExtra*) { handleBPMClick(); }
        );
        btnBPM->setPosition({-145, -115});
        m_buttonMenu->addChild(btnBPM);

        return true;
    }

    void handleBPMClick() {
        auto* settings = m_delegate->getLevelSettings();
        if (!settings) {
            Notification::create("Could not find level settings!", NotificationIcon::Error)->show();
            return;
        }
        BPMPopup::create(settings)->show();
    }

    void handleImportClick() {
        bool expected = false;
        if (!s_picking.compare_exchange_strong(expected, true)) return;

        auto* delegate = m_delegate;

        std::thread([delegate]() {
            HWND gdWindow = GetActiveWindow();
            OPENFILENAMEA ofn;
            char szFile[260] = {0};
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = gdWindow;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile);
            ofn.lpstrFilter = "Audacity Labels\0*.txt\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

            bool picked = GetOpenFileNameA(&ofn);
            std::string path = picked ? std::string(szFile) : "";
            SetForegroundWindow(gdWindow);
            s_picking = false;
            if (path.empty()) return;

            auto guidelines = parseAudacityFile(path);
            if (guidelines.empty()) {
                Loader::get()->queueInMainThread([]() {
                    Notification::create("Failed to parse file!", NotificationIcon::Error)->show();
                });
                return;
            }

            Loader::get()->queueInMainThread([delegate, guidelines]() {
                auto* settings = delegate->getLevelSettings();
                if (!settings) {
                    Notification::create("Could not find level settings!", NotificationIcon::Error)->show();
                    return;
                }
                auto existing = std::string(settings->m_guidelineString);
                settings->m_guidelineString = existing + guidelines;
                settings->m_guidelinesUpdated = true;
                if (auto* editor = LevelEditorLayer::get()) editor->levelSettingsUpdated();
                Notification::create("Guidelines imported!", NotificationIcon::Success)->show();
            });
        }).detach();
    }
};