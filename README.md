# Battery Status

Ein kleines Windows-Tool, das für **jedes verbundene Bluetooth-Gerät ein eigenes Icon im Infobereich der Taskleiste** anzeigt – mit Gerätesymbol und Akkuanzeige.

## Funktionen

- Ein Tray-Icon pro Gerät: oben das Gerätesymbol (Kopfhörer, Maus, Tastatur, Controller, Lautsprecher), darunter der Akkustand
- Tooltip mit Gerätename und Akkustand in Prozent
- **AirPods-Unterstützung**: Akkustand von linkem/rechtem Ohrhörer und Ladecase über Bluetooth-LE
- Benachrichtigung bei schwachem Akku (≤ 20 %)
- Passt sich automatisch an helles/dunkles Windows-Design an
- Rechtsklick-Menü: Bluetooth-Einstellungen öffnen, aktualisieren, Autostart ein/aus, beenden

Die Akkuwerte stammen aus derselben Quelle wie in den Windows-Einstellungen (`DEVPKEY_Bluetooth_Battery`). Geräte, die Windows keinen Akkustand meldet, werden ohne Prozentwert angezeigt.

## Voraussetzungen

- Windows 10 oder 11
- Zum Bauen: CMake ≥ 3.25 und ein C/C++-Compiler mit C++23-Unterstützung (getestet mit MinGW)

## Installation

Das mitgelieferte Skript baut das Programm als Release, installiert es nach `%LOCALAPPDATA%\Programs\BatteryStatus` und richtet den Autostart ein:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

Deinstallieren:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
```

> **Hinweis:** `install.ps1` verwendet derzeit die CMake- und MinGW-Toolchain, die mit **JetBrains CLion** installiert wird. Ohne CLion bitte manuell bauen (siehe unten).

## Manuell bauen

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Die fertige `Battery_Status.exe` liegt danach in `build\`. Bei MinGW wird `libwinpthread-1.dll` automatisch daneben kopiert – sie muss zusammen mit der `.exe` weitergegeben werden.

## Lizenz

[MIT](LICENSE)
