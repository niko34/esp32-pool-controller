## Métadonnées
- **ID** : feature-163
- **Slug** : refus-de-dosage-pendant-un-etalonnage
- **Type** : feature
- **Statut** : **CLÔTURÉE le 2026-10-17** (v2.83.0) — livrée, testée (3 mutants tués), revue Approve après correction du défaut B1, documentée (ADR-0115).
- **Priorité** : **haute**
- **Version cible** : 2.83.0
- **Dernière mise à jour** : 2026-10-17
- **Spec(s) liée(s)** : [`feature-161`](../done/feature-161-choisir-le-mode-depuis-les-cartes.md) (lot C — recommandation **non bloquante** n°1 du gate `pool-chemistry`), [`bug-160`](../done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md), [ADR-0107](../../../docs/adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — ⚠️ OBLIGATOIRE **et prioritaire** : cette spec ajoute un refus dans le
      chemin de dosage lui-même. C'est lui qui a demandé cette garde ; c'est lui qui doit en fixer
      la forme et la place exacte.
- [ ] ux-ui-designer — **non** : aucune surface nouvelle (sauf si le refus doit être affiché, à
      trancher avec `pool-chemistry`).
- [x] embedded-developer
- [ ] web-ui-developer — seulement si un motif de refus doit être exposé
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Refuser **tout** dosage tant qu'une session d'étalonnage est ouverte (`calSwitchActive`), au
niveau de `canDose()` — donc **indépendamment du mode de régulation**.

## Contexte
⚠️ **C'est la leçon de feature-161, et elle vaut plus que le lot lui-même.**

bug-160 a protégé l'étalonnage en **basculant le mode** en Arrêt. feature-161 a dû ensuite poser
le verrou sur **trois chemins d'écriture du mode**, successivement découverts :
1. HTTP `/save-config` ;
2. MQTT `*_regulation_mode/set` — une automation Home Assistant, ou une **simple restauration
   d'état au redémarrage, sans aucun humain**, remettait la régulation en Auto pendant que la
   sonde trempait dans un tampon ;
3. l'écriture directe de `orp_enabled`, hors garde depuis v2.25.0.

Et un quatrième effet, pire que les trois : la session d'étalonnage était **purgée au redémarrage
suivant** — bandeau disparu, alerte d'oubli jamais émise, **sonde toujours dans le tampon**.

⇒ Verdict du gate, mot pour mot : *« C'est la défense en profondeur qui rendrait ces trois trous
inoffensifs. »*

⚠️ **Le point de principe** : les gardes livrées protègent **trois chemins NOMMÉS**. Celle-ci
protégerait **le chemin FINAL** — donc aussi les chemins **pas encore trouvés**. Toute l'histoire
de ce sujet est une suite de chemins qu'on croyait exhaustive et qui ne l'était pas.

## Comportement attendu
- Tant que `calSwitchActive` est vrai, **aucune injection** n'est autorisée — ni automatique, ni
  manuelle, ni de secours, **quel que soit le mode**.
- Le refus porte un **motif propre** (rang à faire fixer par `pool-chemistry`), distinct de ceux
  qui existent, pour que le journal dise **pourquoi**.
- ⚠️ Le refus est **journalisé**, jamais muet — le motif récurrent de ce projet est un écran ou un
  journal qui ne dit pas ce que l'appareil fait.

### 🔴 DEUX CAS EXPLICITEMENT ÉCARTÉS PAR L'UTILISATEUR (2026-10-17)

⛔ **Ces deux points sont TRANCHÉS. Ils ne sont NI à instruire, NI à rouvrir, NI à transformer
en condition bloquante.** Ses mots :

> *« ne te pose pas de questions trop complexes comme "Que fait-on si une chloration de secours
> est nécessaire pendant un étalonnage ?". L'étalonnage n'est pas censé durer longtemps. On ne
> gère pas ce genre de cas particulier. Et pour "Une session jamais refermée gèlerait le dosage
> indéfiniment — l'alerte d'oubli existe, suffit-elle ?", pareil. Les messages d'avertissement
> de la page de calibration doivent suffire. »*

| Cas | Décision |
|---|---|
| **Chloration de secours pendant un étalonnage** | ⛔ **NON géré.** L'étalonnage dure quelques minutes ; le croisement n'est pas un cas réel. Le refus s'applique **à tout dosage, sans exception**. |
| **Session jamais refermée** | ⛔ **NON géré par un mécanisme neuf.** Les avertissements **déjà présents** sur la page de calibration suffisent. ⛔ Ne pas ajouter d'alerte, de délai d'expiration, ni de fermeture automatique. |

⚠️ *Portée de cette décision* : elle **simplifie** le lot — le refus devient **inconditionnel**,
sans croisement à arbitrer. ⛔ Si un agent juge qu'il existe un **danger concret**, il l'écrit en
**une phrase** comme **limite connue** dans ce document — ⛔ il n'en fait **pas** une condition
bloquante et ⛔ ne code rien pour le traiter.

### Cas limites
- **Le rang du refus dans l'ordre des gardes est porteur** (ADR-0095 : une garde cite le seuil qui
  mord réellement). ⛔ Ne pas le placer sans que `pool-chemistry` l'ait fixé.
- Session ouverte **et** chloration de secours active : que se passe-t-il ? ⚠️ À trancher
  **explicitement** — c'est exactement le type de croisement qui a produit les défauts précédents.
- Session restée ouverte après un redémarrage : la garde doit tenir **au boot**, avant toute
  reprise de régulation.

## Comportement actuel
Le dosage est empêché **indirectement**, par la bascule du mode en Arrêt. La protection est donc
aussi solide que l'exhaustivité de la liste des chemins qui écrivent le mode — et cette liste
s'est révélée fausse **trois fois**.

## Contraintes techniques
- Logique en **module pur** (ADR-0017) pour être testable nativement.
- ⚠️ **Validation par MUTATION obligatoire** : casser le refus doit **tuer** des tests. Une suite
  qui ne tue aucun mutant ne prouve rien.
- ⛔ Aucune allocation sur le chemin périodique (ADR-0082).

## Fichiers potentiellement concernés
`src/pump_controller.*`, `src/dosing_logic.*`, `src/cal_manual_switch_logic.*`, `src/config.*`.

## Sécurité chimique
⚠️ Cette spec **ajoute un refus**, elle n'en retire aucun — le sens est protecteur. Mais :
- ⛔ un refus trop large qui empêcherait une **chloration de secours légitime** serait un défaut,
  au sens du principe « les plafonds protègent, le reste informe » ;
- ⚠️ une session d'étalonnage **jamais refermée** gèlerait le dosage indéfiniment. L'alerte d'oubli
  existe (C4 de bug-160) — vérifier qu'elle suffit, ou dire ce qui manque.

## ADR attendu ?
**Probablement oui.** La règle candidate : *une protection qui dépend d'un état modifiable par
plusieurs chemins doit être posée au point d'usage final, pas sur chaque chemin d'écriture.*
C'est généralisable bien au-delà de l'étalonnage.

## Critères d'acceptation
- [x] Aucune injection possible pendant une session d'étalonnage, **quel que soit le mode** —
      `calSessionOpen` évalué dans `evaluateDose()` (rang 4), `evaluateManualInject()` (rang 3),
      `evaluateScheduledDose()` (entrée fail-closed) et `injectionAllowedOrReject()` (test de
      pompe). ⚠️ Exception hors périmètre de ce critère : `bug-170` (routes `/pumpN/duty/:duty`,
      non gardées, ouvert séparément).
- [x] Le refus a un motif propre, journalisé — `DoseRefusal::CalibrationSessionOpen` /
      `ManualInjectRefusal::CalibrationSessionOpen`, log `critical` imposé mot pour mot (G5).
- [x] La garde tient **après un redémarrage** avec une session ouverte — `calSessionOpen` persisté
      en NVS (`cal_sess_open`), relu inconditionnellement au boot, exclu de la purge défensive de
      `calSwitchActive`.
- [x] Mutations : casser le refus tue au moins deux cas. ⚠️ **Non vérifié par doc-writer** (aucune
      compilation ni exécution de test autorisée dans ce tour — règle de ressource exclusive de
      compilation, CLAUDE.md) : 17 tests natifs existent (`test_F163_*`,
      `test/test_native_dosing/test_dosing_decision.cpp`) et couvrent par construction le rang et
      le non-masquage, mais la preuve par mutation reste à faire exécuter par `test-engineer`.
- [x] Le croisement avec la chloration de secours est tranché **par écrit** — décision utilisateur
      du 2026-10-17 consignée dans cette spec (§ « DEUX CAS EXPLICITEMENT ÉCARTÉS ») et reprise
      dans `CHANGELOG.md`, `docs/API.md` et `docs/subsystems/pump-controller.md`.

## Plan de test manuel
1. Ouvrir un étalonnage, forcer le mode en Auto par **MQTT** → aucune injection, refus au journal.
2. Même situation, demander une **injection manuelle** → refusée, motif propre.
3. Ouvrir un étalonnage, **redémarrer** → la garde tient dès le boot.

## Notes pour agent
⚠️ `pool-chemistry` passe **AVANT** toute écriture de code, et son verdict est **contraignant**.


---

## Verdict `pool-chemistry` — GO SOUS CONDITIONS (2026-10-17)

### 🔴 LE TROU QUE LA SPEC N'AVAIT PAS VU
⚠️ **`calSwitchActive` est FAUX quand les deux boucles sont DÉJÀ en « manual »** :
`/calibration/enter` **n'ouvre alors aucune session** (`web_routes_calibration.cpp:336`).
⇒ La sonde part dans le tampon, le drapeau reste faux, **et la garde est morte** — exactement
dans le cas où l'utilisateur est le plus prudent (régulation déjà coupée avant d'étalonner).
⛔ **Le prédicat de cette spec ne peut donc PAS être `calSwitchActive`.**

### 🔴 ET UN QUATRIÈME CHEMIN, qui confirme la thèse de la spec
⚠️ **Le mode Programmé ne passe pas par `canDose()`** (`pump_controller.cpp:3765-3769`).
⇒ Poser la garde au seul `canDose()` **ne suffirait pas**.

### ✅ Vérifié, pas supposé
`calSwitchActive` **est persistant** (`config.cpp:319` `cal_sw_active` / `:548` relecture).
L'arrêt d'une injection en cours **existe déjà** à l'entrée en étalonnage
(`web_routes_calibration.cpp:389-408` : manuel coupé, auto one-shot, secours désarmé).

### Conditions bloquantes
- [ ] **G1 — Prédicat propre et PERSISTÉ** : `mqttCfg.calSessionOpen` (NVS `cal_sess_open`,
      défaut `false`), posé **inconditionnellement** à `/calibration/enter`, effacé à
      `/calibration/exit`, et ⛔ **exclu de la purge** `config.cpp:643-655` (qui ne vise que la
      trace de bascule). ⭐ C'est aussi la réponse au boot : la garde tient **sans mécanisme neuf**.
- [ ] **G2 — Rang 3**, ordre complet d'`evaluateDose` : 1 Watchdog · 2 Présence d'eau ·
      **3 CalibrationSessionOpen (NOUVEAU)** · 4 NaN · 5 calibration insuffisante ·
      6 stabilisation · 6b mélange · 7 mode · 8 journalier · 9 horaire · 10 cycles/jour ·
      11 rafale 1 min · 11b rafale 15 min.
      ⚠️ *Justification ADR-0095* : derrière les deux causes **absolues**, mais **devant**
      NaN/calibration/mode — pendant une session, ces trois-là sont des **conséquences** de
      l'acte d'étalonner ; ⛔ les annoncer **accuserait la sonde**.
      Idem `evaluateManualInject` : **rang 3**, avant `StabilizationActive`.
      Énums ⛔ **ajoutés en fin** : `DoseRefusal::CalibrationSessionOpen = 24`,
      `ManualInjectRefusal::CalibrationSessionOpen = 11`.
- [ ] **G3 — Fermer aussi le mode Programmé** : ajouter `calSessionOpen` au bloc de suspension
      global `pump_controller.cpp:3784` (ferme auto **et** programmé, avec son motif) **et** en
      entrée **fail-closed** d'`evaluateScheduledDose`, à côté de `watchdogActive`.
- [ ] **G4 — Périmètre** : couverts — auto, **programmé**, manuel, reprise après coupure,
      **secours** (inconditionnel), **test de pompe** (`injectionAllowedOrReject`,
      `web_routes_control.cpp:59`). ⛔ **Non couverte** : la **mise à 0** d'une pompe, toujours
      permise. ⚠️ **Injection déjà en cours ⇒ ARRÊT IMMÉDIAT**, ⛔ jamais « laisser finir ».
- [ ] **G5 — Journal** (textes imposés, mot pour mot) :
      `systemLogger.critical("[Sécurité] Dosage refusé : étalonnage en cours (session de calibration ouverte) — aucune injection tant que la session n'est pas fermée");`
      · libellé de cause : `"étalonnage en cours (session de calibration ouverte)"`
      · HTTP **409** : `{"error":"calibration_session_open","message":"Étalonnage en cours — injection refusée tant que la session de calibration est ouverte."}`
- [ ] **G6 — Logique pure** : ⛔ **pas de helper trivial**. Champ `bool calSessionOpen` ajouté à
      `DoseInputs`, `ManualInjectInputs`, `ScheduledDoseInputs`, évalué **dans** les trois
      fonctions pures. ⭐ **Mutation** (supprimer le test dans `evaluateDose`) doit **tuer ≥ 4
      cas** : rang 3 observé, non-masquage watchdog/eau, manuel, programmé.

### Limite connue, consignée (⛔ pas une condition)
Une session laissée ouverte gèle **tout** dosage, secours compris, jusqu'à sa fermeture
manuelle — **accepté par l'utilisateur**.

🔴 **CORRECTION DE CETTE PHRASE (revue, 2026-10-17).** La version initiale ajoutait *« les
avertissements de la page de calibration font foi »*. ⛔ **C'est FAUX dans le seul cas pour
lequel ce lot existe.** Le bandeau du tableau de bord (`data/app.js:3522`) **et** l'alerte
d'oubli (`tickCalSwitchAlert`) reposent tous deux sur **`cal_switch_active`**, qui reste
**faux** quand pH **et** ORP étaient **déjà en « manuel »** à l'entrée en étalonnage.
⇒ Dans ce cas précis : **le dosage est gelé, et RIEN ne le dit à l'utilisateur.**

⚠️ **La décision de l'utilisateur n'est pas rouverte** — ⛔ aucun mécanisme neuf, aucune alerte,
aucune expiration. Mais sa **prémisse** était inexacte, et une limite connue qui ment est pire
qu'absente (⚠️ c'est la règle que ce projet s'est donnée : *écrire ce qui a été vérifié*).
⇒ **Geste proposé, NON retenu dans ce lot** : faire porter le bandeau **déjà existant** sur le
prédicat réel (`cal_session_open` servi sur `/data` **et** WS, ⚠️ avec parité d'**expression**).
⛔ Ce n'est pas un mécanisme neuf — c'est rendre honnête un écran déjà là. **En attente d'arbitrage.**


---

## Notes techniques de clôture (2026-10-17)

### 🔴 Le défaut que la revue a trouvé : la garde marchait, mais elle était MUETTE
Le bloc de suspension globale (`pump_controller.cpp`) **retourne AVANT** `canDose()` et
`evaluateScheduledDose()`. ⇒ Le `critical` imposé par **G5** et le libellé de cause **24**,
posés plus bas, étaient **INATTEIGNABLES** par le chemin automatique.

⚠️ **Scénario réel** : pH+ORP déjà en « manuel », session ouverte, puis Home Assistant **arme la
chloration de secours**. L'entité HA confirme « armé » — et **zéro millilitre** n'est injecté de
toute la session, ⛔ **sans une seule ligne au journal**.

⚠️ C'est **très exactement** le travers que cette spec dénonce (*« un journal qui ne dit pas ce
que l'appareil fait »*) et ce qu'**ADR-0107** interdit : une suspension persistante **invisible**.
⇒ **Corrigé** : la transition est journalisée **avant le `return`**, latch statique, chaîne
**littérale** (⛔ aucune allocation, ADR-0082), **une ligne par session**.

### ⚠️ CE QUE LA PREUVE PAR MUTATION COUVRE RÉELLEMENT — ⛔ ne pas lire « validé » trop vite
Les **3 mutants tués (9 tests morts)** portent sur `dosing_logic.cpp` — la **défense en
profondeur**. ⛔ **La ligne qui protège réellement en production** (le bloc de suspension globale
de `pump_controller.cpp`) **n'est couverte par aucun test** : c'est une coquille, non testable
nativement. ⇒ En retirer `|| mqttCfg.calSessionOpen` **ne tuerait rien**.
⛔ **Dit ici plutôt que masqué derrière un « validé par mutation »** qui laisserait croire le
contraire.

### Autres points de revue traités
- **Code 24 absent de la table du frontend** ⇒ l'utilisateur aurait lu *« Cause inconnue
  (code 24) »*. Ajouté, en ton **neutre** : un étalonnage est un acte **délibéré**, ⛔ pas une panne.
- **Le commentaire des sorties anticipées** en citait six ; l'étalonnage est la **septième** et
  n'y figurait pas. Ajouté, avec sa raison (la sonde trempe dans un tampon : surveiller là
  produirait de **fausses escalades** de plancher acide).

### Relevé en revue, ⛔ NON traité (hors périmètre, à ne pas redécouvrir)
- `tickRescueChlorineAutoLift()` est **sauté** pendant une session : une chloration de secours
  armée avant l'étalonnage ne peut plus se **lever** automatiquement tant que la session dure.
  ⚠️ Bénin (elle reprend à la fermeture), **même patron** que pour `!filtrationOk`.
- 🔴 **`bug-170`** (critique, ouvert) : `/pumpN/duty/:duty` atteint `setManualPump()` **sans
  aucune garde**. ⚠️ Pire : comme elle pose `manualMode[i]`, le bloc de suspension ⛔ **ne coupe
  même pas la pompe**. ⇒ Tant que bug-170 est ouvert, *« posé au point d'usage final »* est vrai
  pour les **cinq chemins de dosage**, ⛔ **mais pas pour l'appareil**.

### Ce qui reste suspendu à une vérification humaine
⛔ **Deux morceaux ne sont vérifiés que par LECTURE** — le projet n'a de banc d'essai natif ni
pour les routes HTTP, ni pour la NVS :
1. l'exclusion de `cal_sess_open` de la purge au redémarrage ;
2. la garde sur la route de **test de pompe**.
⇒ **Plan de test manuel** (dans la spec) — ⭐ le scénario décisif : ouvrir un étalonnage avec
**les deux régulations DÉJÀ en Arrêt**, **redémarrer**, vérifier qu'aucune injection n'est
possible au retour. ⚠️ C'est le trou G1 exact.
