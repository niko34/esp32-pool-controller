# feature-071 — Carte « Signaux capteurs » : afficher la dispersion qui décide du drapeau `unstable`

## Métadonnées
- **ID** : feature-071
- **Slug** : carte-signaux-dispersion-drapeau
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.20.7
- **Dernière mise à jour** : 2026-08-23
- **Spec(s) liée(s)** : feature-057 (trace capteur), feature-058 (latch EMI), v2.20.6 (exposition de `dispersion` sur `/debug/sensor_filter_state`)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
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
- [ ] pool-chemistry — non : affichage pur, aucun chemin vers le dosage
- [ ] ux-ui-designer — non : enrichissement d'une carte existante, pas de refonte ni d'arbitrage de mise en page
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
La carte « Signaux capteurs (brut / lissé) » des paramètres avancés doit afficher, pour le pH et l'ORP, la **dispersion** telle que le firmware la calcule pour décider du drapeau `unstable` (amplitude max−min sur la fenêtre médiane de 13 échantillons), avec son seuil — au lieu du seul `Δ` crête-à-crête sur tout le buffer de trace, qui n'est comparable à rien.

## Contexte
Le `Δ` actuel de la carte est `max − min` sur les ~300 points du buffer (≈ 25 min). Pendant les campagnes de mesure d'août 2026, cette grandeur a **induit en erreur** : elle englobe la dérive lente (thermique notamment) et n'a aucun rapport d'échelle avec la grandeur décisionnelle. Mesuré sur la même série : crête-à-crête 25 min = 0,279 quand la dispersion réelle valait 0,055 — facteur 5, entièrement dû à la dérive. L'utilisateur a explicitement demandé que la carte reflète « le delta tel qu'il est mesuré pour déterminer le drapeau unstable du pH », pour que la carte soit directement comparable aux résultats des cycles de mesure (référence : campagnes des 22-23/08, dispersion 0,055 filtration arrêtée / 0,277 en marche, seuil 0,5).

Tout existe côté firmware depuis la v2.20.6 : `GET /debug/sensor_filter_state` publie `dispersion`, `dispersion_max`, `latch_cycles`, `clean_streak`, `unstable`, `ready` par capteur. **Aucune modification firmware attendue.**

## Comportement attendu
À l'affichage de la carte (activation de l'onglet Avancé, clic « Recharger la trace ») :
1. La carte interroge `GET /debug/sensor_filter_state` en plus de `/debug/sensor_trace`.
2. Deux nouvelles lignes de stats (pH et ORP) affichent la **dispersion instantanée** publiée par la route, avec le seuil : ex. `0.277 / 0.5` — et un état lisible du drapeau (`stable` / `INSTABLE`).
3. Si `dispersion` est `null` (warmup ou porte d'alimentation fermée — filtration arrêtée), afficher `—` avec un libellé explicite du type « pas de jugement (circulation arrêtée ou fenêtre en cours de remplissage) ». **Ne jamais laisser croire qu'un tiret = stable.**
4. Le `Δ` crête-à-crête existant sur tout le buffer est conservé mais **renommé** pour dire ce qu'il est (ex. `Δ 25 min`) afin qu'il ne soit plus confondu avec la grandeur du drapeau.
5. (Souhaitable) En complément de la valeur instantanée, des stats de dispersion **reconstruites sur la trace affichée** (fenêtre glissante de 13 sur les points bruts : médiane · max) pour être comparable aux rapports de campagne qui donnent médiane/p95/max.

### Cas limites
- `dispersion: null` (porte fermée / warmup) → tiret + libellé, pas de fausse assurance de stabilité (piège documenté dans `docs/API.md` §Marge d'instabilité).
- `unstable: true` → état visuellement distinct (l'inhibition du dosage est en cours).
- Route `/debug/sensor_filter_state` injoignable → les lignes dispersion affichent « indisponible », le reste de la carte fonctionne comme avant (les deux requêtes sont indépendantes).
- Reconstruction sur trace (item 5) : une fenêtre contenant un saut ≥ seuil de rejet (`1.0` pH / `250` mV — `kPhFilterSpikeReject`/`kOrpFilterSpikeReject`, valeurs à vérifier dans `constants.h`) doit être écartée du calcul : le firmware rejette ces pics AVANT la fenêtre, la trace les contient encore. Précédent réel : reconstruction naïve → faux max 5,425 (transitoire de redémarrage de filtration) là où le firmware voyait 0,308.
- Buffer de trace < 13 points → stats reconstruites : tiret.

## Comportement actuel
`_setTraceStat()` (data/app.js ≈ l. 5900) affiche `min · max · Δ` sur la totalité des points non nuls du buffer, pour pH brut/lissé, ORP brut/lissé, T° eau. Aucune lecture de `/debug/sensor_filter_state` nulle part dans le frontend. Le drapeau `unstable` n'est visible nulle part dans l'UI.

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS : vérifier avec `./build_fs.sh` (layout v4, 320 KB)
- `/debug/sensor_filter_state` est sans auth (contrat v2.20.6) — utiliser `authFetch` comme le reste du fichier par uniformité
- Pas de polling permanent : mêmes déclencheurs que la trace (activation onglet + bouton Recharger)
- Arrondis existants de la route : 3 décimales pH, 1 décimale mV

## Fichiers potentiellement concernés
- `data/index.html` — nouvelles lignes de stats dans la carte `#card-sensor-trace`, renommage du Δ existant
- `data/app.js` — fetch `/debug/sensor_filter_state`, rendu des nouvelles lignes, reconstruction fenêtre 13 sur la trace (item 5)
- `data/app.css` — état visuel `INSTABLE` (si nécessaire, réutiliser les classes existantes de préférence)
- `src/version.h` — bump 2.20.7
- `docs/features/page-settings.md` — la carte documentée
- `CHANGELOG.md` — entrée 2.20.7

## Livrables — check-list
- [ ] B1 — Lignes « Dispersion pH » et « Dispersion ORP » : valeur instantanée route + seuil + état du drapeau
- [ ] B2 — Cas `null` : tiret + libellé explicite « pas de jugement », jamais assimilable à « stable »
- [ ] B3 — Renommage du Δ existant en libellé explicite (« Δ 25 min » ou équivalent)
- [ ] B4 — Stats de dispersion reconstruites sur la trace affichée (médiane · max, fenêtres à saut ≥ rejet écartées)
- [ ] B5 — Panne de la route indépendante du reste de la carte
- [ ] C1 — `docs/features/page-settings.md` mis à jour
- [ ] C2 — `CHANGELOG.md` entrée 2.20.7
- [ ] D1 — `pio run` sans nouveau warning + `pio test -e native` 609/609 (aucune régression attendue, rien de natif touché)
- [ ] D2 — `./build_fs.sh` passe sans dépasser la partition

## Nouvelles signatures / API
Aucune. Consommation exclusive de l'existant (`GET /debug/sensor_filter_state`, v2.20.6).

## Sécurité chimique
- Dosage impliqué : non — affichage en lecture seule d'un diagnostic existant, aucun chemin de code vers `pump_controller`.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun nouveau — consommation de `GET /debug/sensor_filter_state` par le frontend (à mentionner comme consommateur dans `docs/API.md` si la section liste les usages)

### Champs WebSocket
- Aucun

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-settings.md` — carte « Signaux capteurs » : nouvelles lignes dispersion, sémantique du tiret, renommage du Δ
#### Docs transverses
- `CHANGELOG.md` — entrée 2.20.7
#### Docs immuables
- Aucune

## Critères d'acceptation
- [ ] Filtration en marche et fenêtre pleine : la ligne « Dispersion pH » affiche la même valeur que `GET /debug/sensor_filter_state → ph.dispersion` (vérif croisée curl), avec le seuil `0.5` visible
- [ ] Filtration arrêtée : la ligne affiche un tiret ET un libellé explicite — pas « stable », pas `0`
- [ ] `unstable: true` simulable → l'état INSTABLE est visuellement distinct
- [ ] Le Δ historique est renommé et toujours présent
- [ ] Les stats reconstruites (médiane · max fenêtre 13) sur une trace de campagne concordent avec la valeur route à ±10 % en régime établi
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous

## Plan de test manuel
1. Filtration en marche, onglet Avancé : les lignes dispersion pH/ORP affichent valeur + seuil + « stable » ; `curl /debug/sensor_filter_state` donne la même valeur.
2. Filtration arrêtée (porte fermée) : tiret + libellé « pas de jugement » ; recharger la trace ne change rien.
3. Appareil injoignable au moment du fetch état-filtre (couper le WiFi après chargement de la page) : lignes « indisponible », graphes et stats de trace intacts.
4. Trace avec transitoire de redémarrage de filtration (saut > 1,0 pH) : le max reconstruit N'EST PAS le saut (fenêtres écartées), la valeur route reste la référence affichée.

## Notes pour agent
- Le piège sémantique central est documenté dans `docs/API.md` §« Marge d'instabilité (v2.20.6) » : `unstable: false` sans données ne veut pas dire « stable ». La carte ne doit pas recréer ce piège.
- Réutiliser les patterns existants du fichier (`authFetch`, `_setTraceStat`, classes CSS de pills/badges existantes) — pas de nouveau framework de rendu.
- `latch_cycles` et `clean_streak` sont disponibles mais NON demandés sur la carte — ne pas surcharger (hors périmètre).

## Hors périmètre
- Toute modification firmware (la route v2.20.6 suffit)
- Affichage de `clean_streak` / progression de l'auto-levée du latch
- Le badge `unstable` sur la page principale ou la carte pH (autre surface, autre spec si souhaité)
- Historisation de la dispersion
