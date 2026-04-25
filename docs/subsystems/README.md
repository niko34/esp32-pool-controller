# docs/subsystems — Composants firmware

Documentation vivante, **toujours à jour**, d'un composant firmware à la fois. Chaque fiche couvre :
- rôle du composant,
- API publique (méthodes du singleton),
- algorithme / machine à états,
- interactions avec les autres composants,
- endpoints HTTP ou topics MQTT exposés,
- cas limites.

## Différence avec `specs/features/` et `docs/features/`

- **`specs/features/`** — spécifications **éphémères** (todo → doing → done), décrivent ce qui a été demandé, débattu, implémenté pour un jalon donné. Une fois en `done/`, servent d'archive historique.
- **`docs/features/`** — documentation **vivante** d'une page / surface UI. Décrit le produit tel qu'il est actuellement.
- **`docs/subsystems/`** (ce dossier) — documentation **vivante** d'un composant **firmware**. Un composant = un fichier, indépendant de la UI.
- **`docs/adr/`** — décisions architecturales **immuables**, documentent les choix structurants du projet.

Quand un composant change, la fiche correspondante dans `subsystems/` doit être mise à jour **au même commit**.

## Index

### Régulation & actuateurs

- [pump-controller.md](pump-controller.md) — régulation PID pH/ORP, anti-cycling, cumuls journaliers, stabilisation (**composant critique** — passe par `pool-chemistry`)
- [filtration.md](filtration.md) — pilote relais filtration (GPIO25), modes auto/manual/off, calcul horaire selon température
- [lighting.md](lighting.md) — pilote relais éclairage (GPIO26), programmation horaire

### Capteurs & temps

- [sensors.md](sensors.md) — ADS1115 (pH/ORP) + DS18B20 (température), calibration
- [rtc-manager.md](rtc-manager.md) — DS3231 + hiérarchie NTP / RTC / NVS+uptime

### Communication

- [ws-manager.md](ws-manager.md) — WebSocket temps réel, push 5 s + événements
- [mqtt-manager.md](mqtt-manager.md) — client MQTT + auto-discovery Home Assistant

### Infrastructure

- [web-server.md](web-server.md) — AsyncWebServer, routing, CORS, reboot différé
- [auth.md](auth.md) — auth admin + API token + rate limiting
- [history.md](history.md) — historique 3 granularités, partition dédiée, import/export CSV
- [ota-manager.md](ota-manager.md) — mise à jour firmware + filesystem, partitions double-bank
- [logger.md](logger.md) — buffer circulaire RAM + persistance LittleFS + push WS

## Conventions

- Tous les singletons globaux sont déclarés via `extern` dans le header et définis dans le `.cpp` correspondant.
- Les mutexes FreeRTOS protègent les structures partagées entre tâches et handlers web asynchrones.
- Les timeouts globaux vivent dans [`constants.h`](../../src/constants.h) (timeouts mutex, intervalles, rate limits).
- Les limites de sécurité chimique vivent dans [`config.h`](../../src/config.h) structs `SafetyLimits`, `PumpProtection`.

## Composants non documentés ici

Intentionnellement hors périmètre, soit parce qu'ils sont internes / triviaux, soit déjà couverts ailleurs :
- `web_routes_*.cpp` — implémentations par domaine, vue d'ensemble dans [web-server.md](web-server.md) et matrice dans [docs/API.md](../API.md).
- `config.h / config.cpp` — simples structs POD + save/load, consommé par tous les composants ci-dessus.
- `version.h` — constante `FIRMWARE_VERSION`.
- `constants.h` — constantes globales.
- `json_compat.h` — aliasing ArduinoJson 7.
- `github_root_ca.h` — certificat racine embarqué.
