# Subsystem — `mqtt_manager`

- **Fichiers** : [`src/mqtt_manager.h`](../../src/mqtt_manager.h), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)
- **Singleton** : `extern MqttManager mqttManager;`
- **Lib** : [PubSubClient v2.8](https://github.com/knolleary/pubsubclient)

## Rôle

Client MQTT avec **auto-discovery Home Assistant**. Publie les états des capteurs, relais, pompes et reçoit les commandes HA (set target pH/ORP, filtration on/off, etc.). Support Last Will & Testament.

## API publique

```cpp
void begin();
void update();                   // appelé dans loop(), reconnexion auto
void connect();
void disconnect();
void requestReconnect();         // force une reconnexion (changement config)
bool isConnected();

void publishSensorState(const String& topic, const String& payload, bool retain = true);
void publishAllStates();
void publishFiltrationState();
void publishLightingState();
void publishDosingState();
void publishProductState();
void publishTargetState();
void publishAlert(const String& alertType, const String& message);
void publishLog(const String& logMessage);
void publishStatus(const String& status);
void publishDiagnostic();
```

## Topics

Structure complète dans [`MqttTopics`](../../src/mqtt_manager.h). Résumé :

```
{base}/temperature
{base}/ph, {base}/ph_target, {base}/ph_target/set, {base}/ph_dosing, {base}/ph_limit
{base}/orp, {base}/orp_target, {base}/orp_target/set, {base}/orp_dosing, {base}/orp_limit
{base}/ph_regulation_mode, {base}/ph_daily_target_ml
{base}/orp_regulation_mode, {base}/orp_daily_target_ml
{base}/ph_remaining_ml, {base}/ph_stock_low
{base}/orp_remaining_ml, {base}/orp_stock_low
{base}/filtration_state, {base}/filtration/set
{base}/filtration_mode, {base}/filtration_mode/set
{base}/lighting_state, {base}/lighting/set
{base}/status                 (LWT : "online" / "offline")
{base}/alerts
{base}/logs
{base}/diagnostic
```

Voir [`docs/MQTT.md`](../MQTT.md) pour la liste exhaustive avec les entités HA correspondantes.

## Auto-discovery Home Assistant

`publishDiscovery()` publie des messages `retain=true` sur `homeassistant/.../config` pour déclarer automatiquement les entités : `sensor.*_ph`, `sensor.*_orp`, `sensor.*_temperature`, `switch.*_filtration_switch`, `switch.*_lighting`, `binary_sensor.*_ph_dosing`, `number.*_ph_target`, `select.*_filtration_mode`, etc.

Publié **une seule fois** par session (`discoveryPublished` guard).

## Intervalles

- Publication d'état périodique : **10 s** (`kMqttPublishIntervalMs` [`constants.h:17`](../../src/constants.h:17)).
- Publication diagnostic : **5 min** (`kDiagnosticPublishIntervalMs` [`constants.h:19`](../../src/constants.h:19)).

## Reconnexion

Backoff exponentiel : 5 s → 10 s → 20 s → ... → **120 s max** (`_reconnectDelay` [`mqtt_manager.h:52`](../../src/mqtt_manager.h:52)).

Reset sur appel explicite à `requestReconnect()` (après changement de config broker, username, etc.).

## Gestion des commandes

`messageCallback()` dispatche selon le topic reçu :
- `{base}/filtration/set` → `filtration.setManualOn/Off`
- `{base}/lighting/set` → `lighting.setManualOn/Off`
- `{base}/ph_target/set`, `{base}/orp_target/set` → mise à jour + `saveConfig()`
- `{base}/filtration_mode/set` → change `filtration_mode` dans `filtrationCfg`

## LWT / Status

Connexion avec `willTopic = {base}/status`, `willMessage = "offline"`, `willRetain = true`. Dès la connexion réussie, publication de `"online"`.

## Cas limites

- **MQTT désactivé** (`mqtt_enabled = false`) : `begin()` initialise les topics mais `connect()` n'est pas appelé tant que le toggle reste off.
- **WiFi disponible mais broker injoignable** : backoff exponentiel, aucun blocage du loop principal.
- **Broker accepte puis refuse auth** : déconnexion, tentative de reconnexion avec le backoff, log WARN.

## Fichiers liés

- [`src/mqtt_manager.h`](../../src/mqtt_manager.h), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)
- [`src/config.h`](../../src/config.h) — struct `MqttConfig`
- [docs/MQTT.md](../MQTT.md) — topics complets + entités HA
