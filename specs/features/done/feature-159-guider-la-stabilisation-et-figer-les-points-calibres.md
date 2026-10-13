## Métadonnées
- **ID** : feature-159
- **Slug** : guider-la-stabilisation-et-figer-les-points-calibres
- **Type** : feature
- **Statut** : **livré et clôturé** — périmètre RECADRÉ par l'utilisateur (voir section dédiée), gate GO sous conditions, revue Request changes puis **Approved** en contre-visite
- **Priorité** : normale
- **Version cible** : 2.75.0 (livrée)
- **Dernière mise à jour** : 2026-10-12
- **Spec(s) liée(s)** : feature-034 (le pas-à-pas de calibration), feature-156 (l'écran dédié qui
  l'héberge)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — **calibration**
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [ ] architect — *a priori* non : aucun contrat, aucun calcul nouveau. **À confirmer.**
- [x] **pool-chemistry** — ⚠️ OBLIGATOIRE : la calibration des capteurs pH/ORP est dans son
      périmètre explicite, et ce lot touche **la condition dans laquelle on déclenche un
      étalonnage**. ⚠️ Question centrale ci-dessous.
- [x] **ux-ui-designer — MANDAT COMPLET SUR L'ÉCRAN ENTIER** (règle du 2026-10-11 : « l'UX n'est
      pas optionnel quand on touche à l'interface », et son mandat **ne se découpe pas**).
- [ ] embedded-developer — non, rien côté firmware *a priori*.
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Sur l'écran de calibration, dire clairement **quand attendre et quand agir**, et **figer** les
boutons des points déjà calibrés.

## Contexte

**Demande utilisateur du 2026-10-12** : « *Dans la calibration, indiquer un message clair qui dit
d'attendre la stabilisation. Actuellement, l'utilisateur n'est pas assez guidé. Il doit déduire
s'il doit attendre parce que la stabilisation est en cours. Et quand on calibre à 7.0 et qu'on
passe à la suite, le bouton calibrer pour la partie 7.0 reste actif alors qu'il faudrait le
désactiver. Peut-être que c'est pareil pour le bouton de calibration 4.0.* »

### (1) ⚠️ LE MESSAGE EXISTE DÉJÀ — le problème est ailleurs, et c'est un motif connu

Vérifié dans `renderStability()` (`data/app.js`) : l'écran affiche bien
`● Stable — vous pouvez calibrer` ou `○ Stabilisation… (Δ60s 0.07)`.

**Donc ce n'est pas un texte manquant.** Trois causes plausibles, à instruire :

- ⚠️ **LE BOUTON « Calibrer » RESTE ACTIF PENDANT LA STABILISATION.** Le texte chuchote
  « attendez » pendant que le bouton dit « allez-y ». **C'est le motif déjà rencontré trois fois
  dans ce projet** : une commande qui autorise ce que l'état déconseille (feature-155 : bouton
  actif menant à un refus 400 ; correctif du 11/10 : bouton désactivé qui mentait). Un utilisateur
  n'a aucune raison de lire une ligne grise quand le bouton principal est cliquable.
- **Le texte est discret** : classe `cal-stability small`, sous la lecture, sans couleur forte
  dans l'état « attendez ».
- **L'état initial du HTML est MUET** : `Δ60 s : --` — purement numérique, aucun mot, tant que la
  fenêtre de mesure n'est pas remplie (< 3 échantillons). **C'est exactement le moment où
  l'utilisateur arrive sur l'étape**, donc le premier contact est avec un libellé technique.

### (2) Le figeage des boutons — mécanisme présent, à vérifier plutôt qu'à réécrire

`_calRefreshFrozenButtons()` désactive `btn-cal-ph-mid` si `_calActiveIdx.ph > 1` et
`btn-cal-ph-low` si `> 3`, et elle est appelée à chaque rafraîchissement
(`updateCalibrationReadouts`) **et** à la fin d'une attente (`_calEndAwait`).

⇒ **Sur le chemin nominal, le figeage DEVRAIT fonctionner.** Il faut donc **reproduire** avant de
corriger. Pistes à instruire, par ordre de vraisemblance :
- **le chemin d'ÉCHEC** : `_calEndAwait()` réactive inconditionnellement le bouton
  (`btn.disabled = false`) avant de re-figer ; sur un **délai dépassé** ou un **EZO injoignable**,
  l'étape n'est pas franchie ⇒ le bouton reste actif, légitimement — mais rien ne distingue
  visuellement ce cas d'un succès ;
- **la réouverture de l'écran** : `resetCalStepper()` **réactive TOUS les boutons** et revient à
  l'étape 0, sans tenir compte des points **déjà calibrés** connus du firmware (`phCalPoints`) ;
- un délai de rafraîchissement pendant lequel le bouton reste cliquable.

⚠️ **Confirmation sur l'intuition de l'utilisateur** : oui, le point 4.0 relève du **même
mécanisme** (même fonction, même motif) — ce qui vaut pour l'un vaut pour l'autre.

## Comportement attendu

- **À l'arrivée sur une étape de calibration, l'écran dit quoi faire**, en mots, sans que
  l'utilisateur ait à interpréter un nombre. L'état « en cours de stabilisation » doit être
  **impossible à confondre** avec « prêt ».
- **Un point déjà calibré ne propose plus son bouton** — quel que soit le chemin emprunté
  (succès, réouverture de l'écran, retour par le menu).
- Les deux points, **7.0 et 4.0**, se comportent **identiquement**.

### ⚠️ LA QUESTION CENTRALE, à trancher par `pool-chemistry` ET `ux-ui-designer`

**Faut-il DÉSACTIVER le bouton « Calibrer » tant que la mesure n'est pas stable ?**

- **Pour** : supprime la contradiction entre le texte et la commande ; un étalonnage fait sur une
  mesure instable fausse durablement la sonde, et c'est la régulation d'acide qui en hérite.
- **Contre** : ⚠️ **le principe du projet est « les plafonds protègent, le reste informe »** — une
  garde qui bloque plus qu'elle ne protège est un **défaut**. Une sonde vieillissante peut ne
  jamais atteindre le seuil de stabilité : l'utilisateur serait alors **empêché d'étalonner**,
  sans recours, avec un bouton mort et aucune explication. C'est le scénario du 10/10, où les
  gardes ont transformé un problème de sonde en impasse.
- **Pistes intermédiaires** (à arbitrer, pas à choisir ici) : bouton actif mais **secondaire**
  tant que c'est instable et **principal** quand c'est stable ; ou bouton actif avec
  **confirmation explicite** si l'on calibre sur une mesure instable ; ou désactivation **avec
  échappatoire visible et nommée**.
- ⛔ **Ce qui est exclu dans tous les cas** : laisser un bouton principal, plein, actif, à côté
  d'un texte gris qui dit d'attendre.

### Cas limites
- **Mesure qui ne se stabilise jamais** (sonde en fin de vie) : l'utilisateur ne doit **jamais**
  se retrouver sans issue.
- **Réouverture de l'écran alors que la sonde a déjà 2 points** : que montre le pas-à-pas ?
- **Recalibration volontaire d'un point déjà fait** : doit rester possible — par quel geste, si le
  bouton est figé ? ⚠️ **À ne pas oublier** : le code gère déjà ce cas côté détection
  (`CAL_SETTLE_MS`), donc il est prévu quelque part.
- **Échec** (EZO injoignable, délai dépassé) : l'état doit être **distinguable** d'un succès.
- **ORP** : le même pas-à-pas existe (`btn-cal-orp`). ⚠️ **À traiter ou à exclure explicitement**
  — ne pas laisser l'asymétrie s'installer par omission, c'est l'erreur de feature-156.

## Contraintes techniques
- ⚠️ **RESPONSIVE 390 px D'ABORD**, caractères **comptés** pour tout libellé partagé.
- ⚠️ **Ne PAS toucher** aux textes imposés par `pool-chemistry` (le conseil post-étalonnage de
  feature-156, les refus de la carte de correction).
- ⚠️ **REPRODUIRE AVANT DE CORRIGER** le défaut du bouton actif : le mécanisme de figeage existe
  et paraît correct sur le chemin nominal. Corriger sans avoir reproduit, c'est risquer de
  « réparer » un chemin qui marche et de laisser le vrai défaut.
- Vanilla JS, français, aucune dépendance nouvelle.
- ⚠️ Logique de décision **extraite et pure** si elle devient non triviale (ADR-0017), pour être
  validable **par mutation** — le harnais `test/js_extract.js` existe (ADR-0099).

## Livrables — check-list
- [ ] A1 — L'état « attendez » est **impossible à confondre** avec « prêt », dès l'arrivée sur
      l'étape (y compris avant le 3ᵉ échantillon, où l'affichage est aujourd'hui muet).
- [ ] A2 — Le bouton d'un point **déjà calibré** ne propose plus l'action, **sur tous les
      chemins** (succès, réouverture, retour par le menu).
- [ ] A3 — **7.0 et 4.0 identiques**. ORP traité ou **exclu explicitement**.
- [ ] A4 — Le chemin d'**échec** est visuellement distinct du succès.
- [ ] A5 — **Aucune impasse** : une sonde qui ne se stabilise pas laisse une issue nommée.
- [ ] A6 — Défaut du bouton actif **REPRODUIT** avant correction, scénario écrit dans la spec.
- [ ] B1 — Responsive **vérifié à 390 px**, caractères comptés.
- [ ] B2 — Tests sur la logique pure si elle change ; sinon le dire.
- [ ] C1 — `docs/features/page-ph.md`, `CHANGELOG.md`, et `docs/user/src/*.html` si le texte
      visible change.

## Sécurité chimique
- **Dosage impliqué : indirectement, mais réellement.** Un étalonnage est la **référence** de
  toute mesure ultérieure : l'étalonner sur une mesure instable fausse durablement la grandeur sur
  laquelle la régulation d'acide décide.
- **Sens du risque, dans les deux directions** : trop permissif ⇒ étalonnage sur une mesure
  instable ; trop strict ⇒ **impasse** pour une sonde vieillissante, et c'est précisément le
  scénario vécu les 10 et 11/10.
- **Questions au gate** : (a) faut-il désactiver le bouton tant que ce n'est pas stable, ou
  seulement le rendre moins proéminent ? (b) le seuil et la fenêtre de stabilité actuels
  (`CAL_STAB_THRESHOLD`, `CAL_STAB_WINDOW_S`) sont-ils les bons, et d'où viennent-ils ? (c) un
  étalonnage sur mesure instable doit-il être **journalisé** comme tel ?

## ADR attendu ?
- **Peu probable** — sauf si le gate impose une règle générale sur les commandes qui contredisent
  l'état affiché.

## Éléments visibles (pour doc-writer)
- `docs/features/page-ph.md` — le pas-à-pas de calibration.
- `CHANGELOG.md` — comportement utilisateur changé.
- `docs/user/src/guide-utilisation.html` si le texte visible change (puis les PDF à la release).
- **Aucun** endpoint, **aucun** champ nouveau attendu.

## Critères d'acceptation
- [ ] En arrivant sur l'étape 7.0, on comprend **sans interpréter un nombre** s'il faut attendre.
- [ ] Après avoir calibré 7.0, son bouton **ne propose plus l'action** — y compris après avoir
      fermé et rouvert l'écran.
- [ ] Idem pour 4.0.
- [ ] Une sonde qui ne se stabilise jamais **laisse une issue**.
- [ ] Vérifié **à 390 px** sur appareil réel.

## Plan de test manuel
1. Ouvrir la calibration, arriver sur l'étape 7.0 **avant** que la fenêtre soit remplie : que
   lit-on ?
2. Calibrer 7.0, avancer : le bouton 7.0 est-il figé ? Fermer et rouvrir l'écran : toujours ?
3. Idem 4.0.
4. Provoquer un échec (sonde débranchée) : l'état est-il distinct d'un succès ?
5. Téléphone, 390 px.

## Hors périmètre
- Le **firmware** de calibration EZO (`/calibrate_ph`, `sensors.*`) — ce lot est une affaire
  d'interface, sauf si le gate exige une journalisation.
- Les **textes imposés** par `pool-chemistry` en feature-155 et feature-156.
- L'écart **matériel** de la sonde de cet utilisateur (décision du 2026-10-12 : problème de
  matériel, pas de logiciel).

## ✅ VERDICT `pool-chemistry` — GO SOUS 13 CONDITIONS (2026-10-12)

### ⚠️ LE VRAI DÉFAUT N'EST PAS LA PROÉMINENCE DU BOUTON : C'EST LE SEUIL

L'indicateur affiche **« ● Stable — vous pouvez calibrer » alors que la sonde est encore à 0,79 à
1,10 unité pH de son équilibre.** Atténuer un bouton pendant une instabilité que l'appareil **ne
sait pas détecter** ne protège de rien.

Chiffré par le gate (modèle du premier ordre, `résiduel = étendue_W / (e^(W/τ) − 1)`), passage du
tampon 7,00 au tampon 4,00 avec le critère **actuel** (fenêtre 20 s, seuil 0,9 pH) :

| τ de l'électrode | « Stable » annoncé à | écart RÉEL à l'équilibre |
|---|---|---|
| 30 s (sonde saine) | t ≈ 40 s | **0,79 pH** |
| 60 s (vieillissante) | t ≈ 60 s | **1,10 pH** |

⚠️ Le commentaire du code justifie le seuil par « *un bruit de lecture ~0,07 pH* » — **0,9 vaut
13 fois ce bruit**. La décision « non bloquant » de feature-034 est confirmée ; **la valeur, elle,
est fausse au regard de sa propre justification écrite.**

### ⚠️ ET UNE DÉCOUVERTE HORS SUJET QUI PÈSE PLUS QUE LE LOT (C10)

**Rien ne suspend le dosage pendant la calibration.** `armStabilizationTimer()` n'est armé
qu'**après** un `Cal` réussi. Pendant que les sondes trempent dans les béchers, l'appareil
**continue de réguler sur ce qu'elles lisent**. Côté ORP : étalon à 470 mV contre consigne 530 ⇒
erreur de 60 mV sur 200 de pleine échelle ⇒ **il peut injecter du chlore dans le bassin en se
fondant sur la lecture d'un bécher.** La condition C1 **allonge** cette fenêtre (20 s → ≥ 60 s par
point, 2-3 min côté ORP) : le gate refuse de l'imposer sans traiter la conséquence.
⇒ **Traitement dans CE lot : texte seul** (T8/T15). Le mécanisme de suspension fait l'objet de
**R1**, avec son propre passage au gate. ⚠️ *« Une suspension sans minuteur de relâche serait une
régression de sécurité, pas une amélioration »* — elle resterait collée si le navigateur meurt.

### La chaîne de conséquences, direction par direction

**Point BAS étalonné trop tôt** ⇒ pente ≈ **74 %** ⇒ mesure fausse proportionnellement à l'écart
à 7. Déjà détectable : la bande de santé est 90-110 % (feature-154) — **personne ne fait le lien**.

**Point MILIEU étalonné trop tôt** (scénario catastrophique, et c'est celui que le geste
« Recalibrer » va rendre facile) : zéro décalé de −0,79 ⇒ la boucle acide **s'arrête** ⇒ le pH vrai
monte ⇒ `kPhOrpInhibit` (7,60) est comparé à la mesure **falsifiée** ⇒ **l'inhibition ORP ne se
déclenche jamais** ⇒ à pH vrai 8,0 la part active du chlore tombe à ~24 % ⇒ la boucle ORP conclut
au manque et **surdose**. ⚠️ **C'est l'aggravation exacte de l'épisode du 10/10, fabriquée par un
clic trop rapide.** Et un biais d'étalonnage constant est **indiscernable du droop** d'un P pur
dans les journaux — la confusion du 28/08, qui a coûté une journée de suspicion au firmware.

### (a) Bouton : **ATTÉNUER, jamais `disabled`** — confirmé, mais amendé

Trois motifs : l'impasse vécue les 10-11/10 par cet utilisateur (une jonction dégradée **dérive en
permanence**, elle ne satisfera jamais un critère serré — un bouton mort la condamnerait) ; le
principe « les plafonds protègent, le reste informe » ; et la symétrie des deux erreurs déjà
commises — bouton actif qui ment (feature-155) **et** bouton désactivé qui ment (11/10).

⚠️ **AMENDEMENT IMPOSÉ** : l'atténuation seule n'est **pas un consentement éclairé** et repose sur
la couleur (daltonisme). ⇒ **`confirmDialog` sur tout clic émis dans un état autre que
« stable »** (instable **ou** muet). Jamais dans le chemin nominal. Le lien « Calibrer quand
même » séparé devient **sans objet** — ce serait un troisième chemin vers la même action.

⚠️ **Dissymétrie assumée** : `disabled` est **autorisé** pour « déjà fait » et « calibration en
vol » (il existe un autre chemin), **interdit** pour « mesure instable » (il n'y en aurait aucun).

### (b) Seuils — faux, et de source illégitime (ADR-0098)

⚠️ **`CAL_STAB_WINDOW_MS` vaut 20 000 ms, pas 60 s.** Le HTML affiche `Δ60 s` à l'état initial
(3 emplacements) puis le JS affiche `Δ20s` : **l'écran annonce une fenêtre qu'il n'utilise pas.**

| Constante | Valeur imposée | Source déclarée (ADR-0098) |
|---|---|---|
| `CAL_STAB_WINDOW_MS` | **60 000** | **Matériel** — Atlas Scientific, stabilisation 1-2 min avant `Cal` |
| `CAL_STAB_THRESHOLD.ph` | **0,05** | **Décision produit** — `kPhGuardReleaseHysteresis`, plus petite différence sur laquelle le projet change une décision de dosage |
| `CAL_STAB_THRESHOLD.orp` | **3 mV** | **Physique** — Nernst : 0,05 pH × 59,16 mV/pH ≈ 2,96 mV |

⚠️ **LA STATISTIQUE CHANGE, PAS SEULEMENT LE NOMBRE.** `max − min` est une **étendue** : elle
croît avec le nombre d'échantillons et mélange bruit et dérive — avec 12 points, 0,05 ne serait
**jamais atteint** ⇒ indicateur éternellement orange ⇒ on apprend à l'ignorer. **Le pire des
résultats.**
⇒ Imposé : **dérive médiane moitié/moitié** —
`D = |médiane(moitié récente) − médiane(moitié ancienne)|`. Résiduel garanti : **0,018 pH** à
τ = 30 s, **0,060** à τ = 60 s (contre 0,79-1,10 aujourd'hui).
⇒ Rétention robuste à la cadence (5 s avec WS, **30 s sans**) : écarter > 60 s **sauf** s'il reste
moins de 6 échantillons ; verdict seulement si couverture ≥ 45 s **et** n ≥ 6 **et** ≥ 3 de chaque
côté ; sinon **muet**. Sans WS : plus tard et plus conservateur, **jamais bloquant**.
⚠️ Si le vert s'avérait inatteignable sur une sonde saine : **ne pas gonfler la constante** —
ADR-0098 (« c'est le mécanisme qu'il faut retirer, pas la constante qu'il faut ajuster »).

### (c)-(f) Les autres réponses

- **(c) Journaliser OUI, bloquant.** Il n'existe **pas de dé-calibration** ; l'effet est
  irréversible et indiscernable du droop. Le firmware **ne peut pas** le savoir seul (la fenêtre
  vit dans le navigateur). ⇒ `POST /calibrate_ph|_orp` acceptent **deux champs optionnels**
  (`stable`, `drift`) ; absents ⇒ comportement **strictement inchangé** ; `stable == false` ⇒ une
  ligne `warning` au **point d'exécution réel** ; `drift` hors bornes ⇒ **ignoré, jamais un refus**
  (une sécurité de journalisation ne doit pas pouvoir empêcher un étalonnage).
  ⛔ **Pas de nouveau drapeau de santé** : la conséquence est **déjà détectée** par la bande de
  pente 90-110 %. Ce qui manque est **le lien au moment du geste** (C8).
- **(d) Recalibration ASYMÉTRIQUE.** ⚠️ `Cal,mid` **efface tous les autres points** (datasheet
  Atlas, déjà écrit dans `docs/subsystems/sensors.md:202`). La proposition « rouvre cette étape
  sans toucher aux autres » est donc **physiquement impossible** pour le 7.0. ⇒ 7.0 : libellé
  « refaire les 2 points », `danger: true`, retour à l'étape 0. 4.0 et ORP : lien simple.
  *« Une friction uniforme est une friction qu'on apprend à cliquer sans lire. »*
  ⚠️ Le toast « réussie (1/2 points) » après recalibration du milieu est **EXACT** — ne pas le
  « corriger » en 2/2, il annonce le travail restant.
- **(e) ORP : même mécanique, nombres et attente DIFFÉRENTS.** Pas de membrane mais un **film
  d'oxydes** : 2-3 min typiques, parfois sans jamais se poser. ⚠️ **Cette sonde-ci est suspecte**
  (11/10, hypochlorite ~1000× concentré) — un verrou dur la condamnerait. Et **un seul point, donc
  aucune pente pour rattraper** : une erreur y est un décalage **pur, invisible**. 25 mV de biais
  = **12,5 % de pleine échelle de commande, en permanence**, sur une consigne déjà calée à la
  bandelette.
- **(f) L'intro MENT, le pas-à-pas a raison.** L'ordre 7.0 → 4.0 est **imposé par le matériel**.
  ⚠️ Et la condition C5 **repose entièrement** sur cette garantie : `phCalPoints` n'est un compteur
  interprétable (1 ⇒ milieu fait) **que si l'ordre est garanti**. ⇒ Corriger **DEUX** emplacements :
  `data/index.html:1076` **et** `docs/UPDATE_GUIDE.md:879` — l'un ment à l'utilisateur, l'autre à
  la prochaine session de développement.

### Les 13 conditions

**C1** réparer le critère **avant** tout habillage (valeurs, statistique, rétention, sources
ADR-0098 au site de déclaration ; supprimer `Δ60 s` des 3 emplacements HTML) · **C2** jamais
`disabled` pour instabilité + `confirmDialog` si état ≠ stable, avec le `D` **capturé au clic** ·
**C3** recalibration asymétrique · **C4** **le nombre affiché EST le nombre qui décide**, même
unité, **virgule française** (`0,42`, défaut actuel via `toFixed(2)`) · **C5** `resetCalStepper()`
consulte le firmware, et ⚠️ **ne réveille PAS le conseil transitoire de feature-156** (il
réapparaîtrait sans qu'aucun étalonnage n'ait eu lieu, et perdrait sa crédibilité) ; `phCalPoints
== -1` ⇒ **n'armer aucun bouton** · **C6** corriger l'ordre aux deux endroits · **C7** journaliser
l'étalonnage non stabilisé · **C8** boucler sur la pente après `Cal` (**informatif**,
`constants.h:1824` interdit d'en faire une garde) · **C9** échec persistant, causes distinguées,
⚠️ **ne jamais affirmer que la sonde est hors service ni que la calibration précédente est
intacte** — sur un délai dépassé, l'appareil **ne sait pas** · **C10** prévenir que la régulation
**ne se suspend pas** · **C11** les trois étapes, cinq états chacune · **C12** logique pure +
mutation, ⚠️ **le mutant qui compte** : remettre 0,9 / 20 s doit **tuer** la suite — c'est celui
qui survit depuis feature-034 · **C13** 390 px, caractères comptés (tous les libellés tiennent).

### Textes imposés — mot pour mot

**T1** `Lecture de la sonde en cours — ne calibrez pas encore` · **T2** `La mesure bouge encore —
attendez` + `écart 0,42 pH sur 60 s` · **T3** `Stable — vous pouvez calibrer` (inchangé) · **T4**
`Calibration en cours — ne retirez pas la sonde du tampon` · **T5** `Calibration non confirmée —
sonde injoignable. Vérifiez le câblage, puis réessayez.` (+ variantes délai / message) · **T6/T7**
modales d'instabilité : *« Étalonner maintenant fige cet écart dans la sonde : toutes les mesures
suivantes seront décalées d'autant, et le dosage s'appuiera dessus. Attendre une minute de plus ne
coûte rien. »* — boutons `Calibrer quand même` / `Attendre` · **T8** intro réécrite (ordre imposé,
effacement du 4.0, **et** la phrase sur la régulation à mettre sur Arrêt) · **T9** `Une sonde redox
demande souvent 2 à 3 minutes pour se poser.` · **T10** ligne de journal `warning` · **T11** modale
de recalibration 7.0 (`Refaire les 2 points`) · **T12** `Le nouvel étalonnage remplace l'ancien. Il
n'existe pas de retour en arrière.` · **T13** EZO injoignable · **T14** pente hors bande · **T15**
`N'oubliez pas de remettre la régulation pH et ORP en Auto.`

### Recommandations (hors périmètre, à arbitrer)

**R1** spec de suivi : suspendre le dosage pendant une calibration, **bornée par un minuteur
firmware** · **R2** alerter sur une régulation laissée en Arrêt (on fabrique l'oubli en demandant
de couper) · **R3** forcer `/data` à 5 s pendant une étape active · **R4** afficher la date du
dernier étalonnage (`phCalEpoch` existe) · **R5** ADR probablement justifié, contre le pronostic
de cette spec.

### Portée de ce que le gate a vérifié (et pas)

Il **n'a pas** mesuré le bruit réel en bécher (ADR-0098 l'interdit comme source) ; **n'a pas**
audité l'écran « État sonde pH » ni la Température ; **n'a pas** cherché si une alerte
« régulation en Arrêt » existe déjà. ⚠️ Et le comportement de l'EZO devant un `Cal,low` **sans**
`Cal,mid` préalable n'est documenté **nulle part dans ce dépôt** — d'où l'ordre imposé **côté
interface** plutôt qu'un refus matériel supposé.

## ⚠️ RECADRAGE DU PÉRIMÈTRE PAR L'UTILISATEUR (2026-10-12) — fait autorité

> « *Je crois qu'il y a une incompréhension. Ce que j'ai indiqué, c'est que le message n'est pas
> clair quand c'est en cours de stabilisation. Et le bouton reste actif même quand on passe à
> l'étape d'après. Je préférais par ex qu'on permette à l'utilisateur de recommencer plutôt que
> de laisser les boutons des étapes précédentes actifs.* »

**Ce lot ne comporte donc que TROIS choses**, et rien d'autre :

1. **Le message pendant la stabilisation est clair.** ⚠️ Le vrai coupable est l'**état MUET** —
   `Δ60 s : --`, purement numérique, qui est le **premier contact** avec l'étape (10 à 45 s). Il
   ne dit pas qu'il faut attendre, il ne dit rien.
2. **Les boutons des étapes précédentes sont figés** — le défaut reproduit (`resetCalStepper()`
   réactive tout sans consulter `phCalPoints`).
3. **Un geste pour RECOMMENCER**, qui remplace le figeage comme issue. C'est la préférence
   explicite de l'utilisateur, et elle est juste : aujourd'hui le seul moyen de recommencer **est
   le bug qu'on corrige**.

### ⛔ CE QUI SORT DE CE LOT — et pourquoi je l'avais mal présenté

⚠️ **L'atténuation du bouton pendant l'instabilité n'a JAMAIS été demandée.** Elle vient de moi,
puis de `ux-ui-designer`. L'utilisateur n'a pas demandé d'agir sur le bouton **en fonction de la
stabilité** — il a demandé de figer les boutons des étapes **déjà faites**. Ce sont deux sujets
différents, et je les ai fondus.

⇒ **Sortent de ce lot** : l'atténuation `secondary`/`primary` (C2), la confirmation sur mesure
instable (C2), **la correction des seuils et de leur statistique (C1)**, la journalisation
firmware (C7), le renvoi vers la pente (C8), l'avertissement de non-suspension (C10).

⚠️ **Ces points ne sont PAS abandonnés** : ils constituent `bug-160`, et deux d'entre eux sont
**des défauts de sécurité réels, indépendants de cette demande** — un indicateur qui annonce
« stable » à 0,8-1,1 pH de l'équilibre, et une régulation qui continue de doser pendant que les
sondes sont dans les béchers. Les traiter ici les aurait noyés dans une demande d'ergonomie.

### ✅ CE QUI RESTE DU GATE, parce que ça porte sur le geste demandé

- **C3 — la recalibration est ASYMÉTRIQUE.** ⚠️ Fait matériel, pas une préférence : `Cal,mid`
  **efface tous les autres points** (datasheet Atlas, déjà écrit dans `sensors.md:202`). « Rouvrir
  cette étape sans toucher aux autres » est donc **impossible** pour le 7.0. Recommencer le 7.0,
  c'est recommencer les deux. Textes **T11** et **T12** imposés.
- **C5 — `resetCalStepper()` consulte `phCalPoints`/`orpCalPoints`**, seule vérité. Et ⚠️ **ne
  réveille PAS le conseil transitoire de feature-156** (il réapparaîtrait sans qu'aucun étalonnage
  n'ait eu lieu). `phCalPoints == -1` ⇒ **n'armer aucun bouton** (texte **T13**).
- **C6 — corriger l'ordre, aux DEUX endroits** (`index.html:1076` et `UPDATE_GUIDE.md:879`). Reste
  **dans** ce lot : la condition C5 **repose** sur la garantie d'ordre (le compteur n'est
  interprétable que si l'ordre est tenu), et l'utilisateur qui veut « recommencer » doit savoir
  que refaire le 7.0 oblige à refaire le 4.0. ⚠️ Mais **sans** la phrase sur la régulation à
  mettre sur Arrêt, qui part avec `bug-160`.
- **C9 — l'état d'échec est persistant** et ses causes distinguées (**T5**). Reste : sans lui, une
  étape échouée ressemble à « pas encore tentée », donc on ne sait pas s'il faut **recommencer**.
- **C11** (les trois étapes traitées identiquement, **ORP compris**), **C12** (logique pure +
  mutation si elle devient non triviale), **C13** (390 px, caractères comptés).
- **Les cinq états** : conservés, mais pour la **lisibilité** (demande n°1), pas comme support
  d'un verrou. Textes **T1, T2, T3, T4** imposés.
- **C4 — le nombre affiché est celui qui décide**, virgule française. Reste : si on montre un
  écart, il doit être le bon.

### Livrables — check-list RÉVISÉE (remplace celle du haut)
- [ ] A1 — L'état **muet** dit qu'il faut attendre (T1). Les cinq états sont distincts et ne
      reposent pas sur la seule couleur.
- [ ] A2 — Le bouton d'un point **déjà calibré** ne propose plus l'action, **sur tous les
      chemins** — y compris après fermeture/réouverture de l'écran.
- [ ] A3 — **Un geste pour RECOMMENCER**, asymétrique (C3). ⛔ Pas de lien uniforme.
- [ ] A4 — L'échec est **persistant** et distinct du succès (T5, C9).
- [ ] A5 — **7.0, 4.0 et ORP** traités identiquement.
- [ ] A6 — L'ordre imposé est écrit aux deux endroits (C6).
- [ ] B1 — 390 px, caractères comptés. B2 — tests si la logique devient non triviale.
- [ ] C1 — `docs/features/page-ph.md`, `CHANGELOG.md`.
⛔ **Aucun seuil touché. Aucun changement firmware. Aucune confirmation sur l'instabilité.**

## Notes de clôture (2026-10-12)

### ⚠️ Ce lot a d'abord été MAL CADRÉ — par moi

J'ai relayé l'élargissement du gate `pool-chemistry` comme s'il était la décision à prendre par
l'utilisateur. Il a dû me reprendre : *« Je crois qu'il y a une incompréhension. »* Sa demande
portait sur **trois** choses ; j'en avais fait **treize**, touchant `src/`, les seuils et la
documentation.
⚠️ **Et j'avais fondu deux sujets distincts** : il demandait de figer les boutons des étapes
**déjà faites** ; l'atténuation du bouton **selon la stabilité** venait de moi et de
`ux-ui-designer`. ⇒ Leçon : quand un gate élargit, **ne pas présenter l'élargissement comme une
question au demandeur**. Le trier d'abord : ce qui sert SA demande reste, le reste devient une
spec à part.

### Le défaut d'origine : reproduit avant correction (A6 tenu)

Rouvrir l'écran appelait `resetCalStepper()`, qui réactivait **inconditionnellement** les deux
boutons et remettait le compteur d'étape à 0 **sans consulter `phCalPoints`**. Le figeage se
fondant sur ce compteur local remis à zéro, **aucun rafraîchissement ultérieur ne corrigeait** :
le bouton restait actif en permanence.
⚠️ **Conséquence que personne n'avait vue avant de reproduire** : le seul moyen de recalibrer
volontairement **ÉTAIT ce bug**. Le fermer sans fournir le geste de remplacement aurait rendu la
recalibration impossible — c'est précisément ce que l'utilisateur avait demandé en préférant
« *qu'on permette à l'utilisateur de recommencer* ».

### ⚠️ Le bloquant de revue : un FAIL-OPEN dans un lot qui existe pour FIGER

Le `code-reviewer` a exhibé **deux chemins réels** (rechargement d'onglet sur `#/ph`, et `/data`
en échec) où `latestSensorData` est absent ⇒ compteur `undefined` ⇒ **rien de figé, et aucun lien
« Recommencer » visible, pour toute la session** — le calcul n'ayant lieu qu'à l'ouverture,
l'arrivée tardive des données ne réparait rien.
Correctif en deux moitiés, la seconde indispensable : l'inconnu **ne fait plus autorité**
(`_calIsPtsKnown`), et une **auto-réparation UNE SEULE FOIS** au premier compteur reçu. ⛔ Pas un
recalcul permanent : après un clic « Recommencer », le firmware annonce **encore** le point comme
calibré, et un recalcul continu **re-figerait le bouton aussitôt**, annulant le geste que ce lot
existe pour fournir.

### ⚠️ Le mutant qui survivait à cause d'une DUPLICATION

Le mutant `>= -1` **ne mourait pas** tant que la fonction extraite **dupliquait** l'expression au
lieu d'appeler la fonction commune : le test passait au vert sur un code portant deux fois la même
règle, et casser l'une ne cassait rien. Supprimé avant validation.
**La contre-visite a trouvé qu'il en restait DEUX autres** (calcul de `baseline`, pH et ORP) —
quatre sites au total, alors que le commentaire promettait « une SEULE expression, jamais
dupliquée ». ⚠️ *Une garantie écrite plus forte que le code est pire que pas de garantie.* Les
quatre sites appellent désormais la même fonction.

### Et l'en-tête affirmait ce qu'il ignorait

Compteur inconnu ⇒ l'en-tête affichait **« Aucun point calibré »** pendant que l'indication juste
en dessous disait « inconnu ». Règle C9 du gate appliquée au même champ d'état : il dit désormais
« État de calibration inconnu ». **Dire ce qu'on a vérifié, jamais un jugement.**

### ⚠️ Ce que ce lot ne corrige PAS — et le changelog le dit

Il rend l'attente **lisible**, il ne rend pas l'indicateur **juste**. Le seuil qui décide
d'afficher « Stable — vous pouvez calibrer » est **inchangé** et reste trop permissif
(`bug-160`). Le changelog portait « *l'écran guide enfin, et ne ment plus* » — **affirmation
fausse, corrigée** : elle aurait enterré `bug-160` dans un document relu dans six mois.
Et l'avertissement qui compte le plus y figure : **pendant une calibration, l'appareil continue de
réguler sur le contenu des béchers.**

### Couverture — ce qui est prouvé et ce qui ne l'est pas

**Prouvé** : `_calPointsAcquired(product, pts)` extrait en fonction pure, harnais
`test/cal_points_acquired_vectors.js` (ADR-0099, extraction du texte réel), 16ᵉ contrôle
d'hygiène + job CI. Trois mutants tués, dont le `>= -1` qui aurait attrapé le bloquant lui-même.
**Non prouvé, et dit comme tel** : le reste de la logique (`resetCalStepper`, `renderStability`,
`_calRestartPoint`) est couplé au DOM, non extractible par le harnais du projet. `test-engineer`
a **refusé de fabriquer un test qui ne prouverait rien** et a fait une relecture ligne à ligne —
c'est la bonne réaction, mais ce n'est pas une protection automatisée.

### Validation finale (mesurée, ADR-0073)

`pio run -e esp32dev_core3` SUCCESS, Flash 50,0 %, zéro warning neuf · **69 suites / 2 141 cas**,
zéro régression · **16** contrôles d'hygiène verts · `node --check` OK · image FS dans les 512 Ko
(contenu réel ~203 Ko — le 524 288 est du **remplissage** `mklittlefs`, pas un plafond atteint) ·
`src/` non touché par le lot, hors le bump de version.

⚠️ **CAPTURE À REFAIRE (seule étape humaine)** : `screenshots/07_pH_Calibration.jpg` — message de
stabilisation en clair au lieu de `Δ60 s : --`, et liens « Recommencer » visibles sur un point
déjà calibré. `08_ORP_Calibration.jpg` si elle montre un état de stabilité.
