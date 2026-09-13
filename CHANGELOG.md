# Changelog - ESP32 Pool Controller

> ℹ️ **Réalignement des versions** : le versioning a été réaligné sur la dernière release publiée `v2.6.0`. Les entrées `[2.7.0]` et suivantes suivent cette numérotation. Les entrées historiques `[2.18.x]` / `[2.19.x]` ci-dessous correspondent à des développements antérieurs à ce réalignement (numérotation conservée telle quelle).

## [Unreleased] - 2026-09-13

> **Ce qui change pour vous** : en mode Automatique, les horaires de
> filtration affichés ne bougent plus en cours de journée — ils sont
> calculés à l'arrêt du cycle précédent et appliqués à minuit. Les arrêts
> intempestifs de 5 minutes après un démarrage programmé disparaissent.

### Firmware

- **Filtration (bug-126)** : le planning automatique n'est plus recalculé
  **pendant la marche** — c'était le défaut qui pouvait arrêter un cycle
  quelques minutes après son démarrage si la température avait baissé
  (fenêtre rétractée, `decideFiltrationRun()` arrêtant une pompe qui venait
  de démarrer). Le planning est désormais calculé à partir de la **moyenne**
  de température du cycle qui vient de se terminer (échantillons post-purge
  et en plage horaire), figé dans un planning en attente, et appliqué **à
  minuit seulement**. Voir [ADR-0079](docs/adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md).
- **Filtration** : les fenêtres calculées **s'allongent de 15 à 30 min** en
  moyenne pour une même journée — la référence passe d'un instantané pris
  5 minutes après le démarrage (l'heure la plus froide du cycle) à la
  moyenne de tout le cycle, plus représentative. ⚠️ Cette ampleur (+0,5 à
  +1 °C sur la référence) est une **estimation** de `pool-chemistry`, pas
  une mesure sur bassin réel ; seul le sens de l'effet (fenêtres plus
  longues) est établi avec certitude.
- **Filtration** : l'offset de calibration de température **s'applique
  désormais systématiquement** au planning auto. L'ancien chemin
  (`sensors.getTemperature()`) n'appliquait l'offset que lorsqu'une sonde de
  rôle « eau » était identifiée, et rendait la valeur **brute** (sans
  offset) sur son repli vers la première sonde détectée. Le nouveau chemin
  passe par `sensors.getWaterTemperature()`, qui applique toujours l'offset
  borné (±5 °C, bug-114) dès qu'une sonde eau est identifiée.
- **Filtration** : une plage auto **`start == end`** héritée d'un état
  antérieur (NVS corrompue, ancien défaut, saisie incohérente) est désormais
  **réparée automatiquement** (repli `08:00`-`20:00` + alerte MQTT
  `filtration_schedule_invalid`). Auparavant ce cas arrêtait la filtration
  **définitivement et en silence**, sans aucune auto-réparation possible.
- **Dosage (effet indirect)** : les arrêts intempestifs relançaient sans
  cesse la purge de mesure sans jamais la laisser aboutir — `mesures non
  représentatives` restait vrai en permanence, et **aucun dosage programmé
  n'avait lieu** pendant ces épisodes. Ces journées redeviennent dosables.
  **Aucune limite de dosage n'est modifiée** (journalières, horaires, par
  injection, `kMaxRequestsPerMinute`) : c'est une opportunité de dosage
  restaurée, pas un plafond relevé.
- **MQTT** : deux nouveaux types d'alerte sur le topic générique
  `{base}/alerts` — `filtration_schedule_invalid` (plage nulle réparée) et
  `filtration_temp_implausible` (référence de température hors bornes
  physiques, panne de sonde présumée). Aucune entité d'auto-discovery
  Home Assistant nouvelle.

### Documentation

- `docs/adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md` créé :
  la règle « calculé à l'arrêt, appliqué à minuit », les quatre options
  écartées, le refus (chiffré) d'élargir l'horizon de dosage, l'arbitrage
  RAM contre NVS, les bornes de plausibilité `[4, 40] °C`.
- `docs/adr/0074-temperature-figee-statu-quo-instrumente.md` : note additive
  — le mécanisme d'immunisation du planning contre une sonde figée change
  (l'ancienne bande morte sur `_lastScheduledTemp` n'existe plus), la
  conclusion de l'ADR ne bouge pas.
- `docs/subsystems/filtration.md`, `docs/features/page-filtration.md`,
  `docs/MQTT.md` : mise à jour du mécanisme de planning auto (planning en
  attente, gardes de promotion, alertes neuves).
- `docs/user/src/guide-utilisation.html` : les horaires affichés en mode
  Automatique viennent du cycle de la veille, ils ne bougent plus en cours
  de journée.

---

## [2.51.0] - 2026-09-12

> **Ce qui change pour vous** : rien de visible côté écrans. Le journal du
> firmware et la route de diagnostic `/get-system-info?hf=1` deviennent plus
> fiables et un peu plus riches — utile en cas de nouvel incident de
> fragmentation, sans effet sur la régulation ni sur l'interface.

### Firmware

- **Journal `fopen` (lot B1)** : correctif d'un défaut qui rendait
  `fopenGuardDrain()` incapable d'émettre `EpisodeStart` — la branche était
  **structurellement inatteignable** depuis la v2.43.0 (`fsGuardAdmit()` posait
  déjà `active = true` avant que le drain ne teste `!st.active`). Conséquence
  vérifiée sur 28,9 h de terrain réel (incident du 10/09) : **zéro** ligne
  « Ouvertures de fichiers refusées » n'avait jamais été émise, et les trois
  durées d'épisode journalisées (92 279 s, 99 488 s, 102 792 s) valaient en
  réalité l'**uptime** depuis le boot, pas la durée de l'épisode. Deux drapeaux
  distincts (`active` / `reported`) remplacent l'unique `active` ; le front
  montant de l'épisode est désormais extrait dans une fonction pure dédiée,
  `fopenGuardNoteRefusal()`, testée nativement.
- **Diagnostic mémoire (lot B2)** : publication de deux compteurs déjà calculés
  par `heap_caps_get_info()` et jusqu'ici jetés, `free_blocks` /
  `allocated_blocks` — aucun parcours de tas supplémentaire. Exposés en `b804=`
  sur la ligne de journal `Heap masq fen:` (référence `0x0804` uniquement) et en
  `fb` / `ab` par masque dans `GET /get-system-info?hf=1`. `libre / fb` donne la
  taille moyenne d'un trou : la grandeur qui manquait pour distinguer un
  morcellement modéré d'une pulvérisation, à `libre` constant.

### Documentation

- `docs/API.md` : champs `fb` / `ab` de l'objet `hf`, leur sémantique `null`
  pour les masques `def` / `spi` (chemin O(régions), aucun compte de blocs
  mesuré), et re-mesure de la charge JSON de `/get-system-info?hf=1` (sans
  `?hf=1` inchangée à 964 o, avec `?hf=1` ~1 870-1 876 o selon le harnais de
  mesure — méthode détaillée, ADR-0073).
- `docs/subsystems/acces-fichiers.md` : le correctif à deux drapeaux
  `active` / `reported` de `fs_guard` / `fopen_guard_logic`, pourquoi
  l'extraction dans le module pur compte pour la validation par mutation, et la
  nuance sur le mutant équivalent (pas tué) de `fopenGuardEpisodeDurationMs()`.
- `docs/subsystems/system-health.md` : format `ligne3` (`b804=`, tampon élargi
  à 176 o), nouveaux champs `fb` / `ab` par emplacement, et l'angle mort de test
  sur `heapProbeForCaps()` (non exécutée par la suite native).
- `docs/subsystems/logger.md` : pire cas de `ligne3` recalculé après l'ajout de
  `b804=`.
- `docs/incidents/2026-09-10-recidive-fragmentation-v2500.md` : le défaut
  `fs_guard` est **confirmé et corrigé** ; les trois durées relevées le 10/09
  étaient des artefacts, pas des mesures d'épisode.

---

## [2.50.0] - 2026-09-09

> ⚠️ **REQUALIFIÉ LE 10/09/2026 — le problème est revenu après 28,9 h.** Le
> résultat ci-dessous est réel, mais il a été mesuré sur un **tas frais** de moins
> d'une heure, avec seulement **500 octets** de marge au-dessus de la garde. Le
> 10/09, `maxBloc8` est resté bloqué à **7 872 o** à charge nulle (`ws = 0`), donc
> sous le seuil : le message est réapparu. S7 traite le coût par connexion, **pas
> la dégradation temporelle**. Voir
> `docs/incidents/2026-09-10-recidive-fragmentation-v2500.md`.
>
> ⭐ **RÉSULTAT SUR CIBLE, 09/09 08:23 — le message « mémoire insuffisante » avait
> disparu.** Sous une rafale de rechargements presque **deux fois plus forte**
> que celle qui déclenchait le défaut : **327 requêtes servies**, **zéro** refus,
> **zéro** déconnexion MQTT, et **un seul** échec d'allocation contre 25. Le plus
> gros bloc libre au plus creux passe de **1 076 o à 8 692 o** — facteur **8**, et
> au-dessus du seuil de la garde, qui n'a donc plus eu de raison de se déclencher.
>
> Le refus « Too many requests » observé pendant le test est une **autre** garde,
> le limiteur de débit d'authentification : intentionnelle, sans rapport avec la
> mémoire, et refusant **proprement** avec l'écran de login intact. C'est la
> différence entre une protection et une panne.

> **Ce qui change pour vous** : rien de visible. Aucun écran, aucun message,
> aucun réglage ne bouge. Le firmware change sa **politique d'allocation
> mémoire** pour laisser respirer le tas interne, celui dont la pénurie provoque
> les refus « Service momentanément indisponible » au chargement de l'interface.

### Firmware

- **La bascule PSRAM cesse d'être chirurgicale : le seuil descend de 4 095 à
  1 024 octets** (`kPsramExtmemThresholdBytes`). Les allocations `malloc`/`new`
  ordinaires de la classe **[1 025 ; 4 095]** préfèrent désormais la PSRAM — dont
  les **pbufs lwIP** (~1 600 o, une structure par connexion TCP), la classe qui
  fragmente le tas interne sous six requêtes parallèles. La valeur n'est pas un
  tâtonnement mais un **encadrement** : sous 1 600 pour que lwIP bascule,
  au-dessus de 512 pour laisser en RAM interne les caches LittleFS (512 o) et le
  pool FILE de la libc (428 o), qui côtoient les écritures flash — fenêtre
  **[512 ; 1 599]**. Cinq `static_assert` verrouillent ces relations à la
  compilation.
- ⚠️ **Ce que ce lot NE fait PAS** : il ne déplace **aucun tampon WiFi**. Les
  allocations qui échouent (`caps=0x080C`, 1 622 o) demandent leurs capacités
  **explicitement** : elles contournent ce seuil et le contourneront toujours. Le
  lot ne les corrige pas, il leur **fait de la place**.
- **Premier des trois pas** de la stratégie arrêtée le 08/09 pour clore
  `bug-080` : S7 (ce lot), puis la **mesure du fragmenteur**, puis S2
  (« sérialiser le service des fichiers statiques »). Il a été choisi pour son
  rapport bénéfice/risque — coût quasi nul, réversible en une ligne — **pas** pour
  son efficacité attendue.
- ⚠️ **Son effet reste à vérifier, et l'échec est un résultat possible.** Le
  critère est **binaire** : faire remonter le plancher fin `hf.i8.mb` au-dessus de
  **1 622 o** sous reproduction volontaire identique. Il ne se mesure **que sur
  cible**, après déploiement.
- **Le témoin de bascule PSRAM devient relatif au seuil** : il alloue désormais
  `seuil + 1` et `seuil` au lieu de 4 096 et 4 095 o. À seuil 1 024, un
  `malloc(4095)` part en PSRAM — le témoin de contrôle aurait signalé une
  **anomalie inexistante**. Les **clés JSON `w96`/`w95` et les positions de bits
  sont conservées** pour que le relevé du 09/09 reste comparable à celui d'après
  déploiement.
- **Le message PSRAM du démarrage annonce le nouveau seuil** : « allocations de
  PLUS de 1024 o tentées en PSRAM d'abord […] ; 1024 o et moins : RAM interne ».

### Documentation

- **[ADR-0078](docs/adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md)** créé —
  « la bascule PSRAM cesse d'être chirurgicale ». Il **révise assumément** la
  doctrine du lot B-1 ([ADR-0061](docs/adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md),
  dont la décision reste en vigueur), trace les alternatives écartées — garder
  4 095 ; **2 048, issu d'un raisonnement inversé** ; 512 d'un coup ; paliers sous
  512, **exclus** car l'exposition y devient binaire — et réénonce l'invariant de
  la condition C5 du gate `pool-chemistry` : l'innocuité sous cache coupé repose
  sur la **copie interne d'`esp_flash`** *et* sur l'**absence d'ISR IRAM
  applicative** dans `src/`.
- ⭐ **Le creux réel du tas interne est 1 076 o, et non 2 932.** Le chiffre de
  2 932 sur lequel toute la stratégie des sept scénarios avait été bâtie était le
  **seuil de déclenchement** observé par la garde 503 au moment où elle refuse,
  pas le creux. Le creux relevé le 09/09 est presque **trois fois plus bas** — et
  il explique le défaut : **1 076 < 1 622**, la taille du tampon TX WiFi.
- `docs/subsystems/system-health.md` : nouvelle section sur la politique de
  bascule PSRAM (encadrement, motif lwIP, ce que le lot ne fait pas, critère
  binaire) ; le témoin A7 et sa table de bits passent à la sémantique
  `seuil`/`seuil + 1`.
- `docs/API.md` : sémantique de `w96`/`w95` devenue relative au seuil, clés
  inchangées, avec le motif de cette conservation.
- `docs/subsystems/oom-guard.md`, `docs/subsystems/web-server.md` : seuil et
  message de démarrage réalignés.

## [2.49.3] - 2026-09-08

> **Ce qui change pour vous** : rien de visible. Troisième et dernier défaut de
> l'instrument de mesure corrigé — il enregistrait le creux sans savoir dire
> **quel régime** l'avait produit.

### Firmware

- **Le régime certain est posé par l'appelant, plus hérité d'un échantillonnage.**
  La reproduction volontaire de 23:17 a fait descendre le plancher à **2 932 o** et
  l'a étiqueté « aucun régime », alors que le creux s'est produit en pleine rafale
  HTTP. La poussée fine réutilisait un contexte vieux de 2 s. Le site HTTP pose
  désormais son propre bit, fusionné avec le contexte échantillonné — sans mesure
  ni lecture nouvelle.

### Documentation

- **2 932 octets est une constante structurelle.** La même valeur, à l'octet près,
  que l'épisode du 29/08 qui a motivé l'ADR-0067 — deux firmwares, deux
  reproductions indépendantes, dix jours d'écart. Une rafale de six requêtes
  parallèles laisse ce trou-là.
  ⚠️ **Relu le 09/09** : cette constante est le **seuil de déclenchement** observé
  par la garde 503 au moment où elle refuse, **pas le creux**. Le creux réel,
  mesuré finement, est **1 076 o** — voir `[2.50.0]`.
- Le mécanisme complet est capté pour la première fois : 176 requêtes, 25 échecs
  d'allocation, `maxBloc` de la sonde à **1 204 o** (sous les 1 622 du tampon TX
  WiFi), 503 déclenché à 5 108 et rétabli en 30 s.

## [2.49.2] - 2026-09-08

> **Ce qui change pour vous** : rien de visible. Un défaut de l'**instrument de
> mesure** livré le jour même est corrigé — il sous-estimait de près du double le
> creux qu'il sert à mesurer.

### Firmware

- **La mesure fine alimente enfin les planchers par masque.** Le creux de tas
  relevé **à l'instant d'une requête** (`web_server.cpp`, filtre de la garde 503,
  et `fs_guard.cpp`) n'alimentait que les deux compteurs historiques, jamais les
  planchers de `feature-122`. Ceux-ci ne voyaient donc que l'échantillonnage à
  2 s et **rataient les creux de requête** — précisément ceux qui comptent.
  Aucun parcours de tas n'est ajouté : la valeur est déjà mesurée par l'appelant.
- **Ce que le défaut aurait coûté** : `hf.dma.mb` devait dimensionner la réserve
  DMA de `feature-123`, lot qui retire de la RAM interne **irréversiblement au
  démarrage**, sans repli série. La réserve aurait été calibrée sur un chiffre
  faux de 94 %.

### Tests

- Le correctif est verrouillé : le test meurt si la poussée fine est retirée, si
  elle vise le mauvais masque, ou si un parcours de tas est ajouté sur ce chemin
  (`async_tcp`, pendant la famine — condition 2 du gate `pool-chemistry`).

### Documentation

- **Le critère de clôture de `bug-080` est réécrit pour la seconde fois de la
  soirée**, et les deux erreurs sont consignées comme garde-fou : d'abord bâti sur
  le **nombre d'épisodes 503** — donc sur la chance qu'une requête tombe au
  mauvais moment — puis sur des planchers dont la **résolution** n'avait pas été
  éprouvée. La règle qui s'en dégage : un critère de clôture ne se rédige
  qu'**après** un relevé qui a effectivement mordu, et provoqué volontairement.
- **L'hypothèse de l'utilisateur est confirmée** : charger l'interface provoque la
  famine. 117 requêtes provoquées volontairement ont déclenché **2 épisodes 503**
  et **4 échecs d'allocation**, avec le plus gros bloc descendu à **4 084**. Les
  8 h 49 précédentes n'en montraient aucun — une période calme ne prouve rien.

## [2.49.1] - 2026-09-08

> **Ce qui change pour vous, en une phrase** : **rien de visible.** Aucun écran,
> aucun réglage, aucun comportement de régulation ne change. Cette version règle
> l'**instrument de mesure** installé par la 2.49.0, à partir du premier relevé
> réel de 8 h 49 sur l'appareil.

> ✅ **Une question ouverte depuis le 05/09 est fermée : le lot B-1 est CONFIRMÉ
> OPÉRANT.** Le témoin d'allocation du démarrage montre que le `malloc(4096)`
> part bien en **PSRAM** et que le témoin de contrôle `malloc(4095)` reste en
> **interne**, sans aucune discordance entre les deux méthodes de classement
> d'adresse. `heap_caps_malloc_extmem_enable(4095)` (v2.35.0,
> [ADR-0061](docs/adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))
> fait donc ce pour quoi il a été livré : le soupçon de « correctif décoratif »
> est levé. `psram_free` était simplement un **mauvais témoin** — le plancher
> PSRAM, lui, montre un creux de 5 764 o que l'échantillonnage à 30 s ne pouvait
> pas voir.

### Firmware

- **Cadence du tour de rôle des planchers divisée par deux**
  (`kHeapFloorWalkEverySamples = 2`). Le relevé du 08/09 a mesuré la durée réelle
  d'un parcours de tas à **516 µs au pire** (médiane ≤ 512 µs) — **six fois** le
  repère de ~84 µs sur lequel le gate `pool-chemistry` avait raisonné, et
  **au-dessus des 300 µs** de sa condition 5, qui se déclenche donc. Le masque
  supplémentaire n'est plus mesuré qu'**un échantillon sur deux** : coût moyen
  **1,5 parcours par échantillon (0,75/s)** au lieu de 1/s — sous le plafond du
  gate, **avec de la marge pour la première fois**. La durée d'une occurrence
  n'était pas le levier disponible : c'est la **fréquence** qui a baissé.
  ⚠️ **Ce qui est perdu** : la résolution du plancher `0x080C` passe de **4 à
  8 s**. C'est le chiffre qui dimensionnera `feature-123` — il doit être lu en
  connaissant sa résolution. Atténuation : `heaps(0x080C)` est un sous-ensemble de
  `heaps(0x0804)`, dont le plancher reste mesuré à **chaque** échantillon et le
  **borne supérieurement**, donc dimensionner sur `0x0804` reste conservateur.
- **Les durées de parcours sont enfin PUBLIÉES** : clés `hf.wmax` et `hf.wmed` de
  `GET /get-system-info?hf=1`. Elles étaient **mesurées depuis le premier jour
  mais jamais exposées par la route** — le relevé n'a pu les lire que dans le
  journal, et la condition 5 du gate n'était donc satisfaite que **par accident**.
  `wmed` est une médiane **approchée**, rendue comme **borne supérieure** du seau
  dyadique contenant le rang médian : à lire « ≤ `wmed` », jamais comme une
  valeur exacte.
- **Deux accumulateurs de contexte au lieu d'un** : clé neuve `hf.ctxb`
  (`ctxSeenBoot`), qui ne se réarme **jamais**. Motif : au relevé du 08/09,
  `hf.ctx` valait **1** après 8 h 49, ce qui se lisait comme « un seul régime
  jamais rencontré » — **faux**, ce champ est remis à zéro toutes les 30 minutes
  par le réarmement de fenêtre et ne portait que la fenêtre courante. Un test
  interdit désormais au réarmement de toucher `ctxSeenBoot`.
- **Budget JSON re-mesuré** : `GET /get-system-info?hf=1` passe de **1 694 à
  1 741 o** pire cas (+47 o pour les trois clés). Sans le paramètre, la réponse
  reste **inchangée à l'octet près** (964 o).
- ⚠️ **Toujours aucun correctif de fragmentation.** Aucune réserve DMA n'est
  posée, aucune garde n'est modifiée, aucun seuil ne bouge : la métrique du refus
  503, de la garde d'ouverture de fichiers, du parachute OOM et des gardes TLS
  **reste `0x0804`** ([ADR-0077](docs/adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md)).

### Documentation

- `docs/subsystems/system-health.md` : relevé du 08/09 intégré (8 h 49, 12 673
  échantillons, zéro redémarrage) — verdict A7, durées de parcours, trajectoire
  de `maxBloc8`, contexte au creux, plancher PSRAM ; nouvelle résolution du tour
  de rôle et ce qu'elle coûte ; **protocole de relevé corrigé** : lire `ctxb` et
  non `ctx` pour les régimes vus, et interpréter `wmed` comme une **borne**.
  Nouveau point de méthode : un plancher sous le seuil de garde **n'implique pas**
  un 503, la garde ne mesurant qu'**au moment d'une requête**.
- `docs/API.md` : les trois clés neuves de `hf`, la sémantique **fenêtre vs
  depuis-boot** de `ctx`/`ctxb` avec le contresens qui a motivé l'ajout, la
  lecture « ≤ » de `wmed`, et le budget JSON re-mesuré.
- `specs/features/doing/bug-080-*.md`, `specs/features/todo/bug-121-*.md`,
  `specs/features/todo/feature-123-*.md` : le relevé de 8 h 49 **ne reproduit
  pas** la corrélation entre niveau de fragmentation et déconnexions MQTT
  (déconnexions aux niveaux **hauts**, zéro pendant la période la plus basse). Le
  lien reste une **hypothèse de travail**, ni confirmée ni infirmée — la
  rectification écrite le même jour sur la base du journal de 21 h est nuancée en
  conséquence, sans être annulée.

### Tests

- **Incident de méthode consigné** (`feature-122`, section « Relevé du
  08/09/2026 ») : la première sentinelle du diviseur de cadence était
  **décorative** et **a survécu à sa propre mutation** — elle comptait la
  présence de jetons (`kHeapFloorWalkEverySamples`, `walkThisSample`) qui restent
  tous deux en place quand `if (walkThisSample)` devient `if (true)`. Corrigée
  sur deux axes : le **texte exact** de la garde, et surtout l'extraction de la
  règle en **logique pure** (`heapFloorWalkDue`, `heapFloorWalkNextDivider`),
  testée par table de vérité replis compris. **Troisième forme du même piège en
  deux jours** dans ce dépôt.

---

## [2.49.0] - 2026-09-08

> **Ce qui change pour vous, en une phrase** : **rien de visible.** Cette version
> n'ajoute aucune fonctionnalité, ne corrige aucun bug et ne modifie aucun
> réglage : elle installe un **instrument de mesure** destiné à trouver la cause
> des coupures d'interface (erreurs 503) de `bug-080`. Aucun écran ne change,
> aucune notice n'est à relire.

### Firmware

- **Planchers mémoire multi-masques** (`feature-122`, lot **C-1a** de `bug-080`,
  [ADR-0077](docs/adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md)).
  Le firmware retient désormais, pour **cinq pools de tas** — `0x0804`
  (`INTERNAL|8BIT`, la métrique actuelle des gardes), `0x080C`
  (`INTERNAL|DMA|8BIT`, la **classe d'échec observée en production**), `0x1000`
  (`DEFAULT`), `0x1804` (`INTERNAL|8BIT|DEFAULT`) et `MALLOC_CAP_SPIRAM` — le
  **minimum depuis le démarrage** et le **minimum de fenêtre**, sur le **libre**
  comme sur le **plus gros bloc contigu**. Un minimum, jamais un instantané : une
  pointe de quelques millisecondes échappe par construction à un échantillonnage
  périodique.
- **Un octet de contexte au creux** : quel événement tenait la machine au moment
  du minimum — client WebSocket connecté, pompe doseuse sous tension, fenêtre
  post-connexion MQTT (rafale de découverte HA), rafale de fichiers statiques,
  refus de la garde d'ouverture de fichiers. Sans lui, un plancher est un chiffre
  anonyme qui ne dimensionne rien.
- **Témoin `malloc(4096)` + témoin de contrôle `malloc(4095)`** au démarrage,
  **libérés aussitôt**, pour trancher une question restée ouverte : le lot B-1
  (`heap_caps_malloc_extmem_enable(4095)`, v2.35.0) est-il **opérant ou
  décoratif** ? `psram_free` est immobile à 4 180 340 dans **152 relevés**, ce qui
  peut aussi bien signifier « rien ne part en PSRAM » que « l'indicateur ne le
  voit pas ». Le couple d'allocations donne une signature non ambiguë.
- **Durée des parcours de tas** exposée (`walkMaxUs`, `walkMedianUs`), exigée par
  la condition 5 du gate `pool-chemistry` : ce qui menace la régulation est la
  **durée d'une occurrence interruptions coupées** (~84 µs relevés), pas le taux
  d'occupation. Médiane **approchée** par histogramme dyadique à 7 seaux, rendue
  comme **borne supérieure de seau** — donc **surestimée**, biais conservateur
  assumé et documenté. Horloge `esp_timer_get_time()` et non `CCOUNT`, qui est
  par cœur.
- **Exposition en lecture** : `GET /get-system-info?hf=1` (route authentifiée,
  aucune surface non authentifiée élargie) + **deux lignes de journal agrégées**
  à la cadence de 30 min de la trace `Heap 8bits:`, laquelle reste **intacte**
  (c'est la seule série longue du projet, 152 relevés).
- **Coût, énoncé sans arrondi** : le plan annonçait un « coût net nul » —
  **c'était faux**, `heapFree8Bit()` étant O(régions) et non O(blocs). Réel :
  **1 parcours O(blocs) par échantillon avant, 2 après**, soit **1 parcours par
  seconde — exactement le plafond** du gate, sans marge. Verrouillé par
  `static_assert` : ni cadence accélérée, ni 3ᵉ masque à parcours complet.
- ⚠️ **Ce lot NE CORRIGE RIEN.** Aucune réserve DMA n'est posée, aucune garde
  n'est modifiée, aucun seuil ne bouge : la métrique du refus 503, de la garde
  d'ouverture de fichiers, du parachute OOM et des gardes TLS **reste `0x0804`**.
  La pose de la réserve et la bascule de métrique — **dans le même lot**, sans
  quoi la garde 503 serait aveuglée — sont `feature-123`.
- **MQTT** : une seule méthode publique ajoutée, `lastDiscoveryMarkMs()`, qui ne
  sert qu'à composer le bit de contexte « rafale de découverte ». **Aucun topic,
  aucune charge utile, aucun comportement MQTT modifié.**
- **Tests** : 2 suites natives neuves (`test_native_heap_caps_table`,
  `test_native_heap_floors`) — la table de sélection `(all_caps & mask) == mask`
  mise sous test à 12 cas, le `fetch_min` prouvé par **entrelacement scripté
  déterministe** (il a désormais trois écrivains), la médiane et son biais, et des
  **sentinelles de câblage** : aucune garde ne lit les nouveaux planchers, la
  route ne lit que des atomiques rangées, les octets de contexte restent des
  `uint8_t`. Total mesuré : **44 suites / 1 677 cas**, zéro régression.

### Documentation

- **Nouvel [ADR-0077](docs/adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md)**
  — « **une métrique de garde doit exclure les pools qu'un `malloc()` ne peut pas
  atteindre** ». C'est la **troisième** occurrence du même piège dans ce dépôt :
  le parachute anti-OOM logé en PSRAM (donc décoratif), les quatre gardes rendues
  mortes par une métrique IRAM-inclusive
  ([ADR-0058](docs/adr/0058-gardes-heap-metrique-8-bits.md)), et maintenant la
  réserve DMA **visible par `0x0804`**. C'est le **motif** qui est figé, plus que
  le masque `0x1804` lui-même. L'ADR **ne bascule pas** la métrique : il énonce le
  principe et fige la table (épinglée en compilation) ; la bascule sera sa
  conséquence, en C-1b/C-3.
- `docs/subsystems/system-health.md` : table des cinq masques et de ce que chacun
  voit, résolutions temporelles à ne pas confondre, coût réel, biais de la
  médiane, avertissement « `pred` ne porte aucune information d'exécution », les
  deux chemins de lecture et leur motif chiffré, et le **protocole de relevé sur
  cible**.
- `docs/API.md` : paramètre `?hf=1`, clés de l'objet `hf`, les **deux budgets
  mesurés** (964 o sans le paramètre — identique **à l'octet près** à
  l'avant-lot — et 1 694 o avec), et la distinction entre les deux sortes de
  `null` (absence de mesure vs masque incapable de rendre un bloc).
- `docs/subsystems/mqtt-manager.md` : `lastDiscoveryMarkMs()` et son unique
  usage.
- `docs/subsystems/logger.md` : les **deux** nouveaux émetteurs périodiques
  (≈ 0,55 Ko/h) et pourquoi la ligne `Heap 8bits:` n'a **pas** été élargie.
- **Aucune documentation utilisateur touchée** (`docs/user/`) : rien de visible à
  l'écran, aucune capture d'écran à refaire, aucun PDF à régénérer.

---

## [2.48.1] - 2026-09-08

> **Ce qui change pour vous, en une phrase** : **le journal système survit
> désormais à une coupure de courant survenue pendant sa rotation** — avant
> cette version, les dernières heures de journal étaient perdues, définitivement,
> et c'est précisément la trace dont on a besoin pour comprendre la coupure.

### Firmware

- **Reprise d'une rotation de journal interrompue** (`bug-099`,
  [ADR-0076](docs/adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md)). La
  rotation de `/system.log` exécute `remove()` **puis** `rename()` : **deux
  opérations LittleFS non atomiques entre elles**. Une coupure entre les deux
  laissait le journal **absent** et son fichier temporaire porteur de l'**unique
  copie** des 6 144 derniers octets — que rien ne reprenait. Au démarrage,
  `Logger::reconcileLogTmp()` réconcilie désormais les deux fichiers :
  - journal **absent** + temporaire présent → le temporaire est **promu** (le
    journal est récupéré), avec un `WARNING` explicite ;
  - journal **présent** + temporaire présent → le temporaire est **effacé** (la
    rotation se rejouera), avec un `WARNING` distinct ;
  - pas de temporaire → **rien, et aucune ligne de journal** (premier démarrage
    compris) ;
  - mémoire insuffisante → **abstention** : ni promotion, ni effacement,
    silencieuse.
- **La réconciliation est ancrée en tête de `setPersistenceFs()`**, et l'ordre
  n'est pas cosmétique : l'ouverture du marqueur `--- DÉMARRAGE ---` est en mode
  `"a"`, donc elle **crée** `/system.log`. Un bloc déplacé après elle rendrait la
  promotion **inatteignable** et **détruirait le survivant en silence**.
- **Nouveau module pur** `src/logger_logic.{h,cpp}` — `evaluateLogTmpRecovery()`
  (l'**intention**, avant tout effet de bord) et `classifyLogTmpOutcome()` (le
  **fait**, d'après le code de retour du filesystem) : **30 cas** natifs
  (`test/test_native_logger_logic/`). La présence du fichier temporaire n'est
  **jamais sondée** — elle se déduit du code de retour, ce qui évite une seconde
  ouverture de fichier.
- **Commentaire de rotation corrigé** : il annonçait « 32KB / 24 derniers KB »
  face à des constantes valant **8 192** et **6 144** — **faux d'un facteur 4**
  pendant des mois. Réécrit **par référence aux constantes**, sans aucun chiffre
  en dur. La documentation, elle, ne portait pas cette erreur (vérifié).
- ⚠️ **Ce que ce correctif n'apporte PAS.** `bug-099` avait été ouverte pour un
  motif **budgétaire** : deux blocs LittleFS immobilisés sur une partition
  `history` de 64 Ko. Le motif était réel — mesuré à **8 176 o** de plafond perdu
  et **73 tailles refusées sur 124** dans la bande réaliste. Sous le layout v5
  ([ADR-0068](docs/adr/0068-partitions-layout-v5-flash-8-mo.md), 256 Ko) :
  **zéro refus**. **La conséquence budgétaire est dissoute** — ce que la
  requalification du 06/09 avait vu juste, et ce que la spec n'avait pas vu. Ce
  lot ne vaut **que** par la perte de journal.
- ⚠️ **Non validé sur matériel.** Ni la promotion ni l'effacement n'ont tourné
  contre un LittleFS réel ; la sémantique de `rename` quand la cible existe n'est
  jamais testée. La reprise est **idempotente** (un appel échoué laisse le
  fichier temporaire intact, le démarrage suivant réessaie), et son inférence
  « échec de l'appel filesystem ⇒ pas de fichier temporaire » reste une
  **hypothèse** — acceptable parce qu'elle ne pilote que la journalisation,
  jamais un effacement.

### Documentation

- **Nouvel [ADR-0076](docs/adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md)**
  — réconciliation du `.tmp` de rotation au démarrage : contrainte d'ordre
  imposée à tout futur appelant de `setPersistenceFs()`, choix de la classe de
  contention B, abstention silencieuse, et les quatre alternatives écartées
  (effacement aveugle, sonde du `.tmp`, classe A, remplacement atomique renvoyé à
  une spec de suite).
- `docs/subsystems/logger.md` : nouvelle section « Reprise d'une rotation
  interrompue au démarrage » (tableau des états, pourquoi l'effacement aveugle
  est faux, pourquoi le `.tmp` promu est complet, contrainte d'ordre, niveaux de
  log et silence nominal, ce qui n'est pas prouvé).
- `docs/subsystems/acces-fichiers.md` : **compte des sites `fopen` réconcilié**.
  Le document annonçait « 13 sites » en quatre endroits et « 14 » en trois
  autres, alors que le décompte par classe donnait 13. Recompté depuis les
  tableaux : **14 sites = 10 (A) + 4 (B)**, le nouveau étant la sonde de
  `reconcileLogTmp()`. Toutes les références `logger.cpp:NNN` — qui avaient
  **toutes glissé** — sont renumérotées, ainsi que celles de `history.cpp`,
  `main.cpp` et `web_routes_data.cpp`. Nouvel **invariant de la classe B**
  consigné : elle parie sur « appelé au boot », pari que le site voisin (classe A,
  trois lignes plus loin) refuse explicitement de faire.
- `docs/subsystems/history.md` : bandeau d'accès fichiers recompté (5 sites, plus
  6, et gardes annoncées posées) ; les **deux** chemins appelants de
  `setPersistenceFs()` sont documentés — `history.cpp:332` (tripwire, où
  l'abstention est le comportement **normal** puisque l'admission a déjà été
  refusée) et `history.cpp:377` (nominal) — avec la garantie que la
  réconciliation précède la première admission d'écriture (`historySaveAdmit`).

---

## [2.48.0] - 2026-09-07

> **Ce qui change pour vous, en deux phrases** : **restaurer un historique
> sauvegardé fonctionne enfin, quelle que soit sa taille**. Avant cette version,
> au-delà d'environ 145 mesures l'appareil refusait l'envoi **sans rien
> expliquer** — et le contournement que documentait ce projet, « découper en
> plusieurs envois », **détruisait les données** au lieu de les cumuler.

> **Et, dans le même lot** : l'appareil accepte désormais **cinq fois moins** de
> données d'un visiteur **non identifié** avant de l'avoir authentifié.

### Fonctionnalités

- **La restauration d'historique n'a plus de taille maximale.** Paramètres →
  Avancé → « Historique des mesures » : l'interface **découpe elle-même** l'envoi
  et affiche une **barre de progression**. Le cas réel qui a motivé le correctif :
  le 05/09, après le changement de partitions, **245 mesures** devaient être
  réimportées et **145 seulement** sont passées — les 100 plus anciennes (23 au
  31 août) sont restées dans le fichier.
- **Deux boutons au lieu d'un, et la différence est écrite.** « **Remplacer
  l'historique** » (rouge) efface l'existant et le reconstruit depuis le fichier ;
  « **Compléter l'historique** » conserve l'existant et ne remplace que les
  périodes communes. Envoyer **deux fois** le même fichier en « Compléter » donne
  exactement le même résultat qu'une fois : **aucun doublon**.
- **Une coupure réseau en cours d'import ne fait plus perdre le travail.** Les
  paquets déjà arrivés **restent** dans l'appareil, un bouton « **Reprendre
  l'import** » repart du paquet interrompu, et « **Abandonner la reprise** »
  renonce aux paquets restants **sans rien effacer**.
- **Tant qu'une reprise est en attente, « Remplacer » et « Compléter » sont
  grisés.** Ce n'est pas une gêne, c'est la protection : cliquer « Remplacer »
  à cet instant effacerait précisément l'import partiel qu'on cherche à
  récupérer. Une confirmation aurait laissé ce bouton à **un clic de distance**
  dans l'état où l'on est le plus pressé et le moins attentif — juste après une
  coupure. Le geste est rendu **impossible**, pas seulement déconseillé.
- **Un refus n'est plus muet.** Un envoi hors gabarit reçoit un message
  intelligible (au lieu d'une page d'erreur nue du serveur), l'interface annonce
  la limite **avant** d'envoyer, et toute éviction de mesures anciennes est
  **rapportée et affichée**.
- ⚠️ **Rechargez la page (Ctrl-Maj-R) après la mise à jour du filesystem.** Une
  interface restée en cache envoie l'historique **en une seule fois** et se
  heurte au nouveau plafond, abaissé : elle reçoit un refus. Le message d'erreur
  le dit explicitement.

### Firmware

- **`POST /history/import` gagne un mode `append`** (`bug-101`). Le champ `mode`
  vaut `"replace"` **par défaut** — donc **aucun client existant ne change de
  comportement** —, et toute valeur inconnue retombe sur `"replace"` plutôt que
  sur une fusion silencieuse. La réponse porte désormais `{status, count, total,
  dropped}`.
- **Règle de collision : le point entrant écrase**, à `(timestamp, granularité)`
  identique, **toutes granularités RAW comprise** et **toutes** les occurrences du
  créneau. Ce n'est pas un détail : `dedupAggregatedPoints()` **ignore les points
  RAW** (`history.cpp:1248`) alors qu'un export en porte jusqu'à **96** — un lot
  RAW réenvoyé **doublait en silence**. Et écraser *toutes* les occurrences est ce
  qui préserve l'idempotence face à un doublon hérité d'une version ≤ 2.18.0, que
  la déduplication aurait sinon **moyenné** avec le point neuf. C'est l'idempotence
  qui rend le bouton « Reprendre » sûr.
- **Fusion en place, zéro tampon supplémentaire** : recherche linéaire dans le
  buffer (~15 000 comparaisons de scalaires par lot au pire), compromis
  délibérément inverse d'une `std::map` — sur ce chemin la ressource rare est la
  **mémoire**, pas le CPU. Le seul contigu reste le vecteur existant, 339 × 28 =
  **9 492 o**, verrouillé par `static_assert`.
- **`trimToCaps()`** borne le nouveau mode granularité par granularité, en
  évinçant les **plus anciens**. Les deux sites de bornage existants
  (`recordDataPoint` pour RAW, pas 4 de `consolidateData` pour les agrégats)
  restent **inchangés**.
- ⚠️ **`dropped` compte les points ÉVINCÉS DE L'HISTORIQUE, jamais les entrants
  refusés.** Importer 20 points récents dans un historique plein rend
  `dropped = 20`, et ce sont **20 points anciens déjà présents** qui partent. Un
  client qui traduirait ce compteur par « mesures de votre fichier rejetées »
  mentirait à l'utilisateur.
- **Les plafonds de corps JSON sont abaissés, par route** (`bug-113`, **close par
  ce lot**). Trois routes tamponnent le corps entier **avant toute
  authentification** : `/history/import` **8 192 o**, `/wifi/connect` **1 024 o**,
  `/sensors/onewire/identify` **256 o**. Total réclamable sans identifiants :
  **9 472 o** contre 49 152 auparavant, soit **−80,73 %**. Les trois valeurs et
  leur total sont **assertés à la compilation** — une révision casse le build au
  lieu de périmer la documentation. Le flux de **premier démarrage** est préservé :
  la charge WiFi nominale pèse **58 o**, **595 o** au pire cas tout échappé, sous
  le plafond de 1 024.
- **Nouvelle garde `registerJsonBodyGuard()`** (patron ADR-0067) : elle rend un
  **413 en JSON maison** (au lieu du 413 nu de la bibliothèque) et un **503 +
  `Retry-After: 5`** en famine mémoire, sur la métrique **8 bits** et le seuil
  **existants**. Elle ne journalise pas et ne construit aucune `String` — elle
  tourne sur `async_tcp`, où un `WARNING` déclencherait le flush synchrone du
  journal, donc l'`abort()` même qu'elle prévient. **Son mécanisme est l'ordre
  d'enregistrement** : elle doit précéder le handler gardé, sans quoi elle est
  inerte **sans rien casser visiblement**.
- ⚠️ **Le motif du plafond bas a changé, pas la décision.** La prémisse « un gros
  tampon part dans la RAM interne affamée » est **fausse** au-delà de 4 095 o —
  établi **par désassemblage** : `calloc` préfère la **PSRAM**. Sans plafond, la
  sûreté de ce chemin reposerait **entièrement sur une préférence d'allocateur**
  dont le repli interne est le contre-exemple. **Corollaire** : découper **plus
  fin** ramènerait l'allocation **dans** le tas affamé — le budget de lot client
  (**7 168 o**) est **au-dessus** de 4 095 à dessein, et un `static_assert`
  l'interdit de descendre.
- **Découpage client par octets mesurés, jamais par nombre de points** : un point
  pèse de **110** à **149 octets** selon les décimales rendues par
  `JSON.stringify` (mesuré le 2026-09-07 en exécutant `parseHistoryCsv` sur des
  fixtures à 17 chiffres significatifs). `constants.h` retient **157 o** comme
  **borne conservatrice, nommée comme telle** — pas comme une mesure. Un découpage
  « par 145 points » aurait repassé le plafond au premier fichier à décimales
  longues.

### Tests

- **Suite neuve `test/test_native_history_import/`** (73 cas) : natif passe de
  **40 suites / 1 535 cas** à **41 suites / 1 608 cas**, 100 % vert. `pio run`
  après `-t clean` : SUCCESS, **6 warnings tous préexistants**, Flash **49,6 %**.
- **Campagne de mutation : 10 mutants, 10 morts, 0 survivant.** Dont le plus
  important, ajouté par le `test-engineer` : « **garde enregistré après son
  handler** » — c'est **celui qui prouve l'ordre**, et sans lui un test se
  contentant de constater la *présence* de l'appel serait resté vert sur une garde
  totalement inerte.
- **Une sentinelle verrouille la *méthode* de découpage, pas son chiffre.**
  `chunkHistoryPoints()` doit **mesurer** les octets de chaque point
  (`jsonByteLength(JSON.stringify(point))`) et non diviser le budget par un poids
  supposé. Les trois mutants correspondants meurent, y compris celui qui met
  l'appel réel **en commentaire** en laissant sa mention visible — le piège exact
  qui avait rendu décorative la sentinelle de `test_native_temp_compensation` le
  matin même.
- **Fixtures synthétiques et déterministes** (`test/fixtures/*.csv`, régénérables
  par `tools/gen_history_fixture.py`, PRNG à graine fixe). La sauvegarde réelle du
  05/09 a servi à **calibrer les formes**, jamais à les remplir : elle est restée
  **hors dépôt**, car elle porte l'**IP du broker MQTT** et le **SSID**. Un test
  verrouille cette règle.

### Documentation

- **Le « poids d'un point » corrigé pour la quatrième fois — et cette fois avec
  sa méthode.** Le chiffre existait en deux versions divergentes (148 o dans les
  notes de test, 149 o dans `constants.h`). Tranché en **exécutant**
  `parseHistoryCsv()` et `chunkHistoryPoints()` : **148 o** est le pire cas de la
  fixture, **105-109 o** un point typique, et le maximum **théorique** vaut
  **172 o** — 97 o de squelette de clés plus trois nombres de 25 o, 25 étant la
  longueur JSON maximale d'un `double` (`-0.0000012345678901234567`), atteinte
  dès qu'une valeur passe sous `1e-6` : un **ORP qui traverse zéro**, une
  **température proche de 0 °C**. Conséquence : les **157 o** que `constants.h`
  annonçait comme une « borne conservatrice » **n'en étaient pas une** — le
  chiffre avait été emprunté au pire cas de l'**export firmware**, un autre
  format, et 172 le dépasse. Rien n'a jamais cassé parce que le découpage
  **mesure** au lieu de diviser ; l'historique des quatre chiffres faux est
  désormais conservé sur place, dans le commentaire même, pour qu'ils ne
  reviennent pas (ADR-0073).

- **Nouvel [ADR-0075](docs/adr/0075-decoupage-client-contre-streaming-firmware.md)** :
  « La restauration d'historique est découpée par le CLIENT, pas streamée par le
  firmware ». Il porte la **résolution de la contradiction** entre `bug-101` (plus
  de capacité) et `bug-113` (moins) — le découpage client permet d'**abaisser** le
  plafond et satisfait les deux. **Streaming firmware écarté, et le motif n'est
  pas la flash** (1,9 Mo libres) : c'est la machine à états de parsing JSON
  incrémental sur `async_tcp`, sur des fragments qui coupent **au milieu d'un
  nombre**. Le dépôt a **déjà payé ce prix exact** sur le chemin **sortant** — le
  pied `],"count":N}` de `/get-history` n'était **jamais émis**, donc **JSON
  tronqué sur toutes les réponses pendant plusieurs versions**, et la sauvegarde
  réelle du 05/09 en porte encore la cicatrice (sa clé `count` apparaît **deux
  fois**, `0` puis `245`). **Format compact écarté** aussi : diviser par 1,8
  laisserait le pire cas à ~28 Ko — **encore** le plateau — et créerait un
  **troisième** format d'historique.
- `docs/API.md` : contrat de `POST /history/import` **refait** — plafond **8 192**
  (il annonçait encore 16 384), champ `mode`, réponse `{status, count, total,
  dropped}` avec la **sémantique exacte de `dropped`** et son contre-exemple,
  413/503 **en JSON maison**, exemple `curl` corrigé (le corps est un **objet**,
  pas un tableau nu). Plafonds ajoutés sur `/wifi/connect` et
  `/sensors/onewire/identify`.
- `docs/subsystems/history.md` : `appendData()` / `trimToCaps()` — règle de
  collision et son motif (écrasement contre moyenne), fusion en place, atomicité,
  et les **deux défauts ouverts** de ce chemin.
- `docs/subsystems/web-server.md` : `registerJsonBodyGuard()` — l'ordre
  d'enregistrement comme mécanisme, le filtre **sans allocation**, la
  correspondance d'URI **souple** (une égalité stricte aurait laissé
  `/history/import/` filer jusqu'au 413 nu), et le message 413 **générique** parce
  qu'il sert trois routes.
- `docs/features/page-settings.md` : les **quatre** boutons, le verrou de reprise
  et son motif, la formulation exacte du message d'éviction.
- `docs/UPDATE_GUIDE.md` : l'étape « réimporter l'historique » est **refaite**.
  Elle portait encore le contournement destructeur (« lots de 145 points, les lots
  se cumulent ») — **troisième** endroit où il vivait, après `docs/API.md` et ce
  fichier.
- `docs/user/src/guide-utilisation.html` : section « Restaurer une sauvegarde
  d'historique » — les deux modes, l'absence de taille maximale, la reprise après
  coupure, le verrou, et ce que `dropped` veut dire en langage d'utilisateur.
  📸 **Capture à refaire** : `screenshots/10_Paramètres_8_Avancé_1.jpg` (la carte
  « Historique des mesures » montre désormais **quatre** boutons au lieu d'un).

### Reste à valider sur l'appareil

Le critère d'acceptation est **on-target** et comporte une étape que seul l'humain
peut jouer : **coupure Wi-Fi au 3ᵉ lot sur 6**, historique **partiel et
cohérent**, reprise **sans doublon**. À relever en même temps : `psram_free` et
`max_alloc_8bit` **avant/après** — l'invariant est que l'**interne ne bouge pas de
plus de 9 492 o** et que la **PSRAM absorbe** les tampons de corps. C'est la
mesure qui **confirmera sur l'appareil** le désassemblage de l'allocateur.

---

## [2.47.1] - 2026-09-07

> **Ce qui change pour vous, en deux phrases** : la durée d'injection annoncée
> par l'interface utilise enfin le **débit que le firmware applique vraiment**.
> Et le garde-fou « mesure de débit interrompue » — qui refuse d'enregistrer un
> débit relevé sur une marche avortée — cesse d'être **neutralisé** quelques
> secondes après chaque chargement de page.

> **Et, dans le même lot** : l'appareil **dit** désormais quand sa sonde de
> température d'eau ne bouge plus, sans rien changer à la régulation — parce que
> le chiffrage a montré que cela ne coûte **qu'un millième de pH**. En revanche,
> l'**offset de calibration de température** est enfin **borné** : c'était lui,
> et pas la sonde, qui pouvait fausser le pH sans limite.

### Fonctionnalités

- **L'assistant « Mesurer le débit » annonce le bon volume.** Sa ligne de budget
  affichait « **≈ 61 mL par purge** » alors que la pompe en délivre **74** : un
  volume **sous-estimé de 18 %**, que l'opérateur validait avant de lancer la
  marche (recommandation du visa `pool-chemistry`). L'assistant lit désormais ses
  paramètres — durées de purge et de mesure, purges restantes, débit majorant —
  sur une lecture **forcée** à son ouverture, au lieu de retomber sur des valeurs
  recopiées en dur dans le navigateur (60 s / 120 s / 3 purges).
- **La carte de l'assistant s'ouvre immédiatement** et signale l'attente là où
  elle porte : « lecture des paramètres en cours… » sur la ligne de budget, avec
  « continuer » **neutralisé** le temps de la réponse — impossible d'avancer sur
  des durées de repli.
- **L'appareil signale une sonde de température immobile.** Quand la sonde d'eau
  rend **900 lectures d'affilée** dans une bande de **0,03 °C** — soit environ
  **30 minutes** sans le moindre frémissement, ce qu'une sonde vivante ne fait
  pas —, un badge orange « **Sonde température figée** » apparaît sur la carte du
  tableau de bord **et** sur la page Température, et une entité Home Assistant
  passe à « problème ». **Rien d'autre ne change** : le dosage, la compensation
  de température du pH et le planning de filtration continuent exactement comme
  avant. C'est un signal de **maintenance** (remplacer le capteur), pas une
  alarme de sécurité — l'écart de pH en jeu est de **0,001**, à comparer aux
  **0,01** de la bande morte la plus serrée de la régulation.
- **« Sonde eau non identifiée » ne se dit plus comme « eau figée ».** Deux états
  distincts, deux messages distincts : le premier s'adresse à la page
  d'identification des sondes, le second au remplacement du capteur. Et une sonde
  **débranchée** en cours d'épisode fait **retomber** l'indication au lieu de la
  laisser affichée sur un état périmé.
- **Une calibration de température aberrante est ramenée à ±5 °C.** Saisir une
  référence très éloignée de la mesure ne produit plus un décalage sans limite :
  l'appareil borne la valeur, la **journalise** (valeur demandée, valeur retenue,
  adresse du poste) et — c'est le point important — **affiche celle qu'il
  applique**. Une valeur héritée d'une version antérieure est corrigée **au
  démarrage**.

### Firmware

- **`flow_assist` est publié sur la trame WebSocket** (`bug-082`, **5ᵉ récidive**
  du motif de parité `/data` ↔ WebSocket). L'objet n'existait que dans
  `GET /data`, alors que la trame WS **remplace en entier** l'état capteur du
  client toutes les 5 s : ses champs repassaient à `undefined` en quelques
  secondes, **en permanence**.
- **Ce n'était pas cosmétique : `aborted` est un garde-fou de dosage**, et il
  était **mort en pratique**. Un débit mesuré sous-estimé agit **deux fois dans
  le même sens** : le calcul d'impulsion **allonge le pouls** (`pulseMs =
  periodMs × meanFlow / ampFlow`, donc sur-injection **réelle**) *et* le crédit
  de volume injecté utilise **le même** débit, donc le total du jour
  **sous-compte** et la **limite journalière est desserrée du même facteur**. La
  sécurité volumique se ment à elle-même. Le filet en aval n'est que partiel
  (bornes `0 < débit ≤ 200`).
- **Deux champs seulement en parité** : `aborted` et `flow255` (**tableau de 2**,
  jamais un scalaire — un scalaire retomberait silencieusement sur le repli). Les
  **sept** champs de session restent servis par `/data` seul, avec la lecture
  forcée à l'ouverture de l'assistant comme contrepartie.
- **Garde préventive** sur le hook d'expiration de session : il est appelé
  **aussi** sur les trames WS et ne survivait **que** parce que `flow_assist` en
  était absent. Sans elle, une publication future du drapeau de session ferait
  lire **chaque trame** comme une expiration — retour à la première étape et
  purges relancées, **éventuellement pompe en marche**.
- **Le repli client 61 / 67 mL/min reste distinct de 74, délibérément.** Sur un
  firmware ancien la pompe débite réellement 61 / 67 : annoncer 74 raccourcirait
  l'estimation de **18 à 21 %** sous la marche réelle, l'utilisateur croirait à un
  défaut et **relancerait** — la sur-injection venant de la répétition. Et sur un
  firmware à jour, aligner le repli rendrait la **présence** du champ
  indiscernable de son **absence** : une 6ᵉ récidive serait muette. **Le repli
  distinct est le détecteur.**
- **Budget de trame, méthode consignée** ([ADR-0073](docs/adr/0073-chiffre-consigne-porte-sa-methode.md)) :
  **70 o** au pire cas structurel, obtenus par **balayage exhaustif des 2³¹
  motifs de bits** de `float` finis ≥ 0 sérialisés par l'ArduinoJson du projet
  (longueur max **12 o**, `1.40129846e-45` — les 7 chiffres significatifs sont
  repris par l'exposant) ; **50 o en marche**. Trame **3 447 + 70 = 3 517 o**,
  marge **67 o** sous `kWsFrameBufBytes = 3584` (elle était de 137 o). La réserve
  de 49 o (`ph_integral`, `ph_integral_term`, sans consommateur client depuis
  feature-092) **n'est pas dépensée**. ⚠️ Le **3 447 hérité n'a pas été
  re-dérivé** : c'est exactement le genre d'héritage qui a produit quatre
  chiffrages faux pendant feature-092.
- **`bug-114` a changé de cible en cours de route, et c'est le résultat le plus
  utile du lot.** Le défaut visé était la **sonde** ; le chiffrage a montré
  qu'elle était **déjà bornée** — lectures filtrées à `(−55 ; 125)` °C avec rejet
  de `85.0` (power-on reset) et de `DEVICE_DISCONNECTED_C`
  ([`src/sensors.cpp:324-326`](src/sensors.cpp)), erreur plafonnée à
  0,05-0,07 pH **même aux bornes**. Le trou réel était l'**offset de
  calibration**, écrit **sans aucune borne** : **+1000 ⇒ `RT,1025.0` ⇒ ≈ +0,15 pH**,
  et un offset très **négatif** fait tendre la température de compensation vers
  **0 K**, où l'erreur n'est **plus bornée du tout**. C'est une entrée de
  **configuration**, donc **durable en NVS**.
- **Écrêtage de l'offset à ±5 °C** (`kTempCalOffsetMin/Max`). Motif : un DS18B20
  dérive de **dixièmes** de degré, pas de degrés. **QUATRE points de clamp**, et
  aucun n'est un doublon — ils couvrent quatre **provenances** distinctes :
  écriture (`handleSaveConfig`), **chargement NVS** (`loadMqttConfig`), émission
  de la valeur de **mesure** (`_readDs18b20s` — affichage, MQTT, historique) et
  **émission `RT`** (`getWaterTemperature` — le seul site qui alimente la commande
  envoyée à la sonde). **Ne pas fusionner les deux derniers.** Le clamp au
  chargement a un motif nommé : sans lui, `/get-config` afficherait une valeur que
  le firmware **n'applique pas**, et la règle du dépôt « toujours lire
  `/get-config` avant tout diagnostic chiffré » deviendrait un **piège** au lieu
  d'une protection — un diagnostic du 06/09 avait déjà déraillé là-dessus.
- **Fenêtre `[0 ; 40] °C` pour la température envoyée dans `RT`**, avec un
  **repli en cascade** : valeur courante plausible → **dernière** valeur plausible
  si son âge est sous la fenêtre stale (`kSensorStaleTimeoutMs` = 20 s) →
  `kEzoFallbackTempC` = **25 °C**. `warning` à **chaque** substitution (immédiat à
  la transition, rappelé toutes les 60 s tant qu'elle dure), `critical` si le
  repli au défaut **dure** plus de 10 min. Coût maximal de ce repli : **±0,01 pH**.
  ⚠️ **La borne de 40 °C est une borne de PRUDENCE, NON CONFIRMÉE par la datasheet
  Atlas** : la plage réellement admise par `RT` n'a **aucune** trace dans le dépôt
  (recherche du 07/09). Elle est signalée comme telle partout où elle est citée.
- **⛔ « Suspendre la compensation » n'existe pas, et c'est écrit dans le code.**
  Ne pas envoyer `RT` ([`src/atlas_ezo.cpp:149-155`](src/atlas_ezo.cpp)) fait
  retomber la lecture sur `R` : le module **conserve sa dernière température
  interne**, silencieusement périmée — **invisible et indatable**. Pire que
  n'importe quel repli. La cascade rend **toujours** une valeur, et l'étage qui
  l'a produite est **toujours** exposé.
- **`tempFrozenAlarm`** en **parité stricte** `/data` ↔ WebSocket (**+24 o** au
  pire cas structurel, marge résiduelle **43 o** sous `kWsFrameBufBytes = 3584`),
  **`<base>/temp_sensor_frozen`** en retain + `binary_sensor` HA
  `device_class: problem`, et **`ezo_comp_t` / `ezo_comp_stage`** sur le topic de
  diagnostic (**+71 o**, marge **408 o**) — la température **effectivement
  envoyée** dans `RT` et l'étage de la cascade qui l'a produite. Comparés à
  `temperature`, ce sont les **seuls** champs qui distinguent une valeur figée,
  un repli et un offset mal réglé.
- **Maintien de 60 s à la PUBLICATION MQTT uniquement** (`kMqttTempFrozenHoldMs`).
  La sortie de l'état figé est **immédiate côté firmware, sans hystérésis**
  (décision du visa : le drapeau est informatif, un battement ne coûte rien
  chimiquement, et une hystérésis ajouterait un second seuil à régler) — mais une
  entité HA qui clignote est inutilisable en automatisation. `/data`, la trame WS
  et l'UI voient la valeur **instantanée**.
- **Le planning de filtration était DÉJÀ immunisé — zéro ligne écrite.**
  [`src/filtration.cpp:86`](src/filtration.cpp) (NaN ⇒ planning conservé), `:89`
  (bande morte de 1 °C ⇒ une température **figée** ne recalcule **rien**) et
  [`src/schedule_logic.cpp:64-67`](src/schedule_logic.cpp) (durée clampée à
  `[1 h ; 24 h]`). Pour le planning, « statu quo » et « fail-closed » sont
  **identiques en fait**.

### Tests

- **La sentinelle de parité déclarait légitime le bug qu'elle devait signaler.**
  Livrée le **matin même** ([ADR-0071](docs/adr/0071-parite-data-trame-capteurs.md)),
  elle portait `flow_assist` comme exception `/data`-seule. Quand le firmware a
  corrigé la parité, elle est passée **rouge d'elle-même** : son mécanisme
  « **exception morte = échec** » s'est déclenché pour la **première fois**, et son
  message disait quoi faire.
- **Second axe, imbriqué** : le contenu de `flow_assist` est désormais diffé des
  deux côtés (receveur `faObj`, extracteur unique), avec sa propre table
  d'exceptions (7 champs de session, **motif écrit obligatoire**, **cliquet à
  7**) et **deux champs formellement non exceptables** — `aborted` et `flow255`.
  **16 cas** dans `test_native_ws_data_parity` (7 de plus), zéro régression.
- **Propriété vérifiée par contre-mesure** : retirer `aborted` de la trame, **le
  déclarer** en exception **et relever le cliquet** produit **trois** échecs. On
  ne peut donc plus détourner la garde pour légitimer le bug.
- **`test_native_temp_compensation` — nouvelle suite** (clamp, fenêtre, cascade,
  libellés, non-régression, sentinelles structurelles). Parc porté à
  **40 suites / 1 535 cas** (mesuré le 2026-09-07 par
  `ls -d test/test_native_* | wc -l` et `grep -rn 'RUN_TEST(' test/ | wc -l`),
  zéro régression.
- **13 mutants, 12 tués, 1 équivalent PROUVÉ.** Le survivant est la garde `NaN`
  de la fenêtre de plausibilité : IEEE-754 rend `NaN >= borne` **faux**, donc la
  retirer ne change **aucun** comportement observable. Ce n'est vrai que sans
  `-ffast-math` — **vérifié absent de `platformio.ini`** —, et un test échouerait
  s'il y était ajouté.
- **La sentinelle des quatre points de clamp était satisfaite par un
  COMMENTAIRE.** Sa première version comptait les occurrences textuelles de
  `clampTempCalOffset` au niveau du **fichier** : la mention
  `// bug-114 : clampTempCalOffset()` de [`src/config.cpp:5`](src/config.cpp)
  suffisait, et **supprimer l'appel réel laissait la suite verte**. Corrigée :
  elle lit désormais du code **dépouillé de ses commentaires** et vise chaque
  point **dans sa fonction**, par **signature** — donc insensible à la
  renumérotation. **Démonstration faite** : pour les **quatre** suppressions,
  l'ancienne sentinelle restait **verte**, la nouvelle **tue chaque cas**.
- **Le compte des points de clamp a été annoncé « deux », puis « trois », avant
  d'être grepé et fixé à QUATRE** — dans les commentaires mêmes qui devaient
  empêcher une simplification future. L'erreur venait du brief de l'agent
  principal et s'est **propagée** de là. Exactement le mécanisme de
  [ADR-0073](docs/adr/0073-chiffre-consigne-porte-sa-methode.md), consigné parce
  qu'il instruit.
- **Non-régression prouvée par sentinelles STRUCTURELLES**, pas par intention :
  `pump_controller.cpp` ne référence **aucun** des nouveaux symboles, `DoseInputs`
  ne porte **aucun** champ de température, et `computeAutoWindow` reste bornée
  `[60 ; 1440]` min même alimentée par −127 / −55 / 0 / 85 / 125 / 1025 °C.

### Documentation

- `docs/subsystems/ws-manager.md` : correction d'un chiffre **faux** — le gabarit
  annoncé était `StaticJson<3456>` pour **3584** ; nouvelle entrée de budget
  `bug-082` **avec sa méthode** ; marge courante **67 o** propagée partout où
  « 137 o » était cité ; second axe imbriqué de la sentinelle ; compteurs de clés
  **remesurés** (121 / 97 / 26 / 2).
- `docs/API.md` : la mention « pas de champ WS » de l'objet `flow_assist` était
  un **défaut, pas un choix** — corrigée, et nouvelle section décrivant le
  contrat WS (deux champs, `clé absente ≠ session finie`).
- `docs/features/page-settings.md`, `page-ph.md`, `page-orp.md`,
  `docs/subsystems/pump-controller.md` : lecture forcée à l'ouverture de
  l'assistant, parité du garde-fou, repli distinct.
- **[ADR-0074](docs/adr/0074-temperature-figee-statu-quo-instrumente.md) créé** —
  « **Une sonde de température figée ne suspend rien : statu quo instrumenté** ».
  Décision **invisible dans le code** (critère 2), dont le corps est le
  **chiffrage** : pente de Nernst, `≈ 0,7 millième de pH par °C` d'erreur de
  compensation, **+0,0011 pH** sur le cas de production, **≈ 15 °C** d'erreur
  nécessaires pour atteindre `kPhStopThreshold = 0,01` et **≈ 75 °C** pour 0,05.
  L'ADR **refuse** explicitement le fail-closed **et écrit le motif** :
  suspendre la régulation pH sur soupçon de sonde figée échangerait **0,001 pH**
  d'erreur de mesure contre une **sous-chloration réelle en quelques heures** — le
  pH monte, la fraction de HOCl chute (pKa 7,54 : **52 %** à pH 7,50, **41 %** à
  7,70), et la boucle ORP, qui **sature déjà son quota horaire de 10 min/h** sur
  cet appareil, ne peut pas compenser ; l'inhibition à 7,60 la **couperait** même.
  **La prudence apparente était du mauvais côté.**
- **Le refus documenté d'entrer dans `alerts/sensor_frozen`.** La sémantique
  **établie** de ce canal est « le dosage est compromis » : y ajouter un événement
  **sans effet sur le dosage** déclencherait des automatisations Home Assistant
  **existantes** — contrat externe cassé **sans bump de version**. La température
  garde son topic dédié ; qui veut les deux états s'abonne aux deux topics.
- `docs/subsystems/sensors.md` : cascade de résolution de `RT` (trois étages,
  journalisation, seuils), les **quatre** points de clamp et leurs motifs, la
  fenêtre `[0 ; 40]` avec son avertissement « **non confirmée par la datasheet** »,
  l'**interdit `RT`**, les accesseurs `getEzoCompensationTempC` /
  `getEzoCompensationStage` / `getEzoCompensationStageLabel` /
  `isEzoCompensationSubstituted`, et le détecteur figé requalifié
  « **warning-only par DÉCISION** ».
- `docs/API.md` : champ `tempFrozenAlarm` en **parité** `/data` ↔ WS, avec
  l'avertissement explicite « **ne pas l'utiliser comme condition de sécurité** ».
- `docs/MQTT.md` : topic `<base>/temp_sensor_frozen` (retain, dédup slot 28,
  garde « aucune sonde eau », maintien 60 s **à la publication seulement**),
  `binary_sensor` HA, champs de diagnostic `ezo_comp_t` / `ezo_comp_stage` avec
  leur budget, et la section « pourquoi la température n'entre pas dans
  `alerts/sensor_frozen` ».
- `docs/features/page-temperature.md`, `page-dashboard.md` : badge, **précédence
  des trois états** de la sonde eau, et pourquoi la condition s'appuie sur
  `sondes_identified` et **non** sur `temperature_raw` (WS-seul ⇒ le badge aurait
  **clignoté**).
- `docs/subsystems/ws-manager.md` : nouveau champ, budget **+24 o**, **marge
  résiduelle 43 o** — acceptable pour ce lot, **pas pour le suivant** : le prochain
  champ devra consommer la **réserve de 49 o** ou bumper `kWsFrameBufBytes` par
  **ADR**.
- **Aucun autre ADR créé** : la décision de fond du volet `bug-082` (sentinelle de
  parité et ses exceptions déclarées sous cliquet) est déjà **ADR-0071**, et le
  repli distinct **ADR-0050** ; ce cycle les **applique**, il n'arbitre pas
  d'alternative neuve.
- **Consigné comme ouvert** : `bug-119` — `loadSensorData()` ne **résout rien**,
  donc trois comportements de `verifyRunEnd()` sont morts. Le garde-fou `aborted`
  avait **deux morts indépendantes** ; ce lot en corrige **une**. Et
  l'avertissement de vraisemblance 20-35 mL/min reste **non bloquant**.

---

## [2.47.0] - 2026-09-07

> **Ce qui change pour vous, en deux phrases** : l'historique ne prétend plus
> qu'un dosage de **pH** a eu lieu quand c'était le **chlore**. Et une journée
> dont le compteur de filtration a été remis à zéro ne s'affiche plus comme la
> **meilleure de la semaine**.

### Fonctionnalités

- **Les graphiques du tableau de bord disent QUELLE pompe a dosé.** Sous les
  mini-graphiques pH et chlore, une ligne de repères marque les moments
  d'injection : **brun au rang du bas** pour le pH, **bleu au rang du dessus**
  pour le chlore, et un **simple contour gris** pour les mesures d'avant la mise
  à jour, dont l'appareil ne connaît plus la voie. L'infobulle nomme la ou les
  pompes concernées — la couleur ne porte jamais seule l'information. Le
  graphique de température n'a pas de repères : aucune pompe ne la corrige.
- **Une journée dont la durée n'a pas pu être mesurée l'avoue.** Jusqu'ici, une
  remise à zéro du compteur de filtration (restauration d'usine, restauration
  d'une sauvegarde de réglages) archivait la journée à **0 minute** — donc
  affichée « 0h00 » **en vert**, c'est-à-dire comme la **meilleure journée
  possible**. Elle se rend maintenant en **pointillé estompé avec un tiret**, et
  la note sous la rangée nomme la cause (« compteur de filtration remis à
  zéro »). Ce n'est ni une réussite ni un échec : c'est une **absence de
  mesure**.

### Firmware

- **Historique — le drapeau de dosage devient un MASQUE DE VOIES** (`bug-098`).
  À l'écriture, `phDosing` et `orpDosing` étaient **fusionnés** en une seule clé
  booléenne (`obj["d"] = phDosing || orpDosing`) ; à la relecture, cette clé
  était réaffectée à `phDosing`. Une injection de **chlore seul** rechargée
  depuis `/history.json` — donc **après chaque redémarrage** — se relisait donc en
  **dosage pH** : le firmware ne perdait pas l'information, il en **fabriquait
  une fausse**, puis `consolidateData()` la figeait dans les agrégats journaliers
  pour **75 à 90 jours**. Remplacé par un champ de bits `doseMask`
  (`kHistDosePh 0x01`, `kHistDoseOrp 0x02`, `kHistDoseUnknownChannel 0x04`), avec
  un **troisième état explicite** : un point ancien se relit « dosage, voie
  **indéterminée** », jamais « pH ».
- **⚠️ Changement de format — la clé `dosing` de `GET /get-history` est un
  ENTIER** (0..7 ; 0..4 en pratique), là où c'était un booléen. Un client qui la
  lisait en booléen JavaScript **ne change pas de verdict** (`0` *falsy*,
  `1`/`2`/`3`/`4` *truthy*) et la clé reste **toujours présente** — seul son type
  change. Les **deux** surfaces portent les **trois** bits : le fichier interne
  `/history.json` et la route HTTP, qui sert le masque **verbatim**.
- **Relecture tolérante aux deux formats, sans clé de version** : le type JSON
  suffit à les séparer (booléen ⇒ firmware ≤ 2.46 ⇒ voie indéterminée ; entier ⇒
  masque ; clé absente ⇒ aucun dosage). Le masque de relecture porte sur **`0x07`
  et non `0x03`** — avec `0x03`, le premier cycle sauvegarde/relecture suivant la
  mise à jour (**5 minutes**) écrivait `"d":4` puis relisait `0` : le mensonge
  réapparaissait, **réintroduit par le correctif lui-même**.
- **Rétrogradation sûre** : un firmware ≤ 2.46 relisant `"d":3` obtient `true`,
  soit **exactement son comportement actuel**. `"d":0` obtient `false`.
- **Le fichier d'historique RÉTRÉCIT** : `,"d":0` … `,"d":7` (6 o) contre
  `,"d":false` (10 o) et `,"d":true` (9 o), soit **−1 113 octets** sur la charge
  de référence de 339 points. Le pire cas d'un point exporté passe de 163
  (annoncés) à **161 o mesurés** — la marge sur le tampon de report **gagne 2 o**.
- **Agrégation et déduplication par union BIT À BIT** : un créneau horaire ou
  journalier cumule les voies réellement dosées, voie par voie. `anyTrue()` est
  **supprimée** de `history_logic` avec ses trois tests — c'est ce diff qui a tué
  son dernier appelant, et des assertions vertes sur du code mort font croire à un
  invariant vivant.
- **Temps d'atteinte de cible — `kTttFlagCounterReset` (`0x20`)** (`bug-106`).
  Quand le compteur de filtration recule, la durée archivée n'est **plus** un
  `0` muet : le bit dit « cette valeur n'est pas une mesure ». Il est posé aux
  **trois** sites de recul, dont le chemin **vif** (celui qui alimente
  l'affichage), que la spec ne nommait pas. Une **sentinelle dans le champ
  `minutes`** a été écartée : elle aurait recommis, dans le champ voisin, la faute
  que `bug-098` corrige.
- **Aucun chemin de dosage touché.** Ces deux drapeaux sont des champs
  d'observation : aucune décision de régulation, aucune limite de sécurité, aucun
  budget horaire ou journalier ne les lit. `pump_controller` est inchangé.
- **Limite assumée, consignée** : un `"dosing"` **flottant** (`3.0`) ou entier
  **64 bits hors plage** ne satisfait aucun des deux cas du discriminant et
  retombe sur `0`. Inatteignable sans édition manuelle du fichier — ni le
  firmware ni l'export ne produisent ces formes.
- **Trou résiduel connu, non corrigé** : un recul **puis** une remontée du
  compteur au-dessus du départ, **avec une coupure secteur entre les deux**, perd
  le drapeau (il n'est archivé qu'à la clôture de la journée). Atténué par le
  drapeau « reprise après coupure », qui signale la journée pour le mauvais
  motif. Motif du report : **usure de la flash** pour un scénario à trois
  conditions simultanées.

### Interface

- **Export CSV de l'historique — colonne `dosing_mask` AJOUTÉE**, à côté de
  `dosing` **conservée** en `0`/`1`. ⚠️ **Le nom est neuf à dessein** :
  réutiliser `dosing` aurait rendu un `1` **indécidable** — pH, ou voie
  inconnue ? — puisque les exports ≤ 2.46 écrivaient déjà `1` pour dire « il y a
  eu dosage ». À l'import, `dosing_mask` a **priorité** quand les deux colonnes
  sont présentes ; un fichier ancien reste importable et ses points se lisent
  « voie indéterminée ».
- **Marqueurs de dosage** : mini-graphiques pH et chlore du tableau de bord
  **seulement**. Les graphiques des écrans détail (1 point par jour) sont
  inchangés, et la carte Température ne trace aucun marqueur.

### Tests

- **39 suites / 1 451 cas natifs**, zéro régression. Nouvelle suite
  `test_native_dose_mask` (le masque de bout en bout : contrat de bits, trois
  formats de fichier, deux cycles consécutifs, union à l'agrégation, budget).
- **13 mutants tués sur 13**, dont le mutant décisif `& 0x07` → `& 0x03`.
  Couverture **lignes et fonctions à 100 %** sur les modules purs touchés.
- **Balayage exhaustif des 65 536 couples (masque, granularité)** : aucun ne fait
  sauter un point de l'export — le mode de défaillance **silencieux** (trou de
  courbe indiscernable d'une panne de sonde) est structurellement exclu. Le pire
  cas est le **maximum du balayage**, mesuré, pas postulé.
- `sizeof(DataPoint) == 28` prouvé **par mutation**, l'allocation contiguë de
  `/get-history` en dépend.
- **Réserve consignée** : le modèle `historyPointPersistedLen`
  (`test_native_history`) décrit encore `"d":true/false` et une partition de
  64 Ko (256 Ko depuis le layout v5). Il se trompe **du côté sûr** — majorant —
  donc rien n'est rouge ; le corriger propage sur une dizaine de constantes
  asservies et sur tout le récit de budget. **Spec de suivi nécessaire.**

### Documentation

- **[ADR-0072](docs/adr/0072-absence-de-valeur-hors-du-domaine.md) — « Le domaine
  d'une valeur ne porte jamais l'absence de valeur »** : l'idiome que ce dépôt
  appliquait **sans l'avoir nommé**. La règle était **déjà écrite deux fois, dans
  deux fichiers, comme deux remarques locales** (`history.h:23` « NaN = absence
  […] JAMAIS 0 » ; `time_to_target_logic.h:78` « CHAMP DE BITS et non
  énumération ») — et les deux bugs de ce lot sont précisément ce qui arrive
  quand une règle existe sans être énoncée. Consigne aussi la **forme validée
  comme précédent** (`tttUpdateElapsedMinutes` mute son entrée) et sa **condition
  de copie** : OR idempotent obligatoire.
- **[ADR-0073](docs/adr/0073-chiffre-consigne-porte-sa-methode.md) — « Un chiffre
  consigné porte sa méthode, ou il ne vaut rien »** : **onze valeurs numériques
  du dépôt se sont révélées fausses en une seule journée**, dont le budget de la
  trame WebSocket **quatre fois de suite** (une mesure vivante sur laquelle huit
  « pire cas » avaient été empilés) et un troisième chiffre faux **dans le bloc
  de commentaires qui corrigeait les deux premiers**. Décision : tout chiffre
  porte **comment il a été obtenu**, **la source à relire** et **sa date**. Un
  chiffre sans méthode se recopie ; un chiffre avec méthode se refait.
- `docs/API.md` : `GET /get-history` — `dosing` **entier**, table des trois bits,
  règle « tester des bits, jamais l'égalité », compatibilité des clients
  booléens ; `POST /history/import` — lecture typée des deux formats et sa limite.
- `docs/subsystems/history.md` : nouveau chapitre « Masque de voies de dosage » —
  discriminant de format, `0x07` et non `0x03`, rétrogradation sûre, union bit à
  bit à l'agrégation et à la déduplication, budget avant/après avec sa méthode,
  pire cas 161/157 o. Retrait d'`anyTrue()` de la table du module pur.
- `docs/subsystems/time-to-target.md` : bit `0x20` ajouté au champ de bits (bits
  6-7 désormais libres), ses **trois** sites de pose, le rendu en « absence de
  mesure » et **non** en « échec », le trou résiduel.
- `docs/features/page-dashboard.md` : marqueurs de dosage — deux séries
  distinctes plus une « voie inconnue » **en creux**, portée limitée aux
  mini-graphiques.
- `docs/features/page-ph.md`, `page-orp.md` : nouveau cas de la bande de 7
  pastilles, et **pourquoi il est testé avant l'état**.
- `docs/user/src/guide-utilisation.html`, `guide-regulation.html` : repères de
  dosage, sixième circonstance de « journée non comparable », et l'encadré
  « pourquoi un tiret plutôt qu'un zéro ». ⚠️ **Capture à refaire** :
  `screenshots/03_Tableau_de_bord.jpg` (les repères de dosage sont visibles sur
  les mini-graphiques pH et chlore).

---

## [2.46.2] - 2026-09-07

> **Ce qui change pour vous, en deux phrases** : une demande de mise à jour
> envoyée **sans être connecté** ne peut plus rien écrire dans la mémoire de
> l'appareil. Elle recevait déjà un refus à l'écran — mais l'image était déjà
> installée quand ce refus partait.

### Sécurité

- **Une requête de mise à jour non authentifiée ne peut plus faire écrire de
  partition** (`bug-117`, **critique**). Sur
  [`POST /update`](docs/API.md#authentification-de-lupload-v2462), le fichier
  envoyé était traité **sans aucune vérification d'authentification** : le
  contrôle n'existait que sur la **réponse**, prononcée **après** que l'image
  ait été écrite et validée. N'importe quel appareil du réseau local pouvait
  donc installer un firmware de son choix — et par là prendre le contrôle des
  **pompes doseuses**. Le refus est désormais prononcé **avant** la première
  écriture : aucun démarrage de mise à jour, aucune partition touchée, aucun
  redémarrage. La route répond maintenant `401` dans ce cas.
- **Le dosage ne peut plus être suspendu sans authentification** (bénéfice
  relevé en revue, non anticipé). Déclarer une mise à jour **coupe les pompes
  doseuses** le temps de l'opération. Ce mécanisme était atteignable sans
  connexion : un appareil du réseau local pouvait **arrêter la régulation pH et
  chlore** en annonçant une mise à jour fantôme, sans jamais envoyer d'image
  valide. Le vecteur est fermé.
- **Aucune mise à jour légitime n'est affectée** : les trois façons de mettre
  l'appareil à jour par fichier (script `ota_update.sh` par mot de passe, le
  même par jeton, et la page **Mise à jour** de l'interface) passent le nouveau
  contrôle — équivalence vérifiée dans les deux sens, ce point étant vital :
  la mise à jour par le réseau est le **seul** canal de déploiement de cet
  appareil.
- **Journalisation des tentatives** : une tentative non authentifiée écrit une
  ligne `WARNING` avec l'adresse IP d'origine, **au plus une par minute** et une
  seule fois par envoi. ⚠️ Ce plafond **réduit** la pression sur le journal, il
  ne la supprime pas : le refus `401` lui-même écrit déjà une ligne non plafonnée
  par requête. Le sujet reste ouvert (`bug-112`).
- **`POST /download-update` (mise à jour depuis GitHub) audité : SAIN**, et pour
  une raison structurelle — cette route n'a **aucun** traitement de corps, le
  contrôle d'authentification y précède donc nécessairement tout accès à la
  mémoire de programme. Consigné pour ne pas la ré-auditer.
- **Non traité dans cette version** : l'appareil vérifie l'**intégrité** de
  l'image reçue (empreinte SHA-256), pas son **authenticité** — et, sur l'envoi
  manuel, seulement si l'appelant fournit l'empreinte. Point renvoyé à
  `feature-118`.

### Documentation

- `docs/API.md` : `POST /update` — un **`401` est désormais possible dès
  l'upload**, le corps est refusé avant toute écriture de partition, et le `401`
  reste produit par le handler de requête. Note structurelle sur
  `/download-update`.
- `docs/subsystems/ota-manager.md` : nouveau chapitre « Authentification du
  handler d'UPLOAD » — la garde et son emplacement, la règle de méthode
  « chercher la **place** de la garde, jamais sa seule présence » (trois défauts
  du même jour, même cause), la réévaluation à chaque fragment et son coût
  **assumé**, le WARN throttlé **et la limite de ce throttle**, le bénéfice sur
  l'inhibition du dosage, et ce que le correctif ne fait pas.
- `docs/subsystems/logger.md` : troisième site du même throttle, avec rappel de
  sa limite.

---

## [2.46.1] - 2026-09-07

> **Ce qui change pour vous, en deux phrases** : une demande de calibration
> envoyée **sans être connecté** ne déclenche plus rien sur la sonde. Elle était
> déjà refusée à l'écran — le refus s'affichait, mais la sonde exécutait quand
> même l'ordre une seconde plus tard, et une calibration ne se défait pas.

### Sécurité

- **Une requête de calibration non authentifiée ne touche plus la sonde**
  (`bug-save-config-applique-avant-authentification`). Les trois routes
  [`/calibrate_ph`, `/calibrate_orp` et `/calibrate_clear`](docs/API.md#calibration-atlas-ezo--feature-021)
  affichaient bien un refus `401`, mais l'ordre partait vers le module Atlas EZO
  **avant** que le refus ne soit décidé, et s'exécutait **~900 ms plus tard**.
  Conséquences possibles selon l'ordre reçu : **régulation du chlore arrêtée en
  silence** (une calibration effacée bloque le dosage automatique — sous-chloration
  qui se compte en jours), ou à l'inverse **pompe doseuse en saturation permanente**
  sur une mesure faussée (jusqu'à 260 mL/h d'acide). Il n'existe **aucune
  dé-calibration** : l'effet était irréversible. Le refus est désormais prononcé
  **avant** toute mise en file, avant même la lecture du corps de la requête.
  Aucune calibration légitime n'est affectée : le contrôle est le même que celui
  de l'interface.
- **Journalisation des tentatives** : une tentative non authentifiée écrit une
  ligne `WARNING` avec l'adresse IP d'origine, **au plus une par minute et par
  route**, en annonçant le nombre de tentatives masquées. Ce plafond protège le
  journal : sans lui, un client anonyme sur le réseau local pouvait effacer en
  quelques secondes les heures de traces conservées par l'appareil.
- **Audit des autres routes acceptant un corps de requête** conduit à cette
  occasion (7 sites) : seules les trois routes de calibration agissaient avant
  vérification ; `/save-config` avait été corrigée en v2.25.0 et sert de
  référence. Une **réserve** sur `POST /update` (mise à jour OTA) est consignée
  dans la spec, non instruite dans cette version. Trois constats annexes ont été ouverts en specs distinctes
  (`bug-112`, `bug-113`, `bug-114`), sans correctif dans cette version.

### Documentation

- `docs/API.md` : section Calibration — le corps est refusé avant toute mise en
  file, le `401` reste produit par le handler de requête.
- `docs/subsystems/sensors.md` : nouveau chapitre « Authentification du CORPS
  des routes de calibration » — le piège d'ordonnancement corps/requête (et
  pourquoi `REQUIRE_AUTH`, bien présent, ne suffisait pas), la garde, le
  throttle, et la portée chimique des deux régimes opposés (arrêt silencieux
  vs surdosage).
- `docs/subsystems/logger.md` : chiffrage du motif du throttle et rappel que
  l'émetteur non borné (`sendAuthRequired`) **préexiste** — spec de suivi ouverte.

---

## [2.46.0] - 2026-09-07

> **Ce qui change pour vous, en trois phrases** : les pages **pH** et **ORP**
> présentent désormais le débit d'injection dans **une seule carte** —
> « **Débit d'injection** » — qui dit d'un coup d'œil ce que la régulation
> demande, de quoi c'est composé, ce qui la borne, et **pourquoi** elle
> l'applique ou non. Côté **chlore**, cette carte répond enfin à la question
> « pourquoi la pompe ne tourne-t-elle pas ? ». Et la **dose de base apprise**
> reste **lisible entre deux sessions** : elle ne reste plus figée sur
> « en apprentissage » pendant les sept heures qui séparent l'archivage de
> minuit du premier dosage du matin.

### Fonctionnalités

- **Carte « Débit d'injection pH »** (`feature-092`). Quatre lignes — dose de
  base apprise, écart courant, rattrapage accumulé, **Demande** en total — et
  une **pastille d'état** à cinq priorités (vert = pompe commandée, gris = état
  normal, rouge = panne uniquement ; compte à rebours sur la seule pause
  mélange). Elle **remplace quatre blocs empilés** au fil des versions : le
  bloc « Rattrapage accumulé », la ligne « Dose de base apprise », la ligne
  « Débit soutenable » des capacités et le bandeau « Dosage au maximum
  autorisé ». Le « N pH·s » disparaît — jargon sans usage. L'alerte
  « Capacité de dosage insuffisante » est **conservée telle quelle**, placée
  juste sous la carte.

- **Carte « Débit d'injection ORP »**. Deux lignes seulement, et c'est
  volontaire : le chlore est régulé en **P pur** — ni dose de base apprise, ni
  rattrapage cumulé. La carte montre au passage *pourquoi* la régulation du
  chlore est plus simple, et son pied permanent dit ce qui manquait le plus :
  **un écart résiduel permanent est normal**, il ne demande ni consigne plus
  haute ni gain plus fort. Elle sert aussi un verdict que le firmware ne
  publiait que côté pH : « la voie chlore est bornée par sa limite horaire en
  ce moment » — un cas qui se produit **tous les jours** sur cet appareil.

- **La dose de base apprise est enfin lisible hors session.** Elle n'était
  servie **nulle part** (ni `/data`, ni WebSocket, ni MQTT — seulement dans le
  journal, à l'ouverture de session) : impossible de distinguer « rien
  d'appris » de « 1,41 mL/min appris, en attente ». Elle est désormais un champ
  à part entière, calculé **à la lecture**, donc vrai en permanence. Corollaire
  du même correctif : le statut « en apprentissage » n'est plus servi dès que
  l'appareil a **3 journées exploitables** en mémoire — la ligne quitte cet
  état **dans les 30 secondes** après l'archivage de minuit, sans attendre la
  filtration du matin.

- **Le total de la carte est la DEMANDE, jamais un « débit appliqué ».** C'est
  un choix, et il a été chiffré : la pompe **sert la demande, puis s'arrête**
  quand son budget horaire est épuisé — elle ne débite jamais en continu à un
  débit réduit. Afficher `min(demande, soutenable)` comme « ce que la pompe
  délivre » **sous-estimait de 17 %** ce que le bassin reçoit dans les dix
  minutes suivantes. Le plafond descend donc en note, sous son vrai nom :
  « apport moyen possible … **en moyenne sur l'heure** ».

### Firmware

- ⚠️ **Changement de sémantique — `ph_ff_ml_per_min` passe au régime appliqué.**
  Le champ (et l'entité Home Assistant « **Piscine pH Dose de Base** ») rapporte
  désormais **`0,00`** en régime de maintien à zéro — c'est-à-dire pendant une
  prolongation de session où le bassin est repassé sous la consigne — au lieu de
  la valeur du feedforward. **C'est une correction** : le champ est documenté
  « appliqué » depuis son introduction en v2.28.0, et c'est ce qui rend exacte
  la reconstitution `demande − dose de base = couple servi`. La dose de base
  **apprise**, elle, conserve sa valeur : c'est le rôle de la nouvelle entité
  « Piscine pH **Dose de Base Apprise** », à ne pas confondre avec la
  précédente.

- **MQTT / Home Assistant** : **5** topics et **5** entités nouvelles (3 pH, 2
  ORP), toutes `sensor` / `diagnostic` / `mL/min` / `state_class: measurement`.
  Aucune entité existante n'est touchée. Trois champs sont délibérément **hors
  MQTT**, avec leur motif écrit — `ph_i_term_ml_per_min` (doublon de
  `ph_integral_term`, déjà publié), `orp_sustainable_ml_per_min` et
  `orp_limited_by_hourly_budget` (miroirs de champs pH qui ne sont pas publiés
  non plus).

- **Parité `/data` ↔ WebSocket** : la **pause mélange** (4 champs) était
  WebSocket-seule et rejoint `GET /data` ; réciproquement, les deux booléens de
  **caractérisation de pompe** étaient `/data`-seuls et rejoignent la trame. Un
  champ servi d'un seul côté ne disparaît pas, il **clignote**.

- **Aucune ligne du chemin de dosage ne change.** Les huit accesseurs ajoutés
  sont en **lecture seule** ; les expressions de composition sont **factorisées**
  à résultat strictement identique, prouvé par la suite native du feedforward
  laissée **intacte**. Un **filet de non-divergence** compare les deux écritures
  de l'équation à chaque tour de dosage et journalise **un WARN par session**
  au-delà de 0,05 mL/min d'écart.

### Documentation

- `docs/adr/0071-parite-data-trame-capteurs.md` (**nouvel ADR**) : la parité
  `/data` ↔ trame `sensor_data` devient une **garde**, pas une consigne orale —
  marqueurs textuels, sentinelle native, trois tables d'exceptions à motif
  obligatoire, échec sur exception morte, cliquet sur la dette. **Extension** de
  l'[ADR-0065](docs/adr/0065-trame-websocket-etat-mutable-pas-constantes.md) au
  second axe, avec un mécanisme différent parce que le critère de partage
  diffère. La sentinelle a **attrapé un défaut réel pendant son propre cycle**,
  avant la revue.
- `docs/subsystems/ws-manager.md` : nouvelle section normative « **deux trames,
  deux gabarits, un seul plafond dur** » et **méthode de chiffrage** du budget
  (pire cas **structurel**, jamais une trame vivante). La confusion entre
  gabarit documentaire et plafond dur avait produit **quatre chiffrages faux
  d'affilée** ; le décompte est rectifié de **+115 o** sans qu'aucun champ ait
  été ajouté, et la marge réelle sous `kWsFrameBufBytes` est de **137 o**.
- `docs/API.md` : les 8 champs de composition, les 4 de pause mélange et les 2
  de caractérisation ; sémantique de `ph_ff_status = 3` précisée ; avertissement
  explicite qu'**aucun champ « appliqué » n'existe**.
- `docs/MQTT.md`, `docs/subsystems/pump-controller.md`,
  `docs/features/page-ph.md`, `docs/features/page-orp.md` : cartes, accesseurs,
  bornage en deux étages, filet de divergence, fenêtre résiduelle des trois
  sorties anticipées d'`update()`.
- `docs/user/src/guide-utilisation.html` et `guide-regulation.html` mis à jour.
  ⚠️ **Deux captures d'écran à refaire** (pages pH et ORP) — seule étape
  humaine.

---

## [2.45.1] - 2026-09-07

> **Ce qui change pour vous, en une phrase** : sur les pages **pH** et **ORP**, la
> rangée de sept pastilles « **Atteinte de la cible** » affiche enfin les
> **journées passées** — elle n'affichait que celle du jour.
>
> ✅ **Aucune donnée n'a été perdue.** Les journées étaient bien archivées dans
> l'appareil pendant tout ce temps : le relevé du 06/09 portait **356 min** côté
> pH et **120 min** côté ORP, celui du 05/09 **224** et **54**. Le défaut était
> **purement d'affichage**. Si vous avez vu six cases vides pendant des semaines
> et cru votre historique effacé, il ne l'a jamais été — il apparaît tel quel
> après cette mise à jour, sans rien à réimporter.

### Frontend

- **Six pastilles sur sept restaient grises avec un tiret** (`bug-111`). La
  réponse de [`GET /get-kpi`](docs/API.md#get-get-kpi--write) range les valeurs
  d'une journée dans une **sous-structure par régulation**
  (`{"d":20260906,"ph":{"s":2,"m":356,…},"orp":{…}}`), alors que le rendu lisait
  `s` / `m` / `e0` / `f` **au premier niveau** de l'entrée. La valeur lue valait
  donc `undefined`, aucune branche d'état ne correspondait, et les sept cases
  tombaient dans le cas par défaut — « journée en cours », contour tireté, `—`.
  Les charges utiles servies par l'appareil, elles, étaient conformes : **rien
  n'a changé côté firmware**.

- **Pourquoi le défaut a traversé sa revue** : la case d'aujourd'hui est
  **écrasée** juste avant l'affichage par l'état vivant issu de `/data` — un
  choix délibéré, commenté sur place, destiné à éviter d'afficher le « 0 min »
  figé de l'entrée archivée à t0. Le seul jour qui **n'avait pas besoin** de
  l'historique était donc aussi le seul à s'afficher correctement, ce qui donnait
  à la bande l'apparence de fonctionner.

- **Deux comportements sont explicitement préservés** : un échec de `/get-kpi`
  affiche toujours « **Historique indisponible** » et **jamais** une bande vide
  (qui se lirait « aucune journée enregistrée ») ; une journée sans valeur pour
  la voie affichée reste un **trou à sa place** (`·`), la rangée étant alignée
  par date — une pastille décalée annoncerait la mauvaise date.

### Documentation

- `docs/API.md` : la forme **imbriquée** de `/get-kpi` est désormais explicite
  dans le tableau des clés (`s` / `m` / `e0` / `f` sont **dans** `ph` / `orp`, pas
  au premier niveau) — c'est précisément l'ambiguïté qui a produit le bug.
- `docs/subsystems/time-to-target.md` : même précision sur le contrat servi, et
  mention de l'aplatissement côté interface.
- **Aucun ADR** : correction d'un défaut de lecture d'une charge utile déjà
  spécifiée, aucune alternative à documenter, aucun contrat modifié.
- ✅ `docs/features/page-ph.md`, `docs/features/page-orp.md` et
  `docs/user/src/guide-regulation.html` **non modifiés** : ils décrivaient déjà
  le comportement attendu (rangée alignée par date, écrasement de la case du
  jour, légende d'indisponibilité). La documentation était juste, seul le code
  était faux — aucun PDF à régénérer, aucune capture d'écran à refaire.

---

## [2.45.0] - 2026-09-07

> **Ce qui change pour vous, en une phrase** : la page **Produits** affiche en
> permanence le **relevé** (volume injecté + démarrages de pompe), et
> **« Nouveau bidon » demande désormais confirmation** en vous montrant ce
> relevé juste avant de l'effacer.

### Fonctionnalités

- **Le relevé est affiché en permanence** sur les deux cartes produit, sous la
  barre de volume : **Volume injecté** (en mL) et **Démarrages pompe**. Il ne
  faut plus aller le chercher : il **existe avant** le jour où il est détruit.

- **« Nouveau bidon » demande confirmation.** Le bouton partait au **premier
  clic**, sans rien demander, sur une action **irréversible** — les deux seules
  valeurs qui permettent de vérifier la précision du comptage disparaissaient
  sans avoir été vues. La confirmation les présente maintenant **à l'instant
  exact où elles vont être effacées**, avec un avertissement « Relevé peu
  fiable » **uniquement** s'il y a lieu (un « 0 » permanent apprendrait à
  ignorer la zone). La configuration est **rechargée par HTTP** avant l'affichage :
  une trame WebSocket partielle donnerait un relevé incomplet au moment même
  où il est détruit.

  Rien d'autre ne change : « Enregistrer » ne touche toujours pas au décompte,
  et une valeur de bidon saisie mais non enregistrée reste prise en compte par
  la remise à zéro.

### Firmware

- **Compteur de démarrages de pompe doseuse** (`feature-110`) — un compteur `N`
  par produit, à côté du volume cumulé `V`. Relevés **ensemble** au changement
  de bidon, ils **départagent** deux modèles de biais de comptage dont les
  correctifs sont **incompatibles** : erreur **multiplicative** (débit
  surestimé) contre **déficit fixe par démarrage** (`V = V_réel + N·δ`). La
  concordance jerricane du **06/09/2026** a mesuré **+4,76 %** de surcomptage
  (~240 mL sur ~5,04 L), soit **δ ≈ 0,21 mL/démarrage** sous le second modèle.
  **Aucune correction n'est appliquée : cette version MESURE.** Le correctif
  appartient à `feature-078`, et il ne doit pas être tenté avant que la nature
  du biais soit établie.

- **Point d'incrémentation** : le front `OFF→ON` de la pompe, **dans la branche
  même qui crédite le volume** (`accountPumpVolume`), sous **la même garde**
  `productTrackingEnabled()`. Par construction, un démarrage compté ⇔ un volume
  crédité par ce démarrage : `N` et `V` couvrent **exactement** le même
  ensemble d'événements, sans aucune garde ajoutée. Remise à zéro
  **exclusivement** couplée à celle du volume, dans les blocs
  `*_reset_container`. Incrément **saturant** : un compteur figé est un aveu
  lisible, un compteur qui reboucle donnerait un rapport `V/N` silencieusement
  faux.

- **Quatre compteurs d'hygiène** (`*_anom`, `*_anom_ml`) captent un défaut
  **pré-existant**, révélé et non introduit par cette version : les sorties
  anticipées de `update()` (eau absente, stabilisation, assistant de débit,
  capteurs non initialisés) coupent les pompes mais reviennent **avant** la
  comptabilité — à la reprise, toute la durée de la coupure est créditée au
  débit plateau (5 min → ~5,7 mL, 1 h → ~68 mL, 6 h → ~408 mL) pendant que le
  redémarrage physique n'est pas compté. Les 240 mL du 06/09 valent **3,5 min**
  de crédit fantôme. Seuil `kAcctDeltaAnomalyMs = 5 000 ms` (frontière `>`
  **stricte**). Ces ticks ne sont **ni rejetés ni corrigés** — la comptabilité
  du volume est intouchée : ils servent à **disqualifier** un relevé pollué.

- **MQTT** : 8 clés ajoutées au payload du topic **retenu**
  `<base>/diagnostic` (`*_starts`, `*_total_ml`, `*_anom`, `*_anom_ml`) —
  **aucun topic neuf, aucune entité Home Assistant neuve**. Le relevé se relit
  donc **à froid, plusieurs jours après**, et survit à un redémarrage.

- **`kMqttPublishBufferBytes` porté de 1024 à 1536 o** — **correction d'un
  refus silencieux**. Sous 1024, PubSubClient **refusait la publication** du
  diagnostic sans rien écrire sur le fil : la garde est
  `bufferSize < 5 + 2 + strnlen(topic) + plength` (`MQTT_MAX_HEADER_SIZE = 5`),
  soit **1 057 o requis au pire cas** contre 1 024 disponibles. Le diagnostic
  n'aurait pas été tronqué, il aurait **disparu en entier** — `hdef` / `ldef` /
  `xdef` compris. Marge rétablie : **479 o (31 %)**. Coût RAM **+512 o**,
  alloués une fois dans `begin()`.

- **Le retour de `safePublish()` est désormais testé à ce site** — il était
  **ignoré**, la disparition du diagnostic était donc **totalement
  silencieuse**. Deux `WARNING` throttlés à 1 h, séparés parce que les causes
  n'appellent pas la même action : « trop long » se corrige dans le firmware,
  « refusée » est un problème de lien. Le second capte aussi le cas où
  `setBufferSize()` échoue au `realloc` et laisse la bibliothèque à ses 256 o
  par défaut — cas jusqu'ici **muet**.

- **HTTP / WebSocket** : `GET /get-config` sert **6 clés** neuves
  (`*_starts`, `*_anom`, `*_anom_ml`) ; seules `ph_starts` et `orp_starts`
  entrent dans la trame WebSocket `config` (**+47 o**, trame à **83 clés**,
  ≈ **2 995 o** au pire cas, **marge 589 o** sous `kWsFrameBufBytes = 3584`).
  Les **4 clés d'hygiène sont HTTP-seules** et **déclarées** comme telles dans
  la sentinelle de parité (motif écrit, asserté) : elles sont lues **une fois
  par campagne**, les pousser en continu dépenserait le budget de trame pour
  rien.

### Documentation

- `docs/features/page-dosages.md` : **mode d'emploi du relevé** — quoi noter,
  quand, et **comment reconnaître un relevé qui ne conclut rien** (verdict
  `pool-chemistry` du 07/09 : `anom_ml / V_bidon` < 0,5 % concluant, 0,5–1,5 %
  concluant après soustraction explicite, **> 1,5 % NON CONCLUANT sur δ** ;
  co-critère `anom_count / N ≤ 2 %`). Y sont écrits les trois points de fond :
  le phénomène se nomme **« déficit de démarrage »** et jamais « amorçage »
  (0,21 mL = **0,19 s** de plateau à 68 mL/min, signature d'une rampe) ; les
  deux biais **poussent δ dans le même sens**, donc le déficit fixe est
  systématiquement **favorisé par l'asymétrie** ; **une seule campagne ne peut
  pas conclure** (il faut deux campagnes dont le mL/démarrage moyen diffère
  d'un facteur ≥ 2), et le relevé du 06/09 **n'est pas requalifiable après
  coup**. Croisement vers le **protocole de banc** — 24 pouls de 5 s contre
  12 de 10 s, à temps de pompe égal — qui est le chemin **rapide** là où le
  relevé de bidon est le chemin **sans effort humain**.
- `docs/subsystems/ws-manager.md` : **table de budget de trame rectifiée** —
  elle annonçait 81 clés / ≈ 2 948 o / marge 636 o, valeurs **périmées** depuis
  feature-110. C'est le chiffre sur lequel le prochain contributeur arbitre son
  propre ajout : 83 clés, ≈ 2 995 o, **marge 589 o**, gabarit 3008. Nouvelle
  section sur les 2 clés WS et les 4 exceptions HTTP-seules (table
  `kHttpOnlyKeys` à **12 entrées**).
- `docs/subsystems/pump-controller.md` : les 6 compteurs, le point
  d'incrémentation, `kAcctDeltaAnomalyMs`, la persistance NVS (`pool_prod`,
  flush différé 60 s) et le **défaut pré-existant** des sorties anticipées.
- `docs/API.md` : les 6 clés de `/get-config`, dont les 4 **HTTP-seules**.
- `docs/MQTT.md` : les 8 clés du diagnostic, le motif du passage à 1536 o et
  les deux `WARNING` ; cadence de publication du diagnostic corrigée
  (**toutes les 5 min**, pas seulement au démarrage et à la reconnexion).
- `docs/user/src/guide-utilisation.html` : le nouveau comportement du bouton
  « Nouveau bidon » et ce qu'il faut noter au changement de bidon.
- **Aucun ADR** : ajout d'observabilité, aucune décision structurante (voir le
  rapport de cycle).

---

## [2.44.0] - 2026-09-06

> ℹ️ **Bump résolu** : `src/version.h` a été porté directement à `2.45.0` au cycle suivant (feature-110) — la 2.44.0 n'a jamais eu de `FIRMWARE_VERSION` propre. Aucune action ne reste sur cette entrée.
>
> **Ce qui change pour vous, en une phrase** : le **journal de bord redevient exploitable** — il couvrait 28 minutes, il doit couvrir **des heures**.

### Firmware

- **La découverte Home Assistant n'écrase plus le journal** (`feature-109`). À chaque (re)connexion au broker, l'appareil redéclare ses **54 entités** — et écrivait **54 lignes** au journal, soit **≈ 5,3 Ko**, pour un fichier qui n'en fait que **8**. Une seule reconnexion suffisait à effacer tout ce qui précédait. Le 06/09, l'appareil a redémarré à 14:19 : il **avait correctement enregistré la cause** (la ligne « Cinétique heap avant reset »), mais au relevé de 19:15 le journal ne remontait plus qu'à **28 minutes** en arrière. La cause est perdue — et c'est **le même mécanisme** qui avait détruit le post-mortem du crash du 15/08.

- **Une rafale = une ligne, y compris quand tout échoue.** Exemples réels :

  ```text
  INFO    Découverte HA #3 : 54/54 entités, 0 échec, 1240 ms
  WARNING Découverte HA #4 : 12/54 entités, 42 échecs, interrompue à la 13e (sensor/pool_ph/config), 310 ms
  WARNING Découverte HA #7 : 52/54 entités, 2 échecs épars, 1er à la 9e (sensor/pool_orp/config), 1180 ms
  WARNING Découverte HA #6 : abandonnée avant publication (lien MQTT absent)
  ```

  **Aucun échec n'est masqué** : le niveau passe en `WARNING` au premier, et le topic fautif est **nommé**. Le pire cas reste **une seule ligne** : un lien coupé dès la 1ʳᵉ entité produit 54 échecs et une ligne. Une ligne par échec aurait ramené les 5,3 Ko exactement le jour du crash.

- **« interrompue » et « épars » ne veulent pas dire la même chose.** Le premier signifie que le lien MQTT est tombé en cours de rafale ; le second, que deux entités ont échoué alors que le lien tenait. Ce sont **deux pannes opposées**, et la distinction est **calculée** à partir de la position des échecs, pas déclarée à la main.

- **Le cas silencieux a disparu.** Une découverte demandée sans lien MQTT ne produisait **rien du tout** ; elle produit désormais sa ligne d'abandon. Chaque rafale porte aussi un **numéro** (`#3`, `#4`…) : deux reconnexions rapprochées ne sont plus deux lignes indiscernables, et un numéro manquant est lui-même une information.

- **Le contrat Home Assistant est strictement inchangé** : mêmes 54 topics, mêmes charges utiles, même `retain`, même ordre, même republication à chaque reconnexion. **Rien à refaire côté HA.** Seule la journalisation change.

- **Le détail entité par entité reste disponible** en activant *Paramètres → Avancé → Logs DEBUG*. Il n'entre alors **pas** dans le fichier journal (c'est le but), mais il occupe à nouveau la mémoire, la page Logs et la console : à réserver aux sessions de diagnostic.

- **Effet chiffré** : la découverte passe de **756 à 11 octets par minute** (**×67** sur ce poste), le débit total du journal de **913 à 168 o/min** (**÷5,4**), et la fenêtre couverte de **28 minutes à ≈ 2 h 30** — estimation de conception, **à confirmer par un relevé sur l'appareil**.

  ⚠️ **93 % de ce qui reste vient désormais des autres émetteurs.** Ils étaient minoritaires, ils sont dominants : leur traitement, annoncé comme optionnel, ne l'est plus.

- **Coût** : flash 1 828 981 → **1 830 809 o** (+1 828) ; RAM +8 o. Logique de synthèse extraite en module pur ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : **22 cas natifs** (1 325 → **1 347 tests**), **5 mutants sur 5 tués**.

### Documentation

- `docs/subsystems/mqtt-manager.md` : format exact des quatre lignes, prédicat « interrompue / épars », `burstSeq`, troncature du topic **par la tête**, et rappel explicite que le contrat de découverte est inchangé. Compte d'entités corrigé **52 → 54** (il avait dérivé en silence).
- `docs/subsystems/logger.md` : nouveau budget du journal (tableau avant/après), et **coût résiduel de `DEBUG`** — le fichier est épargné, le ring RAM / WebSocket / Serial ne le sont pas ; plus le piège de **placement de la garde** au site d'appel.
- `specs/features/doing/bug-080-*.md` : l'obstacle d'**observabilité** à son critère « 7 jours sans redémarrage inopiné » est levé (la spec **reste ouverte**).
- **Aucun ADR** : site unique, contrat inchangé, aucun verrouillage, aucune primitive de journalisation réutilisable introduite — aucun critère de déclenchement rempli.
- **Aucune régénération de `docs/user/`** : aucun écran, message ou réglage utilisateur ne change.

---

## [2.43.0] - 2026-09-06

> ⚠️ **Correctif NON validé sur matériel.** L'appareil est joignable (192.168.1.48) mais **aucun déploiement n'a été fait** — c'est une décision humaine. Cette version livre une défense **argumentée et testée en logique pure** ; elle **ne prouve pas** la disparition du crash en production. Le critère « 7 jours sans redémarrage inopiné » de `bug-080` **reste ouvert**.
>
> **Ce qui change pour vous, en une phrase** : quand la mémoire de l'appareil vient à manquer, **il ne redémarre plus brutalement** — il **reporte** l'écriture, la **retente**, et le **dit**.

### Firmware

- **Plus aucun redémarrage brutal quand la mémoire manque à l'écriture d'un fichier** (`bug-100`, [ADR-0070](docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)). Le crash du **05/09** (PANIC daté au SHA256 du coredump, sous la v2.40.0) est survenu pendant la **sauvegarde périodique de l'historique** : sur ESP32, ouvrir un fichier fait allouer à la libc un verrou de 92 octets, et si cette allocation échoue, le système **s'arrête net, sans erreur exploitable**. La garde 503 de la v2.41.0 ne couvrait que les pages web ; **elle ne couvrait aucune des 14 ouvertures que le firmware fait pour son propre compte**.

- **Historique — la sauvegarde est REPORTÉE, pas perdue.** Sous famine mémoire, la sauvegarde des 5 minutes est **sautée sans toucher au fichier existant** (mieux vaut un historique périmé de 5 min qu'un fichier détruit) et **retentée au cycle suivant**. Les mesures continuent d'être enregistrées en mémoire et affichées à l'écran. Un avertissement est journalisé, **au plus une fois par heure**.

- **Au démarrage, l'appareil DOSE au lieu d'aborter.** Si la mémoire manque au boot, l'historique persistant est **désactivé pour ce démarrage** — l'appareil démarre, régule et dose normalement, simplement sans reprendre les mesures des jours précédents. L'alternative était un redémarrage en boucle. Le **journal de bord, lui, reste enregistré** : l'observabilité ne dépend pas du sous-système qu'elle garde.

- **Journal système — les lignes ne sont plus perdues.** Les **7 ouvertures** de `/system.log` sont gardées. Sous famine, l'écriture disque est reportée et les lignes en attente sont **remises dans la file** (plafond de 100 entrées, les plus anciennes sortent). Une seule mesure mémoire par écriture au lieu de six.

- **`GET /download-logs` ne ment plus quand il est tronqué.** Si le journal persistant ne peut pas être ouvert, l'export continue d'être servi (session courante seule) mais **l'annonce désormais dans le fichier téléchargé** — `# === Historique persistant INDISPONIBLE === … export TRONQUE a la session courante`. Le repli existait déjà ; il était **silencieux**.

- **Trois compteurs dans le diagnostic MQTT — `hdef`, `ldef`, `xdef`** (topic **retenu** `<base>/diagnostic`, aucun topic ni endpoint nouveau). `0` en régime normal. Le topic étant retenu, ces valeurs **survivent au redémarrage** — c'est délibéré : sous famine, le journal de bord ne survit pas, mais le compteur si. ⚠️ `hdef` (famine **mémoire**) ne recoupe pas le compteur « partition pleine » (saturation **disque**) : deux pannes différentes, deux libellés distincts.

- **Un seul point d'admission, vérifié par la CI.** Toute ouverture de fichier interne passe par `src/fs_guard.cpp` ; le contrôle `tools/check_fopen_guard.sh` (job `fopen-guard` du workflow **Hygiène du dépôt**) échoue si un autre fichier de `src/` ouvre un fichier lui-même. La garde par site — le mode de défaillance du lot B-3 — est **rejetée par construction** : le défaut devient **inexprimable**, pas seulement non testé.

- **Une ouverture de fichier a été SUPPRIMÉE, pas gardée** : `exists()` du framework **ouvre le fichier** (aucun `stat()`), et l'appel qui précédait le chargement de l'historique était strictement redondant. ⚠️ Contrepartie assumée : « fichier absent » et « fichier illisible » ne sont plus distinguables ; le message est rendu explicitement non discriminant.

- **7ᵉ barreau de l'échelle des seuils mémoire** : `kInternalFopenMinAllocBytes = 5 120 o`, intercalé entre l'auto-guérison (4 096) et les pushes WebSocket (6 144). **Les 6 barreaux existants sont inchangés** — l'ordre complet `2 048 < 4 096 < 5 120 < 6 144 < 8 192 < 12 288 < 18 432` est verrouillé à la compilation par `static_assert`.

- **Trois chiffres de dimensionnement du projet étaient faux — corrigés.** Verrou récursif : **92 o** (et non « ~88 »), expansion du pool `FILE` : **428 o** (et non « ~440 »), et `__sfmoreglue` **n'existe pas comme symbole** (il est inliné — une recherche par symbole aurait conclu à tort à son absence). Établis par désassemblage de `libc.a`. **Aucun seuil n'en dépendait.**

- **Coût** : flash 1 825 701 → **1 829 013 o** (+3 312, **49,0 %**, marge 1 906 539) ; RAM +64 o.

### Documentation

- `docs/subsystems/acces-fichiers.md` : recensement passé de 14 à **13 sites, tous gardés**, avec la classe et le point d'admission de chacun ; observabilité des refus ; limites du contrôle automatique ; **dette de test assumée** (3 mutants non tués) ; et surtout le **piège de placement nº 2** — *une garde mémoire va avant le PIC D'ALLOCATION du chemin, pas seulement avant l'appel qu'elle protège*. C'est le bloquant de revue le plus transférable du cycle : la première admission validait une marge que le `JsonDocument` dépensait ensuite, autorisant le `fopen` **sur une mesure devenue fausse**. D'où une **seconde** admission après la sérialisation.
- [ADR-0070](docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) : **bannière d'amendement** (le §d est nécessaire mais **non suffisant** ; le compte passe de 14 à 13 sites). **Corps inchangé** — un ADR accepté ne se réécrit pas.
- [ADR-0058](docs/adr/0058-gardes-heap-metrique-8-bits.md) : **bannière d'amendement** pour le 7ᵉ barreau, les 6 précédents inchangés.
- `docs/subsystems/history.md` : deux admissions dans `saveToFile()` et pourquoi aucune n'est redondante, tripwire de boot, suppression de l'`exists()`, mesure sous mutex acceptée (argument chiffré).
- `docs/subsystems/logger.md` : une mesure par flush pour six ouvertures, ce que devient le journal sous famine, `setPersistenceFs()` qui arme inconditionnellement. Le corollaire « `flushToDisk()` reste un `fopen` abort-prone » de la v2.41.0 est **marqué fermé**.
- `docs/MQTT.md`, `docs/API.md`, `docs/subsystems/mqtt-manager.md`, `docs/subsystems/web-server.md` : contrat des trois compteurs, troncature annoncée de `/download-logs`, périmètre du lot B-3 marqué implémenté.
- `docs/BUILD.md` : job CI `fopen-guard` et ses **deux trous connus**, compte de tests **1 291 → 1 325** (34 suites), dette des 3 mutants.
- ✅ Documentation utilisateur (`docs/user/`) **non concernée** : aucun écran, aucun bouton, aucun message d'interface ne change. Aucun PDF à régénérer, **aucune capture d'écran à refaire**.

---

## [2.42.1] - 2026-09-06

> ℹ️ **Aucun changement de comportement.** Le binaire est **fonctionnellement identique** à celui de la 2.42.0 : sections `text` / `data` / `bss` **inchangées à l'octet**, table des symboles identique (11 124 entrées, diff vide, adresses comprises). Les 77 octets d'écart du `.bin` sont tous imputés à l'horodatage de compilation, à la chaîne de version, au descripteur d'application et au SHA256 d'image. Rien à reconfigurer, aucune limite de sécurité touchée.
>
> ⚠️ **Un seul fait vous concerne : la procédure de retour arrière vers le core Arduino 2 disparaît.** Elle était publiée dans le guide de mise à jour et elle était **fausse** — l'environnement qu'elle invoquait ne compilait plus depuis la v2.40.0, et aucune image de secours n'avait jamais été archivée. Le retour arrière se fait désormais sur une **version antérieure du firmware** (image archivée ou asset de release), en OTA si l'appareil répond, par câble USB sinon.

### Interne / maintenance

- **Retrait définitif du repli core Arduino 2.x** ([ADR-0069](docs/adr/0069-retrait-repli-core-2.md), feature-102). Le projet n'a plus qu'un **environnement matériel**, `[env:esp32dev_core3]` (**non renommé**, `default_envs` conservé). Motifs cumulés : le repli n'était **plus compilable** depuis la v2.40.0 (`esp_cpu_get_cycle_count()` appelé hors garde dans `oom_guard.cpp`, resté invisible deux versions parce que la CI ne construit que l'env `native`), **plus flashable** (bootloader core 3 depuis la v2.20.4, machine de développement sans port série) et **sans image de secours** (aucun binaire core 2 n'a jamais été archivé — case ouverte de l'[ADR-0034](docs/adr/0034-migration-core3-idf5.md) jamais honorée). L'argument « marge flash » qui le justifiait est périmé depuis le layout v5 : **48,9 %** d'occupation, marge 1,82 Mo.
- **Suppression des 8 gardes `#if ESP_ARDUINO_VERSION_MAJOR >= 3`** — `pump_controller.cpp` (6 sites) **et** `pump_controller.h`, `main.cpp`, `oom_guard.cpp`. Seule la branche core 3, **déjà la seule compilée depuis la v2.20.4**, subsiste. La garde `#if MBEDTLS_VERSION_NUMBER >= 0x03000000` d'`ota_integrity.cpp` est **conservée** : elle est indexée sur le contrat d'une bibliothèque tierce, pas sur l'environnement retiré. *(Rectification d'archive : l'entrée `[2.15.0]` de ce changelog et `docs/BUILD.md` annonçaient « 3 fichiers : `pump_controller.cpp`, `main.cpp`, `ota_integrity.cpp` » — la répartition réelle est celle ci-dessus.)*
- **Durcissement de fait sur `isPumpUsable()`** : la branche « core 2 → retourne toujours `true` » disparaît. Une pompe n'est plus jamais réputée utilisable sans vérification, dans aucune configuration de build. L'alerte MQTT `pump_unusable` est **armée sans condition** (topic, charge utile et cadence inchangés). Gate `pool-chemistry` **GO** — durcissement unilatéral, échelle du duty (20 kHz / 8 bits, 0-255) strictement préservée.
- **Fin du conflit de paquet `framework-arduinoespressif32`** : plus de `pio pkg install` à chaque bascule d'environnement — donc plus de réécriture de `platformio.ini` qui en supprimait les commentaires. ⚠️ La plateforme `espressif32@6.9.0` **reste installée** malgré tout : elle fournit `tool-mklittlefs`, absent de pioarduino 55.03.37 et référencé en dur par `build_fs.sh`. La désinstaller casserait la construction du filesystem.
- `deploy.sh`, `ota_update.sh`, `tools/archive_build.py` et `tools/decode_coredump.sh` : exemples et justifications alignés sur l'environnement unique. Le **cloisonnement d'archivage par environnement** (`builds/<VERSION>/<PIOENV>/`) est **maintenu**. Aucune modification de la CI (elle ne construit aucun env matériel).

### Documentation

- `docs/adr/0069-retrait-repli-core-2.md` **[nouveau]** : les sept justifications chiffrées, les alternatives écartées (réparer `oom_guard`, garder un env non compilable « pour documentation », renommer `esp32dev_core3`), la décision de conserver la garde mbedtls, celle de ne pas désinstaller `espressif32@6.9.0`, et la **méthode de validation par équivalence binaire** — la validation par mutation étant inapplicable à des branches `#else` jamais compilées, et n'ayant pas été simulée.
- [ADR-0042](docs/adr/0042-core3-environnement-par-defaut.md) et [ADR-0034](docs/adr/0034-migration-core3-idf5.md) : **bannières d'amendement** (clause de repli refermée ; phase code bi-core close, case « binaires de retour arrière archivés » close sans avoir été honorée). Corps des ADR inchangés.
- `docs/UPDATE_GUIDE.md` : **suppression de la procédure de retour arrière core 2**, remplacée par ce qui fonctionne. Les avertissements « changer de bootloader = flash USB obligatoire » et « le filet `otadata` a disparu » sont **conservés et renforcés**.
- `docs/BUILD.md` : environnement unique, tableau de marge flash, PSRAM, esptool 4/5, archivage, décodage de coredump et pièges connus mis à jour ; section « Conflit de paquet » retirée.
- `docs/subsystems/pump-controller.md`, `docs/subsystems/mqtt-manager.md`, `docs/subsystems/ws-manager.md` : chemin PWM unique (invariant **20 kHz / 8 bits, échelle du duty inchangée** rendu explicite), `isPumpUsable()` sans branche fail-open, alerte `pump_unusable` non conditionnelle, requalification de la note « migration future vers ESP-IDF 5.x » (la migration a eu lieu ; la ré-isolation de `TCP_USER_TIMEOUT` reste une piste ouverte, hors périmètre).
- `README.md` : environnement unique, et correction de la taille du filesystem annoncée (**512 Ko**, layout v5 — la valeur « 832 KB » était périmée).
- `CLAUDE.md`, `.claude/skills/platformio-workflow/SKILL.md`, `.claude/commands/release.md`, `.claude/agents/{embedded-developer,debugger}.md` : plateforme, environnement, chemins d'ELF et compte des suites de tests réalignés sur l'état réel.
- ✅ Documentation utilisateur (`docs/user/`) **non concernée** : aucune mention du core 2 ni de `esp32dev`. Aucun PDF à régénérer, aucune capture d'écran à refaire.

---

## [2.42.0] - 2026-09-05

> ⛔ **Mise à jour par câble USB obligatoire — l'OTA ne peut PAS installer cette version.** ⚠️ **L'historique des mesures est effacé : le sauvegarder AVANT le flash, appareil encore en ligne.** ✅ **En revanche toute la configuration est conservée** (consignes, calibrations, offset pH, taille de bassin, WiFi, mot de passe) : rien à reconfigurer.
>
> **Pourquoi cette version existe** : **l'appareil contient une flash de 8 Mo, et n'en utilisait que 4.** Pas « utilisait mal » : la table de partitions s'arrêtait à la moitié, et la moitié haute n'était référencée nulle part — ni lue, ni écrite, ni comptée. Le firmware occupait **99,5 %** de la place qui lui était allouée, soit **9 291 octets de marge** : moins que le coût d'une fonctionnalité moyenne. Depuis plusieurs versions, chaque correctif devait littéralement **financer sa place** en supprimant du code ailleurs. Cette contrainte disparaît : l'occupation tombe à **48,9 %**. Rien ne change à l'écran, aucun bouton ne bouge, aucune limite de sécurité n'est touchée.

### Firmware

- **Table de partitions — layout v5, la flash de 8 Mo est utilisée en entier** ([ADR-0068](docs/adr/0068-partitions-layout-v5-flash-8-mo.md)). `platformio.ini` déclarait `flash_size = 4MB` et la table se terminait à `0x400000` — **la déclaration ne décrivait pas le matériel**. Confirmation par `esptool flash_id` **avant toute modification** : ESP32-D0WD-V3 rev v3.1, `Manufacturer 20`, **`Device 4017`** (octet de capacité `0x17` = 2²³ = **8 388 608 octets**), `Detected flash size: 8MB`.

  | Partition | v4 | v5 |
  |---|---|---|
  | `app0` | `0x010000`, 1792 Ko | `0x010000`, **3648 Ko** |
  | `app1` | `0x1D0000`, 1792 Ko | **`0x3A0000`**, **3648 Ko** |
  | `spiffs` | `0x390000`, 320 Ko | **`0x730000`**, **512 Ko** |
  | `history` | `0x3E0000`, 64 Ko | **`0x7B0000`**, **256 Ko** |
  | `coredump` | `0x3F0000`, 64 Ko | **`0x7F0000`**, 64 Ko |
  | Fin de table | `0x400000` (4 Mo) | **`0x800000` (8 Mo, pile)** |

  Aucun chevauchement, partitions applicatives alignées sur 64 Ko, **zéro octet perdu**.

- **Marge applicative : 9 291 → 1 909 835 octets** (occupation 99,5 % → **48,9 %** d'un slot passé de 1 835 008 à 3 735 552 o). C'était **la contrainte dominante du projet** ; la phrase « le prochain lot devra arriver avec son propre poste de récupération flash », portée par les notes des lots B-2 et B-3 de `bug-080`, est **retirée** — elle n'a plus d'objet.

- **Partition `history` : 64 → 256 Ko** (16 → 64 blocs LittleFS). Ce n'était pas un confort : la marge de croissance tombait à **90 octets** dans le pire cas (`bug-099` — `.tmp` de rotation orphelin cumulé à un tampon de log plein), et le mode de défaillance de ce budget est **le silence** (la sauvegarde cesse, rien ne casse visiblement). **`bug-099` est dissous** ; **`bug-098`** (un octet de plus par point, 339 o au total) **devient trivial à financer**. ⚠️ Les deux specs sont à relire au nouveau budget. La garde d'admission `historySaveAdmit()` reste en place — un budget confortable n'est pas un budget infini.

- **Partition `spiffs` : 320 → 512 Ko** — 221 184 o utilisés, **303 104 o libres**. La contrainte « frontend limité à 320 Ko » de l'ADR-0024 est levée.

- **Répartition assumée : 3648 Ko par slot et non 3840** (le maximum). 192 Ko ont été délibérément prélevés pour `spiffs` et 192 pour `history`, parce qu'une contrainte qui mord aujourd'hui prime sur une marge qui passerait de 51 % à 56 %.

- **Alternative écartée, chiffrée** : rogner `spiffs` de 320 à 256 Ko dans le cadre 4 Mo — le palier qu'on aurait pris sans le `flash_id` — n'aurait rendu que 64 Ko, à partager entre `app0` **et** `app1`, soit **+32 Ko par partition**. Contre **+1 856 Ko**. **Un facteur 58.** C'est l'illustration du vrai risque : optimiser à l'intérieur d'une contrainte fausse.

- ⚠️ **Une commande de dépannage change d'adresse.** L'effacement manuel de la partition `history` devient `esptool erase_region 0x7B0000 0x40000`. **Ne plus lancer l'ancienne `erase_region 0x3E0000 0x10000`** : cette zone est désormais **au milieu de `app1`** — elle détruirait le slot OTA de secours au lieu d'effacer l'historique.

- ⚠️ **Limite découverte à l'usage, désormais documentée** : `POST /history/import` **refuse tout corps de plus de 16 384 octets** avec un `413` (plafond de la bibliothèque HTTP, jamais surchargé côté firmware — et émis **avant** le contrôle d'authentification). À ~112 octets par point compacté, **145 points passent, 146 non**. Sur les 245 points sauvegardés avant le flash, **100 n'ont pas pu être réimportés** (23 au 31 août) ; ils restent dans le fichier de sauvegarde. 🛑 **RÉTRACTATION (2026-09-07, `bug-101`) — le contournement publié ici était FAUX ET
DESTRUCTEUR.** Il conseillait de « découper la restauration en lots de ≤ 145 points, qui
se cumulent » : **les lots ne se cumulaient pas**. `src/history.cpp:964` faisait
`memoryBuffer = dataPoints;`, un **remplacement total** — chaque lot **écrasait** le
précédent, et qui suivait ce conseil ne gardait que le **dernier** morceau, en perdant
tout le reste **sans aucun message**. Le découpage sûr est livré en **v2.48.0** : il est
fait par l'interface, avec un mode `append` explicite et une règle d'écrasement par
créneau. Le même conseil vivait à **deux autres endroits** — `docs/API.md` et
`docs/UPDATE_GUIDE.md` — tous deux corrigés le même jour.

### Documentation

- `docs/adr/0068-partitions-layout-v5-flash-8-mo.md` : **nouvel ADR** — utiliser les 8 Mo réels plutôt que dégraisser le firmware ou rogner le filesystem, répartition retenue, flash USB obligatoire / DATA effacées / NVS préservée, et clôture du sujet flash.
- `docs/adr/0024-partitions-layout-v4.md` : passé en **`Superseded by ADR-0068`**, avec bannière datée expliquant que sa prémisse (flash de 4 Mo) était fausse. Contenu historique non réécrit.
- `docs/UPDATE_GUIDE.md` : nouvelle note « Migration layout v4 → v5 (flash 8 Mo) » — sauvegarde préalable, `esptool flash_id`, `./deploy.sh all`, interdiction de `factory` et de l'OTA, réimport par lots de 145 points, vérifications post-flash, commande de secours corrigée.
- `docs/API.md` : limite **16 384 octets** de `POST /history/import` documentée (codes d'erreur, `413` avant authentification, contournement) ; exemple `/get-system-info` réaligné (`flash_size`, `ota_partition_size`, `fs_total_bytes`) et ces trois champs érigés en **moyen de vérifier depuis l'extérieur qu'un appareil est bien passé en table v5**.
- `docs/BUILD.md` : tableau de marge flash refait sur le slot de 3 735 552 o, taille FS 524 288, et deux pièges ajoutés — `LITTLEFS_OFFSET` comme point de défaillance **silencieux** d'un changement de layout, et l'obligation de vérifier la flash physique avant de raisonner sur la place.
- `docs/subsystems/history.md` : partition 256 Ko / `0x7B0000`, budget avant-après (12/16 → 12/64 blocs), effet sur `bug-098` / `bug-099`, et cas réel de l'effacement au premier boot.
- `docs/subsystems/system-health.md` : commande de récupération corrigée + avertissement sur l'ancienne, périmée et dangereuse.
- `README.md` : description de `partitions.csv` réalignée (elle décrivait encore le **layout v2**).
- `docs/user/README.md` : taille du filesystem embarqué corrigée.

---

## [2.41.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration, aucun réglage modifié, aucune limite de sécurité touchée. Aucun écran ne change, aucun bouton ne bouge.** **Pourquoi cette version existe** : quand l'appareil manquait vraiment de mémoire, **ouvrir une page web le faisait redémarrer**. Pas « ramer », pas « afficher une erreur » : **redémarrer**, sans avertissement — c'est le plantage du 29 août. Désormais, dans cette situation, l'appareil **refuse poliment** de servir la page (message « Memoire insuffisante, reessayez » dans le navigateur) au lieu de tomber. **Ce qui continue de fonctionner pendant ce temps** : la régulation, le dosage, la filtration, l'éclairage, Home Assistant, le suivi temps réel et **toutes les pages de diagnostic** — c'est-à-dire tout ce qui permet de comprendre ce qui se passe.

### Firmware

- ⚠️ **Un chemin de plantage SANS RECOURS est fermé.** Servir une page demande à la bibliothèque système un petit verrou interne (~88 octets). Si cette allocation échoue, le code responsable **avorte le programme** — il n'y a **aucun code de retour à tester**, aucune branche d'erreur possible à écrire. La seule défense est de **ne jamais entrer dans l'ouverture de fichier**. C'est ce que fait cette version : sous **8 192 octets** de plus gros bloc libre, tout `GET` de fichier statique ou de page HTML répond **`503`** avec `Retry-After: 5` **avant** d'ouvrir quoi que ce soit.
- ⚠️ **Le parachute mémoire ne pouvait RIEN sur cette famille de plantages, et ne le pourra jamais.** Ce chemin passe par `malloc` (C), pas par `operator new` : le parachute est un `std::set_new_handler`, il ne voit tout simplement pas cette allocation. Ce n'est pas un réglage à corriger, c'est une **absence par construction**. Cette garde en est la **seule** défense.
- **Un handler prioritaire unique, pas sept vérifications à répéter.** Un seul point de code (`HTTP_GET`, matcher universel, filtre = famine) couvre **les 7 pages HTML explicites et tous les assets** (CSS, JS, images, polices). Un filtre posé sur `serveStatic` **n'aurait pas marché** — pour deux raisons indépendantes, chacune suffisante : `AsyncStaticWebHandler::canHandle` (`WebHandlers.cpp:105-107`) appelle `_getFile()` → `_searchFile()`, donc `serveStatic` **touche le système de fichiers dès `canHandle`** ; et son filtre **rejette les `.html`**, donc il n'aurait **jamais** couvert le chemin exact du plantage du 29/08.
- ⚠️ **L'ORDRE D'ENREGISTREMENT DES ROUTES EST DÉSORMAIS LA POLITIQUE D'EXEMPTION.** ESPAsyncWebServer s'arrête au **premier** handler qui accepte : les **19 routes `HTTP_GET`** déclarées au-dessus de la garde (`/data`, `/get-config`, `/get-logs`, `/download-logs`, `/get-history`, `/get-kpi`, `/get-system-info`, `/time-now`, auth, WiFi, coredump, `/check-update`, sondes, `/debug/*`) restent servies **même en famine**, sans qu'aucune liste ne soit maintenue. **Corollaire verrouillé : toute route `GET` ajoutée après ce point sera couverte par la garde** — une route de diagnostic ou de secours doit être enregistrée **avant**. Une liste s'oublie ; l'ordre d'enregistrement se voit.
- **Aucune commande de dosage ne peut être avalée** : la garde est **GET-only** et `web_routes_control.cpp` ne contient **aucune** route `HTTP_GET` (19 `HTTP_POST`) — vérifié, motif pour lequel aucun gate `pool-chemistry` n'était requis. **Nuance conservée** : le filtre s'exécute **avant** le contrôle de méthode, donc un `POST` qui remonte jusqu'à cette position **paie le parcours de tas** — sans jamais être refusé.
- **6ᵉ barreau de l'échelle mémoire** : `kStaticFileMinAllocBytes` = **8 192 o**, intercalé entre les pushes WebSocket sautés (6 144) et l'alerte mémoire (12 288). **Les 5 seuils existants sont INCHANGÉS.** Dimensionnement : ×13,6 le besoin dur (~600 o sur le chemin qui avorte) et ×2,1 le besoin sans bégaiement réseau (~3 850 o) ; le plateau calme mesuré le 05/09 vaut **13 300 o**, soit **+38 %** au-dessus du seuil. **Interdiction de descendre sous 6 144 o** — verrouillée par `static_assert`, donc **le build casse**, ce n'est pas une consigne écrite.
- **Le coût de la mesure est arbitré, pas subi** : **70-250 µs** par requête atteignant la garde, **1-3 ms** par chargement de page, soit **≲ 5-10 %** du coût d'un service de fichier qui existe déjà. **Sans cache et sans bornage**, et ce n'est pas un oubli : la borne est acquise **par construction** (masque figé, plafond pathologique ~800 µs) et l'autolimitation du lot précédent est **sans objet** ici (pas d'ISR, fréquence bornée par le trafic TCP). ⚠️ **Un cache est refusé fermement** — le faux négatif d'une garde anti-crash est un **redémarrage**, et `heap_metrics.h:133-135` documente **~45 Ko d'oscillation entre deux échantillons distants de 2 s**. **Caveat consigné** : le coût **croît avec la fragmentation**, il est donc maximal exactement dans le régime pour lequel la garde existe.
- ⚠️ **TROUVAILLE DU LOT, et c'est sa vraie valeur : un log `ERROR` déclencherait le plantage qu'on prévient.** `logger.cpp:71-73` flushe **en synchrone, dans la tâche appelante**, sur `ERROR` et `CRITICAL` → `_persistFs->open("/system.log","a")` → **`fopen`** → exactement l'`abort()` fermé ici. **Interdit GÉNÉRAL, pas particularité de cette version** : tout code écrit pour éviter `fopen` en famine est contraint à **`WARN` / `INFO`**, et **ne journalise jamais depuis `async_tcp`**. Noté pour la suite : **`Logger::flushToDisk()` reste un second `fopen` qui peut avorter**, non couvrable par un 503 (il n'y a pas de requête à refuser) — identifié, **hors périmètre**, lot ultérieur.
- **Journal borné à 2 lignes par épisode** (`WARN` « Service fichiers suspendu (503) — maxBloc8=X o < 8192 o » / `INFO` « Service fichiers rétabli — N refus en T s »), drainées depuis la boucle principale. Une rafale de chargement de page produit 6 à 20 refus : journaliser par requête effacerait la fenêtre forensique de l'épisode qu'on cherche justement à documenter (leçon du 15/08, journal de 8 Ko, rétention 25-35 min).
- **Constante ajoutée hors plan** : `kHttpLowHeapEpisodeQuietMs` = **30 000 ms**. C'est **d'abord une garde de coût**, pas un confort de lecture — sans elle, la boucle principale parcourrait le tas **à sa propre fréquence pendant toute la famine**, au pire moment possible. Anti-spam ensuite (un tas oscillant autour du seuil produirait une paire de lignes par oscillation).
- ⚠️ **Conséquence assumée, énoncée honnêtement : il existe une bande de verrouillage STABLE.** Le redémarrage d'auto-guérison mord à **4 096 o**, la garde à **8 192**. Entre les deux, **toute l'interface web est refusée et AUCUN redémarrage ne survient** — le régime est **stable**, pas transitoire, et le plateau ayant décru de façon monotone (42 996 → 26 612 → 20 468 → 15 860 → 13 300) cette bande est **atteignable**. La conclusion opérationnelle ne bouge pas — un `503` vaut mieux qu'un plantage — mais la justification confortable (« on ne peut pas rester verrouillé dehors, le reboot finira par arriver ») est **fausse** et n'est pas reprise.
- ⚠️ **En famine, `/wifi.html` et `/` sont refusés** : le **portail AP de secours** et la **page OTA** deviennent inaccessibles **par le navigateur**. Les chemins de secours réels survivent : **`POST /update`** (méthode POST → hors de la garde), **`./deploy.sh ota-firmware`**, et toutes les routes de diagnostic.
- **Un `WARN` sans son `INFO` n'est pas un défaut** : si la famine persiste sans visiteur, l'épisode est **réellement en cours**. Le seul avertissement vraiment orphelin est celui interrompu par un redémarrage — auquel cas, lors d'un post-mortem, **une bannière de démarrage clôt un épisode au même titre qu'un `INFO`**.
- **Aucun nouvel endpoint, aucun topic MQTT, aucun message WebSocket, aucun réglage.** Le WebSocket, MQTT, la régulation et toutes les routes `POST` sont **inchangés et servis** pendant la famine.
- **Empreinte** : Flash 1 824 525 → **1 825 717 o** (**+1 192**), marge **9 291 o** — **99,5 % occupés**. La cible du plan était de 600 o : **dépassement assumé et consigné**, les trims restants valaient ~90 o et **dégradaient la garde**. Tests : **33 suites / 1 286 cas**, **7 mutants tués sur 7**.

### Documentation

- **Nouvel [ADR-0067](docs/adr/0067-garde-503-avant-fopen-handler-prioritaire.md)** : « Le refus 503 précède le premier `fopen` : handler prioritaire, et l'ordre d'enregistrement FAIT la politique d'exemption ». Y sont consignés le **handler prioritaire** avec sa double disqualification du filtre `serveStatic`, le **6ᵉ barreau à 8 192 o** avec son dimensionnement chiffré et son **interdiction de baisse sous 6 144**, l'arbitrage du **coût de mesure sans cache ni bornage** (borne par construction, autolimitation sans objet, couplage coût/fragmentation), et l'**interdit `ERROR`/`CRITICAL` dans tout chemin anti-`fopen`**. Alternatives écartées : filtre sur `serveStatic`, helper aux 7 routes, drapeau global + `onNotFound`, réponse émise depuis le filtre, liste d'exemption explicite, garde sur le **libre total** (aveugle : 58 424 o libres alors que le plus gros bloc valait 15 860 et que 11 allocations échouaient), seuils 6 144 (ambigu avec le barreau WS) et 12 288 (UI perdue à 1 012 o du plateau), mise en cache de la mesure, et le parachute OOM (**structurellement absent** de ce chemin).
- `docs/adr/0058-gardes-heap-metrique-8-bits.md` : **amendement daté** ajoutant le 6ᵉ barreau à l'échelle (les 5 seuils **inchangés**), et la conséquence qui n'y était pas visible — la bande stable `4 096 ≤ maxBloc8 < 8 192`.
- `docs/subsystems/web-server.md` : la garde 503 (mécanisme, tableau **route par route** de ce qui est servi/refusé en famine, seuil, coût, journal), l'**ordre d'enregistrement comme politique d'exemption**, l'avertissement que **toute route `GET` ajoutée après ce point sera couverte**, et la nuance interception/évaluation pour les `POST`.
- `docs/subsystems/logger.md` : la chaîne complète `ERROR` → flush synchrone → `fopen` → `abort()` écrite comme **interdit général**, les 2 émetteurs de la garde et leur débit, et `flushToDisk()` noté comme **second `fopen` non couvrable par un 503**.
- `docs/API.md` : le code **503** entre dans le tableau des codes d'erreur, et une section dédiée documente le refus sur **tout GET de fichier statique ou de page HTML** (corps, `Retry-After`, routes concernées) ainsi que **la liste de ce qui reste servi en famine** — c'est elle qui permet de diagnostiquer l'appareil quand son interface est refusée.
- `docs/user/src/guide-utilisation.html` : **nouvelle entrée au chapitre « Que faire si… »** — « le navigateur affiche “Mémoire insuffisante, réessayez” ». C'est le **seul texte de cette version que verra un utilisateur**, et il est anxiogène sans explication : quelqu'un qui le lit peut croire à une panne et **aller couper le courant**, exactement la mauvaise réaction. L'entrée dit ce qui s'affiche, **que la régulation pH/ORP, la filtration, les mesures et les sécurités ne sont pas affectées**, qu'il faut attendre une dizaine de secondes et recharger, et — si le message se répète — télécharger le journal (Paramètres → Avancé → Journal) où chaque épisode est tracé même quand l'affichage était indisponible.
- **PDF régénérés** : `docs/user/guide-d-utilisation-v2.41.pdf` et `docs/user/guide-regulation-v2.41.pdf` ; les PDF `v2.38` sont **supprimés** (flotte mono-appareil, pas d'archive). `docs/user/README.md` : état courant v2.38 → **v2.41**.
- **Aucune capture d'écran à refaire** : aucun écran ne change, le message est un texte transitoire du navigateur.

---

## [2.40.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration, aucun réglage modifié, aucune limite de sécurité touchée. Aucun écran ne change, aucun bouton ne bouge.** **Pourquoi cette version existe** : quand l'appareil manque de mémoire, il écrit un rapport d'autopsie qu'il relit au démarrage suivant. Ce rapport se **trompait de conclusion** — il annonçait une « suspicion de corruption » alarmante alors que la mémoire n'était ni corrompue ni épuisée : c'est un **type précis** de mémoire (celui réservé aux transferts rapides) qui manquait. La version 2.40 fait mesurer l'appareil **au moment exact du refus**, et non plus deux secondes plus tôt sur une mesure qui ne répondait pas à la question. Un message d'erreur grave et faux disparaît ; un diagnostic juste le remplace.

### Firmware

- **La mesure est prise AU MOMENT DU REFUS, plus deux secondes avant.** Le verdict d'autopsie confrontait jusqu'ici la taille refusée à un échantillon **périmé de 0 à 2 s** et mesuré sur des **capacités fixes**. Il fait désormais **deux mesures contemporaines** : une sur les capacités **réellement demandées** par l'allocation qui a échoué, une sur une **référence** `INTERNAL|8BIT` qui sert de point de comparaison.
- **Verdict neuf : `FAMINE DMA (pool restreint)`**, journalisé en **WARN** — ce n'est pas une corruption. Il nomme exactement le régime observé le 05/09 : **11 échecs d'allocation en 1 h 12**, sans redémarrage, tous sur **1 622 octets** en `caps=0x080C` (`8BIT|DMA|INTERNAL`), pendant que le tas 8 bits annonçait **26 612 octets** de plus gros bloc libre — 16 fois la taille refusée.
- ⚠️ **Un `CRITICAL` faux disparaît. `SUSPICION CORRUPTION` n'est plus atteignable sans mesure contemporaine.** C'était la conclusion **par défaut** de l'ancienne règle, et elle était fausse sur le régime réel de l'appareil. Sans sonde représentative, le verdict rend désormais **`INDÉTERMINÉ`** — et il dit **pourquoi** (`sonde=oui / absente / ISR / désactivée / autre refus`). Un avertissement grave et faux coûte plus cher qu'une indétermination honnête.
- ⚠️ **L'interdit qui bloquait ce diagnostic depuis trois cycles était FAUX dans sa justification.** Le code et la doc interdisaient tout `heap_caps_*` depuis le chemin d'échec pour cause de **deadlock**. Le **désassemblage de `libheap.a`** le réfute : `heap_caps.c.obj` ne contient **aucune relocation** vers `multi_heap_internal_lock`/`_unlock`, et `heap_caps_alloc_failed` est statique en `.iram1.0`, atteinte seulement **après le retour complet** de `heap_caps_malloc_base` — **aucun verrou de tas n'est détenu**. L'interdit reste valable, mais c'est un **coût interruptions coupées**, pas un risque de réentrance. Formulation désormais retenue, et honnête : « sans verrou pris **par nous** » — `multi_heap_get_info_impl` en prend bien un, brièvement.
- **L'exception est bornée par quatre garde-fous cumulés** : **1 échantillon par épisode** (jeton atomique réarmé par la boucle, donc suivant le backoff existant), **plancher de 1 000 ms**, **jamais depuis une ISR** (`xPortInIsrContext()`), **désactivation définitive au-delà de 1 500 µs**. Plus un WARN de conservation à **300 µs**, et une **borne a priori** — parce que l'auto-limitation est post-hoc par construction : le dépassement qui la déclenche a déjà eu lieu.
- ⚠️ **Le fail-safe de la borne PSRAM a failli être écrit à l'envers, et le défaut était bloquant.** La première écriture testait la **présence** du bit `MALLOC_CAP_SPIRAM`. Or tout `malloc()` / `new` ordinaire remonte `caps = MALLOC_CAP_DEFAULT` (`0x1000`), **sans** ce bit — c'est la classe d'échec des crashs du 11/08 et du 13/08, celle qui fonde tout le chantier. Le tas PSRAM étant enregistré `DEFAULT|SPIRAM|8BIT|32BIT`, la sonde aurait parcouru **4 Mo interruptions coupées** et se serait **désactivée dès le premier `new` refusé** : morte sur son cas d'usage principal. Le test porte désormais sur `MALLOC_CAP_INTERNAL` — parcours complet **uniquement** si le masque **garantit** la RAM interne —, verrouillé par un `static_assert` qui casse la **compilation du firmware** si le bit IDF change.
- **Zone post-mortem RTC : 32 → 48 octets**, magic **`0xF00DFA12` → `0xF00DFA13`**. Sans ce bump, les 4 mots neufs seraient lus dans de la RAM RTC **non initialisée** : des valeurs de sonde **inventées et crédibles**. Le post-mortem du boot de bascule est perdu, c'est accepté. Budget RTC noinit : 1 160 → **1 176 o** sur 8 192.
- ⚠️ **L'octet `flags` de cette zone est désormais SATURÉ — 8 bits sur 8.** Tout drapeau futur exigera un mot supplémentaire, donc un nouveau bump, donc la perte d'une trace.
- **Le dump du démarrage passe à DEUX lignes**, la 2ᵉ n'étant émise **que si une sonde existe** — afficher `maxBloc=0 libre=0 … en 0 µs` ferait passer des zéros pour une mesure. Fusionner les deux aurait économisé ~120 o de flash mais ramené le **risque de troncature silencieuse** corrigé au lot précédent, et la troncature frappe la **fin** de la ligne, c'est-à-dire le verdict.
- **Le WARN d'échec vivant porte la sonde** (ou dit `sonde=absente`). Son tampon passe de `[160]` à `[256]` : un `[160]` aurait tronqué **silencieusement** le message de diagnostic — exactement le défaut que ce lot supprime.
- **Horloge de la sonde : CCOUNT**, pas `esp_timer_get_time()`. Une instruction, **aucun verrou** — `esp_timer_get_time()` prend un `portMUX` dans certaines implémentations ESP32, rédhibitoire dans un chemin appelable en ISR. Le compteur étant **par cœur**, une migration de tâche marque la durée invalide **sans** appliquer l'auto-limitation : une migration ne doit jamais désactiver la sonde à vie.
- **Item A16 — la dernière garde 503 restée sur la métrique brute bascule.** `GET /debug/sensor_trace` comparait sa taille de réponse à `ESP.getMaxAllocHeap()`, qui compte l'**IRAM 32 bits inutilisable** par le `new` concerné. Elle lit désormais `heapLargestFreeBlock8Bit()`. **Seuil inchangé : on corrige la mesure, pas la marge.** Écart quasi nul en régime normal (**1 024 o** mesurés le 05/09), mais la brute **reste figée en famine** pendant que le 8 bits s'effondre — la garde cessait alors de protéger tout en paraissant fonctionner.
- **Aucun nouvel endpoint, aucun topic MQTT, aucun message WebSocket, aucun réglage.** Le signal sort par le **journal** et par quatre champs additifs de `/get-system-info`.
- **Empreinte** : Flash 1 821 797 → **1 824 545 o** (+2 748), marge **10 463 o** — **99,4 % occupés**. Tests : **32 suites / 1 261 cas**, **26 mutants tués sur 26**.

### Documentation

- **Nouvel [ADR-0066](docs/adr/0066-sonde-du-tas-au-moment-du-refus.md)** : « Le tas est mesuré au moment exact du refus, sur les caps de l'échec **et** sur une référence 8 bits » — trois décisions liées (requalification de l'interdit `heap_caps_*` avec sa preuve par désassemblage, double mesure, `SUSPICION CORRUPTION` rendu inatteignable sans sonde contemporaine). Alternatives explicitement écartées : **caps fixes seules** (reproduisent littéralement le faux verdict du 05/09), **caps de l'échec seules** (verdict non attribuable, aucun point de comparaison), lever l'interdit sans bornage, **sauter** la sonde sur masque PSRAM, ne pas bumper le magic, `esp_timer_get_time()` comme horloge, journaliser depuis le hook. Y sont consignés l'**octet `flags` saturé**, la troncature de `probeCaps` à 16 bits et le **fail-safe de la borne PSRAM**.
- `docs/subsystems/oom-guard.md` : la contrainte structurante **requalifiée** (le mot « deadlock » retiré, la preuve par désassemblage citée, les 4 garde-fous tabulés), le format de zone **48 o / 12 mots / magic `0xF00DFA13`** avec les 4 mots neufs, les **3 drapeaux** ajoutés et la saturation de l'octet, la **règle de verdict révisée** intégralement réécrite, les **5 sous-cas d'indétermination attribuables**, le **dump à 2 lignes** avec sa politique de niveau, les 3 lignes d'observabilité en vol, les 5 constantes `kOomProbe*`, la sémantique **élargie** de `kOomCorruptionSaneBlockRatio`, la bascule de la garde 503 `/debug/sensor_trace`, et le **verdict de la prédiction falsifiable de B-1** (n°2 confirmée, n°1 **réfutée** : `minFen` à 1 908 o, plateau monotone décroissant passé **sous** le seuil de réarmement du parachute).
- `docs/API.md` : section `oom_probe_*` — les 4 champs additifs de `/get-system-info` et leur **protocole de lecture** (durée < 300 µs, sonde non désactivée, capacités des échecs), avec l'avertissement qu'il s'agit d'une métrique de l'**instrument**, pas de la santé du tas.
- **Aucun guide utilisateur ni PDF régénéré, aucune capture d'écran à refaire** : ce lot est de l'**instrumentation interne**, aucun écran ne change.

---

## [2.39.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration, aucun réglage modifié, aucune limite de sécurité touchée. Aucun écran ne change.** **Pourquoi cette version existe** : quand vous changez un réglage dans un onglet, **les autres onglets ouverts se mettent maintenant à jour tout seuls**, sans rechargement — un téléphone et un ordinateur affichant l'appareil en même temps ne se contredisent plus. Jusqu'ici, une vingtaine de champs restaient figés chez les autres clients connectés jusqu'à leur prochain rechargement de page : la **jauge de bidon** (qui avance à chaque injection), la **vitesse de correction**, la **taille du bassin**, le nombre de **cycles de dosage par jour**, et surtout le **motif d'une bascule automatique de la régulation pH en manuel** — c'est-à-dire l'information de sécurité qu'on souhaite le moins découvrir avec dix minutes de retard.

### Fonctionnalités

- **Les onglets ouverts restent d'accord entre eux.** Après une sauvegarde de réglage (depuis l'interface web **ou** depuis Home Assistant), tous les clients connectés reçoivent les nouvelles valeurs en moins de 5 secondes. Cela vaut désormais aussi pour la jauge de bidon pH/ORP, la vitesse de correction, la taille du bassin, la limite de cycles de dosage par jour, et l'horodatage + le motif d'une bascule automatique de sécurité de la régulation pH.
- **Aucun nouveau réglage, aucun nouveau bouton, aucune nouvelle entité Home Assistant, aucun nouveau topic MQTT.** Seul le rafraîchissement multi-clients s'améliore.

### Firmware

- **Parité de la trame `config` avec `/get-config` : 15 clés ajoutées.** `/get-config` servait **89** clés, la trame WebSocket **66**. Les 15 clés ajoutées sont `max_cycles_per_day`, `regulation_speed`, `pool_size`, `ph_auto_manual_epoch`, `ph_auto_manual_reason` et les 10 clés de suivi produit pH/ORP.
- ⚠️ **Le correctif évident aurait été pire que le bug — et c'est la vraie décision de cette version.** Ajouter les 23 clés manquantes coûtait **+1 365 o** sur une trame dont le plafond est **dur** (`kWsFrameBufBytes` = 3 584 o) : en cas de dépassement, la garde **saute le push**, elle **ne tronque pas**. On aurait remplacé des champs manquants chez certains clients par **une trame absente pour tous les clients**.
- **Critère retenu, qui contraindra tous les ajouts futurs** : *la trame porte l'état **mutable à l'exécution** et **non dérivable** ; tout le reste est HTTP-seul et **déclaré***. Les 6 bornes de réglage pH et `regulation_defaults_by_size` sont des **constantes de compilation** (un client les lit une fois) ; `regulation_defaults` est **dérivable** côté client depuis `pool_size` — désormais poussé — et le cache des colonnes. **8 exceptions déclarées**, chacune avec un motif écrit.
- **Budget de trame** : pire cas **≈ 2 948 o** pour 3 584, **marge 636 o**. Gabarit `StaticJson<>` porté **2560 → 2944** — **documentaire** en ArduinoJson 7, donc **0 octet de RAM**.
- ⚠️ **Un chiffre de dimensionnement rectifié en revue, faux d'un facteur 2,7.** Le bloc annonçait **167 o** pour `ph_auto_manual_reason` (« ~140 caractères »). Les chaînes réelles, reconstruites depuis leur **unique écrivain** (`setPhAutoManualTrace`), font **411 caractères / 427 octets**. Coût réel **452 o**, ajout total **+866 o**. **Aucun dépassement, rien à corriger dans le code** — mais c'est ce chiffre que lira le prochain contributeur, et **le pire cas de longueur coïncide avec l'escalade de sécurité pH**, donc le moment où perdre la trame coûterait le plus cher.
- **Une sentinelle remplace une discipline orale qui a échoué quatre fois.** Ce motif de parité en est à sa **4ᵉ occurrence documentée** ; la leçon retenue avait toujours été « penser à vérifier des deux côtés ». La nouvelle suite native `test/test_native_ws_config_parity/` lit le **texte source** entre des **marqueurs commentés** (`PARITE-CONFIG:DEBUT/FIN`, et non des bornes de fonction qui casseraient au premier refactor) et casse le build sur : clé HTTP non couverte · **clé WS-seule** (le sens **dangereux** : un champ WS-seul ne disparaît pas, il **clignote**) · **marqueur absent** (message explicite « restaurer le marqueur, **ne pas désactiver le test** ») · **exception morte** (une clé exemptée qui n'existe plus côté HTTP — c'est elle qui empêche la liste de devenir un cimetière de justifications périmées). Chaque exception porte un motif écrit d'au moins 20 caractères, **asserté**. **Coût : zéro octet de flash** (test natif).
- **Angle mort assumé et documenté** : l'extraction est **textuelle**, une clé mise en commentaire entre les marqueurs reste comptée. La sentinelle vise **la distraction, pas la malveillance** — parser du C++ ici serait un remède pire que le mal.
- **Empreinte** : Flash **1 821 813 / 1 835 008 o**, marge **13 195 o**. Tests : **32 suites / 1 232 cas**, **4 mutations tuées**.

### Documentation

- **Nouvel [ADR-0065](docs/adr/0065-trame-websocket-etat-mutable-pas-constantes.md)** : « La trame WebSocket porte l'état mutable, pas les constantes » — le critère de partage, les 15 ajouts et les 8 exclusions, la **sentinelle comme mécanisme qui rend la règle exécutoire**, et les alternatives chiffrées écartées (parité intégrale → marge ~100 o et trame abandonnée au premier topic MQTT long ; extraction de `fillRegulationDefaults` → +703 o pour une valeur dérivable ; relèvement de `kWsFrameBufBytes` → +512 o de **RAM permanents** pour transporter des constantes ; trame différentielle ; analyse syntaxique C++ ; consigne écrite seule).
- `docs/subsystems/ws-manager.md` : les 15 champs, le décompte d'octets **rectifié** clé par clé, le gabarit 2560 → 2944, la marge de 636 o, les **8 exceptions avec leur motif**, et la sentinelle (rôle, 4 assertions, marqueurs, angle mort).
- `docs/API.md` : section « Message `config` — contrat de parité » — la trame est un **snapshot complet**, **pas** un miroir de `/get-config` ; un intégrateur doit continuer d'appeler `/get-config` pour les 8 clés HTTP-seules. Corrige au passage une affirmation fausse (« les mises à jour suivantes sont différentielles »).
- `docs/features/page-settings.md` : rafraîchissement multi-onglets des réglages, et la reconstruction de `regulation_defaults` depuis `pool_size` + cache `by_size`.
- **Aucun guide utilisateur ni PDF régénéré, et aucune capture d'écran à refaire** : aucun écran, aucun libellé, aucun réglage ne change — seul le rafraîchissement multi-clients s'améliore, ce qui ne se photographie pas.

---

## [2.38.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration, aucun réglage modifié, aucune limite de sécurité touchée.** **Pourquoi cette version existe** : jusqu'ici, analyser le comportement de la régulation exigeait un **enregistreur externe** — un script tournant sur un Mac qui devait rester éveillé et interroger l'appareil toutes les 10 à 60 secondes. Une simple mise en veille a déjà **troué un relevé** en pleine analyse. L'appareil enregistre désormais lui-même, dans son propre historique, **la mesure sur laquelle sa régulation décide**. Plus d'ordinateur à garder allumé, plus de relevé perdu.
>
> **Ce que vous verrez changer** : sur le tableau de bord, les mini-graphiques pH et chlore affichent maintenant **deux courbes** — la mesure **lissée** (celle du grand nombre, celle qui décide) en courbe principale, la mesure **brute** en second plan. Les graphiques des pages pH et Chlore, eux, **ne changent pas**.

### Fonctionnalités

- **La courbe dit enfin la même chose que le nombre au-dessus d'elle.** Le grand chiffre des cartes pH et Chlore affichait **déjà** la mesure **lissée** — celle que la régulation utilise pour décider — alors que la courbe juste en dessous traçait la mesure **brute**. Deux signaux différents dans la même carte : cette incohérence a produit **deux mauvais diagnostics** par le passé. La version 2.38 ne « promeut » rien : elle **aligne la courbe sur le nombre qu'elle est censée expliquer**.
- **Deux courbes, une hiérarchie claire.** Le **lissé** est la courbe principale (trait épais, zone colorée) ; le **brut** est un trait fin en arrière-plan. Le survol donne les deux valeurs.
- **Un interrupteur « Brut » sous chaque graphique**, indépendant pour le pH et pour le chlore, **mémorisé** d'une visite à l'autre. Il est **activé par défaut**. Le masquer a un vrai bénéfice : l'échelle verticale **se resserre sur la mesure lissée**, et les petites variations deviennent lisibles.
- **La courbe lissée ne remonte qu'à ~8 heures**, la fenêtre affichée va jusqu'à 24 h. C'est voulu (voir plus bas). Pour que cela ne **ressemble pas à une panne**, trois choses : le brut couvre **toute** la fenêtre, le lissé est dessiné comme une **zone** (une zone qui commence évoque une période, une ligne qui s'arrête évoque une panne), et le brut **change d'intensité exactement à la même frontière**. On lit un seul événement, pas deux.
- **Quand il n'y a aucune mesure lissée sur la période affichée** (la première heure après la mise à jour, ou une journée entièrement antérieure à cette version), la légende **le dit** : « Lissé (régulation) — *aucune sur la période* ». La couleur ne porte jamais seule l'information.
- **Le pH des mini-graphiques passe à 2 décimales** : à 0,1 près, le pas d'affichage était **plus large que l'amplitude utile** du signal lissé.
- **Les graphiques des pages pH et Chlore restent inchangés** — ils résument **une journée par point**, échelle à laquelle la mesure lissée n'apporte rien (elle y serait indiscernable du brut). Seuls les mini-graphiques du tableau de bord sont touchés.
- **Aucun réglage nouveau, aucune entité Home Assistant nouvelle, aucun topic MQTT nouveau.**

### Firmware

- **Chaque point d'historique 5 min porte désormais la mesure lissée pH et chlore** (`phFiltered` / `orpFiltered`), lue par **les mêmes accesseurs que `/data`** — donc exactement la valeur affichée et utilisée par la régulation au même instant. **Aucun repli sur le brut** : filtre non amorcé ⇒ valeur **absente**, jamais maquillée. Mesure non représentative (filtration arrêtée, purge) ⇒ la lissée subit **le même sort** que le brut.
- ⚠️ **La lissée n'est historisée QUE sur les points 5 min (~8 h de profondeur) — et le motif est MESURÉ, pas supposé.** L'historiser aussi sur les moyennes horaires et journalières coûtait **+7 119 o** et portait la partition `history` à **16 blocs sur 16** : `historySaveAdmit()` aurait alors **refusé la sauvegarde**. En 5 min seulement : **+2 016 o**, **12/16 blocs**, résiduel **8 102 o**, et la profondeur horaire (7 jours) **intacte**. Second argument, indépendant : la lissée est un signal passe-bas, sa moyenne horaire **coïncide** avec celle du brut — on aurait payé 5,1 Ko pour une série **redondante**.
- ⚠️ **À connaître : le mode de défaillance du budget de partition est le SILENCE, pas la corruption.** Un dépassement ne casse rien de visible — la sauvegarde est simplement **refusée**, un avertissement throttlé 1/h est journalisé, et l'interface continue d'afficher l'historique qui vit en mémoire. **La perte n'apparaît qu'au redémarrage suivant.** C'est pourquoi ce budget est chiffré et non estimé.
- **La lissée n'est JAMAIS moyennée** : ni accumulée en consolidation horaire/journalière, ni fusionnée à la déduplication (ne rien écrire garantit qu'aucune valeur n'est **inventée**). Le compteur d'échantillons valides partagé est **intact** — les moyennes produisent exactement les mêmes valeurs qu'en 2.37.
- **Asymétrie voulue entre le fichier et le fil.** Dans `/history.json` : clés **courtes** (`pf`, `of`), clé **omise** si la valeur n'est pas finie — chaque octet compte sur une partition de 64 Ko. Sur `/get-history` : clés **longues** (`phFiltered`, `orpFiltered`), **toujours présentes**, valeur **`null`** en cas d'absence — c'est le contrat existant de `ph` / `orp`.
- **`POST /history/import` relit les deux nouvelles clés** : l'aller-retour export → import → export restitue la lissée à l'identique. Sans cela, restaurer une sauvegarde l'aurait perdue **sans le moindre message**.
- **Correction — une perte silencieuse de points d'historique est fermée.** Si le JSON d'un point dépassait la taille maximale, il était **sauté sans un mot**, en produisant un JSON parfaitement valide : un **trou de courbe indiscernable d'une panne de capteur**. Désormais les omissions sont **comptées** et **un seul** avertissement throttlé 1/h est émis à la clôture de la réponse, disant **combien**, **pourquoi** et **quoi faire**.
- **`kHistoryPointJsonMaxBytes` : 128 → 192 o**, décompte du pire cas réaliste refait ligne à ligne dans le commentaire de la constante (**161 o**, dont 114 de clés et ponctuation). Coût : +64 o d'état par requête `/get-history`. `/download-logs` n'est pas concerné.
- **Nouvelle constante `kHistorySkipWarnThrottleMs`** (1 h) — même cadence que les avertissements de sauvegarde d'historique.
- **`sizeof(DataPoint)` 20 → 28 o, verrouillé par `static_assert`.** Ce n'est pas cosmétique : c'est ce chiffre qui dimensionne l'allocation **contiguë** de `/get-history` — **9 492 o d'un seul bloc** demandés en contexte `async_tcp`, sur un appareil dont le plus gros bloc libre tourne autour de 28-30 Ko. Le `static_assert` **force à recompter** au lieu de le découvrir en production.
- **Initialiseurs `= NAN` obligatoires** sur les deux nouveaux champs : `DataPoint` est construit **sans initialisation explicite** à l'import et dans les agrégats — sans eux, on persisterait de la mémoire non initialisée, affichée ensuite comme une mesure réelle.
- **Rétrocompatibilité totale, aucune migration** : un `/history.json` écrit par une version ≤ 2.37 se relit sans perte (lissée à « absente ») ; un retour arrière OTA ignore simplement les clés inconnues.
- **Première utilisation de `localStorage` dans le projet** (`poolctl.ui.showRaw.ph` / `.orp`), **entièrement protégée** : `localStorage` peut lever **à la lecture comme à l'écriture** (navigation privée, stockage désactivé par politique). Repli en mémoire, et un échec ne remonte **jamais** à l'utilisateur — la préférence vaut alors pour la session.
- **Empreinte** : Flash **1 821 097 / 1 835 008 o**, marge **13 911 o**. Partition `history` : **12/16 blocs**, résiduel **8 102 o**. Tests : **31 suites / 1 226 cas**, **7 mutations tuées** (lissée agrégée au lieu d'être forcée absente ; clé écrite sur un agrégat ; clé omise au lieu de `null` sur le fil ; import ignorant les nouvelles clés ; arrondi du pH lissé au dixième ; compteur de points omis jamais incrémenté ; garde de granularité retirée de la sérialisation).

### Documentation

- **Nouvel [ADR-0064](docs/adr/0064-la-courbe-suit-le-nombre-affiche.md)** : « La courbe suit le nombre affiché » — les quatre règles (lissée sur les points 5 min, jamais sur les agrégats, principale sur les courbes 5 min, asymétrie fichier/fil), le budget de partition chiffré, et les **huit alternatives écartées** (toutes granularités, réduction de la profondeur horaire, clés courtes sur le fil, omission de clé sur le fil, historisation du signal lent de décision, remplacement du brut, promotion sur les écrans détail, omission du brut).
- `docs/subsystems/history.md` : distinction explicite entre le point **persisté** et le point **exporté** (la page les confondait), budget de partition **avant/après** chiffré, règle d'agrégation, décompte de `kHistoryPointJsonMaxBytes`, section dédiée au **mode de défaillance silencieux** du budget, et la nuance documentée de `getPhFiltered()` **sans garde d'âge** (volontairement non corrigée).
- `docs/API.md` : `phFiltered` / `orpFiltered` sur `/get-history` (type, arrondi, `null`, « uniquement sur les points RAW », rétrocompatibilité) et leur relecture par `POST /history/import`.
- `docs/features/page-dashboard.md` : les deux séries, l'interrupteur « Brut » mémorisé, la frontière ~8 h et ses trois dispositifs, la légende d'absence. `page-ph.md` / `page-orp.md` : mention explicite que **les graphiques d'historique restent bruts**.
- **Guide d'utilisation régénéré** (`guide-d-utilisation-v2.38.pdf`) : nouvelle explication des deux courbes et de l'interrupteur « Brut ». Le guide de régulation est régénéré par le même script — **la capture du tableau de bord est à refaire** (elle ne montre pas encore les légendes). Les PDF de la version antérieure (v2.37) sont supprimés.

---

## [2.37.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration, aucun réglage modifié, aucune limite de sécurité touchée.** Cette version **n'ajoute aucun automatisme** : elle ajoute un **instrument de mesure**. L'appareil chronomètre désormais lui-même, chaque jour, le temps qu'il met à ramener le pH et le chlore sur leur consigne — et l'affiche sur les pages pH et ORP. C'est cette mesure qui dira, chiffres à l'appui, si les versions récentes de la régulation font vraiment mieux que les précédentes.

### Fonctionnalités

- **Nouveau : « Atteinte de la cible » sur les pages pH et ORP.** Une ligne annonce où en est la journée — « en cours — 1 h 10 », « en 3 h 53 », « non atteinte », « déjà à la cible au départ » — accompagnée de l'écart qu'il y avait au départ. En dessous, **sept pastilles** résument les sept derniers jours : verte si la cible a été atteinte **en moins de 2 heures**, ambrée au-delà, grise si elle ne l'a pas été, pointillée si la journée est absente de la mémoire.
- **Le chronomètre compte des minutes de CIRCULATION, pas des heures de montre.** Une nuit sans filtration ne fait pas vieillir la journée : sans eau qui circule, il n'y a ni mélange ni mesure représentative, donc la régulation ne progresse pas. Deux journées de durées de filtration différentes redeviennent ainsi comparables.
- **« Atteinte » a un sens précis : la régulation ne redémarrerait pas.** La cible est déclarée atteinte quand l'écart reste sous le seuil qui déclencherait un dosage (0,05 pour le pH, 15 mV pour le chlore) pendant **30 minutes de circulation d'affilée**. Le temps affiché est celui du **début** de ces 30 minutes — la demi-heure suivante n'a fait que le prouver.
- **Certaines journées sont marquées d'un astérisque et de hachures : elles ne sont pas comparables aux autres.** Cinq circonstances les signalent : consigne modifiée en cours de journée, boost chlore actif, stabilisation des mesures jamais atteinte, coupure de courant, et **régulation non automatique** (mode manuel, maintenance, assistant de mesure de débit). Une liste sous la bande dit lesquelles.
- ⚠️ **Un point à comprendre pour ne pas mal lire l'indicateur** : quand la régulation n'a **pas** atteint sa cible parce qu'elle a épuisé son quota journalier, buté sur la limite horaire ou saturé sa pompe, la journée est marquée « **non atteinte** » **sans astérisque**. Ce n'est pas une erreur : c'est une **vraie** non-atteinte, et c'est exactement ce que l'instrument doit dénoncer. L'astérisque est réservé à ce qu'on **impose** à la régulation de l'extérieur.
- **Le journal raconte la journée** : une ligne `[KPI]` au départ du chronomètre, une à l'atteinte de la cible, une à la clôture — avec l'écart initial et l'écart courant.
- **Une mesure aberrante ne démarre pas le chronomètre.** Si l'écart initial dépasse 1,0 pH ou 300 mV, l'appareil **refuse de démarrer** (un avertissement, une seule fois) et attend la première mesure plausible. Cas réel du 05/09 au matin : de l'air dans le circuit avait fait plonger le pH brut à 5,45 — un tel départ aurait pollué la mémoire des sept jours.
- **Une coupure de courant en cours de journée ne remet pas le compteur à zéro** : le départ du jour est relu, les minutes restent exactes, seule la période de tenue repart de zéro (l'eau s'est arrêtée). La journée est marquée « reprise après coupure ».
- **Aucune nouvelle entité Home Assistant, aucun nouveau topic MQTT** — volontairement.
- **Le tableau de bord n'est pas touché** : les cartes y sont déjà denses, et c'est une grandeur d'analyse, pas une mesure d'eau.

### Firmware

- **Nouveau module pur `time_to_target_logic.{h,cpp}`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : machine d'états 0-3, garde de vraisemblance, tenue, clôture, anneau 7 jours et sceaux. La coquille `time_to_target.{h,cpp}` ne fait que le câblage (mesures, heure, circulation, NVS, RAM RTC, journal).
- **On juge sur la GRANDEUR DE DÉCISION, pas sur la mesure filtrée** — règle **uniforme**, réalisation **asymétrique** parce que les deux régulateurs le sont : pour le pH c'est le **signal lent** de la v2.36.0 (moyenne 10 min), pour l'ORP c'est la **filtrée**, qui **EST** sa grandeur de décision. Un juge qui n'observe pas la grandeur sur laquelle le régulateur agit ne mesure pas le même objet.
- ⚠️ **Conséquence assumée : le zéro du chronomètre pH est indexé sur `kPhDecisionWindowMs`.** Il démarre ~10 min après l'ouverture de la porte de mesure, le temps que la moyenne se constitue. **Toute modification future de cette constante crée une discontinuité de série** entre les journées archivées. Un test le démontre : la même courbe du 04/09 donne **233 min** si le départ part du brut et **223 min** sur la grandeur de décision — les 10 min d'écart sont exactement la fenêtre. L'anneau NVS porte une **estampille de série** et le démarrage émet un avertissement si elle a changé : la discontinuité est **lisible plutôt que silencieuse**.
- **Base de temps `SafetyLimits::filtTotalMin` (v2.34.0) réutilisée telle quelle, en lecture seule** — et nommée honnêtement : ce sont des minutes de **présence d'eau**. **Libellé « minutes de circulation » partout, jamais « de filtration ».**
- **On juge contre la consigne EFFECTIVE, on surveille la CONFIGURÉE** : un boost chlore ne lève **pas** le drapeau « consigne modifiée » (juger contre 530 mV pendant que le régulateur vise 630 déclarerait une atteinte fictive). Le boost est tracé en **bit 1**.
- **NVS `pool-kpi`, namespace DISJOINT de `pool-daily`.** L'argument n'est ni l'espace ni la corruption de page — les namespaces partagent les mêmes pages — mais le **droit de destruction en masse** : le namespace est l'unité de `clear()`. **Moindre privilège : un non-garde-fou ne détient pas la clé d'écriture des garde-fous.** `filt_total` y est lu en lecture seule.
- **Persistance à deux étages, exactement 2 écritures par jour et par régulation** : le départ, l'écart initial, la date et les drapeaux sont écrits **une seule fois** à la pose du départ (c'est l'unique donnée irrécupérable) ; la clôture écrit la seconde et dernière fois. Tout le reste se recalcule. L'état vivant volatil (progression de la tenue) vit en RAM RTC avec un magic **propre** à cet instrument.
- **Rattrapage des journées orphelines** (`tttHistoryCloseOrphans`) : appareil éteint plus d'une journée ⇒ l'entrée « en cours, 0 min » de la veille est close en non-atteinte au démarrage. **Trois gardes** : heure invalide ⇒ on ne touche à rien (sinon un démarrage sans NTP détruirait sept journées) ; la journée du jour est épargnée ; une journée **atteinte** n'est jamais rouverte.
- **Sens unique, vérifié mécaniquement** : aucune sortie de l'indicateur n'est lue par `pump_controller`. La garde est un **test de grep bidirectionnel** exécuté à chaque `pio test`. Un `#error` de compilation avait été envisagé puis **écarté** : il aurait cassé le build du module lui-même, qui doit inclure `pump_controller.h` pour lire la grandeur de décision.
- **`GET /get-kpi`** (`RouteProtection::WRITE`, la même classe que `/data`) : mémoire 7 jours des deux voies, le plus récent en tête, **503 explicite** si la mémoire est indisponible — **jamais un tableau vide**, qui se lirait « aucune journée enregistrée ». Non bloquant : copie d'un miroir RAM de 188 o sous spinlock, aucune lecture NVS.
- **8 champs sur `/data` ET le WebSocket** (`{ph,orp}_ttt_state|_min|_e0|_flag`), même passage, même ordre, mêmes arrondis — parité prouvée par grep des deux côtés. `state` 4-255 réservés, `flag` en **champ de bits** : encodage **figé dès le premier déploiement** (il est aussi archivé en NVS).
- **Budget WebSocket** : +150 o au pire cas, trame **2 842 → 2 992 o**, gabarit documentaire porté **2944 → 3072**. **Marge résiduelle sous le seul plafond dur (`kWsFrameBufBytes` = 3584) : 592 o** (742 o avant). Aucun bump requis.
- **Nouvelles constantes** : `kTimeToTargetHoldMin` (30 min de circulation, `static_assert` : ≥ 2 fenêtres du signal lent), `kTimeToTargetHistoryDays` (7), `kTimeToTargetOffDayEndMin` (60 min murales), `kTttMaxPlausibleE0Ph` (1,0) / `kTttMaxPlausibleE0Orp` (300).
- **Empreinte** : Flash **1 819 517 / 1 835 008 o**, marge **15 491 o**. Tests : **31 suites / 1 202 cas**, mutations tuées (tenue 30 → 0 ; minutes murales au lieu de minutes de circulation ; départ reposé à chaque re-purge ; les trois gardes du rattrapage des orphelines).

### Documentation

- **Nouvel [ADR-0063](docs/adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)** : « Définition normative du temps d'atteinte de cible » — les six règles (grandeur de décision, « atteint » = « la régulation ne redémarrerait pas », minutes de circulation, consigne effective vs configurée, règle de partage du bit 4, garde de vraisemblance), la **garde anti-dérive** exigée par le gate `pool-chemistry` (« le KPI est un instrument ; toute rétroaction exige une nouvelle spec et un gate — supprimer le test de grep, c'est fermer la boucle »), et les **huit alternatives écartées** (filtrée brute, lissage propre au KPI, base de temps murale, base « filtration en marche », boost levant le bit 0, namespace NVS mutualisé, état vivant en RTC seul, `#error` de compilation).
- **Nouveau [`docs/subsystems/time-to-target.md`](docs/subsystems/time-to-target.md)** : rôle, machine d'états 0-3, base de temps, persistance à deux étages, cadence, logs `[KPI]`, rattrapage des journées orphelines, et la garde explicite « instrument de mesure, sens unique ».
- `docs/API.md` : `GET /get-kpi` (contrat, auth, exemple, borne 7 jours, 503) et les 8 champs de `/data` — dont le **sens variable** de `*_ttt_min` selon l'état et la table des bits du drapeau.
- `docs/subsystems/ws-manager.md` : les 8 champs de `sensor_data`, le décompte d'octets et la marge résiduelle de 592 o.
- `docs/features/page-ph.md`, `docs/features/page-orp.md` : ligne « Atteinte de la cible » (toutes ses formulations), les 7 pastilles (code couleur, seuil 2 h, hachures, `*`, jour absent), et la mention explicite que le tableau de bord n'est pas touché.
- **Guide utilisateur régulation régénéré** (`guide-regulation-v2.37.pdf`) : nouvelle section « Combien de temps la correction met-elle ? ». Le guide d'utilisation est régénéré par le même script — **la capture de la page pH est à refaire** (elle ne montre pas encore le nouveau bloc). Les PDF de la version antérieure (v2.36) sont supprimés.

---

## [2.36.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration, aucun réglage modifié.** Cette version corrige la façon dont la régulation pH décide d'ouvrir et de fermer ses dosages quand le pH est **déjà proche de sa consigne**. **Une chose à savoir en la regardant vivre** : l'indicateur « dosage pH en cours » — sur la page pH comme dans Home Assistant — **change de sens** (voir plus bas). **Aucune limite de sécurité n'a été touchée** : ni les quotas journaliers, ni la limite horaire, ni les seuils de refus, ni la pause de mélange.

### Fonctionnalités

- **Près de la consigne, la régulation pH n'hésite plus.** Jusqu'ici, quand le pH était déjà proche de sa cible, l'appareil ouvrait une session de dosage, la refermait quelques minutes plus tard, s'imposait alors 15 minutes de pause de mélange, recommençait… La coupable n'était pas l'eau du bassin : c'était la **petite agitation permanente de la mesure**, qui — mesurée — est **plus large que l'écart nécessaire pour déclencher ou arrêter un dosage** (0,043 d'écart-type contre une bande de 0,05 / 0,01). Résultat : **les deux tiers du temps de travail utile étaient perdus**. Désormais, la décision d'ouvrir ou de fermer une session se prend sur une **moyenne des 10 dernières minutes**, et une session ouverte **tient au moins 30 minutes**.
- **Ce que vous devriez constater** : des **injections plus longues et moins nombreuses**, des pauses de mélange plus rares, la **dose de base apprise servie en continu** au lieu d'un tiers du temps — et un pH qui devrait **se rapprocher de sa consigne** au lieu de se stabiliser légèrement au-dessus (le bassin était parqué à 7,38-7,40 pour une consigne à 7,30 : ce n'était pas son équilibre, c'était le symptôme du hachage. Mouvement attendu : −0,07 pH au maximum, **dans le bon sens**).
- **La dose calculée, elle, n'est pas ralentie.** Seule la **décision** d'ouvrir ou de fermer utilise la moyenne lente ; le calcul de la dose continue de suivre la mesure du moment. On ne ralentit pas la régulation, on stabilise sa porte.
- ⚠️ **Changement de sens de l'indicateur « dosage pH en cours »** — à lire avant de s'étonner. Il signifiait « la pompe tourne » ; il signifie maintenant « **une session de dosage est ouverte** ». Il peut donc **rester allumé pompe à l'arrêt** pendant la prolongation d'une session (jusqu'à 3 h au pire cas). Concerné : la page pH, le champ `ph_dosing` de l'API et du WebSocket, le topic MQTT `ph_dosing` et l'entité Home Assistant « Dosage pH Actif », le champ `ph_dosing_active` du diagnostic, et l'historique. **Aucun champ n'est ajouté ni renommé.** Une automatisation Home Assistant qui en déduisait « la pompe tourne » doit être revue : cette information reste disponible via le calendrier d'impulsion (`ph_pulse_phase_ms < ph_pulse_ms`). L'indicateur ORP est **inchangé**.
- **Trois motifs d'arrêt de session distincts au journal** là où « erreur résorbée » couvrait tout : arrêt normal, pH franchement passé sous la consigne, et mesure redevenue non représentative ne se lisent plus de la même façon.

### Firmware

- **Nouveau module pur `ph_session_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : `phSessionGate()` rend **la porte de session ET la bande morte du PI dans le même appel**. C'est le cœur du lot — la régression v2.26.0 (« la session ne se referme jamais ») venait de la divergence entre ces deux décisions écrites à deux endroits ; les rendre indissociables rend le défaut **structurellement impossible** à réintroduire.
- **Signal lent de décision** (`PhDecisionFilter`, moyenne glissante du 1ᵉʳ ordre sur `kPhDecisionWindowMs` = 10 min) : tické **hors branche**, à côté du tick intégral, pour continuer de mûrir pendant les pauses de mélange. Ré-amorçage fail-closed sur mesure non finie (reprise de filtration) et sur `dt` aberrant > 10 s — un trou d'évaluation donnerait α ≈ 1, soit une « moyenne » réduite au dernier échantillon avec le filtre déclaré prêt.
- **Le verrou de session suit la PORTE, plus le débit.** `phDosingState.active` latchait sur `flow > 0` : la session se refermait au premier tour où le PI retombait dans sa bande morte. Sans ce changement, ni le signal lent ni la durée minimale n'auraient d'effet — c'est lui qui entraîne le changement de sens d'`isPhDosing()` ci-dessus.
- **Prolongation bornée par trois garde-fous** : marge de fermeture de porte (0,03 ⇒ la porte ferme à −0,02, l'ouverture reste inchangée) ; **plancher de sous-dépassement −0,05 qui PRIME sur la durée minimale** ; et un **régime de débit** — total plafonné au feedforward seul dès que le signal lent passe sous le seuil d'arrêt, **débit nul** dès qu'il passe sous la consigne. Sans ce dernier point, la dose de base serait servie aveuglément pendant toute la prolongation. **Sous-dépassement pire cas calculé : −0,058 pH → 7,242**, au-dessus du plancher acide 7,20.
- **Déplacement du point d'équilibre : 0,000 pH** — condition d'acceptation du gate `pool-chemistry`. `phStartThreshold` / `phStopThreshold` gardent position **et** rôle (seuils d'hystérésis, bande morte, cible de l'intégrande).
- **Gardes de sécurité inchangées et prioritaires** : plancher, butées brutes, capteur instable, filtration absente, limite horaire, quota journalier, cycles/jour, anti-rafale, OTA, maintenance ferment la session **dans le même tour**, quelle que soit la durée minimale. Elles ne passent jamais par la porte.
- **Comptabilisation du cycle au premier débit réel** (témoin `_phCycleCounted`) : la porte pouvant ouvrir la session plusieurs tours avant le premier débit, l'ancien témoin (vrai un seul tour) aurait laissé `cyclesToday` et l'anneau anti-rafale **aveugles**. L'invariant « un cycle compté ⇔ un cycle réellement démarré » est préservé.
- **Journal** : la ligne `Démarrage dosage pH (auto)` est émise **au premier débit réel** (plus à la décision) et porte désormais **`erreur_lente=`** en plus de `erreur=` — sans elle, elle pouvait afficher une erreur instantanée **sous** le seuil de démarrage et envoyer le diagnostic sur une fausse piste. Le warning « Limite cycles atteinte » devient possible **session ouverte** (effets de bord journaliers extraits dans `tickDosingCycleDay()`).
- **Nouvelles constantes** : `kPhDecisionWindowMs` (10 min), `kPhDoorCloseMarginPh` (0,03), `kPhSessionMinWallMs` (**30 min murales** — au rapport cyclique mesuré de 3,4-5,3 %, 30 min de pompe-ON dureraient ~10 h murales et la borne serait décorative), `kPhSessionUndershootFloorPh` (0,05) ; `kPhStartThresholdDefault` / `kPhStopThreshold` **promues `constexpr`** dans `constants.h` (vérification faite : jamais relues depuis la NVS, jamais exposées par l'API). Six `static_assert` neufs verrouillent la cohérence — dont « bande de décision ≥ 2σ du signal lent » et « plancher de sous-dépassement == seuil défavorable du feedforward ».
- **Garde d'intégrité du tas** : `isPhDosing()` / `isOrpDosing()` **retirés** de la conjonction de `main.cpp` (elle passe de 8 à **6 gardes**). Depuis que ces prédicats suivent la porte, ils seraient restés vrais **jusqu'à 3 h** pompe à l'arrêt et auraient rendu le détecteur de corruption **aveugle des heures d'affilée**. `isAnyPumpEnergized()` couvre exactement ce qu'il faut protéger — la cadence des impulsions **pendant qu'une pompe tourne** — et couvre en plus le test manuel armé depuis la tâche web. **Confirmé par `pool-chemistry`**, avec un `static_assert` neuf verrouillant que le parcours reste **sous 1 % du pouls minimal** (20 ms contre 10 s).
- **Empreinte** : Flash **1 811 321 / 1 835 008 o**, marge **23 687 o**. Tests : **30 suites / 1 154 cas**, **8 mutations tuées sur 8**.
- ⚠️ **Ce que ce lot NE prouve pas encore.** Le rejeu des séries réelles en test natif est en **boucle ouverte** : il démontre l'anti-chatter, **pas** que le bassin reste dosé. Le critère « dose de base servie ≥ 80 % du temps » y ressort à **0,0 %** parce que le signal lent y siège à la consigne (l'acide des sessions réelles est déjà dans la série). **À mesurer sur l'appareil** : enregistrement `/data` sur 4 h une après-midi à la consigne. C'est la réserve principale du lot.

### Documentation

- **Nouvel [ADR-0062](docs/adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)** : « La porte de session pH est dimensionnée par le bruit de mesure, pas par la précision souhaitée » — la caractérisation **τc ≈ 50 s** (processus quasi blanc) qui rend le moyennage viable, le choix L1 + L3, le **refus argumenté de L2** (élargir la bande exigerait 0,17 pH, donc parquer le bassin sous la consigne **sans moyen de remonter** : acide seul, remontée passive ≤ 0,02 pH/h, ~4 h de récupération), le verrou de session qui change de sens avec son alternative crédible (garder le latch sur le débit et renoncer aux deux leviers), le déplacement d'équilibre nul comme condition d'acceptation, et les limites connues (dérive plus lente que la fenêtre, preuve terrain due).
- [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md) : **note d'amendement** — la garde du contrôle d'intégrité du tas ne dépend plus de l'état de session mais de l'**énergisation réelle**. Statut inchangé (**Accepté**).
- `docs/subsystems/pump-controller.md` : section « Porte de session pH robuste au bruit » (mécanisme, valeurs **et leur origine mesurée**, constantes, `static_assert`, trois motifs d'arrêt, verrou de porte et ses implications observables) ; sens d'`isPhDosing()` en tête d'API.
- `docs/subsystems/regulation-vue-d-ensemble.md` : place de la porte dans la chaîne mesure → décision → débit, et la distinction essentielle (le signal **lent** décide de la porte ; P, I et feedforward lisent la filtrée **actuelle**).
- `docs/subsystems/system-health.md` et `docs/subsystems/oom-guard.md` : la conjonction passe à 6 gardes, avec le motif et le nouveau verrou de marge.
- `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/ws-manager.md`, `docs/features/page-ph.md` : **changement de sens** de `ph_dosing` / `ph_dosing_active` — aucun champ, aucun topic, aucune entité ajoutés.
- **Guide utilisateur régulation régénéré** (`guide-regulation-v2.36.pdf`) : ce que l'utilisateur voit change. Le guide d'utilisation est régénéré par le même script mais **son contenu est inchangé** — aucun écran, aucun libellé, aucune capture ne bouge. Les PDF de la version antérieure (v2.34) sont supprimés.

---

## [2.35.0] - 2026-09-05

> ℹ️ **Rien à configurer, aucune migration.** Cette version ne touche aucun réglage, aucune donnée enregistrée et aucun écran. Elle change la façon dont l'appareil répartit sa mémoire interne. **Deux choses à savoir en la regardant vivre** : la ligne **PSRAM** de Paramètres → Système ne restera plus figée à « presque tout libre » — elle **baissera pendant les chargements de l'interface**, et c'est le signe **attendu** que le correctif fonctionne, pas une anomalie ; et une **nouvelle alerte** peut désormais apparaître si le filet anti-plantage de l'appareil n'est plus armé.

### Corrections

- **Moins de redémarrages inopinés attendus — la cause commune des deux plantages est corrigée** (bug-080, lot B-1). Les deux redémarrages sur PANIC de l'appareil (**29/08** et **03/09**) sont survenus **au même moment** : pendant un **chargement de l'interface web**. La raison est enfin identifiée. Chaque fois que l'appareil ouvre un fichier pour vous l'envoyer (page, script, feuille de style), le système lui attache un tampon de travail de **4 096 octets** en mémoire interne — et il ouvre **deux fois** chaque fichier (une fois pour vérifier qu'il existe, une fois pour le lire). Un seul chargement de l'interface, c'est jusqu'à **~49 Ko réclamés d'un coup**, alors que l'appareil n'en a que 42 à 59 de libres. Il tombait en panne sèche de mémoire, et le plantage était **sans recours**.
  La mémoire supplémentaire (**PSRAM**, 4 Mo, jusqu'ici inutilisée à 99 %) aurait dû absorber ces tampons depuis toujours. Elle ne le faisait pas **à un octet près** : la règle du système est « 4 096 octets ou moins ⇒ mémoire interne », et le tampon fait **exactement** 4 096. Le seuil est désormais réglé à **4 095** : ces tampons partent en PSRAM, la mémoire interne respire. Rien d'autre ne change de camp.
  ⚠️ **Honnêteté sur ce correctif** : c'est une hypothèse **vérifiable**, déployée seule pour être jugée seule. Si les redémarrages persistent, elle sera déclarée fausse et le diagnostic repartira des mesures — pas d'un correctif de plus.
- **Le filet anti-plantage de l'appareil était peut-être inerte depuis des mois — il ne peut plus l'être sans le dire.** L'appareil garde en permanence une **réserve de 12 Ko** de mémoire, mise de côté pour absorber une panne sèche au lieu de planter. Or 12 Ko dépasse le seuil ci-dessus : **depuis l'activation de la PSRAM, cette réserve partait probablement en PSRAM** — et une réserve de PSRAM ne sert **à rien** face à une pénurie de mémoire **interne**, tout en affichant « réserve nominale » de façon parfaitement crédible. Ce lot **ne crée pas** ce défaut : il le **révèle**. Désormais la réserve est **explicitement** exigée en mémoire interne, et son emplacement réel est **vérifié puis affiché** au journal à chaque armement (« en RAM interne », ou « PSRAM (ANOMALIE) » s'il dérivait). C'est la ligne du prochain démarrage qui confirmera — ou non — que le défaut existait bel et bien.
- **Nouvelle alerte : « plus aucun filet anti-plantage ».** Si l'appareil n'arrive plus à constituer **aucune** réserve — ni les 12 Ko, ni le repli de 1 Ko — pendant **5 minutes** (soit 30 tentatives), il le signale : ligne **CRITIQUE** au journal et alerte MQTT `oom_reserve_absent` vers Home Assistant. C'est exactement l'état dans lequel l'appareil a volé **des heures** en août sans que rien ne le dise. Une alerte moins grave (**avertissement**) signale aussi le cas où il tourne sur sa réserve de secours de 1 Ko au lieu des 12 Ko nominaux.
- **La PSRAM devient un pool de travail** : la ligne « PSRAM » de Paramètres → Système, et le champ `psram_free` de l'API, **varient désormais avec la charge**. Une baisse pendant un chargement de page est **normale et attendue**. La mise à jour en ligne, elle, continue d'exiger 1 Mo de PSRAM libre — seuil inchangé.

### Firmware

- **Bascule PSRAM** : `heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)` avec `kPsramExtmemThresholdBytes` = **4095**, appelé **une seule fois** au `setup()`, juste après `initOomGuard()` et sous garde `psramFound()`. Le seuil IDF des libs précompilées vaut `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` et son test est **inclusif** (`taille <= seuil ⇒ interne`, désassemblage de `libheap.a`) : à 4096, le tampon stdio de `vfs_api.cpp:22`/`:250` restait interne. La bascule est **fail-safe** — l'allocateur se replie sur `MALLOC_CAP_DEFAULT` si la PSRAM refuse, et n'appelle le hook d'échec qu'**après** ce repli : **aucun échec nouveau n'est créé, les compteurs OOM ne sont pas pollués**.
- **Réserve du parachute OOM** : les **trois** sites d'allocation (nominal 12 288 o, repli 1 024 o, **réarmement**) passent par un entonnoir unique `heap_caps_malloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`. L'unité de compilation `oom_guard.cpp` est protégée par `#pragma GCC poison malloc calloc realloc strdup` — toute réintroduction d'une allocation par défaut **casse la compilation**. `free` en est exclu (la réserve est libérée par `free()`, y compris depuis le `new_handler`).
- **L'invariant est observé, pas supposé** : `oomGuardReserveIsInternalRam()` lit `esp_ptr_internal()` sur le pointeur publié (comparaison d'adresse *inline*, aucun parcours de tas).
- **Nouvelles constantes** : `kPsramExtmemThresholdBytes` (4095), `kVfsStdioBufferBytes` (4096, miroir documentaire de `DEFAULT_FILE_BUFFER_SIZE`), `kOomNoReserveCriticalDelayMs` (5 min). Deux `static_assert` verrouillent `seuil + 1 == tampon` et `kOomReserveBytes > seuil`.
- **Journal — trois formats modifiés ou ajoutés** : le message PSRAM du boot est **réécrit en octets, avec sa direction** (il disait « > 4 Ko », arrondi **faux** qui a masqué le défaut pendant trois cycles) ; les lignes d'**armement** et de **réarmement** portent l'**emplacement** (`RAM interne` / `PSRAM (ANOMALIE)`) ; deux lignes nouvelles — **WARN** « Parachute OOM DÉCLASSÉ » (repli armé, nominal refusé) et **CRITICAL** « Parachute OOM SANS RÉSERVE depuis 5 min ». La trace « Heap 8bits: » (cadence 30 min) porte un champ **`psram=`** : sans lui, la corrélation « chargement d'UI ⇒ chute de PSRAM » ne serait pas observable.
- **`/get-system-info`** : deux champs additifs, `oom_reserve_bytes` (uint32) et `oom_reserve_internal` (bool) — contrat **à quatre états** (le booléen seul est ambigu). Budget JSON ~778 o pires cas (748 avant).
- **Alerte MQTT `oom_reserve_absent`** : `oom_guard` lève un **drapeau**, drainé et publié par `mqtt_manager` depuis **sa** tâche — publier alloue, et le filet de plus bas niveau ne doit rien allouer en famine. MQTT absent ⇒ alerte perdue, le `critical()` du journal reste la source de vérité. **Aucune entité d'auto-discovery HA créée.**
- **Seuils heap inchangés** : l'échelle de l'[ADR-0058](docs/adr/0058-gardes-heap-metrique-8-bits.md) (2 048 < 4 096 < 6 144 < 12 288 < 18 432) n'est **pas** retouchée — un seul levier, un seul effet à mesurer. L'assiette de calibrage ayant changé, elle sera réévaluée après **48 h** d'observation.
- **Empreinte** : Flash **98,7 %** (**1 810 361 / 1 835 008 o**, marge **24 647 o**) sur `esp32dev_core3`. Le levier lui-même coûte ~200 o ; le reste est le prix de l'**observabilité** exigée par le gate `pool-chemistry` (emplacement journalisé, alerte sans réserve, champs API).

### Documentation

- **Nouvel [ADR-0061](docs/adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)** : « Les allocations de plus de 4095 octets partent en PSRAM ; la réserve du parachute reste en RAM interne explicite » — mécanisme du seuil inclusif, caractère fail-safe, invariant de la réserve, règle « appel unique au setup », alternatives écartées (sdkconfig **impossible** car libs précompilées, patch `vfs_api.cpp` **réécrasé** à chaque mise à jour du core, `setvbuf(_IONBF)` **inaccessible** car `FILE*` encapsulé, seuil 0 qui **casserait DMA/ISR**, cache PSRAM des assets **reporté en spec dédiée**, garde 503 seule qui traite le **symptôme**), limites connues (latence PSRAM **non mesurée** sur le service des assets ; rebond interne d'`esp_flash_read` **non quantifié**).
- [ADR-0058](docs/adr/0058-gardes-heap-metrique-8-bits.md) : **note d'amendement** — l'assiette de calibrage change, les seuils ne sont **pas** retouchés dans ce lot. L'ADR reste **Accepté**.
- `docs/subsystems/oom-guard.md` : section « politique d'allocation par défaut », les trois sites d'allocation, le `#pragma GCC poison`, les nouvelles constantes, les trois formats de log, ce qu'il faut lire pendant les 48 h.
- `docs/subsystems/system-health.md` : champ `psram=` de la trace « Heap 8bits: ».
- `docs/API.md` : `oom_reserve_bytes` / `oom_reserve_internal` avec le **contrat à quatre états** et l'avertissement « croiser avec `oom_handler_registered` ne suffit pas » ; changement de **sens** de `psram_free`.
- `docs/MQTT.md` : alerte `oom_reserve_absent` (topic, charge utile, déclencheur, point d'architecture du drapeau).
- `docs/features/page-settings.md` : nouveau sens de la ligne PSRAM de la carte « Infos système ».
- **Guides utilisateur PDF NON régénérés** : aucun écran, aucun libellé et aucun comportement de régulation ne change.

### Tests

- **29 suites natives, 1 090 cas** verts, zéro régression. `test_native_oom_guard` : nouveau cas `test_psram_extmem_threshold_arithmetic_v2350` figeant les **valeurs absolues** (4096 / 4095), la relation `seuil + 1 == tampon`, `kOomReserveBytes > seuil`, `kOomFallbackReserveBytes <= seuil` et `kOomNoReserveCriticalDelayMs = 300 000`.
- **Mutations tuées à la compilation** : `seuil 4095 → 4096` et le retrait de `MALLOC_CAP_INTERNAL` de l'entonnoir. Le test couvre en plus la mutation que les `static_assert` **ne voient pas** — une **translation** des deux constantes (8192 / 8191 compile sans un mot et ramènerait le tampon réel en RAM interne).
- Verdicts : gate `pool-chemistry` **GO sous 5 conditions** ; `code-reviewer` **Request changes** puis correctifs appliqués.

---

## [2.34.0] - 2026-09-04

> ℹ️ **Une seule chose à savoir avant de mettre à jour** : au **tout premier démarrage** de cette version, la « réserve de correction » du pH (le rattrapage accumulé) **repart de zéro, une fois** — le format de sa sauvegarde a changé et l'ancienne n'est volontairement pas réinterprétée. Rien à faire, aucun réglage n'est perdu. Voir [UPDATE_GUIDE.md](docs/UPDATE_GUIDE.md#la-régulation-attend-que-leau-soit-rincée-et-un-redémarrage-ne-coûte-plus-lapprentissage--depuis-v2340-2026-09-04).

### Fonctionnalités

- **Le matin, la régulation attend maintenant que l'eau soit vraiment rincée avant de croire les sondes** (feature-091). Après une nuit d'arrêt, l'eau immobilisée dans les canalisations est chimiquement fausse (pH artificiellement bas, chlore consommé) et met **45 à 50 minutes** à être chassée. Jusqu'ici l'appareil attendait un **délai fixe** que vous régliez vous-même — et il rouvrait les mesures **au moment exact** où ce bouchon d'eau morte arrivait devant les sondes : trois alertes pH à tort le 02/09, les mêmes le 04/09 avec le délai pourtant porté à 15 minutes. Désormais la purge **se termine quand la mesure est stable**, pas quand un chronomètre sonne : au moins 10 minutes, au plus 90, et la durée réelle s'ajuste toute seule à la situation. **Aucune injection, aucune alerte, aucun apprentissage** n'est nourri par ces mesures de rinçage.
- **Le réglage « délai de stabilisation » a disparu** — remplacé par cet automatisme. C'était un réglage impossible à deviner : trop court, il recréait l'incident ; trop long, il retardait la régulation chaque matin. À sa place, Paramètres → Régulation affiche désormais la ligne **« Dernière purge »** avec la durée **mesurée** et le motif de fin : « 12 min — stabilité atteinte », « 90 min — plafond atteint, mesures non stabilisées », « dispensée — coupure brève (≤ 10 s) » ou « 8 min — relancée, la mesure a bougé ». Si vous aviez réglé ce délai, votre valeur est simplement ignorée : rien d'autre à faire.
- **Sur les cartes pH et chlore**, le compte à rebours « Stabilisation : ~N min » devient **« Stabilisation depuis N min »** : il n'y a plus de fin prévisible à décompter. Si la mesure redevient instable après ouverture, le badge affiche **« Re-stabilisation depuis N min »**.
- **Un redémarrage de l'appareil ne fait plus perdre la correction en cours** (feature-093). Le 03/09, un redémarrage de **7 secondes** en pleine filtration a coûté **une heure de régulation** : l'apprentissage du pH avait été jugé « périmé » et remis à zéro. La cause était structurelle — la fraîcheur de cette mémoire se comptait en heures d'**horloge**, alors que l'apprentissage, lui, est **gelé la nuit**. Elle se compte désormais en heures de **filtration** : une nuit d'arrêt ne périme plus rien, et un redémarrage matinal retrouve la valeur de la veille au soir. Les protections sont inchangées : la valeur n'est **jamais** appliquée au démarrage, elle attend que le bassin confirme lui-même un écart pendant 5 minutes, elle reste plafonnée, et la persistance se désarme après 3 redémarrages rapprochés.
- **Un redémarrage bref ne relance plus une purge inutile** : si l'appareil a redémarré **tout seul** (mise à jour, plantage, chien de garde) pendant moins de **10 secondes** alors que la filtration tournait, l'eau de la conduite n'a pas eu le temps de stagner — la purge est **dispensée** et la régulation reprend dès que les sondes sont amorcées (~1 min). Une **coupure de courant** ne dispense jamais : sa durée est inconnue, donc la purge complète s'applique.
- **Si la stabilité n'arrive jamais** (sonde bruyante, bulles, encrassement), l'appareil ne reste pas bloqué en silence : au bout de **90 minutes** il rouvre les mesures — la régulation reprend sous ses gardes de sécurité habituelles — et le signale par un bandeau **« Stabilisation des mesures non atteinte »** sur les pages pH et ORP, une ligne d'alerte au journal et une alerte MQTT `measures_purge_timeout`.

### Firmware

- **Détecteur de stabilité** (`purge_stability_logic`, module pur) : médiane par minute de **12 lectures** des sondes (cadence 5 s), sur les valeurs **brutes** pH et ORP ; fenêtre glissante de **10 médianes** ; mesures déclarées stables si l'étendue de la fenêtre reste ≤ **0,15 pH** et ≤ **15 mV**. Refermeture de la porte si l'étendue redépasse **0,30 pH / 30 mV** (= 2 × le seuil, hystérésis verrouillée à la compilation). Plancher **10 min**, plafond **90 min**.
- **Pourquoi 10 médianes et pas 4** : le rinçage n'est pas une pente mais un **fond en V** (descente −0,047 pH/min, remontée +0,043 pH/min, relevé du 04/09). Le fond occupe une demi-fenêtre de chaque côté, la pente détectable vaut donc `seuil / ((W−1)/2)`. Les rejeux de la courbe réelle ont **invalidé successivement** une fenêtre de 4 min (ouverture à t+7 à pH **5,6**, soit l'incident lui-même) puis de 8 min (ouverture à t+13 à pH 5,71, à **5 millièmes** du seuil, suivie d'une refermeture). Avec 10 : ouverture **sur le plateau** (pH 6,88). Détail et limites : [ADR-0059](docs/adr/0059-purge-adaptative-fenetre-de-medianes.md).
- **La porte couvre désormais TOUS les termes**, pas seulement l'injection : le filtre de mesure ne se déclare plus « prêt » tant que la porte est fermée, et le mode Programmée (qui ne regarde pas les mesures) attend que la circulation soit établie. Correcteur proportionnel, intégral, dose de base apprise et gardes d'alerte ne voient donc **aucune** mesure de rinçage.
- **Dispense de purge après coupure courte** : enregistrement en mémoire RTC (survit à un reset logiciel, pas à une coupure secteur), liste blanche des motifs de reset (logiciel / panique / chiens de garde), heure valide obligatoire, **au plus une dispense par heure**.
- **`stabilization_delay_min`** : retiré de `/get-config` (**7 réglages de régulation** au lieu de 8) et de la page Réglages ; la clé reste **acceptée sans effet** en POST (un client ancien ou l'assistant ne provoquent pas d'erreur) ; le champ NVS `stab_delay` subsiste, **inerte** ; `regulation_defaults.h` ne le porte plus.
- **Nouveaux champs temps réel** (parité `/data` ↔ WebSocket) : `measures_stab_elapsed_s` (remplace `measures_stab_remaining_s`), `measures_purge_last_duration_s`, `measures_purge_last_end` (0 aucune, 1 stabilité, 2 plafond, 3 dispensée, 4 re-purge) et `measures_purge_count`.
- **Sauvegarde de l'intégrale pH** : instantané NVS passé au **schéma 2** (24 octets, nouveau champ « minutes de filtration cumulées à l'écriture », adossé au compteur monotone `pool-daily/filt_total`). Fraîcheur = **≤ 360 min de filtration** depuis l'écriture **ET** ≤ **72 h** d'horloge (filet de sécurité) **ET** compteur cohérent. Nouvelle écriture **par échéance** (180 min de filtration, même à valeur stable) : sans elle, un régime établi n'était jamais rafraîchi et périmait. Coût mesuré : **≤ 6 écritures NVS par jour de filtration**.
- **Correction de revue (majeure)** : tant qu'une valeur restaurée attend sa confirmation par la mesure, **plus aucune écriture** n'est émise. Sans cette garde, la valeur en attente (donc 0 en mémoire vive) écrasait au bout de 30 min la sauvegarde valide — et le redémarrage suivant repartait de zéro, annulant exactement ce que cette version apporte.
- **Empreinte** : Flash **98,6 %** (1 808 697 / 1 835 008 o) sur `esp32dev_core3`, RAM 22,1 %, aucun warning nouveau.

### Documentation

- **Nouvel [ADR-0059](docs/adr/0059-purge-adaptative-fenetre-de-medianes.md)** : « La purge des mesures se termine sur la stabilité, jugée par une fenêtre de 10 médianes : le rinçage est un fond en V, pas une rampe — et le réglage utilisateur disparaît » (fenêtres 4 et 8 min invalidées par rejeu terrain, seuil élargi à 0,20 refusé par `pool-chemistry`, plafond **ouvrant** assumé, limites connues consignées, suite → feature-097).
- **Nouvel [ADR-0060](docs/adr/0060-fraicheur-en-temps-de-filtration.md)** : « La fraîcheur d'une grandeur qui ne vit qu'en filtration se mesure en temps de filtration — et tant qu'une candidate attend sa confirmation, la NVS a raison, pas la RAM » (amende R3 et R5 de l'[ADR-0053](docs/adr/0053-persistance-conditionnelle-integrale-ph.md) ; R1, R2, R4, R6, R7 inchangées).
- `docs/subsystems/measure-validity.md` : détecteur de stabilité, machine d'état, dispense après coupure courte, constantes, logs `[STAB]`, limites assumées.
- `docs/subsystems/pump-controller.md`, `docs/subsystems/sensors.md`, `docs/subsystems/ws-manager.md`, `docs/API.md`, `docs/MQTT.md` : nouveaux champs de purge, alerte `measures_purge_timeout`, `/get-config` à 7 réglages, persistance révisée.
- `docs/features/` (pages Paramètres, pH, ORP, Tableau de bord, Assistant) : ligne « Dernière purge », bandeaux de plafond, badges « Stabilisation depuis N min », retrait du champ de délai.
- `docs/UPDATE_GUIDE.md` : note de migration 2.33 → 2.34 (perte unique de la réserve de correction, disparition du réglage, ce qu'il faut observer le premier matin).
- Guides utilisateur PDF régénérés en v2.34 (`docs/user/`) ; les PDF v2.33 sont supprimés (flotte mono-appareil).

### Tests

- **29 suites natives, 1 089 cas** verts, zéro régression. Nouvelles suites `test_native_purge_stability` (25 cas : rejeux des matins des 02/09 et 04/09, bruités et non bruités, dérives lentes, ORP en pente, démonstration de la fenêtre de 8 min à tort) et extensions de `test_native_measure_validity` / `test_native_ph_persist` (89 cas).
- **Mutations tuées** : fenêtre 10 → 8 (6 tests), plancher plus court que la fenêtre (échec de compilation), garde « pas d'écriture pendant une confirmation en attente » (1 test dédié, exactement 1 échec).
- Revue `code-reviewer` : **Approved** le 04/09/2026 après correction de 3 points majeurs.

---

## [2.33.0] - 2026-09-02

### Corrections

- **Les quatre gardes mémoire voient enfin la vraie mémoire** (bug-080, lot A) : jusqu'ici, l'alerte « heap fragmenté », le saut des pushes WebSocket, la garde des réponses HTTP volumineuses et le redémarrage d'auto-guérison lisaient tous une métrique qui compte de la mémoire **inutilisable** (IRAM 32 bits, ~27 Ko figés) — leur plancher n'était **matériellement jamais franchissable** : le crash du 29/08 et la famine du 02/09 sont passés **sans une seule alerte**. Ces quatre gardes lisent désormais le plus gros bloc **réellement allouable** (8 bits), avec des seuils recalibrés sur les mesures terrain : échelle complète 2 048 (repli parachute) < 4 096 (auto-guérison) < 6 144 (pushes WS) < 12 288 (WARN) < 18 432 (réarmement nominal du parachute) < 21 492 o (nominal terrain).
- **Le parachute anti-crash se réarme à nouveau en régime fragmenté** : son seuil de réarmement nominal (24 576 o) était au-dessus du régime réel de l'appareil — une fois consommé, il ne revenait jamais à pleine réserve. Nouveau seuil 18 432 o (= réserve 12 288 + marge auto-guérison 4 096 + repli 2 048), atteignable en fonctionnement normal.
- **L'auto-guérison peut désormais redémarrer l'appareil en famine réelle** : sous 4 096 o de bloc allouable pendant ~15 s (3 mesures consécutives), redémarrage **propre** — dosage coupé et compteurs journaliers persistés AVANT le restart, plafond de 3 redémarrages par fenêtre de 15 min, puis mode maintenance (dosage suspendu, appareil joignable). Séquence sécurité chimique inchangée (conditions `pool-chemistry`).
- Journal : les traces mémoire disent désormais `maxBloc8=` (métrique 8 bits) au lieu de `maxBloc=` — les relevés restent auto-descriptifs.

### Documentation

- **Nouvel [ADR-0058](docs/adr/0058-gardes-heap-metrique-8-bits.md)** : « Les gardes heap basculent sur la métrique 8 bits : l'immunité vient du temps, pas du seuil » — révocation du NO GO v2.20.2 (les données ont changé : nominal fragmenté stable 21 492 o, creux transitoires de quelques secondes), immunité par débounce temporel (3 mesures, famine continue ≥ 10-15 s), réarmement 16 384 refusé par le gate → 18 432 = 12 288 + 4 096 + 2 048 (verrouillé par test), hors périmètre consigné (`web_routes_debug.cpp`, `web_routes_ota.cpp` — lot B). [ADR-0040](docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md) passe en Superseded.
- `docs/subsystems/oom-guard.md`, `docs/subsystems/system-health.md`, `docs/subsystems/ws-manager.md`, `docs/API.md` (503 `/get-logs`) : échelle 8 bits unique, seuils et libellés `maxBloc8=` à jour.

---

## [2.32.0] - 2026-08-31

> ℹ️ **Appareils existants : RIEN ne change tant que la taille n'est pas touchée.** La clé NVS étant absente, la taille vaut « 20 à 60 m³ » dont les défauts sont **exactement** les réglages actuels (v2.30.0) — égalité **verrouillée à la compilation** (`static_assert`). Visa + contre-visa `pool-chemistry` : **GO**, 31/08/2026 (feature-090).

### Fonctionnalités

- **Choisissez la taille de votre bassin, les bons réglages suivent** (feature-090) : nouveau sélecteur « Taille du bassin » en première position de Paramètres → Régulation (« Moins de 20 m³ » / « 20 à 60 m³ » / « Plus de 60 m³ ») — le seul réglage que l'utilisateur connaît vraiment. Changer de taille présélectionne les 8 réglages de régulation adaptés (limites d'injection 5/10/15 min/h, vitesse de correction, délai de stabilisation ; les consignes pH/ORP ne dépendent pas du volume), après un **récapitulatif avant/après** — annulation sans effet, tout reste modifiable ensuite. La taille est aussi demandée dans l'assistant de premier démarrage (étape « Installation et bassin »), sans dialogue : rien à écraser.
- **La vitesse de correction disparaît de l'écran** : elle dérive désormais de la taille (douce / normale / rapide) et n'est plus visible que dans le récapitulatif de changement de taille. Le réglage reste **intact dans l'API, la NVS et MQTT** (cas experts, rétrocompat) ; face à un ancien firmware, le sélecteur de taille est masqué et la vitesse ré-affichée.
- **La taille ne sert JAMAIS de base de calcul** ([ADR-0057](docs/adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)) : elle présélectionne des réglages, point — le dosage reste piloté par les réglages et l'apprentissage de la consommation réelle (`ph_load7`, feedforward). Principe fondateur : l'appareil doit fonctionner sur n'importe quelle piscine.
- **Colonnes < 20 et > 60 m³ = extrapolations étiquetées** (visées par `pool-chemistry`, logique d'excursion de mélange aveugle ~constante) ; seule la colonne 20-60 m³ est adossée aux mesures du bassin de référence. **Note honnête sur > 60 m³** : le quota journalier d'usine de 300 mL/j (inchangé, c'est une protection) limite en pratique la capacité promise par 15 min/h (≈ 11 min d'injection/jour à 28 mL/min) — le relever manuellement si besoin, l'alerte de sous-dimensionnement ([ADR-0056](docs/adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) guide.

### Firmware

- **`regulation_defaults.h`** : la source unique 088 devient une **table à trois colonnes** (`kRegulationDefaultsBySize`, enum `PoolSize`), avec `static_assert` de bornes chimiques **par colonne**, de **monotonie** (limites et vitesse croissantes avec la taille) et **verrou de migration** (colonne Medium == littéraux v2.30.0 en dur — la retoucher exige un nouveau visa). `applyRegulationDefaults(cfg, size)` et le prédicat de journal `regulationDefaultsRaiseDosing()` sont paramétrés par la colonne cible.
- **`/save-config`** : nouvelle clé `pool_size` (`"small"`/`"medium"`/`"large"`) — si elle diffère de la taille courante, les 8 réglages reçoivent les défauts de la nouvelle taille par le chemin de la restauration 088 (mutex, NVS, retune PID différé), avec **journal avant → après** (niveau `warning` si la capacité de dosage augmente). Un POST combiné applique la taille **puis** les clés individuelles. Valeur inconnue ou inchangée : aucun effet.
- **`/get-config`** : nouveaux champs `pool_size` et `regulation_defaults_by_size` (les trois colonnes — l'UI ne code aucune valeur en dur) ; `regulation_defaults` reflète la colonne de la taille **courante** (contrat 088 intact), et le bouton « Restaurer les valeurs par défaut » restaure donc les défauts de la taille sélectionnée. `regulation_speed` inchangé dans l'API. Pas de nouvelle entité HA.
- **Migration** : clé NVS `pool_size` absente ou illisible ⇒ `medium` — **aucun réglage modifié** par la mise à jour seule.
- **Empreinte** : +2 784 o de firmware, +1 201 o de filesystem.

### Documentation

- `docs/API.md` : `pool_size` (/get-config + /save-config, journal, POST combiné), `regulation_defaults_by_size`, reset 088 dépendant de la taille.
- `docs/features/page-settings.md` : sélecteur de taille, dialogue avant/après, masquage de la vitesse, rétrocompat.
- `docs/features/page-wizard.md` : **nouvelle page** — assistant de premier démarrage (l'étape « Installation et bassin » y est détaillée).
- `docs/subsystems/pump-controller.md` : table par taille, enum `PoolSize`, verrous compile-time, signatures généralisées.
- `docs/UPDATE_GUIDE.md` : note de migration v2.32.0 (sans effet) ; la condition « < 20 m³ » de la v2.30.0 devient « choisir “Moins de 20 m³” » ; note quota journalier sur > 60 m³.
- **Nouvel [ADR-0057](docs/adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)** : « Taille du bassin : présélection de réglages, jamais entrée de calcul » (invariant prouvé par grep + mutation, statut des colonnes, verrou de migration, vitesse dérivée masquée ; alternatives écartées : volume en entrée de dosage, suppression du champ vitesse, entité HA, table dupliquée en JS).

### Tests

- `pio run` SUCCESS 0 warning, **1020/1020 tests natifs** verts (suite `test_native_regulation_defaults` étendue : colonnes Small/Large, monotonie, sérialisation `PoolSize`, migration valeur inconnue ⇒ défauts v2.30.0). Validation par mutation : **6/6 mutants tués** (dont les `static_assert` de monotonie et de verrou de migration, prouvés par échec de compilation).

---

## [2.31.0] - 2026-08-31

### Fonctionnalités

- **Un clic pour revenir aux réglages d'usine de la régulation** (feature-088) : nouveau bouton « Restaurer les valeurs par défaut » dans Paramètres → Régulation, sous Sauvegarder. Il remet les 8 réglages de tuning aux valeurs d'usine de la v2.30.0 (recalibrées sur mesures) : consignes pH (7,3) et ORP (600 mV), limites horaires d'injection pH/ORP (10 min/h), délai de stabilisation (5 min), volumes quotidiens du mode programmé (0) et vitesse de correction (normale). Fini la ressaisie champ par champ avec le risque de se tromper de valeur.
- **Récapitulatif « actuel → usine » avant confirmation** : le dialogue liste les 8 paramètres avec leur valeur actuelle et la valeur d'usine, met **en gras** ceux qui changent réellement, et affiche « Tous les paramètres sont déjà aux valeurs d'usine » avec confirmation **désactivée** si rien ne change. Après confirmation : rechargement complet de la page de réglages + toast de succès. Erreurs gérées : configuration occupée (503 — rien n'a été modifié), session expirée, panne réseau. Le bouton est **masqué** face à un firmware antérieur.
- **Ce que le bouton ne touche JAMAIS** : la calibration des pompes (une pesée physique, la « restaurer » fausserait la comptabilité de volume), le type de produit acide/base (il décrit le bidon réellement branché), les modes de régulation automatique/programmé/manuel (un état choisi, pas un réglage), l'offset pH (une correction de mesure propre à l'installation) et les limites de sécurité journalières (des garde-fous, pas du tuning). Périmètre arbitré par `pool-chemistry` (visa GO SOUS CONDITIONS, 3 conditions livrées).

### Firmware

- **Nouvelle route `POST /reset-regulation-defaults`** (auth CRITICAL, corps vide — aucune valeur ne transite par le client) : applique les défauts, persiste en NVS, retune le PID par le hook différé existant et pousse la config aux autres clients WebSocket. Sur mutex config occupé : **503 `success:false`**, restauration annulée **en bloc** — jamais d'application partielle (correctif de revue : la première implémentation répondait un faux succès sur ce chemin).
- **Source unique `src/regulation_defaults.h`** : les valeurs d'usine de tuning n'existent qu'à un seul endroit, partagé entre les défauts de `MqttConfig`, la restauration et l'objet `regulation_defaults` de `/get-config` — au prochain recalibrage, une seule ligne change et rien ne peut diverger. Chaque défaut est verrouillé par `static_assert` dans sa fenêtre de sécurité (un recalibrage hors fenêtre casse la compilation).
- **Journal avant → après tracé sur l'appareil** : un message compact avec les 8 champs (`x → y`, un champ inchangé se lit `x → x`) — niveau **alerte** (`warning`, mention « dosage AUGMENTÉ ») si la restauration augmente la capacité de dosage (consigne ORP ou limite horaire qui monte), `info` sinon. L'IP d'origine est journalisée.
- **`/get-config`** : nouvel objet `regulation_defaults` (8 clés, lecture seule) — le frontend ne code aucune valeur d'usine en dur.
- **Empreinte** : +2 020 o de firmware, +1 378 o de filesystem.

### Documentation

- `docs/API.md` : route `POST /reset-regulation-defaults` (200/401/503, périmètre, invariants) + objet `regulation_defaults` de `/get-config`.
- `docs/features/page-settings.md` : bouton, dialogue récapitulatif, visibilité conditionnelle, erreurs.
- `docs/subsystems/pump-controller.md` : nouvelle section « Restauration des valeurs d'usine de régulation » (`restoreRegulationDefaults()`, source unique, verrous compile-time, prédicat de niveau de journal).
- `docs/subsystems/ws-manager.md` : aucun champ WS nouveau (broadcast config différé réutilisé) ; écart de parité **préexistant** de la trame `config` consigné dans [bug-089](specs/features/done/bug-089-parite-ws-config-champs-manquants.md).
- Pas d'ADR : mécanisme local sans alternative structurante, la source unique est documentée dans le code et le périmètre tracé dans la spec + le visa `pool-chemistry`.

### Tests

- `pio run` SUCCESS 0 warning, **1007/1007 tests natifs** verts (nouvelle suite `test_native_regulation_defaults`). Validation par mutation : **4/4 mutants tués**, dont un mutant `static_assert` prouvé par échec de compilation ; la liste des champs copiés est prouvée exhaustive ET exclusive (aucun champ hors périmètre ne bouge).

---

## [2.30.0] - 2026-08-31

> ℹ️ **Aucun changement pour les appareils existants.** Les trois valeurs modifiées sont des **défauts d'usine** : la configuration enregistrée en NVS **prime toujours**. Un appareil déjà provisionné conserve ses réglages à l'identique après cette mise à jour — seuls les appareils **neufs ou réinitialisés** (`deploy.sh factory`, première mise en service) démarrent sur les nouvelles valeurs. Visa `pool-chemistry` : **GO**, 31/08/2026 (feature-087) — deux conditions d'application, voir [UPDATE_GUIDE.md](docs/UPDATE_GUIDE.md#nouvelles-valeurs-dusine-de-régulation--depuis-v2300-2026-08-31).

### Firmware

- **Limite horaire d'injection pH** : défaut **5 → 10 min/h** (`phInjectionLimitMinutes`). Le défaut 5 était **prouvé impuissant** sur un bassin médian : ~911 mL/j délivrables pour un besoin estival de ~1 800 mL/j — des semaines de pH bloqué au-dessus de la consigne sans explication visible, qu'aucune amélioration du correcteur ne pouvait compenser (mesuré le 23/08 : 765-802 mL en saturation permanente). Depuis la v2.29.0, l'alerte de sous-dimensionnement ([ADR-0056](docs/adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) guide explicitement un relèvement **au-delà** si le bassin le demande : défaut prudent + guidage explicite plutôt que défaut agressif. ⚠️ Condition du visa : **bassin ≥ ~20 m³** — en dessous, redescendre à 5 min/h (voir guide de mise à jour).
- **Consigne ORP** : défaut **650 → 600 mV** (`orpTarget`). 650 mV est quasi inatteignable sur un bassin extérieur stabilisé (CYA 30-50 mg/L déprime l'ORP) : une consigne inatteignable = **dosage de chlore permanent silencieux**, borné par les seules limites volumétriques — le mécanisme exact de l'épisode de surconsommation de 07/2026. 600 mV est tenable et relevable ; bassin **intérieur non stabilisé** : relever la consigne vers 650.
- **Consigne pH** : défaut **7,2 → 7,3** (`phTarget`). Centre de la plage autorisée [7,20 ; 7,50] : à pH plus bas l'acide surconsomme sans bénéfice net, à pH plus haut c'est le chlore qui surconsomme (fraction HOCl plus faible à ORP égale). Valeur éprouvée sur le bassin de référence.
- **Repli du filet de vraisemblance conservé à 2,17 mL/min** (`kPhLoadSustainableFallbackMlPerMin`) : arbitrage du visa — la valeur était historiquement dérivée de l'ancien défaut (5/60 × 26), elle devient un **plancher prudent délibéré**. Un repli sert quand la configuration est indisponible ou aberrante : il doit être fail-closed (filtre de vraisemblance plus strict, jours écartés, pas de restauration) — l'aligner sur le nouveau défaut (4,67) l'affaiblirait. Seul le commentaire de `constants.h` change ; aucun comportement.

### Documentation

- `docs/UPDATE_GUIDE.md` : note de migration v2.30.0 avec les **deux conditions du visa** (taille minimale de bassin ~20 m³ pour le défaut 10 min/h ; consigne ORP à relever vers 650 sur bassin intérieur non stabilisé).
- `docs/subsystems/pump-controller.md` : la mention « 5 min/h (défaut) » du tableau volume-aveugle est datée (défaut d'usine ≤ v2.29.x), ligne 10 min/h ajoutée.
- Pas d'ADR : valeurs paramétrables sans contrainte structurelle, l'arbitrage vit dans la spec et le visa ; le principe « seuil fonction de la config » est déjà couvert par l'[ADR-0054](docs/adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md).

### Tests

- Aucune suite nouvelle (changement de valeurs d'usine uniquement) : `pio run` SUCCESS 0 warning, **994/994 tests natifs** verts, `static_assert` des plages de consigne vérifiés à la compilation (7,3 ∈ [7,20 ; 7,50]).

---

## [2.29.0] - 2026-08-30

> 🔎 **RECTIFICATIF (30/08, soir — première lecture des nouveaux indicateurs).**
> Le paragraphe ci-dessous, écrit avant le déploiement, raisonne sur la limite
> horaire **d'usine** (5 min/h). Or la config **réelle** de l'appareil vaut
> **25 min/h** — relevée par l'utilisateur entre le 24 et le 30/08 (le 23/08,
> les 765-802 mL délivrés en saturation prouvent la limite encore à 5 ; le
> 30/08, les 1 052 mL délivrés prouvent 25). À 25 min/h le maximum délivrable
> vaut **~8 450 mL/j** : l'installation n'est **plus** limitée par l'actionneur,
> et le pH a atteint 7,298 (consigne 7,30) sous filtration active le 30/08 à
> 17h31. Les analyses des v2.27.0-2.29.0 (ADR-0053, feature-086) restaient
> justes **pour une limite à 5 min/h** — leur erreur est d'avoir lu la valeur
> d'usine de `constants.h` au lieu de la config vivante de l'appareil. C'est
> précisément la visibilité apportée par cette version qui a révélé l'écart.
> Le point 3 de la feature-086 (relever la limite) est donc **sans objet** ;
> l'avertissement sur le volume de mélange aveugle s'applique en revanche dès
> maintenant (~650 mL/h possibles à 25 min/h ≈ 0,23 pH à 40 m³ — sous la marge
> de 0,50, protections 2.27.0/2.28.0 déployées, observation terrain en cours).
>
> ✅ **Vous pouvez enfin voir POURQUOI le pH ne descend pas** — quand c'est le cas. Avec une limite horaire d'injection à **5 min/h** (valeur d'usine), le débit moyen que la pompe peut délivrer plafonne à **~2,17 mL/min**, soit **~911 mL/jour** sur la filtration programmée — sous le besoin du bassin (**~1 800 mL/jour**). Jusqu'ici, cette conséquence n'était calculée nulle part et affichée nulle part. Cette version l'affiche noir sur blanc sur la page pH, calculée sur la **configuration réelle**.
>
> ℹ️ **Ces indicateurs sont en lecture seule** : aucun comportement de dosage ne change dans cette version. Le **relèvement de la limite horaire** (le seul remède réel avec l'allongement de la filtration) était **volontairement différé** après l'observation terrain — jalons : 3 journées exploitables dans l'anneau de charge (~2-3/09), 72 h de feedforward (~5-6/09), et une restauration vérifiée ([feature-086](specs/features/done/feature-086-limite-actionneur-dosage-ph.md), point 3). Cf. rectificatif : ce relèvement avait en réalité déjà été fait par l'utilisateur.

### Firmware

- **La conséquence de la limite horaire est désormais calculée et publiée** ([ADR-0056](docs/adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) : 4 nouveaux champs en **parité stricte `/data` ↔ WebSocket** (même passage de code des deux côtés — le motif du champ qui « clignote » a 5 récidives dans ce projet) : `ph_sustainable_ml_per_min` (débit moyen soutenable, `null` si pompe non caractérisée), `ph_daily_max_ml` (maximum journalier délivrable sur la filtration **programmée**, `null` si planning inexploitable), `ph_limited_by_hourly_budget` (régulation bornée par la limite horaire **en ce moment**), `ph_undersized` (sous-dimensionnement **prouvé sur 3 jours**). MQTT hors périmètre (budget flash).
- **Détection du sous-dimensionnement** : ≥ **3 journées calendaires consécutives exploitables** délivrées à ≥ **80 %** du maximum théorique (le délivré réel plafonne à 84-88 % en saturation permanente — chiffrage de l'incident du 23/08 : un seuil à 90-95 % ne se déclencherait jamais), **pendant que** l'épisode « demande saturée sans atteindre la consigne » reste ouvert. Jours non exploitables et trous calendaires **cassent** la série ; **réarmement implicite** : la série est réévaluée au débit soutenable **courant** — relever la limite fait retomber l'alerte d'elle-même. Nouveau module pur `src/ph_actuator_limit_logic.h`, cache relu hors de la boucle (démarrage + clôture de journée, zéro NVS dans `update()`).
- **Garde mesure-valide sur les deux verdicts** : sur panne de sonde, l'épisode de saturation gèle à sa dernière valeur — sans cette garde (trou trouvé par les tests en cours de cycle, corrigé), l'alerte pouvait s'émettre sur un épisode figé. « Borné par la limite » reste ainsi distinct de « bidon vide » et de « panne de sonde ».
- **Nouveaux messages de journal par transition** : montée de l'alerte (`warning`, avec la série et le débit soutenable) et retombée (`info`).
- **Empreinte** : +2 428 o de firmware, +800 o de filesystem gzippé.

### Fonctionnalités

- **Page pH — bloc « capacités de dosage »** : débit soutenable (`X,XX mL/min`) et maximum journalier (`N mL/j`) dérivés des réglages ; « Indisponible — pompe non caractérisée » plutôt qu'une valeur inventée.
- **Page pH — bandeau informatif « Dosage au maximum autorisé »** (ton neutre, état **normal**) : *« La limite horaire d'injection (X min/h) plafonne l'apport de correcteur à Y mL/min, soit au plus Z mL sur la journée de filtration programmée. Le pH redescend au rythme le plus rapide que ce réglage autorise. »*
- **Page pH — alerte « Capacité de dosage insuffisante »** (diagnostic, distincte du bandeau qu'elle **remplace** — jamais d'empilement) : signale le sous-dimensionnement prouvé sur 3 jours, conseille **la filtration d'abord**, le relèvement de limite seulement « après vérification de l'installation ». **Aucune mention de bidon** (leçon v2.26.0). Firmware antérieur : tout reste masqué, rien d'inventé.

### Documentation

- **[ADR-0056](docs/adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) créé** — *Sous-dimensionnement d'actionneur : 3 jours × 80 %, anneau évalué au soutenable courant*. Consigne la marge contre-intuitive (< 90 % à cause des pauses de mélange, chiffrage incident 23/08), le réarmement implicite (invisible dans le code) et les alternatives écartées (stocker le soutenable du jour dans l'anneau ; compteur persisté de jours en anomalie).
- `docs/subsystems/pump-controller.md` : nouvelle section « Limite d'actionneur pH » — accesseurs, critère de détection, réarmement implicite, garde mesure-valide, cache, `scheduledMinutesToday()`, constantes, journal.
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/features/page-ph.md` : les 4 champs (budgets de payload : +119 o des deux côtés, gabarit WS 2688 → 2816), les trois éléments UI et leurs règles.
- `specs/features/doing/feature-086-*.md` : livrables des points 1-2 cochés, notes techniques. **La spec reste dans `doing/`** : le point 3 (relèvement de la limite) est verrouillé jusqu'aux jalons d'observation terrain.

### Tests

- **970 → 994 tests natifs**, nouvelle suite `test/test_native_ph_actuator_limit/`. Validation par mutation exigée par le workflow : **5 mutations tuées sur 5**.

---

## [2.28.1] - 2026-08-30

> ✅ **Les repères des graphiques d'historique pH et ORP disent enfin la vérité.** La bande pH affichait 7,0-7,4, une plage qui ne correspondait ni à la plage idéale de l'eau ni aux réglages de l'appareil. La bande ORP 600-800 mV affirmait une norme qui n'existe pas (le potentiel utile dépend du pH, du stabilisant et de la température) et annonçait « hors plage » en permanence une installation correctement réglée à 550 mV.

### Fonctionnalités

- **Graphique pH (page pH)** : la bande ombrée reflète désormais la **plage idéale de l'eau 7,2-7,6** (littérature, optimum 7,4), et une **ligne de consigne pointillée** trace la valeur configurée (`ph_target`). Trois plages distinctes, et c'est voulu : bande du graphique **7,2-7,6** / plage de consigne réglable **7,20-7,50** / seuils de garde **6,70-7,60** — elles ne doivent pas être unifiées (la consigne laisse une marge avant l'inhibition du chlore à 7,60).
- **Graphique ORP (page ORP)** : la **bande 600-800 mV trompeuse disparaît**, remplacée par la **ligne de consigne** à `orp_target`.
- Les lignes de consigne se mettent à jour **sans rechargement de page**, quel que soit le canal du changement (UI, API, MQTT). Firmware antérieur ne servant pas la consigne : **repère absent — jamais de valeur inventée** ; une trame partielle ne fait jamais disparaître la ligne. Empreinte : +258 o gzippé sur le filesystem, aucun changement firmware hors numéro de version.

### Documentation

- `docs/features/page-ph.md` : nouvelle section « Repères du graphique d'historique » — bande 7,2-7,6, ligne de consigne, table des trois plages (arbitrage du 30/08 consigné dans la spec), règles de mise à jour et de repli.
- `docs/features/page-orp.md` : nouvelle section « Repère du graphique d'historique » — suppression motivée de la bande, ligne de consigne.
- Pas d'ADR : arbitrage Option A/B consigné dans la spec, portée locale à l'affichage, la bande pH reste une constante d'affichage commentée avec sa référence dans `data/app.js` (Option B — bornes servies par le firmware reportables dans une spec future).

---

## [2.28.0] - 2026-08-30

> ✅ **La charge quotidienne connue est désormais couverte dès la première heure de filtration, au lieu de J+2,4.** Le bassin consomme une dose d'acide très régulière (~80 % vient du dégazage de CO₂, stable d'un jour à l'autre), et l'appareil la connaît déjà : elle est dans son historique de consommation depuis la v2.27.0. Jusqu'ici la régulation la redécouvrait chaque fois par tâtonnement — et le moindre à-coup (pluie, appoint) effaçait l'apprentissage. Cette version injecte d'office la charge connue pendant les sessions automatiques (« **dose de base apprise** », feedforward), la régulation ne traitant plus que l'écart résiduel. Elle est **apprise, jamais saisie** : aucun volume de bassin, TAC ou concentration à renseigner.
>
> ⚠️ **Ce que cette version NE change PAS : la limitation par la pompe** (voir l'avertissement de la v2.27.0). Avec 5 min/h de limite d'injection, le maximum délivrable reste ~911 mL/j pour un besoin de ~1 800 mL/j — dose de base ou pas, la consigne peut rester hors d'atteinte. Le sujet reste suivi dans [feature-086](specs/features/done/feature-086-limite-actionneur-dosage-ph.md).
>
> ⚠️ **Résultat non vérifié sur le terrain** : validé en natif (970 tests) et par le gate chimie (GO amont sous 4 conditions + contre-visite GO, risque résiduel borné ~0,01-0,02 pH), mais pas encore observé sur le bassin. À surveiller : quota journalier sur 72 h, annulation en < 30 min sur une vraie perturbation acide, redémarrage en pleine séance de dosage.

### Firmware

- **Nouvelle dose de base apprise (feedforward) sur la régulation pH automatique** ([ADR-0055](docs/adr/0055-feedforward-appris-additif-plafonne.md)) : la **médiane 7 jours** de l'anneau de charge `ph_load7` (v2.27.0) entre comme **terme additif** dans le calcul de débit — `débit = FF(charge apprise) + kp·e + ki·∫(e − seuil)`. **Source réutilisée telle quelle** via `phIntegralObservedLoad()` : aucune seconde médiane, aucun second anneau — la restauration R1 et le FF lisent la même grandeur et ne peuvent pas diverger. **Fail-closed sous 3 jours** d'historique exploitable : un appareil neuf ne dose rien « de mémoire ».
- **UN SEUL chemin de commande vers la pompe** (condition bloquante n°1 du gate `pool-chemistry`) : le FF passe par le chemin de dosage existant (PI borné à `ampFlow − ff`, somme clampée, ordonnanceur d'impulsions, comptabilité) — **une seule comptabilité de volume, un seul jeu de gardes**. L'alternative « seconde branche type programmée » est rejetée : deux branches qui dosent en parallèle, c'est un double dosage à la première divergence.
- **Plafond DOUBLE** (condition n°2) : plafond d'**actionneur** — le FF ne dépasse jamais **60 % du débit soutenable**, le PI garde 40 % d'autorité pour l'écart résiduel — **ET** gel de **quota** — FF = 0 dès que 60 % de la limite journalière est consommée. Le second n'est pas redondant : contre-exemple du gate, quota 300 mL/j sur 12 h de filtration ⇒ 0,42 mL/min suffisent à vider 100 % du quota quand l'actionneur en autorise 1,30 — sans ce gel, le FF viderait **seul** la limite journalière. `static_assert` : le gel du FF (0,60) mord **avant** le gel d'anti-windup de l'intégrale (0,80) — l'autorité aveugle à l'erreur cède toujours la première.
- **Latch de sécurité fail-closed** (condition n°3) — le FF est la seule source de dosage **aveugle à l'erreur mesurée** ; sans mesure digne de confiance, personne ne peut constater qu'il dose à tort : erreur **signée** < −0,05 tenue **30 min** de temps de branche (le scénario « fausse pluie ») ; mesure invalide (**immédiat, sans tenue**) ; changement de réglages de régulation (produit / consigne / vitesse / plafond, même empreinte de contexte que la persistance). Réarmement au front d'ouverture de la session automatique suivante. **Tenue produit** : après un changement pH- ↔ pH+, le latch tient jusqu'à une journée **entière** archivée sous le nouveau régime — RAM-only, fail-open **borné et chiffré** par le gate : ~39 mL au pire, soit ~0,01-0,02 pH sur 30-50 m³.
- **Anti-windup recalculé sur (demande − FF)** (condition n°4) : le soutenable vu par le tick intégral devient `max(0, soutenable − ff)` — sinon l'intégrale se gèlerait sur une saturation **causée par le FF** et le PI deviendrait sourd. D'où l'ordre impératif dans `update()` : `tickPhFeedforward()` **avant** `tickPhIntegralAuto()`, dans le même tour.
- **Stabilité structurelle, par l'hystérésis de session** — ni boucle sur le FF, ni hystérésis interne : le FF ne vit que sous une session automatique ouverte ; s'il sur-corrige, l'erreur retombe sous le seuil d'arrêt, la session se ferme et le FF cesse **avec elle**. L'argument de fond est le **bilan de matière** : le volume journalier livré converge vers la charge réelle du bassin quel que soit le FF — la médiane apprise est **non expansive**, elle ne peut pas s'auto-amplifier d'un jour sur l'autre. Le plafond d'actionneur n'est qu'une ceinture, pas le mécanisme.
- **Aucun état volumique** : le FF est un **débit**, pas un accumulateur — pas de « dette » à rattraper, donc **pas de rafale possible après un redémarrage** en pleine séance. **Sonde figée = FF gelé** : structurellement (pas de session sans mesure valide) et par le latch « mesure invalide » explicite.
- **Nouveau module de logique pure `src/ph_feedforward_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)), coquille `tickPhFeedforward()` dans `pump_controller.cpp`. Nouveaux accesseurs **lecture seule** `getPhFeedforwardFlow()` / `getPhFeedforwardStatus()`, code `PhFfStatus` **figé 0-4** (0 inactif · 1 actif · 2 latch · 3 historique < 3 j · 4 quota), jamais renuméroté (même règle que `DoseRefusal`, [ADR-0049](docs/adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md)). Nouvelles constantes `kPhFf*` dans `constants.h`.
- **Observabilité en parité stricte `/data` ↔ WebSocket ↔ MQTT** (le motif de parité a 5 récidives dans ce projet ; la spec l'exigeait explicitement) : nouveaux champs `ph_ff_ml_per_min` (mL/min, 2 décimales, 0 hors session) et `ph_ff_status` — les états 2-4 sont servis **même hors session** : ils disent *pourquoi* le feedforward ne s'appliquera pas.
- **MQTT** : nouveaux topics retain `{base}/ph_ff_ml_per_min` et `{base}/ph_ff_status` (code brut, le mapping appartient aux clients), deux entités HA auto-découvertes en `entity_category: diagnostic` (52 → 54 publications de discovery), deux slots de déduplication (`kDedupCacheSlots` 21 → 23).
- **Nouveaux messages de journal** : annonce de la dose de base au premier tick actif de chaque session (avec charge observée, profondeur d'historique et plafond), pose du latch **motivée** (par transition, jamais par tour), tenue produit, réarmement à l'ouverture de session.
- **Empreinte** : +3 884 o de firmware, +273 o de filesystem.

### Fonctionnalités

- **Page pH — nouvelle ligne « Dose de base apprise »** dans le bloc Rattrapage accumulé (mode Automatique uniquement) : débit courant (`X,XX mL/min`, affiché **uniquement** à l'état actif — partout ailleurs il vaut 0 par construction et un « 0,00 » se lirait comme une mesure) + libellé d'état (« active », « suspendue (pH sous consigne) », « en apprentissage (historique < 3 j) », « suspendue (60 % du quota atteint) »). Ligne **masquée** sur firmware antérieur (clé absente) ; code inconnu (firmware plus récent) → tiret, jamais un libellé voisin.

### Documentation

- **[ADR-0055](docs/adr/0055-feedforward-appris-additif-plafonne.md) créé** — *Feedforward appris additif plafonné*. Consigne les cinq alternatives écartées (dont la seconde branche de dosage, la régulation du FF lui-même, le plafond d'actionneur **seul** — condition affaiblie, contre-exemple chiffré — et le dosage volumétrique calculé, qui exigerait le volume du bassin, **interdit par l'utilisateur**) et ce que ça verrouille : un seul chemin de commande pour tout futur contributeur, contrat `PhFfStatus` figé, `ph_load7` source unique de la charge observée, pas d'hystérésis interne au FF.
- `docs/subsystems/pump-controller.md` : nouvelle section « Dose de base apprise — feedforward pH (v2.28.0) » — principe, source réutilisée, plafond double, latch et réarmement, tenue produit, stabilité par bilan de matière, constantes, accesseurs, journal, testabilité.
- `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/ws-manager.md`, `docs/features/page-ph.md` : les deux nouveaux champs (avec budgets de payload — +43 o sur `/data`, +44 o sur la trame WS, aucun bump de gabarit), les deux topics + entités HA, la ligne UI et ses règles de rétrocompat.
- `specs/features/doing/feature-085-*.md` : critères prouvés en natif cochés, notes techniques, suivi d'itérations. **La spec reste dans `doing/`** : l'observation terrain (quota sur 72 h, fausse pluie réelle, confirmation du comportement sonde figée / redémarrage en séance) reste ouverte.

### Tests

- **932 → 970 tests natifs**, nouvelle suite `test/test_native_ph_feedforward/`. Validation par mutation exigée par le workflow : **6 mutations tuées sur 6**.

---

## [2.27.0] - 2026-08-30

> ⚠️ **À lire avant tout le reste : votre installation est limitée par la POMPE, pas par la régulation.** Avec la limite horaire d'injection réglée à **5 min/h** et une pompe qui débite ~26 mL/min, le maximum que le système peut **physiquement** délivrer vaut **~2,17 mL/min**, soit **~911 mL par jour** sur 7 h de filtration. Le besoin du bassin est d'environ **1 800 mL/jour**. **Aucune amélioration de la régulation ne peut combler cet écart** — ni cette version, ni la suivante.
>
> **Conséquence directe : l'alerte « régulation saturée » est VRAIE et légitime.** Elle ne signale ni un bidon vide, ni une sonde en panne : elle signale que **la limite horaire d'injection est atteinte**. Ce n'est pas un faux positif à faire taire.
>
> ⚠️ **Le seul levier est de relever `phInjectionLimitMinutes` — et il n'est pas neutre.** Ce réglage augmente **proportionnellement** le volume injecté pendant les **15 minutes de retard de mélange**, pendant lesquelles la régulation dose sans voir encore l'effet des doses précédentes : **130 mL à 5 min/h** (soit **0,046 pH** d'excursion) mais **780 mL à 30 min/h** (soit **0,28 pH**), à comparer à la marge de sécurité de **0,50 pH**. **Ne pas relever ce réglage sans en tenir compte.**
>
> ✅ **Ce que cette version apporte : de la ROBUSTESSE, pas de la vitesse.** L'apprentissage de la régulation pH survit désormais à un redémarrage et aux à-coups. Un redémarrage coûtait **2,4 jours** de reconvergence ; il coûte maintenant **environ une heure**.
>
> ⚠️ **Résultat non vérifié sur le terrain** : cette version n'a pas encore tourné sur le bassin. Le problème de fond est suivi dans [feature-086](specs/features/done/feature-086-limite-actionneur-dosage-ph.md) (limite d'actionneur) et [feature-085](specs/features/done/feature-085-dose-de-base-apprise-feedforward.md) (dose de base apprise).

### Firmware

- **L'apprentissage de la régulation pH survit à un redémarrage** — sous conditions, et **avec confirmation par la mesure**. L'[ADR-0052](docs/adr/0052-terme-integral-ph-dimensionne-par-autorite.md) l'**interdisait** explicitement (« une valeur stockée serait une **autorisation de dosage pré-chargée** au démarrage ») ; le gate `pool-chemistry` du 30/08 conclut **GO** et l'ADR est **amendé** par l'[ADR-0053](docs/adr/0053-persistance-conditionnelle-integrale-ph.md). **Pourquoi l'objection tombe** : la valeur relue **n'est jamais appliquée au démarrage**. Elle attend que le bassin démontre lui-même, pendant **5 minutes** de régulation effective, un écart au-dessus du seuil de démarrage — état dans lequel la régulation aurait de toute façon ouvert une injection sur le seul terme proportionnel. **Ce qui est restauré n'est donc pas une autorisation de doser : c'est la vitesse d'une correction déjà décidée par la mesure courante.**
- **Sept garanties cumulatives (R1-R7), aucune facultative** : plafond de restauration `min(30 % du plafond ; 50 % de l'équilibre **observé sur ce bassin**)` ; contrôle d'intégrité (taille exacte + empreinte + numéro de schéma, **aucune réinterprétation partielle**) et empreinte de **contexte** (vitesse, produit, consigne, plafond) ; **péremption 6 h** réévaluée en continu ; **confirmation par la mesure** ; écriture **conjonctive** (≤ 1 / 30 min **ET** > 5 % du plafond, ~2 écritures/jour) ; journalisation de chaque rejet motivé ; **anti-boucle de démarrage** (3 restaurations sans écriture normale ⇒ désarmement). Retirer une seule garantie rétablit l'objection de l'ADR-0052.
- **Le forfait « 30 % du plafond » NE SUFFIT PAS seul, et c'est un problème de flotte** : écrit en fraction, l'équilibre vaut `(charge − 0,08) / 6`, égal à 30 % à **1,88 mL/min**. Tout bassin **moins chargé** que celui de référence — plus petit, bâché, TAC bas, acide plus concentré, hors saison — verrait la butée forfaitaire passer **au-dessus de son propre équilibre** (à 1,5 mL/min : 1,27 ×). D'où le second terme, calculé **à l'exécution** sur la charge réellement observée de **ce** bassin. ⚠️ **Ce terme est LE PORTEUR de l'argument de sûreté** : l'intégrité de la charge observée — fraîcheur de l'historique, plausibilité du dénominateur — est une **propriété de SÉCURITÉ**, pas un confort de diagnostic.
- **Nouvel anneau NVS de charge acide sur 7 jours** (`pool-daily` / `ph_load7`, 64 octets, CRC FNV-1a, **une écriture par jour**) : la charge observée **n'existait nulle part** avant, et le fail-closed rendait donc la restauration inerte. Alimenté aux **deux** points de basculement de date existants, avec déduplication sur toute l'étendue de l'anneau (un recul d'horloge à cheval sur minuit aurait sinon **pondéré double** une même journée). **Médiane** et non moyenne : elle encaisse jusqu'à trois journées aberrantes sur sept.
- **Nouveau compteur de minutes de circulation** (`SafetyLimits::dailyFiltrationMinutes`) : **la charge se mesure en mL/min, pas en mL/jour** — un jour à 4 h et un jour à 10 h de filtration qui injectent le même volume ne décrivent pas la même charge.
- **Trois défauts fail-open trouvés en revue et corrigés** (tous surévaluaient la charge, donc le plafond de restauration) : **(1) fenêtre d'âge absente** — l'anneau garde les 7 dernières journées *archivées*, pas les 7 derniers *jours* ; un appareil arrêté deux mois restaurait sur une médiane d'été, soit **1,5 × l'équilibre courant** ; **(2) dénominateur perdu** sur tous les chemins sans flush — le flush 30 s vivait derrière quatre `return` (OTA, maintenance, capteurs non initialisés, stabilisation) alors que l'eau circule pendant ces épisodes et qu'OTA et maintenance finissent par un **redémarrage** : une charge observée **quatre fois trop grande** ; **(3) purge sèche n'abandonnant pas la candidate** — pluie acide → purge → le pH remonte → la candidate se confirme et **recharge l'autorité que la purge venait d'invalider**.
- **L'intégrale accumule désormais pendant les pauses de mélange** : le tick d'entretien sort de la conjonction de dosage (précédent : la surveillance des gardes pH en v2.25.0). Rapport cyclique **25 % → 29 %**, convergence **2,4 j → 2,05 j**. La **sortie reste gatée exactement comme avant** — entretenir l'intégrale n'autorise aucune injection. L'intégrale **n'accumule toujours pas filtration coupée** (mesure non représentative en eau stagnante), ni sonde figée/périmée, ni en mode manuel, ni sous garde de sécurité.
- **Le reset de l'apprentissage sur excursion acide est TEMPORISÉ à 15 min** au lieu d'être immédiat (le seuil, −0,15, est inchangé). **Une pluie légèrement acide effaçait deux jours et demi d'apprentissage.** Ce différé **ne suspend aucune protection** : pendant l'attente, la décharge signée et l'oubli exponentiel travaillent tous les deux (**−16 % d'intégrale sur les 15 minutes**) ; seule la purge *instantanée* est différée, le temps de vérifier que l'excursion est réelle. 15 min = la durée du retard de mélange, en deçà de laquelle l'excursion mesurée peut n'être que la dose précédente pas encore homogénéisée.
- **Nouveau gel d'anti-windup sur l'anti-cycling épuisé**, par symétrie avec le quota volumique : l'anti-cycling ne se relève qu'au basculement de 24 h. Épuisé à 10 h du matin, il laissait l'intégrale charger **neuf heures** sur une erreur que la pompe n'a plus le droit de résorber (~1 300 pH·s servis d'un bloc le lendemain).
- **Paliers d'alerte de saturation recalibrés** : `warning` **2 h → 2 h 20**, `critical` **6 h → 7 h**, rappel **12 h → 14 h** (comptés en temps de régulation effective). **La fenêtre de preuve en temps réel est INCHANGÉE** (~8 h / ~24 h / ~48 h) : c'est le *tarif* de la base de temps qui a changé, pas l'intention. ⚠️ Une première rédaction avait retenu un facteur ×4 issu d'une analyse d'impact préliminaire : le résultat aurait été un bidon vide détecté **3,45 × plus lentement**, l'inverse exact du but. Trois `static_assert` verrouillent désormais le rapport.
- **Le seuil de vraisemblance de l'anneau suit la CONFIGURATION** ([ADR-0054](docs/adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md)) : il vaut `1,10 × débit soutenable`, calculé sur la limite horaire **configurée** et le débit d'amplitude **calibré**. Figé à 2,17 mL/min comme dans la première version, il aurait écarté **toutes** les journées dès que l'utilisateur relève la limite horaire — c'est-à-dire dès qu'il applique la recommandation ci-dessus — et la restauration ne se serait plus jamais armée, **en silence**. `kPhLoadSustainableMlPerMin` devient `kPhLoadSustainableFallbackMlPerMin` : une **valeur de repli**, jamais un seuil nominal.
- ⚠️ **Usure NVS assumée, facteur 5 à 6** : `_dailyCountersDirty` est désormais posé **une fois par minute de circulation**, soit **~420-600 transactions `pool-daily` par jour contre ~70-100** auparavant. Le choix est **délibéré** — numérateur (mL) et dénominateur (minutes) sont persistés **ensemble**, donc une coupure ne perd au plus qu'une minute de chacun et **le rapport mL/min est préservé**. L'ordre de grandeur reste très en deçà des 100 000 cycles garantis d'un secteur.
- **Compteurs journaliers posés en NVS à l'entrée en maintenance** et **avant les redémarrages OTA, mode Point d'accès, web et MQTT** : seul `performFailSafeRestart()` le faisait.
- **Nouvelle fonction publique `phSustainableFlowFromConfig()`** ([`config.h`](src/config.h)) : **source unique** du débit moyen soutenable de la voie pH, partagée par le filet de vraisemblance de l'anneau et le gel d'anti-windup — les deux ne peuvent plus diverger. `setMaintenanceInhibit()` n'est plus `inline`. Nouvelles signatures `readPhLoadObservation()`, `recordPhLoadDay()`, `loadPhLoadHistory()`, `savePhLoadHistory()`, `loadPhIntegralSnapshot()`, `savePhIntegralSnapshot()`.
- **Nouveau module de logique pure `src/ph_persist_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) — **motif** : R1-R7 vivaient dans `config.cpp` et `pump_controller.cpp`, **ni l'un ni l'autre n'étant compilé en natif**. Trois mutations survivaient (supprimer l'écrêtage du plafond, supprimer la vérification de CRC, rendre le prédicat permissif). Situation identique à celle qui avait précédé l'extraction de `ph_integral_logic.h`, où le défaut réel se trouvait dans la garde de C2.
- **Correction d'un commentaire faux dans `constants.h`** : les deux rapports cycliques y étaient qualifiés de « **MESURES**, pas des estimations » — c'est faux (0,29 vaut 7/24 exactement, soit le calcul de la spec **privé de son facteur 0,85**), et c'était écrit dans le paragraphe même qui **interdit de recalibrer sur une estimation**. Ce sont des **estimations analytiques**, dont le sens d'erreur est **conservateur** : elles avancent les alertes, elles ne produisent jamais de silence.
- **Nouveaux messages de journal** : retenue d'un instantané, restauration confirmée (avec charge observée, profondeur d'historique et **origine du plafond**), rejets motivés, abandons (péremption / plafond retombé / heure non synchronisée / mode manuel), archivage quotidien d'une journée, **anneau NVS illisible**. ⚠️ **Format changé** : la ligne de purge sèche devient `Intégrale pH purgée (erreur … — pH franchement au-delà de la consigne depuis plus de 15 min) : … pH·s abandonnés` — un filtre de journal sur l'ancienne phrase exacte ne matche plus.

### Documentation

- **[ADR-0053](docs/adr/0053-persistance-conditionnelle-integrale-ph.md) créé** — *Persistance conditionnelle de l'intégrale pH : ce qui est restauré n'est pas une autorisation de doser, c'est la vitesse d'une correction déjà décidée par la mesure*. **Amende la clause « aucune persistance NVS » de l'[ADR-0052](docs/adr/0052-terme-integral-ph-dimensionne-par-autorite.md)** (dont le statut est annoté en conséquence ; **tout le reste est inchangé** : autorité 6 mL/min, `integralMax` dérivé, intégration signée, gel asymétrique, `Kd = 0`, `Ki_ORP = 0`). Consigne la décomposition de l'objection en deux volets, les sept garanties, **les trois défauts fail-open trouvés en revue** — *un ADR qui tait ses quasi-accidents ne prévient pas leur reprise* — les neuf alternatives écartées, et ce que ça verrouille (dont : l'intégrité de la charge observée est une **propriété de sécurité**, aucune sortie anticipée d'`update()` ne doit repasser devant le flush des compteurs).
- **[ADR-0054](docs/adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) créé** — *Un seuil de vraisemblance dérivé d'un paramètre configurable est une FONCTION de ce paramètre, jamais une constante*. Consigne le piège silencieux évité, la sentinelle qui **verrouille les deux sens** (même journée à 6 mL/min : écartée à 5 min/h, retenue à 15 min/h), les cinq alternatives écartées, et la **règle générale** applicable à tout seuil futur.
- `docs/subsystems/pump-controller.md` : nouvelle section « Cadence, mémoire et charge observée de l'intégrale pH (v2.27.0) » — **le point de fond sur la limitation par l'actionneur en tête**, prédicat d'entretien état par état, reset sec temporisé, les sept garanties R1-R7, points d'abandon d'une candidate, anneau de charge et critère de jour exploitable, dénominateur et **usure NVS chiffrée**, points de flush ajoutés, recalibration des paliers, nouvelles signatures, nouvelles constantes, journal, testabilité. Tables « API publique », « Paramètres en dur », « État persistant (NVS) », « Garde-fous », « Boucle d'exécution » et « Fichiers liés » mises à jour.
- `docs/UPDATE_GUIDE.md` : note de migration v2.27.0 — aucune action requise, avertissement sur `phInjectionLimitMinutes`, ce que signifie réellement l'alerte « régulation saturée ».
- `specs/features/doing/feature-084-*.md` : suivi d'itérations, visa `pool-chemistry` coché, critères d'acceptation. **La spec reste dans `doing/`** : le déploiement OTA n'a pas eu lieu et le critère « temps de convergence mesuré sur données réelles » reste ouvert.

### Tests

- **849 → 932 tests** natifs, nouvelle suite `test/test_native_ph_persist/`. Les **trois mutations** qui survivaient (écrêtage du plafond, vérification de CRC, prédicat permissif) **meurent désormais**. La sentinelle du filet de vraisemblance verrouille les **deux** sens : un seuil figé — à 2,17 comme à 6,5 mL/min — casse l'une des deux lignes.

---

## [2.26.0] - 2026-08-28

> ⚠️ **La régulation pH devrait enfin atteindre sa consigne.** Jusqu'ici elle se stabilisait systématiquement **au-dessus** : 7,50-7,66 mesurés pour une consigne à 7,30, malgré ~1 000 mL d'acide injectés dans la journée. Ce n'était **ni une panne, ni un problème de dosage, ni un produit trop dilué** : c'est une propriété mathématique du correcteur utilisé.
>
> Un correcteur **proportionnel** ne délivre du produit que **proportionnellement à l'écart** au but. Face à une dérive **permanente** (le CO₂ qui s'échappe de l'eau fait remonter le pH en continu, ~80 % de la charge ; le chlore, basique, apporte le reste), il s'arrête donc là où la dose qu'il produit compense exactement la dérive — **à un écart non nul de la consigne**. Vérification : avec les réglages en place, cet écart d'équilibre vaut 0,358 pour 10,5 h de filtration par jour. **Le système était exactement à son équilibre théorique.**
>
> La correction ajoute un second terme, qui **accumule le retard** au lieu de ne regarder que l'écart instantané : la dose ne dépend plus seulement de « à combien suis-je du but », mais aussi de « depuis combien de temps je n'y suis pas ». C'est ce terme qui, une fois la consigne atteinte, fournit **toute** la dose d'entretien. Point d'équilibre attendu : **consigne + 0,01** — la valeur exacte à laquelle la régulation coupe de toute façon.
>
> ⚠️ **Résultat non vérifié sur le terrain.** La correction est fondée sur la théorie du réglage, vérifiée en boucle fermée simulée et par bilan de matière, mais **elle n'a pas encore tourné une saison sur le bassin**. Surveiller le pH et le compteur journalier pendant quelques jours. Si le pH ne descend toujours pas, c'est l'estimation de la charge (~1 800 mL/jour) qu'il faudra revoir, pas le correcteur.
>
> ✅ **Aucun réglage à changer.** Les limites journalière et horaire, les seuils de sécurité et les modes sont inchangés.

### Firmware

- **Régulation pH : de P pur à PI (`Ki` levé)** — gate `pool-chemistry` **GO SOUS CONDITIONS C1-C5**. Le `Ki = 0` posé comme **non négociable** par la feature-025 est levé sur la **seule** boucle pH. `Kd = 0` reste **impératif sur les deux chimies** et **`Ki_ORP` reste à 0** (hors périmètre : l'ORP dépend du pH, de la T° et du stabilisant, et son écart résiduel n'est pas le défaut diagnostiqué).
- **Pourquoi c'était juste en feature-025 et pourquoi ça ne l'est plus** : à l'époque, la régulation venait de basculer sur une mesure filtrée, la pause mélange n'avait aucun recul terrain et le risque d'emballement de l'intégrale était réel et non maîtrisé. L'ADR-0016 avait explicitement prévu la sortie (« infrastructure conservée pour une réactivation progressive après validation terrain »). Depuis : l'**anti-windup est complet et testé à 100 %** (bande morte ⇒ sortie nulle **et** intégrale non modifiée, clamp symétrique) et **six barrières de sécurité** ont été ajoutées (seuils pH fixes avec `static_assert`, deux butées de dernier recours sur le pH brut à 6,20 / 8,10, hystérésis par seuil, surveillance hors conjonction, escalade fail-closed à 3 refus/24 h, inhibition du chlore sur pH haut), en plus des limites journalière et horaire.
- **Le plafond de l'intégrale se dimensionne par l'AUTORITÉ, jamais « en pH·s »** : on pose le débit maximal que l'intégrale seule peut demander (`kPhIntegralAuthorityMlPerMin` = **6,0 mL/min**, soit 2× la charge mesurée de 2,86), puis `integralMax = U × Ti / kp`. Table : `slow` kp 4 / Ti 8 h / **43 200** ; `normal` kp 8 / Ti 6 h / **16 200** ; `fast` kp 12 / Ti 4 h / **7 200**. **`integralMax` VARIE donc avec la vitesse de régulation**, exactement comme `kp` — l'autorité est constante, seule la vitesse de charge change. **9 `static_assert`** verrouillent l'ensemble, dont le verrou central `ki × integralMax ≈ autorité` pour les trois vitesses.
- ⚠️ **Le piège évité, à ne jamais refaire** : l'ancien `integralMax = 50` (inerte tant que `Ki` valait 0) aurait plafonné la contribution intégrale à **0,0185 mL/min** pour un besoin de 2,86 — **0,6 %**, saturé en **140 s**. Réactiver `Ki` sans redimensionner ce plafond aurait été un **échec silencieux** : une régulation « avec intégrale » sur le papier, l'écart résiduel intact dans le bassin. C'est exactement ce que le `static_assert` central rend désormais impossible. Rappel structurant : **à l'équilibre l'erreur tend vers 0, le terme proportionnel disparaît et l'intégrale porte TOUTE la charge**.
- **Intégration SIGNÉE sur `erreur − phStopThreshold`, par quanta d'une seconde**, évaluée à **chaque tick automatique** (que l'on dose ou non ; la sortie, elle, reste gatée). La **décharge devient structurelle** : sous le seuil d'arrêt, l'intégrande devient négatif. **Le quantum n'est pas cosmétique** — à 16 200 pH·s l'*ulp* du flottant vaut 2e-3, un incrément de 5 ms × 0,01 pH aurait été **purement perdu à l'addition** et la décharge n'aurait pas existé. `dt > 10 s` **rejeté** (jamais crédité d'un bloc). Gel d'anti-windup **asymétrique** : il bloque la charge, **jamais** la décharge.
- **Défaut préexistant corrigé au passage** : la bande morte du calcul valait **toujours** `phStartThreshold` (0,05), alors que l'hystérésis de dosage poursuit jusqu'à `phStopThreshold` (0,01). La plage `]0,01 ; 0,05]` était donc une **séance vivante à débit nul**, et le point d'équilibre visé par l'intégration signée était **inatteignable par la sortie**. La bande morte suit désormais l'hystérésis. Le volume total injecté à l'équilibre est **inchangé** (bilan de matière).
- **Nouveau module de logique pure `src/ph_integral_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : charge, oubli, péremption 24 h, hystérésis de saturation. **Motif** : les conditions C2-C5 vivaient dans la coquille ESP32 et **aucune mutation ne les tuait** — et le défaut trouvé était **exactement** dans la garde de C2.
- **Garde-fous** : gel de l'accumulation si la sonde sature la demande (prédicat sur le **débit moyen soutenable** `ampFlow × budget/60`, et non sur le débit d'amplitude — comparé à l'amplitude, ce gel ne s'était **jamais** déclenché) ou si le quota journalier dépasse **80 %** ; **péremption à 24 h** sans évaluation ; **jamais de persistance NVS** (une intégrale stockée, a fortiori corrompue, serait une **autorisation de dosage pré-chargée au boot**). Purge également au changement **réel** de vitesse de régulation, à la bascule `ph_minus` ↔ `ph_plus` et en fin de mode Programmée.
- **Surveillance de saturation** : anomalie = demande **saturée** ET erreur **encore au-dessus du seuil d'arrêt** ; tenue comptée en **temps de branche automatique** (un passage en Manuel suspend l'épisode) ; hystérésis **95 / 85 %** ; `warning` à **2 h**, `critical` à **6 h**, **rappel du critical toutes les 12 h**. Une seule alerte par épisode laissait un silence **permanent** sur un état qui, lui, ne cessait pas.
- **MQTT** : 3 topics retain `{base}/ph_integral`, `/ph_integral_term`, `/ph_integral_pct` + 3 entités auto-discovery HA (« Piscine pH Rattrapage » en tête, les deux autres en `diagnostic`) + alerte **`ph_integral_saturated`**. Aucune entité existante touchée. `kDedupCacheSlots` 18 → 21, publications de discovery 49 → 52.
- **Format de journal changé** : la ligne de configuration PID devient `PID régulation: vitesse=… Kp_pH=… Ki_pH=… (Ti=…h, intMax=… pH·s, autorité=… mL/min) Kp_ORP=… Ki_ORP=0 Kd=0`. Un filtre de journal sur « P temporisée » ou « Ki=0 » **ne matche plus**.

### Fonctionnalités

- **`GET /data` et WebSocket** : 3 champs nouveaux, en **parité stricte** (mêmes noms, mêmes arrondis, `null` explicite) — `ph_integral` (pH·s, signé), `ph_integral_term` (mL/min), `ph_integral_pct` (% du plafond). Quatrième application du contrat de parité après les pentes de sonde (v2.21.2), les champs de calibration (v2.23.0) et l'état de blocage de dosage (v2.25.0). Gabarit de trame WS porté 2560 → 2688 (marge sous le tampon dur : 1 032 o).
- **Page pH — bloc « Rattrapage accumulé »** en pied de la carte Régulation, **en mode Automatique uniquement** : pourcentage de remplissage en tête, détail discret (pH·s et mL/min demandés), et encart d'avertissement au-delà de 95 %. Libellés « correction lente » / « rattrapage accumulé » — aucun jargon d'automatique. Le bloc est **masqué en bloc** sur un firmware antérieur (clé absente ≠ `0`).
- **Le pourcentage est servi par le firmware, jamais recalculé côté client** : le plafond de l'anti-windup **varie** avec la vitesse de correction, le recopier dans le navigateur rouvrirait le motif du miroir de constante, éliminé quatre fois dans ce projet.

### Documentation

- **[ADR-0052](docs/adr/0052-terme-integral-ph-dimensionne-par-autorite.md) créé** — *Levée de `Ki = 0` sur la régulation pH : terme intégral dimensionné par l'AUTORITÉ, intégré en signé sur le seuil d'arrêt*. **Amende le `Ki = 0` de l'[ADR-0016](docs/adr/0016-regulation-p-temporisee-vs-pid.md)** (dont le statut est annoté en conséquence ; `Kd = 0`, `Ki_ORP = 0`, temporisation et anti-windup strict **inchangés**). Consigne le diagnostic de l'écart résiduel, la vérification numérique, la raison pour laquelle `Ki = 0` était **juste en 2026-06** et ne l'est plus, les **sept alternatives écartées** — dont le **biais de consigne**, refusé par principe (« les règles de protection ne doivent pas empêcher la bonne régulation » : une consigne biaisée cesserait de signifier ce qu'elle affiche et exigerait une révision saisonnière, intenable sur une flotte) et `kp 8 → 12`, insuffisant (l'écart tomberait à 0,24 ; atteindre 7,35 en P pur exigerait `kp ≈ 86`) — et **ce que ça verrouille**.
- `docs/subsystems/pump-controller.md` : section « Terme intégral pH — levée de `Ki = 0` », table de dimensionnement par vitesse, points de purge, garde-fous, surveillance, journal, testabilité ; tables PID / constantes / ADR liés mises à jour ; note P5 (« aucun `dt` synthétisé ») requalifiée en garde **active**.
- `docs/API.md`, `docs/subsystems/ws-manager.md` : les 3 champs, budgets de payload, règles de consommation client (clé absente ≠ `null` ≠ `0`).
- `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md` : 3 topics, 3 entités HA, alerte `ph_integral_saturated` (deux paliers, rappel 12 h), slots de déduplication.
- `docs/features/page-ph.md` : bloc « Rattrapage accumulé », règles d'affichage, pourquoi le pourcentage est servi et non recalculé, absences volontaires (tableau de bord, Réglages).
- `docs/UPDATE_GUIDE.md` : note de migration v2.26.0 — aucune action requise, effet attendu et **réserve explicite** sur la vérification terrain.
- `specs/features/todo/feature-078-*.md` : suivi v2.26.0 — la **cause n°1** (« la loi P plafonne la demande ») est traitée ; les mesures M1/M2 et la correction du modèle de comptage restent dues.

### Tests

- **823 → 849 tests** natifs, suite dédiée `test/test_native_ph_integral/`. **Cinq mutations tuent désormais**, dont **trois qui n'en tuaient aucune** avant l'extraction du module pur. Le **test de droop** vérifie la propriété qui fonde la décision : en boucle fermée, un P pur **ne converge pas**, un PI **converge**.

---

## [2.25.3] - 2026-08-28

> ⚠️ **Les injections de correcteur pH deviennent moins fréquentes mais deux fois plus longues.** La durée minimale d'une injection passe de **5 à 10 secondes**. **La quantité demandée par la régulation ne change pas** : ce sont les mêmes millilitres, servis en paquets deux fois plus gros et deux fois moins souvent.
>
> Pourquoi : sur une pompe péristaltique, **chaque démarrage coûte du produit qui n'est pas compté** (le tube se détend, se ré-occlut, la colonne se ré-amorce : 0,1 à 0,5 mL, sur une pompe dont un tour vaut 1-2 mL). Comme **toutes** les injections étaient à la durée minimale, cette perte portait sur **la totalité** du volume. Deux fois moins de démarrages pour le même volume, c'est cette perte amortie sur deux fois plus de produit utile.
>
> Ce qui a déclenché le changement : **968 mL comptés en une journée** et un pH resté à **7,50** pour une consigne à **7,30**, alors que le besoin réel est de **800-900 mL** pour un tel écart sur 40 m³ — chiffre concordant par le calcul chimique **et** par le dosage constructeur. Le pH devrait donc enfin descendre ; **c'est une hypothèse en cours de vérification**, pas un résultat mesuré.
>
> ⚠️ **Dette assumée, à connaître** : le comptage du volume reste **faux** (il ignore la perte au démarrage). En divisant les démarrages par deux à volume *compté* constant, on délivre **réellement** un peu plus de produit sous le même plafond journalier — quelques dizaines de mL par jour. La limite journalière devient donc légèrement **moins prudente**. Il est recommandé de **surveiller le plafond journalier pendant 3 jours** après la mise à jour.

### Firmware

- **Durée minimale d'une impulsion de dosage : `kPulseMinMs` 5 000 → 10 000 ms** (gate `pool-chemistry` : **GO SOUS CONDITIONS**). Un seul entier change — ni la période du train d'impulsions, ni l'amplitude des pompes, ni le modèle de comptage.
- **Le changement n'augmente PAS la dose demandée** — c'est le point contre-intuitif. `planPulseWithCarry` accumule `demande × période` et retire **exactement** le volume d'une impulsion minimale au moment du tir : la moyenne restituée est **mathématiquement indépendante** de cette constante (invariance vérifiée par le gate dans le code). Ce qui change est le **rendement** : deux fois moins de démarrages pour le même volume.
- **Chiffres** (débit à l'amplitude **mesuré** 25,75 mL/min) : volume d'une impulsion minimale **2,47 → 4,61 mL** ; seuil de bascule vers le régime proportionnel direct **2,15 → 4,29 mL/min** de demande ; à une demande de 1,76 mL/min, une impulsion toutes les **~2,7 min au lieu de ~1,4 min**.
- **Innocuité du paquet unitaire** : 4,6 mL = **32 méq**, soit **ΔpH < 0,002** même sur 15 m³ — négligeable jusqu'à ~5 m³ (réserve du gate : un spa de moins de 2 m³ sortirait de ce raisonnement, hors périmètre de la flotte).
- **Effets de bord instruits, tous inchangés** : les cycles/jour et l'anti-rafale sont comptés par **séance**, pas par impulsion ; la limite horaire est réévaluée à chaque tick, donc un dépassement reste borné par un tour de boucle et non par 10 s ; la garde de temps minimum d'injection raisonne en **temps pompe-ON cumulé**, donc la durée murale d'une séance ne bouge pas ; la pause mélange s'arme en **fin de séance** seulement.
- **La période reste à 60 s** (`kPulsePeriodMs`, décision explicite) : la fraction de temps pompe maximale est inchangée, et le refus antérieur de l'utilisateur d'allonger la période reste fondé — l'allonger dégraderait la latence de régulation sans rien gagner sur la moyenne.
- ⚠️ **Le compteur de volume reste faux** (condition n°1 du gate) : cet allongement **atténue** le défaut, il ne le corrige pas. La **mesure de la perte fixe par démarrage reste due** (24 impulsions de 5 s contre 12 de 10 s, à temps de pompe égal), après quoi le modèle de comptage devra être corrigé — et traité explicitement comme une **hausse de dose**, pas comme un correctif neutre. Suivi : [feature-078](specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md).
- **Message de journal** : `Dosage par accumulation : impulsion minimale de …` affiche désormais **10 s**. Une recherche de journal portant sur « impulsion minimale de 5 s » ne matche plus.

### Documentation

- **[ADR-0051](docs/adr/0051-pouls-minimal-double-rendement-avant-comptage.md) créé** — *Pouls minimal doublé (5 s → 10 s) : améliorer le rendement d'abord, corriger le comptage ensuite*. **Amende le pouls minimal (condition B3) de l'[ADR-0043](docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)** (dont le statut est annoté en conséquence ; le reste — proportionnel de temps, amplitude calibrée, kick, aucune interpolation — est inchangé, comme l'est la totalité de l'[ADR-0046](docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md)). Consigne l'invariance de la dose moyenne, les six alternatives écartées (corriger le comptage d'abord, allonger la période, relever `kp`, augmenter l'amplitude, relever la limite journalière, pouls > 10 s), la **dette de comptage assumée** et ce qu'elle verrouille (pas de retour sous 10 s, pas d'empilement de deux atténuations non mesurées).
- `docs/subsystems/pump-controller.md` : nouvelle section « Pouls minimal porté à 10 s », table avant/après, recalcul des exemples chiffrés de l'accumulateur (`V_pouls` 4,61 mL, cadences), écart du volume nominal vs réel ramené à +7-8 % au pouls de 10 s, effet du kick sur les compteurs journaliers divisé par deux, tables de constantes et d'ADR liés mises à jour.
- `docs/subsystems/logger.md` : le message de tir d'accumulateur affiche `10 s`, conséquence sur les filtres de journal et sur le volume de journal produit.
- `docs/API.md` et `docs/subsystems/ws-manager.md` : description de `ph/orp_pulse_ms = 0` corrigée — la valeur du pouls minimal est mise à jour et la mention obsolète du « seuil C » (refus supprimé en v2.22.0 au profit de l'accumulateur) disparaît.
- `docs/UPDATE_GUIDE.md` : note de migration v2.25.3 — aucune action requise, changement de rythme des injections, surveillance du plafond journalier recommandée sur 3 jours.

---

## [2.25.2] - 2026-08-28

> ⚠️ **Les compteurs de dosage journaliers se remplissent légèrement plus vite.** Le débit utilisé pour créditer les brefs à-coups de démarrage des pompes (et les injections manuelles) était mesuré sur **un seul exemplaire de pompe**, puis figé en dur sur un numéro de pompe. Il devient un **majorant unique**, valable pour tous les appareils. Conséquence visible : environ **+3,6 % de volume compté sur la pompe pH** et **+1,9 % sur la pompe chlore** (soit ~+47 mL et ~+25 mL par jour sur ~1 300 mL comptés). Rien n'est injecté en plus — c'est le **comptage** qui devient prudent, donc les limites journalières mordent un peu plus tôt.
>
> Le défaut corrigé : sur un appareil dont la pompe pH aurait été de l'exemplaire le plus rapide, le firmware **sous-comptait de 11 %** — il injectait plus que ses propres limites ne le croyaient.

### Firmware

- **Un seul débit à pleine puissance pour toute la flotte.** `kPump1Flow255MlPerMin = 61,0` et `kPump2Flow255MlPerMin = 67,0` sont remplacées par **`kPumpFlow255MlPerMin = 74,0`**. Ces valeurs n'étaient **pas configurables**, provenaient d'**un seul exemplaire chacune** et servaient de **majorant de sécurité** dans trois usages : crédit de l'à-coup de démarrage de chaque impulsion, majorant du mode manuel `?duty=N`, durée d'une injection volumée sur le chemin **dégradé**. Attribuer la mesure d'un exemplaire à un **index physique de pompe** n'a aucun sens sur une flotte. Les helpers `pumpFlow255()` / `flowAssistFlow255()`, qui réalisaient ce dispatch, sont **supprimés** (11 sites d'appel ramenés à la constante).
- **Pourquoi 74,0 et non 70,0** : la valeur vaut **maximum réellement mesuré 68,0 + 8 % de dispersion inter-unités**. 70,0 a été **refusé par le gate `pool-chemistry`** — +3 % de marge alors que l'écart **déjà constaté entre deux exemplaires du même lot est de 11 %** : une marge inférieure à la dispersion observée est statistiquement nulle.
- **Pourquoi une seule valeur et non deux.** Dédoubler la constante (un majorant pour créditer, un estimateur central pour calculer la durée) a été **rejeté** : les trois usages poussent dans le **même sens**, surestimer ne sur-injecte jamais. Un « estimateur central » destiné à la durée serait par construction une valeur choisie pour **allonger la marche de la pompe**, soit exactement le risque interdit. La précision de l'injection volumée dispose déjà d'un **chemin exact** (amplitude == duty calibré ⇒ débit mesuré) ; le chemin à 255 est un **repli dégradé** dont la précision ne s'achète pas contre une garde.
- **Le débit d'usine à mi-puissance reste 28,0 mL/min, mais sa justification est refondée sur une MESURE.** Les mesures réelles du 2026-08-28 (assistant de mesure du débit, 120 s à duty 127) donnent **26,0 mL/min pour la pompe 1 et 25,5 pour la pompe 2** — les interpolations d'origine (24,7 / 27,2) étaient bonnes à **4-7 %** près. La valeur d'usine n'est donc plus « interpolation » mais **max mesuré 26,0 + 8 % de dispersion**. 26,0 comme défaut a été **refusé** pour la même raison que 70,0 : c'est le maximum de **n = 2 exemplaires du même lot**, donc une marge nulle.
- **Deux `static_assert` interdisent désormais de descendre une valeur d'usine sous le maximum réellement mesuré** : nouvelles constantes `kPumpFlow255MeasuredMaxMlPerMin = 68,0` et `kPumpTypeFlowMeasuredMaxMlPerMin = 26,0`, qui n'interviennent dans **aucun calcul** et ne changent que sur une nouvelle mesure d'exemplaire.
- **`GET /data`** : `flow_assist.flow255` sert désormais **74,0 pour les deux pompes**. Le tableau **reste de taille 2** — casser sa forme ferait tomber les clients existants sur leur repli générique.

### Interface

- **Un troisième miroir de constante de sécurité disparaît du navigateur.** `data/app.js` portait `FLOW_255_ML_PER_MIN = {1: 61.0, 2: 67.0}`, copie en dur des constantes firmware servant à annoncer la durée d'une injection manuelle — devenue fausse de **~21 %**. L'interface lit maintenant `flow_assist.flow255` servi par `/data`.
- **Le repli à 61 / 67 est conservé délibérément** : un firmware antérieur à la v2.25.2 ne publie pas ce champ et dose réellement à ces débits-là. L'aligner sur 74 reviendrait à **mentir sur un appareil ancien**.
- **Assistant de mesure du débit** : le coût budget annoncé pour une purge de 60 s passe de ~61-67 à **~74 mL** (le majorant servi a changé, la purge est inchangée).

### Documentation

- **[ADR-0050](docs/adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md) créé** — *Le débit à pleine puissance est une constante unique de flotte, majorée de la dispersion mesurée*. **Amende les valeurs d'usine de l'[ADR-0047](docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)** (dont le statut est annoté en conséquence ; le reste de l'ADR-0047 — sémantique NVS, drapeaux « mesuré », gel de l'amplitude — est inchangé). Consigne les deux arbitrages de valeur (74,0 contre 70,0 ; 28,0 contre 26,0), le **refus du dédoublement** de la constante, la règle « une marge de flotte doit dépasser la dispersion observée », l'interdiction d'indexer une constante de sécurité sur une pompe physique, et l'interdiction de recopier une valeur de sécurité dans le navigateur.
- `docs/subsystems/pump-controller.md` : nouvelle section « Débit à 255 : constante unique de flotte », table des trois usages du majorant, effet chiffré sur les compteurs, tables de constantes et de calibration de type mises à jour.
- `docs/API.md` : `flow_assist.flow255` (valeur unique de flotte, forme du tableau conservée, règle de consommation client), comptabilité du mode manuel et durée de l'injection volumée sur le chemin dégradé.
- `docs/features/page-ph.md` / `page-orp.md` : origine du débit utilisé pour la durée estimée « ≈ X min », repli historique assumé.
- `docs/features/page-settings.md` : coût budget annoncé par l'assistant de mesure.
- **Deux dettes consignées, non corrigées** : `phPumpControl` / `orpPumpControl` ([`src/config.cpp`](src/config.cpp)) sont **entièrement mortes** (déclarées, initialisées, lues nulle part dans `src/` ni `test/`) ; `MAX_INJECT_DURATION_S = 600` dans [`data/app.js`](data/app.js) est un **quatrième miroir de valeur** (copie de `kManualInjectMaxDurationS`), candidat à être servi par `/get-config`.

---

## [2.25.1] - 2026-08-27

> ⚠️ **Correctif d'observabilité — une panne matérielle était affichée comme un état normal.** Le libellé de refus de dosage « filtre capteur non prêt (warmup / EZO injoignable) » mélangeait deux causes physiques opposées dans une seule phrase, que l'interface analysait ensuite par recherche de fragments de texte. Comme « warm » était testé avant « ezo », **une vraie panne du module EZO s'affichait « Stabilisation du filtre »** — côté pH comme côté ORP. La régulation était à l'arrêt et l'appareil annonçait une attente banale. Mise à jour recommandée pour tous les appareils.
>
> Le défaut symétrique, celui qui a été rapporté, était moins grave mais visible à chaque démarrage : pendant environ six minutes, l'interface affichait **« EZO indisponible »** sur une sonde parfaitement saine (le mot « indispo » du libellé de préchauffage correspondait à la règle de l'UI).

### Fonctionnalités

- **Une panne de sonde n'est plus présentée comme une stabilisation.** L'interface distingue désormais visuellement les **pannes** (« Sonde figée », « Sonde muette » — rouge) des **états d'attente normaux** (« Stabilisation… », « Mesures non représentatives » — ambré). La catégorie est donnée par le firmware, elle n'est plus devinée à partir du texte affiché.
- **Plus de « EZO indisponible » au démarrage sur une sonde saine.** Pendant le délai de stabilisation puis le remplissage de la fenêtre de lissage, l'interface annonce ce qui se passe réellement : une stabilisation en cours.
- **Une lecture manquante depuis moins d'une minute devient « Lecture EZO manquante »** (avertissement) au lieu de « EZO indisponible » (panne) : un blip du bus I²C n'est pas une panne établie.
- **Deux messages contradictoires ne s'affichent plus côte à côte.** Sur le tableau de bord, la ligne d'état de dosage se **tait** quand le badge capteur annonce déjà « Stabilisation : ~N min ». Elle reste bavarde pour tout autre motif, et **toujours** en cas de panne.
- **Une panne de mesure persistante déclenche enfin une alerte.** Sonde figée ou muette pendant plus d'une minute → alerte Home Assistant / MQTT (`ph_dose_sensor_fault` / `orp_dose_sensor_fault`). Jusqu'ici, l'arrêt de régulation pour ce motif n'était visible que dans l'interface web.
- **L'interface avoue son ignorance plutôt que de se tromper.** Face à un firmware plus récent qu'elle, elle affiche « Cause inconnue (code N) » au lieu du libellé le plus proche.

### Firmware

- **`GET /data` et la trame WebSocket `sensor_data` : 2 champs ajoutés** — `phDoseBlockedCode` / `orpDoseBlockedCode`, **code machine** de la cause de refus, publié **à côté** du texte existant. **Parité stricte d'emblée** (mêmes noms, mêmes valeurs, mêmes règles `null` : `0` ⇒ `null` explicite), dans le même passage de sérialisation des deux canaux — la parité n'est plus rattrapée après coup comme en 2.21.2, 2.23.0 et 2.25.0. Coût : **51 o** au pire cas.
- ⚠️ **`enum class DoseRefusal` devient un CONTRAT DE SÉRIALISATION.** Valeurs **fixées explicitement**, **jamais renumérotées**, **jamais insérées au milieu** — toute cause nouvelle s'ajoute **à la fin**, une cause retirée reste **réservée**. **23 `static_assert`** verrouillent la correspondance valeur par valeur, plus un `static_assert` de non-collision avec la sentinelle. Un décalage d'une seule valeur ferait afficher une mauvaise cause, soit précisément la classe de défaut corrigée ici.
- **8 codes ajoutés en fin d'énum** : `15`-`18` `ReadingNaN{Warmup,Frozen,Stale,GateClosed}` (qualifient la garde « lecture filtrée NaN »), `19`-`22` `Filter{Warmup,Frozen,Stale,GateClosed}` (qualifient la garde « filtre non prêt »). Sentinelles hors énum : `kDoseBlockedCodeNone` = `0` (aucun refus), `kDoseBlockedCodeGuard` = `250` (cause de garde de sécurité, message à valeurs runtime non énumérable → le client lit le texte).
- **Plus aucun libellé firmware ne mélange deux causes.** Les deux causes génériques historiques (`3` et `4`) subsistent et disent explicitement « état du filtre indéterminé » : le pire cas est une cause **imprécise**, jamais une panne maquillée en état normal (fail-closed d'observabilité).
- **Les quatre raisons de `SensorFilter::ready() == false` sont enfin discriminées**, sur l'**état interne du filtre** : `warmingUp()`, `frozen()`, `stale()`, `gateClosed()` — quatre prédicats `const` composés par la fonction **pure** `resolveFilterState()` ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)), **unique lieu de décision de priorité** : `frozen → gateClosed → stale → warmup → ready`, **pannes d'abord**. `gateClosed` passe **avant** `stale` : porte fermée, l'âge croît par conception, l'annoncer « périmé » accuserait la sonde d'un blocage voulu par le système.
- ⚠️ **Un premier discriminant fondé sur la fraîcheur de la lecture BRUTE a été refusé par le gate `pool-chemistry`.** Le cache brut est alimenté par **toute** lecture, y compris hors plage et y compris une valeur constante : **une sonde figée répond, donc paraît saine**. Il aurait classé « warmup » un EZO figé ou débitant du garbage — la panne silencieuse aurait été réintroduite sous un nom rassurant, par le correctif censé la supprimer.
- **Le verdict de dosage est inchangé au bit près.** `DoseInputs::filterState` ne sert qu'à **qualifier** la cause : aucune condition ajoutée à `evaluateDose()`, aucun seuil touché, aucune garde déplacée. Initialiseur de membre `Unknown` obligatoire (l'agrégat n'est pas value-initialisé par `DoseInputs in;`).
- **Nouvelle alerte MQTT `ph_dose_sensor_fault` / `orp_dose_sensor_fault`** (`warning`, non-retained) sur sonde **figée** ou **muette**, **une fois par épisode** après `kDoseSensorFaultAlertDelayMs` = **60 s** de panne **continue**. Le délai (12 cycles de lecture EZO) écarte le transitoire de ré-amorçage du filtre à la reprise de circulation — sans lui, chaque démarrage de filtration criait « sonde muette », un cri au loup quotidien. L'**affichage**, lui, reste immédiat.
- **L'épisode d'alerte n'est plus redaté quand la panne change de nature** (correctif de revue) : un EZO agonisant alternant `FilterFrozen` ↔ `FilterStale` par phases plus courtes que la temporisation repoussait indéfiniment la date de début et réarmait le droit d'alerter — **l'alerte ne partait jamais** alors que la régulation était à l'arrêt. Le correctif anti-panne-silencieuse introduisait une panne silencieuse.
- **Une seule trace de journal par épisode** : `publishAlert()` journalise déjà le message en `warning`, y compris broker déconnecté. Le `systemLogger.warning()` supplémentaire produisait deux lignes quasi identiques dans une partition `history` de 64 Ko.
- **`SensorFilter::rearm()` arme une grâce d'observabilité sur `stale()`** (correctif de revue) : sans elle, chaque réouverture de porte affichait une puce rouge « Sonde muette » sur une sonde saine, au démarrage de la filtration. La datation n'est **pas** falsifiée (`_hasValid` / `_lastValidMs` ne décrivent que des échantillons réellement acceptés — c'est l'invariant qui rend `ready()` honnête) : on mémorise que l'âge courant est **imputable au ré-amorçage**, pendant `maxAgeMs` (20 s) au plus, levée par la première mesure acceptée. **Seul `stale()` lit ce drapeau** — `ready()`, `unstable()` et tout le chemin du verdict de dosage l'ignorent. **`reset()` n'arme rien** : une sonde muette au démarrage à froid reste annoncée sans délai.
- ⚠️ **Correctif de repliement sur cette même grâce — 3ᵉ défaut de cette famille dans le projet** (après la retentative de `Slope,?` en v2.21.2 et la comptabilité d'impulsions `tickPulse`). La première écriture n'évaluait l'échéance que dans `stale()`, qui est `const` : sur une sonde restée muette le drapeau ne redescendait **jamais**, et la comparaison de deadline `(int32_t)(now − until) < 0` **se retournait 2³¹ ms plus tard** — une panne de sonde aurait été **masquée à nouveau pendant 24,8 jours**, l'appareil réaffichant « en stabilisation » sur une sonde morte. Atteignable, `millis()` se repliant à 49,7 j sur un appareil qui tourne en continu. Correction **redondante par conception** : (1) le prédicat devient un **intervalle fermé** depuis l'instant d'armement (`now − from < durée`, non signé) au lieu d'une demi-droite vers une échéance — il ne peut plus se retourner, indépendamment de tout désarmement ; (2) un **désarmement effectif** (`_expireStaleGrace()`) en tête de `setContext()` et de `addSample()`, le premier étant atteint toutes les 5 s **inconditionnellement**, y compris bus I²C mort. Les deux protections sont **testées séparément** : une mutation supprimant l'une laisse survivre les tests de l'autre.
- **Règle générale consignée pour tout code manipulant `millis()`** : mémoriser l'**instant d'armement** et tester la **durée écoulée** (`now − from >= durée`, non signé) — jamais une date de fin, jamais une comparaison signée à une échéance, jamais une expiration confiée au seul getteur `const` qui la lit.
- **Le code de cause est écrit AVANT le texte** à chaque pose (`logRefusalOnce()`, `setGuardRefusalCause()`) : un lecteur asynchrone voit au pire le nouveau code avec l'ancien texte, et le code fait foi. `resetRefusalLogState()` / `clearGuardRefusalCause()` remettent les deux à zéro ensemble.
- **Accesseurs ajoutés** : `PumpController.getPhDoseBlockedCode()` / `getOrpDoseBlockedCode()` (`uint8_t`, aucune allocation), `SensorManager.getPhFilterState()` / `getOrpFilterState()` (lock-free, même contrat de concurrence que `is*FilterReady()`), `SensorFilter::warmingUp()` / `stale(nowMs)`.

### Interface

- **Table `DOSE_BLOCK_CODES`** (`data/app.js`) : mapping code → libellé, ton (`normal` / `fault` / `unknown`), libellé court de chip et appartenance à la famille « stabilisation des mesures ». **Plus aucun `includes()`** : `_doseBlockReasonFr()` est supprimée.
- **Trois chemins strictement distincts** (clé absente ≠ `null` ≠ `0`) : code `0`/`null` ⇒ dosage autorisé, ligne masquée ; **clé absente** ⇒ information indisponible, **seul** cas de repli sur le texte serveur, affiché tel quel sans en déduire de catégorie ; **code inconnu** ⇒ « Cause inconnue (code N) », jamais un libellé voisin — un frontend plus ancien que le firmware doit produire un doute visible.
- **Le code de refus est consulté avant le test « brut invalide »** dans `_classifyFilterState()`. Ce test condamnait l'EZO sans savoir pourquoi, ce qui produisait le « EZO indisponible » du démarrage.
- **Ton d'alerte réservé aux pannes** dans le modal « État filtre » : un préchauffage ou une porte de validité fermée restent en ton neutre.

### Documentation

- **[ADR-0049](docs/adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) créé** — *La cause de refus de dosage est transportée par un code machine, et discriminée sur l'état interne du filtre*. Consigne le contrat de sérialisation (pourquoi un code plutôt qu'un texte à parser, pourquoi un entier plutôt qu'une chaîne d'identifiant, pourquoi les valeurs ne doivent jamais être renumérotées), le **refus du discriminant fondé sur la lecture brute** (une sonde figée répond), la priorité pannes-d'abord, l'ordre `gateClosed` avant `stale`, et l'**étanchéité observabilité / décision** (la grâce est lue par `stale()` mais pas par `ready()`). Consigne également, section « Pourquoi la forme du prédicat compte », la **règle de forme des fenêtres de temps** (intervalle fermé depuis l'instant d'armement, jamais de deadline) et ce qu'elle verrouille pour tout module futur. **Douze alternatives écartées** documentées, dont trois erreurs commises puis corrigées en revue (épisode d'alerte redaté, absence de grâce de ré-amorçage, grâce se réarmant seule au repliement).
- `docs/API.md` : 2 champs `/data`, nouvelle section « Code de cause de refus de dosage » (table des 23 valeurs + sentinelle 250, contrat de sérialisation, règles de consommation client), exemple de libellé périmé corrigé, note de parité étendue à six champs.
- `docs/subsystems/ws-manager.md` : section « Code de cause de refus — parité d'emblée », budget de payload, ordre d'écriture code/texte, justification du choix entier plutôt que chaîne.
- `docs/MQTT.md` : alertes `ph_dose_sensor_fault` / `orp_dose_sensor_fault` documentées (déclencheurs, temporisation de 60 s, règle d'épisode, complémentarité avec `ph_filter_not_ready`).
- `docs/subsystems/pump-controller.md` : table des causes de refus réécrite avec les codes, contrat de sérialisation, section « Qualification par l'état interne du filtre », section « Alerte de panne de mesure » (dont la **sémantique exacte de l'épisode** : table des transitions, seule la transition non-panne → panne date l'épisode), ordre d'écriture code/texte, 4 constantes ajoutées à la table, API publique et gardes #4 / #4b mises à jour.
- `docs/subsystems/sensors.md` : section « Prédicats d'état du filtre (observabilité) » — les 4 primitifs, la composition, l'interdiction de reconstruire depuis `get*Raw()`, la grâce de ré-amorçage et son étanchéité, ses **deux protections cumulatives contre le repliement de `millis()`** et les tests qui les couvrent séparément ; table `rearm()` vs `reset()` et table des méthodes de `SensorFilter` complétées.
- `docs/features/page-ph.md` et `page-orp.md` : ordre de classification du chip d'état filtre réécrit, nouveau libellé « Lecture EZO manquante », trois chemins de traduction du code dans le modal.
- `docs/features/page-dashboard.md` : dé-duplication de la ligne d'état de dosage avec le badge capteur, limitée à la famille « stabilisation des mesures ».
- `docs/subsystems/logger.md` : effet de la déconflation des libellés sur le budget du journal (jusqu'à 3 lignes de refus au démarrage au lieu d'1), nouvel émetteur `warning` d'alerte de panne de sonde et retrait en revue du doublon de journalisation.
- `docs/UPDATE_GUIDE.md` : note de migration v2.25.1 — aucune action requise, deux types d'alerte MQTT ajoutés, changement d'affichage, firmware et filesystem à mettre à jour ensemble.
- **Deux specs de suivi créées** : [feature-076](specs/features/todo/feature-076-detection-produit-mal-declare.md) (détection d'un produit de correction mal déclaré, issue de la contre-visite `pool-chemistry`) et [feature-077](specs/features/todo/feature-077-extraction-machine-episode-panne-sonde.md) (extraction de la machine d'épisode de panne sonde en module pur testable — elle vit aujourd'hui dans la coquille, donc hors couverture native).

---

## [2.25.0] - 2026-08-27

> ⚠️ **Correctif critique de sécurité chimique et d'authentification.** Mise à jour recommandée pour tous les appareils, en particulier ceux utilisant un offset pH non nul.
>
> ⚠️ **Deux réglages changent de plage.** La **consigne pH maximale passe de 7,60 à 7,50** : une installation réglée à 7,60 est corrigée automatiquement au premier démarrage, et signalée dans le journal. Le **refus d'alcaliniser passe de 7,70 à 7,60**. Motif commun : au-dessus de 7,60 la fraction de chlore actif tombe sous 25 %, et une consigne à 7,60 pile aurait inhibé le chlore en permanence — une panne sanitaire silencieuse.
>
> ⚠️ **Deux alertes MQTT sont renommées** (`ph_raw_floor` → `ph_guard`, `orp_ph_raw_inhibit` → `orp_ph_inhibit`) et une est supprimée (`ph_target_unreachable`). Une automation Home Assistant filtrant sur les anciens noms doit être mise à jour.

### Fonctionnalités

- **Les seuils de sécurité du dosage pH sont enfin des valeurs fixes, lisibles et vérifiables.** Le plancher variable qui s'adaptait à l'offset est remplacé par des seuils **fixes** portant sur le **pH affiché** (offset compris) : refus d'acidifier sous **6,70**, refus d'alcaliniser au-dessus de **7,60**. L'ancien mécanisme était juste mais incompréhensible — le seuil annoncé bougeait à chaque changement d'offset — et c'est sa complexité qui avait désactivé la régulation en production le 25/08. Deux butées de dernier recours (6,20 et 8,10) restent sur la mesure brute du capteur, pour le cas où l'offset serait mal réglé.
- **La consigne pH redevient atteignable avec un offset large.** Sur une installation avec offset `+0,50` et consigne `7,20`, la régulation dispose désormais de **0,50 pH de marge** avant tout refus. La situation à l'origine de l'incident du 25/08 ne peut plus se reproduire.
- ⚠️ **La consigne pH est limitée à 7,20 – 7,50** (au lieu de 7,20 – 7,60), par pas de 0,10. En dessous, l'eau devient agressive pour le liner, les joints et les parties métalliques, elle est moins confortable et elle consomme le TAC ; **à 7,60 pile**, la consigne atteindrait le seuil d'inhibition du chlore et la désinfection serait suspendue **en permanence** dès que la régulation tient sa consigne. Une consigne à 7,60 enregistrée par une version antérieure est **corrigée au démarrage** et signalée.
- **Le dosage de pH+ est encadré pour la première fois.** Jusqu'ici aucun seuil ne le limitait : rien n'empêchait l'appareil d'alcaliniser l'eau jusque dans la zone où il inhibe lui-même le chlore.
- **Les réglages pH ne peuvent plus être saisis hors plage.** La consigne devient un **sélecteur à 4 positions** (7,20 / 7,30 / 7,40 / 7,50) et l'offset un **incrémenteur** `−` / valeur / `+` (pas de 0,10, boutons désactivés aux bornes). Plus de champ libre, plus de curseur : un curseur donnait 25 px par cran sur mobile pour l'offset, le réglage le plus sensible de la page.
- **Un nouvel avertissement d'eau acide à pH 7,00**, non bloquant : il prévient bien avant que le dosage ne soit refusé (6,70), sans rien arrêter.
- **Les alertes de sécurité partent enfin quand elles sont le plus nécessaires.** Elles étaient muettes quota journalier épuisé et **en mode manuel** — c'est-à-dire dans l'état exact où l'appareil se met après une bascule de sécurité. La surveillance est désormais indépendante de la décision de doser.
- **L'appareil prévient avant une injection manuelle qui contourne une sécurité.** Quand une butée sur la mesure brute est active, un encart explique la situation au-dessus du bouton et une confirmation est demandée. L'injection manuelle reste possible — c'est le dernier recours de l'opérateur — mais elle n'est plus silencieuse.
- **Le message d'alerte n'accuse plus la sonde à tort.** Quand le diagnostic électrochimique de la sonde EZO est normal, l'alerte indique explicitement que **la sonde n'est pas en cause** et oriente vers un décalage de mesure d'origine externe (courants de fuite de la pompe de filtration, équipotentialité du bassin).
- **Une sécurité ne peut plus être annulée par accident.** Quand l'appareil bascule la régulation pH en manuel par sécurité, la simple consultation de l'application pouvait réactiver la régulation et effacer la trace de l'incident. Il faut désormais **acquitter explicitement** l'alerte avec le bouton « Acquitter l'alerte » de la bannière, sur la page pH. L'acquittement **n'enclenche aucun dosage**. Le refus s'applique aussi aux commandes venues de Home Assistant.
- **Un dosage programmé refusé ne ferme plus la régulation.** Seuls les refus survenus en mode automatique comptent vers la bascule de sécurité ; le mode programmé signale son refus (au plus une fois par heure) sans escalader.

### Firmware

- **Sécurité chimique — seuils FIXES dérivés de la plage de consigne** (gate `pool-chemistry` : GO amont + GO en contre-visite ; `code-reviewer` Approved en 2ᵉ passe). Une **source unique** (`kPhTargetMin` / `kPhTargetMax` + trois marges) dont **tous** les seuils dérivent en `constexpr` — aucun littéral, sauf les deux à justification chimique propre (`kPhOrpInhibit`, `kPhLowWarn`) :

  | Seuil | Valeur | Référentiel |
  |---|---:|---|
  | Consigne | `7,20` → `7,50`, pas `0,10` | corrigé |
  | `kPhFloorCorrected` (refus d'acidifier) | `6,70` | corrigé |
  | `kPhCeilingCorrected` (refus d'alcaliniser) | `7,60` | corrigé |
  | `kPhOrpInhibit` (inhibition ORP) | `7,60` (relâche ≈ 7,55) | **corrigé** (était brut) |
  | `kPhLowWarn` (avertissement non bloquant) | `7,00` | corrigé |
  | `kPhFloorRawHard` (butée basse) | `6,20` | brut |
  | `kPhCeilingRawHard` (butée haute) | `8,10` | brut |
  | `kPhGuardReleaseHysteresis` | `0,05` sur **toutes** les gardes | — |

- **Chaque butée brute est à exactement l'offset maximal de son seuil corrigé** : à l'offset extrême, butée et seuil mordent au même point et se relâchent ensemble — aucune ne peut rendre l'autre inopérante. Une butée à **6,40** avait été proposée par le gate : refusée sur objection de l'utilisateur, car avec un offset légitime de `+0,50` elle équivaut à 6,90 corrigé et mordrait **avant** le seuil de 6,70, rendant l'offset inopérant.
- **Trois `static_assert` verrouillent les invariants chimiques** : (i) la consigne haute ne peut pas atteindre le seuil d'inhibition ORP, (ii) le plafond d'alcalinisation ne peut pas le dépasser, (iii) chaque butée brute ne peut pas mordre avant son seuil corrigé. Les `static_assert` (iii) sont écrits en **inégalité** : remonter une butée ne compile plus, l'abaisser reste possible.
- **`kPhHighMargin` abaissée de 0,20 à 0,10** : à 0,20 le plafond tombait à 7,70, soit **au-dessus** de `kPhOrpInhibit` — l'appareil s'autorisait à alcaliniser une eau dont il venait de décider que le chlore y était inactif. Aucun argument physique ne soutient cette zone.
- **`kPhTargetMax` abaissée de 7,60 à 7,50**, avec `kPhOrpInhibitMargin` = 0,10 comme garde-fou **structurel** (n'entre dans aucun calcul de dosage, n'existe que pour le `static_assert`).
- **SÉPARATION SURVEILLANCE / DÉCISION** (contre-visite) : `tickPhGuardSurveillance()` et `tickOrpPhInhibitSurveillance()` sont appelées **inconditionnellement**, une fois par tick, hors de **toute** conjonction d'éligibilité. La détection vivait auparavant dans une conjonction `&&` — le court-circuit la rendait muette dès qu'un terme amont était faux (quota épuisé, mode `manual`, régulation désactivée), c'est-à-dire précisément dans l'état où l'escalade laisse l'appareil. Les surveillances ne comptent rien et n'escaladent jamais ; elles ne lisent que des caches capteur (aucune transaction I²C supplémentaire).
- **Inhibition ORP sur le pH CORRIGÉ** (était le brut) : la fraction de HClO dépend du pH réel. Cela supprime la limitation connue de la première mouture (avec un offset négatif légitime, l'inhibition devenait **permanente**). Une butée brute haute (8,10) s'y ajoute en OU logique.
- **Deux correctifs de revue sur l'inhibition ORP.** **R1** — découplage de la mémoire d'hystérésis et du latch d'inhibition : les branches NaN armaient le latch pour des motifs étrangers à tout seuil, et l'hystérésis, croyant sortir d'un blocage, **maintenait l'inhibition du chlore indéfiniment** à chaque démarrage de filtration (cas observé : pH revenu à 7,57 sans jamais avoir dépassé 7,60). **R2** — une mémoire par seuil : les deux seuils partageaient un drapeau, le moins souvent armé perdait toute hystérésis ; un bassin à 8,03-8,12 avec offset 0,00 produisait un `critical` + une alerte MQTT par oscillation, avec deux messages contradictoires en alternance. Anti-rafale ajouté : **une alerte par gravité et par épisode**, jamais de réémission en dégradation.
- **`ph_plus` gardé** : refus d'alcaliniser au-dessus de 7,60 corrigé + butée brute 8,10. Sans la butée, un offset `−0,50` erroné laissait le pH+ pousser l'eau au-delà de 8,10 brut pendant que le chlore était inhibé — l'appareil conduisait lui-même l'eau dans l'impasse sanitaire.
- **Compteur des 3 refus / 24 h limité au mode `automatic`** (condition du gate). Le mode programmé refuse tout autant mais **signale** à la place : `warning` + alerte MQTT `ph_guard_scheduled` à débit limité (`kPhGuardScheduledAlertIntervalMs` = 1 h) — sans quoi un refus permanent y serait un no-op silencieux après la première transition.
- **Politique NaN uniforme dans les gardes** : une entrée NaN ne produit **jamais** de refus (les gardes NaN existantes couvrent déjà le cas ; fabriquer un refus ajouterait un refus **fantôme** au compteur, jusqu'à fermer la régulation sur une donnée qui n'existe pas). Côté pH, les mémoires d'hystérésis sont **réarmées** ; côté ORP, **conservées** (asymétrie documentée dans l'ADR).
- **La relâche d'une garde efface la cause de refus exposée** — c'est le seul point du firmware qui tourne à chaque tick quel que soit le mode. En `manual`, `canDose(0)` n'étant jamais appelé, la chaîne restait figée sur un blocage révolu, y compris le message d'escalade, dans le mode où l'UI expose justement l'injection manuelle.
- **Suppressions** : `phRawFloorFor()`, `kPhRealFloorTarget`, `kPhRawFloorAbsolute`, `kPhOffsetWideThreshold`, `checkPhTargetReachable()`, `isPhTargetUnreachable()`, `kPhTargetFloorMarginMin` et l'alerte `ph_target_unreachable`. La marge est devenue **structurelle** (garantie à la compilation) : il n'y a plus rien à détecter à l'exécution.
- **Renommages d'API publique** : `getPhDoseBlockedReason()` / `getOrpDoseBlockedReason()` (`String`) → `copyPhDoseBlockedReason(char*, size_t)` / `copyOrpDoseBlockedReason(...)` — rendre une `String` par valeur allouait sur le tas dans la tâche `async_tcp` à chaque appel. `isPhRawFloorBlocking()` → `isPhGuardBlocking(bool countRefusal)`. `kPhRawFloorRefusal*` → `kPhGuardRefusal*`. `kPhRawOrpInhibit` → `kPhOrpInhibit`.
- **`GET /data` : 4 champs ajoutés** — `phDoseBlockedReason`, `orpDoseBlockedReason`, `phRawGuardBlocked`, `orpPhRawGuardBlocked` — en **parité stricte** avec le WebSocket. Ils n'existaient que sur le canal WS alors que l'UI les consomme depuis les deux sources. **Troisième récidive** de ce motif (pentes de sonde en 2.21.2, calibration de pompe en 2.23.0) : l'exigence de parité est désormais consignée dans un ADR.
- **`GET /get-config` : 6 champs ajoutés** — `ph_target_min/_max/_step`, `ph_offset_min/_max/_step`, dérivés des constantes firmware. Le frontend ne code plus **aucune** borne pH en dur ; élargir la plage se fait côté firmware seul. Asymétrie assumée : ces bornes ne sont **pas** dans la trame `config` du WebSocket (constantes de compilation, budget de trame) — le client ignore en bloc un lot absent ou incohérent et conserve les bornes connues.
- **Alertes MQTT** : `ph_raw_floor` → **`ph_guard`**, `orp_ph_raw_inhibit` → **`orp_ph_inhibit`** (les seuils ne portent plus exclusivement sur le brut, l'ancien nom était faux) ; ajout de **`ph_low_warning`** et **`ph_guard_scheduled`** ; suppression de `ph_target_unreachable`. Auto-discovery HA : `min`/`max`/`step` de la consigne republiés (7,20-7,50, pas 0,10), `unique_id` **inchangé**.
- **Validation serveur conservée en défense en profondeur** malgré des contrôles UI qui rendent la saisie hors plage impossible : `/save-config` refuse en **400** hors bornes et **aligne sur le pas** dans la plage ; MQTT **refuse** + `publishTargetState()` pour resynchroniser HA ; le **chargement NVS clampe** + journalise + réécrit immédiatement. L'offset est arrondi au pas par la fonction pure `roundPhOffsetToStep()`, partagée par la route et par la migration NVS (0,47 → 0,50 ; écart maximal 0,05 pH, soit le bruit résiduel du filtre).
- **Acquittement fail-closed de la bascule de sécurité pH** : tant qu'une trace existe, `/save-config` **refuse en 409** toute sortie du mode manuel (gate placé avant toute mutation) et la commande MQTT équivalente est refusée puis **resynchronise Home Assistant** sur le mode réel, sans jamais effacer la trace. Sortie unique : le champ `ph_auto_manual_ack` (**booléen strict** — ArduinoJson rend `true` pour toute chaîne, un `"false"` aurait acquitté). Envoyé seul, il efface la trace **sans réarmer**.
- **`ph_enabled` n'est plus accepté en écriture** (dérivé pur du mode, toujours exposé en lecture) : il ouvrait un second chemin de réarmement contournant le gate. `collectConfig()` côté client n'envoie plus ni le mode ni `ph_enabled` — le repli en dur sur `"automatic"` depuis un onglet périmé était la cause directe de l'incident.
- **Message d'escalade conditionné au diagnostic EZO** : `isPhProbeHealthy()` (pentes ∈ [90 ; 110] %, \|zéro\| ≤ 15 mV, `NaN` ⇒ non concluant) sélectionne l'un des deux textes. Ces seuils ne gouvernent **aucune** décision de dosage.
- ⚠️ **Correctif d'authentification (trou préexistant sur TOUTE la route `/save-config`)** : le contrôle d'authentification n'était posé que dans le handler de complétion, exécuté **après** l'application du corps. Un `POST` **anonyme** pouvait donc écrire la configuration (RAM **et** NVS) avant que le `401` ne parte. Le contrôle est désormais en tête du handler de corps : un corps non autorisé est ignoré en totalité, IP journalisée. L'exemption « premier démarrage » (assistant) est conservée.
- **`publishAlert()` tronque proprement** : les nouveaux messages dépassaient `kMaxPayloadLen` = 128 o et sortaient en **JSON invalide** (Home Assistant rejetait l'alerte entière). Troncature respectant les frontières UTF-8, suffixe `... (voir journal)` ; texte intégral conservé dans le journal et en NVS.
- **Traçabilité** : l'IP d'origine est journalisée sur les deux issues d'un changement de mode de régulation pH (accepté / refusé 409).
- **Module pur `src/ph_guard_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : deux comparateurs symétriques avec hystérésis (`phGuardBlocksBelow` / `phGuardBlocksAbove`), `roundPhOffsetToStep()`, `isPhProbeHealthy()`, `phModeChangeNeedsAck()`. Suite native **746/746**, `pio run` **0 warning**, flash 96,5 %.
- **Dette résiduelle tracée** : l'alerte de refus en mode `programmé` vit encore sous une conjonction d'éligibilité — même classe que le défaut corrigé ici, impact faible (la surveillance couvre la détection). Les indicateurs `phRawGuardBlocked` / `orpPhRawGuardBlocked` **gèlent** pendant les sorties anticipées de la boucle (filtration arrêtée) : voulu, la mesure n'est pas représentative en eau stagnante.

### Documentation

- **[ADR-0048](docs/adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) réécrit** — *Seuils de sécurité pH fixes, dérivés de la plage de consigne, appliqués au pH corrigé*. Remplace la version décrivant le plancher continu asservi à l'offset (jamais publiée). Alternatives écartées consignées : (i) **plancher asservi à l'offset** — incompréhensible, seuil affiché mobile, et sa complexité avait désactivé la régulation ; (ii) escalier à deux paliers — plancher réel variable, pire cas juste avant le seuil de bascule ; (iii) **butée brute à 6,40** — mordait avant le seuil corrigé pour un offset légitime de +0,50, rendant l'offset inopérant (objection de l'utilisateur) ; (iv) **consigne max à 7,60** — inhibition permanente du chlore ; (v) **monter l'inhibition à 7,70 pour légaliser 7,60** — desserrer une sécurité pour autoriser un réglage agressif ; (vi) plafond à consigne + 0,20 — dépassait l'inhibition ORP ; (vii) **curseurs `input[type=range]`** — 25 px par cran sur mobile pour l'offset ; (viii) bornes différenciées par produit ; (ix) écrêtage silencieux ; (x) blocage de l'injection manuelle. Documente aussi les deux **asymétries assumées** (politique NaN opposée pH/ORP ; gel des indicateurs pendant les sorties anticipées) et la dette résiduelle du mode programmé.
- `docs/subsystems/pump-controller.md` : section « Gardes de sécurité pH » entièrement refondue — deux référentiels, table des seuils dérivés, démonstration des butées 6,20/8,10, invariants de compilation, table surveillance vs décision, gardes par produit, avertissement 7,00, alertes et escalade, correctifs R1/R2 de l'inhibition ORP, bornes consigne/offset et bornes servies au client, table des constantes réécrite. API publique de l'en-tête mise à jour.
- `docs/API.md` : 4 champs `/data` + exigence de parité `/data` ↔ WS et mention du gel, 6 bornes servies par `/get-config`, bornes de consigne 7,20-7,50 et alignement au pas, arrondi de l'offset au pas de 0,10, suppression de la section « Garde de cohérence consigne pH » et de `ph_target_unreachable`.
- `docs/MQTT.md` : alertes `ph_guard` / `ph_low_warning` / `ph_guard_scheduled` / `orp_ph_inhibit` documentées, note de **rupture** sur les renommages, bornes `ph_target/set` 7,20-7,50, auto-discovery `min`/`max`/`step`, clamp NVS d'une consigne héritée à 7,60.
- `docs/features/page-ph.md` : contrôle segmenté de la consigne, incrémenteur d'offset, retrait du miroir client de `phRawFloorFor()`, paliers d'avertissement reformulés, règles firmware refondues, encart + confirmation avant injection manuelle sous butée.
- `docs/features/page-orp.md` : encart + confirmation avant injection manuelle sous butée pH brute haute.
- `docs/subsystems/ws-manager.md` : section parité `/data` ↔ `sensor_data` des indicateurs de garde, budget de payload, règle « clé absente ≠ `false` », asymétrie assumée des bornes non transportées par la trame `config`.

---

## [2.24.0] - 2026-08-24

### Fonctionnalités

- **Assistant guidé « Mesure du débit » des pompes doseuses** (feature-075 ; gate `pool-chemistry` : GO amont sous conditions + GO en contre-visite). Le formulaire nu de la feature-074 est remplacé par un assistant pas à pas en **6 étapes** (Préparation → Purge à l'eau claire → Débranchement → Mesure → Volume recueilli → Remontage), plein écran dans Réglages → Avancé (pattern de la calibration pH) : bouton « Mesurer le débit » par pompe, purge minutée (60 s, 3 max), marche de mesure à l'amplitude exacte d'exploitation (120 s), conversion volume lu → ml/min avec avertissement de vraisemblance (20-35 ml/min attendus), enregistrement par le chemin de la feature-074 (drapeau « mesuré » posé). Le coût budget de sécurité est annoncé dès l'étape 1 (chaque marche est créditée au débit majorant).
- **Le mot « pesée » disparaît de l'interface** (grep `data/` vide) : « mesure du débit » partout, l'indication de provenance « pesé » devient « mesuré ». La saisie directe experte subsiste sous un dépliant discret « Saisie avancée ».
- **Les pompes sont toujours arrêtées côté serveur** : purge et mesure sont minutées par le firmware (duty et durée jamais acceptés du client) — onglet fermé ou WiFi coupé, la pompe s'arrête seule à l'échéance. L'UI signale la perte de connexion et le bouton « Arrêter » reste toujours actif.
- **Le dosage automatique est suspendu pendant l'assistant et rétabli automatiquement** à la sortie — ou par un timeout serveur de 15 min sans interaction (un assistant abandonné ne laisse jamais la régulation suspendue). Une mesure interrompue par le contrôleur (perte de filtration) n'est **jamais enregistrable** (drapeau `aborted` — un débit partiel sous-estimé provoquerait une sur-injection systématique).

### Firmware

- **3 nouvelles routes** `POST /flow-assist/start|stop|exit` (auth WRITE, params `pump`+`phase` seuls) : session « une pompe à la fois », **toutes** les gardes d'injection manuelle appliquées au volume majorant (`flow255 × durée`), refus 409 structurés (`assist_busy`, `purge_limit`, `already_injecting`, `invalid_amplitude` + codes des gardes manuelles). Véhicule d'exécution : le chemin `manualInject*` existant, aucune nouvelle mécanique de pompe.
- **Sécurité chimique (P4)** : marche d'assistant interrompue (arrêt utilisateur, sortie, timeout, perte de filtration) → volume **crédité prorata au majorant** au registre journalier (sous suspension, l'intégration par tick est sautée — sans ce crédit, une pompe alimentée serait comptée zéro). Les injections manuelles classiques (`/ph|orp/inject/start`) sont **refusées (409 `assist_active`)** pendant la session (contre-visite gate). Anti-course `exit`/réconciliation : session fermée **avant** la levée de la suspension + re-pose fail-closed au tick.
- **`GET /data`** : nouvel objet `flow_assist` (active, pump, phase, remaining_s, aborted, purges_left, purge_s, measure_s, flow255[2]) — resynchronisation du compte à rebours client, aucun nouveau rythme réseau (buffer 1920 → 2304 o).
- **`PumpController`** : `setFlowAssistSuspend()`/`isFlowAssistSuspended()` — fermeture du dosage auto+programmé par la garde structurelle existante (chemin perte-filtration), mode manuel préservé, log one-shot par transition. Constantes `kFlowAssist*` ; logique pure `src/flow_assist_logic.h` (ADR-0017), 25 tests natifs (674/674 au total).

### Documentation

- `docs/API.md` (3 routes `/flow-assist/*`, objet `flow_assist` de `/data`, code 409 `assist_active`), `docs/features/page-settings.md` (déroulé des 6 étapes, cas limites, amendement du formulaire 074), `docs/subsystems/pump-controller.md` (section suspension : timeout, invariant exit, comptabilité P4, constantes), `docs/subsystems/logger.md` (nouveaux émetteurs événementiels) mis à jour. Aucun ADR : véhicule d'exécution imposé par le gate (chemin `manualInject*` existant), assemblage de mécanismes existants — aucun critère de déclenchement rempli.

---

## [2.23.0] - 2026-08-24

### Firmware

- **Un appareil neuf dose immédiatement, sur la valeur usine du modèle de pompe** (feature-074, [ADR-0047](docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md) — amende le « pesée obligatoire, pas de valeur provisoire » de l'ADR-0043 ; gate `pool-chemistry` : GO amont sous conditions C1-C5 + GO en contre-visite). Contexte flotte (appareils fournis à des tiers, pompe péristaltique standardisée) : les défauts de calibration passent du fail-closed (0, dosage refusé jusqu'à pesée) à la **calibration de type** — duty 127 (`kPumpTypeCalDuty`, même expression entière que l'amplitude, verrouillé par `static_assert`) et débit 28,0 mL/min (`kPumpTypePulseFlowMlPerMin`, mesures de référence du 23/08, **biais sûr** : surestimer le débit ⇒ sous-dosage + compteurs majorants ; à remplacer par la pesée de type).
- **Sémantique NVS** : clé absente (appareil vierge) → défaut de type ; clé **présente à 0** (effacement explicite ou appareil ≤ 2.22 jamais calibré) → **fail-closed conservé**, jamais de conversion silencieuse. Un appareil provisionné garde ses valeurs intactes au flash.
- **Provenance du débit persistée** : nouveaux drapeaux `pumpN_flow_weighed` (NVS `p1/p2_flow_weighed`, purement informatifs — aucun gate de dosage ne s'y fie) ; la garde de plausibilité NVS rabat aussi `weighed = false` (une pesée corrompue n'en est plus une).
- **Correctif on-target** : la trame `config` WebSocket ne portait pas les champs de calibration — le premier push (≤ 5 s) écrasait l'affichage avec « débit non caractérisé » alors que la calibration existait. Parité stricte `/get-config` ↔ trame WS rétablie (+6 champs, buffer `_buildConfigJson` 2368 → 2560 o) ET côté client, clé absente = pas d'information (early-return — même leçon que la parité `/data` des pentes en 2.21.2).

### Fonctionnalités

- **Le réglage de puissance des pompes disparaît des réglages** : l'amplitude d'impulsion est **figée à 50 %** (valeur usine) — les curseurs « Puissance max » sont retirés, remplacés par une ligne lecture seule « Amplitude d'impulsion : X % — débit Y ml/min (**valeur usine** \| **pesé**) ». Le champ `pumpN_max_duty_pct` reste modifiable **par API seule** (validation [40..100] conservée) et est purgé des payloads de save du frontend — plus jamais d'écrasement par un onglet périmé.
- **Formulaire de pesée simplifié à un champ** : seul le débit mesuré (ml/min) est saisi (vide/0/NaN refusés) — le duty calibré est déduit par le serveur de l'amplitude courante et l'indication passe à « pesé ». La pesée devient **facultative** (raffinement de l'appareil) ; le badge d'alerte « Débit non caractérisé — pesée requise » est restreint au seul état encore fail-closed (calibration effacée / NVS hérité à 0).

### Documentation

- **[ADR-0047](docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)** : calibration de type par défaut pour la flotte + gel de l'amplitude — contexte produit, conditions C1-C5 du gate, alternatives écartées (fail-closed en flotte, valeur de type + limites réduites), résidus statués (duty explicite d'un vieux client pose `weighed=true` ; appareil 2.21/2.22 n'ayant jamais sauvegardé sa config = équivalent vierge). Note d'amendement croisée dans l'ADR-0043.
- `docs/API.md` (drapeaux `pumpN_flow_weighed`, duty déduit, résidus rétrocompat `/save-config`), `docs/features/page-settings.md` (curseurs retirés, ligne lecture seule, formulaire à champ unique), `docs/subsystems/pump-controller.md` (section calibration de type, sémantique NVS) mis à jour. Tests natifs : 649/649 sans régression.

---

## [2.22.0] - 2026-08-23

### Firmware

- **Le pH atteint désormais réellement la consigne au lieu de stagner à ~0,26 au-dessus** (feature-073, [ADR-0046](docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md) — amende le seuil C de l'ADR-0043 ; gate `pool-chemistry` : GO sous 5 conditions + GO en contre-visite). Le refus des demandes sous le pouls minimal de 5 s créait une **zone morte** : la régulation s'arrêtait dès que `kp × erreur` passait sous ~2,1 mL/min (stagnation constatée en production le 23/08). Sous les faibles demandes, la pompe **tire désormais une impulsion de 5 s toutes les quelques minutes au lieu de ne rien faire** : la demande est cumulée de période en période (accumulateur borné, par produit pH/ORP) et, dès que le cumul vaut le volume **réel kick-inclus** d'une impulsion minimale (~2,3 mL — le nominal aurait sur-livré de +10-12 %), un pouls de 5 s exactement part par le chemin standard, résidu conservé. Exemple : 0,7 mL/min → une impulsion de 5 s toutes les 3-4 min, volume restitué exact à ±5 %.
- **Sécurité inchangée** : le pouls d'accumulateur n'a **aucun passe-droit** — toutes les gardes (présence d'eau, limites journalières/horaires, cycles, calibration) restent évaluées avant chaque tir, le volume est crédité sur les débits pesés, jamais plus d'une impulsion par période ni d'impulsion < 5 s. L'accumulateur est remis à zéro à l'ouverture et à la fermeture de séance, au reboot (non persistant), et sur toute demande disparue (aucune dette sans demande vivante) ; au retour d'une demande ≥ plancher, le cumul est abandonné et le comportement redevient **bit-identique** à la v2.21 (vérifié en natif sur 255 demandes).
- **Journal** : nouveau log `INFO Dosage par accumulation : impulsion minimale de 5 s (X mL cumulés, résidu Y mL)` — un par séance ; l'ancien `INFO … injection différée (seuil C)` est supprimé.
- **Aucun changement d'interface** : l'état existant « Dosage en attente : Ns » couvre déjà l'attente entre deux impulsions d'accumulateur (calendrier ADR-0045 inchangé, zéro modification web).

### Documentation

- **[ADR-0046](docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md)** : accumulateur de demande sous le pouls minimal — contexte terrain, alternatives écartées (conserver le refus, période 120 s refusée par l'utilisateur, kp plus fort, V_pouls nominal, séance par pouls, fusion, persistance), les 5 conditions du gate et F1/F2/F3 motivées. Note d'amendement croisée dans l'ADR-0043.
- `docs/subsystems/pump-controller.md` (section accumulateur : `planPulseWithCarry`/`PulseCarryPlan`, resets, invariants, log) et `docs/subsystems/logger.md` (émetteur seuil C supprimé, nouveau log par séance) mis à jour. 9 tests natifs `test_F073_*` (649/649 au total).

---

## [2.21.2] - 2026-08-23

### Firmware

- **La puce sonde pH n'affiche plus « EZO indisponible » puis « Pente non disponible » à tort pendant ~20 min après un redémarrage** (bug-slope-boot-perdue-et-data-incomplet). Cause racine : la requête `Slope,?` enfilée au boot pouvait échouer **en silence** (statut EZO 255/NoData ou parasite pendant l'init du module — un échec isolé n'était jamais journalisé) et n'était **jamais retentée** avant le cycle 24 h. Le firmware retente désormais automatiquement, borné et sécurisé (visa `pool-chemistry` GO) : 5 tentatives max (borne dure, jamais réarmée — chaque `Slope,?` bloque `loopTask` ~900 ms), 30 s d'intervalle, jamais sans module EZO détecté. Logique pure `src/slope_retry_logic.h` (ADR-0017), 15 tests natifs (640/640 au total). **L'état se résout seul en < 2 min**, sans action manuelle.
- **Journal** : un échec de `Slope,?` alors qu'aucune pente n'a jamais été lue produit un **warning dès la première occurrence** (one-shot) ; chaque retentative est tracée en info (`n/5`) ; warning unique à l'épuisement des 5 tentatives ; les statuts EZO hors protocole (≠ 1/2/254/255) sont désormais journalisés avec leur valeur (avant : totalement muets, diagnostic impossible).

### API

- `GET /data` expose les 4 champs de pente sonde pH `phSlopeAcid` / `phSlopeBase` / `phSlopeZero` / `phSlopeAgeMs` — **parité de transport** avec la trame WS qui les portait déjà depuis feature-024 (mêmes règles null/arrondi, lecture cache, zéro I²C). Un client sans WebSocket voit enfin la santé de sonde. Buffer `/data` : 1792 → 1920 octets.

### Fonctionnalités

- **Libellés transitoires honnêtes sur la puce sonde pH** : « Sonde pH : lecture en cours… » (premières secondes après boot, remplace le trompeur « EZO indisponible ») et « Pente en cours de lecture… » (pente jamais lue, se résout seul en < 2 min) sont des états **neutres** ; « Pente non disponible » (ambré) est réservé à l'erreur réelle — pente lue puis invalidée après échecs répétés. États sonde usée/à remplacer/calibration inchangés.

### Documentation

- `docs/subsystems/sensors.md` (mécanisme de retentative de boot, nouveaux logs, cause racine), `docs/API.md` (4 champs `/data`), `docs/features/page-ph.md` (les 3 états de la puce) mis à jour. Aucun ADR : fix + patterns existants (ADR-0017), aucun critère de déclenchement rempli.

---

## [2.21.1] - 2026-08-23

### Fonctionnalités

- **La carte pH/ORP du tableau de bord ne dit plus « Injection en cours » quand la pompe ne tourne pas** (bug-dosage-fantome, itération 3, défaut n°3, [ADR-0045](docs/adr/0045-etat-impulsion-expose-en-calendrier.md)). Avec le dosage en impulsions (v2.21.0), une séance alterne pouls et repos ; l'UI affichait « Injection en cours » pendant les repos — la conflation qui avait masqué le dosage fantôme d'origine. La carte montre désormais l'**état réel de la pompe** : « **Injection en cours** » (dot animé) uniquement quand la pompe est alimentée ; « **Prochaine impulsion : Ns** » (dot statique, compte à rebours) pendant les silences du train ; « **Dosage en attente : Ns** » quand la période ne contient aucun pouls (demande trop faible) ; « **Injection manuelle : Ns** » affichée distinctement du dosage automatique. Le compte à rebours est animé localement à la seconde et resynchronisé à chaque trame (±1-2 s) ; à l'échéance sans donnée fraîche il passe en « Prochaine impulsion… » — jamais de bascule optimiste vers « pompe qui tourne ».
- **Bouton « Calibrer la sonde » pH harmonisé restrictif** : sa désactivation proactive inclut désormais la séance de dosage automatique (`ph_dosing`), comme l'ORP le faisait déjà (le clic était déjà bloqué ; le bouton reflète maintenant le verrou).

### API

- `GET /data` **et** push WebSocket `sensor_data` : **7 nouveaux champs identiques** de calendrier d'impulsion — `pulse_period_ms` (période du train, 60000 ms), `ph/orp_pulse_session` (séance ouverte, auto ou programmée), `ph/orp_pulse_ms` (durée d'impulsion planifiée, 0 = deadband), `ph/orp_pulse_phase_ms` (position **relative** dans la période, clampée [0..période]). Getters `PumpController` lecture seule (`isPulseSessionActive`, `getPulseMs`, `getPulsePhaseMs`), fail-closed sur index invalide, aucun chemin de dosage touché (visa `pool-chemistry` : GO). Buffers : `/data` 1536 → 1792 o, pool WS 2176 → 2432 o, `kWsFrameBufBytes` 3072 → **3584** o (trame mesurée ~2,2 Ko). Rétrocompat : champs absents → comportement historique de l'UI.

### Documentation

- **[ADR-0045](docs/adr/0045-etat-impulsion-expose-en-calendrier.md)** : état d'impulsion exposé en **calendrier** (période/pouls/phase) plutôt qu'en booléen instantané (aliasing à 5 s de push pour des impulsions de 5-60 s) ou en `/data` seul (30 s trop lent) ; animation client resynchronisée ; agrandissement contractuel du buffer WS.
- `docs/subsystems/ws-manager.md` (contrat des 7 champs + buffer 3584), `docs/API.md`, `docs/features/page-dashboard.md` (tableau des 6 états, sémantique du dot, honnêteté du compte à rebours), `docs/features/page-ph.md` (verrou Calibrer harmonisé), `docs/subsystems/pump-controller.md` (les 3 getters) mis à jour.

---

## [2.21.0] - 2026-08-23

> ⚠️ **ACTION REQUISE APRÈS MISE À JOUR — le dosage automatique et programmé est BLOQUÉ tant que la pesée des pompes n'est pas saisie.** C'est **voulu** (fail-closed, [ADR-0043](docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) : l'ancien modèle débit↔puissance était faux (prouvé au banc : ×1,9 de sous-estimation en bas, ×1,5 de surestimation en haut) et créditait de l'acide qui ne sortait pas du bidon. Aucune valeur héritée n'est convertie. Procédure par pompe : **peser le volume délivré en 120 s au duty d'exploitation** (affiché à côté du réglage « Puissance max », eau claire, balance au gramme), puis saisir duty + débit dans **Réglages → Avancé → Configuration des pompes → « Enregistrer la pesée »**. Tant que ce n'est pas fait : badge « Débit non caractérisé — pesée requise » + log `CRITICAL` throttlé, et les injections volumées tournent à pleine puissance (255, seul autre débit mesuré).

### Firmware

- **Régulation pH/ORP — dosage en proportionnel de temps** (bug-dosage-fantome, itération 2, défaut n°1 ; option (B)+(C) du gate `pool-chemistry`, GO en contre-visite) : la pompe ne tourne plus à un duty interpolé mais par **impulsions** — période fixe 60 s, pouls minimal 5 s, **kick de démarrage 400 ms à pleine puissance** (couple de décollage à froid) puis plateau à l'**amplitude** (= puissance max × 255/100). Le débit moyen demandé par le régulateur est réalisé par la fraction de temps de marche. Module pur `src/pulse_dosing_logic.{h,cpp}` (ADR-0017), **625 tests natifs verts**. **Conséquence terrain : le pH descendra réellement désormais** — l'ancien régime commandait une puissance à laquelle la pompe ne tournait que par intermittence.
- **Calibration par pompe, fail-closed** : nouvelles clés `pumpN_calibrated_duty` (0 = non caractérisé) + `pumpN_pulse_flow_ml_per_min` (NVS `p1_cal_duty`/`p1_pulse_flow`/`p2_cal_duty`/`p2_pulse_flow`) ; ancienne clé partagée `pump_max_flow` **supprimée sans conversion**. Amplitude ≠ duty calibré ou pompe jamais pesée → dosage auto/programmé **refusé**. **Aucune interpolation nulle part** (`dutyToFlow`/`flowToDuty` et `kPumpMin/MaxFlowMlPerMin` supprimés). Calibration **par pompe** : +10 % d'écart mesuré entre deux pompes identiques (le tube commande).
- **Comptabilité du volume sur débits mesurés** : pouls = 0,4 s × débit-255 pesé + reste × débit d'amplitude pesé (`pulseDeltaVolumeMl`, exact au jitter près — limite connue : sous-comptage ~0,1 mL/pouls à la transition kick→plateau) ; marche manuelle `?duty=N` créditée au **majorant** débit-255 sauf duty == calibré. Constantes de banc (2026-08-23) : Dmin = 82 → `MIN_ACTIVE_DUTY` 80 → **98** (marge P2) ; P1 61,0 / P2 67,0 mL/min à 255.
- **Limite horaire et temps minimum d'injection comptés en temps POMPE-ON** ([ADR-0044](docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md), validé gate) : `usedMs` et la garde des 30 s accumulent sur les impulsions (« minutes où le produit coule »), plus sur la session. Le cycle (`cyclesToday`, anti-rafale) reste compté **par session** ; la pause mélange s'arme en fin de session, y compris fin par deadband.
- **Seuil C (deadband de refus)** : une demande exigeant un pouls < 5 s est refusée (log `INFO` throttlé 30 min) au lieu d'être réalisée par un pouls au débit non reproductible ; ne nourrit pas le WARN « régulation sans effet ».
- **Injections volumées** (`/ph|orp/inject/start`) : duty **calibré** si l'amplitude y correspond, sinon **255 forcé** (durée exacte volume/débit pesé) ; état incohérent → **409 `flow_uncharacterized`**. Plus jamais de durée déduite du modèle linéaire.

### API

- `POST /save-config` accepte les 4 clés de calibration (validation stricte, **400 explicite** — patron `ph_offset`, jamais d'écrêtage d'une pesée) ; `pump_max_flow_ml_per_min` entrant **ignoré** (rétrocompat) ; `pumpN_max_duty_pct` clampé **[40..100]** (plancher de rotation).
- `GET /get-config` expose les 4 clés ; `GET /data` expose `ph/orp_pump_flow_uncharacterized` + `ph/orp_pump_amplitude_duty`.

### Fonctionnalités

- **Réglages → Avancé** : formulaire de pesée par pompe (duty calibré + débit mesuré, « Pesée en vigueur »), badge « Débit non caractérisé — pesée requise », duty d'exploitation affiché à côté du pourcentage, slider « Puissance max » borné à **40 % minimum** (il ne descend plus en dessous — sous ce seuil la pompe cale). Champ « Débit nominal des pompes » supprimé.
- **Recommandation de validation (P3 du gate)** : abaisser les limites pendant **3 jours** (`max_ph/orp_ml_per_day` → 300 mL, limites horaires → 6/12 min, cycles/jour → 12), comparer le compteur au niveau réel des bidons, puis restaurer.

### Documentation

- **[ADR-0043](docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)** : arbitrage complet du modèle de commande (options A/B/C, NO GO initial sur A, re-arbitrage post-mesure — pourquoi (A)+(C) est une régulation morte —, amendement amplitude + kick, fail-closed sans interpolation).
- **[ADR-0044](docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)** : bascule du budget horaire et du temps min en temps pompe-ON (changement de sens d'un invariant préexistant, ADR-0020).
- `docs/subsystems/pump-controller.md` : section « Dosage en proportionnel de temps » (machine à états, gate, comptabilité, getters), purge du modèle linéaire ; `docs/API.md`, `docs/features/page-settings.md`, `docs/features/page-dosages.md`, `docs/subsystems/logger.md` (nouveaux logs throttlés) mis à jour.

---

## [2.20.8] - 2026-08-23

### Fonctionnalités

- **On voit désormais la trajectoire de la grandeur qui décide de l'inhibition du dosage.** Dans la carte « Signaux capteurs » (Réglages → Avancé), un sous-graphe sous chaque graphe brut/lissé (pH puis ORP) trace l'**évolution de la dispersion** (fenêtre glissante de 13 mesures, celle du drapeau `unstable`) sur toute la trace, en **valeur absolue** (pH / mV — pas de % du seuil, décision utilisateur). Courbe ambre à points visibles, identité visuelle propre, sur le **même axe X** que les graphes du dessus : un transitoire visible sur le brut se lit à la verticale sur la dispersion.
- **La marge au seuil se lit graphiquement** : ligne de seuil en pointillé rouge (0,5 pH / 100 mV, légende « Seuil 0,50 » / « Seuil 100 mV »), et l'échelle Y inclut **toujours** le seuil — une courbe très basse montre la marge, jamais un zoom sur le bruit. Le seuil vient de `dispersion_max` (`GET /debug/sensor_filter_state`) si la route répond, sinon d'un repli local : la courbe s'affiche même route en panne (indépendance des deux chargements conservée, invariant 2.20.7).
- **Les transitoires rejetés se voient : ce sont les trous.** Une fenêtre écartée (saut ≥ 1,0 pH / 300 mV, ou trou de mesure) laisse un **trou** dans la courbe, sans interpolation — la note de la carte le précise : *« Un trou dans la courbe de dispersion signale une fenêtre écartée (transitoire rejeté), pas une absence de mesure. »* Moins de 13 points de trace → bloc masqué, pas d'erreur. Statut « indicatif » hérité de la reconstruction 2.20.7, porté par la légende « Dispersion pH (indicatif) ».

### Documentation

- `docs/features/page-settings.md` : carte « Signaux capteurs » — nouvelle sous-section « Courbes d'évolution de la dispersion » (lecture du graphe, sémantique des trous, échelle incluant le seuil, source du seuil route/repli, statut indicatif).
- Pas de nouvel ADR : décision « valeur absolue, pas de % » = décision utilisateur tracée dans la spec ; extension visible et réversible de `createTraceChart` ; arbitrage de placement purement visuel documenté dans page-settings.

---

## [2.20.7] - 2026-08-23

### Fonctionnalités

- **La carte « Signaux capteurs » (onglet Avancé) affiche enfin la grandeur qui décide de l'arrêt du dosage.** Deux nouvelles lignes « Dispersion pH » / « Dispersion ORP » montrent la dispersion **instantanée** publiée par `GET /debug/sensor_filter_state` (v2.20.6), au format « valeur / seuil », avec un état lisible : pill « **stable** » (verte) ou « **INSTABLE** » (rouge — l'inhibition du dosage est en cours). La valeur est directement comparable aux campagnes de mesure. Filtration arrêtée ou fenêtre en cours de remplissage : « — · pas de jugement (circulation arrêtée ou fenêtre en cours de remplissage) », **sans pill** — jamais « stable », car aucun verdict n'est rendu faute de données. Ces lignes s'affichent même buffer de trace vide, et une panne de la route les passe à « indisponible » sans toucher au reste de la carte.
- **Le « Δ » des stats est renommé « Δ 25 min »** pour ne plus s'y méprendre : c'est un crête-à-crête sur **tout** le buffer (≈ 25 min), dérive lente comprise — jusqu'à 5× la dispersion réelle sur les mêmes données. C'est cette confusion (prendre ce Δ pour la grandeur du drapeau `unstable`) qui a motivé la fonctionnalité.
- **Deux lignes « Dispersion trace (indicatif) »** reconstruisent la dispersion sur la trace affichée (fenêtre glissante de 13 points bruts, affichage médiane · max, fenêtres écartées si trou ou saut consécutif ≥ 1,0 pH / 300 mV ; tiret sous 13 points) pour comparaison avec les rapports de campagne. « Indicatif » car deux écarts assumés avec le firmware : sa fenêtre est gelée circulation arrêtée (la trace enregistre tout), et le rejet se fait sur sauts consécutifs et non sur l'écart à la médiane — hors régime établi, la valeur instantanée fait foi (note affichée dans la carte).

### Documentation

- `docs/features/page-settings.md` : carte « Signaux capteurs » — lignes dispersion, sémantique exacte du tiret, renommage du Δ, stats reconstruites et leurs deux limites de fidélité.
- `docs/API.md` §`sensor_filter_state` : la carte « Signaux capteurs » référencée comme consommateur frontend de la route.
- Pas de nouvel ADR : affichage en lecture seule d'un diagnostic existant, aucune contrainte nouvelle — la duplication JS des constantes de rejet est visible dans le code, commentée et réversible.

---

## [2.20.6] - 2026-08-22

### API

- **On peut enfin lire la valeur qui décide de l'arrêt du dosage automatique.** La route de diagnostic `GET /debug/sensor_filter_state` publie désormais, pour le pH comme pour le chlore, la **dispersion de la mesure** (`dispersion`) : l'écart entre la plus haute et la plus basse des 13 dernières mesures. C'est **cette** grandeur, et elle seule, qui fait basculer la mesure en « instable » et coupe la régulation — or elle n'était visible **nulle part**, ni dans l'interface ni dans l'API : l'appareil arrêtait de doser sur un critère que personne ne pouvait observer. Pour estimer la marge restante pendant une campagne de mesure, il fallait la reconstruire de l'extérieur à partir de relevés espacés de 30 s alors que le critère porte sur 13 mesures à 5 s — avec une incertitude impossible à lever.
- Le **seuil** (`dispersion_max` : 0,5 pH / 100 mV), le nombre de fenêtres avant blocage durable (`latch_cycles` = 3) et la **progression vers le déblocage automatique** (`clean_streak`, 12 requises) sont publiés à côté de la valeur : la marge se lit directement, sans connaître les réglages internes du firmware.
- ⚠️ **`dispersion` est vide (`null`) quand la filtration est arrêtée, et c'est normal** : depuis la 2.20.3, l'appareil refuse d'alimenter ses filtres avec des mesures prises dans de l'eau qui ne circule pas. Pas de mesures ⇒ pas de dispersion. **Corollaire important** : filtration arrêtée, un indicateur « instable = non » ne veut **pas** dire « la mesure est stable », il veut dire « **aucun verdict n'a été rendu** ». Confondre les deux fait conclure à tort qu'un réglage est sain. Ne juger de la stabilité que **circulation en marche**.

### Documentation

- `docs/API.md` : section `GET /debug/sensor_filter_state` — les quatre nouveaux champs, le cas `null` (porte d'alimentation), le corollaire sur `unstable`, et l'insensibilité de l'amplitude à l'offset pH. La route reste **sans authentification**.
- `docs/subsystems/sensors.md` : nouvelle section « Observabilité de la dispersion (v2.20.6) » rattachée à la porte d'alimentation et au latch EMI ; les quatre passerelles `SensorManager` (`getPhDispersion()`, `getOrpDispersion()`, `getPhDispersionCleanStreak()`, `getOrpDispersionCleanStreak()`) ajoutées à l'API publique documentée.
- Pas de nouvel ADR : exposition en lecture seule d'un diagnostic **déjà calculé**, aucun arbitrage, aucune contrainte nouvelle sur le reste du système.

---

## [2.20.5] - 2026-08-22

> ⚠️ **Ce correctif ne répare PAS la pompe qui semble ne pas tourner.** Ce symptôme a une autre cause, **non corrigée** à ce jour : pour une erreur de 0,8 pH, l'appareil commande à la pompe une puissance de **32 %**, à laquelle elle ne tourne que **par intermittence**, alors que son modèle interne la croit à 6,4 mL/min. Le compteur de produit monte donc sur de l'acide qui **ne sort pas du bidon**. La correction demande une **mesure au banc** du seuil de rotation réel de la pompe (pesée du volume recueilli) — la version 2.20.5 livre l'outil de cette mesure, pas la correction.

### Firmware

- **Chaque cycle de dosage en était compté deux.** Au premier instant d'un cycle, le régulateur ne peut pas encore calculer de débit (l'écart de temps avec le cycle précédent, ~29 min, dépasse la limite de 10 s au-delà de laquelle un delta est jugé aberrant et rejeté). L'appareil comptait pourtant déjà un démarrage, puis en comptait un **second** à l'instant suivant, celui où la pompe démarrait vraiment. Conséquences visibles : **deux lignes « Démarrage dosage » à la même seconde** dans le journal, et surtout un **budget de cycles par jour qui valait réellement la moitié** de la valeur réglée — la régulation s'arrêtait donc à mi-journée bien avant la limite affichée. Désormais un cycle n'est compté que s'il a réellement démarré.
- **Limite de cycles par jour : nouveau défaut 24 au lieu de 48.** Compte tenu du double comptage ci-dessus, l'enveloppe **réellement vécue** valait déjà 24 : corriger le comptage sans abaisser la limite aurait **doublé** le nombre d'injections quotidiennes réelles, sous couvert de correctif. 24 reste dans le régime d'entretien normal (20-40 cycles/jour).
  > ⚠️ **Ce nouveau défaut ne s'applique pas aux appareils déjà en service** : la valeur est enregistrée dans l'appareil, une mise à jour ne l'écrase pas. Si la vôtre est à 48, **abaissez-la à la main** dans Paramètres → Régulation, sinon vous doublez le nombre d'injections quotidiennes par rapport à ce que vous viviez.
- **Nouvel avertissement au journal « démarrage décidé mais aucun débit commandé »** (pH et chlore, au plus une ligne toutes les 5 min). Il signale une régulation **sans effet** : la décision de doser est prise, mais aucune puissance n'est envoyée à la pompe. Une occurrence isolée au démarrage d'un cycle est normale ; la même ligne **toutes les 5 minutes en continu** indique une anomalie. C'est le bruit involontaire de l'ancien comportement qui avait révélé le bug ci-dessus — sans cet avertissement, la situation serait devenue totalement silencieuse.

### API

- **Nouveau paramètre optionnel `?duty=N` sur `POST /pump1/on` et `POST /pump2/on`** (0-255, défaut 255 : sans le paramètre, le comportement est **strictement inchangé**). Il permet de faire tourner une pompe à puissance réduite depuis le test manuel, afin de **mesurer le seuil en dessous duquel elle s'arrête réellement** — mesure indispensable pour corriger le défaut décrit en tête d'entrée. Volontairement non borné par le plancher interne de 80/255 : sonder **sous** ce plancher est justement l'objet de la manœuvre. Le paramètre ne peut que **réduire** la puissance par rapport aux 255 déjà atteignables, et toutes les sécurités (authentification, présence d'eau, mode maintenance) restent appliquées en amont.

### Documentation

- `docs/API.md` : paramètre `?duty=N` des deux routes de test de pompe — bornes, motif, exemple de balayage au banc.
- `docs/subsystems/pump-controller.md` : nouvelle section « Comptabilisation d'un démarrage de cycle : invariant » (pourquoi « cycle compté ⇔ cycle réellement démarré » est désormais **structurel** et non une garde ajoutée à côté) ; défaut `maxCyclesPerDay` à 24 et **piège de la persistance** (aucune migration, réglage manuel requis sur un appareil déjà provisionné) ; nouvel avertissement et son throttle ; encadré « limite connue, non corrigée » sur la conversion puissance ↔ débit.
- `docs/subsystems/logger.md` : le nouvel avertissement chiffré dans le budget de rétention du journal persistant (régime nominal ≈ 0,36 Ko/h → rétention ≈ 8-10 h ; régime pathologique ≈ 4,3 Ko/h).
- Pas de nouvel ADR : correction de bug locale, tranchée par le gate `pool-chemistry` dans le cadre existant. L'arbitrage structurant (proportionnel de **temps** vs recalibrage du modèle linéaire) fera l'objet d'un ADR **lorsqu'il sera implémenté**, après la mesure au banc.

### Tests

- 4 tests natifs ajoutés (**609 au total**) sur la logique pure de comptabilisation, dont le **rejeu du scénario réel** de production traversant la garde des 10 s.

---

## [2.20.4] - 2026-08-22

> ⚠️ **Bascule structurante : le core Arduino 3.x / ESP-IDF 5 devient l'environnement par défaut du projet — bootloader ET application.** Le **bootloader** ne se met **pas** à jour en OTA : cette bascule exige un **flash USB une fois** (`./deploy.sh all`). Voir [ADR-0042](docs/adr/0042-core3-environnement-par-defaut.md) et [UPDATE_GUIDE.md](docs/UPDATE_GUIDE.md#changer-de-bootloader--flash-usb-obligatoire-core-3-par-défaut-v2204).

### Build

- **`default_envs = esp32dev_core3`** : le core 3 (pioarduino, release taguée `55.03.37` = core 3.3.7 / IDF 5.5.0) n'est plus un banc de compilation, c'est **l'environnement de production**. Les mentions « BUILD-ONLY — ne flashe JAMAIS la production » de `platformio.ini` étaient devenues fausses : l'image core 3 tournait déjà en production depuis des semaines, poussée en OTA sur un bootloader core 2. `esp32dev` (core 2) reste **compilable et déployable en repli explicite** : `pio run -e esp32dev`, `PIO_ENV=esp32dev ./deploy.sh all`.
- **Toute la chaîne d'outillage passe sur une variable unique `PIO_ENV`** (défaut `esp32dev_core3`, surchargeable) au lieu de dépendre du `default_envs` ou de chemins `.pio/build/esp32dev/` codés en dur : `deploy.sh` (passe `-e "$PIO_ENV"` à `pio run`, `pio run -t upload` et `pio device monitor` ; exporte `PIO_ENV`/`BUILD_DIR`), `build_fs.sh` (écrit `littlefs.bin` dans `$BUILD_DIR`), `ota_update.sh` et `build_all.sh` (chemins d'artefacts dérivés), `tools/decode_coredump.sh` (ELF par défaut dérivé).
- **`tools/decode_coredump.sh` prend l'ELF de l'env réellement déployé** — motif de sûreté, pas de confort : un ELF core 2 confronté à un dump core 3 ne produit **pas** d'erreur franche, il produit une pile de **noms de fonctions plausibles et FAUX**. C'est ce qui a rendu le coredump de production du 15/08 inexploitable.
- **⚠️ Piège documenté : les deux plateformes s'écrasent mutuellement.** Elles installent leur framework sous le **même nom de paquet** `framework-arduinoespressif32` (`3.20017.x` en core 2, `3.3.7` en core 3), donc dans le **même répertoire** — un build core 2 détruit le framework core 3 et réciproquement. Symptôme : échec **immédiat** (~2 s) avec `TypeError: expected str, bytes or os.PathLike object, not NoneType` dans `arduino.py` (`FRAMEWORK_DIR` vaut `None`). **Ce n'est pas aléatoire** — c'est systématiquement un build de l'autre env qui vient de passer. Remède : `pio pkg install -e <env>`. Ce défaut a été pris pour un aléa pendant tout un cycle de travail, les échecs étant contournés par simple relance.
- **Marge flash resserrée** : le core 3 occupe **1 730 053 o** sur une partition applicative de 1 835 008 o, soit **94,3 %** (marge ≈ 102 Ko), contre **84,5 %** en core 2. Le plafond de vigilance de 95 % est presque atteint — à surveiller à chaque nouvelle fonctionnalité.
- **Perte du filet de repli sans câble** : tant que le bootloader restait en IDF 4.4, une image core 3 défaillante se rattrapait en repointant `otadata` (`erase_region 0xE000 0x2000`). Avec un bootloader core 3, ce recours disparaît : **un échec impose un flash USB**, donc un accès physique au boîtier.
- **CI non impactée** : `.github/workflows/tests-natifs.yml` ne construit que l'env `native`.

- **Le moniteur série est enchaîné après tout flash USB** (`firmware`, `fs`, `all`, `factory`). Ce n'est pas un confort : l'ouverture du port bascule DTR/RTS, ce qui **redémarre l'ESP32** — le boot est donc capté **depuis sa première ligne**. Un moniteur lancé à la main dans un second terminal arrive systématiquement trop tard et manque les lignes émises une seule fois. `NO_MONITOR=1 ./deploy.sh all` pour s'en passer (scripts non interactifs). Sortir par `Ctrl+C` n'est plus compté comme un échec de déploiement.
- **Le port série est détecté au lieu d'être codé en dur.** `PORT` valait `/dev/cu.usbserial-0001` en dur : l'upload échouait dès qu'on branchait un autre module (un adaptateur neuf n'énumère pas sous le même nom). Ordre de résolution : variable `PORT`, puis le port historique s'il existe, puis le premier `usbserial` / `wchusbserial` / `SLAB_USBtoUART` trouvé. Sans port, le script s'arrête **avant toute compilation** en listant les ports disponibles, au lieu d'échouer après 2 min de build. Surcharge : `PORT=/dev/cu.usbserial-10 ./deploy.sh all`.
- **`pio run -t upload` reçoit enfin `--upload-port`.** Sans ce paramètre, PlatformIO retombait sur l'`upload_port` de `platformio.ini` et **ignorait le port détecté** : l'upload échouait alors que le module était bien branché, ailleurs.
- **`factory` réordonné — le mot de passe AP est enfin lisible.** L'effacement de flash était fait **avant** les uploads, or `upload_firmware` et `upload_filesystem` redémarrent la puce : le boot qui générait et affichait le mot de passe AP avait donc déjà eu lieu, **sans personne pour le lire**. La partition `nvs` seule (`0x9000` / `0x5000`) est désormais ré-effacée **en dernier**, avec `--after no_reset` pour que le seul reset soit celui provoqué par l'ouverture du moniteur. Le mot de passe s'affiche donc dans les premières lignes du moniteur enchaîné — **seule occasion de le lire** sur un appareil dont l'assistant a déjà été complété.
- **Compatibilité esptool 4 / 5 sondée à l'exécution.** Le paquet `tool-esptoolpy` est **partagé** entre les environnements et chacun impose sa version (`esp32dev` → 4.5.1, `esp32dev_core3` → 5.1.2) : elle bascule au gré du dernier build lancé. esptool 5 a renommé sous-commandes et valeurs d'options en tirets (`erase-region`, `--after no-reset`) là où la 4.x utilise des underscores. Le script sonde `--help` et choisit l'orthographe correspondante, au lieu de supposer une version.
- **Archivage `deploy.sh` supprimé** (`archive_elf()`). L'archivage est fait depuis la v2.20.2 par le hook post-build `tools/archive_build.py`, qui range `builds/<VERSION>/<ENV>/` et couvre **aussi** `pio run` direct et `ota_update.sh`. L'ancienne fonction déversait en parallèle des ELF non versionnés à plat dans `builds/` — un doublon qui repolluait le dossier après chaque ménage.

### Documentation

- **Nouvel [ADR-0042](docs/adr/0042-core3-environnement-par-defaut.md)** — « Le core Arduino 3.x / IDF 5 devient l'environnement par défaut (bootloader ET application) » : ce qui a rendu la porte franchissable (module WROVER-E avec PSRAM, accès USB, image core 3 éprouvée en production), le conflit de paquet framework, la perte du filet `otadata`, la marge flash 94,3 %, et les alternatives écartées (dont « rester en core 2 par défaut avec OTA core 3 »).
- `docs/adr/0034-*` : **note additive datée** — porte de bascule **franchie**, clauses « BUILD-ONLY » et « retour arrière OTA-only » déclarées **caduques**, renvoi vers l'ADR-0042.
- `docs/BUILD.md` : tableau des environnements **inversé** (core 3 = défaut/production, core 2 = repli), nouvelle section `PIO_ENV` (rôle dans chacun des 5 scripts), section dédiée au **conflit de paquet** avec symptôme exact et remède, marge flash par env, avertissement bootloader/USB, et la garde sur l'ELF de décodage de coredump. Plus, du cycle outillage : moniteur enchaîné, détection/surcharge du port, `NO_MONITOR=1`, séquencement du mode `factory`, partage de `tool-esptoolpy`, archivage corrigé (`deploy.sh` n'archive plus rien).
- `docs/UPDATE_GUIDE.md` : nouvelle section « **Changer de bootloader — flash USB obligatoire** » (tableau de ce que l'OTA peut/ne peut pas écrire, procédure, disparition du filet `otadata`, retour arrière `PIO_ENV=esp32dev`). Nouvelle section « Récupérer le mot de passe WiFi AP » — l'ancienne manœuvre (ouvrir `pio device monitor` puis redémarrer) **ne fonctionne pas** sur un appareil configuré. Trois entrées ajoutées au dépannage (port non détecté, upload sur mauvais port, mot de passe AP perdu).
- `README.md` : environnement de build par défaut et repli `PIO_ENV` ; première connexion et liste des scripts alignées sur le moniteur enchaîné et la détection de port.

---

## [2.20.3] - 2026-08-21

> 🔴 **Correctif de sécurité chimique — mise à jour fortement recommandée.** Le filtre de mesure pH pouvait se **verrouiller définitivement** et couper **toute** la régulation — pH **et** chlore — **sans aucune alarme** et sans aucun moyen de reprise depuis l'interface. C'est arrivé en production le 16/08 : le bassin est resté ≥ 3 h sans dosage, et l'anomalie n'a été détectée que parce qu'une valeur affichée semblait incohérente. Firmware **et** filesystem à mettre à jour (`./deploy.sh ota-all`).

### Fonctionnalités

- **Alerte « Régulation à l'arrêt »** : une bannière rouge s'affiche en tête du tableau de bord dès qu'un filtre de mesure est bloqué depuis plus de 5 minutes alors que la filtration tourne — c'est-à-dire dès qu'aucun dosage n'est plus effectué. Elle indique quel produit n'est plus régulé (pH, chlore ou les deux) et mène directement à la page concernée. Une purge de la chambre de mesure, un démarrage de filtration ou une filtration à l'arrêt ne la déclenchent **jamais** : elle ne signale que les blocages anormaux.
- **Bouton « Réarmer le filtre »** dans les modales « État filtre pH » et « État filtre ORP ». Jusqu'ici, la seule façon de débloquer un filtre était une URL de diagnostic non documentée — pratiquement inaccessible. Le réarmement demande une **confirmation explicite** qui prévient que la détection de sonde figée sera effacée, il est limité à **un par capteur toutes les 10 minutes**, et il agit **sur un seul capteur** : réparer le pH ne casse plus le filtre ORP.
- **Le chip d'état du filtre affiche « Régulation à l'arrêt »** en rouge, avant toute autre indication, quand ce blocage est détecté.
- **Nouvelle bannière « Bascule automatique en mode manuel »** sur la page pH : quand l'appareil coupe lui-même la régulation pH par sécurité (refus de plancher acide répétés), il l'écrit désormais **noir sur blanc**, avec la date et la cause — au lieu de laisser l'utilisateur découvrir un mode `manuel` inexpliqué. L'information est conservée même après redémarrage, et disparaît dès que vous choisissez vous-même un mode de régulation.
- **Deux nouvelles entités Home Assistant** : « Filtre pH Bloqué » et « Filtre ORP Bloqué » (`device_class: problem`), pour automatiser une notification externe.

### Firmware

- **Le filtre de mesure ne pouvait plus jamais se débloquer.** Un échantillon éloigné de plus de 1,0 pH (300 mV en ORP) de la médiane était écarté **avant** d'alimenter le filtre — mais la médiane ne pouvant évoluer que par des échantillons acceptés, dès que le pH réel s'écartait durablement, **chaque** mesure suivante était rejetée à son tour et la médiane restait figée **indéfiniment**. Relevé en production : médiane interne **4,79** contre un pH brut **6,96**, compteur de rejets saturé à 255. Sans dosage pH (filtre non prêt) **et** sans dosage chlore (suspendu par sécurité en l'absence de pH fiable).
- **Cause première : le filtre avalait des mesures que l'appareil savait fausses.** Pendant un arrêt de circulation, l'eau stagne dans la chambre de mesure — au voisinage du point d'injection d'acide, elle peut être très acide. L'appareil déclarait bien ces mesures « non représentatives » et suspendait l'injection, mais **rien n'empêchait le filtre de les ingérer** : sa médiane convergeait vers la valeur de l'eau stagnante, puis rejetait comme aberrantes les vraies mesures qui suivaient la reprise. **Le filtre n'ingère plus rien** quand l'eau est à l'arrêt ou pendant la stabilisation, avec une garde de 60 s après chaque reprise de circulation (utile même si le délai de stabilisation est réglé à 0), et il **repart de zéro** au retour à des mesures représentatives.
- **Sortie de secours** : 12 mesures rejetées d'affilée mais **proches les unes des autres** (≤ 0,20 pH / 40 mV) décrivent une nouvelle valeur, pas du bruit — le filtre abandonne alors sa médiane et se réamorce. Trois garde-fous cumulatifs empêchent d'en faire une porte de sortie pour une sonde défaillante : **30 min** minimum entre deux réamorçages, valeur plausible exigée (pH 6,50–8,20 brut, ORP 300–950 mV), et cohérence de la série vérifiée. Si l'un refuse, le filtre **reste** bloqué, un `CRITICAL` et une alerte MQTT partent, et la sonde est à vérifier. Les 12 mesures rejetées sont **jetées**, jamais réinjectées.
- **Une sonde qui rate une lecture de temps en temps peut quand même réamorcer** : au plus 2 trous par série, jamais 2 consécutifs. Sans cette tolérance, une sonde manquant un cycle sur douze pendant un vrai changement de pH n'aurait **jamais** réamorcé — l'incident se serait reproduit à l'identique.
- **Un réamorçage n'efface jamais un diagnostic de sonde** : contrairement à une réinitialisation complète, il ne touche ni la détection de sonde figée ni l'alarme de bruit électrique. Le dosage reste bloqué pendant le réamorçage complet (~1 min). Motif : l'origine de l'excursion à ~4,8 **n'est pas encore établie** — stagnation acide ou sonde défaillante — et le correctif ne doit pas masquer un défaut matériel.
- **Alarme d'absence de régulation** : filtre non prêt pendant 5 min alors que la filtration tourne et que les mesures sont représentatives → `CRITICAL` + alerte MQTT avec la cause probable, répété toutes les 30 min, `INFO` à la levée. La purge et la filtration à l'arrêt ne comptent jamais.
- **La bascule automatique de la régulation pH en manuel est persistée** (date + cause en NVS) : le journal étant un tampon circulaire, cette trace disparaissait en quelques heures et il devenait impossible de dire si le mode `manuel` venait de l'appareil ou d'une action humaine. Elle n'est acquittée que sur un **changement réel** de mode — enregistrer les réglages sans toucher au mode ne l'effacera pas.
- **Instrumentation pour trancher l'origine du défaut** : les mesures brutes pH/ORP sont désormais journalisées à la reprise de circulation **et** en fin de stabilisation, avec l'écart entre les deux. Un écart franc confirmerait la stagnation acide ; un écart nul sur une valeur aberrante orienterait vers une sonde défaillante.
- **Correctif de concurrence** : plus aucune réinitialisation de filtre n'est exécutée depuis le serveur web. Les demandes sont mises en attente et traitées par la boucle principale (< 100 ms), seul contexte autorisé à écrire dans un filtre — l'ancienne route de diagnostic violait ce contrat et pouvait corrompre le filtre pendant une lecture.
- **Le filtre ORP bénéficie des mêmes correctifs** : la logique est partagée, donc le défaut l'était aussi.

### Sécurité

- **`POST /debug/sensor_filter_reset` exige désormais une authentification administrateur.** Cette route effectuait exactement la même action destructrice que le nouveau bouton de réarmement (effacement de la détection de sonde figée), mais était **ouverte sans mot de passe** : elle contournait intégralement les garde-fous posés sur l'autre chemin. Les autres routes `/debug/*` restent inchangées.

### API

- **Nouveau `POST /reset-sensor-filter?sensor=ph|orp`** — auth administrateur, sélectif par capteur, 1 réarmement/capteur/10 min (`429` avec `retry_after_s`), réponse `{"queued":true}` (exécution au tour de boucle suivant).
- `POST /debug/sensor_filter_reset` accepte `?sensor=ph|orp` (sans paramètre : les deux filtres, comme avant).
- `GET /data` et WebSocket : nouveaux booléens `phFilterAlarm` / `orpFilterAlarm`.
- `GET /get-config` : nouveaux champs **lecture seule** `ph_auto_manual_epoch` / `ph_auto_manual_reason`.
- MQTT : nouveaux topics retain `{base}/ph_filter_stuck` et `{base}/orp_filter_stuck` (`ON`/`OFF`), et nouvelles alertes `ph|orp_filter_not_ready` et `ph|orp_filter_rearm_denied`.

### Tests

- **605 tests natifs**, zéro régression — dont le **rejeu de l'incident** à partir des valeurs réellement relevées (médiane établie à 4,79 puis échelon soutenu à 6,96 : le filtre doit redevenir prêt en un nombre d'échantillons borné et asserté), la transition purge → mesures valides, la non-régression sur aberration isolée, et la tolérance aux trous de lecture. `pio run` bi-core SUCCESS.

### Documentation

- **Nouvel [ADR-0041](docs/adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)** — « Autorité des mesures non représentatives et sortie de secours du filtre de tendance centrale » : le verdict `pool-chemistry` et son raisonnement, les six alternatives écartées (dont « geler l'alimentation seule », **inopérante** — le filtre devient non prêt de toute façon et la médiane pré-arrêt reste autoritaire — et « `ready` immédiat après réamorçage », qui masquerait une sonde morte), et ce que la décision verrouille pour la suite.
- `docs/adr/0027-filtre-tendance-centrale.md` : note additive datée — le fail-closed « spike répété » est requalifié en piège sans issue, renvoi vers l'ADR-0041 (aucune décision annulée).
- `docs/API.md` : ⚠️ **correction d'une information fausse** — le titre de `POST /debug/sensor_filter_reset` annonçait « pas d'auth » ; documentation de `POST /reset-sensor-filter`, du paramètre `?sensor=`, de la sémantique *queued*, des champs `/data` et `/get-config`.
- `docs/subsystems/sensors.md` : ⚠️ **correction** — le reset n'agit plus sur « les deux » filtres ; sections porte d'alimentation, sortie de secours et ses 3 gardes, tolérance aux trous, distinction `rearm()`/`reset()` et **pourquoi**, alarme, réarmement par capteur.
- `docs/subsystems/measure-validity.md` : le filtre est un **nouveau consommateur** du verdict de représentativité ; instrumentation des bruts au front montant / en fin de purge.
- `docs/subsystems/pump-controller.md`, `docs/subsystems/mqtt-manager.md`, `docs/subsystems/ws-manager.md`, `docs/MQTT.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/features/page-dashboard.md` : persistance de la bascule pH, topics et entités HA, champs WS et budget de payload, boutons et bannières.

---

## [2.20.2] - 2026-08-15

> ⚠️ **Correctif de disponibilité — mise à jour recommandée.** L'appareil tournait **sans protection contre les crashs mémoire** environ 2 h après chaque démarrage. Firmware seul, aucune mise à jour du filesystem nécessaire.

### Firmware

- **Le parachute anti-crash mémoire était MORT ~2 h après chaque boot.** Le filet de sécurité (`new_handler`) se désinscrit lui-même quand sa réserve d'urgence est épuisée, et sa ré-inscription se trouvait **derrière un `return`** : passé la deuxième famine, plus aucun chemin ne le rétablissait. C'est la cause directe des **4 crashs de production** `abort()` sur la tâche réseau `async_tcp` (le serveur web fait des allocations « lançantes » à chaque connexion HTTP et à chaque trame WebSocket). La ré-inscription est désormais faite **en tête du tick, avant toute sortie possible**.
- **Nouveau témoin permanent `oom_handler_registered`** dans `GET /get-system-info` : on peut enfin vérifier à tout instant que le filet est présent, sans dépendre du journal. Champ **additif** — aucun client existant n'est cassé.
- **Le réarmement du parachute tournait en boucle infinie** et fabriquait **~100 % de son propre signal d'alarme** : il lisait une mesure mémoire figée à 27 636 o (reliquat d'IRAM inutilisable) alors que le plus gros bloc réellement allouable valait 4 596 o, et retentait donc une allocation de 12 Ko vouée à l'échec **toutes les 10 s** — soit les 1 025 « échecs d'allocation » comptés le 15/08. Le réarmement lit maintenant la mesure **8 bits réelle**, et les échecs **auto-infligés** sont exclus du compteur.
- **Paliers de réarmement recalibrés** sur le régime réel mesuré (`maxBloc8 ∈ [4 084 ; 9 204]`, creux 3 060) : seuil du palier de repli **12 288 → 2 048**, taille de la réserve de repli **2 048 → 1 024**. Le palier nominal reste à 24 576, **délibérément inatteignable** en régime fragmenté — il repart tout seul si la fragmentation disparaît.
- **Le journal ne se détruit plus lui-même.** Le WARN d'échec d'allocation passe d'un rythme **fixe de 30 s** (~120 lignes/h, ~90 % la même ligne) à un **backoff** immédiat → 30 s → 5 min → 30 min, avec retour au rythme initial après 1 h sans incident. La trace « Heap 8bits: » passe de **5 min à 30 min** et perd sa partie `interne:`, figée et sans information. Résultat mesuré : **rétention du journal persistant de 26-34 min à 14,6-19,5 heures** (14,1 Ko/h → 0,41 Ko/h), à taille de fichier **inchangée** (8 Ko). C'est ce flot qui avait détruit le post-mortem du crash de 16:43 avant qu'il puisse être lu.
- **Identification du fragmenteur** : le nom de la fonction appelante et le masque `caps` de chaque allocation échouée, jusqu'ici **jetés**, sont désormais capturés et journalisés — depuis la boucle principale, **jamais** depuis le callback (le verrou du tas y est détenu). Seule voie vers l'origine de la fragmentation, inconnue après trois cycles.
- **⚠️ Ce que cette version NE change PAS, volontairement** : les gardes `kRestartMaxAllocHeapBytes` (redémarrage d'auto-guérison), `kHttpHeapMarginBytes`, `kWsPushMinAllocBytes`, `kLowHeapWarnBytes` et les deux seuils de la mise à jour en ligne **lisent toujours l'ancienne mesure**. Le gate de sécurité chimique a rendu un **NO GO explicite** : les basculer aux valeurs actuelles rendrait la **suspension du dosage atteignable en fonctionnement normal** (seuil 8 192 contre un minimum réel de 3 060), casserait l'interface web, les mises à jour temps réel et la mise à jour en ligne. Recalibrage après **24 h de relevés propres**.

### Build

- **Archivage automatique des artefacts à chaque `pio run`** (`tools/archive_build.py`, branché en `extra_scripts = post:` sur les deux environnements matériels) : `firmware.elf` **et** `firmware.bin` sont copiés dans `builds/<VERSION>/<ENV>/firmware_<horodatage>.{elf,bin}`, rétention de 5 par version **et par environnement**. Cause : `deploy.sh` archivait bien l'ELF, mais `pio run` direct et `./ota_update.sh` **ne passent pas par lui** — le dernier ELF archivé datait du 02/08, ce qui a rendu le **coredump du 15/08 inexploitable** (`coredump SHA256 ≠ app SHA256`). Le cloisonnement par environnement est essentiel : 5 builds core 2 auraient évincé l'ELF core 3 réellement déployé.

### Tests

- **582 tests natifs** (+16 : condition de ré-inscription du handler, escalade et saturation du backoff, exclusion des échecs auto-infligés, paliers de repli recalibrés), zéro régression. `pio run` et `pio run -e esp32dev_core3` SUCCESS (flash 83,8 % / 93,6 %).

### Documentation

- **Nouvel [ADR-0040](docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md)** — « On ne calibre pas des seuils sur une fragmentation pathologique » : acte le **régime bi-métrique assumé** (seuls les paliers de réarmement passent en 8 bits), le NO GO du gate chimique sur les 5 autres gardes, et les 4 alternatives écartées.
- `docs/adr/0035-*` : **note additive datée** — invariant de ré-inscription du `new_handler` (en tête de tick, indépendante du miroir ; miroir posé **avant** l'inscription sur les trois sites).
- `docs/adr/0038-*` : **note additive datée** — la règle `clamp(0,40 × min24h ; 8192 ; 16384)` est déclarée **inapplicable**, les mesures du 15/08 sont **invalidées** comme base de calibrage, l'invariant d'ordre des seuils est momentanément sans signification.
- `docs/subsystems/oom-guard.md` : ré-inscription et son invariant d'ordre (avec le pourquoi), réarmement 8 bits, exclusion des échecs auto-infligés, backoff du WARN, capture de l'appelant, échelle des seuils annotée **par métrique**.
- `docs/subsystems/logger.md` : rétention **réelle chiffrée** avant/après et cadences des émetteurs périodiques ; règle « chiffrer le débit en Ko/h avant d'ajouter une trace périodique persistée ».
- `docs/subsystems/system-health.md` : cadence et format de la trace « Heap 8bits: » (30 min, `interne:` retiré).
- `docs/API.md` : champ additif `oom_handler_registered` de `/get-system-info`.
- `docs/BUILD.md` : archivage ELF+BIN à chaque `pio run`, nommage, cloisonnement par version **et** par environnement, et la panne forensique qui l'a motivé.
- Spec de suivi créée : `specs/features/todo/bug-bascule-gardes-8bits-recalibrage.md` (bloc E différé).

---

## [2.20.1] - 2026-08-15

> 🔄 **Mise à jour du filesystem nécessaire** : l'interface change (carte « Mise à jour manuelle »). Faire le **firmware d'abord**, puis `./build_fs.sh` et `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi).

### Fonctionnalités

- **Le type de mise à jour est déduit du nom du fichier** dans la carte « Mise à jour manuelle » (Paramètres > Système) : sélectionner `littlefs.bin` règle automatiquement le type sur « Système de fichiers », et une notice bleue l'annonce. Le sélecteur revenait auparavant à « Firmware » **à chaque rechargement de page** — donc juste après un flash de firmware réussi, qui redémarre l'appareil et recharge l'interface : envoyer le filesystem dans la foulée partait déclaré « firmware » et échouait. C'est exactement l'incident vécu deux fois.
- **Avertissement, jamais blocage** : si vous changez le type et qu'il ne correspond plus au nom du fichier, la notice passe en orange et une confirmation est demandée à l'envoi — **franchissable en un clic**. Le cas légitime existe (image renommée), il n'est pas interdit. Le type n'est **pas mémorisé** d'une visite à l'autre : ce serait recréer le même piège dans l'autre sens.
- **La cause d'un échec est enfin affichée**, en clair et en français, sous la barre de progression : « *l'image reçue n'est pas un firmware ESP32 (octet magique 0xE1 au lieu de 0xE9) — type « Système de fichiers » attendu ?* ». Auparavant l'interface n'affichait que « FAIL ».
- **Une image envoyée avec le mauvais type est refusée sans aucun effet de bord** : les pompes doseuses ne sont pas inhibées, le filesystem n'est pas démonté, rien n'est écrit, l'appareil ne redémarre pas. Corriger le type et renvoyer suffit.

### Firmware

- **Le journal n'est plus noyé par un échec OTA.** Une tentative ratée écrivait la **même ligne ~200 fois**, ce qui **effaçait tout l'anneau de journal** (200 entrées) — y compris les lignes qui désignaient le coupable, et y compris les traces d'un incident sans rapport en cours d'analyse. Un échec produit désormais **exactement une** ligne, et le chemin d'échec d'écriture est devenu terminal.
- **La cause réelle n'est plus écrasée.** `Update.errorString()` est capturé **avant** `Update.abort()` sur les **deux** sites d'écriture (upload manuel et téléchargement GitHub) : `abort()` remplaçait le code d'erreur réel (`MAGIC_BYTE`, `ERASE`, `WRITE`, `SPACE`…) par « Aborted », rendant toute cause indéchiffrable jusque dans le message de finalisation.
- **Pré-contrôle du type au premier octet reçu**, avant l'inhibition des pompes, avant le démontage de LittleFS et avant `Update.begin()`. **Asymétrie délibérée** : refus **dur** uniquement quand le type déclaré est « firmware » et que le premier octet n'est pas `0xE9` — ce contrôle est prouvablement équivalent à celui que la couche `Update` applique déjà, donc **l'ensemble des images acceptées est INCHANGÉ**. Dans le sens inverse (image d'allure firmware déclarée « système de fichiers »), simple **avertissement** : LittleFS n'a pas d'octet magique fiable à l'offset 0, un refus produirait des faux positifs.
- **`POST /update` — le code HTTP porte enfin le verdict** : `400` pour un refus de type ou d'empreinte, `500` pour un échec de flash, `409` inchangé pour un upload concurrent, `200` avec corps exactement `OK` pour un succès (inchangé). Jusqu'ici **tout échec était servi en `200 OK`** avec un corps `FAIL` : un client qui testait le code HTTP voyait un succès. Le **texte** de la cause est destiné à l'utilisateur et peut être reformulé sans préavis — le contrat machine est le **code HTTP**.
- **Remontage du filesystem sur échec** (`LittleFS.begin(false)`, **jamais** de formatage) : l'interface web reste servie au lieu de disparaître jusqu'au prochain redémarrage. Log `CRITICAL` si le remontage réussit **après un échec** — une partie de l'ancienne image a pu être écrasée, le contenu peut être incohérent : **relancer la mise à jour**.
- **Ordre imposé (sécurité dosage)** : `setOtaInProgress(false)` **toujours avant** tout `LittleFS.begin()`. Ces handlers tournent sur la tâche AsyncTCP, **non abonnée au watchdog** : un montage long ou paniquant y laisserait les pompes doseuses inhibées sans redémarrage possible. Le chemin de téléchargement GitHub, qui avait déjà l'ordre inverse, a été corrigé.
- **Détection par nom de fichier** : le nom **nu** `littlefs.bin` produit par `build_fs.sh` est enfin reconnu (l'ancienne règle exigeait un point devant, `.littlefs.bin` — aucun fichier réel ne correspondait).
- **`ota_update.sh`** : le test de succès `grep -q "OK"` n'était pas ancré, tout corps contenant « OK » passait pour un succès. Désormais code `200` **et** corps exactement `OK` ; le corps d'erreur est affiché.
- **Tests** : nouvelle suite `test/test_native_ota_type/` (29 cas) — **565 tests natifs**, zéro régression. `pio run` et `pio run -e esp32dev_core3` SUCCESS (flash 93,5 %), `./build_fs.sh` SUCCESS (327 680 o).

### Documentation

- `docs/API.md` : contrat de réponse de `POST /update` remplacé par le tableau des statuts (l'ancien « `200 OK` / `200 OK` corps `FAIL` » était périmé), pré-contrôle de l'octet magique, règle « le contrat machine est le code HTTP, le texte de la cause peut être reformulé ».
- `docs/subsystems/ota-manager.md` : politique « une seule erreur journalisée par session » et sa raison, remontage FS sur échec et son risque, ordre imposé dosage-avant-montage et son motif watchdog, et le **principe général de l'asymétrie de refus** (« on ne bloque en dur que là où le contrôle est prouvablement équivalent à ce que la couche sous-jacente impose déjà ; ailleurs on trace »).
- `docs/features/page-settings.md` : carte « Mise à jour manuelle » — déduction du type, notice, confirmation franchissable, affichage de la cause d'échec.
- `docs/UPDATE_GUIDE.md` : procédure manuelle actualisée + section « Lire un échec de mise à jour manuelle ».
- **Aucun ADR** : les 4 critères de déclenchement ont été confrontés explicitement et aucun n'est rempli (justification consignée dans la spec).

---

## [2.20.0] - 2026-08-15

> ⚠️ **À lire avant de saisir un offset non nul — deux effets irréversibles sur vos historiques.**
>
> 1. **Home Assistant : marche dans les courbes.** Les entités `sensor.*_ph` et `sensor.*_ph_filtered` **ne sont pas renommées** (aucune automatisation, aucun dashboard n'est cassé) mais elles **changent de valeur** le jour où vous enregistrez un offset non nul. Vos graphiques HA présenteront un **saut** de la hauteur de l'offset à cette date. C'est le comportement voulu : renommer les entités aurait détruit tout l'historique long terme pour un décalage de ±0,50 pH au maximum. Pour comparer avant/après, superposez `sensor.*_ph_raw` (toujours brut) et `sensor.*_ph`.
> 2. **Historique interne : discontinuité non réversible.** `/history.json` enregistre désormais le pH **corrigé**. Les points **antérieurs** restent en brut et ne sont **pas** recalculés — le graphique de la page pH présentera donc la même marche, définitivement. Aucune migration n'est proposée : réécrire l'historique reviendrait à inventer des mesures qui n'ont jamais été faites.
>
> 🔄 **Mise à jour du filesystem obligatoire** : l'interface change (nouvelle carte sur la page pH). Après le firmware, reconstruire et reflasher le FS : `./build_fs.sh` puis `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi). Vider le cache du navigateur si l'ancien affichage persiste.

### Fonctionnalités

- **Correction logicielle de la mesure pH** (feature-067) : nouvelle carte « **Correction de la mesure pH** » sur la page pH, avec un champ **Offset pH** au centième, de **−0,50 à +0,50** (défaut 0,00 = aucune correction). `pH corrigé = pH brut EZO + offset`. Destinée à compenser un **écart d'implantation de la sonde** (position dans le circuit, potentiel électrique de l'eau) constaté face à une mesure de référence (trousse DPD, photomètre). La carte est visible dans **tous** les modes de régulation : elle corrige la mesure, pas la régulation.
- **La calibration du module Atlas EZO n'est JAMAIS touchée.** Aucune commande `Cal,*` n'est émise : l'offset est une correction en aval de la lecture, réversible à tout moment en repassant à 0,00. Un décalage supérieur à ~0,30 pH signale d'ailleurs un problème de **sonde ou de calibration** — le message d'avertissement le dit explicitement.
- **Deux paliers d'avertissement** dans la carte, calculés **pendant la saisie** (avant d'enregistrer) : à partir de **0,15**, le système annonce le **pH réel** qu'il visera (« avec une consigne de 7,20 et cet offset, l'eau sera régulée à 7,00 ») ; à partir de **0,30**, il ajoute que la sonde devrait être recalibrée plutôt que compensée, et que le dosage d'acide sera bloqué sous 6,90.
- **Valeur refusée, jamais rabotée** : un offset hors plage est **rejeté** (erreur affichée, champ conservé tel quel), il n'est pas ramené silencieusement à la borne. Un offset écrêté serait un mensonge de mesure — vous croiriez avoir enregistré 0,80 pendant que le système appliquerait 0,50.
- **Diagnostic complet** dans le modal « État filtre pH » (clic sur le chip d'état) : brut EZO, offset appliqué, valeur corrigée, médiane corrigée, valeur filtrée corrigée — de quoi dire à tout instant ce qui vient de la sonde et ce qui vient de la correction.
- **L'écran de calibration continue d'afficher le BRUT**, exclusivement. Un repli qui pouvait y faire apparaître la valeur corrigée pendant une fenêtre de lecture périmée a été **supprimé** : on ne calibre pas une sonde sur une valeur déjà corrigée.

### Firmware

- **Point d'application : en SORTIE des accesseurs** ([ADR-0039](docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Le filtre pH (médiane 13 + EMA + rejet de pics + latch EMI) continue de travailler sur le signal **brut**. Médiane et EMA étant invariantes par translation, le résultat est mathématiquement identique — mais en sortie, un changement d'offset est pris en compte **immédiatement** au lieu d'être dilué sur ~65 s de fenêtre de lissage, et les seuils de rejet comme le latch EMI restent calés sur le signal capteur. Un test natif **démontre** `filtre(x+c) == filtre(x)+c` sur le filtre réel.
- **Sémantique des accesseurs pH — règle mnémotechnique : *seuls les `*Raw` sont bruts***. `getPh()`, `getPhMedian()`, `getPhFiltered()` renvoient la valeur **corrigée** ; `getPhRaw()` renvoie le **brut EZO** — il était auparavant un **alias pur** de `getPh()`, ce qui n'est plus vrai. Nouveau `getPhFilteredRaw()` (brut filtré), **réservé aux filets de sécurité**, assorti d'une **garde d'âge** : il renvoie NaN si le filtre n'est pas frais, faute de quoi une moyenne périmée pourrait déclencher un faux blocage — ou masquer un vrai.
- **Filet de sécurité #1 — plancher d'injection d'acide sur le pH BRUT** : sous **6,80** (ou **6,90** si \|offset\| > 0,30), **toute** injection d'acide est refusée, en régulation automatique **comme** programmée, quelle que soit la valeur corrigée. Log `CRITICAL` + alerte MQTT `ph_raw_floor`. Deux paliers parce que l'incertitude sur le pH réel croît avec l'ampleur de la correction. **Inerte à offset nul.**
- **Escalade : 3 refus de plancher en 24 h** → `CRITICAL` « sonde pH à recalibrer ou remplacer » et **bascule de la régulation pH en manuel** (persistée). Un plancher qui se déclenche à répétition signe une sonde morte ou un offset saisi à l'envers, pas un incident ponctuel.
- **Filet de sécurité #2 — inhibition du dosage chlore si le pH brut dépasse 7,60** : à ce pH la fraction d'acide hypochloreux (forme active du chlore) s'effondre, l'ORP mesuré chute à concentration constante, et la régulation ORP compenserait en **surdosant**. Le dosage est suspendu ; le correctif est de faire baisser le pH. `CRITICAL` + alerte MQTT `orp_ph_raw_inhibit`.
- **Filet de sécurité #3 — pH brut indisponible = chloration automatique suspendue** (fail-closed). Raison : `canDose(1)` ne consulte **jamais** le pH, ce filet est donc l'**unique** garde du couplage pH/chlore. Journalisé en `INFO` (situation transitoire au démarrage), pas en `CRITICAL`. Le dosage **programmé** reste aveugle à la mesure, par conception.
- **Écrire l'offset ne réinitialise aucun compteur de dosage** (cycles du jour, cumul journalier, pause anti-cycling) : sinon des enregistrements répétés permettraient de contourner les limites journalières. Un changement pendant une injection en cours ne l'interrompt pas — il prend effet au cycle suivant.
- **`POST /save-config`** : `ph_offset` hors de `[−0,50 ; +0,50]` → **HTTP 400** (aucun champ de la requête n'est appliqué). Champ absent ou non numérique → paramètre inchangé. C'est le **premier** champ de cette route à refuser plutôt qu'à écrêter — incohérence de style assumée. Chaque changement effectif est journalisé (`Offset pH modifié: ancien=… nouveau=…`).
- **Persistance NVS** (clé `ph_offset`, défaut 0,00) avec **fail-safe au boot** : une valeur relue hors bornes est neutralisée à 0,00 avec un `WARN` — l'offset ne peut jamais entrer corrompu dans la régulation.
- **MQTT — aucun renommage, aucune entité ajoutée** : `poolcontroller_ph` et `poolcontroller_ph_filtered` portent le **corrigé**, `poolcontroller_ph_raw` reste le **brut**. Le test de vivacité capteur (`phStale` de l'alerte `sensor_stale`) consomme le **brut** : un offset ne peut ni provoquer ni masquer une détection de sonde muette. **Aucune entité `ph_offset`** n'est créée — c'est un paramètre de configuration, pas une mesure.
- **Alerte `ph_abnormal` enrichie** : la détection porte sur le pH corrigé, mais le message cite **toujours** le brut et l'offset (`pH=5.30 (brut=4.90, offset=0.40)`) — sans quoi on ne peut pas distinguer une sonde à la dérive d'un offset mal saisi.
- **`GET /debug/sensor_trace`** : nouveau champ `phCorrected` **à côté** de `phRaw` et `phFiltered`, qui restent tous deux **bruts** dans cette trace de diagnostic (c'est le seul endroit où `phFiltered` n'est pas corrigé — l'objectif de l'outil est justement de séparer mesure et correction). Coût : échantillon 24 → 28 octets, buffer ≈ 8,4 Ko RAM, page de 50 ≈ 4,5 Ko.
- **Tests** : **537 tests natifs** (+30, nouvelle suite `test_native_ph_offset`), zéro régression. Flash : core 2 **83,5 %**, core 3 **93,3 %**. `build_fs.sh` OK.

### Documentation

- **Nouvel [ADR-0039](docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)** — « Offset logiciel de mesure pH appliqué en sortie des accesseurs ». **Alternatives écartées** : *appliquer à l'ingestion comme l'offset de température* (ce patron n'est **pas transposable** — la température corrigée est renvoyée à l'EZO dans `RT,<temp>` pour la compensation thermique, elle DOIT donc être corrigée tôt ; le pH n'est réinjecté nulle part) ; *modifier le filtre* (interdit, module pur [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) ; *corriger seulement à l'affichage* (le système régulerait sur une valeur invisible) ; *renommer les entités MQTT* (casserait toutes les automatisations HA) ; *publier une entité HA `ph_offset`* (point de commande sur l'entrée de la régulation chimique hors du chemin validé) ; *écrêter au lieu de refuser*. **Verrouillages** : sémantique publique des entités `ph` / `ph_raw` / `ph_filtered`, règle « seuls les `*Raw` sont bruts », pureté de `SensorFilter`, filets de sécurité qui lisent le brut, garde d'âge de `getPhFilteredRaw()`, interdiction de réinitialiser un compteur de dosage en écrivant l'offset.
- `docs/API.md` : champ `ph_offset` dans `/get-config` et `/save-config` (type, plage, **corps exact de la réponse 400**, comportement si absent ou non numérique), tableau brut/corrigé des valeurs pH de `/data`, champ `phCorrected` de `/debug/sensor_trace` et son exception à la règle `*Raw`.
- `docs/MQTT.md` : sémantique explicite de `ph` / `ph_raw` / `ph_filtered` / `ph_median`, mention explicite qu'**aucune entité n'a été renommée** et qu'aucune entité `ph_offset` n'existe, conséquence sur les historiques HA, nouvelles alertes `ph_raw_floor` et `orp_ph_raw_inhibit`, message `ph_abnormal` enrichi.
- `docs/features/page-ph.md` : nouvelle section « Carte Correction de la mesure pH » (champ, texte d'aide, **les deux paliers d'avertissement** avec les libellés exacts, enregistrement et gestion du refus serveur, modal de diagnostic), et section « L'écran de calibration reste sur le BRUT » (repli supprimé et pourquoi).
- `docs/subsystems/sensors.md` : section « Offset logiciel de mesure pH » — point d'application, **divergence assumée avec l'offset de température**, tableau du contrat des accesseurs, garde d'âge de `getPhFilteredRaw()` et sa justification, copie locale `_phOffset`, tableau des consommateurs brut/corrigé.
- `docs/subsystems/pump-controller.md` : section « Filets de sécurité sur le pH brut » — les trois filets, leur logique fail-closed, l'escalade des 3 refus, l'**interdiction de réinitialiser les compteurs**, le tableau des constantes ; note sur l'entrée corrigée du calcul d'erreur ; rappel que l'ordre de `canDose()` est inchangé.
- `docs/subsystems/ws-manager.md` : champ de config `ph_offset` (message `config`, pas `sensor_data`) et changement de valeur des champs `sensor_data` existants.

---

## [2.19.0] - 2026-08-15

> 🔎 **Cause racine des 3 crashs de production trouvée : la métrique mémoire du projet était fausse depuis toujours.** Cette version **ne corrige rien du comportement** — elle **mesure**. Le recalibrage des seuils et la bascule des gardes de sécurité arrivent en **2.19.1**, après relevés on-target. Risque de déploiement : **nul** (aucun seuil, aucune garde, aucune décision de l'appareil n'est modifiée).
>
> 🔄 **Mise à jour du filesystem nécessaire** : l'interface change (carte Infos système). Après le firmware, reconstruire et reflasher le FS : `./build_fs.sh` puis `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi). Vider le cache du navigateur si l'ancien affichage persiste.

### Firmware

- **La métrique mémoire mesurait le mauvais pool — toutes les gardes étaient aveugles** (`bug-metrique-heap-inclut-iram-32bits`, [ADR-0038](docs/adr/0038-metrique-memoire-allouable-8-bits.md)) : `ESP.getFreeHeap()` et `ESP.getMaxAllocHeap()` interrogent `MALLOC_CAP_INTERNAL`, un pool qui **englobe l'IRAM accessible en accès 32 bits alignés seulement**. Cette mémoire ne peut **pas** servir à un `malloc()`/`new` ordinaire, qui exige `MALLOC_CAP_8BIT`. Le firmware surestimait donc en permanence la mémoire disponible. Trois preuves convergentes, toutes issues de l'instrumentation livrée en 2.18.8 : le « plus gros bloc » **figé à 27 584 o pendant des jours et à travers les redémarrages** (signature d'un reliquat d'IRAM, pas d'un bloc DRAM vivant) ; des allocations de **92 à 2 872 o qui échouent réellement** pendant que l'indicateur annonce 27 Ko libres (aucune contradiction : la **famine 8 bits était réelle**) ; l'**intégrité du tas vérifiée OK toutes les 5 min**, donc **aucune corruption**. C'est la **cause racine des trois crashs** du 11/08 (`new AsyncWebServerRequest`, 356 o), du 13/08 (`new AsyncFileResponse`, 156 o) et du 15/08 (verrou UART créé paresseusement dans le chemin de log → `abort()`).
- **Conséquence la plus coûteuse : l'auto-guérison mémoire n'a JAMAIS fonctionné.** Le seuil `kRestartMaxAllocHeapBytes` = 8 192 o est comparé à un indicateur qui **ne descend jamais sous ~27 Ko** : depuis sa création ([ADR-0029](docs/adr/0029-auto-guerison-heap.md)), ce filet n'a pas pu déclencher un seul redémarrage. Sont dans le même cas les gardes de push WebSocket, le WARN heap, les deux seuils de la garde TLS ([ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)) et les deux paliers de réarmement du parachute OOM ([ADR-0035](docs/adr/0035-parachute-oom-new-handler.md)) : **9 gardes au total, toutes calibrées sur des chiffres faux**.
- **Nouveau module `heap_metrics`** (`src/heap_metrics.h` / `.cpp`) — **source unique** de la mesure : `heapFree8Bit()`, `heapLargestFreeBlock8Bit()` et `heapMetricsSnapshot()` (les quatre valeurs des deux familles en un appel) sur le pool `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, c'est-à-dire **exactement ce qu'un `malloc()`/`new` peut obtenir**. Sans log, sans allocation, sans `String` — appelable depuis les chemins critiques. Ces fonctions restent **interdites** depuis le callback d'échec d'allocation et depuis le `new_handler` (réentrance allocateur = deadlock, contrat [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md)).
- **Ce qui bascule sur la mesure 8 bits : uniquement l'observation.** L'anneau de cinétique RTC et le miroir qui alimente le post-mortem d'allocation enregistrent désormais la mémoire réellement allouable. **Aucune décision ne dépend de ces zones** — c'est purement observationnel.
- **Les deux magics de RAM RTC sont bumpés** : anneau `0xB10C4EA9` → **`0xB10C4EAA`**, post-mortem `0xF00DFA11` → **`0xF00DFA12`**. Une trame écrite par un firmware 2.18.x porte l'**ancienne** métrique : la relire avec la nouvelle grille afficherait un chiffre **faux et crédible**. **Perdre la trace du boot de bascule est acceptable, afficher un chiffre faux ne l'est pas.** Conséquence assumée : la cinétique et le post-mortem du tout premier boot en 2.19.0 sont perdus. Budget RAM RTC inchangé (1073 o sur 8192).
- **Libellés du journal suffixés pour être auto-descriptifs** : le dump de l'anneau et la ligne de post-mortem écrivent `maxB8=` / `free8=` au lieu de `maxB=` / `free=`. En relisant d'anciens logs, un `maxB=` signale sans ambiguïté un firmware **≤ 2.18.9**, donc la métrique surestimée.
- **Nouvelle trace INFO de calibrage**, émise toutes les 5 min par `checkHeapHealth()` — c'est elle qui servira à recalibrer les seuils : `Heap 8bits: libre8=… maxBloc8=… minMaxBloc8=… | interne: libre=… maxBloc=… (octets)`. Les deux familles sont données **côte à côte**, en octets bruts, pour rendre l'écart mesurable. `minMaxBloc8` (le **pire creux depuis le boot**) est mis à jour à **chaque tick** du watchdog heap (60 s, ou 5 s sous seuil d'alerte) et non toutes les 5 min : un creux transitoire entre deux traces serait sinon invisible — or c'est lui qui dimensionne le seuil de redémarrage.
- **`/get-system-info` expose trois champs ADDITIFS** : `free_heap_8bit`, `max_alloc_8bit` et `max_alloc_internal`. `free_heap` et `heap_size` gardent **exactement** leur sémantique : aucun client existant n'est cassé. Budget JSON mesuré avec `measureJson()` sur des valeurs pires cas : **716 octets sur 1024** (628 auparavant).
- **Ce que cette version NE fait PAS, volontairement** : **aucun seuil de `constants.h` n'est modifié**, **aucune des 9 gardes n'est basculée** (auto-guérison, pushes WebSocket, réarmement du parachute OOM, gardes HTTP, garde TLS). Le comportement de l'appareil est **strictement inchangé**. Basculer et recalibrer dans le même cycle exigerait de **deviner** les nouvelles valeurs — le régime 8 bits réel de l'appareil n'a **jamais** été mesuré, et l'auto-guérison est la garde qui **redémarre l'appareil** : un seuil posé à l'aveugle produirait des reboots en cascade sur un système sain qui pilote des pompes doseuses. Mesurer d'abord, décider ensuite.
- **Ce qui arrive en 2.19.1** : après **≥ 24 h de relevés on-target**, recalibrage des seuils puis bascule des gardes **dans l'ordre du risque croissant, auto-guérison en DERNIER**. Règle validée par le gate de sécurité chimique : `kRestartMaxAllocHeapBytes = clamp(0,40 × min(maxBloc8 sur 24 h) ; 8192 ; 16384)`, **débounce maintenu à 3**, invariant d'ordre `restart < rearmOom ≤ kWsPushMinAllocBytes < kLowHeapWarnBytes < min(maxBloc8 sur 24 h)`. Le gate a par ailleurs **refusé** de différer un redémarrage pendant une injection en cours : le PWM des pompes est **matériel**, une boucle figée laisserait la pompe **énergisée** — différer transformerait une dose tronquée (bénigne) en **surdosage potentiel**.
- **Ce qui reste inconnu** : **quel** consommateur épuise la mémoire 8 bits. Pistes de la phase 2 : chemin de service des fichiers statiques (`AsyncStaticWebHandler`, présent dans 2 des 3 crashs), `LittleFS.begin(false)` = **10 fichiers ouverts maximum** par défaut, fuite éventuelle de `File`. Cette recherche n'avait aucun sens tant que l'instrument mentait.

### Fonctionnalités

- **Carte « Infos système » (Paramètres > Système)** : la ligne « Heap libre » devient « **Mémoire utilisable** » et affiche la mémoire **réellement allouable**, au format `<libre> · plus gros bloc : <bloc>`. Le plus gros bloc est montré à côté du total parce que c'est **lui** qui décide si une allocation passe : un total confortable avec un bloc effondré signifie mémoire fragmentée. Une **note discrète** en dessous conserve l'ancien chiffre : « *Mesure brute : X KB, dont une zone technique inutilisable pour les allocations courantes.* » — l'écart entre les deux est **normal** et volontairement rendu visible. La note ne s'affiche que si les deux mesures sont disponibles ; sur un firmware antérieur à 2.19.0, la ligne retombe silencieusement sur la mesure brute (aucun `undefined`).

### Documentation

- **Nouvel [ADR-0038](docs/adr/0038-metrique-memoire-allouable-8-bits.md)** — « La mémoire se mesure sur le pool 8 bits réellement allouable, jamais sur `MALLOC_CAP_INTERNAL` ». **Alternatives écartées** : *garder `MALLOC_CAP_INTERNAL` et relever les seuils* (c'est exactement ce qui a été fait en 2.18.5/2.18.6/2.18.8 — **et c'est ce qui a produit trois crashs** : le problème n'est pas la valeur du seuil, c'est la grandeur comparée) ; *n'exposer que la valeur 8 bits* (l'**écart entre les deux familles EST l'information** — sans lui affiché, un futur cycle rebranchera innocemment `ESP.getFreeHeap()` et le piège se refermera) ; *basculer et recalibrer dans la même version* (exigerait de deviner les seuils d'une garde qui redémarre l'appareil) ; *champ « version de format » plutôt que bump de magic* (une branche de lecture rétrocompatible pour une trame relue **une seule fois** dans la vie du produit). **Verrouillages** : interdiction permanente d'utiliser `ESP.get*Heap()` dans une garde, `heap_metrics` source unique, extension de l'interdiction réentrante au helper, magics RTC réservés (anciens compris), tout changement de métrique d'une zone RTC impose un bump de magic, invariant d'ordre des 5 seuils, et **jamais de report d'un redémarrage d'auto-guérison pendant une injection**.
- **Notes additives datées (2026-08-15)** dans [ADR-0029](docs/adr/0029-auto-guerison-heap.md) (le filet d'auto-guérison n'a jamais pu se déclencher — le mécanisme reste valide, c'est la grandeur mesurée qui était fausse), [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md) (les paliers de réarmement 24 Ko / 12 Ko ont été calibrés sur des valeurs terrain surestimées), [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) (les deux seuils heap de la garde TLS sont à refaire — ce qui **renforce** le critère PSRAM fail-closed) et [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md) (le verdict `SUSPICION CORRUPTION` du 13/08 était faux : la comptabilité n'était pas « saine », elle était fausse — la règle, inchangée, redevient exploitable). **Aucun de ces ADR n'est réécrit.**
- `docs/subsystems/oom-guard.md` : avertissement en tête sur l'**état transitoire assumé** de la 2.19.0 (l'anneau mesure en 8 bits pendant que les seuils décident encore sur la mesure brute — c'est volontaire, pas un oubli), bump des deux magics et sa justification, libellés `maxB8=` / `free8=`, verdict du 13/08 corrigé, les 5 seuils marqués « à refaire en 2.19.1 » avec la règle de calibrage.
- `docs/subsystems/system-health.md` : nouvelle section « Trace de calibrage des seuils mémoire » (format exact, tableau des 4 champs, pourquoi `minMaxBloc8` est échantillonné à chaque tick et pas toutes les 5 min, **ce qu'il faut relever on-target**), avertissement clair sur le fait que l'auto-guérison **n'est pas encore basculée**, budget RAM RTC (inchangé, magics bumpés) et règle « changement de métrique ⇒ bump de magic ».
- `docs/API.md` : les 3 champs additifs de `/get-system-info`, tableau des deux familles de mesures, pourquoi les deux sont publiées, consigne d'intégration (superviser `max_alloc_8bit`, jamais `free_heap`), compatibilité ascendante et budget JSON.
- `docs/features/page-settings.md` : ligne « Mémoire utilisable », note sur la mesure brute, affichage conditionnel et repli sur firmware antérieur.

---

## [2.18.9] - 2026-08-14

> 🔄 **Mise à jour du filesystem obligatoire** : ce cycle ne modifie que l'interface web (`data/app.js`). Mettre à jour le **firmware seul ne change rien** — il faut reconstruire et reflasher le filesystem : `./build_fs.sh` puis `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi). Pensez à vider le cache du navigateur si l'ancien affichage persiste.

### Fonctionnalités

- **Mini-graphiques de l'accueil limités à la journée en cours** (`ux-minicharts-journee-en-cours`, demande utilisateur) : les courbes pH, ORP et Température des cartes du tableau de bord affichaient une fenêtre **glissante de 24 h**, donc une partie de la veille en tête de courbe — difficile de lire « où en est-on aujourd'hui ». Elles ne montrent désormais que les points postérieurs à **minuit local**. Le filtrage est fait **côté navigateur** : la requête reste `GET /get-history?range=24h`, **aucun changement de contrat HTTP ni de firmware** ([docs/API.md](docs/API.md) inchangé).
- **Passage de minuit géré tout seul** : le jour civil affiché est mémorisé ; au premier rafraîchissement suivant le changement de date (cadence 5 min), le polling incrémental (`?since=`) laisse place à un **rechargement complet** qui remplace la série. Les points de la veille disparaissent **sans action de l'utilisateur ni rechargement de la page**.
- **Conséquence assumée** : en début de journée les courbes sont **courtes, voire vides entre minuit et le premier relevé**. C'est le comportement demandé, pas un défaut — le repli d'échelle existant (cadre cible ± tolérance) garde un graphique lisible tant qu'aucun point n'est disponible.
- **Inchangé** : les graphiques des écrans détail pH / ORP / Température conservent leurs plages (`Tout` / `30j` / `7j` / `24h`) ; l'infobulle reste **datée et typée** (`jj/mm hh:mm` + mesure / moyenne horaire / moyenne journalière, acquis v2.18.4) ; les **coupures de courbe** aux arrêts de filtration (> 90 min) fonctionnent à l'identique — le filtrage à minuit est appliqué **avant** leur calcul, pour qu'aucun faux trou n'apparaisse en tête de journée.

### Documentation

- `docs/features/page-dashboard.md` : nouvelle section « Plage des mini-graphiques : journée en cours », mécanisme de passage de minuit, cas « début de journée », rappel du reflash du filesystem ; mentions résiduelles des « 24 dernières heures » corrigées.

---

## [2.18.8] - 2026-08-14

> ⚠️ **À lire honnêtement** : ce cycle **n'explique pas** le crash du 13/08. Il corrige deux défauts de l'instrumentation — dont le fait que **le parachute OOM livré en 2.18.5 était inopérant après son premier déclenchement** — et met en place de quoi **trancher** la cause à la prochaine occurrence. La cause racine reste **inconnue**.

### Firmware

- **Le parachute OOM ne se réarmait JAMAIS** (`bug-parachute-oom-jamais-rearme-et-postmortem`, [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md)) : 2ᵉ crash de production le 13/08 (v2.18.7, uptime 12,9 h) — abort dans `async_tcp` sur `new AsyncFileResponse` (**156 o**) en servant un fichier statique, puis échec de l'allocation de l'objet exception (**100 o**) → `std::terminate`. Pour que libstdc++ lève, il faut que le `new_handler` se soit **désenregistré** : le réarmement exigeait `maxBloc ≥ 32 768 o` alors que le plus gros bloc **réel** de l'appareil vaut **29 684 o à 33 min** et **27 584 o à 12,9 h** — un seuil **au-dessus du régime normal**, donc une réserve jamais ré-allouée. La protection livrée en 2.18.5 était **one-shot en pratique** ; c'est corrigé : `kOomRearmMinMaxAllocBytes` = **24 576 o** (= 2 × la réserve), plus un **palier de repli** de **2 048 o** armé dès 12 288 o de plus gros bloc (« mieux vaut un petit parachute qu'aucun » : 2 Ko couvrent 8× le duo fautif 156 + 100 o). Surclassement repli → nominal autorisé, **déclassement interdit**. Échelle complète des seuils désormais documentée : 8 192 (auto-guérison) < 12 288 (réarmement repli) < 16 384 (push WS) < 20 000 (WARN heap) < 24 576 (réarmement nominal).
- **Post-mortem d'allocation en RAM RTC** : le CRITICAL « Réserve OOM consommée » étant émis **en différé** par la loop, il est **perdu** quand l'abort suit immédiatement — comme le 13/08. Une **3ᵉ zone RTC noinit** (magic `0xF00DFA11`, **32 octets**) est désormais écrite **par les chemins d'échec eux-mêmes**, en stores bruts alignés : taille du 1ᵉʳ et du dernier échec, `caps`, compteur, uptimes, état du tas comptabilisé, drapeaux, épisodes de réserve. Elle est **dumpée au boot suivant en une ligne unique**, juste après le dump de l'anneau, avec un **verdict** (`FAMINE MÉMOIRE` / `SUSPICION CORRUPTION` / `INDÉTERMINÉ`). Le **drapeau « réserve consommée » est l'octet `flags`** de cette zone : il survit à un abort immédiat. Contrainte structurante : depuis le callback `heap_caps_register_failed_alloc_callback`, **tout appel `heap_caps_*` est interdit** (le verrou du composant heap est déjà détenu par l'allocateur → deadlock) — d'où le **miroir** du dernier échantillon de l'anneau comme seule source « free / maxBloc », à cadence **inchangée** de 2 s et sans aucun parcours de tas supplémentaire. Verdict : famine si plus de 8 échecs **en rafale** (span ≤ 60 s — fenêtre ajoutée en revue, sans elle quelques échecs bénins étalés sur des heures auraient conclu « famine » sur le scénario même à trancher), suspicion de corruption si la comptabilité reste saine (plus gros bloc ≥ 4 × la taille demandée ; le 13/08 : **176×**). Limites assumées et écrites : trace **indicative** (compteurs sans verrou), post-mortem du boot n-2 perdu si un crash survient pendant `setup()`, uptimes tronqués sur 16 bits (repliement toutes les 18 h 12, durées calculées modulo — la validité de la zone ne dépend **pas** de l'ordre des uptimes, un défaut trouvé en revue qui rendait le dump muet au-delà de 18 h).
- **Contrôle d'intégrité périodique du tas** : `heap_caps_check_integrity_all()` toutes les **5 min**, durée attendue **1-3 ms** (framework compilé en `CONFIG_HEAP_POISONING_LIGHT` : parcours des blocs, pas de scan octet par octet), **mesurée à chaque passage**, 1ʳᵉ durée journalisée en INFO, WARN au-delà de 5 ms, **désactivation définitive** au-delà de 20 ms. Détecteur **direct** de l'hypothèse de corruption.
- **Sécurité chimique du contrôle d'intégrité** (gate `pool-chemistry`) : appelé **après** `PumpController.update()` et **jamais** si l'une des **8 gardes** n'est pas remplie — mode maintenance, OTA, dosage pH, dosage ORP, injection manuelle pH, injection manuelle ORP, **pompe énergisée**, uptime minimal. La garde « pompe énergisée » s'appuie sur un **nouvel accesseur en lecture seule** `PumpController::isAnyPumpEnergized()` fondé sur les rapports cycliques réels : les indicateurs de dosage ne couvraient **que la régulation automatique**, une injection manuelle ou un test de pompe passait au travers. Retard maximal admissible établi par `pool-chemistry` : **65 ms** (0,1 mL à 90 mL/min) ; plafond retenu **20 ms** = 0,03 mL. Corruption détectée → `stopAll()` + inhibition **en tête**, puis redémarrage propre via `performFailSafeRestart()` (alerte MQTT `heap_corruption`) ou mode maintenance si le plafond de redémarrages est atteint — **jamais d'`ESP.restart()` nu**.
- **MQTT** : nouvelle alerte `heap_corruption` sur `{base}/alerts` (**type unique** publié par la branche redémarrage propre **et** par la branche mode maintenance).
- **Chiffres** : **507 tests natifs** (+41), builds bi-core verts — core 2 flash 83,2 % / RAM 20,0 %, core 3 flash 93,0 % / RAM 21,1 %. Budget RAM RTC noinit du projet : **1073 o sur 8192** (auto-guérison 9 + anneau heap 1032 + post-mortem 32).

### Documentation

- **Nouvel [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md)** — post-mortem d'allocation en RAM RTC + contrôle d'intégrité périodique du tas. Alternatives écartées : **échantillonner le tas toutes les 200-500 ms** (rejetée — `heap_caps_get_largest_free_block` s'exécute interruptions coupées et injecterait de la gigue dans le phénomène mesuré, pour zéro pouvoir discriminant) ; **poisoning comprehensive** (coût prohibitif sur chaque `malloc`/`free`) ; **`ESP.restart()` nu** (viole la sécurité chimique) ; persistance du post-mortem en LittleFS/NVS (impossible depuis le chemin d'échec). Verrouillages documentés : échelle de 5 seuils interdépendants, 3ᵉ zone RTC, interdiction réentrante `heap_caps_*`, subordination du contrôle d'intégrité à la sécurité chimique, contrat MQTT `heap_corruption`. **Note additive datée** dans [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md) : le contrat du parachute change (réarmement recalibré, paliers, post-mortem) — la décision d'origine n'est pas réécrite.
- `docs/subsystems/oom-guard.md` : seuil recalibré **avec les mesures qui le justifient** et l'explication du défaut v2.18.5, palier de repli et règles de transition, post-mortem complet (format 32 o, champs, drapeaux, règle exacte du verdict, ligne de dump), contrainte « aucun `heap_caps_*` depuis le chemin d'échec » et miroir qui en découle, les 3 limites assumées, 66 tests dédiés / 507 au total.
- `docs/subsystems/system-health.md` : contrôle d'intégrité (cadence, durée, autolimitation, les 8 gardes chimiques, action sur échec), **budget RAM RTC noinit consolidé des 3 zones**, second dump au boot à côté de celui de l'anneau, alerte `heap_corruption`, constantes `kHeapIntegrity*`.
- `docs/MQTT.md` : alerte `heap_corruption` (condition, messages exacts des deux branches, non-retained, best-effort).

---

## [2.18.7] - 2026-08-13

> Amélioration d'expérience demandée par l'utilisateur après essai réel de la 2.18.6 : le refus fonctionnait, mais il se présentait mal. Aucun changement de la garde mémoire, du contrat HTTP ni des seuils.

### Fonctionnalités

- **Message d'indisponibilité de la mise à jour en ligne reformulé** (`ux-message-psram-mise-a-jour-en-ligne`) : le refus s'affichait « **Erreur vérification mise à jour :** Mise à jour en ligne indisponible : ce module n'a pas de PSRAM (connexion sécurisée GitHub impossible). Utilisez la mise à jour par upload de fichier. » Or ce n'est **pas une erreur** mais une caractéristique du matériel, « upload de fichier » n'est pas le vocabulaire de l'interface (qui dit « **Mise à jour manuelle** ») et la mention TLS/PSRAM n'indiquait aucune action. Nouveau texte, identique sur les deux routes : « La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à jour manuelle. » Il est affiché **tel quel, sans préfixe**, en toast **informatif** (et non rouge ou orange : rien n'est corrigeable, une tonalité d'échec suggérerait à tort une action). Même traitement sur le bouton d'installation.
- **Indisponibilité annoncée avant le clic** : à l'ouverture de Paramètres > Système, l'interface lit la PSRAM du module ; si elle est absente, le bouton « Vérifier » est **grisé** et une explication permanente s'affiche dans la carte « Mise à jour en ligne (GitHub) », renvoyant vers la carte « Mise à jour manuelle ». Plus besoin de déclencher un refus pour savoir que la méthode n'est pas disponible — le toast ne subsiste qu'en filet de sécurité. Neutralisation **uniquement** si la PSRAM est explicitement annoncée absente (`psram_size = 0`) : si le champ manque, l'interface ne change rien (compatibilité ascendante).

### Firmware

- **Texte du 503 `psram_required` remplacé dans le helper partagé** — donc sur `/check-update` **et** `/download-update`. Le statut **503** et le champ `"code":"psram_required"` sont **inchangés** : le contrat machine est intact, seul le texte destiné à l'utilisateur change. Les logs WARN/INFO restent volontairement techniques (diagnostic on-target). Aucune modification de la garde, des seuils ni de la logique de refus ([ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) inchangé sur le fond).
- **Trois codes machine de refus au lieu d'un seul** (correctif de revue) : `psram_required` est désormais réservé à la PSRAM **réellement absente** (`ESP.getFreePsram() == 0`, indisponibilité **permanente**) ; une PSRAM installée mais **saturée** renvoie le nouveau `psram_low` (« La mise à jour en ligne est momentanément indisponible : la mémoire PSRAM est saturée. Réessayez dans quelques minutes ou utilisez la mise à jour manuelle. », **réessayable**) et le refus heap dégradé porte le nouveau champ `"code":"tls_low_memory"` (texte inchangé : « Mémoire insuffisante pour TLS, réessayez »). Le firmware n'affirme donc plus l'absence de PSRAM sur un module qui en a. La discrimination est faite **dans la route** par un simple test de nullité de la mesure déjà en main : `oomTlsGateReason()`, les seuils, l'ordre de la garde et le parachute OOM sont **intacts**.
- **Côté interface**, le champ `code` de la réponse est désormais propagé sur l'objet `Error` (il était perdu, ce qui empêchait de distinguer une indisponibilité matérielle d'une panne) et le front **ne devine plus rien dans le texte du message** : il lit `code`. Conséquence utilisateur : seul `psram_required` neutralise le bouton et affiche le bandeau permanent ; `psram_low` et `tls_low_memory` affichent un toast informatif **sans** neutraliser (on peut réessayer) puis rafraîchissent l'état via `/get-system-info`, qui reste la source de vérité. Le bandeau `#online_update_unavailable` porte `role="status"` / `aria-live="polite"`. **466 tests natifs**, builds bi-core verts (flash 82,9 % core 2 / 92,7 % core 3), `./build_fs.sh` OK.

### Documentation

- `docs/API.md`, `docs/UPDATE_GUIDE.md`, `docs/subsystems/oom-guard.md` : nouveau texte du 503 partout où il est cité au caractère près, puis **les trois refus 503** (`psram_required` / `psram_low` / `tls_low_memory`) — condition, caractère permanent ou temporaire, message et effet UI — sur `/check-update` **comme** `/download-update`. `oom-guard.md` documente le mapping motif → code (la scission de `kOomTlsGateNoPsram` est faite par la **route**, pas par le prédicat pur). Précision maintenue : le champ `error` est un **texte utilisateur reformulable**, les clients doivent se fier au champ `code`. `UPDATE_GUIDE.md` distingue pour l'utilisateur les trois situations (matériel sans PSRAM = définitif ; PSRAM saturée et mémoire interne dégradée = réessayer) avec le même repli manuel.
- `docs/features/page-settings.md` : état « mise à jour en ligne indisponible » de la carte GitHub (avertissement permanent, boutons désactivés, condition `psram_size === 0`, renvoi vers la mise à jour manuelle, `role="status"` / `aria-live`), plus le tableau des trois codes de refus et de leur effet UI — seul `psram_required` neutralise. `docs/features/README.md` : convention `showToast` — le type `info` sert aussi aux indisponibilités matérielles, message affiché sans préfixe.
- [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) : **deux notes additives datées** (présentation du refus, puis scission en trois codes machine) — la décision technique n'est pas réécrite.

---

## [2.18.6] - 2026-08-13

### Firmware

- **Mise à jour en ligne refusée explicitement sans PSRAM** (`bug-garde-tls-trop-permissive-sans-psram`, [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)) : la garde bicritère de la v2.18.5 laissait passer le régime **normal** d'un WROOM (mesure de production du 12/08 : heap libre=87 708 o, plus gros bloc=34 804 o) ; le handshake TLS échouait ensuite **après** avoir affamé le tas — une allocation de **100 octets** refusée alors que 87 Ko restaient libres, c'est-à-dire la fenêtre exacte du crash `bad_alloc` du 11/08. La garde exige désormais **1 Mo de PSRAM LIBRE** (`kOtaTlsMinPsramBytes`), évaluée **en premier** et fail-closed, avant le bicritère heap (seuils **inchangés**). Le critère porte sur la PSRAM *libre* (`ESP.getFreePsram()`) et non sur la taille installée : une PSRAM saturée ferait retomber mbedTLS sur la RAM interne. Réponse **503** au message lisible : « Mise à jour en ligne indisponible : ce module n'a pas de PSRAM (connexion sécurisée GitHub impossible). Utilisez la mise à jour par upload de fichier. » (`code: "psram_required"`) — au lieu d'un « Erreur HTTP GitHub: -1 » opaque une seconde plus tard. Repli inchangé et pleinement fonctionnel : upload de fichier ou `deploy.sh ota-firmware`.
- **`POST /download-update` protégé lui aussi** : cette route ouvrait la **même** session TLS **sans aucune garde** — un POST direct contournait celle de `/check-update`, avec un flash à la clé. Elle applique désormais le même prédicat, placé **après** la validation url/allowlist/digest et **avant** `WiFiClientSecure`, `setOtaInProgress()`, `LittleFS.end()` et tout effacement de partition : un refus n'a **aucun effet de bord** (pompes non gelées, filesystem non démonté, rien d'effacé).
- **Prédicat partagé à motifs** : `oomTlsGateOk` (booléen) devient `oomTlsGateReason()` → `kOomTlsGateOk` / `kOomTlsGateNoPsram` (refus **définitif**, matériel) / `kOomTlsGateLowMemory` (refus **temporaire**, réessayable) — logique pure, aucune comparaison de seuil dans les handlers. Les logs INFO nominal et WARN de refus mentionnent la **PSRAM libre**, les deux mesures heap, leurs seuils et l'IP client. **466 tests natifs** (+7), builds bi-core verts (flash 82,9 % core 2 / 92,7 % core 3).

### Fonctionnalités

- **Toasts longs enfin lisibles** : un message long (comme le refus de mise à jour ci-dessus, ~220 caractères) débordait du viewport sur une seule ligne. Les toasts reviennent désormais à la ligne (`max-width` 92 vw / 560 px), et leur durée d'affichage s'adapte à la longueur du texte (40 ms/caractère, plancher **3 s** = comportement historique préservé pour les messages courts, plafond 9 s).
- **Toasts d'avertissement visibles** : quatre messages étaient émis en type `warning` (calibrations pH/ORP sans retour, notamment) alors qu'aucun style ne correspondait — ils s'affichaient **transparents/illisibles**. Style orange dédié ajouté.
- **Plus de faux « ✓ Mise à jour terminée »** (défaut pré-existant, rendu atteignable par le 503 ci-dessus) : à l'étape firmware d'une installation depuis GitHub, l'interface traitait TOUTE erreur comme « l'ESP32 a redémarré, donc le flash a réussi ». Un refus explicite du contrôleur affichait donc un succès alors que le firmware n'était pas installé — avec, si l'étape filesystem avait réussi, un filesystem neuf sur un firmware ancien à l'insu de l'utilisateur. L'interface distingue désormais une **réponse d'erreur du serveur** (affichée telle quelle) d'une **vraie coupure réseau** (seul cas où le redémarrage vaut succès).

### Documentation

- **Nouvel [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)** — PSRAM libre requise pour toute session TLS sortante (alternatives écartées : relever le seuil maxBloc à 48-50 Ko, illusoire car la réserve du parachute fait tomber le maxBloc nominal WROOM de ~49 à ~37 Ko ; laisser tenter et rattraper ; garder `/download-update` sans garde). Notes additives datées dans [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md) et [ADR-0032](docs/adr/0032-reponses-http-bornees.md) — décisions d'origine non réécrites.
- `docs/API.md` : 503 `psram_required` de `/check-update` **et** de `/download-update` (jamais documenté jusqu'ici), corps exacts, logs, position de la garde ; la note « limitation connue — core Arduino 3 uniquement » est **rectifiée** (l'attribution réelle, mesurée en production, est l'absence de PSRAM, pas le core Arduino).
- `docs/UPDATE_GUIDE.md` : la mise à jour en ligne est **activement refusée** sans PSRAM sur les deux étapes (message exact + repli) — l'annonce « 503 Mémoire insuffisante pour TLS » était devenue fausse.
- `docs/subsystems/oom-guard.md` : prédicat à motifs, critère PSRAM **libre**, `kOtaTlsMinPsramBytes`, 466 tests.
- `docs/features/README.md` : composant transverse `showToast` documenté (types, durée adaptative, retour à la ligne).

---

## [2.18.5] - 2026-08-12

### Firmware

- **Parachute OOM global** (`bug-oom-accept-http-durcissement`, [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md)) : crash de production le 11/08 au soir — `std::bad_alloc` non rattrapé dans `async_tcp` (`new AsyncWebServerRequest`, 356 o, à l'**acceptation d'une connexion HTTP**) après un effondrement du heap de ~60 Ko en ~5 s. L'investigation mesurée a **réfuté** les hypothèses save historique / clients TCP gelés / rafale de page ; la cause du pic reste **non attribuée** (candidat restant documenté dans la spec : buffers RX du driver WiFi en tempête/réassociation — corrèle avec le socket MQTT gelé au crash). Le durcissement est donc **trigger-agnostique** : réserve d'urgence de 12 Ko pré-allouée au plus tôt du boot + `std::set_new_handler` qui la libère à la première famine (le `new` en cours est re-tenté et **réussit** au lieu d'aborter), log CRITICAL différé one-shot par épisode, réarmement à hystérésis (maxBloc ≥ 32 Ko, tick 10 s, critère pointeur — course handler/réarmement fermée en revue), callback IDF diagnostic flag-seul (compte les échecs `malloc` C, WARN throttlé 30 s). Réserve épuisée → comportement d'avant (bad_alloc, coredump en filet ultime) ; le containment de fond reste l'auto-guérison [ADR-0029](docs/adr/0029-auto-guerison-heap.md). Voir [docs/subsystems/oom-guard.md](docs/subsystems/oom-guard.md).
- **Garde mémoire sur `/check-update`** : refus **503** `{"error":"Mémoire insuffisante pour TLS, réessayez"}` AVANT la session TLS (~58 Ko) si heap libre < 65 536 o OU plus gros bloc < 20 480 o — remplace l'échec lent et aléatoire en plein handshake. Le WARN de refus et le log INFO nominal incluent désormais l'**IP du client** (le crash du 11/08 avait un profil TLS compatible mais l'appelant était invérifiable). Note : seuils conservateurs sur module WROVER/PSRAM (buffers mbedTLS > 4 Ko en PSRAM), à réévaluer.
- **Dump du ring heap enrichi** : le WARN d'en-tête « Cinétique heap avant reset » inclut l'uptime absolu du dernier échantillon (mod 65 536 s) et la valeur brute d'`esp_reset_reason` — corrélation temporelle a posteriori avec journal et coredumps.
- **Purge des fichiers ressuscités par OneDrive** : `src/uart_{commands,protocol,transport}.{cpp,h}`, `src/debug_tls_verify.h`, `tools/uart_test.py` (retirés en v2.17.0, réapparus par sync) — le linker les élaguait déjà (`--gc-sections`) ; delta flash net du durcissement ≈ +3,1 Ko. Builds bi-core verts (flash 82,9 % core 2 / 92,6 % core 3), **459 tests natifs** (+18, `test_native_oom_guard`).

### Documentation

- Nouveau [docs/subsystems/oom-guard.md](docs/subsystems/oom-guard.md) : réserve, contrat du handler, réarmement, limites.
- **Nouvel [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md)** — parachute OOM hybride (comportement global d'allocation invisible au point d'appel ; callback IDF seul écarté comme mécanisme principal, retenu en complément flag-seul ; les seuils de garde dépendent de la réserve).
- `docs/API.md` : 503 bicritère de `/check-update` + IP client + note WROVER/PSRAM.
- `docs/subsystems/system-health.md` : ligne de dump du ring enrichie ; renvoi vers oom-guard.

---

## [2.18.4] - 2026-08-10

### Firmware

- **Plus d'agrégat fantôme daté du futur dans l'historique** (`bug-points-futurs-et-ux-courbes`, constaté en production le 10/08 : un point « moyenne journalière » daté du jour courant — normalement impossible — polluait le bord droit des courbes). Cause : des points horodatés dans le **futur** (excursion d'horloge pendant les reboots des essais des 8-9/08) paraissaient vieux de ~136 ans par wrap `uint32` du prédicat d'âge → consolidation immédiate en agrégat journalier daté du futur. Deux gardes : les prédicats d'âge deviennent **future-safe** (`historyIsExpired` pour la purge 90 j, `historyBucketConsolidable` durcie — un point/créneau futur n'est plus jamais « ancien » ; logique pure, l'invariant de wrap d'`isOlderThan` reste inchangé) ; au chargement de `/history.json`, les points horodatés au-delà de `now + 1 h` (`kHistoryFutureToleranceSeconds`) sont **purgés** quand l'horloge est disponible (log INFO du nombre purgé) — le point fantôme disparaît au premier boot. Builds bi-core + 441 tests natifs (+5) verts.
- **Widget Éclairage de l'accueil** : la ligne « Horaires » est masquée quand la programmation est désactivée (elle affichait des horaires inertes sous un mode « Programmation désactivée »).

### Fonctionnalités

- **Tooltip des courbes toujours daté et typé** : le survol d'un point affiche désormais systématiquement la date ET l'heure (`jj/mm hh:mm` — plus d'heure seule pour « aujourd'hui ») ainsi que le type de point (« mesure », « moyenne horaire », « moyenne journalière ») — mini-graphiques du tableau de bord ET écrans détail. Lève l'ambiguïté des étiquettes courtes de l'axe X (inchangées).
- **Mini-graphiques de l'accueil sur 24 h** (au lieu de 3 jours) : lecture directe de la journée écoulée ; les écrans détail conservent leurs plages (`24h`/`7j`/`30j`/`Tout`).
- **Les arrêts de filtration se voient sur les courbes** : plus de 90 min entre deux points consécutifs → la courbe est coupée (trou), y compris lors du rafraîchissement incrémental des mini-graphiques ; sur les écrans détail (1 point/jour), un jour calendaire entièrement absent crée le même trou.

### Documentation

- `docs/subsystems/history.md` : garde anti-futur des prédicats d'âge + purge des points futurs au chargement documentées.
- `docs/features/page-dashboard.md` : mini-graphiques 24 h, tooltip daté/typé, coupures de courbe aux arrêts.
- Pas de nouvel ADR (correctif de robustesse + UX, aucune décision structurante).

---

## [2.18.3] - 2026-08-09

### Fonctionnalités

- **Les graphiques s'affichent correctement sur mobile** (`bug-minicharts-ecretes-mobile`) : sur iPhone, les mini-graphiques du dashboard et les graphiques des écrans détail étaient étirés puis écrêtés en bas de leur carte (axe Y collé au bord). Cause : une règle CSS morte héritée de Chart.js (`canvas { height: 240px !important }` sous 980 px) écrasait la hauteur posée par uPlot — d'où le rendu parfait sur ordinateur. Règles mortes supprimées et `ResizeObserver` existant durci (debounce 100 ms, garde de dimensions, re-synchronisation initiale ; la rotation portrait/paysage est couverte). Frontend uniquement (`data/app.css`, `data/app.js`), aucun changement firmware.

### Documentation

- `docs/features/page-dashboard.md` : comportement responsive des graphiques uPlot documenté (ResizeObserver partagé, interdiction de dimensionner les canvas uPlot en CSS).
- Pas de nouvel ADR (bug d'affichage pur, aucune décision structurante).

---

## [2.18.2] - 2026-08-09

### Firmware

- **Les graphiques s'affichent réellement cette fois** (suite et **fin** de `bug-history-doublons-troncature`). La vraie cause racine de la troncature de `/get-history` — démontrée au flux chunked brut sur l'appareil — est une **erreur de machine à états présente depuis la v2.14.0** (déployée le 05/08) : le cas `kFooter` chargeait le pied `],"count":N}` dans le tampon de report ET passait en phase `kDone` dans la **même itération**, or la condition de boucle sortait avant d'émettre le tampon → **le pied n'était JAMAIS envoyé** → JSON invalide sur **toutes** les réponses → graphiques morts depuis le déploiement v2.14. Fix : la boucle continue tant que le tampon de report n'est pas vide, **même en `kDone`**. Validé sur l'appareil (pied présent, JSON valide).
- **Refactor de l'export `/get-history`** : instantané filtré copié en **une seule** prise de mutex à l'ouverture de la requête (`snapshotFiltered`, timeout `kHistorySnapshotTimeoutMs` = 100 ms ; refus rare → **`503` `{"error":"historique occupé, réessayez"}`**), puis streaming **sans aucun mutex**. Le mécanisme timeout/espaces/`RESPONSE_TRY_AGAIN` introduit en v2.18.1 est **définitivement supprimé** (`getPointByIndex`, `kHistoryPointReadTimeoutMs`, `kHistoryChunkMaxRetries` retirés) — il traitait un symptôme mal diagnostiqué (cf. note sur l'entrée [2.18.1] ci-dessous). Coût borné : ~11 Ko transitoires, libérés en fin de réponse ; plus aucun glissement de buffer possible (instantané figé). Trace de clôture conservée en **DEBUG** (`get-history clos: emis=… o, points=…`). Builds bi-core + 436 tests natifs verts.

### Documentation

- Note corrective ajoutée sur l'entrée **[2.18.1]** ci-dessous : sa « cause B » (espaces de maintien de flux) était un diagnostic erroné.
- `docs/subsystems/history.md` : section export chunked réécrite (`snapshotFiltered`, cause racine C documentée) ; encadré **dette** — extraire la machine à états chunked en logique pure testable (ADR-0017) avec test « le flux se termine par le pied », dont l'absence a laissé passer le bug 5 jours et 2 revues.
- `docs/API.md` : `/get-history` — note « espaces sous contention » retirée (fausse), `503` rare documenté.
- Pas de nouvel ADR (application/correction de mécanismes existants).

---

## [2.18.1] - 2026-08-09

> ⚠️ **Note corrective (v2.18.2)** : le second diagnostic de cette entrée (« Réponse `/get-history` tronquée sous contention » / `RESPONSE_TRY_AGAIN` / espaces de maintien de flux) était **erroné** — la troncature venait d'un pied JSON jamais émis par la machine à états (cause racine corrigée en 2.18.2, présente depuis v2.14.0). Le mécanisme espaces/retries a été **retiré** en 2.18.2. Les deux autres corrections de cette entrée (doublons d'agrégats, capacité RAW) restent valides.

### Firmware

- **Les graphiques (dashboard, écrans pH/ORP) s'affichent à nouveau** (bug `bug-history-doublons-troncature`, constaté en production les 08-09/08 : `/get-history` renvoyait un JSON tronqué → `SyntaxError` côté navigateur → courbes vides). Deux causes racines corrigées :
  - **Agrégats horaires/journaliers dupliqués** : la consolidation créait un agrégat partiel à chaque passage pour le même créneau (jusqu'à 12 doublons/heure au même timestamp) — elle ne convertit désormais un créneau que lorsqu'il est **complet** (un seul agrégat par heure/jour, de meilleure qualité), et les agrégats dupliqués d'un même créneau sont **fusionnés** à la consolidation, au chargement et à l'import.
  - **Réponse `/get-history` tronquée sous contention** : sur timeout du mutex history sans octet en vol, le callback chunked renvoyait `RESPONSE_TRY_AGAIN` que la lib ne re-sonde qu'à l'ack TCP suivant — sans rien en vol, silence → timeout TCP → JSON tronqué sans pied ; il émet désormais **un espace** (légal entre deux éléments JSON) qui maintient le flux vivant, la borne `kHistoryChunkMaxRetries` (clôture propre avec pied) est conservée.
- **Assainissement automatique au premier boot** : les doublons déjà persistés par les versions ≤ 2.18.0 sont fusionnés au chargement de `/history.json` (moyennes équipondérées, **sans effacement de l'historique**) — log INFO `Historique : N agrégats dupliqués fusionnés`.
- **Capacité RAW étendue à 8 h** : `kMaxRawDataPoints` 72 → 96 — nécessaire pour que les points survivent jusqu'à la complétude de leur créneau (~7 h). Budget partition history ([ADR-0033](docs/adr/0033-budget-espace-partition-history.md)) : pire cas 13 → 14 blocs / 16, garde `historySaveAdmit` inchangée. Builds bi-core verts, 436 tests natifs (+8).

### Documentation

- `docs/adr/0033-budget-espace-partition-history.md` : note additive — budget pire cas recalculé 14/16 blocs (`kMaxRawDataPoints` 96). Pas de nouvel ADR (application de mécanismes existants).
- `docs/subsystems/history.md` : consolidation par créneau complet, déduplication (3 points d'appel), capacité RAW 8 h ; **suivi connu** documenté — les agrégats journaliers ne se forment jamais (rétention horaire 7 j < âge de consolidation 15 j, affamage préexistant hors lot).
- `docs/API.md` : des espaces peuvent apparaître dans le flux `/get-history` sous contention (maintien de flux, sans effet pour `JSON.parse`).

---

## [2.18.0] - 2026-08-08

### Firmware

- **Support PSRAM — module WROVER-E** (feature-066) : `-DBOARD_HAS_PSRAM` (+ `-mfix-esp32-psram-cache-issue` / `-mfix-esp32-psram-cache-strategy=memw`, alignés sur les libs précompilées) activé sur les **deux envs** (`esp32dev` core 2 production, `esp32dev_core3` banc core 3). **Binaire unique** WROOM/WROVER : sur un module sans PSRAM, l'init échoue proprement et le firmware continue à l'identique (dégradation gracieuse, validée sur les deux cores).
- **Log de boot PSRAM** : `PSRAM : X Mo détectés (les allocations > 4 Ko y sont placées automatiquement)` ou `PSRAM absente — RAM interne seule (module WROOM)`.
- **Infos système** : champs `psram_size` / `psram_free` ajoutés à `GET /get-system-info` (0 = absente).
- **Levier TLS core 3** : sur module **WROVER-E**, la vérification/installation de mises à jour GitHub (`/check-update`, TLS sortant) **refonctionnera sous core 3** — les allocations > 4 Ko partent automatiquement en PSRAM (`SPIRAM_USE_MALLOC` précompilé), levant la limitation acceptée en v2.16.2 (spec `bug-checkupdate-tls-core3`). Validation à réception du module (procédure de swap : spec feature-066). Builds bi-core + 428 tests natifs verts.

### Fonctionnalités

- **Ligne « PSRAM » dans la card Infos système** (Paramètres → Système) : `libre / totale KB libres` sur module WROVER, « absente » sur module WROOM.

### Documentation

- `docs/API.md` : champs `psram_size` / `psram_free` documentés sur `/get-system-info`.
- `docs/BUILD.md` : flags PSRAM des deux envs (justification `-mfix-*` / libs précompilées).
- `docs/adr/0034-migration-core3-idf5.md` : note additive — levier PSRAM implémenté côté firmware, en attente du module pour validation. Pas de nouvel ADR (mise en œuvre du levier déjà consigné dans ADR-0034).
- `docs/features/page-settings.md` : ligne PSRAM de la card Infos système.

---

## [2.17.1] - 2026-08-08

### Firmware

- **Suppression du réglage « Écran LVGL » devenu sans objet** (suivi de feature-065 / v2.17.0) : card retirée de Paramètres → Avancé (elle était déjà masquée), champ `screen_enabled` retiré du contrat `/get-config` / `/save-config` (un client legacy qui l'envoie encore est ignoré sans erreur), clé NVS plus écrite ni lue (clé orpheline inoffensive sur les appareils existants), champ `authCfg.screenEnabled` supprimé.

### Documentation

- `docs/API.md` : note de compatibilité — `screen_enabled` retiré, champ legacy ignoré depuis v2.17.1.
- `docs/features/page-settings.md` : card « Écran LVGL » documentée comme supprimée.

---

## [2.17.0] - 2026-08-08

### Firmware

- **Retrait complet du support écran externe LVGL / UART2** (feature-065) : l'écran déporté (ESP32 dédié, liaison UART2 sur GPIO 16/17) était un essai **jamais mis en production**. Suppression des trois couches `uart_transport` / `uart_protocol` / `uart_commands` (~1 000 lignes) et de toutes les émissions d'événements dispersées (`main.cpp`, `pump_controller.cpp`, `filtration.cpp`, `sensors.cpp`, `web_routes_calibration.cpp`). **Aucun changement de contrat** HTTP / WebSocket / MQTT — retrait pur, régulation et gardes de sécurité chimique strictement identiques (double GO pool-chemistry, 428/428 tests natifs).
- **Sécurité chimique — fermeture du bypass `pump_test` UART** : la commande écran `run_action/pump_test` appelait `setManualPump()` **sans passer par les gardes** `evaluateManualInject()` (bypass assumé « écran non déployé », condition pool-chemistry #4 de la feature-006). Cette voie non gardée disparaît avec `uart_commands` : toutes les activations manuelles restantes passent par les gardes web.
- **Gains flash** : core 2 (production) **−35,3 Ko** (1 481 445 o, 80,7 % d'app0) ; core 3 (banc) **−30,0 Ko** (1 667 566 o, marge app0 92,3 % → **90,9 %**).
- **GPIO 16/17 libérés** — prérequis matériel du module **WROVER-E (+8 Mo PSRAM)** levé (levier TLS core 3, cf. ADR-0034 et limitation connue v2.16.2).
- **Toggle « Écran LVGL » de Paramètres → Avancé** : la card (déjà masquée) et le champ `screen_enabled` sont **conservés mais inertes** — la valeur reste persistée en config sans aucun effet. Retrait éventuel via une spec de suivi.

### Documentation

- Purge des mentions écran/UART dans `README.md`, `docs/MQTT.md`, `docs/subsystems/` (pump-controller, sensors, filtration, mqtt-manager, measure-validity) ; avertissement « `pump_test` non gardée » de `docs/subsystems/pump-controller.md` historicisé (bypass FERMÉ en v2.17.0).
- `docs/features/page-settings.md` : card « Écran LVGL » documentée comme masquée / toggle inerte.
- `docs/adr/0034-migration-core3-idf5.md` : note additive — GPIO 16/17 libérés, prérequis WROVER-E levé, nouvelle marge app0 core 3. Pas de nouvel ADR (retrait de code mort, décision portée par la spec + ADR-0034).

---

## [2.16.2] - 2026-08-08

### ⚠️ Limitation connue — core Arduino 3 uniquement (banc, pas la production)

- **Sous core 3 / IDF 5, « Vérifier les mises à jour » et « Installer depuis GitHub » sont inopérants** (tout TLS **sortant** depuis l'ESP32) — limitation **acceptée** (décision utilisateur 2026-08-08, spec `bug-checkupdate-tls-core3`). Cause démontrée (pas supposée) : l'empreinte statique d'IDF 5 sur ESP32 classique laisse un plus gros bloc heap contigu de ~43-47 Ko **dès le boot**, insuffisant pour une session TLS sortante (~50 Ko en pointe) — les allocations refusées pendant le handshake remontent en erreurs mbedtls trompeuses (« X509 alloc failed », « certificate verification failed »). GitHub, les certificats embarqués et la crypto matérielle sont hors de cause (validation X509 rejouée sur l'ESP32 lui-même : rc=0). Le remède upstream (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`) n'existe pas dans les cores 3.3.x. **L'échec est propre** : `500` + log détaillé, aucun crash, le reste du firmware n'est pas affecté. **Contournement** : mise à jour OTA par **upload manuel** des binaires de release (`/update`, flux déjà utilisé). **Levier futur** : module WROVER-E (+8 Mo PSRAM) — la config core 3 place déjà les allocations > 4 Ko en PSRAM (seuil 4096, `SPIRAM_USE_MALLOC`) → TLS restauré ; prérequis : libérer les GPIO 16/17 (retrait du code écran UART, feature-065 à venir). La production (core 2) n'est pas concernée.

### Modifié

- **Certificats racines GitHub (`github_root_ca.h`) : les racines USERTrust RSA/ECC passent en variantes auto-signées** (exp. 2038) — les variantes cross-signées par AAA Certificate Services sont retirées (hygiène et pérennité de la chaîne de confiance ; ce n'était pas la cause du bug). Empreintes SHA-256 vérifiées contre les racines officielles Sectigo, sources consignées dans le header. ISRG Root X1 (assets CDN) inchangée.

### Ajouté

- **Diagnostics `/check-update` enrichis** : état heap loggé à chaque vérification (« heap libre=… o, plus gros bloc=… o » **avant** l'ouverture de la session TLS — distingue épuisement total et fragmentation) ; en cas d'échec côté client (code HTTP négatif), le log porte désormais le **détail mbedtls** (`client.lastError()`, message + code) et le libellé `HTTPClient::errorToString()` au lieu d'un « Erreur HTTP GitHub: -1 » muet.

### Documentation

- `docs/adr/0034-migration-core3-idf5.md` : note additive 2026-08-08 — régression TLS sortant sur ESP32 classique ajoutée aux conséquences négatives (acceptation, contournement, levier WROVER-E). Pas de nouvel ADR (conséquence de la migration déjà actée).
- `docs/API.md` / `docs/subsystems/ota-manager.md` : limitation core 3 et nouveaux diagnostics documentés sur `/check-update`.

---

## [2.16.1] - 2026-08-04

### Corrigé

- **La vérification de mise à jour (`/check-update`) ne tombe plus en erreur sur un appareil en fonctionnement prolongé** (constaté en production le 2026-08-04 sur un appareil 2.14.0 : « Erreur HTTP GitHub: -1 » systématique sur heap fragmenté — l'allocation du contexte TLS ~48 Ko échouait avant toute communication — puis « JSON parse error » juste après reboot). Cause : le handler violait le patron [ADR-0032](docs/adr/0032-reponses-http-bornees.md) côté flux **entrant** — la réponse GitHub (~10 Ko, croissante à chaque release) était chargée entière en RAM (`getString()`) puis parsée en document complet (pic ~35 Ko en plus de la session TLS). Correctif : parse **en flux** (`useHTTP10(true)` + `deserializeJson` sur `getStream()` avec filtre 7 champs) et `JsonDocument` sur allocateur **plafonné net** (`BudgetJsonAllocator`, budget `kOtaCheckJsonBudgetBytes` = 8192 o) → pic mémoire ~35 Ko → **≤ 8 Ko** (hors session TLS ~48 Ko, incompressible). Corps de release hors budget → `500` propre « GitHub response too large » + log dédié (chemin d'erreur distinct du « Erreur parsing JSON GitHub », qui porte désormais le détail de l'erreur). Réponse JSON de la route **inchangée** vue du frontend (mêmes champs).
- **Les notes de release tronquées ne produisent plus de JSON invalide côté navigateur** : la coupe à `kOtaReleaseNotesMaxChars` (2048) respecte désormais les frontières UTF-8 (`utf8SafeCutLen`) — un caractère accentué ou un emoji coupé en deux au point de troncature rendait la réponse illisible (symptôme « réponse mal formée » de la 1ʳᵉ tentative post-reboot).

### Ajouté

- **Nouveau header pur `src/ota_check_logic.h`** (`AllocBudget` admit/release, `utf8SafeCutLen` — logique testable en natif, ADR-0017) + suite native `test/test_native_ota_check/` (**+13 tests, 428 au total**).

### Documentation

- `docs/adr/0032-reponses-http-bornees.md` : extension 2026-08-04 — le patron couvre désormais aussi les **réponses entrantes** d'API externes (parse en flux + budget dur), dernière ligne « bornage amont » du tableau d'audit soldée. Pas de nouvel ADR (application du patron existant).
- `docs/API.md` : `/check-update` — contrat inchangé, nouveau cas d'erreur 500 « GitHub response too large », troncature UTF-8 sûre.
- `docs/subsystems/ota-manager.md` : parse en flux borné de la réponse GitHub Releases.

---

## [2.16.0] - 2026-08-04

### Firmware

- **Alerte MQTT « pompe hors service »** (feature-064 — exigence pool-chemistry P1, condition de la porte de bascule core 3, ADR-0034) : si une pompe doseuse a échoué son init PWM au boot et a été mise en sécurité fail-closed (`pumpUsable`, feature-063 — **core Arduino 3 uniquement**, impossible en production actuelle core 2), une alerte `pump_unusable` est désormais publiée **par pompe**, en français (« Pompe 1 (pH) HORS SERVICE : échec init PWM (GPIO=25, canal LEDC=0) — aucune injection possible jusqu'au reboot »), sur le canal d'alertes existant `{base}/alerts`. Le CRITICAL local ne reste plus la seule remontée : Home Assistant voit la pompe hors service au lieu de « compter » des cycles sans injection réelle (dérive pH/ORP silencieuse). Re-publication **à chaque (re)connexion MQTT** (arbitrage Q1, décision pool-chemistry) : l'alerte est non-retained et l'état permanent jusqu'au reboot — un broker absent au boot ne la perd plus. Nouvel accesseur lecture seule stricte `PumpController.isPumpUsable(index)` (aucun setter, reboot requis pour réarmer ; core 2 : toujours utilisable). Aucun chemin d'injection modifié — visibilité uniquement (double GO pool-chemistry). Code dormant en production, actif sur le banc `esp32dev_core3` et après bascule.

### Documentation

- `docs/MQTT.md` : type d'alerte `pump_unusable` (condition, re-publication à chaque connexion, exemple de message, limitation HA assumée).
- `docs/subsystems/pump-controller.md` : accesseur `isPumpUsable()` (sémantique bi-core, fail-closed hors bornes) + exigence P1 marquée livrée.
- `docs/subsystems/mqtt-manager.md` : hook de re-publication dans `connectInTask()` (arbitrage Q1, outQueue, code dormant core 2).
- `docs/adr/0034-migration-core3-idf5.md` : item « alerte MQTT pompe inutilisable » de la porte de bascule coché (livré feature-064).

---

## [2.15.0] - 2026-08-04

### Firmware

- **Code bi-core core 2 / core 3** (feature-063, exécution de l'étude feature-062) : le firmware compile désormais à l'identique sur core Arduino 2.0.17 (production) ET core Arduino 3.x / ESP-IDF 5.x, via gardes `#if ESP_ARDUINO_VERSION_MAJOR >= 3` dans 3 fichiers — LEDC pompes doseuses (`ledcAttachChannel`/`ledcWriteChannel`, 20 kHz/8 bits/canaux strictement identiques, échec d'attache → CRITICAL + pompe inutilisable fail-closed `pumpUsable`, double GO `pool-chemistry`), watchdog (`esp_task_wdt_config_t` + `esp_task_wdt_reconfigure`), mbedtls sha256 (macros de compat 2.28/3.x) ; + include explicite `WiFiClient.h` dans `web_routes_ota.cpp` (include transitif disparu en core 3). **Le comportement production est strictement inchangé — prouvé à l'octet près** (binaire core 2 identique à la baseline, delta ±16 octets = artefact `__TIME__`) ; 415/415 tests natifs.
- **Nouvel environnement de banc `esp32dev_core3`** (pioarduino, BUILD-ONLY — jamais flashé) : compile SUCCESS avec le fork ESP32Async épinglé 3.4.10/3.11.2 (compatibilité core 3 désormais **prouvée**) — Flash 1 693 142 o = 92,27 % d'app0 (marge 141 866 o), RAM 69 248 o, versions résolues pioarduino 55.3.37 / core 3.3.7 / IDF 5.5.0. `default_envs = esp32dev` et `deploy.sh` inchangés ; la bascule production reste une porte conditionnelle (ADR-0034).
- **Retrait des flags lwIP inopérants** `-DCONFIG_LWIP_MAX_SOCKETS=24` / `-DCONFIG_LWIP_TCP_MAXRTX=5` de `platformio.ini` : flags « fantômes » jamais appliqués (lwIP précompilé dans le core — 16 sockets / 12 retransmissions effectifs depuis toujours). Retrait sans effet binaire, prouvé.

### Documentation

- `docs/adr/0034-migration-core3-idf5.md` (**nouveau** — ex-DRAFT numéroté et accepté) : décision de migration en deux phases, résultats réels du banc, check-list de la porte de bascule production (banc matériel L3, épinglage pioarduino sur release taguée, alerte MQTT pompe inutilisable, hors saison, 14 j, binaires archivés + sha256).
- `docs/adr/0011-mqtt-task-dediee.md` : note corrective « effet fantôme » (les flags lwIP n'ont jamais été appliqués — le comportement réseau réel n'a jamais été celui documenté en IT3).
- `docs/adr/0031-*.md` / `docs/adr/0033-*.md` : notes « défense CONSERVÉE » (déclencheur littlefs div/0 corrigé sous IDF 5 → défenses dormantes après bascule, PAS retirées — décision utilisateur du 2026-08-03).
- `docs/BUILD.md` : section « Les deux environnements firmware » + pièges pioarduino (pio du penv, `pio pkg install`, épinglage requis).
- `docs/subsystems/pump-controller.md` : section « Mise en sécurité `pumpUsable` » (chemin core 3 uniquement).

---

## [2.14.0] - 2026-07-30

### Corrigé

- **Crash WS récurrent : l'angle mort de la garde heap v2.11.1 est comblé** (2ᵉ coredump `bad_alloc` dans `textAll`, 30/07, v2.13.0 — garde **passée** puis prise de vitesse). Deux failles fermées : (1) **TOCTOU** — entre le contrôle `getMaxAllocHeap()` et l'allocation du buffer de trame, la tâche `async_tcp` peut consommer le bloc mesuré → `try/catch (std::bad_alloc)` autour des 3 `textAll` (filet absolu : l'exception part de `makeSharedBuffer` **avant** toute mutation de file du fork → toute course résiduelle = push sauté compté dans `skippedPushCount()`, plus jamais d'`abort()`) ; (2) **seuils adjacents** — la garde WS (8192) était **égale** au seuil de restart auto-guérison (8192), créant une zone grise 8-16 Ko où les pushes passaient sans que l'auto-guérison s'arme → `kWsPushMinAllocBytes` relevé à **16384** (marge 2×) : les pushes s'arrêtent avant la zone rouge, l'auto-guérison garde sa fenêtre d'action.

### Modifié

- **`/get-history` passe en flux chunked zéro-buffer** (ADR-0032 mécanisme (b), patron `/download-logs`) : supprime **la plus grosse allocation contiguë du firmware** — la `String` ~40 Ko épinglée pendant tout le transfert à chaque ouverture du dashboard (générateur de fond G3 de l'audit churn-heap du 24/07, différé « si récidive » — récidive constatée). Chaque point est copié sous mutex **court** (25 ms, `getPointByIndex`) ; contention (consolidation en cours) → `RESPONSE_TRY_AGAIN` borné (~200 retries consécutifs, reset après succès — tolère ~100 s vs ~1,5 s de pire consolidation), au-delà : clôture **propre** (JSON toujours valide, moins de points — jamais tronqué syntaxiquement). **Contrat JSON octet pour octet inchangé** (y compris la clé `count` émise deux fois — bizarrerie historique conservée pour compat) ; le 503 « heap fragmenté » disparaît de cette route ; UI intouchée (`data/` inchangé, chunked transparent pour `fetch()`). Post-revue : seul le token `range` **reconnu** est échoé dans l'en-tête JSON (valeur inconnue → traitée et échoée `all`).

### Ajouté

- **Buffer statique de trame WS** (`sWsFrameBuf`, `kWsFrameBufBytes = 3072`, réservé au link en `.bss` **hors heap** — pattern « réserver une fois, réutiliser toujours ») : plus aucune `String` temporaire par push (churn G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s). Trame trop grande → push **sauté** (jamais tronqué) + compteur `oversizedFrameCount()` + WARN throttlé dédié. Invariant documenté : buffer réservé à `loopTask`. `kWsSensorJsonReserveBytes` supprimée.
- **Télémétrie heap post-crash** : ring de 128 entrées × 8 o en RAM RTC noinit (1032 o vérifiés à l'ELF, magic `0xB10C4EA9` distinct de l'anti-boucle auto-guérison), échantillonné toutes les **2 s** depuis la loop (uptime, maxBloc, heap libre, clients WS, delta de pushes sautés). Au boot après un reset ≠ POWERON, les **32 dernières entrées (~64 s de cinétique d'effondrement)** sont dumpées dans le journal (~8 lignes) puis le magic est invalidé. Limites assumées : dump aussi sur reboot volontaire (`SW_RESET` inclut OTA/manuel — bruit accepté, trié par le motif en en-tête) ; lignes en RAM/Serial flushées ensuite (perdues si re-crash immédiat).
- **2 nouvelles suites de tests natives** : `test_native_history_export` (cutoff `range` + format point JSON purs) et `test_native_heap_ring` (encodage 8 o, index du ring, âges modulaires) — **+27 tests, 415 au total**.

### Documentation

- `docs/adr/0032-reponses-http-bornees.md` : **extension 2026-07-30** — `/get-history` migre du patron (c) au patron (b), tableau d'audit soldé (G3 traité) ; durcissement WS (seuil 16384, catch, buffer statique) ; pas de nouvel ADR (application des mécanismes déjà actés).
- `docs/subsystems/ws-manager.md` : garde 16384 (zone grise), filet `bad_alloc`, buffer statique + invariant `loopTask`, `oversizedFrameCount()`.
- `docs/API.md` : `/get-history` chunked (contrat inchangé, comportement sous contention, double clé `count` documentée, fin du 503).
- `docs/subsystems/history.md` : `getPointByIndex` (25 ms), export chunked, glissement toléré, note `migrateLegacyHistory`.
- `docs/subsystems/system-health.md` : ring de télémétrie heap (format, cadence, dump, invalidation du magic, limites).

---

## [2.13.0] - 2026-07-29

### Ajouté

- **Les mesures pH/ORP/température ne sont plus présentées comme valides ni historisées quand l'eau ne circule pas** (feature-061). Les sondes sont montées dans le circuit de filtration : filtration arrêtée = eau stagnante autour des sondes → valeurs dérivantes **non représentatives du bassin** (plateaux/dérives nocturnes qui polluaient les courbes). Désormais : valeurs pH/ORP/température à « `--` » sur le tableau de bord et les pages dédiées avec un **badge bleu informatif** « Filtration arrêtée » (nouvelle variante `info` — état normal « attendre », distinct du rouge « Capteur indisponible » = « agir » ; « Signal filtration périmé » en ambré si le signal externe est stale) ; points d'historique enregistrés avec pH/ORP/température `null` → **trous dans les courbes** (état filtration et flags dosage toujours enregistrés ; pas de purge rétroactive des anciens points). À la **reprise** de la filtration, un **délai de purge de 5 min** (réutilise `stabilizationDelayMin`, badge « Stabilisation : ~N min » recalculé à chaque push WS) laisse l'eau stagnante de la chambre de mesure se renouveler — délai complet appliqué aussi au boot avec eau présente (conservateur). Source de décision unique : nouveau module `measure_validity` (logique pure + coquille avec cache portMUX) branché sur `resolveWaterPresence()` (feature-056) ; champs nouveaux WS + `/data` : `measures_valid`, `measures_stab_remaining_s` (+ `water_present` ajouté à `/data`). Chaîne de mesure, calibration, onglet Avancé, dosage (déjà gardé sans eau) et MQTT/HA **inchangés**. +14 tests natifs (388 au total).
  - **Itération 2 — extension à la température** : la sonde DS18B20 est dans la canalisation → à l'arrêt, l'eau du tuyau dérive vers la température ambiante du local (c'est la température du local qui était enregistrée, pas celle du bassin). Même drapeau `measures_valid` (pas de validité séparée : le délai de purge de 5 min est conservateur pour la température — l'eau du tuyau se renouvelle en < 1 min — mais une validité distincte ajouterait de la complexité pour un gain nul) ; carte/compact température à « `--` » + mêmes badges que pH/ORP ; `NAN` dans l'historique → trous dans la courbe température. Le **mode Auto de la filtration n'est pas impacté** (il lit `sensors.getTemperature()` en direct), MQTT/UART écran inchangés (live).

### Documentation

- `docs/subsystems/measure-validity.md` (**nouveau**) : règles de décision, contrat des 3 champs, concurrence (cache portMUX), logs ; itération 2 : périmètre étendu aux 3 mesures, justification « même drapeau », non-impact du mode Auto filtration.
- `docs/subsystems/history.md` : exclusion pH/ORP — puis température (itération 2) — sans circulation (NaN → `null`, agrégation, pas de purge rétroactive).
- `docs/subsystems/ws-manager.md` + `docs/API.md` : champs `measures_valid` / `measures_stab_remaining_s` / `water_present` (`/data`), note de repli HTTP (`/data` sans `filtration_state_stale` — asymétrie préexistante).
- `docs/features/page-dashboard.md`, `page-ph.md`, `page-orp.md` : badges (libellés exacts, hiérarchie danger > warning > info), valeurs `--`, trous dans les courbes, dé-duplication dosing-status ; itération 2 : carte température (badge composé, `--`, courbe avec trous).
- Pas d'ADR (confirmé architect) : politique de présentation/persistance sans contrainte structurante — la source unique de présence d'eau est déjà actée par l'ADR-0026, le pattern module pur + coquille par l'ADR-0017.

---

## [2.12.0] - 2026-07-29

### Modifié

- **Limite de cycles de dosage par jour (garde 9 anti-cycling) : défaut 20 → 48, et désormais réglable 10-60** (feature-060). Motivation : incident du 2026-07-29 — régulation ORP bloquée en début d'après-midi (« Limite cycles/jour atteinte — reprise à minuit ») avec seulement **104 mL** injectés. En régime d'entretien post-remontée du stabilisant, la régulation travaille par petites retouches (~5 mL/cycle, pause mélange 20 min) : un régime **sain** qui produit naturellement 20-40 cycles/jour et épuisait la limite de 20 — non fondée chimiquement, alors que l'ORP dérivant tout l'après-midi est lui-même un risque sanitaire. Nouveau défaut **48** (1 cycle/30 min moyen, sous le plafond physique ~70/j imposé par la pause mélange), exposé dans Paramètres → Régulation pH / ORP (champ « Cycles de dosage max / jour (par pompe) », `max_cycles_per_day` dans `/get-config` / `/save-config`). **Bornes dures 10-60** (`kCyclesPerDayMin`/`kCyclesPerDayMax`) **clampées côté serveur** (fonction pure `clampCyclesPerDay`) et au chargement NVS (clé `max_cycles_day`, migration ≤ 2.11.x automatique) ; warn journalisé sur valeur hors bornes et sur toute modification. Anti-rafale (6/min, 20/15 min), limites volumétriques/horaire et toutes les autres gardes **inchangées** ; le compteur du jour n'est pas remis à zéro à la modification. **Double validation `pool-chemistry` (2026-07-29)** : GO pré-design sous conditions (toutes implémentées) + GO sur re-validation du diff. +6 tests natifs (374 au total).

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Limite de cycles/jour configurable » (fondement du défaut 48, bornes, double clamp serveur/NVS, warns de traçabilité, verdicts pool-chemistry).
- `docs/API.md` : champ `max_cycles_per_day` (`/get-config`, table de validation `/save-config` — clamp silencieux 10-60 + warn).
- `docs/features/page-settings.md` : nouveau champ de l'onglet Régulation pH / ORP ; `maxCyclesPerDay` retiré du tableau des paramètres non exposés.
- Pas d'ADR : paramétrage borné d'une garde existante, décision et alternatives tracées dans la spec + verdicts pool-chemistry (précédent : feature-025, `orpStartThreshold`).

---

## [2.11.1] - 2026-07-28

### Corrigé

- **Crash/reboot `bad_alloc` dans les broadcasts WebSocket sur heap fragmenté** (coredump du 2026-07-28, v2.11.0, décodé GDB) : `AsyncWebSocket::textAll()` alloue son buffer de trame (`makeSharedBuffer`) **sans rattraper** `std::bad_alloc` → plus gros bloc libre < ~1,8 Ko = `abort()` et reboot brutal (sans arrêt ordonné des pompes ni sauvegarde des cumuls). Correctif : **garde heap** `_heapAllowsPush()` en tête des 3 broadcasts, **avant** toute construction de JSON — plus gros bloc < `kWsPushMinAllocBytes` (8 Ko, aligné sur le seuil de restart auto-guérison) → push **sauté** (l'UI rate un cycle de 5 s, sans conséquence), compteur atomique de pushes sautés (`skippedPushCount()`) + WARN throttlé 30 s émis uniquement depuis `update()` (invariant anti-ré-entrance : le helper ne logge jamais). Extension de la philosophie ADR-0032 (réponses bornées) à la voie WebSocket.
- **Clients WebSocket zombies qui épinglaient la mémoire** (cause n°1 identifiée par l'investigation churn-heap) : un client qui n'ACK plus (téléphone verrouillé, WiFi en veille) conservait connexion ouverte une file de **32 buffers ~1,8 Ko** (~58 Ko épinglés en blocs dispersés) — de quoi effondrer le plus gros bloc en 2-3 min, plus vite que la fenêtre 60 s de l'auto-guérison. Correctifs : file d'envoi par client bornée à **5 trames** (`-DWS_MAX_QUEUED_MESSAGES=5`) + `setCloseClientOnQueueFull(true)` — le fork ferme lui-même le client saturé **sous son verrou** (la boucle manuelle sur `getClients()` a été rejetée en revue : itération sans verrou = UB face à l'érase concurrent d'`async_tcp`) ; + PING keepalive 30 s (ne couvre que les zombies inactifs, le PING n'étant envoyé que file vide — la saturation couvre les autres). En complément : `reserve` du JSON `sensor_data` 1600 → **2048** (`kWsSensorJsonReserveBytes`, payload mesuré 1777 o) — supprime ~6 réallocations de String par push.

### Modifié

- **L'auto-guérison heap réagit en ~15 s au lieu de ~3 min** : le bloc heap de `checkSystemHealth()` est extrait vers `checkHeapHealth()`, sur un timer propre à **cadence adaptative** (fonction pure `heapSampleIntervalMs()`, `self_heal_logic`) — 60 s en régime sain, **5 s** dès que le plus gros bloc passe sous le seuil d'alerte (20 Ko) → le débounce (3 ticks) confirme avant que le crash ne gagne la course. Invariants ADR-0029 strictement conservés (seuils, débounce, budget de reboots, mode maintenance, garde OTA) ; **+5 tests natifs** (368 au total).
- **Alerte `low_memory` (heap total) émise sur transition** au lieu d'être ré-émise à chaque tick de 60 s (chaque CRITICAL re-flushait le disque) : une alerte à l'entrée sous le seuil, une INFO « Mémoire rétablie » au retour au-dessus. Limite assumée : alerte MQTT non-retained, un abonné hors ligne pendant la transition rate l'épisode (borné par l'INFO de rétablissement).

### Documentation

- `docs/adr/0029-auto-guerison-heap.md` : **amendement** « échantillonnage adaptatif » (pas de nouvel ADR — la décision est inchangée, seule la fréquence d'observation évolue).
- `docs/adr/0032-reponses-http-bornees.md` : **extension** aux émissions WebSocket (garde côté appelant, buffers de file bornés).
- `docs/subsystems/ws-manager.md` : garde heap, récolte des zombies, `skippedPushCount()` ; **code mort signalé** : `broadcastLog()`/`setLogCallback()` jamais branchés depuis 2.7.0 (aucun message WS `type:"log"` n'a jamais été émis — purge reportée).
- `docs/subsystems/system-health.md` : `checkHeapHealth()` séparée, cadence adaptative, alerte `low_memory` en transition.

---

## [2.11.0] - 2026-07-27

### Corrigé

- **Cause racine des « corruptions » récurrentes de la partition historique** (2 incidents en 14 jours, boot-loop `IntegerDivideByZero` dans littlefs) : ce n'était pas une corruption mais une **saturation structurelle d'espace** — au pire moment, la réécriture copy-on-write de `/history.json` (ancien + nouveau fichiers coexistants) plus le journal et son fichier de rotation demandaient **19-22 blocs sur les 16 que compte la partition de 64 Ko**. Le manque d'espace déclenchait alors un bug littlefs amont répertorié (division par zéro dans `lfs_alloc` sur système de fichiers plein — [littlefs #1063](https://github.com/littlefs-project/littlefs/issues/1063)), non corrigeable localement (version figée par la plateforme). Correctif structurel (**ADR-0033**) : `saveToFile` mesure désormais le JSON, vérifie l'espace disponible (garde pure `historySaveAdmit`, marge de 2 blocs), **supprime l'ancien fichier avant d'écrire** (pic copy-on-write divisé par deux) et **vérifie l'écriture** (un fichier partiel est supprimé, jamais laissé sur le disque) ; en cas d'espace insuffisant, le save est sauté proprement (ancien fichier préservé, nouvelle tentative 5 min plus tard) — l'ancien code tronquait l'historique dès l'ouverture, avant toute vérification. Les plafonds du journal passent de 16/12 Ko à **8/6 Ko**. **Nouveau pire cas : 13 blocs sur 16, marge garantie de 3 blocs** — la partition ne peut structurellement plus se remplir.
- **Anti-tempête de flush** : les échecs de sauvegarde de l'historique sont journalisés en WARNING throttlé (1/h) au lieu d'ERROR — un ERROR déclenchait un flush disque immédiat… sur le système de fichiers déjà plein, toutes les 5 minutes.

### Ajouté

- **Préservation de preuve dans le disjoncteur de boot-loop** (feature-059) : avant l'effacement automatique de la partition, les 2 premiers blocs (8 Ko de métadonnées littlefs) sont dumpés en hexadécimal sur le port série (`[HISTDUMP]`, ~2,4 s, watchdog rafraîchi) et une empreinte courte (CRC32 des 2 blocs + 16 premiers octets) est conservée en NVS — de quoi discriminer, si une occurrence survenait encore, la saturation d'une éventuelle corruption mémoire persistée (hypothèse résiduelle de l'investigation).
- **Instrumentation de la persistance** : occupation de la partition journalisée (throttlée 1/h), compteur de saves refusés (`nospc_cnt`) et horodatage du dernier save réussi en NVS (écrits uniquement sur échec, pour préserver la flash NVS).

### Documentation

- `docs/adr/0033-budget-espace-partition-history.md` : budget d'espace chiffré avant/après, trade-off remove-avant-write (fenêtre de perte bornée à 5 min, couverte par le disjoncteur) vs write-tmp-puis-rename écarté (pic ×3), bug amont documenté.
- `docs/subsystems/system-health.md` (préservation de preuve), `docs/subsystems/history.md` (garde d'espace, instrumentation), `docs/subsystems/logger.md` (nouveaux plafonds 8/6 Ko).

## [2.10.5] - 2026-07-27

### Corrigé

- **Valeurs pH/ORP/température figées à l'écran quand un capteur devient indisponible** : sonde débranchée → le firmware invalide la lecture (NaN) et publie `null` (`/data` + WebSocket), mais l'UI conservait indéfiniment la **dernière valeur connue** (pattern « skip si null ») sur le tableau de bord et les widgets compacts, sans aucune indication d'indisponibilité. Désormais, **convention unique** : toute valeur capteur `null` s'affiche « `--` » sur **tous** les sites d'affichage numérique — métriques du tableau de bord (`#m-temp`, `#m-ph`, `#m-orp`), widgets compacts (`#compact-ph`, `#compact-orp`, `#compact-temperature`, unité conservée), sections détaillées — alignée sur les pages Réglages/Calibration qui faisaient déjà le bon geste. En complément, la carte **Température** gagne le badge « **Capteur indisponible** » (`#temperature-sensor-badge`), calqué sur les badges pH/ORP de la feature-022 (même garde anti-faux-positif au boot : aucun badge avant le premier message WS). Capteur rebranché → l'affichage reprend automatiquement, sans recharger la page. Graphiques et pages Réglages inchangés (sauter les `null` y est correct : trous dans les courbes). Frontend uniquement (`data/app.js`, `data/index.html`), aucun fichier firmware touché.

---

## [2.10.4] - 2026-07-24

### Corrigé

- **Crash/reboot au chargement du journal sur heap fragmenté** : `GET /get-logs` sérialisait jusqu'à 200 entrées de log (20-40 Ko) dans le `cbuf` **contigu** d'`AsyncResponseStream` (`resize()` = `new char[]` throwant) → `std::bad_alloc` non rattrapé sur la tâche `async_tcp` → `abort()` → **reboot**. **2ᵉ occurrence de la classe en 2 jours** (coredump du 2026-07-24, décodé GDB — même signature que `/debug/sensor_trace`, corrigé en 2.10.1) : la garde v2.8.2 à seuil heap FIXE (`kMinHeapForLogsResponse` = 24 Ko, indépendant de la taille réelle) et son commentaire « realloc, jamais d'abort » sont **invalidés**. Correctif **systémique** après audit exhaustif des routes à réponse bufferisée (toutes les autres prouvées bornées), règle générale gravée dans l'**ADR-0032** :
  - **`/get-logs` paginé** : curseur `?since=` réutilisé, page de 30 entrées (hard cap 60 — la queue d'entrées partageant la même milliseconde que la dernière incluse est embarquée, sinon le curseur strictement supérieur les perdrait entre deux pages), champ `truncated` dans la réponse (`{uptime_ms, logs[], truncated}`), page construite en **une passe sous le mutex logger**, `measureJson` + marge `kHttpHeapMarginBytes` (4 Ko) vs `ESP.getMaxAllocHeap()` → **503** propre ou stream **pré-dimensionné exact**. Échappement manuel supprimé (délégué à `serializeJson`). **UI** : boucle de pagination transparente dans `loadLogs()` (max 8 itérations, curseur strictement croissant, compatible ancienne API sans `truncated`).
  - **`/download-logs` en flux chunked zéro-RAM** (patron `/coredump/download`) : fichier `/system.log` lu par blocs puis entrées RAM une à une (nouveau `Logger::getLogEntryByIndex`, best-effort sur timeout mutex), report de ligne entre chunks via module pur `logs_export_chunk.h` (garantie de progression), lignes > 256 o tronquées « … ». Garde heap supprimée (inutile en chunked) — plus de 503 ni de troncature d'export sous heap fragmenté.
  - **`/get-history`** : `String::reserve(points × kHistoryPointJsonMaxBytes(128) + 128)` dont le retour `bool` EST le test du bloc contigu — échec → **503** (jamais de JSON tronqué silencieux). Pire cas du point JSON **mesuré** (113-120 o, constante 96 → 128 post-revue).
  - **`/check-update`** : release notes GitHub (non bornées) tronquées à `kOtaReleaseNotesMaxChars` (2048) + « … ».
  - `Logger::forEachRecentLog` : le visiteur retourne un `bool` (`false` = arrêt anticipé — une page = une prise de mutex) ; `kMinHeapForLogsResponse` **supprimée**, `kHttpHeapMarginBytes` (4096) généralisée (alias `kSensorTraceHeapMarginBytes` conservé) ; commentaires mensongers v2.8.2 corrigés. Aucun chemin de dosage touché.

### Ajouté

- **2 nouvelles suites de tests natives** : `test_native_logs_page` (admission de page pure `logs_page.h`) et `test_native_logs_export` (report de ligne `logs_export_chunk.h`) — **21 tests**, modules purs pattern ADR-0017, **353 tests natifs au total**.

### Documentation

- `docs/adr/0032-reponses-http-bornees.md` : **nouvel ADR** — toute réponse HTTP pouvant dépasser ~4 Ko doit être (a) paginée + pré-dimensionnée exactement, (b) chunked sans accumulation RAM, ou (c) allocation unique testée (`reserve` bool) avec 503 propre ; **gardes à seuil heap fixe interdites**. Tableau d'audit des routes (corrigées + prouvées sûres), 3 patrons de référence.
- `docs/API.md` : contrats mis à jour — `/get-logs` (pagination, `truncated`, 503), `/download-logs` (chunked, limites best-effort), `/get-history` (503 possible), `/check-update` (`release_notes` tronqué).
- `docs/subsystems/logger.md` : signatures `forEachRecentLog` (visiteur `bool`) et `getLogEntryByIndex` (best-effort), section gardes heap réécrite (suppression `kMinHeapForLogsResponse`).
- `docs/features/page-settings.md` : chargement paginé transparent de la card Logs + export chunked.

---

## [2.10.3] - 2026-07-23

### Corrigé

- **Libellé de log de reconnexion WiFi en mode AP secours** : en AP+STA, le log périodique (30 s) affichait « tentative de reconnexion (n/3) » avec un compteur jamais réinitialisé (ex. « 2881/3 » après une journée) ; le message devient « WiFi déconnecté, tentative de reconnexion STA (AP secours actif) » sans compteur — le « (n/3) » reste affiché uniquement en mode STA, où il arme réellement la bascule AP secours au 3ᵉ échec (`src/main.cpp`, aucune logique modifiée).

---

## [2.10.2] - 2026-07-23

### Modifié

- **Trace heap périodique retirée du journal** : la trace INFO `Heap: libre=… maxBloc=… minLibre=…` (instrumentation v2.9.1, émise toutes les 5 min ou sur chute significative de `maxBloc`) n'est plus émise — réduction du bruit : le journal persisté de 64 Ko ne stocke plus cette télémétrie de routine. Le bloc d'émission est **mis en commentaire** dans `checkSystemHealth()` (`src/main.cpp`), ré-activable en le décommentant pour un futur diagnostic. **Détection et alertes inchangées** : l'échantillonnage de `maxAllocHeap` alimente toujours l'auto-guérison (ADR-0029) et le flush one-shot ; le WARN « Heap fragmenté sous seuil d'alerte » et le CRIT « Mémoire faible » restent actifs.

### Documentation

- `docs/subsystems/system-health.md` et `docs/subsystems/logger.md` : trace INFO heap marquée désactivée (2.10.2) avec procédure de ré-activation ; détection/alertes documentées comme inchangées.

---

## [2.10.1] - 2026-07-23

### Corrigé

- **Crash/reboot à l'ouverture de l'onglet Avancé sur heap fragmenté** : `GET /debug/sensor_trace` sérialisait les **300 échantillons** du ring buffer de trace en une seule réponse (~20 Ko JSON) dans le `cbuf` **contigu** d'`AsyncResponseStream` — sur heap fragmenté (le plus gros bloc libre peut descendre sous 8 Ko, cf. ADR-0029, dont le seuil d'auto-guérison ne protégeait pas cette allocation de ~20 Ko), `std::bad_alloc` non rattrapé sur la tâche `async_tcp` → `abort()` → **reboot**, déclenché involontairement depuis le navigateur à chaque activation de l'onglet Avancé. Diagnostic établi par **décodage GDB du coredump** du 2026-07-23 (v2.10.0). Correctif :
  - **Route paginée** : `GET /debug/sensor_trace?offset=N&count=M`, page bornée à `kSensorTraceMaxSamplesPerPage = 50` (~3,5 Ko), défauts sûrs sans paramètres (première page), **clamp silencieux** hors bornes (jamais de 400 — calcul pur `computeTracePage()` dans `src/sensor_trace_page.h`), réponse enrichie (`count` total, `offset` effectif, `returned`).
  - **Garde heap** : `measureJson` + marge `kSensorTraceHeapMarginBytes = 4096` comparés à `ESP.getMaxAllocHeap()` → **503** `{"error":"heap fragmenté, réessayez"}` au lieu d'un crash, même pour une page ; stream **pré-dimensionné** (une seule allocation exacte, zéro `resize()`).
  - **UI onglet Avancé** : agrégation **transparente** des pages avant tracé (borne dure 12 itérations), message français sur 503 (« Mémoire fragmentée, réessayez. ») — rendu inchangé vu de l'utilisateur.
  - Contrat **lock-free** du ring buffer inchangé (`SensorManager::appendSensorTraceJson(JsonArray, offset, count)`) ; glissement du ring toléré entre deux pages (~1 échantillon max, route de diagnostic). Aucun chemin de dosage touché.

### Ajouté

- **Nouvelle suite de tests native** `test_native_trace_page` : 16 tests du clamp pur de pagination (`sensor_trace_page.h`, pattern ADR-0017 — invariants `offset ≤ total`, `n ≤ max`, `offset + n ≤ total` pour tout input, y compris `SIZE_MAX`) — **332 tests natifs au total**.

### Documentation

- `docs/API.md` : contrat paginé complet de `GET /debug/sensor_trace` (paramètres, défauts, clamp, champs de réponse, 503, note sur le glissement du ring).
- `docs/subsystems/sensors.md` : signature paginée `appendSensorTraceJson`, section « Pagination », constantes `kSensorTraceMaxSamplesPerPage` / `kSensorTraceHeapMarginBytes`, renvoi vers `sensor_trace_page.h`.
- `docs/features/page-settings.md` : pagination transparente + message 503 sur la card « Signaux capteurs ». Pas d'ADR : correctif local suivant un pattern existant (logique pure ADR-0017, garde heap ADR-0029), aucune alternative structurante verrouillée.

---

## [2.10.0] - 2026-07-22

### Corrigé

- **Boot-loop sur partition history corrompue nécessitant une récupération USB** : une corruption des métadonnées littlefs de la partition `history` provoquait un **panic CPU non rattrapable** (`IntegerDivideByZero` dans `lfs_alloc`) à la **première écriture** dans `/system.log` — LittleFS monte un filesystem corrompu **sans erreur** puis panique à l'écriture, et le garde-fou `.fscheck` existant (2 octets, stockés *inline*) n'exerçait jamais le chemin fautif. Le crash survenant **avant la montée du WiFi**, l'appareil bouclait indéfiniment : incident réel du 2026-07-22 (v2.9.2), **appareil injoignable pendant 1 semaine**, récupéré manuellement en USB (`esptool erase_region 0x3E0000 0x10000`). **5ᵉ mode de défaillance « appareil injoignable » traité** (après 2.8.2, 2.9.0, 2.9.1 et 2.9.2).
- **Lignes série entrelacées entre tâches** : l'émission console du logger passait par 4 `Serial.print` séparés hors mutex — deux tâches (mqttTask core 0 / loopTask core 1) pouvaient entrelacer préfixe et message (observé sur bench). La ligne est désormais composée dans un buffer pile (`kLoggerConsoleLineMax = 512`) et émise en **un seul `Serial.write` atomique** (troncature avec fin de ligne garantie). Format inchangé. Le « spam » de 6 286 lignes `sensor_problem → ON` observé le même jour a été démontré être un **rejeu matériel du FIFO UART0** (bench), pas un défaut logiciel — le dédup MQTT est correct et inchangé (investigation complète : `specs/features/todo/bug-spam-sensor-problem-serial.md`).
- **Course use-after-free potentielle sur les topics MQTT lors des reconnexions** (bug latent, découvert par analyse statique pendant l'investigation ci-dessus) : à **chaque tentative de connexion** au broker, `refreshTopics()` réassignait les ~90 `String` de topics depuis mqttTask, pendant que loopTask pouvait lire ces mêmes topics en enfilant un message sortant — une réassignation de `String` libère/réalloue son buffer, un lecteur concurrent pouvait donc déréférencer un pointeur libéré (topic corrompu publié, corruption heap silencieuse, crash apériodique — d'autant plus probable que le broker flappe). `refreshTopics()` lit désormais la config sous mutex, **ne reconstruit que si le préfixe a réellement changé** (early-return sinon), et la reconstruction comme la lecture cross-task sont sérialisées par un nouveau `topicsMutex` (timeout → message droppé + WARN throttlé, l'état retain est republié au cycle 10 s suivant). **Bénéfice collatéral** : plus **aucune** réallocation de topics sur reconnexion sans changement de config — supprime le churn heap de ~90 `String` toutes les 5 s sur broker instable. Le changement de préfixe MQTT à chaud reste fonctionnel (appliqué à la reconnexion qui suit le save). Aucun chemin de dosage touché.

### Ajouté

- **Auto-guérison de la partition history (disjoncteur de boot-loop)** : l'appareil **se répare désormais tout seul** quand la partition `history` est corrompue au point de faire paniquer littlefs au boot. Détection **inter-boot** : un compteur `crash_cnt` (NVS, namespace `hist_meta`) est posé juste avant le montage et remis à zéro après init complète ; si le boot précédent a crashé pendant cette fenêtre **2 fois consécutives** (`kHistoryInitCrashThreshold`), la partition est **effacée automatiquement** avant remontage (log CRIT), puis le boot se termine normalement — l'historique repart de zéro, l'appareil reste joignable (**disponibilité > données**, trade-off assumé). Seuls les resets « crash » comptent (panic, watchdogs) ; POWERON, reset logiciel/OTA et **brownout** (cause électrique, pas FS) réarment sans compter. Indépendant du budget anti-boucle `kAutoHealMaxReboots` et du mode maintenance : le disjoncteur **ne redémarre jamais**, il répare puis laisse booter.
- **Alerte MQTT `history_partition_reset`** : publiée (différée, après l'init MQTT) quand la partition a été effacée automatiquement — l'effacement est traçable dans HA.
- **Nouvelle suite de tests native** `test_native_history_guard` : 24 tests de la logique pure du disjoncteur (`history_guard_logic`, pattern ADR-0017).
- **Nouvelle suite de tests native** `test_native_mqtt_topics` : 27 tests de la normalisation pure du préfixe de topics MQTT (`mqtt_topics_logic`, extraite de `refreshTopics()` pour le correctif de course ci-dessus — trim, `/` finaux, fallback `pool/sensors`, troncature) — **316 tests natifs au total**.

### Documentation

- `docs/adr/0031-disjoncteur-bootloop-history.md` : **nouvel ADR** (détection inter-boot NVS + effacement au seuil de 2 ; alternatives écartées : test d'écriture élargi — impossible face à une exception CPU non rattrapable, validation du superblock — fragile aux internals littlefs, RAM RTC — effacée au power-on alors que la corruption FS y survit ; trade-off disponibilité > historique).
- `docs/subsystems/system-health.md` : 3ᵉ volet d'auto-guérison (disjoncteur history : machine drapeau/compteur NVS, mapping des reset reasons, alerte différée, récupération manuelle de secours).
- `docs/MQTT.md` : alerte `history_partition_reset`.
- `docs/subsystems/logger.md` : nouvelle section « Émission console » (write unique atomique) + note sur les logs pré-persistance (le CRIT du disjoncteur n'atteint jamais `/system.log`).
- `docs/adr/README.md` : index complété (ADR-0030, absent de l'index depuis la v2.9.2, et ADR-0031).
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Reconstruction des topics — `refreshTopics()` + `topicsMutex` » (contrat de concurrence : writer unique mqttTask, lecture cross-task `enqueueOutbound()` seule, `configMutex`/`topicsMutex` jamais imbriqués, mutex non-récursif ; guard no-op ; module pur `mqtt_topics_logic` ; mode dégradé sur timeout) + note sur le changement de préfixe à chaud dans la section feature-018. Pas d'ADR : correctif local dans le modèle producer/consumer existant (ADR-0011), aucune alternative structurante verrouillée.

---

## [2.9.2] - 2026-07-15

### Corrigé

- **Gel de la pile réseau nécessitant une coupure secteur** : après l'auto-guérison mémoire (2.9.1), un **nouveau** gel « appareil injoignable » est survenu — mais cette fois **avec un tas parfaitement sain** (plus gros bloc contigu ≈ 37 Ko, jamais proche du seuil de 8 Ko). Diagnostic : c'est la **pile réseau lwIP elle-même qui se fige** (serveur web, mDNS et même le ping ICMP tous morts) alors que le WiFi reste **associé**. Le thread `tcpip`/lwIP n'est **pas** surveillé par le watchdog matériel, et `loopTask`/`mqttTask` continuaient de tourner → ni la TWDT, ni l'auto-guérison mémoire (2.9.1) ne pouvaient l'attraper. C'est le cousin « en gel » du crash `tcp_recved` corrigé par la migration fork (2.9.0). **4ᵉ mode de défaillance « appareil injoignable » traité.**

### Ajouté

- **Auto-guérison réseau (watchdog de connectivité)** : l'appareil **se répare désormais tout seul** aussi quand c'est la **pile réseau** qui se fige, et plus seulement la mémoire — **plus besoin de couper le secteur**. Une **sonde ICMP active** pinge la passerelle toutes les 30 s (`kNetworkProbeIntervalMs`, timeout 2 s) : un echo reply prouve que toute la pile réseau (réception → lwIP → émission) fonctionne. Ce signe de vie est **indépendant de MQTT** (qui peut être désactivé). Réponse **graduée** : si le WiFi est associé mais la pile muette depuis **2 min** (`kNetworkStallSoftMs`), une **reconnexion WiFi** est tentée (`WiFi.disconnect()/reconnect()`, alerte MQTT `network_soft_recover`) ; si le gel persiste **5 min** (`kNetworkStallRebootMs`), un **redémarrage propre fail-safe** a lieu (mêmes garanties que l'auto-guérison mémoire : pompes coupées, cumuls sauvegardés, puis reboot ; alerte MQTT `network_stall_reboot`).
- **Anti-faux-positif** : la décision est **verrouillée** sur l'état du WiFi. Une coupure WiFi légitime, un broker MQTT en panne ou un reboot de la box **ne déclenchent rien** — l'horloge de gel est gelée tant que le WiFi n'est pas associé. Seul un vrai gel de la pile *alors que le WiFi tient* déclenche une action.
- **Budget anti-boucle partagé** : les deux auto-guérisons (mémoire + réseau) partagent le **même compteur** de reboots (3 max sur 15 min, `kAutoHealMaxReboots`). Après 3 reboots cumulés (heap et/ou réseau), l'appareil bascule en **mode maintenance** (pompes OFF, dosage inhibé, joignable) au lieu d'enchaîner. Conséquence assumée : un épisode combinant fragmentation mémoire **et** gel réseau bascule en maintenance plus tôt (comportement conservateur volontaire).

> ⚠️ **Correctif de containment, pas de cause racine** : le blocage réside dans AsyncTCP/lwIP (piste racine identifiée : churn WebSocket/HTTP), à traiter ultérieurement. Le watchdog de connectivité sort l'appareil du gel automatiquement en attendant.

### Documentation

- `docs/adr/0030-watchdog-connectivite.md` : ADR de la décision (sonde ICMP vers la passerelle plutôt que dépendance MQTT, réponse graduée soft→hard, budget anti-boucle partagé avec l'auto-guérison mémoire ; alternatives écartées).
- `docs/subsystems/system-health.md` : extension avec le watchdog de connectivité (sonde ICMP passerelle, machine à états `net_health`, réponse graduée soft→hard, budget partagé heap+réseau, refactor `performFailSafeRestart`, module pur `net_health_logic`, constantes).
- `docs/MQTT.md` : alertes `network_soft_recover` et `network_stall_reboot`.

---

## [2.9.1] - 2026-07-15

### Corrigé

- **Gel récurrent nécessitant une coupure secteur** : l'appareil pouvait se **figer** (web injoignable, écriture des logs arrêtée) tout en restant sous tension — seule une coupure/remise sous tension le débloquait. Contrairement aux crashes précédents (`std::bad_alloc` sur `/logs` en 2.8.2, assert lwIP WebSocket en 2.9.0), il n'y avait **ni panic, ni coredump, ni reboot automatique** : les tâches surveillées par le watchdog (loop, MQTT) restaient vivantes, donc le chien de garde ne se déclenchait jamais. Cause diagnostiquée : **épuisement / fragmentation du tas** — le plus gros bloc contigu libre s'effondre alors que le heap total reste correct, ce qui asphyxie le serveur web et la persistance des logs. Amplifié par un WiFi instable (double NAT via prise CPL → IP qui saute), qui rejouait `MDNS.end/begin` à chaque oscillation d'adresse et accélérait la fuite mémoire. **3ᵉ mode de défaillance « appareil injoignable » traité.**
- **Ré-annonce mDNS en boucle sur réseau instable** : `poolcontroller.local` n'est plus ré-annoncé à chaque oscillation d'IP. La ré-annonce est **debouncée** (au plus une fois toutes les 30 s, `kMdnsReannounceMinIntervalMs`) et n'a lieu que si l'IP a **réellement** changé, ce qui supprime le churn `MDNS.end/begin` en roaming CPL/mesh.

### Ajouté

- **Auto-guérison mémoire** : l'appareil **se répare désormais tout seul** en cas de fragmentation critique du tas. Quand le plus gros bloc contigu libre reste sous 8 Ko (`kRestartMaxAllocHeapBytes`) pendant 3 vérifications de santé consécutives (≈ 3 min) et après au moins 3 min de fonctionnement, il effectue un **redémarrage propre** : coupure des pompes, sauvegarde des cumuls journaliers de dosage (anti-sur-dosage), log critique, alerte MQTT `auto_reboot`, puis reboot. Plus besoin de couper le secteur à la main.
- **Mode maintenance anti-boucle** : si l'auto-guérison redémarre **3 fois** (`kAutoHealMaxReboots`) dans une fenêtre glissante de 15 min (compteur en RAM RTC survivant au reboot), l'appareil **cesse d'enchaîner les reboots** et bascule en mode maintenance : pompes coupées, **dosage inhibé sur toutes les voies** (auto, programmé, manuel web, manuel écran/UART, test), auto-guérison désactivée, alerte MQTT `maintenance_mode`. L'appareil **reste joignable** pour diagnostic. Sortie du mode maintenance : redémarrage manuel du contrôleur.
- **Instrumentation du tas** : à chaque vérification de santé (60 s), une ligne INFO persistée `Heap: libre=… maxBloc=… minLibre=…` permet de visualiser la fragmentation. Un flush disque **one-shot** est déclenché au premier franchissement à la baisse du seuil d'alerte (`kLowHeapWarnBytes` = 20 Ko) pour capturer la descente même si le redémarrage n'est pas atteint.

### Documentation

- `docs/adr/0029-auto-guerison-heap.md` : ADR de la décision (auto-reboot sur fragmentation du tas + mode maintenance anti-boucle plutôt que subir le gel ; alternatives écartées ; dépendance au réglage réseau et au pulldown de gate MOSFET).
- `docs/subsystems/system-health.md` : nouvelle fiche (santé système, instrumentation heap, machine à états auto-guérison / maintenance, module pur `self_heal_logic`, debounce mDNS, constantes).
- `docs/subsystems/pump-controller.md` : `setMaintenanceInhibit()` / `isMaintenanceInhibited()` et inhibition du dosage en mode maintenance.
- `docs/subsystems/logger.md` : instrumentation heap et flush one-shot dans `checkSystemHealth()`, debounce de la ré-annonce mDNS.
- `docs/MQTT.md` : alertes `auto_reboot` et `maintenance_mode`.

---

## [2.9.0] - 2026-07-14

### Corrigé

- **Crash / reboots liés aux clients WebSocket** : l'appareil pouvait redémarrer de façon récurrente (`panic → reboot`) et devenir **injoignable** en HTTP/WebSocket jusqu'au redémarrage, notamment lors de la **fermeture ou du timeout d'un client WebSocket** (onglet fermé, WiFi qui décroche, client inactif). Un coredump a identifié un **bug de course connu** des bibliothèques web asynchrones **originales** (`me-no-dev`, non maintenues) : un assert de la pile réseau lwIP au moment du `close()`. C'est la **2ᵉ cause de crash de la pile web** diagnostiquée par coredump après le `std::bad_alloc` sur `/logs` corrigé en 2.8.2.

### Modifié

- **Migration vers le fork maintenu ESP32Async** (ex-mathieucarbou), avec **versions épinglées** : `AsyncTCP@3.4.10` et `ESPAsyncWebServer@3.11.2` (avant : dépôts Git `me-no-dev` non figés). Le fork corrige le bug de course à la source et rend le build reproductible. **Aucun changement de comportement fonctionnel** côté utilisateur (API, UI, MQTT, OTA inchangés) : seule la stabilité s'améliore. Plateforme figée inchangée (`espressif32@6.9.0`). `ESPAsyncWiFiManager` conservé (le portail WiFi est géré nativement par le projet).

### Documentation

- `docs/adr/0028-migration-fork-esp32async.md` : ADR de la décision (migration + épinglage des versions, alternatives écartées, dette de suivi des releases ESP32Async).
- `docs/subsystems/ws-manager.md` : dépendance au fork ESP32Async (versions), mode de défaillance historique (assert lwIP au close/timeout WS) et sa correction.

---

## [2.8.5] - 2026-07-13

### Amélioré

- **Statut de dosage — cause anti-cycling visible** : le widget pH/ORP affichait un statut **vide** quand le dosage était bloqué par une garde anti-cycling (**cycles/jour** atteint, **anti-rafale**), ce qui était ambigu (« ça n'injecte pas et rien n'est indiqué »). Le statut remonte désormais la **cause de refus du firmware** (`ph/orp_DoseBlockedReason`, déjà diffusée) : « Limite cycles/jour atteinte — reprise à minuit », « Anti-rafale — patienter… ». Un statut vide signifie maintenant sans ambiguïté que la mesure est **dans l'hystérésis** (rien à doser). Purement UI.

---

## [2.8.4] - 2026-07-13

### Amélioré

- **Latch d'instabilité EMI pH/ORP auto-levé** : le drapeau qui marque un capteur `unstable` (inhibant la régulation) quand la dispersion de la fenêtre de mesure dépasse le seuil sur 3 fenêtres consécutives (bruit EMI soutenu) était **collant** — seul un `reset()` (reboot ou `POST /debug/sensor_filter_reset`) ou une calibration le levait. Il s'auto-lève désormais après **12 fenêtres pleines propres consécutives** (`kSensorFilterLatchClearCycles`), soit ≈ 2 min de signal réellement propre (60 s + purge de la fenêtre médiane). Répond au cas « **ORP marqué instable alors que l'oscillation est retombée** » sans nécessiter de reset manuel après un burst passé. Asymétrie voulue : rapide à poser (≈ 15 s), lent à lever (anti-flapping).
- **Anti-flapping garanti** : la « clean streak » qui pilote l'auto-levée n'est incrémentée que sur une fenêtre **pleine et sans aucune perturbation** ; **tout** rejet (spike dur isolé, NaN, hors-plage) **ou** tout dépassement de dispersion la remet à 0. Un capteur qui crache des spikes ne peut donc jamais lever le latch à tort. `reset()` reste la levée manuelle immédiate ; les inhibitions indépendantes (`unstable()` sur dispersion courante, rejets consécutifs, ready/frozen) sont inchangées.
- **Logs de transition** (`SensorManager`) : `warning` « Filtre pH/ORP : latch EMI posé… » à la pose, `info` « … latch EMI levé (signal stable)… » à l'auto-levée (une ligne par transition, par capteur).

### Documentation

- `docs/subsystems/sensors.md` : auto-levée du latch EMI (`kSensorFilterLatchClearCycles`, clean streak, logs de transition), accesseur `dispersionCleanStreak()`.
- `docs/adr/0027-filtre-tendance-centrale.md` : note sur l'évolution du latch (auto-levée, dans le cadre de l'ADR).

---

## [2.8.3] - 2026-07-13

### Corrigé

- **Injection manuelle — estimation de durée trompeuse** : le champ d'injection affichait la durée brute pour le volume demandé (ex. « ≈ 35 min » pour 1 L) sans tenir compte du **plafond de 10 min par injection** (`kManualInjectMaxDurationS`) appliqué par le firmware. L'estimation reflète désormais le plafond : au-delà du volume délivrable en une injection, elle affiche la durée réelle (10 min) **et** le volume effectif (« plafonné à ~N mL par injection, répéter pour le reste »). Purement UI ; le plafond de sécurité est inchangé.

---

## [2.8.2] - 2026-07-13

### Corrigé

- **Crash de l'endpoint `/logs`** : la lecture des logs (`/get-logs`, `/download-logs`) plantait par `std::bad_alloc` (PANIC → reboot) sous fragmentation du tas — l'appareil devenait alors injoignable en HTTP alors que MQTT continuait de fonctionner. Cause : `Logger::getRecentLogs()` deep-copiait tout le buffer (avec ses `String`) dans un `std::vector<LogEntry>`, `operator new` levant une exception non rattrapée. La méthode est **supprimée** et remplacée par `Logger::forEachRecentLog()` (itération **en place sous mutex, sans copie**) ; les handlers sont réécrits en **réponse flux** (`beginResponseStream`, dont le buffer grandit par `realloc` — ne lève jamais). Diagnostiqué via coredump.
- **Garde mémoire basse** : nouvelle constante `kMinHeapForLogsResponse` (24 Ko) ; si le plus gros bloc contigu libre est en dessous, `/get-logs` et `/download-logs` renvoient **503** (`{"error":"low_memory"}` / message texte) sans tenter d'allocation.

### Retiré

- **Log de diagnostic `[Diag]`** périodique (introduit temporairement en 2.8.1) — sa raison d'être (diagnostic du crash `/logs`) est levée. La ré-annonce mDNS sur changement d'IP (2.8.1) et le getter `WsManager::clientCount()` sont **conservés**.

### Documentation

- `docs/API.md` : code 503 `low_memory`, comportement flux et limitation de troncature de `/get-logs` et `/download-logs`.
- `docs/subsystems/logger.md` : `forEachRecentLog` (contrat mutex / itération sans copie), garde heap, fix `bad_alloc` ; retrait de la doc du log `[Diag]`.

---

## [2.8.1] - 2026-07-12

### Corrigé

- **mDNS périmé après roaming** : `poolcontroller.local` est désormais ré-annoncé sur **tout changement d'IP** (événement WiFi `STA_GOT_IP`, drapeau consommé dans `loopTask`), y compris lors d'un roaming CPL/mesh **sans** perte de WiFi. Avant, la ré-annonce n'avait lieu que sur reconnexion manuelle → l'appareil pouvait être injoignable en HTTP (IP périmée annoncée) alors que MQTT continuait de fonctionner. Une déconnexion invalide l'IP mémorisée. Log INFO `mDNS ré-annoncé sur <IP> (<raison>)`.
- **Divergence valeur pH/ORP MQTT vs tableau de bord** : les topics MQTT `{base}/ph` et `{base}/orp` publiaient la valeur **brute** alors que le tableau de bord affiche la valeur **filtrée** (lissée) — écart visible sous bruit de mesure (ex. pH 6.39 sur MQTT vs 7.3 dans l'app). Les deux publient désormais la **valeur filtrée identique** (repli sur le brut au démarrage tant que le filtre n'est pas amorcé). Les valeurs brutes restent disponibles sur `{base}/ph_raw` et `{base}/orp_raw`.

### Ajouté

- **Diagnostic** : log INFO périodique (toutes les 60 s) `[Diag] heap=NNNNN min=NNNNN IP=x.x.x.x wsClients=N` (heap libre, min free heap, IP courante, nombre de clients WebSocket) pour diagnostiquer une panne « web injoignable » en distinguant roaming réseau et fuite heap. **Temporaire** — à retirer une fois la cause tranchée. Nouveau getter `WsManager::clientCount()`.

### Documentation

- `docs/subsystems/logger.md` : documentation du log `[Diag]` (format, but, périodicité, caractère temporaire) et de la ré-annonce mDNS sur changement d'IP.

---

## [2.8.0] - 2026-07-11

### Firmware

- **Filtre de mesure pH/ORP recalé sur la tendance centrale (feature-058)** : le filtre (entrée du PID) ne **colle** plus au haut ou au bas d'une oscillation soutenue et ne **gèle** plus sur une valeur trompeuse. Nouvelle chaîne : rejet des **vraies aberrations uniquement** (NaN / hors-plage / spike dur isolé) → **médiane sur fenêtre longue** (13 échantillons ≈ 65 s) → EMA (α pH 0,10 / ORP 0,08). La valeur filtrée est **recalculée en continu**, jamais figée.
- **Inhibition de la régulation conservée mais découplée** : `isPhFilterUnstable()` / `isOrpFilterUnstable()` bloquent toujours le dosage sous bruit extrême via un **drapeau de dispersion** (amplitude `max−min` de la fenêtre > cap, OU latch EMI soutenu, OU rejets consécutifs), sans jamais fausser ni figer la valeur publiée. Invariant iso-sécurité préservé (valeur ∈ [min, max], jamais tirée vers la consigne).
- **Latence +15 s** (fenêtre médiane 7 → 13) — négligeable devant la pause mélange hydraulique post-injection. Module `sensor_filter.*` rendu 100 % pur (testé en natif, 218 tests).
- Suppression de la mécanique re-sync / saut-max / warmup à seuil court (constantes `kSensorFilterMedianWindow`, `kPhFilterMaxStep`/`kOrpFilterMaxStep`, `kSensorFilterWarmupSamples`, `kSensorFilterResyncRejects`, `kSensorFilterMaxResyncPerWindow`, `kSensorFilterResyncWindowMs` retirées).

> ℹ️ Ce filtre livre une valeur **honnête et centrée** malgré le bruit ; le **vrai correctif de la source du bruit reste matériel** (isolation / pompe), traité séparément.

### Documentation

- `docs/adr/0027-filtre-tendance-centrale.md` : nouvel ADR (successeur de la stratégie feature-025) — estimateur central robuste + découplage valeur/latch, alternatives écartées, invariant iso-sécurité.
- `docs/subsystems/sensors.md` : section filtrage réécrite (chaîne, contrat `filtered()`, dispersion/latch découplés, paramètres, latence).
- `docs/API.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md` : références aux constantes supprimées corrigées.

---

## [2.7.0] - 2026-07-11

### Ajouté

- **Trace des signaux capteurs brut / lissé (feature-057)** : nouvelle card « Signaux capteurs (brut / lissé) » dans les réglages **Avancé**, avec 2 graphes haute cadence (pH brut vs lissé, ORP brut vs lissé), stats min/max/Δ, compteur d'échantillons et boutons « Recharger la trace » / « Vider le buffer ». Permet de diagnostiquer visuellement le **bruit** ou une **oscillation** de la chaîne de mesure. Les données proviennent d'un ring buffer firmware de 300 échantillons (~7,2 Ko RAM) alimenté à chaque cycle capteur (~5 s) — bien plus fidèle que l'échantillonnage WebSocket. La trace est chargée à l'activation de l'onglet (pas de polling), et le buffer est volatil (perdu au reboot).
- Réintroduction **fidèle** (et **étendue à l'ORP**) de l'outil « Debug oscillation pH » retiré en v2.5.0 (feature-045). Les outils « pause WiFi » et « diagnostic EZO » **ne sont pas** réintroduits.

### Firmware

- **Capteurs** : ring buffer de trace `SensorManager` (`kSensorTraceBufferSize = 300`), enregistrement lock-free en fin de `_readEzoSensors()` — aucune transaction I²C ajoutée, watchdog non impacté.
- Nouveaux endpoints `GET /debug/sensor_trace` (dump JSON non bloquant, streamé) et `POST /debug/sensor_trace_clear`.

### Documentation

- `docs/API.md` : documentation des routes `/debug/sensor_trace` et `/debug/sensor_trace_clear`.
- `docs/features/page-settings.md` : card « Signaux capteurs (brut / lissé) » de l'onglet Avancé.
- `docs/subsystems/sensors.md` : section « Trace des signaux capteurs » (ring buffer, coût RAM, contrat lock-free).

---

## [2.19.1] - 2026-07-09

### Ajouté

- **Pause mélange visible sur le tableau de bord** : le widget pH/ORP affiche désormais la **pause mélange hydraulique** post-injection (« Pause mélange : X min YY s »), au même titre que la stabilisation. Auparavant ce délai (20 min ORP / 15 min pH) n'apparaissait que dans le modal « État filtre », ce qui donnait l'impression d'un dosage bloqué sans explication. Nouveaux champs WebSocket `ph_mix_remaining_s` / `orp_mix_remaining_s`.

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Cycle de régulation : chronologie stabilisation + pause mélange » (diagramme temporel + cadence de régulation ~1 injection / 20 min en ORP).
- Échos utilisateur dans `docs/features/page-orp.md`, `page-ph.md`, `page-dashboard.md`.

## [2.19.0] - 2026-07-08

### Ajouté

- **Mode d'installation (feature-056)** : nouveau réglage unique décrivant le câblage réel du PoolController, dans une section « Installation » dédiée (1er onglet des Paramètres) et une étape de l'assistant de 1ᵉʳ lancement. Trois choix :
  - **PoolController pilote la filtration** — alimentation permanente + sortie 12 V vers contacteur ; le contrôleur commande le relais, la programmation de filtration est active, le dosage est autorisé quand la filtration commandée est en marche.
  - **Alimenté par le circuit de filtration** — le contrôleur est alimenté en même temps que la pompe ; il ne pilote pas le relais et l'eau est présumée présente en permanence.
  - **Filtration externe signalée** *(nouveau cas)* — le contrôleur est alimenté en permanence et une domotique tierce lui signale l'état de la filtration via `POST /filtration/external-state` ou MQTT `.../filtration_external_state/set`. Sans signal « en marche » récent (au-delà de 3 min), l'eau est considérée absente et **le dosage est suspendu** (sécurité).
- Signalement de l'état de la filtration externe depuis Home Assistant (interrupteur) et sélection du mode d'installation (liste déroulante, libellés en français).
- **Aide intégrée au mode « Filtration externe signalée »** : bloc repliable dans les Paramètres avec des exemples prêts à l'emploi (commande HTTP `curl`, publication MQTT, automatisation Home Assistant), renseignés automatiquement avec l'adresse et le topic MQTT réels du contrôleur.

### Modifié

- La garde de sécurité « présence d'eau » du dosage repose désormais sur une résolution unique selon le mode d'installation (comportement inchangé pour les installations existantes, migration automatique).
- Le mode d'installation remplace et fusionne les anciens réglages « Fonctionnement Piloté/Continu » (section Régulation pH/ORP) et « filtration gérée par le PoolController ».

### Migration

- Mise à jour automatique de la configuration : « Piloté » → *PoolController pilote la filtration*, « Continu » → *Alimenté par le circuit de filtration*. Aucune action requise. Voir `docs/UPDATE_GUIDE.md` pour les cas de câblage particuliers.

## [2.18.3] - 2026-07-07

### Corrigé

- **Mode Boost — texte d'aide incohérent** : la description sous le widget Boost promettait toujours « filtration prolongée + cible ORP relevée », même quand l'effet réel affiché était « Surchloration seule » (filtration non gérée par le contrôleur) ou « Filtration prolongée seule » (ORP non automatique). Le texte est désormais cohérent : masqué quand le Boost est actif (la ligne « Effet » fait foi), et **prospectif honnête** quand inactif (« À l'activation : … ») en fonction de la configuration réelle. Purement UI, aucun impact sur le dosage.

---

## [2.18.2] - 2026-07-07

### Amélioré

- **Mode Boost — indication d'effet persistante (feature-055, finition feature-053/054)** : le widget Boost du tableau de bord affiche désormais **en permanence** les leviers réellement actifs — « Filtration prolongée + surchloration », « Surchloration seule », « Filtration prolongée seule », ou « Sans effet » (badge orange) —, complément persistant du toast fugace de la feature-054. L'info reste visible **après une activation depuis Home Assistant** et **après un rechargement de page**, via deux nouveaux champs WebSocket `boost_filtration_extended` (= filtration gérée par PoolController) et `boost_chlorine_boosted` (= régulation ORP en mode Automatique) calculés au vol. Affichage immédiat au clic Activer (mapping de la réponse `POST /boost/start`). **Purement informatif** : aucune logique de dosage ni de filtration modifiée.

### Documentation

- `docs/subsystems/ws-manager.md` : champs WS `boost_filtration_extended` / `boost_chlorine_boosted` (calculés au vol, buffer 1600 → 1664).
- `docs/features/page-dashboard.md` : ligne « Effet » persistante du widget Boost (4 libellés + badge `--warn`).

---

## [2.18.1] - 2026-07-07

### Amélioré

- **Mode Boost — signalement des leviers inertes (feature-054, finition feature-053)** : à l'activation, le Boost indique désormais quels leviers s'appliquent réellement selon la config. Si la filtration n'est pas gérée par PoolController (`filtration_enabled = false`), la filtration n'est pas prolongée ; si la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé. Deux canaux d'information : (1) `startBoost()` logue un `warning` par levier inerte — couvre l'activation HTTP **et** MQTT/Home Assistant ; (2) la réponse 200 de `POST /boost/start` renvoie `filtration_extended` et `chlorine_boosted`, à partir desquels le dashboard affiche un **toast adaptatif** (nominal « vérifiez le taux de chlore », surchloration seule, filtration prolongée seule, ou « Boost sans effet »). **Purement informatif** : aucune logique de dosage, de garde ni de filtration n'est modifiée ; l'activation n'est jamais bloquée.

### Documentation

- `docs/API.md` : réponse 200 de `POST /boost/start` complétée avec `filtration_extended` / `chlorine_boosted` + note sur le log `warning`.
- `docs/features/page-dashboard.md` : tableau des 4 cas du toast adaptatif du Boost.
- `docs/subsystems/pump-controller.md` : section « Signalement des leviers inertes » (conditions d'effet de chaque levier, warnings `startBoost()`).

---

## [2.18.0] - 2026-07-06

### Fonctionnalités

- **Mode Boost — surchloration temporaire du jour (feature-053)** : après une forte fréquentation, un geste active le Mode Boost pour assainir davantage la piscine **pour la journée** : filtration prolongée (forçage dédié) + cible ORP relevée (`+60 mV`, plafonnée à 850 mV sous l'alerte). Surcouche **non destructive** (la config n'est jamais modifiée) auto-expirant au prochain minuit local. Pilotable depuis le dashboard (carte Boost avec expiration + toast « vérifier le taux de chlore avant baignade ») et depuis Home Assistant (switch « Boost »). L'activation est refusée si l'heure n'est pas synchronisée.

### Firmware

- **Régulation ORP — valeurs effectives boostées (feature-053)** : tant que le Boost est actif et **uniquement en mode de régulation ORP `automatic`**, la cible ORP effective = `min(orp_target + 60, 850)` mV et la limite journalière chlore effective = `min(max_chlorine_ml_per_day × 1,5, 1000)` mL — **bornes de sécurité dédiées figées par pool-chemistry**, jamais abaissées. En Manuel / Programmé, seule la filtration s'étend ; l'injection manuelle reste bornée à la limite normale. **Tous les autres garde-fous sont inchangés** (limite horaire, temps min d'injection, anti-rafale, injection si filtration active, watchdog) : le Boost n'ouvre aucun chemin de dosage. Décision structurante formalisée en [ADR-0025](docs/adr/0025-mode-boost.md).
- **Persistance** : état `boostState` (`active` + `untilEpoch`) persisté en NVS dédié (namespace `poolctrl`) → survit à un reboot dans la journée ; expiration honorée avant le reset des compteurs journaliers dans les deux branches de `tickDailyRollover()`.
- **MQTT / Home Assistant** : switch « Boost » (topic retain `{base}/boost` + commande `{base}/boost/set`, payload `ON`/`OFF`). L'état retombe automatiquement sur `OFF` à minuit. Activation refusée sans heure synchronisée (commande ignorée + resync HA).
- **Web / API** : routes `POST /boost/start` (auth WRITE, `409 time_not_synced` si horloge non synchronisée) et `POST /boost/stop` ; champs `boost_active` / `boost_until` (epoch) dans `/get-config` et le WebSocket.

### Documentation

- **[ADR-0025](docs/adr/0025-mode-boost.md)** créé (Accepté) : surcouche temporaire « valeurs effectives » + relèvement borné de la limite chlore, 4 bornes de sécurité et leur justification, alternatives écartées (mutation de config, relèvement non borné, durée fixe en heures, switch de mode), conséquences (edge boost court le soir, heure synchronisée requise, vérification chimique à la charge de l'utilisateur). Indexé dans `docs/adr/README.md`.
- `docs/MQTT.md` : topic `{base}/boost` + commande `{base}/boost/set`, entité discovery `switch` « Boost », section feature-053 dédiée.
- `docs/API.md` : routes `POST /boost/start` \| `/boost/stop`, champs `boost_active` / `boost_until` de `/get-config`.
- `docs/subsystems/ws-manager.md` : champs WS `boost_active` (bool) + `boost_until` (epoch).
- `docs/subsystems/pump-controller.md` : section Mode Boost (état, cible/limite effectives, expiration minuit, gate mode automatic), 4 constantes ajoutées au récapitulatif.
- `docs/features/page-dashboard.md` : carte Boost (activation, expiration, rappel baignade).

---

## [2.17.3] - 2026-07-06

### Firmware

- **Home Assistant — modes de régulation pH/ORP affichés en français (bug-ha-regulation-mode-labels)** : les selects « Mode Régulation pH » et « Mode Régulation ORP » exposaient dans HA les valeurs brutes anglaises `automatic`/`scheduled`/`manual` (feature-009), alors que l'UI web affiche « Automatique » / « Programmée » / « Manuelle ». Les deux selects affichent désormais ces libellés français via `value_template` / `command_template`, même technique que les modes filtration et éclairage. **Le protocole MQTT sur le fil reste strictement `automatic`/`scheduled`/`manual`** (topics d'état et handlers `PhRegulationMode`/`OrpRegulationMode` inchangés) : aucune automatisation Home Assistant existante n'est cassée.

### Documentation

- `docs/MQTT.md` : options des entités discovery pH/ORP passées aux libellés français, note dédiée aux templates, mise à jour de la note « Mode Filtration » (les selects de régulation ne sont plus en anglais).
- `docs/subsystems/mqtt-manager.md` : section « Selects Mode Régulation pH/ORP » (`value_template`/`command_template`, wire inchangé).

---

## [2.17.2] - 2026-07-06

### Firmware

- **Home Assistant — mode de programmation éclairage en liste déroulante (bug-ha-eclairage-select, finition feature-052)** : la programmation horaire de l'éclairage, jusque-là exposée en `switch` ON/OFF, est désormais un `select` **« Mode Éclairage »** avec les options **« Programmation »** / **« Désactivé »**, aligné sur le select « Mode Filtration ». Même technique que les libellés filtration : `value_template` / `command_template` traduisent l'affichage HA, le protocole sur le fil reste strictement `ON`/`OFF`. Le booléen `scheduleEnabled` et le handler `LightingSchedule` sont inchangés. Migration : un payload retain vide est publié sur l'ancien topic `homeassistant/switch/{id}_lighting_schedule/config` pour retirer le switch orphelin de HA.

### Documentation

- `docs/MQTT.md` : entité éclairage passée de `switch` à `select` « Mode Éclairage » (tableau discovery, section feature-052, note dédiée aux templates et au nettoyage de l'ancien switch).
- `docs/subsystems/mqtt-manager.md` : section « Select Mode Éclairage », migration de l'orphelin, mise à jour du handler et des compteurs.

---

## [2.17.1] - 2026-07-06

### Corrigé

- **Éclairage — rafraîchissement temps réel des champs de programmation (bug-ui-eclairage-refresh-ws, finition feature-052)** : l'écran Éclairage rafraîchit désormais ses champs de programmation (mode, heures début/fin) en temps réel lors d'un changement depuis Home Assistant, comme l'écran Filtration. Auparavant, `loadConfig()` (message WS `config`) repeuplait les champs filtration mais pas éclairage : il fallait quitter/revenir sur l'écran pour voir la valeur à jour. Correctif UI uniquement (`data/app.js`, miroir du mécanisme `filtrationDirty`) ; une saisie éclairage en cours n'est pas écrasée par un message WS. Aucun firmware modifié.

---

## [2.17.0] - 2026-07-06

### Fonctionnalités

- **Éclairage — section Programmation uniformisée avec la filtration (feature-052)** : la carte Programmation de la page Éclairage adopte les mêmes libellés que la filtration — select **« Mode d'éclairage »** avec options **« Programmation »** / **« Désactivé »**, heures **Début** / **Fin** en grille 2 colonnes. Les valeurs sous-jacentes (booléen `lighting_schedule_enabled`) et le backend sont inchangés ; seuls les libellés affichés changent. Le contrôle manuel (Allumer / Éteindre) reste indépendant.

### Firmware

- **Home Assistant — planning éclairage pilotable (feature-052)** : l'activation de la programmation et les heures de l'éclairage sont désormais exposées à Home Assistant, en miroir de la filtration (feature-051). Nouvelle entité `switch` « Programmation éclairage » (topic retain `{base}/lighting_schedule` + commande `.../set`, payload `ON`/`OFF` → booléen `scheduleEnabled`) et 2 entités `text` « Éclairage début » / « Éclairage fin » (`{base}/lighting_start` / `{base}/lighting_end` + `.../set`, format `HH:MM`). Validation `ON`/`OFF` et `HH:MM` (invalide → ignoré + `warning` + resync HA) ; changement persisté en NVS, appliqué via `lighting.update()`, et reflété dans l'UI web sous ≤ 5 s (broadcast WS `config`). `publishLightingState()` enrichie et publiée au cycle périodique. Le booléen `scheduleEnabled` est conservé côté firmware (pas de stringification, cf. feature-028). Aucun chemin de dosage touché.

### Documentation

- `docs/MQTT.md` : 3 topics d'état + 3 commandes `/set` éclairage, 3 entités discovery (1 `switch` + 2 `text` `HH:MM`), section feature-052 dédiée, compte discovery total mis à jour.
- `docs/subsystems/mqtt-manager.md` : `InboundCmdType` `LightingSchedule`/`LightingStart`/`LightingEnd`, handler planning éclairage, `publishLightingState()` enrichie, compteurs de publish (états 23 / discovery 20) et compte discovery (44).
- `docs/features/page-lighting.md` : nouveau libellé « Mode d'éclairage », options Programmation/Désactivé, exposition HA du planning, indépendance du contrôle manuel.

---

## [2.16.0] - 2026-07-06

### Firmware

- **Home Assistant — heures de filtration modifiables (feature-051)** : les heures de début et de fin du créneau de filtration sont désormais exposées à Home Assistant en 2 entités `text` **éditables** (« Filtration début » / « Filtration fin »), complétant le select « Mode Filtration ». Nouveaux topics retain `{base}/filtration_start` / `{base}/filtration_end` (état, format `HH:MM`) et commandes `{base}/filtration_start/set` / `{base}/filtration_end/set`. La saisie est validée `HH:MM` (format invalide → ignoré + `warning` + resync HA sur la valeur réelle) ; le changement efface les overrides marche/arrêt forcés, applique le planning immédiatement, persiste en NVS et se reflète dans l'UI web sous ≤ 5 s (broadcast WS). **Effet réel en mode Programmation** : en mode **Auto**, les heures sont recalculées par la température (une saisie HA est alors consultative et auto-corrigée). Aucun chemin de dosage touché.

### Documentation

- `docs/MQTT.md` : 2 topics d'état + 2 commandes filtration_start/end, 2 entités discovery `text` (pattern `HH:MM`), section feature-051 dédiée, sémantique Programmation/Auto, compte discovery mis à jour (41).
- `docs/subsystems/mqtt-manager.md` : `InboundCmdType` `FiltrationStart`/`FiltrationEnd`, handler groupé (validation `HH:MM` avant mutex, efface overrides, `filtration.update()` hors mutex sur `loopTask`), `publishFiltrationState()` enrichi (4 topics).

---

## [2.15.0] - 2026-07-06

### Firmware

- **Home Assistant — libellés du mode filtration en français et non ambigus (bug-ha-filtration-mode-labels)** : le select « Mode Filtration » exposait dans HA les valeurs brutes `auto`/`manual`/`force`/`off`, alors que l'UI web affiche « Auto / Programmation / Manuel / Désactivé ». Au-delà de l'écart de langue, c'était trompeur : `manual` correspond à « Programmation » (créneau à heures fixées) et `force` à « Manuel » (contrôle ON/OFF sans planning) — l'inverse de l'intuition. Le select HA affiche désormais les **libellés français** via `value_template` (état brut → libellé) et `command_template` (libellé → valeur brute). **Le protocole MQTT sur le fil reste strictement `auto`/`manual`/`force`/`off`** (topic d'état et handler de commande inchangés) : aucune automatisation Home Assistant existante n'est cassée. Les selects de régulation pH/ORP restent en valeurs anglaises (non trompeuses).

### Documentation

- `docs/MQTT.md` : sémantique des 4 modes filtration détaillée sur les topics d'état/commande, note dédiée sur les templates d'affichage du select (libellés français côté HA, wire inchangé).
- `docs/subsystems/mqtt-manager.md` : nouvelle section sur les `value_template`/`command_template` du select `filtration_mode` (traduction affichage uniquement, `doc.clear()` anti-fuite, handler brut inchangé).

---

## [2.14.1] - 2026-07-06

### Firmware

- **MQTT / UI temps réel — synchronisation WebSocket (bug-sync-ws-config-mqtt)** : quand la configuration est changée depuis Home Assistant (consignes pH/ORP, modes de régulation, volumes quotidiens, mode filtration, éclairage), l'UI web se met désormais à jour automatiquement sous ≤ 5 s **sans recharger la page**. Auparavant, seul le chemin HTTP `POST /save-config` déclenchait le broadcast WebSocket `config` ; les commandes reçues par MQTT modifiaient bien la config (persistée + republiée sur le broker) mais ne notifiaient jamais les clients WS connectés. Correctif : `MqttManager::drainCommandQueue()` appelle `wsManager.requestConfigBroadcast()` une fois en fin de drain dès qu'au moins une commande (hors `Reboot`) a été traitée — le message `config` part au prochain cycle WS. Broadcast unique et idempotent (coalescé), aucun contrat modifié.

### Documentation

- `docs/subsystems/mqtt-manager.md` : `drainCommandQueue()` déclenche un broadcast WS `config` après une commande modifiant la config (cohérence UI temps réel, même effet que `POST /save-config`).
- `docs/subsystems/ws-manager.md` : le chemin MQTT (`drainCommandQueue`) documenté comme second déclencheur du message `config`.

---

## [2.14.0] - 2026-07-06

### Firmware

- **MQTT / Home Assistant — complétude de couverture (feature-050)** : issu d'un audit web/API vs MQTT/HA (périmètre « rentable »). Ajout de 5 entités auto-discovery HA. **Cumuls journaliers de dosage exposés** : nouveaux topics retain `{base}/ph_daily_ml` et `{base}/orp_daily_ml` (cumul injecté en mL, dédup au cycle 10 s, retombe à 0 à minuit) + 2 `sensor` HA « Dosage pH/Chlore aujourd'hui » (`state_class: measurement`). **Volumes quotidiens programmés commandables** : les topics `{base}/ph_daily_target_ml` / `{base}/orp_daily_target_ml` deviennent commandables via `.../set` + 2 `number` HA « Volume quotidien pH/Chlore » (min 0 / max 2000 / step 10 mL) — payload non numérique ignoré + `warning`, clamp négatif → 0, **REFUS si valeur > limite journalière VIVE** (`maxPhMlPerDay` / `maxChlorineMlPerDay`) + resync HA (condition pool-chemistry), persistance NVS. **Bouton Redémarrer** : topic `{base}/reboot/set` + `button` HA `device_class: restart` — redémarrage différé propre (flush MQTT `offline` puis restart, même séquence que `POST /reboot`).

### Documentation

- `docs/MQTT.md` : nouveaux topics d'état et de commande, 5 entités discovery (compte total 39), section feature-050 détaillée, et **nouvelle section « Exclusions volontaires »** documentant comme une politique assumée ce qui n'est délibérément pas exposé en MQTT/HA (injections manuelles, limites de sécurité, calibration/OTA/factory-reset/Wi-Fi/auth, transients d'affichage, historique).
- `docs/subsystems/mqtt-manager.md` : `InboundCmdType` `PhDailyTarget`/`OrpDailyTarget`/`Reboot`, handlers volume quotidien (garde limite vive) et reboot différé (flag consommé en tête de drain), compte discovery mis à jour.

---

## [2.13.0] - 2026-07-06

### Firmware

- **Infrastructure — repartitionnement flash, layout v4 (feature-049)** : les slots firmware `app0`/`app1` passent de 1664 à **1792 KB** (+128 KB chacun), pris sur la partition `spiffs` réduite de 576 à **320 KB** (offset déplacé à `0x390000`) — rendu possible par la pré-compression gzip (feature-048, payload FS 155 KB, soit 48 % de la nouvelle partition). Occupation firmware ≈**78,5 %** (marge par slot 264 → ~392 KB). Partitions `nvs`, `history` et `coredump` strictement inchangées : **config, historique et coredump préservés**. ⚠️ **Mise à jour par câble USB requise une seule fois** (`./deploy.sh all`) — l'OTA ne peut pas réécrire la table de partitions ; **ne jamais pousser le FS v4 en OTA sur un appareil en table v3** ; ne pas utiliser `./deploy.sh factory` (efface la NVS). L'OTA redevient normal ensuite. Quatuor aligné : `partitions.csv`, `platformio.ini` (`filesystem_size = 327680`), `build_fs.sh` (`-s 327680`), `deploy.sh` (`LITTLEFS_OFFSET=0x390000`, contrôle de taille 327680 avec reconstruction automatique d'une image de mauvaise taille).

### Documentation

- Nouvel [ADR-0024](docs/adr/0024-partitions-layout-v4.md) : partitions app à 1792 KB (layout v4) — contexte (gzip feature-048 → `spiffs` occupée à 27 %), alternatives écartées (statu quo — marge suffisante aujourd'hui mais flash USB groupable avec la release ; +64 KB — demi-mesure ; reprendre `history` — données à préserver), conséquences (flash USB unique, interdiction FS v4 en OTA sur table v3, granularité 64 KB des slots). [ADR-0019](docs/adr/0019-partition-app-1664k.md) marqué « Superseded by ADR-0024 ».
- `docs/UPDATE_GUIDE.md` : nouvelle note « Migration layout v3 → v4 (v2.13.0) » — procédure USB, interdiction explicite de l'OTA v4-sur-v3 (métadonnées LittleFS discordantes), vérifications post-flash (Infos système : FS ~320 KB, historique présent).
- `docs/BUILD.md` : taille filesystem 327 680 o = 320 KB (layout v4, ADR-0024), occupation firmware actualisée (~78,5 %, marge ~392 KB).

---

## [2.12.1] - 2026-07-06

### Frontend

- **Performance — assets web pré-compressés gzip (feature-048)** : `minify.js` gzippe (zlib niveau 9) tous les fichiers texte (`.js`, `.html`, `.css`) après minification — seule la variante `.gz` est embarquée dans le filesystem, images/icônes inchangées. ESPAsyncWebServer sert nativement ces fichiers avec `Content-Encoding: gzip` sur tous les chemins (routes HTML explicites, `serveStatic`, portail captif) — **aucune modification firmware**. Payload FS **443 → 155 KB** (partition `spiffs` 78 % → 27 %), **chargement des pages ~4× plus rapide** (le débit de l'ESP32 est le facteur limitant sur mobile). Routes API/JSON non concernées ; un `curl` brut sur un asset statique reçoit du gzip (utiliser `--compressed`).

### Documentation

- `docs/BUILD.md` : étape gzip du pipeline FS (`minify.js` → `data-build/` `.gz` → `build_fs.sh`), règle « jamais de fichier texte non compressé dans `data-build/` », caveat `curl --compressed`.
- `docs/subsystems/web-server.md` : mécanisme de résolution `.gz` d'ESPAsyncWebServer sur les deux chemins de serving (références lib vérifiées).

---

## [2.12.0] - 2026-07-06

### Frontend

- **UX — remplacement des 43 dialogues natifs (feature-031)** : plus aucun `alert()` / `confirm()` dans l'UI. Les **31 `alert()`** informatifs passent par les toasts `showToast` existants (mapping `error`/`success`/`info` selon le contexte) ; les **12 `confirm()`** (actions destructives : purge historique, effacement logs/coredump, déconnexion Wi-Fi, redémarrage, reset sondes, reset usine…) passent par une nouvelle **modale de confirmation maison** `confirmDialog({title, message, confirmLabel, cancelLabel, danger}) → Promise<boolean>` — `<dialog id="confirm-modal">` statique (pattern `probe-modal`), focus initial sur **Annuler**, fermeture `Escape`/backdrop = annulation, réentrance refusée, contenu injecté en `textContent` (anti-XSS), fallback `window.confirm` si `<dialog>` indisponible, **variante danger (bouton rouge)** pour les 7 actions irréversibles. Refus = défaut sûr sur les 12 flux async convertis (revue site par site). Le **reset usine conserve sa double confirmation** (« Réinitialiser » puis « Tout effacer »). Emojis ⚠ retirés des messages — le rouge porte la sévérité.
- **Page « À propos » mise à jour** : retrait des crédits obsolètes DFRobot_PH et Adafruit ADS1X15 (matériel remplacé par les modules Atlas Scientific EZO en I²C natif depuis la feature-021) et de Playwright (jamais utilisé) ; ajout des modules EZO et des tests natifs Unity (185 tests, CI).

### Documentation

- `docs/features/README.md` : nouvelle section « Composants transverses » — API du helper `confirmDialog`, règle « toute action destructive passe par `confirmDialog` ».
- `docs/features/page-settings.md` : confirmations via modale maison (logs, coredump, reset usine à double confirmation).

---

## [2.11.2] - 2026-07-06

### Firmware

- **Sécurité — comparaison de token à temps constant (feature-028)** : le token API n'est plus comparé via `==`/`!=` sur `String` (court-circuit au premier octet différent → canal temporel sur le contrôle d'accès central). Nouvelle comparaison `constantTimeEquals` (`auth.cpp`) : longueur vérifiée d'abord (non secrète — format fixe 32 hexa), puis accumulation XOR/OR de **tous** les octets sans court-circuit. Appliquée à l'auth **HTTP** (`checkTokenAuth`) ET à l'auth **WebSocket** via la nouvelle API publique `AuthManager::secureTokenEquals()` — la revue de code a relevé que le WS comparait encore le token en `!=` (corrigé avant merge). Plus aucune comparaison directe du token dans le code. Code mort de debug (`maskedReceived`/`maskedExpected`) supprimé. Protections préservées : refus du token en query param, token jamais loggué en clair.
- **Sécurité — retrait complet du mécanisme CORS ([ADR-0023](docs/adr/0023-politique-cors-retrait.md))** : le CORS était partiellement implémenté et trompeur (seul le **premier** origin de la liste était posé, statiquement, sans validation par requête — le multi-origines configuré était silencieusement ignoré). Décision : **politique même-origine stricte** (l'UI est servie par l'ESP32, offline first). Retirés : bloc `DefaultHeaders` CORS + `setCorsHeaders` (code mort), champ `AuthConfig::corsAllowedOrigins` + persistance NVS (valeur existante orpheline, pas de migration), champ `auth_cors_origins` de `/get-config`/`/save-config` et du message WebSocket `config`, preflight `HTTP_OPTIONS`. En-têtes CSP / `X-Content-Type-Options` / `X-Frame-Options` conservés. Toute intégration cross-origin future : reverse proxy ou nouvel ADR.

### Frontend

- **Page Paramètres → Sécurité** : card CORS supprimée (champ `#auth_cors_origins` + bouton `#cors-save-btn`) — mécanisme retiré côté firmware.

### Documentation

- Nouvel [ADR-0023](docs/adr/0023-politique-cors-retrait.md) : retrait complet du CORS, politique même-origine stricte (alternatives écartées : réparation multi-origines par requête — ESPAsyncWebServer s'y prête mal, besoin inexistant ; origin unique assumé — mécanisme conservé sans besoin réel).
- `docs/subsystems/auth.md` : section « Comparaison de token à temps constant » + règle « jamais `==`/`!=` sur le token » ; `docs/subsystems/ws-manager.md`, `docs/subsystems/web-server.md`, `docs/features/page-settings.md`, `docs/API.md` : politique même-origine documentée, mentions CORS retirées.

---

## [2.11.1] - 2026-07-06

### Firmware

- **Fiabilité — plus aucune attente infinie de mutex (feature-027)** : les **24 prises de mutex** encore en `portMAX_DELAY` (mqtt_manager ×9, logger ×7, history ×6, config ×2) sont désormais **bornées** — la `loopTask` ne peut plus être figée jusqu'au reset watchdog (30 s) par une contention (ex. mutex tenu pendant une I/O LittleFS lente). Dégradation gracieuse par site, comportement nominal strictement inchangé :
  - **Logger** (`kLoggerMutexTimeoutMs = 100 ms`) : politique **silencieuse anti-récursion** (jamais de `systemLogger` dans les chemins d'échec) — entrée perdue comptée dans le compteur diagnostic interne `_droppedLogs`, lectures → résultat vide, `clearAll()`/`flushToDisk()` abandonnent **avant** toute modification (pas d'état incohérent).
  - **Historique** (`kHistoryMutexTimeoutMs = 2000 ms`, dimensionné sur consolidation + `saveToFile` ~1–1,5 s) : point/consolidation sautés avec **retry naturel** (`lastRecord`/`lastSave` non avancés), lectures → vecteur vide, `clearHistory()` devient `bool` — la route `POST /history/clear` répond **`503` « Historique occupé — réessayer »** en cas d'échec.
  - **Config** (`kConfigMutexTimeoutMs`, récursif) : `saveMqttConfig` → `error` (config RAM appliquée, persistance NVS perdue) ; `saveProductConfig` → dirty flag conservé → retry au prochain passage.
  - **MQTT** : snapshots de publication sautés (republication naturelle au prochain cycle, topics retain) ; commande HA reçue → **abandon complet** (aucun état partiel) + **resync immédiat** de l'entité HA sur l'état réel (`publishFiltrationState()`/`publishTargetState()`).
  - Warns de timeout **throttlés à 1/min/site** (`kMutexTimeoutWarnThrottleMs = 60000`). Audit `grep portMAX_DELAY src/` à blanc. Revue de concurrence complète (Approved), 185/185 tests natifs verts.

### Documentation

- `docs/subsystems/logger.md`, `docs/subsystems/history.md`, `docs/subsystems/mqtt-manager.md` : stratégie de timeout mutex et politique d'échec par site (dont `_droppedLogs` write-only volontaire et pattern accepté de lecture sans mutex dans le resync filtration).

---

## [2.11.0] - 2026-07-06

### Firmware

- **Sécurité — vérification d'intégrité SHA-256 avant flash OTA (feature-026, [ADR-0022](docs/adr/0022-verification-integrite-ota.md))** : la règle de sécurité #6 (« toujours vérifier la signature/CRC avant flash ») est désormais satisfaite. L'empreinte SHA-256 de chaque flux OTA est calculée **au fil de l'eau** (`OtaStreamHasher`, mbedtls accéléré matériellement, non bloquant pour AsyncWebServer) et comparée à l'empreinte de référence — le champ `digest` (`sha256:<64hex>`) publié par l'API GitHub Releases pour chaque asset (présence vérifiée sur la release réelle v2.5.2, zéro changement du process de release). **Chemin GitHub fail-closed strict** : `/check-update` relaie les champs additifs `firmware_digest`/`filesystem_digest`, l'UI les repasse en paramètre `digest=` **requis** de `/download-update` (même modèle de menace que l'URL déjà relayée : whitelist d'hôtes + auth CRITICAL). Digest absent → `400 integrity_digest_missing` (refus avant toute connexion) ; malformé → `400 integrity_digest_invalid` ; mismatch (image corrompue/tronquée, coupure WiFi incluse) → **firmware** : `Update.abort()` avant tout `end()` — l'image n'est **jamais activée**, version courante conservée ; **filesystem** : `422` sans restart, remontage LittleFS best-effort, réinstallation FS requise (firmware intact et bootable). **Upload manuel `/update`** : empreinte toujours calculée et **loggée** ; champ POST `sha256` optionnel (mismatch ou valeur malformée → flash refusé) — fail-open documenté, l'utilisateur est physiquement présent. ArduinoOTA/espota inchangé (déjà protégé par le MD5 du protocole). Tout refus émet un log `CRITICAL` avec empreintes attendue/calculée. Logique pure `ota_integrity_logic` (parse, comparaison **temps constant**, formatage — pattern ADR-0017), **22 tests natifs ajoutés** (185/185 verts).
- **Corrigé — verrou anti-upload concurrent sur `/update`** : deux uploads simultanés corrompaient l'état statique du handler (hasher, empreinte attendue, progression). Une seule session propriétaire à la fois (`g_uploadOwner`) : le 2ᵉ client reçoit **`409`** ; déconnexion du client en cours de transfert → `Update.abort()` + réarmement du dosage (`setOtaInProgress(false)`) + libération du verrou ; owner périmé repris si `!Update.isRunning()`.
- **Corrigé — progression entre deux uploads (AC5)** : le throttle de log de progression (`static lastLog`) n'était pas réinitialisé entre deux uploads sans reboot — désormais remis à zéro à chaque début d'upload.
- **Corrigé — pompes inhibées après échec `Update.begin()`** : `setOtaInProgress(true)` n'était pas réarmé sur ce chemin d'échec — les pompes doseuses restaient coupées jusqu'au reboot. Appariement `setOtaInProgress` garanti sur tous les chemins terminaux (relevé en revue).

### Frontend

- **Page Mise à jour — fail-closed visible (feature-026)** : bouton **Installer désactivé** avec message explicite si la release GitHub ne fournit pas d'empreinte d'intégrité ; messages d'erreur français distincts sur refus (`Empreinte d'intégrité absente ou invalide…`, `Échec de la vérification d'intégrité — image refusée, version actuelle conservée.` + invite à réinstaller le filesystem le cas échéant) ; le digest est transmis automatiquement à `/download-update`.

### Documentation

- Nouvel [ADR-0022](docs/adr/0022-verification-integrite-ota.md) : SHA-256 comparé au digest de l'API GitHub, fail-closed chemin GitHub, empreinte loggée + param optionnel en upload manuel, transit du digest par l'UI (alternatives écartées : `Update.setMD5` — ne couvre pas le flux FS écrit via `esp_partition_write` ; re-fetch API côté firmware — double le pic RAM TLS ; signature asymétrique — feature ultérieure ; relecture flash post-write — redondante). Conséquences assumées : anciennes releases sans digest refusées par l'UI ; usure flash sur digest faux (image écrite avant refus).
- `docs/subsystems/ota-manager.md` : politique d'intégrité par chemin, verrou anti-upload concurrent, appariement `setOtaInProgress`, constantes `kOtaSha256HexLen`/`kOtaSha256Prefix`, logs CRITICAL.
- `docs/API.md` : champs `firmware_digest`/`filesystem_digest` de `/check-update`, param `digest=` requis de `/download-update` (erreurs 400/422), param `sha256` optionnel et `409` de `/update`.
- `docs/UPDATE_GUIDE.md` : section « Vérification d'intégrité SHA-256 » (cas mismatch firmware/FS, release sans digest, upload manuel) + dépannage.

---

## [2.10.1] - 2026-07-06

### Firmware

- **Corrigé — alerte « limite journalière atteinte » répétée toutes les 60 s** : après le latch de la limite journalière pH ou ORP, le health check de `main.cpp` (60 s) republiait l'alerte MQTT `ph_limit`/`orp_limit` (topic `{base}/alerts`, non-retained) et son log `WARN` **à chaque cycle**, indéfiniment. L'alerte n'est désormais émise qu'à la **transition** vers la limite atteinte (edge-trigger, même flag que l'alarme écran UART) — une seule ligne WARN par épisode. L'état permanent pour Home Assistant reste porté par le binary_sensor `{base}/ph_limit` / `{base}/orp_limit` (retain, inchangé). Après un reboot avec limite déjà latchée : une alerte unique au premier health check (~60 s), voulu. Alarme écran UART : comportement identique.
- **Corrigé — libellé pH codé en dur** : le message d'alerte disait « Limite journalière pH- atteinte » même en configuration pH+. Libellé désormais dynamique (« pH+ » / « pH- » selon `phCorrectionType`, même règle que le log CRIT de `pump_controller`). Libellé ORP (« chlore ») inchangé, toujours exact.

### Documentation

- `docs/MQTT.md` : sémantique de transition des alertes `ph_limit`/`orp_limit` précisée (événement non-retained vs binary_sensor retain, libellé dynamique).

---

## [2.10.0] - 2026-07-05

### Firmware

- **Sécurité — détection capteur pH/ORP figé (feature-022 re-scopée, P2.1/P2.2)** : un module EZO qui répond sans erreur I²C mais retourne indéfiniment la **même valeur** (variance nulle — électronique figée, sonde HS côté BNC) est désormais détecté : nouvelle classe pure `FrozenDetector` (« run ancré » O(1) : bornes `runMin`/`runMax` + longueur de run, ré-ancrage dès qu'une lecture sort de la bande) composée dans `SensorFilter` et alimentée **uniquement par les lectures brutes acceptées** (l'état figé persiste pendant une rafale de rejets). Figé = **30 lectures valides** dans une bande < epsilon (`kSensorFrozenSamples` = 30 ≈ 2,5 min ; `kSensorFrozenEpsilonPh` = 0.0005 = ½ LSB EZO pH ; `kSensorFrozenEpsilonOrp` = 0.05 mV = ½ LSB EZO ORP — un capteur vivant bruite ≥ ±1 LSB, un toggle de 1 LSB casse le run, validé pool-chemistry). Figé ⇒ `ready()` = false ⇒ **dosage bloqué fail-closed** par la garde `FilterNotReady` existante (aucune modification des gardes verrouillées) ; **sortie immédiate sans re-warmup** dès une lecture vivante. Log `critical` edge-triggered `[SENSOR_FROZEN]` (+ `info` à la levée) et alerte MQTT retain `{base}/alerts/sensor_frozen` (JSON, clear payload vide). Consigne terrain : en cas de faux positifs, **durcir N, jamais élargir epsilon**.
- **Température figée warning-only (P2.3)** : `FrozenDetector` dédié à la sonde DS18B20 « eau » (N = 900 ≈ 30 min à 2 s, epsilon 0.03 °C < ½ LSB 12 bits, alimenté par les valeurs brutes non arrondies) — log `warning` seulement, **aucun impact dosage/filtration** (effets réels : compensation EZO pH + planning auto filtration), hors alerte MQTT `sensor_frozen`.
- **MQTT/HA — états problème capteur (P1.1)** : 2 nouveaux topics state retain `{base}/ph_sensor_problem` / `{base}/orp_sensor_problem` (`ON` = capteur **stale OU figé**, `OFF` sinon, publication dédupliquée) + 2 entités auto-discovery `binary_sensor` `device_class: problem` (« Capteur pH — problème » / « Capteur ORP — problème », `unique_id: poolcontroller_ph|orp_sensor_problem`) — l'indisponibilité capteur devient visible dans HA sans parser les JSON d'alerte.
- 11 tests natifs ajoutés (FrozenDetector + intégration SensorFilter), 163/163 verts. Validé pool-chemistry (GO, 4 conditions levées).

### Frontend

- **Dashboard — segment « Capteur indisponible » (P1.2)** : nouveau segment **danger, priorité 0 (la plus haute)** dans le badge multi-états des cartes pH/ORP, piloté par les valeurs `ph`/`orp` `null` du WS (capteur stale ou figé) — aucun champ WS nouveau. Garde anti-faux-positif au boot (aucun segment tant qu'aucun message WS reçu). Échelle des priorités désormais : 0 = capteur indisponible, 1 = hors plage, 2 = limite journalière, 3 = cumul ≥ 75 %, 4 = stock faible (toujours max 2 segments affichés).

### Documentation

- `docs/MQTT.md` : nouvelle section feature-022 (2 topics `*_sensor_problem` + 2 entités discovery + alerte `alerts/sensor_frozen` ajoutée au tableau des alertes dédiées).
- `docs/subsystems/sensors.md` : nouvelle section « Détection capteur figé » (FrozenDetector, run ancré, ½ LSB, intégration `ready()`, sortie immédiate, température warning-only, consigne durcir-N) ; API, états du filtre et constantes complétés.
- `docs/subsystems/mqtt-manager.md` : blocs de publication problème capteur / sensor_frozen, caches de dédup, discovery 32 → 34 entités.
- `docs/features/page-dashboard.md` : segment « Capteur indisponible » + échelle de priorités 0-4.

Écartés explicitement (documentés dans la spec) : compteurs de refus par cause, refresh périodique du cache cal_points, topics `dose_refusal`, ring RAM injections.

---

## [2.9.2] - 2026-07-05

### Firmware

- **Corrigé — écrêtage au reliquat pour l'injection manuelle** : demander une injection manuelle supérieure au reliquat du quota journalier (ex. limite 50 mL, cumul réel 39,4 affiché « 39 », demande de 11 mL) était refusée avec un message contradictoire « reste disponible aujourd'hui : 11 mL » (`lroundf` arrondissait le reliquat au plus proche alors que la garde raisonnait en volume exact). Désormais, `/ph|orp/inject/start` **écrête automatiquement la demande au reliquat** (durée recalculée arrondie **vers le bas** : le volume effectif ne re-déborde jamais la limite) au lieu de refuser — nouveau log info « [Injection] pH|ORP : volume écrêté de X à Y mL (reliquat journalier) ». En fin naturelle, le crédit plancher (v2.9.1) porte le cumul **exactement à la limite** → latch `ph/orp_limit_reached` + badge « Limite journalière atteinte ». Le refus `daily_limit` ne subsiste que si le reliquat est **nul ou < 1 s de pompe**, et son message/champ `remaining_ml` arrondissent désormais **vers le bas** (`floorf` — plus jamais de promesse intenable). Cas double-clamp (reliquat > 10 min de pompe) : le plafond `kManualInjectMaxDurationS` gagne, crédit = volume effectif. Garde pure `evaluateManualInject()` inchangée ; symétrie pH/ORP + chemin legacy `?duration=` ; logs de démarrage d'injection à 1 décimale (reco cosmétique code-reviewer). Validé pool-chemistry (GO), 152/152 tests natifs verts.

### Documentation

- `docs/API.md` : écrêtage automatique au reliquat documenté sur `/ph|orp/inject/start` ; sémantique **floor** du `remaining_ml` du 409 `daily_limit`.
- `docs/subsystems/pump-controller.md` : section « Crédit plancher » complétée (écrêtage → `creditMl = reliquat` entier ; double-clamp → `effectiveMl`).
- `docs/features/page-ph.md`, `page-orp.md` : mention de l'écrêtage automatique dans la garde journalière prédictive.

---

## [2.9.1] - 2026-07-05

### Firmware

- **Corrigé — sous-compte du cumul journalier en fin d'injection manuelle volumée** : demander exactement le reliquat du quota journalier (ex. limite 50 mL, injection 50 mL — frontière `==` acceptée depuis feature-006) laissait le cumul de sécurité à 49,x mL : l'intégration débit × temps perdait la dernière tranche (pompe coupée avant le tick de comptage suivant) et l'arrondi à la seconde → `ph/orp_limit_reached` jamais latché, badge dashboard « Cumul 98 % » figé au lieu de « Limite journalière atteinte ». À la **fin naturelle** d'une injection manuelle volumée, le compteur journalier de sécurité est désormais **planché au volume promis** (`creditManualInjectionFloor` : `daily = max(daily, startCumul + effectiveMl)` ; rollover minuit pendant l'injection → volume entier crédité au jour nouveau, conservateur). **Jamais de crédit sur arrêt anticipé** (stop manuel `/inject/stop`, interruption filtration) : l'intégration réelle fait foi. Propriété validée pool-chemistry (GO) : le registre de sécurité **ne sous-compte jamais** (sur-compte possible de quelques dixièmes de mL) ; le suivi produit (stock) reste sur l'intégration réelle (légère sous-estimation cumulative possible, assumée). Nouveau log info « [Sécurité] Compteur journalier pH|ORP ajusté de X à Y mL (crédit fin d'injection manuelle) ». Symétrie pH/ORP. 152/152 tests natifs verts.

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Crédit plancher à la fin naturelle » (propriété fail-safe, tableau des 3 chemins d'arrêt, rollover minuit, suivi produit non planché et dérive de stock, nouveaux logs) ; `creditManualInjectionFloor` ajoutée à l'API publique.

---

## [2.9.0] - 2026-07-05

### Frontend

- **Dashboard — badge multi-états des cartes pH/ORP (feature-012, périmètre réduit sur avis ux-ui-designer)** : nouveau segment **« Cumul XX % »** (warning) affiché à partir de **75 %** du quota journalier (même arithmétique que la barre des pages /ph et /orp : arrondi des entrées, plafond 100 %), masqué quand la limite est atteinte (le segment « Limite journalière atteinte » prend le relais), quand la limite n'est pas configurée ou données indisponibles. Règle de composition **formalisée** (`composeSensorBadge`) : priorité hors plage (danger) > limite atteinte > cumul ≥ 75 % > stock faible, **maximum 2 segments** (troncature aux 2 plus prioritaires), variant `danger` si un segment danger retenu sinon `warning`. Changement visible : « hors plage + limite atteinte » simultanés affichent désormais **les 2 segments** (avant : seul « hors plage »). 100 % UI (`data/app.js`), aucun changement firmware.
- **Dashboard — stabilisation par pompe (feature-012)** : le statut « Stabilisation : X min YY s » des cartes pH/ORP consomme les champs **par pompe** `ph/orp_stab_remaining_s` (feature-006, repli sur le champ global pour les firmwares plus anciens) — chaque carte affiche la stabilisation de **sa** pompe (une calibration ORP n'affiche plus « Stabilisation » sur la carte pH, et inversement).

### Documentation

- `docs/features/page-dashboard.md` : nouvelles sections « Badge multi-états des cartes pH/ORP » (4 segments, priorité, max 2, seuil 75 %) et « Statut dosage et stabilisation par pompe » ; champs WS consommés complétés.

---

## [2.8.0] - 2026-07-05

### Firmware

- **Régulation — répartition 24 h du mode Programmée (feature-011)** : le volume quotidien du mode `scheduled` (pH et ORP) n'est plus injecté d'un bloc en début de filtration (pic de concentration localisé) mais **réparti par fenêtres de 15 min** alignées sur l'horloge (`kScheduledWindowMinutes`) sur la plage de filtration restante, **bornée à minuit** ([ADR-0021](docs/adr/0021-repartition-scheduled.md)). Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — auto-corrigé après changement de cible, injection manuelle comptée dans le cumul, retard subi ou reboot) est borné par le **budget horaire partagé** ([ADR-0020](docs/adr/0020-budget-horaire-dosage-unique.md), warning edge-triggered si tronqué) et **reporté** si l'injection durerait moins de 30 s (`minInjectionTimeMs`, anti short-cycling — avec une pompe rapide et une petite cible, les premières fenêtres sont reportées et les injections démarrent quand l'horizon se resserre : comportement voulu). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info` dans `tickDailyRollover`). Décision extraite en fonction pure testable `evaluateScheduledDose()` (`dosing_logic`, pattern ADR-0017) + helper pur `remainingRangeMinutes()` (`schedule_logic`) ; fail-closed strict (watchdog inactif, heure locale invalide, horizon ou débit nuls → aucune injection, warning unique). Le **ring anti-rafale partagé est consulté avant tout démarrage** (6/min, 20/15 min, mêmes seuils que `canDose`) ; l'exemption `cyclesToday` de la branche scheduled est **conservée et documentée** (démarrages bornés structurellement ≤ 4/h par le cadencement — arbitrage R4 validé pool-chemistry). Mode filtration `continu` : horizon = fin de journée (le dosage hors plage reste permis). Validé pool-chemistry (GO, 5 conditions levées) ; **17 tests natifs ajoutés** (12 `evaluateScheduledDose` + 5 `remainingRangeMinutes`), 152/152 verts.
- **WebSocket** : nouveaux champs `ph_scheduled_flow_ml_per_min` / `orp_scheduled_flow_ml_per_min` (float 1 décimale = débit moyen planifié restant ; `null` hors mode scheduled / hors plage / heure invalide). Buffer `sensor_data` 1472 → 1536 octets.

### Frontend

- **Cartes Régulation pH/ORP — bloc Programmée (feature-011)** : nouvelle ligne « Débit calculé : X,X mL/min » (« — » avec infobulle « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Pas d'affichage « sur Y h restantes » (l'horloge du navigateur peut différer de celle de l'ESP32).

### Documentation

- Nouvel [ADR-0021](docs/adr/0021-repartition-scheduled.md) : répartition scheduled par fenêtres de 15 min alignées horloge (snapshot `stopTargetMl`, report < 30 s, pas de rattrapage J+1, exemption `cyclesToday`, horizon minuit en continu ; alternatives écartées : PID slow imbriqué, fenêtres relatives au début de plage, rattrapage J+1, fenêtre 30 min, limite de cycles dédiée, recalcul continu sans snapshot). Note de renvoi ajoutée dans [ADR-0002](docs/adr/0002-mode-programmee-volume-quotidien.md).
- `docs/subsystems/pump-controller.md` : §Mode scheduled réécrit (algorithme pur + coquille, ring consulté, exemption `cyclesToday` justifiée, nouveaux logs edge-triggered, `kScheduledWindowMinutes`, accesseurs `get*ScheduledPlannedFlow`).
- `docs/subsystems/filtration.md` : `remainingRangeMinutes` ajouté au tableau des fonctions pures de `schedule_logic`.
- `docs/features/page-ph.md`, `page-orp.md` : mode Programmée détaillé (répartition, report, budget horaire, reliquat, nuance pompe rapide) + ligne « Débit calculé » et hint.
- `docs/API.md`, `docs/subsystems/ws-manager.md` : champs WS `ph/orp_scheduled_flow_ml_per_min`.

---

## [2.7.0] - 2026-07-04

### Firmware

- **MQTT/HA — modes de régulation commandables (feature-009)** : deux nouvelles entités Home Assistant `select` (« Mode Régulation pH », « Mode Régulation ORP », options `automatic` / `scheduled` / `manual`) permettent de changer `ph_regulation_mode` / `orp_regulation_mode` depuis HA (automatisations d'hivernage, absence longue, dashboards). Nouveaux topics de commande `{base}/ph_regulation_mode/set` et `{base}/orp_regulation_mode/set` (non retain, souscrits à la connexion) ; les topics state existants sont **inchangés** (retain, rétrocompat dashboards HA). Validation stricte de l'enum (insensible à la casse ; valeur invalide → ignorée + log `warning`), application dans `drainCommandQueue()` (loopTask, sous `configMutex`) par la **même voie que l'UI web** — miroir `ph/orp_enabled` ([ADR-0004](docs/adr/0004-mode-regulation-enum-3-valeurs.md)) — avec persistance NVS et log `info` uniquement si changement réel, puis republication immédiate de l'état (feedback HA). Un changement pendant une injection en cours ne l'interrompt pas : le nouveau mode prend effet au cycle de régulation suivant (validé pool-chemistry). L'UI web reste la source principale et reflète un changement initié par HA via les canaux existants (WS + `/get-config`) — aucun code UI ajouté.

### Documentation

- `docs/MQTT.md` : 2 entités `select` mode de régulation (discovery, state/command topics, options) + topics de commande ajoutés aux tableaux ; nouvelle section de détail feature-009.
- `docs/features/page-ph.md`, `page-orp.md` : tableau « Interaction MQTT » complété — mode commandable depuis HA, UI web source principale, reflet via WS.
- `docs/subsystems/mqtt-manager.md` : nouveaux `InboundCmdType::PhRegulationMode` / `OrpRegulationMode`, comportement du handler (validation, miroir ADR-0004, persistance conditionnelle, feedback `publishTargetState()`, logs), cas valeur invalide.

---

## [2.6.0] - 2026-07-04

### Firmware

- **Sécurité — injections manuelles gardées (feature-006)** : les injections manuelles (`POST /ph/inject/start`, `POST /orp/inject/start`) sont désormais soumises aux mêmes limites de sécurité chimique que la régulation automatique — limite journalière **prédictive** (cumul + volume demandé, avec reliquat dans le refus), limite horaire (**budget partagé** avec la régulation auto, [ADR-0020](docs/adr/0020-budget-horaire-dosage-unique.md) : le manuel n'était auparavant ni vérifié ni **compté**, permettant jusqu'à 2× le budget/heure), stabilisation post-calibration (par pompe), refus de double démarrage, cycles/jour et anti-rafale (ring **partagé** avec l'auto). Décision extraite en fonction pure testable `evaluateManualInject()` (`dosing_logic`, pattern ADR-0017 ; ordre des gardes et frontières `==` acceptées validés pool-chemistry ; 17 tests natifs dédiés, 135 au total, couverture `dosing_logic` 100 %). Tout refus retourne **409 JSON structuré** `{"error","message","seconds_remaining"?,"remaining_ml"?}` (8 codes) ; la garde filtration des routes de test `/pumpN/on` migre au même format. Les pompes test et commandes UART consomment aussi le budget horaire partagé. `inject/stop` reste un coupe-circuit jamais gardé.
- **fix(limites)** : le drapeau « limite journalière atteinte » (`ph/orp_limit_reached` — badges UI, blocage du bouton d'injection, alarme UART, MQTT) était **verrouillé jusqu'à minuit** : augmenter la limite journalière en cours de journée débloquait bien le dosage réel (gardes en valeurs vives) mais l'UI restait bloquée sur « limite atteinte ». Le drapeau suit désormais la condition vive et se lève dès que `cumul < limite` (limite augmentée ou compteurs réinitialisés) — validé pool-chemistry (indicateur miroir, les gardes bloquantes étaient déjà vives).

### Frontend

- **Injection manuelle — refus détaillés dans l'UI (feature-006)** : toast rouge français par code de refus (reliquat affiché pour `daily_limit`, compte à rebours pour `stabilization_in_progress`) ; bouton « ▶ Injecter » **désactivé proactivement** avec raison en hint (`#ph|orp_inject_block_hint`) quand le blocage est connu côté client (limite journalière, stabilisation, filtration arrêtée). Le firmware reste l'autorité (WS déconnecté → bouton cliquable). Le bouton « ⏹ Arrêter » n'est jamais désactivé.

### Documentation

- Nouvel [ADR-0020](docs/adr/0020-budget-horaire-dosage-unique.md) : budget horaire de dosage unique partagé auto + manuel (alternative « compteur séparé » écartée ; prédicat safety-tracking → pompes test/UART comptent aussi).
- `docs/API.md` : sections `/ph|orp/inject/start` réécrites — tableau des 8 codes de refus 409 JSON, note `/pumpN/on`.
- `docs/features/page-ph.md`, `page-orp.md` : avertissements « limites volumétriques non gardées » retirés (obsolètes) ; gardes, toasts par code et blocage proactif du bouton documentés.
- `docs/subsystems/pump-controller.md` : nouvelle section « Gardes des injections manuelles » (ordre des 9 gardes, frontières figées, budget horaire partagé, pending atomique, asymétrie stabilisation auto/manuel, note bypass UART `pump_test`).

---

## [2.5.2] - 2026-07-04

### Fonctionnalités

- **Infos système — ligne « Firmware (flash) » (feature-047)** : la carte Infos système (Paramètres → Système) affiche l'occupation de la partition app active au format `X KB / Y KB (Z%)` (taille du binaire / taille de la partition OTA courante), au même format que la ligne FS. Rend visible la marge flash gagnée par le repartitionnement layout v3 ([ADR-0019](docs/adr/0019-partition-app-1664k.md)). Côté API, `GET /get-system-info` expose un nouveau champ `sketch_size` (octets, `ESP.getSketchSize()`) ; le champ `ota_partition_size` existait déjà. Affichage `—` si les champs sont absents (compatibilité ancien firmware).

### Documentation

- `docs/API.md` : champs `sketch_size` et `ota_partition_size` ajoutés à l'exemple de `/get-system-info`.
- `docs/features/page-settings.md` : ligne « Firmware (flash) » documentée dans la carte Infos système.

---

## [2.5.1] - 2026-07-04

### Fonctionnalités

- **Paramètres — réorganisation des onglets Système / Avancé (feature-046)** : la carte « Infos système » (Firmware, Build, Uptime, Chip, CPU, Heap libre, Flash, FS, Wi-Fi RSSI, MAC + bouton Actualiser) est **déplacée** de l'onglet Avancé vers la **première position** de l'onglet Système ; la carte « Version du firmware », redondante, est **supprimée** de l'onglet Système (la version reste visible sur la ligne « Firmware » d'Infos système) ; la carte « Historique des mesures (température, pH, ORP) » (export/import/suppression CSV) est **déplacée** de l'onglet Système vers la **première position** de l'onglet Avancé. Frontend uniquement — aucun changement backend (`GET /get-system-info`, `/get-history`, `/history/import`, `/history/clear` inchangés).

### Documentation

- `docs/features/page-settings.md` : contenu des onglets Système et Avancé mis à jour (Infos système en tête de Système, Historique des mesures en tête d'Avancé, position de la card Diagnostic crash dans Avancé).

---

## [2.5.0] - 2026-07-04

### Firmware

- **Retrait des outils de diagnostic d'oscillation pH (feature-045)** : les trois outils ajoutés pour la campagne de diagnostic 2026-05/06, devenus inutiles en exploitation, sont supprimés :
  - **Debug oscillation pH** : carte UI du panel Avancé + ring buffer 300 échantillons (~6 KB RAM) + routes `GET /debug/ph_trace` et `POST /debug/ph_trace_clear` ;
  - **Pause WiFi** : routes `GET`/`POST /debug/wifi_pause` + mécanisme de pause associé ;
  - **Diagnostic EZO** : carte UI + routes `POST /debug/ezo_command` et `POST /debug/ezo_factory`.

  Les 5 routes répondent désormais **404**. Gains mesurés : **Flash −12 272 o**, **RAM statique −6 024 o**, **payload FS minifié −13 809 o**. Restent en place : `/debug/ph_slope_refresh`, `/debug/sensor_filter_reset`, `/debug/sensor_filter_state` (levier de sécurité feature-025) et les routes coredump.

  > **Réintégration** : code récupérable via `git revert` du commit de la feature-045 ; état complet avec les outils figé au tag `v2.4.0`.

### Documentation

- `docs/API.md` : sections « Diagnostic Atlas EZO » et `ph_trace`/`ph_trace_clear` retirées, remplacées par une note « Routes de debug supprimées (v2.5.0) ».
- `docs/features/page-settings.md` : cartes « Debug oscillation pH » et « Diagnostic EZO » retirées du panel Avancé (note de réintégration ajoutée).
- `docs/BUILD.md` : décompte des graphiques uPlot ramené à 6 (le chart debug oscillation est retiré).

---

## [2.4.0] - 2026-07-04

### Firmware

- **Repartitionnement flash — layout v3 (feature-044)** : les slots firmware `app0`/`app1` passent de 1536 à **1664 KB** (+128 KB chacun), pris sur la partition `spiffs` réduite de 832 à **576 KB** (offset déplacé à `0x350000`) — rendu possible par la migration uPlot (feature-043, payload FS 449 KB). Occupation firmware : 90,8 % → **83,8 %** (1 427 753 / 1 703 936 o, marge ~270 KB) ; FS à ~82 % de 576 KB (marge ~104 KB). Partitions `nvs`, `history` et `coredump` strictement inchangées : **config, historique et coredump préservés**. ⚠️ **Mise à jour par câble USB requise une seule fois** (`./deploy.sh all`) — l'OTA ne peut pas réécrire la table de partitions ; ne pas utiliser `./deploy.sh factory` (efface la NVS). L'OTA redevient normal ensuite. Fichiers alignés : `partitions.csv`, `platformio.ini` (`filesystem_size = 589824`), `build_fs.sh`, `deploy.sh`.
- **`deploy.sh`** : correction du check de taille de `littlefs.bin` resté au layout v1 (1 114 112 → 589 824 octets) — il déclenchait à tort une reconstruction du FS à chaque upload.

### Documentation

- Nouvel [ADR-0019](docs/adr/0019-partition-app-1664k.md) : partitions app à 1664 KB (layout v3) — contexte (app0 à 90,8 %, gains `CORE_DEBUG_LEVEL` −33 KB et uPlot −148 KB FS), alternatives écartées (palier +64 KB conservateur ; +192 KB impossible, FS à 100 %), conséquences (flash USB unique, données préservées). [ADR-0015](docs/adr/0015-partition-app-1.5mb.md) marqué « Superseded by ADR-0019 ».
- `docs/BUILD.md` : taille filesystem 589 824 o = 576 KB (layout v3, ADR-0019), piège « changement de table = flash USB », occupation firmware actualisée.
- `docs/UPDATE_GUIDE.md` : nouvelle note « Migration layout v2 → v3 (v2.4.0) » — procédure USB, préservé/réécrit, avertissement `factory`.

---

## [2.3.0] - 2026-07-04

### Frontend

- **Graphiques — migration Chart.js → uPlot (feature-043)** : la bibliothèque de graphiques passe de Chart.js v4.5.1 (`chart.umd.min.js`, ~208 KB, **supprimé**) à **uPlot 1.6.32** (`uPlot.iife.min.js` + `uPlot.min.css`, paquet npm `uplot@1.6.32` figé, committés dans `data/` — offline, sans bundler). **Rendu à parité stricte** sur les 7 graphiques (3 mini-charts dashboard avec coloration conditionnelle par segment et dégradé de remplissage, 3 historiques détail pH/ORP/Température avec bandes min/max, 1 debug oscillation multi-axes avec séries masquées par défaut) : labels X catégoriels (« Aujourd'hui », mois français, `HH:MM`, `-Ns`), tooltip, boutons de plage synchronisés, gel pendant calibration et polling incrémental 5 min conservés. **Payload FS réduit de −148,3 KB** (601 054 → 449 177 octets, ≈ 449 KB / partition `spiffs` 832 KB) — prépare le repartitionnement `app0`/`app1`. Les options mortes de l'ancienne usine `createLineChart` (`hideYAxis`, `showYAxisGrid`, `extraPlugins`, `annotation`, `backgroundColor`) ne sont pas reprises.

### Documentation

- Nouvel [ADR-0018](docs/adr/0018-migration-uplot.md) : uPlot au lieu de Chart.js (contrainte Flash/FS — alternatives Chart.js slim et statu quo écartées).
- `docs/BUILD.md` : nouvelle section « Bibliothèque de graphiques uPlot » (provenance npm `uplot@1.6.32` figé, procédure de mise à jour manuelle, gain mesuré, options non reprises) + correction de la taille de la partition `spiffs` (832 KB, layout v2 — la mention 1088 KB était obsolète).
- `docs/features/page-dashboard.md`, `page-ph.md`, `page-orp.md`, `page-temperature.md`, `page-settings.md` : mentions Chart.js remplacées par uPlot.

---

## [2.2.11] - 2026-06-27

### Firmware

- **Refactor interne (feature-041) — math d'agrégation de l'historique testable** : la math **scalaire** d'agrégation est extraite de `history.cpp::consolidateData()` vers le module **pur** `src/history_logic.{h,cpp}` (headers C uniquement `<stdint.h>`/`<math.h>`, sans `<vector>`/`<map>`/Arduino/FreeRTOS, pattern *Humble Object*) : `bucketTimestamp` (troncature au bucket horaire/quotidien, garde `bucketSeconds==0`), `isOlderThan` (prédicat d'ancienneté `(now-ts) > maxAge`, frontière **stricte**, wrap `uint32` conservé), `finalizeMean` (moyenne, `count==0` → `NaN`), `isMajority` (majorité stricte `> total/2`, division entière), `anyTrue` (`>0`). Les deux passes (raw → horaire, horaire → quotidien) **délèguent** ; le regroupement `std::map`/`std::vector`, l'accumulation et la persistance restent dans la coquille. *Characterization refactor* : **aucun changement de comportement** (frontières strictes, divisions entières et wrap reproduits à l'identique — revue **Approved**). Aucun comportement visible utilisateur, aucun endpoint / WS / MQTT touché.

### Tests

- **25 tests Unity natifs** (feature-041) couvrant la math d'agrégation : troncature au bucket (garde division par zéro), prédicat d'ancienneté (frontière `==`, wrap post-`0xFFFFFFFF`), moyenne (`count==0` → `NaN`), majorité stricte (`2/4`, `3/4`, `3/5`) et `anyTrue`. `history_logic.cpp` couvert à **100 % des lignes** (**118 tests au total**). `coverage.sh` et `build_src_filter` étendus au nouveau module. Le regroupement `std::vector`/`std::map` de `consolidateData()` reste **hors tests natifs** (contrainte libc++ absente en natif).

### Documentation

- `docs/subsystems/history.md` : nouvelle section « Math d'agrégation pure (`history_logic`) » — les 5 fonctions pures, leurs frontières (strictes, divisions entières, wrap `uint32`, `NaN`), la délégation des deux passes, et la note explicite que le regroupement `std::vector`/`std::map` reste hors tests natifs. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.

---

## [2.2.10] - 2026-06-27

### Firmware

- **Refactor interne (feature-040) — logique d'horaire de l'éclairage testable** : la décision d'horaire de l'éclairage est extraite de `lighting.cpp` vers le module **pur** `src/schedule_logic.{h,cpp}` (créé en feature-038, **réutilisé** ici). Nouvelle fonction `decideLightingOn(manualOverride, enabledFlag, scheduleEnabled, haveTime, nowMin, startMin, endMin, currentlyOn)` ; `isMinutesInRange` gagne un 4ᵉ paramètre `bool equalMeansAlways = false`. La divergence `start == end` entre les deux domaines est **intentionnellement préservée** : filtration → `false` (plage invalide, jamais) ; éclairage → `true` (allumé toute la journée). `lighting.cpp::update()` devient une coquille mince déléguant ; RTC/`millis()`/`digitalWrite`/`publishState`/MQTT restent dans la coquille. *Characterization refactor* : **aucun changement de comportement** (ni éclairage ni filtration — la valeur par défaut `false` garantit la non-régression de la filtration). Aucun comportement visible utilisateur, aucun endpoint / WS / MQTT touché.

### Tests

- **11 nouveaux tests Unity natifs** (feature-040) couvrant la décision d'horaire éclairage : équivalence stricte de `decideLightingOn`, divergence `start == end` (éclairage → `true`, filtration → `false`), fenêtre simple et fenêtre franchissant minuit, priorités manuel/horaire, conservation de l'état quand l'heure est indisponible. `schedule_logic.cpp` reste couvert à **100 % des lignes** (**93 tests au total**).

### Documentation

- `docs/subsystems/lighting.md` : nouvelle section « Logique d'horaire pure (`schedule_logic`) » — `decideLightingOn`, paramètre `equalMeansAlways`, divergence intentionnelle `start==end` (filtration `false` / éclairage `true`), non-régression filtration, testabilité native. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.
- `docs/subsystems/filtration.md` : la ligne `isMinutesInRange` documente le 4ᵉ paramètre `equalMeansAlways` et la divergence éclairage.

---

## [2.2.9] - 2026-06-27

### Firmware

- **Refactor interne (feature-039) — anti-rafale & rollover journalier testables** : la logique d'**anti-rafale** du dosage (comptage de cycles sur fenêtres glissantes 1 min / 15 min via ring buffer, gestion du **wrap `millis()`** en `uint32_t`, écriture circulaire) et les **déclencheurs de rollover journalier** (reset des quotas à minuit date NTP / fallback 24 h) sont extraits de `pump_controller.cpp` vers le module **pur** `src/dosing_logic.{h,cpp}` : `countCyclesInWindow`, `recordCycleTimestamp`, `shouldRolloverByDate`, `shouldRolloverByMillis` — sans dépendance Arduino / `millis()` / `time()` / NVS / état membre, donc **testables en natif**. Les méthodes `recordDosingCycleStart` / `countRecentDosingCycles` / `tickDailyRollover` deviennent des coquilles minces (fournissent `millis()` / `time()` / les buffers et délèguent ; `localtime_r`/`strftime`, écriture `safetyLimits`, `saveDailyCounters`, `armStabilizationTimer`, logs et branche première-init restent dans la coquille). *Characterization refactor* : **seuils et frontières strictement préservés** (6 cycles/min, 20 cycles/15 min, ring buffer 20, rollover 24 h) — la garde anti-emballement de `canDose()` (#14/#15) et le reset des quotas sont inchangés (équivalence stricte validée par `pool-chemistry`, 2 passages). Aucun comportement visible utilisateur.

### Tests

- **15 nouveaux tests Unity natifs** (feature-039) couvrant l'anti-rafale et le rollover : comptage en fenêtre glissante (buffer vide, slots à 0, `ts` dans/hors fenêtre, `ts == now`, **wrap `millis()` post-`0xFFFFFFFF`**), écriture circulaire du ring buffer (avance d'index, écrasement du plus ancien), déclencheurs de rollover par date (vide / identique / différente) et par fallback 24 h (frontière `== 86400000`, wrap). `dosing_logic.cpp` couvert à **100 % des lignes** (**85 tests au total**).

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Anti-rafale & rollover journalier (logique pure) » — les 4 fonctions pures, comptage wrap-safe `uint32_t`, déclencheurs de rollover (date NTP / fallback 24 h), coquilles `pump_controller` qui délèguent, comportement strictement préservé, testabilité native. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.

---

## [2.2.8] - 2026-06-27

### Firmware

- **Refactor interne (feature-038) — logique d'horaire de filtration testable** : la décision d'horaire de `filtration` (parsing `"HH:MM"` `timeStringToMinutes`, appartenance à une fenêtre `isMinutesInRange` gérant le passage minuit, calcul du créneau auto selon température `computeAutoWindow` — durée = temp/2 bornée `[1,24]`, centrée sur `kFiltrationPivotHour`, wrap des bornes dans `[0,24)` —, et décision marche/arrêt `decideFiltrationRun` avec priorités **`forceOn` > `forceOff` > plage** et **conservation de l'état si l'heure est indisponible**) est extraite vers un module **pur** `src/schedule_logic.{h,cpp}` (pattern *Humble Object*), sans dépendance Arduino / RTC / `millis()` / NVS / FreeRTOS — donc **testable en natif**. Module **générique**, réutilisable pour l'éclairage. `update()` / `begin()` / `computeAutoSchedule()` deviennent des coquilles minces ; RTC, `millis()`, NVS, deadband 1 °C, timeout 4 h, stabilisation et commande relais restent dans la coquille. *Characterization refactor* : **aucun changement de comportement** (équivalence stricte validée par `pool-chemistry`, dont la préservation de la garde « présence d'eau » de `canDose()`). Aucun comportement visible utilisateur.

### Tests

- **19 nouveaux tests Unity natifs** (feature-038) couvrant la décision d'horaire : parsing `"HH:MM"` (cas invalides inclus), appartenance à une fenêtre traversant minuit, créneau auto borné/centré, priorités des forçages et conservation de l'état sans heure valide. `schedule_logic.cpp` couvert à **100 % des lignes** (**70 tests au total**).

### Documentation

- `docs/subsystems/filtration.md` : nouvelle section « Logique d'horaire pure (`schedule_logic`) » — les 4 fonctions pures, décision/priorités, temps indisponible → conservation d'état, fenêtre auto centrée et wrap minuit, testabilité native, coquilles `update()`/`begin()`/`computeAutoSchedule()`. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR** ; `schedule_logic` est un module générique partagé filtration/éclairage.

---

## [2.2.7] - 2026-06-27

### Firmware

- **Refactor interne (feature-037) — calcul proportionnel testable** : le cœur du calcul PID de `pump_controller` (terme proportionnel `kp × error`, termes `Ki`/`Kd`, anti-windup par gel/bornage de l'intégrale à `±integralMax`, plancher sortie négative → 0, **bornage final min/max**) est extrait de `computePID` vers une fonction **pure** `computePidPure` (`src/dosing_logic.{h,cpp}`), sans dépendance Arduino / `millis()` / état membre, donc **testable en natif**. L'état PID (intégrale + dernière erreur) est désormais **injecté en paramètre** et **renvoyé** via `struct PidResult`. Le `constrain` de débit qui était appliqué chez les appelants pH/ORP est déplacé **dans la fonction pure** (« Option Y ») : `computePidPure` renvoie le débit **final borné**, et `computePID` ainsi que les deux chemins pH/ORP deviennent des coquilles minces (gestion du temps + état uniquement). `computeFlowFromError` reste du code mort (non extrait). *Characterization refactor* : **les débits calculés et l'évolution de l'intégrale sont strictement préservés** (équivalence stricte validée par `pool-chemistry`). Aucun comportement visible utilisateur.

### Tests

- **Tests Unity natifs étendus (feature-037)** : couverture **100 % des lignes** de `computePidPure` (21 tests et plus) — terme proportionnel, gel de l'intégrale, bornage anti-windup `±integralMax`, plancher 0, bornage final min/max. Le temps et l'état PID étant injectés, chaque branche est exercée sans matériel ni attente réelle. `pio test -e native` continue de couvrir les suites `test/test_native_sensor_filter/` + `test/test_native_dosing/`.

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Calcul proportionnel pur (`computePidPure`) » — fonction pure, état PID injecté/renvoyé (`PidResult`), bornage final intégré (« Option Y »), testabilité native 100 %, `computePID` et chemins pH/ORP réduits à des coquilles. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.

---

## [2.2.6] - 2026-06-27

### Firmware

- **Refactor interne (feature-036) — décision de dosage testable** : la logique de décision « peut-on doser ? » de `pump_controller` est extraite dans un module **pur** `src/dosing_logic.{h,cpp}` (pattern *Humble Object*), sans dépendance Arduino / FreeRTOS / I²C, donc **testable en natif**. `canDose()` devient une coquille mince qui collecte les globals, délègue à `evaluateDose(DoseInputs) → DoseDecision`, puis mappe l'énum de cause vers la chaîne française exposée (valeurs runtime réinjectées). *Characterization refactor* : **aucun seuil, verdict, cause de refus ni ordre d'évaluation des gardes n'a changé** (équivalence stricte validée par `pool-chemistry`). Hystérésis de démarrage/arrêt et temps minimum d'injection également exposés en fonctions pures (`shouldStartDosingPure` / `shouldContinueDosingPure`, temps injecté). Aucun comportement visible utilisateur.

### Tests

- **21 nouveaux tests Unity natifs** (feature-036) couvrant la décision de dosage : chaque cause de refus de `evaluateDose`, l'hystérésis start/stop aux bornes (deadband), et un **verrou de non-régression** du bug pause-mélange v2.2.5 (une injection auto dure ≥ `minInjectionTimeMs` avant l'armement de la pause). `pio test -e native` couvre désormais deux suites (`test/test_native_sensor_filter/` + `test/test_native_dosing/`).

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Décision de dosage : logique pure (`src/dosing_logic`) vs coquille hardware » (frontière POD/String, table énum `DoseRefusal` ↔ cause FR, fonctions pures). Correction d'une imprécision préexistante : limite horaire évaluée avec `>=` (et non `>`).
- `docs/BUILD.md` : section « Tests natifs (hors matériel) » — `pio test -e native` couvre deux suites, `build_src_filter` inclut `sensor_filter.cpp` + `dosing_logic.cpp`.
- `docs/adr/0017-logique-metier-pure-humble-object-testabilite.md` : nouvel ADR « Logique métier pure (Humble Object) séparée de la couche hardware pour testabilité native » (lié à ADR-0016 et feature-025).

---

## [2.2.5] - 2026-06-23

### Firmware

- **Corrections — régulation pH/ORP auto** : la pause mélange hydraulique (15 min pH / 20 min ORP, feature-025) était armée par erreur **au démarrage** de l'injection. Conséquence : la garde `canDose()` (« pause mélange en cours », #5b/#6b) sautait la branche de régulation au cycle `update()` suivant, coupant la pompe après ~un cycle de boucle. Les injections automatiques ne duraient jamais le minimum prévu (`minInjectionTimeMs` = 30 s) → régulation inefficace (le pH/ORP ne convergeait pas vers la cible, chip « Mélange en cours » affiché en continu). La pause est désormais armée à l'**arrêt** de l'injection (homogénéisation post-dose) : `notifyPhDose()` / `notifyOrpDose()` sont appelés quand l'injection en cours se termine, sans interrompre celle-ci. `minInjectionTimeMs` et `shouldContinueDosing` sont de nouveau effectifs. Bug introduit par feature-025 ; **fail-safe** (sous-dosage, aucun risque de surdosage).

### Documentation

- `docs/subsystems/pump-controller.md` : section « Pause mélange hydraulique » corrigée — pause armée à l'arrêt de l'injection (et non au démarrage), interaction avec `minInjectionTimeMs` / `shouldContinueDosing` précisée.

---

## [2.2.4] - 2026-06-23

### Fonctionnalités

- **Uniformisation de l'écran Température** (feature-035) : l'écran Température adopte le même agencement que pH/ORP — un **bloc « stats » en haut** (valeur courante + **chip d'état de calibration** + bouton **« Calibrer la sonde »**), et une **carte de calibration masquée** qui s'affiche au clic (masque les cartes Activation + Historique, bouton **Fermer** pour revenir). La calibration reste **par offset 1 point** (saisie d'une température de référence connue → offset, via `/save-config`) : logique inchangée. Le chip affiche « Calibré · aujourd'hui / il y a N j / le JJ/MM » (vert), « Calibré · ancien (il y a N mois) » si > 6 mois (ambré), ou « Non calibré » (gris). La lecture live de la température reste visible pendant la calibration (pas d'indicateur de stabilité — température lente). Les anciens callouts (`#temp_cal_date_header` / `#temp_calibrated_status`) sont **supprimés** au profit du chip.
- **Désactivation de la sonde** (feature-035) : décocher l'activation masque désormais correctement la carte du tableau de bord ainsi que les blocs stats/historique/calibration de la page Température (la carte d'activation reste accessible pour réactiver).

### Documentation

- `docs/features/page-temperature.md` : réécriture pour le nouvel agencement (feature-035) — bloc stats (valeur + chip de calibration), bouton « Calibrer la sonde », carte de calibration masquée (stepper offset + lecture live + Fermer), états du chip, données consommées et cas limites.

---

## [2.2.3] - 2026-06-23

### Fonctionnalités

- **Calibration guidée pH/ORP accessible dans tous les modes** (feature-034) : la calibration des sondes pH et ORP est désormais accessible quel que soit le mode de régulation (**automatique, programmée, manuelle**) — auparavant limitée au mode automatique. Le mode en cours est **conservé** à la sortie de la calibration (plus de bascule forcée en automatique).
- **Écran de calibration guidé** (feature-034) : nouvel accompagnement par **stepper** avec états visuels (étape faite ✓ / en cours / restante, `aria-current`), **minuterie de stabilisation** par étape d'attente (compte à rebours 60 s, non bloquant — on peut calibrer avant la fin), et **indicateur de stabilité Δ60 s** (amplitude max−min de la mesure brute, purement indicatif). Le readout reste la valeur **brute** (feature-025, non régressé).
- **Sécurité (feature-034)** : le bouton « Calibrer » est désormais aussi bloqué pendant une **injection pH** en cours (le garde n'existait que côté ORP).
- **Chip d'état de calibration** (feature-034, itération 2) : sur les pages pH et ORP, l'état de calibration est désormais résumé par un **chip** dans la rangée de chips (à côté des chips sonde et filtre) — états « Calibré 2/2 » (pH) / « Calibré » (ORP) en vert, « Calibration 1/2 » en ambré (pH partiel), « Calibration requise » en rouge (régulation auto inhibée), « EZO indisponible » en gris, et « Calibration — » tant que les données ne sont pas reçues. Les **3 anciens callouts** (calibré / régulation inhibée / EZO injoignable) sont **supprimés** ; leur information est portée par le chip + un hint texte.
- **Bouton « Calibrer la sonde » repositionné** (feature-034, itération 3) : sur les pages pH et ORP, le bouton de calibration est désormais affiché **sous la rangée de chips** (dans le bloc Statistiques) avec un **libellé fixe « Calibrer la sonde »** (remplace les libellés adaptatifs « Calibrer » / « Continuer la calibration » / « Recalibrer » de l'itération 2). L'état de calibration reste porté par le chip + le hint. Les blocs `#ph-calibration-info` / `#orp-calibration-info` en bas de la carte de régulation sont **retirés**. Le bouton reste **toujours accessible** dans tous les modes (recalibration possible) et **désactivé** pendant une injection en cours.
- **Chip pH unique « sonde + calibration »** (feature-034) : sur la page pH, le chip de calibration séparé est **supprimé** ; son information est **fusionnée** dans le chip sonde (`#ph-probe-chip`) pour lever la redondance. Le chip pH affiche « EZO indisponible » (gris) si EZO injoignable, « Calibration requise » (rouge) si 0 point, « Calibration 1/2 » (ambré) si 1 point, et le **diagnostic de pente** habituel (« Sonde excellente / correcte / usée / à remplacer · N% ») dès 2 points calibrés. La page ORP conserve son chip de calibration dédié (pas de chip sonde côté ORP). Le bouton « Calibrer la sonde » et son hint restent inchangés.

### Documentation

- `docs/features/page-ph.md` / `docs/features/page-orp.md` : calibration accessible tous modes, écran guidé (stepper d'états, minuterie de stabilisation, indicateur de stabilité Δ60 s), garde injection, **chip d'état de calibration** remplaçant les callouts (itération 2), et **bouton « Calibrer la sonde »** repositionné sous les chips avec libellé fixe (itération 3, volet A ; volet B « calibration périmée » abandonné).
- `docs/subsystems/sensors.md` : précision que le reset filtre + warmup post-calibration est **mode-indépendant** (déclenché par le succès EZO).

---

## [2.2.2] - 2026-06-21

### Firmware

- **fix(sensors)/tune** : délai de re-synchronisation du filtre réduit de ~2 min à ~1 min (feature-033) — `kSensorFilterResyncRejects` 24 → 12. Invariant fail-closed préservé (12 > 10 instable, 12 > 7 fenêtre médiane).

### Documentation

- `docs/subsystems/sensors.md` : seuil et délai de re-synchronisation (24 → 12, ~120 s → ~60 s).

---

## [2.2.1] - 2026-06-20

### Firmware

- **fix(sensors)** : correction du gel de la valeur filtrée après immersion en solution de calibration (re-synchronisation + anti-boucle). Un changement réel et durable au-delà de `maxStep` (ex. sonde plongée en solution puis retour en bassin) figeait `filtered` à vie → dosage bloqué en permanence. Le filtre se ré-amorce désormais sur la médiane des bruts rejetés après `kSensorFilterResyncRejects` (= 24, ≈ 120 s) rejets consécutifs, puis repart en warmup. Anti-boucle EMI : au-delà de `kSensorFilterMaxResyncPerWindow` (= 3) re-sync sur `kSensorFilterResyncWindowMs` (= 10 min), le capteur est déclaré instable de façon **latchée** jusqu'à un `reset()` explicite (`POST /debug/sensor_filter_reset` ou calibration réussie). Gardes `canDose()` inchangées.

### Fonctionnalités

- **fix(calibration)** : le readout live de calibration pH/ORP affiche la valeur brute (la valeur lissée figeait l'affichage en changeant de solution étalon).

### Documentation

- `docs/subsystems/sensors.md` : sections re-synchronisation (latch-up résolu) et anti-boucle latché, 3 nouvelles constantes, getters `resyncCount()` / `unstableLatched()`, levée du latch par `reset()`.

---

## [2.2.0] - 2026-06-16

### Fonctionnalités

- **Lissage des mesures pH/ORP** (feature-025) : les pages pH et ORP affichent désormais la valeur **filtrée** (médiane glissante + moyenne exponentielle) en grand, avec une ligne discrète « brut · médiane · maj » pour le diagnostic. Un **chip d'état filtre** à 5 états (`Stabilisation…` / `Mesure stable` / `Pics rejetés` / `Capteur instable` / `EZO indisponible`) résume la santé de la chaîne de mesure ; un clic ouvre un modal détail (brut/médiane/filtré, rejets, âge, raison de blocage du dosage).
- **Debug oscillation pH** : la courbe de diagnostic du panel Avancé trace désormais **deux séries pH** — « pH brut » (bleu) et « pH lissé » (orange) — sur le même axe, avec une ligne de stats « pH lissé min / max / Δ » en complément du brut. Permet de visualiser l'effet du filtre feature-025. Le payload de `GET /debug/ph_trace` expose le nouveau champ `phFiltered` par échantillon.

### Firmware

- **Filtrage capteurs** (feature-025) : nouvelle chaîne `mesure brute Atlas EZO → rejet aberrants → médiane (fenêtre 7) → EMA (alpha pH 0.10 / ORP 0.08) → valeur filtrée`. Classe dédiée `SensorFilter` déterministe (buffer fixe, testable hors matériel). Valeurs brutes conservées pour diagnostic EMI. Filtre réinitialisé (warmup) après calibration pH/ORP réussie. Paramètres centralisés dans `constants.h` (`kSensorFilter*`, `kPhEmaAlpha`, `kOrpEmaAlpha`, plages plausibles, seuils de rejet).
- **Régulation P temporisée** (feature-025, [ADR-0016](docs/adr/0016-regulation-p-temporisee-vs-pid.md)) : le PID consomme désormais la mesure **filtrée et prête** (jamais la brute). Stratégie par défaut proportionnelle pure (`Kp` pH=8 / ORP=0.3, `Ki=0`, `Kd=0` impératif). Anti-windup strict, zone morte = seuil de démarrage, nouvelles gardes `canDose()` fail-closed (filtre non prêt, capteur instable, pause mélange).
- **Pause mélange hydraulique** (feature-025) : après chaque injection, blocage de toute nouvelle décision de dosage le temps de l'homogénéisation du bassin — `kPhMixingDelayMs = 15 min`, `kOrpMixingDelayMs = 20 min` (gérée par timestamps, pas de `delay()`).
- **MQTT** : nouveaux topics retain `{base}/ph|orp_raw|median|filtered|filter_ready|filter_unstable|rejected_count` + `{base}/ph|orp_mixing_delay_active`. Les topics `{base}/ph` et `{base}/orp` publient désormais la valeur filtrée. Auto-discovery HA : sensors « pH/ORP Brut » et « pH/ORP Filtré » + binary_sensors « Filtre pH/ORP Prêt ».

### API

- **WebSocket / `GET /data`** : champs `phRaw/phMedian/phFiltered/phFilterReady/phFilterUnstable/phRejectedCount/phMixingDelayActive/phDoseBlockedReason` (+ équivalents `orp*`). `ph` / `orp` correspondent désormais à la valeur **filtrée** (fallback brut tant que le filtre n'est pas amorcé).
- **`POST /debug/sensor_filter_reset`** : réinitialise les deux filtres (warmup). **`GET /debug/sensor_filter_state`** : état JSON brut des filtres.

### Documentation

- `docs/subsystems/sensors.md` : chaîne de filtrage, classe `SensorFilter`, paramètres, reset après calibration, états warmup/unstable.
- `docs/subsystems/pump-controller.md` : entrée PID filtrée, régulation P temporisée, anti-windup, zone morte, pause mélange, nouvelles gardes `canDose()`.
- `docs/MQTT.md` / `docs/API.md` : nouveaux topics, champs WS et endpoints debug.
- `docs/features/page-ph.md` / `docs/features/page-orp.md` : affichage filtré/brut, chip d'état filtre, modal détail.
- `docs/features/page-settings.md` / `docs/API.md` : card « Debug oscillation pH » à 2 courbes (brut + lissé), champ `phFiltered` du payload `GET /debug/ph_trace`.
- **ADR-0016** créé : régulation P temporisée par défaut (Kp seul, Ki=0, Kd=0) sur mesure filtrée vs PID complet.

---

## [2.1.2] - 2026-05-10

### Sécurité chimique

- **Garde filtration sur l'injection manuelle** — bug observé en production : injection manuelle volumée pH lancée pendant filtration active continuait après l'arrêt programmé de la filtration → surdosage local d'acide dans la zone du retour d'eau (pas de circulation). Refonte de [`src/web_routes_control.cpp`](src/web_routes_control.cpp) avec deux mitigations :
  - **Refus en amont (HTTP 409)** — helper privé `injectionAllowedOrReject(req, tag)` appelé en début des handlers `/ph/inject/start`, `/orp/inject/start`, `/pump1/on`, `/pump2/on`. Vérifie `mqttCfg.regulationMode == "continu" || filtration.isRunning()` (même critère que `PumpController::canDose()`, cohérence des deux gardes). Refus = HTTP `409 Conflict` avec corps texte explicite. Les handlers d'arrêt (`/ph/inject/stop`, `/orp/inject/stop`, `/pump[12]/off`) restent **inconditionnels** — pouvoir arrêter en toute circonstance.
  - **Arrêt cyclique en cours d'injection** — `updateManualInject()` (appelée à chaque tour `loopTask`) interrompt l'injection si la filtration tombe pendant celle-ci. Latence < 100 ms après détection. Logue `critical("[Injection] {pH|ORP} INTERROMPUE — filtration arrêtée (sécurité chimique)")` et publie l'alerte MQTT correspondante.
- **Bornage durée injection 3600 → 600 s** — nouvelle constante `kManualInjectMaxDurationS = 600` ([`src/constants.h`](src/constants.h)) appliquée aux deux routes inject/start. Justification pool-chemistry : 3600 s trop long en cas d'arrêt filtration en milieu de cycle ; 10 min couvrent les usages typiques et l'utilisateur peut toujours relancer.
- **Pas de reprise automatique** après reprise filtration — choix produit. L'injection en cours est perdue, l'utilisateur doit relancer manuellement (toast UI explicite).

> Validation `pool-chemistry` : GO sous conditions, **toutes appliquées**. Cohérence avec la condition #3 de `canDose()` (filtration en marche sauf mode `continu`) — voir [docs/subsystems/pump-controller.md](docs/subsystems/pump-controller.md).

### API

- **`POST /ph/inject/start`, `POST /orp/inject/start`, `POST /pump1/on`, `POST /pump2/on`** : nouveau code retour **`409 Conflict`** « filtration arrêtée — injection refusée pour sécurité chimique » (sauf si `regulationMode == "continu"`). Voir [`docs/API.md`](docs/API.md).
- **`POST /ph/inject/start`, `POST /orp/inject/start`** : paramètre `duration` désormais borné à **600 s** (`kManualInjectMaxDurationS`) au lieu de 3600 s.

### MQTT

Deux nouveaux types d'alerte sur le topic existant `{base}/alerts` (QoS 0, no retain — mécanisme `mqttManager.publishAlert()` inchangé). Aucun nouveau topic, aucune nouvelle entité auto-discovery HA — l'utilisateur peut créer une automation HA sur le topic alerts.

| Type | Condition |
|------|-----------|
| `ph_injection_aborted` | Injection manuelle pH interrompue par la sécurité chimique (filtration arrêtée pendant injection) |
| `orp_injection_aborted` | Injection manuelle ORP/chlore interrompue par la sécurité chimique |

### Frontend

- **Refus 409 au démarrage** : `startInject(product)` dans [`data/app.js`](data/app.js) restaure le bouton et affiche un toast rouge « Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local). »
- **Interruption en cours** : `_onWsLog(entry)` détecte `entry.level === 'CRITICAL'` + message contenant `[Injection]` et `INTERROMPUE` (capté via le canal WS log existant `broadcastLog()`, pas de nouveau message WS) → toast rouge « Injection {pH|ORP/chlore} interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. »
- **Erreurs HTTP autres** : lecture du body texte et affichage si court (< 200 chars), sinon message générique.

### Documentation

- `docs/API.md` : nouveau code 409 documenté sur les 4 routes concernées, mention du nouveau bornage `kManualInjectMaxDurationS = 600 s`, encart sur l'arrêt cyclique automatique et l'alerte MQTT émise.
- `docs/MQTT.md` : ajout des deux types d'alertes `ph_injection_aborted` et `orp_injection_aborted` dans la table des alertes.
- `docs/subsystems/pump-controller.md` : nouvel encart « Garde filtration injection manuelle » dans la section sécurité chimique, ajout de `kManualInjectMaxDurationS` au tableau des constantes.
- `docs/features/page-ph.md` et `docs/features/page-orp.md` : section comportement UI mise à jour (toasts, refus, interruption), avertissement « injection manuelle non gardée » remplacé par la documentation de la nouvelle garde filtration.

### Notes

- Aucun ADR créé : c'est une mitigation d'un bug de sécurité (ajout d'une garde manquante). Pas de contrainte structurante, pas d'alternatives crédibles à arbitrer (`pool-chemistry` impose la garde), pas de verrouillage long terme.
- Pas de spec formelle — fix réactif sur bug observé en production. Le rapport `pool-chemistry` (GO sous conditions) tient lieu de cadrage.

---

## [2.1.1] - 2026-05-10

### Firmware

- **Détection corruption LittleFS au boot** (`3c8b657`) — `HistoryManager::begin()` force maintenant un test write/read sur fichier témoin `/.fscheck` après le mount LittleFS de la partition `history`. Si l'opération échoue (open / write / read / compare KO), la partition est effacée via `esp_partition_erase_range` puis remontée propre. Garde-fou contre un bug LittleFS connu : un FS corrompu peut être monté sans erreur puis crasher en `IntegerDivideByZero` à la 1ʳᵉ écriture (observé sur ESP32 PCB v1 après une longue inactivité). Logs : warning « Partition history corrompue détectée — reformatage automatique » + info « Partition history reformatée et remontée ». ⚠️ L'historique persistant est perdu si le reformatage est déclenché. Coût négligeable en cas nominal (~10 ms, ~500 B flash).
- **Underflow uint32 dans la garde 24 h Slope query** (`933f17c`, feature-024) — `SensorManager::update()` étape 5 calculait `(now - _phSlopeQueriedMs)` avec `now` figé en début de fonction. Après `_processEzoQueue()` qui peut bloquer ~900 ms, le handler met `_phSlopeQueriedMs = millis()` à un instant postérieur à `now` → underflow uint32 → ~4,3 milliards → toujours ≥ 86 400 000 → ré-enqueue immédiat → spam de query `Slope,?` ~1/s, monopolisation du mutex I²C, EZO ORP perturbé (logs « bus I²C dégradé » récurrents). Fix : recalcul de `now` après `_processEzoQueue()` + garde anti-underflow `nowAfterQueue >= _phSlopeQueriedMs`.
- **EZO ORP utilise `R` au lieu de `RT,<temp>`** (`c0f2962`) — `AtlasEzoSensor::readSingle()` envoyait indistinctement `RT,<temp>` aux deux modules Atlas. Sur l'EZO pH (0x63), cette commande compense la T° (Nernst) ET retourne la valeur compensée (statut 1 + payload). Sur l'EZO ORP (0x62), elle est ACCEPTÉE (statut 1 success) mais NE RETOURNE PAS de payload — l'ORP est potentiométrique direct sans compensation T°. Le firmware attendait un payload qui ne venait jamais → fail streak → bus I²C dégradé après 2 cycles → régulation ORP inhibée. Fix : différenciation par adresse I²C — `kEzoPhAddress` → `RT,<temp>`, `kEzoOrpAddress` → `R`. Validation empirique via `/debug/ezo_command` : `0x62 RT,25.0 → status=1 resp=""` vs `0x62 R → status=1 resp="-369.2"`.
- **Rate limit 30 → 120 req/min** (`6c79cfc`) — `kMaxRequestsPerMinute` passé de 30 à 120 (`src/constants.h`). Ancienne valeur trop basse pour navigation UI active normale (page /params ouverte + polls `/get-config` + `/data` + `/coredump/info` + clics → 30-45 req/min observés → warnings « Rate limit dépassé » fréquents). 120 req/min = 2 req/s moyenne reste une protection efficace contre brute-force auth (des années pour craquer un mot de passe 8 caractères) et DOS accidentel. WebSocket `/ws` non comptabilisé (connexion permanente).

### Fonctionnalités

- **Outil diagnostic EZO + endpoint factory reset** (`20e4a9b`) — 2 nouveaux endpoints HTTP (pas d'auth, cohérent avec les autres `/debug/*`) :
  - `POST /debug/ezo_command` — envoie une commande Atlas EZO arbitraire (1-30 chars) à n'importe quelle adresse I²C (`8..119` décimal = `0x08..0x77`) avec délai d'attente paramétrable (50-5000 ms). Retourne `{success, addr, cmd, status_code, status_label, response, raw_hex, delay_ms}`. Permet de diagnostiquer un module qui ne répond pas comme attendu, valider une réponse vide silencieuse, préparer un RMA Atlas avec preuves reproductibles.
  - `POST /debug/ezo_factory?addr=N` — restaure les paramètres usine d'un module Atlas EZO (calibration effacée, adresse I²C par défaut, baud rate UART par défaut, compensation T° reset). Le mode de communication (I²C vs UART) est PRÉSERVÉ — la commande ne touche pas le firmware EZO. Power-cycle ESP32 + recalibration recommandés après usage.
- **Carte UI « Diagnostic EZO »** dans Paramètres → Avancé (sous « Debug oscillation pH ») : sélecteur module (ORP 0x62 / pH 0x63), 10 boutons préprogrammés (`I`, `Status`, `R`, `Cal,?`, `Slope,?`, `L,?`, `L,1`, `L,0`, `Plock,?`, `Find`), champ commande personnalisée (max 30 chars) + délai paramétrable (50-5000 ms), affichage parsé (status code coloré vert/rouge selon code, réponse texte, bytes hex), historique scrollable des 30 dernières commandes (timestamp + cmd + status + réponse). Touche Entrée dans le champ perso → envoie.

### Documentation

- `docs/API.md` : nouvelle section « Outil diagnostic EZO » avec les 2 endpoints `/debug/ezo_command` et `/debug/ezo_factory`, exemples curl complets, table des status codes Atlas.
- `docs/features/page-settings.md` : nouvelle sous-section « Card Diagnostic EZO » sous le panel Avancé.
- `docs/subsystems/sensors.md` : section `readSingle()` mise à jour pour la différenciation pH/ORP, encart sur la garde 24 h `Slope,?` et l'anti-underflow `nowAfterQueue`.
- `docs/subsystems/history.md` : nouvelle sous-section « Détection de corruption LittleFS au boot » détaillant le test `/.fscheck` et la procédure de reformatage automatique.

### Notes

- Aucun ADR créé : les 5 commits sont des mitigations de bugs externes (LittleFS, firmware Atlas EZO), un outil de debug et un ajustement de seuil empirique. Pas de décision structurante avec alternative crédible à arbitrer.
- Build OK, validé en condition réelle avant publication.

---

## [2.1.0] - 2026-05-08

### Firmware
- **Pente sonde pH (feature-024)** — exposition des indicateurs d'usure de la sonde pH Atlas EZO via la commande `Slope,?`. Cache RAM `_phSlopeAcid/Base/Zero/QueriedMs` dans `Sensors`, getters publics `getPhSlopeAcid/Base/Zero()` + `getPhSlopeAgeMs()` + `enqueuePhSlopeQuery()`. Refresh automatique au boot, après chaque calibration EZO réussie (mid/low/clear) et toutes les 24 h (`kPhSlopeQueryIntervalMs = 86_400_000` ms). Dédoublonnage via flag `_phSlopeQueryPending` (anti-spam queue 4 slots). Invalidation cache à NaN après ≥ 2 échecs consécutifs (cohérent avec `_phCalCachedPoints`).
- **`AtlasEzoSensor::querySlope(PhSlopeInfo&)`** — parsing tolérant 2 ou 3 floats sur la réponse `?Slope,<acide>,<base>[,<zéro>]`. Mutex I²C tenu pendant toute la séquence (cmd + delay + read).
- **Feature strictement diagnostique** : aucun impact sur `canDose()` ni le PID. Validation `pool-chemistry` skip (passive).

### API HTTP / WebSocket
- **Nouvel endpoint** : `POST /debug/ph_slope_refresh` (sans auth, cohérent avec autres `/debug/*`) — force une re-query `Slope,?` immédiate. Réponse 200 `{success:true, queued:true}` ou 503 `{error:"queue full or already pending"}`.
- **Champs WS ajoutés à `sensor_data`** (4 champs, `null` si jamais lus / bus dégradé) : `phSlopeAcid` (% 1 décimale), `phSlopeBase` (% 1 décimale), `phSlopeZero` (mV 2 décimales), `phSlopeAgeMs` (entier ms ou `null`). Buffer WS bumpé à 1024 octets (+80).

### MQTT / Home Assistant
- **3 nouveaux topics** retain edge-triggered : `{base}/ph_slope_acid` (1 décimale), `{base}/ph_slope_base` (1 décimale), `{base}/ph_slope_zero` (2 décimales). Publication uniquement à la transition de la valeur arrondie — pas de spam.
- **3 sensors auto-discovery HA** : « Piscine pH Pente Acide » (`mdi:angle-acute`, unit `%`), « Piscine pH Pente Base » (`mdi:angle-obtuse`, unit `%`), « Piscine pH Décalage Zéro » (`mdi:sine-wave`, unit `mV`), tous avec `state_class: measurement`.
- Pas de `binary_sensor` « à remplacer » côté firmware — l'utilisateur peut le créer en automation HA selon ses propres seuils.

### Frontend
- **Chip d'état sonde pH** sur la carte « Lecture pH » (page `/ph`) — bouton focusable sous l'affichage pH 3 décimales. 5 variantes CSS (`chip--probe-good/warn/warn2/bad/unknown`) + classe `chip--probe-stale` (encadré jaune si âge > 36 h). Évaluation dans `data/app.js` (fonction `classifyPhProbe()`, constantes `PH_PROBE_*`) — seuils ajustables sans reflasher.
- **Modal détails** (`<dialog id="ph-probe-modal">`) au clic du chip : pente acide, pente base, décalage zéro, âge dernière vérif + bouton « Rafraîchir » → `POST /debug/ph_slope_refresh` (timeout 8 s, attente que `phSlopeAgeMs` redescende sous 60 s). Fallback non-cliquable si `<dialog>` non supporté (warning console).

### Fixes (correctifs code-reviewer feature-024)
- **Anti-spam HA** : trace brute `Slope,?` passée en `debug` (au lieu de `warning`) — la query auto 24 h faisait remonter du bruit dans le topic `{base}/logs` consommé par HA.
- **Cohérence WS** : `phSlopeAgeMs` désormais `null` si jamais lu (au lieu de `UINT32_MAX`), aligné avec les `phSlope*` nullables.
- **Icônes HA discriminantes** : `mdi:angle-acute` / `mdi:angle-obtuse` / `mdi:sine-wave` (au lieu de `mdi:chart-line` générique partagé).

### Documentation
- `docs/subsystems/sensors.md` : nouvelle section « Pente sonde pH — feature-024 » (méthode `querySlope()`, cache, refresh policy, invalidation), ajout `kPhSlopeQueryIntervalMs` au tableau des constantes, mention dans la liste des consommateurs WS / MQTT.
- `docs/MQTT.md` : nouvelle section « Topics et entités ajoutés en feature-024 » (3 topics + 3 sensors HA), ajout des 3 lignes dans le tableau topics capteurs.
- `docs/API.md` : nouvelle section « Endpoints ajoutés en feature-024 » (`POST /debug/ph_slope_refresh`), nouveaux champs WS dans le tableau payload `sensor_data`.
- `docs/features/page-ph.md` : nouvelle section « Chip d'état sonde » (placement, classification, modal, états stale, fallback `<dialog>`), ajout dans la table Actions et Interaction MQTT.
- Spec `specs/features/done/feature-024-pente-sonde-ph.md` (déplacée depuis `doing/`) avec section « Notes d'implémentation ».

### Build
- `pio run` SUCCESS, **RAM 18.0 %**, **Flash 99.8 %** (marge ~2.6 KB — point d'attention : prochaine feature potentiellement bloquée si pas de gain). `./build_fs.sh` SUCCESS, FS 1.1 MB.

### Risques résiduels
- Format de réponse `Slope,?` à confirmer empiriquement avec EZO réel — tolérance 2 ou 3 floats déjà en place pour les firmwares anciens.

---

## [2.0.0] - 2026-05-06

### Hardware
- **Migration PCB v2 — chaîne pH/ORP refondue sur Atlas Scientific EZO Embedded I²C (feature-021)**. Suppression matérielle de l'ADS1115 et de la sonde pH analogique. Modules EZO pH (`0x63`) et EZO ORP (`0x62`) sur le bus I²C partagé avec le DS3231. Voir [ADR-0014](docs/adr/0014-migration-atlas-ezo.md) — supersedes [ADR-0001](docs/adr/0001-capteurs-analogiques-ads1115.md).

### Firmware
- **Refonte complète chaîne pH/ORP** (`src/sensors.h/cpp`, `src/atlas_ezo.h/cpp`). Mini-classe maison `AtlasEzoSensor` (~80 lignes) encapsulant les commandes Atlas (`R`, `RT,<t>`, `Cal,*`, `Cal,?`, `Cal,clear`, `I`) et les délais EZO (600/900 ms). Mutex I²C tenu pendant la séquence atomique `RT,<temp>` + delay + `R` + delay (cond #6 pool-chemistry).
- **Queue FreeRTOS `_ezoQueue`** (4 slots) pour exécuter les calibrations EZO (~900 ms bloquantes) hors handlers HTTP. Pattern miroir de `mqttTask` ([ADR-0011](docs/adr/0011-mqtt-task-dediee.md)). Routes `/calibrate_*` retournent `{success:true, queued:true}` immédiatement.
- **`canDose(int pumpIndex)` refondu — 10 garde-fous fail-closed** dans l'ordre : index pompe valide, watchdog actif, filtration en marche, lecture pH/ORP non NaN (cond #1 stale 20 s + cond #5 bus dégradé), EZO calibré (cond #2, `cal_points >= 2` pH / `>= 1` ORP, `-1` bloque), pas de stabilisation post-cal en cours (cond #3), mode régulation = `automatic`, limite journalière, limite horaire, anti-rafale court terme (`kMaxDosingCyclesPerMinute=6` + `kMaxDosingCyclesPer15Min=20` via ring buffer 20 entrées par pompe — correctif Pass 3.5). Log « edge-triggered » : 1 entrée par transition de cause de refus.
- **Stabilisation post-calibration par pompe** : `armStabilizationTimer(int pumpIndex)` avec durée différenciée — `kStabilizationDurationPhMs = 5 min`, `kStabilizationDurationOrpMs = 3 min`. Surcharge legacy `armStabilizationTimer()` conservée pour les sites « globaux » (filtration, boot continu, rollover minuit) — applique aux 2 pompes simultanément avec `mqttCfg.stabilizationDelayMin`.
- **Stale timeout** `kSensorStaleTimeoutMs = 20000` ms : `getPh()`/`getOrp()` retournent NaN si dernière lecture > 20 s. Logger `critical` 1× à la transition. Alerte MQTT `pool/alerts/sensor_stale` edge-triggered.
- **Cache `_phCalCachedPoints` / `_orpCalCachedPoints`** mis à jour en `begin()` puis après chaque calibration. Invalidé à `-1` si `_phI2cFailStreak >= kEzoBusFailMaxConsecutive = 2` (durcissement Pass 3.5) → `_lastPh = NaN` ET cache à `-1` simultanément. Rafraîchissement opportuniste à la 1ʳᵉ lecture réussie suivante.
- **Compensation T° pH** : `RT,<temp>` envoyé avant chaque `R`. Source : `getWaterTemperature()` (feature-020). Fallback **25.0 °C** si NaN (sonde eau non identifiée ou en erreur).
- **Suppression** : libs PlatformIO `Adafruit ADS1X15` et `DFRobot_PH` retirées de `lib_deps`. 10 fonctions publiques `Sensors` legacy supprimées (`getRawPh`, `getRawOrp`, `getPhVoltageMv`, `isPhCalibrated`, `getRawTemperature`, `calibratePhNeutral/Acid/Alkaline`, `clearPhCalibration`, `detectAdsIfNeeded`, `recalculateCalibratedValues`, `publishValues`). 7 champs `MqttConfig` ORP/pH legacy supprimés (`orp_cal_*`, `ph_cal_*`).

### API HTTP / WebSocket
- **Nouvelles routes** `POST /calibrate_ph {step:"mid"|"low"}`, `POST /calibrate_orp {reference:<0..1000 mV>}`, `POST /calibrate_clear {sensor:"ph"|"orp"}`. Toutes répondent immédiatement `{success:true, queued:true}` (< 1 ms). 400 si payload invalide, 503 si queue saturée.
- **Routes supprimées** (404 désormais) : `POST /calibrate_ph_neutral`, `POST /calibrate_ph_acid`, `POST /clear_ph_calibration`.
- **Précision pH** : 3 décimales sur tous les contrats publics (`GET /data`, WS `sensor_data`, MQTT `{base}/ph`). Avant : 1 décimale.
- **Champs WS ajoutés** : `phCalPoints` (int `-1..3`), `orpCalPoints` (int `-1..1`).
- **Champs WS supprimés** : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw` (la notion de « valeur brute » n'existe plus côté EZO).

### MQTT / Home Assistant
- **Nouveaux topics** (retain) : `{base}/ph_cal_points`, `{base}/orp_cal_points`, `{base}/alerts/calibration_required` (edge-triggered, payload JSON ou vide=clear), `{base}/alerts/sensor_stale` (idem).
- **Auto-discovery HA enrichi** : 2 nouveaux sensors « Piscine pH Points Calibrés » (`unique_id: poolcontroller_ph_cal_points`, `icon: mdi:numeric`) et « Piscine ORP Points Calibrés ».
- **`{base}/ph`** publié avec **3 décimales** (vs 1 décimale en v1.x). Tout consommateur HA qui parsait `int()` doit basculer sur `float()`.

### Frontend
- **Refonte carte calibration pH** : 2 sous-blocs **parallèles** `.cal-point-block` (point milieu pH 7.00 + point bas pH 4.00). Plus de stepper séquentiel. Chaque bloc : badge état, micro-étapes, readout live, bouton « Calibrer le point X.X ». Workflow : POST `/calibrate_ph` → toast « Calibration en cours… » → polling 15 s sur `phCalPoints` → toast succès. Lectures pH 3 décimales sur les readouts.
- **Refonte carte calibration ORP** : 1 sous-bloc `.cal-point-block` unique avec input `orp-cal-reference` (`0..1000` mV, défaut 470). Suppression du sélecteur 1pt/2pts hérité du firmware v1. Polling 15 s avec fallback succès si `orpCalPoints >= 1` (recalibration ne change pas le compteur).
- **Cartes Régulation pH / ORP** : callout vert « Calibré N points ✓ » si calibration nominale, chip ambrée d'inhibition si `phCalPoints < 2` ou `orpCalPoints < 1`, chip rouge « EZO injoignable » si compteur à `-1`.
- **Suppression** : chip tension pH legacy de la page pH, sélecteur 1pt/2pts ORP.

### Sécurité chimique
- 6 conditions impératives `pool-chemistry` intégrées (#1 stale, #2 cal_points, #3 stabilisation, #4 alerte MQTT, #5 bus I²C dégradé, #6 mutex atomique).
- 2 correctifs additionnels Pass 3.5 : invalidation simultanée cache cal_points + lecture en mode bus dégradé ; ring buffer anti-rafale 6/min + 20/15min.

### Documentation
- **Nouveau** : [`docs/adr/0014-migration-atlas-ezo.md`](docs/adr/0014-migration-atlas-ezo.md) — décision, alternatives écartées (lib EZO_pH, ADS1115 + filtre amélioré, chaîne mixte, tâche FreeRTOS dédiée, exécution synchrone), conséquences, ce que ça verrouille.
- **Annoté Superseded** : [`docs/adr/0001-capteurs-analogiques-ads1115.md`](docs/adr/0001-capteurs-analogiques-ads1115.md) — bandeau « Superseded by ADR-0014 ».
- Mis à jour : [`docs/subsystems/sensors.md`](docs/subsystems/sensors.md) (refonte majeure : architecture EZO, mini-classe `AtlasEzoSensor`, queue, cache cal_points, stale, mutex), [`docs/subsystems/pump-controller.md`](docs/subsystems/pump-controller.md) (10 garde-fous `canDose`, ring buffer anti-rafale, stabilisation par pompe, constantes ajoutées au tableau), [`docs/MQTT.md`](docs/MQTT.md) (4 nouveaux topics + 2 sensors HA), [`docs/API.md`](docs/API.md) (3 routes refondues, payloads `/data` et WS, champs `phCalPoints`/`orpCalPoints`), [`docs/features/page-ph.md`](docs/features/page-ph.md) (refonte UI 2 sous-blocs), [`docs/features/page-orp.md`](docs/features/page-orp.md) (refonte UI 1 bloc), [`docs/BUILD.md`](docs/BUILD.md) (suppression libs ADS1115/DFRobot_PH des `lib_deps`), [`docs/UPDATE_GUIDE.md`](docs/UPDATE_GUIDE.md) (procédure recalibration obligatoire v1.x → v2.0.0), [`docs/adr/README.md`](docs/adr/README.md) (index : ADR-0001 superseded, ADR-0014 ajouté).

### Migration utilisateur — v1.x → v2.0.0 (BREAKING)
- **Recalibration obligatoire** : pH 2 points (mid 7.00 + low 4.00) + ORP 1 point (kit 225 mV ou 470 mV). Tant que ce n'est pas fait, régulation auto inhibée + alerte MQTT `pool/alerts/calibration_required` retain.
- Données NVS legacy (`ph_cal_*`, `orp_cal_*`) supprimées silencieusement au 1ᵉʳ boot — aucune migration possible (chaîne de mesure totalement différente).
- Firmware v2.0.0 conçu pour PCB v2 uniquement. **NE PAS** flasher sur PCB v1 (sondes incompatibles).

### Build
- `pio run` SUCCESS, RAM 16.5 %, **Flash 98.8 %** (marge ~17 KB — point d'attention pour les futures features), 0 nouveau warning. `./build_fs.sh` SUCCESS, FS 1.1 MB.

---

## [Unreleased] - 2026-05-06

### Hardware/Firmware (PCB v2)
- **Support 2 sondes DS18B20 (eau piscine + circuit électronique) avec identification (feature-020)**. Le PCB v2 ajoute une 2ᵉ sonde DS18B20 sur le bus OneWire (GPIO 5 partagé). Identification par adresse ROM persistée NVS via workflow utilisateur (chauffer une sonde dans la main 30 s, cliquer le bon bouton dans Paramètres → Avancé). Auto-permutation activée : si l'utilisateur identifie la sonde A comme « eau » alors qu'une autre était déjà eau, cette dernière bascule automatiquement à « circuit ». Fallback gracieux : `Sensors::getTemperature()` reste un alias de `getWaterTemperature()` avec repli sur la 1ʳᵉ sonde détectée si non identifié, garantissant la rétrocompat MQTT/WS/HA. Voir [ADR-0013](docs/adr/0013-identification-sondes-onewire.md).

### Frontend
- Nouvelle card « Identification des sondes de température » dans Paramètres → Avancé (affichage temps réel adresses ROM + T° brutes, boutons d'assignation, badge état une fois identifiée, bouton réinitialiser). Polling 2 s scoped au panel-dev.
- Chip de notification Dashboard ambré (pattern `.chip` existant) tant que `sondes_identified === false && sondes_detected >= 1`. Clic → redirige vers la card.

### MQTT/HA
- Nouveau topic `pool/sensors/temperature_circuit` (retain) + entité auto-discovery HA « Piscine Température Circuit ». Topic `pool/sensors/temperature` (eau) inchangé (rétrocompat).

### API HTTP / WebSocket
- 3 nouveaux endpoints (auth) : `GET /sensors/onewire/scan`, `POST /sensors/onewire/identify`, `POST /sensors/onewire/reset`.
- 3 nouveaux champs WS `sensor_data` : `temperature_circuit`, `sondes_identified`, `sondes_detected`. Buffer 832 → 896 octets.

### Documentation
- `docs/adr/0013-identification-sondes-onewire.md` créé. ADR-README mis à jour.
- `docs/subsystems/sensors.md`, `docs/subsystems/ws-manager.md`, `docs/MQTT.md`, `docs/API.md`, `docs/features/page-settings.md`, `docs/UPDATE_GUIDE.md` mis à jour.

### Build
- `pio run` SUCCESS, RAM 16.5 %, **Flash 98.5 %** (marge ~21 KB — point d'attention pour feature-021 EZO), 0 nouveau warning. `./build_fs.sh` SUCCESS.

---

## [Unreleased] - 2026-05-05

### BREAKING / Hardware
- **Bascule cible PCB v1 → PCB v2 (feature-019)**. Le firmware n'est plus compatible avec le PCB v1. Mapping GPIO entièrement réassigné : 10 pins actifs (LED=2, OneWire DS18B20=5, I²C SDA=21, I²C SCL=22, pompe pH=25, relais filtration=26, relais éclairage=27, pompe ORP=33, bouton factory reset=35) + 3 pins réservés feature-future (`kRtcSqwPin=23`, `kCtnAuxPin=32` MOSFET 12V tableau, `kRtcIntPin=36`). L'ADS1115 et ses pins associés sont supprimés (les modules Atlas EZO pH/ORP les remplaceront en feature-020). La 2ᵉ sonde DS18B20 est câblée sur le bus OneWire mais ne sera détectée qu'en feature-021. Ce changement est unidirectionnel : la version cible 2.0.0 ne fonctionnera plus sur PCB v1. Voir [ADR-0012](docs/adr/0012-mapping-gpio-pcb-v2.md) pour la justification complète des choix de pin et les alternatives écartées

### Firmware
- **Migration des `#define` de pin vers `constexpr kXxxPin`**. Suppression de 7 `#define` historiques de `src/config.h` (`PUMP1_PWM_PIN`, `PUMP2_PWM_PIN`, `TEMP_SENSOR_PIN`, `FILTRATION_RELAY_PIN`, `LIGHTING_RELAY_PIN`, `BUILTIN_LED_PIN`, `FACTORY_RESET_BUTTON_PIN`) au profit de constantes typées `constexpr uint8_t kXxxPin` regroupées en tête de `src/constants.h` sous une section dédiée « GPIO PIN ASSIGNMENTS - PCB v2 ». Cohérent avec la convention CLAUDE.md (`kConstantes` dans `constants.h`, pas de `#define` dispersés)
  - **Logique du bouton factory reset inversée** : `pinMode(kFactoryResetButtonPin, INPUT)` (au lieu de `INPUT_PULLDOWN` interne v1) car GPIO 35 est input-only et ne supporte pas de pull-up/pull-down interne — un pull-up externe 10 kΩ vers 3V3 sur le PCB v2 est obligatoire. Lecture firmware passée à `pressed = digitalRead(kFactoryResetButtonPin) == LOW` (au lieu de `== HIGH` v1). **Comportement utilisateur identique** : appui maintenu 10 s = factory reset
  - **Convention `pumps[0]` = pH (kPumpPhPin=25), `pumps[1]` = ORP (kPumpOrpPin=33)** figée dans `pump_controller.cpp` `begin()`. Inversion = bug de sécurité chimique (mauvaise pompe activée)
  - **Pins réservés `kRtcSqwPin`, `kCtnAuxPin`, `kRtcIntPin`** déclarés mais sans `pinMode` initial — restent en haute impédance jusqu'à activation par une feature future (économie de courant pull-up, détection facile d'un court-circuit hardware)
  - 7 fichiers touchés : `src/constants.h`, `src/config.h`, `src/pump_controller.cpp`, `src/filtration.cpp`, `src/lighting.cpp`, `src/sensors.cpp`, `src/main.cpp`
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning
  - Tests dynamiques (boot, voltmètre relais, bouton 10 s) délégués à l'humain sur PCB v2 réel — le PCB v1 ne peut plus être testé fonctionnellement

### Documentation
- `docs/adr/0012-mapping-gpio-pcb-v2.md` créé — décision complète, tableau du mapping (13 pins), alternatives écartées (mapping v1 conservé / bouton sur pin avec pull-up interne / bus I²C séparé pour EZO / `pinMode(INPUT_PULLUP)` activé immédiatement sur les pins réservés / `#define` ré-injecté dans `config.h`), conséquences positives et dette assumée, ce que la décision verrouille
- `docs/adr/README.md` : index mis à jour (entrées ADR-0011 et ADR-0012 ajoutées)
- `docs/subsystems/pump-controller.md` : nouvelle section « Mapping pompes ↔ pins (PCB v2) » documentant la convention `pumps[0]`=pH/`pumps[1]`=ORP avec référence ADR-0012 et feature-019
- `docs/subsystems/sensors.md` : tableau Matériel mis à jour (pin OneWire `kTempSensorPin=5` cité, mention du bus partagé avec la 2ᵉ sonde DS18B20 à activer en feature-021), note sur la suppression de l'ADS1115 au profit des Atlas EZO (feature-020)
- `docs/subsystems/filtration.md` : `kFiltrationRelayPin = 26` (au lieu de `FILTRATION_RELAY_PIN = 25` v1)
- `docs/subsystems/lighting.md` : `kLightingRelayPin = 27` (au lieu de `LIGHTING_RELAY_PIN = 26` v1)

---

## [Unreleased] - 2026-04-30

### Firmware
- **MQTT — `requestReconnect()` n'est plus déclenché après un save de paramètres non-MQTT (feature-018)**. Évite la republication des 17 messages discovery HA et la transition transitoire « Déconnecté » du badge UI lors d'un save de modes de régulation, NTP, logs DEBUG, écran LVGL, calibrations, etc. Le handler `POST /save-config` snapshotte désormais les 6 champs MQTT (`server`, `port`, `topic`, `username`, `password`, `enabled`) après prise du `configMutex` et avant parsing JSON, puis compare après application du payload : `requestReconnect()` n'est appelé que si au moins un champ a réellement changé. Nouveau log INFO FR `"MQTT reconnect demandé (config MQTT modifiée)"` pour tracer les reconnects légitimes. Comportement déclenché par feature-015 IT1bis qui a rendu visibles ces transitions transitoires (badge MQTT en temps réel via WS) — la session MQTT était coupée puis rétablie en ~1 s à chaque save UI quel qu'il soit. Contrat externe `POST /save-config` strictement inchangé (mêmes payloads, mêmes réponses)
  - **`src/web_routes_config.cpp`** : snapshot des 6 champs + booléen `mqttChanged` + appel conditionnel `mqttManager.requestReconnect()` (+24/-1 lignes)
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : nouvelle sous-section « Déclenchement conditionnel post-`POST /save-config` (feature-018) » sous « Reconnexion » — tableau des 6 champs MQTT comparés, mécanique du snapshot/comparaison/appel conditionnel, bénéfice (plus de re-discovery HA inutile, badge UI stable), cas `enabled` true → false

### Firmware
- **Toggle pour activer/désactiver les logs DEBUG (firmware + UI)** — `Logger::debug()` est désormais conditionné par `authCfg.debugLogsEnabled` (default `false`, persisté en NVS sous la clé `debug_logs`). Quand le toggle est désactivé, la fonction effectue un early return en première ligne — aucune allocation `String`, aucun lock mutex, aucun push WebSocket, aucune écriture buffer. Default désactivé pour alléger le buffer logs (200 entrées max) en production. Pattern miroir de `sensorLogsEnabled` (« Log des sondes ») introduit précédemment. Les niveaux `INFO`, `WARN`, `ERROR`, `CRITICAL` ne sont **pas** affectés. Effet immédiat (pas de redémarrage requis), persistance NVS au reboot
  - **`src/config.h`** : `bool debugLogsEnabled = false;` ajouté dans `AuthConfig`
  - **`src/config.cpp`** : write/read NVS clé `debug_logs` (default `false`) dans `saveAuthConfig()` / `loadAuthConfig()`
  - **`src/logger.cpp`** : `#include "config.h"` + early return en TOUTE PREMIÈRE LIGNE de `Logger::debug()`. Autres niveaux non touchés
  - **`src/web_routes_config.cpp`** : champ `debug_logs_enabled` exposé dans `/get-config` et accepté dans `/save-config` avec log INFO « Logs DEBUG: activés/désactivés »
  - **`src/ws_manager.cpp`** : champ `debug_logs_enabled` ajouté dans la payload WS `config` (snapshot config push WS broadcast)
  - Le filtre UI `#log_level_debug` reste indépendant (filtre d'affichage seulement, ne pilote pas la production firmware)
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Frontend
- **Nouveau switch « Logs DEBUG activés »** dans Paramètres → Avancé → card Logs, placé immédiatement sous « Log des sondes ». Description en français : « Active les messages de diagnostic détaillé. À activer uniquement pour le diagnostic. ». Lecture/écriture via `applyConfig()` (avec `=== true` pour default `false`) et `collectAuthConfig()`, autosave via event listener (pattern miroir de `sensor_logs_enabled`)
  - **`data/index.html`** : nouveau switch `#debug_logs_enabled` dans la card Logs du panneau Avancé
  - **`data/app.js`** : lecture, écriture et listener autosave

### Documentation
- `docs/features/page-settings.md` : section card Logs étoffée — nouveau toggle « Logs DEBUG activés » documenté sous « Log des sondes » (default `false`, effet immédiat, persistance NVS, indépendance du filtre UI `#log_level_debug`)
- `docs/subsystems/logger.md` : nouvelle section « Toggle DEBUG runtime » documentant l'early return dans `Logger::debug()`, la clé NVS `debug_logs`, le default `false`, l'absence d'impact sur `info/warning/error/critical`, et la complémentarité avec le filtre UI
- `docs/API.md` : champs `sensor_logs_enabled` et `debug_logs_enabled` ajoutés dans le payload de `/get-config` et la table des champs notables (validés dans `/save-config`)
- `docs/UPDATE_GUIDE.md` : nouvelle note de migration « Logs DEBUG désactivés par défaut — depuis 2026-04-30 » expliquant le comportement par défaut et la procédure d'activation via Paramètres → Avancé

### Firmware
- **IT5 — MQTT — fix déconnexions `exceeded timeout` Mosquitto** : remplacement du mode non-bloquant `O_NONBLOCK` (IT4) par un timeout d'écriture borné `SO_SNDTIMEO=500 ms` posé via `setsockopt()` après chaque `mqtt.connect()` réussi. Le PINGREQ keepalive PubSubClient (2 octets toutes les 60 s) part désormais de manière fiable même quand un publish concurrent occupe le send buffer TCP — avant IT5, un `lwip_send()` qui retournait `EAGAIN` instantanément faisait perdre silencieusement le PINGREQ (PubSubClient n'audite pas le retour de `_client->write` pour le PINGREQ), et Mosquitto coupait la session après 90 s sans paquet reçu. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 5 »
  - **Nouvelle constante** `kMqttSocketSendTimeoutMs = 500` (ms) dans `src/constants.h` avec commentaire d'unité explicite et référence ADR-0011 IT5
  - **`src/mqtt_manager.cpp` `connectInTask()`** : `fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)` remplacé par `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))` avec `tv = {0, 500_000 µs}` ; include `<fcntl.h>` retiré, `<lwip/sockets.h>` ajouté
  - **Wrapper `safePublish()` inchangé runtime** : commentaire d'en-tête mis à jour pour refléter le nouveau mécanisme (`mqtt.publish()` borné à 500 ms par appel via `SO_SNDTIMEO`, plus de retour immédiat `EAGAIN`)
  - **Trade-off** : un publish lent peut prendre jusqu'à 500 ms (vs retour immédiat IT4 sur send buffer plein). Pire cas `publishDiscovery` (17 publishes enchaînés) = 8.5 s, sous le watchdog 30 s avec marge. Imperceptible utilisateur sur LAN sain
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : section « Garde-fou » renommée « `safePublish()` + socket avec `SO_SNDTIMEO` (IT5, remplace O_NONBLOCK d'IT4) » avec snippet `setsockopt`, explication du side-effect IT4 sur le PINGREQ keepalive et son fix IT5 ; tableau des paramètres tâche enrichi de `kMqttSocketSendTimeoutMs` ; sections « Keepalive », « Bornage TCP côté lwip », « Bascule de dominance entre `-3` et `-4` » et tableaux mis à jour pour refléter le timeout socket borné IT5 au lieu du non-bloquant IT4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 5 — 2026-04-30 » dans « Évolutions » détaillant la cause racine (PINGREQ silencieusement perdu en `O_NONBLOCK`), le fix `SO_SNDTIMEO=500 ms`, les fixes F17–F20, le trade-off et les tests dynamiques restants. La décision principale de l'ADR (tâche dédiée) reste retenue
- `specs/features/done/feature-014-mqtt-task-dediee.md` : statut passé à `done`, version cible 1.0.5, itération 5 marquée livrée (build vert + revue OK ; AC-IT5-3 et AC-IT5-4 délégués à l'humain post-flash)

---

## [Unreleased] - 2026-04-29

### Frontend
- **UI cards — placement uniformisé des badges d'état (feature-001)** : uniformisation du placement des badges d'état (Marche/Arrêt, Allumé/Éteint, Connecté/Déconnecté) à droite du titre dans le `card__head` des cards Filtration « Contrôle manuel », Éclairage « Contrôle manuel » et MQTT (Paramètres). Suppression des lignes « État actuel » redondantes dans le body des cards Filtration et Éclairage. Cohérence visuelle inter-pages, gain de place vertical. Côté JS, `updateFiltrationBadges()` et `updateLightingStatus()` ont été splittés (page détail = `pill ok/bad/mid` ; dashboard `card--status` = `state-badge--*` inchangé), `getFiltrationState()` expose désormais `pillClass` (mapping `warn → mid`). Règle CSS de garde `.card__head .pill { flex-shrink: 0; white-space: nowrap; }`. Aucun impact sur le dashboard ni sur les autres cards (Wi-Fi, Heure, Sécurité, Régulation, Calibrations, Produits, Historique, Système).
- **Bug fix** : Badge MQTT (Paramètres → MQTT) — propagation fiabilisée vers l'UI : la mise à jour temps réel via WS s'applique désormais en tête de `_onWsSensorData` (blindée par try/catch) et un re-render explicite est déclenché au passage sur le panel MQTT. Corrige un cas où le badge restait à « Déconnecté » après reconnexion firmware sans switch d'onglet.
- **WebSocket** : badge statut MQTT (Paramètres → MQTT) mis à jour en temps réel via push WS toutes les 5 s, sans nécessité de reload page. Quand le broker devient injoignable (câble HA débranché, broker arrêté), le badge bascule sur « Déconnecté » en moins de 5 s suivant la détection firmware ; idem pour la reconnexion. Le champ `mqtt_connected` est désormais inclus dans la payload `sensor_data` (en plus du snapshot `config` déjà présent). Source : `mqttManager.isConnected()` (single source of truth `connectedAtomic` introduit par feature-014 IT2)

### Documentation
- `docs/features/page-filtration.md` : badge Marche/Arrêt désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateFiltrationBadges()` / `getFiltrationState()`
- `docs/features/page-lighting.md` : badge Allumé/Éteint désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateLightingStatus()`
- `docs/features/page-settings.md` : précision du placement DOM uniformisé du badge MQTT (frère direct du `<h2>` dans `card__head`) et de la règle CSS de garde
- `docs/subsystems/ws-manager.md` : nouveau champ `mqtt_connected` documenté dans `sensor_data` avec la précision du doublon volontaire vs `config` (canal temps réel 5 s vs snapshot stable à la transition)
- `docs/features/page-settings.md` : précision sur le comportement temps réel du badge MQTT (Paramètres → MQTT) — bascule en < 5 s sans reload

---

## [Unreleased] - 2026-04-29

### Firmware
- **IT4 — Wrapper `safePublish()` + socket non-bloquante** — corrige un nouveau PANIC watchdog observé au 3ᵉ re-test D2 humain APRÈS le flash IT3. Le point de blocage avait migré vers `drainOutQueue()` (publish ~110 octets `orp_limit`), fonction qui n'avait pas été instrumentée par F8/F9 d'IT3 (oubli). Découverte parallèle : `CONFIG_LWIP_TCP_MAXRTX=5` borne en réalité à ~93 s (et non ~10 s comme estimé en IT3) à cause de `TCP_RTO_INITIAL=3 s` × backoff exponentiel — le bornage seul ne protège pas `mqttTask`. Pivot architectural : socket TCP non-bloquante via `fcntl(F_SETFL, O_NONBLOCK)` après chaque `mqtt.connect()` réussi → tout `WiFiClient::write()` retourne immédiatement avec `EAGAIN` si send buffer plein, plus de blocage dans `lwip_select`. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 4 »
  - **Mode non-bloquant via `fcntl()`** : passage de la socket en `O_NONBLOCK` après chaque connect réussi dans `connectInTask()`, avant `subscribe()`. Includes `<fcntl.h>` et `<errno.h>` ajoutés
  - **Wrapper unique `safePublish(topic, payload, retain)`** : remplace les 24 `mqtt.publish()` directs dans `mqttTask` (status `online` au connect, `drainOutQueue`, 20 publishes de `publishAllStatesInternal`, `publishDiagnosticInternal`, 17 publishes de la lambda `publishConfig` dans `publishDiscovery`). Le wrapper fait `esp_task_wdt_reset()` puis check `mqtt.connected()` avant délégation à `mqtt.publish()`
  - **Suppression des garde-fous IT3 redondants** : ~50 lignes de `esp_task_wdt_reset()` et `if (!mqtt.connected()) return;` éparpillées dans `publishAllStatesInternal()` (20 resets + 5 bail-out) et la lambda `publishConfig` (1 bail-out) ont été supprimées — factorisées dans le wrapper, plus lisible
  - **F12 annulé** : la constante `kMqttPublishHeadersOverhead` envisagée pour un check `availableForWrite()` est **inutile** car `WiFiClient::availableForWrite()` retourne toujours 0 dans Arduino-ESP32 6.9.0 (méthode héritée de `Print::availableForWrite()` non override). Le mode non-bloquant remplace ce mécanisme
  - **Trade-off accepté** : drop silencieux des publish quand le send buffer TCP est plein. Les états retain sont republiés au cycle suivant (10 s), les alertes ponctuelles peuvent être perdues — c'était déjà le cas en mode bloquant pré-IT4 (timeout 30 s puis drop), IT4 rend le drop instantané

- **IT3 — Borne TCP write côté lwip + bail-out anticipé** — corrige un nouveau PANIC watchdog observé au re-test D2 humain APRÈS le flash IT2. Le point de blocage avait migré de `mqtt.connect()` (résolu en IT2) vers `mqtt.publish()` : coredump-5 confirme `mqttTask` bloquée dans `WiFiClient::write` → `lwip_select` lors d'un publish 33 octets ("OFF") sur send buffer TCP saturé. Le reset wdt par groupes de 5 publish d'IT2 ne suffisait pas. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 3 »
  - **`CONFIG_LWIP_TCP_MAXRTX=5` dans `platformio.ini`** (`build_flags`) : limite à 5 retransmissions TCP avant abandon de socket par lwip (~10 s cumulés au lieu de ~75 s avec la valeur par défaut 12). Trade-off : paramètre global à toute la pile lwip → impact aussi AsyncWebServer / OTA HTTP / NTP (abandon socket ~10 s vs ~75 s). Acceptable pour un firmware temps réel ; OTA en réseau très dégradé peut être avorté plus tôt et nécessiter un retry humain
  - **Cadence `esp_task_wdt_reset()` 1:1 dans `publishAllStatesInternal()`** : reset posé **avant chaque** `mqtt.publish()` (20 resets, vs 2 resets par groupes en IT2). Garantit qu'au pire un seul publish reste à l'intérieur de la fenêtre wdt 30 s
  - **Bail-out fail-fast** : 5 `if (!mqtt.connected()) return;` répartis dans `publishAllStatesInternal()` (tous les ~3-4 publish) + 1 bail-out en tête de la lambda `publishConfig` de `publishDiscovery()`. Dès que lwip ferme la socket (cf. `CONFIG_LWIP_TCP_MAXRTX=5`), les publish restants sont court-circuités au lieu d'enchaîner 14 erreurs de ~2 s chacune
  - **F7/F10 annulés** : `setsockopt(TCP_USER_TIMEOUT)` envisagé initialement pour borner par-socket (RFC 5482) — **non supporté par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). Le bornage TCP repose donc uniquement sur F6 (paramètre global lwip). Constante `kMqttTcpUserTimeoutMs` non créée

- **Durcissement watchdog `mqttTask` sur broker injoignable** — corrige un PANIC watchdog 30 s observé pendant le test D2 (câble Ethernet HA débranché 2–3 min) où `mqttTask` restait bloquée dans `WiFiClient::connect()` → `lwip_select` sans reset. La régulation pH/ORP, la filtration et les autres tâches n'étaient pas concernées (déjà isolées par ADR-0011), mais le PANIC du core 0 entraînait un reboot complet. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions »
  - **Timeout client TCP corrigé de 5000 s à 2 s** : `WiFiClient::setTimeout()` (Arduino-ESP32 6.9.0) attend des **secondes**, pas des ms — le code historique `setTimeout(5000)` programmait 5000 secondes (~83 min), bug latent qui rendait le timeout côté client TCP totalement inopérant. Nouvelle constante `kMqttClientConnectTimeoutSec = 2` dans `constants.h` avec commentaire d'unité explicite
  - **`esp_task_wdt_reset()` granulaire** : reset ajouté juste après le retour de `mqtt.connect()` (couvre le cas SYN TCP retransmis sur broker injoignable jusqu'à ~75 s avant abandon lwip), reset au milieu de `publishAllStatesInternal()` (≤ 5 publish entre 2 resets), reset après chaque publish individuel dans le helper `publishConfig` de `publishDiscovery()` (17 publishes auto-discovery)
  - **`connectedAtomic` single source of truth** : store canonique posé en début de `taskLoop()` à chaque tour, complété par les transitions explicites en `connectInTask()` (succès) et `disconnect()`. Suppression de 3 stores intermédiaires redondants qui créaient une fenêtre de divergence UI/WARN observée pendant D2 (UI affichait « Déconnecté » sans WARN dans les logs)
  - **Suppression de la race au boot** : `mqttManager.requestReconnect()` retiré de `setup()` dans `main.cpp` — `mqttTask` se connecte déjà toute seule au premier tour de `taskLoop()`. Élimine le double publish d'auto-discovery au démarrage (32 messages au lieu de 17)

### Documentation
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT après validation D2 humaine du 2026-04-29 14:12:43 — avec le mode non-bloquant IT4, l'état dominant lors d'une coupure réseau est `-4` (`MQTT_CONNECTION_TIMEOUT`, keepalive PubSubClient, ~60 s) au lieu de `-3` (`MQTT_CONNECTION_LOST`, abandon TCP lwip, 90–180 s). La dominance documentée pré-IT4 (`-3` majoritaire) est inversée. Bénéfice indirect : détection plus précoce et plus propre. Tableau « États observés selon le scénario », tableau des codes d'état et workflow troubleshooting mis à jour en conséquence ; D2 marqué VALIDÉ dans la spec avec logs de référence
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Garde-fou : `safePublish()` + socket non-bloquant (IT4) » (snippet du wrapper, `fcntl(F_SETFL, O_NONBLOCK)` après chaque connect, tableau des 24 call sites, trade-off drop silencieux) ; AVERTISSEMENT en tête de section sur `WiFiClient::availableForWrite()` qui retourne toujours 0 dans Arduino-ESP32 6.9.0 ; section « Watchdog dans `mqttTask` » mise à jour (les `esp_task_wdt_reset()` IT3 ont été factorisés dans `safePublish()`) ; section « Bornage TCP côté lwip » corrigée — `CONFIG_LWIP_TCP_MAXRTX=5` borne à ~93 s (RTO_INITIAL × backoff), pas 10 s. Le mode non-bloquant IT4 est le vrai mécanisme de protection ; section « Bail-out fail-fast pendant les salves de publish » mise à jour pour pointer vers le wrapper
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 4 — 2026-04-29 » dans « Évolutions » détaillant le pivot socket non-bloquante, le wrapper `safePublish()` sur 24 call sites, l'annulation de F12 (`availableForWrite()` cassé dans Arduino-ESP32 6.9.0), la découverte du bornage réel à ~93 s pour `MAXRTX=5`, le trade-off drop silencieux. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : section « Watchdog dans `mqttTask` » mise à jour (cadence 1:1 dans `publishAllStatesInternal()`, 5 bail-out répartis, rationnel IT3) ; section « Détection des déconnexions » étendue avec sous-section « Bail-out fail-fast pendant les salves de publish (IT3) » ; nouvelle section « Bornage TCP côté lwip » documentant `CONFIG_LWIP_TCP_MAXRTX=5` et son impact global (AsyncWebServer / OTA / NTP) avec mention de l'absence de support `TCP_USER_TIMEOUT` dans lwip ESP-IDF 4.4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 3 — 2026-04-29 » dans « Évolutions » détaillant les 3 fixes appliqués (F6, F8, F9), l'annulation de F7/F10 (TCP_USER_TIMEOUT non supporté par lwip ESP-IDF 4.4), le coredump-5 confirmant le déplacement du blocage de connect vers publish, et le trade-off du paramètre lwip global sur AsyncWebServer / OTA / NTP. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Watchdog dans `mqttTask` » (tableau des 10 emplacements de `esp_task_wdt_reset()` avec cadence garantie ≤ 5 publish entre 2 resets) ; nouvelle section « Timeout client TCP — UNITÉ EN SECONDES » avec avertissement visible pour le bug latent `setTimeout(5000)` ; section « Détection des déconnexions » étendue avec le store canonique `connectedAtomic` en début de `taskLoop()` et la suppression des stores intermédiaires ; section « Reconnexion » étendue avec la sous-section « Connexion initiale au boot » documentant la suppression du `requestReconnect()` dans `setup()`
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la section « Évolutions » → « Itération 2 — 2026-04-29 » détaillant les 5 fixes appliqués, le bug latent `WiFiClient::setTimeout()` et le coredump confirmant la cause racine. La décision principale de l'ADR (tâche dédiée) reste retenue — c'est un durcissement post-test, pas un revirement

---

## [Unreleased] - 2026-04-27

### Firmware
- **MQTT déplacé dans une tâche FreeRTOS dédiée** (`mqttTask`, core 0, prio 2, stack 8 KB) — corrige les crashes `PANIC IntegerDivideByZero` watchdog 30 s observés en production lorsqu'une publication MQTT (de 33 octets dans le 3ᵉ crash) bloquait `loopTask` dans `lwip_select` à cause de la saturation du TCP send window sur CPL bruyant. La régulation pH/ORP, la filtration et le watchdog continuent désormais indépendamment de l'état du réseau MQTT. Comportement utilisateur strictement identique : mêmes topics, mêmes payloads, mêmes intervalles, même auto-discovery HA. API publique de `MqttManager::publishXxx()` inchangée — les méthodes deviennent simplement non-bloquantes (producteurs sur queue FreeRTOS). Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md)
- **Arrêt propre OTA** : publication `status=offline` synchrone (timeout 1 s) avant `ESP.restart()` sur tous les sites concernés (OTA firmware, redémarrage mode AP, factory reset bouton, factory reset HTTP). Les clients HA voient désormais le passage à `offline` immédiatement, sans attendre les 90 s de timeout broker
- **Stabilité MQTT** : fin des déconnexions périodiques (toutes les 1–2 h) — désactivation du WiFi power save (`WiFi.setSleep(false)`), pré-résolution DNS explicite avant `WiFiClient::connect`, `setSocketTimeout` réduit à 2 s, suppression du `requestReconnect()` périodique qui réinitialisait le backoff exponentiel
- **Stabilité réseau** : latence LAN ramenée sous 10 ms (était 90–260 ms à cause du DTIM WiFi) ; loop principale jamais bloquée plus de ~7 s par opération réseau ; backoff MQTT effectif jusqu'à 120 s sur broker injoignable
- **Coredump** : ajout de la partition `coredump` dédiée (64 KB, offset `0x3F0000`) — les crashes `PANIC` sont désormais persistés en flash et accessibles via l'API HTTP ou l'UI
- **Partitions** : partition `history` réduite de 128 KB à 64 KB pour libérer l'espace nécessaire au coredump ; flash USB obligatoire pour cette migration (OTA insuffisant)
- **History** : `kMaxHourlyDataPoints` réduit de 360 à 168 (7 jours de rétention horaire, contre 15 jours précédemment) pour respecter le budget de la partition réduite
- **History** : `HistoryManager::begin()` détecte un redimensionnement de partition via NVS (`hist_meta/part_sz`) et efface le filesystem avant montage si la taille a changé — évite un crash `IntegerDivideByZero` dans `lfs_alloc`
- **Régulation** : reset journalier des compteurs `dailyPhInjectedMl` / `dailyOrpInjectedMl` désormais indépendant de l'état de la filtration — extraction de la logique de bascule de date dans `tickDailyRollover()`, appelée depuis `update()` **avant** le check `canDose()`. Le bug rendait le compteur figé sur la valeur de la veille tant que la filtration n'avait pas tourné dans la journée. La transition `currentDayDate` vide → date NTP valide remet aussi `dayStartTimestamp = 0` pour invalider tout timer fallback `millis()` accumulé depuis le boot
- **Régulation** : ajout de `saveDailyCounters()` + `armStabilizationTimer()` dans la branche fallback `millis()` du reset journalier (manquaient avant le fix)
- **Régulation** : warnings/criticals du mode `scheduled` (capteur pH/ORP hors plage, daily target plafonné, débit pompe à 0) passés en **edge-triggered** — un seul log à l'entrée dans l'état + un INFO de recovery au retour à la normale. Stoppe le spam de centaines de lignes par seconde quand l'état persistait
- **Historique** : suppression de l'appel `saveToFile()` redondant dans `HistoryManager::update()` (`consolidateData()` l'appelle déjà en interne) — moitié moins d'écritures flash sur la partition `history` toutes les 5 min
- **Historique** : trace `Consolidation terminée: N points` rétrogradée de INFO à DEBUG, suppression du marqueur `DEBUG: Début consolidation historique` ; commentaire corrigé (consolidation effective toutes les 5 min, pas « toutes les heures »)
- **Logger** : intervalle de flush ramené de 60 s à **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et la persistance du coredump couvrent les crashes, l'écriture flash périodique en INFO/DEBUG n'apporte rien
- **Logger** : nouvelle méthode `clearAll()` qui vide RAM + `_persistBuffer` + supprime `/system.log` et `/system.log.tmp` (la méthode existante `clear()` ne touche que le buffer RAM)
- **Diagnostic réseau** : ajout d'un handler `WiFi.onEvent()` dans `setupWiFi()` qui logge les événements `STA_DISCONNECTED` (WARN, avec `reason=N`), `STA_CONNECTED` (INFO), `STA_GOT_IP` (INFO, avec IP et RSSI), `STA_LOST_IP` (WARN). Permet de distinguer un drop Wi-Fi (séquence `DISCONNECTED → CONNECTED → GOT_IP`), un DHCP renew (`LOST_IP → GOT_IP`) ou un problème non-Wi-Fi (broker, firewall) lors d'un blackout réseau
- **Diagnostic MQTT** : log de la cause de déconnexion au front de transition `connecté → déconnecté` (`WARN: MQTT déconnecté détecté — état=N` où `N` est le code `PubSubClient::state()` : `-4` timeout keepalive, `-3` TCP fermé, `-2` TCP refusé, `-1` déconnexion propre). Court-circuit DNS quand `mqttCfg.server` est déjà une IP littérale (`IPAddress::fromString()` avant `WiFi.hostByName()`) — élimine tout cycle DNS pour les installations LAN par IP fixe
- **Stabilité MQTT** : keepalive PubSubClient relevé de 30 s à **60 s** (`mqtt.setKeepAlive(60)` dans `MqttManager::begin()`). La tolérance broker passe corrélativement de 45 s à 90 s (1.5 × keepalive côté Mosquitto), ce qui absorbe les microcoupures réseau prolongées sans déclencher de `état=-4`. Cible : chemins instables type CPL/Powerline où des publishes et PINGREQ se perdent sporadiquement à cause du bruit électrique secteur. Trade-off accepté : une vraie déconnexion (broker arrêté, WiFi coupé) est détectée en 90 s au lieu de 45 s — sans impact sur la régulation pH/ORP qui ne dépend pas d'une latence sub-minute

### Fonctionnalités
- **Paramètres → Avancé** : nouvelle card "Diagnostic crash" — statut coredump (tâche, exception, PC), boutons Actualiser / Télécharger / Effacer, hint de décodage `./tools/decode_coredump.sh`
- **Paramètres → Avancé → Logs** : bouton existant « Effacer » renommé **« Effacer (écran) »** (vide uniquement la vue navigateur) ; nouveau bouton **« Effacer (firmware) »** en rouge `btn--danger` qui appelle `DELETE /logs` après confirmation et purge intégralement les logs côté ESP32 (RAM + fichier persistant)
- **Script** : `tools/decode_coredump.sh` — décode un `coredump.bin` avec `xtensa-esp32-elf-gdb` et `esp_coredump` du penv PlatformIO

### API
- `GET /coredump/info` : résumé JSON du dernier crash (tâche, PC, cause exception)
- `GET /coredump/download` : téléchargement du binaire brut `coredump.bin` (streamé, pas d'allocation 64 KB)
- `DELETE /coredump` : effacement de la partition pour le prochain crash
- `DELETE /logs` (WRITE) : efface intégralement les logs côté ESP32 (RAM + tampon de flush + `/system.log` + `/system.log.tmp`). Réponse `{"success": true}`. Une entrée INFO `Logs effacés (RAM + fichier persistant)` est écrite immédiatement après pour tracer l'action

### Documentation
- `docs/adr/0011-mqtt-task-dediee.md` : ADR créé — déplacement de toute la logique MQTT (publish, connect, loop, callback) dans une tâche FreeRTOS dédiée `mqttTask` ; producer/consumer via deux queues (`outQueue` 32 entrées, `inQueue` 16 entrées) ; arrêt propre `status=offline` avant `ESP.restart()`. Décision motivée par 3 crashes production confirmant que `setSocketTimeout(2)` (ADR-0010) ne couvre pas `lwip_write` sur réseau lossy
- `docs/adr/0010-stabilite-mqtt-reseau.md` : note de mise à jour — l'alternative « Tâche FreeRTOS dédiée pour MQTT » écartée à l'origine est désormais retenue par ADR-0011 (Superseded for MQTT decoupling). Les fixes synchrones D1–D5 restent valides et sont préservés intégralement dans la nouvelle tâche dédiée
- `docs/subsystems/mqtt-manager.md` : refonte majeure — section « Architecture producer/consumer », règles d'or (aucun `mqtt.publish()` depuis `loopTask`, aucun acteur direct depuis `mqttTask`, aucun appel `Async*` depuis `mqttTask`), tableau des paramètres `mqttTask`, refonte gestion des commandes HA (queue entrante drainée par `loopTask`), section « Arrêt propre OTA », troubleshooting drops `outQueue`
- `docs/UPDATE_GUIDE.md` : note user-facing — stabilité réseau améliorée (la régulation continue de tourner même en cas de microcoupures broker MQTT)
- `docs/adr/0010-stabilite-mqtt-reseau.md` : ADR créé — décisions de stabilité réseau (WiFi sans power save, pré-résolution DNS, backoff non réinitialisé)
- `docs/adr/0009-partition-coredump.md` : ADR créé — table de partitions avec coredump + conséquences de migration
- `docs/adr/0007-table-partitions-custom.md` : statut mis à jour → `Superseded by ADR-0009`
- `docs/subsystems/history.md` : partition 64 KB, rétention 7 jours, section protection redimensionnement, budget partition ; clarification que `consolidateData()` appelle `saveToFile()` en interne et que la trace de fin est en DEBUG
- `docs/subsystems/logger.md` : flush 10 min, flush immédiat ERROR/CRITICAL, `_persistBuffer` borné, fichier log 16 KB ; ajout de `clearAll()` et de l'endpoint `DELETE /logs`
- `docs/subsystems/pump-controller.md` : section "Reset journalier" décrivant `tickDailyRollover()` et son emplacement dans `update()` avant `canDose()` ; section "Warnings edge-triggered" listant les six logs concernés et le pattern `static bool`
- `docs/subsystems/mqtt-manager.md` : section "Pattern de connexion (DNS séparé du TCP)" ; clarification que la reconnexion est 100 % pilotée par `update()` ; lien vers ADR-0010
- `docs/API.md` : section "Diagnostic crash (coredump)" avec les 3 endpoints + nouvelle entrée `DELETE /logs`
- `docs/features/page-settings.md` : card "Diagnostic crash" dans le panneau Avancé + section dédiée à la card Logs (4 boutons : Actualiser, Effacer (écran), Télécharger, Effacer (firmware))
- `docs/subsystems/logger.md` : nouvelle section « Logs WiFi » documentant les 4 messages émis par le handler `WiFi.onEvent()` et les codes de raison Wi-Fi courants
- `docs/subsystems/mqtt-manager.md` : note de troubleshooting — vérifier d'abord les logs WiFi (`WARN: WiFi déconnecté (reason=...)`) avant de chercher la cause d'une reconnexion MQTT répétée
- `docs/subsystems/mqtt-manager.md` : section « Diagnostic — Codes d'état PubSubClient » (codes `-4` à `-1` du log `MQTT déconnecté détecté`), tableau du chemin DNS selon IP littérale vs hostname, workflow de troubleshooting basé sur le code d'état
- `docs/subsystems/logger.md` : nouvelle section « Logs MQTT » listant les 6 messages émis par `MqttManager` (incluant le nouveau `MQTT déconnecté détecté — état=N` edge-triggered)
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Keepalive » (60 s côté client, 90 s de tolérance broker, trade-off de détection des vraies déconnexions) ; précision dans le tableau des codes d'état (`-4` = sans PINGREQ/PONG dans la fenêtre 90 s) ; ajout d'une étape 3 au workflow troubleshooting couvrant les chemins physiques instables (CPL/Powerline, RSSI marginal) quand WiFi et broker sont innocentés
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT — état dominant `-3` (`MQTT_CONNECTION_LOST`, socket TCP invalide), `-4` (timeout keepalive) quasi-inatteignable car `mqtt.connected()` est testé avant `mqtt.loop()` dans `taskLoop()` ; délai typique 100–180 s sur arrêt brutal du broker (RTO TCP lwip, pas keepalive applicatif), quasi-immédiat sur fermeture propre TCP. Nouvelle section « Détection des déconnexions » dans la doc subsystem

---

## [Unreleased] - 2026-04-24

### Firmware
- **Persistance compteurs journaliers** : `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont désormais persistés en NVS (namespace `pool-daily`) — les compteurs survivent aux reboots ESP32 et sont restaurés si le jour calendaire est identique
- **Reset journalier** : aligné sur minuit local (RTC/NTP) au lieu d'une fenêtre glissante 24 h ; `armStabilizationTimer()` est armé au passage de minuit (mitigation double quota)
- **`kMinValidEpoch`** : constante consolidée dans `src/constants.h` (valeur : 1700000000, 14 nov. 2023)
- **Raison du dernier reboot** : champ `reset_reason` ajouté dans le payload WebSocket `sensor_data` — valeurs possibles : `POWER_ON`, `SW_RESET`, `WATCHDOG`, `BROWNOUT`, `PANIC`, `DEEP_SLEEP`, `EXTERNAL`, `UNKNOWN` ; constant pendant le runtime

### Fonctionnalités
- **Pages /ph et /orp** : les blocs Statistiques sont grisés (`opacity: 0.5`) lors d'une déconnexion WebSocket — indique visuellement que les données affichées ne sont plus à jour
- **Toast reboot inattendu** : un toast dismissable s'affiche une fois par session si le champ `reset_reason` indique un reboot inattendu (`WATCHDOG`, `BROWNOUT` ou `PANIC`) — libellé : « Redémarrage inattendu détecté (raison : X) »
- **Régulation pH** : remplacement du toggle binaire `ph_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation pH** : mode Programmée — volume quotidien configurable (mL), injecté pendant les plages de filtration jusqu'au quota journalier
- **Régulation pH** : migration automatique au premier boot : `ph_enabled=true` → `automatic`, `ph_enabled=false` → `manual`
- **Limites horaires** : renommage `phInjectionLimitSeconds` → `phInjectionLimitMinutes` (idem ORP) — les limites sont désormais saisies en minutes (1–60) au lieu de secondes ; migration NVS transparente au boot (`ph_limit_sec` → `ph_limit_min`)
- **Protection pompes** : suppression de `minPauseBetweenMs` — la pause inter-injections configurable est retirée ; la protection contre le short-cycling reste assurée par `minInjectionTimeMs` (30 s) et `maxCyclesPerDay` (20/24 h)
- **MQTT** : publication des champs `ph_regulation_mode` et `ph_daily_target_ml` dans `publishTargetState()`
- **Sécurité** : suppression du log du mot de passe WiFi en clair dans les traces de reconnexion
- **Régulation pH (Programmée)** : refonte de l'algorithme d'injection — la pompe injecte librement pendant la filtration jusqu'à atteindre le quota journalier (`phDailyTargetMl`), sans répartition sur 24 h ; la limite horaire (`phInjectionLimitMinutes`) reste la seule barrière contre l'injection rapide
- **Régulation ORP** : remplacement du toggle binaire `orp_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation ORP** : mode Programmée — volume quotidien de chlore configurable (mL), aveugle au capteur ORP, borné par `maxChlorineMlPerDay` ; PID réinitialisé au retour en mode automatique
- **Régulation ORP** : migration automatique au premier boot : `orp_enabled=true` → `automatic`, `orp_enabled=false` → `manual` ; champ `orp_enabled` conservé comme miroir pour compatibilité HA
- **MQTT** : publication des champs `orp_regulation_mode` et `orp_daily_target_ml` dans `publishTargetState()`

### API
- `GET /get-config` / `POST /save-config` : `ph_limit_seconds` → `ph_limit_minutes`, `orp_limit_seconds` → `orp_limit_minutes` ; suppression de `min_pause_between_min`
- WebSocket config : mêmes renommages (`ph_limit_minutes`, `orp_limit_minutes`)
- `GET /get-config` : ajout des champs `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`, `orp_cal_valid`
- `POST /save-config` : validation de `orp_regulation_mode` (enum), `orp_daily_target_ml` (borné par `max_orp_ml_per_day`, HTTP 400 si dépassé)

### Fonctionnalités
- **Page pH** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels
- **Page pH** : mode Programmée avec saisie du volume quotidien (mL) borné par la limite journalière configurée
- **Paramètres** : champs durée max pH/ORP en minutes (1–60 min/h) au lieu de secondes ; suppression du champ « Pause entre deux injections »
- **Page ORP** : refonte complète — architecture 4 cartes (Statistiques compact / Régulation / Historique / Calibration conditionnelle)
- **Page ORP** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels (symétrie avec page pH)
- **Page ORP** : mode Programmée avec saisie du volume quotidien de chlore (mL), borné par la limite journalière de sécurité
- **Page ORP** : calibration accessible uniquement en mode Automatique (bouton Calibrer dans le sous-bloc Automatique) ; carte Calibration en superposition pendant le protocole
- **Page ORP** : bloc Statistiques compact (ORP actuelle + Dosage du jour) en en-tête de page, hors carte

---

## [1.1.0] - 2026-03-29

### Firmware
- **MQTT** : ajout des topics publiés `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph_target`, `orp_target`
- **MQTT** : ajout des topics de commande `ph_target/set` et `orp_target/set` (modification des consignes pH et ORP depuis HA ou MQTT)
- **MQTT** : correction du switch "Filtration Marche/Arrêt" — la commande `OFF` forçait l'arrêt de la filtration mais elle redémarrait immédiatement selon le planning
- **Home Assistant Auto-Discovery** : ajout de 6 nouvelles entités (Dosage pH Actif, Dosage Chlore Actif, Limite Journalière pH, Limite Journalière Chlore, Consigne pH, Consigne ORP)

### Documentation
- `docs/MQTT.md` : documentation complète des topics publiés, commandes et entités Home Assistant avec les noms tels qu'ils apparaissent dans l'interface HA
- `docs/API.md` : réécriture complète — tous les endpoints documentés (30+)
- `docs/UPDATE_GUIDE.md` : mise à jour avec les modes OTA de `deploy.sh`
- `deploy.sh` : ajout des modes `ota-firmware`, `ota-fs`, `ota-all` (compile + envoi OTA en une commande)
- Renommage `quick_update.sh` → `ota_update.sh`

---

## [1.0.3] - 2026-03-27

### Firmware
- **Factory reset** : détection par appui long 10s pendant le fonctionnement normal — plus besoin de couper l'alimentation
- Suppression des constantes `PH_SENSOR_PIN` / `ORP_SENSOR_PIN` (vestiges ADC interne non utilisés depuis le passage à l'ADS1115 I2C)

### Documentation
- Procédure factory reset mise à jour (fonctionnement runtime)
- Section Matériel Requis : schéma électronique et PCB illustrés, liens vers fichiers Gerber et STL
- `build_all.sh` documenté dans BUILD.md et UPDATE_GUIDE.md

---

## [1.0.1] - 2026-03-26

### Firmware
- **Bouton factory reset (GPIO32)** : appui de 10 secondes au démarrage pour réinitialisation usine complète
  - LED intégrée clignote pendant l'appui pour indiquer la progression
  - Efface la partition NVS (mot de passe, WiFi, MQTT, calibrations)
  - Préserve les consignes, limites et l'historique des mesures
  - L'ESP32 redémarre en mode AP avec l'assistant de configuration

### Hardware
- Ajout des fichiers Gerber (fabrication PCB) dans le dossier `hardware/`
- Ajout des fichiers STL du boîtier v3 (corps + couvercle) dans le dossier `hardware/`

---

## [1.0.0] - 2026-03-24 — Première release publique

### Fonctionnalités
- Régulation automatique pH et ORP (chlore) via algorithme PID
- Gestion filtration (auto / manuel / off) avec programmation horaire
- Contrôle éclairage avec programmation horaire
- Interface web avec tableau de bord temps réel (graphiques pH, ORP, température)
- Intégration Home Assistant via MQTT Auto-Discovery
- Mises à jour OTA via interface web (firmware et filesystem)
- Assistant de configuration au premier démarrage (mot de passe, WiFi, heure)
- Protocole UART pour écran LVGL externe
- Historique des mesures sur partition dédiée (préservé lors des mises à jour)
- Alertes MQTT en cas d'anomalie (valeurs aberrantes, limites atteintes, mémoire faible)
- Factory reset via bouton physique GPIO32
