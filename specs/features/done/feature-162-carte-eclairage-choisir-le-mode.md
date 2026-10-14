## Métadonnées
- **ID** : feature-162
- **Slug** : carte-eclairage-choisir-le-mode
- **Type** : feature
- **Statut** : doing (implémentation + documentation livrées 2026-10-14, clôture par l'orchestrateur)
- **Priorité** : normale
- **Version cible** : 2.79.0
- **Dernière mise à jour** : 2026-10-14 (modes **Allumé/Éteint/Programme** — décision utilisateur)
- **Spec(s) liée(s)** : [`feature-161`](../done/feature-161-choisir-le-mode-depuis-les-cartes.md) (dont cette spec a été **détachée sur décision utilisateur**), [ADR-0107](../../../docs/adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md), [ADR-0108](../../../docs/adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md), [ADR-0109](../../../docs/adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md) (atterrissage de migration + rupture HA assumée, créé pendant ce lot)

## Notes d'implémentation (2026-10-14, doc-writer)

- **Modèle d'état** : `lightingCfg.mode` (`String`, jetons `on`/`off`/`schedule`) devient la seule
  autorité, remplaçant `enabled`/`scheduleEnabled`/l'ancien `manualOverride` caché. Décision pure
  `decideLightingOn(LightingMode, ...)` en switch exhaustif (pas de `default`) dans
  `src/schedule_logic.{h,cpp}` — voir [docs/subsystems/lighting.md](../../../docs/subsystems/lighting.md).
- **Migration NVS** : `migrateLightingMode()` (pure, testée natif) dérive le mode des deux anciens
  booléens au premier démarrage sans `light_mode` ; `light_sched_en` prime (jamais Programme perdu
  au profit d'Allumé). Nuance assumée par écrit : le critère « jamais Programme sur un forçage
  perdu » ne vaut qu'à partir du premier démarrage sur ce firmware (le forçage n'a jamais été
  persisté). Voir [ADR-0109](../../../docs/adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md).
- **UI** : fusion des cartes « Contrôle manuel »/« Programmation » de `/lighting`, `.mode-select`
  réutilisé (carte d'accueil + page de réglage), horaires en carte en mode Programme via la
  composition `DESIGN_SYSTEM.md §5.20` (déclarée avant implémentation, comme exigé). Badge d'état
  restreint au mode Programme.
- **MQTT/HA** : nouveau topic `{base}/lighting_mode` (+ `/set`), ancien `lighting_schedule/set`
  supprimé. Ancien select HA 2 options **dépublié**, nouveau select 3 options publié à un
  `unique_id` distinct (pas d'alias) — rupture assumée, confirmée par l'utilisateur.
- **Tests natifs** : 15 tests dédiés `LightingMode`/migration ajoutés à
  `test/test_native_schedule/test_schedule_logic.cpp` (94 `RUN_TEST` au total dans ce fichier) ;
  parité d'expression `lighting_mode`/`lighting_enabled`/`lighting_schedule_enabled` vérifiée par
  `test_h_lighting_expression_partagee_identique_des_deux_cotes`
  (`test/test_native_ws_config_parity/test_ws_config_parity.cpp`).
- **Documentation** : `docs/subsystems/lighting.md` réécrit, `docs/subsystems/ws-manager.md`,
  `docs/features/page-dashboard.md`, `page-lighting.md`, `docs/MQTT.md`, `docs/API.md`,
  `docs/user/src/guide-utilisation.html` mis à jour ; `docs/DESIGN_SYSTEM.md` **vérifié déjà à
  jour** (§5.2, §5.20) — non modifié (hors périmètre de cette passe). `src/version.h` → 2.79.0.
- ⛔ Spec **non déplacée** vers `done/` sur consigne explicite de l'orchestrateur — clôture prise
  en charge séparément.

## Domaines touchés
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **non** : aucun dosage, aucune régulation chimique
- [x] **ux-ui-designer — MANDAT SUR L'ÉCRAN ENTIER.** ⚠️ Les trois autres cartes ont été
      refondues par feature-161 ; l'Éclairage est resté **seul dans l'ancien motif**. Son brief
      doit dire : *évalue la carte dans l'ensemble du tableau de bord, et dis tout choix visuel
      que ce lot impose — même ceux que je n'ai pas pensé à te demander.*
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Aligner la carte Éclairage sur le motif posé par feature-161 : choisir le mode depuis la carte,
sans quitter l'accueil. **Et corriger au passage un défaut existant** : la carte affirme
« Programmation activée » alors que la programmation peut être **neutralisée** sans que l'écran
le dise.

## Contexte
feature-161 a refondu les cartes pH, ORP et Filtration. L'Éclairage en a été **sorti sur décision
de l'utilisateur**, pour ne pas étendre un lot déjà large.

⚠️ **Le défaut découvert en instruisant feature-161 est le vrai sujet de cette spec.** La carte
affiche « Programmation activée » sur le seul `lighting_schedule_enabled`, alors que
`state.manualOverride` **neutralise** la programmation — et cet état n'est **jamais publié**.
L'écran annonce donc un comportement que l'appareil n'a pas.

⚠️ Et `decideLightingOn()` évalue l'override **AVANT** la programmation : l'ordre du code et
l'ordre affiché **ne disent pas la même chose**. C'est le motif de l'ADR-0093 (« le mode est la
seule autorité ») appliqué à l'éclairage.

## Comportement attendu

### ⚠️ Les trois modes sont **Allumé / Éteint / Programme** (décision utilisateur, 2026-10-14)

Mot pour mot : *« Quand c'est allumé, il faut écrire Allumé. Quand c'est éteint, il faut écrire
Éteint. Et quand c'est Programme (comme les autres cartes), c'est Programme. »*

⛔ **Pas de libellé d'état séparé.** *« Pourquoi écrire un libellé alors que le bouton actif
permettra de voir ce qui est sélectionné ? »* ⇒ le segment actif **EST** l'état. Toute ligne qui
répète le mode est à **supprimer**, pas à corriger.

⚠️ **Cette décision DISSOUT le défaut au lieu de le corriger.** Aujourd'hui, allumer à la main
crée un état **caché** (`state.manualOverride`) qui neutralise la programmation sans le dire et
n'est jamais publié. Avec ces trois modes, allumer à la main revient à **choisir le mode Allumé** :
il n'y a plus d'état invisible à publier, donc plus rien qui puisse mentir.
⇒ C'est l'application directe d'[ADR-0107](../../../docs/adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)
(un état **visible et persistant** plutôt qu'une suspension invisible).

⚠️ **Différence assumée avec les cartes pH/ORP** : là-bas, la ligne de mode en en-tête double le
sélecteur, et le gate `pool-chemistry` a **refusé** qu'on la retire — cette redondance est la
protection contre un arrêt accidentel du dosage. **Ici il n'y a aucun enjeu chimique**, donc
l'argument ne s'applique pas. ⛔ Ne pas invoquer l'un pour justifier l'autre.

### ⚠️ Les HORAIRES font partie de ce lot (arbitrage du 2026-10-14)

En mode **Programme**, les heures de début et de fin doivent être **modifiables depuis la carte**.
⚠️ Elles figuraient aussi dans [`feature-164`](feature-164-regler-les-valeurs-depuis-les-cartes.md) ;
⛔ **l'utilisateur a tranché : c'est ICI**, et feature-164 ne touchera plus l'Éclairage. Motif :
deux lots successifs sur la même carte, c'est exactement le motif d'accumulation qui a dégradé la
page Produits.

⇒ En **Allumé** ou **Éteint**, les horaires n'ont aucun effet : ⛔ ne pas les afficher. La ligne
« Horaires `--:-- - --:--` » visible aujourd'hui quand la programmation est désactivée **n'est pas
une information** — elle disparaît.
⚠️ Le projet **exclut explicitement** les champs horaires de l'enregistrement automatique
(`bindAutosave()`). Mettre ces champs en enregistrement automatique serait un **écart délibéré**,
à assumer par écrit — ⛔ pas à présenter comme une mise en cohérence.
⚠️ **Aucun composant « modifier une valeur dans une carte » n'est déclaré** dans
[`docs/DESIGN_SYSTEM.md`](../../../docs/DESIGN_SYSTEM.md) (§2). C'est à l'`ux-ui-designer` de
l'y ajouter **avant** l'implémentation — règle de gouvernance du document.

### La seule information que les trois boutons ne portent PAS
En mode **Programme**, le sélecteur ne dit pas **si la lampe est allumée à cet instant**. C'est une
information **différente** du mode, pas une redite ⇒ **à conserver, et uniquement dans ce mode**.
En Allumé ou Éteint elle serait redondante : ⛔ ne pas l'afficher.

### Cas limites
- ⚠️ **Migration de l'existant** : que devient une installation où `manualOverride` est actif au
  moment de la mise à jour ? Elle doit se retrouver en **Allumé** ou **Éteint** selon l'état réel,
  jamais en Programme.
- Un changement venu de MQTT ou d'un autre navigateur : reflété sans rechargement.
- Échec d'enregistrement : le sélecteur **revient à l'état réel** (réutiliser `resolveSelectorMode`,
  ⛔ ne pas en écrire un second).

## Comportement actuel
La carte expose la programmation ; le mode se change depuis la page de réglage. `manualOverride`
n'est pas publié, et la carte peut mentir.

## Contraintes techniques
- ⚠️ **RESPONSIVE — 390 px D'ABORD**, pas en repli. Compter les caractères de tout libellé placé
  sur une ligne partagée, et le **raccourcir** s'il ne tient pas.
- ⚠️ **Parité `/data` ↔ WebSocket** : tout nouveau champ publié doit l'être **des deux côtés**,
  avec la **même expression** — pas seulement la même clé (4ᵉ récidive constatée, dont une
  invisible au grep).
- Réutiliser le chemin partagé `applyRegulationMode` et la fonction pure `resolveSelectorMode`.
- Vocabulaire : **Allumé / Éteint / Programme** (décision utilisateur du 2026-10-14, ci-dessus) —
  ⛔ **pas** « Auto / Programme / Arrêt » : ces mots-là sont ceux des cartes pH/ORP/Filtration,
  pas ceux retenus ici.

## Fichiers potentiellement concernés
`data/index.html`, `data/app.js`, `data/app.css`, `src/lighting.*`, `src/web_routes_*.cpp`,
`src/ws_manager.cpp`, `src/mqtt_manager.cpp`.

## ADR attendu ?
Probablement **non** : ADR-0108 couvre déjà la règle. ⚠️ **Sauf** si le cas « override + programmation »
impose un arbitrage général sur la publication de l'état appliqué — alors oui.

## Éléments visibles (pour doc-writer)
`docs/features/page-dashboard.md`, `docs/features/page-eclairage.md` (ou équivalent),
`docs/MQTT.md` si un état est publié, `docs/subsystems/ws-manager.md` si un champ WS est ajouté,
`CHANGELOG.md`, `docs/user/src/guide-utilisation.html`.

## Critères d'acceptation
- [ ] Le mode Éclairage se choisit depuis la carte : **Allumé / Éteint / Programme**.
- [ ] Les **horaires sont modifiables depuis la carte** en mode Programme, et **absents** en Allumé/Éteint.
- [ ] `docs/DESIGN_SYSTEM.md` déclare le composant de saisie en carte **et son contexte**.
- [ ] Rendu **capturé** avec `./tools/ui_shot.sh` à 390 px, et comparé aux trois autres cartes.
- [ ] ⛔ **Aucun libellé ne répète le mode** — le segment actif est la seule source.
- [ ] `manualOverride` ne subsiste plus comme **état caché** : allumer à la main **est** un mode.
- [ ] En mode **Programme uniquement**, la carte indique si la lampe est allumée à cet instant.
- [ ] Une installation dont l'override était actif se retrouve en Allumé/Éteint, **jamais** en Programme.
- [ ] L'état appliqué est publié, et la parité `/data` ↔ WS est vérifiée **par expression**.
- [ ] Un échec d'enregistrement ramène le sélecteur à l'état réel.
- [ ] Vérifié **à 390 px sur appareil réel** (tâche humaine).

## Plan de test manuel
1. Activer la programmation, puis forcer un override → la carte doit **cesser** d'annoncer la
   programmation comme active.
2. Changer le mode depuis MQTT → la carte suit sans rechargement.
3. Couper le réseau pendant un changement → le sélecteur revient à l'état réel.

## Notes pour agent
⚠️ **Le défaut d'affichage est le cœur de cette spec, pas un bonus.** Une implémentation qui
ajoute le sélecteur sans corriger `manualOverride` **ne répond pas** à la demande.

## Clôture — et ce que l'IMAGE a trouvé que la relecture n'avait pas vu

**Version 2.79.0.** Mesuré, pas recopié (ADR-0073) : `pio run -e esp32dev_core3` **SUCCESS**, 0
warning · `pio test -e native` **2 182 cas / 70 suites**, zéro régression · **18/18** contrôles
d'hygiène · **5 606** liens résolus · `build_fs.sh` 512 Ko.

### Verdicts
`pool-chemistry` **écarté avec motif mesuré** (relais GPIO26, aucun chemin vers `pump_controller`)
· `code-reviewer` **Approved**, 3 mineures appliquées · **4 mutations sur 4 tuent des tests**,
rien n'a survécu.

### ⚠️ TROIS défauts trouvés par la CAPTURE, aucun par la relecture

C'est le fait marquant du lot. `./tools/ui_shot.sh` venait d'être créé ; c'est sa première
utilisation en conditions réelles.

1. **Les deux champs horaires s'empilaient.** L'ergonome avait calculé « ≈155 px par colonne,
   largement suffisant » — ⛔ faux à 390 px : `@media (max-width: 640px) { .grid--2 {
   grid-template-columns: 1fr } }` ramène la grille à **une** colonne. **+80 px de hauteur.**
   Corrigé par `.schedule-fields` (technique de `.filtration-modes`, précédent du projet).
   ⚠️ **Ni l'ergonome, ni le développeur, ni la revue ne l'avaient vu** — tous trois raisonnaient
   sur la règle de base sans la règle mobile.
2. **Le badge reprenait le mot du bouton.** En Programme il affichait « Éteint », **exactement**
   le libellé du segment juste en dessous. Collision **créée par ce lot** (avant, le texte disait
   « Programmation désactivée »). ⇒ « Lampe allumée / Lampe éteinte ».
3. ⚠️ **Le plus discret, et le plus instructif : le badge affichait une valeur ÉCRITE EN DUR.**
   Le chemin JavaScript était **correct** — il n'écrit le texte que si l'état réel est connu. Mais
   son **point de départ** mentait : le HTML contenait « Éteint », et `/data` (HTTP) **ne porte
   aucune information d'éclairage** (seule la trame temps réel la porte). ⇒ Avant la première
   trame, le badge affichait une valeur **potentiellement fausse**.
   Corrigé : le badge est **masqué tant que l'état est inconnu** (DESIGN_SYSTEM §4, *donnée
   absente ⇒ on n'affiche rien*).
   ⚠️ **La leçon** : relire un chemin de code ne suffit pas quand c'est son **état initial** qui
   est faux. Seul le rendu le montre.

### Décisions structurantes
- **ADR-0109** : atterrissage de migration, et suppression de l'entité Home Assistant plutôt
  qu'un alias compatible.
- ⚠️ **Rupture Home Assistant assumée**, confirmée par l'utilisateur (aucune automatisation chez
  lui) : l'ancien sélecteur à 2 options est **supprimé**, pas abandonné orphelin. Motif : le
  garder obligeait à décider **en silence** que « Désactivé » = « Éteint ».
- **Effet de bord favorable** : la commande Home Assistant simple **persiste désormais** l'état,
  ce qu'elle ne faisait pas — asymétrie relevée par le développeur, non demandée.

### ⛔ Restant à l'humain
- Vérification **à 390 px sur appareil réel**.
- Captures `screenshots/05_Eclairage.jpg` **et** `03_Tableau_de_bord.jpg`.
