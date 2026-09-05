## Métadonnées
- **ID** : feature-021
- **Slug** : migration-atlas-ezo
- **Type** : feature (refactor majeur — migration capteurs)
- **Statut** : done
- **Priorité** : haute (objectif fonctionnel principal du PCB v2 : lectures pH/ORP fiables filtration en marche)
- **Version cible** : 2.0.0
- **Dernière mise à jour** : 2026-05-06
- **Spec(s) liée(s)** : feature-019 (mapping GPIO PCB v2 — fournit les pins I²C 21/22 et la convention pumps[0]=pH/pumps[1]=ORP), **feature-020 (2 sondes DS18B20 — DÉPENDANCE OBLIGATOIRE : cette spec consomme `Sensors::getWaterTemperature()` pour la compensation T° du pH côté EZO, ce qui exige l'identification préalable des 2 sondes)**, ADR-0001 (capteurs analogiques ADS1115 — sera **partiellement supersédé** par cette spec), ADR-0012 (mapping GPIO PCB v2)
- **Ordre d'implémentation** : feature-020 **doit être implémentée et validée avant** cette feature-021. Sans feature-020, la compensation T° du pH utiliserait la première sonde détectée par défaut, ce qui peut être la sonde « circuit » sur certains PCB (ordre de scan OneWire non garanti) → erreur de mesure pH.

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (calibration, payload sensors)
- [x] MQTT / Home Assistant (auto-discovery cohérent — mêmes topics)
- [ ] UART / écran LVGL
- [x] OTA — fichier `platformio.ini` modifié (suppression libs ADS1115 + DFRobot_PH)
- [ ] Authentification
- [x] Historique / persistance (NVS : suppression clés calibration ADS1115/DFRobot_PH)
- [x] Frontend (`data/`) — refonte UI calibration pH/ORP (4 steps EZO au lieu de sélecteur 1pt/2pts ADS1115)

## Agents requis
- [x] spec-reader
- [x] architect — arbitre la nouvelle abstraction `Sensors` autour des EZO + stratégie d'init bus I²C partagé (DS3231 + EZO pH + EZO ORP)
- [x] **pool-chemistry — OBLIGATOIRE** (refonte calibration et lecture des sondes pH/ORP qui pilotent le dosage chimique)
- [x] ux-ui-designer — refonte de la card Calibration pH (2 points : mid 7.0 + low 4.0) et ORP (1 point) cohérente avec l'esprit existant
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Remplacer la chaîne de mesure analogique pH/ORP du PCB v1 (ADS1115 + lib `DFRobot_PH` + calculs offset/slope côté firmware) par les **modules Atlas Scientific EZO Embedded** présents sur le PCB v2 (EZO pH à 0x63, EZO ORP à 0x62, bus I²C partagé avec le DS3231). L'objectif est d'**obtenir des lectures pH et ORP fiables et stables même quand la filtration tourne** (cf. ADR-0012 motivation isolation galvanique).

La calibration pH passe à **2 points (mid 7.0 + low 4.0)** côté UI. La calibration ORP reste à **1 point**. La compensation de température du pH est appliquée automatiquement via la sonde DS18B20 d'eau de piscine.

L'ADS1115 et la lib `DFRobot_PH` sont **entièrement supprimés** du firmware (libs PlatformIO retirées, fichiers source nettoyés, clés NVS associées supprimées).

## Contexte

Le PCB v1 souffrait de **courants de fuite** entre la pompe de filtration 230V et les modules pH/ORP analogiques (cf. ADR-0012). Conséquence : valeurs pH/ORP instables dès que la filtration tournait, ce qui était un problème majeur car la régulation chimique a précisément besoin de mesurer pendant la filtration (eau qui circule devant la sonde).

Le PCB v2 résout ce problème par 3 leviers cumulés (cf. ADR-0012) : modules EZO numériques (I²C), alimentation isolée, bus I²C galvaniquement isolé. Cette spec adapte le firmware pour exploiter ce nouveau matériel.

Bénéfices attendus côté firmware :

- **Précision** : les EZO Embedded sont des modules certifiés laboratoire (résolution 0.001 pH, 1 mV ORP)
- **Calibration plus fiable** : l'EZO mémorise sa propre calibration en interne (EEPROM module), plus de calculs manuels offset/slope côté firmware
- **Compensation T° intégrée** : envoyer la T° de l'eau au module EZO pH via commande I²C, l'EZO applique la correction Nernst
- **Code plus simple** : lecture I²C (commande `R`) renvoie une chaîne ASCII `"7.234"` ou `"650"` directement, plus de conversions ADC

## Comportement attendu

### Côté firmware

#### Initialisation

Au boot dans `Sensors::begin()` :
- Bus I²C partagé `Wire.begin()` (déjà fait pour DS3231)
- Détection EZO pH à `0x63` (nouvelle constante `kEzoPhI2cAddress`) — log si absent (mais ne pas crasher)
- Détection EZO ORP à `0x62` (nouvelle constante `kEzoOrpI2cAddress`) — idem
- Lecture du firmware EZO (`I` info command) au boot pour logger la version (utile diagnostic)
- Configuration du mode lecture continue désactivé (`C,0`) — on lira à la demande

#### Lecture cyclique

Toutes les `kPhOrpSensorIntervalMs` (5s, déjà existant) :
- Envoyer T° eau au module pH : `RT,<temp>` où `<temp>` est la lecture DS18B20 actuelle (en °C, 1 décimale)
- Wait 600 ms (timing requis par EZO pour traiter la commande)
- Envoyer commande de lecture : `R`
- Read 32 octets I²C depuis le module
- Parser la chaîne ASCII (ex `"7.234"`) en `float`
- Stocker dans `sensors.getPh()` / `sensors.getOrp()` (interface inchangée)

#### Calibration

**pH (2 points : mid 7.0 + low 4.0)** :

Étapes UI :
1. Plonger la sonde dans solution pH 7.0
2. Attendre stabilisation (1 min)
3. Bouton « Calibrer point milieu » → firmware envoie `Cal,mid,7.00` à l'EZO pH
4. Plonger la sonde dans solution pH 4.0
5. Attendre stabilisation (1 min)
6. Bouton « Calibrer point bas » → firmware envoie `Cal,low,4.00` à l'EZO pH
7. UI affiche callout « Calibré 2 points »

Le firmware ne calcule plus rien : c'est l'EZO qui mémorise tout.

**ORP (1 point)** :

Étapes UI :
1. Plonger la sonde dans solution standard ORP (typiquement 225 mV ou 470 mV — l'utilisateur saisit la valeur)
2. Attendre stabilisation (1 min)
3. Bouton « Calibrer » → firmware envoie `Cal,<valeur>` à l'EZO ORP
4. UI affiche callout « Calibré »

#### Endpoints HTTP

- `POST /calibrate_ph` modifié : payload `{ "step": "mid" | "low" }`. Firmware envoie la commande `Cal,mid,7.00` ou `Cal,low,4.00` à l'EZO pH. Plus de paramètres `voltage`/`offset`/`slope` (gérés en interne EZO). Renvoie `{ "success": true, "message": "Point milieu calibré" }`.
- `POST /calibrate_orp` modifié : payload `{ "reference": <mV> }`. Firmware envoie `Cal,<reference>` à l'EZO ORP. Renvoie `{ "success": true }`.
- Routes existantes conservées pour compatibilité d'API (mêmes URLs).

### Côté UI

- **Page pH** : refonte de la card calibration en 2 sous-blocs successifs (« Étalon 7.0 » + « Étalon 4.0 ») avec boutons distincts. Suppression du sélecteur 1pt/2pts (l'UX existant ADS1115 disparaît).
- **Page ORP** : conservation du flux 1 point (input pour la valeur de référence + bouton).
- **Card Régulation pH/ORP** : inchangée (consigne, mode régulation, etc.).
- **Indicateur calibration** : callout « Calibré 2 points » (pH) ou « Calibré » (ORP) lu depuis l'EZO via commande `Cal,?` (renvoie 0/1/2/3 selon le nombre de points calibrés).

### Côté MQTT/Home Assistant

- Topics MQTT inchangés (`pool/sensors/ph`, `pool/sensors/orp`, etc.)
- Auto-discovery HA inchangé (mêmes entités sensor)
- Précision payload améliorée : 3 décimales pour pH (au lieu de 1), 1 décimale pour ORP en mV

### Cas limites

- **EZO non détecté au boot** : log `[ERROR] EZO pH non détecté à 0x63` ou idem ORP. Sensors::isInitialized() retourne false. Régulation chimique automatique inhibée. UI affiche capteur stale.
- **EZO retourne valeur hors plage** (pH < 0 ou > 14, ORP < -1100 ou > 1100) : ignorer la lecture, garder la précédente, log warning.
- **Réponse I²C tronquée/vide** : retry une fois, sinon log warning et garder lecture précédente.
- **Calibration en cours et lecture demandée** : sérialiser via le mutex I²C existant (`kI2cMutexTimeoutMs`).
- **Migration utilisateurs** : les clés NVS `ph_cal_temp`, `orp_cal_off`, `orp_cal_slope`, `orp_cal_ref` deviennent obsolètes — ne pas les lire ni les écrire dans cette spec, suppression dans le code.
- **Compensation T° si DS18B20 absente** : envoyer 25.0 °C par défaut à l'EZO pH (valeur de référence).

## Comportement actuel

- `src/sensors.cpp` lit l'ADS1115 (canaux pH et ORP) en analogique via `Adafruit_ADS1X15` (lib).
- Conversion mV → pH via formule Nernst dans `DFRobot_PH` (lib externe).
- Calibration pH : 1 point (mémorisée dans EEPROM ESP32 via `DFRobot_PH`) ou 2 points (offset+slope custom).
- Calibration ORP : 1 ou 2 points (offset + slope) calculée côté firmware, stockée en NVS.
- UI offre un sélecteur calibration 1pt/2pts pour pH et ORP.
- ADS1115 connecté sur I²C bus partagé avec DS3231.

## Contraintes techniques

- ESP32 (esp32dev), framework Arduino + PlatformIO
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB — vérifier `./build_fs.sh`
- AsyncWebServer : aucun handler bloquant > 50 ms (les commandes EZO bloquent ~600 ms — doivent être appelées depuis une tâche dédiée ou via `mqttTask`/`loopTask` non concurrente avec les handlers HTTP)
- Concurrence : bus I²C partagé entre 3 devices (DS3231 + EZO pH + EZO ORP). Mutex existant `i2cMutex` (cf. `kI2cMutexTimeoutMs`) à utiliser pour sérialiser les accès.
- Watchdog 30 s : la séquence calibration peut prendre plusieurs secondes (envoi commande + wait 600 ms + read). Reset wdt entre étapes.
- **Sécurité chimique** : tant que la calibration EZO n'est pas validée, **inhiber la régulation pH automatique** (laisser l'utilisateur décider du dosage manuel). pool-chemistry à valider.
- **Migration NVS** : suppression silencieuse des clés obsolètes (`ph_cal_temp`, etc.) — pas de migration de données.

## Fichiers potentiellement concernés

- `src/sensors.h` / `src/sensors.cpp` — refonte majeure (suppression ADS1115/DFRobot_PH, ajout pilotage EZO I²C)
- `src/constants.h` — ajout `kEzoPhI2cAddress = 0x63`, `kEzoOrpI2cAddress = 0x62`, `kEzoCommandWaitMs = 600`, `kEzoReadBufferSize = 32`
- `src/web_routes_calibration.cpp` — refonte des handlers `/calibrate_ph` et `/calibrate_orp`
- `src/web_routes_data.cpp` — payload pH/ORP (ajustement précision si nécessaire)
- `src/config.h` / `src/config.cpp` — suppression des champs et clés NVS obsolètes (`phCalTemp`, `orpCalOffset`, `orpCalSlope`, `orpCalRef`, `orpCalDate`)
- `data/index.html` — refonte UI calibration pH (2 sous-blocs mid + low) et ORP (1 point uniquement)
- `data/app.js` — refonte handlers JS calibration pH/ORP, suppression sélecteur 1pt/2pts
- `data/app.css` — éventuels styles pour les sous-blocs calibration
- `platformio.ini` — suppression `Adafruit_ADS1X15` et `DFRobot_PH` des `lib_deps`
- `lib/DFRobot_PH/` — éventuellement à supprimer si lib locale (à vérifier)
- `docs/subsystems/sensors.md` — refonte majeure
- `docs/features/page-ph.md`, `docs/features/page-orp.md` — UI calibration
- `docs/adr/0001-capteurs-analogiques-ads1115.md` — note **Superseded by feature-021 ADR** (ADR-0013 ?)
- `docs/adr/NNNN-migration-atlas-ezo.md` — **nouvel ADR** (probablement ADR-0013)
- `CHANGELOG.md` — entrée Hardware/Firmware sous version 2.0.0

## Livrables — check-list

- [x] A1 — Constantes EZO ajoutées dans `src/constants.h` : adresses 0x63/0x62, délais EZO (`kEzoReadDelayMs=900`, `kEzoCalDelayMs=900`, `kEzoRtDelayMs=600`), `kSensorStaleTimeoutMs=20000`, `kEzoBusFailMaxConsecutive=2` (les noms finaux diffèrent des noms initialement proposés)
- [x] A2 — `Sensors::begin()` détecte les EZO sur I²C via `AtlasEzoSensor::readInfo()` (cmd `I`), logue la version
- [x] A3 — `Sensors::update()` → `_readEzoSensors()` lit pH avec compensation T° (`RT,<t>` + delay 600 ms + `R` + delay 900 ms) et ORP (`R` direct)
- [x] A4 — Mutex I²C tenu pendant TOUTE la séquence `RT + delay + R + delay` (cond #6 pool-chemistry, atomicité chimique)
- [x] A5 — `POST /calibrate_ph {step:"mid"|"low"}` → enqueue `Cal,mid,7.00` ou `Cal,low,4.00` via `_ezoQueue`
- [x] A6 — `POST /calibrate_orp {reference:<mV>}` (plage 0..1000 mV) → enqueue `Cal,<mV>`
- [x] A7 — `getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` lus en `begin()` puis rafraîchis après chaque calibration
- [x] A8 — `Adafruit ADS1X15` et `DFRobot_PH` retirés de `platformio.ini` (Pass 4a) + tous les `#include` et appels supprimés
- [x] A9 — Champs NVS obsolètes supprimés (7 champs `MqttConfig` + clés associées)
- [x] A10 — Régulation auto inhibée si `phCalPoints < 2` ou `orpCalPoints < 1` (cond #2 pool-chemistry, intégré à `canDose(int)`)
- [x] B1 — `data/index.html` page pH : 2 sous-blocs `.cal-point-block` parallèles (mid + low)
- [x] B2 — `data/index.html` page ORP : 1 sous-bloc `.cal-point-block` avec input référence (sélecteur 1pt/2pts supprimé)
- [x] B3 — `data/app.js` : handlers calibration pH/ORP via POST + polling 15 s sur `phCalPoints`/`orpCalPoints`
- [x] B4 — Callouts vert/ambré/rouge sur cartes Régulation pH/ORP selon `phCalPoints`/`orpCalPoints`
- [x] B5 — Code JS legacy ADS1115 supprimé (sélecteur, calculs offset/slope)
- [ ] D1 — Test manuel humain : boot avec EZO branchés, vérifier détection + version EZO dans les logs (à effectuer sur hardware)
- [ ] D2 — Test manuel humain : calibration pH 2 points avec solutions tampons certifiées (à effectuer sur hardware)
- [ ] D3 — Test manuel humain : calibration ORP 1 point avec solution standard (à effectuer sur hardware)
- [ ] D4 — Test manuel humain : **filtration ON pendant lecture pH/ORP, vérifier ABSENCE de variation parasite** (test critique, à effectuer sur hardware)
- [ ] D5 — Test compensation T° : changer la T° eau, vérifier ajustement pH (à effectuer sur hardware)
- [ ] D6 — Test résilience : débrancher temporairement un EZO, vérifier non-crash + lecture conservée (à effectuer sur hardware)
- [x] D7 — `pio run` SUCCESS, RAM 16.5 %, Flash 98.8 %, 0 nouveau warning
- [x] D8 — `./build_fs.sh` SUCCESS, FS 1.1 MB OK
- [x] C1 — `docs/subsystems/sensors.md` refondu (architecture EZO, mutex I²C, queue, cache cal_points, stale)
- [x] C2 — `docs/features/page-ph.md` et `page-orp.md` mis à jour (refonte UI documentée)
- [x] C3 — ADR créé : `docs/adr/0014-migration-atlas-ezo.md`
- [x] C4 — `docs/adr/0001-capteurs-analogiques-ads1115.md` annoté « Superseded by ADR-0014 »
- [x] C5 — `CHANGELOG.md` : entrée majeure v2.0.0 sous Hardware/Firmware/API/MQTT/Frontend/Documentation/Migration

## Nouvelles signatures / API

- Endpoints HTTP (modifiés mais URLs inchangées) :
  - `POST /calibrate_ph` payload `{ "step": "mid" | "low" }` (au lieu de `{ "voltage": <mV> }` ou similaire)
  - `POST /calibrate_orp` payload `{ "reference": <mV> }` (inchangé en signature, mais derrière l'EZO ne calcule plus rien)
- Champs WebSocket : aucun nouveau (mêmes `ph`, `orp`, `temperature` dans la payload `sensor_data`)
- Topics MQTT : aucun nouveau, précision améliorée (pH 3 décimales)
- Fonctions publiques nouvelles dans `src/sensors.h` :
  - `int Sensors::getPhCalibrationPoints()` — renvoie 0/1/2 selon le nombre de points calibrés EZO pH
  - `int Sensors::getOrpCalibrationPoints()` — renvoie 0/1 selon le nombre de points calibrés EZO ORP
  - `bool Sensors::calibratePhMid()` / `bool Sensors::calibratePhLow()` — déclenchent la calibration EZO
  - `bool Sensors::calibrateOrp(float referenceMv)` — idem ORP
- Fonctions publiques **supprimées** : toutes celles liées à `DFRobot_PH` (offset, slope, calculs custom)
- Constantes nouvelles dans `src/constants.h` :
  - `constexpr uint8_t kEzoPhI2cAddress = 0x63;`
  - `constexpr uint8_t kEzoOrpI2cAddress = 0x62;`
  - `constexpr uint32_t kEzoCommandWaitMs = 600;` (timing Atlas EZO Embedded)
  - `constexpr size_t kEzoReadBufferSize = 32;`

## Sécurité chimique (obligatoire — pool-chemistry doit valider)

- **Dosage impliqué : oui** — le pH et l'ORP pilotent directement les pompes doseuses (acide pH-, chlore)
- Limites journalières / horaires concernées : `maxPhMinusMlPerDay`, `maxChlorineMlPerDay` — inchangées, mais la lecture qui les déclenche change radicalement de chaîne
- Filtration requise avant action : oui (déjà géré par `canDose()`, inchangé)
- Calibration capteur modifiée : **OUI, totalement** — c'est l'objet principal de la spec
- Algorithme PID touché : non (PID consomme `getPh()` / `getOrp()` qui restent en `float`, juste la source change)
- Risques identifiés :
  - **Lecture pH/ORP fausse au boot tant que l'EZO n'est pas calibré** → inhiber régulation auto si `Cal,?` < 2 (pH) ou < 1 (ORP) (cf. A10)
  - **Saut de valeur lors de la migration** : un utilisateur qui flashe directement v2.0.0 sans recalibrer aura une lecture par défaut EZO (pas zéro, mais valeur usine ~7.0 pour pH, 0 mV pour ORP). Pas de risque chimique direct (la valeur par défaut pH 7.0 ne déclenche pas de dosage acide), mais un utilisateur qui ne suit pas le guide migration peut être surpris.
  - **Compensation T° absente si DS18B20 HS** : valeur 25 °C par défaut envoyée à l'EZO pH, l'erreur de mesure pH reste < 0.1 pH dans la plage 15-30 °C piscine, acceptable.
  - **Bus I²C partagé** : si EZO bloque (firmware EZO buggé), le DS3231 RTC peut aussi être inaccessible. Mutex avec timeout court à mettre en place.

## ADR attendu ?

- **Probable** : OUI
- **Critère(s) déclenché(s)** : 1 (contrainte structurante — change la chaîne de mesure complète), 3 (alternatives crédibles à arbitrer : conserver ADS1115+DFRobot_PH, passer à `EZO_pH` lib externe, lib custom EZO maison), 4 (verrouillage long terme — le firmware sera lié à l'écosystème Atlas EZO)
- **Sujet pressenti** : `docs/adr/NNNN-migration-atlas-ezo.md` (probablement ADR-0013)
- **Note** : ADR-0001 (Capteurs analogiques ADS1115) sera annoté « Superseded by ADR-0013 (Atlas EZO) »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /calibrate_ph` (signature payload modifiée)
- `POST /calibrate_orp` (signature payload conservée, sémantique différente derrière)

### Champs WebSocket
- aucun nouveau (mêmes `ph`, `orp`, `temperature` dans `sensor_data`)
- Précision : `ph` passe à 3 décimales (avant : 1 ou 2)

### Topics MQTT
- aucun nouveau
- Précision identique à WS (3 décimales pH)

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-ph.md` — UI calibration 2 points (mid + low)
- `docs/features/page-orp.md` — UI calibration 1 point
- `docs/subsystems/sensors.md` — refonte majeure : architecture EZO, mutex I²C, commandes EZO clés (`R`, `Cal`, `RT`, `I`, `Cal,?`), gestion absence module

#### Docs transverses
- `docs/API.md` — modification des payloads `POST /calibrate_ph` et `/calibrate_orp`
- `docs/MQTT.md` — précision améliorée pH (3 décimales)
- `docs/BUILD.md` — éventuelle note sur la suppression des libs ADS1115/DFRobot_PH
- `docs/UPDATE_GUIDE.md` — note de migration v1.x → v2.0.0 : flash uniquement sur PCB v2 + recalibration obligatoire des sondes après bascule
- `CHANGELOG.md` — entrée majeure Hardware + Firmware + UI + Documentation sous v2.0.0

#### Docs immuables
- `docs/adr/NNNN-migration-atlas-ezo.md` — à créer (ADR-0013 probable)
- `docs/adr/0001-capteurs-analogiques-ads1115.md` — annotation « Superseded by ADR-0013 »

## Critères d'acceptation

- [ ] AC1 — Lecture pH stable à ± 0.05 dans une solution tampon certifiée pH 7.00 après calibration mid + low (test humain — à effectuer sur hardware)
- [ ] AC2 — Lecture ORP stable à ± 5 mV dans une solution standard après calibration 1 point (test humain — à effectuer sur hardware)
- [ ] AC3 — **Lecture pH/ORP non perturbée par la filtration** (différence < 0.05 pH et < 10 mV ORP entre filtration ON et OFF) — **objectif fonctionnel principal, à valider sur hardware**
- [x] AC4 — Régulation pH automatique inhibée si EZO pH non calibré (`canDose(0)` cond #2 : `phCalPoints < 2` → false ; cond #5 : `phCalPoints == -1` → false)
- [x] AC5 — Aucun crash firmware si un EZO est débranché en runtime (gestion fail-streak `kEzoBusFailMaxConsecutive=2` → cache cal_points = -1 + lecture = NaN ; log critical 1×)
- [x] AC6 — Bus I²C protégé par mutex `i2cMutex` (timeout `kI2cMutexTimeoutMs=2000`) tenu pendant TOUTE la séquence `RT+R` (cond #6)
- [x] AC7 — Topics MQTT existants conservés ; nouveaux topics ajoutés (`ph_cal_points`, `orp_cal_points`, alertes) sans casser la rétrocompat HA. `orp_cal_valid` conservé en miroir.
- [x] AC8 — Aucune référence subsistante à `Adafruit_ADS1X15` ni `DFRobot_PH` (Pass 4a — `lib_deps`, includes, code)
- [x] AC9 — Tous les libellés utilisateur sont en français (vérifié par code-reviewer)
- [x] AC10 — `pio run` compile sans nouveau warning, RAM 16.5 %, Flash 98.8 %
- [x] AC11 — `./build_fs.sh` SUCCESS, partition 1088 KB OK (FS 1.1 MB)
- [x] AC12 — ADR-0014 créé, ADR-0001 annoté « Superseded by ADR-0014 » (numéro 0013 déjà pris par feature-020)
- [x] AC13 — `docs/UPDATE_GUIDE.md` documente la procédure de migration (flash + recalibration obligatoire pH 2 points + ORP 1 point + délais stabilisation)

## Plan de test manuel

### Préparation
- PCB v2 connecté à l'ESP32, modules EZO pH et ORP en place
- Solutions tampons certifiées : pH 7.00, pH 4.00, ORP standard (au choix : 225 mV ou 470 mV)
- Sonde DS18B20 eau dans un bain d'eau à T° connue (~25 °C)

### Tests fonctionnels
1. **Boot et détection** : flash, observer logs ESP32 — `EZO pH détecté à 0x63 (firmware vX.YY)` et idem ORP. Si log `non détecté`, vérifier câblage I²C + alimentation EZO.
2. **Calibration pH point milieu** : plonger sonde dans pH 7.00, attendre 1 min, ouvrir UI page pH, cliquer « Calibrer point milieu ». Vérifier réponse `success`. Lecture pH affichée doit converger vers 7.00 ± 0.05 en quelques secondes.
3. **Calibration pH point bas** : rincer la sonde, plonger dans pH 4.00, attendre 1 min, cliquer « Calibrer point bas ». Vérifier réponse `success`. Lecture pH doit converger vers 4.00 ± 0.05.
4. **Callout calibration** : UI affiche « Calibré 2 points ». Recharger la page, vérifier que l'état persiste (lu depuis l'EZO via `Cal,?`).
5. **Calibration ORP** : plonger sonde dans solution standard (note la valeur, ex. 225 mV), attendre 1 min, saisir la valeur dans l'UI, cliquer « Calibrer ». Vérifier réponse `success`. Lecture doit converger.
6. **Test filtration ON vs OFF (objectif principal)** : avec sonde dans la même eau (par ex. eau du robinet), lecture pH et ORP filtration arrêtée pendant 1 min, puis filtration en marche pendant 1 min. **Vérifier différence < 0.05 pH et < 10 mV ORP**. C'est le test critique de validation du PCB v2.
7. **Test compensation T°** : modifier `setpoint` ou simuler T° (ou simplement chauffer/refroidir l'eau de la sonde). Vérifier que la lecture pH ajuste légèrement (typiquement -0.003 pH par °C autour de pH 7).
8. **Test résilience EZO débranché** : débrancher physiquement le câble I²C d'un EZO en runtime. Vérifier log warning, pas de crash, lecture conservée à la dernière valeur connue.
9. **Test régression UI** : page Régulation pH/ORP, MQTT, auto-discovery HA — tout doit fonctionner identiquement à v1.

## Notes pour agent

- **Atlas EZO Embedded** : modules I²C-only (pas d'UART). Adresses par défaut 0x63 (pH), 0x62 (ORP). Toutes les commandes EZO sont des chaînes ASCII envoyées en `Wire.write` puis lues en `Wire.requestFrom`. Doc commande complète : datasheet Atlas Scientific.
- **Timing critique** : entre `Wire.write(cmd)` et `Wire.requestFrom(...)` il faut **600 ms** pour les commandes de lecture/calibration. Pour `R` (read sample), parfois 900 ms si T° vient d'être envoyée. Le code doit utiliser `delay(kEzoCommandWaitMs)` (mais sous mutex I²C uniquement, pour ne pas bloquer le bus).
- **Format de réponse EZO** : 1er octet = code statut (1=succès, 254=pas encore prêt — re-essayer, 255=pas de data, 2=erreur). Octets suivants = chaîne ASCII null-terminée.
- **Pas de lib Atlas officielle Arduino-ESP32 propre** : on peut utiliser `EZO_PH` (lib externe) mais elle est limitée. **Recommandation** : implémenter une mini-classe `Atlas EZO` maison dans `src/sensors.cpp` (50-80 lignes) qui encapsule les 4-5 commandes utiles (`R`, `Cal,...`, `RT,...`, `Cal,?`, `I`). Plus simple à maintenir et debugger.
- **Compensation T° pH** : envoyer `RT,<temp>` AVANT chaque `R`. La commande `RT,<t>` envoie la T° **et** lit le pH compensé en une transaction (gain de temps). Format : `RT,25.5` (1 décimale).
- **DS18B20 (sonde eau via feature-020)** : cette spec dépend de `Sensors::getWaterTemperature()` exposée par feature-020. Au moment d'implémenter feature-021, feature-020 doit être livrée et la sonde « eau » doit être identifiée. Si `getWaterTemperature()` retourne NaN (sonde non identifiée ou en erreur), la commande `RT,<temp>` envoyée à l'EZO pH utilise une **valeur de référence sécuritaire 25.0 °C** (erreur de mesure < 0.1 pH dans la plage 15-30 °C piscine).
- **Suppression des libs** : retirer `Adafruit_ADS1X15` et `DFRobot_PH` de `platformio.ini` `lib_deps` AVANT de supprimer les `#include` dans le code, pour s'assurer qu'on ne casse pas le build progressivement. Build après chaque grosse étape.
- **Préservation MQTT/HA** : aucun changement côté topic ou auto-discovery. Les utilisateurs HA existants ne doivent rien changer chez eux.

---

## Plan d'implémentation consolidé (cadrage validé 2026-05-06)

### Validations préalables

- **spec-reader GO** ✓
- **pool-chemistry GO sous 6 conditions impératives** (cf. ci-dessous)
- **ux-ui-designer** : refonte UI calibration validée (2 sous-blocs parallèles pH, 1 point ORP, chip ambrée d'inhibition)
- **architect** : plan technique en 14 étapes, arbitrages A1/A2/A3/A4 tranchés

### 6 conditions impératives pool-chemistry

1. **`kSensorStaleTimeoutMs = 20000`** (~20 s) : si dernière lecture EZO valide > 3 cycles, `getPh()`/`getOrp()` retournent NaN, `pump_controller::canDose()` refuse dosage automatique. Log critical à la transition.
2. **Inhibition régulation auto si `Cal,?` injoignable** (pas seulement < 2) : `getPhCalibrationPoints()` retourne -1 si EZO muet, 0/1/2/3 sinon.
3. **Stabilisation post-calibration automatique** : `armStabilizationTimer()` à la fin de chaque calibration EZO réussie.
4. **Logger `critical` + alerte MQTT `pool/alerts/calibration_required`** (retain) au premier boot non calibré et à chaque transition vers stale. Clear (retain payload vide) à la sortie.
5. **Bloquer dosage si bus I²C indisponible** (timeout `kI2cMutexTimeoutMs` dépassé sur 2 cycles consécutifs).
6. **Tenir le mutex I²C pendant toute la séquence `RT,<t>` + delay 600 ms + `R`** pour éviter qu'une autre lib (DS3231) interrompt entre RT et R.

### Décisions architect (arbitrages)

- **A1 — Routes** : route unique `POST /calibrate_ph` paramétrée avec body `{step:"mid"|"low"}` (option a). Cohérent UI 2 boutons + UART écran adaptable trivialement.
- **A2 — Exécution** : queue + `loopTask` (option b). Handler HTTP retourne `{success:true, queued:true}` immédiatement, l'UI observe la transition via WS. Pattern déjà existant (mqttTask).
- **A3 — ADR** : **ADR-0014** (ADR-0013 déjà pris par feature-020).
- **A4 — Mini-classe maison** : `AtlasEzoSensor` dans `src/atlas_ezo.h/cpp` (~80 lignes). Aucune lib Atlas EZO Arduino mature.

### Décisions UX (workflow calibration)

- **pH** : 2 sous-blocs parallèles dans `#ph-card-calibration` (pas de stepper séquentiel). Chacun avec son `__state` (Calibré/Non calibré), 2 micro-étapes (plonger + attendre), bouton « Calibrer le point 7.0 » / « Calibrer le point 4.0 ».
- **ORP** : 1 input pour valeur de référence + 1 bouton « Calibrer ». Suppression du sélecteur 1pt/2pts existant.
- **Statut** : callout vert « Calibré 2 points ✓ » dans card régulation + chip ambrée d'inhibition (réutilise `.chip` existant) tant que `phCalPoints < 2` ou `orpCalPoints < 1`.
- **Lecture live** : `<div class="readout">` rafraîchi 5 s pendant la calibration.
- **Nouvelle classe CSS** : `.cal-point-block` (encadrement des 2 sous-blocs). Tout le reste réutilise l'existant.

### Plan d'implémentation en 14 étapes (à découper en 4 passes courtes pour éviter les timeouts)

**Pass 1** : `src/atlas_ezo.h/cpp` (mini-classe) + `src/constants.h` (constantes EZO + `kSensorStaleTimeoutMs`)

**Pass 2** : refonte `src/sensors.h/cpp` (suppression ADS1115/DFRobot_PH, intégration AtlasEzoSensor, queue commandes, getPh/getOrp avec stale timeout, getPhCalibrationPoints/getOrpCalibrationPoints) + suppression champs NVS `src/config.h/cpp`

**Pass 3** : `src/pump_controller.cpp::canDose()` (durcissement 6 conditions pool-chemistry) + `src/mqtt_manager.cpp` (alerte calibration_required + auto-discovery HA pour phCalPoints/orpCalPoints) + `src/ws_manager.cpp` (champs WS phCalPoints/orpCalPoints)

**Pass 4** : `src/web_routes_calibration.cpp` (routes refondues, retour immédiat queued:true) + `src/uart_commands.cpp` (adaptation actions UART) + `data/index.html` + `data/app.js` + `data/app.css` (UI calibration refonte) + `platformio.ini` (suppression libs ADS1115/DFRobot_PH)

### ADR-0014 prévu

- **Sujet** : « Migration Atlas EZO pH/ORP »
- **Critères déclencheurs** : 1 (contrainte structurante — change la chaîne de mesure complète), 3 (alternatives crédibles à arbitrer), 4 (verrouillage long terme — firmware lié à l'écosystème Atlas EZO)
- **Alternatives écartées** :
  - Lib externe Arduino-EZO (aucune lib mature)
  - Conserver ADS1115 + filtre numérique amélioré (ne résout pas la dérive thermique ni la calibration laborieuse)
  - Chaîne mixte (ADS1115 pH + EZO ORP) — double effort de maintenance
  - Tâche FreeRTOS dédiée Atlas (surcoût stack injustifié pour 2-3 commandes/jour)
  - Exécution synchrone des cal dans handler HTTP (viole AsyncWebServer 50 ms)
- **Note** : annoter ADR-0001 « Superseded by ADR-0014 »

### Tests à prévoir

- Unitaire `AtlasEzoSensor` : mock I²C, parsing réponses (`?CAL,2`, `7.012`, `*ER`)
- Intégration : `Cal,?` injoignable → `getPhCalibrationPoints() == -1` → `canDose() == false`
- Intégration : stale > 20 s → NaN + log critical + alerte MQTT
- Intégration : calibration mid puis low → `phCalPoints` passe 0→1→2, alerte clearée, `armStabilizationTimer()` appelé
- UI : queue → spinner → WS transition → callout success
- Régression UART écran : `actionCalibratePhNeutral` enqueue sans crash

## Hors périmètre

- Pas de gestion 2 sondes DS18B20 (eau + circuit) — c'est feature-020. Cette spec **consomme** `Sensors::getWaterTemperature()` exposée par feature-020 (dépendance obligatoire).
- Pas de migration des données NVS de calibration ADS1115 vers EZO (techniquement impossible : chaîne de mesure totalement différente).
- Pas de support multi-PCB simultané (la branche `pcb-v2` cible le v2 seul).
- Pas de modification du protocole MQTT, des topics, ou de l'auto-discovery HA.
- Pas de migration vers `esp-mqtt` (hors scope, séparé).
- Pas de modification des limites de dosage chimique (`maxPhMinusMlPerDay`, etc.).
- Pas de modification du PID de régulation (algorithme inchangé, juste la source de mesure change).

---

## Notes d'implémentation

- **Date livraison** : 2026-05-06
- **Statut final** : livré — tests verts (`pio run` SUCCESS, RAM 16.5 %, Flash 98.8 %, 0 nouveau warning), revues `pool-chemistry` + `code-reviewer` OK, correctifs Pass 3.5 appliqués.

### Passes effectuées

| Pass | Périmètre | Fichiers principaux |
|------|-----------|---------------------|
| Pass 1 | Mini-classe `AtlasEzoSensor` + constantes EZO + stale + bus fail | `src/atlas_ezo.h`, `src/atlas_ezo.cpp`, `src/constants.h` |
| Pass 2 | Refonte `Sensors` (suppression ADS1115/DFRobot_PH, queue `_ezoQueue`, getPh/getOrp avec stale, getPhCalibrationPoints + cache) + suppression champs NVS legacy | `src/sensors.h/cpp`, `src/config.h/cpp` |
| Pass 3 | `canDose(int)` 6 conditions pool-chemistry + alertes MQTT `calibration_required` / `sensor_stale` + champs WS `phCalPoints`/`orpCalPoints` | `src/pump_controller.cpp`, `src/mqtt_manager.cpp`, `src/ws_manager.cpp` |
| Pass 3.5 | Correctifs pool-chemistry post-revue : invalidation cache + lecture en mode bus dégradé ; ring buffer anti-rafale 6/min + 20/15min | `src/sensors.cpp`, `src/pump_controller.cpp`, `src/constants.h` |
| Pass 4a | Routes HTTP refondues + adaptation UART écran + suppression `lib_deps` ADS1115/DFRobot_PH | `src/web_routes_calibration.cpp`, `src/uart_commands.cpp`, `platformio.ini` |
| Pass 4b | Refonte UI calibration (cartes pH/ORP, callouts, polling) | `data/index.html`, `data/app.js`, `data/app.css` |

### Décisions structurantes

- **Cache cal_points** (`getPhCalibrationPointsCached()`) vs lecture live ~900 ms bloquante : indispensable pour le hot path PID 100 Hz / WS broadcast 5 s. La version live est réservée aux routes de diagnostic et `mqttTask`.
- **Surcharge legacy `armStabilizationTimer()` (sans paramètre)** conservée : les sites « globaux » (filtration, boot continu, rollover minuit) gardent leur ergonomie historique en armant les 2 pompes simultanément avec `mqttCfg.stabilizationDelayMin`. La nouvelle surcharge `armStabilizationTimer(int pumpIndex)` est utilisée uniquement par les calibrations EZO (durées différenciées 5 min pH / 3 min ORP).
- **pH 3 décimales** sur tous les contrats publics (HTTP `/data`, WS `sensor_data`, MQTT `{base}/ph`) — l'EZO rend 3 décimales fiables, autant les exposer.
- **ORP plage 0..1000 mV** côté `POST /calibrate_orp` (élargie post code-review depuis la suggestion initiale 200..1000) pour couvrir des cas exotiques (kits étalonnés à 0 mV pour test, etc.).
- **Numéro ADR** : ADR-**0014** (et non 0013 comme initialement prévu — 0013 a été pris par feature-020 pour l'identification des sondes OneWire, livrée juste avant).

### Tests manuels recommandés

Voir rapport `test-engineer` (priorités haute 1-6 sur hardware réel) :
1. Boot avec EZO branchés → log version EZO via `I` (D1)
2. Calibration pH 2 points avec tampons certifiés (D2)
3. Calibration ORP 1 point (D3)
4. Filtration ON/OFF — variation pH/ORP < 0.05 / < 10 mV (D4 — **critère AC3 critique**)
5. Compensation T° — variation T° eau → ajustement pH (D5)
6. Résilience EZO débranché en runtime — non-crash + lecture conservée (D6)

### Risques résiduels gérables

- **Flash 98.8 %** (marge ~17 KB) : point d'attention pour les futures features. Toute nouvelle lib ou route doit être pesée contre ce budget.
- **Comportement EZO froid au boot non testé sur cycle long** : risque mineur d'un EZO qui répond `255 (no data)` pendant les premières lectures — atténué par `kEzoBusFailMaxConsecutive = 2` qui tolère un échec isolé.
- **Indistinction mid/low quand `phCalPoints == 1`** : le badge UI ambré « calibration incomplète » ne sait pas dire lequel des deux points a été calibré (l'EZO ne renvoie qu'un compteur via `Cal,?`). Acceptable — l'utilisateur sait normalement quel point il vient d'effectuer.
