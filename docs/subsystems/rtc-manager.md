# Subsystem — `rtc_manager`

- **Fichiers** : [`src/rtc_manager.h`](../../src/rtc_manager.h), [`src/rtc_manager.cpp`](../../src/rtc_manager.cpp)
- **Singleton** : `extern RtcManager rtcManager;`
- **Matériel** : DS3231 (module I²C, pile CR2032 pour maintenir l'heure hors tension).

## Rôle

Fournit une base de temps fiable au système. Quatre sources coexistent avec une **hiérarchie de confiance** :

1. **NTP (via WiFi)** — priorité haute, plus précis. Synchronisation au démarrage et périodiquement par l'OS ESP32.
2. **RTC DS3231** — priorité moyenne, conserve l'heure hors tension grâce à la pile bouton.
3. **NVS + uptime** — estimation à partir du dernier timestamp connu + durée depuis boot.
4. **Uptime seul** (epoch = 0, 1970) — dernier recours, rejeté par `kMinValidEpoch = 1700000000` ([`constants.h:111`](../../src/constants.h:111) — 14 nov. 2023).

## API publique

```cpp
bool begin();                         // détecte DS3231 sur I²C
bool isAvailable() const;
bool hasLostPower() const;            // true si pile DS3231 vide
DateTime now();                       // heure RTC, ou DateTime invalide
bool isTimeValid();                   // année >= 2021
bool syncFromSystem();                // met le RTC à l'heure système (après NTP)
bool setTime(const DateTime& dt);
bool setTimeFromEpoch(time_t epoch);
bool applyToSystem();                 // applique l'heure RTC au système ESP32 au boot
time_t getEpoch();                    // 0 si non disponible
```

## Flow de démarrage

```
setup()
 └─ Wire.begin()
 └─ rtcManager.begin()
    └─ si RTC détecté :
       └─ if (!rtc.lostPower() && isTimeValid())
          └─ applyToSystem()   // ESP32 a l'heure dès le démarrage
 └─ WiFi.begin() → event SYSTEM_EVENT_STA_GOT_IP
    └─ configTime()            // NTP
    └─ au prochain sync réussi : rtcManager.syncFromSystem()
```

## Synchronisation croisée

- **NTP → RTC** : après chaque sync NTP réussie, `syncFromSystem()` met à jour la DS3231 pour ne pas dériver.
- **RTC → ESP32** : `applyToSystem()` appelé au boot tant que NTP n'est pas encore disponible — évite d'attendre le WiFi pour avoir l'heure.
- **NVS + uptime** : utilisé seulement si RTC et NTP échouent. Fournit une estimation « best-effort ».

## Mutex I²C partagé

Le bus I²C est partagé avec ADS1115. Timeout d'acquisition `kI2cMutexTimeoutMs = 2000 ms` ([`constants.h:30`](../../src/constants.h:30)). Toute lecture RTC passe par ce mutex.

## Cas limites

- **Pile DS3231 vide** : `rtc.lostPower() == true` → `rtcLostPower = true`, l'heure retournée est ignorée, fallback NTP/NVS.
- **RTC absent** : `rtcAvailable = false`, le système s'appuie uniquement sur NTP + NVS.
- **WiFi sans NTP** (ex. réseau bloqué) : l'heure reste celle du RTC jusqu'à synchronisation ou reboot.

## Utilisation par d'autres composants

| Composant | Utilisation |
|-----------|-------------|
| `history` | Horodatage des data points — voir `_applyPreNtpCorrection()` pour re-dater les points pré-NTP |
| `pump_controller` | Détection minuit local (reset cumuls journaliers) |
| `filtration`, `lighting` | `getCurrentMinutesOfDay()` pour la programmation horaire |
| `logger` | Timestamp des entrées (ms depuis boot si heure non synchronisée) |

## Fichiers liés

- [`src/rtc_manager.h`](../../src/rtc_manager.h), [`src/rtc_manager.cpp`](../../src/rtc_manager.cpp)
- [`src/constants.h:111`](../../src/constants.h:111) — `kMinValidEpoch`
- Lib : [RTClib v2.1.4](https://github.com/adafruit/RTClib)
