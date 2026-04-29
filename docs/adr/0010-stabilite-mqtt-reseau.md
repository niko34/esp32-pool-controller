# ADR-0010 — Stabilité MQTT et réseau : WiFi sans power save, pré-résolution DNS, backoff non réinitialisé

- **Statut** : Accepté (l'alternative « Tâche FreeRTOS dédiée pour MQTT » écartée ici a été reprise et retenue par [ADR-0011](0011-mqtt-task-dediee.md) après 3 crashes production confirmant son nécessité)
- **Date** : 2026-04-27
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : aucune (correctif de stabilité)

> **Note 2026-04-27 (post-publication)** : les mesures D1–D5 ci-dessous restent valides
> et appliquées (WiFi sans power save, pré-résolution DNS, `setSocketTimeout(2)`,
> suppression `requestReconnect()` périodique, backoff exponentiel jusqu'à 120 s).
> Elles couvrent le **connect TCP** et le **DNS**, mais pas `lwip_write` qui peut
> bloquer indéfiniment sur un CPL bruyant. Trois crashes production (publication
> de 33 octets bloquant > 30 s) ont confirmé que la borne « ~7 s par opération »
> évoquée plus bas n'est plus vraie. **L'alternative « Tâche FreeRTOS dédiée »
> écartée à la section « Alternatives considérées » est désormais retenue par
> [ADR-0011](0011-mqtt-task-dediee.md).** Cet ADR-0010 reste la référence pour
> les fixes synchrones (qui sont préservés intégralement dans la nouvelle tâche
> dédiée) — il n'est pas annulé, il est complété.

## Contexte

Deux symptômes observés en production sur l'installation cible :

1. **Erreurs `AbortError: Fetch is aborted`** dans la console navigateur — les requêtes `/data` (timeout 10 s) expiraient régulièrement.
2. **Déconnexions MQTT toutes les 1 à 2 heures**, avec dans les logs :
   - `WARN: Échec publication MQTT`
   - `Tentative connexion MQTT (délai=5s)...` répétés.
3. **Crash `PANIC IntegerDivideByZero`** une fois, avec `loopTask` bloqué dans `WiFiClient::connect → lwip_select` → watchdog 30 s déclenché.
4. **Latence ping** anormalement élevée : 90–260 ms sur LAN local (attendu < 10 ms).

L'analyse a identifié quatre causes coexistantes :

1. **WiFi power save activé par défaut** (`WIFI_PS_MIN_MODEM`) — la radio dort entre les beacons DTIM (cycle ~100–300 ms). C'est exactement la signature de la latence observée. Au-delà de la latence, le buffer paquets de l'AP finit par déborder pendant les phases de sommeil → pertes TCP → la session MQTT meurt après 1–2 h.
2. **Bug d'auto-reconnexion** dans [`main.cpp`](../../src/main.cpp) : `mqttManager.requestReconnect()` était appelé périodiquement (toutes les minutes) en plus de `MqttManager::update()`. Cet appel réinitialisait le backoff exponentiel à 5 s, empêchant le délai de monter à 120 s. Sur un broker injoignable, cela générait une tentative toutes les 5 s en permanence. Le check était de plus **redondant** : `MqttManager::update()` gère déjà sa propre reconnexion autonome avec rate-limit interne.
3. **`WiFiClient::connect(hostname, port)` est doublement bloquant** : il enchaîne DNS + TCP dans le même appel synchrone. Le timeout DNS lwip (~5 s, plus si retry) s'ajoute au timeout TCP. Sur broker injoignable + DNS lent, l'appel pouvait dépasser 30 s → WDT panic.
4. **`mqtt.setSocketTimeout(5)`** — chaque tentative TCP ratée gelait la loop principale 5 s. Avec la fréquence accrue causée par le bug #2, le cumul devenait problématique.

## Décision

Cinq changements coordonnés appliqués dans la même intervention :

### D1 — Désactivation du WiFi power save

Dans [`setupWiFi()`](../../src/main.cpp:289) :

```cpp
WiFi.setSleep(false);
```

Le surcoût énergétique (~30 mA ; 50 mA → 80 mA total) est acceptable : le contrôleur est alimenté en permanence (alimentation secteur dans le local technique).

### D2 — `setSocketTimeout` réduit à 2 secondes

Dans [`mqtt_manager.cpp`](../../src/mqtt_manager.cpp:23) :

```cpp
mqtt.setSocketTimeout(2);  // était 5
```

Plafonne à 2 s le gel de la loop par tentative TCP ratée.

### D3 — Pré-résolution DNS explicite avant `connect()`

Dans [`MqttManager::connect()`](../../src/mqtt_manager.cpp:112) :

```cpp
IPAddress brokerIp;
if (!WiFi.hostByName(mqttCfg.server.c_str(), brokerIp)) {
  // backoff exponentiel + log + return false
}
mqtt.setServer(brokerIp, mqttCfg.port);
esp_task_wdt_reset();  // juste avant le connect bloquant
mqtt.connect(...);
```

Le DNS est ainsi tenté **séparément** (max ~5 s lwip), suivi du TCP (max 2 s grâce à D2). Total worst-case ~7 s, bien sous le watchdog de 30 s. Un `esp_task_wdt_reset()` est posé juste avant l'appel TCP bloquant pour donner toute la fenêtre WDT à cette unique opération.

### D4 — Suppression du `requestReconnect()` périodique dans `main.cpp`

L'appel a été retiré ; un commentaire explicite ([`main.cpp:523`](../../src/main.cpp:523)) interdit sa réintroduction. La reconnexion MQTT est désormais **100 %** gérée par `MqttManager::update()` avec son rate-limit interne et son backoff.

### D5 — Backoff exponentiel jusqu'à 120 s effectif

Le backoff (`5 s → 10 s → 20 s → 40 s → 80 s → 120 s` plafond) existait déjà dans `MqttManager` mais n'était jamais effectif à cause de D4 (réinitialisé toutes les minutes). Aucun changement de code, mais la décision est consignée car le comportement observable change : sur broker injoignable longue durée, les tentatives s'espacent au lieu de marteler.

## Alternatives considérées

- **Tâche FreeRTOS dédiée pour MQTT** (~~rejetée, à reconsidérer~~ → **retenue par [ADR-0011](0011-mqtt-task-dediee.md)** *(Superseded for MQTT decoupling by ADR-0011, 2026-04-27)*) — déplacer toute la logique MQTT dans une task séparée du loopTask éliminerait totalement les blocages réseau de la loop principale. Refactor non trivial (mutex sur les structures partagées). Les fixes D1–D5 ramènent les blocages sous 7 s pour le connect TCP, mais **ne couvrent pas `lwip_write`** qui peut bloquer indéfiniment sur réseau lossy (CPL bruyant en particulier). Trois crashes production avec publication de 33 octets bloquant > 30 s ont confirmé que la borne « 7 s en pratique » n'était valide qu'en LAN propre — voir ADR-0011 pour la décision retenue.
- **Garder le power save WiFi** (rejetée) — la signature de latence (90–260 ms) et les déconnexions périodiques étaient directement causées par le DTIM. Aucun autre fix ne corrigeait ces deux symptômes.
- **Augmenter le watchdog à 60 s** (rejetée) — masque le problème au lieu de le corriger, et dégrade la détection de vraies boucles infinies.
- **Garder le `requestReconnect()` périodique** (rejetée) — il était redondant et nuisible : annulait le backoff au lieu de le respecter.

## Conséquences

### Positives

- **Latence LAN** revenue sous 10 ms (de 90–260 ms).
- **Session MQTT stable** sur la durée : disparition des coupures toutes les 1–2 h.
- **Loop principale** jamais bloquée plus de ~7 s par opération réseau (DNS + TCP cumulés).
- **WDT 30 s respecté** dans tous les cas observés depuis correctif.
- **Web UI** redevenue réactive : plus d'`AbortError` sur `/data`.
- **Tentatives sur broker injoignable** : espacées jusqu'à 120 s — moins de bruit dans les logs et moins de churn TCP.

### Négatives / dette assumée

- **Surcoût énergie** : +30 mA en moyenne (~60 % de plus). Bloquant pour une éventuelle version batterie ou solaire.
- **DNS et MQTT restent synchrones** dans le loopTask. Pas de garantie sub-seconde sur la régulation pH/ORP **pendant** une tentative de connexion (max ~7 s d'arrêt). Mitigation actuelle : la régulation utilise des cadences de plusieurs minutes, l'impact est invisible.
- **Couplage Arduino-ESP32** : si la lib `WiFi` retire `setSleep(false)` ou change sa sémantique, il faudra adapter.

### Ce que ça verrouille

- **Mode batterie impossible** sans revisiter D1 (et donc accepter à nouveau les déconnexions, ou faire D6 = task FreeRTOS dédiée).
- **Tout futur appel réseau bloquant** ajouté au firmware (HTTP client sortant, second broker, webhook…) doit suivre le pattern de D3 : DNS séparé via `WiFi.hostByName()` + `esp_task_wdt_reset()` juste avant l'appel TCP. Documenter cette règle dans `docs/subsystems/mqtt-manager.md` et la rappeler en revue de code.
- **Le check `requestReconnect()` périodique supprimé en D4 ne doit pas être réintroduit.** `MqttManager` est seul propriétaire de son cycle de vie : toute logique de reconnexion appartient à son `update()`.

## Références

- Code : [`src/main.cpp`](../../src/main.cpp) — `setupWiFi()` ligne 289, suppression `requestReconnect()` ligne 523
- Code : [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — `setSocketTimeout(2)` ligne 23, pré-résolution DNS et `esp_task_wdt_reset()` lignes 112–121
- Doc : [`docs/subsystems/mqtt-manager.md`](../subsystems/mqtt-manager.md)
- CHANGELOG `[Unreleased]` 2026-04-27 — section Firmware « Stabilité réseau »
