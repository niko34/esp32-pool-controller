# ADR-0090 — Un détecteur de chute se compare à un maximum glissant auto-réarmé, jamais au relevé précédent

- **Statut** : Accepté
- **Date** : 2026-10-08
- **Décideurs** : architect (contredit la spec initiale, qui pariait « pas d'ADR »), doc-writer (formalisation)
- **Spec(s) liée(s)** : feature-144 (détecteur-de-chute-de-contiguïté-remis-en-service), ADR-0058 (gardes heap sur métrique 8 bits), ADR-0073 (chiffre-consigne porte sa méthode), ADR-0082 (arène circulaire du journal, chemin de journalisation sans allocation)

## Contexte

Le bloc de `main.cpp` qui journalisait une chute du plus gros bloc contigu
8 bits (`maxBloc8`) a été commenté le 2026-07-23 (« demande utilisateur ») :
il émettait toutes les 5 minutes (`kHeapLogIntervalMs`), soit ~288 lignes/j
sur un journal persistant plafonné à 8 Ko — une légitime demande de silence.

Le défaut : la cadence périodique et la détection de chute vivaient dans le
**même `if`**. Éteindre le bruit a éteint le détecteur d'événement avec lui.
Conséquence mesurée le 2026-10-08 : une chute de 18 432 o (−41 %) n'a produit
aucune trace, découverte 30 minutes plus tard au relevé périodique suivant,
sans horodatage ni déclencheur identifiable.

En remettant le détecteur en service seul (feature-144), trois choix de
conception se sont révélés structurants — pas de simples détails
d'implémentation — et contestables séparément :

1. **À quoi comparer le relevé courant ?** Au relevé précédent (delta
   immédiat), ou à un maximum glissant ?
2. **Le seuil de chute est-il un nombre d'octets fixe, ou une fraction de la
   référence ?**
3. **Comment éviter une rafale d'émissions** quand `checkHeapHealth()` passe
   en cadence rapprochée (5 s sous `kLowHeapWarnBytes`) ?

## Décision

Le détecteur compare le relevé courant à un **maximum glissant auto-réarmé**
(`ref`), jamais au relevé précédent ni à un cliquet monotone :

- **`ref` se réarme silencieusement vers le haut à chaque tick** où le relevé
  courant le dépasse (`ref = max(ref, courant)`), sans émettre de ligne de
  remontée. Sans ce réarmement, `ref` ne peut que descendre : le détecteur
  devient un cliquet qui se tait après quelques lignes et rate toute nouvelle
  chute depuis un palier remonté — précisément le scénario qu'on cherche à
  attraper.
- **Le seuil de chute est relatif à `ref`** : `max(4096, ref >> 2)` — un quart
  de la référence, plancher 4 Ko (= `kRestartMaxAllocHeapBytes`, échelle déjà
  posée par le projet).
- **L'anti-rafale est temporel**, pas structurel : une chute franchie pendant
  la fenêtre `kHeapDropMinIntervalMs` (5 min) ne déclenche pas d'émission,
  mais ne ré-ancre pas non plus `ref` — elle reste en attente et sera émise au
  premier instant autorisé, avec son amplitude vraie depuis le palier
  d'origine. L'anti-rafale **décale** la ligne, il ne l'**ampute** jamais.
- **La cadence périodique (tendance) et la détection de chute (événement)
  n'ont plus d'interrupteur commun.** La tendance reste couverte par la trace
  « Heap 8bits » (`kHeap8LogIntervalMs` = 30 min), vivante en continu depuis
  la v2.20.2 ; la détection de chute est un module séparé
  (`src/heap_drop_logic.h`), sans cadence propre — seul un événement réel la
  déclenche.

## Alternatives considérées

- **Comparaison au relevé précédent** (rejetée) — ne détecte qu'une chute
  brutale entre deux ticks consécutifs ; une dégradation étalée sur plusieurs
  ticks, chacun sous le seuil, ne déclenche jamais. Ne corrige pas non plus le
  défaut historique (cliquet) puisqu'il n'y a pas de notion de palier haut à
  réarmer.
- **Cliquet monotone descendant** (`ref` ne remonte jamais, rejetée) — c'est
  le comportement de l'ancien bloc hors cadence périodique : il émet au plus
  quelques lignes par démarrage puis se tait pour de bon. Explicitement
  identifié comme « le scénario d'échec le plus probable de ce lot » dans la
  spec.
- **Réarmement tracé** (ligne de remontée en plus de la ligne de chute,
  rejetée) — réintroduirait du bruit périodique de facto (toute oscillation
  normale du tas produirait une paire remontée/chute), pour un gain
  diagnostique jugé nul : l'amplitude de la chute suivante, mesurée depuis le
  palier haut, porte déjà l'information de la remontée dans son champ
  « avant ».
- **Seuil absolu** `kHeapLogDropBytes = 8192` repris tel quel (rejetée) —
  chiffré sur données réelles (35 boots, dédupliquées et segmentées par
  démarrage) : un seuil absolu de 8 Ko représente 18 % de la référence à
  45 Ko libres mais 80 % à 10 Ko. Il est aveugle précisément dans la bande où
  l'auto-guérison approche, et il **manque** la chute réelle du 13/09
  (23 540 → 16 372 o, −30 %) à quatre paliers de `kRestartMaxAllocHeapBytes`.
- **Anti-rafale structurel** (ex. compteur d'émissions par fenêtre glissante,
  rejetée) — plus complexe à raisonner et à tester qu'un plancher temporel
  simple ; le gain (borner un débit) est déjà obtenu par le plancher
  `kHeapDropMinIntervalMs`, y compris dans le pire cas (cadence 5 s sous
  `kLowHeapWarnBytes`, précédent du crash du 15/08/2026 où un journal sans
  garde a détruit le post-mortem qu'il devait produire).
- **Seuil relatif + maximum glissant auto-réarmé + anti-rafale temporel**
  (retenue) — seule combinaison qui, simulée sur 35 boots réels, produit
  ~0,88 ligne/jour, toutes des chutes ≥ 30 %, sans manquer l'incident nommé du
  13/09 ni celui du 08/10.

## Conséquences

### Positives
- Le détecteur reste silencieux en régime normal (zéro ligne de plus
  qu'aujourd'hui) tout en restant sensible sur toute la plage d'exploitation,
  des hauts niveaux de contiguïté jusqu'aux niveaux bas où l'auto-guérison
  intervient.
- La logique de décision est une fonction pure et totale
  (`src/heap_drop_logic.h`, ADR-0017), testée en natif sur 15 cas incluant un
  rejeu de données réelles embarqué — indépendante du matériel.
- Un prochain épisode de fragmentation sera daté et quantifié dès sa
  première occurrence, au lieu d'être découvert 30 minutes plus tard sans
  horodatage ni déclencheur (comme le 08/10).

### Négatives / dette assumée
- Le champ `ctx` (masque de contexte au moment du relevé) ne nomme pas
  nécessairement le déclencheur exact d'une chute : il indique ce qui était
  en cours, pas une causalité prouvée. Identifier la cause reste le
  périmètre de feature-133, pas de ce détecteur.
- La forme additive naïve du test de seuil (`courant + seuil <= ref`)
  déborde pour `courant` proche de `UINT32_MAX`, produisant un faux positif
  qui corromprait durablement `ref`. Corrigée par une garde `courant < ref`
  avant toute arithmétique ; le risque résiduel (lecture de tas déjà corrompu
  produisant une valeur garbage) est documenté par un test qui passe et
  assume ce comportement en l'état (non traité ici, cf. notes
  test-engineer de la spec).

### Ce que ça verrouille
- **Toute instrumentation future ajoutant une cadence périodique à
  `checkHeapHealth()` (ou à tout autre détecteur d'événement du projet) doit
  utiliser un déclencheur séparé** — le couplage cadence/événement dans un
  même `if` est désormais un défaut de conception identifié et nommé, pas une
  simple anecdote d'implémentation.
- Tout changement du seuil de chute doit repasser par un chiffrage sur
  données réelles (rejeu de boots), pas par une valeur reprise « telle
  quelle » d'une autre grandeur du projet — c'est exactement l'erreur que
  cette spec a évitée de justesse après une première correction erronée.

## Références

- Code : [`src/heap_drop_logic.h`](../../src/heap_drop_logic.h) (module pur),
  [`src/main.cpp`](../../src/main.cpp) `checkHeapHealth()` (coquille impure),
  [`src/constants.h`](../../src/constants.h) (`kHeapDropMinIntervalMs`,
  `kHeapDropFloorBytes`, `kHeapDropRefShift`)
- Tests : `test/test_native_heap_drop_logic/`,
  `test/test_native_heap_telemetry_guard/` (sentinelles D4)
- Spec : `specs/features/done/feature-144-detecteur-de-chute-de-contiguite-remis-en-service.md`
- Doc externe : [`docs/subsystems/system-health.md`](../subsystems/system-health.md#détecteur-de-chute-de-contiguïté-remis-en-service-v2600),
  [`docs/subsystems/logger.md`](../subsystems/logger.md)
