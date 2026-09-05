# Bug — plancher pH incompatible avec un offset légitime + bascule de sécurité désarmée en silence

- **Type** : bug critique (sécurité chimique) — DEUX défauts distincts, même cycle
- **Priorité** : critique — un verrouillage de sécurité s'annule tout seul, et la régulation pH ne peut pas atteindre sa cible
- **Statut** : **CLÔTURÉE le 2026-08-27** — livré, tests verts (**723/723**), revue
  Approved, gate `pool-chemistry` GO ×3. Le seul critère restant est devenu
  **invérifiable** : il portait sur le plancher asservi à l'offset, mécanisme
  **supprimé** par la refonte des seuils (v2.25.0, ADR-0048 réécrit). Voir
  « Note de clôture » en fin de document.
- **Créée** : 2026-08-25, diagnostic on-target
- **Version cible** : 2.25.0
- **Domaine** : Régulation pH (`pump_controller`, plancher brut), config/API, frontend
- **Specs liées** : feature-067 + ADR-0039 (offset pH), v2.20.3 C4/C7 (plancher + trace de bascule), feature-074 (précédent : champ retiré du payload global)

## Incident déclencheur (2026-08-25, journal on-target)

```
[07:23:23] INFO: [Config] Bascule automatique pH → manuel acquittée par l'utilisateur
           (trace effacée) : 3 refus de plancher pH en 24 h — sonde pH à recalibrer
           ou remplacer ; régulation pH basculée en manuel
[07:23:23] INFO: Mode régulation pH changé: automatic
[07:23:23] INFO: Configuration mise à jour via interface web
[07:23:23] INFO: Démarrage dosage pH (auto): pH=7.62 cible=7.20 erreur=0.422 (cycle 1/24)
```

L'utilisateur **a seulement consulté l'application** — aucune action délibérée. La bascule de sécurité a été annulée et sa trace effacée dans la même seconde.

## Défaut n°1 — le plancher élargi punit un offset LÉGITIME (impasse arithmétique)

`isPhRawFloorBlocking()` (`src/pump_controller.cpp:395-424`) refuse l'injection d'acide quand le pH **brut** passe sous un plancher. Palier élargi si `|offset| > kPhOffsetWideThreshold` (0,30) : `kPhRawDosingFloorWide = 6,90` (`constants.h:615-617`). Trois refus en 24 h (`kPhRawFloorRefusalMax`) → bascule `manual` fail-closed.

Installation de référence : offset **+0,50**, cible **7,20**.

| | brut | affiché (brut + 0,50) |
|---|---|---|
| Plancher élargi actuel | 6,90 | **7,40** |
| Cible utilisateur | 6,70 | 7,20 |

**La cible est 0,20 SOUS le plancher : elle est inatteignable par construction.** Chaque approche de la cible déclenche un refus ; trois refus ferment la régulation. Confirmé par l'historique du 24/08 : pH affiché à 7,30 (= 6,80 brut) à 15h39, 16h09, 16h54, 17h24, 18h10.

**L'hypothèse « sonde à recalibrer » du message est FAUSSE dans ce cas**, et c'est le cœur du défaut :
- Diagnostic EZO on-target : pente acide **101,0 %**, base **100,0 %**, **zéro −1,03 mV**. Un décalage interne de 0,50 pH donnerait un zéro à ≈ −29 mV (59 mV/pH). **La sonde est saine.**
- L'offset compense un décalage **externe** : le potentiel de l'eau déplacé par les **courants de fuite de la pompe de filtration** (mécanisme établi par la campagne de bruit 22-23/08 : dispersion ×5 pompe en marche, asymétrie pH/ORP cohérente avec un couplage électrique via le verre à ~100 MΩ, test sur batterie de l'utilisateur).
- L'offset a été établi avec **trois instruments indépendants** (PoolLab, réactif colorimétrique, pH-mètre autonome) — feature-067/ADR-0039, hors périmètre.

Le palier élargi part du principe « offset large ⇒ incertitude large ». Quand l'offset est une **correction mesurée d'un décalage systématique**, la prémisse tombe.

**Correctif demandé par l'utilisateur** : `kPhRawDosingFloorWide` 6,90 → **6,70**.

**Réserve à trancher par `pool-chemistry`** : à 6,70 brut, le blocage tombe **pile sur la cible** (7,20 affiché). Un dépassement d'inertie après injection (l'acide continue d'agir après l'arrêt de la pompe) repasserait sous le plancher → refus → possible re-bascule. Le gate doit dire si 6,70 suffit, s'il faut un cran plus bas (6,60 ? 6,50 = 7,00 affiché), ou si la garde doit changer de nature (ex. plancher sur le pH CORRIGÉ avec une borne dure sur |offset|, sachant que le brut a été choisi pour rester insensible à un offset ERRONÉ — cf. commentaire `sensors.cpp` « insensible à un offset erroné »). **Le gate arbitre ; l'utilisateur a demandé 6,70.**

## Défaut n°2 — la bascule de sécurité s'annule à la simple consultation

Le firmware est correctement protégé (`web_routes_config.cpp` : n'acquitte que sur `modeChanged` réel, revue 2.20.3 bloquant n°1). Le navigateur a donc bien envoyé `ph_regulation_mode: "automatic"` alors que l'appareil était en `manual`.

Cause côté client — `collectConfig()` (`data/app.js:1876-1877`) :

```js
ph_enabled: (window._config?.ph_regulation_mode || "automatic") !== "manual",
ph_regulation_mode: window._config?.ph_regulation_mode || "automatic",
```

**Toute sauvegarde globale réémet le mode, avec un repli EN DUR sur `"automatic"`** si `window._config` est absent ou périmé (onglet resté ouvert, réveil d'onglet, config pas encore chargée). Le mode a pourtant son **propre chemin de sauvegarde immédiate** (`app.js:7602`, payload `{ph_regulation_mode}` sur clic) : sa présence dans le payload global est inutile et dangereuse.

Quatrième occurrence du même patron cette semaine (limites journalières, `max_cycles_per_day` remis à 48, débits de pompe, et maintenant une **sécurité chimique**). Précédent de correctif : feature-074 a retiré `pumpN_max_duty_pct` des deux payloads de sauvegarde.

**Non déterminé** (assumé) : le déclencheur exact du POST — les logs ne journalisent ni le corps ni l'origine de la requête. Un onglet périmé est le candidat le plus probable.

## Correctifs attendus

### A — Plancher (sécurité chimique, gate obligatoire EN AMONT)
- [x] A1 — **Plancher CONTINU asservi à l'offset** (raffinement post-revue, arbitrage gate) : `phRawFloorFor(offset) = max(kPhRawFloorAbsolute, kPhRealFloorTarget − offset)` = `max(6,50 ; 7,00 − offset)`. Les trois constantes du modèle à paliers (`kPhOffsetWideThreshold`, `kPhRawDosingFloor`, `kPhRawDosingFloorWide`) sont **supprimées**. Commentaire réécrit : l'offset large peut être une correction LÉGITIME d'un décalage externe (courants de fuite), la pente/zéro EZO permettant de distinguer sonde saine et sonde dérivée.
- [x] A5 *(ajouté au raffinement)* — **Bornes de consigne pH** `kPhTargetMin` = 7,20 / `kPhTargetMax` = 7,60, identiques `ph_minus`/`ph_plus`, appliquées sur les **trois** chemins (HTTP 400, refus MQTT + resync HA, clamp NVS + `critical`) ; UI bornée + hint ; auto-discovery HA réalignée (`unique_id` inchangé). Correctif annexe : `ph_target` lu **sans conversion explicite** (une chaîne JSON valait 0).
- [x] A6 *(ajouté au raffinement)* — **Invariant garanti** marge ≥ 0,20 pH pour tout offset valide : **4 `static_assert`** + balayage de **4141 couples** en test natif.
- [x] A2 — Message d'escalade corrigé : conditionné au diagnostic EZO (`isPhProbeHealthy`), deux textes dont l'un dit explicitement « la sonde n'est pas en cause » et oriente vers le décalage externe.
- [x] A3 *(ajouté)* — Hystérésis de relâche `kPhRawFloorReleaseHysteresis` = 0,05 pH (plus de rafale de refus sur une mesure oscillante).
- [x] A4 *(ajouté)* — Garde de cohérence `checkPhTargetReachable()` (boot + save-config) : alerte `critical` + MQTT `ph_target_unreachable`.

### B — Désarmement silencieux (fail-closed)
- [x] B1 — Client : `ph_regulation_mode` ET `ph_enabled` retirés du payload de `collectConfig()` (patron feature-074). Le clic sur le segmented conserve son envoi dédié.
- [x] B2 — Firmware : `/save-config` refuse **409** toute sortie du mode manuel sous trace, sans `ph_auto_manual_ack: true` (booléen STRICT). Gate placé avant toute mutation de `mqttCfg`.
- [x] B3 — UI : bouton « Acquitter l'alerte » ajouté à la bannière existante (2.20.3 C4, adaptée et non dupliquée) ; acquitter n'efface que la trace, sans réarmer.
- [x] B4 — Origine (IP) journalisée sur les deux issues d'un `/save-config` qui change le mode de régulation.
- [x] B5 *(ajouté)* — UI : gestion du 409 sur le segmented (revert du mode, toast avec le message serveur, défilement vers la bannière, `loadConfig()`).
- [x] B6 *(ajouté)* — Refus aligné côté MQTT/Home Assistant + resynchronisation HA sur le mode réel ; la trace n'est plus jamais effacée par MQTT.
- [x] B7 *(ajouté)* — `ph_enabled` n'est plus accepté en ÉCRITURE (dérivé pur du mode, toujours exposé en lecture).

### C — Transverse
- [x] C1 — `src/version.h` → 2.25.0
- [x] C2 — Docs : `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/MQTT.md`, `docs/features/page-ph.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`, **[ADR-0048](../../../docs/adr/0048-plancher-ph-continu-et-bornes-de-consigne.md)** créé et indexé (réécrit au raffinement : le modèle à paliers n'est jamais entré dans un commit).
- [x] C3 *(ajouté)* — Spec de suivi tracée : `specs/features/todo/bug-inhibition-orp-seuil-ph-brut-non-asservi.md` (même classe de bug côté chlore).
- [x] D1 — `pio run` 0 warning, `pio test -e native` **723/723** (base 674, +44 puis +5), `./build_fs.sh` OK.

## Sécurité chimique
- Dosage impliqué : OUI (pH−, acide). Le plancher est un garde-fou de dernier recours contre l'injection d'acide dans une eau déjà acide.
- Risque du correctif A : abaisser le plancher réduit la marge de protection si un jour l'offset est saisi à tort. À arbitrer avec la borne existante sur `ph_offset` et le diagnostic de pente EZO disponible.
- Risque du correctif B : aucun (durcissement).
- L'offset (feature-067, ADR-0039) et sa valeur sont **hors périmètre**.

## Critères d'acceptation
- ⊘ **SANS OBJET** (2026-08-27) — « Avec offset +0,50 et cible 7,20 : la régulation
  atteint sa cible sans refus de plancher ; brut min ≥ 6,60, plancher applicable
  6,50 ». Ce critère décrit le **plancher asservi à l'offset**, supprimé par la
  refonte des seuils (v2.25.0). Il n'existe plus de « plancher applicable »
  dépendant de l'offset : le seuil est fixe à 6,70 sur le pH **corrigé**, doublé
  d'une butée à 6,20 sur le **brut**. Le critère équivalent est repris et
  élargi par `specs/features/done/refonte-seuils-ph-fixes-derives.md`.
- [x] Un `/save-config` contenant `ph_regulation_mode: automatic` sans acquittement, alors qu'une trace de bascule existe, est REFUSÉ (409) et la trace survit
- [x] `collectConfig()` n'envoie plus `ph_regulation_mode` ni `ph_enabled` (grep)
- [x] L'acquittement délibéré depuis la bannière fonctionne et efface la trace
- [x] Aucune configuration acceptée ne peut rendre la consigne inatteignable (marge ≥ 0,20 pH pour tout offset valide) — vérifié par 4 `static_assert` + 4141 couples en test natif
- [x] Une consigne hors [7,20 ; 7,60] est refusée en 400 (HTTP), refusée + resynchronisée (MQTT), clampée + journalisée `critical` (chargement NVS)
- [x] Les constantes `kPhOffsetWideThreshold` / `kPhRawDosingFloor` / `kPhRawDosingFloorWide` n'existent plus (grep : 0 occurrence)
- [x] `pio run` 0 warning, natifs sans régression, `build_fs.sh` OK

## Notes d'implémentation (v2.25.0)

### Arbitrage du plancher, en deux temps

**Temps 1 — la valeur.** L'utilisateur demandait `6,70`. Le gate `pool-chemistry` l'a **refusé** : à 6,70 le blocage tombe **pile sur la cible brute** (7,20 affiché − 0,50 d'offset), marge nulle — le premier dépassement d'inertie après injection repasse sous le plancher et relance le cycle de refus. Marge retenue : **0,20 pH**, décomposée en acide « en vol » après arrêt de la pompe (0,10-0,15 pH sur 30-50 m³) + bruit résiduel du filtre (~0,05 pH). GO amont sous **7 conditions**, GO en contre-visite sur **8 points**.

**Temps 2 — la forme (raffinement post-revue).** La première version conservait l'**escalier** en abaissant le palier élargi à `6,50`. Elle a été abandonnée avant tout commit : le défaut n'était pas la valeur d'un palier mais l'escalier lui-même.

- **Discontinuité, pire cas juste AVANT le seuil** : à `offset = 0,30` **pile** (comparaison stricte ⇒ palier normal `6,80`), la marge sous une cible de `7,20` valait `0,10` — moitié du critère de 0,20 retenu par le gate. Le point le plus dangereux du domaine se trouvait donc du côté « offset raisonnable ».
- **Plancher réel incohérent** : le plancher effectivement protégé dans le bassin (`plancher_brut + offset`) montait puis redescendait (`6,80` → `7,10` → `7,00` selon la branche). La grandeur protégée ne peut pas dépendre du côté d'un seuil administratif.

Décision finale : **plancher continu** `max(kPhRawFloorAbsolute, kPhRealFloorTarget − offset)` = `max(6,50 ; 7,00 − offset)`. Le plancher **raisonne** toujours sur le brut (ADR-0039) mais sa **valeur** s'asservit à l'offset ⇒ plancher **réel constant à 7,00**, butée dure `6,50` sur le brut inchangée (à l'offset max `±0,50`, on retombe **pile** dessus — la protection historique est intacte là où elle sert). Offset `NaN` ⇒ traité comme nul.

**Bornes de consigne, indissociables du plancher.** L'invariant de marge ne tient que si la consigne est bornée : `kPhTargetMin` = 7,20 / `kPhTargetMax` = 7,60, **identiques** pour `ph_minus` et `ph_plus` (le produit choisit la pompe, pas la fenêtre de pH). Basse : agressivité liner/joints/métaux, inconfort, consommation de TAC — le gain HOCl de 7,2 → 7,0 ne justifie pas une corrosion permanente, le levier correct est le TAC/stabilisant. Haute : alignée sur `kPhRawOrpInhibit`, au-delà la régulation ORP serait inhibée en permanence. Constat au passage : la consigne n'était bornée **nulle part** côté HTTP, et le chemin MQTT clampait à `[6,0 ; 8,5]` en littéraux magiques — deux portes, deux règles.

**Invariant garanti** : consigne ∈ [7,20 ; 7,60] + plancher réel 7,00 ⇒ marge ≥ **0,20 pH** pour tout offset valide (branche asservie `(7,20 − off) − (7,00 − off)` ; branche butée `(7,20 − 0,50) − 6,50`). **Aucune configuration acceptée ne peut être inatteignable** — vérifié par 4 `static_assert` et 4141 couples en natif. `checkPhTargetReachable()` devient un **détecteur d'anomalie** (NVS hors bornes).

L'alternative « plancher sur le pH **corrigé** + borne dure sur `|offset|` » (évoquée dans la réserve ci-dessus) a été **rejetée par le gate** : elle ferait dépendre le garde-fou de la valeur contre laquelle il protège. Le plancher continu obtient le résultat qu'elle visait (plancher réel constant) **sans** ce prix. Toutes ces alternatives sont consignées dans [ADR-0048](../../../docs/adr/0048-plancher-ph-continu-et-bornes-de-consigne.md), **réécrit** au raffinement plutôt que superseded (il n'était pas encore commité).

### Décision B6 — le chemin Home Assistant

Le refus a été **aligné côté MQTT** (hors demande initiale). Motif : un select HA est un clic **distant** qui n'affiche ni la cause ni le refus, et une automation — ou une simple restauration d'état HA au redémarrage — rejouerait `automatic` en désarmant la sécurité **sans témoin**. Le chemin MQTT refuse et republie l'état réel (resynchronisation HA), sans jamais effacer la trace. L'acquittement est donc **exclusivement web** (recommandation du gate : seul endroit où la cause est affichée).

### Les 2 bloquants de revue (rebouclage unique, puis Approved)

1. **Trou d'authentification préexistant sur toute la route `/save-config`** — `REQUIRE_AUTH` n'était posé que dans le handler de **complétion**, exécuté **après** le handler de corps : un POST anonyme voyait son corps intégralement appliqué (mutations RAM + NVS) avant que le 401 ne parte. Avec le gate d'acquittement, cela devenait un contournement direct d'une sécurité chimique. Contrôle ajouté en **tête du handler de corps** (`saveConfigBodyAuthorized`), sortie **avant** toute écriture dans les maps (sinon nœud orphelin non purgeable, atteignable sans auth). Exemption first-boot conservée ; le rate-limit n'est pas rejoué.
2. **Ordre des mutex** — `checkPhTargetReachable()` peut aboutir à `publishAlert()` → `enqueueOutbound()` qui prend `topicsMutex`. L'imbrication `configMutex` → `topicsMutex` est interdite (l'ordre inverse existe ailleurs). L'appel a été déplacé **après** le `xSemaphoreGiveRecursive(configMutex)`.

Constat collatéral traité dans le même cycle : `publishAlert()` sortait en **JSON invalide** pour les nouveaux messages (~330 caractères contre `kMaxPayloadLen` = 128 o) — troncature UTF-8-safe ajoutée avec suffixe `... (voir journal)`.

### Fichiers modifiés

`src/constants.h`, `src/ph_guard_logic.h` (nouveau), `src/pump_controller.{h,cpp}`, `src/config.{h,cpp}`, `src/main.cpp`, `src/web_routes_config.cpp`, `src/mqtt_manager.cpp`, `src/sensors.h`, `src/version.h`, `data/app.js`, `data/index.html`, `test/test_native_ph_guard/` (nouveau), `test/test_native_ph_offset/test_ph_offset.cpp`.

Tests : **723/723** (base 674 ; +44 au premier passage, +5 au raffinement — dont le balayage des 4141 couples consigne × offset et les cas de bornes de consigne).

## Constats hors périmètre à tracer

1. **L'ORP porte le même patron de réémission** dans `collectConfig()` (`data/app.js:1903-1904` : `orp_regulation_mode` / `orp_enabled` avec repli en dur sur `"automatic"`). Non corrigé ici (périmètre pH), mais le vecteur est identique.
2. **Les refus 400 préexistants sur les cibles journalières pH/ORP ne remontent pas au client** : la réponse posée par le handler de corps est écrasée par celle de la complétion. Le mécanisme `g_configErrorStatus` livré ici fournit désormais le véhicule pour les faire remonter.
3. ✅ **Tracé** — `kPhRawOrpInhibit` (7,60) reste calé sur le pH **BRUT sans asservissement à l'offset** : **même classe de bug**, côté chlore. Avec un offset négatif légitime (−0,30/−0,50), le brut d'exploitation vaut 7,50-8,10 alors que le pH réel est nominal ⇒ **inhibition ORP permanente** (chloration à l'arrêt, signalée par une seule alerte edge-triggered). Signalé par le gate pendant ce cycle, hors périmètre. Spec : `specs/features/todo/bug-inhibition-orp-seuil-ph-brut-non-asservi.md`.

## Hors périmètre
- La fonctionnalité d'offset et sa valeur
- La correction physique des courants de fuite (broche d'équipotentialité / alim linéaire — discussions en cours, hors code)
- Le mécanisme des 3 refus / 24 h lui-même (seuls le plancher et le message changent)

## Note de clôture (2026-08-27)

Spec déplacée en `done/` sans que son dernier critère ait été coché, parce qu'il
est devenu **invérifiable par disparition de son objet**. Justification :

**Ce qui est livré et acquis** (revue Approved, gate GO, 723 → 778 tests) :
- l'acquittement obligatoire du retour en automatique après une bascule
  silencieuse (409 sans acquittement explicite, la trace survit au refus) ;
- `collectConfig()` n'envoie plus `ph_regulation_mode` ni `ph_enabled` — un vieil
  onglet ne peut plus réarmer la régulation à l'insu de l'utilisateur ;
- le **trou d'authentification** sur `/save-config` (corps appliqué avant
  vérification) fermé par `saveConfigBodyAuthorized()` ;
- le bornage de la consigne sur les trois chemins d'écriture (HTTP 400, MQTT
  refus + republication, NVS clamp + journal).

**Ce qui a été remplacé plutôt que vérifié** : le volet « plancher » de cette
spec. Le diagnostic était juste — un plancher asservi à l'offset est
incompréhensible et peut désactiver la régulation — mais le correctif retenu ici
(abaisser le plancher) a été jugé insuffisant et refondu entièrement dans le
cycle suivant. La mesure faite sur l'installation de production a montré pourquoi :
la marge réelle valait **0,025 pH**, soit moins de la moitié de la dispersion du
signal. Le refus ne dépendait pas de l'état de l'eau mais du bruit du filtre.
La refonte porte cette marge à **0,725 pH**.

**Deux constats « hors périmètre » de cette spec sont également résolus** par la
refonte, et leurs specs de suivi sont closes en même temps :
- constat n°3 — l'inhibition ORP calée sur le pH brut non asservi : elle porte
  désormais sur le pH **corrigé** (`kPhOrpInhibit`, 7,60), doublée d'une butée
  brute à 8,10 contre un offset négatif erroné
  (`todo/bug-inhibition-orp-seuil-ph-brut-non-asservi.md` → `done/`) ;
- constat n°2 — les refus 400 qui ne remontaient pas au client : le mécanisme
  `g_configErrorStatus` livré ici est effectivement utilisé par la refonte.

**Reste ouvert, hors de cette spec** : le constat n°1 (l'ORP porte le même patron
de réémission dans `collectConfig()`, `orp_regulation_mode` / `orp_enabled` avec
repli en dur sur `"automatic"` — même vecteur, côté chlore). **Aucune spec ne le
couvre à ce jour.**
