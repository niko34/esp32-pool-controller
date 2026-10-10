## Métadonnées
- **ID** : feature-148
- **Slug** : un-message-un-seul-endroit
- **Type** : bug
- **Statut** : doing — implémentation + tests livrés (B1-B4, D1-D2, mutations rejouées), **documentation livrée** (C1-C5, ce tour) ; non déplacée vers `done/` faute de commit référencé au moment de cette passe (voir « Notes d'implémentation »)
- **Priorité** : normale
- **Version cible** : 2.64.0
- **Dernière mise à jour** : 2026-10-10
- **Spec(s) liée(s)** : feature-149 (valeur figée dans le texte de garde — défaut de SOURCE
  constaté sur le même écran, traité séparément car firmware + `pool-chemistry`)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — *non* : aucun texte n'est réécrit, aucun chemin de dosage touché.
      La spec ne change QUE l'endroit où un texte existant s'affiche. ⚠️ Si
      l'implémentation en vient à reformuler, abréger ou tronquer un libellé verrouillé,
      la case devient obligatoire et le cycle reboucle.
- [ ] ux-ui-designer — déjà passé : l'audit du 2026-10-10 EST l'origine de cette spec
      (verdict reproduit sous « Contexte »). Ne pas le réinvoquer sans question nouvelle.
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Un même message de blocage de dosage s'affiche deux fois sur le tableau de bord : dans le
bandeau global en haut de page, et dans le badge de la carte du capteur concerné. Faire taire
le badge sur toute cause que le bandeau affiche déjà, pour l'ensemble des codes concernés et
non pour le seul cas signalé.

## Contexte

Constat utilisateur du 2026-10-10, capture téléphone à l'appui :

> « Sur le tableau de bord, le message en haut et celui dans le widget de pH semblent
> redondants. »

L'écran montrait, à 1 500 px d'intervalle vertical, deux fois la chaîne
« pH 5.79 sous le plancher de sécurité (6.70) — dosage acide suspendu. »

**Le mécanisme anti-redondance existe déjà et a un trou.** `data/app.js:6685` tient
`DOSE_BLOCK_CODE_ALREADY_SOURCED = new Set([2,3,4,7,8,10,11,12,15,18,19,22])`, consulté par
`dosingLimitSegments()` (`data/app.js:3042-3045`) pour que le badge ne répète pas ce que le
bandeau porte. Le code du cas signalé — **250**, `DOSE_BLOCK_CODE_GUARD` — n'y figure pas.

**Ce n'est pas un cas isolé.** `_doseBlockRowData()` (`data/app.js:6750-6757`) n'a **aucun
filtre par code** : le bandeau affiche toute voie dont le ton n'est pas `none`. Tout code
absent du `Set` se duplique donc. Recensement complet sur `DOSE_BLOCK_CODES`
(`data/app.js:6586-6646`) :

| Code(s) | Libellé | Dupliqué |
|---|---|---|
| 1 | Watchdog inactif | oui |
| 5 · 6 | Capteur instable · Calibration insuffisante | oui |
| 9 | Régulation non automatique | oui |
| 13 · 14 | Anti-rafale | oui |
| 16 · 17 · 20 · 21 | Sonde figée / muette | oui |
| **250** | **Sécurité pH/ORP active** | **oui — cas signalé** |

Les autres passent inaperçus car leurs libellés sont courts et statiques ; celui du 250 est
long et porte des valeurs runtime. **Même défaut de principe.** Les traiter d'une seule passe,
sinon l'audit se rejoue dans deux mois sur le code 5.

### ⚠️ Correction d'une affirmation de l'audit — lire avant de concevoir
Le rapport d'ergonomie conclut qu'un ajout au `Set` « casserait » les pages `/ph` et `/orp`,
au motif que leurs badges partageraient `buildSensorSegments()`. **C'est faux, vérifié :**

```js
// data/app.js:3215-3218
const phPageBadge = composeSensorBadge(measureStateSegments('ph', latestSensorData));
```

Les badges de page n'appellent que `measureStateSegments()`. Le libellé de blocage est poussé
par `dosingLimitSegments()`, atteinte uniquement depuis `buildSensorSegments()`, elle-même
appelée **seulement** aux lignes 3123 et 3133 — les deux badges du TABLEAU DE BORD. Le texte
n'a jamais atteint les pages dédiées. **Aucun mécanisme conditionné à la vue n'est nécessaire**,
et aucune régression n'est possible sur `/ph` et `/orp`.
⚠️ Cette invariance est le pivot de la spec : si l'implémentation introduit un appel de
`buildSensorSegments()` depuis une autre vue, elle doit rouvrir la question.

## Comportement attendu

Sur le **tableau de bord**, quand le bandeau de blocage affiche déjà une cause pour une voie,
le badge de la carte de cette voie **ne répète pas cette cause**. Il continue d'afficher ses
faits propres : hors-plage (« Trop bas (6.5) »), capteur indisponible, stock faible, décomptes,
et les quotas (cf. exception ci-dessous).

Appliqué à la capture d'origine : le bandeau garde son texte complet, sa durée et ses actions ;
le badge pH n'affiche plus que « Trop bas (6.5) ».

**Règle des trois niveaux**, à inscrire en doc et à appliquer à tout message futur :

| Niveau | Où | Contenu |
|---|---|---|
| 1 — Bandeau global | tableau de bord **uniquement** | texte narratif, valeurs runtime, durée, actions (CTA). Fait autorité. |
| 2 — Badge de carte | tableau de bord **et** page dédiée | faits courts propres au capteur. **Se tait sur une cause portée par le niveau 1 dans la vue courante.** |
| 3 — Pastille de section | carte de dosage | forme courte (« injection en cours », « pause mélange »). Registre différent — coexistence normale. |

**Critère pour trancher un cas futur** : le texte raconte-t-il une cause (panne, garde,
attente) ? → niveau 1 prioritaire, niveau 2 se tait. Est-ce un compteur de capacité, court et
sans récit ? → duplication tolérée.

### Exception à CONSERVER telle quelle
Les **quotas** (codes 10 · 11 · 12, limites journalière / horaire / cycles) restent
volontairement aux deux niveaux. Ce ne sont pas des causes racontées mais des compteurs, utiles
à lire au niveau de la carte même quand le bandeau existe. Ils sont déjà dans le `Set` et
poussés par un chemin distinct (`data/app.js:3033-3035`) — **ne pas les y toucher**.

### Cas limites
- **WebSocket déconnecté** : le bandeau porte déjà `.dose-block-banner__stale-note`
  (`data/index.html:213`). Si le bandeau passe en « information non rafraîchie », vérifier que
  le badge ne reste pas muet sur une cause qui n'est plus affichée de façon fiable — préciser
  le comportement retenu plutôt que de le laisser émerger.
- **Fusion pH→ORP** : quand le bandeau fusionne les deux voies en une ligne
  (`data/app.js:6978`), le silence du badge doit couvrir **les deux** voies fusionnées, pas la
  seule voie pH. À vérifier explicitement.
- **Carte masquée / régulation sur Arrêt** : le badge ne doit pas devenir la seule source d'une
  cause que plus rien n'affiche.
- **Code inconnu d'un firmware plus récent** (`« cause inconnue (code N) »`, sémantique stricte
  documentée `data/app.js:6688-6695`) : doit rester visible quelque part, jamais avalé par le
  silence.

## Comportement actuel
Le badge et le bandeau affichent la même chaîne, issue du même appel `_doseBlockInfo()`, pour
tous les codes du tableau ci-dessus. Les deux emplacements sont des régions `aria-live` —
`#dose-block-banner` (`data/app.js:6804-6806`) et le badge (`data/index.html:395`) — donc un
lecteur d'écran annonce **deux fois** la même phrase par deux canaux distincts.

## Contraintes techniques
- ⚠️ **RESPONSIVE** : téléphone **ET** tablette **ET** ordinateur, conçu **à 390 px d'abord**.
  Le gain de hauteur est l'objet même de la spec — le mesurer sur téléphone, pas l'affirmer.
- ⚠️ **Aucun libellé n'est réécrit.** Plusieurs sont verrouillés MOT À MOT par le gate
  `pool-chemistry` (bug-135, feature-139/140/146 ; commentaires `data/app.js:6952-7007` :
  « ne JAMAIS le reformuler, abréger ou tronquer »). La spec déplace et fait taire, jamais elle
  ne reformule.
- ⚠️ **Accessibilité** : `aria-live`. Supprimer une annonce en double est un gain, mais ce que
  dit la couleur doit rester dit autrement, et le badge ne doit pas perdre son rôle `status`.
- Vanilla JS, pas de framework. Français dans toute l'UI.
- Budget FS vérifié par `./build_fs.sh`.

## Fichiers potentiellement concernés
- `data/app.js` — `DOSE_BLOCK_CODE_ALREADY_SOURCED` (6685), `dosingLimitSegments()` (3009-3047),
  `_doseBlockRowData()` (6750). ⚠️ Le nom du `Set` confond deux questions (« déjà affiché
  ailleurs DANS le badge » vs « déjà porté par le bandeau ») : le commentaire doit lever
  l'ambiguïté, faute de quoi le trou se rouvrira.
- `data/index.html` — seulement si la structure du badge change (a priori : non).
- `docs/features/page-dashboard.md` — la règle des trois niveaux.
- `CHANGELOG.md` — changement visible utilisateur.

## Livrables — check-list
- [x] B1 — Le badge du tableau de bord se tait sur toute cause portée par le bandeau, pour
      l'ENSEMBLE des codes recensés (1, 5, 6, 9, 13, 14, 16, 17, 20, 21, 250) — pas le seul 250.
- [x] B2 — Exception quotas (10, 11, 12) préservée, vérifiée par test.
- [x] B3 — Commentaire levant l'ambiguïté du `Set` et consignant l'invariant « seules les lignes
      3123/3133 alimentent ce chemin ».
- [x] B4 — Comportement explicite sur les cas limites : WS déconnecté, fusion pH→ORP, code inconnu.
- [x] D1 — Tests couvrant : silence du badge quand le bandeau affiche · maintien du badge quand
      le bandeau est absent (pages dédiées) · quotas toujours dupliqués · cas fusionné.
- [x] D2 — **Validation par MUTATION** : vider le `Set`, y mettre TOUS les codes, inverser la
      condition — chaque mutation doit tuer au moins un test.
- [x] C1 — Règle des trois niveaux documentée dans `docs/features/page-dashboard.md`.
- [x] C2 — Entrée `CHANGELOG.md`.
- [x] C5 — **Corriger les étiquettes de version périmées de feature-147** dans
      `docs/features/page-dashboard.md` et `docs/features/page-filtration.md` : elles disent
      `v2.64.0` alors que feature-147 est sortie en **2.63.0** (section `[2.63.0]` du CHANGELOG,
      et `src/version.h` = 2.63.0 ; la doc est le SEUL endroit à dire 2.64.0, reliquat de la
      spec qui visait cette version avant d'être intégrée à la livraison 2.63.0).
      ⚠️ **13 occurrences**, dont des TITRES de section et les ANCRES qui pointent dessus
      (`#carte-filtration--...-feature-147-v2640`) : changer un titre casse l'ancre. Corriger
      les deux côtés ensemble et revalider par `./tools/check_doc_links.sh`.
      **Pourquoi c'est dans le périmètre et non du refactoring opportuniste** : feature-148 vise
      2.64.0 et écrit dans `page-dashboard.md`. Sans cette correction, le même fichier
      attribuerait la même version à deux lots différents.

## Sécurité chimique
- Dosage impliqué : **non** — aucun chemin vers `pump_controller`, aucun seuil, aucune garde.
- Limites journalières / horaires concernées : aucune (leur AFFICHAGE est explicitement
  préservé, cf. exception quotas).
- Filtration requise avant action : sans objet.
- Calibration capteur modifiée : non. Algorithme PID touché : non.
- Risque identifié : **un seul** — faire taire un message de sécurité sans que le bandeau le
  porte réellement, créant un silence là où il y avait deux voix. C'est le risque que les cas
  limites et D1 doivent fermer. Un message en double est bénin ; un message absent ne l'est pas.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte structurante imposée au reste du système) et
  2 (décision invisible dans le code)
- **Sujet pressenti** : la règle des trois niveaux d'affichage d'un message. Sans ADR, elle vit
  dans un commentaire et le prochain message ajouté repartira au hasard — c'est exactement le
  mécanisme qui a produit ce défaut.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun (consommation seule : `*DoseBlockedCode`, `*DoseBlockedReason`, `*DoseBlockedForS`)

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-dashboard.md` — règle des trois niveaux + exception quotas
- `docs/subsystems/` — aucun composant firmware touché

#### Docs transverses
- `CHANGELOG.md` — entrée sous 2.64.0
- `docs/user/src/guide-utilisation.html` — **à évaluer** : l'écran change d'aspect (un message
  en moins). Si la capture correspondante devient fausse, le signaler à l'utilisateur.

#### Docs immuables
- `docs/adr/NNNN-regle-des-trois-niveaux-de-message.md` — si l'ADR est confirmé

## Critères d'acceptation
- [x] Quand le bandeau affiche une cause pour le pH, le badge de la carte pH ne la répète pas
- [x] Le badge continue d'afficher « Trop bas (x) », stock faible et décomptes
- [x] Les quotas restent visibles aux deux niveaux
- [x] Sur `/ph` et `/orp`, aucun changement observable (régression zéro, vérifiée)
- [x] Un lecteur d'écran n'annonce plus la même phrase deux fois sur le tableau de bord
- [x] Tous les libellés utilisateur sont en français, et **aucun n'a été reformulé**
- [x] `./build_fs.sh` passe sans dépasser la partition
- [x] `pio test -e native` : zéro régression

## Plan de test manuel
1. **Nominal** — provoquer un blocage de garde pH (code 250) : vérifier une seule occurrence du
   texte sur le tableau de bord, et que le bandeau garde durée + boutons.
2. **Page dédiée** — ouvrir `/ph` pendant le même blocage : vérifier que l'affichage est
   identique à avant la spec.
3. **Quota** — atteindre la limite journalière : vérifier que le message reste aux DEUX niveaux.
4. **Fusion** — provoquer l'attente de mesure (pH 3/4/15/18/19/22 + ORP 250) : une seule ligne
   fusionnée, et le badge muet sur les DEUX voies.
5. **Offline** — couper le WebSocket pendant un blocage : vérifier le comportement retenu en B4.
6. **Téléphone 390 px** — mesurer la hauteur de l'écran avant / après.

## Notes pour agent
- L'audit d'ergonomie complet du 2026-10-10 est résumé dans « Contexte ». **Sa conclusion sur
  les pages dédiées est erronée et corrigée ci-dessus** — ne pas la reprendre telle quelle.
- Précédent à imiter, pas à réinventer : `#regul-alert` et le chip de filtre
  (`_classifyFilterState`, `data/app.js:6459-6461`) gèrent déjà correctement le même schéma
  « bandeau sur le dashboard, relais sur la page dédiée ». Le commentaire de `_doseBlockRowData`
  mentionne d'ailleurs explicitement une « non-duplication voulue » avec `#regul-alert`.
- Chantier de simplification en cours (direction du 2026-10-09) : retirer fait partie du travail
  courant. **Garde-fou** : ne pas confondre redondant et rassurant.

## Hors périmètre
- Le **texte figé** citant une valeur de pH périmée → feature-149. Défaut de SOURCE (firmware),
  pas d'affichage. Cette spec réduit la contradiction de deux occurrences à une ; elle ne la
  supprime pas, et **ne doit pas tenter de la corriger côté frontend** — recalculer un texte
  verrouillé mot à mot serait pire que le défaut.
- Toute reformulation de libellé.
- Les pages `/ph` et `/orp`, inchangées par construction.
- La pastille de section (niveau 3), explicitement conservée.

## Notes d'implémentation (doc-writer, 2026-10-10)

**Mécanisme livré** : le silence du badge (niveau 2) n'est pas accordé par une liste de codes
étendue, mais par `doseBlockBannerCarries(prefix, json)`, qui relit le **même plan pur** que
celui réellement rendu par le bandeau (`_doseBlockBannerPlan()`), avec garde de présence DOM
(absente sur `/ph` et `/orp` → le badge parle toujours, « en cas de doute on duplique »). Les
onze codes recensés en « Contexte » (1, 5, 6, 9, 13, 14, 16, 17, 20, 21, 250) sont couverts par
construction, sans énumération dédiée — exactement le choix documenté dans
[ADR-0094](../../../docs/adr/0094-trois-niveaux-d-affichage-d-un-message.md).

**Tests (D1/D2)** : `test/test_native_dose_banner_silence/test_dose_banner_silence.cpp` couvre
silence/maintien/quotas/fusion/code inconnu/péremption WS. Suite native complète au moment de
cette passe : **62 suites / 2 113 cas, zéro échec**. Deux mutations rejouées et vérifiées par le
coordinateur (`carries` forcé à `true`, suppression de la garde `FilterAlarm`) tuent chacune des
tests **comportementaux**.

**Build** : `./build_fs.sh` → SUCCESS, bundle gzippé 512 Ko, dans le budget partition `spiffs`
(524 288 o, ADR-0068).

**Documentation mise à jour (C1-C5)** :
- `docs/features/page-dashboard.md` — nouvelle section « Règle des trois niveaux d'affichage
  d'un message (feature-148) », avec exception quotas et renvoi à l'ADR-0094.
- [ADR-0094](../../../docs/adr/0094-trois-niveaux-d-affichage-d-un-message.md) (nouveau) — indexé
  dans `docs/README.md`, qui ne listait déjà plus 0092/0093 (omission antérieure, corrigée au
  passage, minimale).
- `CHANGELOG.md` — nouvelle section `[2.64.0]`, domaine **Fonctionnalités** (comportement
  utilisateur) + **Documentation**. ⚠️ `src/version.h` est encore à **2.63.0** au moment de
  cette passe : la section `[2.64.0]` est créée par anticipation de la prochaine release, sur
  consigne explicite reçue pour ce tour — le bump effectif de `src/version.h` n'est pas du
  ressort de `doc-writer`.
- `docs/features/page-dashboard.md` et `docs/features/page-filtration.md` — correction des
  **20 occurrences** réelles (titres de section, texte inline, ancres) de `v2.64.0`/`v2640` →
  `v2.63.0`/`v2630` pour feature-147 (version réelle de sortie, confirmée par `src/version.h` et
  la section `[2.63.0]` du CHANGELOG). Le chiffre de 13 occurrences indiqué dans le brief était
  sous-estimé — recompté par `grep -o` sur les deux fichiers, titres + ancres inclus. Revalidé
  par `./tools/check_doc_links.sh` (5 008 liens relatifs, toutes cibles résolues — ce script ne
  vérifie que l'existence des fichiers cibles, pas la validité des ancres internes, qui ont été
  vérifiées manuellement par cohérence avec un renvoi déjà présent dans le même fichier).
- `docs/user/src/guide-utilisation.html` — **évalué, aucune modification nécessaire** : la
  capture `screenshots/03_Tableau_de_bord.jpg` illustre l'état normal du tableau de bord, sans
  bandeau « Dosage bloqué » actif ; le changement de feature-148 n'altère l'affichage que lorsque
  ce bandeau porte déjà une cause partagée avec un badge — condition transitoire, non représentée
  dans la capture existante. Aucune capture à refaire pour ce lot. **Observation hors périmètre**
  signalée sans correction : ce même fichier contient encore (ligne ~294) « Depuis la
  version 2.64, le Mode Boost… » — même défaut que C5, mais sur un fichier non listé dans le
  périmètre explicite de cette spec ; à traiter dans un futur lot si confirmé.

**Clôture** : tous les critères d'acceptation et la check-list de livrables sont cochés. La
spec **n'est pas déplacée vers `done/`** : au moment de cette passe, les modifications
(`data/app.js`, la suite de tests, cette spec) sont encore **non commitées** (voir `git status`)
— la règle de clôture exige des commits référencés, absents à ce stade. À déplacer dès que le
commit (ou la série de commits) couvrant B1-D2 et cette documentation est créé.
