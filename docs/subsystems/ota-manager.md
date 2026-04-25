# Subsystem — `ota_manager`

- **Fichiers** : [`src/ota_manager.h`](../../src/ota_manager.h), [`src/ota_manager.cpp`](../../src/ota_manager.cpp)
- **Singleton** : `extern OTAManager otaManager;`
- **Lib** : ArduinoOTA (ESP32 core)

## Rôle

Gère les mises à jour OTA **firmware** et **filesystem** (LittleFS). Supporte deux canaux :
1. **Arduino OTA** (push IDE / PlatformIO) — réservé au développement, désactivé en production (`otaEnabled = false` par défaut).
2. **HTTP OTA** via les routes web `/update`, `/check-update`, `/download-update` — canal utilisé par l'UI.

## Partitions

Double-bank app0/app1 pour l'OTA firmware :

| Partition | Type | Taille |
|-----------|------|--------|
| `nvs` | data | 20 KB |
| `otadata` | data | 8 KB |
| `app0` | app | **1408 KB** |
| `app1` | app | **1408 KB** |
| `spiffs` | data | 1088 KB |
| `history` | data | 128 KB |

Voir [`partitions.csv`](../../partitions.csv) et [ADR-0007](../adr/0007-table-partitions-custom.md).

## API publique

```cpp
void begin();
void handle();                       // appelé dans loop() si OTA enabled
void setPassword(const String& password);
bool isEnabled() const;
```

## Flow OTA HTTP (UI → firmware)

1. Client appelle `GET /check-update` → le firmware interroge l'API GitHub Releases pour comparer la version courante (`FIRMWARE_VERSION` dans [`version.h`](../../src/version.h)) à la dernière release.
2. Client appelle `POST /download-update` avec le type (firmware / filesystem).
3. Le firmware télécharge le `.bin` via HTTPS depuis `github.com` (certificat racine embarqué dans [`github_root_ca.h`](../../src/github_root_ca.h)).
4. Flash dans la partition inactive (Update API ESP32).
5. Reboot après `kRestartAfterOtaDelayMs = 3000` ms ([`constants.h:26`](../../src/constants.h:26)).

Pour l'upload manuel : `POST /update` (multipart `.bin`).

## Interaction avec `pump_controller`

**Sécurité critique** : dès qu'une OTA démarre, `PumpController.setOtaInProgress(true)` est appelé → `stopAll()` coupe les pompes doseuses. L'interruption d'une OTA firmware pendant une injection laisserait la pompe à un état indéterminé.

## Interaction avec `history`

L'historique est **préservé** pendant une OTA filesystem car il vit sur une partition distincte (`history` vs `spiffs`). Voir [ADR-0007](../adr/0007-table-partitions-custom.md).

## Yield pendant OTA

`kOtaYieldDelayMs = 1` ms ([`constants.h:11`](../../src/constants.h:11)) pour laisser respirer le watchdog ESP32 pendant le flash.

## Cas limites

- **Interruption WiFi pendant download** : Update API rejette le flash (checksum incohérent) → pas de corruption, partition inactive écartée, reboot sur partition active intacte.
- **Partition pleine** : refus au moment du `Update.begin()` → erreur HTTP 500, log CRITICAL.
- **Version GitHub < locale** : pas de mise à jour proposée côté UI (comparaison semver).

## Fichiers liés

- [`src/ota_manager.h`](../../src/ota_manager.h), [`src/ota_manager.cpp`](../../src/ota_manager.cpp)
- [`src/github_root_ca.h`](../../src/github_root_ca.h) — certificat racine ISRG Root X1
- [`partitions.csv`](../../partitions.csv)
- [docs/UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- [ADR-0007](../adr/0007-table-partitions-custom.md)
