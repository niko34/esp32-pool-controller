# Page Produits — `/dosages`

- **Fichier UI** : [`data/index.html:977`](../../data/index.html:977) (section `#view-dosages`)
- **URL** : `http://poolcontroller.local/#/dosages`

## Rôle

Suivre le volume des bidons de produits chimiques (pH et chlore), alerter quand un bidon approche la fin. Le cumul injecté depuis le dernier « Nouveau bidon » est comparé au volume nominal du bidon pour afficher un volume restant et une barre de progression.

## Structure

Deux cartes symétriques, une par produit :

1. **Carte pH** (`#product-ph-card`)
   - Toggle `ph_tracking_enabled` — active / désactive le suivi de ce bidon.
   - Barre de volume (`#product-ph-bar`) + labels « restant / total » (L).
   - Champ **Volume du bidon (L)** (`#ph_container_l`) — par défaut 20 L.
   - Champ **Seuil d'alerte (L)** (`#ph_alert_threshold_l`) — par défaut 2 L.
   - Bouton **Enregistrer** (met à jour volume / seuil sans toucher au cumul).
   - Bouton **Nouveau bidon** (réinitialise le cumul à 0).
2. **Carte Chlore / ORP** (`#product-orp-card`) — strictement symétrique, toggle `orp_tracking_enabled`.

Le titre de la carte pH s'adapte à `ph_correction_type` (affiche « pH+ (base) » si mode pH+ — [`app.js:1770`](../../data/app.js:1770)).

## Données consommées (WebSocket `/ws` + `GET /get-config`)

**pH**
- `ph_tracking_enabled` (bool)
- `ph_container_ml` (float, mL)
- `ph_remaining_ml` (float, mL) — calculé : `container_ml − total_injected_ml` (borné à 0)
- `ph_alert_threshold_ml` (float, mL)

**ORP / chlore**
- `orp_tracking_enabled`, `orp_container_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml`

Voir [`config.h:129`](../../src/config.h:129) struct `ProductConfig` et [`ws_manager.cpp:171`](../../src/ws_manager.cpp:171) pour la diffusion.

## Actions

| Action | Endpoint | Auth | Payload |
|--------|----------|------|---------|
| Activer / désactiver suivi | `POST /save-config` | CRITICAL | `{ "ph_tracking_enabled": bool }` ou idem `orp_` |
| Sauvegarder volume & seuil | `POST /save-config` | CRITICAL | `{ "ph_container_ml": N, "ph_alert_threshold_ml": N }` (UI convertit L → mL) |
| Nouveau bidon (reset cumul) | `POST /save-config` | CRITICAL | `{ "ph_container_ml": N, "ph_reset_container": true }` — reset `phTotalInjectedMl` à 0 ([`web_routes_config.cpp:546`](../../src/web_routes_config.cpp:546)) |

Remarques :
- Les volumes sont **stockés côté firmware en mL** ; l'UI les saisit et les affiche en L.
- Le cumul `ph_total_injected_ml` / `orp_total_injected_ml` est incrémenté par [`pump_controller.cpp`](../../src/pump_controller.cpp) à chaque injection effectivement réalisée (durée × débit nominal `kPumpMaxFlowMlPerMin = 90.0`).

## Règles firmware

- **Persistance** : `saveProductConfig()` / `loadProductConfig()` ([`config.h:196`](../../src/config.h:196)). `productConfigDirty` est armé par [`pump_controller.cpp`](../../src/pump_controller.cpp) à chaque injection ; flush différé pour limiter l'écriture NVS.
- **Alerte** : quand `remaining_ml ≤ alert_threshold_ml`, la barre passe en rouge côté UI (seuil appliqué au rendu, pas au firmware — aucune action automatique, juste un signal visuel).
- **Tracking désactivé** : le cumul continue d'être comptabilisé côté firmware ; seules la barre et les toggles UI sont masqués.

Voir aussi [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) pour le contrat de comptabilisation du volume injecté.

## Cas limites

- **Bidon vide** (`remaining_ml ≤ 0`) : la barre affiche 0 %, l'injection **n'est pas bloquée** par le firmware (la sécurité vient de `max_ph_ml_per_day` / `max_chlorine_ml_per_day`, pas du suivi produit).
- **Toggle désactivé** : la carte reste visible, le toggle l'est aussi, mais les champs sont grisés — voir `applyProductTracking()` dans [`app.js:1821`](../../data/app.js:1821).
- **Aucun capteur pH/ORP connecté** : le suivi fonctionne quand même tant qu'une injection a lieu (mode manuel ou programmée).

## Interaction MQTT / Home Assistant

Aucune entité HA dédiée aux bidons produits à date. Les cumuls journaliers (`ph_daily_ml`, `orp_daily_ml`) sont exposés séparément via les entités pH/ORP — voir [page-ph.md](page-ph.md), [page-orp.md](page-orp.md).

## Fichiers

- [`data/index.html:977`](../../data/index.html:977) — structure HTML
- [`data/app.js:1783`](../../data/app.js:1783) — `setupProductScreen()`, `saveProduct()`, `resetProduct()`, `updateProductUI()`
- [`src/config.h:129`](../../src/config.h:129) — struct `ProductConfig`
- [`src/web_routes_config.cpp:526`](../../src/web_routes_config.cpp:526) — persistance (parsing `ph_*` / `orp_*`, reset container)
- [`src/web_routes_data.cpp:87`](../../src/web_routes_data.cpp:87) — champs exposés dans `/data`
- [`src/ws_manager.cpp:171`](../../src/ws_manager.cpp:171) — diffusion via WebSocket
- [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — incrémentation `phTotalInjectedMl` / `orpTotalInjectedMl`

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — contrat de comptabilisation du volume injecté.
