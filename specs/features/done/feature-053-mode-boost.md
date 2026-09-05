# Feature-053 — Mode Boost (surchloration temporaire du jour)

## Métadonnées
- **ID** : feature-053
- **Slug** : mode-boost
- **Type** : feature
- **Statut** : done (tests verts, revue OK, v2.18.0 — 2026-07-06)
- **Priorité** : normale
- **Version cible** : 2.18.0
- **Créée** : 2026-07-06 (design validé par l'utilisateur)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **pool-chemistry OBLIGATOIRE (double passage)**
- [x] Filtration (forçage temporaire)
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] Frontend (`data/`)
- [x] Historique/persistance (état boost persisté)

## Objectif

Après une forte fréquentation, permettre d'assainir davantage la piscine **pour la journée**, d'un geste, via deux leviers physiques : **turnover** (filtration prolongée) et **résidu chloré plus élevé** (cible ORP relevée). Sans jamais relâcher les garde-fous de sécurité — le Boost a son propre plafond dur.

## Design validé (surcouche temporaire non destructive)

Un état **Boost** activable d'un geste, **auto-expirant au prochain minuit local** (réutilise `tickDailyRollover`, feature-039). Tant qu'il est actif :

1. **Filtration forcée en marche** (turnover maximal) jusqu'à expiration. Via un chemin de forçage DÉDIÉ au boost (indépendant du `forceOn` utilisateur et de son `kForceTimeoutMs`), pour ne pas être annulé par un changement d'horaire ni expirer avant le boost.
2. **Cible ORP effective relevée** : `effectiveOrpTarget = min(orpTarget + kBoostOrpDeltaMv, kBoostOrpCeilingMv)`. La régulation ORP **automatique** existante injecte naturellement vers cette cible — aucun nouveau chemin d'injection. (En mode ORP Manuel/Programmé, le Boost n'étend que la filtration.)
3. **Limite journalière chlore effective relevée** : `effectiveMaxChlorine = min(maxChlorineMlPerDay × kBoostDailyFactor, kBoostDailyHardCapMl)`. Donne un effet réel tout en restant **borné en dur**.
4. **Tous les autres garde-fous INCHANGÉS** : limite horaire, temps min d'injection, anti-rafale, injection uniquement si filtration active, watchdog.

### Surcouche non destructive
Le Boost ne modifie PAS la config persistée (cible, limites, modes). Il expose des valeurs *effectives* consommées par la régulation/filtration. À l'expiration → retour automatique au comportement normal, rien à « remettre ».

### Persistance
`boostActive` (bool) + `boostUntilEpoch` (time_t, prochain minuit local) persistés → survit à un reboot dans la journée. Au boot : si `now >= boostUntilEpoch`, boost inactif.

### Expiration
Dans `tickDailyRollover` (minuit local) : si boost actif → le désactiver + log info. Défaut : **jusqu'au prochain minuit** (budget = 1 jour calendaire, trivialement borné, jamais à cheval sur 2 fenêtres de compteur journalier). Edge assumé : activé tard le soir = boost court cette nuit (bénéfice = filtration nocturne) ; ré-activable le lendemain.

## Bornes à figer par pool-chemistry (double passage)
- `kBoostOrpDeltaMv` (proposé +60 mV) et `kBoostOrpCeilingMv` (≤ 900, borne dure ORP existante).
- `kBoostDailyFactor` (proposé 1,5×) et `kBoostDailyHardCapMl` (plafond absolu).
- Confirmer que relever la limite journalière UNIQUEMENT pendant le boost, borné en dur + auto-expirant, est sûr (vs. maintenir la limite normale).
- Confirmer l'interaction avec l'anti-rafale, le temps min d'injection, la garde filtration.

## Comportement / API / IHM
- **HTTP** : `POST /boost/start`, `POST /boost/stop` (auth WRITE) — ou champ dans /save-config (à trancher architect). État dans `/get-config` + WS (`boost_active`, `boost_until`).
- **WS sensor_data** : `boost_active` (bool), `boost_remaining_s` (ou epoch d'expiration) pour affichage.
- **MQTT/HA** : `switch` « Boost » (état actif + auto-off à minuit), miroir des autres commandes. Topic `{b}/boost` + `.../boost/set`.
- **UI** : bouton/switch « Boost » (écran Dosage ou dashboard) avec état actif + heure d'expiration + **rappel « vérifier le taux avant baignade »** au déclenchement.
- **Constantes** : les 4 `k*` ci-dessus dans `constants.h`.

## Hors périmètre
- Détection automatique de fin de surchloration / niveau baignable (le contrôleur ne mesure pas le chlore libre ppm).
- Durée configurable en heures (v1 = jusqu'à minuit ; extensible plus tard).
- Boost côté écran LVGL.

## Critères d'acceptation
- [ ] Activer Boost → filtration forcée en marche, cible ORP effective +Δ (plafonnée), limite journalière effective ×facteur (plafonnée). *(logique effective couverte par tests natifs + revue ; forçage filtration à confirmer on-target)*
- [ ] Auto-expiration à minuit → retour intégral au comportement normal (cible, limite, filtration selon config). *(à confirmer on-target)*
- [x] Aucun garde-fou dur relâché hors la limite journalière boostée (elle-même plafonnée) ; horaire/anti-rafale/min-time/filtration-active/watchdog intacts. *(pool-chemistry GO pré + post ; code-reviewer Approved ; le Boost n'ouvre aucun chemin de dosage)*
- [ ] État boost persisté (reboot dans la journée) ; expiré si dépassé au boot. *(logique persistance + boot vérifiée en revue ; à confirmer on-target)*
- [ ] Boost visible UI + HA avec expiration ; rappel baignade affiché. *(implémenté ; à confirmer on-target)*
- [x] pool-chemistry GO (pré + post), code-reviewer Approved, `pio run` 0 warning, tests natifs verts (logique effective cible/limite en pur si extractible), `build_fs.sh` OK.

## Notes d'implémentation (2026-07-06, v2.18.0)

**Design livré** — surcouche temporaire **non destructive** « valeurs effectives » :
- État `boostState` = `{ bool active; time_t untilEpoch; }` persisté en **NVS dédié** (namespace `poolctrl`) ; survit à un reboot dans la journée ; au boot, inactif si `now >= untilEpoch`.
- **Filtration forcée** via chemin dédié `boostForce` prioritaire dans `decideFiltrationRun`, indépendant de `forceOn`/`kForceTimeoutMs`, dérivé de `isBoostActive()` → ré-appliqué au boot.
- **Cible ORP effective** = `min(orpTarget + kBoostOrpDeltaMv, kBoostOrpCeilingMv)`, jamais abaissée. **Limite journalière chlore effective** = `min(maxChlorineMlPerDay × kBoostDailyFactor, kBoostDailyHardCapMl)`.
- Effet chlore **strictement gaté au mode ORP `automatic`** via fonctions pures `effectiveOrpTargetPure` / `effectiveMaxChlorinePure` ; Manuel/Programmé = filtration seule ; injection manuelle bornée à la limite normale.
- **Expiration** : `tickBoostExpiry()` appelée **avant** le reset des compteurs journaliers, **dans les 2 branches** de `tickDailyRollover()`.

**4 bornes figées par pool-chemistry** : `kBoostOrpDeltaMv = +60 mV`, `kBoostOrpCeilingMv = 850 mV` (< alerte `orp_abnormal` 900), `kBoostDailyFactor = 1,5×`, `kBoostDailyHardCapMl = 1000 mL`.

**5 conditions pool-chemistry (pré) couvertes** : (1) relèvement limite borné en dur + auto-expirant ; (2) cible plafonnée sous l'alerte ORP, jamais abaissée ; (3) effet chlore uniquement en `automatic` ; (4) anti-rafale / min-time / garde filtration inchangés (aucun nouveau chemin d'injection) ; (5) activation refusée sans heure synchronisée.

**Exposition** : `POST /boost/start` (auth WRITE, `409 time_not_synced`) | `/boost/stop` ; champs WS `boost_active` + `boost_until` (epoch) ; `/get-config` ; switch HA « Boost » (`{base}/boost` + `/set`) ; carte Boost sur le dashboard (expiration + toast rappel « vérifier le taux de chlore avant baignade »).

**Verdicts** : pool-chemistry **GO** (pré : 4 constantes figées + 5 conditions ; post : GO) ; code-reviewer **Approved** (1 mineure concurrence appliquée) ; **199/199 tests natifs** (+14 boost) ; `pio run` **0 warning** ; `build_fs.sh` OK.

**Mineure concurrence corrigée** : ordre d'écriture de `boostState` — `untilEpoch` écrit **avant** `active`, pour qu'un lecteur inter-core ne voie jamais `active == true` avec un `untilEpoch` non encore initialisé.

**Décision structurante** : [ADR-0025](../../docs/adr/0025-mode-boost.md) (surcouche « valeurs effectives » + relèvement borné de la limite chlore).

**Reste à valider on-target** : activer Boost → filtration ON + cible/limite relevées ; expiration à minuit → retour normal ; reboot avec heure non synchronisée ; mode manuel = filtration seule ; entité HA + carte UI (état + expiration + toast).
