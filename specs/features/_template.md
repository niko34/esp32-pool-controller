<!--
Template de spec FEATURE pour ESP32 Pool Controller.
Copier ce fichier dans specs/features/todo/ sous le nom :
    feature-<NNN>-<slug-court>.md
Remplir les sections, supprimer ce bloc commentaire et les lignes « [remplir] ».
Les sections marquées (optionnelle) peuvent être supprimées si non pertinentes.
-->

## Métadonnées
- **ID** : feature-NNN
- **Slug** : [remplir : quelques-mots-separes-par-tirets]
- **Type** : feature
- **Statut** : todo
- **Priorité** : basse | normale | haute | critique
- **Version cible** : [remplir : 1.X.Y d'après `src/version.h`] ou `N/A` si référence
- **Dernière mise à jour** : [remplir : YYYY-MM-DD]
- **Spec(s) liée(s)** : [remplir ou « aucune »]

## Domaines touchés
<!-- Cocher tout ce que la spec modifie ou consomme — guide l'orchestrator -->
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
<!-- Décoché = non applicable, coché = à invoquer. L'orchestrator applique cette liste. -->
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **obligatoire si** dosage / régulation / calibration touchés
- [ ] ux-ui-designer — **obligatoire si** nouvelle surface UI / refonte visuelle / arbitrage
      entre plusieurs mises en page / **3ᵉ ajout successif sur le même écran** (règle
      d'accumulation, cf. CLAUDE.md). ⚠️ Dans ce dernier cas son mandat porte sur
      **l'ÉCRAN ENTIER**, pas sur l'ajout du jour — c'est l'erreur qui a dégradé la page
      Produits : trois interventions correctes, aucune vue d'ensemble.
- [ ] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
<!-- 1-3 phrases : quoi, pour qui, résultat attendu. Pas de détail technique ici. -->
[remplir]

## Contexte
<!-- Pourquoi cette feature. Problème actuel, besoin utilisateur, références. -->
[remplir]

## Comportement attendu
<!--
Décrire l'expérience / les règles métier, pas l'implémentation.
Structurer par carte UI, sous-fonction ou flux utilisateur si utile.
Préciser les cas nominaux ET les cas limites.
-->
[remplir]

### Cas limites
- [remplir : WebSocket déconnecté, capteur stale, limite atteinte, filtration OFF, etc.]

## Comportement actuel
<!-- État actuel du firmware / UI avant modification. « Rien n'existe » est une réponse valide. -->
[remplir]

## Contraintes techniques
- ⚠️ **RESPONSIVE (si `data/` est touché)** : téléphone **ET** tablette **ET** ordinateur.
  Concevoir **à 390 px d'abord** — le téléphone est l'écran de référence, pas un cas
  dégradé. Un `flex-wrap` est le cas NORMAL en mobile, pas un repli. Compter les
  caractères des libellés placés sur une ligne partagée. Cf. CLAUDE.md § Principes.
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB — vérifier avec `./build_fs.sh` si ajouts frontend
- AsyncWebServer : aucun handler bloquant > 50 ms
- WebSocket pour toute donnée temps réel ; HTTP authentifié pour les actions
- [remplir contraintes spécifiques : ADC, I2C, mutex, heap, etc.]

## Fichiers potentiellement concernés
<!-- Lister avec une courte justification. Préciser [nouveau] si création. -->
- `data/index.html` — [pourquoi]
- `data/app.js` — [pourquoi]
- `data/app.css` — [pourquoi]
- `src/web_routes_xxx.cpp` — [pourquoi]
- `src/ws_manager.cpp` — [champs à ajouter/consommer]
- `src/constants.h` — [nouvelles constantes éventuelles]
- `docs/xxx.md` — [pourquoi]

## Livrables — check-list
<!--
Liste PLATE et NUMÉROTÉE des items à livrer, consommée telle quelle par l'orchestrator (étape 0).
Une ligne = un item vérifiable. Préfixes suggérés : A = firmware, B = UI, C = docs, D = tests.
Ne pas imbriquer ; si un item est gros, le scinder.
-->
- [ ] A1 — [remplir]
- [ ] A2 — [remplir]
- [ ] B1 — [remplir]
- [ ] C1 — [remplir]

## Nouvelles signatures / API (optionnelle)
<!-- À remplir si la spec introduit du nouveau public. Guide l'architect. -->
- Endpoints HTTP nouveaux : `POST /api/...` (auth ?)
- Champs WebSocket nouveaux : `ph_xxx`, `orp_xxx`
- Topics MQTT nouveaux : `pool/.../state` (retain ?) + auto-discovery HA associé
- Fonctions publiques : `bool doFoo(int);` dans `src/xxx.h`
- Constantes : `constexpr unsigned long kFooMs = ...;`

## Sécurité chimique (optionnelle — obligatoire si dosage/régulation/calibration)
<!-- Aide `pool-chemistry` à statuer GO / NO GO rapidement. -->
- Dosage impliqué : oui / non — produit : [pH- / chlore / autre]
- Limites journalières / horaires concernées : [lister]
- Filtration requise avant action : oui / non
- Calibration capteur modifiée : oui / non
- Algorithme PID touché : oui / non
- Risque identifié : [remplir ou « aucun »]

## ADR attendu ?
<!--
Déclencheurs ADR (cf. `.claude/agents/orchestrator.md` §Création d'ADR) :
  1. Contrainte structurante imposée au reste du système
  2. Décision invisible dans le code (refus, compromis entre options équivalentes)
  3. Alternatives crédibles à tracer
  4. Verrouillage long terme (compat OTA, contrat MQTT public, API stable)
Cocher « probable » si au moins UN critère est a priori atteint ; `architect` / `pool-chemistry`
confirmeront pendant le cycle.
-->
- **Probable** : oui / non
- **Critère(s) candidat(s)** : [1 | 2 | 3 | 4 | aucun]
- **Sujet pressenti** : [remplir ou « aucun »]

## Éléments visibles (pour doc-writer)
<!-- Ce qui doit apparaître dans la doc externe. Laisser vide si rien de visible. -->

### Endpoints HTTP concernés
- [remplir ou « aucun »]

### Champs WebSocket
- [remplir ou « aucun »]

### Topics MQTT
- [remplir ou « aucun »] — préciser auto-discovery HA si nouveau capteur/switch

### Documentation à produire / mettre à jour
<!--
Appliquer la matrice de déclenchement de `.claude/agents/orchestrator.md` :
- zone UI modifiée → `docs/features/page-<nom>.md`
- composant firmware modifié → `docs/subsystems/<composant>.md`
- décision structurante → `docs/adr/NNNN-<slug>.md` (via doc-writer)
- contrat externe changé → docs transverses (API / MQTT / BUILD / UPDATE_GUIDE)
-->
#### Docs vivantes (1 par surface touchée)
- `docs/features/page-<nom>.md` — [remplir ou « aucune page UI touchée »]
- `docs/subsystems/<composant>.md` — [remplir ou « aucun composant firmware touché »]

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — [si endpoints / champs WS]
- `docs/MQTT.md` — [si topics / auto-discovery HA]
- `docs/BUILD.md` — [si toolchain, partitions, scripts build]
- `docs/UPDATE_GUIDE.md` — [si migration OTA / NVS ou note user-facing]
- `README.md` — [si fonctionnalité visible utilisateur]
- `CHANGELOG.md` — entrée à ajouter sous la version cible

#### Docs immuables
- `docs/adr/NNNN-<slug>.md` — [à créer si ADR attendu ci-dessus]

## Critères d'acceptation
<!-- Testables, observables. Pas « implémenter X » mais « quand Y, alors Z ». -->
- [ ] [remplir]
- [ ] [remplir]
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous

## Plan de test manuel
<!-- Scénarios concrets pour test-engineer (en complément des tests unitaires). -->
1. [remplir scénario nominal]
2. [remplir scénario cas limite]
3. [remplir scénario erreur / offline]

## Notes pour agent
<!--
Points d'attention transverses, pièges connus, interactions avec autres écrans / MQTT / HA.
Rappels de sécurité, décisions produit déjà prises, etc.
-->
- [remplir ou retirer la section]

## Hors périmètre (optionnelle)
<!-- Lister explicitement ce qui N'EST PAS couvert pour éviter le scope creep. -->
- [remplir ou retirer]
