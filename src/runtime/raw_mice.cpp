#include "raw_mice.h"

#include <hidsdi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "../common/clock.h"

namespace raw_mice {
namespace {

struct Mouse {
    HANDLE device;
    LONG x = 0, y = 0;        // screen pixels: the mouse's own pointer
    bool have_pos = false;
    bool left = false, right = false;
    bool clicked = false;
    std::string path;         // lower case
    std::wstring name;
    bool absolute = false;
    unsigned presses = 0;
    double last_input = 0;
};

std::vector<Mouse> g_mice;
int g_owner[2] = {-1, -1};    // index in g_mice of each player's mouse
int g_last_clicked = -1;      // player 1's mouse until a second one joins
bool g_join_allowed = false;
RECT g_area{};                // the window's client area, screen pixels
bool g_have_area = false, g_clipped = false;
std::string g_saved[2];       // GUN TEST's choice: each player's device path

// The device's path, and the name its USB descriptor gives (else its ids)
void identify(Mouse& m) {
    UINT len = 0;
    GetRawInputDeviceInfoW(m.device, RIDI_DEVICENAME, nullptr, &len);
    std::wstring path(len, L'\0');
    if (len && GetRawInputDeviceInfoW(m.device, RIDI_DEVICENAME, path.data(), &len) != UINT(-1)) {
        path.resize(wcslen(path.c_str()));
        for (wchar_t c : path) m.path += char(tolower(c < 128 ? int(c) : '_'));
    }
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t product[128] = {};
        if (HidD_GetProductString(h, product, sizeof(product))) m.name = product;
        CloseHandle(h);
    }
    if (m.name.empty()) {   // "VID_2341 PID_8042" from the path
        size_t v = m.path.find("vid_"), p = m.path.find("pid_");
        if (v != std::string::npos && p != std::string::npos) {
            std::string ids = "VID " + m.path.substr(v + 4, 4) + "  PID " + m.path.substr(p + 4, 4);
            for (char& c : ids) c = char(toupper(c));
            m.name = std::wstring(ids.begin(), ids.end());
        } else {
            m.name = L"Mouse";
        }
    }
}

int find(HANDLE device) {
    for (size_t i = 0; i < g_mice.size(); i++)
        if (g_mice[i].device == device) return int(i);
    g_mice.push_back({device});
    int i = int(g_mice.size() - 1);
    identify(g_mice[i]);
    for (int p = 0; p < 2; p++)   // a gun picked in GUN TEST is its player's at once
        if (!g_saved[p].empty() && g_mice[i].path == g_saved[p]) g_owner[p] = i;
    return i;
}

// Every mouse Windows lists now, known to find() from here on
void list_all() {
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return;
    std::vector<RAWINPUTDEVICELIST> list(n);
    n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    if (n == UINT(-1)) return;
    for (UINT i = 0; i < n; i++)
        if (list[i].dwType == RIM_TYPEMOUSE) find(list[i].hDevice);
}

void keep_inside(Mouse& m) {
    if (!g_have_area) return;
    m.x = std::clamp(m.x, g_area.left, g_area.right - 1);
    m.y = std::clamp(m.y, g_area.top, g_area.bottom - 1);
}

}  // namespace

void init(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;   // generic desktop
    rid.usUsage = 0x02;       // mouse
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) printf("[mouse] no raw input: a single mouse\n");
    list_all();
}

void set_saved(const std::string& p1, const std::string& p2) {
    if (p1 != g_saved[0] || p2 != g_saved[1]) g_owner[0] = g_owner[1] = -1;   // a new choice: from scratch
    g_saved[0] = p1, g_saved[1] = p2;
    for (int p = 0; p < 2; p++) {
        if (g_saved[p].empty()) continue;
        g_owner[p] = -1;
        for (size_t i = 0; i < g_mice.size(); i++)
            if (g_mice[i].path == g_saved[p]) g_owner[p] = int(i);   // the newest entry: plugged in again
    }
    if (g_owner[0] >= 0 && g_owner[0] == g_owner[1]) g_owner[0] = -1;
    list_all();   // a saved gun plugged in since the start
}

void on_input(HRAWINPUT input) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(input, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE || !ri.header.hDevice) return;   // no device: input sent by software
    int i = find(ri.header.hDevice);
    Mouse& m = g_mice[i];
    const RAWMOUSE& rm = ri.data.mouse;
    m.last_input = now_seconds();
    if (g_owner[0] < 0 && g_owner[1] >= 0 && i != g_owner[1]) g_owner[0] = i;   // only player 2's gun was picked
    if (rm.usFlags & MOUSE_MOVE_ABSOLUTE) {
        m.absolute = true;   // tablets and light guns: 0..65535 over the screen
        bool virt = rm.usFlags & MOUSE_VIRTUAL_DESKTOP;
        int left = virt ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0, top = virt ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        int h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        m.x = left + MulDiv(rm.lLastX, w, 65535);
        m.y = top + MulDiv(rm.lLastY, h, 65535);
        m.have_pos = true;
    } else if (rm.lLastX || rm.lLastY) {   // one pixel per count, without the pointer's acceleration
        if (!m.have_pos) {
            POINT p;
            GetCursorPos(&p);
            m.x = p.x, m.y = p.y;
            m.have_pos = true;
        }
        m.x += rm.lLastX;
        m.y += rm.lLastY;
        keep_inside(m);
    }
    USHORT b = rm.usButtonFlags;
    if (b & RI_MOUSE_LEFT_BUTTON_DOWN) {
        m.left = m.clicked = true;
        m.presses++;
    }
    if (b & RI_MOUSE_LEFT_BUTTON_UP) m.left = false;
    if (b & RI_MOUSE_RIGHT_BUTTON_DOWN) m.right = true;
    if (b & RI_MOUSE_RIGHT_BUTTON_UP) m.right = false;
    if (!(b & RI_MOUSE_LEFT_BUTTON_DOWN)) return;
    int first = g_owner[0] >= 0 ? g_owner[0] : g_last_clicked;   // player 1's gun picked in GUN TEST, or the one it played with
    if (g_owner[1] < 0 && g_join_allowed && first >= 0 && i != first) {
        g_owner[0] = first;
        g_owner[1] = i;
        g_mice[g_owner[0]].have_pos = false;   // it starts where the pointer is
        printf("[mouse] second mouse: player 2's gun\n");
    }
    if (g_owner[1] < 0) g_last_clicked = i;
}

void update(HWND hwnd, bool join_allowed, bool in_game) {
    g_join_allowed = join_allowed;
    POINT tl{0, 0};
    RECT rc;
    GetClientRect(hwnd, &rc);
    ClientToScreen(hwnd, &tl);
    g_area = {tl.x, tl.y, tl.x + rc.right, tl.y + rc.bottom};
    g_have_area = rc.right > 0 && rc.bottom > 0;
    // With two mice the pointer moves with both: kept in the window so a
    // click never lands outside it (the menus get it back)
    bool clip = two_mice() && in_game && GetForegroundWindow() == hwnd && g_have_area;
    if (clip) ClipCursor(&g_area);
    else if (g_clipped) ClipCursor(nullptr);
    g_clipped = clip;
}

bool two_mice() { return g_owner[1] >= 0; }

void reset() {
    g_owner[0] = g_owner[1] = -1;
    g_saved[0].clear(), g_saved[1].clear();
    if (g_clipped) ClipCursor(nullptr);
    g_clipped = false;
}

Gun gun(int player, HWND hwnd) {
    Gun g{};
    if (g_owner[player] < 0) return g;
    Mouse& m = g_mice[g_owner[player]];
    if (!m.have_pos) {
        POINT p;
        GetCursorPos(&p);
        m.x = p.x, m.y = p.y;
        m.have_pos = true;
    }
    g.pos = {m.x, m.y};
    ScreenToClient(hwnd, &g.pos);
    g.trigger = m.left;
    g.reload = m.right;
    g.clicked = m.clicked;
    m.clicked = false;
    return g;
}

std::vector<Device> devices(HWND hwnd) {
    std::vector<Device> out;
    // GUN TEST asks at every refresh: the devices plugged in are listed again
    // four times a second, what each one does is read every time
    static std::vector<RAWINPUTDEVICELIST> list;
    static double listed_at = -1;
    if (double now = now_seconds(); listed_at < 0 || now - listed_at >= 0.25) {
        listed_at = now;
        UINT n = 0;
        list.clear();
        if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) == 0 && n) {
            list.resize(n);
            n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
            if (n == UINT(-1)) n = 0;
            list.resize(n);
        }
    }
    for (const RAWINPUTDEVICELIST& d : list) {
        if (d.dwType != RIM_TYPEMOUSE) continue;
        int i = find(d.hDevice);
        const Mouse& m = g_mice[i];
        if (m.path.find("rdp_mou") != std::string::npos) continue;   // Remote Desktop's, never a real one
        Device o;
        o.path = m.path;
        o.name = m.name;
        o.absolute = m.absolute;
        o.has_pos = m.have_pos;
        o.pos = {m.x, m.y};
        ScreenToClient(hwnd, &o.pos);
        o.trigger = m.left;
        o.reload = m.right;
        o.presses = m.presses;
        o.last_input = m.last_input;
        o.player = i == g_owner[0] ? 0 : i == g_owner[1] ? 1 : -1;
        out.push_back(o);
    }
    return out;
}

}  // namespace raw_mice
