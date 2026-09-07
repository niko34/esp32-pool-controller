# bug-082 — `flow_assist` absent des trames WebSocket (5ᵉ récidive du motif de parité)

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug — parité `/data` ↔ WebSocket |
| Statut | **livré en 2.47.1 — tests verts, revue OK** (2026-09-07) |
| Priorité | ~~Moyenne — fausse une estimation affichée, sans effet sur le dosage réel~~ → **relevée : `aborted` est un garde-fou de dosage** (visa `pool-chemistry`, voir [« Ce que la spec sous-estimait »](#ce-que-la-spec-sous-estimait--le-visa-chimique-a-releve-la-gravite-dun-cran)) |
| Découvert | 29/08/2026, en auditant les autres blocs après un bug d'affichage |
| Version en cours | 2.26.0 (déployée) à la découverte — corrigé en **2.47.1** |
| Transit | ⚠️ Cette spec est passée **directement de `todo/` à `done/`** le 2026-09-07 : elle n'a jamais séjourné dans `doing/`. C'était l'un des points bloquants de clôture relevés en revue, consigné ici plutôt que masqué. |

## Domaines touchés

- `src/ws_manager.cpp` — objet `flow_assist` à ajouter à la trame `sensor_data`
- `data/app.js` — `flow255For()` et l'assistant de mesure
- `test/test_native_ws_data_parity/` — second axe (imbriqué) de la sentinelle de parité
- `docs/subsystems/ws-manager.md`, `docs/API.md`, `docs/features/page-settings.md`

## Sécurité chimique

**Visé par `pool-chemistry`.** Le verdict a **inversé** la lecture initiale de la
spec : l'objet ne porte pas seulement une estimation d'affichage, il porte
`aborted`, qui est un **garde-fou de dosage**. Voir la section suivante.

## Le défaut

L'objet `flow_assist` — qui porte notamment `flow255`, le débit majorant de
flotte — n'existait que dans `GET /data`. Il était **absent des trames WebSocket**.

Or l'interface tourne **quasi exclusivement au WebSocket** : `latestSensorData`
est **remplacé en entier** à chaque trame, sans fusion.
`latestSensorData.flow_assist` redevenait donc `undefined` quelques secondes
après chaque chargement, et `flow255For()` retombait **en permanence** sur son
repli — **61 / 67 mL/min au lieu des 74 servis** depuis la v2.25.2.

Conséquence visible : la durée d'injection annoncée était calculée avec un débit
inférieur à celui que le firmware applique réellement, et la ligne de budget de
l'assistant annonçait **« ≈ 61 mL par purge »** pour **74 mL réellement
servis** — un volume **sous-estimé de 18 %** que l'opérateur validait.

Ironie du repli : il est correct **en soi** (61/67 sont bien les valeurs des
firmwares antérieurs à la fusion) — c'est son déclenchement permanent qui était
fautif, pas sa valeur.

## Ce que la spec sous-estimait — le visa chimique a relevé la gravité d'un cran

La ligne « Priorité : moyenne, sans effet sur le dosage réel » était **fausse**.

1. **`aborted` n'est pas un affichage, c'est un garde-fou.** C'est le drapeau
   « mesure de débit **interrompue par le serveur** » (perte de filtration), lu
   par `saveMeasuredFlow()` pour **refuser** de router une mesure avortée vers la
   saisie du volume. Il était **mort en pratique** : la trame WS écrasait
   `latestSensorData` avant la saisie.
2. **Un débit mesuré sous-estimé agit DEUX FOIS DANS LE MÊME SENS.**
   - `planPulse` ([`src/pulse_dosing_logic.cpp:45`](../../../src/pulse_dosing_logic.cpp)) :
     `pulseMs = periodMs × meanFlow / ampFlow` → un `ampFlow` trop petit
     **allonge le pouls**, donc **sur-injection réelle** ;
   - `pulseDeltaVolumeMl` ([`:161`](../../../src/pulse_dosing_logic.cpp)) crédite
     le plateau au **même** `ampFlow`, donc `dailyInjectedMl` **sous-compte**
     d'autant et la garde `maxDailyMl` est **desserrée du même facteur**.

   **La sécurité volumique se ment à elle-même** : le même chiffre faux allonge
   la marche et minore ce qu'on en compte.
3. **Le filet en aval n'est que partiel** : les bornes de saisie n'acceptent que
   `0 < flow ≤ 200`, donc **12 mL/min au lieu de 26 passe sans bruit**.
   L'avertissement de vraisemblance 20-35 mL/min
   ([`data/app.js:9617`](../../../data/app.js)) **n'est pas bloquant** — le
   rendre bloquant est une **recommandation du visa, hors périmètre de ce
   correctif**, laissée ouverte (voir [Reste ouvert](#reste-ouvert--consigné-non-traité)).

## Le motif, cinquième occurrence

| # | Version | Champ servi d'un seul côté | État |
|---|---|---|---|
| 1 | v2.21.2 | pentes de sonde (`phSlope*`) absentes de `/data` | corrigé |
| 2 | v2.23.0 | 6 champs de calibration absents de la trame config WS | corrigé |
| 3 | v2.25.0 | causes de refus (`ph`/`orpDoseBlockedReason`) absentes de `/data` | corrigé |
| 4 | v2.25.2 | miroir client `FLOW_255_ML_PER_MIN` en dur (variante du motif) | corrigé |
| **5** | **v2.26.0 → 2.47.1** | **`flow_assist` absent des trames WS** | **corrigé (cette spec)** |
| — | v2.46.0 | `ph`/`orp_pump_flow_uncharacterized` `/data`-seuls — **attrapés par la sentinelle pendant son propre cycle** | corrigé avant livraison |

La leçon est déjà écrite en mémoire projet : *un champ servi d'un seul côté ne
disparaît pas franchement, il **clignote*** — et le défaut se manifeste par
intermittence, précisément quand le réseau flanche.

Ici la variante était plus vicieuse : le champ ne clignotait pas, il était
**structurellement absent** de la source dominante. Le repli fonctionnait donc
en permanence, et le défaut était **invisible** — aucune valeur manquante,
aucune erreur, juste un chiffre faux.

**Une 6ᵉ occurrence de ce motif est désormais détectable en CI** (sentinelle
`test_native_ws_data_parity`, [ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md))
au lieu d'être découverte au bord du bassin.

## Ce qui a été livré (v2.47.1)

### Deux champs en parité, et deux seulement

`aborted` (bool) et `flow255` (**tableau de 2 éléments**, jamais un scalaire —
un scalaire retomberait **silencieusement** sur le repli client) sont ajoutés à
la trame `sensor_data` avec les **mêmes règles null/arrondi** que `/data`.
Receveur nommé **`faObj`** des **deux** côtés, exprès : la sentinelle extrait les
champs imbriqués avec **un seul extracteur textuel**, qu'un nom différent
casserait.

Les **sept champs de session** (`active`, `pump`, `phase`, `remaining_s`,
`purges_left`, `purge_s`, `measure_s`) restent **`/data`-seuls** : ils ne
pilotent l'écran de l'assistant que **pendant** une session. Contrepartie
obligatoire livrée avec eux : **un `GET /data` forcé à l'ouverture de
l'assistant** (`openFlowWizard()` devient `async`). Sans ce rafraîchissement,
`purge_s` / `measure_s` / `purges_left` restaient adossés à des **miroirs en dur
60 / 120 / 3** — c'est-à-dire le **4ᵉ cas du même motif de parité**, réintroduit
par le correctif du 5ᵉ. La carte s'ouvre **avant** la requête (jusqu'à 10 s de
timeout) : l'attente est signalée sur la ligne de budget et « continuer » est
neutralisé le temps de la réponse.

### Garde préventive sur `window._flowWizardOnData`

Le hook est appelé **aussi sur les trames WS**, et ne survivait **que** parce que
`flow_assist` en était absent. Il sort désormais tôt sur `fa.active === undefined`
(« clé absente ≠ session finie »). Sans cette garde, toute publication future
d'`active` dans la trame lirait **chaque trame** comme une expiration de session :
retour à l'étape 0 et purges relancées, **éventuellement pompe en marche**.
Défense en profondeur assumée, **sans effet visible aujourd'hui**, validée en
revue.

### Le repli 61/67 reste distinct de 74 — deux motifs, verrouillés par un test

- sur **firmware ancien**, la pompe débite réellement 61 / 67 : annoncer 74
  **raccourcirait l'estimation de 18 à 21 %** sous la marche réelle → l'utilisateur
  voit la pompe tourner « trop longtemps », en déduit un défaut et **relance** —
  la sur-injection vient de cette **répétition**, pas du premier pouls ;
- sur **firmware à jour**, aligner le repli sur 74 rendrait la **présence** du
  champ **indiscernable de son absence** : ce bug serait invisible pour toujours
  et la 6ᵉ récidive muette. **Le repli distinct EST le détecteur.**

C'est asserté : `test_p_le_repli_client_flow255_reste_distinct_de_la_constante_firmware`.

### La sentinelle bénissait le bug — et s'est corrigée elle-même

`flow_assist` était déclarée **exception `/data`-seule légitime** dans la
sentinelle de parité livrée **le matin même** ([ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md)) :
la garde censée attraper cette classe de défaut **validait celui-ci**. Erreur de
l'agent principal, consignée telle quelle.

Au moment où le firmware a corrigé la parité, la sentinelle est passée **rouge
d'elle-même** : son mécanisme « **exception morte = échec** » s'est déclenché
pour la **première fois**, et son message disait quoi faire (« SUPPRIMER l'entree
de kDataOnly »). `kDataOnly` passe de **3 à 2** entrées.

Elle a ensuite gagné un **second axe imbriqué** : table `kFlowAssistDataOnly`
(7 champs de session, **motif écrit obligatoire**, **cliquet à 7**), receveur
`faObj` lu dans les deux fichiers, et **interdiction formelle d'y inscrire
`aborted` ou `flow255`**. Cas (j) à (p), 16 cas au total dans la suite.

> **Propriété vérifiée par contre-mesure en revue** : retirer `aborted` de la
> trame, **le déclarer** en exception **et relever le cliquet** produit **trois**
> échecs. **On ne peut donc plus détourner la garde pour légitimer le bug** —
> c'est la leçon transférable de ce cycle : une table d'exceptions n'est une
> garde que si ses propres entrées sont, elles aussi, sous contrainte.

### Budget de trame — méthode appliquée ([ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md))

**Pire cas : 70 o.** Obtenu par **balayage exhaustif des 2³¹ motifs de bits** de
`float` finis ≥ 0, sérialisés par l'ArduinoJson des `libdeps` du projet :
longueur maximale **12 o** (`1.40129846e-45`) — ce n'est pas « 9 chiffres
significatifs + point » (10 o), ArduinoJson en rend **7**, mais l'exposant reprend
les octets économisés. Le même balayage restreint à la borne fonctionnelle du
débit (`0 < f ≤ 200`) donne **le même** résultat. Décompte : `"flow_assist":{`
15 o + `"aborted":false,` 16 o + `"flow255":[…,…]` 37 o + `},` 2 o = **70 o**.
**50 o en marche** (`74` = 2 o par élément) — valeur **exacte**, non estimée :
`kPumpFlow255MlPerMin` est une `constexpr float`.

Trame : **3447 + 70 = 3517 o**, gabarit documentaire porté **3456 → 3584**,
**marge 67 o** sous le seul plafond dur `kWsFrameBufBytes = 3584` (elle était de
137 o). Jugée acceptable ; la **réserve de 49 o** (`ph_integral` +
`ph_integral_term`, sans consommateur client depuis feature-092) **n'est pas
dépensée**.

> ⚠️ **Le 3447 hérité n'a PAS été re-dérivé** dans ce cycle : il est repris du
> décompte v2.40.0. C'est exactement le genre d'héritage qui a produit **quatre
> chiffrages faux** pendant feature-092 — dit ici plutôt qu'absorbé.

## Critères de clôture

- [x] `flow_assist` présent dans les deux sources, parité prouvée par **test**
      (sentinelle, extraction textuelle des deux fichiers) et non par relecture.
- [x] L'assistant de mesure et l'estimation de durée affichent le débit
      réellement utilisé par le firmware (74 mL/min à ce jour).
- [x] Le repli 61/67 reste en place pour les firmwares antérieurs, **et n'est
      plus jamais atteint** sur un firmware à jour — assertion dédiée.
- [x] Taille de trame vérifiée, aucune troncature : 3517 o pour un plafond dur de
      3584 o, marge 67 o, méthode consignée.
- [x] Mécanisme de prévention de la divergence **implémenté** : second axe
      imbriqué de la sentinelle, cliquet, motifs obligatoires, deux champs
      non exceptables.

## Reste ouvert — consigné, non traité

- **[`bug-119`](../todo/bug-119-loadsensordata-ne-resout-rien.md) — la seconde
  mort du même garde-fou.** `loadSensorData()` ne résout **rien** (aucun
  `return json` dans tout `data/app.js`), donc `fa` est **toujours nul** à
  `data/app.js:9505` et **trois** comportements de `verifyRunEnd()` sont morts.
  Le garde-fou `aborted` avait **deux morts indépendantes** : ce lot en corrige
  **une** (il passe par `latestSensorData`, mis à jour par effet de bord),
  `bug-119` corrigera l'autre.
- **Vraisemblance 20-35 mL/min non bloquante** : recommandation du visa
  `pool-chemistry`, hors périmètre. Aujourd'hui, 12 mL/min au lieu de 26 passe
  sans bruit (seules les bornes `0 < flow ≤ 200` refusent).
- **Réserve de 49 o** (`ph_integral`, `ph_integral_term`) : chiffrée, refusée le
  07/09/2026, disponible pour une future feature à l'étroit — voir
  [ws-manager.md](../../../docs/subsystems/ws-manager.md#élagage-chiffré-non-appliqué--et-pourquoi-il-a-été-refusé).

## Fichiers modifiés

- [`src/ws_manager.cpp`](../../../src/ws_manager.cpp) — objet `flow_assist`
  (`faObj`), gabarit `StaticJson<3584>`, bloc de budget commenté
- [`data/app.js`](../../../data/app.js) — `flow255For()` (avertissement console
  une fois sur forme inattendue), `openFlowWizard()` en `async` + `GET /data`
  forcé, garde `fa.active === undefined` sur `_flowWizardOnData`
- [`test/test_native_ws_data_parity/test_ws_data_parity.cpp`](../../../test/test_native_ws_data_parity/test_ws_data_parity.cpp) — second axe imbriqué (cas j à p)
- [`src/version.h`](../../../src/version.h) — 2.47.1
- Docs : `docs/subsystems/ws-manager.md`, `docs/API.md`,
  `docs/features/page-settings.md`, `docs/features/page-ph.md`,
  `docs/features/page-orp.md`, `docs/subsystems/pump-controller.md`,
  `docs/BUILD.md`, `CHANGELOG.md`
