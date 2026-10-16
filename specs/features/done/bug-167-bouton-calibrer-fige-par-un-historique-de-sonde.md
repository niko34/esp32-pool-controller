# bug-167 — Le bouton « Calibrer » reste inactif, figé par l'historique de la sonde

## Métadonnées
- **ID** : bug-167
- **Slug** : bouton-calibrer-fige-par-un-historique-de-sonde
- **Type** : bug — **interface bloquante** : un geste légitime devient impossible
- **Statut** : **CLÔTURÉE le 2026-10-16** (v2.82.0) — correctif complété, testé, revu.
  Restent : tests natifs, revue, documentation. Spec rédigée **après** le correctif
  (patch à chaud demandé par l'utilisateur en cours de calibration réelle) — elle
  consigne ce qui a été fait, elle ne le précède pas.
- **Priorité** : haute — empêche l'étalonnage de la sonde pH, alors que la sonde
  de ce bassin **a justement perdu son étalonnage** (zéro EZO à 128 mV le 11/10)
- **Version cible** : 2.82.0
- **Dernière mise à jour** : 2026-10-16
- **Spec(s) liée(s)** : feature-163 (refus de dosage pendant un étalonnage, `todo/`) ·
  ADR-0099 (tester la logique navigateur par extraction de texte)

## Domaines touchés
- [x] **Capteurs (pH / ORP / température)** — écran d'étalonnage
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] **Frontend (`data/`)** — `data/app.js` uniquement

## Agents requis
- [x] spec-reader
- [ ] architect — correctif local, aucune conception à arbitrer
- [ ] pool-chemistry — **non** : aucun chemin de dosage touché, aucune constante
      chimique, aucune valeur d'étalonnage modifiée. Le défaut et son correctif sont
      entièrement dans l'**activation d'un bouton**.
- [ ] ux-ui-designer — aucun changement de forme : un bouton déjà dessiné redevient
      cliquable quand il doit l'être
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Pendant l'étalonnage de la sonde pH en solution tampon 7, la mesure se stabilise
mais le bouton **« Calibrer »** reste **grisé** : l'étalonnage ne peut pas être
validé. L'utilisateur est bloqué devant son bassin, flacon en main.

## Contexte

Mots de l'utilisateur, en cours d'opération :

> « Je calibre ma sonde pH avec la solution à 7. Une fois que c'est stable, le
> bouton calibrer reste inactif. »

puis, après une première tentative de correction qui **n'a pas fonctionné** :

> « C'est stable et je ne peux toujours pas cliquer sur le bouton calibrer. »

Le contexte matériel rend le blocage coûteux : la sonde de ce bassin **ne tient
plus son étalonnage** (deux épisodes en deux jours, zéro EZO à 128 mV pour un
sain < 15 mV). L'étalonnage est précisément le geste de diagnostic à faire.

## Cause racine

L'écran d'étalonnage **gèle** les boutons de point déjà acquis, pour éviter
qu'un point validé soit réécrit par mégarde. Ce gel se calculait sur
`phCalPoints` — le **nombre de points mémorisés par la sonde**, une valeur
**historique** lue au chargement.

La sonde de ce bassin ayant déjà été étalonnée, `phCalPoints` valait **2**. Le
gel s'appliquait donc **dès l'ouverture de l'écran**, avant tout geste de
l'utilisateur dans la session en cours — c'est-à-dire exactement quand les
boutons doivent être actifs.

**Le gel portait sur la MÉMOIRE DE LA SONDE au lieu de l'AVANCEMENT DE LA
SESSION.** Les deux coïncident sur une sonde neuve, et divergent sur toutes les
autres.

### Pourquoi la première tentative a échoué

Le gel était écrit à **DEUX endroits** et seul le premier avait été corrigé. Le
second (`data/app.js:9021`) réarmait les boutons à la réception des données puis
les re-figeait aussitôt, annulant le correctif en silence.

⚠️ **Leçon de méthode** : la seconde occurrence n'a pas été trouvée en relisant
le code mais en **inspectant le DOM rendu** (`./tools/ui_shot.sh --dump-dom`).
Deviner une deuxième fois aurait coûté un déploiement de plus.

## Comportement attendu

1. À l'ouverture de l'écran d'étalonnage, **aucun bouton de point n'est gelé**,
   quel que soit l'historique de la sonde.
2. Un bouton de point se gèle **à partir du moment où ce point a été acquis dans
   la session en cours**, et pas avant.
3. Changer de produit (pH ↔ ORP) ou rouvrir l'écran **ouvre une session neuve** :
   tous les gels retombent.
4. Le gel reste **par produit** : acquérir un point pH ne gèle rien côté ORP.

## Correctif appliqué (déployé, non commité)

- `data/app.js:9323` — `_calSessionAcquired = { ph: false, orp: false }` : l'état
  d'avancement **de la session**, distinct de l'historique de la sonde.
- `data/app.js:9527` — le gel n'est calculé que si `_calSessionAcquired[product]`.
- `data/app.js:9559` — un point acquis arme le drapeau.
- `data/app.js:9675` — l'entrée dans l'écran remet les drapeaux et les gels à zéro.
- `data/app.js:9021` — **seconde occurrence** : le `Object.assign` qui re-figeait
  les boutons est supprimé des deux branches ; le bloc ne fait plus que **réarmer**
  les boutons au premier chargement.

## Éléments visibles
- [x] **Interface** — le bouton « Calibrer » redevient cliquable quand la mesure
      est stable, sur une sonde déjà étalonnée
- [ ] Endpoint HTTP · [ ] Message WebSocket · [ ] Topic MQTT · [ ] Log

## Critères d'acceptation
⚠️ **Note du doc-writer (2026-10-16)** : coché sur la foi d'une **relecture du code**
(`data/app.js`, fonctions `resetCalStepper`, `completeCalStep`, `_calSwitchEnter`, le handler de
succès de calibration) — **déjà confirmé par l'utilisateur sur l'appareil réel** (voir
« Vérification humaine requise » ci-dessous). Aucune compilation ni `build_fs.sh` exécutés dans
cette session (consigne explicite, un autre agent compilait en parallèle).
- [x] Sur une sonde rapportant `phCalPoints = 2`, les boutons de point sont
      **actifs** à l'ouverture de l'écran — `resetCalStepper()` n'applique le gel
      (`Object.assign(_calFrozen[product], ...)`) que si `_calSessionAcquired[product]` est vrai,
      faux à l'ouverture. **Confirmé sur l'appareil** par l'utilisateur.
- [x] Après acquisition d'un point dans la session, **ce point** est gelé — `completeCalStep()`
      arme `_calSessionAcquired[product] = true`, et le handler de succès pose
      `_calFrozen[a.product][a.step] = true`
- [x] 🔴 Sortir puis revenir dans l'écran dégèle tout — ⚠️ **CORRIGÉ**, après avoir été
      **coché à tort par relecture de code puis démontré faux par `code-reviewer`.** `_calSwitchEnter()` remet bien les
      drapeaux à zéro, ⛔ **mais il s'exécute APRÈS `resetCalStepper()`** (`data/app.js:9942`),
      qui a déjà recalculé le gel depuis l'historique de la sonde — et ⛔ `_calSwitchEnter` ⛔ **ne
      retouche jamais le DOM**. ⇒ À la **2ᵉ ouverture**, les deux boutons restent gris, et la
      seule sortie (« Recommencer le point 7.0 ») **efface aussi le point 4.00**.
      ⚠️ **Leçon** : lire que la remise à zéro *existe* ne dit rien sur le moment où elle
      s'exécute. C'est le **3ᵉ** faux-pas de ce bug, tous dus à une lecture statique.
      ✅ **Correctif** : `_calResetSessionState()` + `_calEnterScreen(product)` — ⭐ **l'ORDRE
      EST le correctif** (remise à zéro **avant** `resetCalStepper`, jamais après).
      ✅ **Reproduit dans le navigateur AVANT correction** (captures avant/après), ⛔ pas déduit.
- [x] Les points ORP ne sont pas affectés par un point pH acquis — `_calSessionAcquired`/`_calFrozen`
      sont des structures **par produit** (`ph`/`orp`), jamais partagées
- [x] **Validation par MUTATION** — ✅ **exécutée**, `test/cal_session_reentry_vectors.js`
      (21 vecteurs, 5 scénarios) : (a) gel inconditionnel sur `phCalPoints` → **12 vecteurs
      tués** ; (b) `Object.assign` de re-gel réintroduit → **1** ; (c) ⭐ **ordre inversé dans
      `_calEnterScreen`** → **3**.
- [x] `./build_fs.sh` OK — ✅ 512 Ko. Harnais branché sur la CI :
      `tools/check_cal_session_reentry_vectors.sh`, job `cal-session-reentry-vectors`.

## Vérification humaine requise
- [x] **Confirmé par l'utilisateur sur l'appareil** : le bouton est actif.
      ⚠️ Il a aussi relevé *« La mesure n'est pas stable et le bouton est actif »* —
      le bouton ne dépend plus de la stabilité. **À trancher** : est-ce un défaut à
      ouvrir séparément, ou le comportement voulu ? Sa réaction : *« Mais ce n'est
      peut-être pas plus mal »* — donc **pas de régression à corriger en urgence**.

## Hors périmètre
- L'état matériel de la sonde (zéro à 128 mV) — **matériel, pas logiciel**
- Le refus de dosage pendant un étalonnage — c'est feature-163
- Le critère de stabilité lui-même (voir « Vérification humaine requise »)


## Notes techniques de clôture (2026-10-16)

### ⚠️ Ce bug a été manqué TROIS fois, toujours de la même façon
1. **1ʳᵉ correction** — un seul des **DEUX** sites de gel avait été vu. Le second réarmait les
   boutons puis les re-figeait **en silence**. Trouvé en inspectant le **DOM rendu**, ⛔ pas en
   relisant le code.
2. **Clôture documentaire** — le critère « sortir puis revenir dégèle tout » a été **coché après
   relecture** : la remise à zéro *existe* bien… ⛔ mais elle s'exécute **après** le calcul du gel.
3. **Le défaut réel** (`code-reviewer`) — `resetCalStepper()` tournait **avant**
   `_calSwitchEnter()` (qui est `async`), et `_calRefreshFrozenButtons()` ⛔ **n'active jamais**
   un bouton. ⇒ À la **2ᵉ ouverture**, les deux boutons restaient gris, et la seule sortie
   (« Recommencer le point 7.0 ») **efface aussi le point 4.00**.

⭐ **La leçon, et elle vaut au-delà de ce bug** : *lire qu'une remise à zéro EXISTE ne dit rien
sur le MOMENT où elle s'exécute.* Un ordre d'exécution ⛔ ne se vérifie pas par lecture — il se
**rejoue**. D'où le harnais de réentrée, dont c'est la raison d'être.

### Question laissée ouverte par l'utilisateur
Il a relevé : *« La mesure n'est pas stable et le bouton est actif »*, puis *« Mais ce n'est
peut-être pas plus mal »*. ⇒ Le bouton ne dépend plus de la stabilité. ⛔ **Pas une régression
à corriger en urgence** — à trancher séparément si le sujet revient.
