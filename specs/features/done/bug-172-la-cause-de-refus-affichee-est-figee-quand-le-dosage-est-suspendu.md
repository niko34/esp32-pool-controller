# bug-172 — La cause de refus affichée est FIGÉE quand le dosage est suspendu

## Métadonnées
- **ID** : bug-172
- **Slug** : la-cause-de-refus-affichee-est-figee-quand-le-dosage-est-suspendu
- **Type** : bug — **l'appareil affiche une raison FAUSSE**, pas seulement incomplète
- **Statut** : doing — démarrée le 2026-10-18
- **Priorité** : **haute** — ⚠️ **constaté par l'utilisateur sur son appareil**, et c'est
  exactement le travers récurrent du projet : *un écran qui ne dit pas ce que l'appareil fait*
- **Version cible** : 2.83.0
- **Dernière mise à jour** : 2026-10-18
- **Spec(s) liée(s)** :
  `done/feature-163-refus-de-dosage-pendant-un-etalonnage.md`
  (⭐ **sa revue avait relevé CE mécanisme — pour le JOURNAL seulement ; la cause affichée est la
  moitié restée ouverte**) ·
  ADR-0095 (*une garde cite le seuil qui mord réellement*) ·
  ADR-0107 (⛔ pas de suspension persistante **invisible**)

## Domaines touchés
- [x] **Capteurs** — indirectement (la cause cite l'état de mesure)
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**
- [x] **Filtration** — c'est l'un des quatre déclencheurs
- [x] **Web — routes HTTP / WebSocket** — `phDoseBlockedReason` / `orpDoseBlockedReason`
- [ ] MQTT · [ ] UART · [ ] OTA · [ ] Authentification · [ ] Historique
- [x] **Frontend (`data/`)** — c'est là que la fausse raison s'affiche

## Agents requis
- [x] spec-reader
- [ ] architect — correctif **local**, le mécanisme est identifié
- [x] **pool-chemistry — OBLIGATOIRE** : ⚠️ c'est lui qui fixe **le RANG** et **les TEXTES**
      (ADR-0095)
- [ ] ux-ui-designer — ⛔ aucune surface nouvelle : un texte **déjà affiché** devient **juste**
- [x] embedded-developer
- [ ] web-ui-developer — ⚠️ seulement si un libellé client doit suivre
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Constat — relevé par l'utilisateur, **mesuré** sur l'appareil (2026-10-18)

> *« Sur le PoolController, il y a marqué pause mélange pour le pH alors que la filtration est
> arrêtée »*

`GET /data` au même instant :
```json
"filtration_running": false,
"water_present":      false,
"orpDoseBlockedReason": "pause mélange en cours",
"phMixingDelayActive":  true,
"ph_mix_remaining_s":   207
```

🔴 **La contradiction est dans la réponse elle-même** : l'appareil déclare `water_present: false`
**et** annonce « pause mélange ». Or la **présence d'eau** est une garde de **rang 2** et le
**mélange** de **rang 6b**. ⇒ Le rang 2 doit l'emporter. ⛔ Il ne l'emporte pas.

## Cause racine, vérifiée dans le code

Le bloc de suspension globale d'`updateDosingCore()` (`src/pump_controller.cpp`) se déclenche sur
**quatre** conditions — eau absente, stabilisation post-calibration, assistant de mesure du débit,
**étalonnage en cours** — coupe les pompes, et **`return`**.

⚠️ Ce `return` est **AVANT** `canDose()`. ⇒ `evaluateDose()` n'est **jamais évalué**, donc
`setGuardRefusalCause()` n'est **jamais appelé**, donc **`_lastRefusalCause` conserve sa valeur
PRÉCÉDENTE** — celle calculée au dernier tick où la filtration tournait encore.

⇒ **La cause affichée n'est pas calculée, elle est HÉRITÉE.**

⚠️ Le bloc **connaît pourtant la bonne raison** : il construit déjà une chaîne `reason`
(« eau absente (filtration arrêtée / signal externe périmé) », « stabilisation post-cal en
cours », « assistant de mesure du débit », « étalonnage en cours (session de calibration
ouverte) ») — ⛔ **mais seulement à l'intérieur de `if (phDosingState.active ||
orpDosingState.active)`**, pour le journal. Si aucune injection n'était en cours, ⛔ elle n'est
même pas construite.

### ⚠️ Pourquoi ça n'a pas été vu plus tôt — et c'est à écrire
La revue de **feature-163** avait relevé **exactement ce mécanisme** (son point bloquant B1) :
*« le `return` précède `canDose()` : le `critical` imposé et le libellé de cause sont
inatteignables »*. ⇒ ⛔ **Seule la moitié « journal » a été corrigée** (une transition
journalisée ajoutée avant le `return`). La moitié **« cause affichée »** est restée ouverte, et
c'est elle que l'utilisateur a vue **le lendemain**.

## Ce que ça coûte
⛔ **Ce n'est pas cosmétique.** Le projet a un travers nommé : *un écran qui ne dit pas ce que
l'appareil fait*. Ici il dit **autre chose** — il désigne une pause anodine et **masque la vraie
condition**. ⚠️ Un utilisateur qui attend la fin d'une « pause mélange » de 207 s attendra pour
rien : la vraie cause, elle, ne s'écoule pas.

## Comportement attendu
1. Tant que le bloc de suspension est actif, la cause affichée ⛔ **ne peut PAS être héritée** :
   elle nomme **la condition qui suspend réellement**.
2. ⚠️ Les **quatre** conditions sont couvertes, ⛔ pas seulement « eau absente ».
3. La cause est posée **pour les DEUX pompes** (pH et chlore).
4. ⚠️ **Priorité explicite** si plusieurs conditions sont vraies — ⛔ à fixer par
   `pool-chemistry` (ADR-0095 : *la garde citée est celle qui mord*).
5. À la sortie de la suspension, la cause redevient celle que calcule `canDose()`, ⛔ sans rémanence.
6. ⚠️ **Aucune allocation sur le chemin périodique** (ADR-0082) : ce bloc tourne **à chaque tick**.

## Éléments visibles
- [x] **Interface** — la raison affichée sur les cartes pH et chlore
- [x] **Endpoint HTTP / WebSocket** — `ph/orpDoseBlockedReason` (⚠️ **parité d'EXPRESSION**)
- [ ] Log — ⚠️ déjà traité par feature-163
- [ ] MQTT

## Critères d'acceptation
- [ ] Filtration arrêtée ⇒ la cause affichée **nomme l'absence d'eau**, ⛔ pas une pause héritée
- [ ] Les **quatre** conditions produisent **chacune** leur cause propre
- [ ] ⚠️ Vérifié **sans injection en cours** — c'est le cas où la raison n'était même pas construite
- [ ] Les deux pompes portent la cause
- [ ] Sortie de suspension ⇒ ⛔ **aucune rémanence**
- [ ] **Mutation** : rétablir l'héritage doit **tuer** un test
- [ ] ⛔ Aucune allocation neuve sur le chemin périodique (`check_memory_discipline.sh`)
- [ ] `pio run` 0 warning · natifs sans régression

## Plan de test manuel
1. Filtration à l'arrêt, **aucune injection en cours** ⇒ lire `/data` : la cause **nomme l'eau**.
2. Doser, puis **arrêter la filtration** pendant la pause mélange ⇒ la cause **bascule** sur l'eau.
3. Relancer la filtration ⇒ la cause redevient celle de `canDose()`.

## Hors périmètre
- ⛔ La **durée** de la pause mélange et son décompte en temps réel : ⚠️ **vérifié, ce n'est pas
  un défaut** — aucun dosage ne peut partir sans filtration, donc la pause qui s'écoule à vide ne
  crée **aucun surdosage**. ⇒ ⛔ Ne pas la convertir en temps de filtration dans ce lot.
- Le fond des quatre gardes — ⛔ inchangé, seul **l'affichage de leur cause** est en cause.


---

## Verdict `pool-chemistry` — GO SOUS CONDITIONS (2026-10-18)

### 🔴 Un défaut DE PLUS, trouvé par le gate
⚠️ La chaîne `reason` **déjà présente** (`src/pump_controller.cpp:3840-3843`) ordonne
eau → **stabilisation → assistant → étalonnage**. ⛔ Elle **CONTREDIT le rang 3** de
l'étalonnage dans `canDose()`. ⇒ Selon le chemin emprunté, l'appareil nommerait **deux causes
différentes pour le même état**. Elle doit devenir **la même cascade — source unique**.

### Conditions
- [x] **H1 — Ordre complet**, calqué sur `evaluateDose` (`src/dosing_logic.cpp:107-156`) :
  1. `!filtrationOk` — **rang 2** (`FiltrationOff`)
  2. `mqttCfg.calSessionOpen` — **rang 3** (`CalibrationSessionOpen`)
  3. `stabilizationActive` — **rang 6** (`StabilizationActive`)
  4. `flowAssistSuspend` — **dernier** : ⚠️ aucun rang dans `canDose()`, il ⛔ **ne peut donc pas
     passer devant** une garde rangée.
- [x] **H2 — Les quatre textes, mot pour mot.** ⚠️ Les trois premiers **existent déjà** dans
  `canDose()` : ⛔ **toute reformulation créerait deux voix pour un même fait**.
  1. `"filtration arrêtée"`
  2. `"étalonnage en cours (session de calibration ouverte)"`
  3. `"stabilisation post-calibration en cours"`
  4. `"mesure du débit en cours — dosage suspendu"` ⚠️ **seul texte neuf** ; libellé client
     « Mesure du débit en cours » (24 car.).
- [x] **H3 — Servir aussi le CODE**, pas seulement le texte : **2 / 24 / 7**, et
  ⚠️ **ajouter `FlowAssistSuspend = 25` EN FIN d'énum** (`dosing_logic.h`, ⛔ jamais de
  recyclage) **+** son entrée dans `DOSE_BLOCK_CODES` (`data/app.js`), ⛔ sinon l'utilisateur lit
  *« Cause inconnue (code 25) »*.
- [x] **H4 — Passer par le chemin PROPRIÉTAIRE `canDose`** (`_refusalCauseFromGuard = false`),
  ⛔ **pas** `setGuardRefusalCause` : ⚠️ la sentinelle **250** est réservée aux gardes chimiques.
- [x] **H5 — 🔴 Côté client : TAIRE le décompte.** Tant qu'un de ces codes est servi, le client
  doit masquer `dosingStabOrMixSegment` (`data/app.js:5268`), qui lit `*_mix_remaining_s`
  **seul**. ⛔ Sans ça, **le compte à rebours de 207 s reste affiché** — c'est-à-dire le symptôme
  exact que l'utilisateur a signalé.

### Risque chimique
⛔ **Aucun** : les quatre gardes coupent les pompes **de façon inchangée** ; seule
l'**observabilité** est corrigée.

---

## Clôture (doc-writer, 2026-10-18)

### Vérifié directement dans le code livré (lecture, sans compilation)
- `src/dosing_logic.h`/`.cpp` : `evaluateSuspendCause()` présente, ordre eau → étalonnage →
  stabilisation → assistant (H1), `DoseRefusal::FlowAssistSuspend = 25` ajouté en fin d'énum avec
  son `static_assert` de verrouillage (H3).
- `src/pump_controller.cpp` : `postSuspendCause()` appelée pour les **deux** pompes,
  **inconditionnellement** (hors de la conjonction `phDosingState.active || orpDosingState.active`)
  — confirme H1/H3 sans injection en cours. Chemin `logRefusalOnce()` emprunté, `setGuardRefusalCause()`
  non touché (H4). Edge-trigger strict sur le code avant toute construction de `String` (cohérent
  avec ADR-0082, critère d'acceptation « aucune allocation neuve »).
- `src/constants.h` : les quatre textes (`kCauseFiltrationOff` et consorts) sont des `static const
  char*` partagés, pas de littéral dupliqué (H2).
- `data/app.js` : `DOSE_BLOCK_CODES[25]` ajouté, `_doseSuspendOutranksMixing()` (renommée depuis
  `_isDoseSuspendRemanentCode()` lors d'une revue ultérieure, voir rectification ci-dessous) et ses
  deux points d'appel (`dosingStabOrMixSegment()`, `_renderDosePill()`) présents (H5).
- `test/dose_suspend_outranks_mixing_vectors.js` + `tools/check_dose_suspend_outranks_mixing_vectors.sh`
  (renommés pour la même raison) : vecteurs dédiés à H5, patron ADR-0099 (extraction depuis le code
  réellement livré, pas une recopie), job CI `dose-suspend-outranks-mixing-vectors` ajouté dans
  `.github/workflows/hygiene-specs.yml`.
- `72` suites natives mesurées par `ls -d test/test_native_* | wc -l` (2026-10-18, ne pas recopier
  — ADR-0073) ; `test/test_native_dosing/test_dosing_decision.cpp` et le nouveau
  `test/test_native_dose_suspend_guard_priority/` (test structurel, voir rectification ci-dessous)
  sont parmi les fichiers modifiés/ajoutés du lot (git status).

### Reporté par le cycle en amont, non ré-exécuté par doc-writer (compilation/tests exclus de son
### périmètre — un seul agent compile à la fois)
- `pio run` / `pio test -e native` sans régression, et la validation par **mutation** de H1/H5
  (rétablir l'héritage doit tuer un test ; servir le décompte sur un code rémanent doit tuer un
  test) : effectués par `test-engineer` en amont de cette clôture, non rejoués ici.
- `./tools/check_memory_discipline.sh` : périmètre logger uniquement (`logger.{h,cpp}`,
  `log_arena_logic.*`) — **ne couvre pas** `dosing_logic.cpp`/`pump_controller.cpp`, donc non
  invoqué pour ce lot ; l'absence d'allocation sur `postSuspendCause()` a été vérifiée par lecture
  directe (ci-dessus), pas par cet outil.

### Critères d'acceptation
- [x] Filtration arrêtée ⇒ la cause affichée **nomme l'absence d'eau**, ⛔ pas une pause héritée
- [x] Les **quatre** conditions produisent **chacune** leur cause propre
- [x] ⚠️ Vérifié **sans injection en cours** — `postSuspendCause()` appelée hors de la conjonction
      `*DosingState.active`
- [x] Les deux pompes portent la cause
- [x] Sortie de suspension ⇒ ⛔ **aucune rémanence** — ⚠️ **précision apportée en rectification** :
      l'affirmation initiale (« le flux normal `canDose()`/`logRefusalOnce()` reprend la main dès
      que l'OR de suspension redevient faux ») était **incomplète** : une cause de garde chimique
      propriétaire (`_refusalCauseFromGuard`) n'est reprise par `canDose()` que lorsque la garde
      elle-même relâche la main, ce qui n'est pas automatique à la seule sortie de la suspension
      globale — voir la section « Rang 0 de la cascade » dans `pump-controller.md` et la
      rectification ci-dessous
- [x] **Mutation** : rétablir l'héritage doit **tuer** un test — reporté par `test-engineer` (non
      ré-exécuté par doc-writer, voir ci-dessus)
- [x] ⛔ Aucune allocation neuve sur le chemin périodique — vérifié par lecture (edge-trigger strict
      sur le code avant toute `String`), `check_memory_discipline.sh` non applicable à ces fichiers
- [x] `pio run` 0 warning · natifs sans régression — reporté par `test-engineer` (non ré-exécuté)

### Documentation
`CHANGELOG.md` (section `[2.83.0]`), `docs/API.md` (code `25`, note sur la publication pendant la
suspension globale), `docs/subsystems/pump-controller.md` (nouvelle section « Cascade de la
suspension globale »), `docs/features/page-dashboard.md` (nouvelle section « Rémanence de la pause
mélange… », avec signalement d'un drift pré-existant hors périmètre sur la section « Statut dosage
des cartes pH/ORP »), `docs/adr/0118-une-sortie-anticipee-publie-la-cause-quelle-rend-inatteignable.md`
(nouvel ADR) + `docs/adr/README.md` (index).

⚠️ **Voir « Rectification (doc-writer, 2026-10-18, second passage) » ci-dessous : une partie de
cette clôture décrivait une cause FAUSSE, corrigée depuis.**

Spec déplacée vers `specs/features/done/` (tous les critères d'acceptation et conditions
`pool-chemistry` H1-H5 cochés).

---

## Rectification (doc-writer, 2026-10-18, second passage)

La clôture ci-dessus affirmait que `*_mix_remaining_s` / `*MixingDelayActive` étaient
**« rémanents »**, **« périmés »**, **« plus recalculés »** pendant la suspension globale.
⛔ **C'est faux**, et la capture `/data` de cette spec le prouvait déjà elle-même : `207` s n'est pas
une valeur figée, c'est un décompte en cours. Ces deux champs sont des getters **purs**
(`isPhMixingDelayActive()` / `isOrpMixingDelayActive()` / `getMixingRemainingS()`,
`src/pump_controller.cpp`) appelés **au moment de la sérialisation** `/data`/WebSocket
(`ws_manager.cpp`, `web_routes_data.cpp`) contre l'échéance `_mixingEndMs[pompe]` — jamais par
`updateDosingCore()`. Ils décroissent réellement et retombent à `0` tout seuls. **Les 207 s que
l'utilisateur a vues étaient une pause mélange vraie.**

**Seul le couple code/texte était hérité** — c'est le vrai défaut, celui que corrige
`evaluateSuspendCause()` et que documente [ADR-0118](../../docs/adr/0118-une-sortie-anticipee-publie-la-cause-quelle-rend-inatteignable.md),
qui lui ne porte pas cette confusion.

**La vraie raison de taire le décompte de mélange côté client** : une cause de suspension
(filtration, étalonnage, stabilisation, mesure du débit) **prime** sur la pause mélange. Annoncer
« Pause mélange · 4 min » quand la filtration est arrêtée affiche une information **exacte mais
subordonnée**, à la place de la cause qui commande réellement l'attente. Ce n'est pas qu'une
question de mots : le modèle « ces champs sont périmés » est précisément ce qui a laissé passer un
défaut plus grave en revue (voir ci-dessous).

Renommages effectués dans le code pour refléter la bonne raison (hors périmètre de ce second
passage, déjà faits à la livraison) : `_isDoseSuspendRemanentCode` → `_doseSuspendOutranksMixing` ;
`test/dose_suspend_remanent_vectors.js` → `test/dose_suspend_outranks_mixing_vectors.js` ;
`tools/check_dose_suspend_remanent_vectors.sh` → `tools/check_dose_suspend_outranks_mixing_vectors.sh` ;
job CI `dose-suspend-remanent-vectors` → `dose-suspend-outranks-mixing-vectors`.

### Une régression distincte, trouvée en revue, introduite par ce même lot

Le correctif initial de `postSuspendCause()` empruntait le chemin propriétaire de `canDose()`
(`logRefusalOnce()`), qui pose inconditionnellement `_refusalCauseFromGuard = false`. Sans garde
supplémentaire, cela **écrasait aussi les causes de GARDE CHIMIQUE** (pH/ORP hors seuil, sentinelle
`250`) dès qu'une suspension globale démarrait — et la garde ne reprenait jamais la main au retour :
elle ne re-pose sa cause que sur un **front**, sa surveillance ne tourne pas pendant la suspension
(placée après le `return`), et `canDose()` est lui-même derrière `!isPhGuardBlocking(true)`.
Scénario concret : pH sous le plancher à 20 h, filtration coupée à 23 h, reprise à 9 h ⇒ l'écran
aurait annoncé « filtration arrêtée » toute la journée, filtration en marche, **cause chimique
effacée** — pire en mode Programmé/manuel.

**Correctif retenu** (verdict du gate, option (a)) : `if (_refusalCauseFromGuard[pumpIndex])
return;` posé en **rang 0** de la cascade — une cause de garde n'est **jamais** écrasée.
L'alternative (réarmer les latches de garde à la reprise) a été **refusée pour une raison
chimique** : elle réalimenterait le compteur des 3 refus / 24 h, dont l'escalade bascule la
régulation en manuel ; un correctif d'affichage ne doit pas ouvrir de chemin vers un changement de
mode de régulation. Vérifié par un test **structurel** (lecture du texte livré, patron des suites
sœurs `test_native_ph_guard_causes`/`test_native_dose_banner_silence`/`test_native_ota_order_guard`),
**pas une exécution** : `test/test_native_dose_suspend_guard_priority/`. Il protège d'une régression
par **distraction**, pas d'un contournement délibéré. Mutation tuée : retirer la ligne de garde fait
échouer le test.

### Autres points relevés dans ce second passage

- Le journal distingue de nouveau « signal externe périmé » de « filtration arrêtée » (le texte de
  transition journalisé a changé de source, voir `pump-controller.md`) — le champ d'API, lui, ne
  change pas.
- `72` suites natives (`ls -d test/test_native_* | wc -l`, mesuré le 2026-10-18 — à remesurer par
  quiconque le cite ensuite, ADR-0073). Le compte de cas (`2 266`) est fourni par le cycle amont
  (`test-engineer`) et n'a pas été ré-exécuté ici : la compilation/les tests sont hors du périmètre
  de ce passage (une seule compilation à la fois, règle machine).
- ⚠️ **Dette d'ergonomie relevée, non traitée** : trois composants rendent le même fait de trois
  façons sur les cartes pH/ORP — durée **écoulée** (bandeau de durée de refus), **aucune** durée
  (pastille « Débit d'injection »), **compte à rebours** (ce segment). Passe `ux-ui-designer` à
  prévoir séparément.

### Fichiers corrigés par ce second passage
`CHANGELOG.md`, `docs/features/page-dashboard.md`, `docs/subsystems/pump-controller.md`,
`docs/API.md`, cette spec. `docs/adr/0118-...md` vérifié : ne portait pas la raison fausse, non
modifié sur ce point (l'ADR décrit `ph/orpDoseBlockedReason`, dont l'hérédité était réelle — pas
`*_mix_remaining_s`).
