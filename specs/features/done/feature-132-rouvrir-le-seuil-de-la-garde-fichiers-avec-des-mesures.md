# feature-132 — Rouvrir le seuil de la garde fichiers, avec des mesures

## Métadonnées
- **ID** : feature-132
- **Slug** : rouvrir-le-seuil-de-la-garde-fichiers-avec-des-mesures
- **Type** : refactor
- **Statut** : **livré — mesuré, seuil inchangé, tests verts, revue OK**
- **Priorité** : basse
- **Version cible** : N/A — aucun changement de comportement, aucune version bumpée
- **Dernière mise à jour** : 2026-09-20
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
- [x] Mesure du besoin réel, datée, avec sa méthode
- [x] Décision écrite (changer ou ne pas changer), avec son motif
- [x] Commentaire et `static_assert` de `constants.h` remis en cohérence
- [ ] ADR si le seuil descend sous 6 144 — **N/A, le seuil ne descend pas** (voir « ADR attendu ? »)

## ADR attendu ?
**Seulement si** le seuil descend sous 6 144, ce qui lèverait un interdit
existant. **Ce n'est pas le cas** : aucun ADR n'est créé. Les 4 critères de
déclenchement d'ADR sont négatifs — la décision ne contraint rien de nouveau,
elle est déjà lisible dans le commentaire de `constants.h`, elle n'a pas
d'alternative crédible non documentée (le seul palier intermédiaire, 7 168, est
écarté et chiffré dans le même commentaire), et elle ne verrouille aucun
trade-off nouveau : elle **confirme** un verrou déjà posé par ADR-0070/ADR-0058.
L'annotation datée du 2026-09-20 a été ajoutée à
[ADR-0070 §b](../../docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)
(motif dupliqué), sans modifier son statut ni son contenu accepté.

## Éléments visibles (pour doc-writer)
Aucun endpoint nouveau. **`CHANGELOG.md` : aucune entrée** — aucun comportement
visible ne change (le seuil reste 8 192, zéro 503 avant comme après).

## Critères d'acceptation
- [x] Le besoin réel est **mesuré** sur l'appareil, en charge, après feature-131
- [x] Le commentaire de `constants.h:404` décrit la mesure **courante**, pas une périmée
- [x] Aucun 503 sur fichier statique en usage normal sur ≥ 24 h (209 requêtes HTTP sur 2 jours, toutes en 200)
- [x] Les assertions d'ordre restent vraies

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

## Notes techniques (2026-09-20)

### La mesure

**Méthode** : rafale **parallèle** des 9 assets d'`index.html` (le parallélisme
crée la contention pbuf que le seuil provisionne), encadrée de deux lectures
`GET /get-system-info?hf=1`. Lecture seule, sur l'appareil en production
(v2.53.0, 192.168.1.48), qui dose pendant la mesure. Grandeur lue : `hf.i8.wb`,
plancher de `max_alloc_8bit` (masque `0x0804`) sur fenêtre glissante de 30 min
(`fetch_min`, réarmement `main.cpp:1302`).

**Résultat, recompté le 2026-09-20 sur les 19 fichiers JSON de la campagne**
(⚠️ trois comptes divergents ont circulé le même jour — 17/153, 18/162 — avant
ce recomptage définitif, cf. ADR-0073) : **19 rafales, 171 requêtes d'assets +
38 lectures de sonde = 209 requêtes HTTP, toutes en 200, zéro 503.** Plancher
glissant le plus bas atteint : **19 444 o** (×2,37 du seuil 8 192, ×4,72 du
besoin avec contention TCP 4 118 o). Détail complet, table par fenêtre :
`/root/pool-captures/f132/mesures.md` (non versionné — poste de travail).

### La décision

`kStaticFileMinAllocBytes` **reste à 8 192**. Aucune valeur changée, ni dans
`src/` ni dans `test/` — seuls des commentaires et le message d'un
`static_assert`. Deux raisons :

1. **La prémisse de la spec était fausse.** Le bloc de dimensionnement
   (`constants.h:445-464`) ne contient **aucun terme d'historique** — le besoin
   dur (874 o) et le besoin avec contention (4 118 o) sont entièrement newlib
   + pool `FILE` + LittleFS + pbufs lwip. feature-131 a supprimé un
   **concurrent** sur le même tas, pas un **terme** de cette somme : il n'y
   avait rien à rouvrir.
2. **La garde ne refuse rien** en usage réel mesuré : 209/209 requêtes en 200.
   La baisser ne peut gagner rien d'observable — elle ne change le comportement
   que sous 8 192 o, précisément le régime de famine qu'elle existe pour
   couvrir. Le seul palier intermédiaire crédible (7 168) ramènerait la marge à
   ×1,74 — sous le ×1,99 revendiqué — et ferait glisser en silence le second
   palier dérivé de `fs_guard.cpp:171` (`probeFloorArmed`, armé sous
   `2 × kStaticFileMinAllocBytes`) de 16 384 à 14 336, sans aucune mesure pour
   le justifier.

### Découverte notable : la décroissance décrite dans le code est INVERSÉE

Le commentaire d'origine décrivait une décroissance monotone du 05/09
(42 996 → 26 612 → 20 468 → 15 860 → 13 300 o), « sans jamais remonter hors
reboot », en annonçant qu'une reprise de la décroissance rendrait la garde
visible en régime calme — « c'est le signal voulu ». On mesure aujourd'hui
**32 756 à 55 284 o** selon la fenêtre : feature-129 puis feature-131 ont fait
leur travail, et le signal dit maintenant l'inverse. Les relevés du 05/09 sont
conservés dans le code, marqués **HISTORIQUES**, comme trace de pourquoi la
garde a été posée — pas comme description de l'état actuel.

### Ce que la mesure du 2026-09-20 NE dit PAS

1. **Ce n'est pas un besoin.** `hf.i8.wb` mesure ce qui est **disponible** à
   l'admission, jamais ce que le service **consomme**. Le besoin reste le
   budget de `constants.h:445-464`, établi par désassemblage.
2. **`delta = 0` ne veut pas dire « gratuit ».** `wb` est un `fetch_min`
   glissant (réarmement toutes les 30 min, `main.cpp:1302`) : une rafale qui
   reste au-dessus du creux déjà enregistré ne le déplace pas. **Une seule
   fenêtre sur 19 est pleinement attribuée** (celle des 19 444 o) ; les 18
   autres **bornent**, elles ne mesurent pas.
3. **Pas le pic intra-requête.** La sonde (`src/web_server.cpp:165`)
   échantillonne à l'**admission** du GET : elle voit la contention des
   requêtes concurrentes en vol, pas le pic de la requête admise (≤ 874 o,
   dont ≤ 428 o contigus).
4. **Pas un pire cas annuel.** 19 rafales sur deux jours, un appareil, un
   firmware, un réseau.
5. **L'ambiant n'est pas une constante.** Pendant cette campagne il vaut
   **32 756-34 804 o** en instantané ; le 55 284 du suivi feature-131 vient
   d'une autre fenêtre. Le rapport « ×6,5 au repos » n'a de sens qu'avec sa
   date — d'où l'exigence de mesurer *pendant*, pas *autour*.

### Fichiers modifiés
`src/constants.h` (commentaires + message de `static_assert`, aucune valeur
changée) · `test/test_native_fopen_guard/test_fopen_guard.cpp` (annotation
« HISTORIQUES » + repère feature-132) ·
`test/test_native_http_lowheap_guard/test_http_lowheap_guard.cpp` (idem) ·
`docs/subsystems/acces-fichiers.md` · `docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md`
(annotation datée, statut inchangé).
