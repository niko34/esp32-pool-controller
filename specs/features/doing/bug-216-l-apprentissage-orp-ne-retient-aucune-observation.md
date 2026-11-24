# bug-216 — L'apprentissage ORP ne retient aucune observation en conditions réelles

## Métadonnées
- **ID** : bug-216
- **Slug** : l-apprentissage-orp-ne-retient-aucune-observation
- **Type** : bug
- **Statut** : doing — **volet 1 livré en v3.3.1 (retrait pur), déployé sur l'appareil le 2026-11-24 à 20 h 54** (firmware seul, signé ; `/get-system-info` rend 3.3.1 ; compteurs du jour conservés) ; volets 2 et 3 en attente de R1/R2
- **Priorité** : haute — ⚠️ **constaté en production le 2026-11-24** : les deux apprentissages
  livrés en v3.2.0 et v3.3.0 sont restés à zéro observation, et l'ORP a dépassé sa cible de
  110 mV après une nuit de filtration.
- **Version cible** : à définir (`src/version.h` = 3.3.0 au moment de l'écriture)
- **Dernière mise à jour** : 2026-11-24
- **Spec(s) liée(s)** : feature-202 (pause mélange apprise), feature-203 (rendement appris)

## Domaines touchés
- [x] Régulation — dosage ORP
- [x] Capteurs — représentativité de la mesure ORP après une reprise de circulation
- [ ] Filtration · MQTT · Web · OTA · Authentification · Historique · Frontend

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **OBLIGATOIRE et en premier** : les trois volets touchent des gardes
      qu'il a lui-même posées (gate P2 du 2026-11-23)
- [ ] ux-ui-designer — seulement si un libellé de la page Chlore change
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Les apprentissages de feature-202 (durée de la montée après injection) et de feature-203
(ce qu'un millilitre rapporte) reposent sur la **même observation**. En conditions réelles,
cette observation est presque toujours rejetée. ⇒ Les deux fonctions sont livrées, affichées,
et **inactives** : l'appareil se comporte comme avant elles.

## Contexte — ce qui a été relevé le 2026-11-24

Filtration laissée en marche la nuit du 23 au 24 par l'utilisateur. Cible ORP 530 mV.

| Moment | ORP | Chlore injecté |
|---|---|---|
| 23/11, 23 h | 473 mV | en cours |
| 0 h → 4 h 15 | 469 → 510 | ≈ 1 070 mL, au plafond de 10 min/h |
| 4 h 40 → 7 h 17 | 505 à 517 | ≈ 70 mL, sept petites séances |
| 7 h 17 | 515 | **dernière injection du jour** (total 1 136 mL) |
| 7 h 24 → 9 h 36 | pas de mesure | filtration arrêtée |
| 9 h 50 | 586 | rien |
| vers midi | 646 (maximum) | rien |
| 15 h 50 | 616 | rien |

- `/data` à 15 h 53 : `orp_mix_obs_count = 0`, `orp_yield_obs_count = 0`,
  `orp_mix_delay_learned = false`, `orp_yield_mv_per_ml = null`.
- Journal : huit observations jetées, toutes au motif « fenêtre perturbée » ou
  « conditions perturbées » ; **0 close**.
- ⚠️ **L'appareil n'a jamais injecté au-dessus de la cible.** Le dépassement est l'effet
  différé du chlore versé avant 7 h 17.
- Jours précédents (filtration 9 h 36 – 16 h 24) : 400 à 600 mL/jour d'après les journaux
  archivés ; maximum d'ORP 605 (19/11), 576, 557, 533, 554. ⇒ Un dépassement existait déjà
  certains jours ; la nuit du 24 l'a amplifié avec **environ le double** de chlore.
- Température de l'eau : 12,7 °C → 11,5 °C. Aucune corrélation relevée entre eau froide et
  ORP haut sur deux semaines.

## Comportement actuel — trois causes distinctes

### Volet 1 — une séance de pH ouverte rejette l'observation

`src/pump_controller.cpp:4130` :

```
const bool orpEnvClean = waterPresence.waterPresent && !phDosingState.active && pumpDuty[0] == 0 && …
```

⚠️ **C'est un choix délibéré**, pas une étourderie : le commentaire au-dessus dit « aucune
injection pH/ORP (session ouverte ou pompe en marche) ». La raison est chimique — l'acide
fait baisser le pH, donc monter l'ORP, ce qui fausserait la montée observée.

⛔ **Mais sa conséquence n'a pas été mesurée** : une séance de pH dure jusqu'à
`kPhSessionMaxWallMs` = **3 heures** (`src/constants.h:3854`), pour 4 à 8 minutes de pompe.
Le 24/11 : séances de 3 h 35 à 6 h 35 (pompe 480 s), puis à 7 h 18, puis de 11 h 38 à
14 h 38 (pompe 250 s). ⇒ La fenêtre où l'observation est permise est presque vide.

### Volet 2 — la réinjection annule l'observation avant qu'elle puisse conclure

Constater qu'une montée dure plus de 20 minutes demande au moins deux jalons, soit 40 minutes
sans injection de chlore (`orpQuiet`, `:4133`). Or tant que la mesure est sous la cible,
l'appareil réinjecte dès la fin de la pause de 20 minutes. Vu au journal à 7 h 16 :
l'observation ouverte à 6 h 55 est jetée par l'injection suivante.

⇒ L'apprentissage ne peut aboutir que sur la **dernière** injection, cible atteinte. Quand le
bassin est loin de la cible — le cas où l'empilement des doses coûte le plus — il n'apprend
rien.

### Volet 3 — le plafond de la pause apprise est sous la durée relevée

`kOrpMixingDelayLearnedMaxMs` = 6 × 20 min = **2 heures** (`src/constants.h:2347`). Or du 19
au 24/11, la mesure continue de monter **3 à 4 heures** après la dernière injection (19/11 :
une injection vers 9 h, puis 542 → 605 mV sans rien ajouter). ⚠️ Une observation « sans fin
au plafond » est jetée : ces montées-là ne seraient donc jamais enregistrées.

## ⚠️ Hypothèse à instruire AVANT de toucher aux volets 2 et 3

**Une partie de la montée après reprise pourrait ne pas venir du chlore injecté.**

- Chaque matin, l'ORP est lu ~100 mV plus bas qu'à l'arrêt de la veille (395 à 494 mV).
- 22/11, 9 h → 10 h : 395 → 433 mV **sans aucune injection**.
- 24/11, 9 h 50 → midi : 586 → 646 mV **sans aucune injection**.

Si la mesure remonte en partie seule après une reprise de circulation (sonde, dérivation,
eau stagnante), alors l'appareil injecte chaque matin contre un écart qui se serait réduit
de lui-même, et une « montée de 3 à 4 heures » mêle deux phénomènes. ⛔ **Ne pas allonger une
pause ni relever un plafond sur une durée dont on ne sait pas ce qu'elle mesure.**

**Ce qui est en place pour trancher** : copie du journal, des mesures, de l'historique et
des réglages **toutes les deux heures** depuis le 2026-11-24, dans
`/root/pool-captures/suivi-orp/` (lecture seule). L'utilisateur laisse tourner quelques
jours sur les horaires habituels, sans rien modifier. Essai complémentaire proposé, non
décidé : un matin avec le chlore en manuel pendant trois heures.

## Gate pool-chemistry — P1 sur le critère du volet 1 (2026-11-24)

**Verdict : GO SOUS CONDITIONS**, avec une réserve franche (point 7). Recopié ici parce
qu'un gate ne garde aucune mémoire entre deux passages.

**Ordres de grandeur retenus par l'agent** (⚠️ bornes prudentes, pas des mesures) : une
impulsion d'acide ≈ 4,6 mL ; ≈ 0,0004 pH par mL sur ce bassin ; ≤ 60 mV par unité de pH
⇒ **≤ 0,1 mV d'ORP par impulsion**, sous le bruit de la sonde. Chlore de la nuit du 24/11 :
≈ 0,1 mV/mL ; acide : ≤ 0,024 mV/mL. L'effet de l'acide mélangé va dans le sens sûr (ORP
plus haut ⇒ pause plus longue, moins de chlore). Seul sens dangereux : un pic passager.

**Critère tranché**
1. Pompe pH en marche (régulation, manuel ou test) ⇒ état « acide récent ».
2. **Délai D = 5 min après CHAQUE arrêt de la pompe pH, toutes origines** (filtre de mesure
   ≈ 3 min + retour éventuel vers les sondes). ⇒ Exige un horodatage « dernier arrêt acide »
   posé en un seul endroit, à chaque passage marche → arrêt — ⚠️ **il n'existe pas
   aujourd'hui** pour le manuel et le test.
3. Au démarrage, l'horodatage vaut l'instant du démarrage (5 premières minutes « sales »).
4. Une impulsion ne jette PAS une observation ouverte : pas d'ouverture en « acide récent » ;
   un jalon tombant en « acide récent » est **reporté** au premier instant propre, sans
   toucher la grille ; aucun instant propre avant le jalon suivant ⇒ jetée ; **volume d'acide
   pendant l'observation ≤ 25 % du chlore de l'injection observée**, sinon jetée.
5. Même critère pour 202 et 203, une seule source.

**Conditions** : C1 retirer `!phDosingState.active`, garder tous les autres termes ·
C2 un seul horodatage d'arrêt acide · C3 report du jalon et budget d'acide dans la logique
pure ; une décision prise en « acide récent » ne raccourcit jamais l'observation ·
C4 ni limite, ni pause, ni dose ne change · C5 réécrire le critère d'acceptation du 24/11.

**Tests exigés** : rythme d'une impulsion toutes les 7,2 min ⇒ observation close ; jalon
dans D ⇒ reporté ; ouverture dans D ⇒ refusée ; injection manuelle hors séance ⇒ D
s'applique ; budget d'acide dépassé ⇒ jetée ; chacun validé par mutation.

### ⚠️ Point 7 — la réserve : sur la nuit du 24/11, ce volet seul ne change RIEN

- Séance 3 h 35 – 6 h 35 : 480 s de pompe, une impulsion toutes les 3,75 min ⇒ le délai de
  5 min couvre la séance entière.
- Le chlore est réinjecté toutes les ~22 min alors qu'il en faut 40 pour conclure (volet 2).
- ≈ 50 mL d'acide par observation contre ≈ 10 mL de chlore ⇒ ces observations auraient été
  **ininterprétables** de toute façon.

⇒ Le critère « la nuit du 24/11 ne produit plus huit rejets sur huit » **relève du volet 2**.
Le gain du volet 1 ne porte que sur les journées types (une impulsion toutes les 7,2 min).

### ⚠️ Remarque de l'agent principal, à vérifier avant de coder

Le budget « acide ≤ 25 % du chlore » risque de rejeter aussi les journées types : l'agent
compte ≈ 25 mL d'acide par 40 min l'après-midi du 24/11, ce qui exige une injection de
chlore d'au moins 100 mL, alors que les séances de chlore relevées au journal font le plus
souvent 5 à 50 mL. ⇒ **Le gain pratique du volet 1 seul n'est pas établi.**

## ⭐ Décision de l'utilisateur sur le volet 1 (2026-11-24) — RETRAIT PUR

Le critère de remplacement du gate P1 (horodatage d'arrêt acide, report de jalon, budget
d'acide) est **écarté**. Mots de l'utilisateur : *« Je n'ai jamais demandé ça et ça ne
fonctionnait pas comme ça avant. Pourquoi ajouter des fonctionnements qui posent plus de
problèmes que cela n'en résout ? […] on finit par des usines à gaz. »* — puis : *« oui,
retire »*.

⇒ **Le volet 1 se réduit à une ligne en moins** : retirer `!phDosingState.active` de
`orpEnvClean`, ⛔ **sans rien mettre à la place**. Tous les autres termes restent, dont
`pumpDuty[0] == 0` (pompe d'acide en marche ⇒ tick sale).

Ce qui fonde ce choix : les chiffres du gate P1 lui-même — une impulsion d'acide déplace
l'ORP d'au plus 0,1 mV, sous le bruit de la sonde, et l'erreur éventuelle va vers moins de
chlore. La garde bloquait trois heures par séance pour protéger de presque rien.

⚠️ **Limite assumée et dite à l'utilisateur** : ce retrait seul ne fait pas aboutir
l'apprentissage quand le bassin est loin de la cible (volet 2). La décision de garder ou
non cet apprentissage se prendra sur les relevés (R1).

### Gate pool-chemistry — P1 bis sur le retrait pur (2026-11-24) : **GO**

Le retrait rend l'apprentissage moins précis, mais n'ouvre aucun chemin vers plus de chlore.
- **Pause apprise (202)** : l'acide ne peut que faire monter l'ORP ⇒ pause plus longue. Pire
  cas, une observation de 20 min : `orpMixingEffectiveDelayMs` prend le maximum de trois
  observations et ne descend jamais sous 20 min (`orp_mixing_learn_logic.h:111-121`) ⇒ on
  retombe sur le comportement d'avant feature-202.
- **Rendement (203)** : surestimé ⇒ arrêt plus tôt. Le budget ne fait que couper
  (`shouldDose = false`, `pump_controller.cpp:5368-5370`), il ne prolonge jamais.
- **Cas « 50 mL d'acide pour 10 mL de chlore »** : rendement compté au plus deux fois trop
  haut ; un épisode isolé est absorbé par la médiane de trois ; trois épisodes faussés ⇒
  sous-dosage temporaire de quelques mV, sans danger sanitaire, couvert par le garde-fou
  « trois arrêts sans atteindre la consigne » (`:5374`).
- ⚠️ **Hors lot, consigné** : `pumpDuty[0] == 0` vise la pompe 1 **en dur**, pas la pompe
  d'acide configurée (`mqttCfg.phPump`, voir `:3800`). Pompes permutées ⇒ l'acide
  automatique n'est plus vu par l'observation. Sans danger (effet dans le sens sûr), mais la
  phrase « pompe d'acide en marche ⇒ tick sale » serait alors fausse. Sur l'appareil de
  l'utilisateur : `orp_pump = 2`, donc non concerné.

## Comportement attendu

- **Volet 1** : l'observation n'est rejetée que lorsque l'acide a **réellement** pu fausser
  la mesure — ⚠️ critère à fixer par `pool-chemistry` (pompe pH en marche, ou injection pH
  plus récente qu'un délai à justifier), ⛔ pas la seule ouverture d'une séance.
- **Volets 2 et 3** : à définir **après** l'analyse des relevés. Pistes, sans préjuger :
  ne pas réinjecter tant qu'une observation est en cours ; relever le plafond appris ;
  ignorer la montée propre à la reprise de circulation.
- ⛔ **Invariant conservé** : l'apprentissage ne peut qu'arrêter plus tôt ou faire attendre
  plus longtemps. Jamais injecter plus, plus vite, ni plus fort.

### Cas limites
- Bassin loin de la cible pendant des heures (nuit du 24/11) : c'est le cas à couvrir.
- Séance de pH et séance de chlore simultanées.
- Reprise de circulation après un arrêt long : observation à écarter ou non — voir hypothèse.

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` — `orpEnvClean` / `orpQuiet` (`:4130-4134`), `tickOrpLearn` (`:714`)
- `src/orp_mixing_learn_logic.h`, `src/orp_yield_learn_logic.h` — logique pure
- `src/constants.h` — `kOrpMixingDelayLearnedMaxMs` (`:2347`)
- `test/test_native_*` — suites des deux apprentissages
- `docs/subsystems/`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`

## Livrables — check-list
- [ ] R1 — Analyse des relevés de `/root/pool-captures/suivi-orp/` sur plusieurs jours
- [ ] R2 — Verdict écrit sur l'hypothèse « montée propre à la reprise »
- [x] A1 — Volet 1 : condition `!phDosingState.active` **retirée sans remplacement** (décision utilisateur), validée par `pool-chemistry` (P1 bis : GO)
- [ ] A2 — Volets 2 et 3 : décision tracée, avec la mesure qui la fonde
- [x] T1 (volet 1) — sentinelle `test_24` + deux mutants tués
- [ ] T1 (volets 2 et 3) — tests rejouant la nuit du 24/11 (réinjections) + mutation
- [x] D1 (volet 1) — `CHANGELOG.md` 3.3.1, `docs/subsystems/pump-controller.md`, note datée dans l'ADR-0145
- [ ] D1 (volets 2 et 3)

## Sécurité chimique
- Dosage impliqué : **oui** — dosage de chlore.
- ⚠️ Toute modification passe par `pool-chemistry` **avant** le code. Les limites horaires et
  journalières ne sont pas touchées par cette fiche.
- ⚠️ 1 136 mL en une nuit, plus la dose de la veille : ordre de grandeur de plusieurs mg/L
  pour un bassin de 30 à 50 m³. Les plafonds ont tenu ; c'est leur somme sur 24 h glissantes
  qui n'est bornée par rien d'autre que le plafond journalier remis à zéro à minuit.

## ADR attendu ?
- **Probable** : oui, si le critère de propreté ou le plafond appris changent.
- **Sujet pressenti** : ce qu'une observation de montée ORP a le droit de mesurer.

## Éléments visibles (pour doc-writer)
- Page Chlore : lignes « Effet du chlore », « Quantité prévue », « Pause mélange » — elles
  restent masquées ou à leur valeur d'usine tant que rien n'est appris.
- Journal : messages « Observation de la montée jetée », « Épisode de rendement jeté ».
- Endpoints, WebSocket, MQTT : aucun changement prévu.

## Critères d'acceptation
- [ ] Sur une journée type avec séances de pH, au moins une observation est **close**
- [ ] **(volet 2, déplacé par le gate P1)** Rejouée en test, la nuit du 24/11 ne produit plus huit rejets sur huit — ⚠️ le volet 1 seul doit au contraire **affirmer** 0 observation close sur ce scénario
- [ ] Aucune modification ne permet d'injecter plus qu'avant
- [x] `pio run` et `pio test -e native` passent, tests neufs validés par mutation — **pour le volet 1** (chiffres en notes d'implémentation)

## Notes d'implémentation — volet 1 (2026-11-24, v3.3.1)

**Ce qui a changé, et rien d'autre** (`git diff -- src/ test/`) :
- `src/pump_controller.cpp:4135` — `!phDosingState.active &&` retiré de `orpEnvClean` ;
  commentaire `:4114-4122` réécrit. Aucune constante, aucun horodatage, aucun journal ajouté.
- `src/version.h` — 3.3.0 → 3.3.1.
- `test/test_native_orp_mixing_learn/test_orp_mixing_learn.cpp:362` — sentinelle `test_24` :
  `orpEnvClean` ne cite plus `phDosingState.active` et cite toujours `pumpDuty[0] == 0`.

**Rapport test-engineer** (mesuré, 2026-11-24) :

| Mutant | Résultat |
|---|---|
| M1 — remettre `!phDosingState.active &&` | `test_24` échoue ✅ |
| M2 — retirer `pumpDuty[0] == 0 &&` | `test_24` échoue ✅ |

- `ls -d test/test_native_* | wc -l` : **99 suites** ; `pio test -e native` : **2 800 cas, 2 800
  réussis**.
- `pio run -e esp32dev_core3` : SUCCESS, aucun avertissement. RAM 30,1 % (98 680 o), Flash
  53,0 % (1 980 505 o).
- ⚠️ Ces tests prouvent que la condition est retirée, ⛔ **pas** que l'apprentissage aboutit
  sur l'appareil.

**Revue de code** : le code est jugé approuvable — périmètre exact, deux seuls lecteurs de
`orpEnvClean` (`:732` rendement, `:752` pause), aucune course créée. Sa seule réserve était
de procédure (rapport de test absent de la fiche) : levée par le tableau ci-dessus.
- Limite mineure notée sur `test_24` : un `//` en fin de ligne **dans** la définition serait
  lu ; il ferait échouer le test à tort, jamais réussir à tort.
- ⚠️ **Correction du fait hors lot** : pompes permutées, la pause apprise (202) voit encore
  l'acide, par hasard, via `pumpDuty[1]` dans `orpQuiet` ; seul l'épisode de rendement (203)
  ne le voit plus. Défaut **antérieur à ce lot**, non traité ici.

**Gate chimie sur le diff** : pas de troisième passage. Le diff livré est mot pour mot le
changement validé en P1 bis, vérifié par l'agent principal dans `git diff`.

**Pas d'ADR** : la décision est un retrait, tracé dans cette fiche ; note datée ajoutée à
l'ADR-0145.

**Reste ouvert** : critère « au moins une
observation close sur une journée type », vérifiable seulement sur l'appareil ; volets 2 et
3 ; décision de fond — garder ou non cet apprentissage — sur les relevés (R1).

## Notes pour agent
- ⚠️ **Défaut de la famille « livré, vert, jamais vu fonctionner »** : les compteurs
  d'observations étaient exposés dans `/data` et lisibles dès le premier jour.
- ⛔ Ne pas présenter le volet 1 comme une erreur de code : c'est une garde chimique posée
  volontairement, dont l'effet réel n'avait pas été chiffré.
- ⛔ Ne pas traiter les volets 2 et 3 avant R1 et R2.

## Hors périmètre
- Le plafond journalier remis à zéro à minuit (constat noté en « Sécurité chimique »).
- Le bruit de mesure pH et la carte v3 (feature-215).
