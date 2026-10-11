## Métadonnées
- **ID** : feature-154
- **Slug** : pastilles-de-sante-par-critere-sonde-ph
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.70.0
- **Dernière mise à jour** : 2026-10-11
- **Spec(s) liée(s)** : feature-024 (chip d'état sonde pH), feature-153 (écrans d'état des
  sondes dans leur forme actuelle)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **OUI.** Aucune ligne de dosage n'est touchée, mais cette spec expose à
      l'utilisateur les **critères de santé de la sonde pH**, qui sont exactement ceux
      qu'`isPhProbeHealthy()` utilise pour **formuler le diagnostic de la bascule de sécurité**
      (`pump_controller.cpp:1160`). Un seuil affiché qui diverge du seuil qui décide est
      précisément le défaut que cette spec corrige — le gate doit confirmer les seuils retenus
      et le vocabulaire employé.
- [x] ux-ui-designer — **OUI, mandat COMPLET** (cf. CLAUDE.md, règle du 2026-10-11) : couleurs,
      formes, contraste, daltonisme, cibles tactiles, responsive **et tout autre choix visuel
      que ce lot impose, même non demandé ici**.
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Afficher, sur l'écran « État sonde pH », une **pastille verte ou rouge par critère** —
**Pente acide**, **Pente base**, **Décalage zéro** — pour qu'un coup d'œil dise *lequel* des
trois ne va pas, au lieu d'un verdict global qui les amalgame.

## Contexte

**Demande utilisateur du 2026-10-11** : « *Sur l'écran sonde pH, mettre une pastille verte sur
ce qui est ok, rouge sur ce qui n'est pas ok, pour les champs : Pente acide, Pente base,
Décalage zéro.* »

### Ce que la demande corrige vraiment — DEUX DÉFAUTS RÉELS, vérifiés

**1. Le verdict global ment par amalgame.** Le 2026-10-11 au matin, l'utilisateur a signalé :
« *la sonde est indiquée comme "sonde à remplacer . 100%"* ». Ses valeurs réelles : pente acide
**100,2 %**, pente base **100,0 %**, zéro **128,16 mV**. Les deux pentes sont **parfaites** — la
membrane de verre est intacte. C'est la **jonction de référence** qui est en cause. Mais
`classifyPhProbe()` (`data/app.js:6374`) rend un verdict UNIQUE et accole « Sonde à remplacer »
à un chiffre (« 100 % ») qui, lui, est irréprochable. Trois pastilles séparées diraient
immédiatement **vert · vert · rouge**, ce qui est le diagnostic exact.

**2. ⚠️ IL EXISTE DEUX JUGEMENTS DE SANTÉ DIFFÉRENTS DANS LA MÊME APPLICATION.**

| | Firmware — `isPhProbeHealthy()` (`src/ph_guard_logic.h:138`) | Interface — `classifyPhProbe()` (`data/app.js:6374`) |
|---|---|---|
| Pente basse | `< 90 %` ⇒ non sain | `< 95 %` ⇒ orange, `< 90 %` ⇒ orange foncé, `< 85 %` ⇒ rouge |
| **Pente haute** | `> 110 %` ⇒ **non sain** | 🔴 **JAMAIS TESTÉE** |
| Zéro | `|zéro| > 15 mV` ⇒ non sain | `> 15` ⇒ orange, `> 30` ⇒ rouge |

Conséquences : une pente de **115 %** est **anormale pour le firmware** et **« good » pour
l'interface** — l'utilisateur verrait du vert sur une sonde que l'appareil juge hors norme. Et
une pente de 92 % est **saine** pour le firmware mais **orange** à l'écran.

Les seuils de l'interface sont **écrits en dur** dans `data/app.js`, sans source. Ceux du
firmware vivent dans `constants.h` (`kPhSlopeHealthyMinPct`, `kPhSlopeHealthyMaxPct`,
`kPhZeroHealthyMaxMv`) et ne sont **servis à personne**.

### Décisions déjà prises par l'utilisateur (2026-10-11)

- **Les seuils affichés sont CEUX DU FIRMWARE** : 90-110 % pour les deux pentes, 15 mV pour le
  zéro. Une seule source de vérité, et la **borne haute enfin testée**.
- **Deux états seulement : vert / rouge.** Pas de niveau orange intermédiaire (proposé, écarté).
- **L'écran de la sonde ORP n'est PAS touché.** Motif établi en analyse : le module EZO ORP
  se calibre en **un seul point** et n'expose **ni pente, ni zéro** ; le firmware ne calcule
  **aucun** critère de santé ORP. Il n'y a donc rien à équiper, et inventer des critères serait
  un sujet de chimie, pas d'affichage. Asymétrie **assumée et explicable** : le pH a trois
  critères parce que son module en fournit trois.

## Comportement attendu

Sur l'écran « État sonde pH » (`#ph-probe-modal`), les trois lignes **Pente acide**,
**Pente base** et **Décalage zéro** portent chacune une **pastille** :

- **verte** quand le critère est dans la plage saine du firmware ;
- **rouge** quand il en sort — **des deux côtés** pour les pentes (sous 90 % **comme** au-dessus
  de 110 %).

### Cas limites

- **Valeur indisponible** (`null`, `NaN`, pente jamais lue, EZO injoignable) : **AUCUNE
  pastille**, ou une pastille neutre explicitement distincte. ⚠️ **Jamais de vert par défaut** :
  une absence n'est pas un succès. Jamais de rouge non plus — ce serait annoncer une panne de
  sonde là où il n'y a qu'une lecture manquante.
- **Sonde non calibrée** (`calPoints = 0`) : les pentes n'ont pas de sens. Dire l'état de
  calibration, ne pas afficher trois pastilles sur des valeurs vides.
- **Lecture périmée** (`phSlopeAgeMs` ancien) : les pastilles décrivent la **dernière lecture
  connue**. La ligne « Vérifié » porte déjà l'âge — ne pas le dupliquer, mais ne pas laisser
  croire que le verdict est frais s'il ne l'est pas.
- **Valeur exactement au seuil** (pente = 90,0 ou 110,0 ; zéro = 15,0) : le firmware utilise
  `<` et `>` stricts, donc **le seuil lui-même est SAIN**. Le reproduire à l'identique, et le
  **verrouiller par un test**.

## Comportement actuel

Les trois lignes affichent une valeur nue (« 100,2 % », « 128,16 mV »), sans jugement. Le seul
jugement visible est la **pastille globale** du chip (`classifyPhProbe`), avec ses propres
seuils divergents et sa borne haute absente.

## Contraintes techniques

- ⚠️ **SOURCE UNIQUE DES SEUILS.** Les trois valeurs de `constants.h` doivent être **servies au
  navigateur**, jamais recopiées dans `data/app.js`. C'est tout l'objet du défaut n° 2 : le
  jour où un seuil bouge dans `constants.h`, l'écran doit bouger avec lui. Un seuil réécrit
  côté client est un troisième jugement en puissance.
- ⚠️ **RESPONSIVE 390 px** : la pastille partage sa ligne avec un libellé et une valeur.
  **Compter les caractères** de « Décalage zéro » + « 128,16 mV » + pastille et dire si ça
  tient. Un `flex-wrap` est le cas NORMAL en mobile, pas un repli.
- ⚠️ **Accessibilité** : la couleur seule ne suffit pas. Forme, symbole ou libellé doivent
  doubler l'information (daltonisme rouge-vert — c'est précisément la paire employée ici).
  Prévoir aussi ce que lit un lecteur d'écran : une pastille muette n'est pas une information.
- Vanilla JS, aucune bibliothèque nouvelle, français partout.
- Budget FS `spiffs` 512 Ko — vérifier avec `./build_fs.sh`.

## Fichiers potentiellement concernés

- `src/web_routes_config.cpp` ou `src/web_routes_data.cpp` — exposer les trois seuils.
  L'`architect` tranche **où** : une valeur immuable de configuration n'a pas sa place dans une
  charge servie toutes les 5 s.
- `data/index.html` — les trois lignes du modal `#ph-probe-modal`.
- `data/app.js` — rendu des pastilles ; **retrait des seuils codés en dur** de
  `classifyPhProbe()` au profit des seuils servis.
- `data/app.css` — style de la pastille.
- `docs/features/page-ph.md`, `docs/API.md`, `CHANGELOG.md`.

## ✅ VERDICTS DU CYCLE — 2026-10-11

### `pool-chemistry` : GO SOUS CONDITIONS (6 conditions, toutes bloquantes)

- **C1** — Zéro `null`/`NaN` ⇒ **« indisponible »**, jamais 0. ⚠️ Le gate a trouvé **plus grave
  que prévu** : `atlas_ezo.cpp:271` laisse `zeroOffsetMv` à `NaN` quand le firmware EZO ne
  rapporte **que deux valeurs** (modules anciens, 3ᵉ champ absent). Le faux vert n'est donc pas
  seulement transitoire (sonde injoignable) : il est **PERMANENT** sur un tel module. Un
  appareil afficherait un diagnostic rassurant sur un critère **jamais mesuré**.
- **C2** — Le **chip global ne peut être vert que si les TROIS critères sont mesurés** et dans
  la plage. Un seul « indisponible » interdit le vert.
- **C3 — VOCABULAIRE, bloquant.** « Sonde à remplacer » **disparaît**. Il a accusé une sonde
  saine le 2026-08-25 (pentes 101/100, zéro −1,03 — cause réelle : décalage **externe**) et
  récidive le 2026-10-11 (100,2/100,0, zéro 128,16 — **jonction de référence**, membrane
  intacte). Libellés retenus :
  · pente rouge → « Pente hors plage 90-110 % — verre vieilli ou calibration à refaire » ;
  · zéro rouge → « Décalage de zéro hors ±15 mV — jonction de référence ou décalage EXTERNE
    (équipotentialité, courants de fuite pompe de filtration) ». Action suggérée : **trempage
    KCl / mise à la terre, JAMAIS « remplacer »**.
- **C4** — Divergence d'arrondi : **à fermer, pas à tolérer**. ⇒ Voir la levée ci-dessous.
- **C5** — Commentaire `constants.h:1775-1785` réécrit : « Fenêtre de diagnostic EZO.
  (1) nuance le message d'escalade ; (2) depuis feature-154, **affichée critère par critère à
  l'utilisateur**. **Aucune décision de dosage n'en dépend — ne jamais en faire une garde.**
  Toute modification change un texte visible : doc + CHANGELOG + passage `pool-chemistry`. »
- **C6** — Écrire **noir sur blanc** (code **et** `docs/features/page-ph.md`) qu'**aucune
  pastille ne conditionne un dosage**.

**Nuance du gate à ne pas perdre** : un zéro à 20 mV reste **exploitable après recalibration**.
La pastille rouge dit « **hors fenêtre de diagnostic** », pas « sonde morte ».

**Réserve non bloquante** : trois pastilles vertes sur le pH ne doivent **pas** suggérer
« sondes vérifiées ». L'ORP du 2026-10-11 était faux (dérivation fermée) sans qu'aucun critère
ne bronche. Aucun diagnostic interne ne voit ça.

### ⚖️ LEVÉE DU POINT DE FRICTION C4 — chimie vs architecture

`pool-chemistry` exige de **fermer** la divergence d'arrondi ; l'`architect` démontre que la
fermeture la plus propre (servir un verdict booléen calculé par le firmware) **exigerait
d'ajouter un champ à `/data` ou à la trame WS**, ce que cette spec **interdit explicitement**.

**Pas de conflit réel** : `pool-chemistry` prévoit lui-même le repli — « *à défaut, test
verrouillant l'égalité sur la valeur arrondie* ». C'est exactement ce que propose l'`architect`.
⇒ **C4 est satisfaite par** : (i) un vecteur de test aux bornes **sur la valeur servie**
(89,9 / 90,0 / 110,0 / 110,1) ; (ii) une note dans `docs/API.md` disant que la pastille juge la
valeur **arrondie servie**, pas la valeur interne.

**Ce que l'`architect` a vérifié, et la portée de son contrôle** : `querySlope()`
(`atlas_ezo.cpp:239-294`) parse par `strtof`, **sans arrondi** ; l'arrondi est fait **à
l'émission** (`web_routes_data.cpp:345-347`) ; le format documenté est **1 décimale pour les
pentes, 2 pour le zéro**, conforme aux valeurs réelles relevées. ⚠️ **N'ont PAS été vérifiés** :
la fiche technique Atlas, ni une trame brute à d'autres ordres de grandeur. La divergence
théorique est bornée à **< 0,05 %** (pentes) et **< 0,005 mV** (zéro).

### `architect` — arbitrages tranchés

1. **Route : `GET /get-config`**, dans le bloc v2.25.0, entre les marqueurs `PARITE-CONFIG`
   (sinon la sentinelle devient **aveugle** au lieu d'échouer). Clés :
   `ph_slope_healthy_min_pct`, `ph_slope_healthy_max_pct`, `ph_zero_healthy_max_mv`. Motif : ce
   sont des `constexpr` ; `kWsFrameBufBytes = 3584 o` n'a que ~571 o de marge au pire cas, et
   ce pire cas **coïncide avec l'escalade de sécurité pH**.
2. **Aucun repli**, et la garantie est **DANS la fonction pure** : `classifyPhCriterion` rend
   `'indisponible'` **avant tout autre test** si les seuils ne sont pas finis ou si `min > max`.
   Le vert est **structurellement inatteignable** sans seuils reçus. `PH_BOUNDS_FALLBACK` n'est
   **pas** réutilisé : un repli sur une borne d'IHM dégrade l'ergonomie, un repli sur un seuil
   de santé **innocente une sonde**.
4. **`classifyPhCriterion(value, minInclusive, maxInclusive)`** → `'sain' | 'hors' |
   'indisponible'` (**chaînes**, pas un booléen : l'indisponible est un **troisième état
   irréductible**). Les pentes s'évaluent sur `(slopeMin, slopeMax)`, le zéro sur
   `(-zeroMax, +zeroMax)` ⇒ **une seule fonction**, et le zéro négatif devient un cas naturel.
   ⚠️ **Ne PAS appliquer `Math.abs` aux pentes** — ce sont déjà des pourcentages positifs.
5. **Scanner d'extraction factorisé** dans `test/js_extract.js` (option b) : les deux
   implémentations existantes sont **identiques au commentaire près**. ⇒ **ADR-0099 amendé**
   (sa propre alternative « direction naturelle si un troisième cas apparaît » est retenue) —
   **amendement, pas supersession**.
- **ADR-0100 à créer** : « un seuil de santé est servi, jamais réécrit côté client ».
  Contrainte **mécanique** : le harnais assertera que le texte extrait ne contient **aucun
  littéral numérique hors 0 et 1** — c'est ce qui rend opposable le critère « vérifié par grep ».

### `ux-ui-designer` — design ARRÊTÉ

**Quatre états, forme ET couleur** (jamais la couleur seule) :

| État | Rendu | Lu par un lecteur d'écran |
|---|---|---|
| Sain | cercle **plein** vert, glyphe `✓` | « sain » |
| Hors norme | cercle **plein** rouge, glyphe `✕` | « hors norme » |
| Indisponible | cercle **CREUX**, contour pointillé gris, `–` | « indisponible » |
| Non calibré | **aucune pastille** (la valeur affiche déjà `--`) | rien |

**Daltonisme** : même sans distinguer rouge et vert, on ne confond pas un cercle **plein** et un
cercle **creux**, ni un `✓` d'un `✕`. Le glyphe porte `aria-hidden`, un `.sr-only` porte le sens
(classe déjà présente, `app.css:3424`). Pastille **non interactive** ⇒ pas de contrainte 44 px.

**Réorganisation de l'écran entier** (règle d'accumulation, 3ᵉ ajout) : les 7 lignes à plat
deviennent **2 groupes nommés** — « **Étalonnage** » (pentes, zéro) et « **Mesure en direct** »
(brute, retenue, âge). La ligne « **Vérifié** » cesse d'être une ligne pleine largeur : elle
faisait **doublon** avec « Dernière mesure », qui est un **autre âge**. Elle devient une
**légende** sous le groupe. ⇒ **6 lignes au lieu de 7**, plus 2 titres : plus clair, pas plus
haut.

**Responsive 390 px, compté** : modale plafonnée à 320 px, contenu ≈ 284 px ; pire cas
(« Décalage zéro » + « -128,16 mV » + pastille) ≈ **207 px** ⇒ tient, marge ~77 px. Le
`flex-wrap` reste posé **par prudence**, et ne doit jamais se déclencher en usage réel.

**Ne pas animer la pastille rouge** (contrairement au chip fermé qui pulse) : une ligne de
détail déjà ouverte par choix n'a pas besoin d'attirer l'œil davantage.

### 👤 DÉCISION UTILISATEUR — libellé du chip global

Trois formulations lui ont été soumises. **Retenu : « Calibré · critère hors norme »** (état
sain : « **Calibré · sain** »). Motif : n'accuse **aucune pièce** et renvoie à l'écran de détail
pour savoir lequel des trois critères est en cause.
⚠️ Les variantes proposées par l'`architect` (« Sonde hors norme · zéro 128 mV ») et par
`pool-chemistry` (« Diagnostic EZO hors plage · zéro ») sont **écartées** : elles conservent un
jugement sur la pièce ou allongent le libellé. **Ne pas les réintroduire.**
Le dégradé à 4 niveaux (`good/warn/warn2/bad`) **disparaît pour la santé pH** : le chip devient
**sain / hors norme / indisponible**, symétrique aux trois pastilles qu'il résume. ⚠️ Les classes
CSS `warn`/`warn2` **restent définies** — elles servent au chip ORP et à `chip--cal`.

## Livrables — check-list

- [x] A1 — Les trois seuils (`kPhSlopeHealthyMinPct`, `kPhSlopeHealthyMaxPct`,
      `kPhZeroHealthyMaxMv`) sont **servis au navigateur**, depuis `constants.h`, sans
      littéral dupliqué.
- [x] B1 — Pastille verte / rouge sur **Pente acide**, **Pente base**, **Décalage zéro**.
- [x] B2 — **Borne HAUTE des pentes testée** (> 110 % ⇒ rouge) — elle ne l'était pas.
- [x] B3 — Valeur indisponible ⇒ **pas de vert par défaut**, et pas de rouge non plus.
- [x] B4 — `classifyPhProbe()` **cesse de porter ses propres seuils** : plus qu'un seul
      jugement de santé dans l'application. ⚠️ Si le verdict global change de libellé pour
      l'utilisateur, c'est un changement **visible** — le dire au `doc-writer` et au CHANGELOG.
- [x] B5 — Information **non portée par la seule couleur** (forme/symbole/libellé + lecteur
      d'écran).
- [x] B6 — Responsive **vérifié à 390 px**, caractères comptés.
- [x] C1 — `docs/features/page-ph.md`, `docs/API.md`, `CHANGELOG.md`.
- [x] D1 — Tests sur la **logique pure de classement** (valeur, seuils) → {vert, rouge,
      indisponible}, via le harnais d'extraction (**ADR-0099**) : bornes exactes 90,0 / 110,0 /
      15,0 **incluses comme SAINES**, valeurs `null`/`NaN`, zéro **négatif** (c'est une valeur
      absolue qui est comparée).
- [x] D2 — **Mutations** : (i) inverser une borne ; (ii) retirer le test de la borne haute ;
      (iii) rendre `null` vert. Chacune doit tuer au moins un test.

## Sécurité chimique

- Dosage impliqué : **non** — aucune ligne de `pump_controller` n'est modifiée.
- ⚠️ **Mais les seuils exposés sont ceux qui qualifient le diagnostic de la bascule de sécurité
  pH** (`isPhProbeHealthy`, cité dans le message d'escalade). Le gate doit confirmer : (a) que
  90-110 % / 15 mV sont les bons critères à montrer à l'utilisateur ; (b) que le vocabulaire
  n'induit pas en erreur — rappel de l'incident du 2026-08-25, où « sonde à recalibrer ou
  remplacer » avait désigné une sonde saine alors que la cause était un décalage **externe**.
- Risque identifié : **un faux vert**. Une pastille verte sur un critère non mesuré, ou sur une
  borne non testée, est pire que pas de pastille — elle **innocente** la sonde.

## ADR attendu ?

- **Probable** : non (estimation initiale) → **réalisé en cours de cycle : OUI**.
- **Sujet confirmé par l'`architect`** : « les seuils de santé sont servis, jamais réécrits
  côté client » verrouille un choix futur (critère 1 — contrainte sur d'autres parties du
  système). La divergence corrigée ici s'était installée en silence pendant des mois ; rien
  n'empêchait qu'elle revienne sur un futur critère.
- **[ADR-0100](../../../docs/adr/0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)
  créé** — contrainte mécanique : le harnais `test/ph_health_vectors.js` asserte que le texte
  extrait de `classifyPhCriterion` ne contient aucun littéral numérique hors `0` et `1`.
- **[ADR-0099](../../../docs/adr/0099-tester-par-extraction-de-texte-jamais-par-duplication.md)
  amendé** (section « Mise à jour 2026-10-11 »), pas supersédé : son alternative « généraliser
  le patron » est retenue — le scanner d'extraction est factorisé dans `test/js_extract.js`,
  requis par les trois harnais.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Trois seuils ajoutés à une route existante (laquelle : décision `architect`).

### Documentation à produire / mettre à jour
- `docs/features/page-ph.md` — les trois pastilles et leurs seuils.
- `docs/API.md` — les champs de seuils ajoutés.
- `CHANGELOG.md` — entrée 2.70.0, **dont le fait que le verdict global change de critères**
  (la borne haute est désormais testée).

## Critères d'acceptation

- [x] Avec pente 100,2 / 100,0 et zéro 128,16 (valeurs réelles de l'utilisateur au 2026-10-11),
      l'écran affiche **vert · vert · rouge**.
- [x] Une pente de **115 %** affiche **rouge** (elle serait « good » aujourd'hui).
- [x] Une pente de **90,0 %** exactement affiche **vert** (seuil strict, inclus).
- [x] Une valeur indisponible n'affiche **jamais** de pastille verte.
- [x] Aucun seuil de santé n'est écrit en dur dans `data/app.js` (vérifié par grep).
- [x] Lisible sans distinguer le rouge du vert (daltonisme).
- [x] Vérifié **à 390 px**.
- [x] `pio run` sans nouveau warning · `./build_fs.sh` dans les 512 Ko · `pio test -e native`
      zéro régression.

## Plan de test manuel
1. Ouvrir l'écran sonde pH sur l'appareil : vérifier **vert · vert · rouge** (le zéro réel est
   à 128 mV, les pentes à 100 %).
2. Après un trempage KCl réussi (zéro redescendu sous 15 mV) : la troisième pastille doit
   passer au vert **sans intervention**.
3. Sonde débranchée : aucune pastille verte.
4. Téléphone, 390 px.

## Hors périmètre
- **L'écran de la sonde ORP** — décision utilisateur : aucun critère de santé ORP n'existe
  (calibration 1 point, ni pente ni zéro). Ne pas en inventer.
- **Changer les valeurs des seuils eux-mêmes** : on expose ceux qui existent, on ne les
  renégocie pas.
- **Le niveau orange intermédiaire** : proposé à l'utilisateur, **écarté** (vert/rouge seuls).
