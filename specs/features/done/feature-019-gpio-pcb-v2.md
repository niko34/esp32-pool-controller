## Métadonnées
- **ID** : feature-019
- **Slug** : gpio-pcb-v2
- **Type** : feature (adaptation hardware)
- **Statut** : done
- **Priorité** : haute (préliminaire à feature-021 EZO et feature-020 2e sonde DS18B20)
- **Version cible** : 2.0.0 (incrément MAJEUR : changement de hardware cible)
- **Dernière mise à jour** : 2026-05-05
- **Spec(s) liée(s)** : feature-021 (Atlas EZO pH/ORP — utilise les GPIOs I2C définis ici), feature-020 (2ᵉ sonde DS18B20 — utilise le pin OneWire défini ici)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — pinout ADC, OneWire, I2C
- [x] Régulation (pump_controller, PID) — pins relais pompes
- [x] Filtration / Éclairage — pins relais
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL — éventuellement si UART écran déplacé
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect — arbitre les choix de pins (conflits potentiels avec strapping pins, ADC, etc.)
- [ ] pool-chemistry — non applicable (pas de logique de dosage modifiée, juste des numéros de pin)
- [ ] ux-ui-designer — non applicable
- [x] embedded-developer
- [ ] web-ui-developer — non applicable
- [x] test-engineer — vérifier `pio run` SUCCESS
- [x] code-reviewer
- [x] doc-writer

## Description

Réassignation des broches GPIO pour le nouveau circuit imprimé **PCB v2**. Cette feature est purement matérielle : aucune logique métier ne change, seuls les numéros de pin sont mis à jour dans `src/constants.h` et éventuellement `src/main.cpp` (initialisations `pinMode`). C'est le **socle technique** sur lequel reposeront feature-021 (migration Atlas EZO pH/ORP) et feature-020 (ajout 2ᵉ sonde DS18B20 circuit).

## Contexte

Le **PCB v1** utilise une organisation GPIO conçue autour de :
- ADS1115 (I2C bus partagé avec écran LVGL éventuel)
- 1 sonde DS18B20 (OneWire eau piscine)
- 4 relais (filtration, éclairage, pompe pH, pompe ORP)
- UART écran LVGL (Serial2)
- DS3231 RTC (I2C)

Le **PCB v2** introduit :
- 2 modules **Atlas EZO** I2C (pH + ORP) — **remplacent** l'ADS1115 sur le bus I2C
- **2 sondes DS18B20** sur le bus OneWire (eau piscine + circuit/électronique)
- Réorganisation des pins relais et entrées numériques
- Suppression de l'ADS1115 et de ses pins associés (CS, ALERT/RDY si utilisés)

L'objectif de cette feature est uniquement **d'aligner les constantes de pin** dans le firmware sur la nouvelle assignation matérielle, **sans modifier la logique applicative**. Les features suivantes (020, 021) tireront parti de ce nouveau mapping.

## Comportement attendu

### Côté firmware

- Les constantes `kXxxPin` dans `src/constants.h` (et tout autre `#define`/`constexpr` de pin éparpillé dans le code) reflètent **exclusivement** le mapping PCB v2.
- Les `pinMode(...)` dans `src/main.cpp` `setup()` et dans les modules concernés (pompes, filtration, éclairage, sondes) utilisent ces constantes (pas de littéraux numériques en dur).
- Le firmware compile **sans warning** avec le nouveau mapping.
- Le firmware **ne sera pas testé fonctionnellement** sur PCB v1 après cette feature (incompatibilité hardware assumée — c'est une bascule unidirectionnelle).
- Aucun comportement utilisateur n'est modifié : mêmes pages UI, mêmes topics MQTT, mêmes APIs HTTP/WS.

### Cas limites

- **Pin de strapping ESP32** (GPIO0, 2, 5, 12, 15) : à éviter ou à utiliser avec précaution (état au boot peut interférer). Architect doit vérifier qu'aucune nouvelle assignation ne crée un conflit de boot.
- **Pins input-only** (GPIO34, 35, 36, 39) : OK pour des entrées numériques mais incompatibles avec OUTPUT/relais.
- **Bus I2C** : SDA + SCL doivent être cohérents avec ce qui est utilisé pour DS3231 + Atlas EZO (bus partagé). Si bus séparé prévu pour les EZO, ADR souhaitable.
- **OneWire** : le pin choisi doit pouvoir piloter 2 sondes DS18B20 en parasite ou en alimentation externe (à confirmer par le câblage du PCB v2).

## Comportement actuel

Mapping GPIO PCB v1 (à auditer par architect via lecture de `src/constants.h` et `src/main.cpp`) :
- DS18B20 OneWire : GPIO 5
- I2C SDA / SCL : pins par défaut Arduino-ESP32 (probablement GPIO21 / GPIO22)
- ADS1115 ALERT/RDY : éventuellement utilisé
- Relais filtration / éclairage / pompe pH / pompe ORP : à inventorier
- UART écran (Serial2) : kUart2RxPin = 16, kUart2TxPin = 17 (cf. mémoire projet)

## Contraintes techniques

- ESP32 (esp32dev), framework Arduino + PlatformIO
- Mapping doit éviter :
  - Pins de strapping en sortie active au boot (peut empêcher le boot)
  - Pins input-only (34-39) en sortie
  - Conflits avec USB JTAG (GPIO19 + GPIO20 sur certaines puces)
- `kConstantes` toujours dans `src/constants.h`, jamais de pin en dur ailleurs
- Documenter chaque pin avec un commentaire bref expliquant son rôle hardware (cf. style existant `// I2C SDA — DS3231 + EZO pH + EZO ORP`)
- Pas d'allocation dynamique, pas de modification d'API publique
- Watchdog non concerné

## Mapping GPIO cible PCB v2 (à compléter par l'utilisateur)

> **Section à remplir avec les numéros de pin exacts du PCB v2.** Pour chaque entrée, indiquer :
> - Le rôle hardware
> - Le numéro de pin GPIO ESP32
> - Le mode (INPUT, OUTPUT, INPUT_PULLUP, etc.)
> - Une note si le pin diffère du PCB v1

| Rôle | GPIO PCB v1 | GPIO PCB v2 | Mode | Notes |
|---|---|---|---|---|
| OneWire DS18B20 (eau + circuit) | 5 | 5 | INPUT_PULLUP | 2 sondes sur le même bus |
| I2C SDA (DS3231 + EZO pH + EZO ORP) | 21 (default) | 21 | I2C | bus partagé |
| I2C SCL (DS3231 + EZO pH + EZO ORP) | 22 (default) | 22 | I2C | bus partagé |
| Relais filtration | 25 | 26 | OUTPUT | actif haut |
| Relais éclairage | 26 | 27 | OUTPUT | actif haut |
| Pompe doseuse 1 — **pH** | 18 | 25 | OUTPUT (PWM/relais) | par défaut affectée à la pompe pH (`kPumpPhPin`) |
| Pompe doseuse 2 — **ORP** | 19 | 33 | OUTPUT (PWM/relais) | par défaut affectée à la pompe ORP/chlore (`kPumpOrpPin`) |
| Bouton factory reset | 32 | 35 | INPUT | **actif bas** — pull-up externe 10 kΩ vers 3V3 sur le PCB v2 (GPIO35 input-only sans pull-up interne). **Logique de lecture firmware inversée** par rapport au v1 (était actif haut avec INPUT_PULLDOWN interne) — comportement utilisateur identique : appui = reset |
| LED interne (status) | 2 | 2 | OUTPUT | inchangé v1/v2 — LED bleue intégrée à la plupart des dev kits ESP32, utilisée pour indicateur de statut |
| UART2 RX (futur écran LVGL — non actif en v2) | 16 | 16 | UART (réservé) | non utilisé actuellement, libre pour usage futur |
| UART2 TX (futur écran LVGL — non actif en v2) | 17 | 17 | UART (réservé) | non utilisé actuellement, libre pour usage futur |
| ADS1115 (supprimé en v2) | I2C | — | — | retiré (les EZO pH/ORP remplacent l'ADS1115 sur le bus I2C — feature-021) |
| ADS1115 ALERT/RDY (PCB v1) | non câblé | — | — | n'a jamais été relié à un GPIO ESP32 — aucune libération nécessaire |
| RTC_SQW : sortie carrée du DS3231 (entrée pour l'ESP32) | — | 23 | INPUT_PULLUP | DS3231 SQW est open-drain — pas de pull-up externe sur le PCB v2, le pull-up interne ESP32 est obligatoire |
| CTN_AUX : MOSFET pilote 12V tableau électrique (usage futur ouvert : pompe à chaleur, vanne, etc.) | — | 32 | OUTPUT | actif haut |
| CRTC_INT : pin 32K du module RTC (à priori permet de gérer une interruption) | — | 36 | INPUT | pull-up externe 10 kΩ vers 3V3 sur le PCB v2 (GPIO36 input-only sans pull-up interne) |

## Fichiers potentiellement concernés

- `src/constants.h` — toutes les constantes `kXxxPin` à mettre à jour
- `src/main.cpp` — `setup()` : éventuels `pinMode(...)` qui utilisent des littéraux à migrer vers constantes
- `src/sensors.cpp` / `sensors.h` — pin OneWire DS18B20 si défini localement (à factoriser dans constants.h)
- `src/uart_transport.h` ou `uart_transport.cpp` — `kUart2RxPin` / `kUart2TxPin` à mettre à jour si l'UART écran change
- `src/pump_controller.cpp` / `pump_controller.h` — pins relais pompes
- `src/filtration.cpp` / `lighting.cpp` — pins relais
- `partitions.csv` — non concerné (partitions inchangées)
- `platformio.ini` — non concerné
- `docs/subsystems/sensors.md`, `docs/subsystems/pump-controller.md`, `docs/subsystems/filtration.md`, `docs/subsystems/lighting.md` — mettre à jour le mapping documenté
- `docs/BUILD.md` — éventuelle note sur la cible PCB v2
- `CHANGELOG.md` — entrée Firmware sous la version cible 2.0.0
- `docs/adr/NNNN-pcb-v2-gpio-mapping.md` — à créer si ADR justifié

## Livrables — check-list

- [x] A1 — Mapping GPIO PCB v2 complet documenté dans la section ci-dessus de cette spec (utilisateur)
- [x] A2 — Mise à jour des `kXxxPin` dans `src/constants.h` selon le mapping
- [x] A3 — Audit + factorisation des pins en dur dans le code (`src/`) vers `constants.h`
- [x] A4 — Vérification qu'aucun pin de strapping n'est utilisé en OUTPUT actif au boot (architect)
- [x] A5 — Vérification qu'aucun pin input-only n'est utilisé en OUTPUT (architect)
- [x] A6 — Aucune référence subsistante aux pins ADS1115 (qui n'existent plus en v2)
- [x] D1 — `pio run` SUCCESS, RAM/Flash variations négligeables, 0 nouveau warning
- [ ] D2 — Vérification visuelle du mapping vs schéma PCB v2 (utilisateur, après code écrit) — délégué à l'humain (test sur PCB v2 réel)
- [x] C1 — `docs/subsystems/sensors.md` reflète le nouveau pinout
- [x] C2 — `docs/subsystems/pump-controller.md`, `filtration.md`, `lighting.md` idem
- [ ] C3 — `docs/BUILD.md` : note sur cible PCB v2 si pertinent (non requise — la doc Build n'est pas spécifique à la version PCB)
- [x] C4 — `CHANGELOG.md` : entrée Firmware/Hardware
- [x] C5 — ADR `docs/adr/0012-mapping-gpio-pcb-v2.md` créé (critères 1 + 4 remplis : décision structurante qui contraint feature-021/021 et verrouille l'incompatibilité PCB v1)

## Nouvelles signatures / API

- Endpoints HTTP : aucun nouveau
- Champs WebSocket : aucun
- Topics MQTT : aucun nouveau
- Fonctions publiques : aucune nouvelle
- Constantes : valeurs des `kXxxPin` modifiées (signatures inchangées)

## Sécurité chimique

- Dosage impliqué : non (pas de logique de dosage modifiée — juste les pins relais pompes)
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non
- Calibration capteur modifiée : non (la calibration EZO sera traitée en feature-021)
- Algorithme PID touché : non
- Risque identifié : si un pin pompe doseuse est mal mappé, on commande la mauvaise pompe → risque chimique. **Vérification visuelle obligatoire par l'utilisateur** avec voltmètre/oscilloscope avant tout dosage réel.

## ADR attendu ?

- **Probable** : oui (probablement)
- **Critère(s) candidat(s)** :
  - 1 (contrainte structurante : choix de pins verrouille le futur du PCB)
  - 4 (verrouillage long terme : tout futur module devra utiliser ce mapping ou être incompatible avec PCB v2)
- **Sujet pressenti** : `docs/adr/NNNN-pcb-v2-gpio-mapping.md` — justifier les choix de pins, surtout pour les pins critiques (I2C, OneWire) et expliquer la logique de répartition

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/subsystems/sensors.md` — pinout DS18B20, I2C
- `docs/subsystems/pump-controller.md` — pins relais pompes
- `docs/subsystems/filtration.md` — pin relais filtration
- `docs/subsystems/lighting.md` — pin relais éclairage

#### Docs transverses
- `docs/BUILD.md` — note sur cible PCB v2 (si différenciation avec v1 nécessaire)
- `CHANGELOG.md` — entrée sous la version cible 2.0.0

#### Docs immuables
- `docs/adr/NNNN-pcb-v2-gpio-mapping.md` — à créer (ADR probable)

## Critères d'acceptation

- [x] AC1 — Toutes les constantes `kXxxPin` correspondent au mapping PCB v2 documenté
- [x] AC2 — Aucun littéral de pin GPIO en dur dans `src/` (sauf valeurs de référence dans les commentaires)
- [x] AC3 — Aucun pin de strapping ESP32 utilisé en OUTPUT actif au boot
- [x] AC4 — Aucun pin input-only utilisé en OUTPUT
- [x] AC5 — Aucune référence subsistante à l'ADS1115 côté GPIO mapping (les libs ADS1115/DFRobot_PH restent utilisées par `sensors.cpp` jusqu'à la migration EZO en feature-021 — hors scope feature-019)
- [x] AC6 — `pio run` compile sans nouveau warning
- [x] AC7 — RAM/Flash variations négligeables (< +0.1 %)
- [x] AC8 — Tous les libellés utilisateur restent en français (non concerné, pas d'UI)
- [x] AC9 — Documentation des subsystems mise à jour avec le nouveau mapping
- [x] AC10 — ADR-0012 créé (critères 1 + 4 remplis)

## Plan de test manuel

> **Tests dynamiques limités** — le PCB v1 ne pourra plus être testé fonctionnellement après cette feature (mapping incompatible). Les tests fonctionnels seront effectués sur le PCB v2 réel.

1. **Test statique post-build** : `pio run` SUCCESS, lecture du mapping final dans `constants.h`, vérification que chaque entrée correspond au schéma PCB v2.
2. **Test au flash sur PCB v2 (utilisateur, quand le PCB sera branché)** :
   - Boot de l'ESP32 sans crash (pas de conflit pin de strapping)
   - Sondes DS18B20 détectées sur le nouveau pin OneWire (logs `Capteur de température DS18B20 initialisé sur GPIO X`)
   - Bus I2C scanné : DS3231 + Atlas EZO pH + Atlas EZO ORP détectés (à voir avec feature-021)
   - Activation manuelle des relais (filtration, éclairage) via UI : vérifier au voltmètre que le bon relais s'enclenche
   - **Test pompes doseuses** : ne pas mettre de produit dans les bidons lors du premier test ; commander une dose courte et vérifier au voltmètre la sortie GPIO de la pompe (sécurité chimique)

## Notes pour agent

- **Tu n'es pas responsable de fournir le mapping** : c'est à l'utilisateur de le saisir dans le tableau ci-dessus. L'agent embedded-developer applique le mapping fourni.
- **Stratégie d'implémentation** : 
  1. Lire `src/constants.h` et identifier toutes les constantes `kXxxPin` existantes
  2. `grep -r "GPIO\|pinMode\|digitalRead\|digitalWrite" src/` pour trouver les pins en dur restants
  3. Factoriser les pins en dur en `kXxxPin` dans `constants.h`
  4. Appliquer le nouveau mapping selon le tableau spec utilisateur
- **Pin strapping ESP32 à surveiller** :
  - GPIO0 : doit être HIGH au boot (boot mode select)
  - GPIO2 : doit être LOW ou flottant au boot (download)
  - GPIO5 : LOW au boot (timing SDIO)
  - GPIO12 : LOW au boot (3.3V flash voltage select)
  - GPIO15 : HIGH au boot (silence du log boot ROM)
- **Pin input-only ESP32** : 34, 35, 36 (VP), 39 (VN). Ne supportent PAS de pull-up interne ni de OUTPUT.
- **Bus I2C par défaut Arduino-ESP32** : SDA=21, SCL=22 (à confirmer pour PCB v2 — peut être déplacé)
- Aucune dépendance pool-chemistry pour cette feature (pas de logique de dosage modifiée, mais le code-reviewer vérifiera que les pins de pompe ne créent pas de risque accidentel)

## Hors périmètre

- Pas de migration ADS1115 → Atlas EZO (c'est feature-021)
- Pas d'ajout de la 2ᵉ sonde DS18B20 (c'est feature-020)
- Pas de modification de la logique de calibration, dosage, régulation
- Pas de modification UI ni de protocole MQTT/WS
- Pas de support multi-PCB simultané (la branche `pcb-v2` cible le v2 ; `main` reste compatible v1)
- Pas de migration des fichiers KiCad (le hardware est figé côté schéma, juste le firmware s'adapte)

## Travail réalisé (2026-05-05)

**Statut** : livré — tests verts, revue OK, branche `pcb-v2`.

### Fichiers code touchés (7)

- `src/constants.h` : nouvelle section « GPIO PIN ASSIGNMENTS - PCB v2 » avec 10 constantes actives (`kBuiltinLedPin=2`, `kTempSensorPin=5`, `kI2cSdaPin=21`, `kI2cSclPin=22`, `kPumpPhPin=25`, `kFiltrationRelayPin=26`, `kLightingRelayPin=27`, `kPumpOrpPin=33`, `kFactoryResetButtonPin=35`) + 3 réservées (`kRtcSqwPin=23`, `kCtnAuxPin=32`, `kRtcIntPin=36`)
- `src/config.h` : suppression des 7 `#define` migrés (`PUMP1_PWM_PIN`, `PUMP2_PWM_PIN`, `TEMP_SENSOR_PIN`, `FILTRATION_RELAY_PIN`, `LIGHTING_RELAY_PIN`, `BUILTIN_LED_PIN`, `FACTORY_RESET_BUTTON_PIN`)
- `src/pump_controller.cpp` : `pumps[0] = {kPumpPhPin, PUMP1_CHANNEL}` et `pumps[1] = {kPumpOrpPin, PUMP2_CHANNEL}` dans `begin()`
- `src/filtration.cpp` : références `kFiltrationRelayPin` (4 occurrences `pinMode` / `digitalWrite`)
- `src/lighting.cpp` : références `kLightingRelayPin` (4 occurrences)
- `src/sensors.cpp` : `oneWire(kTempSensorPin)` constructeur + 3 logs cités
- `src/main.cpp` : `pinMode(kFactoryResetButtonPin, INPUT)` (au lieu de `INPUT_PULLDOWN`), logique inversée `pressed = digitalRead(...) == LOW`, `pinMode(kBuiltinLedPin, OUTPUT)` + 5 `digitalWrite(kBuiltinLedPin, ...)`

### Documentation

- `docs/adr/0012-mapping-gpio-pcb-v2.md` créé (Statut Accepté, 2026-05-05) — décision complète + alternatives écartées + conséquences
- `docs/adr/README.md` mis à jour (entrées 0011 + 0012)
- `docs/subsystems/pump-controller.md` : nouvelle section « Mapping pompes ↔ pins (PCB v2) »
- `docs/subsystems/sensors.md` : table Matériel mise à jour, note 2ᵉ DS18B20 et migration EZO
- `docs/subsystems/filtration.md` : `kFiltrationRelayPin = 26`
- `docs/subsystems/lighting.md` : `kLightingRelayPin = 27`
- `CHANGELOG.md` : nouvelle section `[Unreleased] - 2026-05-05` avec sous-sections BREAKING/Hardware, Firmware, Documentation

### Build

- `pio run` : SUCCESS
- RAM 16.4 % (inchangé)
- Flash 97.8 % (inchangé)
- 0 nouveau warning

### Tests dynamiques restants (délégués à l'humain sur PCB v2 réel)

1. Boot ESP32 sans crash (vérifier qu'aucun pin de strapping en mauvais état au boot)
2. Voltmètre sur relais filtration (GPIO 26) et éclairage (GPIO 27) lors de l'activation manuelle UI — vérifier que le bon relais s'enclenche
3. Voltmètre sur sortie pompe pH (GPIO 25) et pompe ORP (GPIO 33) lors d'un test manuel court (sans produit dans les bidons — sécurité chimique)
4. Bouton factory reset (GPIO 35) : appui simple ne déclenche rien, maintien 10 s déclenche le reset (logique active bas)
5. Sonde DS18B20 eau détectée sur GPIO 5 (logs `Capteur de température DS18B20 initialisé sur GPIO 5`)

### Note de version

Cette feature pose la base de la **version 2.0.0** (incrément MAJEUR). Les versions ≥ 2.0.0 ne supportent plus le PCB v1.
