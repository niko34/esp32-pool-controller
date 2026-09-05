## Métadonnées
- **ID** : feature-033
- **Slug** : reduction-seuil-resync-filtre
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.2.2 (correctif) ou 2.3.0 — à confirmer
- **Dernière mise à jour** : 2026-06-21
- **Spec(s) liée(s)** : `specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md` (chaîne de filtrage + re-sync), correctif latch-up 2.2.1

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — modifie un paramètre du chemin d'entrée de la régulation (re-sync du filtre = quand le lissé suit un changement réel)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Réduire le délai de re-synchronisation du filtre pH/ORP pour que la valeur lissée suive un **changement réel et durable** plus rapidement (gel ramené de ~2 min à ~1 min), sans toucher au reste de la chaîne de filtrage ni au rythme d'échantillonnage.

## Contexte
Le filtre feature-025 conclut à un « vrai changement » (et non un pic à éliminer) après `kSensorFilterResyncRejects = 24` rejets consécutifs sur saut excessif, soit **24 × 5 s = 120 s**. En pratique (constaté en calibration : sonde déplacée d'une solution pH 7 vers pH 4), la valeur affichée et utilisée par la régulation reste **figée ~2 min** avant de rattraper la nouvelle valeur.

Analyse menée (2026-06-21) :
- Le bruit de lecture réel est faible (~0,07 pH crête-à-crête, < `maxStep` = 0,15) → le rejet de saut et le moyennage médiane+EMA fonctionnent correctement en usage normal. **Aucune refonte de la chaîne n'est nécessaire.**
- Le gel de 2 min n'apparaît **jamais en bassin** (le pH/ORP réel ne saute pas brutalement) ; c'est un artefact des manipulations de banc / calibration.
- Le seul ajustement justifié est donc de **raccourcir le délai de re-sync**, pour le confort en calibration, sans dégrader la robustesse anti-bruit.

Sécurité : la réduction est sans risque pour le dosage car dès `kSensorFilterMaxConsecutiveRejects = 10` rejets consécutifs le capteur est déjà marqué `unstable()` → dosage bloqué **avant** la re-sync ; et pendant le re-warmup qui suit la re-sync, `ready()=false` → dosage toujours bloqué.

## Comportement attendu
- Après un changement réel et durable > `maxStep`, le filtre se re-synchronise après **~12 lectures** (≈ 60 s à 5 s/cycle) au lieu de 24 (~120 s).
- Le seuil de re-sync reste **STRICTEMENT supérieur** au seuil d'instabilité (`kSensorFilterMaxConsecutiveRejects`) : un capteur réellement instable est flaggé `unstable` (et bloque le dosage) **avant** toute re-sync.
- Un pic isolé (quelques lectures) reste rejeté sans modifier le lissé (inchangé).
- L'anti-boucle latché (`kSensorFilterMaxResyncPerWindow` re-sync dans la fenêtre → `unstable` latché) reste actif et inchangé.
- (Optionnel) Exprimer les constantes du filtre en **base temps** (ms) plutôt qu'en compteurs d'échantillons, pour découpler le réglage du rythme d'échantillonnage et clarifier l'intention (médiane ~30 s, re-sync ~60 s, warmup ~25 s).

### Cas limites
- Changement réel durable (pH 7 → pH 4) : re-sync après ~60 s, dosage bloqué pendant divergence + re-warmup.
- Pic isolé < seuil de re-sync : toujours rejeté, lissé inchangé.
- Bruit EMI en boucle : doit toujours atteindre `unstable` puis le latch anti-boucle (inchangé).
- Calibration réussie / `POST /debug/sensor_filter_reset` : reset complet (warmup), inchangé.

## Comportement actuel
- `kSensorFilterResyncRejects = 24` (≈ 120 s). Re-sync trop lente en calibration → impression de gel.

## Contraintes techniques
- ESP32 — modification de constantes uniquement (et logique de dérivation si base temps).
- **Ne pas** modifier `maxStep`, la chaîne médiane+EMA, ni le rythme d'échantillonnage (`kPhOrpSensorIntervalMs = 5000`).
- Conserver l'invariant : `kSensorFilterResyncRejects` STRICTEMENT > `kSensorFilterMaxConsecutiveRejects`.
- Pas d'allocation dynamique ; comportement déterministe (tests native existants).
- Respecter `kConstants` ; valeurs centralisées dans `constants.h`.

## Fichiers potentiellement concernés
- `src/constants.h` — baisse de `kSensorFilterResyncRejects` (24 → valeur validée pool-chemistry) ; éventuelles constantes temps si base temps retenue [modif]
- `src/sensor_filter.cpp` — uniquement si passage en base temps (dérivation compteur = durée / intervalle) [modif optionnel]
- `test/test_native/test_sensor_filter.cpp` — ajuster les tests au nouveau seuil [modif]
- `docs/subsystems/sensors.md` — nouveau délai de re-sync documenté [doc]
- `CHANGELOG.md` — entrée correctif [doc]

## Nouvelles signatures / API (optionnelle)
- Constantes : `kSensorFilterResyncRejects` (valeur réduite) ; si base temps : `kSensorFilterResyncDelayMs`, `kSensorFilterMedianWindowMs`, etc. (dérivés en compteurs depuis `kPhOrpSensorIntervalMs`).
- Aucune API publique nouvelle.

## Sécurité chimique (obligatoire)
- Dosage impliqué : oui — la re-sync conditionne quand le lissé (entrée régulation) suit un changement.
- Limites journalières / horaires concernées : non modifiées.
- Filtration requise avant action : inchangé.
- Calibration capteur modifiée : non.
- Algorithme PID touché : non (seul le timing de mise à jour de l'entrée filtrée change).
- Risque identifié : un seuil trop bas pourrait faire suivre une perturbation moyenne-durée au lieu de la rejeter. **Mitigation** : rester > seuil `unstable` (10) ; dosage bloqué pendant divergence (≥10 rejets) ET pendant re-warmup. **pool-chemistry doit fixer/valider la valeur retenue** (proposition : 12).

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun (ajustement de paramètre couvert par l'ADR-0016 régulation existant)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun (comportement de timing seulement)

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/sensors.md` — délai de re-sync mis à jour (et base temps si retenue)

#### Docs transverses
- `CHANGELOG.md` — entrée correctif (re-sync plus rapide)

## Critères d'acceptation
- [ ] AC1 — Après un changement réel et durable > `maxStep`, le filtre se re-synchronise après ~12 lectures (≈ 60 s), vérifié en test native (`now` simulé).
- [ ] AC2 — `kSensorFilterResyncRejects` reste STRICTEMENT > `kSensorFilterMaxConsecutiveRejects` (assertion ou test de cohérence).
- [ ] AC3 — Un pic isolé (< seuil re-sync) reste rejeté, lissé inchangé (non-régression).
- [ ] AC4 — Le dosage reste bloqué pendant toute la divergence (≥ seuil unstable) et le re-warmup (`ready()=false`).
- [ ] AC5 — L'anti-boucle latché reste fonctionnel (non-régression).
- [ ] `pio run` compile sans nouveau warning ; `pio test -e native` tous verts.

## Plan de test manuel
1. Calibration : sonde pH 7 → pH 4, mesurer le délai avant que le live rattrape (~60 s attendu).
2. Vérifier qu'un pic isolé simulé ne déplace pas le lissé.
3. Vérifier qu'un capteur instable (rejets continus) atteint `unstable` et bloque le dosage avant re-sync.

## Notes pour agent
- Changement **minimal** : ne pas élargir le périmètre à la refonte de la chaîne ou à l'échantillonnage rapide (option étudiée puis écartée faute de besoin terrain — bruit réel ~0,07 pH bien géré).
- `pool-chemistry` fixe la valeur finale du seuil (proposition 12) et confirme l'invariant > seuil unstable.
- Si la base temps est retenue, garder le comportement identique à rythme d'échantillonnage inchangé (5 s) — c'est une clarification, pas un changement fonctionnel.

## Hors périmètre
- Refonte de la chaîne de filtrage (médiane/EMA/maxStep) — non justifiée (bruit faible).
- Découplage échantillonnage capteur / push web et lecture EZO non bloquante — option B étudiée, **écartée** faute de besoin terrain démontré (à rouvrir uniquement si des relevés montrent un bruit résiduel que le filtre actuel ne peut absorber sans latence).
- Changement du rythme d'échantillonnage `kPhOrpSensorIntervalMs`.
