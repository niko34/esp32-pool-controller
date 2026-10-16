# feature-168 — La correction de mesure pH n'est plus bornée

## Métadonnées
- **ID** : feature-168
- **Slug** : correction-ph-sans-borne
- **Type** : feature — **levée d'une limite**, avec requalification de deux gardes chimiques
- **Statut** : doing
- **Priorité** : haute — l'utilisateur ne peut pas corriger un décalage de mesure **réel et mesuré** de 1,0 pH
- **Version cible** : 2.82.0
- **Dernière mise à jour** : 2026-10-16
- **Spec(s) liée(s)** :
  `done/refonte-seuils-ph-fixes-derives.md` (**la spec qui a créé la dérivation qu'on défait ici**) ·
  `done/bug-plancher-ph-offset-legitime-et-securite-desarmee.md` (l'incident du 25/08) ·
  `todo/bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md` (même filet brut) ·
  `todo/feature-137-perturbation-de-mode-commun-sur-les-electrodes.md` (la cause physique) ·
  ADR-0039 (nature de l'offset) · ADR-0048 (seuils pH dérivés — **à réécrire**) ·
  ADR-0100 (un seuil est servi, jamais réécrit côté client)

## Domaines touchés
- [x] **Capteurs (pH / ORP / température)** — correction de mesure
- [x] **Régulation (pump_controller, PID)** → **passage obligatoire par `pool-chemistry`** ✅ fait, 2 passes
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP / WebSocket** — `/get-config`, `/save-config`
- [x] **MQTT / Home Assistant** — alertes
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] **Historique / persistance** — relecture NVS de la correction
- [x] **Frontend (`data/`)** — écran de calibration et correction pH

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT** ✅ **2 passes rendues** (verdict ci-dessous)
- [x] **ux-ui-designer** — l'écran reçoit un avertissement de 4 phrases et perd sa borne de
      saisie. ⚠️ Mandat sur **l'ÉCRAN ENTIER**, captures jointes au brief.
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La correction de mesure pH est bornée à **±0,50**. L'utilisateur a besoin de **+1,00**.
Cette spec **retire la borne** et, parce que deux garde-fous chimiques en **dérivaient**,
les refonde sur les limites chimiques de l'eau réelle.

## Contexte

Mots de l'utilisateur, le 2026-10-16 :

> « J'ai une eau de piscine à 8 de pH. Je vais la corriger, ce n'est pas le sujet.
> Je mesure 7 quand la sonde est dans le circuit de filtration à cause des courants
> de fuite du circuit de la piscine. Je ne peux pas corriger à 8 dans l'écran de
> calibration et correction de pH. Je voudrais qu'il n'y ait pas de limite lors de
> cette correction. Le message d'avertissement peut rester, mais je ne veux pas bloquer. »

⚠️ **Ce n'est pas une suspicion de sonde.** Le projet l'a établi trois fois : la correction
pH compense un **décalage de MESURE** causé par les **courants de fuite de la pompe de
filtration** (campagne de bruit 22-23/08 : dispersion ×5 pompe en marche, couplage
électrique via le verre à ~100 MΩ, test sur batterie). C'est un phénomène **documenté de
cette installation**, pas une dérive d'électrode.

### Décision explicite de l'utilisateur

`pool-chemistry` a proposé une borne large calée sur la chimie (**−0,90 / +1,20**).
Les deux options lui ont été présentées **chiffrées, avec leur justification chimique**.
⚠️ **Il a choisi « vraiment aucune borne », en connaissance de cause.** La décision est
la sienne et n'est pas à rediscuter ; le travail consiste à rendre ce geste **aussi sûr
que possible dans ce cadre**.

## Le défaut structurel que la demande met au jour

`kPhOffsetMax` n'est **pas une borne de saisie isolée** : deux filets de sécurité en
**dérivent** (`src/constants.h:1808,1829`).

```
kPhFloorRawHard   = kPhFloorCorrected - kPhOffsetMax;   // 6,20  [sur le pH BRUT]
kPhCeilingRawHard = kPhOrpInhibit     + kPhOffsetMax;   // 8,10  [sur le pH BRUT]
```

⇒ **La correction commande les gardes.** L'élargir les desserre **sans que personne ne le
décide**. L'inversion est donc obligatoire : les **limites chimiques absolues de l'eau
réelle** deviennent les constantes nommées, et plus rien n'en dérive.

🔴 **Et un défaut préexistant apparaît au passage** : `kPhCeilingRawHard` protège d'une
correction **NÉGATIVE** mais se calcule avec la borne **POSITIVE**. La coïncidence ne tenait
que par la symétrie des bornes. Elle disparaît avec la dérivation.

## Comportement attendu

1. La correction pH accepte **toute valeur finie** de l'échelle pH. Aucun refus, aucun écrêtage.
2. Un **avertissement non bloquant** apparaît dès que la correction dépasse **0,50** en valeur
   absolue — l'ancienne borne, qui reste le **signal** d'un défaut matériel possible.
3. Les deux filets sur la mesure **brute** sont **fixes** : refus d'acidifier sous **5,50**,
   refus d'alcaliniser au-dessus de **8,50**. Ils ne bougent plus avec la correction.
4. Au-delà d'une correction de **+1,20**, le filet brut mord **avant** la consigne : la
   régulation est en **impasse**. L'appareil le **dit** au lieu de se fermer en silence.
5. La correction **survit au redémarrage** telle qu'elle a été saisie.

## Verdict `pool-chemistry` — GO SOUS CONDITIONS (2ᵉ passe, remplace la 1ʳᵉ)

> Justification chimique des deux filets : sous ~5,0, l'acide rencontrant l'hypochlorite
> libère du **chlore gazeux** (0,5 de marge ⇒ 5,50). À 8,50, il ne reste que **9 %** de
> chlore actif (HOCl) pour un pKa de 7,5.

### Conditions bloquantes

- [x] **C1** — `kPhRawFloorAbsolute = 5.50f`, `kPhRawCeilingAbsolute = 8.50f`, littéraux à
      justification chimique propre (ADR-0098). `kPhFloorRawHard`/`kPhCeilingRawHard` leur
      sont **égaux**. ⛔ Plus **aucune** occurrence de `kPhOffsetMax` dans leur définition.
- [x] **C2** — Supprimer `kPhOffsetMin`/`kPhOffsetMax` et les champs servis
      `ph_offset_min`/`ph_offset_max` (`web_routes_config.cpp:299-300`).
      🔴 **BLOQUANT CACHÉ** : `roundPhOffsetToStep()` (`ph_guard_logic.h:113-124`) **écrête**
      sur ces bornes, et elle est appelée **au rechargement NVS** (`config.cpp:700`) *et*
      dans `/save-config:1440`. ⚠️ **Laissée telle quelle, elle ramènerait +1,00 à +0,50 au
      premier redémarrage**, en silence. **L'arrondi au pas reste ; l'écrêtage part.**
- [x] **C3** — **Salubrité, pas chimie** : `isPhOffsetValid(o)` devient
      `isfinite(o) && fabsf(o) <= kPhOffsetScaleSpan`, `kPhOffsetScaleSpan = 14.0f`.
      NaN / inf / 1e30 refusés (HTTP 400, remise à 0 au rechargement NVS) ; **toute** valeur
      finie du domaine acceptée. ⛔ Ce n'est **pas** une borne de réglage.
- [x] **C4** — `static_assert` : **retirer** (iv) `:1881` et (ix) `:1906` — ils encodaient
      « le filet ne mord jamais avant le seuil corrigé », qui n'est plus vrai et ne peut plus
      l'être. **Les retirer consciemment**, pas les laisser tomber. **Ajouter** l'ordre absolu
      `kPhRawFloorAbsolute < kPhFloorCorrected && kPhCeilingCorrected < kPhRawCeilingAbsolute`.
      Conserver la rondeur au dixième.
- [x] **C5** — **Zone d'impasse** : constante **calculée**
      `kPhOffsetImpasseThreshold = kPhFloorCorrected - kPhRawFloorAbsolute` (= 1,20).
      Au-delà, les refus du **filet brut seul** sont **exclus** du compteur 3 refus / 24 h.
      ⚠️ *Justification* : en impasse, l'acidification est **déjà** arrêtée par le filet —
      la bascule en manuel n'ajoute **aucune** protection, elle **retire** la régulation.
      ⛔ Le compteur reste **intact** pour le plancher corrigé et toutes les autres gardes.
- [x] **C6** — Alerte d'impasse dédiée, `critical` + MQTT, débit limité :
      `"[pH] Impasse de régulation : avec une correction de +1,40, la consigne 7,20 exige une mesure brute de 5,80, sous le refus d'acidifier fixé à 5,50. L'appareil cessera d'acidifier avant d'atteindre la consigne."`
- [x] **C7** — Journalisation dans `setPhOffsetStamped` (`config.cpp:185`), seuil nommé
      `kPhOffsetUnusualAbs = 0.50f` (**l'ancienne borne — le SIGNAL de défaut matériel ne
      doit pas disparaître avec elle**), `critical` + MQTT :
      `"[pH] Correction de mesure inhabituelle : +1,00 pH (au-delà de 0,50, ancienne borne de l'appareil). Rien ne la limite : elle est appliquée telle quelle. Le refus d'acidifier sur la mesure brute reste fixé à 5,50, quelle que soit la correction."`
- [x] **C8** — Avertissement UI, **non bloquant**, dès `|valeur| > 0,50` :
      `"Au-delà de 0,50 pH, l'appareil régule sur une valeur qu'il ne mesure pas : si cette correction est fausse de 0,3 pH, l'eau réelle sera 0,3 pH plus acide que la consigne. Un écart de cette ampleur vient de l'installation — ce réglage ne vaut que ce que vaut votre test. Rien ne borne cette correction ; le seul filet restant est le refus d'acidifier sous 5,50 de mesure brute. Au-delà de +1,20, ce refus survient avant la consigne : l'appareil arrêtera d'acidifier sans l'avoir atteinte. Revérifiez par un test manuel après tout étalonnage, toute intervention sur la pompe de filtration, et au moins une fois par mois."`
      ⚠️ **TEXTE RÉVISÉ (3ᵉ passe du gate)** : la 2ᵉ phrase a été **ajoutée** parce que C14
      masque l'encadré permanent qui la portait. ⛔ Sans elle, le seul texte restant à l'écran
      ne dirait plus que l'écart vient de **l'installation** — acquis des incidents du 25/08 et
      du 11/10, à ⛔ **ne pas perdre en chemin**.
- [x] **C9** — **Pas-à-pas d'accord** : supprimer `PH_TUNE_RANGE = 1.00`
      (`data/app.js:2308`, et `:2447-2448, 2522-2523, 2541-2542`). L'excursion devient
      l'**échelle physique seule**, `[0 ; 14]` autour de la lecture vive, au pas
      `ph_offset_step` **servi**. ⛔ Aucun seuil neuf côté client (ADR-0100).
      `data/index.html:1537` à reformuler. Côté firmware,
      `PhTuneRefusal::OutOfRange` (`ph_tune_logic.h:140`) ne garde que la salubrité de C3.
- [x] **C10** — **Péremption** : `kPhOffsetConfirmIntervalDays = 30`. Au-delà, **et seulement
      si** `|offset| > kPhOffsetUnusualAbs`, lever `ph_offset_stale` (bandeau + MQTT +
      `warning`). **Non bloquant**, conforme à la décision utilisateur.
- [x] **C11** — **Tests** : supprimer **explicitement** l'assertion de symétrie
      `test/test_native_ph_offset/test_ph_offset.cpp:219` (`kPhOffsetMin + kPhOffsetMax == 0`)
      — elle fige une symétrie qui n'a plus lieu d'être. Remplacer les littéraux
      0,50 / 6,20 / 8,10 (`:216,436,441`, `test_ph_guard.cpp:538-541,651-688`) par 5,50 / 8,50.

- [x] **C5bis** — ⚠️ **L'IMPASSE EXISTE DES DEUX CÔTÉS** (trou trouvé par `architect`, concédé
      par le gate : *« l'asymétrie n'est pas justifiable »*).
      `kPhOffsetImpasseThresholdHigh = kPhCeilingCorrected - kPhRawCeilingAbsolute` (= −0,90,
      **calculée**, ⛔ jamais littérale). Offset **≤** ce seuil ⇒ les refus du filet **haut
      seul** sont exclus du compteur 3 refus / 24 h, exactement comme le bas.
      ⛔ Le compteur reste intact pour le plafond **corrigé** et toutes les autres gardes.
      Alerte dédiée, `critical` + MQTT, **même** débit limité que C6 :
      `"[pH] Impasse de régulation : avec une correction de −1,10, le plafond 7,60 exige une mesure brute de 8,70, au-dessus du refus d'alcaliniser fixé à 8,50. L'appareil cessera d'alcaliniser avant d'atteindre le plafond."`
      > *Pourquoi* : `ph_plus` est **configurable**, et le projet a **déjà payé** ce raisonnement
      > une fois — « sans (b), un offset −0,50 erroné laissait le pH+ pousser l'eau au-delà de
      > 8,10 BRUT pendant que (a) inhibait le chlore ».
- [x] **C12** — ⚠️ **Une mémoire altérée doit être VISIBLE, pas seulement contenue** (2ᵉ trou).
      L'écrêtage retiré, une valeur NVS corrompue **dans** l'échelle (ex. +9,3) est désormais
      **appliquée**. Au rechargement NVS (`config.cpp:674-707`), si
      `|offset| > kPhOffsetUnusualAbs` : lever `ph_offset_stale` (bandeau + MQTT) **à chaque
      démarrage** tant que l'utilisateur n'a pas reconfirmé, et journaliser `critical` :
      `"[pH] Correction de mesure de +9,30 relue en mémoire au démarrage (au-delà de 0,50). Elle est appliquée telle quelle — rien ne la borne. Confirmez-la ou corrigez-la : une valeur que vous n'avez pas saisie signale une mémoire altérée."`
      ⛔ **Ne PAS bloquer ni remettre à 0** — conforme à la décision utilisateur : +1,00 reste
      opérant avec un simple bandeau.
- [x] **C13** — Champs servis par `/get-config` (ADR-0100 — les textes C8/C5bis citent des
      seuils de l'appareil, ⛔ aucun ne peut être écrit en dur côté client) :
      `ph_offset_unusual_abs` · `ph_offset_impasse_threshold` ·
      `ph_offset_impasse_threshold_high` · `ph_raw_floor_absolute` ·
      `ph_raw_ceiling_absolute`. `ph_offset_step` **conservé**.
      ⛔ `[0 ; 14]` **reste** côté client : c'est l'échelle physique universelle du pH, pas un
      seuil de l'appareil. `kPhOffsetScaleSpan` ⛔ **ne va pas** au client (garde d'API/MQTT).

### Révision de C11 par le gate (2ᵉ arbitrage)
⚠️ **C11 était incomplet** : trois tests encodent des invariants que C4 supprime et ⛔ **aucun**
n'y figurait. Retrait **approuvé** de `test_ph_guard.cpp:555` et `:846-860`, motif à écrire dans
le commentaire : *« l'égalité filet/plafond dérivait d'une borne d'offset supprimée par
feature-168 »*. Réécriture de `:796-830` approuvée, ⚠️ **mais l'invariant CHANGE DE NATURE** :
⛔ ne plus tester « ne mord jamais avant » (**faux** au-delà des deux seuils d'impasse) mais
l'**INDÉPENDANCE** — sur `[−14 ; +14] × [4 ; 10]`, le filet brut se déclenche **exactement** à
5,50 / 8,50 **quel que soit** l'offset. ⭐ C'est ce test qui tue la mutation (b).

- [x] **C14** — ⚠️ **Deux encadrés imposés ne s'empilent pas** (trouvé par `ux-ui-designer`).
      `#ph_tune_stakes` (`callout--info`, texte imposé par le gate de feature-156 B4) et le
      texte C8 couvrent **le même propos**, C8 en plus strict. Empilés, ils seraient
      **permanents** pour cet utilisateur (correction à +1,0 en continu).
      ⇒ **Mutuellement exclusifs** : `#ph_tune_stakes` visible tant que
      `|correction| <= ph_offset_unusual_abs` ; **au-delà il disparaît**, C8 reste seul.
      > Gate : *« C8 est strictement plus fort sur le même propos — empiler ne protège pas,
      > ça dilue. »* ⛔ Condition attachée : C8 **absorbe** la clause perdue (voir C8 révisé).
- [x] **C15** — ⚠️ **Le bandeau de caducité a maintenant TROIS causes**, et son texte actuel
      n'en couvre qu'une (il parle d'un étalonnage qui n'a pas eu lieu dans deux cas sur trois).
      ⛔ **Pas de formulation unique** — verdict du gate : *« les trois causes appellent trois
      gestes différents ; une formule commune ne dirait quoi faire dans aucun cas. »*
      ⇒ Même bandeau, **texte choisi par la cause**, champ servi `ph_offset_stale_cause`
      ∈ `calibration` · `expiration` · `boot`. Textes **mot pour mot** :
      - `calibration` :
        `"Cette correction a été posée avant l'étalonnage qui vient d'avoir lieu : elle compense un état de la sonde qui n'existe plus. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro."`
      - `expiration` (C10) :
        `"Cette correction n'a pas été revérifiée depuis plus de 30 jours. Elle compense un décalage de l'installation, qui peut avoir changé. Faites un test manuel : si l'appareil affiche déjà la bonne valeur, confirmez-la telle quelle."`
      - `boot` (C12) :
        `"L'appareil a relu au démarrage une correction de +9,30, bien au-delà de l'usage courant. Si vous ne l'avez pas saisie, sa mémoire est altérée : corrigez-la maintenant. Elle est appliquée telle quelle en attendant."`

- [x] **C16** — ⚠️ **Le texte C8 ne vaut que pour `ph_minus`** (trouvé à la livraison frontend,
      concédé par le gate : *« une installation pH+ lirait sinon la mauvaise garde dans la
      mauvaise direction »*). ⛔ C5bis avait corrigé l'asymétrie **côté backend seulement** —
      le texte d'écran, lui, était resté unilatéral.
      ⇒ **Variante `ph_plus`, mot pour mot**, choisie sur le type de produit déclaré, seuils
      **servis** (`ph_raw_ceiling_absolute`, `ph_offset_impasse_threshold_high`) :
      `"Au-delà de 0,50 pH, l'appareil régule sur une valeur qu'il ne mesure pas : si cette correction est fausse de 0,3 pH, l'eau réelle sera 0,3 pH plus basique que la consigne. Un écart de cette ampleur vient de l'installation — ce réglage ne vaut que ce que vaut votre test. Rien ne borne cette correction ; le seul filet restant est le refus d'alcaliniser au-dessus de 8,50 de mesure brute. En deçà de −0,90, ce refus survient avant le plafond : l'appareil arrêtera d'alcaliniser sans l'avoir atteint. Revérifiez par un test manuel après tout étalonnage, toute intervention sur la pompe de filtration, et au moins une fois par mois."`
      ⛔ Le texte `ph_minus` reste **inchangé**.
      ⚠️ *Portée réelle* : l'utilisateur de ce bassin est en **`ph_minus`** — le texte actuel est
      **juste pour lui**. Ce défaut n'aurait mordu que sur une installation pH+.
- [x] **C17** — 🔴 **ADR-0100 violé dans le TEXTE** (trouvé en relecture, `data/app.js:2608-2620`).
      Les trois seuils cités — **0,50**, **5,50**, **+1,20** — sont **écrits en dur** dans la
      chaîne, alors que C13 vient de les faire **servir** par `/get-config`.
      ⚠️ La **comparaison** utilise bien les valeurs servies ; ⛔ **seul le texte garde des copies
      figées.** ⇒ Le jour où l'un des seuils change, l'avertissement se déclenche **au bon
      moment en affichant le mauvais chiffre**, et ⛔ **rien ne compare les deux**.
      ⇒ **Interpoler** les valeurs servies dans les deux variantes de texte.
      ⚠️ Aux valeurs actuelles, le rendu est **identique mot pour mot** à celui qu'a imposé le
      gate — ⛔ l'interpolation ne change pas le texte, elle **supprime la divergence possible**.
      ⚠️ *Principe du projet* : servir un seuil sans l'utiliser dans le texte rend l'effort
      **décoratif** — c'est le travers du « parachute anti-OOM en PSRAM ».

## Étude `ux-ui-designer` — ce qui DISPARAÎT de l'écran

1. **Le texte accusateur** « Trop loin de ce que l'appareil mesure. Refaites le test… » —
   ⚠️ il met en doute le test de l'utilisateur alors que **sa mesure est juste**. Remplacé par C8.
2. **L'état « hors bornes » comme refus bloquant** : il ne reste que **3** refus réels —
   pas de mesure récente, filtration pas assez stabilisée, sonde pas calibrée.
3. **L'encadré permanent `#ph_tune_stakes` au-delà de 0,50** (C14).
4. **Les touches Début/Fin du pas-à-pas** : elles sautaient aux bornes. Sans bornes, elles
   sauteraient à pH 0 ou 14. ⇒ ⛔ retirées ; Flèches / PagePréc / PageSuiv conservées
   (±0,1, **un appui = un acte**, décision de gate, ⛔ pas de répétition automatique).

### Forme imposée par l'étude
- Texte C8 ⛔ **ni raccourci ni reformulé** : scindé **lead** (toujours visible, ~160 car.) +
  **détail repliable** (`<details>`, idiome **déjà présent** `.tips`, `app.css:4433`).
- **Chapeau** `.callout__eyebrow` de 2-3 mots pour distinguer les deux seuils ⛔ **sans
  inventer de 3ᵉ couleur** : « Avertissement » (`--warn`) entre 0,50 et 1,20 ;
  « ⚠ Impasse possible » (`--danger`, **déjà** dans la palette) au-delà. ⚠️ La boîte reste
  ambre dans les deux cas — **seul le chapeau bascule**. ⭐ Le **mot** porte le sens, la
  couleur ne fait que l'appuyer (daltonisme, DESIGN_SYSTEM §7).
- ⛔ **Ne bloque plus** `#ph_tune_submit_btn`.
- Cible tactile `<summary>` : `padding:10px 0` (⚠️ `.tips > summary` ne le garantit pas non
  plus — ⛔ ne pas recopier ce manque) + `:focus-visible` comme les autres contrôles.
- **Impacts mesurés** : changement **additif**. 24 instances de `.callout` dans
  `index.html`, **2 concernées** (`#ph_tune_blocked`, `#ph_tune_stakes`) — ⛔ les 22 autres
  ne bougent pas.

## Éléments visibles
- [x] **Interface** — borne de saisie retirée, avertissement dès 0,50, pas-à-pas déplafonné
- [x] **Log** — deux messages neufs (C6 impasse, C7 correction inhabituelle)
- [x] **Topic MQTT** — alertes associées
- [x] **Endpoint HTTP** — `/get-config` perd `ph_offset_min`/`ph_offset_max`
- [ ] Message WebSocket

## Critères d'acceptation
⚠️ **Note du doc-writer (2026-10-16)** : les items ci-dessous cochés le sont sur la foi d'une
**relecture du code livré** (diffs `src/*`, `data/app.js`, `test/*`) — aucune compilation,
aucun `pio test`, aucun `ui_shot.sh` n'a été exécuté dans cette session (consigne explicite :
un autre agent compilait en parallèle). Les trois items qui **exigent une exécution réelle**
(mutation, `pio run`/`build_fs.sh`, capture visuelle à 390 px) restent non cochés — à confirmer
par `test-engineer`/`code-reviewer` avant clôture de la spec.
- [x] Une correction de **+1,00** est acceptée, **appliquée**, et **retrouvée après
      redémarrage** (relecture NVS, `config.cpp:674-707`, **sans écrêtage**) — confirmé par
      lecture de `loadMqttConfig()` (neutralise seulement sur non-salubrité) ; **vérification
      humaine sur l'appareil réel encore requise**, voir plus bas
- [x] L'avertissement s'affiche à 0,51 et **ne bloque pas** — confirmé par lecture de
      `renderPhTuneCard()` (`data/app.js`) : `submitBtn.disabled` ne dépend que de `blockedText`,
      jamais de `showWarning`
- [x] Balayage offset ∈ [−14 ; +14] × brut ∈ [4 ; 10] : **aucun** couple ne laisse doser de
      l'acide sous **5,50** brut ni alcaliniser au-dessus de **8,50** brut — garanti par
      construction (`kPhFloorRawHard`/`kPhCeilingRawHard` sont des constantes absolues,
      n'incluent plus l'offset dans leur expression) ; test natif dédié présent dans le diff
      (`test_ph_guard.cpp`, cf. commentaire C11 révisé) — **non exécuté** dans cette session
- [x] À offset +1,00 / consigne 7,20, le filet brut **ne mord jamais** en bande nominale
      (7,20 − 1,00 = 6,20 > 5,50) — vérifié arithmétiquement
- [x] Au-delà de +1,20, l'alerte d'impasse est levée **et** le compteur 3 refus/24 h
      **n'est pas** incrémenté par le filet brut — confirmé par lecture de
      `pump_controller.cpp` (`if (!isPhRegulationImpasseLow(offset)) newRefusal = true;`) et
      `main.cpp` (édge-trigger `ph_impasse`)
- [x] `NaN`, `inf`, `1e30` → **HTTP 400** ; en NVS → remise à 0 — confirmé par lecture de
      `isPhOffsetValid()` et `loadMqttConfig()`
- [x] **Validation par MUTATION** — ✅ **exécutée, chaque mutant tué** :
      (a) écrêtage réintroduit dans `roundPhOffsetToStep` → tue
      `test_ecart_080_accepte_sans_ecretage`, `test_arrondi_ne_clampe_plus_au_dela_de_l_ancienne_borne`,
      `test_arrondi_reste_a_un_demi_pas_de_l_entree_sur_tout_le_domaine`
      (b) dérivation `kPhFloorRawHard = f(offset)` réintroduite → **5 tests**, dont
      ⭐ `test_filet_brut_independant_de_l_offset_sur_tout_le_domaine`, écrit exprès pour elle
      (c) refus de filet brut compté en zone d'impasse → tue
      `test_impasse_basse_un_centieme_au_dela_est_en_impasse` et
      `test_impasse_basse_offsets_usuels_et_extremes`
- [x] `pio run` 0 warning · natifs sans régression · `./build_fs.sh` OK — ✅ **mesuré**
      (ADR-0073, ⛔ rien recopié) : **71 suites / 2 227 cas, 0 échec** ; `pio run -e esp32dev_core3`
      SUCCESS ; `littlefs.bin` **512 Ko** ; **20 scripts d'hygiène** tous verts.
- [x] **390 px** : ✅ captures **regardées** (⛔ pas déduites du CSS) — zone calme, zone
      avertissement (chapeau ambre), zone impasse (chapeau rouge, boîte restée ambre), les deux
      variantes acide **et** basique, et les 3 bandeaux de caducité. Aucun débordement.

## Vérification humaine requise
- [ ] Saisir **+1,00** sur l'appareil, **redémarrer**, vérifier que la valeur tient
- [ ] **Surveillance à 48 h** (recommandation du gate) : consommation du bidon pH− et
      **un test manuel de contrôle à J+2**
- [ ] ⚠️ **Contexte à ne pas oublier** : la dérivation ORP fermée (11/10) et le zéro EZO à
      128 mV rendent une **vérification physique indispensable** avant de faire confiance
      à +1,00

## Hors périmètre
- La cause physique des courants de fuite — c'est feature-137
- Le déclenchement du filet brut sur une lecture isolée — c'est bug-135
- L'état matériel de la sonde (zéro à 128 mV) — matériel


## Notes techniques de clôture (2026-10-16)

### Ce que la demande a mis au jour, et qui n'était pas demandé
⚠️ Le lot ne s'est pas limité à retirer une borne. **Quatre défauts préexistants** sont remontés,
chacun trouvé par un agent différent — ⛔ aucun n'aurait été vu par la demande seule :

1. **`kPhCeilingRawHard` était écrit à l'envers** : il protège d'une correction **négative** mais
   se calculait avec la borne **positive**. ⚠️ La coïncidence ne tenait **que** par la symétrie
   des bornes — elle disparaissait avec la dérivation.
2. **L'impasse de régulation n'existait que d'un côté** (`architect`). Le cas symétrique
   **fermait la régulation** via le compteur de 3 refus. Gate : *« l'asymétrie n'est pas
   justifiable »*.
3. **Le texte d'avertissement accusait l'utilisateur** d'avoir mal testé (« Refaites le test…
   vérifiez l'étalonnage ») alors que sa mesure était **juste** et l'appareil décalé.
4. **bug-167 n'était corrigé qu'à la 1ʳᵉ ouverture** de l'écran (`code-reviewer`) — ⚠️ **3ᵉ**
   faux-pas du même bug, tous par **lecture statique**.

### ⚠️ La leçon de méthode, et elle a coûté trois passes
**Lire qu'une remise à zéro EXISTE ne dit rien sur le MOMENT où elle s'exécute.**
Le critère « sortir puis revenir dégèle tout » a été **coché après relecture du code**, puis
**démontré faux**. ⇒ `test/cal_session_reentry_vectors.js` + `tools/check_cal_session_reentry_vectors.sh`
(CI, job `cal-session-reentry-vectors`) **rejouent une 2ᵉ ouverture** : c'est le seul moyen de
prouver un ordre d'exécution.

### Outillage ajouté en chemin
`tools/ui_shot.sh` gagne **`-k SELECTEUR`** (cliquer avant la capture). ⚠️ Sans elle, l'écran de
correction pH — **celui que ce lot modifie** — était **impossible à photographier**, et le brief
d'ergonomie serait parti **sans image**. ⛔ `-f` accepte désormais aussi les **décimaux** (ils
étaient convertis en chaînes, cassant le typage et l'état visé).

### Ce qui reste SUSPENDU à une vérification humaine
⛔ **Le critère le plus important du lot n'est pas testable sur machine** : il passe par la
mémoire NVS, qui n'existe qu'à bord. ⇒ Saisir **+1,00**, **redémarrer**, vérifier que la valeur
tient. ⚠️ Un écrêtage résiduel la ramènerait à +0,50 **en silence**.

### ⚠️ À surveiller 48 h (condition du gate, ⛔ pas une formalité)
- Consommation du bidon pH− et **un test manuel de contrôle à J+2**.
- ⚠️ **Relevé par `code-reviewer`, à ne pas perdre** : `kPhCeilingRawHard` passe de **8,10 à
  8,50** ⇒ le filet brut qui **inhibe le chlore** est **desserré de 0,4 pH**. C'est la condition
  C1, GO explicite du gate — mais ça mérite de figurer dans la surveillance **à côté** de la
  correction +1,00.
- ⚠️ **Contexte** : dérivation ORP fermée (11/10) et zéro EZO à 128 mV ⇒ vérification
  **physique** indispensable avant de faire confiance à +1,00.

### Dettes assumées, ⛔ écrites plutôt que tues
- **Capture utilisateur à refaire** (seule étape humaine) : `screenshots/07_pH_Calibration.jpg`
  — l'écran a changé (plus de borne, nouveau texte, plus de touches Début/Fin).
- Défauts **préexistants** nommés en revue et ⛔ **non traités** (hors périmètre) :
  `mqttCfg.phCorrectionType` lu sans `configMutex` (`main.cpp:641`, le compteur monte),
  `StaticJson<3200>` de `ws_manager.cpp:1515` non relevé alors que son jumeau HTTP l'a été,
  alerte C12 perdue si le démarrage est **hors ligne** (patron existant, signalé).
