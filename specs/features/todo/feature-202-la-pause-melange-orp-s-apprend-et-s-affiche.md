## Métadonnées
- **ID** : feature-202
- **Slug** : la-pause-melange-orp-s-apprend-et-s-affiche
- **Type** : feature
- **Statut** : todo
- **Priorité** : moyenne
- **Version cible** : 2.98.0
- **Dernière mise à jour** : 2026-11-19
- **Spec(s) liée(s)** : [feature-085](../done/feature-085-dose-de-base-apprise-feedforward.md) (le patron d'apprentissage à copier) · [feature-025](../done/feature-025-lissage-mesures-ph-orp-pid.md) (la pause créée) · [bug-200](../done/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md) (le principe de visibilité qui s'applique ici)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP / WebSocket
- [x] Frontend (`data/`)
- [ ] MQTT / Home Assistant

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry *(⚠️ **deux passages** : P1 sur le DESIGN — plafond et critère de plateau — avant tout code ; P2 sur le DIFF)*
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Après une injection de chlore, l'appareil s'interdit de réinjecter pendant une **pause mélange**
de **20 minutes**, identique sur tous les bassins. Sur ce bassin, l'effet d'une injection met
**~88 minutes** à se produire entièrement. La pause autorise donc une seconde injection alors que
la première n'a exprimé qu'un quart de son effet — c'est un empilement de doses.

Cette fiche fait **apprendre** cette durée à l'appareil, à partir de ce qu'il observe sur son
propre bassin, et la rend **visible**.

## Contexte
Demande de l'utilisateur du 2026-11-19, après le diagnostic de l'injection du 2026-11-18.

### Les chiffres relevés sur l'appareil de production (2026-11-18)
| Fait | Valeur |
|---|---|
| Déficit au départ | 479 mV mesurés pour 530 de consigne → **51 mV** |
| Fin de l'injection | 09:59:55, arrêtée par le **plafond horaire**, pas par l'atteinte de la cible |
| Volume injecté | **456 mL** |
| Mesure au sommet | **585 mV à 11:28** |
| ⭐ Durée de montée **après la fin** de l'injection | **~88 min** |
| Pause mélange en vigueur | **20 min** (`kOrpMixingDelayMs`, `src/constants.h:2312`) |

⇒ À 10:20 l'appareil se croyait autorisé à réinjecter. Si la mesure était restée sous la consigne
à cet instant — ce qui était le cas —, il l'aurait fait **par-dessus une dose encore en train de
monter**.

⚠️ **Ce que cette fiche NE corrige PAS** : le dépassement du 18/11 vient de la **taille de la
dose** (456 mL pour un besoin d'environ 220), pas de la pause. La pause n'a fait qu'éviter que ce
soit pire. L'apprentissage du rendement (mV par mL) est un **levier distinct**, à traiter
séparément. ⛔ Ne pas laisser croire que ce lot supprime les dépassements.

### Décisions déjà arrêtées par l'utilisateur (2026-11-19) — ⛔ ne pas les rouvrir
1. **Les noms actuels sont conservés.** « Pause mélange » reste « Pause mélange », et le bloc
   « Dernière pause » des réglages reste tel quel. ⛔ Aucun travail de vocabulaire dans ce lot.
2. **L'apprentissage ne peut qu'ALLONGER la pause**, jamais la raccourcir sous les 20 minutes
   d'aujourd'hui.
3. **La durée en vigueur doit être affichée.**

## Comportement attendu

### 1. Observer — ⚠️ au moment où ça se passe, PAS dans l'historique
À la fin de chaque session d'injection ORP, l'appareil ouvre une **observation** : il note la
mesure filtrée à l'instant de l'arrêt, puis la suit jusqu'à ce qu'elle **cesse de monter**. La
durée écoulée est l'observation du jour.

⛔ **IMPOSSIBLE de reconstruire cette courbe depuis l'historique, et c'est vérifié** :
`src/history.h:79` fixe `RAW_MAX_AGE = 21600` — les points à 5 minutes ne vivent que **6 heures**,
après quoi ils deviennent des **moyennes horaires** (`HOURLY_MAX_AGE`, 15 jours). Une montée de
88 minutes s'y réduit à deux points. ⇒ L'observation se fait **en direct**, et seul le **résultat**
est persisté — exactement le choix de feature-084 (`src/ph_persist_logic.h`), qui persiste un
instantané compact en NVS plutôt que de réinterroger l'historique.

⚠️ **Une observation ne compte que si elle est propre.** Elle est **jetée** si, pendant la fenêtre :
la filtration s'est arrêtée · une seconde injection (pH **ou** ORP) a eu lieu · une attente eau
neuve s'est déclenchée · la mesure est devenue non représentative. ⇒ Le projet a déjà été trompé
par une observation contaminée (matin du 05/09, air dans le circuit) : **mieux vaut une journée
manquante qu'une journée fausse**, règle déjà écrite dans `src/time_to_target_logic.h`.

### 2. Apprendre — médiane, pas moyenne
La durée retenue est la **médiane** des observations propres sur une fenêtre de **7 jours**, avec
un **minimum de 3 jours** d'historique avant de servir à quoi que ce soit. C'est l'idiome du
dépôt : `kPhIntegralRestoreHistoryWindowDays = 7` / `kPhIntegralRestoreMinHistoryDays = 3`
(`src/constants.h:2606-2607`) et `kTimeToTargetHistoryDays = 7` (`:3891`).

⇒ **Sous 3 jours d'observations, le comportement est RIGOUREUSEMENT celui d'aujourd'hui** : 20 min.

### 3. Appliquer — un seul point de code, et une borne qui ne regarde que vers le haut
    duréeEnVigueur = min( max( médiane, kOrpMixingDelayMs ), kOrpMixingDelayLearnedMaxMs )

⭐ **Le câblage est minuscule, et c'est vérifié** : la pause n'est armée qu'à **deux endroits** —
`src/pump_controller.cpp:545` (pH) et `:549` (ORP). La logique pure
(`src/dosing_window_logic.h:298`) reçoit **déjà** la durée en **paramètre** (`fullMixingDelayMs`),
alimentée par un tableau à `src/dosing_window.cpp:96`. ⇒ **Rien à changer dans la logique de
fenêtre** : seule la valeur qu'on lui passe change.

⛔ **Ce lot ne touche QUE la voie ORP.** La pause pH (15 min, `kPhMixingDelayMs`) reste
**strictement inchangée** : son terme intégral est verrouillé dessus par des `static_assert`
(`src/constants.h:2966`, `:3063`, `:3094`, `:3804`, `:3859`). ⚠️ La rendre variable casserait ces
preuves — c'est un autre chantier, pas celui-ci.

⚠️ **Pourquoi cette exclusion est écrite noir sur blanc dans une fiche qui parle de chlore** : les
deux pauses sont **jumelles dans le code**. Elles sont armées à **quatre lignes d'écart**
(`pump_controller.cpp:545` et `:549`) et transmises **dans le même tableau** —
`const uint32_t fullMixing[2] = {kPhMixingDelayMs, kOrpMixingDelayMs};`
(`src/dosing_window.cpp:96`). ⇒ Rendre « la pause mélange » apprise **sans préciser laquelle**
mènerait naturellement à les changer **toutes les deux**. L'exclusion n'est pas un rappel de
chimie : c'est une **barrière contre un dégât collatéral**.

ℹ️ ⛔ **Et ce n'est pas en contradiction avec le fait que le pH apprend déjà** (feature-085) : le pH
apprend **sa DOSE** (combien d'acide par jour), il n'apprend **pas sa pause**. Deux grandeurs
distinctes, et seule la seconde est verrouillée.

### 4. Afficher — la contrepartie obligatoire
⭐ **Le principe du projet s'applique directement** : *quand l'appareil s'écarte de son automatisme
nominal, il le dit sans qu'on ait à le chercher.* Une pause qui passe silencieusement de 20 à
60 minutes refabriquerait le défaut de bug-200.

L'écran doit donner, **sans geste** :
- la **durée en vigueur** et son origine : « réglage d'usine » ou « observée sur votre bassin » ;
- sur combien d'observations elle repose.

⛔ Le compte à rebours, lui, **existe déjà** et ne change pas : `orp_mix_remaining_s` est servi sur
`/data` (`src/web_routes_data.cpp:631`) **et** sur la trame WS (`src/ws_manager.cpp:1351`), par le
**même accesseur** `getMixingRemainingS(1)`. ⇒ Le champ **neuf** est la *durée en vigueur*.

### Cas limites
- **Moins de 3 observations propres** ⇒ 20 min, et l'écran dit « réglage d'usine ».
- **Redémarrage** ⇒ la valeur apprise survit (NVS). Une observation **en cours** est perdue, jamais
  comptée à moitié.
- **Changement de produit ou de consigne** ⇒ ⛔ les observations antérieures ne valent plus.
  Patron existant : l'« empreinte de contexte » de `PhIntegralSnapshot` (`src/ph_persist_logic.h`).
- **La mesure ne se stabilise jamais** (dérive, fuite, bassin en déséquilibre) ⇒ l'observation est
  **abandonnée** au plafond, ⛔ jamais enregistrée comme « très longue ».
- **Médiane sous 20 min** ⇒ écrêtée à 20. ⛔ Et ce n'est **pas** une anomalie à signaler.
- **Mode maintenance / dosage suspendu** ⇒ aucune observation.

## Comportement actuel
La pause vaut 20 minutes sur tout bassin, n'apparaît nulle part dans les réglages, et rien ne
mesure si elle correspond à la réalité de l'installation.

## Contraintes techniques
- **ADR-0017** : toute la décision dans un module **pur** (`src/orp_mixing_learn_logic.h` attendu) —
  ⛔ pas de `millis()`, pas de `time()`, pas de NVS, pas d'`Arduino.h` ; le temps est un
  **paramètre**. `pump_controller.cpp` ne fait que le câblage.
- ⚠️ **Parité `/data` ↔ WebSocket sur l'EXPRESSION, pas seulement sur la clé** — 4ᵉ récidive connue,
  la 4ᵉ forme était **invisible au grep**. Les deux chemins appellent **le même accesseur**.
- **Budget de trame WS** : documenté à `src/ws_manager.cpp:1484`. Le **vérifier**, ⛔ ne pas
  l'affirmer.
- **ADR-0082** : aucune allocation sur le chemin de journalisation.
- **ADR-0100** : ⛔ le navigateur ne recalcule **jamais** la durée ni son origine depuis un seuil
  local — il **lit** ce qui est servi.
- ⚠️ **RESPONSIVE** : 390 px d'abord.
- ⚠️ **Usure NVS** : une observation par injection, ⛔ pas une écriture par échantillon. Patron de
  cadence : R5 de `ph_persist_logic.h`.

## Fichiers potentiellement concernés
- `src/orp_mixing_learn_logic.h` *(neuf — logique pure)*
- `src/pump_controller.{h,cpp}` — observation, accesseur unique, armement à `:549`
- `src/dosing_window.cpp:96` — passer la durée apprise au lieu de la constante
- `src/constants.h` — `kOrpMixingDelayLearnedMaxMs` + `static_assert` de bornes
- `src/config.cpp` — persistance NVS de l'instantané
- `src/web_routes_data.cpp`, `src/ws_manager.cpp` — le champ neuf, **même expression**
- `data/app.js`, `data/index.html`, `data/app.css`
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/subsystems/ws-manager.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Module pur `orp_mixing_learn_logic.h`, compilable en natif
- [ ] A2 — Observation ouverte/fermée/**jetée** selon les conditions de propreté
- [ ] A3 — Médiane 7 j, minimum 3 j, persistée avec empreinte de contexte et CRC
- [ ] A4 — Écrêtage **vers le haut uniquement**, appliqué à `pump_controller.cpp:549`
- [ ] A5 — ⛔ `kPhMixingDelayMs` **inchangé**, `static_assert` pH tous verts
- [ ] B1 — Durée en vigueur + origine + nombre d'observations servies dans `/data`
- [ ] B2 — **Même champ, même expression** dans la trame WS
- [ ] B3 — Affichage sans geste sur la page ORP, lisible à 390 px
- [ ] C1 — Doc : API, ws-manager, pump-controller, page-orp, guide de régulation, CHANGELOG
- [ ] D1 — Test de parité `/data` ↔ WS sur l'**expression**
- [ ] D2 — ⭐ **Mutation exigée** : retirer l'écrêtage bas (autoriser < 20 min) doit **tuer** un test
- [ ] D3 — ⭐ **Mutation exigée** : retirer le champ d'**un seul** des deux chemins doit **tuer** un test
- [ ] D4 — ⭐ **Mutation exigée** : accepter une observation contaminée doit **tuer** un test
- [ ] D5 — Vecteurs natifs : < 3 observations ⇒ sortie **exactement** 20 min
- [ ] E1 — Essai réel sur l'appareil de production : la durée affichée converge vers la réalité
  observée (repère du 18/11 : ~88 min)

## ADR attendu ?
**Oui** — premier paramètre de **sécurité de dosage** dont la valeur est **apprise** plutôt que
fixée. Le trade-off à consigner : *pourquoi l'apprentissage ne peut jouer que dans le sens
prudent*, et *pourquoi la pause pH reste figée alors que l'ORP ne l'est plus*.

## Éléments visibles (pour doc-writer)
### Champs WebSocket
- Durée de pause mélange ORP en vigueur + origine + profondeur d'observation *(noms à arrêter)*
### Endpoints HTTP concernés
- `/data` — mêmes champs, **même expression**
### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/subsystems/pump-controller.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html` + `./tools/build_user_pdfs.sh`,
  `CHANGELOG.md`, ADR
- 📸 **Étape humaine** : capture de la page ORP montrant la durée apprise

## Sécurité chimique
- **Dosage impliqué** : **oui** — la pause conditionne l'autorisation de réinjecter
- **Limites concernées** : ⛔ **aucune n'est modifiée**. Plafond horaire, plafond journalier,
  nombre de cycles, présence d'eau, watchdog : tous **intacts**
- **Filtration requise** : sans objet
- **Calibration capteur modifiée** : non
- **Algorithme PID touché** : non — ⛔ la voie pH, seule porteuse d'un terme intégral, est **hors
  périmètre**
- **Sens de l'écart** : ⭐ l'apprentissage ne peut que **rendre l'appareil plus patient**. Au pire
  il injecte **moins souvent** qu'aujourd'hui, ⛔ jamais plus

### ⭐ Pourquoi le mécanisme se corrige TOUT SEUL dans le bon sens (2026-11-19)
Ce que l'observation mesure, c'est **le moment où la mesure cesse de monter**. Or en forte
consommation — eau chaude, fréquentation, soleil — le chlore est **consommé pendant qu'il se
disperse** : la courbe s'aplatit **plus tôt** et **plus bas**. ⇒ **Plus la saison est exigeante,
plus la durée apprise est COURTE**, donc plus vite écrêtée au plancher de 20 min.

⇒ ⭐ **Le cas « l'appareil devient trop patient » ne peut PAS se produire quand le bassin a
réellement besoin de chlore.** Il se produit en **basse saison** — eau froide, demande faible,
montée longue et propre — c'est-à-dire précisément quand le bassin n'a **pas** besoin de la
cadence maximale. ⚠️ En pleine saison, le mécanisme **dégénère vers le comportement
d'aujourd'hui**, ce qui est exactement le repli souhaitable.

### Le chiffrage du débit maximal, puisqu'il était l'argument du plafond
| | Pause 20 min (aujourd'hui) | Pause 88 min (valeur observée le 18/11) |
|---|---|---|
| Cycle complet (10 min d'injection + pause) | 30 min | 98 min |
| Temps de pompe maximal | **non contraignant** — le plafond horaire (10 min/h) borne avant | ~**6,1 min/h** — la pause devient contraignante |
| Pour atteindre le plafond journalier (2 000 mL ≈ **44 min** de pompe au débit relevé de 45,6 mL/min) | ~4,4 h de filtration | ~**7,2 h** de filtration |

⇒ Même à 88 minutes, le plafond journalier reste **atteignable dans une fenêtre de filtration
normale**. ⚠️ **Le plafond de la durée apprise n'est donc PAS un frein saisonnier** — c'était une
inquiétude mal orientée, retirée le 2026-11-19 après chiffrage.

### ⚠️ Points à trancher par `pool-chemistry` AVANT tout code (passage P1)
1. **Le plafond `kOrpMixingDelayLearnedMaxMs`.** ⛔ **Ce n'est PAS une protection saisonnière**
   (voir le chiffrage ci-dessus) : c'est un **garde-fou contre une observation ABERRANTE** — bassin
   en déséquilibre où la mesure ne plafonne jamais, fuite, sonde morte, dérive. ⇒ Il peut donc être
   posé **largement** (ordre de grandeur : 2 à 3 h) sans rien coûter au dosage. ⚠️ Ce qu'il faut
   trancher, c'est **ce que l'appareil FAIT quand le plafond est atteint** : jeter l'observation en
   silence, ou le **dire** — une mesure qui ne se stabilise jamais est une information utile sur
   l'état du bassin, pas un simple raté de calcul.
2. **Le critère de plateau** : à partir de quelle variation, sur quelle fenêtre, considère-t-on que
   la mesure « ne monte plus » ? ⚠️ Le bruit de mesure ORP de cette installation est connu et
   documenté — le critère doit être au-dessus.
3. **La fenêtre de 7 jours est-elle la bonne échelle ?** ⚠️ La grandeur apprise est
   **hydraulique** (volume du bassin, débit de filtration, point d'injection) : elle **ne varie
   pas** d'un jour ni d'un mois à l'autre. ⇒ Converger lentement n'est pas un défaut ici — ça
   converge **une fois**, et ça tient.
   ⚠️ **Mais compter en JOURS est trompeur** : il n'y a **qu'une observation par injection**, et le
   18/11 l'appareil n'a injecté **qu'une seule fois**. « 3 jours » peut donc vouloir dire
   « 3 observations ». ⇒ Trancher si le seuil se compte en **jours** ou en **observations
   propres** — et, si c'est en jours, dire ce qu'on fait d'une journée sans injection.

## Hors périmètre
- ⛔ L'apprentissage du **rendement** (mV par mL) — levier distinct, fiche séparée
- ⛔ Toute modification de la pause mélange **pH**
- ⛔ Tout renommage dans l'interface (décision utilisateur du 2026-11-19)
- ⛔ Le compteur de stock de chlore (`orp_total_injected_ml` > `orp_container_ml` constaté le
  18/11 : le bidon n'a pas été remis à zéro au dernier remplissage) — défaut réel, **autre sujet**
