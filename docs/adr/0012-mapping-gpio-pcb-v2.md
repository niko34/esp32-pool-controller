# ADR-0012 — Mapping GPIO PCB v2

- **Statut** : Accepté
- **Date** : 2026-05-05
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : [`feature-019-gpio-pcb-v2`](../../specs/features/done/feature-019-gpio-pcb-v2.md), feature-020 (Atlas EZO pH/ORP — à venir), feature-021 (2ᵉ sonde DS18B20 — à venir)

## Contexte

### Motivation hardware — pourquoi un PCB v2

Le **problème de fond** que résout le PCB v2 est la **fiabilité des lectures pH et ORP**. Sur le PCB v1 :

- Les sondes pH et ORP étaient lues en **analogique** via l'ADS1115, **sans isolation galvanique** entre le signal analogique et l'alimentation des modules de mesure.
- En **fonctionnement filtration**, la pompe principale 230V générait des **courants de fuite** qui se propageaient jusqu'aux modules pH/ORP via la masse commune et l'absence d'isolation.
- Conséquence directe : les **valeurs pH et ORP étaient perturbées dès que la filtration tournait** (la mesure utile coïncide pourtant avec les périodes de filtration). En pratique cela rendait la régulation chimique automatique imprécise et obligeait à des compromis (lecture en filtration arrêtée, fenêtres de stabilisation, seuils élargis).

Le PCB v2 corrige ce défaut par **trois leviers cumulés** :

1. **Modules Atlas Scientific EZO pH + EZO ORP** au lieu de l'ADS1115. Les EZO encapsulent le conditionnement de signal, la compensation de température et la calibration multi-points, et exposent une **interface I²C numérique** insensible aux perturbations analogiques sur le câble de signal.
2. **Alimentation isolée des modules de mesure** sur le PCB. L'alimentation des EZO ne partage plus la masse de la pompe / des relais.
3. **Isolation galvanique du bus I²C** entre l'ESP32 et les EZO. Aucun courant de fuite ne peut remonter par les lignes SDA/SCL.

L'objectif explicite du PCB v2 est d'obtenir des **lectures pH et ORP fiables et stables même filtration en marche**, condition nécessaire pour que la régulation chimique automatique soit utile en pratique.

### État technique avant migration

Le **PCB v1** était bâti autour d'un ADS1115 (canaux pH/ORP analogiques), d'une seule sonde DS18B20 (eau piscine) et de quatre relais (filtration, éclairage, pompe pH, pompe ORP). Le bus I²C n'hébergeait que le DS3231 RTC + l'ADS1115. Les pins relais étaient répartis sur GPIO18/19/25/26.

Le **PCB v2** introduit trois changements matériels structurants :

1. **Modules Atlas Scientific EZO pH + EZO ORP** (I²C) qui **remplacent** l'ADS1115. Les EZO partagent le bus I²C avec le DS3231. La régulation côté firmware passera à la lecture I²C en feature-020.
2. **Deuxième sonde DS18B20** (température circuit/électronique) ajoutée sur le même bus OneWire que la sonde eau. La 2ᵉ sonde sera adressée en feature-021.
3. **Nouveau MOSFET 12V `CTN_AUX`** piloté par l'ESP32, à disposition pour un usage futur (pompe à chaleur, vanne motorisée, etc.). Doit être OUTPUT actif haut, sans fonction firmware aujourd'hui.

L'ADS1115 et ses pins associés sont supprimés. Le PCB v2 expose aussi désormais deux signaux du DS3231 vers l'ESP32 : `RTC_SQW` (sortie carrée open-drain) et `RTC_INT` (broche 32K, interruption disponible). Ces deux pins sont câblés mais resteront en haute impédance (`pinMode` non appelé) tant qu'aucune feature ne les utilise.

Cette feature doit aligner les constantes de pin du firmware (`kXxxPin` dans `src/constants.h`) sur le nouveau câblage. Le firmware ne supportera plus le PCB v1 après cette bascule — c'est un choix unidirectionnel assumé : le coût de maintenir les deux mappings en parallèle (compile flags, tests croisés) dépasse le bénéfice (un seul exemplaire PCB v1 en service personnel, déjà migré).

## Décision

Le mapping GPIO PCB v2 est figé comme suit. Les 10 premiers pins sont **actifs** dès la v2.0.0 ; les 3 derniers sont **réservés** (déclarés en constantes mais sans `pinMode` initial — `pinMode` non appelé = haute impédance par défaut, économie de courant pull-up sur les pins concernés).

| GPIO | Constante (`src/constants.h`) | Rôle | Mode | Notes |
|------|-------------------------------|------|------|-------|
| 2 | `kBuiltinLedPin` | LED bleue interne (status) | OUTPUT | Inchangé v1/v2 — strapping pin (download mode) mais OUTPUT activé après boot, sans incidence |
| 5 | `kTempSensorPin` | OneWire DS18B20 (eau + circuit, 2 sondes) | INPUT_PULLUP via DallasTemperature | Strapping pin (timing SDIO), tolérée car le pull-up OneWire (4.7 kΩ externe) tire HIGH après boot |
| 21 | `kI2cSdaPin` | I²C SDA — DS3231 + EZO pH + EZO ORP | I²C | Default Arduino-ESP32, aucune raison de déplacer |
| 22 | `kI2cSclPin` | I²C SCL — DS3231 + EZO pH + EZO ORP | I²C | Default Arduino-ESP32, idem |
| 23 | `kRtcSqwPin` | DS3231 SQW (sortie carrée, vers ESP32) | **réservé** — `pinMode` non appelé | DS3231 SQW est open-drain — `INPUT_PULLUP` interne attendu côté ESP32 le jour où la feature utilisera ce signal |
| 25 | `kPumpPhPin` | Pompe doseuse **pH** (PWM via `ledc`) | OUTPUT | `pumps[0]` dans `pump_controller.cpp` |
| 26 | `kFiltrationRelayPin` | Relais filtration | OUTPUT | Actif haut |
| 27 | `kLightingRelayPin` | Relais éclairage | OUTPUT | Actif haut |
| 32 | `kCtnAuxPin` | MOSFET `CTN_AUX` (12V tableau électrique) | **réservé** — `pinMode` non appelé | OUTPUT actif haut attendu le jour où une feature pilote ce MOSFET |
| 33 | `kPumpOrpPin` | Pompe doseuse **ORP/chlore** (PWM via `ledc`) | OUTPUT | `pumps[1]` dans `pump_controller.cpp` |
| 35 | `kFactoryResetButtonPin` | Bouton factory reset | INPUT | Input-only, pull-up externe 10 kΩ vers 3V3 sur le PCB v2 — **actif bas** (logique inversée vs v1) |
| 36 | `kRtcIntPin` | DS3231 broche 32K (futur INT) | **réservé** — `pinMode` non appelé | Input-only, pull-up externe 10 kΩ vers 3V3 sur le PCB v2 |

Justifications clés des choix non triviaux :

- **GPIO 21/22 = I²C** : default Arduino-ESP32 (`Wire.begin()` sans argument). Aucune raison de déplacer le bus, et toute la chaîne d'initialisation des libs (DS3231, futur Atlas EZO) repose sur ce default — déplacer obligerait à passer `Wire.begin(SDA, SCL)` explicite partout.
- **GPIO 5 = OneWire conservé** : strapping pin (LOW au boot, timing SDIO). Toléré parce que le pull-up externe 4.7 kΩ tire HIGH **avant** le boot (état requis pour les capteurs DS18B20). Validé dès le PCB v1, aucun crash boot reproductible.
- **GPIO 35 = bouton reset** : choix d'un input-only (libère GPIO 32 pour `CTN_AUX`). Conséquence : pull-up **externe** obligatoire (les input-only n'ont pas de pull-up interne sur ESP32) et **logique de lecture inversée** côté firmware (`pressed = digitalRead(...) == LOW`). Comportement utilisateur identique : appui = factory reset après 10 s.
- **GPIO 25/33 = pompes pH/ORP** : ni l'un ni l'autre n'est un strapping pin, tous deux supportent OUTPUT/PWM. GPIO 33 a aussi été choisi parce qu'il évite tout conflit avec le bus I²C (21/22) et avec les relais filtration/éclairage (26/27) — les pompes restent isolées des relais 230V même côté firmware.
- **GPIO 23/32/36 réservés sans `pinMode`** : `pinMode` non appelé = haute impédance. Pas de courant statique consommé par un pull-up interne tant que la fonctionnalité n'est pas activée. La feature qui activera l'un de ces pins devra explicitement déclarer son mode.
- **Logique inversée du bouton** : `INPUT_PULLDOWN` interne (v1) → `INPUT` + pull-up externe (v2). Le firmware lit désormais `LOW` = pressé. Le test 10 s de maintien (`kFactoryResetButtonHoldMs`) reste inchangé.

## Alternatives considérées

- **Conserver le mapping v1** (rejetée) — impossible par construction : l'ADS1115 est physiquement supprimé du PCB v2, les EZO sont sur I²C partagé avec le DS3231 (le bus partagé est obligatoire — voir option suivante), et le PCB v2 exige un pin dédié pour `CTN_AUX`. Aucun chemin de coexistence.
- **Bouton factory reset sur un pin avec pull-up interne** (ex. GPIO 13) **au prix de garder GPIO 32 occupé** par autre chose (rejetée) — GPIO 32 est utilisé par `CTN_AUX` côté schéma PCB v2 (MOSFET 12V tableau), et la feature future `CTN_AUX` est jugée plus prioritaire que l'économie d'un pull-up externe. La complication firmware (logique inversée) est minime et localisée à 2 lignes dans `main.cpp`.
- **Bus I²C séparé pour les Atlas EZO** (ex. `Wire1` sur GPIO 17/18) (rejetée) — complexité d'init (deux instances `TwoWire`, deux mutex à arbitrer), risque de timing à débugger sur deux bus, gain inexistant : la fréquence I²C par défaut (100 kHz) supporte largement DS3231 + 2 EZO simultanés. Les EZO Atlas sont prévus pour le bus partagé.
- **`pinMode(INPUT_PULLUP)` activé immédiatement sur GPIO 23/32/36** (rejetée) — même si les broches deviendront un jour des entrées, activer le pull-up interne avant que la fonctionnalité existe consomme du courant statique inutile et masque un éventuel court-circuit hardware (un pin réservé qui flotte est un signal de test). On préfère le `pinMode` explicite dans la feature qui activera le pin.
- **Ré-injecter les `#define` de pin dans `config.h`** (rejetée) — `config.h` mélangerait runtime config (NVS) et constantes hardware figées. La convention CLAUDE.md impose `kConstantes` dans `constants.h` ; appliquée ici, elle clarifie que le mapping pin n'est PAS modifiable à chaud par l'utilisateur.

## Conséquences

### Positives

- **Mapping unique et lisible** dans `src/constants.h` — plus de `#define` dispersés dans `config.h`.
- **Préparation feature-020 et feature-021** : les pins I²C et OneWire sont prêts, la 2ᵉ sonde DS18B20 et les EZO peuvent être ajoutés sans toucher à la table de pin.
- **Pin `CTN_AUX` réservé** sur GPIO 32 : disponible immédiatement pour une future feature (pompe à chaleur, vanne) sans nouvelle migration GPIO.
- **Bouton reset insensible au strapping** : GPIO 35 input-only n'a aucun rôle au boot, contrairement à GPIO 32 (v1) qui partageait certains states.
- **Style code modernisé** : suppression des derniers `#define` historiques (`PUMP1_PWM_PIN`, `PUMP2_PWM_PIN`, `TEMP_SENSOR_PIN`, `FILTRATION_RELAY_PIN`, `LIGHTING_RELAY_PIN`, `BUILTIN_LED_PIN`, `FACTORY_RESET_BUTTON_PIN`) au profit de `constexpr uint8_t kXxxPin`.

### Négatives / dette assumée

- **Bascule unidirectionnelle** : la branche `pcb-v2` (et toute version ≥ 2.0.0) ne fonctionne plus sur PCB v1. La branche `main` reste compatible v1 jusqu'à fusion explicite — au-delà, plus aucun support v1.
- **Pull-ups externes obligatoires** sur GPIO 35 et GPIO 36 : 10 kΩ vers 3V3 sur le PCB v2. Une défaillance hardware (résistance arrachée, mauvaise soudure) = lecture flottante imprévisible. Le PCB v2 doit être inspecté visuellement avant flash.
- **Inversion de logique du bouton firmware** : si quelqu'un porte ce code sur un autre PCB qui câble le bouton en pull-down, la lecture sera inversée. Le commentaire dans `constants.h` explicite la convention.
- **3 pins réservés sans usage immédiat** : `kRtcSqwPin`, `kCtnAuxPin`, `kRtcIntPin` apparaissent dans `constants.h` mais sont muets côté firmware. Risque de devenir du bruit si les features futures sont reportées indéfiniment. Mitigé par le commentaire « future feature » et la référence à cet ADR.

### Ce que ça verrouille

- **Toute future feature qui utilisera GPIO 23/32/36 doit explicitement appeler `pinMode`** dans son `begin()` — l'ADR documente l'intention « réservé », pas une initialisation automatique.
- **`pumps[0]` = pH (kPumpPhPin)** et **`pumps[1]` = ORP (kPumpOrpPin)** est désormais une convention figée. Un module externe (ex. test interactif via `setManualPump`) doit utiliser cet ordre — l'inversion serait un bug de sécurité chimique (mauvaise pompe activée).
- **Le bouton factory reset est actif bas** sur PCB v2. Toute future logique qui consomme `digitalRead(kFactoryResetButtonPin)` doit comparer à `LOW` pour détecter un appui. Documenté dans `constants.h` et `main.cpp`.
- **L'ADS1115 et `DFRobot_PH` sont en sursis** : feature-020 retirera les libs et la chaîne de calibration ADS1115. Toute feature pH/ORP ajoutée d'ici là doit être migrée par feature-020.
- **Plus de support PCB v1** sur les versions ≥ 2.0.0. La compatibilité descendante n'est pas garantie : un utilisateur qui flashe une 2.0.0 sur PCB v1 verra des lectures aberrantes (mauvais pin OneWire, mauvais relais commandés).

## Références

- Code : [`src/constants.h`](../../src/constants.h) — section « GPIO PIN ASSIGNMENTS - PCB v2 » (10 actifs + 3 réservés)
- Code : [`src/config.h`](../../src/config.h) — suppression des 7 `#define` migrés
- Code : [`src/main.cpp`](../../src/main.cpp) — `pinMode(kFactoryResetButtonPin, INPUT)`, logique bouton inversée (`pressed = digitalRead == LOW`)
- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `pumps[0] = {kPumpPhPin, ...}`, `pumps[1] = {kPumpOrpPin, ...}`
- Code : [`src/filtration.cpp`](../../src/filtration.cpp), [`src/lighting.cpp`](../../src/lighting.cpp), [`src/sensors.cpp`](../../src/sensors.cpp) — références renommées
- Spec : [`specs/features/done/feature-019-gpio-pcb-v2.md`](../../specs/features/done/feature-019-gpio-pcb-v2.md)
- Specs à venir : feature-020 (Atlas EZO pH/ORP), feature-021 (2ᵉ DS18B20 circuit)
- Doc subsystem : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md), [`docs/subsystems/sensors.md`](../subsystems/sensors.md), [`docs/subsystems/filtration.md`](../subsystems/filtration.md), [`docs/subsystems/lighting.md`](../subsystems/lighting.md)
- ADR lié (capteur analogique antérieur) : [ADR-0001](0001-capteurs-analogiques-ads1115.md) — sera partiellement révisé par l'ADR feature-020 (passage Atlas EZO)
- CHANGELOG `[Unreleased]` 2026-05-05 — section Hardware « Bascule cible PCB v1 → PCB v2 »
