# feature-132 — Rouvrir le seuil de la garde fichiers, avec des mesures

## Métadonnées
- **ID** : feature-132
- **Slug** : rouvrir-le-seuil-de-la-garde-fichiers-avec-des-mesures
- **Type** : refactor
- **Statut** : todo
- **Priorité** : basse
- **Version cible** : N/A — **bloquée par feature-131**
- **Dernière mise à jour** : 2026-09-18
- **Spec(s) liée(s)** : feature-131 (**prérequis strict**) · ADR-0070 · `specs/features/done/feature-129-…`

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP / WebSocket** (service de fichiers statiques)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] **Historique / persistance** (la garde est partagée)
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

Réexaminer `kStaticFileMinAllocBytes` (8 192) **une fois le pic d'allocation de
l'historique supprimé**. Si le seuil a été dimensionné pour un pic qui n'existe
plus, il refuse des requêtes légitimes pour rien.

## ⛔ PRÉREQUIS STRICT

**Ne pas ouvrir cette spec avant que feature-131 soit livrée ET mesurée.** Sans
ça, il n'y a rien à rouvrir : le seuil protège d'un pic qui existe encore.

## Contexte

`constants.h:404` :
```cpp
constexpr uint32_t kStaticFileMinAllocBytes = 8192;  // Plus gros bloc 8 bits < seuil → fichiers statiques refusés (503)
```

Ce seuil est **verrouillé** par deux assertions et un interdit d'ADR :
```cpp
static_assert(kStaticFileMinAllocBytes > kWsPushMinAllocBytes &&
              kStaticFileMinAllocBytes < kLowHeapWarnBytes, …);
static_assert(kStaticFileMinAllocBytes >= 6144, "Interdiction ADR : cf. constants.h:271-274");
```

Il a donc **une histoire**, et la mesure d'origine (besoin dur ≈ 874 o, avec deux
pbufs ≈ 4 118 o, marge ×1,99) tenait compte du contexte d'alors — dont le pic du
`JsonDocument` de l'historique, qui se produit sur le même tas.

⚠️ **Le baisser sans mesurer serait exactement l'erreur que ce projet a déjà
commise plusieurs fois** : reprendre un chiffre sans sa méthode (ADR-0073), ou
recalibrer sur une métrique qui ne mesure pas ce qu'on croit (cause racine du
15/08/2026). Cette spec est une **mesure**, pas un ajustement.

## Comportement attendu

1. **Mesurer** le besoin réel du service de fichiers statiques, après
   feature-131, sur l'appareil, en charge — pas en théorie.
2. **Décider** : soit le seuil reste justifié et on l'écrit (avec la nouvelle
   mesure et sa date), soit il est surdimensionné et on le baisse **avec sa
   méthode inscrite à côté**.
3. Dans les deux cas, **mettre à jour le commentaire et les assertions** : un
   seuil dont le motif écrit est périmé est pire qu'un seuil discutable, parce
   qu'il fait croire à une justification.

> Un résultat parfaitement acceptable de cette spec est **« on ne change rien,
> et voici pourquoi, mesuré le … »**. Ce n'est pas un échec.

## Comportement actuel

Seuil à 8 192 o, dimensionné dans un contexte où l'historique construisait
« des dizaines de kilooctets » sur le même tas.

## Contraintes techniques

- **Ne pas descendre sous 6 144** sans lever explicitement l'interdit d'ADR
  (`constants.h:271-274`) — c'est-à-dire sans un nouvel ADR qui remplace
  l'ancien, jamais par simple édition d'un `static_assert`.
- La relation d'ordre avec `kWsPushMinAllocBytes` et `kLowHeapWarnBytes` doit
  rester vraie.
- Toute mesure doit porter **sa date et sa méthode** (ADR-0073).
- ⚠️ Mesurer **pendant** une requête de fichier statique sous charge, pas au
  repos. Le besoin est transitoire.

## Fichiers potentiellement concernés

`src/constants.h` · `src/fs_guard.*` · `src/web_server.cpp` · ADR existant sur le
seuil

## Livrables — check-list
- [ ] Mesure du besoin réel, datée, avec sa méthode
- [ ] Décision écrite (changer ou ne pas changer), avec son motif
- [ ] Commentaire et `static_assert` de `constants.h` remis en cohérence
- [ ] ADR si le seuil descend sous 6 144

## ADR attendu ?
**Seulement si** le seuil descend sous 6 144, ce qui lèverait un interdit
existant.

## Éléments visibles (pour doc-writer)
Aucun endpoint nouveau. `CHANGELOG.md` si le comportement change (moins de 503
sur les fichiers statiques).

## Critères d'acceptation
- [ ] Le besoin réel est **mesuré** sur l'appareil, en charge, après feature-131
- [ ] Le commentaire de `constants.h:404` décrit la mesure **courante**, pas une périmée
- [ ] Aucun 503 sur fichier statique en usage normal sur ≥ 24 h
- [ ] Les assertions d'ordre restent vraies

## Plan de test manuel
1. Charger l'interface complète (8 requêtes, comme un navigateur) pendant une
   sauvegarde d'historique, et relever `max_alloc_8bit` **pendant**.
2. Vérifier l'absence de 503.
3. Refaire en famine simulée, au seuil, pour vérifier que la garde mord toujours
   quand elle doit.

## Notes pour agent

Cette spec est courte mais **c'est un piège classique** : elle invite à baisser
un nombre. Résister. Le livrable est une **mesure datée**, et la décision en
découle — y compris la décision de ne rien changer.

## Hors périmètre
- L'optimisation de l'historique elle-même (feature-131).
- Les seuils de dosage ou de régulation.
