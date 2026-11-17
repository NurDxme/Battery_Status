// Liest den Akkustand von AirPods/Beats aus deren Bluetooth-LE-Werbepaketen
// (Apple "Proximity Pairing", Hersteller-ID 0x004C). Windows selbst stellt für
// AirPods keinen Akkuwert bereit.
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AppleBattery {
    int left, right, caseBattery;              // 0..100, -1 = unbekannt
    int chargingLeft, chargingRight, chargingCase;
    int rssi;                                  // Signalstärke in dBm
} AppleBattery;

// Startet den Empfang in einem eigenen Thread. Bei geänderten Werten wird
// notifyMsg an notifyWnd gepostet.
BOOL apple_ble_start(HWND notifyWnd, UINT notifyMsg);
void apple_ble_stop(void);

// Letzter Messwert für das Modell (= PID aus der Bluetooth-Geräte-ID, z.B. 0x200E).
// Gibt es dafür keinen, wird ein sehr nahes Apple-Gerät (starkes Signal) genommen.
BOOL apple_ble_query(unsigned short model, DWORD maxAgeMs, AppleBattery* out);

#ifdef __cplusplus
}
#endif
