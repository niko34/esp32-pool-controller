## Métadonnées
- **ID** : feature-010
- **Slug** : renommage-max-ph-ml
- **Type** : refactor
- **Statut** : **clôturée** (2026-07-04 — 90 % supersédée, résidu interne implémenté)
- **Priorité** : basse (à la rédaction)
- **Version cible** : n/a (renommage interne, aucun changement de contrat)
- **Dernière mise à jour** : 2026-07-04

## Note de clôture (2026-07-04)

Audit contre le code v2.7.0 : **le contrat externe portait déjà le nom cible partout** — clé NVS `max_ph_ml` (config.cpp l.163/336), API `/get-config`/`/save-config` `max_ph_ml_per_day`, WS `max_ph_ml_per_day`, UART idem, JS idem (6 occurrences app.js), docs sans aucune occurrence de l'ancien nom, champ non exposé en MQTT. La migration NVS (A2/A3, le point le plus risqué de la spec) et le breaking change API (A5) n'ont donc **jamais eu lieu d'être** : le renommage externe a été absorbé silencieusement au fil des refontes précédentes.

**Résidu vivant implémenté ce jour** : renommage interne `SafetyLimits::maxPhMinusMlPerDay` → `maxPhMlPerDay` (8 fichiers src/ + docs/subsystems/pump-controller.md, ~25 occurrences, sed mécanique vérifié par compilation). Bénéfice observable unique : le log de plafonnement (`pump_controller.cpp` ~l.885) cite désormais `maxPhMlPerDay` au lieu du nom trompeur `maxPhMinusMlPerDay` quand on dose du pH+ (support pH+ confirmé : `phCorrectionType`, PID bidirectionnel). Aucune valeur, aucune logique, aucun contrat modifiés. Validation : `pio run` SUCCESS 0 warning, 135/135 tests natifs (résultats en fin de cycle), revue de code OK.
- **Doc(s) liée(s)** : [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif), [docs/API.md](../../docs/API.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (NVS)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non (juste renommage, aucun changement de valeur ni de logique)
- [ ] ux-ui-designer — non (libellé UI inchangé)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Renommer `maxPhMinusMlPerDay` en `maxPhMlPerDay` (et clé NVS, champ API, champ WS, label UI correspondants) car la limite s'applique aussi au cas `ph_plus` — le nom actuel suggère à tort qu'elle ne couvre que le pH-.

## Contexte
Le champ `maxPhMinusMlPerDay` (dans `SafetyLimits`, voir [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif)) exprime la limite journalière totale du dosage pH (pH- **ou** pH+), mais son nom parle uniquement de pH-. Source de confusion pour les contributeurs et les utilisateurs via l'API.

Renommage pur : **aucun changement sémantique**, aucun impact chimique. `pool-chemistry` peut être consulté par prudence mais n'est pas bloquant.

## Comportement attendu

### Côté firmware
- `src/config.h` : `SafetyLimits::maxPhMinusMlPerDay` → `maxPhMlPerDay`
- `src/config.cpp` : clé NVS `max_ph_minus_ml` → `max_ph_ml` (dans le namespace approprié — à vérifier)
- **Migration NVS au premier boot** : si `max_ph_minus_ml` existe, lire sa valeur, écrire dans `max_ph_ml`, supprimer l'ancienne clé. Si aucune des deux n'existe (install fraîche) → valeur par défaut 300 mL/j.
- Toutes les occurrences de `maxPhMinusMlPerDay` dans `src/` (notamment `pump_controller.cpp`, `ws_manager.cpp`, `mqtt_manager.cpp`, `web_routes_config.cpp`) → renommées.
- API HTTP :
  - `GET /get-config` : champ `max_ph_minus_ml_per_day` → `max_ph_ml_per_day`
  - `POST /save-config` : idem
  - Aucune compat ascendante côté API (break propre, versionnable via CHANGELOG).
- WebSocket : champ `max_ph_ml_per_day` déjà publié sous ce nom dans les docs (voir `docs/features/page-ph.md`) — **vérifier côté code** s'il est réellement nommé comme ça ou si c'est un alias à renommer aussi.

### Côté UI
- `data/index.html` + `data/app.js` : renommer tout identifiant JS qui réfère l'ancien champ.
- Aucun libellé utilisateur à changer : l'UI dit « Limite journalière pH (mL) », ce qui est déjà correct.

### Cas limites
- ESP32 ayant l'ancienne clé NVS : migration transparente au premier boot après update.
- Dashboards HA utilisant directement le topic MQTT : vérifier qu'aucun topic externe n'est nommé `max_ph_minus_ml_per_day` (cf. auto-discovery). Si oui, renommer l'entité HA — ça produit un nouvel `object_id` côté HA (ancienne entité devient orpheline, l'utilisateur doit la nettoyer).

## Comportement actuel
- Champ `maxPhMinusMlPerDay` partout (firmware, NVS, API, WS, JS). Valeur par défaut 300 mL/j.
- Fonctionnement correct, seul le nom prête à confusion.

## Contraintes techniques
- ESP32, offline first
- Migration NVS **obligatoire** pour ne pas repartir sur la valeur par défaut après update
- Aucun changement sémantique — la migration doit préserver la valeur existante
- Breaking change API documenté dans CHANGELOG

## Fichiers potentiellement concernés
- `src/config.h` — struct `SafetyLimits`
- `src/config.cpp` — load/save, migration NVS
- `src/pump_controller.cpp` — toutes les références à la limite
- `src/pump_controller.h`
- `src/ws_manager.cpp` — champ WS
- `src/mqtt_manager.cpp` — éventuel topic / auto-discovery
- `src/web_routes_config.cpp` — `/get-config` et `/save-config`
- `data/app.js` — toutes les références JS
- `data/index.html` — éventuels `data-field="..."` ou équivalents
- `docs/API.md` — mise à jour du champ
- `docs/MQTT.md` — si topic concerné
- `docs/features/page-ph.md`, `page-settings.md` — cohérence des tableaux
- `docs/subsystems/pump-controller.md` — §limites de sécurité
- `CHANGELOG.md` — entrée breaking change

## Livrables — check-list (annotée à la clôture, 2026-07-04)
- [x] A1 — `SafetyLimits::maxPhMinusMlPerDay` → `maxPhMlPerDay` dans `config.h` — **FAIT ce jour**
- [x] A2 — Clé NVS → **SANS OBJET** : déjà `max_ph_ml` (config.cpp l.163/336)
- [x] A3 — Migration NVS → **SANS OBJET** : aucune ancienne clé à migrer
- [x] A4 — Références dans `src/` → **FAIT ce jour** (8 fichiers, ~25 occurrences, y compris chaînes de log et commentaires ; mqtt_manager non concerné : champ non exposé en MQTT)
- [x] A5 — API `/get-config`/`/save-config` → **DÉJÀ EN PLACE** (`max_ph_ml_per_day`, web_routes_config.cpp l.133/423)
- [x] A6 — WS → **DÉJÀ EN PLACE** (`max_ph_ml_per_day`, ws_manager.cpp l.339)
- [x] A7 — MQTT → **SANS OBJET** (champ jamais exposé en MQTT, vérifié)
- [x] B1 — Frontend → **DÉJÀ EN PLACE** (`max_ph_ml_per_day`, 6 occurrences app.js ; aucun identifiant à l'ancien nom)
- [x] C1 — `docs/API.md` → **DÉJÀ COHÉRENT** (zéro occurrence de l'ancien nom dans docs/)
- [x] C2 — pages → **DÉJÀ COHÉRENTES**
- [x] C3 — `docs/subsystems/pump-controller.md` → **FAIT ce jour** (occurrences du nom de membre C++ renommées)
- [x] C4 — `docs/MQTT.md` → **SANS OBJET**
- [x] C5 — CHANGELOG breaking change → **SANS OBJET** (aucun contrat externe modifié — le renommage externe avait déjà été absorbé par les refontes antérieures)
- [x] D1 — `pio run` SUCCESS 0 warning ; `./build_fs.sh` non requis (aucune modif `data/`) ; 135/135 tests natifs verts
- [x] D2 — Test migration NVS → **SANS OBJET** (pas de migration)

## Nouvelles signatures / API
- Ancien : `max_ph_minus_ml_per_day` (API + NVS + JS)
- Nouveau : `max_ph_ml_per_day`
- Migration NVS : `max_ph_minus_ml` → `max_ph_ml` (si c'est la clé actuelle — à confirmer en lisant `config.cpp`)

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun — refactor pur. Le breaking change API est noté dans CHANGELOG, suffisant.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-config`, `POST /save-config` — champ renommé

### Champs WebSocket
- `max_ph_ml_per_day` (nouveau nom ; vérifier s'il était déjà exposé)

### Topics MQTT
- À vérifier — si un topic contient `ph_minus`, renommer et documenter.

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-ph.md`, `docs/features/page-settings.md` — tableaux mis à jour
- `docs/subsystems/pump-controller.md` — §limites de sécurité

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — champ renommé
- `docs/MQTT.md` — si topic concerné
- `CHANGELOG.md` — **breaking change API** listé explicitement

#### Docs immuables
- aucune ADR (voir §ADR attendu)

## Critères d'acceptation
- [ ] Aucune occurrence de `maxPhMinusMlPerDay` ou `max_ph_minus_ml` dans `src/` et `data/` après refactor
- [ ] Un ESP32 migrant d'une version précédente conserve la valeur configurée (ex. 250 mL/j reste 250 mL/j)
- [ ] `GET /get-config` retourne `max_ph_ml_per_day` (et plus l'ancien nom)
- [ ] `POST /save-config` accepte `max_ph_ml_per_day` (rejette l'ancien nom ou l'ignore)
- [ ] UI affiche et persiste correctement la valeur
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe
- [ ] CHANGELOG documente le breaking change API + script / note pour clients externes

## Plan de test manuel
1. Flasher sur un ESP32 ayant l'ancienne config 250 mL/j → après boot, vérifier NVS (`nvs_get` ou endpoint debug) que `max_ph_ml` = 250 et que `max_ph_minus_ml` n'existe plus.
2. `GET /get-config` → contient `max_ph_ml_per_day: 250`, ne contient plus `max_ph_minus_ml_per_day`.
3. `POST /save-config` avec `max_ph_ml_per_day: 200` → valeur persistée, régulation la respecte.
4. UI : charger settings, vérifier que l'input affiche 200, le modifier, sauver, recharger → OK.
5. MQTT : si l'auto-discovery référence l'ancien nom, vérifier le renommage.

## Notes pour agent
- **Pas de compat ascendante API** : simpler, un seul nom. Les clients externes (scripts HA custom, curl) doivent être notifiés via CHANGELOG.
- **Migration NVS silencieuse** : aucune action utilisateur requise.
- **Nommage** : `maxPhMlPerDay` est cohérent avec `maxChlorineMlPerDay` (ORP) — pas de « minus » dans ce dernier non plus.
- **Tester sur un ESP32 réel** ayant déjà de l'ancienne config : le scénario de migration est le seul point à risque.

## Hors périmètre
- Modification de la valeur par défaut (300 reste 300)
- Ajout de limites séparées pour pH- vs pH+ (non, une seule limite couvre les deux)
- Renommage symétrique `maxChlorineMlPerDay` (déjà bien nommé)
