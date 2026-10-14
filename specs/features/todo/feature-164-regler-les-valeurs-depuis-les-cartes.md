## Métadonnées
- **ID** : feature-164
- **Slug** : regler-les-valeurs-depuis-les-cartes
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.79.0
- **Dernière mise à jour** : 2026-10-14
- **Spec(s) liée(s)** : [`feature-161`](../done/feature-161-choisir-le-mode-depuis-les-cartes.md) · [`feature-162`](feature-162-carte-eclairage-choisir-le-mode.md) ⚠️ **recouvrement à arbitrer, voir « Hors périmètre »** · [ADR-0108](../../../docs/adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md) · [`docs/DESIGN_SYSTEM.md`](../../../docs/DESIGN_SYSTEM.md)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader · [x] architect
- [x] **pool-chemistry** — ⚠️ OBLIGATOIRE : les consignes **commandent le dosage**.
- [x] **ux-ui-designer — MANDAT SUR L'ÉCRAN ENTIER**, et ⚠️ **il doit mettre à jour
      `docs/DESIGN_SYSTEM.md`** : aucun composant « modifier une valeur dans une carte » n'y est
      déclaré aujourd'hui (§2). C'est **lui** qui l'ajoute, règle de gouvernance du document.
- [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] embedded-developer — *a priori* non (les routes d'écriture existent déjà). ⚠️ À confirmer :
      l'« a priori non » de feature-161 s'est révélé **faux**.

## Description
Permettre de régler, **depuis les cartes du tableau de bord**, les valeurs qui pilotent le mode
actif : la **consigne pH**, la **consigne ORP**, et les **horaires** de filtration et d'éclairage.

## Contexte
Demande utilisateur du **2026-10-14**, mot pour mot : *« En mode auto ou programme, il faut
pouvoir modifier les valeurs du réglage dans les cartes. »*

Suite directe de feature-161, qui a mis le **mode** sur les cartes. Le geste suivant est naturel :
on voit le mode, on devrait pouvoir ajuster ce qu'il utilise sans aller dans une page de réglage.

### ⚠️ Ce que l'utilisateur a EXCLU, et pourquoi c'est cohérent
Questionné sur le périmètre, il a retenu **les consignes** et **les horaires**, et **écarté** le
**volume quotidien** et la **limite de sécurité**.
⇒ C'est exactement le principe déjà posé : **les plafonds protègent, le reste informe.** Une
consigne s'ajuste au fil de l'eau ; un plafond de sécurité se modifie délibérément, dans une page
dédiée. ⛔ **Ne pas « compléter » ce périmètre en ajoutant les deux lignes écartées.**

## Comportement attendu
- **Carte pH, mode Auto** : la consigne pH est modifiable.
- **Carte ORP, mode Auto** : la consigne ORP (mV) est modifiable.
- **Carte Filtration, mode Programme** : heures de début et de fin modifiables.
  ⛔ **En mode Auto, les horaires sont CALCULÉS** d'après la température — ils restent en lecture
  seule. Les rendre saisissables serait un mensonge d'interface.
- **Carte Éclairage, mode Programme** : heures de début et de fin modifiables.
- Un échec d'enregistrement **ramène le champ à la valeur réelle** et le dit.
- Une valeur changée ailleurs (MQTT, autre navigateur) est **reflétée sans rechargement**.

### Cas limites
- ⚠️ **Début et fin incohérents** (fin avant début, ou égales) : que fait l'appareil ? Le refus
  existe-t-il déjà côté serveur, ou seulement côté navigateur ? **Vérifier, ne pas supposer.**
- Consigne saisie **hors des bornes** autorisées : refus serveur **et** message compréhensible.
- Modification pendant un **étalonnage** ou une **escalade de sécurité pH** : la règle doit être
  la même que pour le mode (segments désactivés, jamais masqués) — à trancher par `pool-chemistry`.
- Champ laissé **vide** ou à demi saisi, puis perte de focus.

## Comportement actuel
Ces valeurs sont affichées **en lecture seule** sur les cartes (`.detail-row`) et ne se modifient
que depuis les pages de réglage.

## Contraintes techniques

### ⚠️ ADR-0108 ne couvre PAS ce geste — c'est une TROISIÈME catégorie
L'ADR dit : **basculer le mode** est une commande d'accueil, **injecter** n'y est jamais.
Modifier une **consigne** n'est ni l'un ni l'autre : ce n'est pas une action immédiate, mais
**cela commande le dosage à venir**. ⚠️ Changer la consigne ORP vers le haut **déclenche de la
chloration** ; changer la consigne pH déplace la cible de l'acide.
⇒ `pool-chemistry` doit **trancher explicitement** si ce geste a sa place sur l'accueil, et sous
quelles conditions (confirmation ? bornes resserrées ? pas de modification pendant un étalonnage ?).
⛔ Ne pas présumer que « c'est comme le mode ».

### ⚠️ L'enregistrement automatique est un ÉCART au motif du projet, à ASSUMER
`bindAutosave()` (`data/app.js`) **exclut explicitement** les champs numériques et horaires —
commentaire du code : *« Programmation (mode, start, end) — pas d'auto-save, bouton Sauvegarder
uniquement »*. Seuls les interrupteurs et les sélecteurs segmentés s'enregistrent au changement.
⇒ Mettre un champ **numérique ou horaire** en enregistrement automatique sur une carte est un
**écart délibéré**, à justifier — ⛔ **pas à présenter comme une mise en cohérence**.
⇒ Alternative à instruire : un bouton d'enregistrement par carte, ou un enregistrement différé.
**L'`ux-ui-designer` tranche la forme, `pool-chemistry` tranche si un geste de confirmation est
exigé pour les consignes.**

### ⚠️ RESPONSIVE — 390 px d'abord
Un champ de saisie dans une carte, c'est précisément le motif qui a dégradé la page Produits :
champ et bouton sur une même ligne, bouton « perdu entre les 2 champs » sur téléphone.
⇒ **Compter les caractères**, donner le calcul, et **capturer le rendu** avec
`./tools/ui_shot.sh` — ⛔ pas d'affirmation sur un rendu sans image.

### Autres
- **Parité `/data` ↔ WebSocket** par **expression**, pas seulement par clé (4 récidives).
- Réutiliser le chemin partagé déjà en place ; ⛔ aucun second chemin de décision.
- Le **document de design system** doit déclarer le nouveau composant **avant** l'implémentation.

## Fichiers potentiellement concernés
`data/index.html`, `data/app.js`, `data/app.css`, `docs/DESIGN_SYSTEM.md`,
`src/web_routes_config.cpp`, `src/ws_manager.cpp`, `src/mqtt_manager.cpp`, `src/filtration.*`,
`src/lighting.*`.

## Sécurité chimique
⚠️ **Les consignes commandent le dosage.** À instruire par `pool-chemistry` :
1. Un geste d'accueil **peut-il** déplacer une cible de dosage ? Si oui, avec quelle friction ?
2. Les **bornes** doivent-elles être plus serrées depuis l'accueil que depuis la page de réglage ?
   ⚠️ Attention au principe inverse : *une garde qui bloque plus qu'elle ne protège est un défaut.*
3. Que se passe-t-il si la consigne change **pendant** un dosage en cours ?
4. Interaction avec la **chloration de secours** et avec l'**inhibition pH**.

## ADR attendu ?
**Probablement oui** : ADR-0108 a tracé deux catégories, ce lot en introduit une **troisième**
(régler une valeur qui commande le dosage futur). Si `pool-chemistry` pose une règle générale,
elle amende ou complète l'ADR-0108.

## Éléments visibles (pour doc-writer)
`docs/features/page-dashboard.md`, `page-ph.md`, `page-orp.md`, `page-filtration.md`,
`docs/API.md` si une route change, `docs/MQTT.md` si un topic change,
`docs/subsystems/ws-manager.md` si un champ WS est ajouté, `CHANGELOG.md`,
`docs/user/src/guide-utilisation.html` **et** `guide-regulation.html` (les consignes y sont
décrites), `docs/DESIGN_SYSTEM.md`.

## Critères d'acceptation
- [ ] Consigne pH modifiable depuis la carte en mode Auto.
- [ ] Consigne ORP modifiable depuis la carte en mode Auto.
- [ ] Horaires de filtration modifiables en mode Programme, **et en lecture seule en Auto**.
- [ ] Horaires d'éclairage modifiables en mode Programme.
- [ ] Un échec d'enregistrement ramène le champ à la valeur réelle.
- [ ] Une valeur changée ailleurs est reflétée sans rechargement.
- [ ] Le verdict de `pool-chemistry` sur la friction est **appliqué et consigné**.
- [ ] `docs/DESIGN_SYSTEM.md` déclare le nouveau composant **et son contexte**.
- [ ] Rendu **capturé** à 390 px et à 1024 px, et **comparé entre les quatre cartes**.
- [ ] Vérifié **à 390 px sur appareil réel** (tâche humaine).

## Plan de test manuel
1. Changer la consigne ORP depuis la carte → la valeur est reprise par la régulation, visible au
   journal.
2. Saisir une consigne hors bornes → refus **compréhensible**, champ ramené à la valeur réelle.
3. Saisir une heure de fin antérieure à l'heure de début → comportement **constaté et écrit**.
4. Couper le réseau pendant une modification → le champ revient à la valeur réelle.
5. Passer la filtration en Auto → les horaires **redeviennent non saisissables**.

## Hors périmètre
- ⛔ **Volume quotidien** et **limite de sécurité** : écartés par l'utilisateur, ils restent dans
  les pages de réglage.
- ⛔ **L'ÉCLAIRAGE EST ENTIÈREMENT HORS PÉRIMÈTRE** — arbitrage de l'utilisateur du 2026-10-14.
  La carte Éclairage, **horaires compris**, est traitée par
  [`feature-162`](feature-162-carte-eclairage-choisir-le-mode.md), qui passe **en premier**.
  ⚠️ Motif : deux lots successifs sur la même carte, c'est le motif d'accumulation qui a dégradé
  la page Produits. ⇒ Ce lot ne touche que **pH**, **ORP** et **Filtration**.
