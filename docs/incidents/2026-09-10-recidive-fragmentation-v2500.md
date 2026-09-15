# Récidive de fragmentation — v2.50.0, 10/09/2026

Note factuelle **préalable à tout codage**, conforme à la phase 1 demandée.
Séparation stricte : certain / fortement probable / à mesurer.

## Chronologie reconstituée

L'`uptime` du post-mortem est stocké **modulo 65 536 s**. Le journal donne
`uptime=38404 s mod 65536` au reset.

| Instant | Événement | Source |
|---|---|---|
| **09/09 ~08:14** | Démarrage v2.50.0 (`38 404 + 65 536 = 103 940 s` avant le reset) | calcul sur l'uptime modulo |
| 09/09 ~08:16 | **OOM à uptime = 116 s**, 1 612 o `caps=0x080C`, `maxBloc=1 536` | post-mortem NVS |
| 09/09 08:23 | Test de charge volontaire (rate-limit à 122 req/min) | journal du 09/09 |
| 10/09 07:54 → 12:55 | **Plateau : `maxBloc8 = 9 716` CONSTANT, `free8 ≈ 48 Ko`** | 11 relevés |
| 10/09 09:58:47 | Sauvegarde d'historique **sautée**, `maxBloc8 = 6 644` | journal |
| 10/09 11:58:56 | Sauvegarde d'historique **sautée**, `maxBloc8 = 7 668` | journal |
| 10/09 ~13:12 | **`maxBloc8 = 7 872` pendant ≥ 78 s**, `free8 ≈ 48 Ko`, `ws=0` | cinétique pré-reset, 32 échantillons |
| **10/09 13:13:47** | **SW_RESET** (`reset_reason=3`) — redémarrage manuel par cURL | journal |

Durée de vie du firmware avant récidive : **28,9 h**.

## ✅ CERTAIN — établi par le code ET le journal

1. **Le message est bien le 503 de la garde mémoire.** `src/web_server.cpp:143`
   renvoie littéralement `Memoire insuffisante, reessayez`, avec `Retry-After: 5`.
2. **La bande morte 4 096 – 8 191 existe.** `kStaticFileMinAllocBytes = 8192`
   (`constants.h:350`) refuse les fichiers statiques ; `kRestartMaxAllocHeapBytes
   = 4096` (`constants.h:200`) déclenche l'auto-guérison. Entre les deux :
   régulation active, interface web refusée, **aucun redémarrage automatique**.
   L'appareil y a séjourné à **7 872 o** pendant au moins 78 s.
3. **Ce n'est pas un épuisement, c'est une fragmentation.** `free8 ≈ 48 Ko`
   stable pendant tout l'épisode, PSRAM à 4,18 Mo libres. Le total libre ne
   diminue pas : **aucune fuite**.
4. **Le reset est manuel.** `SW_RESET, reset_reason=3` — ni panic, ni
   auto-guérison (qui n'aurait pu se déclencher qu'à moins de 4 096).
5. **Aucun client WebSocket au moment de l'incident** : `ws=0` sur les 32
   échantillons de la cinétique.
6. **Le post-mortem n'est PAS celui du 13:13.** Il porte `uptime = 116 s`, contre
   ~103 940 s au reset. Son profil de tas (`free8=58880 maxB8=27584`) est celui
   d'un démarrage frais. Il provient d'un boot de la fenêtre de déploiement du
   09/09 ; l'attribuer à l'incident serait faux.
7. **Deux sauvegardes d'historique ont creusé le tas** à 6 644 o et 7 668 o,
   **après** construction du JSON. Vérifié dans `src/history.cpp` :
   `JsonDocument doc` puis `measureJson(doc)` **précèdent** le second
   `fsGuardAdmit()`. Le pic est donc payé avant d'être contrôlé.

### ✅ CERTAIN — le défaut de télémétrie `fs_guard`, pire que soupçonné

`fsGuardAdmit()` pose `sFopenEpisode.active = true` (`fs_guard.cpp:90`) au moment
du refus. `fopenGuardDrain()` n'initialise `episodeStartMs` et `deferralsAtStart`
que sous `if (!st.active)` (`fopen_guard_logic.cpp:44`).

**Conséquence : la branche est structurellement inatteignable.** Donc :

- `FopenGuardAction::EpisodeStart` n'est **jamais** retourné — **zéro occurrence**
  de « Ouvertures de fichiers refusées » dans le journal terrain ;
- `episodeStartMs` reste à **0** ⇒ la durée rapportée est `millis()` **depuis le
  boot** ;
- `deferralsAtStart` reste à **0** ⇒ le nombre rapporté est le **cumul** depuis le
  boot.

Vérification arithmétique sur le terrain :

| Ligne journalisée | Durée annoncée | Uptime réel à cet instant |
|---|---|---|
| « 1 refus en **92 279** s » (09:59) | 25,6 h | 25,6 h |
| « 2 refus en **99 488** s » (11:59) | 27,6 h | 27,6 h |
| « 3 refus en **102 792** s » (12:54) | 28,6 h | 28,6 h |

Les trois durées **égalent l'uptime**, et les compteurs 1→2→3 sont cumulatifs.
Aucun épisode n'est mesuré.

## 🔶 FORTEMENT PROBABLE — cohérent, non démontré

1. **La dégradation est TEMPORELLE, pas liée à la charge.** À l'incident : `ws=0`,
   `skip=0`, pas de rafale en cours, RSSI −37 dBm. Le plateau s'est installé sur
   ~29 h sans événement déclencheur identifiable.
2. **Le plateau est structurel.** `maxBloc8 = 9 716` est **exactement constant**
   sur 11 relevés couvrant 5 h, puis **exactement 7 872** sur 32 échantillons.
   Des valeurs aussi stables suggèrent une frontière de découpage du tas, pas une
   fluctuation — même phénomène que les 2 932 o reproductibles du 08/09.
3. **Le poste de dépense est ailleurs que dans les rafales HTTP.** Le contexte au
   creux du plancher `m804` bascule de `0x02` (rafale HTTP) à `0x01` (clients WS)
   après la première sauvegarde d'historique ratée.

## ❓ À MESURER — aucune conclusion permise

1. **Ce qui immobilise les blocs pendant 29 h.** Aucun candidat n'est démontré.
   Interdiction expresse de toucher aux constantes ou structures du `logger`
   sans preuve instrumentée.
2. **Pourquoi 9 716 puis 7 872**, et pourquoi ces valeurs sont si stables.
3. **La part respective** de la sauvegarde d'historique et du service HTTP dans
   l'installation du plateau.
4. **L'origine exacte** du post-mortem à 116 s (quel boot de la fenêtre de
   déploiement).

## ⚠️ CE QUI INVALIDE MES CONCLUSIONS DU 09/09

`feature-125` posait **trois conditions de réouverture** pour S2. **Aucune n'est
remplie** par cette récidive :

| Condition écrite le 09/09 | État au 10/09 |
|---|---|
| « plus de connexions simultanées » | `ws = 0`, aucune rafale |
| « un firmware plus bavard » | inchangé depuis le 09/09 |
| « un réseau plus lent » | RSSI −37 dBm, excellent |

**Les conditions étaient donc mal posées** : elles décrivaient une aggravation par
la **charge**, alors que la récidive s'installe par la **durée**. Une spec de
réserve dont les conditions ne se déclenchent pas au moment où le problème
revient ne protège de rien.

**Et la validation de S7 était insuffisante.** Le plancher de 8 692 o mesuré le
09/09 se situait à **500 o** au-dessus de la garde à 8 192 — sur un **tas frais**,
après moins d'une heure de fonctionnement. Ce test ne pouvait pas voir un plateau
qui met 29 h à s'installer.

⇒ **S7 n'est pas invalidé** — il a déplacé les structures lwIP et le facteur 8 est
réel — **mais il ne traite pas la dégradation temporelle**, qui est un phénomène
distinct et non instruit.

## Conséquences documentaires (phase 1, item 4)

- `bug-080` et `feature-124` ne doivent plus présenter le sujet comme résolu.
- `feature-125` doit être **rouverte**, et ses conditions **réécrites** autour de
  la durée, pas de la charge.
- Le protocole de validation doit exiger une mesure **après 24–48 h d'uptime**,
  jamais sur tas frais.

## Mise à jour — correctif confirmé (v2.51.0, lot B, 12/09/2026)

### ✅ CERTAIN — le défaut de télémétrie `fs_guard` est corrigé

La section « ✅ CERTAIN — le défaut de télémétrie `fs_guard`, pire que soupçonné »
ci-dessus décrivait une branche `fopenGuardDrain()` **structurellement
inatteignable** (`if (!st.active)` toujours faux au moment du drain, puisque
`fsGuardAdmit()` avait déjà posé `active` au point de décision). C'est
**confirmé** — la lecture de code de ce diagnostic était exacte — et c'est
désormais **corrigé** : `FopenGuardEpisode` porte deux drapeaux distincts,
`active` (cache du refus, écrit par `fsGuardAdmit()`) et `reported` (neuf,
propriété exclusive du drain), et le front montant de l'épisode est extrait
dans une fonction pure dédiée, `fopenGuardNoteRefusal()`. Détail complet :
[`docs/subsystems/acces-fichiers.md`](../subsystems/acces-fichiers.md#-correctif-v2510-lot-b1--episodestart-était-structurellement-inatteignable).

**Les trois durées de la table ci-dessus étaient des artefacts, pas des
mesures.** « 1 refus en 92 279 s », « 2 refus en 99 488 s » et « 3 refus en
102 792 s » ne mesuraient **rien de l'épisode** : `episodeStartMs` restant
figé à 0 par le défaut, ces trois nombres étaient l'**uptime** au moment de
chaque drain — ce que la table « Vérification arithmétique sur le terrain »
montrait déjà en les comparant à l'uptime réel, sans encore disposer du
correctif pour l'affirmer de façon définitive. Il en va de même pour les
compteurs « 1 », « 2 », « 3 » : cumulatifs depuis le boot, pas propres à un
épisode.

### 🔶 CE QUE CE CORRECTIF NE TRANCHE PAS

Réparer la télémétrie ne dit rien sur les questions encore ouvertes de ce
document — la nature du plateau à 7 872 o, ce qui immobilise les blocs pendant
29 h, la part respective de l'historique et du service HTTP. Le lot B ne
portait que sur l'observabilité du refus `fopen` et sur deux compteurs de
fragmentation déjà calculés par `heap_caps_get_info()` (`fb`/`ab`, voir
[`docs/subsystems/system-health.md`](../subsystems/system-health.md#planchers-glissants-multi-masques-v2490-feature-122-lot-c-1a)) :
un futur épisode de refus `fopen` sera enfin **journalisé avec une durée et un
compte justes**, ce qui pourra aider à instruire ces questions — mais rien
n'a encore été mesuré sur cible avec le correctif actif.

---

## Mise à jour — LA MESURE (fenêtre v2.51.0, 12-13/09/2026)

**La question centrale de ce document est tranchée.** La fenêtre d'observation
ouverte le 12/09 à ~13:19 UTC a produit 28 relevés exploitables entre 6,7 h et
29,2 h d'uptime, avec les compteurs `fb`/`ab` que le lot B a rendus publics.

Relevés par `tools/trace_fragmentation.py` sur `/root/pool-captures`.

### ✅ CERTAIN — la fragmentation, oui. 🔶 L'absence de fuite, NON

| | 6,7 h | 29,2 h | écart |
|---|---:|---:|---|
| blocs alloués | 883 | 907 | **+3 %** |
| trous libres | 77 | 128 | **+66 %** |
| plus gros bloc | 23 540 | 7 924 | **−66 %** |
| libre | 55 468 | 45 484 | −18 % |

⚠️ **CORRECTION du 13/09 au soir.** La premiere redaction de cette section
concluait « les blocs alloues sont plats, donc pas de fuite ». **Ce raisonnement
est trop court et la conclusion depassait la mesure.** Ce qui suit le remplace.

**Ce qui est plat, c'est le NOMBRE de blocs**, pas les octets alloues. Le compte
oscille entre 883 et 988 et revient a 908, avec **10 baisses sur 28 intervalles** :
cela ecarte une fuite en *nombre d'allocations*, et rien de plus.

**Mais le libre a perdu ~10 400 octets qui ne sont jamais revenus** (55 468 ->
45 072 ; le maximum de la serie ne retrouve jamais sa valeur initiale). A nombre
de blocs quasi identique, cela signifie que les blocs alloues pesent en moyenne
**plus lourd** qu'au depart. Deux explications restent indiscernables :

- un **jeu de travail plus gros** (tampons legitimement plus grands, conserves) ;
- une **fuite en octets** plutot qu'en nombre d'allocations.

**Pourquoi on ne peut pas trancher** : `total_allocated_bytes` **n'est pas publie**.
Les champs exposes par masque sont `f`, `b`, `mf`, `mb`, `wf`, `wb`, `cf`, `cb`,
`n`, `fb`, `ab`. `heap_caps_get_info()` calcule pourtant les octets alloues — et
comme `fb`/`ab` avant le lot B, **cette valeur est calculee puis jetee**. C'est
exactement le meme defaut de telemetrie, un champ plus loin.

⚠️ **A FAIRE avant de reparler de fuite** : publier `total_allocated_bytes` par
masque. Sans lui, toute affirmation sur l'absence de fuite est une opinion.

**Un point rassure neanmoins** : la **PSRAM est plate** a **-320 octets sur
22,5 h**. Si fuite il y a, elle n'est pas la.

Ce qui est en revanche **etabli**, c'est que la *forme* du tas se degrade
independamment de son remplissage — voir la section CLIQUET ci-dessous, qui est
la preuve la plus forte du document.

C'est exactement la grandeur que les deux hypothèses concurrentes du 10/09 ne
permettaient pas de départager, et que `free_blocks` rendait enfin observable.

### ✅ CERTAIN — le plateau est un ESCALIER, pas une érosion

```
 6,7 h → 20,7 h   bloc = 23 540   CONSTANT pendant 14 heures
21,2 h            bloc = 16 372   −7 168
22,2 h            bloc = 14 836   −1 536
23,2 h            bloc = 10 228   −4 608
28,2 h            bloc =  7 924   −2 304
```

Quatorze heures de stabilité rigoureuse, puis quatre marches en sept heures.
**C'est la densification de l'échantillonnage à 30 min qui l'a rendu visible** :
au pas de 2 h on voyait l'avant et l'après, jamais le passage.

Les trois dernières marches sont dans un rapport exact : 4 608 = 2 × 2 304, et
1 536 = 2 304 × 2/3. Ces valeurs sont un indice à instruire, pas une conclusion.

### ✅ CERTAIN — le phénomène est un CLIQUET, pas une pénurie

C'est l'observation la plus contre-intuitive de la série, et elle exclut toute
explication par le manque de mémoire :

| | 25,7 h | 27,7 h |
|---|---:|---:|
| libre | 36 240 | **46 640** (+29 %) |
| plus gros bloc | 10 228 | **10 228** (inchangé) |

**La mémoire libre remonte de 10 400 octets sans que le plus gros bloc récupère
un seul octet.** Puis il descend encore, à 7 924. Le libre fait des allers-retours ;
le plus gros bloc ne fait que descendre. Il n'y a donc pas de coalescence des
trous adjacents qui reconstituerait le grand bloc — une fois découpé, il le reste.

### ✅ CERTAIN — deux régimes, pas un

| phase | trous | plus gros bloc |
|---|---|---|
| 6,7 h → 20,7 h | **77 → 117 (+52 %)** | intact |
| 21,2 h → 29,2 h | 122 → 128 (+5 %) | **s'effondre de 66 %** |

Pendant les 14 premières heures, le tas se crible de petits trous **sans toucher
au grand bloc contigu**. Ensuite le grand bloc s'effondre **pendant que le nombre
de trous cesse de croître**. Ce sont deux mécanismes distincts, et seul le second
produit le plateau. Chercher une cause unique serait une erreur.

### ✅ CERTAIN — le plateau est REPRODUCTIBLE d'une version à l'autre

| version | valeur du plateau | uptime |
|---|---:|---:|
| v2.50.0 (10/09) | 7 872 o | 28,9 h |
| v2.51.0 (13/09) | **7 924 o** | **28,2 h** |

Même ordre de grandeur, même moment. Le lot B n'a rien changé au phénomène — il
n'y prétendait pas — mais il confirme que le plateau n'est pas un accident de
mesure : c'est un état d'équilibre que le système atteint.

### ✅ CERTAIN — le coût opérationnel, chiffré sur la journée du 13/09

- **138 suspensions** d'ouverture de fichiers (`fs_guard`), journalisées avec la
  durée et le compte justes grâce au lot B.
- **14 sauvegardes d'historique sautées** (`maxBloc8 < 8 192`).
- Plus gros bloc descendu à **12 octets** à un instant, avant 16:52 locale
  (`minBoot=12`, relevé dans le journal).
- L'export du journal de 20:29 locale annonce lui-même : *« Historique persistant
  INDISPONIBLE — mémoire insuffisante pour l'ouvrir : export TRONQUÉ à la session
  courante »*. **L'appareil ne peut plus relire son propre historique.**

Autrement dit, la perte de donnée n'est plus une hypothèse : elle est mesurée.

### La série complète

| capture (UTC) | h | libre | plus gros bloc | trous | alloués | trou moyen hors gros bloc |
|---|---:|---:|---:|---:|---:|---:|
| 20260912_2000 | 6.7 | 55468 | **23540** | 77 | 883 | 420 o |
| 20260912_2200 | 8.7 | 54784 | **23540** | 89 | 884 | 355 o |
| 20260913_0000 | 10.7 | 53940 | **23540** | 96 | 884 | 320 o |
| 20260913_0200 | 12.7 | 53336 | **23540** | 99 | 885 | 304 o |
| 20260913_0400 | 14.7 | 52840 | **23540** | 108 | 884 | 274 o |
| 20260913_0530 | 16.2 | 51820 | **23540** | 115 | 888 | 248 o |
| 20260913_0600 | 16.7 | 51416 | **23540** | 118 | 887 | 238 o |
| 20260913_0800 | 18.7 | 50844 | **23540** | 120 | 886 | 229 o |
| 20260913_0900 | 19.7 | 50812 | **23540** | 117 | 886 | 235 o |
| 20260913_0930 | 20.2 | 50272 | **23540** | 116 | 892 | 232 o |
| 20260913_1000 | 20.7 | 50716 | **23540** | 117 | 886 | 234 o |
| 20260913_1030 | 21.2 | 48316 | **16372** | 122 | 899 | 264 o |
| 20260913_1100 | 21.7 | 44396 | **16372** | 124 | 911 | 228 o |
| 20260913_1130 | 22.2 | 49780 | **14836** | 120 | 888 | 294 o |
| 20260913_1200 | 22.7 | 49708 | **14836** | 123 | 887 | 286 o |
| 20260913_1230 | 23.2 | 44376 | **10228** | 119 | 917 | 289 o |
| 20260913_1300 | 23.7 | 43232 | **10228** | 119 | 932 | 280 o |
| 20260913_1330 | 24.2 | 41076 | **10228** | 123 | 949 | 253 o |
| 20260913_1400 | 24.7 | 39060 | **10228** | 123 | 966 | 236 o |
| 20260913_1430 | 25.2 | 37392 | **10228** | 122 | 980 | 224 o |
| 20260913_1500 | 25.7 | 36240 | **10228** | 129 | 988 | 203 o |
| 20260913_1530 | 26.2 | 37452 | **10228** | 133 | 983 | 206 o |
| 20260913_1600 | 26.7 | 37280 | **10228** | 136 | 984 | 200 o |
| 20260913_1630 | 27.2 | 43660 | **10228** | 116 | 929 | 291 o |
| 20260913_1700 | 27.7 | 46640 | **10228** | 131 | 895 | 280 o |
| 20260913_1730 | 28.2 | 46732 | **7924** | 130 | 900 | 301 o |
| 20260913_1800 | 28.7 | 44168 | **7924** | 126 | 913 | 290 o |
| 20260913_1830 | 29.2 | 45484 | **7924** | 128 | 907 | 296 o |

### ❓ CE QUE CETTE MESURE NE TRANCHE TOUJOURS PAS

1. **Ce qui découpe le grand bloc** entre 20,7 h et 28,2 h. La mesure dit *quand*
   et *de combien*, pas *qui*. Les tailles de marche (7 168 / 1 536 / 4 608 /
   2 304) sont la piste la plus concrète dont dispose ce document.
2. **Pourquoi les 14 premières heures épargnent le grand bloc** alors que les
   trous s'y multiplient déjà.
3. **Ce qui se passe avant 6,7 h** : la fenêtre n'a aucun point plus tôt, et
   cette transition reste non observée. Elle ne le sera pas sans un redéploiement.
4. **La part respective** de la sauvegarde d'historique et du service HTTP —
   question ouverte depuis le 10/09, toujours ouverte.

⚠️ **Interdiction maintenue** : aucune modification de constante ou de structure
du `logger` sans preuve instrumentée. Cette mesure ne fournit pas cette preuve.

### Méthode

Captures par `/root/pool-captures/capture-logs.sh` (lecture seule sur l'appareil,
`GET /get-system-info?hf=1` et `GET /download-logs`), toutes les 2 h sur la
fenêtre complète, densifiées à 30 min sur la bande 20-32 h — celle où le palier
s'était installé sur v2.50.0. Série rendue par `tools/trace_fragmentation.py`.

⚠️ Les heures de capture sont en **UTC** (conteneur), les horodatages du journal
en **heure locale** (Europe/Paris, UTC+2 en été). Ne pas les comparer sans convertir.

## Mise à jour — l'instrument existe (feature-127/128, [ADR-0081](../adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md))

**Ce document reste ouvert au point « CE QUE CETTE MESURE NE TRANCHE TOUJOURS
PAS » ci-dessus.** Ce qui change : un recensement bloc par bloc
(`GET/POST /debug/heap_census`) et une lecture de contenu ciblée
(`GET/POST /debug/heap_peek`, build de diagnostic uniquement) sont désormais
disponibles pour la **prochaine** fenêtre d'observation. Ni l'un ni l'autre
n'a encore tourné en conditions réelles au moment où cette section est écrite
— rien ci-dessous n'est une conclusion, c'est le **protocole** à suivre quand
la prochaine fenêtre sera ouverte.

### Champ `a` (feature-127) — à lire en même temps que `fb`/`ab`

`GET /get-system-info?hf=1` publie désormais `a` (octets réellement alloués,
`total_allocated_bytes`) à côté de `f`/`fb`/`ab`. C'est la mesure qui manquait
pour trancher la question ouverte plus haut dans ce document (« un jeu de
travail plus gros ou une fuite en octets ? ») : si `a` croît sur la fenêtre
pendant que le **nombre** de blocs oscille sans tendance, la fuite en octets
est établie ; si `a` suit `libre` en sens inverse à somme quasi constante,
c'est un jeu de travail plus gros. `libre + a` reste stable à l'échelle du
tas, aux métadonnées de l'allocateur près — c'est le calcul de contrôle.

### Protocole de la prochaine fenêtre d'observation

**Ce qui doit être capturé, en plus de ce que la fenêtre du 12-13/09 captait
déjà** (`GET /get-system-info?hf=1` et `GET /download-logs`) :

1. **Amorçage, une fois en tout début de fenêtre** : `POST /debug/heap_census`
   **sans** `used_from`/`used_to` (fenêtre vide par construction, voir
   `API.md`), puis `GET /debug/heap_census?kind=free` pour lire `heap_start`/
   `heap_end` — l'enveloppe réelle du tas sur **cet** appareil, à ne pas
   deviner.
2. **À chaque cycle de capture ensuite** : `POST /debug/heap_census?used_from=<heap_start>&used_to=<heap_end>`
   puis, après le délai de traitement par la loop (< 1 s, mais laisser au
   moins `kHeapCensusMinIntervalMs` = 5 s entre le POST et le GET par
   prudence), `GET /debug/heap_census?kind=used&offset=0&count=…` en
   paginant jusqu'à `returned + offset == total` (`used_truncated`/`truncated`
   à surveiller — un `true` signifie qu'il faut resserrer `used_from`/
   `used_to` autour de la zone d'intérêt plutôt que tout demander d'un coup),
   **et** `GET /debug/heap_census?kind=free` pour le top-N de blocs libres.
3. **Cadence : encadrer la bande où les découpes ont eu lieu, pas la fenêtre
   entière.** La mesure du 12-13/09 a situé les quatre marches entre **20,7 h
   et 28,2 h** d'uptime. Reprendre le même principe de densification que la
   fenêtre précédente (grossier hors bande, fin dans la bande) :
   - **toutes les 2 h** de 0 à 18 h d'uptime (amorçage + régime stable) ;
   - **toutes les 15-20 min** de 18 h à 30 h d'uptime — plus fin que les
     30 min de la fenêtre précédente, parce que c'est un **recensement de
     blocs individuels** qui est comparé cette fois, pas seulement un
     agrégat : plus la fenêtre entre deux captures est courte, moins il y a
     de candidats à départager dans l'étape d'appariement ci-dessous ;
   - retour à 2 h au-delà de 30 h, jusqu'à la fin de fenêtre (40 h visée).

### Comment apparier deux recensements pour désigner un bloc suspect

1. Prendre le **top-N de blocs libres** (`kind=free`) d'une capture **avant**
   une marche connue (ex. la capture à 20,7 h, dernière avant la 1ʳᵉ marche
   du 13/09 — voir la table `### La série complète` plus haut).
2. Prendre la **liste des blocs occupés** (`kind=used`, `used_from`/`used_to`
   couvrant l'adresse du grand bloc libre identifié à l'étape 1) d'une
   capture **après** la marche.
3. **Un bloc occupé de l'étape 2 dont l'adresse tombe dans l'intervalle
   `[addr, addr + size)` d'un bloc libre de l'étape 1 est un candidat** :
   une allocation est apparue là où il n'y avait, à la capture précédente,
   qu'un grand trou. C'est la signature recherchée — celle qu'aucun agrégat
   (`fb`/`ab`/`a`) ne peut donner.
4. **Corroborer, ne pas conclure sur un seul candidat.** Répéter sur les
   quatre marches connues (7 168 / 1 536 / 4 608 / 2 304 o) ; un candidat
   dont la `size` ne colle à aucune marche n'est probablement pas le bon
   repère temporel, mais peut rester un candidat sur une autre marche.

### Du bloc suspect à la lecture de contenu

**Uniquement si un candidat crédible a été désigné par l'appariement
ci-dessus** — R3 ne sert pas à explorer au hasard, elle sert à confirmer une
hypothèse déjà formée par R2 :

1. Reflasher (OTA ou USB) le **build de diagnostic** :
   `PLATFORMIO_BUILD_FLAGS=-DPOOL_HEAP_PEEK=1 pio run -e esp32dev_core3`
   (voir [ADR-0081](../adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md)
   pour le mécanisme de non-livraison que ce build contourne délibérément,
   sous contrôle).
2. `POST /debug/heap_peek?addr=<addr du candidat>&len=<jusqu'à 128>`, puis
   `GET /debug/heap_peek`.
3. Lire `ascii` : une chaîne de journal (horodatage `AAAA-MM-JJ HH:MM:SS`,
   niveau `[INFO]`/`[WARN]`/…) désigne le `logger` sans ambiguïté. Un motif
   binaire ou une structure C++ reconnaissable (ex. un en-tête de
   `AsyncResponseStream`, un buffer WiFi) désigne autre chose — **dans les
   deux cas, c'est une identification, pas encore une correction.**

### Conditions d'exploitation, non négociables (S11/S12, `security-reviewer`)

Reprises intégralement de la spec [`feature-128`](../../specs/features/done/feature-128-recensement-du-tas-et-identification-des-intrus.md#conditions-dexploitation-s11s12-security-reviewer--ce-sont-des-consignes-humaines-rien-ne-les-verifie-a-la-place-de-lopérant) —
**rien dans le firmware ni dans la CI ne les vérifie à la place de l'opérant** :

- **S11 — aucune sortie de `/debug/heap_peek` (hex/ASCII) n'est versionnée.**
  Toute mise à jour de ce document qui exploite une lecture de contenu doit
  citer **adresse, taille, identification** — **jamais le contenu lu**. Les
  captures brutes vivent hors du dépôt.
- **S12 — reflash d'un binaire de release après la fenêtre, preuve
  consignée ici.** `GET /debug/heap_census` → `"build":{"peek":0}` (ou `404`
  sur `GET /debug/heap_peek`), à coller dans ce document à la clôture de la
  fenêtre — pas seulement exécuté, **écrit**.

### Ce que le script de capture externe doit devenir

`/root/pool-captures/capture-logs.sh` (hors dépôt, cron 04:00 et 05:30 UTC)
n'est **pas modifié par ce lot**. Pour servir le protocole ci-dessus, il
devra, en plus de ce qu'il fait déjà (`GET /get-system-info?hf=1`,
`GET /download-logs`) :

- ajouter les deux requêtes `heap_census` (`POST` puis `GET`, `kind=free` et
  `kind=used`) à chaque cycle de capture, avec la cadence densifiée décrite
  ci-dessus (statique — le script n'a pas besoin de connaître l'uptime pour
  décider de sa propre fréquence, c'est la planification cron qui densifie) ;
- **ne jamais appeler `/debug/heap_peek`** — cette route n'existe que sur un
  build de diagnostic reflashé à la main pour une désignation précise, jamais
  en capture automatisée (S11 : aucune sortie de contenu ne doit être écrite
  sur disque sans revue humaine avant tout archivage) ;
- conserver la séparation déjà en place : lecture seule sur l'appareil,
  aucune écriture, aucun redémarrement.


---

## Mise à jour — LE MÉCANISME EST NOMMÉ (fenêtre v2.52.0, 15/09/2026)

**L'instrument a livré.** Deux événements de découpe du plus gros bloc contigu ont
été observés **à l'adresse près, avec identification du bloc responsable**.

⚠️ **Condition S11 respectée** : ce document cite l'adresse, la taille et
l'**identification** (quel site d'émission a produit l'allocation). Aucun vidage
hexadécimal ou ASCII de `/debug/heap_peek` n'est versionné ici, et aucun ne doit
l'être — la lecture peut traverser des blocs portant des identifiants.

### ✅ CERTAIN — ce sont des `String` du journal qui découpent le grand trou

| uptime | trou avant | trou après | bloc responsable | taille demandée |
|---|---:|---:|---|---:|
| 18,0 h | 0x3FFF7308–0x3FFFB478 (16 752 o) | 0x3FFF7308–0x3FFF9648 (9 024 o) | `0x3FFF964C` | **144 o** |
| 19,4 h | 0x3FFF7308–0x3FFF9648 (9 024 o) | 0x3FFF79EC–0x3FFF9648 (7 260 o) | `0x3FFF7894` | **144 o** |

Les deux blocs portent **le même message** : l'avertissement de famine mémoire
émis par `src/history.cpp:542` (« sauvegarde de l'historique sautée »). Même
taille demandée, adresses différentes, deux marches distinctes séparées de 1,4 h.

Ce sont des entrées de `std::vector<LogEntry>` (`src/logger.h:22,35`), dont le
champ `String message` est alloué sur le tas et de **taille variable** — la
famille d'allocations que la seule mesure de taille ne pouvait pas identifier,
et pour laquelle la lecture de contenu (R3) a été conçue.

### ✅ CERTAIN — l'amplification, chiffrée

À 18,0 h, **160 octets alloués ont détruit 7 680 octets de contiguïté** : un trou
de 16 752 o coupé en 9 024 + 7 568, le « plus gros bloc » tombant au premier des
deux. Le coût pour le système n'est pas la taille de l'allocation, c'est **la
position** où elle tombe.

### 🔶 FORTEMENT PROBABLE — la boucle de rétroaction

    famine  →  avertissement journalisé  →  String de ~144 o dans le grand trou
            →  trou coupé en deux  →  famine aggravée  →  …

L'avertissement est throttlé à **une émission par HEURE**
(`kHistorySaveWarnThrottleMs = 3600000UL`, `src/constants.h:281`) tant que la
famine dure. ⚠️ Ne pas confondre avec les « 5 min » que porte le texte du
message : c'est l'intervalle de RETENTATIVE de la sauvegarde, pas celui de la
journalisation — première rédaction de cette section, corrigée le 15/09.

La cadence horaire **colle aux observations** : les deux découpes sont espacées
de **1,4 h**. Chaque émission alloue une `String` de taille quasi constante.
L'hypothèse — cohérente avec le cliquet, non démontrée — est que la suivante se
réinsère dans l'emplacement que la précédente vient de libérer, si bien que le
trou n'a jamais l'occasion de se recoalescer. Le tampon circulaire du journal
retient 200 entrées (`kMaxLogEntries`, `src/constants.h:174`), donc une `String`
y survit longtemps avant éviction.

**Ce qui manque pour en faire une certitude** : observer l'occupation de
l'emplacement en continu, et non par sondages espacés de 20 minutes.

### ❓ CE QUE CECI N'EXPLIQUE PAS

1. **L'effondrement initial de −29 696 octets à 1,0 h d'uptime** (53 236 → 21 492
   en deux marches). Une autre échelle, un autre mécanisme probable.
2. **Les trois autres blocs apparus** pendant la marche de 19,4 h
   (`0x3FFF7938`, `0x3FFF9A9C`, `0x3FFF9B1C`) : libérés avant qu'on les lise,
   leur nature restera inconnue.
3. **Pourquoi le palier s'installe** vers 7-8 Ko et y reste — atteint ici à 19,4 h
   contre 28,2 h sur v2.51.0, soit **8 heures plus tôt**.

### ⚠️ CE QUE CELA CHANGE POUR L'INTERDICTION

L'interdiction posée plus haut — « ne jamais toucher aux constantes ou structures
du `logger` sans preuve instrumentée » — avait pour condition l'absence de preuve.
**Cette preuve existe désormais**, deux fois, avec adresse, taille et
identification.

Elle **n'autorise pas pour autant un correctif au fil d'une session**. Ce qu'elle
autorise, c'est d'**ouvrir un chantier** sur le `logger` avec une spec, parce
qu'on sait enfin sur quoi travailler. Plusieurs corrections sont pensables — et
elles n'ont pas du tout les mêmes effets :

- allouer les `String` de journal en PSRAM (mais le plancher de politique à 512 o
  existe pour une raison : `constants.h` documente que les caches LittleFS et le
  pool FILE de la libc côtoient les écritures flash) ;
- réserver les entrées de journal dans un pool statique au démarrage, quand le
  tas est vierge ;
- borner la taille des `String` de journal pour qu'elles tombent dans une classe
  de taille qui ne fractionne pas les grands blocs ;
- ne pas journaliser la famine pendant la famine — le remède le plus simple au
  cycle décrit ci-dessus, et le plus discutable, puisqu'il supprime le signal.

Aucune de ces options ne doit être choisie sans `pool-chemistry` (le `logger`
sert les gardes de dosage) ni sans mesure de l'effet réel.

### Méthode

Instruments livrés par `feature-127` et `feature-128` (v2.52.0, ADR-0081).
Recensement par `heap_caps_walk` toutes les 20 min sur la bande 18-30 h d'uptime,
ciblé sur le voisinage du plus gros trou libre ; comparaison par
`tools/compare_census.py`, restreinte à l'intersection des fenêtres ; lecture de
contenu par `/debug/heap_peek` sur un **build de diagnostic**, absent du binaire
de release.

⚠️ Après la fenêtre : reflash d'un binaire de release et preuve
`"build":{"peek":0}` (condition S12).
