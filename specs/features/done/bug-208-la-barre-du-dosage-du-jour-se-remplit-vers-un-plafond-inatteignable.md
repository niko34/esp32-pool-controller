## Métadonnées
- **ID** : bug-208
- **Slug** : la-barre-du-dosage-du-jour-se-remplit-vers-un-plafond-inatteignable
- **Type** : bug (affichage — ⛔ aucune garde de dosage en cause)
- **Statut** : **done** — code livré en v3.1.0 (2026-11-22), tests verts (2 742 cas natifs + 30 vecteurs JS), revue de code passée et **ses 9 constats corrigés**, deux visas `pool-chemistry` obtenus et transcrits, captures 390 px faites. Reste l'étape humaine : la capture `screenshots/08_ORP.jpg`.
- **Priorité** : normale
- **Version cible** : v3.1.0 (ajout d'un accesseur firmware ⇒ mineur, pas correctif) — **livrée**
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [bug-201](../done/bug-201-l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation.md)
  (trouvé en instruisant son étude d'ergonomie) · ADR-0053 / feature-086 (« limité par l'actionneur »)

## Domaines touchés
- [x] **Frontend (`data/`)** — carte héros des pages pH et ORP
- [ ] Régulation · [ ] Capteurs · [ ] MQTT · [ ] OTA · [ ] Authentification
- [ ] Web — routes HTTP / WebSocket *(les deux valeurs sont déjà servies)*

## Agents requis
- [x] spec-reader · [x] ux-ui-designer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] pool-chemistry — ⚠️ **à cocher si** la correction change ce que l'utilisateur croit être sa
      limite : c'est un chiffre qui oriente ses réglages de dosage. En cas de doute, le cocher.

## Description
Sur la page pH, la barre « **DOSAGE DU JOUR** » se remplit vers **2 000 mL**, alors que la ligne
juste en dessous annonce « **Maximum journalier : 1 768 mL/j** ». ⇒ La barre ne pourra **jamais**
se remplir : elle vise un plafond que l'appareil n'atteindra pas.

## Ce que j'ai vérifié sur l'appareil (2026-11-22, v2.99.0)
⛔ **Ce ne sont PAS deux affichages de la même chose** — l'hypothèse « doublon » est fausse :

| Clé servie | Valeur | Ce que c'est |
|---|---|---|
| `max_ph_ml_per_day` (`/get-config`) | **2000** | le plafond que l'utilisateur a **réglé** |
| `ph_daily_max_ml` (`/data`) | **1768** | ce que la pompe peut **réellement délivrer** en un jour |

⭐ La seconde est **dérivée du matériel** (débit de la pompe × `ph_limit_minutes` par heure) —
c'est l'acquis d'ADR-0053 / feature-086, *« limité par l'actionneur »*. ⛔ Les deux chiffres sont
donc **utiles tous les deux**, et en retirer un ferait disparaître un fait.

🔴 **Le défaut est que la BARRE prend le mauvais dénominateur** : elle se remplit vers le plafond
**réglé** (2000) alors que la limite **opposable** est l'autre (1768). ⇒ À 1 768 mL, le dosage
s'arrête avec une barre à **88 %**, et rien à l'écran n'explique pourquoi.

⚠️ **Et c'est l'inverse qui serait dangereux** : si un jour le réglage descendait **sous** la
capacité de la pompe, la barre se remplirait trop vite. ⇒ Le dénominateur correct est
**le minimum des deux**, pas l'un des deux.


## ⛔ VÉRIFICATION DE LA FICHE AVANT TRAITEMENT — 2026-11-22, v3.0.3

La fiche datait de la **v2.99.0**. Chaque affirmation a été reprise contre le code et contre
l'appareil. **Une est fausse, et elle changeait le travail.**

### Confirmé

- `updateDailyDoseDisplay()` (`data/app.js:7310`) prend bien `max_ph_ml_per_day` (**2 000**) comme
  dénominateur, alors que `/data` sert `ph_daily_max_ml` = **1 768**. Constaté **sur capture** :
  « 1600 / 2000 mL », barre à 80 %, et juste dessous « Maximum journalier : 1 768 mL/j ».
- Les deux valeurs sont bien des notions **différentes**, et toutes deux utiles.

### ⛔ FAUX — le point 4 de la fiche

> « Vérifier le même défaut sur la page ORP : `max_orp_ml_per_day` = 2000 et
> `orp_daily_target_ml` = 1500 sont servis de la même façon. »

`orp_daily_target_ml` est le **volume quotidien visé en mode programmé** (`dosing_logic.h:646`,
« cible utilisateur ») — ⛔ **pas** un plafond dérivé du matériel. Les comparer aurait été
**exactement la faute que cette fiche dénonce** : confondre deux notions parce qu'elles portent
des nombres voisins.

### Ce qui est vrai à la place, et qui est PIRE

`getPhDailyMaxMl()` (`pump_controller.cpp:2416`) n'existe **que pour le pH**. Côté chlore :

| | pH | Chlore |
|---|---|---|
| Plafond réglé | 2 000 mL | 2 000 mL |
| Débit soutenable **servi** | 4,33 mL/min | 4,25 mL/min |
| Minutes de filtration programmées (22/11) | 408 | 408 |
| Plafond réellement atteignable | **1 768 mL** (servi) | **≈ 1 734 mL** — ⛔ **non servi** |
| Ligne « Maximum journalier » à l'écran | présente | ⛔ **absente** |

⇒ La carte chlore porte **le même défaut, de la même ampleur**, et **rien à l'écran** ne permet
de s'en apercevoir. ⛔ Le navigateur ne peut pas le calculer : `scheduledMinutesToday` n'est pas
servi, et le dériver serait un seuil recalculé côté client (**ADR-0100**).

### Défaut SUPPLÉMENTAIRE, absent de la fiche

`dailyCumulSegment()` (`data/app.js:5121`) — le segment « **Cumul XX %** » du **tableau de bord** —
utilise, de son propre aveu, « la même arithmétique que `updateDailyDoseDisplay` », donc **le
même mauvais dénominateur**. Le défaut vit **à deux endroits**, pas un.

### Arbitrage rendu par l'utilisateur

La contrainte « ⛔ **aucun champ firmware à ajouter** » reposait sur la prémisse fausse ci-dessus.
Mise devant les chiffres, la décision est de **corriger les deux axes** : un accesseur
`getOrpDailyMaxMl()` calqué sur celui du pH est ajouté à `pump_controller`, **en lecture seule**
— ⛔ aucun chemin nouveau vers les pompes, ⛔ aucune limite de dosage modifiée.
⇒ `pool-chemistry` est **coché** et son visa est bloquant.

### Captures de l'état AVANT (390 px)

`/tmp/claude-0/-root-esp32-pool-controller/998bfba8-fb0a-4232-bd9b-381cf4a0a742/scratchpad/b208/`
— `avant_ph_390.png`, `avant_orp_390.png`, `avant_dashboard_cumul_390.png`.

## Comportement attendu
1. La barre se remplit vers la limite **réellement opposable** : `min(max_ph_ml_per_day, ph_daily_max_ml)`.
2. ⚠️ Quand les deux diffèrent, l'écran **dit pourquoi** — ⛔ sans jargon : le plafond réglé n'est
   pas atteignable parce que la pompe ne peut pas débiter autant dans une journée.
3. ⛔ **Ne pas masquer le chiffre réglé** : l'utilisateur doit pouvoir constater que son réglage
   est au-dessus de ce que son matériel sait faire — c'est une information d'installation.
4. ⚠️ **Vérifier le même défaut sur la page ORP** : `max_orp_ml_per_day` = 2000 et
   `orp_daily_target_ml` = 1500 sont servis de la même façon.

### Cas limites
- Les deux clés **égales** ⇒ ⛔ aucune explication affichée, rien ne cloche.
- Une clé **absente** (firmware antérieur) ⇒ se rabattre sur celle qui existe, ⛔ jamais sur un
  nombre écrit en dur côté client (**ADR-0100**).
- `ph_daily_max_ml` à **0** ou non fini ⇒ ⛔ ne pas diviser par zéro, ⛔ ne pas afficher 0 %.

## Contraintes techniques
- ⛔ **Aucun champ firmware à ajouter** : les deux valeurs sont **déjà servies**.
- ⛔ **Aucun seuil recalculé côté navigateur** (ADR-0100) — on compare deux clés servies, on n'en
  dérive pas une troisième.
- ⚠️ **390 px d'abord.** La carte héros pH porte déjà 13 éléments (compté sur capture le
  2026-11-22) : ⛔ toute explication ajoutée doit être **compensée**, cf. bug-201.
- Français, sans jargon de code.


## ⭐ VISAS `pool-chemistry` — transcrits ici, car un gate ne garde aucune mémoire

⛔ Les commentaires livrés citent des conditions numérotées. Sans cette section, elles
seraient **invérifiables par le prochain lecteur** (constat B2 de la revue de code). Une
condition n'existe que si elle est ÉCRITE dans la fiche.

### Visa 1 — ajout de `getOrpDailyMaxMl()` et de la clé `orp_daily_max_ml` — **GO SOUS CONDITIONS**

| # | Condition | État |
|---|---|---|
| C1 | Le dénominateur est le **minimum** des deux plafonds, jamais l'un des deux. ⚠️ À `orp_limit_minutes = 60` (réglage autorisé), `phSustainableAvgFlow()` rend l'amplitude **entière** : ~10 400 mL/j, **cinq fois** le plafond réglé — la barre marquerait 19 % à l'instant où le dosage s'arrête. | ✅ `_effectiveDailyMax()` + 2 vecteurs dédiés |
| C2 | Sur la voie chlore, `max_chlorine_ml_per_day` **n'est pas** la limite opposable : `getEffectiveMaxChlorineMlPerDay()` l'est. Forme minimale exigée : la barre ne contredit **jamais** le verdict firmware — `*_limit_reached` ⇒ 100 % et danger. | ✅ court-circuit + clé `orp_effective_max_ml_per_day` publiée (forme recommandée, faite) |
| C3 | Le libellé ne suggère **aucun levier de réglage**. Les deux qui existent : relever la limite horaire « marche » (12 min/h suffiraient) mais défait la garde qui protège réellement ; relever la consigne ORP est **inopérant** (régulation P pure bornée par le soutenable) **et dangereux** (pousse le pH vers `kPhOrpInhibit` = 7,60, où la boucle chlore s'éteint). Phrase « il n'y a rien à corriger si l'eau est bonne » exigée. | ✅ texte + vecteur « aucun impératif » |
| C4 | ⛔ Le chlore ne mérite **pas** un message plus fort que le pH (1 734 mL/j est **au-dessus** du besoin relevé de 939-1 500 mL/j). Et la note de plafond **cède la place au stock** : au 2026-11-22 le bidon de chlore est au bout, annoncer un plafond donnerait une explication **fausse** d'un symptôme **vrai**. | ✅ `_stockTakesPrecedence()`, branché sur la cascade de bug-211 |
| C5 | `null` se traite, ne se devine pas : repli sur le réglage, **aucune** note, ⛔ jamais 0 %. Et en mode Auto le dénominateur peut **rétrécir en cours de journée** sous le volume déjà injecté : la barre sature, le texte d'état ne conclut **pas** à « limite atteinte ». | ✅ 4 vecteurs |
| C6 | Poser la clé **entre** les marqueurs de parité — ⚠️ le bloc pH jumeau est **HORS** des marqueurs : le calquer aurait offert une 5ᵉ récidive du piège n° 1 du projet. | ✅ sentinelle verte **sans exception ajoutée** |

### Visa 2 — correctif de `effectiveMaxChlorinePure()` (Mode Boost) — **GO SOUS CONDITIONS**

Formule validée telle que proposée : `max(réglé, min(réglé × 1,5 ; 1 000))`.

⭐ **L'argument qui tranche** : la fonction jumelle `effectiveOrpTargetPure()` porte **déjà** cette
garde, avec le commentaire « garantit que la cible n'est JAMAIS abaissée » (condition #5). La
règle existait donc depuis l'origine — elle avait été appliquée à **un** des deux leviers du Boost
et oubliée sur l'autre. **Ce n'est pas un arbitrage neuf, c'est une omission.**

⛔ **Explicitement refusé** par le visa : réinterpréter le plafond dur comme un plafond de
l'incrément (`réglé + min(réglé×0,5 ; 1 000)`) — à 800 mL réglés cela donnerait 1 200 au lieu de
1 000, un desserrage **réel** sans justification chimique. La formule retenue ne change **aucune**
valeur qui n'était pas inversée.

| # | Condition | État |
|---|---|---|
| C1 | Écrire le correctif dans l'**idiome de la jumelle** (`raised` + comparaison), pas avec `fmaxf`, et rattacher explicitement à la condition #5 pour qu'un futur lecteur ne « re-simplifie » pas le `max()`. | ✅ |
| C2 | Signaler le levier devenu **inerte** à l'activation du Boost (réglage ≥ 1 000 mL), sur le patron des deux `warning` d'honnêteté d'effet existants. | ✅ `startBoost()` |
| C3 | Vecteurs + mutations, dont l'invariant **balayé** — ⚠️ la monotonie seule ne tue pas le mutant (l'ancienne formule était monotone aussi) : c'est `eff >= réglé` qui le tue. Zéro régression sur les 5 cas existants. | ✅ 8 vecteurs, 3 mutations, 181 cas de la suite verts |
| C4 | ADR + corrections documentaires **dans le même lot**, dont l'amendement du passage d'ADR-0093 qui justifiait Boost + Marche par « le volume reste borné à 1 000 mL » — phrase devenue fausse, et porteuse d'un visa passé. | ✅ ADR-0143, ADR-0093 amendé, ADR-0025 rectifié |
| C5 | Publier le plafond réellement opposé, à parité. ⚠️ L'écart ne disparaît pas, il **change de signe** : à 500 mL réglés et Boost armé, le firmware autorise 750 et la barre saturerait. | ✅ `orp_effective_max_ml_per_day` |
| R1/R2 | `static_assert` sur les deux constantes ; commentaire de `kBoostDailyHardCapMl` requalifié en **plafond du relèvement**. | ✅ |

### ⛔ Ce que les visas laissent ouvert, écrit et non traité

1. **L'inertie du levier Boost n'est dite qu'au journal**, pas à l'écran : au-delà de 1 000 mL/j
   réglés, un propriétaire qui n'ouvre pas le journal croit encore que le Boost relève sa limite.
2. **Le genou de 666,7 mL/j** n'est publié nulle part : rien ne dit à partir de quel réglage ce
   levier cesse d'agir.
3. **La chloration de secours garde sa borne propre** de 2 000 mL/j, indépendante : les deux
   mécanismes ne sont pas unifiés, et ce lot ne les unifie pas.
4. **Le plafond réglé à 2 000 mL/j lui-même** : le visa note que 2 L/j représentent ~+5,7 mg/L de
   chlore libre sur 40 m³, au-dessus de la fenêtre de confort (2,5-3 mg/L à CYA 35). ⛔ Ce n'est
   **pas** ce correctif qui crée ce risque — ce plafond est déjà opposable 24 h/24 hors Boost —
   mais si la borne doit être rediscutée, c'est une fiche à part.
5. ⚠️ **`max_chlorine_ml_per_day` et `max_ph_ml_per_day` entrent par `/set-config` SANS validation
   serveur** (`web_routes_config.cpp`). Ce lot a fermé la conséquence visible (garde `isfinite`
   avant publication), **pas la cause**. Une fiche de durcissement serait légitime.

### Revue de code — constats traités

| # | Constat | Correction |
|---|---|---|
| B1 | `orp_effective_max_ml_per_day` publiée **sans garde**, seule des quatre clés voisines. Un réglage négatif accepté par `/set-config` ressortait en `(uint32_t)lroundf(-5)` = **4 294 967 291**, affiché tel quel. `lroundf(NaN)` puis cast est en outre un **comportement indéfini**. | garde `isfinite(...) && >= 0`, même expression des deux côtés |
| B2 | Les visas n'étaient écrits nulle part. | **cette section** |
| M1 | La note accusait « la durée de filtration » — **une** cause parmi trois. Filtration en Marche 24 h/24 + limite horaire basse ⇒ coupable innocent désigné. C'était la faute de cette fiche, déplacée de la valeur vers la cause. | phrase neutre + vecteur qui **interdit** de nommer une cause |
| M2 | `réglage = 0` était confondu avec « absent » : l'en-tête annonçait « 0 / 1 768 mL » barre **pleine**. Or `min="0"` est saisissable et le firmware traite 0 comme « tout bloqué ». | `zeroSet()` + vecteur |
| M3 | `dosing_logic.h` documentait encore l'**ancienne** formule. | remis en phase mot pour mot |
| m1/m2 | `aria-valuetext` contradictoire au court-circuit ; `aria-valuenow="0"` statique dans le HTML. | corrigés, 2 vecteurs |
| m3 | `maxMlKey` devenu **paramètre mort**. | retiré de la signature et des 2 appels |
| m4 | Comptage d'octets : clé de 28 caractères, pas 29. | code **et** doc recomptés : 3 664 o, marge 432 |

## Fichiers potentiellement concernés
- `data/app.js` — la barre et son dénominateur (`ph_daily_max_ml`, autour de `:7326`)
- `data/index.html` — carte héros pH, et son équivalent ORP
- `docs/features/page-ph.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] B1 — la barre se remplit vers la limite opposable, pages pH **et** ORP
      (`_effectiveDailyMax()`, `min(enforced, capacity)`) — et le **même** dénominateur alimente
      le segment « Cumul XX % » du tableau de bord (défaut supplémentaire, absent de la fiche)
- [x] B2 — l'écart entre réglé et atteignable est **expliqué**, sans jargon
      (`.daily-dose__note`, gris, conditionnel, ⛔ aucun impératif ni nom de réglage),
      et la note **cède la place au stock** (cascade bug-211)
- [x] D1 — vecteurs `test/daily_dose_limit_vectors.js` + garde CI `daily-dose-limit-vectors`,
      extraction par ancre (ADR-0099). **Deux vecteurs gardent le sens DANGEREUX** (prendre le
      plafond matériel seul : ~10 400 mL/j à 60 min/h). Côté firmware, suite `test_B208_*`
      (8 cas, dont un balayage d'invariant 0→3 000 mL)
- [x] D2 — capture à 390 px, avant / après — faite, et c'est elle qui a montré le défaut
      (⚠️ `screenshots/08_ORP.jpg` à refaire pour la documentation utilisateur)
- [x] C1 — docs + CHANGELOG (v3.1.0) + [ADR-0143](../../../docs/adr/0143-le-mode-boost-ne-peut-qu-ajouter.md)

## Sécurité chimique
- Dosage impliqué : ⛔ **non** — aucune limite n'est modifiée, seul leur **affichage** l'est.
- ⚠️ Risque indirect, et c'est la raison d'être de cette fiche : un propriétaire qui croit son
  plafond à 2 000 mL **règle sa consigne en conséquence**. Un chiffre faux à l'écran oriente de
  vraies décisions de dosage.

## Comment ce bug a été trouvé
En instruisant bug-201, l'`ux-ui-designer` a relevé « deux plafonds pour une même notion » et
proposé de **fusionner les deux lignes**. ⛔ Vérification faite sur l'appareil : ce n'est pas un
doublon, et fusionner aurait **effacé une information vraie**.
⭐ *Le constat était juste, le diagnostic était faux* — et c'est la vérification sur l'appareil,
pas la lecture du code, qui a fait la différence.

## Suivi / itérations — clôture du 2026-11-22

### Itération unique — livré (tests verts, revue OK, visa `pool-chemistry`)

Le lot contient **DEUX corrections**, la seconde hors périmètre d'origine, intégrée sur demande
explicite de l'utilisateur.

**Correction 1 — le dénominateur de la barre.** Conforme à la fiche après sa vérification
(section « ⛔ VÉRIFICATION DE LA FICHE AVANT TRAITEMENT »). ⚠️ La contrainte d'origine
« aucun champ firmware à ajouter » **est levée** : elle reposait sur la prémisse fausse du
point 4. Deux clés sont ajoutées, en lecture seule.

**Correction 2 — le Mode Boost ABAISSAIT le plafond chlore.** `effectiveMaxChlorinePure()`
rendait `min(réglé × 1,5 ; 1 000)` : au-delà de **666,7 mL/j réglés**, le Boost **faisait
descendre** la limite. Sur l'appareil de production (2 000 mL/j), armer le Boost ramenait le
plafond opposé à 1 000 mL/j et déclenchait un `critical` à mi-course. Corrigé en
`max(réglé, min(réglé × 1,5 ; 1 000))` — strictement la **condition #5** que la fonction jumelle
`effectiveOrpTargetPure()` porte depuis l'origine, et qui n'avait été appliquée qu'à **un** des
deux leviers. Décision formalisée : [ADR-0143](../../../docs/adr/0143-le-mode-boost-ne-peut-qu-ajouter.md).

### Fichiers modifiés

**Firmware** : `src/version.h` (3.0.3 → **3.1.0**), `src/dosing_logic.cpp`, `src/constants.h`
(commentaire de `kBoostDailyHardCapMl` + 2 `static_assert`), `src/config.cpp` (`warning`
d'honnêteté d'effet du 3ᵉ levier inerte), `src/pump_controller.{h,cpp}` (`getOrpDailyMaxMl()`),
`src/web_routes_data.cpp`, `src/ws_manager.cpp` (`orp_daily_max_ml`,
`orp_effective_max_ml_per_day`, posées **entre les marqueurs `PARITE-DATA`**).

**Interface** : `data/app.js` (`_effectiveDailyMax()`, `_stockTakesPrecedence()`,
`updateDailyDoseDisplay()`, `buildSensorSegments()`, retrait du pilotage de
`#ph_capacity_block`), `data/index.html` (`#ph_capacity_block` supprimé, `.daily-dose__note`
pH + ORP, « MAJ auto toutes les 5 secondes » retiré de la carte ORP), `data/app.css`
(`.daily-dose__note`).

**Tests** : `test/test_native_dosing/test_dosing_decision.cpp` (suite `test_B208_*`),
`test/daily_dose_limit_vectors.js` (neuf), `tools/check_daily_dose_limit_vectors.sh` (neuf),
`test/decimal_format_vectors.js` (prélude), `.github/workflows/hygiene-specs.yml`
(job `daily-dose-limit-vectors`).

**Documentation** : `CHANGELOG.md`, `docs/adr/0143-*` (neuf), `docs/adr/README.md`,
`docs/adr/0093-*` et `docs/adr/0025-*` (amendements), `docs/API.md`,
`docs/subsystems/ws-manager.md`, `docs/subsystems/pump-controller.md`,
`docs/features/page-ph.md`, `page-orp.md`, `page-dashboard.md`, `docs/DESIGN_SYSTEM.md` (§5.27),
`docs/user/src/guide-utilisation.html`, `docs/user/src/guide-regulation.html`.

### Reste ouvert, écrit plutôt que tu

- **D2 est fait** : captures 390 px avant/après dans le scratchpad de la session
  (`b208/avant_ph_390.png`, `avant_orp_390.png`, `final_ph_390.png`). ⭐ C'est la capture qui a
  révélé le défaut d'origine — « 1 600 / 2000 mL » avec « Maximum journalier : 1 768 mL/j »
  juste dessous — et c'est elle encore qui a montré que la première rédaction de la note tenait
  en trois lignes. Reste l'étape **humaine** : **`screenshots/08_ORP.jpg` est à refaire** (elle montre la ligne « MAJ auto toutes les
  5 secondes » que ce lot retire). ⓘ `screenshots/07_pH.jpg` est périmée **depuis la v3.0.0**
  pour la même ligne — dette antérieure, pas causée par ce lot.
- **L'inertie du levier Boost n'est dite qu'au journal**, pas à l'écran : un propriétaire qui
  n'ouvre pas le journal croit toujours que le Boost relève sa limite journalière
  ([ADR-0143](../../../docs/adr/0143-le-mode-boost-ne-peut-qu-ajouter.md), « Négatives »).
- **Le genou de 666,7 mL/j** (`kBoostDailyHardCapMl / kBoostDailyFactor`) n'est publié nulle
  part : rien à l'écran ne dit à partir de quel réglage ce levier cesse d'agir.
- **Les PDF utilisateur ne sont pas régénérés** (règle du projet : à la release).
