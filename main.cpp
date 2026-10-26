// CustomSpeedometer - ScriptHookV (pure C++, no .NET). Shows speed, clock, altitude, trip while in a vehicle.
// Build: cl /LD /O2 /EHsc /std:c++17 /Isdk\inc main.cpp sdk\lib\ScriptHookV.lib user32.lib /Fe:CustomSpeedometer.dll
#define NOMINMAX
#include <windows.h>
#include "natives.h"
#include "types.h"
#include "enums.h"
#include "main.h"
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>

struct Config {
    std::string anchor = "BottomLeft";
    float offsetX = 208.0f, offsetY = 11.0f, scale = 1.0f;
    int colorKey = 0x21, resetKey = 0x22, toggleKey = 0x24; // PageUp, PageDown, Home
};
static Config g_cfg;
static volatile bool g_kColor = false, g_kReset = false, g_kToggle = false;

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static void loadConfig() {
    const char* paths[] = { "CustomSpeedometer.ini", "scripts\\CustomSpeedometer.ini", "scripts/CustomSpeedometer.ini",
                            "CustomSpeedometer.defaults.ini", "scripts\\CustomSpeedometer.defaults.ini" };
    for (const char* p : paths) {
        std::ifstream f(p);
        if (!f.is_open()) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t c = line.find(';'); if (c != std::string::npos) line = line.substr(0, c);
            size_t e = line.find('='); if (e == std::string::npos) continue;
            std::string k = trim(line.substr(0, e)), v = trim(line.substr(e + 1));
            if (k == "Anchor") g_cfg.anchor = v;
            else if (k == "OffsetX") g_cfg.offsetX = (float)std::atof(v.c_str());
            else if (k == "OffsetY") g_cfg.offsetY = (float)std::atof(v.c_str());
            else if (k == "Scale") g_cfg.scale = (float)std::atof(v.c_str());
            else if (k == "ColorKey") g_cfg.colorKey = std::atoi(v.c_str());
            else if (k == "ResetKey") g_cfg.resetKey = std::atoi(v.c_str());
            else if (k == "ToggleKey") g_cfg.toggleKey = std::atoi(v.c_str());
        }
        return;
    }
}

static void drawText(const std::string& s, float x, float y, float scale, int r, int g, int b, bool right) {
    std::vector<char> buf(s.begin(), s.end()); buf.push_back(0);
    char fmt[] = "STRING";
    UI::SET_TEXT_FONT(4);
    UI::SET_TEXT_SCALE(0.0f, scale);
    UI::SET_TEXT_COLOUR(r, g, b, 255);
    UI::SET_TEXT_OUTLINE();
    if (right) { UI::SET_TEXT_WRAP(0.0f, x); UI::SET_TEXT_RIGHT_JUSTIFY(TRUE); }
    UI::_SET_TEXT_ENTRY(fmt);
    UI::_ADD_TEXT_COMPONENT_STRING(buf.data());
    UI::_DRAW_TEXT(right ? 0.0f : x, y);
}

static std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }

void ScriptMain() {
    loadConfig();
    const int palette[][3] = { {255,255,255}, {80,200,255}, {90,255,120}, {255,170,40}, {255,70,70} };
    int ci = 0; bool visible = true; double tripKm = 0.0;
    int last = GAMEPLAY::GET_GAME_TIMER();
    while (true) {
        WAIT(0);
        int now = GAMEPLAY::GET_GAME_TIMER();
        double dt = (now - last) / 1000.0; last = now;
        if (g_kColor) { g_kColor = false; ci = (ci + 1) % 5; }
        if (g_kReset) { g_kReset = false; tripKm = 0.0; }
        if (g_kToggle) { g_kToggle = false; visible = !visible; }
        Ped player = PLAYER::PLAYER_PED_ID();
        if (!ENTITY::DOES_ENTITY_EXIST(player) || !PED::IS_PED_IN_ANY_VEHICLE(player, FALSE)) continue;
        Vehicle veh = PED::GET_VEHICLE_PED_IS_IN(player, FALSE);
        if (!veh) continue;
        double mps = ENTITY::GET_ENTITY_SPEED(veh);
        if (dt > 0.0 && dt < 1.0) tripKm += mps * dt / 1000.0;
        if (!visible) continue;
        Vector3 pos = ENTITY::GET_ENTITY_COORDS(veh, TRUE);
        char clock[16]; std::snprintf(clock, sizeof clock, "%02d:%02d", TIME::GET_CLOCK_HOURS(), TIME::GET_CLOCK_MINUTES());

        // layout in 1280x720 pixel space, then normalised
        float sc = g_cfg.scale, W = 190.0f * sc, Hh = 66.0f * sc;
        float left = g_cfg.offsetX, top;
        bool bottom = g_cfg.anchor.find("Bottom") != std::string::npos, rightA = g_cfg.anchor.find("Right") != std::string::npos;
        if (rightA) left = 1280.0f - g_cfg.offsetX - W;
        top = bottom ? 720.0f - g_cfg.offsetY - Hh : g_cfg.offsetY;
        float cx = (left + W / 2) / 1280.0f, cy = (top + Hh / 2) / 720.0f;
        GRAPHICS::DRAW_RECT(cx, cy, W / 1280.0f, Hh / 720.0f, 0, 0, 0, 150);
        int r = palette[ci][0], g = palette[ci][1], b = palette[ci][2];
        float tx = (left + 8 * sc) / 1280.0f;
        drawText(fmt("%.0f", mps * 3.6) + " km/h", tx, (top + 4 * sc) / 720.0f, 0.75f * sc, r, g, b, false);
        drawText(std::string(clock) + "   Alt: " + fmt("%.1f", pos.z) + " m", tx, (top + 34 * sc) / 720.0f, 0.34f * sc, 255, 255, 255, false);
        drawText("Trip: " + fmt("%.2f", tripKm) + " km", tx, (top + 49 * sc) / 720.0f, 0.34f * sc, 255, 255, 255, false);
    }
}

void OnKeyboardMessage(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDownBefore, BOOL isUpNow) {
    if (isUpNow || wasDownBefore) return;
    if ((int)key == g_cfg.colorKey) g_kColor = true;
    else if ((int)key == g_cfg.resetKey) g_kReset = true;
    else if ((int)key == g_cfg.toggleKey) g_kToggle = true;
}

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        scriptRegister(hInstance, ScriptMain);
        keyboardHandlerRegister(OnKeyboardMessage);
        break;
    case DLL_PROCESS_DETACH:
        scriptUnregister(hInstance);
        keyboardHandlerUnregister(OnKeyboardMessage);
        break;
    }
    return TRUE;
}
