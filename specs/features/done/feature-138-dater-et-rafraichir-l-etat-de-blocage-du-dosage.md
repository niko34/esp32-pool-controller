# feature-138 — Dater et rafraîchir l'état de blocage du dosage

## Métadonnées
- **ID** : feature-138
- **Slug** : dater-et-rafraichir-l-etat-de-blocage-du-dosage
- **Type** : feature — **observabilité seule**, aucun changement de décision de dosage
- **Statut** : **done** — livré en 2.54.0 le 2026-10-04 (non déployé à la clôture)
- **Priorité** : haute — un relevé périmé a déjà induit un diagnostic faux (voir Contexte)
- **Version cible** : 2.54.0
- **Dernière mise à jour** : 2026-10-04
- **Spec(s) liée(s)** : feature-071 (dispersion exposée) · feature-072 (graphes) · `specs/features/done/feature-097-…` (estimateur de bloc) · bug-135 · feature-139 (option B, plafond de purge — à ouvrir ensuite)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP / WebSocket**
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] **Frontend (`data/`)**

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — **uniquement pour B3** (raccourci vers l'injection manuelle :
      nouveau CHEMIN D'INTERFACE vers `pump_controller`, règle 1 de `CLAUDE.md`).
      Aucune garde, aucun seuil, aucune décision de dosage n'est touchée par cette spec.
- [x] ux-ui-designer — nouvelle surface : bandeau d'état de blocage hors panneau dev
- [x] embedded-developer — durée de blocage côté firmware (A1)
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Rendre lisible, sans ouvrir le panneau développeur, **depuis combien de temps le
dosage est refusé et pourquoi** — et faire en sorte que les indicateurs de
stabilité affichés soient **datés et rafraîchis**, au lieu de rester figés sur un
relevé ancien.

## Contexte

### L'incident qui motive cette spec (2026-10-02)

Après des pluies diluviennes, de la boue a couvert le fond du bassin. Les deux
voies ont été déclarées instables et **tout dosage automatique s'est arrêté** —
chlore compris, au moment précis où la demande en chlore était maximale.

L'utilisateur a rapporté que « le calcul de dispersion est bloqué à 1,012 ».
**C'était faux, et l'erreur venait de l'interface.** Mesure en direct ce jour-là :

```
08:42:00  disp=1.012   rej=6    clean=0     ← valeur affichée
08:42:10  disp=null    rej=0    clean=0     ← filtres réinitialisés (purge)
08:43:10  disp=0.399   rej=0    clean=1
08:44:03  disp=0.402   rej=0    clean=12    ← latch EMI levé
```

La dispersion n'était pas bloquée : elle avait été **recalculée trois fois** en
deux minutes, et le système était en train de sortir de l'impasse tout seul.

### Où est exactement le défaut

⚠️ **Le traitement de `dispersion: null` est CORRECT et ne doit pas être
touché.** `_setDispersionRow()` (`data/app.js:8652`) affiche bien
« — · pas de jugement (circulation arrêtée ou fenêtre en cours de remplissage) ».
Ce piège est documenté dans `docs/API.md §Marge d'instabilité`.

Le défaut est que **`loadFilterState()` n'est jamais rappelée** : ses deux seuls
appelants sont le bouton « Recharger » (`data/app.js:8855`) et l'activation du
panneau (`data/app.js:8877`). La ligne affichée est donc un **instantané sans
date**, qui reste à l'écran des heures après avoir cessé d'être vrai.

### Pourquoi c'est une priorité haute

Le projet a déjà payé cher un chiffre lu sans sa méthode (ADR-0073) et une
métrique qui mesurait la mauvaise chose (cause racine du 15/08/2026). Ici c'est
la troisième déclinaison du même défaut : **un chiffre juste, affiché hors de son
temps**. Il a produit un diagnostic faux chez l'utilisateur comme chez l'agent.

### Ce que l'utilisateur ne pouvait pas voir

Au moment de l'incident, trois verrous indépendants pouvaient refuser le dosage,
et **aucun n'était lisible depuis l'écran principal** :

| Verrou | Déclencheur | Issue de secours |
|---|---|---|
| Porte STAB (`measure_validity_logic`) | étendue des stats de bloc > `kPurgeRecloseRangePh` | plafond `kPurgeCapMs` 90 min → ouverture fail-safe, re-fermeture désarmée |
| Latch EMI (`sensor_filter`) | dispersion > cap sur `kSensorFilterDispersionLatchCycles` fenêtres | auto-levée à `kSensorFilterLatchClearCycles` fenêtres propres |
| Rejets consécutifs | ≥ `kSensorFilterMaxConsecutiveRejects` lectures NaN/hors-plage | levée dès qu'une lecture valide arrive |

Le code de refus EXISTE déjà (`phDoseBlockedCode` / `orpDoseBlockedCode`,
v2.25.1) et il est publié sur `/data` et le WebSocket. **Ce qui manque, c'est sa
DURÉE** — « bloqué » et « bloqué depuis 9 h » n'appellent pas la même réaction.

## Comportement attendu

### A — Firmware : durée du refus

- Le contrôleur expose, par voie, **depuis combien de temps le code de refus
  courant est inchangé**. Remis à zéro à chaque changement de code, y compris
  vers « plus de refus ».
- Grandeur bornée et monotone ; repliement de `millis()` traité explicitement
  (le projet a déjà été mordu par le repliement à 24,8 j).

### B — Interface

- **B1 — Les lignes de dispersion portent leur date.** « relevé il y a 12 s »,
  et un rafraîchissement automatique **tant que le panneau est visible**, arrêté
  dès qu'il ne l'est plus (pas de requête inutile sur un panneau fermé).
- **B2 — Un bandeau d'état de blocage, hors du panneau développeur**, visible
  depuis l'écran principal : la voie concernée, la cause en clair, et la durée.
  Absent quand rien n'est bloqué — pas de bandeau permanent qui s'ignore.
- **B3 — Un raccourci vers l'injection manuelle** depuis ce bandeau, **sans
  présélection de volume** et sans contournement d'aucune garde : il ouvre la
  surface d'injection manuelle existante, rien de plus.

### Cas limites
- WebSocket déconnecté → le bandeau se marque périmé, il ne ment pas.
- Panneau dev fermé puis rouvert → première valeur immédiate, pas d'attente.
- Code de refus qui oscille → la durée repart ; ne pas lisser, ne pas moyenner.
- Dosage jamais tenté depuis le boot → pas de durée inventée, « — ».
- Filtration à l'arrêt → cause attendue et bénigne : ne pas l'alarmer comme une panne.
- Repliement de `millis()` pendant un blocage long.

## Comportement actuel

- `phDoseBlockedCode` / `orpDoseBlockedCode` : publiés, **sans durée**.
- Dispersion : exposée par `/debug/sensor_filter_state`, affichée **une seule
  fois**, sans date, et jamais rafraîchie.
- Aucun indicateur de blocage hors du panneau développeur.
- Aucun chemin depuis un refus vers l'action corrective.

## Contraintes techniques
- ESP32, offline first ; vanilla JS, pas de framework.
- Français dans toute l'UI.
- AsyncWebServer : aucun handler bloquant > 50 ms.
- ⚠️ **Budget d'octets — c'est l'arbitrage central de cette spec, pas un détail.**
  Chiffres **vérifiés le 2026-10-04**, les registres commentés courant jusqu'à
  `src/web_routes_data.cpp:202` et `src/ws_manager.cpp:582` :

  | Charge | Gabarit | Pire cas | Marge | Nature |
  |---|---|---|---|---|
  | `/data` | `StaticJson<2944>` | 2912 o | **32 o** | **documentaire** — aucun plafond dur |
  | WebSocket | `StaticJson<3584>` | 3541 o | **43 o** | **PLAFOND DUR** `kWsFrameBufBytes` (`constants.h:384`) |

  ⚠️ **La trame WS ne peut PAS absorber la paire de champs en l'état.** Décompte
  au pire cas **structurel du type** (ADR-0073 — la borne du type, jamais une
  valeur relevée) :

  ```
  uint32  "phDoseBlockedForS":4294967295,  31 o + orp 32 o = 63 o  > 43  ❌
  uint16  "phDoseBlockedForS":65535,       26 o + orp 27 o = 53 o  > 43  ❌
  court   "phBlockedS":65535,              19 o + orp 20 o = 39 o  ≤ 43  ⚠️ 4 o restants
  ```

  Mode de défaillance en cas de dépassement : le push est **sauté**, jamais
  tronqué — et le pire cas de longueur **coïncide avec l'escalade de sécurité
  pH**, donc la trame se perdrait au pire moment. `ws_manager.cpp` pose lui-même
  la règle : tout champ nouveau exige **un retrait compensatoire OU un
  relèvement de `kWsFrameBufBytes`, « décision d'ADR, pas de routine »**.

  **Trois issues, à chiffrer et trancher par `architect` puis par l'humain** :
  **(a)** retirer `ph_integral` (23 o) + `ph_integral_term` (26 o) de la seule
  trame WS (sans consommateur client depuis feature-092) → marge 92 o, au prix
  d'une exception motivée dans la sentinelle de parité ;
  **(b)** relever `kWsFrameBufBytes` → RAM **statique permanente**
  (`ws_manager.cpp:51`) sur un projet en famine 8 bits (ADR-0082) ;
  **(c)** clés courtes + durée bornée `uint16` → 39 o, et 4 o de marge résiduelle.

  ⚠️ **Ce choix peut retourner le pari « ADR : non » plus bas.** Ne pas le
  trancher en silence.
- ⚠️ **PARITÉ `/data` ↔ WebSocket — trois récidives en trois versions.** Un champ
  publié d'un seul côté ne disparaît pas : il **clignote**, parce que les charges
  `/data` remplacent `latestSensorData`. Prouver la parité par un grep des deux
  côtés, pas par lecture.
- Logique de durée **pure** (ADR-0017), testable nativement, repliement compris.
- Aucune allocation sur un chemin périodique (ADR-0082).

## Fichiers potentiellement concernés
- `src/pump_controller.{h,cpp}` — horodatage du changement de code de refus
- `src/web_routes_data.cpp` — champ de durée + recompte du gabarit
- `src/ws_manager.cpp` — même champ, **parité obligatoire**
- `src/constants.h` — cadence de rafraîchissement, bornes
- un module pur `src/dose_block_duration_logic.{h,cpp}` [nouveau] si la logique le justifie
- `data/index.html` — bandeau (B2), ligne de date (B1)
- `data/app.js` — rafraîchissement sous visibilité, rendu du bandeau, raccourci B3
- `data/app.css` — style du bandeau
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/features/page-*.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Durée du refus courant par voie, exposée, repliement `millis()` traité
- [x] A2 — Champ publié **à la fois** sur `/data` et le WebSocket, parité prouvée par grep
- [x] A3 — Comptabilité d'octets des deux charges mise à jour, marge recalculée et **écrite** (39 o WS / 107 o `/data`, documentaire)
- [x] B1 — Lignes de dispersion datées + rafraîchies tant que le panneau est visible, arrêtées sinon
- [x] B2 — Bandeau de blocage hors panneau dev : voie, cause en clair, durée ; absent si rien n'est bloqué
- [x] B3 — Raccourci vers l'injection manuelle existante, sans présélection ni contournement
- [x] D1 — Tests natifs de la logique de durée : changement de code, retour à « non bloqué », repliement `millis()`, jamais-dosé (`test/test_native_dose_block_duration/`)
- [x] D2 — **Mutation** : le test `test_z_le_harnais_ne_diverge_pas_du_contrat_source` tue explicitement la garde de redatation disparue et l'inversion de l'ordre d'écriture ; `test_repliement_millis_pendant_blocage_long` tue l'ignorance du repliement
- [x] D3 — Vérifié : `_setDispersionRow()` n'est pas touché par le diff ; non-régression du piège `dispersion: null` confirmée et documentée dans `docs/API.md`
- [x] C1 — `docs/API.md` + `docs/subsystems/ws-manager.md`
- [x] C2 — `docs/features/page-dashboard.md` + `docs/subsystems/pump-controller.md`
- [x] C3 — `CHANGELOG.md`
- [x] C4 — `docs/user/src/guide-utilisation.html` mis à jour ; capture à refaire signalée dans le rapport doc-writer (`screenshots/03_Tableau_de_bord.jpg`, à reprendre avec le bandeau visible, et optionnellement une capture dédiée pour la section « Que faire si… »)

## Nouvelles signatures / API
- Champs `/data` **et** WebSocket (nommage à arrêter par l'architect) : durée du
  refus courant, par voie. **Mêmes clés des deux côtés.**
- Aucun endpoint HTTP nouveau. Aucun topic MQTT nouveau.
- Constantes : cadence de rafraîchissement du panneau, borne de durée.

## Sécurité chimique
- **Dosage impliqué : NON.** Aucune garde, aucun seuil, aucun ordre de garde,
  aucune décision de dosage n'est modifié. La spec ne fait que **rapporter** des
  décisions déjà prises ailleurs.
- **Seule réserve, et c'est l'objet du gate `pool-chemistry` : B3.** Un raccourci
  d'interface vers l'injection manuelle est un nouveau chemin vers
  `pump_controller` au sens de la règle 1 de `CLAUDE.md`. À vérifier :
  qu'il n'introduit **aucune présélection de volume**, qu'il passe par la surface
  manuelle existante et donc par `evaluateManualInject()` intégral, et qu'un
  raccourci offert au moment précis où une voie est déclarée non représentative
  n'incite pas à injecter sur une mesure fausse.
- ⚠️ **Fait chimique à porter au dossier du gate** : `evaluateManualInject()`
  (`src/dosing_logic.cpp:404`) ne contient **aucune garde de stabilité** — ni
  porte STAB, ni `unstable()`. C'est délibéré (feature-006) : sur une injection
  manuelle, l'humain juge de la représentativité. B3 rend ce chemin **plus
  accessible**, exactement quand la mesure est déclarée douteuse. C'est le point
  à trancher, pas un détail d'ergonomie.
- Limites journalières / horaires : non touchées. Filtration requise : inchangé.
- Calibration : non touchée. PID : non touché.

## Décisions arrêtées (2026-10-04) — à appliquer, ne plus rouvrir

### Budget d'octets : option **(a) élagage**, choisie par l'utilisateur
- Retirer `ph_integral` (23 o) + `ph_integral_term` (26 o) de la **SEULE** trame
  WS. Vérifié par grep : aucun consommateur dans `data/` (les clés utilisées
  côté client sont `ph_integral_pct`, `ph_integral_saturated` et l'id DOM
  `ph_integral_warning` — trois clés différentes). Elles restent servies sur
  `/data`, MQTT et Home Assistant, où elles sont un contrat externe.
- Pire cas WS : 3541 − 49 + 53 = **3545** sous `kWsFrameBufBytes` 3584 → marge 39 o.
- `/data` : gabarit `StaticJson<2944>` → **`<3072>`**.
- Deux entrées `kDataOnly` dans `test/test_native_ws_data_parity`, motif ≥ 20
  caractères. Vérifié : `kDataOnly` n'a **aucun** plafond (seuls
  `kWsOnlyLiveCeiling = 18` et `kFlowAssistDataOnlyCeiling = 7` en ont).
- **ADR requis** : « La trame WebSocket est un sous-ensemble choisi de `/data`,
  financé par élagage ». Les quatre critères sont remplis.
- ⚠️ **Corriger au passage une affirmation FAUSSE** : `constants.h:383` justifie
  le dernier bump par « +512 o en .bss statique — hors heap 8 bits, aucun impact
  sur la famine mémoire ». C'est inexact : le `.bss` est en DRAM et le tas 8 bits
  n'est que le reliquat de cette DRAM. Le `.bss` ne fragmente pas, mais il réduit
  le total. C'est ce commentaire qui ferait passer l'option (b) pour gratuite.

### Contrat des champs
- Clés **identiques** `/data` et WS : `phDoseBlockedForS`, `orpDoseBlockedForS`.
- Unité : la **seconde**. Valeur `null` quand aucun refus (même convention que
  `phDoseBlockedCode`) — couvre « jamais dosé depuis le boot » sans rien inventer.
- Borne : `kDoseBlockedForMaxS = 86400` (24 h). **C'est le clamp qui fixe le
  budget d'octets (5 chiffres), pas le type C** — à écrire dans le commentaire,
  sinon le prochain lecteur recomptera 10 chiffres. Au-delà, l'UI rend « ≥ 24 h ».

### Horodatage — point d'écriture UNIQUE
- Les **quatre** affectations de `_lastRefusalCode` (`pump_controller.cpp:578`,
  `:656`, `:674`, `:693`) deviennent des appels à un setter privé
  `setRefusalCode(int, uint8_t)`. L'oubli d'un site devient impossible, et la
  revue le vérifie par un grep qui ne doit plus montrer qu'**une** affectation.
- ⚠️ **Invariant d'ordre dans le setter** : poser `_refusalCodeSinceMs` **AVANT**
  `_lastRefusalCode`. Un lecteur async qui tombe entre les deux voit alors
  l'ancien code avec la nouvelle date (durée **sous-estimée**, bénigne), jamais
  le nouveau code avec l'ancienne date — qui annoncerait « bloqué depuis 9 h »
  sur un blocage d'une seconde, exactement le mensonge que cette spec combat.
  C'est l'ordre **inverse** de `logRefusalOnce` pour le texte : le commentaire
  doit dire pourquoi.
- Aucun mutex nouveau : écriture par loopTask seul, lecture alignée par
  async_tcp, atomique sur Xtensa.

### Repliement de `millis()`
- `(uint32_t)(nowMs - sinceMs)` en **non signé**, exact à travers le repliement.
  **Aucune** comparaison `now < since`, aucune branche de rattrapage — ce sont
  elles qui ont produit les défauts passés.
- ⚠️ **La vraie trappe est la sentinelle, pas l'arithmétique.**
  `_sensorFaultSinceMs` utilise `0 = pas d'épisode` (`pump_controller.h:227`),
  qui collisionne avec la milliseconde 0 après boot et après chaque repliement.
  **Ne pas hériter de ce défaut** : la validité est portée par
  `code != kDoseBlockedCodeNone`, jamais par `sinceMs != 0`.

### Module pur
- `src/dose_block_duration_logic.h` [nouveau, **header-only**], idiomatique du
  projet. `doseBlockElapsedS(nowMs, sinceMs, blocked, capS)`. Reste **spécifique**
  à ce besoin : on ne refactore pas les cinq autres occurrences du patron
  « depuis » (hors périmètre), on écrit juste la signature assez générale pour
  qu'une spec ultérieure puisse les y lever.

### Interface
- **Taxonomie déjà existante, ne pas en créer** : `_doseBlockInfo()`
  (`data/app.js:6302`) et `DOSE_BLOCK_CODES` (`:6250-6291`) classent déjà chaque
  code en `normal` / `fault` / `unknown`, alignés 1:1 sur `DoseRefusal`.
- Bandeau `#dose-block-banner` sur le **dashboard**, juste après `#regul-alert`
  → doc cible **`docs/features/page-dashboard.md`**. Un conteneur, **une ligne
  par voie**. Fusion en une ligne « (pH et chlore) » **uniquement** si même code
  ET ton `normal` ; jamais pour `fault` / `unknown`, qu'une panne par voie doit
  rester lisible individuellement.
- Péremption WS : ajouter `#dose-block-banner` à la liste de `_onDisconnect()`
  (`app.js:88-94`) qui pose `.is-stale` — **plus** un texte explicite, l'opacité
  seule n'explique rien à un lecteur d'écran.
- `role="alert"` réservé au ton `fault` ; `role="status" aria-live="polite"`
  sinon. Ne pas sur-alerter sur du bénin.
- Helper d'âge **partagé** (il est déjà écrit deux fois : `app.js:5984-5997` et
  `:6170-6176`), paramétré par préposition — pas de troisième copie.
- B1 : deux horloges distinctes — un tick d'affichage local recalculant l'âge
  depuis un timestamp client, et un intervalle de **fetch** armé/désarmé avec
  `_isPanelDevActive()` (`app.js:8876-8878`) **et** `document.hidden`
  (`visibilitychange`, `app.js:10962`). Les deux, pas l'un : le hash ne change
  pas quand l'onglet passe en arrière-plan. Cadence client `DISPERSION_REFRESH_MS`
  dans `data/app.js` (pas dans `constants.h` : non lisible par le JS, et la
  servir coûterait un champ dans la charge dont le budget est l'objet de la spec).

### B3 — résolution du conflit entre les deux agents
`pool-chemistry` (C6) veut **masquer** le raccourci sur « filtration arrêtée » ;
`ux-ui-designer` veut le masquer sur les causes `fault` et l'afficher sur les
causes bénignes. Les deux se contredisent sur « filtration arrêtée ».

**Règle retenue — l'intersection, la plus prudente :** le raccourci n'est affiché
que si le ton est `normal` **ou** `unknown`, **ET** que la cause n'est pas
« filtration arrêtée ». Le verdict chimique est contraignant ; la restriction du
designer s'y ajoute sans le contredire.

⚠️ **Et le piège que le design a trouvé** : les sections d'injection manuelle ne
sont visibles qu'en `regulation_mode === 'manual'`, et basculer ce sélecteur
**persiste la config et coupe la régulation automatique**
(`app.js:10361-10378` : `ph_regulation_mode: 'manual'`, `ph_enabled = false`).
**Le raccourci ne doit JAMAIS faire ce bascule** — ce serait un changement de
décision de dosage déguisé en commodité d'interface. C'est un **lien de
navigation pur**, comme `#regul-alert-cta`.

## Verdict `pool-chemistry` sur B3 — **GO SOUS CONDITIONS** (2026-10-04)

Conditions **opposables**, à appliquer à la lettre par `web-ui-developer` :

- **C1 — Aucune présélection.** Le raccourci ouvre la surface manuelle avec les
  champs volume/durée **dans leur état par défaut**. Aucun volume, durée, produit
  pré-rempli, ni paramètre d'URL le faisant. Vérifiable par `git diff`.
- **C2 — Confirmation explicite** en plus de celle du bouton d'injection. Un clic
  sur le raccourci ne déclenche **jamais** d'injection : il avertit, puis ouvre.
- **C3 — Libellés imposés mot à mot**, distincts par voie (texte intégral dans le
  rapport du gate, à reprendre sans réécriture). Pour l'ORP : rappeler que
  l'injection manuelle n'applique **aucune** garde de stabilité, exiger un
  contrôle DPD du chlore libre, et avertir qu'en eau chargée l'ORP ne remonte pas
  avant le point de rupture — « si le DPD montre du chlore libre alors que l'ORP
  reste bas, le problème n'est pas un manque de chlore ». Pour le pH : rappeler
  qu'**une injection d'acide ne se reprend pas**.
- **C4 — Asymétrie des deux voies.** Raccourci proposé **par défaut sur l'ORP** ;
  sur le **pH uniquement** si le bandeau concerne cette voie, avec le libellé pH.
  Motif chimique : un excès de chlore se dissipe, un excès d'acide sur un pH déjà
  bas se corrige en déplaçant le TAC. Jamais présenté comme l'action recommandée :
  **pas de bouton primaire**, verbes d'incitation (« Corriger », « Rattraper »)
  **interdits**, libellé imposé **« Ouvrir l'injection manuelle »**.
- **C5 — Traçabilité** : injection journalisée comme toute injection manuelle,
  avec l'utilisateur authentifié. Aucune dégradation de l'existant.
- **C6 — Pas de raccourci sur cause bénigne.** Si la cause du blocage est
  **filtration arrêtée**, le raccourci n'est **pas affiché** : la garde de présence
  d'eau refuserait de toute façon, et le proposer entraînerait l'utilisateur à
  forcer la filtration pour doser.

**Garde de stabilité dans `evaluateManualInject()` : NON exigée et HORS
PÉRIMÈTRE.** L'ordre gelé des 9 gardes reste intact. Rouvrir ce sujet relèverait
d'une spec dédiée avec gate bloquant (à rapprocher du « mode récupération »
listé en Hors périmètre et de feature-139).

**Second passage `pool-chemistry` obligatoire sur le diff réel de B3.**

### Second passage sur le diff réel (2026-10-04) — **GO SOUS CONDITIONS**, corrections appliquées

C1, C2, C4, C5, C6 vérifiés conformes sur le code livré, ainsi que le piège du
mode manuel (le raccourci ne touche ni `setSegmented`, ni `sendConfig`, ni
`ph/orp_regulation_mode` : lien de navigation pur). Deux défauts relevés et
**corrigés** :

1. **Le dialogue mentait sur la cause.** Il ouvre par « La mesure est
   actuellement déclarée non représentative », mais la règle d'exclusion
   autorisait aussi les codes 8 à 14 (mélange, limites journalière/horaire,
   cycles, anti-rafale) et 7 (stabilisation). Sur ces causes le texte était faux
   — et surtout **l'injection manuelle est soumise aux mêmes gardes**
   (`evaluateManualInject`, gardes 3 et 5 à 9) : le raccourci aurait mené à un
   refus, en poussant à relever la limite, précisément le travers visé par C6.
   **Correctif : passage d'une exclusion à une LISTE BLANCHE**
   `DOSE_BLOCK_SHORTCUT_CODES = [3, 4, 5, 15, 18, 19, 22]` — uniquement les
   causes de défiance envers la mesure. Les pannes (16, 17, 20, 21) restent
   écartées par le ton `fault`, et un **code inconnu l'est désormais aussi**,
   ce qui satisfait la recommandation du gate (on ne peut alors pas exclure
   « filtration arrêtée »).
   ⚠️ **Toute addition à cette liste exige un nouveau passage du gate.**
2. **Erreur de réactif sur la voie pH** : « Contrôlez le pH au réactif (DPD… » —
   le **DPD dose le chlore libre, pas le pH**. L'erreur venait du verdict de
   premier passage, corrigée à la source : **« rouge de phénol ou photomètre »**.

⚠️ **Rappel issu de ce cycle** : les libellés C3 avaient d'abord été livrés
**tronqués** (perte de l'avertissement sur les chloramines, de « n'augmentez pas
la dose », de « pas parce que l'eau serait correcte » et du rappel des limites).
Ils sont restaurés mot à mot et le code porte un commentaire interdisant de les
reformuler sans repasser le gate.

## ADR attendu ?
- **Tranché** : **oui** — pari initial « non » retourné par le choix de l'option
  (a) (élagage de `ph_integral`/`ph_integral_term` de la seule trame WS).
  Critères 2 et 3 remplis : la décision contraint tout champ WS futur (financé
  par retrait, pas par défaut) et n'est pas lisible dans le code sans son
  historique de chiffrage.
- **ADR créé** : [`docs/adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md`](../../docs/adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md) — « La trame WebSocket est un sous-ensemble choisi de `/data`, financé par élagage ».
- Le gate `pool-chemistry` a donné **GO SOUS CONDITIONS** sur B3 (conditions C1-C6, cf. ci-dessus) — pas de refus à consigner.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `/data` — champ de durée ajouté (aucun endpoint nouveau)

### Champs WebSocket
- Durée du refus par voie — **mêmes clés que `/data`**

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-*.md` — page portant le bandeau
- `docs/subsystems/ws-manager.md` — champ nouveau
- `docs/subsystems/pump-controller.md` — si la durée y est tenue

#### Docs transverses
- `docs/API.md` — champ `/data`, et **rappel du piège `dispersion: null`**
- `CHANGELOG.md` — entrée 2.54.0
- `docs/user/src/guide-utilisation.html` — si l'écran utilisateur change

## Critères d'acceptation
- [x] Quand une voie est refusée, l'écran principal montre la cause **et la durée**, sans ouvrir le panneau dev
- [x] Quand plus rien n'est bloqué, le bandeau disparaît
- [x] Les lignes de dispersion portent leur âge et se rafraîchissent panneau ouvert ; aucune requête panneau fermé
- [x] `dispersion: null` affiche toujours « pas de jugement », jamais la valeur précédente
- [x] Le champ de durée est présent **des deux côtés**, vérifié par grep
- [x] La marge d'octets des deux charges est recalculée et **écrite** dans les commentaires, au pire cas structurel du type
- [x] Le pire cas de la trame WS reste **sous** `kWsFrameBufBytes` — aucun push sauté (3545 / 3584, marge 39 o)
- [x] Aucun seuil ni garde de dosage modifié — vérifiable par `git diff`
- [x] Tous les libellés utilisateur sont en français
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués — à confirmer par `test-engineer`/`code-reviewer` avant commit (non ré-exécuté par `doc-writer`)
- [ ] `./build_fs.sh` passe sans dépasser la partition — à confirmer avant commit (non ré-exécuté par `doc-writer`)

## Plan de test manuel
1. Filtration à l'arrêt → bandeau avec cause bénigne et durée qui court.
2. Reprise de filtration → purge → la durée repart au changement de code, le bandeau disparaît à l'ouverture de la porte.
3. Panneau dev ouvert pendant une purge : la dispersion passe de `null` à un nombre **sans intervention**, et l'âge reste petit.
4. Panneau dev fermé : vérifier dans l'onglet réseau qu'aucune requête ne part.
5. WebSocket coupé : le bandeau se marque périmé.
6. B3 : le raccourci ouvre la surface manuelle sans volume présélectionné.

## Notes pour agent

⚠️ **Ne pas « corriger » le traitement de `dispersion: null`.** Il est juste, il
est commenté, et il protège un piège documenté dans `docs/API.md`. Le défaut est
l'**absence de rafraîchissement**, pas le rendu.

⚠️ **Cette spec ne doit modifier aucune décision de dosage.** Si l'implémentation
conduit à toucher un seuil, une garde ou l'ordre des gardes, c'est que le
périmètre a dérivé : s'arrêter et le signaler. `git diff` sur
`src/dosing_logic.cpp` et `src/pump_controller.cpp` doit se limiter à
l'horodatage du code de refus.

⚠️ **La parité `/data` ↔ WS a récidivé trois fois en trois versions.** Un champ
WS-seul ne disparaît pas, il **clignote**. Grep des deux côtés, pas lecture.

⚠️ **Compter les octets, ne pas les estimer.** La marge annoncée est de 68 o.

⚠️ **Le bandeau doit rester honnête quand la cause est bénigne.** Filtration
arrêtée la nuit est le cas NORMAL : si le bandeau crie à la panne chaque soir,
il sera ignoré le jour où il aura raison.

## Hors périmètre
- Tout assouplissement de seuil ou de garde de stabilité.
- Le mode « récupération » / eau trouble autorisant un dosage borné porte fermée :
  c'est une spec à part, avec gate `pool-chemistry` bloquant.
- Le plafond de purge `kPurgeCapMs` et sa visibilité : **feature-139** (option B).
- La cause physique du bruit de mesure : feature-137.
- La butée brute pH : bug-135.


---

## Notes de clôture (2026-10-04, v2.54.0)

### Mesuré, pas recopié
| | Avant | Après |
|---|---|---|
| Suites natives | 54 | **55** |
| Cas de test | 1980 | **1989** |
| Mutants tués puis retirés | — | **3 / 3** |
| Marge WS sous `kWsFrameBufBytes` | 43 o | **39 o** (pire cas 3545 / 3584) |
| Gabarit `/data` | `<2944>` | `<3072>` (pire cas 2965 — le bump était REQUIS) |
| `git diff src/dosing_logic.cpp` | — | **vide** |

`pio run` SUCCESS zéro warning · `build_fs.sh` SUCCESS · `check_specs`,
`check_doc_links`, `check_memory_discipline`, `check_fopen_guard` verts.

### Quatre défauts attrapés pendant le cycle, tous de la même famille
Un énoncé juste qui se dégrade en étant recopié ou résumé — précisément ce que
cette spec corrige côté interface.

1. **Le budget d'octets de la spec était périmé.** « 68 o » datait de
   feature-091 (v2.34.0) ; la marge réelle était de **43 o**, et les deux renvois
   de lignes pointaient un bloc obsolète. Corrigé avant implémentation.
2. **Un commentaire inversé** : `web_routes_data.cpp` affirmait que 2965 o
   tenaient « SOUS le gabarit 2944 ». Ils le dépassaient de 21 o — le bump était
   **requis**, pas confortable.
3. **Les libellés de sécurité ont été livrés tronqués**, amputés de
   l'avertissement sur les chloramines, de « n'augmentez pas la dose », de « pas
   parce que l'eau serait correcte » et du rappel des limites. Restaurés mot à
   mot, avec un commentaire interdisant de les reformuler sans repasser le gate.
4. **La règle d'affichage du raccourci était trop large.** Une simple exclusion
   de « filtration arrêtée » le laissait apparaître sur les codes 8-14
   (limites, anti-rafale) : le dialogue y aurait MENTI, et l'injection manuelle
   étant soumise aux mêmes gardes, le raccourci aurait mené à un refus — en
   poussant à relever la limite. Remplacée par une **liste blanche**.

### Corrections issues de la revue
- **Barrière compilateur** dans `setRefusalCode()` : l'invariant d'ordre n'était
  garanti qu'au niveau source, aucun des deux membres n'étant `volatile` ni
  atomique. Sans elle, un réordonnancement rouvrait le « bloqué depuis 9 h » sur
  un blocage d'une seconde. Barrière seule : la SRAM interne de l'ESP32 n'est pas
  mise en cache, la visibilité inter-cœurs est immédiate une fois l'ordre figé.
- **Entrée morte retirée** de la liste blanche : le code 5 porte le ton `fault`,
  donc inatteignable, alors que le commentaire l'annonçait comme offert.

### Écarté sciemment
Le texte de durée du bandeau peut retarder de 30 s (rythme `/data`). C'est la
cadence déjà retenue pour l'alerte de régulation voisine, et un affichage en
minutes ou en heures ne souffre pas de trente secondes. Le défaut combattu était
un relevé vieux de **plusieurs heures**, pas de quelques dizaines de secondes.

### Reste à faire — étapes humaines
- [ ] Déploiement OTA de la 2.54.0.
- [ ] Plan de test manuel sur appareil (§ Plan de test manuel), en particulier le
      point 4 : vérifier en onglet réseau qu'**aucune** requête
      `/debug/sensor_filter_state` ne part panneau fermé.
- [ ] **Capture d'écran à refaire** : `screenshots/03_Tableau_de_bord.jpg`, avec
      le bandeau « Dosage bloqué » visible (déclencher un refus, p. ex. arrêter
      la filtration). Seule étape qui ne peut pas être automatisée.
- [ ] PDF utilisateur : régénérés **uniquement** en release, jamais commités.

### Suites
- **feature-139** (battement de la porte) : priorité abaissée pendant ce cycle.
  La mesure a montré que le battement n'avait coûté **aucun millilitre** le jour
  chiffré, le plafond journalier de chlore étant déjà atteint. Son livrable A1
  décide désormais si la spec a lieu d'être.
