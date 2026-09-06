# Feature 104 — Étude de portage ESP32-S3-WROOM-1 (PSRAM)

> 🔢 **Renumérotée le 2026-09-06 : `feature-067` → `feature-104`.** Le numéro 067 était
> porté par DEUX specs de slugs différents — cette étude, et
> `done/feature-067-offset-mesure-ph.md`, close et référencée par `CHANGELOG.md`,
> `data/app.js`, `src/main.cpp`, `src/constants.h` et `src/web_routes_debug.cpp`. Toute
> référence « feature-067 » était donc ambiguë. C'est **cette étude** qui change de
> numéro, précisément parce que rien ne la référençait : renuméroter la spec close aurait
> exigé de réécrire de l'historique. Ambiguïté signalée par `tools/check_specs.sh`
> (règle 2, feature-103).

- **Type** : étude / spec d'architecture (l'étude produira la ou les specs d'exécution)
- **Statut** : todo
- **Version cible** : à déterminer (chantier, pas un correctif)
- **Créée** : 2026-08-12 (mise à jour du BOM PCB v2 par l'utilisateur : U2 =
  ESP32-S3-WROOM-1 « avec de la PSRAM »)
- **Domaine** : plateforme (platformio.ini, constants.h brochage, docs) — l'étude
  elle-même ne modifie AUCUN code
- **Specs liées** : feature-062/063 (migration core 3 — préalable acquis : le S3
  n'existe qu'en core 3/IDF 5), feature-066 (PSRAM WROVER), ADR-0034

## Contexte

Le BOM du PCB v2 (`hardware/BOM.csv` et `kicad v2/BOM.csv`) cible désormais un
module **ESP32-S3-WROOM-1 avec PSRAM** (U2). Le firmware actuel cible l'ESP32
classique (envs `esp32dev` / `esp32dev_core3`, board esp32dev, Xtensa LX6) et ne
peut PAS tourner sur S3 en l'état.

## Incompatibilités déjà identifiées (à compléter par l'étude)

1. **Brochage** : les GPIO 22 (`kI2cSclPin`) et 25 (`kPumpPhPin`) n'existent pas
   sur le S3 (GPIO disponibles : 0-21 et 26-48). GPIO 35 (`kFactoryResetButtonPin`,
   input-only sur classique) a un comportement différent sur S3. Vérifier CHAQUE
   pin de `constants.h` contre le routage réel du PCB v2 (kicad v3 ?).
2. **Flags PSRAM** : `-mfix-esp32-psram-cache-issue` / `-mfix-esp32-psram-cache-strategy=memw`
   sont un workaround silicium de l'ESP32 CLASSIQUE — à retirer sur S3 (build
   S3 échouerait ou serait incorrect avec). Le S3 gère la PSRAM (quad/octal)
   sans workaround ; vérifier la variante du module (N8R2 ? N16R8 ? octal = pins
   35-37 consommées).
3. **Environnement de build** : nouvel env `esp32s3` (board type esp32-s3-devkitc-1
   ou custom), UNIQUEMENT pioarduino/core 3 (le core 2 ne supporte pas le S3) —
   le bi-core `#if ESP_ARDUINO_VERSION_MAJOR` devient sans objet côté S3.
4. **Périphériques** : LEDC (pompes) OK sur S3 mais canaux/fréquences à revalider
   → **passage pool-chemistry OBLIGATOIRE** (PWM des pompes doseuses). ADC/DAC
   différents (pas de DAC sur S3 — vérifier qu'aucun usage). USB-OTG natif
   (opportunité : console série USB).
5. **Partitions/OTA** : table 4 Mo actuelle à confronter à la flash du module
   retenu ; bootloader S3 différent → premier flash USB obligatoire, procédure
   de bascule à écrire (l'OTA croisé classique↔S3 est IMPOSSIBLE — binaires
   d'architectures différentes, garde-fou à prévoir dans /update).
6. **Parachute OOM / heap** : seuils (kOomReserveBytes, garde TLS) calibrés sur
   le heap du classique — à re-mesurer sur S3 (SRAM 512 Ko, mapping différent).

## Livrables de l'étude

- [ ] Tableau de correspondance brochage constants.h ↔ PCB v2 S3 (avec le schéma kicad)
- [ ] Choix du module exact (variante flash/PSRAM) et env PlatformIO S3 qui compile
- [ ] Liste des zones de code à adapter (grep exhaustif des #if core/pins/PSRAM)
- [ ] Procédure de bascule (flash USB initial, garde anti-OTA-croisé)
- [ ] Découpage en specs d'exécution (env+build, brochage, validation pool-chemistry)

## Critères d'acceptation

- [ ] Aucun code modifié par l'étude (lecture seule)
- [ ] Chaque incompatibilité ci-dessus confirmée ou infirmée avec preuve
- [ ] Verdict pool-chemistry sollicité sur le plan PWM/pompes avant toute exécution
