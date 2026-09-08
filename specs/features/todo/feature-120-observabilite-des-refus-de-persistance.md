## Métadonnées
- **ID** : feature-120
- **Slug** : observabilite-des-refus-de-persistance
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.49.0
- **Dernière mise à jour** : 2026-09-08
- **Spec(s) liée(s)** : `bug-099` (l'a détachée, point 3), `bug-080` (lot C — consommateur direct), `bug-nospc-partition-history` (a créé `nospc_cnt`)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun chemin de dosage
- [x] ux-ui-designer — nouvelle surface sur la page Système, et arbitrage de densité
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Exposer à distance les compteurs qui disent qu'une **écriture a été refusée** :
aujourd'hui, rien ne permet de constater depuis l'interface que l'appareil a
cessé de persister son historique ou ses journaux.

## Contexte

Détachée du **point 3 de `bug-099`**, qui la laissait explicitement « à arbitrer ».
L'arbitrage a été rendu le 2026-09-08 : **hors périmètre** de `bug-099`, parce que
faire entrer une surface UI dans un lot qui manipule le journal persistant au
démarrage aurait dilué la revue sur sa partie **destructrice de données**.

Le défaut de fond est celui relevé en construisant `bug-099` : **le mode de
défaillance est le silence**. `historySaveAdmit()` refuse la sauvegarde et
journalise un avertissement une fois par heure ; `nospc_cnt`
(`kHistoryNospcCountNvsKey`, `constants.h`) compte ces refus en NVS — et **aucun
endpoint ne l'expose**. L'historique peut donc cesser d'être écrit sans qu'aucun
signal n'atteigne l'utilisateur, et la seule trace vit dans le journal… dont la
rétention est de quelques dizaines de minutes.

⚠️ **La priorité n'est plus la saturation.** Mesuré pendant `bug-099` : sous une
partition de 64 Ko un `.tmp` orphelin faisait refuser **73 tailles sur 124** de la
bande réaliste ; sous le layout v5 (256 Ko, ADR-0068), **zéro refus**. Ce n'est
donc pas un correctif d'urgence mémoire — c'est de l'**observabilité**, et sa
valeur est de rendre visible une classe de refus qui, aujourd'hui, ne laisse
aucune trace durable.

## Comportement attendu

Un utilisateur ouvrant **Paramètres → Système** doit pouvoir répondre à
« l'appareil enregistre-t-il bien mes mesures ? » sans lire le journal.

- Un bloc « **Persistance** » affiche le nombre cumulé de refus d'écriture et la
  date du dernier enregistrement réussi.
- **Zéro refus est l'état normal** et doit se lire comme tel — pas comme une
  absence d'information. Un compteur à zéro est un résultat, pas un vide.
- Un compteur non nul est **signalé visuellement**, avec l'indication de ce que
  l'utilisateur peut faire (exporter puis réduire l'historique, ou vérifier
  l'espace).

### Cas limites
- **Horloge non synchronisée** : `last_ok` vaut 0 en NVS → afficher « inconnu »,
  **jamais** une date d'époque. Idiome déjà posé par l'ADR-0072 (l'absence de
  valeur ne se représente jamais par une valeur du domaine).
- **Compteur jamais écrit** (appareil neuf) : la clé NVS est absente → zéro, pas
  une erreur.
- **Famine mémoire au moment de la requête** : la route doit se comporter comme
  ses voisines (garde 503 d'ADR-0067), sans ouvrir de namespace NVS dans un
  handler async si l'arbitrage ci-dessous l'interdit.

## Comportement actuel

`nospc_cnt` et `last_ok` sont **écrits** en NVS (`history.cpp`) et **jamais lus
par personne**. Les compteurs de refus d'ouverture de fichier
(`fsGuardDeferralCount()`, `historyFopenDeferralCount()`,
`loggerFopenDeferralCount()`) partent dans le payload retenu `<base>/diagnostic`,
donc survivent au redémarrage — mais ne sont pas non plus exposés en HTTP.

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- AsyncWebServer : aucun handler bloquant > 50 ms — **c'est la contrainte
  structurante de cette spec** (cf. arbitrage)
- **Parité `/get-system-info` ↔ WebSocket** si un champ temps réel est ajouté :
  trois récidives en trois versions sur ce piège (un champ WS-seul ne disparaît
  pas, il **clignote**)
- Ne pas élargir la surface non authentifiée : `bug-113` vient de la réduire de
  **80,73 %**

## Fichiers potentiellement concernés
- `src/web_routes_config.cpp` — `/get-system-info` (handler existant)
- `src/history.{h,cpp}` — accesseurs de lecture de `nospc_cnt` / `last_ok`
- `src/constants.h` — clés NVS déjà présentes, éventuel seuil d'alerte
- `data/index.html`, `data/app.js`, `data/app.css` — bloc « Persistance »
- `docs/API.md` — champs nouveaux de `/get-system-info`
- `docs/features/page-settings.md` — le bloc et sa lecture
- `docs/subsystems/history.md` — quels compteurs sont désormais exposés

## Livrables — check-list
- [ ] A1 — Accesseurs de lecture pour `nospc_cnt` et `last_ok`, selon l'arbitrage
      « lecture NVS par requête vs cache au boot » tranché par l'architecte
- [ ] A2 — Champs ajoutés à `/get-system-info`, absence représentée sans valeur
      du domaine (ADR-0072)
- [ ] A3 — Décider et documenter si les compteurs `fsGuard*` rejoignent le lot ou
      restent au seul payload `diagnostic`
- [ ] B1 — Bloc « Persistance » sur la page Système, zéro lisible comme un résultat
- [ ] B2 — Signalement visuel d'un compteur non nul, avec l'action possible
- [ ] C1 — `docs/API.md`, `docs/features/page-settings.md`, `docs/subsystems/history.md`
- [ ] C2 — `CHANGELOG.md` + capture d'écran à signaler à l'humain
- [ ] D1 — Tests : absence de clé NVS, `last_ok` à 0, compteur non nul, parité
      `/get-system-info` ↔ WS si champ temps réel
- [ ] D2 — Mutation : au moins un mutant sur la représentation de l'absence

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : **aucun** — enrichissement de `/get-system-info` (authentifié)
- Champs WebSocket nouveaux : **à arbitrer** (un compteur de refus n'est pas une donnée temps réel)
- Topics MQTT nouveaux : aucun
- Fonctions publiques : lecteurs de `nospc_cnt` / `last_ok` dans `src/history.h`

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 2
- **Sujet pressenti** : « Lire NVS dans un handler async, ou mettre en cache au
  boot ? » — la question dépasse ce lot : elle vaudra pour tout compteur persistant
  qu'on voudra exposer ensuite, et le choix verrouille la forme des suivants.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-system-info` — champs ajoutés

### Champs WebSocket
- Aucun a priori — à confirmer par l'arbitrage

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/features/page-settings.md`, `docs/subsystems/history.md`,
  `CHANGELOG.md`, `docs/user/src/guide-utilisation.html` **si** le bloc est
  décrit au guide utilisateur

## Notes

Trois compteurs voisins existent déjà et forment un ensemble avec `nospc_cnt` :
les exposer **un par un** produirait une page Système en sédiment. L'arbitrage
« lesquels, et sous quel regroupement » fait partie du travail, et c'est le motif
pour lequel `ux-ui-designer` est requis avant tout code d'interface.
