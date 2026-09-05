## Métadonnées
- **ID** : feature-047
- **Slug** : info-taille-firmware
- **Type** : feature (info UI + 1 champ API)
- **Statut** : done
- **Priorité** : basse
- **Version cible** : 2.5.2
- **Dernière mise à jour** : 2026-07-04
- **Spec(s) liée(s)** : feature-046 (carte Infos système dans l'onglet Système), ADR-0019 (layout v3 — la marge firmware devient visible)

## Description
Dans **Paramètres → Système → Infos système**, ajouter une ligne **« Firmware (flash) »** affichant la taille du binaire, la taille totale de la partition app active et le pourcentage occupé, **au même format que la ligne FS** : `1382 KB / 1664 KB (83%)`.

## Périmètre exact (inventorié)
- **src/web_routes_config.cpp** (handler `/get-system-info`, ~l.616-621) : ajouter `doc["sketch_size"] = ESP.getSketchSize();` dans le bloc « Partition OTA » (le champ `ota_partition_size` existe déjà : `esp_ota_get_running_partition()->size`).
- **data/index.html** : nouvelle ligne kv dans la carte Infos système, entre « Flash » et « FS » : `<div class="kv__row"><span class="kv__k">Firmware (flash)</span><span class="kv__v" id="sys_app_usage">—</span></div>`.
- **data/app.js** (`loadSystemInfo()`, à côté du rendu FS l.3853-3859) : peupler `#sys_app_usage` avec `sketch_size`/`ota_partition_size` au format EXACT de la ligne FS (`Math.round(o/1024)` KB, pct arrondi) ; `—` si champs absents (compat ancien firmware).
- Docs : page-settings.md (ligne ajoutée), API.md si `/get-system-info` y est documenté champ par champ, CHANGELOG 2.5.2.

## Critères d'acceptation
- [x] AC1 — La ligne « Firmware (flash) » s'affiche dans Infos système au format `X KB / Y KB (Z%)` avec des valeurs cohérentes (≈1382/1664 KB, ~83 %).
- [x] AC2 — `/get-system-info` expose `sketch_size` (octets) ; contrat existant inchangé par ailleurs.
- [x] AC3 — Robustesse : champs absents → `—` (pas d'erreur console).
- [x] AC4 — `pio run` SUCCESS sans warning ; `node --check` + `./build_fs.sh` OK ; 118 tests natifs verts.
- [x] AC5 — Docs + CHANGELOG à jour.
- [x] AC6 — Validation navigateur utilisateur.

## Hors périmètre
- Toute autre ligne d'Infos système ; alertes/seuils sur l'occupation.
