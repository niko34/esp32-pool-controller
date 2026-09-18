# feature-131 — La sauvegarde de l'historique cesse de bâtir le JSON en RAM

## Métadonnées
- **ID** : feature-131
- **Slug** : la-sauvegarde-de-l-historique-cesse-de-batir-le-json-en-ram
- **Type** : bug de conception
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : 2.54.0
- **Dernière mise à jour** : 2026-09-18
- **Spec(s) liée(s)** : `specs/features/done/feature-129-…` (même famille) · feature-132 (conséquence directe) · ADR-0070 (admission unique) · ADR-0082

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] **Historique / persistance**
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Supprimer le plus gros pic d'allocation du firmware : la sauvegarde et le
chargement de l'historique construisent aujourd'hui **tout le document JSON en
mémoire** avant de l'écrire ou après l'avoir lu. Les remplacer par une écriture
et une lecture **en flux**, point par point.

## Contexte — ce n'est pas une hypothèse, le code le dit lui-même

`src/history.cpp:587`, commentaire d'origine :

> « on vient de construire le `JsonDocument` et ses `String` de sérialisation,
> soit **des dizaines de kilooctets** »

Trois faits qui en font la priorité suivante après feature-129 :

1. **C'est le plus gros transitoire du firmware**, et il est **périodique** — pas
   un pic de démarrage qu'on subit une fois.
2. **C'est le site exact du coredump du 05/09/2026.** Le commentaire de
   `history.cpp:583-592` décrit la seconde admission `fsGuardAdmit` comme « LE
   SITE EXACT DU COREDUMP », placée après `measureJson` précisément parce que la
   marge validée par la première mesure est **dépensée entre les deux**.
3. **Le chargement fait pareil en sens inverse** : `history.cpp:729-730`,
   `deserializeJson(doc, f)` monte l'intégralité du fichier en RAM au démarrage.

Volumétrie : `MAX_RAW_POINTS` (8 h en pas de 5 min) + `MAX_HOURLY_POINTS`
(15 jours) + `MAX_DAILY_POINTS` (75 jours), chacun portant jusqu'à 6 champs.

⚠️ **Lien avec feature-129, à ne pas confondre.** feature-129 a traité la
fragmentation **par petits blocs persistants** (le journal). Celle-ci traite un
**gros bloc transitoire**. Ce sont deux mécanismes distincts : le premier
détruisait la contiguïté durablement, le second exige une grande zone contiguë
au moment précis où il s'exécute. Le premier est corrigé ; le second explique
pourquoi la garde exige encore 8 192 octets contigus.

## Comportement attendu

### Écriture en flux
Écrire le JSON **directement dans le fichier**, point par point, sans jamais
matérialiser le document complet. Le pic d'allocation doit tomber à la taille
d'un point plus le tampon de fichier.

### Lecture en flux
Idem au chargement : `ArduinoJson` sait désérialiser par morceaux
(`deserializeJson` sur un filtre, ou parcours incrémental). Ne pas monter le
fichier entier.

### Conséquence attendue, à MESURER et non à supposer
`max_alloc_8bit` ne doit plus **chuter** pendant une sauvegarde. C'est le critère
qui décide si ce lot a servi à quelque chose.

## Comportement actuel

`history.cpp:552` construit un `JsonDocument`, `history.cpp:663` le sérialise
d'un bloc ; `history.cpp:729` remonte tout en RAM au chargement. La garde
`fsGuardAdmit` est appelée **deux fois** (ADR-0070 §d) précisément pour survivre
à ce pic.

## Contraintes techniques

- **Le format de `/history.json` ne doit PAS changer.** Le frontend et les
  exports le consomment. Un changement de format est hors périmètre et
  transformerait un travail mémoire en refonte fonctionnelle.
- **L'écriture ne doit pas devenir moins atomique qu'aujourd'hui.** Le mécanisme
  existant (fichier temporaire puis `rename`, avec réconciliation au démarrage)
  protège d'une coupure en cours d'écriture. Le flux ne doit pas l'affaiblir :
  une sauvegarde interrompue ne doit **jamais** laisser un `/history.json`
  tronqué en place. Le projet a déjà perdu une partition sur ce thème
  (boot-loop `lfs_alloc`, 07/2026).
- **La détection d'écriture partielle doit survivre.** `history.cpp:666` vérifie
  aujourd'hui le retour de `serializeJson` contre la taille attendue. En flux,
  cette garantie doit être reconstruite, pas perdue.
- **`esp_task_wdt_reset()`** dans la boucle d'écriture : elle devient longue.
- **ADR-0070** : une seule ouverture de fichier, par le helper unique. Les deux
  admissions `fsGuardAdmit` peuvent probablement fusionner une fois le pic
  supprimé — **à traiter explicitement**, pas à laisser en l'état par prudence :
  une garde qui protège d'un pic disparu est du bruit qui masque la prochaine.

## Fichiers potentiellement concernés

`src/history.cpp`, `src/history.h` · `src/fs_guard.*` (admissions) ·
`src/constants.h` · un module pur `src/history_stream_logic.*` si la logique de
découpe s'extrait (ADR-0017)

## Livrables — check-list
- [ ] Écriture en flux, format de fichier **identique** (à prouver par comparaison octet à octet d'un fichier avant/après)
- [ ] Lecture en flux
- [ ] Atomicité et détection d'écriture partielle **conservées**
- [ ] Mesure avant/après de `max_alloc_8bit` **pendant** une sauvegarde
- [ ] Réexamen des deux admissions `fsGuardAdmit`
- [ ] Tests natifs de la logique pure extraite + **validation par mutation**
- [ ] ADR si le compromis atomicité/mémoire mérite d'être figé

## ADR attendu ?

**Probablement.** Si l'atomicité doit être reconstruite différemment, c'est un
arbitrage qui verrouille un choix futur.

## Éléments visibles (pour doc-writer)
Aucun endpoint nouveau. `docs/subsystems/history.md` (ou équivalent),
`CHANGELOG.md` si le comportement observable change (sauvegardes qui ne sont plus
reportées).

## Critères d'acceptation
- [ ] `max_alloc_8bit` **ne chute pas** pendant une sauvegarde d'historique
- [ ] Le fichier produit est **identique** à celui de la version précédente pour un même jeu de points
- [ ] Une coupure simulée en cours d'écriture ne laisse jamais un `/history.json` tronqué en place
- [ ] Aucune sauvegarde reportée (`loggerFopenDeferralCount` / compteurs `fs_guard` à 0) sur ≥ 24 h
- [ ] `pio test -e native` vert, zéro régression

## Plan de test manuel
1. Remplir l'historique au maximum (`MAX_RAW_POINTS` atteints), déclencher une
   sauvegarde, observer `max_alloc_8bit` **pendant** — pas seulement avant/après.
2. Comparer octet à octet le `/history.json` produit avant et après le lot.
3. Couper l'alimentation pendant une écriture, redémarrer, vérifier que
   l'historique est cohérent (soit l'ancien, soit le nouveau, jamais un mélange).
4. Redémarrer avec un gros historique et mesurer le pic au chargement.

## Notes pour agent

⚠️ **Le piège de ce lot est l'atomicité, pas la mémoire.** Il est facile
d'écrire en flux et de perdre la garantie qu'une coupure ne corrompt rien. La
partition `history` a déjà été corrompue une fois (07/2026, boot-loop
`lfs_alloc` division par zéro) : une régression ici ne se paie pas en
performance, elle se paie en appareil qui ne démarre plus.

⚠️ **Mesurer PENDANT, pas autour.** L'erreur commise en feature-129 était de
prédire un gain sans le mesurer au bon moment. Ici, `max_alloc_8bit` relevé avant
et après une sauvegarde peut être identique alors que le pic intermédiaire a
frôlé le plancher. Il faut sonder pendant l'écriture.

## Hors périmètre
- Tout changement du format de `/history.json`.
- Les seuils de la garde fichiers (c'est feature-132, qui dépend de ce lot).
