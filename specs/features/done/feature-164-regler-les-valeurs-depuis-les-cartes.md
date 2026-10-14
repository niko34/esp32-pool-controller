## Métadonnées
- **ID** : feature-164
- **Slug** : regler-les-valeurs-depuis-les-cartes
- **Type** : feature
- **Statut** : doing (implémentation + documentation livrées 2026-10-14, clôture par l'orchestrateur)
- **Priorité** : normale
- **Version cible** : 2.80.0
- **Dernière mise à jour** : 2026-10-14
- **Spec(s) liée(s)** : [`feature-161`](../done/feature-161-choisir-le-mode-depuis-les-cartes.md) · [`feature-162`](feature-162-carte-eclairage-choisir-le-mode.md) — **recouvrement résolu** : feature-162 est passée en premier, l'Éclairage reste hors périmètre de ce lot · [ADR-0108](../../../docs/adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md) · [ADR-0110](../../../docs/adr/0110-regler-une-consigne-est-une-commande-differee.md) (créé pendant ce lot — troisième catégorie après ADR-0108) · [`docs/DESIGN_SYSTEM.md`](../../../docs/DESIGN_SYSTEM.md)

## ✅ Verdict `pool-chemistry` consigné (2026-10-14)

**GO sous 12 conditions** (C1-C12) + **deux précisions en contre-vérification** (voir section
« ⛔ TEXTES IMPOSÉS » et « Deux précisions du gate » plus bas dans ce document, conservées
telles quelles). Décisions structurantes retenues et **implémentées** :

- **(a) Bornes identiques partout** — pas de fenêtre resserrée sur l'accueil. Conséquence
  concrète : la fenêtre ORP, plus stricte côté MQTT ([400 ; 900]) qu'HTTP (aucune validation)
  avant ce lot, est élargie aux mêmes bornes des deux côtés : **[300 ; 950] mV**.
- **(b) Bouton Sauvegarder explicite, jamais d'autosave** sur les nouveaux champs — pas de
  confirmation modale systématique (friction uniforme = anti-pattern, leçon feature-159).
- **(C7) Seule friction retenue** : toute baisse de consigne ORP pendant une chloration de
  secours active ouvre la confirmation mutualisée avec le passage en mode Arrêt. Critère = toute
  baisse, jamais une comparaison à la mesure (formulation rejetée explicitement).
- **(C10) Verrou d'étalonnage sur `calActive` SEUL**, jamais l'escalade de sécurité pH (qui
  verrouille déjà le mode) — confirmé en contre-vérification.
- **(C5/C6) Purge de l'intégrale pH au changement RUNTIME de consigne**, sans toucher aux
  compteurs de dosage (« on efface une demande, jamais une dette ») — `phIntegralContextHash()`
  ne couvre PAS ce cas (réduite, pas supprimée : boot + feedforward seulement). Mutation
  obligatoire exigée et livrée sur ce point précis.
- **(C11) Refus serveur `début == fin`** pour les horaires de filtration (HTTP 400, texte
  imposé) ; `fin < début` reste accepté (plage enjambant minuit).
- **(C12) Avertissement non bloquant** sous 120 min de filtration programmée, texte imposé,
  identique écran + journal.
- **Dit explicitement, non tu** : un étalonnage d'UNE SEULE sonde verrouille LES DEUX consignes
  (drapeau `calSwitchActive` global, sans distinction de canal) — accepté sans spec de suivi.

### Implémentation vérifiée (lecture de code, 2026-10-14)

- `src/web_routes_config.cpp` : validation ORP (refus 400, jamais d'écrêtage), validation horaires
  (C11/C12), verrou étalonnage étendu aux consignes (C5/C6/C10), nouveau texte 409 (mode+consignes).
- `src/pump_controller.{h,cpp}` : `requestPhTargetChange()` + `applyRegulationSpeedNow(forcePurge)`
  — drapeau atomique, purge en `loopTask` uniquement (invariant v2.26.0 respecté).
- `src/mqtt_manager.cpp` : bornes ORP élargies à [300 ; 950] (C3/C4). ⚠️ **Limite non corrigée,
  signalée dans la doc** : les commandes MQTT `ph|orp_target/set` ne vérifient PAS `calSwitchActive`
  — seul le chemin HTTP verrouille les consignes pendant un étalonnage.
- `data/app.js`/`data/index.html` : blocs consigne éditables (pH `.segmented--num`, ORP champ
  libre + `.field__hint` de plage), bloc horaires filtration éditable en Programme, retrait DOM de
  « Cible : X » sur pH/ORP, boutons Sauvegarder explicites, drapeaux « saisie en cours » par bloc.
- `docs/DESIGN_SYSTEM.md` : déjà à jour (§2, §5.3, §5.13, §5.20, §6, §7) — **vérifié par
  doc-writer**, aucun écart constaté avec le livré.
- Tests natifs : `test/test_native_regulation_target_validation/` (12 cas, structurels + logique
  pure réelle) — couvre notamment la mutation exigée sur `forcePurge`.

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
- [x] embedded-developer — **CONFIRMÉ requis** (lecture `src/web_routes_config.cpp`) :
      `orp_target` (ligne 796, `mqttCfg.orpTarget = doc["orp_target"];`) n'a **aucune validation**
      de bornes ni d'alignement de pas, contrairement à `ph_target` (bloc complet lignes 636-699,
      refus hors plage + snapping sur `kPhTargetStep`). `filtration_start`/`filtration_end`
      (lignes ~1128-1129) sont affectés en **String brute**, sans passer par `timeStringToMinutes`
      ni aucune règle de cohérence — contrairement au principe de validation serveur déjà posé
      pour `ph_target` et `filtration_mode`. Ces trois chemins doivent être complétés avant toute
      écriture depuis les cartes.

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
- Un échec d'enregistrement **ramène le champ à la valeur réelle** et le dit.
- Une valeur changée ailleurs (MQTT, autre navigateur) est **reflétée sans rechargement**.

### Cas limites
- ⚠️ **Début et fin incohérents** (fin avant début, ou égales) : que fait l'appareil ? Le refus
  existe-t-il déjà côté serveur, ou seulement côté navigateur ? **Vérifier, ne pas supposer.**
  **Vérifié (`src/schedule_logic.cpp:35-44`)** : `fin < début` est une plage **volontaire** qui
  enjambe minuit (`return now >= start || now < end`) — ⛔ **ne pas valider "fin > début"**, ça
  casserait un cas d'usage réel. En revanche **`fin == début`** fait que la filtration **ne tourne
  jamais** (`equalMeansAlways = false` pour la filtration, confirmé par l'appel
  `src/filtration.cpp:324` qui ne passe pas ce paramètre) — **silencieusement**, sans refus ni
  message. Il n'existe **aucune fonction `durationMinutes`** dans le code (recherche vide) ; la
  fonction la plus proche, `remainingRangeMinutes`, renvoie **0** (pas -1) hors plage. ⇒ la
  validation serveur à écrire doit **refuser explicitement `start == end`** pour la filtration,
  avec un message, plutôt que de laisser ce cas produire un arrêt silencieux de la filtration.
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
  ⚠️ **Vérifié (`data/app.js:3429-3463`)** : `bindLightingScheduleSave` prend des **IDs de champs**
  en paramètre (`startId`, `endId`...) mais **code en dur les clés de payload**
  `lighting_start_time` / `lighting_end_time` (ligne 3453, et la restauration d'erreur lignes
  3461-3462 lit aussi `window._config?.lighting_start_time`). Ce n'est **pas** un composant
  réutilisable malgré l'apparence de sa signature — il est **spécifique à l'Éclairage**.
  Pour pH / ORP / Filtration, soit généraliser cette fonction (clés en paramètre), soit écrire un
  nouveau binding — ⛔ ne pas appeler `bindLightingScheduleSave` tel quel sur d'autres champs.
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

## ⛔ TEXTES IMPOSÉS par `pool-chemistry` — à coller, JAMAIS à reformuler

⚠️ Défaut récurrent du projet : *un brief qui résume un texte imposé oblige le développeur à
l'inventer.* Ces quatre textes sont **validés mot pour mot**.

### C7 — confirmation : secours actif **et** baisse de consigne ORP
> « Chloration de secours en cours. Abaisser la consigne de {ancienne} à {nouvelle} mV réduira,
> voire interrompra, le dosage de secours. Confirmer ? »

Boutons : « **Abaisser la consigne** » / « **Annuler** ». Valeurs en **mV entiers**. **Les deux
surfaces**, via `confirmOrpRescueInterruption()` **mutualisée**.
⚠️ **Le critère est TOUTE BAISSE pendant un secours actif** — ⛔ **jamais** une comparaison à la
mesure. Motif du gate : *« une mesure fluctue et rendrait la garde intermittente »*. La formulation
proposée en ergonomie (« sous la mesure actuelle ») est **explicitement REJETÉE**.

### C9 — consigne pH à la borne haute : ⛔ NE PAS ÉCRIRE DE SECOND TEXTE
Le texte **existe déjà** dans `renderPhTargetWarning()` (`data/app.js`) et il **convient** :
> « Consigne à la borne haute (7,50) : à ce pH, le chlore est nettement moins actif et la
> désinfection perd en efficacité. Au-delà, le dosage de chlore serait suspendu — c'est pourquoi la
> plage s'arrête ici. »

⇒ **Généraliser la fonction** (cible de rendu en paramètre). ⚠️ La phrase proposée au premier
passage du gate est **RETIRÉE** — elle aurait fait **deux** textes pour un même avertissement.

### C12 — durée de filtration courte (non bloquant)
⚠️ Cite **la durée saisie ET le seuil** (ADR-0095) :
> « Filtration programmée : {X} min par jour. Sous 120 min, la circulation est trop courte pour que
> la régulation pH/ORP atteigne sa consigne — l'eau restera durablement à côté de la valeur visée. »

Même texte à l'écran **et** au journal (`warning`).

### C11 — refus `début == fin` (bloquant, HTTP 400)
⚠️ **Dit POURQUOI** — le comportement actuel est muet :
> « Heure de début et heure de fin identiques ({HH:MM}) : la filtration ne démarrerait jamais.
> Saisissez deux heures différentes — par exemple 00:00 et 23:59 pour filtrer en continu. »

⛔ `fin < début` reste **ACCEPTÉ** : c'est une plage qui **enjambe minuit**, volontaire.

## Deux précisions du gate, demandées en contre-vérification

**C10 — lecture confirmée** : le verrou de consigne utilise **`calActive` SEUL**. *« Une escalade
force déjà le mode en Arrêt : verrouiller la consigne n'y protégerait rien et ôterait à
l'utilisateur le moyen de déplacer la cible pour sortir. »*

**C6 — RÉDUITE, pas supprimée.** `phIntegralContextHash()` inclut bien `ctx.target`, **mais il ne
couvre que deux choses** : le rejet d'un instantané NVS **au démarrage**, et un verrou interne de
feedforward. ⛔ **Il ne touche JAMAIS l'intégrale en RAM, à chaud.**
⇒ *« Changer la consigne de 7,50 à 7,20 en marche laisse l'intégrale accumulée contre l'ancienne
cible servir l'acide contre la nouvelle. »* **La purge reste obligatoire pour le cas RUNTIME**, et
⛔ ne doit **pas** redoubler le démarrage ni le feedforward. **Mutation obligatoire sur le cas à
chaud.**

## Clôture — la garde était au mauvais endroit, et il a fallu QUATRE découvertes

**Version 2.80.0.** Mesuré (ADR-0073) : `pio run -e esp32dev_core3` **SUCCESS**, 0 warning ·
`pio test -e native` **2 195 cas / 71 suites**, zéro régression · **6 mutations sur 6 tuées** ·
**18/18** contrôles · **5 652** liens résolus · `build_fs.sh` 512 Ko.

### Verdicts
`pool-chemistry` **GO sous 12 conditions**, puis **4 arbitrages** en cours de lot ·
`code-reviewer` **Request changes** (2 textes imposés reformulés) puis **Approved** ·
`ux-ui-designer` 3 passages.

### ⚠️ LE FAIT MARQUANT : une garde posée sur un GESTIONNAIRE, pas sur une VALEUR

Les conditions **C6** (purge de l'intégrale) et **C10** (verrou d'étalonnage) ont d'abord été
implantées dans le **gestionnaire `/save-config`**. ⇒ Tout autre chemin écrivant `phTarget` /
`orpTarget` les **contournait en silence**. Quatre chemins ont été découverts **l'un après
l'autre** :

| # | Chemin | Découvert par | État initial |
|---|---|---|---|
| 1 | HTTP `/save-config` | — | couvert dès le départ |
| 2 | MQTT, **mode** | gate, lot feature-161 | fermé la veille |
| 3 | **MQTT, consignes** | `doc-writer`, en documentant | ⛔ **ni verrou, NI PURGE** |
| 4 | **`pool_size` → `applyRegulationDefaults()`** | `embedded-developer`, sur demande d'inventaire | verrou absent (purge **déjà** couverte) |

⚠️ **Le n°3 était le plus grave** : changer la consigne pH par MQTT laissait agir une intégrale
accumulée **contre l'ancienne cible** — exactement le scénario que C6 existait pour empêcher.
⚠️ **Le n°4 n'a été trouvé que parce que la demande exigeait un INVENTAIRE**, pas une vérification.
⇒ **Leçon, à opposer à tout lot futur** : *une garde se pose au **point où la valeur change**, pas
sur le gestionnaire qui a motivé son écriture.* Et : ⛔ **demander l'inventaire, pas la vérification.**

### ⚠️ La nuance qui a failli créer un défaut inverse
Le gate a imposé que le refus ne s'arme **que sur un changement RÉEL**. L'interface renvoyant
toujours les valeurs courantes, tester la **présence** de la clé aurait refusé **toute** sauvegarde
de réglages pendant un étalonnage — *une garde qui bloque plus qu'elle ne protège*.

### ⚠️ TROIS seuils recopiés côté client, corrigés (ADR-0100)
Bornes ORP · attributs `min`/`max` du champ · seuil des 120 min. ⛔ Un seuil est **SERVI**.
⚠️ Le défaut revient parce que **recopier est toujours plus rapide sur le moment**.

### Défauts trouvés par la CAPTURE, pas par la relecture
🚨 **« CONSIGNE ORP (MV) »** — le **mégavolt**. `.field__label` met en majuscules ; le HTML portait
bien `mV`. **8 libellés** encapsulés au total, dont cinq annonçaient des **mégalitres** sur des
champs de **limites de dosage**. ⛔ Invisible à la relecture du HTML.
Et : le **texte périmé du verrou** subsistait dans `data/index.html` alors que `src/` était purgé.

### Retrait compensant l'ajout
La ligne « **Cible : X** » disparaît des cartes pH et ORP. ⇒ Le 7ᵉ élément est compensé par un
retrait — ⚠️ sans quoi l'écran se dégradait comme la page Produits.

### ⛔ Restant à l'humain
- Vérification **à 390 px sur appareil réel**.
- Captures : `03_Tableau_de_bord.jpg` (3 cartes changées), `04_Filtration.jpg`, `08_ORP.jpg`.
- Plan de test manuel, en particulier le **scénario 2** (sémantique JSON, non testable hors cible).
