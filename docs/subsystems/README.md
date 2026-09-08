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

- [regulation-vue-d-ensemble.md](regulation-vue-d-ensemble.md) — **commencer ici pour comprendre la régulation** : la chaîne pH et ORP de bout en bout, la division feedforward/PI, l'asymétrie pH ↔ chlore, les constantes de temps et les invariants — raconte et renvoie, ne duplique pas
- [pump-controller.md](pump-controller.md) — régulation PID pH/ORP, anti-cycling, cumuls journaliers, stabilisation (**composant critique** — passe par `pool-chemistry`)
- [time-to-target.md](time-to-target.md) — indicateur « temps d'atteinte de cible » (pH / ORP) : **instrument de mesure passif**, sens unique — il lit la régulation, la régulation ne le lit jamais ([ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md))
- [filtration.md](filtration.md) — pilote relais filtration (GPIO25), modes auto/manual/off, calcul horaire selon température
- [lighting.md](lighting.md) — pilote relais éclairage (GPIO26), programmation horaire

### Capteurs & temps

- [sensors.md](sensors.md) — ADS1115 (pH/ORP) + DS18B20 (température), calibration
- [measure-validity.md](measure-validity.md) — validité des mesures pH/ORP (eau qui circule + purge post-reprise), verdict caché consommé par l'historique et l'UI
- [rtc-manager.md](rtc-manager.md) — DS3231 + hiérarchie NTP / RTC / NVS+uptime

### Communication

- [ws-manager.md](ws-manager.md) — WebSocket temps réel, push 5 s + événements
- [mqtt-manager.md](mqtt-manager.md) — client MQTT + auto-discovery Home Assistant

### Infrastructure

- [web-server.md](web-server.md) — AsyncWebServer, routing, en-têtes de sécurité, reboot différé
- [auth.md](auth.md) — auth admin + API token + rate limiting
- [history.md](history.md) — historique 3 granularités, partition dédiée, import/export CSV
- [ota-manager.md](ota-manager.md) — mise à jour firmware + filesystem, partitions double-bank
- [logger.md](logger.md) — buffer circulaire RAM + persistance LittleFS + push WS + **reprise du `.tmp` de rotation au démarrage** ([ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md))
- [system-health.md](system-health.md) — santé système, instrumentation heap, auto-guérison mémoire (reboot propre) + mode maintenance anti-boucle
- [oom-guard.md](oom-guard.md) — parachute OOM global : réserve d'urgence + `std::set_new_handler` + garde TLS sortante à motifs (PSRAM libre + heap, v2.18.6, ADR-0035/ADR-0036)
- [acces-fichiers.md](acces-fichiers.md) — **recensement des `fopen` du firmware** : les 14 sites d'ouverture de fichier pour le compte propre du firmware (tâche, cadence, reportable ou non), leur classe de contention, le périmètre exact de la garde 503 et les accès hors classe. ⚠️ `exists()` **ouvre le fichier** — aucun `grep` ne trouve ce site ([ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md))

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
