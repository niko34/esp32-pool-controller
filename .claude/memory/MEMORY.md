# Mémoire du projet esp32-pool-controller

## Préférences utilisateur

- **Ne jamais créer de release GitHub sans demande explicite.** Sur demande explicite (« publie une release »), Claude PEUT lancer `gh release create` : la règle est en `ask` depuis le 2026-07-04 (avant : `deny`) → l'utilisateur confirme via le prompt. Préparer les notes dans un fichier scratchpad + `--notes-file`.
- **Ne jamais faire de commit ou push sans demande explicite.** Modifier les fichiers uniquement, attendre que l'utilisateur demande le commit.

## Workflow de build

- Après modification de `data/app.js` : lancer `./build_fs.sh` (minifie + génère `littlefs.bin`)
- Après modification de `src/` : lancer `pio run` (génère `firmware.bin`)
- Fichier de version : `src/version.h` — incrémenter le CORRECTIF pour les fixes

## Poste de travail

- [Conteneur LXC Debian 13](env-lxc-esp32-pool-controller.md) — depuis le 2026-09-05 le développement se fait dans un LXC (`/root/esp32-pool-controller`), plus sur le Mac : **aucun port série** (OTA seul, et pas de mDNS → IP explicite), Chromium exige un wrapper `--no-sandbox` pour les PDF, `pio pkg` réécrit `platformio.ini` en supprimant ses commentaires, chaîne PCB et `gh` en place

## esp32-pool-controller-screen — Build / Plateforme

- **Plateforme** : `pioarduino` (Arduino 3.x, IDF 5.x) — OBLIGATOIRE pour ESP32_Display_Panel
  - URL: `https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip`
  - Lancer avec : `/Users/nicolas/.platformio/penv/bin/pio run` (pas le pio système Python 3.12)
  - Si `ModuleNotFoundError: fatfs/littlefs` → utiliser le pio du penv
- **ESP32_Display_Panel v0.2.1** + **ESP32_IO_Expander v0.1.0** (dépendance requise)
- **Board custom** : `include/ESP_Panel_Board_Custom.h` (v0.2.1 API)
  - VIEWE UEDX80480043E-WB-A : ST7262, RGB 16-bit, 15MHz, HSYNC=39, VSYNC=41, DE=40, PCLK=42
  - Data: 8,3,46,9,1,5,6,7,15,16,4,45,48,47,21,14 — Touch GT911 I2C SCL=20 SDA=19 — BL=GPIO2
  - `ESP_PANEL_LCD_BUS_SKIP_INIT_HOST=1` (RGB pur, pas de 3-wire SPI)
- API v0.2.1 dans display_hal.cpp : `ESP_Panel*`, `ESP_PanelLcd*`, `ESP_PanelTouch*`, `getLcd()` (pas `getLCD()`)
- `include/esp_panel_board_supported_conf.h` inutilisé (mauvais nom — library cherche `ESP_Panel_Board_Supported.h`)

## Wizard écran (esp32-pool-controller-screen)

9 étapes : Welcome → Controller Status → Security → WiFi Scan → WiFi Password → Time Config → Basic Settings → Setpoints → Summary.
- Nouvelles commandes UART contrôleur : `wifi_scan`, `wifi_connect`, `change_password` + champs NTP dans `set_config`
- Clavier LVGL en superposition (y=280, h=200), masque footer pendant saisie
- `WizardStep` défini uniquement dans `screen_wizard.h` (supprimé de `app_state.h`)
- `WifiNetwork` struct dans `app_state.h` ; `wifi_networks` dans AppState
- `PoolConfig` contient maintenant `time_use_ntp`, `ntp_server`, `timezone_id`
- `wifiScan()` dans ControllerService utilise timeout 15 s (scan bloquant côté contrôleur)

## Écran (display_hal.cpp) — corrections appliquées

- **Troncature droite résolue** : double-buffering avec page-flip (2 framebuffers LCD via `rgb_bus->configRgbFrameBufferNumber(2)` entre init() et begin()). LVGL utilise les framebuffers du LCD directement (`getFrameBufferByIndex(0/1)`), flush_cb attend la fin de frame via `ulTaskNotifyTake` + callback ISR `onLcdFrameFinished`.
- **Conflit GPIO16** : GPIO16 est utilisé par le bus LCD RGB (G4 green bit DATA9) ET était le UART_CTRL_RX. Corrigé : UART_CTRL_RX=17, UART_CTRL_TX=18 (libres sur la carte VIEWE).
- **Câblage UART screen↔controller** : Contrôleur TX(GPIO17) → Écran RX(GPIO17) ; Écran TX(GPIO18) → Contrôleur RX(GPIO16). Contrôleur uart_transport.h inchangé (kUart2RxPin=16, kUart2TxPin=17).
- **Délai démarrage** : 3000ms dans main.cpp pour voir les logs d'init display dans le moniteur série.
- **GPIOs libres VIEWE UEDX80480043E-WB-A** : 17, 18, 33-38 (LCD occupe : 1,3,4,5,6,7,8,9,14,15,16,21,39,40,41,42,45,46,47,48 ; Touch I2C : 19,20 ; Backlight : 2)

## Interface UART écran — RETIRÉE (feature-065, v2.17.0, 2026-08-08)

Le support écran LVGL côté CONTRÔLEUR a été entièrement supprimé (jamais mis en
production — décision utilisateur) : fichiers `uart_*` et `tools/uart_test.py`
effacés, intégrations purgées (main, pump_controller, filtration, sensors,
calibration). **GPIO 16/17 libérés** (prérequis WROVER-E/PSRAM). Bonus sécurité :
le bypass `pump_test` UART sans `evaluateManualInject` est fermé. Gains flash :
core 2 −35 Ko (80,7 %), core 3 −30 Ko (90,9 %). Le champ/toggle `screen_enabled`
(NVS + Settings) est conservé mais INERTE — spec de suivi si retrait souhaité.
Le projet séparé esp32-pool-controller-screen reste intact (sections ci-dessous
le concernant = autre repo).

## Matériel pH/ORP (PCB v2)

- [Migration EZO pH et ORP](project_ezo_migration.md) — modules Atlas Scientific EZO I2C, plus d'ADS1115

## Bassin utilisateur

- [Paramètres du bassin](parametres-bassin-utilisateur.md) — 30-50 m³, chlore 36°, consigne ORP 530 mV calée sur BANDELETTE (31/08) ; limites restaurées aux défauts (pH 10 min/h) ; épisode surconsommation 07/2026 = stabilisant 15 mg/L

## Chantiers futurs

- [Audit sécurité différé — attendre Fable 5.1](audit-securite-differe-fable-5-1.md) — décision du 02/09 ; périmètre cadré (agent security-reviewer + audit complet, HTTP clair sur LAN en tête) ; relancer quand l'utilisateur a accès au modèle

- [Plan migration core 3.x/IDF 5](plan-migration-core3.md) — pioarduino retenu, spec d'étude feature-062 en todo (chantier hiver) ; jamais sans pool-chemistry (LEDC pompes)
- [Kanban des specs — idée en attente](kanban-specs-idee-en-attente.md) — décision 03/09 « rien pour le moment » ; si relance : champ Statut enrichi + tableau HTML généré, pas d'outil externe
- [Cible matérielle : ESP32-DevKitC-VE (PSRAM obligatoire)](cible-materielle-esp32-s3-bom.md) — BOM PCB v2 U2 ; PSRAM exigée pour la mise à jour en ligne GitHub ; S3 écarté ; coquille de lien dans le BOM à corriger

## Incidents
- ⭐ [Matin 05/09 CONTAMINÉ : air dans la filtration (niveau bas)](matin-0509-air-dans-le-circuit.md) — condition NOUVELLE (pas d'air le 04/09 après-midi) ; ne pas agréger aux matins 02-04/09 ; l'écart +0,11 vs appareil jaune ne dit rien sur l'offset
- [Bypass sondes fermé pendant le 1er dosage réel (23/08/2026)](incident-bypass-sondes-ferme.md) — mesures non représentatives, 765/802 ml comptés ; la garde présence d'eau ne voit pas le bypass ; poser P3 AVANT toute mise en service

- [Corruption partition history (07/2026)](incident-corruption-partition-history.md) — boot-loop lfs_alloc div/0 ; disjoncteur auto-réparation + fix refreshTopics UAF livrés en v2.10.0 (tests on-target restants, tout non commité)
- [Décodage coredump + incident OOM async_tcp (11/08/2026)](decodage-coredump-et-incident-oom-async-tcp.md) — commande esp-coredump validée (gdb toolchain-xtensa-esp32) ; crash bad_alloc à l'accept HTTP en prod core 3, résolu par la garde TLS PSRAM (v2.18.6/2.18.7)
- ⭐ [Le parachute anti-OOM était en PSRAM depuis feature-066 — donc décoratif](parachute-oom-etait-en-psram.md) — prouvé le 05/09 par +12 324 o de psram_free au déploiement v2.35.0 ; corrigé (lot B-1), invariant désormais observable
- ⭐ [CAUSE RACINE : la métrique heap du projet est fausse (15/08/2026)](cause-racine-oom-metrique-heap-fausse.md) — ESP.getFreeHeap/getMaxAllocHeap mesurent MALLOC_CAP_INTERNAL (IRAM 32 bits comprise) ; famine 8 bits réelle, toutes les gardes heap sont aveugles, seuils à recalibrer
- [2e crash OOM sur fichier statique (13/08/2026)](incident-oom-2-fichier-statique-13-08.md) — échec d'un new de 156 o AVEC 81 Ko libres (corruption ou angle mort de 2 s) ; **le parachute ne se réarme jamais** (seuil 32 Ko > maxBloc réel 28-30 Ko) — correctif + post-mortem RTC à faire

## Pièges récurrents

- ⭐ [L'offset pH n'est PAS une suspicion de sonde](offset-ph-nature-et-non-suspicion.md) — il corrige un décalage de MESURE (courants de fuite) ; sonde prouvée saine (zéro −1,03 mV) ; principe rappelé 3 fois par l'utilisateur

- ⭐ [Bruit de mesure pH : fuites de la pompe de FILTRATION](mesure-ph-perturbee-en-phase-pompe.md) — dispersion ~0,36 permanente (était 0,6-0,7), brut 6,75-7,15 ; régulation = filtrée (vérifié 03/09), historique = brut+offset ; hypothèses « phase pompe doseuse » et « panache d'injection » RETIRÉES (sondes EN AMONT de l'injection) ; filtrée fluctue ±0,08 à 10 min > seuil 0,05

- ⭐ [Parité `/data` ↔ WebSocket](parite-data-websocket-recidive.md) — 3 récidives en 3 versions ; un champ WS-seul ne disparaît pas, il CLIGNOTE (`latestSensorData` remplacé par les charges `/data`) ; prouver la parité par grep des deux côtés

- ⭐ [Config d'usine ≠ config vivante](piege-config-usine-vs-config-vivante.md) — le diagnostic « limité par l'actionneur » (ADR-0053, feature-086) reposait sur la limite d'usine 5 min/h alors que l'appareil était à 25 ; toujours lire `/get-config` avant tout diagnostic chiffré

## Architecture clé

- SPA avec toutes les vues dans le DOM (CSS `is-active`)
- `latestSensorData` : données de `/data` (30s refresh)
- `window._config` : config de `/get-config`
- `filtrationRunningOverride` : override optimiste pour le statut filtration
- `updateFiltrationBadges()` : met à jour `#detail-filtration-status` ET `#filtration-current-status` simultanément
- `saveFiltration()` : autosave pour les réglages filtration (séparé de `save()`)
- [Permissions & style de commandes](permissions-et-style-commandes.md) — allowlist déjà complète ; éviter $() /heredocs dans Bash (git commit -F fichier) ; règles ask/deny = garde-fous voulus
