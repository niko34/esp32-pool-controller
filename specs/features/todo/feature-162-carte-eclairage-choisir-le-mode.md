## Métadonnées
- **ID** : feature-162
- **Slug** : carte-eclairage-choisir-le-mode
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.78.0
- **Dernière mise à jour** : 2026-10-14
- **Spec(s) liée(s)** : [`feature-161`](../done/feature-161-choisir-le-mode-depuis-les-cartes.md) (dont cette spec a été **détachée sur décision utilisateur**), [ADR-0108](../../../docs/adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md)

## Domaines touchés
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **non** : aucun dosage, aucune régulation chimique
- [x] **ux-ui-designer — MANDAT SUR L'ÉCRAN ENTIER.** ⚠️ Les trois autres cartes ont été
      refondues par feature-161 ; l'Éclairage est resté **seul dans l'ancien motif**. Son brief
      doit dire : *évalue la carte dans l'ensemble du tableau de bord, et dis tout choix visuel
      que ce lot impose — même ceux que je n'ai pas pensé à te demander.*
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Aligner la carte Éclairage sur le motif posé par feature-161 : choisir le mode depuis la carte,
sans quitter l'accueil. **Et corriger au passage un défaut existant** : la carte affirme
« Programmation activée » alors que la programmation peut être **neutralisée** sans que l'écran
le dise.

## Contexte
feature-161 a refondu les cartes pH, ORP et Filtration. L'Éclairage en a été **sorti sur décision
de l'utilisateur**, pour ne pas étendre un lot déjà large.

⚠️ **Le défaut découvert en instruisant feature-161 est le vrai sujet de cette spec.** La carte
affiche « Programmation activée » sur le seul `lighting_schedule_enabled`, alors que
`state.manualOverride` **neutralise** la programmation — et cet état n'est **jamais publié**.
L'écran annonce donc un comportement que l'appareil n'a pas.

⚠️ Et `decideLightingOn()` évalue l'override **AVANT** la programmation : l'ordre du code et
l'ordre affiché **ne disent pas la même chose**. C'est le motif de l'ADR-0093 (« le mode est la
seule autorité ») appliqué à l'éclairage.

## Comportement attendu
- Trois modes **équivalents** sur la carte — ⚠️ **décision utilisateur prise pendant feature-161**,
  à ne pas rouvrir sans elle.
- Le contenu de la carte **suit** le mode, comme les trois autres.
- L'état réellement appliqué est **publié** et **affiché** : si un override neutralise la
  programmation, la carte le dit. ⛔ Plus jamais « Programmation activée » quand elle ne l'est pas.

### Cas limites
- Override actif **et** programmation activée : que montre la carte ? ⚠️ C'est le cas qui a produit
  le défaut — il doit être tranché **explicitement**, pas déduit.
- Changement venu de MQTT ou d'un autre navigateur : reflété sans rechargement.
- Échec d'enregistrement : le sélecteur **revient à l'état réel** (réutiliser `resolveSelectorMode`,
  ⛔ ne pas en écrire un second).

## Comportement actuel
La carte expose la programmation ; le mode se change depuis la page de réglage. `manualOverride`
n'est pas publié, et la carte peut mentir.

## Contraintes techniques
- ⚠️ **RESPONSIVE — 390 px D'ABORD**, pas en repli. Compter les caractères de tout libellé placé
  sur une ligne partagée, et le **raccourcir** s'il ne tient pas.
- ⚠️ **Parité `/data` ↔ WebSocket** : tout nouveau champ publié doit l'être **des deux côtés**,
  avec la **même expression** — pas seulement la même clé (4ᵉ récidive constatée, dont une
  invisible au grep).
- Réutiliser le chemin partagé `applyRegulationMode` et la fonction pure `resolveSelectorMode`.
- Vocabulaire : **Auto / Programme / Arrêt**, strictement les mêmes mots que les autres cartes.

## Fichiers potentiellement concernés
`data/index.html`, `data/app.js`, `data/app.css`, `src/lighting.*`, `src/web_routes_*.cpp`,
`src/ws_manager.cpp`, `src/mqtt_manager.cpp`.

## ADR attendu ?
Probablement **non** : ADR-0108 couvre déjà la règle. ⚠️ **Sauf** si le cas « override + programmation »
impose un arbitrage général sur la publication de l'état appliqué — alors oui.

## Éléments visibles (pour doc-writer)
`docs/features/page-dashboard.md`, `docs/features/page-eclairage.md` (ou équivalent),
`docs/MQTT.md` si un état est publié, `docs/subsystems/ws-manager.md` si un champ WS est ajouté,
`CHANGELOG.md`, `docs/user/src/guide-utilisation.html`.

## Critères d'acceptation
- [ ] Le mode Éclairage se choisit depuis la carte.
- [ ] La carte ne peut plus annoncer une programmation **neutralisée** comme active.
- [ ] L'état appliqué est publié, et la parité `/data` ↔ WS est vérifiée **par expression**.
- [ ] Un échec d'enregistrement ramène le sélecteur à l'état réel.
- [ ] Vérifié **à 390 px sur appareil réel** (tâche humaine).

## Plan de test manuel
1. Activer la programmation, puis forcer un override → la carte doit **cesser** d'annoncer la
   programmation comme active.
2. Changer le mode depuis MQTT → la carte suit sans rechargement.
3. Couper le réseau pendant un changement → le sélecteur revient à l'état réel.

## Notes pour agent
⚠️ **Le défaut d'affichage est le cœur de cette spec, pas un bonus.** Une implémentation qui
ajoute le sélecteur sans corriger `manualOverride` **ne répond pas** à la demande.
