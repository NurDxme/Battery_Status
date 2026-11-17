// Bluetooth-LE-Empfang über die WinRT-ABI (C-Schnittstelle der MinGW-Header),
// damit kein C++/WinRT bzw. Windows SDK benötigt wird.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.storage.streams.h>

#include "apple_ble.h"

typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher Watcher;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs RecvArgs;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement Advertisement;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData MfgData;
typedef __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData MfgVector;
typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer Buffer;
typedef __FITypedEventHandler_2_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementWatcher_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementReceivedEventArgs RecvHandlerItf;
typedef __FITypedEventHandler_2_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementWatcher_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementReceivedEventArgsVtbl RecvHandlerVtbl;

// IBufferByteAccess (robuffer.h) – selbst deklariert, um nur diese eine Methode zu brauchen
typedef struct ByteAccess ByteAccess;
typedef struct ByteAccessVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(ByteAccess*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(ByteAccess*);
    ULONG(STDMETHODCALLTYPE* Release)(ByteAccess*);
    HRESULT(STDMETHODCALLTYPE* Buffer)(ByteAccess*, BYTE**);
} ByteAccessVtbl;
struct ByteAccess { const ByteAccessVtbl* lpVtbl; };

static const GUID kIID_ByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const GUID kIID_AgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// ---------------------------------------------------------------------------
// Messwerte
// ---------------------------------------------------------------------------

#define MAX_ENTRIES 16

typedef struct Entry {
    unsigned short model;
    AppleBattery bat;
    ULONGLONG tick;
} Entry;

static SRWLOCK g_lock = SRWLOCK_INIT;
static Entry g_entries[MAX_ENTRIES];
static int g_count = 0;

static HWND g_notifyWnd = NULL;
static UINT g_notifyMsg = 0;
static HANDLE g_thread = NULL;
static HANDLE g_stopEvent = NULL;

static int nibbleToPercent(int n) { return n <= 10 ? n * 10 : -1; } // 15 = unbekannt

// Apple Proximity Pairing (Daten ohne die Hersteller-ID):
// [0]=0x07 Typ, [1]=Länge, [2]=?, [3..4]=Modell (LE), [5]=Status,
// [6]=Akku Ohrhörer (2 Nibbles), [7]=Ladeflags (hi) + Akku Etui (lo)
static void handlePayload(const BYTE* d, UINT32 len, int rssi) {
    if (len < 8 || d[0] != 0x07) return;

    const unsigned short model = (unsigned short)(d[3] | (d[4] << 8));
    const int flipped = (d[5] & 0x20) == 0;
    const int hi = d[6] >> 4, lo = d[6] & 0x0F;
    const int flags = d[7] >> 4;

    AppleBattery b;
    b.left = nibbleToPercent(flipped ? hi : lo);
    b.right = nibbleToPercent(flipped ? lo : hi);
    b.caseBattery = nibbleToPercent(d[7] & 0x0F);
    b.chargingLeft = (flags & (flipped ? 0x2 : 0x1)) != 0;
    b.chargingRight = (flags & (flipped ? 0x1 : 0x2)) != 0;
    b.chargingCase = (flags & 0x4) != 0;
    b.rssi = rssi;
    if (b.left < 0 && b.right < 0 && b.caseBattery < 0) return;

    const ULONGLONG now = GetTickCount64();
    int changed = 0;

    AcquireSRWLockExclusive(&g_lock);
    int idx = -1;
    for (int i = 0; i < g_count; ++i)
        if (g_entries[i].model == model) { idx = i; break; }
    if (idx < 0) {
        if (g_count < MAX_ENTRIES) idx = g_count++;
        else { // ältesten Eintrag ersetzen
            idx = 0;
            for (int i = 1; i < g_count; ++i)
                if (g_entries[i].tick < g_entries[idx].tick) idx = i;
        }
        g_entries[idx].model = model;
        g_entries[idx].tick = 0;
    }
    Entry* e = &g_entries[idx];
    // Bei gleichem Modell in der Nähe (z.B. fremde AirPods) das stärkere Signal bevorzugen
    if (e->tick == 0 || now - e->tick > 10000 || rssi >= e->bat.rssi - 5) {
        changed = e->tick == 0 || e->bat.left != b.left || e->bat.right != b.right ||
                  e->bat.caseBattery != b.caseBattery || e->bat.chargingLeft != b.chargingLeft ||
                  e->bat.chargingRight != b.chargingRight || e->bat.chargingCase != b.chargingCase;
        e->bat = b;
        e->tick = now;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (changed && g_notifyWnd) PostMessageW(g_notifyWnd, g_notifyMsg, 0, 0);
}

BOOL apple_ble_query(unsigned short model, DWORD maxAgeMs, AppleBattery* out) {
    const ULONGLONG now = GetTickCount64();
    const Entry* best = NULL;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_count; ++i) {
        const Entry* e = &g_entries[i];
        if (now - e->tick > maxAgeMs) continue;
        if (e->model == model) { best = e; break; }
    }
    if (!best) { // Fallback: Modellnummer unbekannt → nur ein sehr nahes Gerät akzeptieren
        for (int i = 0; i < g_count; ++i) {
            const Entry* e = &g_entries[i];
            if (now - e->tick > maxAgeMs || e->bat.rssi < -60) continue;
            if (!best || e->bat.rssi > best->bat.rssi) best = e;
        }
    }
    if (best) *out = best->bat;
    ReleaseSRWLockShared(&g_lock);
    return best != NULL;
}

// ---------------------------------------------------------------------------
// Event-Handler (statisches COM-Objekt)
// ---------------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE h_QueryInterface(RecvHandlerItf* This, REFIID riid, void** ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &kIID_AgileObject) ||
        IsEqualGUID(riid, &IID___FITypedEventHandler_2_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementWatcher_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementReceivedEventArgs)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_AddRef(RecvHandlerItf* This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE h_Release(RecvHandlerItf* This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE h_Invoke(RecvHandlerItf* This, Watcher* sender, RecvArgs* args) {
    (void)This; (void)sender;
    INT16 rssi = -127;
    Advertisement* adv = NULL;
    MfgVector* vec = NULL;
    UINT32 n = 0;

    args->lpVtbl->get_RawSignalStrengthInDBm(args, &rssi);
    if (FAILED(args->lpVtbl->get_Advertisement(args, &adv)) || !adv) return S_OK;
    if (SUCCEEDED(adv->lpVtbl->get_ManufacturerData(adv, &vec)) && vec) {
        vec->lpVtbl->get_Size(vec, &n);
        for (UINT32 i = 0; i < n; ++i) {
            MfgData* md = NULL;
            UINT16 company = 0;
            if (FAILED(vec->lpVtbl->GetAt(vec, i, &md)) || !md) continue;
            md->lpVtbl->get_CompanyId(md, &company);
            if (company == 0x004C) {
                Buffer* buf = NULL;
                if (SUCCEEDED(md->lpVtbl->get_Data(md, &buf)) && buf) {
                    UINT32 len = 0;
                    ByteAccess* ba = NULL;
                    BYTE* bytes = NULL;
                    buf->lpVtbl->get_Length(buf, &len);
                    if (SUCCEEDED(buf->lpVtbl->QueryInterface(buf, &kIID_ByteAccess, (void**)&ba)) && ba) {
                        if (SUCCEEDED(ba->lpVtbl->Buffer(ba, &bytes)) && bytes) handlePayload(bytes, len, rssi);
                        ba->lpVtbl->Release(ba);
                    }
                    buf->lpVtbl->Release(buf);
                }
            }
            md->lpVtbl->Release(md);
        }
        vec->lpVtbl->Release(vec);
    }
    adv->lpVtbl->Release(adv);
    return S_OK;
}

static RecvHandlerVtbl g_handlerVtbl = {h_QueryInterface, h_AddRef, h_Release, h_Invoke};
static RecvHandlerItf g_handler = {&g_handlerVtbl};

// ---------------------------------------------------------------------------
// Empfangsthread
// ---------------------------------------------------------------------------

static Watcher* createWatcher(void) {
    static const WCHAR cls[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
    HSTRING name = NULL;
    IInspectable* insp = NULL;
    Watcher* w = NULL;
    if (FAILED(WindowsCreateString(cls, (UINT32)(sizeof(cls) / sizeof(WCHAR) - 1), &name))) return NULL;
    if (SUCCEEDED(RoActivateInstance(name, &insp)) && insp) {
        insp->lpVtbl->QueryInterface(insp, &IID___x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher, (void**)&w);
        insp->lpVtbl->Release(insp);
    }
    WindowsDeleteString(name);
    if (w) {
        EventRegistrationToken token;
        if (FAILED(w->lpVtbl->add_Received(w, &g_handler, &token))) {
            w->lpVtbl->Release(w);
            w = NULL;
        }
    }
    return w;
}

static DWORD WINAPI watcherThread(LPVOID param) {
    (void)param;
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;

    Watcher* w = NULL;
    do {
        // Watcher bei Bedarf (neu) starten, z.B. nachdem Bluetooth aus- und wieder eingeschaltet wurde
        if (!w) w = createWatcher();
        if (w) {
            __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CBluetoothLEAdvertisementWatcherStatus st;
            if (SUCCEEDED(w->lpVtbl->get_Status(w, &st))) {
                if (st == BluetoothLEAdvertisementWatcherStatus_Aborted) {
                    w->lpVtbl->Release(w);
                    w = createWatcher();
                    if (w) w->lpVtbl->Start(w);
                } else if (st == BluetoothLEAdvertisementWatcherStatus_Created ||
                           st == BluetoothLEAdvertisementWatcherStatus_Stopped) {
                    w->lpVtbl->Start(w);
                }
            }
        }
    } while (WaitForSingleObject(g_stopEvent, 15000) == WAIT_TIMEOUT);

    if (w) {
        w->lpVtbl->Stop(w);
        w->lpVtbl->Release(w);
    }
    RoUninitialize();
    return 0;
}

BOOL apple_ble_start(HWND notifyWnd, UINT notifyMsg) {
    if (g_thread) return TRUE;
    g_notifyWnd = notifyWnd;
    g_notifyMsg = notifyMsg;
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, watcherThread, NULL, 0, NULL);
    return g_thread != NULL;
}

void apple_ble_stop(void) {
    if (!g_thread) return;
    g_notifyWnd = NULL;
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stopEvent);
    g_thread = NULL;
    g_stopEvent = NULL;
}
