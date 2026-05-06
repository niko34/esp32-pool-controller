# ADR-0013 — Identification des sondes DS18B20 par adresse ROM persistée NVS

- **Statut** : Accepté
- **Date** : 2026-05-06
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : [`feature-020-deux-sondes-temperature`](../../specs/features/done/feature-020-deux-sondes-temperature.md), [`feature-019-gpio-pcb-v2`](../../specs/features/done/feature-019-gpio-pcb-v2.md) (mapping GPIO PCB v2 — fournit le bus OneWire `kTempSensorPin = 5`), feature-021 (Atlas EZO pH/ORP — consommateur direct de `getWaterTemperature()` pour la compensation pH)

## Contexte

Le **PCB v2** ajoute une **2ᵉ sonde DS18B20** sur le même bus OneWire (GPIO 5) que la sonde existante :

1. **Sonde « eau »** — connecteur externe, mesure la température de l'eau de la piscine (déjà présente en PCB v1).
2. **Sonde « circuit »** — composant CMS soudé directement sur le PCB v2, mesure la température interne du boîtier (régulateurs, MCU). Permet de surveiller la chauffe en été et de remonter une alarme de surchauffe à terme.

**Problème :** les 2 sondes partagent le même bus 1-Wire. Chaque DS18B20 possède une **adresse ROM 64 bits unique** (gravée à la fabrication), mais :

- Les adresses ne peuvent **pas être devinées** à l'avance — chaque PCB v2 produit a un couple d'adresses différent.
- L'**ordre de découverte** par la lib `OneWire`/`DallasTemperature` est stable entre 2 boots du **même** PCB, mais **différent entre 2 PCB**. On ne peut donc pas hardcoder « index 0 = eau, index 1 = circuit ».
- Sans mécanisme d'identification, le firmware ne sait pas quelle sonde mesure quoi. Conséquence directe : la **compensation T° du pH** (feature-021) consommerait potentiellement la T° du **circuit** (chauffé par les régulateurs en été) au lieu de la T° de l'**eau**, introduisant une erreur de -0.05 à -0.10 pH non détectable par l'utilisateur.

Il faut donc trancher **comment associer chaque adresse ROM à un rôle métier** (eau ou circuit), de manière **stable entre reboots** et **sans intervention au boot** une fois la première identification faite.

## Décision

L'identification des sondes DS18B20 se fait par **adresse ROM (8 octets) stockée en NVS**, complétée par un **workflow utilisateur explicite** déclenché depuis l'UI :

1. Au boot, `SensorManager::begin()` scanne le bus OneWire et récupère les adresses ROM des sondes présentes.
2. Les adresses sont comparées aux 2 valeurs lues en NVS (clés `ow_water_addr` et `ow_circuit_addr`, 8 octets chacune — voir `kNvsKeyOwWaterAddr` / `kNvsKeyOwCircuitAddr` dans [`src/constants.h`](../../src/constants.h)).
3. Si une adresse NVS correspond à une adresse détectée → la sonde est marquée avec son rôle (`SondeRole::Water` ou `SondeRole::Circuit`).
4. Sinon → la sonde reste `SondeRole::Unknown` jusqu'à intervention de l'utilisateur.

**Workflow utilisateur** (carte « Identification des sondes de température » dans Paramètres → Avancé) :

1. L'utilisateur ouvre la page, voit les 2 adresses ROM détectées et leurs T° brutes en temps réel (polling 2 s).
2. Il **chauffe une sonde dans la main** pendant 30 s.
3. Il observe laquelle des 2 lignes voit sa T° monter et clique le bouton « C'est l'eau de la piscine » ou « C'est le circuit » sur la bonne ligne.
4. La 2ᵉ sonde est **automatiquement** identifiée comme l'autre rôle.
5. Les 2 adresses sont persistées en NVS — l'identification survit aux reboots et OTA.

**Auto-permutation activée :** si l'utilisateur identifie la sonde A en `water` alors que la sonde B portait déjà ce rôle, la sonde B bascule automatiquement en `circuit`. Évite à l'utilisateur de devoir d'abord faire un reset s'il s'est trompé sur la 1ʳᵉ identification.

**Fallback gracieux activé :** tant que l'identification n'est pas faite (NVS vide ou adresse changée après remplacement physique), `SensorManager::getTemperature()` (alias de `getWaterTemperature()`) retourne la **T° de la 1ʳᵉ sonde détectée**. Le système reste fonctionnel pour la régulation de base, au prix d'une compensation pH potentiellement imprécise jusqu'à identification.

## Alternatives considérées

- **Convention « sonde plus chaude = circuit, plus froide = eau »** (rejetée) — peu fiable. En hiver, l'eau de la piscine peut être plus froide que le circuit (différence claire). Mais en été quand l'eau atteint 28 °C et que le boîtier est ventilé, la différence devient < 2 °C, voire **inversée** si la pompe vient de tourner. Impossible de distinguer de manière déterministe.
- **Index ordre de scan OneWire « index 0 = eau, index 1 = circuit »** (rejetée) — l'ordre est stable entre 2 boots du **même** PCB, mais **différent entre PCB**. Casserait l'interchangeabilité firmware. Et un remplacement de sonde change l'ordre.
- **QR code / identification usine** sur chaque sonde (rejetée) — impossible côté firmware seul. Nécessiterait un workflow industriel d'appairage à la fabrication des PCB, hors périmètre projet personnel.
- **Refus strict en cas de conflit de rôle** (rejetée) — alternative à l'auto-permutation. Concrètement : si l'utilisateur tente d'identifier A=eau alors que B=eau, on retourne 409 Conflict et on demande de reset d'abord. Casserait le workflow UX (« j'ai cliqué sur la mauvaise ligne, je veux corriger ») et obligerait à ajouter un bouton « inverser » côté UI. L'auto-permutation est plus fluide et n'introduit aucun risque (les 2 rôles sont symétriques côté firmware).
- **Stocker les adresses dans le `config.json` LittleFS** (rejetée) — moins fiable que NVS pour des données binaires courtes (8 octets × 2). NVS est conçu pour ce type de stockage clé-valeur, atomique, et n'est pas effacé par un OTA filesystem. Les autres données binaires courtes (`token`, `password_hash`) sont déjà stockées en NVS, on garde la cohérence.

## Conséquences

### Positives

- **Workflow utilisateur clair, fluide, indépendant de l'environnement** (pas de dépendance saisonnière, pas de calibration). Documenté dans [`docs/features/page-settings.md`](../features/page-settings.md).
- **Stable entre reboots et OTA** — les clés NVS survivent à un OTA firmware ou filesystem (NVS vit dans une partition séparée).
- **Rétrocompatibilité préservée** : `SensorManager::getTemperature()` reste un alias de `getWaterTemperature()` avec fallback gracieux. Les consommateurs existants (`web_routes_data`, `mqtt_manager`, `history`, `ws_manager`) continuent de fonctionner sans modification de leur logique de lecture.
- **Pattern réutilisable** pour ajouts futurs (3ᵉ sonde par exemple) : le tableau `_sondes[kMaxDs18b20Sondes]` est dimensionné par `kMaxDs18b20Sondes = 2` aujourd'hui mais l'algorithme de scan/identification est générique.
- **Auto-permutation** rend le workflow tolérant aux erreurs utilisateur (clic sur la mauvaise ligne) sans bouton supplémentaire.

### Négatives / dette assumée

- **Action manuelle requise après chaque changement physique** d'une sonde (remplacement, inversion des câbles). Le firmware détecte qu'une adresse NVS ne correspond plus à une sonde présente et log un warning, mais ne peut pas re-identifier seul. L'utilisateur doit refaire le workflow.
- **Si l'utilisateur inverse les câbles physiquement** (eau ↔ circuit) sans refaire l'identification, la T° eau lira la valeur du circuit. Pas de détection automatique possible — c'est la limite intrinsèque de l'identification par adresse ROM (l'adresse ne sait pas où elle est physiquement câblée).
- **Découvrabilité** : l'utilisateur doit savoir qu'il faut aller dans Paramètres → Avancé après le 1ᵉʳ flash. Mitigé par la **chip ambrée** sur le Dashboard (`#sondes-chip`) tant que l'identification n'est pas terminée — pattern réutilisé de `#calib-chip`.

### Ce que ça verrouille

- **feature-021 (Atlas EZO)** consomme `getWaterTemperature()` pour la compensation pH. La sémantique « la valeur retournée est bien la T° de l'eau » est garantie par cette ADR (et par le fallback gracieux qui retourne la 1ʳᵉ sonde si NVS vide).
- **L'ordre `pumps[0]` = pH / `pumps[1]` = ORP** d'[ADR-0012](0012-mapping-gpio-pcb-v2.md) reste indépendant de cette ADR — ne pas confondre.
- **Bus OneWire mono-appelant** : `SensorManager` est aujourd'hui le seul consommateur de `OneWire`/`DallasTemperature`. Si un futur composant (UART, écran, alarme) doit lire le bus en parallèle, un mutex devra être ajouté (pas requis aujourd'hui, mais documenté dans [`docs/subsystems/sensors.md`](../subsystems/sensors.md#contrat-mono-appelant-du-bus-onewire)).
- **`tempCalibrationOffset` ne s'applique qu'à la T° eau** — la T° circuit est exposée brute (pas de calibration utilisateur). Cohérent : la T° circuit est un indicateur diagnostique, pas une mesure de référence.
- **Format des clés NVS** (`ow_water_addr` / `ow_circuit_addr`, 8 octets binaires) figé. Toute migration future devra prévoir un mécanisme de read+rewrite si le format change.

## Références

- Code : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — `enum SondeRole`, `struct SondeInfo`, méthodes publiques `getWaterTemperature`, `getCircuitTemperature`, `areSondesIdentified`, `getDetectedSondeCount`, `getDetectedSondeAddresses`, `identifySonde`, `resetSondeIdentification`
- Code : [`src/constants.h`](../../src/constants.h) — `kMaxDs18b20Sondes`, `kSondeAddrLen`, `kNvsKeyOwWaterAddr`, `kNvsKeyOwCircuitAddr`
- Code : [`src/web_routes_sensor_id.cpp`](../../src/web_routes_sensor_id.cpp), [`src/web_routes_sensor_id.h`](../../src/web_routes_sensor_id.h) — endpoints `/sensors/onewire/{scan,identify,reset}`
- Code : [`src/web_helpers.cpp`](../../src/web_helpers.cpp) — `formatRomHex()`, `parseRomHex()`
- Code : [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — exposition WS/HTTP des champs `temperature_circuit`, `sondes_identified`, `sondes_detected`
- Code : [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — topic `pool/sensors/temperature_circuit` + auto-discovery HA « Piscine Température Circuit »
- Spec : [`specs/features/done/feature-020-deux-sondes-temperature.md`](../../specs/features/done/feature-020-deux-sondes-temperature.md)
- Doc subsystem : [`docs/subsystems/sensors.md`](../subsystems/sensors.md), [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md), [`docs/subsystems/mqtt-manager.md`](../subsystems/mqtt-manager.md)
- Doc transverse : [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md), [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md)
- Doc page : [`docs/features/page-settings.md`](../features/page-settings.md)
- ADR lié : [ADR-0012](0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2 (bus OneWire conservé sur GPIO 5, support 2 sondes simultanées)
