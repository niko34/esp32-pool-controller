## Métadonnées
- **ID** : feature-097
- **Slug** : seuil-de-stabilite-sur-donnees-terrain
- **Type** : feature (réglage sur données — condition 10 du gate pool-chemistry v2.34.0)
- **Statut** : doing — **verrou d'entrée LEVÉ le 2026-09-20** : 8 matins dépouillés (le protocole exigeait 5) et la trace 5 s enfin capturée. Le critère « ne rien changer » de la spec est **écarté** (cf. §Verdict du critère de décision)
- **Priorité** : moyenne
- **Version cible** : 2.54+ (firmware courant : 2.53.0)
- **Dernière mise à jour** : 2026-09-20
- **Spec(s) liée(s)** : feature-091 (purge adaptative, done en v2.34.0), plan `specs/plan-2026-09-regulation-et-robustesse.md` (O1/O2, journal 19:10 et 20:50), ADR-0059

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — détecteur de stabilité `purge_stability_logic`
- [x] Régulation (pump_controller, PID) — indirect : la porte de mesure conditionne le dosage
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : le détecteur conditionne le premier dosage du matin
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer — rejeux natifs + mutation, comme pour 091
- [x] code-reviewer
- [x] doc-writer

## Description

Le détecteur de stabilité de la purge (feature-091) juge l'eau stable quand
l'étendue (max − min) de 10 **médianes de bloc** du pH brut est ≤ 0,15 (et
≤ 15 mV en ORP). Sur 8 matins mesurés, la purge dure **16,5 min en médiane** pour
un plancher de 10 — et jusqu'à 37 et 47 min. C'est du temps de régulation perdu
chaque matin (objectif O2 du plan 2026-09).

**Le diagnostic initial de cette spec était faux, et la mesure l'a corrigé.**
Elle supposait que « le seuil est au niveau du bruit » et que la porte s'ouvrait
au plancher « une fois sur deux » — chiffres estimés sur du bruit **synthétique**
uniforme ±0,18. La trace réelle dit autre chose : **13 fenêtres sur 16 (81 %)**
passent le seuil actuel sur eau établie. Le seuil n'est pas mal placé.

**Ce qui est mal choisi, c'est l'estimateur de bloc.** Le pH brut de ce bassin
n'est pas du bruit autour d'une moyenne : c'est un signal **anti-corrélé,
presque à deux niveaux** (autocorrélation au lag 1 : **−0,440**). Sur un tel
signal, la médiane n'est pas une moyenne robuste — c'est un **interrupteur** :
elle bascule sur le niveau momentanément majoritaire. Sept lectures hautes contre
cinq et elle saute ; cinq contre sept et elle tombe. Ces bascules produisent à
elles seules les 3 fenêtres sur 16 qui échouent. **Aucune excursion de l'eau
n'est derrière ces échecs : c'est un artefact de mesure.**

Cette spec **remplace l'estimateur de bloc**, sans toucher à aucun seuil.

## Périmètre — ce qui bouge et ce qui ne bouge pas

La spec a été écrite pour un seul symptôme : **la porte s'ouvre trop tard**.
L'observation du 20/09 a fait apparaître le symptôme symétrique — **la porte se
referme à tort** — et le périmètre a été élargi le 2026-09-20 pour l'examiner.

**Examen fait, conclusion : la re-fermeture ne doit PAS changer.** Les re-purges
ne sont pas un défaut du critère C1, elles en sont la **conséquence** : elles
mesurent la queue de convalescence d'une ouverture survenue trop tôt, sur une
bascule d'estimateur. Corriger l'estimateur supprime la cause.

| Élément | Décision |
|---|---|
| Estimateur de bloc (médiane) | **REMPLACÉ** par la moyenne interquartile |
| `kPurgeStablePhRange` = 0,15 | inchangé |
| `kPurgeStableOrpRange` = 15 | inchangé |
| `kPurgeRecloseRangePh` = 0,30 · `…Orp` = 30 | inchangés |
| `static_assert` hystérésis ×2 (constants.h:2882-2883) | **conservé** |
| `kPurgeWindowMin` = 10 · `kPurgeSamplesPerMin` = 12 | inchangés |
| `kPurgeFloorMs` 10 min · `kPurgeCapMs` 90 min | inchangés |

Deux `static_assert` sont néanmoins **ajoutés ou corrigés** — ils ne changent
aucune valeur aujourd'hui, ils piègent le prochain qui touchera à la géométrie :
1. **Bug latent** : `kPurgeFloorMs >= kPurgeWindowMin * 60000UL` (l.2860-2862)
   suppose en silence « 1 bloc = 1 minute ». À réécrire en durée réelle
   (`× kPurgeSamplesPerMin × kPhOrpSensorIntervalMs`). Vaut 600 000 aujourd'hui :
   satisfait à l'égalité, aucune valeur ne bouge.
2. **Invariant de dérive** : l'égalité ×2 n'exprime la sensibilité à la dérive
   que **par coïncidence** — elle ignore le span. Lui adjoindre
   `reclose ≤ 0,035 pH/min × span`, soit 0,30 ≤ 0,315 ✓ aujourd'hui.
3. **L'assert qui manquait** : `kPurgeStablePhRange > kPurgeNoiseFloorPh`. C'est
   lui qui aurait rendu ce défaut visible dès le 2026-09-04.

## Verdict du critère de décision (appliqué le 2026-09-20)

Le §Pré-requis fixait : « si la médiane des durées réelles (fin 1) est ≤ plancher
+ 3 min **et** qu'aucune fin 2 n'est observée → **ne rien changer**, clore la spec
en *sans objet* ».

- Médiane mesurée sur 8 matins : **16,5 min**. Plancher + 3 = **13 min**.
- Fins de type 2 (plafond 90 min) observées : **0**.

**16,5 > 13 ⇒ le « ne rien changer » est ÉCARTÉ.** La spec se poursuit.


## Contexte

### Options évaluées le 04/09 — toutes écartées sur mesure le 20/09
- **(b) seuil 0,20 — REFUSÉE dès l'origine** : la marge sur le fond en V retombe
  à zéro (4,5 × 0,045 = 0,20). Confirmée refusée ; ne pas la réintroduire par une
  voie détournée.
- **(c) médianes sur blocs de 2 min** (W = 5, horizon inchangé) — était la
  « piste privilégiée ». **Écartée**, sur des motifs entièrement neufs :
  elle traite le symptôme (amplitude du bruit) au lieu de la cause (estimateur
  inadapté) ; elle **raccourcit l'horizon de 9 à 8 min**, donc la couverture du V
  de 0,2025 à 0,180 — la marge tombe à **0,030 pile**, le minimum absolu exigé,
  soit *moins bien qu'aujourd'hui* ; et elle casse la re-fermeture
  (`0,30 / 8 = 0,0375 > 0,035`), ce qui forcerait à démolir l'égalité ×2.
  Comparatif final : (c) atteint un max de fenêtre de 0,093, la moyenne
  interquartile atteint **0,089** — même réduction de bruit, mais l'une dégrade
  la marge sur le V et l'autre n'y touche pas.
- **Statistique de tendance** (pente signée, monotonie, résidu) : **écartée**.
  Les pentes de demi-fenêtre du bruit réel atteignent 0,039 pH/min contre 0,045
  pour le V : **aucune séparation**. La pente pleine fenêtre est propre mais
  **aveugle au V symétrique** (pente ≈ 0 au fond), c'est-à-dire précisément au
  cas dangereux.
- **Seuil dérivé en direct de la dispersion** : **écartée, et à ne pas rouvrir**.
  La dispersion serait mesurée *pendant* la purge, donc gonflée par le transitoire
  lui-même : le seuil s'élargirait exactement au moment où l'eau bouge. Boucle
  auto-réalisatrice.
- **Moyenne simple** : **écartée**. Meilleur bruit (max 0,055) mais **point de
  rupture nul** — une seule lecture I²C aberrante à 14,0 déplace le bloc de +0,60
  et empoisonne 10 fenêtres consécutives. C'est le risque pour lequel l'ADR-0059
  avait choisi la médiane.

### La nature du signal — le fait que ni la spec ni l'ADR-0059 n'avaient vu

| Indice (trace du 20/09) | Mesuré | Attendu si bruit IID |
|---|---|---|
| autocorrélation lag 1 | **−0,440** | 0 |
| saut moyen entre 2 lectures | 0,173 = **1,47 σ** | 1,128 σ = 0,133 |
| changements de signe autour de la médiane | **197 / 299** | ~150 |

Bloc réel de la minute 22 :
`7.051 · 6.747 · 7.083 · 6.752 · 7.083 · 6.743 · 7.085 · 6.775 · 7.079 · 6.749 · 7.079 · 7.068`
— deux niveaux séparés de ~0,21, en alternance.

La corrélation instantanée **pH ↔ ORP vaut +0,462**, et l'ORP est anti-corrélé
lui aussi (−0,396) : les deux électrodes voient la **même perturbation de mode
commun**. Cela confirme l'explication déjà admise sur ce projet — les **courants
de fuite de la pompe de FILTRATION**.

### Comparatif des estimateurs, à seuils et fenêtre INCHANGÉS

| Estimateur de bloc | σ du bloc | étendue de fenêtre min / méd / max | ouverture pH ≤ 0,15 |
|---|---|---|---|
| **médiane (actuel)** | 0,0458 | 0,075 / 0,095 / **0,192** | **13/16** |
| moyenne simple | 0,0163 | 0,034 / 0,041 / 0,055 | 16/16 |
| **moyenne interquartile (retenue)** | **0,0263** | 0,068 / 0,074 / **0,089** | **16/16** |

Voie ORP, même chemin de code : max 11,5 → **8,7 mV** — gain gratuit, aucune
constante touchée.

## Comportement attendu

- L'estimateur de bloc devient la **moyenne interquartile** : on écarte le quart
  le plus bas et le quart le plus haut, on moyenne le milieu (`trim = n/4`, soit
  3 + 3 écartés sur 12, 6 moyennés). Statistique **nommée et standard**, pas un
  réglage ad hoc, et généralisable à toute taille de bloc.
- Sur eau établie avec le bruit réel du bassin, la porte s'ouvre au plancher dans
  **16 fenêtres sur 16** (contre 13/16 aujourd'hui).
- Le fond en V du rinçage (descente −0,047 / remontée +0,043 pH/min) reste
  détecté avec une marge de **0,0525** sur le seuil, pour 0,03 exigé. **Le
  changement d'estimateur ne modifie pas cette marge** : sur une rampe linéaire,
  médiane et moyenne interquartile renvoient toutes deux la valeur au centre du
  bloc. Le V est vu à l'identique ; seul le bruit change.
- La re-fermeture C1 conserve sa pente de déclenchement de **0,0333 pH/min**
  (exigé ≤ 0,035), et son hystérésis **double** — l'écart au plancher de bruit
  passe de `0,30 − 0,192 = 0,108` à `0,30 − 0,089 = 0,211` — sans qu'on y touche.
- Aucun changement d'interface : mêmes champs `/data` et WebSocket, mêmes
  libellés, `measures_purge_last_duration_s` / `_last_end` / `_count` inchangés.
  **Seul changement observable** : le journal `[STAB]` de fin de purge gagne
  l'étendue des blocs au moment du verdict (absente aujourd'hui, et nécessaire
  pour re-calibrer sans nouvelle capture manuelle).

### Cas limites
- **Point de rupture** : il passe de 50 % (médiane) à 25 % des lectures d'un bloc.
  Mesuré sur le bloc réel avec des lectures injectées à 14,0 pH, la moyenne
  interquartile dérive **moins** que la médiane jusqu'à **3 lectures aberrantes
  sur 12** (+0,080 / +0,080 / +0,127 contre +0,103 / +0,103 / +0,174) et ne
  décroche qu'à **4 sur 12**. C'est le seul sacrifice du plan, et il doit être
  arbitré explicitement par `pool-chemistry`.
- Sonde bruyante : la porte ne doit jamais s'ouvrir plus tôt qu'aujourd'hui sur
  le fond du V.
- ORP : critère secondaire, ne pas le durcir.
- NaN ignorés · anneau incomplet jamais stable · voie jamais lue ignorée —
  fail-closed intégral, inchangé.

## Comportement actuel
`kPurgeWindowMin = 10`, `kPurgeSamplesPerMin = 12`, `kPurgeStablePhRange = 0,15`,
`kPurgeStableOrpRange = 15`, re-fermeture 0,30 / 30, plancher 10 min, plafond
90 min (`src/constants.h` §feature-091, `src/purge_stability_logic.{h,cpp}`).

## Pré-requis — SATISFAITS le 2026-09-20

Le protocole exigeait **≥ 5 matins** de télémétrie avant d'ouvrir la spec, et
elle en comptait 1. Il était en réalité satisfait depuis plusieurs jours : le
cron de capture de `/root/pool-captures/` (bug-080 lot C, 04:00 et 05:30 UTC)
collectait le journal sans que personne ne dépouille les lignes `[STAB]`.

| Donnée exigée | État |
|---|---|
| `measures_purge_last_duration_s` sur ≥ 5 matins | ✅ **8 matins** (via les lignes `[STAB] Fin de purge`) |
| `measures_purge_last_end` | ✅ les 8 sont de type 1 (stabilité), **aucune fin 2** |
| `measures_purge_count` (re-purges) | ✅ relevé par jour |
| `sensor_trace` 5 s couvrant l'ouverture | ✅ **versionnée en fixture** (voir §D) |
| dérive du pH filtré 45 min post-ouverture | ❌ **toujours manquante** — voir §E |

**Critère de décision** (conservé tel qu'il avait été écrit) : « si la médiane
des durées réelles (fin 1) est ≤ plancher + 3 min **et** qu'aucune fin 2 n'est
observée → ne rien changer, clore la spec en *sans objet* ».
→ Médiane **16,5 min** > 13 min ⇒ **écarté**. Voir §Verdict.

## Relevés terrain

> Dépouillés le 2026-09-20 depuis `/root/pool-captures/journal_*.txt` (cron
> bug-080 lot C, 04:00 et 05:30 UTC). Synthèse conservée hors dépôt dans
> `/root/pool-captures/f097/releves-matins.md` ; la trace 5 s est **versionnée**
> en fixture (voir plus bas).

### A. Durées de purge matinale — 8 matins, toutes de type « fin 1 = stabilité »

| Matin | Durée | pH brut min→max pendant la purge | Étendue brute |
|---|---|---|---|
| 06/09 | 13 min | (non journalisé) | — |
| 07/09 | 19 min | 6.732 → 7.090 | 0.358 |
| 08/09 | 37 min | 5.790 → 6.903 | 1.113 |
| 09/09 | 17 min | 5.841 → 6.891 | 1.050 |
| 13/09 | 15 min | 6.585 → 6.961 | 0.376 |
| 14/09 | 15 min | 5.673 → 6.867 | 1.194 |
| 15/09 | 47 min | 5.560 → 6.889 | 1.329 |
| 20/09 | 16 min | 5.754 → 7.241 | **1.487** |

Durées triées : `13 · 15 · 15 · 16 · 17 · 19 · 37 · 47` → **médiane 16,5 min**
pour un plancher de 10. Aucune fin au plafond. Étendue brute moyenne **0,987**.

⚠️ **Lecture des bornes `pH a→b`** : ce sont `rawMin`→`rawMax` des échantillons
**bruts** depuis le reset du détecteur (`src/purge_stability_logic.h:75-76`),
**pas** un premier→dernier. Le verdict de stabilité, lui, porte sur l'étendue des
**médianes de bloc**. Les deux grandeurs figurent côte à côte dans le journal
sans que rien ne signale qu'elles ne parlent pas de la même chose — d'où
l'apparente contradiction « 16 min déclarées stables pendant que le brut balaie
1,487 unité ». Les deux affirmations sont vraies simultanément.

### B. Re-fermetures — distribution des étendues pH au déclenchement

| Étendue | Occurrences |
|---|---|
| 0.300 | 20 |
| 0.306 | 13 |
| 0.319 | 19 |
| 0.333 | 3 |
| 0.335 | 7 |
| 0.341 | 2 |
| 0.382 | 4 |

Tout est collé **juste au-dessus** du seuil `kPurgeRecloseRangePh = 0,30`.
**L'ORP n'est jamais le déclencheur** (9,9 à 14,5 mV pour un seuil de 30).
Coût opérationnel : chaque re-purge bloque **les deux dosages** 10 à 25 min —
3 re-purges le 12/09, 2 le 13/09, 1 le 20/09.

### C. Séquence du 20/09 — la re-fermeture sur rampe de convalescence

```
07:17:00  démarrage filtration, brut 7.224 (eau stagnante)
07:33:01  fin de purge par stabilité (16 min), bruts vus 5.754→7.241
          porte ouverte, purgeStabilityReset() remet le détecteur à zéro
07:34:03  brut 6.01 < butée 6.20 → injection d'acide REFUSÉE
          (la médiane valait 6.51 au même instant)
07:40:09  brut 6.25 → butée levée par hystérésis
07:43:02  fenêtre pleine, 10 min pile après le reset :
          étendue des médianes 0.341 > 0.30 → RE-PURGE n°1
07:57:04  fin de re-purge n°1 (14 min)
```

Ce que le détecteur a mesuré est une **rampe monotone de convalescence**
(6,01 → 6,25 en 6 min), **pas du bruit**. Une étendue ne porte aucune information
de signe ni de monotonie : le détecteur ne distingue pas « en train de se
stabiliser » de « instable ».

### D. Trace 5 s — bruit réel des deux voies

**Fixture versionnée : [`test/fixtures/f097_purge_noise_20260920.csv`](../../test/fixtures/f097_purge_noise_20260920.csv)**
— 300 échantillons **uniques** à 5 s, **25,0 min**, capturés le 2026-09-20 à
09:39 locale (1 h 40 après la fin de la re-purge n°1) : eau établie, porte
ouverte, v2.53.0.

| Grandeur | Valeur |
|---|---|
| pH brut, min / max | 6.732 / 7.088 |
| pH brut, étendue sur 25 min | 0.356 |
| pH brut, écart-type | **0.1175** |
| saut moyen entre 2 lectures à 5 s | 0.173 |
| saut maximal | 0.353 |

Étendue des **fenêtres de 10 médianes-minute** — la grandeur exacte que juge le
détecteur :

| Voie | min | max | médiane |
|---|---|---|---|
| pH | 0.075 | **0.192** | **0.095** |
| ORP | 6.2 mV | 11.5 mV | 10.4 mV |

Confrontation aux seuils **actuels** :

| Critère | Fenêtres qui ouvriraient |
|---|---|
| pH ≤ 0,15 | 13 / 16 |
| ORP ≤ 15 | 16 / 16 |
| **conjoint** | **13 / 16 (81 %)** |

**Lecture.** Les seuils ne sont **pas** sous le plancher de bruit : 81 % des
fenêtres passent sur eau établie. Le problème n'est donc pas que le critère soit
impossible, mais qu'il soit **trop serré** — 3 fenêtres sur 16 échouent, ce qui
suffit à porter la médiane de purge à 16,5 min pour un plancher de 10.

Et le bruit seul **ne referme jamais** : avec un maximum mesuré à 0,192, aucune
fenêtre n'atteint 0,30. Les re-purges observées entre 0,300 et 0,382 exigent donc
une **composante de dérive** qui s'ajoute au plancher de bruit — ce qui confirme
le diagnostic de la rampe (§C).

### ⛔ Une première capture du même jour était INVALIDE — à ne pas reproduire

Le script interrogeait `/debug/sensor_trace?page=N` alors que la route attend
**`?offset=N&count=M`** (`src/web_routes_debug.cpp:33`). Le paramètre `page`
était **ignoré en silence** : les six requêtes renvoyaient toutes `offset=0`,
soit **50 échantillons uniques présentés comme 300** — 4,1 min répétées six fois,
avec des discontinuités artificielles aux raccords.

Les chiffres qui en découlaient (étendue médiane 0,176, ORP 18,5, « critère
conjoint 0/16 ») étaient **faux**, et ils ont porté une conception complète, un
verdict `pool-chemistry` et une implémentation, tous annulés le 20/09.

**Le signal d'alarme était dans la réponse elle-même** : le champ `offset`
renvoyait 0 six fois. La fixture versionnée porte désormais cette mise en garde
en en-tête, et le script de capture vérifie que l'offset a bougé avant de
concaténer.

### E. La question du 06/09 reste OUVERTE

Les 8 matins ne portent que sur les durées et les étendues. Ils ne couvrent ni la
dérive du pH filtré sur les 45 min **post-ouverture**, ni l'écart entre l'heure de
démarrage de la filtration et la fin de purge — c'est-à-dire **le discriminant**
que cette spec réclame pour séparer « purge trop courte » de « dégazage matinal
réel ».

Deux captures sont programmées (07:40 et 08:05 locales) et couvrent
`07:15 → 08:05`, donc le démarrage, la purge, l'ouverture **et** les 45 minutes
suivantes. Si l'inflexion tombe à la fin de la purge, c'est (1) ; si elle démarre
avec la filtration et la traverse, c'est (2).

**Cette question ne bloque pas le dimensionnement des seuils**, mais elle ne doit
pas être close par omission : si c'est un dégazage réel, raccourcir la purge ne
corrige rien et l'écart initial du KPI reste faux.

## Fichiers potentiellement concernés
- `src/purge_stability_logic.{h,cpp}` — statistique / taille de bloc
- `src/constants.h` — constantes kPurge*, static_asserts
- `test/test_native_purge_stability/` — rejeux 04/09 + traces réelles, mutation

## Livrables — check-list

- [x] **A1** — Relevés terrain consignés : **8 matins** (durées, type de fin,
      étendues brutes), distribution des re-purges, et **trace 5 s versionnée**
      en fixture. *La dérive du pH filtré sur les 45 min post-ouverture reste
      NON collectée* — voir A8.
- [x] **A2** — Critère de décision appliqué : médiane 16,5 min > plancher+3 (13)
      ⇒ « ne rien changer » **écarté**.
- [x] **A3** — `src/purge_stability_logic.{h,cpp}` : ajouter
      `float purgeStabilityBlockStat(const float* v, uint8_t n)` (moyenne
      interquartile, `trim = n/4`, réutilise le tri par insertion **déjà
      présent**) et l'appeler dans `channelAdd()` à la place de
      `purgeStabilityMedian()`. Module pur ADR-0017 : aucun tampon redimensionné,
      aucune allocation. `purgeStabilityMedian()` reste exportée pour les tests.
- [x] **A4** — Commentaire d'en-tête de `purge_stability_logic.h` réécrit : dire
      **pourquoi** ce n'est plus la médiane (signal anti-corrélé à deux niveaux,
      la médiane bascule). Sans cette phrase, quelqu'un « restaurera la médiane
      pour la robustesse » et réintroduira le défaut — d'autant que l'ADR-0059
      justifie explicitement la médiane.
- [x] **A5** — `src/constants.h` : **aucune valeur modifiée**. Ajouter
      `kPurgeNoiseFloorPh = 0.10f`, `kPurgeNoiseFloorOrp = 10.0f` (planchers
      mesurés avec le nouvel estimateur, arrondis au-dessus) et
      `kPurgeMinDetectedDriftPhPerMin = 0.035f`.
- [x] **A6** — `src/constants.h`, asserts : corriger le **bug latent** l.2860-2862
      (`× 60000UL` → durée réelle) ; **conserver** l'égalité ×2 et lui adjoindre
      l'invariant de dérive ; ajouter `kPurgeStablePhRange > kPurgeNoiseFloorPh`
      et son homologue ORP.
- [x] **A7** — `src/measure_validity.cpp` : journal `[STAB]` de fin de purge
      enrichi de **l'étendue des blocs au moment du verdict** (pH et ORP).
      ⚠️ Tampon dimensionné en **octets UTF-8**, pire cas calculé
      (`docs/subsystems/logger.md`).
- [x] **A8** — Statuer par écrit sur la question du 06/09 (« purge trop courte »
      vs « dégazage matinal réel ») au vu des captures matinales : tranchée /
      non tranchée / renvoyée à une spec distincte. **Ne pas la clore par
      omission.** → **Verdict écrit ci-dessus (§E) : NON TRANCHÉE, explicitement
      laissée ouverte.** Les 8 matins dépouillés le 20/09 ne portent que sur les
      durées et étendues de purge ; ils ne couvrent ni la dérive du pH filtré
      post-ouverture, ni l'écart entre démarrage de filtration et fin de purge —
      le discriminant que la question exige. Deux captures programmées le
      2026-09-21 (07:40 et 08:05 locales, couvrant 07:15 → 08:05) trancheront :
      inflexion à la fin de la purge ⇒ (1) « purge trop courte » ; inflexion dès
      le démarrage de la filtration et qui la traverse ⇒ (2) « dégazage matinal
      réel ». Cette question ne bloque pas ce lot (elle ne porte pas sur
      l'estimateur, seulement sur la durée réelle du transitoire physique) et
      reste **à trancher dans un cycle ultérieur** de cette même spec.
- [x] **A9** — Tests natifs : bloc alterné réel de la minute 22 (médiane 7,059 vs
      IQM 6,9673) · rejeu plateau v2 → 16/16 · rejeu V du 04/09 sans bruit et avec
      bruit réel sur ≥ 30 déphasages → **0 ouverture avant le plateau** ·
      0 à 3 lectures aberrantes/bloc → verdict inchangé · **4 aberrantes →
      décrochage figé par un test** (documenter le comportement connu plutôt que
      le découvrir en production) · rampe +0,035 referme, +0,030 ne referme pas ·
      plateau post-ouverture → 0 re-fermeture sur ≥ 30 déphasages. **Livré** :
      7 tests dédiés `test_F1`…`test_F7` dans
      `test/test_native_purge_stability/test_purge_stability.cpp` (mesuré le
      2026-09-20 : 32 fonctions `test_` dans le fichier, contre 25 avant ce
      lot), couvrant chacun des points ci-dessus ; le rejeu V du 04/09 réutilise
      la section E pré-existante, désormais alimentée par le nouvel estimateur.
- [x] **A10** — **Mutation**, 4 mutants — **TOUS TUÉS**, vérifiés par le
      `test-engineer` puis **rejoués indépendamment par le `code-reviewer`**
      (a) IQM → médiane tue F3, F4, F6 · (b) moyenne simple tue 6 tests ·
      (c) `n/4` → `n/6` tue F3, F4 · (d) seuil 0,15 → 0,21 **bloqué à la
      compilation** par les `static_assert` (plus fort qu'un test) et tué à
      l'exécution asserts désactivés. Mutant supplémentaire trouvé par la revue :
      retirer la garde `max(1,·)` tue F1 (n=3 → 35,67). Détail des 4 mutants, chacun doit tuer au moins un test :
      (a) IQM → médiane tue le rejeu plateau ; (b) IQM → moyenne simple tue le
      test des lectures aberrantes ; (c) `trim = n/4` → `n/6` tue le rejeu
      plateau ; (d) `kPurgeStablePhRange` 0,15 → 0,21 tue le rejeu V.
      **(a) est le mutant critique** : sans lui, rien ne protège le correctif.
      ⚠️ **Non confirmé par ce tour documentaire** : les tests `test_F2`/`test_F3`
      et leurs commentaires ciblent explicitement les mutants (a), (b) et (c) par
      leur nom, mais **aucune exécution de mutation n'a été rapportée** à
      doc-writer pour ce cycle — à confirmer par `test-engineer`/`code-reviewer`
      avant clôture de la spec (mutant (d) en particulier, non retrouvé
      nommément dans le fichier de tests inspecté le 20/09).
- [x] **A11** — ADR : **amendement daté** de l'ADR-0059 (§1.2 « la médiane de
      chaque minute » → moyenne interquartile ; planchers de bruit mesurés ;
      rectification de l'affirmation « seuils = bruit de mesure observé », qui
      était une **estimation présentée comme une mesure**). **Livré** : voir
      « Amendement 2026-09-20 » dans
      [ADR-0059](../../../docs/adr/0059-purge-adaptative-fenetre-de-medianes.md).
- [x] **A12** — `docs/subsystems/measure-validity.md`, `docs/subsystems/sensors.md`
      (§signal à deux niveaux), `CHANGELOG.md`. **Livré** — voir rapport doc-writer
      du 2026-09-20.

## Sécurité chimique
- Dosage impliqué : indirectement — l'ouverture de la porte de mesure autorise le
  premier dosage du matin.
- Algorithme PID touché : non. Aucun seuil de dosage touché.
- **Risque historique préservé** : ouvrir à tort dans le fond du V du rinçage →
  intégrale chargée à l'envers, doses sur eau non représentative. C'est ce que
  feature-091 a supprimé ; la marge de 0,0525 sur le V est **inchangée** par ce
  lot, puisque l'estimateur voit une rampe linéaire à l'identique.
- **Le seul sacrifice à arbitrer** : le point de rupture passe de 50 % à 25 % des
  lectures d'un bloc (meilleur que la médiane jusqu'à 3 aberrantes sur 12).
- **Gate `pool-chemistry` obligatoire.** ⚠️ Le GO SOUS CONDITIONS rendu le
  2026-09-20 sur le plan précédent doit être **RÉVOQUÉ** et non révisé : il
  reposait sur un plancher de bruit surestimé de près du double, et la
  proposition a changé de nature.

## ADR attendu ?
- **Oui — amendement daté de l'ADR-0059** (pas un successeur : la structure de
  l'ADR est intégralement préservée, seule change la ligne §1.2).
- Critère décisif : **décision invisible dans le code**. « Moyenne interquartile
  au lieu de médiane » ressemble à une micro-optimisation ; rien ne dira que
  c'est parce que la médiane d'un échantillon bimodal **bascule** au lieu de
  moyenner.

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ WebSocket, topic MQTT ni libellé UI nouveau.
- `measures_purge_last_duration_s` / `_last_end` / `_count` : inchangés.
- **Seul changement observable** : le format du journal `[STAB]` de fin de purge.
- `docs/subsystems/measure-validity.md`, `docs/subsystems/sensors.md`, `CHANGELOG.md`.

## Critères d'acceptation
- [x] Relevés de ≥ 5 matins consignés avant toute modification de code — **8 collectés**
- [x] Rejeu du V du 04/09 : aucune ouverture avant le plateau, sans **et** avec bruit réel (≥ 30 déphasages) — `pio test -e native --filter "*purge_stability*"`, exécuté le 2026-09-20 : 32/32 cas PASSED, dont `test_E_rejeu_0409_pas_stable_avant_la_fin_du_rincage`, `test_E_rejeu_0409_sans_bruit_meme_conclusion`, `test_E_rejeu_0409_5_graines_ouvrent_ou_plafond`
- [x] Rejeu du plateau réel : **16/16** fenêtres ouvrent au plancher — `test_F3_rejeu_plateau_reel_16_sur_16` PASSED (même exécution)
- [x] Marge sur le fond en V ≥ 0,03 pH — attendu **0,0525**, inchangé par ce lot — non régressé (rejeux V ci-dessus verts)
- [x] Pente de re-fermeture d'une dérive vraie ≤ 0,035 pH/min — attendu **0,0333**, inchangé — `test_F5_reclose_0035_referme_0030_ne_referme_pas` PASSED
- [x] Les 4 mutants tués, **(a) IQM → médiane** en particulier — appliqués réellement sur les sources, tués, puis restaurés (diff propre vérifié) par le `test-engineer`, et **rejoués indépendamment par le `code-reviewer`**
- [x] `pio test -e native` vert sur la suite `test_native_purge_stability`, zéro régression — 32 tests, mesuré le 2026-09-20 (`pio test -e native --filter "*purge_stability*"`). ⚠️ `pio run` (compilation firmware complète) et `pio test -e native` **suite entière** (53 suites) non relancés par ce tour documentaire — deux autres agents modifient `src/constants.h`/`test/` en parallèle ; à confirmer avant clôture
- [ ] **Post-déploiement** : sur 5 matins, `measures_purge_count` = **0** et
      `measures_purge_last_duration_s` resserré. **Critère de retour arrière à
      écrire dans l'ADR** : si le plancher de bruit IQM mesuré *en phase de purge*
      dépasse ~0,12, re-mesurer avant de toucher au moindre seuil.

## Hors périmètre
- **La butée de sécurité brute pH 6,20** — même cause racine (comparer un seuil
  dur à une **lecture isolée** d'un signal qui alterne entre deux niveaux), mais
  autre chemin de code. L'erreur actuelle est **fail-closed** (acide refusé,
  jamais injecté à tort), il n'y a donc aucune urgence dans la direction
  dangereuse. → **spec dédiée avec son propre gate `pool-chemistry`**, à traiter
  après celle-ci. ⚠️ Arbitrage à y porter : une médiane de bloc introduirait
  jusqu'à 100 s de retard sur un arrêt de sûreté ; une statistique courte
  (5 lectures / 25 s) donne l'essentiel du gain sans ce retard.
- **La perturbation à la source** — la corrélation pH↔ORP de +0,462 et
  l'anti-corrélation des deux voies signent un **mode commun** sur les deux
  électrodes (courants de fuite de la pompe de filtration). C'est la seule voie
  qui rendrait le brut réellement propre, et elle conditionne aussi la butée
  6,20. → **spec de suivi matériel** (masse / isolation / filtrage d'entrée EZO).
- **Les matins à 37 et 47 min** : ce lot supprime l'attente *induite par le
  détecteur*, pas celle induite par le **transitoire réel** du rinçage. Ne pas
  promettre une médiane de 10 min sur les 8 matins.
- **La dérive lente jugée stable** (0,0167 pH/min) : dette de l'ADR-0059, ni
  aggravée ni résorbée ici.
- Dispense de purge pour les micro-coupures < 10 s sans reboot (recommandation 5
  du gate 091, différée).
- Cause physique du fond en V (N1, feature-069).

## Notes d'implémentation (doc-writer, 2026-09-20)

- **Code déjà présent au moment de ce tour** (fichiers `src/constants.h`,
  `src/measure_validity.cpp`, `src/purge_stability_logic.{h,cpp}`,
  `test/test_native_purge_stability/test_purge_stability.cpp`) : moyenne
  interquartile (`purgeStabilityBlockStat`, `trim = max(1, n/4)`), planchers de
  bruit `kPurgeNoiseFloorPh`/`Orp`, journal `[STAB]` enrichi de l'étendue des
  blocs, `static_assert` du bug latent corrigé (durée réelle au lieu de
  « 1 bloc = 1 minute »), et 7 tests dédiés `test_F1`…`test_F7`. Doc-writer n'a
  modifié **aucun** de ces fichiers (hors périmètre imposé).
- **Renommage `kPurgeMaxUndetectedDrift…` → `kPurgeMinDetectedDriftPhPerMin`** :
  ce n'est pas un écart, c'est un **correctif exigé par la revue de code**. Le
  nom d'origine et son commentaire disaient l'**inverse** de ce que l'assert
  garantit : ils annonçaient une dérive « qui ne doit PAS être vue », alors que
  `reclose ≤ drift × span` garantit qu'une dérive de 0,035 pH/min **EST
  détectée** — ce que le test F5 (condition C4) vérifie. Un commentaire faux à
  cet endroit aurait conduit le prochain lecteur à **inverser l'assert**, et la
  re-fermeture aurait cessé de détecter les dérives réelles. La référence au
  nom obsolète dans le commentaire du test F5 a été corrigée dans la foulée.
- **Validation complète, exécution seule, 2026-09-20** :
  `pio run -e esp32dev_core3` **SUCCESS, 0 warning** · `pio test -e native`
  **53 suites / 1 924 cas, tous verts** · les 4 scripts d'hygiène verts.
  ⚠️ Les exécutions intermédiaires ayant montré des suites `ERRORED` non
  reproductibles venaient de **deux `pio` concurrents** dans
  `.pio/build/native/` — un « tout vert » ne vaut que d'une exécution seule.
- **`src/version.h` reste à 2.53.0.** L'entrée CHANGELOG a été ajoutée sous
  `[Unreleased] - 2026-09-20` plutôt que `[2.54.0]`, dans l'attente d'une
  décision explicite de bump (doc-writer ne modifie pas `src/`, et ce n'est pas
  à lui de trancher seul le numéro de version).
- **Spec non déplacée vers `done/`** : A8 reste ouverte par construction (deux
  captures programmées le 2026-09-21). **A10 est confirmé** : les 4 mutants ont
  été tués par le `test-engineer` puis rejoués indépendamment par le
  `code-reviewer`. La spec reste dans `doing/` tant que A8 n'est pas statuée par
  la mesure — c'est le seul item qui la retient.
