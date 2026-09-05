# Refonte — seuils pH fixes dérivés de la plage de consigne

- **Type** : refonte de sécurité chimique (remplace le volet plancher/bornes de la v2.25.0 NON COMMITÉE)
- **Priorité** : haute — simplifie un mécanisme qui a désactivé la régulation en production le 25/08
- **Statut** : livré — 746/746 tests natifs, `pio run` SUCCESS 0 warning, revue **Approved** (2ᵉ passe)
- **Créée** : 2026-08-26
- **Version cible** : 2.25.0 (la version non commitée est AMENDÉE, pas remplacée)
- **Domaine** : régulation pH/ORP, config/API, frontend
- **Specs liées** : bug-plancher-ph-offset-legitime-et-securite-desarmee (en `doing/`, même version), ADR-0048 (à réécrire), feature-067/ADR-0039 (offset)

## Motif

Le plancher continu asservi à l'offset (livré, non commité) est correct mais **trop complexe pour ce qu'il protège** : formule asservie, deux paliers historiques, seuil de bascule, garde de cohérence, bornes de saisie avec refus 400, 4 `static_assert`, 4141 combinaisons de test. Cette complexité a elle-même produit une désactivation de régulation non anticipée en production.

Décision utilisateur : revenir à des **seuils fixes**, dérivés d'une seule source de vérité (la plage de consigne + une marge), comparés au **pH corrigé** — parce que l'offset est une correction de mesure : une fois appliqué, la valeur affichée EST le pH de l'eau, et un seuil chimique doit porter sur le pH réel.

## Conception retenue

### Constantes (une seule source de vérité)
```
kPhTargetMin      = 7.20      (consigne mini, curseur)
kPhTargetMax      = 7.60      (consigne maxi, curseur)
kPhSafetyMargin   = 0.50      (marge de sécurité basse)
kPhHighMargin     = 0.20      (marge haute — voir asymétrie)
kPhOffsetMin/Max  = ∓0.50     (inchangé)

seuil bas   (pH−) = kPhTargetMin − kPhSafetyMargin            = 6.70   [sur pH CORRIGÉ]
seuil haut  (pH+) = kPhTargetMax + kPhHighMargin              = 7.80   [sur pH CORRIGÉ]
butée dure        = kPhTargetMin − kPhSafetyMargin − kPhOffsetMax = 6.20   [sur pH BRUT]
inhibition ORP    = kPhOrpInhibit = 7.60                      [sur pH CORRIGÉ]
```

### Règles
1. **Seuil bas** : appliqué UNIQUEMENT si `phCorrectionType == "ph_minus"`. Refus d'acidifier sous 6,70 corrigé.
2. **Seuil haut** : appliqué UNIQUEMENT si `phCorrectionType == "ph_plus"`. Refus d'alcaliniser au-dessus de 7,80 corrigé. **Asymétrie assumée** (0,20 et non 0,50) : au-delà de 7,60 la désinfection est compromise et l'inhibition ORP est active ; laisser le pH+ alcaliniser jusqu'à 8,10 reviendrait à autoriser le système à conduire l'eau dans une impasse (chlore inactif, inhibition permanente). 7,60 + 0,20 d'inertie = 7,80.
3. **Butée dure sur le BRUT à 6,20** : dérivée, elle ne peut structurellement JAMAIS mordre avant le seuil bas en usage légitime (6,70 − 0,50 d'offset max = 6,20). Elle ne sert que dans un seul cas : offset saisi à tort. À verrouiller par `static_assert`.
   > Historique : une butée à 6,40 avait été proposée par le gate ; refusée car avec un offset légitime de +0,50 elle équivaut à 6,90 corrigé et bloquerait AVANT le seuil bas de 6,70, rendant celui-ci inopérant et réintroduisant une dépendance à l'offset par la bande.
4. **Inhibition ORP** : passe du BRUT au CORRIGÉ (seuil 7,60 inchangé — la fraction de chlore actif dépend du pH réel). Renommer `kPhRawOrpInhibit` → `kPhOrpInhibit`. Hystérésis de relâche 0,05 (relâche à 7,55).
5. **Hystérésis** de relâche 0,05 conservée sur tous les seuils.
6. **Alerte non bloquante** `warn` à 7,00 corrigé : signal précoce conservé malgré l'abaissement du seuil de blocage (condition du gate).
7. **Compteur des 3 refus / 24 h** : ne compter QUE les refus survenus en mode automatique — une commande manuelle refusée ne doit pas fermer la régulation (condition du gate).

### UI
- Consigne pH : **curseur** borné [7,20 ; 7,60], pas 0,1 (5 positions). Une saisie hors plage devient impossible.
- Offset pH : **curseur** borné [−0,50 ; +0,50], pas 0,1. Arrondi au plus proche à la migration, loggé en `info` (0,47 → 0,50 = 0,03 pH, sous le bruit de 0,05).

## À SUPPRIMER (livré dans la 2.25.0 non commitée)
- `phRawFloorFor()` (plancher asservi), `kPhRealFloorTarget`, `kPhRawFloorAbsolute` dans sa forme actuelle (remplacée par la butée dérivée), `kPhOffsetWideThreshold` (déjà supprimée), `kPhRawDosingFloor`/`Wide` (déjà supprimées)
- `isPhTargetUnreachable()`, `checkPhTargetReachable()`, `kPhTargetFloorMarginMin` et leurs `static_assert` — la marge devient structurelle
- Le refus 400 sur `ph_target` **côté route** : à réévaluer. Le curseur rend la saisie hors plage impossible depuis l'UI, mais l'API et MQTT restent ouverts → **garder une validation serveur** (défense en profondeur), sans la garde de cohérence.

## À CONSERVER (livré dans la 2.25.0 non commitée)
- Acquittement fail-closed 409 + champ `ph_auto_manual_ack` (booléen strict) + refus aligné MQTT/HA + bouton de bannière
- Correctif du trou d'authentification de `/save-config`
- Message d'escalade conditionné au diagnostic EZO (`isPhProbeHealthy`)
- Retrait de `ph_regulation_mode`/`ph_enabled` du payload global, `ph_enabled` dérivé pur
- Troncature UTF-8 de `publishAlert`

## Livrables — check-list
- [x] A1 — Constantes refondues dans `constants.h` (source unique + seuils dérivés + `static_assert` de non-interférence de la butée)
- [x] A2 — `ph_guard_logic.h` : fonctions pures réécrites (seuil selon type de correction, comparaison au corrigé, butée brut, hystérésis)
- [x] A3 — `pump_controller.cpp` : garde bas/haut selon `phCorrectionType`, butée brut, compteur limité au mode automatique, alerte `warn` à 7,00
- [x] A4 — Inhibition ORP sur le corrigé + renommage + hystérésis
- [x] A5 — Suppression de la garde de cohérence et du plancher asservi
- [x] B1 — Curseur consigne 7,2-7,6 pas 0,1 ; curseur offset −0,5/+0,5 pas 0,1 ; arrondi migré et loggé
- [x] B2 — Validation serveur conservée (HTTP + MQTT) en défense en profondeur
- [x] C1 — Docs + ADR-0048 **réécrit** (renommé `0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md`) + CHANGELOG amendé
- [x] D1 — Tests natifs réécrits (**746/746**) : table (offset −0,50/0/+0,50) × (6,19/6,20/6,21/6,69/6,71), brut 6,19/6,21 avec offset max, pH+ à 7,79/7,81, ORP à 7,59/7,61 avec relâche 7,55, compteur non incrémenté en manuel
- [x] D2 — `pio run` 0 warning, natifs sans régression, `build_fs.sh` OK

## Critères d'acceptation
- [x] Avec offset +0,50 et consigne 7,20 : aucun refus de seuil en fonctionnement nominal (marge 0,50 sur le corrigé)
- [x] La butée brut 6,20 ne se déclenche JAMAIS avant le seuil bas pour tout offset ∈ [−0,50 ; +0,50] (vérifié par balayage **et** par `static_assert` écrit en inégalité)
- [x] En `ph_plus`, refus au-dessus de **7,60** corrigé (et non 7,80 — critère amendé, voir Suivi) + butée brute 8,10 ; en `ph_minus`, aucun seuil haut
- [x] Inhibition ORP calculée sur le corrigé
- [x] Consigne et offset non saisissables hors plage depuis l'UI (contrôle segmenté + incrémenteur, aucun champ libre ni curseur) ; API/MQTT toujours validés ; bornes **servies par `/get-config`**, jamais codées dans le JS
- [x] `pio run` 0 warning, natifs verts, `build_fs.sh` OK

## Hors périmètre
- La fonctionnalité d'offset elle-même (feature-067/ADR-0039)
- La bascule après 3 refus (conservée, seul le comptage change)
- Le patron de réémission ORP dans `collectConfig` (spec de suivi distincte)

## Suivi / itérations

### Itération 1 — livré (2026-08-27)

**Statut** : livré — tests verts (746/746 natifs), revue OK (`code-reviewer` **Approved** en 2ᵉ passe), `pio run` SUCCESS 0 warning, flash 96,5 %. Non commité.

**Amendements de la conception initiale** (la section « Conception retenue » ci-dessus conserve les valeurs de cadrage ; celles réellement livrées sont ci-dessous) :

| Élément | Cadrage | Livré | Motif |
|---|---:|---:|---|
| `kPhTargetMax` | `7,60` | **`7,50`** | À 7,60 pile la consigne atteint `kPhOrpInhibit` : chlore inhibé **en permanence** dès que la régulation tient sa consigne (panne sanitaire silencieuse). Verrouillé par `static_assert` via `kPhOrpInhibitMargin`. |
| `kPhHighMargin` | `0,20` | **`0,10`** | À 0,20 le plafond tombait à 7,70, **au-dessus** de `kPhOrpInhibit` : l'appareil s'autorisait à alcaliniser une eau dont il venait de décider que le chlore y était inactif. La règle énoncée par la spec était violée par sa propre valeur. |
| Plafond `ph_plus` | `7,80` | **`7,60`** | Conséquence des deux lignes ci-dessus ; aligne le plafond sur l'inhibition et rend l'invariant de la butée brute haute satisfiable sans constante supplémentaire. |
| Pas de l'offset | `0,1` (curseur) | **`0,10` (incrémenteur)** | Curseur `input[type=range]` écarté : ~25 px par cran sur mobile, sur le réglage le plus sensible de la page. |
| Contrôle de la consigne | curseur 5 positions | **contrôle segmenté 4 positions** | Idem, plus la borne haute abaissée. |

**Ajouts hors cadrage, issus des passes de revue** :

- **Butée brute HAUTE `kPhCeilingRawHard` = 8,10** (contre-visite C3), appliquée **au refus d'alcaliniser** en plus de l'inhibition ORP. Sans elle, un offset `−0,50` erroné laissait le pH+ pousser l'eau au-delà de 8,10 brut pendant que le chlore était inhibé.
- **Séparation surveillance / décision** (contre-visite C1) : `tickPhGuardSurveillance()` / `tickOrpPhInhibitSurveillance()` appelées hors de toute conjonction d'éligibilité. La détection était muette quota épuisé et en mode `manual` — l'état exact où l'escalade laisse l'appareil.
- **R1 / R2 sur l'inhibition ORP** : découplage mémoire d'hystérésis ↔ latch d'inhibition, puis une mémoire **par seuil** + anti-rafale par gravité et par épisode. Deux cas physiques observés (inhibition permanente à chaque démarrage de filtration ; battement d'alertes contradictoires sur un bassin à 8,03-8,12).
- **Parité `/data` ↔ WebSocket** : 4 champs ajoutés à `/data` (`ph/orpDoseBlockedReason`, `ph/orpPhRawGuardBlocked`). 3ᵉ récidive du motif → exigence consignée dans l'ADR.
- **Bornes servies par `/get-config`** (6 champs) au lieu d'être codées dans le JS.
- **Encart + confirmation avant injection manuelle** sous butée brute (l'injection manuelle reste une échappatoire, mais informée).
- **`copyPhDoseBlockedReason(char*, size_t)`** en remplacement des ex-getters `String` (allocation tas dans la tâche `async_tcp`).
- **Suppressions** au-delà du cadrage : `checkPhTargetReachable()`, `isPhTargetUnreachable()`, `kPhTargetFloorMarginMin`, alerte `ph_target_unreachable`. Alertes renommées : `ph_raw_floor` → `ph_guard`, `orp_ph_raw_inhibit` → `orp_ph_inhibit` ; ajouts `ph_low_warning`, `ph_guard_scheduled`.

**Fichiers modifiés** : `src/constants.h`, `src/ph_guard_logic.h` (nouveau), `src/pump_controller.{h,cpp}`, `src/config.{h,cpp}`, `src/mqtt_manager.cpp`, `src/web_routes_config.cpp`, `src/web_routes_data.cpp`, `src/ws_manager.cpp`, `src/sensors.h`, `src/version.h`, `data/{app.js,app.css,index.html}`, `test/test_native_ph_guard/`, `test/test_native_ph_offset/`.

**Docs** : [ADR-0048](../../../docs/adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) réécrit, `docs/subsystems/pump-controller.md`, `docs/subsystems/ws-manager.md`, `docs/API.md`, `docs/MQTT.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/README.md`, `docs/adr/README.md`, `CHANGELOG.md`.

**Reste ouvert** (hors périmètre, tracé) : l'alerte de refus en mode `programmé` (`ph_guard_scheduled`) vit encore sous une conjonction d'éligibilité — même classe que le défaut C1 corrigé, impact faible car la surveillance couvre la détection.
