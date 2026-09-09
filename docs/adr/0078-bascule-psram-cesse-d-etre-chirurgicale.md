# ADR-0078 — La bascule PSRAM cesse d'être chirurgicale : le seuil descend de 4 095 à 1 024 octets

- **Statut** : Accepté
- **Date** : 2026-09-09
- **Décideurs** : utilisateur, `architect`, `pool-chemistry` (GO sous conditions C1-C5), `code-reviewer` (Approved)
- **Spec(s) liée(s)** : [`feature-124`](../../specs/features/doing/feature-124-abaisser-le-seuil-de-bascule-psram.md) (scénario **S7** de `bug-080` lot C), [`feature-122`](../../specs/features/doing/feature-122-instrumentation-reserve-dma-et-metrique-du-pool-general.md) (l'instrument qui mesure l'effet), `feature-123` (**gelée**)
- **Révise** : [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md) — dont la décision reste en vigueur, mais dont la **doctrine** est abandonnée

## Contexte

[ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)
(v2.35.0, lot B-1 de `bug-080`) a posé
`heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)` avec un seuil de
**4 095 o**, et a écrit noir sur blanc **pourquoi ce chiffre** :

> « 4 095 est **CHIRURGICAL** — la seule classe d'allocations qui change de camp
> est *exactement 4 096 o*. On ne déplace pas une politique d'allocation, **on
> ferme une inégalité non stricte**. »

Ce paragraphe est **conservé mot pour mot** dans
[`constants.h`](../../src/constants.h), suivi d'une annotation : il documente une
décision qui **était juste à son heure**.

**Ce qui a changé, c'est la mesure.** Le 09/09/2026 à 07:10, sur la v2.49.3 et
pour la première fois avec l'instrument complet de `feature-122` (planchers fins
+ attribution de contexte corrigée), une reproduction volontaire a donné :

| Grandeur | Valeur |
|---|---|
| **Plancher fin `hf.i8.mb`** | **1 076 o** |
| **Régime au creux** | **`0x02` = rafale HTTP** — sans ambiguïté |
| Sondes d'échec | **948**, 1 204, 1 268 o |
| Planchers `0x080C` / `0x1804` (résolution 8 s) | 20 468 o — ils n'ont **rien vu** (facteur **19**) |
| Épisode 503 | déclenché à 2 932, **24 refus en 35 s** |

⭐ **Le chiffre de 2 932 o sur lequel toute la stratégie des sept scénarios avait
été bâtie n'était pas le creux.** C'est le **seuil de déclenchement** que la garde
503 observe **au moment où elle refuse**. Le creux réel est **presque trois fois
plus bas** — et c'est lui qui explique le défaut : **1 076 < 1 622**, la taille du
tampon TX WiFi. L'échec d'allocation est **arithmétique**.

Conclusion opérationnelle : le **tas interne 8 bits est la ressource rare**,
pendant que la PSRAM dort à **4,18 Mo libres**. Une politique qui ne déplace
qu'une seule classe de 4 096 o n'est plus à la mesure du problème.

## Décision

**Le seuil `kPsramExtmemThresholdBytes` passe de 4 095 à 1 024 octets.** Les
allocations `malloc` / `new` **ordinaires** de la classe **[1 025 ; 4 095]**
préfèrent désormais la PSRAM, ce qui libère du tas interne 8 bits. On **déplace
donc bien une politique d'allocation**, cette fois, et en connaissance de cause.

La valeur n'est pas un tâtonnement : c'est un **encadrement**. La règle codée est
**inclusive** — « taille **≤** seuil ⇒ RAM interne » (désassemblage de la
`libheap.a` liée, `bgeu limit, size`) — donc **seul ce qui dépasse le seuil part
en PSRAM** :

- **borne haute, strictement sous 1 600** : les **pbufs lwIP** (~1 600 o, **une
  structure par connexion TCP**) sont la classe qui fragmente sous six requêtes
  parallèles ; ils ne basculent que si `1 600 > seuil` ;
- **borne basse, strictement au-dessus de 512** : les **caches LittleFS** (512 o)
  et le **pool FILE de la libc** (428 o) restent en **RAM interne** — ce sont eux
  qui côtoient les **écritures flash** ;
- **fenêtre utile [512 ; 1 599] → 1 024**.

**Pourquoi lwIP est soumis à ce seuil** (vérifié à la source) : `lwipopts.h:1712`
retient `#define mem_clib_malloc malloc`, avec `MEM_LIBC_MALLOC=1` et
`MEMP_MEM_MALLOC=1`, parce que `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` **n'est pas
activé**. Toutes les structures lwIP passent donc par le `malloc` ordinaire.

L'égalité stricte `seuil + 1 == kVfsStdioBufferBytes` qui épinglait le seuil à
4 095 est **remplacée** par l'inégalité `seuil < kVfsStdioBufferBytes` : l'acquis
de B-1 (le tampon stdio VFS de 4 096 o reste en PSRAM) est conservé, la
**contrainte de calibrage** est abandonnée.

### ⚠️ Ce que cette décision NE fait PAS

Elle ne déplace **aucun tampon WiFi**. Les allocations qui échouent demandent
`caps=0x080C` (`8BIT|DMA|INTERNAL`) **explicitement**, pour 1 622 o : elles
**contournent** ce seuil et le contourneront toujours. Ce lot ne les corrige pas,
il leur **fait de la place**. C'est une promesse plus faible qu'un correctif, et
elle doit être énoncée ainsi partout.

### Le critère de succès est binaire, et non vérifié à ce jour

**Faire remonter le plancher fin `hf.i8.mb` au-dessus de 1 622 o** sous
reproduction volontaire identique. Ce n'est pas une amélioration à apprécier :
c'est un seuil franchi ou non. Il ne se mesure **que sur cible**, et **l'échec est
un résultat possible et prévu**.

### Condition C5 du gate `pool-chemistry` — l'invariant à réénoncer

Une donnée résidant en PSRAM et **touchée pendant que le cache flash/PSRAM est
coupé** (écriture NVS, écriture LittleFS) fait planter la machine. Abaisser le
seuil **élargit** cette exposition. L'innocuité repose sur **deux** propriétés, et
sur elles seules :

1. **La copie interne d'`esp_flash`** : la couche d'écriture flash recopie les
   tampons de l'appelant dans un tampon de rebond en RAM interne avant de couper
   le cache.
2. **L'absence d'ISR IRAM applicative** : **zéro `IRAM_ATTR`, zéro
   `attachInterrupt`, zéro `ESP_INTR_FLAG_IRAM` dans `src/`** — vérifié à la date
   de cet ADR. Aucun code du projet ne s'exécute donc cache coupé, et aucune
   structure applicative n'y est déréférencée.

⚠️ **Ce sont des invariants à réénoncer, pas des acquis.** Le jour où une ISR
IRAM applicative apparaît — capteur de débit, compteur d'impulsions, sortie
temps-réel — cet ADR doit être **relu avant** d'écrire la première ligne : ses
données devront exiger `MALLOC_CAP_INTERNAL` **explicitement**, la politique par
défaut ne les protège plus au-delà de 1 024 o.

Le gate a par ailleurs contre-vérifié : `pump_controller.cpp` ne contient aucun
`malloc` / `new` / `JsonDocument` (uniquement des `snprintf` sur pile, arrêt de
pompe par écriture de registre), les compteurs journaliers vivent en **NVS** et
non en LittleFS, aucune garde de dosage ne lit de métrique de tas, et les piles de
tâches sont allouées avec des **caps explicites** (`pvPortMalloc` →
`MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL`) : elles ne basculent pas.

## Alternatives considérées

- **Garder 4 095** (rejetée) — c'est l'**état d'avant**, et la mesure du 09/09 le
  disqualifie : la classe qui fragmente (~1 600 o) reste intégralement interne.
  Aucun coût, aucun risque… et aucun effet.
- **2 048** (rejetée — et surtout : **issue d'un raisonnement INVERSÉ**) — l'argument
  était « rester au-dessus de 1 622 pour capturer la classe lwIP ». Il **conclut le
  contraire de ce qu'il énonce** : la règle étant `taille ≤ seuil ⇒ interne`,
  `1 622 ≤ 2 048` serait **resté interne** et le lot n'aurait **rien** déporté. Le
  palier aurait été un *no-op* qu'on n'aurait découvert qu'**après déploiement**.
  Erreur rattrapée par le gate `pool-chemistry` **avant tout code** ; le
  raisonnement faux ne doit **pas** être regravé, et un test natif l'épingle
  (2 048 figure dans la table des paliers avec son motif).
- **Descendre à 512 d'un coup** (rejetée) — 512 est **admissible** pour le plancher
  de politique mais fait basculer la **réserve de repli du parachute** (1 024 o) du
  côté PSRAM. Or une réserve d'urgence en PSRAM **ne sauve rien** d'une famine de
  RAM interne : c'est exactement le piège de `feature-066`, prouvé le 05/09/2026.
  Un `static_assert` (condition **C3**) l'arrête à la compilation.
- **Paliers successifs sous 512** (**exclus, pas seulement rejetés**) — au-dessus de
  512 l'exposition au cache coupé est **quantifiée** : on sait quelles classes
  basculent et on peut les nommer. En dessous, les caches LittleFS (512 o) et le
  pool FILE de la libc (428 o) partent en PSRAM et l'exposition devient
  **binaire** : c'est un changement de **nature**, plus un réglage. **Ne jamais
  planifier de palier sous 512.**
- **1 024** (retenue) — seule valeur ronde de la fenêtre [512 ; 1 599], à égale
  distance des deux dangers, et qui laisse la réserve de repli **exactement au
  seuil** (donc interne, test inclusif).

## Conséquences

### Positives
- La classe **[1 025 ; 4 095]** quitte le tas interne 8 bits, dont les **pbufs
  lwIP** — la classe identifiée comme fragmenteur sous rafale HTTP.
- La bascule reste une **préférence** : si la PSRAM refuse, l'allocateur retombe en
  interne. **Aucun échec nouveau n'est créé.**
- **Réversible en une ligne**, et le rapport bénéfice/risque le plus favorable des
  sept scénarios évalués (7/10) pour un coût quasi nul.
- Le témoin A7 devient **relatif au seuil** (`seuil` / `seuil + 1`) : il garde son
  pouvoir discriminant à **tout** palier futur, au lieu de devenir mensonger.

### Négatives / dette assumée
- **L'exposition au cache coupé est élargie** de façon substantielle : elle ne
  repose plus sur « presque rien ne bascule » mais sur les deux invariants de la
  condition C5, explicitement énoncés ci-dessus.
- **La réserve de repli du parachute n'a plus aucune marge** : 1 024 o = le seuil.
  Elle reste interne par le test inclusif *et* par ses caps explicites, mais le
  prochain palier abaissé la ferait basculer — d'où le `static_assert` C3.
- **Sous pression PSRAM, le repli interne est silencieux** : le comportement
  dégrade vers l'ancien sans le signaler.
- **L'effet est non prouvé** à la date de cet ADR. On accepte de déployer une
  révision de doctrine pour une amélioration **mesurable seulement après**.

### Ce que ça verrouille
- **Toute structure devant vivre en RAM interne doit l'exiger explicitement**
  (`heap_caps_malloc(..., MALLOC_CAP_INTERNAL | ...)`) dès qu'elle **dépasse
  1 024 o** — et non plus 4 095 o. Le seuil de vigilance descend d'un facteur 4.
- **Aucun palier sous 512** ne sera planifié (changement de nature de
  l'exposition).
- **Aucune ISR IRAM applicative** ne peut être ajoutée sans relire cet ADR.
- **Les clés JSON `w96` / `w95` et les positions de bits de `w` ne bougent pas**,
  pour que le relevé du 09/09 reste comparable à celui d'après déploiement.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (la valeur, les **cinq**
  `static_assert`, le commentaire de B-1 conservé + annoté),
  [`src/main.cpp`](../../src/main.cpp) (site d'appel et témoin A7 relatif),
  [`src/heap_metrics.h`](../../src/heap_metrics.h) (bits `kHeapWitness*`),
  [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (clés `w96`/`w95`)
- Tests : `test/test_native_oom_guard` (encadrement + table des paliers),
  `test/test_native_heap_caps_table` (témoin relatif, présence des cinq
  `static_assert`, comparabilité des bits et des clés)
- ADR révisé : [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)
- Doc : [`docs/subsystems/system-health.md`](../subsystems/system-health.md#politique-de-bascule-psram-des-allocations-ordinaires-v2500-feature-124--s7),
  [`docs/subsystems/oom-guard.md`](../subsystems/oom-guard.md),
  [`docs/API.md`](../API.md)
