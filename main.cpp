// Battery_Status – zeigt für jedes verbundene Bluetooth-Gerät ein eigenes Icon
// im Infobereich der Taskleiste (unten rechts). Oben im Icon das Gerätesymbol,
// darunter eine Akkuanzeige. Tooltip: Gerätename + Akkustand in Prozent.
//
// Die Akkuwerte kommen aus derselben Quelle wie in den Windows-Einstellungen
// (Geräteeigenschaft DEVPKEY_Bluetooth_Battery), es wird also kein WinRT benötigt.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define NTDDI_VERSION 0x0A000006

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <setupapi.h>
#include <devpropdef.h>

#include "apple_ble.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Geräteeigenschaften
// ---------------------------------------------------------------------------

namespace key {
// Akkustand in Prozent (BYTE), wird von Windows für BT-Classic (HFP) und BLE gesetzt
const DEVPROPKEY Battery{{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
// "Ist verbunden" (BOOLEAN) auf dem Bluetooth-Geräteknoten
const DEVPROPKEY Connected{{0x83DA6326, 0x97A6, 0x4088, {0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29}}, 15};
// Bluetooth Class of Device (UINT32)
const DEVPROPKEY ClassOfDevice{{0x2BD67D8B, 0x8BEB, 0x48D5, {0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A}}, 4};
const DEVPROPKEY DeviceDesc{{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 2};
const DEVPROPKEY Class{{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 9};
const DEVPROPKEY FriendlyName{{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
const DEVPROPKEY ContainerId{{0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};
} // namespace key

enum class Kind { Headphones, Speaker, Mouse, Keyboard, Gamepad, Phone, Generic };

struct Node {
    std::wstring id, name, cls, container;
    std::optional<int> battery;
    std::optional<bool> connected;
    std::optional<uint32_t> cod;
};

struct Device {
    std::wstring container, name;
    Kind kind = Kind::Generic;
    std::optional<int> battery;
    std::optional<uint16_t> applePid; // gesetzt bei Apple/Beats-Geräten
    std::wstring detail;              // z.B. "L 80 % · R 70 % · Etui 50 %"
};

static bool readProp(HDEVINFO h, SP_DEVINFO_DATA& d, const DEVPROPKEY& k, DEVPROPTYPE& type, std::vector<BYTE>& buf) {
    DWORD size = 0;
    type = 0;
    SetupDiGetDevicePropertyW(h, &d, &k, &type, nullptr, 0, &size, 0);
    if (size == 0) return false;
    buf.resize(size);
    return SetupDiGetDevicePropertyW(h, &d, &k, &type, buf.data(), size, &size, 0) != FALSE;
}

static std::optional<std::wstring> propString(HDEVINFO h, SP_DEVINFO_DATA& d, const DEVPROPKEY& k) {
    DEVPROPTYPE t;
    std::vector<BYTE> b;
    if (!readProp(h, d, k, t, b) || t != DEVPROP_TYPE_STRING) return std::nullopt;
    return std::wstring(reinterpret_cast<const wchar_t*>(b.data()));
}

static std::optional<uint32_t> propUInt(HDEVINFO h, SP_DEVINFO_DATA& d, const DEVPROPKEY& k) {
    DEVPROPTYPE t;
    std::vector<BYTE> b;
    if (!readProp(h, d, k, t, b)) return std::nullopt;
    switch (t) {
        case DEVPROP_TYPE_BYTE:   return b[0];
        case DEVPROP_TYPE_UINT16: return *reinterpret_cast<const uint16_t*>(b.data());
        case DEVPROP_TYPE_UINT32: return *reinterpret_cast<const uint32_t*>(b.data());
        default:                  return std::nullopt;
    }
}

static std::optional<bool> propBool(HDEVINFO h, SP_DEVINFO_DATA& d, const DEVPROPKEY& k) {
    DEVPROPTYPE t;
    std::vector<BYTE> b;
    if (!readProp(h, d, k, t, b) || t != DEVPROP_TYPE_BOOLEAN) return std::nullopt;
    return b[0] != 0;
}

static std::optional<std::wstring> propGuid(HDEVINFO h, SP_DEVINFO_DATA& d, const DEVPROPKEY& k) {
    DEVPROPTYPE t;
    std::vector<BYTE> b;
    if (!readProp(h, d, k, t, b) || t != DEVPROP_TYPE_GUID) return std::nullopt;
    const GUID& g = *reinterpret_cast<const GUID*>(b.data());
    wchar_t s[40];
    swprintf(s, 40, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::wstring(s);
}

static bool startsWithI(const std::wstring& s, const wchar_t* prefix) {
    return _wcsnicmp(s.c_str(), prefix, wcslen(prefix)) == 0;
}

// Der eigentliche Geräteknoten (nicht ein Dienst/Profil des Geräts)
static bool isBluetoothDeviceNode(const std::wstring& id) {
    return startsWithI(id, L"BTHENUM\\DEV_") || startsWithI(id, L"BTHLE\\DEV_");
}

static std::vector<Node> enumerateNodes() {
    std::vector<Node> out;
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return out;

    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); ++i) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(h, &d, id, 512, nullptr)) continue;
        auto container = propGuid(h, d, key::ContainerId);
        if (!container) continue;

        Node n;
        n.id = id;
        n.container = *container;
        n.cls = propString(h, d, key::Class).value_or(L"");
        if (auto b = propUInt(h, d, key::Battery); b && *b <= 100) n.battery = static_cast<int>(*b);
        if (isBluetoothDeviceNode(n.id)) {
            n.name = propString(h, d, key::FriendlyName).value_or(propString(h, d, key::DeviceDesc).value_or(L""));
            n.connected = propBool(h, d, key::Connected);
            n.cod = propUInt(h, d, key::ClassOfDevice);
        }
        out.push_back(std::move(n));
    }
    SetupDiDestroyDeviceInfoList(h);
    return out;
}

static bool containsI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring a = hay, b = needle;
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    return a.find(b) != std::wstring::npos;
}

static Kind detectKind(const std::wstring& name, std::optional<uint32_t> cod, const std::set<std::wstring>& classes) {
    if (cod && *cod) {
        const uint32_t major = (*cod >> 8) & 0x1F;
        const uint32_t minor = (*cod >> 2) & 0x3F;
        switch (major) {
            case 0x02: return Kind::Phone;
            case 0x04: return (minor == 0x05) ? Kind::Speaker : Kind::Headphones;
            case 0x05: {
                const uint32_t low = minor & 0x0F;
                if (low == 0x01 || low == 0x02) return Kind::Gamepad;
                const uint32_t upper = (minor >> 4) & 0x03;
                if (upper == 0x02) return Kind::Mouse;
                if (upper == 0x01 || upper == 0x03) return Kind::Keyboard;
                break;
            }
            default: break;
        }
    }
    if (containsI(name, L"controller") || containsI(name, L"gamepad")) return Kind::Gamepad;
    if (containsI(name, L"mouse") || containsI(name, L"maus")) return Kind::Mouse;
    if (containsI(name, L"keyboard") || containsI(name, L"tastatur")) return Kind::Keyboard;
    if (containsI(name, L"speaker") || containsI(name, L"lautsprecher")) return Kind::Speaker;
    if (containsI(name, L"buds") || containsI(name, L"airpods") || containsI(name, L"head")) return Kind::Headphones;
    if (classes.count(L"xnacomposite") || classes.count(L"xboxcomposite")) return Kind::Gamepad;
    if (classes.count(L"mouse")) return Kind::Mouse;
    if (classes.count(L"keyboard")) return Kind::Keyboard;
    if (classes.count(L"audioendpoint") || classes.count(L"media")) return Kind::Headphones;
    return Kind::Generic;
}

// Apple-Geräte haben Dienstknoten wie "BTHENUM\{...}_VID&0001004C_PID&200E\..."
static std::optional<uint16_t> applePidOf(const std::vector<const Node*>& list) {
    static const wchar_t* tag = L"VID&0001004C_PID&";
    for (const Node* n : list) {
        std::wstring id = n->id;
        std::transform(id.begin(), id.end(), id.begin(), ::towupper);
        if (auto pos = id.find(tag); pos != std::wstring::npos)
            return static_cast<uint16_t>(wcstoul(id.substr(pos + wcslen(tag), 4).c_str(), nullptr, 16));
    }
    return std::nullopt;
}

static std::wstring pctText(int v, bool charging) {
    return std::to_wstring(v) + L" %" + (charging ? L"⚡" : L"");
}

// AirPods: Akku aus den BLE-Werbepaketen (Wert der beiden Ohrhörer = der niedrigere)
static void applyAppleBattery(Device& dev) {
    AppleBattery ab{};
    if (!apple_ble_query(*dev.applePid, 5 * 60 * 1000, &ab)) return;

    std::wstring detail;
    auto add = [&](const wchar_t* label, int v, int charging) {
        if (v < 0) return;
        if (!detail.empty()) detail += L" · ";
        detail += label + pctText(v, charging != 0);
    };
    add(L"L ", ab.left, ab.chargingLeft);
    add(L"R ", ab.right, ab.chargingRight);
    add(L"Etui ", ab.caseBattery, ab.chargingCase);
    dev.detail = detail;

    if (ab.left >= 0 && ab.right >= 0) dev.battery = std::min(ab.left, ab.right);
    else if (ab.left >= 0) dev.battery = ab.left;
    else if (ab.right >= 0) dev.battery = ab.right;
}

static std::vector<Device> queryDevices() {
    const auto nodes = enumerateNodes();
    std::map<std::wstring, std::vector<const Node*>> byContainer;
    for (const auto& n : nodes) byContainer[n.container].push_back(&n);

    std::vector<Device> devices;
    for (const auto& [container, list] : byContainer) {
        const Node* root = nullptr;
        bool connected = false, haveConnectedProp = false;
        std::optional<int> battery;
        std::set<std::wstring> classes;

        for (const Node* n : list) {
            if (isBluetoothDeviceNode(n->id)) {
                if (!root || (root->name.empty() && !n->name.empty())) root = n;
                if (n->connected) {
                    haveConnectedProp = true;
                    connected |= *n->connected;
                }
            }
            if (n->battery) battery = std::max(battery.value_or(0), *n->battery);
            std::wstring c = n->cls;
            std::transform(c.begin(), c.end(), c.begin(), ::towlower);
            if (!c.empty()) classes.insert(c);
        }
        if (!root) continue;
        // Falls Windows keinen Verbindungsstatus liefert: Akkuwert vorhanden = verbunden
        if (!connected && (haveConnectedProp || !battery)) continue;

        Device dev;
        dev.container = container;
        dev.name = root->name.empty() ? L"Bluetooth-Gerät" : root->name;
        dev.battery = battery;
        std::optional<uint32_t> cod;
        for (const Node* n : list) if (n->cod && *n->cod) cod = n->cod;
        dev.kind = detectKind(dev.name, cod, classes);
        dev.applePid = applePidOf(list);
        if (!dev.battery && dev.applePid) applyAppleBattery(dev);
        devices.push_back(std::move(dev));
    }
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) { return a.name < b.name; });
    return devices;
}

// ---------------------------------------------------------------------------
// Icon zeichnen: oben Gerätesymbol (Segoe Fluent Icons), unten Akkubalken
// ---------------------------------------------------------------------------

static const wchar_t* glyphFor(Kind k) {
    switch (k) {
        case Kind::Headphones: return L"";
        case Kind::Speaker:    return L"";
        case Kind::Mouse:      return L"";
        case Kind::Keyboard:   return L"";
        case Kind::Gamepad:    return L"";
        case Kind::Phone:      return L"";
        default:               return L""; // Bluetooth
    }
}

static bool isLightTaskbar() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}

static HFONT createIconFont(int height) {
    for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        HFONT f = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        if (!f) continue;
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t actual[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, old);
        DeleteDC(dc);
        if (_wcsicmp(actual, face) == 0) return f;
        DeleteObject(f);
    }
    return nullptr;
}

static HICON makeIcon(Kind kind, std::optional<int> battery, bool drawBattery) {
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    const bool light = isLightTaskbar();
    const uint32_t fg = light ? 0x1C1C1C : 0xFFFFFF;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) { DeleteDC(dc); return nullptr; }
    auto* px = static_cast<uint32_t*>(bits);
    std::fill(px, px + S * S, 0u);

    // Höhe des Akkubalkens; der Rest gehört dem Gerätesymbol
    const int bh = drawBattery ? std::max(4, S / 4) : 0;
    const int glyphH = S - bh - (drawBattery ? 1 : 0);

    // 1) Symbol weiß auf schwarz zeichnen, Helligkeit wird anschließend zur Alpha-Maske
    HGDIOBJ oldBmp = SelectObject(dc, color);
    if (HFONT font = createIconFont(glyphH)) {
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{0, 0, S, glyphH};
        DrawTextW(dc, glyphFor(kind), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    GdiFlush();
    for (int i = 0; i < S * S; ++i) {
        const uint32_t p = px[i];
        const uint32_t a = std::max({p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF});
        px[i] = a ? ((a << 24) | fg) : 0;
    }

    // 2) Akkubalken
    if (drawBattery) {
        auto put = [&](int x, int y, uint32_t rgb, uint32_t a = 0xFF) {
            if (x >= 0 && y >= 0 && x < S && y < S) px[y * S + x] = (a << 24) | rgb;
        };
        const int top = S - bh, bottom = S - 1;
        const int nubW = std::max(1, S / 12);
        const int right = S - 1 - nubW; // letzte Spalte des Balkenkörpers
        const uint32_t outlineA = battery ? 0xFF : 0x80;

        for (int x = 0; x <= right; ++x) { put(x, top, fg, outlineA); put(x, bottom, fg, outlineA); }
        for (int y = top; y <= bottom; ++y) { put(0, y, fg, outlineA); put(right, y, fg, outlineA); }
        const int nubTop = top + bh / 4, nubBottom = bottom - bh / 4;
        for (int x = right + 1; x < S; ++x)
            for (int y = nubTop; y <= nubBottom; ++y) put(x, y, fg, outlineA);

        if (battery) {
            const int pct = std::clamp(*battery, 0, 100);
            const uint32_t fill = pct > 50 ? 0x3CC13C : pct > 20 ? 0xF5B800 : 0xE53935;
            const int innerW = right - 1;
            int w = (innerW * pct + 50) / 100;
            if (pct > 0) w = std::max(w, 1);
            for (int x = 1; x <= w; ++x)
                for (int y = top + 1; y < bottom; ++y) put(x, y, fill);
        }
    }

    SelectObject(dc, oldBmp);
    DeleteDC(dc);

    std::vector<BYTE> maskBits(((S + 15) / 16) * 2 * S, 0);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, maskBits.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// ---------------------------------------------------------------------------
// Infobereich (Tray)
// ---------------------------------------------------------------------------

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_APPLE_BATTERY = WM_APP + 2;
constexpr UINT PLACEHOLDER_UID = 1;
constexpr UINT_PTR TIMER_POLL = 1, TIMER_DEVCHANGE = 2;
constexpr UINT POLL_MS = 30'000;
constexpr int LOW_BATTERY = 20;

enum MenuId : UINT { ID_SETTINGS = 100, ID_REFRESH, ID_AUTOSTART, ID_EXIT };

struct TrayEntry {
    UINT uid = 0;
    HICON icon = nullptr;
    bool warned = false;
};

static HWND g_hwnd = nullptr;
static UINT g_taskbarCreated = 0;
static std::map<std::wstring, TrayEntry> g_tray; // Key: ContainerId bzw. "" für Platzhalter

// Stabile ID je Gerät, damit Windows sich "Symbol immer anzeigen" pro Gerät merkt
static UINT uidFor(const std::wstring& container) {
    uint32_t h = 2166136261u;
    for (wchar_t c : container) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x10000;
}

static void copyTo(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = L'\0';
}

static bool setTrayIcon(TrayEntry& e, bool isNew, HICON icon, const std::wstring& tip,
                        const std::wstring& balloonTitle = L"", const std::wstring& balloonText = L"") {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = e.uid;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    copyTo(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (!balloonText.empty()) {
        nid.uFlags |= NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        copyTo(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), balloonTitle);
        copyTo(nid.szInfo, ARRAYSIZE(nid.szInfo), balloonText);
    }
    if (isNew) {
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        return true;
    }
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

static void removeTrayIcon(TrayEntry& e) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = e.uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (e.icon) DestroyIcon(e.icon);
    e.icon = nullptr;
}

static void upsert(const std::wstring& key, UINT uid, HICON icon, const std::wstring& tip,
                   std::optional<int> battery, const std::wstring& name) {
    auto it = g_tray.find(key);
    const bool isNew = it == g_tray.end();
    TrayEntry e = isNew ? TrayEntry{uid} : it->second;

    std::wstring bTitle, bText;
    if (battery && *battery <= LOW_BATTERY && !e.warned) {
        bTitle = L"Akku schwach";
        bText = name + L": noch " + std::to_wstring(*battery) + L" %";
        e.warned = true;
    } else if (battery && *battery > LOW_BATTERY + 5) {
        e.warned = false;
    }

    if (!setTrayIcon(e, isNew, icon, tip, bTitle, bText)) {
        DestroyIcon(icon); // Taskleiste evtl. noch nicht bereit – nächster Durchlauf versucht es erneut
        return;
    }
    if (e.icon) DestroyIcon(e.icon);
    e.icon = icon;
    g_tray[key] = e;
}

static void refresh() {
    const auto devices = queryDevices();
    std::set<std::wstring> seen;

    for (const auto& d : devices) {
        std::wstring tip = d.name + L"\n" +
                           (!d.detail.empty() ? d.detail
                                              : L"Akku: " + (d.battery ? std::to_wstring(*d.battery) + L" %" : L"unbekannt"));
        upsert(d.container, uidFor(d.container), makeIcon(d.kind, d.battery, true), tip, d.battery, d.name);
        seen.insert(d.container);
    }
    if (devices.empty()) {
        upsert(L"", PLACEHOLDER_UID, makeIcon(Kind::Generic, std::nullopt, false),
               L"Keine verbundenen Bluetooth-Geräte", std::nullopt, L"");
        seen.insert(L"");
    }

    for (auto it = g_tray.begin(); it != g_tray.end();) {
        if (!seen.count(it->first)) {
            removeTrayIcon(it->second);
            it = g_tray.erase(it);
        } else {
            ++it;
        }
    }
}

static void removeAll() {
    for (auto& [k, e] : g_tray) removeTrayIcon(e);
    g_tray.clear();
}

// ---------------------------------------------------------------------------
// Autostart (HKCU\...\Run)
// ---------------------------------------------------------------------------

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"BatteryStatus";

static bool autostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void setAutostart(bool enable) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// Fenster / Nachrichten
// ---------------------------------------------------------------------------

static void openBluetoothSettings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
}

static void showMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Bluetooth-Einstellungen öffnen");
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Jetzt aktualisieren");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Mit Windows starten");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Beenden");

    SetForegroundWindow(g_hwnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case ID_SETTINGS:  openBluetoothSettings(); break;
        case ID_REFRESH:   refresh(); break;
        case ID_AUTOSTART: setAutostart(!autostartEnabled()); break;
        case ID_EXIT:      DestroyWindow(g_hwnd); break;
        default: break;
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { // Explorer neu gestartet
        for (auto& [k, e] : g_tray) if (e.icon) DestroyIcon(e.icon);
        g_tray.clear();
        refresh();
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT: openBluetoothSettings(); break;
                case WM_CONTEXTMENU: showMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
                default: break;
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_DEVCHANGE) KillTimer(hwnd, TIMER_DEVCHANGE);
            refresh();
            return 0;
        case WM_APPLE_BATTERY: // neuer AirPods-Messwert – gebündelt aktualisieren
            SetTimer(hwnd, TIMER_DEVCHANGE, 1500, nullptr);
            return 0;
        case WM_DEVICECHANGE:
            // Verbinden/Trennen: kurz warten, bis Windows die Eigenschaften aktualisiert hat
            SetTimer(hwnd, TIMER_DEVCHANGE, 1500, nullptr);
            return TRUE;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) refresh();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            apple_ble_stop();
            removeAll();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\BatteryStatusTray_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"BatteryStatusTrayWindow";
    RegisterClassExW(&wc);

    // Unsichtbares Top-Level-Fenster (kein Message-Only-Fenster, sonst kommt kein WM_DEVICECHANGE an)
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Battery Status", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    apple_ble_start(g_hwnd, WM_APPLE_BATTERY);
    refresh();
    SetTimer(g_hwnd, TIMER_POLL, POLL_MS, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
