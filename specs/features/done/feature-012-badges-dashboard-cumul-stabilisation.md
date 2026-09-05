## Métadonnées
- **ID** : feature-012
- **Slug** : badges-dashboard-cumul-stabilisation
- **Type** : feature
- **Statut** : done
- **Priorité** : basse
- **Version cible** : 2.9.0 (livrée)
- **Dernière mise à jour** : 2026-07-05 (**PÉRIMÈTRE RÉDUIT sur avis ux-ui-designer** — voir bloc ci-dessous ; implémenté en v2.9.0, voir Notes d'implémentation)

## RÉVISION UX (2026-07-05) — périmètre réduit

Audit contre le code v2.8.0 + avis `ux-ui-designer` : la spec d'avril est aux 2/3 supersédée. Le dashboard affiche DÉJÀ : badge « Limite journalière atteinte » (app.js l.1696-1698), badges hors plage/stock faible (badge multi-états `#ph/orp-sensor-badge` avec concaténation « · »), et **« Stabilisation : X min YY s » dans `#ph/orp-dosing-status`** (`getDosingStopReason()`, l.1948-1953) → B6/B7 déjà implémentés. L'état WS déconnecté est couvert globalement (`.app--offline`) → B5 sans objet.

**Périmètre retenu (remplace B1-B7)** :
- **N1** — Segment « Cumul XX % » ajouté au badge multi-états existant, variant `warning`, affiché à partir de **75 %** du quota journalier (seuil aligné sur `updateDailyDoseDisplay` 75/90, PAS les 70/95 de la spec d'origine), **masqué quand `*_limit_reached`** (le badge « Limite journalière atteinte » prend le relais, pas de redondance).
- **N2** — Règle de composition du badge multi-états formalisée dans le code : priorité danger > warning, **maximum 2 segments** affichés (troncature au plus prioritaire au-delà) — tient en 375 px et prépare le futur segment « Capteur indisponible » (reliquat feature-022 R2).
- **N3** — Micro-fix : `getDosingStopReason()` consomme les champs de stabilisation **par pompe** (`ph/orp_stab_remaining_s`, feature-006) au lieu du champ global.
- **N4** — Doc : `docs/features/page-dashboard.md` + `CHANGELOG.md`.

**Abandonnés sur avis UX** : barre de progression 4 px (chrome permanent pour info actionnable ~10 % du temps ; la lecture pré-75 % n'est pas actionnable et vit sur /ph, /orp), tooltip (hover inexistant sur iOS), icône ⛔, fade 300 ms, décompte animé seconde par seconde (distraction sans action possible — le rafraîchissement WS suffit).

Aucun changement firmware (tous les champs WS existent). Cycle allégé : web-ui-developer → vérifs → code-reviewer → doc-writer.
- **Doc(s) liée(s)** : [docs/features/page-dashboard.md](../../docs/features/page-dashboard.md), [docs/features/page-ph.md](../../docs/features/page-ph.md), [docs/features/page-orp.md](../../docs/features/page-orp.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non (affichage uniquement, pas de logique chimique)
- [ ] ux-ui-designer — non (réutilise les styles existants des pages /ph et /orp)
- [ ] embedded-developer — non (tous les champs existent déjà en WS)
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter sur la page dashboard deux informations transverses : (a) cumul journalier pH/ORP en barre de progression compacte sur chaque carte, (b) badge « stabilisation en cours — XXs » quand un timer est actif.

## Contexte
Voir [docs/features/page-ph.md](../../docs/features/page-ph.md) et [docs/features/page-orp.md](../../docs/features/page-orp.md). Actuellement le cumul journalier n'est affiché que sur /ph et /orp (bloc Statistiques). Le dashboard, point d'entrée de l'utilisateur, n'en dit rien — ce qui masque un dépassement imminent jusqu'à ce qu'on clique sur la page détaillée. De même, quand la stabilisation bloque une injection, aucune information visuelle sur le dashboard.

## Comportement attendu

### Côté UI
- Carte pH sur dashboard :
  - Sous la valeur pH courante, ajouter une barre de progression compacte (hauteur 4 px) : `ph_daily_ml / max_ph_ml_per_day`
  - Couleur : vert < 70 %, orange 70-95 %, rouge ≥ 95 %
  - Tooltip / hover : « XX mL / XXX mL aujourd'hui »
- Symétrique ORP.
- Badge « Stabilisation en cours · 04:20 » sur le dashboard, dans une bande supérieure ou sous les cartes pH+ORP, si `stabilization_remaining_s > 0`.
  - Disparait dès que `stabilization_remaining_s == 0`
  - Décompte client-side (calcul local à partir de la dernière valeur reçue + `Date.now()`) pour éviter les saccades
- Cohérence avec le bloc Statistiques des pages /ph et /orp : mêmes couleurs, mêmes règles de seuil.

### Cas limites
- WebSocket déconnecté : barre grise, tooltip « WebSocket déconnecté »
- Limite atteinte (`ph_limit_reached = true`) : barre rouge pleine + icône `⛔` + tooltip « Limite atteinte, dosage suspendu jusqu'à minuit »
- Stabilisation qui bascule de 300 à 0 : badge disparaît doucement (fade 300 ms)
- Pas de stabilisation active : pas de badge (aucun espace réservé)

## Comportement actuel
- Dashboard : uniquement les valeurs brutes pH, ORP, Temp, statut filtration/éclairage.
- Aucun indicateur cumul ni stabilisation.

## Contraintes techniques
- ESP32, offline first
- Vanilla JS, pas de framework
- Tous les champs sont déjà diffusés par WebSocket → **aucun changement firmware**
- Budget FS à surveiller (`./build_fs.sh`)
- Mobile friendly : la barre de progression ne doit pas casser la mise en page iPhone

## Fichiers potentiellement concernés
- `data/index.html` — ajouter les barres et le badge sur la section `#view-dashboard`
- `data/app.css` — styles des nouveaux éléments
- `data/app.js` — fonction `updateDashboardDaily()`, `updateStabilizationBadge()` appelées depuis le handler WS
- `docs/features/page-dashboard.md` — documenter les nouveaux éléments
- `CHANGELOG.md` — entrée

## Livrables — check-list

### Périmètre réduit N1-N4 (retenu)
- [x] N1 — Segment « Cumul XX % » (warning) dans le badge multi-états pH/ORP, seuil 75 %, même arithmétique que `updateDailyDoseDisplay` (arrondi des entrées, plafond 100 %), masqué si `*_limit_reached`, limite non configurée ou données indisponibles
- [x] N2 — Règle de composition formalisée (`composeSensorBadge`) : priorité hors plage (danger) > limite atteinte > cumul ≥ 75 % > stock faible, max 2 segments, variant danger > warning ; commentaire réservant « Capteur indisponible » (feature-022)
- [x] N3 — `getDosingStopReason()` consomme `ph/orp_stab_remaining_s` (par pompe, feature-006) avec fallback sur le champ global
- [x] N4 — Doc : `docs/features/page-dashboard.md` + `CHANGELOG.md` [2.9.0]

### Check-list d'origine (B1-B7 supersédés par la révision UX)
- [ ] B1 — ~~barre de progression pH~~ — **abandonné avis UX** (remplacé par segment « Cumul XX % », N1)
- [ ] B2 — ~~barre de progression ORP~~ — **abandonné avis UX** (idem N1)
- [ ] B3 — ~~Tooltip / tap mobile~~ — **abandonné avis UX** (hover inexistant sur iOS)
- [ ] B4 — ~~barre rouge + icône ⛔~~ — **abandonné avis UX** (badge « Limite journalière atteinte » déjà couvert)
- [ ] B5 — ~~WS déconnecté : barre grisée~~ — **sans objet** (couvert globalement par `.app--offline`)
- [ ] B6 — ~~badge stabilisation + fade~~ — **déjà couvert** (statut `#ph/orp-dosing-status`) ; fade abandonné avis UX ; par-pompe livré (N3)
- [ ] B7 — ~~décompte client-side animé~~ — **abandonné avis UX** (le rafraîchissement WS suffit)
- [x] C1 — `docs/features/page-dashboard.md` : sections badge multi-états + stabilisation par pompe
- [x] C2 — `CHANGELOG.md` : entrée [2.9.0]
- [x] D1 — `./build_fs.sh` passe sans dépasser la partition
- [ ] D2 — Mobile (375 px) sans débordement, vérification iPhone Safari — **reste on-target** (la troncature à 2 segments est conçue pour 375 px)

## Nouvelles signatures / API
- Aucune — 100 % UI

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun — réutilisation pure des champs WS existants et des styles des pages /ph et /orp.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun

### Champs WebSocket
- Aucun nouveau (déjà existants : `ph_daily_ml`, `max_ph_ml_per_day`, `orp_daily_ml`, `max_chlorine_ml_per_day`, `ph_limit_reached`, `orp_limit_reached`, `stabilization_remaining_s`)

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-dashboard.md` — section « Indicateurs journaliers » + badge stabilisation

#### Docs transverses (si contrat externe modifié)
- `CHANGELOG.md` — entrée

#### Docs immuables
- aucune ADR (voir §ADR attendu)

## Critères d'acceptation
- [ ] ~~Carte pH dashboard : barre de progression~~ — **abandonné avis UX** (remplacé par le critère N1 ci-dessous)
- [ ] ~~Carte ORP dashboard : idem~~ — **abandonné avis UX**
- [ ] ~~Tooltip / tap « X mL / Y mL aujourd'hui »~~ — **abandonné avis UX**
- [ ] ~~Décompte qui descend seconde par seconde~~ — **abandonné avis UX** (rafraîchissement WS suffisant)
- [x] Quand `*_stab_remaining_s > 0` : statut « Stabilisation : X min YY s » visible sur la carte de **sa** pompe (N3) ; disparaît à 0 — **déjà couvert** pour l'affichage, par-pompe livré
- [ ] ~~`ph_limit_reached` : barre rouge + ⛔~~ — **abandonné avis UX** (badge « Limite journalière atteinte » déjà couvert)
- [ ] ~~WS déconnecté : barres grisées~~ — **sans objet** (`.app--offline` global)
- [x] Segment « Cumul XX % » affiché à ≥ 75 % du quota, masqué à limite atteinte / limite non configurée / données absentes (N1)
- [x] Max 2 segments dans le badge ; « hors plage + limite atteinte » affichent les 2 segments (N2)
- [x] Tous les libellés utilisateur sont en français
- [x] `./build_fs.sh` passe sans dépasser la partition
- [ ] Mobile (viewport iPhone SE 375px) : pas de débordement — **reste on-target**

## Plan de test manuel
1. Dashboard nominal : observer les barres à 10 %, 50 %, 90 % selon les scénarios.
2. Déclencher une injection pH jusqu'à limite journalière : barre rouge + icône apparaît en temps réel.
3. Démarrer la filtration : badge stabilisation apparaît avec décompte. Attendre fin → badge disparaît.
4. Débrancher WS (killer le serveur) : barres virent au gris.
5. Redimensionner la fenêtre à 375px : mise en page préservée.

## Notes pour agent
- **Cohérence visuelle** : réutiliser les classes / variables CSS déjà en place dans /ph et /orp pour éviter la divergence.
- **Décompte client-side** : garder un offset entre la valeur serveur reçue et `Date.now()`, puis faire un `setInterval(updateBadge, 1000)` simple. Resynchroniser à chaque message WS.
- **Aucun nouveau champ WS** : toute l'info est déjà poussée. Si un champ est manquant, c'est un bug — le signaler avant d'implémenter.
- **Tester iPhone Safari** : le projet est utilisé en majorité depuis mobile.

## Hors périmètre
- Entités HA pour le cumul journalier (pas demandé ici)
- Graphiques historique sur dashboard (trop lourd, réservé à /ph et /orp)
- Badges pour d'autres états (cycles max, etc.) — spec séparée si demandée

## Notes d'implémentation (2026-07-05)

- **Livré en v2.9.0**, périmètre réduit N1-N4 (avis `ux-ui-designer` suivi : barre de progression, tooltip, décompte animé, icône ⛔ et fade abandonnés).
- **Fichier modifié** : `data/app.js` uniquement (100 % UI, aucun changement firmware — tous les champs WS existaient).
  - N1 : `dailyCumulSegment()` — segment « Cumul XX % », seuil 75 %, masqué si `*_limit_reached` / quota ≤ 0 / données absentes.
  - N2 : `composeSensorBadge()` — priorité 1 hors plage (danger) > 2 limite atteinte > 3 cumul ≥ 75 % > 4 stock faible ; max 2 segments (troncature) ; variant danger > warning ; commentaire réservant « Capteur indisponible » (feature-022). Changement visible : « hors plage + limite atteinte » → 2 segments (avant : seul « hors plage »).
  - N3 : `getDosingStopReason()` — `ph/orp_stab_remaining_s` par pompe (feature-006), fallback `stabilization_remaining_s`.
- **Revue** : code-reviewer **Approved** + 1 remarque mineure corrigée (pourcentage plafonné à 100, arithmétique harmonisée avec `updateDailyDoseDisplay`).
- **Vérifications** : `node --check` OK, `./build_fs.sh` OK.
- **Reste on-target** : vérifier visuellement les badges à ≥ 75 % de cumul, à limite atteinte, et la stabilisation par pompe après une calibration (+ viewport 375 px).
- **Doc** : `docs/features/page-dashboard.md` (badge multi-états + stabilisation par pompe), `CHANGELOG.md` [2.9.0], `src/version.h` → 2.9.0. Aucun ADR (composition UI locale, aucun des 4 critères rempli).
