## Métadonnées
- **ID** : feature-128
- **Slug** : recensement-du-tas-et-identification-des-intrus
- **Type** : feature — instrumentation de diagnostic
- **Statut** : done
- **Priorité** : **haute** — sans elle, la prochaine fenêtre d'observation de 40 h refera la mesure du 12-13/09 avec les mêmes angles morts
- **Version cible** : à déterminer
- **Dernière mise à jour** : 2026-09-14
- **Spec(s) liée(s)** : `feature-127` (octets alloués, même lot), `feature-122` (lot C-1a), `feature-125` (réserves du lot C), incident [`2026-09-10-recidive-fragmentation-v2500.md`](../../../docs/incidents/2026-09-10-recidive-fragmentation-v2500.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] Authentification — la route lit la mémoire du processus
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun dosage, aucune régulation, aucune calibration
- [ ] ux-ui-designer — aucune surface UI
- [x] embedded-developer
- [x] web-ui-developer — la route est de son domaine
- [x] **security-reviewer — INVOCATION OBLIGATOIRE, DEUX PASSES** : route neuve dans `web_routes_*` **et** lecture du contenu du tas. P1 sur le DESIGN avant code, P2 sur le DIFF.
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Recenser le tas interne 8 bits bloc par bloc (`heap_caps_walk`), et permettre de
lire le contenu d'un bloc désigné, afin d'identifier **ce qui découpe le grand
bloc contigu** — quatre allocations longue durée observées le 13/09 entre 20,7 h
et 28,2 h d'uptime.

## Contexte

La fenêtre de mesure du 12-13/09 a établi cinq faits (voir la note d'incident) :
la fragmentation est réelle, le plus gros bloc ne remonte **jamais** (0 remontée
sur 28 intervalles), il s'effondre en **quatre marches discrètes**
(23 540 → 16 372 → 14 836 → 10 228 → 7 924), et le palier tient ensuite 13 h.

**Ce qu'on ne sait pas : qui fait ces quatre découpes.** Sans ce nom, tout
correctif est une conjecture — et la note d'incident porte une interdiction
explicite d'agir sans preuve instrumentée.

### Pourquoi les instruments existants ne suffisent pas

- `heap_caps_get_info()` donne des **agrégats** : combien de trous, combien de
  blocs. Jamais où ni quoi.
- Le **traçage natif de l'IDF est hors d'atteinte** : les bibliothèques Arduino
  précompilées de pioarduino `55.03.37` sont construites avec
  `CONFIG_HEAP_TRACING_OFF=y` et sans `CONFIG_HEAP_TASK_TRACKING`. Les rallumer
  imposerait de recompiler ces bibliothèques, donc d'abandonner le paquet épinglé
  qui stabilise ce projet depuis la migration core 3.
- **Identifier par la TAILLE ne marchera pas.** Le premier suspect est le
  `logger` (`std::vector<LogEntry>` avec `String message`, `logger.h:22,35`) :
  ce sont des allocations de taille **variable**, dépendant du texte. Aucune
  constante à retrouver par `grep`. C'est l'objection qui a fait retenir la
  lecture de contenu plutôt que le seul relevé de tailles.

### L'idée retenue

`heap_caps_walk()` (disponible pour l'ESP32 dans cet IDF) appelle un callback
pour **chaque bloc** avec `{void* ptr; size_t size; bool used;}`. Deux
recensements espacés isolent les blocs occupés apparus **au milieu d'une zone
auparavant libre**. Lire leurs premiers octets les identifie : une `String` de
journal se lit **en clair**.

## Comportement attendu

### A — Recensement

Une route de diagnostic rend, pour le masque interne 8 bits :

- les **N plus gros blocs libres** avec leur adresse et leur taille ;
- les blocs **occupés** dont l'adresse tombe dans une plage demandée, avec
  adresse et taille ;
- les agrégats de contrôle (nombre de blocs libres / occupés), qui doivent
  **concorder** avec `fb`/`ab` de `/get-system-info?hf=1` — c'est le test de
  cohérence de l'instrument lui-même.

### B — Lecture de contenu

Une route rend les **premiers octets** d'un bloc désigné par son adresse, en
hexadécimal **et** en ASCII imprimable, afin qu'une `String` se lise directement.

### Cas limites

- **Coût du parcours.** `heap_caps_walk` parcourt tout le tas. `wmax` (durée du
  parcours mesurée par la sonde existante) vaut déjà **711 µs**, **au-dessus** du
  seuil `kOomProbeWarnDurationUs` de 300 µs, et **croît avec la fragmentation** —
  donc précisément dans le régime que cette spec sert à diagnostiquer. Le
  recensement doit donc être **strictement à la demande**, jamais périodique,
  jamais dans la sonde existante.
- **Verrouillage.** Le parcours prend le verrou du tas. Interdiction de
  l'exécuter depuis une ISR, et à instruire depuis `async_tcp` pendant une
  écriture flash.
- **L'agrégat ne doit pas allouer** pendant qu'il parcourt le tas : tampon
  statique à taille fixe, jamais de `String` ni de `std::vector` dans le callback.
- **Adresse invalide** demandée en lecture : refus explicite, jamais de
  déréférencement. La plage valide est bornée par `walker_heap_into_t`
  (`start`/`end`), rendu par le même callback.

## Comportement actuel

Aucune introspection bloc par bloc n'existe. `/get-system-info?hf=1` rend des
agrégats par masque.

## Contraintes techniques

- **`heap_caps_dump()` est inutilisable** : il écrit sur l'UART, or il n'y a
  **aucun port série** dans l'environnement de développement (LXC, OTA seul).
  Tout doit passer par HTTP.
- Aucune allocation dynamique dans le chemin du callback de parcours.
- Aucun appel supplémentaire à `heap_caps_get_info()` sur le chemin de la sonde.
- Aucun changement de `sdkconfig`, aucune recompilation des bibliothèques Arduino.

## 🔒 Sécurité — le point central de cette spec

**Lire le contenu du tas expose tout ce qui vit en RAM** : mot de passe WiFi,
identifiants MQTT, jeton de session, corps de requêtes en vol. Le projet sert en
**HTTP clair sur le LAN** et a un audit de sécurité encore ouvert.

**Décision de l'utilisateur du 2026-09-14 : la route de lecture est acceptée, en
build de DIAGNOSTIC, non livré.** Il revient à `security-reviewer` (passe P1, sur
le design) de dire **par quel mécanisme** cette non-livraison est garantie, et de
rendre un verdict contraignant. Les pistes à instruire, sans préjuger :

- compilation conditionnelle, la route n'existant pas dans le binaire de production ;
- exigence d'authentification renforcée par rapport aux autres routes ;
- bornage de la quantité lisible par requête ;
- caviardage des plages connues pour porter des secrets, si c'est possible sans
  créer une fausse impression de sûreté.

⚠️ **Une garde qui se contourne par un drapeau de configuration n'est pas une
garantie de non-livraison.** Ce point est explicitement soumis à
`security-reviewer`, qui peut **refuser** la forme proposée.

Le **recensement (A)** n'expose pas de contenu : adresses et tailles seulement.
Il peut relever d'un régime moins strict que la **lecture (B)**. À trancher par
`security-reviewer`, pas par l'architecte.

## Fichiers potentiellement concernés

- `src/heap_metrics.{h,cpp}` — le recensement, à côté des sondes existantes
- `src/web_routes_data.cpp` ou un fichier de routes de diagnostic dédié
- `src/constants.h` — bornes (nombre de blocs rendus, octets lisibles par requête)
- `platformio.ini` — si la compilation conditionnelle est retenue
- `tools/` — un script de comparaison de deux recensements

## Livrables — check-list

- [x] A1 — recensement par `heap_caps_walk`, agrégat en tampon statique, zéro allocation dans le callback
- [x] A2 — les compteurs du recensement **concordent** avec `fb`/`ab` de la route existante (mécanisme codé et testé ; concordance non encore observée sur cible réelle)
- [x] A3 — recensement strictement à la demande, jamais périodique (sentinelle de source)
- [x] B1 — lecture de contenu bornée, adresse validée contre les bornes du tas
- [x] S1 — **mécanisme de non-livraison** validé par `security-reviewer` (P1 design, P2 diff — GO sous conditions, 8 correctifs de `code-reviewer` appliqués et vérifiés)
- [x] T1 — `tools/compare_census.py` : isole les blocs APPARUS entre deux recensements.
      Rejette explicitement un corps 404 enregistré en `.json` (piège relevé par
      `orchestrator` : `capture-logs.sh` écrit ce que `curl` rend, sans contrôle).
      **Ne qualifie jamais une adresse récurrente de « même allocation »** — TLSF
      réutilise une adresse libérée ; une même adresse avec un `alloc_size`
      DIFFÉRENT est signalée comme réallocation, à ne pas suivre. N'affiche
      **aucun contenu** (S11) : sa sortie est collable dans la note d'incident.
- [x] D1 — tests natifs de la logique pure d'agrégation et de validation d'adresse (`test/test_native_heap_census_logic/`)
- [x] D2 — **validation par mutation** de chaque garde (mutants nommés dans `test_native_heap_census_logic` et `test_native_heap_telemetry_guard`, dont M9 « le test le plus important du lot » sur S4)
- [x] C1 — `docs/API.md`, `docs/subsystems/system-health.md`, ADR

## ADR attendu ?

**Oui.** Deux arbitrages avec alternative crédible : embarquer une route qui lit
la mémoire du processus (et par quel mécanisme elle ne part pas en production), et
le choix de `heap_caps_walk` plutôt que le traçage natif de l'IDF — avec le motif
mesuré (`CONFIG_HEAP_TRACING_OFF=y` dans les bibliothèques précompilées épinglées).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Route de recensement (nom à fixer par `architect`)
- Route de lecture de contenu (idem) — **présence conditionnée** par S1

### Champs WebSocket
Aucun. Ne pas exposer au WebSocket.

### Topics MQTT
Aucun.

## Critères d'acceptation

- [ ] Le recensement rend des blocs dont les compteurs concordent avec `fb`/`ab` (à vérifier sur cible réelle — pas encore observé)
- [x] Aucune allocation dynamique pendant le parcours (prouvé par mutation — `test_m10_no_allocation_or_string_in_either_walk_callback`)
- [x] Le recensement n'est jamais déclenché périodiquement (site d'appel unique dans `main.cpp`, vérifié par mutation)
- [x] Une adresse hors des bornes du tas est refusée sans déréférencement (S4, mutant M9 désigné « le test le plus important du lot »)
- [x] `security-reviewer` rend **GO** ou **GO SOUS CONDITIONS** sur le mécanisme de non-livraison (P1 puis P2, GO sous conditions)
- [ ] Deux recensements espacés permettent d'isoler un bloc occupé apparu dans une zone auparavant libre — **outil livré (T1), reste à mener sur cible** pendant la prochaine fenêtre

## Plan de test manuel

1. Recensement, vérifier la concordance avec `/get-system-info?hf=1`.
2. Lire le contenu d'un bloc connu pour porter une `String` de journal : **le
   texte doit se lire en clair**. C'est le test qui valide toute la démarche.
3. Adresse aberrante (0, 0xFFFFFFFF, une adresse flash) → refus, pas de crash.
4. Deux recensements espacés de plusieurs heures sur cible, comparés par le script.

### Conditions d'exploitation S11/S12 (`security-reviewer`) — ce sont des consignes humaines, rien ne les vérifie à la place de l'opérant

Ces deux points ne sont **pas** du code : aucune sentinelle, aucun test
automatisé ne les couvre. Ils s'appliquent à **chaque** usage de R3 pendant
la fenêtre d'observation.

- **S11 — aucune sortie de `/debug/heap_peek` (hex/ASCII) n'est versionnée.**
  Ni copiée dans `docs/incidents/`, ni collée dans une spec, ni dans un
  commentaire de code. Toute note d'incident qui exploite une lecture de
  contenu ne cite que **adresse, taille, identification de l'allocation** —
  **jamais le contenu lu**. Les captures brutes (hex/ASCII) vivent **hors du
  dépôt**.
- **S12 — reflash obligatoire après la fenêtre, avec preuve consignée.** À la
  fin de la fenêtre d'observation, reflasher un binaire de **release**
  (compilé sans `POOL_HEAP_PEEK`) et consigner la preuve dans la note
  d'incident : `GET /debug/heap_census` → `"build":{"peek":0}`, ou `404` sur
  `GET /debug/heap_peek`. Sans cette preuve, la fenêtre suivante ne doit pas
  être ouverte sur la même base.

## Notes pour agent

⚠️ **Cette spec ne corrige rien.** Elle fabrique l'instrument qui permettra de
décider d'un correctif. Toute tentative de « corriger la fragmentation au
passage » est hors périmètre — et la note d'incident interdit expressément de
toucher aux constantes ou structures du `logger` sans la preuve que cet
instrument doit produire.

⚠️ **Le premier suspect est le `logger`**, mais ce n'est **pas** une conclusion :
c'est ce que l'instrument doit confirmer ou infirmer. Ne pas concevoir
l'instrument autour de cette hypothèse.
