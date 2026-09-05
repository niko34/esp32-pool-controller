## Métadonnées
- **ID** : feature-020
- **Slug** : deux-sondes-temperature
- **Type** : feature
- **Statut** : done
- **Priorité** : haute (préliminaire à feature-021 Atlas EZO — la compensation T° du pH a besoin de savoir laquelle des 2 sondes est l'eau de la piscine)
- **Version cible** : 2.0.0
- **Dernière mise à jour** : 2026-05-06 (feature livrée — ADR-0013 créé, build SUCCESS, tests dynamiques sur PCB v2 réel délégués à l'humain)

## Travail réalisé (2026-05-06)

**Firmware** (10 fichiers) :
- `src/constants.h` : `kMaxDs18b20Sondes`, `kSondeAddrLen`, `kNvsKeyOwWaterAddr`, `kNvsKeyOwCircuitAddr`
- `src/sensors.h/cpp` : `enum SondeRole`, `struct SondeInfo`, scan multi-sondes via `getAddress()`, lecture par adresse, fallback gracieux 1ʳᵉ sonde si non identifié, **auto-permutation rôles** (sensors.cpp:828-838), `getTemperature()` reste alias rétrocompat
- `src/web_helpers.h/cpp` : `formatRomHex()`, `parseRomHex()` (validation hex stricte)
- `src/web_routes_sensor_id.h/cpp` (NOUVEAU) : 3 endpoints auth — `GET /sensors/onewire/scan`, `POST /sensors/onewire/identify`, `POST /sensors/onewire/reset`
- `src/web_server.cpp` : enregistrement des routes
- `src/ws_manager.cpp` : 3 nouveaux champs `sensor_data` (`temperature_circuit`, `sondes_identified`, `sondes_detected`), buffer 832→896
- `src/web_routes_data.cpp` : mêmes champs sur `/data`
- `src/mqtt_manager.h/cpp` : topic `pool/sensors/temperature_circuit`, publish dans `publishAllStatesInternal()`, payload diagnostic enrichi, auto-discovery HA "Piscine Température Circuit"

**Frontend** (3 fichiers) :
- `data/index.html` : chip `#sondes-chip` après `#calib-chip` + card `#card-sensors-onewire` dans `#panel-dev`
- `data/app.js` : `bindSondesOnewire`, polling 2s scoped au panel-dev, hook `_updateSondesChip`
- `data/app.css` : 3 sélecteurs `[hidden]` minimaux (élements `display:inline-flex`)

**Décisions clés** : auto-permutation activée (workflow UX fluide à un clic), fallback gracieux préservé (`getTemperature()` retourne T° 1ʳᵉ sonde si non identifié — pas de breaking).

**Documentation** :
- ADR-0013 créé (`docs/adr/0013-identification-sondes-onewire.md`) avec alternatives écartées
- `docs/adr/README.md` : index ADR-0013 ajouté
- `docs/subsystems/sensors.md` : section multi-sondes complète + contrat mono-appelant OneWire
- `docs/subsystems/ws-manager.md` : 3 nouveaux champs `sensor_data`
- `docs/MQTT.md` : nouveau topic + entité HA
- `docs/API.md` : 3 nouveaux endpoints détaillés
- `docs/features/page-settings.md` : workflow card + chip Dashboard
- `docs/UPDATE_GUIDE.md` : note migration v2.0.0
- `CHANGELOG.md` : section `[Unreleased] - 2026-05-06`

**Build** :
- `pio run` SUCCESS, RAM 16.5 % (53944 / 327680), **Flash 98.5 % (1420377 / 1441792 — marge ~21 KB, alerte pour feature-021)**, 0 nouveau warning
- `./build_fs.sh` SUCCESS, partition LittleFS 1088 KB OK

**Tests dynamiques restants (délégués à l'humain sur PCB v2 réel)** :
- Boot avec 2 sondes branchées, NVS vide → détection 2 sondes, statut "0/2 identifiées", chip Dashboard visible
- Workflow d'identification : chauffer sonde dans la main → cliquer bon bouton → 2ᵉ sonde auto-identifiée
- Persistance NVS au reboot
- MQTT côté HA : entité "Piscine Température Circuit" reçoit valeurs cohérentes
- Sonde changée à chaud : log warning, identification de l'autre sonde conservée
- Polling 2s sur card panel-dev : arrêt auto sur navigation


- **Spec(s) liée(s)** : feature-019 (mapping GPIO PCB v2 — fournit le pin OneWire `kTempSensorPin = 5` partagé par les 2 sondes), feature-021 (Atlas EZO pH/ORP — consommateur direct de `getWaterTemperature()` pour la compensation pH côté EZO pH)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — ajout d'une 2ᵉ sonde DS18B20 sur le bus OneWire
- [ ] Régulation (pump_controller, PID) — non modifié (le PID consomme pH/ORP, pas T°)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (nouveau champ `temperature_circuit`, route d'identification des sondes)
- [x] MQTT / Home Assistant (nouveau topic + entité auto-discovery pour la T° circuit)
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (NVS : 2 nouvelles clés pour les adresses 1-Wire « eau » et « circuit »)
- [x] Frontend (`data/`) — nouvelle UI d'identification des sondes (workflow utilisateur)

## Agents requis
- [x] spec-reader
- [x] architect — arbitre la stratégie d'identification (par adresse 1-Wire stockée NVS via workflow utilisateur, OU par convention « plus chaude = circuit »)
- [ ] pool-chemistry — non applicable (la T° n'est pas un produit chimique ; la compensation pH consomme la T° eau mais ne dose rien directement). Le **rôle de la T° eau dans feature-021** sera validé par pool-chemistry à ce moment-là.
- [x] ux-ui-designer — workflow d'identification des 2 sondes : c'est une nouvelle surface UI qui demande une réflexion (chauffer manuellement une sonde et l'identifier via clic UI ?)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le PCB v2 ajoute une **2ᵉ sonde DS18B20** sur le bus OneWire (GPIO 5, partagé avec la sonde existante). Une sonde mesure la **température de l'eau de la piscine**, l'autre est **soudée sur le circuit** pour mesurer la température interne de l'électronique (boîtier).

Cette spec ajoute la lecture des 2 sondes en parallèle, leur **identification univoque** (laquelle est l'eau, laquelle est le circuit), la persistance de cette identification en NVS, et l'exposition de la T° circuit via WS + MQTT/HA. La T° eau reste exposée comme aujourd'hui (champ `temperature` inchangé pour la rétrocompatibilité utilisateurs et HA).

L'identification est nécessaire car les **adresses 1-Wire** des 2 sondes sont uniques à la fabrication (64 bits ROM), donc elles ne peuvent pas être devinées à l'avance, et **l'ordre de découverte** par la lib OneWire/DallasTemperature **n'est pas garanti** entre 2 boots.

## Contexte

Le PCB v2 a 2 emplacements DS18B20 :
1. **DS18B20 « eau »** : connecteur externe vers la sonde plongée dans la piscine (déjà présente en v1)
2. **DS18B20 « circuit »** : sonde CMS soudée directement sur le PCB v2, à proximité des composants critiques (régulateurs, MCU). Permet de surveiller la T° interne du boîtier (hausse en été, alarme si surchauffe).

Sans identification :
- Au boot, le firmware découvre 2 sondes mais ne sait pas laquelle est laquelle
- Si on prend juste l'index 0 et 1, l'ordre dépend de la lib OneWire (basé sur les adresses ROM, **stable** entre 2 boots du même hardware mais **différent entre 2 PCB v2** — chaque PCB a un couple d'adresses différent à cause de la fabrication des sondes)
- La compensation T° du pH (feature-021) a besoin de la T° **eau**, pas circuit. Si on se trompe, la compensation est fausse et la régulation pH dérive.

L'identification doit être faite **une fois par PCB** par l'utilisateur (workflow de configuration initiale ou via Paramètres → Avancé → Capteurs).

## Comportement attendu

### Côté firmware

#### Scan au boot

Au boot dans `Sensors::begin()` :
- `OneWire`/`DallasTemperature` détecte les sondes présentes sur GPIO 5
- Lecture des adresses ROM (`getAddress(addr, i)` pour chaque index)
- Comparaison avec les adresses stockées en NVS (clés `ow_water_addr` et `ow_circuit_addr`, 8 octets chacune)
- Si les 2 adresses NVS correspondent à 2 sondes détectées → tout est mappé
- Si une adresse NVS ne correspond pas (sonde changée ?) → log warning, l'identification doit être refaite par l'utilisateur
- Si aucune adresse NVS (premier boot après update) → toutes les sondes sont marquées « non identifiées », régulation T° eau utilise `temperature` par défaut **= 25 °C** (valeur de référence sécuritaire, pas de NaN qui casserait la compensation pH)

Logger au boot le nombre de sondes détectées, leurs adresses ROM, et le statut d'identification :
```
INFO: OneWire: 2 sondes DS18B20 détectées
INFO:   - sonde[0] = 28FF1234... (identifiée: eau piscine)
INFO:   - sonde[1] = 28FF5678... (identifiée: circuit)
```

#### Lecture cyclique

Toutes les `kTempSensorIntervalMs` :
- `requestTemperatures()` (lance la conversion sur les 2 sondes en parallèle)
- Lecture des 2 températures par adresse (et non par index — par sécurité)
- Stockage dans `Sensors::_waterTemp` et `Sensors::_circuitTemp`

#### API publique

Nouveau dans `src/sensors.h` :
- `float Sensors::getWaterTemperature()` — T° de l'eau de la piscine (NaN si sonde non identifiée ou en erreur)
- `float Sensors::getCircuitTemperature()` — T° du circuit (NaN si sonde non identifiée ou en erreur)
- `bool Sensors::areSondesIdentified()` — true si les 2 adresses sont connues et matchées
- `int Sensors::getDetectedSondeCount()` — nombre de sondes physiquement détectées (0, 1 ou 2)
- `void Sensors::getDetectedSondeAddresses(uint8_t addrs[2][8], bool* matched)` — pour exposer à l'UI les adresses brutes et leur statut d'identification
- `bool Sensors::identifySonde(const uint8_t addr[8], bool isWater)` — assigne une adresse à « eau » ou « circuit » (et stocke en NVS)
- `void Sensors::resetSondeIdentification()` — efface les NVS et oblige à refaire le workflow

`Sensors::getTemperature()` existant **inchangé** : alias de `getWaterTemperature()` pour la rétrocompatibilité.

#### Gestion absence de sonde / lecture en erreur

- 0 sonde détectée → log error, `getWaterTemperature()` et `getCircuitTemperature()` retournent NaN, régulation chimique inhibée (déjà géré par `Sensors::isInitialized()`)
- 1 sonde détectée et identifiée → l'autre retourne NaN, régulation continue (la compensation pH utilise une valeur par défaut 25 °C si NaN — décision pool-chemistry à confirmer en feature-021)
- 1 sonde détectée non identifiée → workflow utilisateur requis pour la marquer « eau » ou « circuit »
- Sonde déconnectée en runtime → CRC erreur, retourner NaN sur cette lecture, retry au tick suivant

### Côté UI

Nouvelle page ou section dans Paramètres → Avancé : **« Identification des sondes de température »**.

Affiche :
- Nombre de sondes détectées
- Adresse ROM de chaque sonde (formatée hex)
- T° brute lue par chaque sonde (mise à jour temps réel)
- Pour chaque sonde non identifiée : 2 boutons « C'est l'eau de la piscine » + « C'est le circuit »
- Bouton « Réinitialiser l'identification » (efface NVS, repart à zéro)
- Indicateur de statut : « 2/2 sondes identifiées » ou « Identification incomplète »

Workflow utilisateur recommandé :
1. Boot le firmware sur PCB v2 avec les 2 sondes branchées
2. Aller dans la page Identification des sondes
3. **Chauffer manuellement** une des 2 sondes (par ex. la tenir dans la main 30 s)
4. Observer laquelle voit sa T° monter
5. Cliquer le bon bouton pour celle qui chauffe (« C'est ... » selon contexte)
6. La 2ᵉ sonde est automatiquement identifiée comme l'autre rôle
7. Confirmation UI : « 2/2 sondes identifiées »

### Côté MQTT/Home Assistant

- Topic existant `pool/sensors/temperature` (eau) → **inchangé**, payload identique (rétrocompat HA)
- **Nouveau topic** `pool/sensors/temperature_circuit` (1 décimale)
- **Nouvelle entité auto-discovery HA** : `sensor` "Piscine Température Circuit" avec `device_class: temperature`, unit °C
- Diagnostic MQTT : ajout du champ `temperature_circuit` dans le payload `diagnostic`

### Côté WS

- Champ existant `temperature` (eau) → **inchangé**
- **Nouveau champ** `temperature_circuit` dans `sensor_data` (poussé toutes les 5 s)

### Cas limites

- **Une sonde change physiquement** (utilisateur la remplace) : son adresse ROM change → ne matche plus en NVS → log warning « Sonde X non détectée — son adresse a peut-être changé. Refaire l'identification ». L'identification existante de l'autre sonde est conservée.
- **Sonde « eau » identifiée mais physiquement remplacée par celle « circuit »** (utilisateur inverse les câbles) : la T° eau lira la valeur du circuit. Pas de détection automatique possible — c'est l'utilisateur qui doit refaire l'identification s'il a inversé les câbles.
- **3 sondes ou plus détectées** : prend en compte uniquement les 2 premières en termes d'identification, log warning « Trop de sondes détectées (N) — seules 2 supportées ».
- **Première utilisation sans identification** : `getWaterTemperature()` retourne NaN → la compensation pH (feature-021) utilise 25 °C par défaut (sécuritaire, pas d'erreur > 0.1 pH dans la plage 15-30 °C).

## Comportement actuel

- 1 seule sonde DS18B20 lue sur GPIO 5 via `OneWire`/`DallasTemperature`
- `Sensors::getTemperature()` retourne la T° de la sonde unique (eau de la piscine implicite)
- Topic MQTT `pool/sensors/temperature` exposé à HA
- Aucune notion d'adressage ROM ni d'identification

## Contraintes techniques

- ESP32 (esp32dev), framework Arduino + PlatformIO
- Vanilla JS dans `data/` (pas de framework)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB
- AsyncWebServer : aucun handler bloquant > 50 ms (la lecture des sondes peut prendre ~750 ms en 12-bit, à faire sous mutex et hors handler HTTP)
- NVS : 2 clés nouvelles (`ow_water_addr` et `ow_circuit_addr`, 8 octets chacune)
- OneWire bus partagé entre les 2 sondes : pas de risque de collision si on lit par adresse (`getTempC(addr)`) plutôt que par index (`getTempCByIndex(i)`)
- Watchdog 30 s : la lecture cyclique reste rapide (< 1 s)

## Fichiers potentiellement concernés

- `src/sensors.h` / `src/sensors.cpp` — refonte de la lecture T° (lecture par adresse, gestion NaN, identification)
- `src/constants.h` — éventuelles constantes nouvelles si besoin (ex. clés NVS noms)
- `src/config.h` / `src/config.cpp` — chargement / sauvegarde des 2 adresses NVS (8 octets chacune, `prefs.putBytes` / `getBytes`)
- `src/web_routes_data.cpp` — payload `temperature_circuit` ajouté à `/data` et au push WS `sensor_data`
- `src/web_routes_config.cpp` ou nouveau `src/web_routes_sensor_id.cpp` — endpoints d'identification :
  - `GET /sensors/onewire/scan` — liste les sondes détectées avec leurs adresses brutes et T° lue
  - `POST /sensors/onewire/identify` — payload `{ "address": "28FF...", "role": "water"|"circuit" }`
  - `POST /sensors/onewire/reset` — efface NVS
- `src/ws_manager.cpp` — ajout `temperature_circuit` dans `_buildSensorJson()`
- `src/mqtt_manager.cpp` — nouveau topic `temperature_circuit` + auto-discovery HA correspondant
- `data/index.html` — nouvelle card « Identification des sondes » dans Paramètres → Avancé
- `data/app.js` — handlers fetch + UI workflow d'identification
- `data/app.css` — styles si nécessaire
- `docs/subsystems/sensors.md` — refonte (architecture 2 sondes, identification, NVS)
- `docs/features/page-settings.md` — section Avancé enrichie (nouveau workflow)
- `docs/MQTT.md` — nouveau topic + entité HA
- `docs/API.md` — nouveaux endpoints d'identification
- `docs/UPDATE_GUIDE.md` — note de migration : après update v2.0.0, l'utilisateur doit identifier les 2 sondes
- `CHANGELOG.md` — entrée Hardware/Firmware/UI sous v2.0.0

## Livrables — check-list

- [ ] A1 — `Sensors::begin()` scanne le bus OneWire, log les adresses détectées et leur statut (identifiée / non identifiée)
- [ ] A2 — Stockage NVS des 2 adresses (clés `ow_water_addr`, `ow_circuit_addr`, 8 octets chacune)
- [ ] A3 — `Sensors::update()` lit les T° par adresse (`getTempC(addr)`) — pas par index
- [ ] A4 — API `getWaterTemperature()` et `getCircuitTemperature()` exposées dans `Sensors`
- [ ] A5 — `getTemperature()` reste un alias de `getWaterTemperature()` pour rétrocompat (pas de breaking côté code consommateur)
- [ ] A6 — Endpoints HTTP : `/sensors/onewire/scan`, `/sensors/onewire/identify`, `/sensors/onewire/reset`
- [ ] A7 — Champ `temperature_circuit` dans payload WS `sensor_data` et HTTP `/data`
- [ ] A8 — Topic MQTT `pool/sensors/temperature_circuit` + auto-discovery HA `sensor` correspondant
- [ ] A9 — Si une seule sonde identifiée et que l'autre n'est plus détectée : `getWaterTemperature()` ou `getCircuitTemperature()` correspondante retourne NaN, pas de crash
- [ ] B1 — Nouvelle card « Identification des sondes de température » dans Paramètres → Avancé
- [ ] B2 — Card affiche en temps réel le nombre de sondes, adresses et T° brutes
- [ ] B3 — Boutons « C'est l'eau » / « C'est le circuit » fonctionnels
- [ ] B4 — Bouton « Réinitialiser l'identification » fonctionnel
- [ ] B5 — Indicateur de statut « 2/2 sondes identifiées » visible
- [ ] B6 — Workflow guide : message d'aide « Chauffer une sonde dans la main pour l'identifier »
- [ ] D1 — Test manuel avec 2 sondes branchées : scan → identification eau → identification circuit auto → confirmation 2/2
- [ ] D2 — Test changement physique d'une sonde : log warning, identification de l'autre conservée
- [ ] D3 — Test reboot après identification : adresses NVS rechargées, mapping persistant
- [ ] D4 — `pio run` SUCCESS, RAM/Flash variations négligeables
- [ ] D5 — `./build_fs.sh` SUCCESS
- [ ] D6 — Test MQTT côté HA : `temperature_circuit` apparaît comme nouvelle entité, valeurs cohérentes
- [ ] C1 — `docs/subsystems/sensors.md` refondu (architecture 2 sondes, NVS, lecture par adresse)
- [ ] C2 — `docs/features/page-settings.md` enrichi (workflow identification)
- [ ] C3 — `docs/MQTT.md` enrichi (nouveau topic + entité HA)
- [ ] C4 — `docs/API.md` enrichi (3 nouveaux endpoints)
- [ ] C5 — `docs/UPDATE_GUIDE.md` note de migration
- [ ] C6 — `CHANGELOG.md` entrée sous v2.0.0
- [ ] C7 — ADR éventuel si arbitrage architectural notable (cf. section ADR)

## Nouvelles signatures / API

- Endpoints HTTP nouveaux :
  - `GET /sensors/onewire/scan` (auth) — `{ "sondes": [ { "address": "28FF...", "temperature": 18.5, "role": "water"|"circuit"|"unknown" } ] }`
  - `POST /sensors/onewire/identify` (auth) — `{ "address": "28FF...", "role": "water"|"circuit" }` → `{ "success": true }`
  - `POST /sensors/onewire/reset` (auth) — `{}` → `{ "success": true }`
- Champs WebSocket nouveaux :
  - `temperature_circuit` (float, 1 décimale, dans `sensor_data`)
- Topics MQTT nouveaux :
  - `pool/sensors/temperature_circuit` (retain) — auto-discovery HA `sensor` device_class=temperature
- Fonctions publiques nouvelles dans `src/sensors.h` :
  - `float Sensors::getWaterTemperature();`
  - `float Sensors::getCircuitTemperature();`
  - `bool Sensors::areSondesIdentified();`
  - `int Sensors::getDetectedSondeCount();`
  - `void Sensors::getDetectedSondeAddresses(uint8_t addrs[2][8], bool* matched);`
  - `bool Sensors::identifySonde(const uint8_t addr[8], bool isWater);`
  - `void Sensors::resetSondeIdentification();`
- Constantes nouvelles éventuelles (peu probable, plutôt utiliser des chaînes littérales pour les clés NVS)

## Sécurité chimique

- Dosage impliqué : non directement (cette feature ajoute une sonde de mesure, ne touche pas au dosage)
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non
- Calibration capteur modifiée : non (DS18B20 est calibré usine, pas de calibration utilisateur)
- Algorithme PID touché : non
- Risque identifié : **inversion eau/circuit** par mauvaise identification utilisateur. Conséquence : la compensation pH (feature-021) sera basée sur la T° du circuit (qui peut être chaude en été à cause des composants) au lieu de l'eau (qui est plus froide). Erreur typique : -0.05 à -0.10 pH si circuit 5-10 °C plus chaud que eau. Mitigation : workflow UI clair + valeur par défaut 25 °C si non identifié.

## ADR attendu ?

- **Probable** : oui (faible — décision architecturale plutôt mineure mais structurante)
- **Critère(s) candidat(s)** : 1 (contrainte structurante — choix d'un mécanisme d'identification par adresse + workflow utilisateur), 3 (alternatives crédibles à arbitrer : par adresse + workflow, ou par convention « plus chaude = circuit », ou par index ordre de scan)
- **Sujet pressenti** : `docs/adr/NNNN-identification-sondes-onewire.md` (probablement ADR-0014 si feature-021 prend ADR-0013)

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /sensors/onewire/scan` (nouveau)
- `POST /sensors/onewire/identify` (nouveau)
- `POST /sensors/onewire/reset` (nouveau)

### Champs WebSocket
- `temperature_circuit` (nouveau, dans `sensor_data`)

### Topics MQTT
- `pool/sensors/temperature_circuit` (nouveau) — auto-discovery HA `sensor` Piscine Température Circuit, device_class temperature

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/subsystems/sensors.md` — refonte sur l'architecture 2 sondes (lecture par adresse, NVS, identification)
- `docs/features/page-settings.md` — nouveau workflow Identification

#### Docs transverses
- `docs/MQTT.md` — nouveau topic + entité HA
- `docs/API.md` — 3 nouveaux endpoints
- `docs/UPDATE_GUIDE.md` — note de migration v2.0.0 (workflow identification à faire au premier boot après update)
- `CHANGELOG.md` — entrée Frontend + Firmware + Documentation sous v2.0.0

#### Docs immuables
- `docs/adr/NNNN-identification-sondes-onewire.md` éventuel (ADR-0014 probable)

## Critères d'acceptation

- [ ] AC1 — Au boot avec 2 sondes branchées et NVS vide, le firmware détecte 2 sondes et les expose comme « non identifiées »
- [ ] AC2 — Workflow UI permet d'identifier une sonde en chauffant et cliquant le bouton correspondant — la 2ᵉ sonde est auto-identifiée
- [ ] AC3 — Identifications persistées en NVS, restaurées au reboot
- [ ] AC4 — `getWaterTemperature()` et `getCircuitTemperature()` retournent les bonnes valeurs après identification
- [ ] AC5 — `getTemperature()` reste un alias de `getWaterTemperature()` (pas de breaking pour les consommateurs existants)
- [ ] AC6 — Topic MQTT `temperature_circuit` exposé, entité HA détectée par Home Assistant
- [ ] AC7 — Si une sonde est physiquement remplacée (adresse ROM différente), log warning et l'identification doit être refaite
- [ ] AC8 — `getWaterTemperature()` retourne NaN si la sonde « eau » n'est plus détectée — pas de crash
- [ ] AC9 — Workflow UI fonctionne en français
- [ ] AC10 — `pio run` compile sans nouveau warning
- [ ] AC11 — `./build_fs.sh` SUCCESS (partition 1088 KB OK)

## Plan de test manuel

### Préparation
- PCB v2 avec 2 sondes DS18B20 branchées (eau + circuit)
- NVS vide (factory reset au préalable si besoin)

### Tests fonctionnels
1. **Boot avec 2 sondes** : observer logs, vérifier détection 2 sondes, statut « non identifiées »
2. **Page Identification** : ouvrir, vérifier affichage des 2 adresses ROM + T° brutes
3. **Identification sonde eau** : tenir la sonde de l'eau dans la main 30 s, observer la sonde dont la T° monte, cliquer « C'est l'eau de la piscine » sur la bonne ligne. Vérifier qu'elle passe à « identifiée: eau ». Vérifier que la 2ᵉ sonde est automatiquement identifiée comme circuit.
4. **Persistance** : redémarrer l'ESP32, vérifier que les 2 adresses sont rechargées depuis NVS et le statut « 2/2 identifiées » persiste.
5. **MQTT** : vérifier dans Mosquitto le topic `pool/sensors/temperature_circuit`. Vérifier dans Home Assistant l'entité « Piscine Température Circuit ».
6. **Changement de sonde** : remplacer une sonde par une autre (adresse ROM différente). Boot. Vérifier log warning, statut « identification incomplète ».
7. **Reset identification** : bouton « Réinitialiser », vérifier que NVS est effacé et qu'on revient au statut « non identifiées ».
8. **Une seule sonde branchée** : débrancher l'eau, boot, vérifier que `getWaterTemperature()` retourne NaN, `getCircuitTemperature()` continue de marcher.

## Notes pour agent

- **Lib OneWire/DallasTemperature** : déjà incluse dans le projet (utilisée pour la sonde unique). Pas besoin d'ajouter de lib.
- **Lecture par adresse** : utiliser `sensors.getTempC(addr)` (où `addr` est un `uint8_t[8]`) plutôt que `getTempCByIndex(i)` — l'index n'est pas stable entre boots de PCB différents.
- **Format adresse** : 8 octets, généralement représenté en hex (`28FF1A2B3C4D5E6F`). UI : afficher en hex en majuscules pour lisibilité.
- **Workflow d'identification** : ux-ui-designer doit arbitrer entre plusieurs UX possibles :
  - (a) Chauffer manuellement une sonde et observer (proposé ci-dessus)
  - (b) Comparer T° eau vs T° circuit en supposant que l'eau est plus froide en été (peu fiable, dépend de l'environnement)
  - (c) Hardcoder en usine via QR code ou similaire (impossible côté firmware seul)
  Recommandation : (a) car simple, fiable, et compréhensible par tous.
- **Compensation pH** : la T° eau sera consommée par feature-021 via `Sensors::getWaterTemperature()`. Si NaN (sonde eau non identifiée), feature-021 utilisera 25 °C par défaut.
- **Fallback gracieux** : tant que l'identification n'est pas faite, le firmware fonctionne comme avant (`getTemperature() == getWaterTemperature() == valeur de la 1ère sonde détectée`). Cela permet à un utilisateur de continuer à utiliser le système même sans avoir fait l'identification, au prix d'une compensation pH potentiellement imprécise.
- **Découvrabilité au premier boot (à arbitrer par ux-ui-designer)** : la spec prévoit la card de configuration dans Paramètres → Avancé. Mais comment l'utilisateur sait-il qu'il faut y aller au premier boot (NVS vide) ? Plusieurs options à arbitrer : (a) card dans Paramètres → Avancé seule (minimal — risque de louper) ; (b) chip de notification ambré sur le Dashboard (pattern `.chip` existant comme `#calib-chip`) avec lien vers la card ; (c) wizard au premier boot ; (d) message inline sur la page Température. Recommandation auteur : (b) + (d) combinés, mais ux-ui-designer tranche en cohérence avec les patterns existants du projet.

## Hors périmètre

- Pas de modification de la régulation chimique (feature-021 consomme la T° eau)
- Pas de support de plus de 2 sondes DS18B20 (seules 2 sont supportées : eau + circuit)
- Pas d'alarme de surchauffe sur la T° circuit (peut être ajouté plus tard si besoin — typiquement seuil 60-70 °C)
- Pas de calibration utilisateur des DS18B20 (calibration usine suffisante, ±0.5 °C accuracy)
- Pas de modification du PID
- Pas de refonte de la page Température (l'affichage T° eau actuel reste inchangé)
