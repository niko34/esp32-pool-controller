## Métadonnées
- **ID** : feature-070
- **Slug** : durcissement-routes-debug-et-cooldown-reset
- **Type** : feature (durcissement sécurité + dette technique)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.21.0
- **Dernière mise à jour** : 2026-08-21
- **Spec(s) liée(s)** : `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue` (v2.20.3 — a fermé `/debug/sensor_filter_reset`, laissé les autres ouvertes), `bug-save-config-applique-avant-authentification` (todo, même famille)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun chemin de dosage nouveau ; **à ré-évaluer** si le repli `|| "automatic"` est jugé sécuritaire
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Trois points relevés en revue du cycle v2.20.3, hors périmètre du correctif de filtre : deux routes `/debug/*` qui **écrivent** dans la queue EZO sans authentification, un repli JavaScript qui peut annuler une escalade de sécurité, et un cooldown de reset dupliqué entre deux routes.

## Contexte

### 1. Routes `/debug/*` en écriture, sans authentification

Le cycle v2.20.3 a fermé `POST /debug/sensor_filter_reset` (auth `CRITICAL`) parce qu'elle effectuait la **même action destructrice** que la nouvelle route de réarmement tout en contournant toutes ses gardes. Le raisonnement s'applique, à un degré moindre, à deux routes voisines restées ouvertes :

| Route | Effet |
|---|---|
| `POST /debug/sensor_trace_clear` | efface le ring buffer de trace capteur — **détruit une preuve de diagnostic** |
| `POST /debug/ph_slope_refresh` | **enqueue une commande EZO** (`Slope,?`) : écriture dans la queue FreeRTOS, transaction I²C bloquante ~900 ms dans `loopTask` |

`ph_slope_refresh` est déjà protégée du spam par `_phSlopeQueryPending` (dédoublonnage) et par la capacité de la queue (`503` si pleine), mais elle reste un **point d'écriture non authentifié** : une sollicitation répétée depuis le réseau local ajoute des transactions I²C bloquantes au cycle capteur.

À trancher : ces routes doivent-elles passer en `WRITE` (lecture/écriture standard), rester ouvertes (diagnostic assumé sur réseau local de confiance), ou être conditionnées à un drapeau de configuration ? `GET /debug/sensor_trace` et `GET /debug/sensor_filter_state`, purement en **lecture**, ne sont pas concernés.

### 2. Repli `|| "automatic"` dans `data/app.js`

Un repli de la forme `mode || "automatic"` sur le mode de régulation peut **réactiver une régulation que le firmware a délibérément coupée** : la bascule automatique en `manual` (escalade « refus de plancher acide répétés », feature-067) est un geste de **sécurité**, et un `undefined` transitoire côté UI ne doit pas la transformer en `automatic` au prochain enregistrement des réglages.

Le risque est réel depuis la v2.20.3, qui persiste la trace de cette bascule (`ph_auto_manual_epoch` / `ph_auto_manual_reason`) et l'acquitte sur **changement réel** du mode : un repli qui renvoie `automatic` alors que le firmware est en `manual` produit **exactement** un changement réel — il annule l'escalade **et** effacerait sa trace.

À faire : localiser tous les replis de ce type, et remplacer par une absence d'envoi du champ (ne rien envoyer plutôt qu'envoyer une valeur devinée).

### 3. Cooldown de reset dupliqué

`POST /reset-sensor-filter` implémente son anti-abus (`sLastResetMs[2]`, `kSensorFilterUiResetMinIntervalMs` = 10 min, `429` + `retry_after_s`) **localement dans le handler**. `POST /debug/sensor_filter_reset`, désormais également protégée par auth `CRITICAL`, n'a **aucun** cooldown : elle offre donc un contournement du plafond de 10 min à un utilisateur déjà administrateur. À factoriser dans un helper partagé.

## Comportement attendu

### Volet 1 — routes de debug
- Décision explicite du niveau de protection pour `sensor_trace_clear` et `ph_slope_refresh`.
- Si passage en `WRITE` : mettre à jour l'UI qui les appelle (le bouton « Rafraîchir » du modal sonde pH utilise `ph_slope_refresh` via `authFetch`, donc déjà porteur des credentials) et **corriger les titres correspondants** dans `docs/API.md`.
- Les routes de **lecture** restent inchangées.

### Volet 2 — repli JS
- Aucun repli ne doit fabriquer une valeur de mode de régulation. En l'absence de valeur connue, le champ n'est **pas** envoyé.
- Vérifier symétriquement le mode de régulation **ORP**.

### Volet 3 — cooldown partagé
- Un seul point de vérité pour « 1 reset par capteur toutes les 10 min », appliqué aux **deux** routes.
- Réponse `429` + `retry_after_s` identique des deux côtés.

### Cas limites
- **Wizard / premier démarrage** : ne pas casser les parcours où l'authentification n'est pas encore établie (cf. `/auth/*` public au premier démarrage).
- **Compteur statique et wrap `millis()`** : conserver la sentinelle `0 = jamais` et la comparaison wrap-safe du code existant.
- **Client tiers / script** appelant `/debug/ph_slope_refresh` sans credentials : passera en `401` — **rupture de contrat** à annoncer dans le `CHANGELOG` et `docs/API.md`.

## Comportement actuel

- `/debug/sensor_trace_clear` et `/debug/ph_slope_refresh` : **aucune authentification**.
- `/debug/sensor_filter_reset` : auth `CRITICAL` depuis v2.20.3, **sans** cooldown.
- `/reset-sensor-filter` : auth `CRITICAL` + cooldown 10 min/capteur, implémenté **dans le handler**.
- `data/app.js` : repli `|| "automatic"` présent sur le mode de régulation.

## Contraintes techniques
- ESP32, offline first
- AsyncWebServer : aucun handler bloquant > 50 ms — le cooldown reste un test scalaire, pas d'I/O
- Vanilla JS dans `data/`
- Français dans toute l'UI utilisateur
- Ne pas écrire dans un `SensorFilter` depuis un handler async : passer par `requestFilterReset()` (contrat `sensor_filter.h`, ADR-0041)
- Toute modification de niveau d'auth est un **changement de contrat externe** ⇒ `docs/API.md` + `CHANGELOG.md` obligatoires

## Fichiers potentiellement concernés
- `src/web_routes_debug.cpp` — niveaux d'auth, cooldown partagé
- `src/web_routes_control.cpp` — bascule du cooldown vers le helper partagé
- `src/auth.h` / un nouvel en-tête utilitaire — helper de cooldown (candidat à la logique pure, ADR-0017)
- `src/constants.h` — si un seuil est ajouté
- `data/app.js` — suppression des replis de mode
- `docs/API.md`, `docs/features/page-ph.md`, `docs/features/page-settings.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Décision de niveau d'auth pour `sensor_trace_clear` et `ph_slope_refresh`, appliquée
- [ ] A2 — Helper de cooldown partagé, appliqué aux **deux** routes de reset de filtre
- [ ] B1 — Suppression des replis `|| "automatic"` (pH **et** ORP) dans `data/app.js`
- [ ] C1 — `docs/API.md` : titres et niveaux d'auth corrigés, `429` documenté sur `/debug/sensor_filter_reset`
- [ ] C2 — `CHANGELOG.md` : rupture de contrat annoncée si des routes passent en `WRITE`
- [ ] D1 — Test natif du helper de cooldown (première demande, refus, expiration, wrap `millis()`, indépendance des capteurs)

## Sécurité chimique
- Dosage impliqué : **non directement** — mais le volet 2 touche le mode de régulation pH, donc la capacité de l'appareil à **rester** en sécurité après une escalade
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : sans objet
- Calibration capteur modifiée : non (`ph_slope_refresh` interroge la pente, ne recalibre pas)
- Algorithme PID touché : non
- Risque identifié : un repli UI qui **réactive** une régulation coupée par sécurité. Ce point seul peut justifier un passage `pool-chemistry`.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun — application d'une politique d'authentification existante et factorisation locale. Un ADR ne serait justifié que si la décision « les routes `/debug/*` en écriture restent ouvertes » était retenue **et** assumée comme politique durable (critère 2).
- **Sujet pressenti** : éventuellement « politique d'authentification des routes de diagnostic »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /debug/sensor_trace_clear` — niveau d'auth possiblement modifié
- `POST /debug/ph_slope_refresh` — niveau d'auth possiblement modifié
- `POST /debug/sensor_filter_reset` — ajout d'un `429` (cooldown)
- `POST /reset-sensor-filter` — comportement inchangé, implémentation factorisée

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-ph.md` — tableau des actions (auth du bouton « Rafraîchir »)
- `docs/features/page-settings.md` — si l'onglet Avancé appelle une route concernée

#### Docs transverses
- `docs/API.md` (⚠️ titres et niveaux d'auth), `CHANGELOG.md`

#### Docs immuables
- aucun a priori

## Critères d'acceptation
- [ ] Aucune route `/debug/*` qui **écrit** (queue EZO, effacement de trace, reset) n'est accessible sans authentification, ou le contraire est **explicitement assumé et documenté**
- [ ] Les routes `/debug/*` en **lecture seule** restent inchangées
- [ ] `POST /debug/sensor_filter_reset` et `POST /reset-sensor-filter` partagent **le même** cooldown (1/capteur/10 min) et renvoient le même `429` + `retry_after_s`
- [ ] Aucun repli JavaScript ne peut envoyer un mode de régulation deviné : en l'absence de valeur, le champ n'est pas envoyé
- [ ] Une bascule automatique en `manual` ne peut pas être annulée ni acquittée par un simple enregistrement des réglages
- [ ] `docs/API.md` ne contient **plus aucun** titre de route annonçant un niveau d'auth faux
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous

## Plan de test manuel
1. `curl -X POST http://poolcontroller.local/debug/ph_slope_refresh` **sans** credentials → comportement attendu selon la décision (`401` ou `200`), cohérent avec `docs/API.md`.
2. Depuis l'UI, bouton « Rafraîchir » du modal sonde pH → fonctionne toujours (credentials portés par `authFetch`).
3. Deux resets de filtre pH successifs, l'un via `/reset-sensor-filter`, l'autre via `/debug/sensor_filter_reset` → le **second est refusé** en `429`.
4. Reset pH puis reset ORP dans la même minute → **les deux acceptés** (cooldown par capteur).
5. Firmware en `ph_regulation_mode = manual` (bascule de sécurité, bannière affichée) → enregistrer les réglages sans toucher au mode : le mode **reste** `manual`, la bannière **reste** affichée.

## Notes pour agent
- Le précédent v2.20.3 est le modèle : une route de debug qui fait la même chose qu'une route protégée doit être protégée **de la même façon**.
- ⚠️ Changer un niveau d'auth est une **rupture de contrat** pour un éventuel script utilisateur : l'annoncer dans le `CHANGELOG` et corriger `docs/API.md` dans le **même** commit.
- Le helper de cooldown est un bon candidat à la logique **pure** (ADR-0017) : entrée `(nowMs, lastMs, minIntervalMs)`, sortie `(autorisé, attente restante)` — testable en natif, wrap `millis()` inclus.
- Voir aussi la spec `bug-save-config-applique-avant-authentification` (todo) : même famille de défauts (ordre auth/effet).

## Hors périmètre
- Refonte générale de la politique d'authentification (niveaux `WRITE` / `CRITICAL`, tokens)
- Retrait des routes `/debug/*` (elles restent des outils de diagnostic utiles)
