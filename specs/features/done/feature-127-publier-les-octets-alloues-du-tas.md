## Métadonnées
- **ID** : feature-127
- **Slug** : publier-les-octets-alloues-du-tas
- **Type** : feature — télémétrie
- **Statut** : done
- **Priorité** : **haute** — c'est la seule mesure qui manque pour trancher la dernière question ouverte de l'incident du 10/09, et le problème est actif (138 suspensions d'ouverture de fichiers le 13/09)
- **Version cible** : à déterminer
- **Dernière mise à jour** : 2026-09-13
- **Spec(s) liée(s)** : `feature-122` (lot C-1a, planchers glissants multi-masques), `feature-125` (réserves du lot C), `bug-080` (lot C), incident [`2026-09-10-recidive-fragmentation-v2500.md`](../../../docs/incidents/2026-09-10-recidive-fragmentation-v2500.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun dosage, aucune régulation, aucune calibration touchés
- [ ] ux-ui-designer — aucune surface UI neuve
- [x] embedded-developer
- [x] web-ui-developer — la route `/get-system-info` est de son domaine
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Publier `total_allocated_bytes` — les **octets** alloués du tas, par masque de
capacité — dans l'objet `hf` de `/get-system-info?hf=1`. La valeur est déjà
calculée par `heap_caps_get_info()` à chaque appel, et jetée.

## Contexte

**C'est exactement le défaut que le lot B a corrigé le 12/09, un champ plus loin.**

`heap_caps_get_info()` remplit une `multi_heap_info_t` de sept champs. Le projet
n'en lisait que deux jusqu'à v2.51.0, qui a publié `free_blocks` et
`allocated_blocks` (`fb`/`ab`). Ces deux compteurs ont **immédiatement tranché**
une question ouverte depuis des semaines : la fenêtre de mesure du 12-13/09 a
montré que le plus gros bloc ne remonte **jamais** (0 remontée sur 28 intervalles)
alors que le libre remonte de 29 % — la fragmentation est établie.

**Mais une question est restée ouverte, et c'est faute de ce champ-ci.**

La note d'incident concluait d'abord « les blocs alloués sont plats, donc pas de
fuite ». **La conclusion dépassait la mesure**, et elle a été corrigée le 13/09 au
soir. Ce qui est plat, c'est le **nombre** de blocs (883 → 908, avec 10 baisses
sur 28 intervalles, donc une oscillation et non une croissance). Or le libre a
perdu **~10 400 octets qui ne sont jamais revenus** (55 468 → 45 072, le maximum
de la série ne retrouvant jamais sa valeur initiale).

À nombre de blocs quasi constant, cela signifie que les blocs alloués pèsent en
moyenne **plus lourd** qu'au départ. Deux explications restent **indiscernables
avec la télémétrie actuelle** :

- un **jeu de travail plus gros** — des tampons légitimement plus grands, conservés ;
- une **fuite en octets** plutôt qu'en nombre d'allocations.

Un seul champ les sépare. Tant qu'il n'est pas publié, toute affirmation sur
l'absence de fuite est une **opinion**, pas une mesure.

Point déjà acquis et à ne pas réinstruire : la **PSRAM est plate** (−320 o sur
22,5 h). Si fuite il y a, elle n'est pas là.

## Comportement attendu

`GET /get-system-info?hf=1` expose, pour **chaque masque** déjà présent dans `hf`
(`i8`, `dma`, `def`, `i8d`, …), un champ neuf portant les octets alloués.

- Le champ suit **exactement** la convention établie par `fb`/`ab` : nom court,
  et `null` quand la grandeur n'a pas de sens pour ce masque.
- Les masques à chemin O(régions) — ceux dont `fb`/`ab` valent déjà `null` —
  rendent `null` ici aussi. **Ne pas inventer de valeur** (ADR-0072 : l'absence
  n'est jamais une valeur du domaine).
- La sentinelle `kOomProbeBlockCountUnknown` (0xFFFFFFFF) est le mécanisme déjà
  en place pour ce « non renseigné » ; la réutiliser plutôt que d'en créer un autre.

### Cas limites

- **Aucun appel supplémentaire à `heap_caps_get_info()`.** La valeur est déjà dans
  la structure remplie par l'appel existant. Un appel de plus coûterait un parcours
  du tas complet — or `wmax` (durée du parcours) est déjà **au-dessus** du seuil
  `kOomProbeWarnDurationUs` et **croît avec la fragmentation**, donc précisément
  dans le régime que cette spec sert à diagnostiquer. Ajouter un parcours
  aggraverait ce qu'on mesure.
- **La charge utile de la réponse grossit.** Vérifier qu'aucun tampon de
  sérialisation n'est dépassé — c'est un dépôt qui a déjà eu une ligne de log
  tronquée pour un `%u` de trop (`main.cpp`, `ligne3`, élargi à 176 o le 12/09).
- L'ordre de grandeur attendu se situe entre 200 et 300 Ko sur le masque `i8` :
  vérifier que le type retenu ne tronque pas.

## Comportement actuel

Champs publiés par masque dans `hf` : `f`, `b`, `mf`, `mb`, `wf`, `wb`, `cf`,
`cb`, `n`, `fb`, `ab`. `total_allocated_bytes` est calculé, puis perdu.

## Contraintes techniques

- Aucune allocation dynamique récurrente ajoutée sur le chemin de la route.
- Aucun appel supplémentaire à `heap_caps_get_info()` (voir Cas limites).
- Constantes dans `constants.h`, préfixe `k`.
- **Rétrocompatibilité** : un champ ajouté, aucun retiré, aucun renommé.
  `tools/trace_fragmentation.py` doit continuer à fonctionner **sans modification**
  sur les captures antérieures.

## Fichiers potentiellement concernés

- `src/web_routes_config.cpp` — le helper de sérialisation par masque, à côté de
  `putHeapBlockCountOrNull()` livré par le lot B
- `src/constants.h` — si une constante s'avère nécessaire
- `tools/trace_fragmentation.py` — afficher la nouvelle colonne et, surtout,
  **l'écart entre octets alloués au début et à la fin de la fenêtre** : c'est la
  grandeur qui tranche
- `docs/API.md` — la route change de contrat
- `docs/subsystems/system-health.md` — inventaire des champs de télémétrie

## Livrables — check-list

- [x] A1 — le champ est sérialisé pour chaque masque, `null` là où `fb`/`ab` le sont
- [x] A2 — aucun appel supplémentaire à `heap_caps_get_info()` (sentinelle de source)
- [x] A3 — `tools/trace_fragmentation.py` affiche la colonne et l'écart début/fin
- [x] D1 — tests natifs du helper de sérialisation, `null` compris
- [x] D2 — **validation par mutation** de chaque garde
- [x] C1 — `docs/API.md` et `docs/subsystems/system-health.md`

## ADR attendu ?

**Non.** Publier une valeur déjà calculée n'est pas un arbitrage : il n'y a pas
d'alternative crédible à documenter. Le raisonnement qui justifie la mesure vit
dans la note d'incident du 10/09, qui sera mise à jour quand la fenêtre
d'observation aura répondu.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-system-info?hf=1` — un champ neuf par masque dans `hf`

### Champs WebSocket
Aucun. **Ne pas exposer au WebSocket** : c'est une grandeur de diagnostic, pas une
donnée de supervision, et la parité `/data` ↔ WS est un piège à trois récidives
documentées dans ce dépôt. Si elle devait l'être un jour, ce serait dans les deux
à la fois.

### Topics MQTT
Aucun.

### Documentation à produire / mettre à jour
- `docs/API.md` — contrat de la route
- `docs/subsystems/system-health.md` — inventaire des champs

## Critères d'acceptation

- [x] Le champ est présent pour tout masque où `ab` est non nul, et `null` partout où `ab` l'est
- [x] Aucun appel supplémentaire à `heap_caps_get_info()` sur le chemin de la route
- [x] `tools/trace_fragmentation.py` tourne **sans modification de ses entrées** sur les captures antérieures à cette version
- [ ] Une capture réelle montre une valeur cohérente : `libre + alloués` reste stable à l'échelle du tas, aux métadonnées de l'allocateur près (fenêtre d'observation pas encore rouverte avec ce champ)
- [x] La réponse HTTP n'est pas tronquée (mesuré : +63 o pire cas sur `hf`, 964 o inchangé sans `?hf=1`, très en dessous du plafond de route)

## Plan de test manuel

1. `GET /get-system-info?hf=1`, vérifier le champ sur chaque masque.
2. Relancer `tools/trace_fragmentation.py` sur `/root/pool-captures` : aucune
   erreur sur les captures antérieures, nouvelle colonne sur les postérieures.
3. Sur une fenêtre d'au moins 30 h, comparer l'écart d'octets alloués à l'écart de
   libre. **C'est la mesure qui tranche** : si les octets alloués croissent de
   ~10 Ko pendant que le nombre de blocs oscille, la fuite en octets est établie.

## Notes pour agent

⚠️ **Ne pas élargir le périmètre.** Un champ, déjà calculé, déjà en mémoire. La
tentation sera d'ajouter d'autres grandeurs « pendant qu'on y est » — c'est ce qui
a fait de `bug-080` une spec de 1 984 lignes dont les chantiers utiles étaient
introuvables à la ligne 1 935.

⚠️ **Cette spec ne conclut rien sur la fuite.** Elle fournit l'instrument. La
conclusion viendra d'une fenêtre d'observation, comme pour `fb`/`ab` — et elle
peut parfaitement établir qu'il **n'y a pas** de fuite. C'est un résultat
acceptable : ce qu'on refuse, c'est de l'affirmer sans mesure.
