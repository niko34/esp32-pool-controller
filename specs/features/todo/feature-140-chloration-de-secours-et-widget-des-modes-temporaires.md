# feature-140 — Chloration de secours, et regroupement des modes temporaires

## Métadonnées
- **ID** : feature-140
- **Slug** : chloration-de-secours-et-widget-des-modes-temporaires
- **Type** : feature — **répond au besoin réel de l'incident du 2026-10-02**
- **Statut** : todo
- **Priorité** : **haute** — c'est la seule des specs issues de cet incident qui
  traite la demande d'origine ; feature-138 ne fait que l'AFFICHER
- **Version cible** : 2.55+
- **Dernière mise à jour** : 2026-10-04
- **Spec(s) liée(s)** : `specs/features/done/feature-138-…` (bandeau de blocage, qui doit proposer ce mode) · feature-011 (mode Programmée) · feature-053 (Boost, **le patron à suivre**) · feature-055 (ligne d'effet honnête du Boost) · feature-139 (battement de la porte) · feature-137

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage — *indirectement : le Boost prolonge la filtration*
- [x] **Web — routes HTTP / WebSocket**
- [x] **MQTT / Home Assistant**
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] **Historique / persistance** — état du mode, expiration
- [x] **Frontend (`data/`)**

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT, avant code et sur le diff réel**
- [x] **ux-ui-designer** — nouveau widget, regroupement, nommage
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Permettre de **continuer à injecter du chlore quand les sondes ne sont plus
fiables** — eau trouble après pluie, boue au fond, sonde douteuse — par un mode
**temporaire, borné en volume, et qui s'auto-annule**. Et regrouper ce mode avec
le Boost dans un widget unique de **modes temporaires**, puisque l'utilisateur
les cherchera au même endroit et que leurs effets se combinent.

## Contexte

### Le besoin, formulé par l'utilisateur

> « Il y a eu des pluies diluviennes. Beaucoup de boue a coulé dans la piscine
> dont le fond est marron. […] Le capteur de pH indique que les mesures sont
> instables. Le capteur ORP aussi. Donc, plus aucune injection ne se fait. Je
> voudrais qu'on travaille sur une solution pour ce genre de cas. Car le fait
> d'arrêter toute injection de chlore accentue le problème de qualité d'eau en
> attendant un retour à la normale. »

Puis, après livraison de feature-138 :

> « L'objectif est donc de pouvoir continuer à injecter du chlore, même avec des
> mesures qui ne sont pas bonnes. S'il y a de la boue et que tout est coupé, la
> piscine ne va jamais revenir à la normale. Pour le moment, ce que tu as fait
> n'apporte aucune solution. »

⚠️ **C'est exact, et c'est une erreur de cadrage à retenir.** L'arbitrage proposé
à l'utilisateur offrait trois options dont **aucune ne traitait sa demande** :
l'observabilité (feature-138, livrée) affiche le blocage sans le lever, et le
mécanisme qui le levait avait été renvoyé à « une spec séparée plus tard ».

### Le mécanisme existe DÉJÀ — et c'est le point de départ

Le mode **Programmée** (`orp_regulation_mode = "scheduled"`, feature-011) dose un
volume quotidien réparti par fenêtres de 15 min sur l'horizon de filtration
restant, **sans jamais regarder l'ORP** :

```cpp
// src/pump_controller.cpp:4365
} else if (orpMode == "scheduled" && orpLimitOk && orpSafetyOk &&
           mqttCfg.orpDailyTargetMl > 0 && !isOrpInhibitedByPh()) {
```

> *« Intentionnellement aveugle à la valeur mesurée de l'ORP : l'utilisateur a
> choisi un volume quotidien fixe de chlore indépendamment de la mesure ; un
> capteur déréglé ne doit pas bloquer l'injection. Seules les gardes
> volumétriques… »*

Il ne passe **ni** par la porte de mesure (`FilterGateClosed`), **ni** par le
latch d'instabilité, **ni** par les rejets consécutifs. Et le seul point qui
aurait pu rouvrir le piège est déjà traité : quand le pH est indisponible,
l'inhibition ne s'applique PAS en mode Programmée
(`pump_controller.cpp:1104-1114` — *« aveugle à la mesure par conception, on ne
change rien »*). Elle ne joue que si le pH est **connu ET trop haut**, ce qui est
chimiquement juste.

**Ce qui manque n'est donc pas un mécanisme de dosage. C'est :**

1. **La découvrabilité.** Rien n'indique à l'utilisateur que ce mode existe ni
   qu'il est la réponse à sa situation. Il a passé plusieurs jours bloqué devant
   un appareil qui savait faire ce qu'il demandait.
2. **La réversibilité.** Basculer `orp_regulation_mode` est un changement de
   configuration **persisté** : il faut penser à revenir en Automatique quand
   l'eau redevient claire, et **rien ne le rappelle**. Un dosage aveugle oublié
   est un risque de surdosage.
3. **Le bornage dans le temps.** Aucune expiration.

### Le patron à suivre existe aussi

Le **Boost** (feature-053) est décrit dans `constants.h` comme une *« surcouche
temporaire NON destructive activable d'un geste, auto-expirant au prochain minuit
local »*, avec un plafond dur dédié (`kBoostDailyHardCapMl = 1000`). C'est
exactement la forme voulue ici. **Ne pas réinventer : décalquer.**

### Pourquoi regrouper avec le Boost — l'argument réel

Ce **n'est pas** la symétrie : les deux modes sont de natures différentes, le
Boost **amplifie** la régulation, celui-ci la **remplace**.

C'est que **leurs effets se combinent, et de façon non évidente**. En mode
Programmée, le Boost n'est pas inerte : sa surchloration tombe
(`pump_controller.cpp:2753` — *« scheduled/manual ne voit JAMAIS la valeur
boostée »*) mais sa **prolongation de filtration continue de fonctionner**. Or
avec de la boue au fond, filtrer plus longtemps est précisément ce qu'on veut.

⚠️ **Conséquence de conception, à ne pas manquer : les deux modes ne sont PAS
mutuellement exclusifs.** Un widget qui les présenterait comme un choix unique
(Aucun / Boost / Secours) serait **faux** et priverait l'utilisateur de la
prolongation de filtration au moment où elle est la plus utile.

L'interface sait déjà dire la vérité sur les effets partiels : feature-055 affiche
« Filtration prolongée seule » / « Sans effet (filtration non gérée, ORP non
automatique) » avec une pastille d'avertissement (`data/app.js:3996-4016`).
**C'est ce patron qu'il faut étendre**, pas remplacer.

## Comportement attendu

### A — Le mode de chloration de secours

- **Non destructif** : il ne modifie PAS `orp_regulation_mode` en NVS. C'est une
  surcouche ; à l'expiration, la régulation Automatique reprend **seule**, sans
  que l'utilisateur ait à se souvenir de quoi que ce soit.
- **Borné en volume**, par un volume quotidien dédié **et** un plafond dur en
  dur dans `constants.h`, sur le modèle de `kBoostDailyHardCapMl`.
- **Auto-expirant.** Durée à trancher — voir Cas limites.
- **Aveugle à l'ORP par construction**, en empruntant le chemin `scheduled`
  **existant**. ⚠️ Ne PAS écrire un second chemin de dosage : réutiliser
  `evaluateScheduledDose()` et la branche en place.
- **Gardes conservées, sans exception** : présence d'eau (filtration), watchdog,
  budget horaire, anti-cycling, anti-rafale, limites de sécurité.
- **Le pH n'est JAMAIS concerné.** Ce mode ne dose que du chlore. Une injection
  d'acide à l'aveugle sur une mesure douteuse ne se reprend pas.

### B — Le widget des modes temporaires

- Regroupe **Boost** et **Chloration de secours** dans une seule carte.
- **Deux leviers indépendants**, pas un choix unique : ils se combinent.
- **Une ligne d'effet réellement obtenu**, étendant le patron de feature-055 :
  elle doit dire ce qui est actif *en vrai*, y compris les cas partiels
  (« Filtration prolongée + chloration de secours », « Filtration prolongée
  seule », « Sans effet »).
- Chaque levier affiche **son échéance**.
- Le bandeau de feature-138 doit **proposer ce mode** quand il signale un blocage
  dû à une mesure non représentative sur la voie ORP — c'est le chaînage qui
  manquait.

### Nommage — recommandation, à valider par `ux-ui-designer`

- **Widget : « Modes temporaires »** — les deux s'auto-annulent, c'est leur
  point commun le plus lisible pour l'utilisateur.
- **Mode : « Chloration de secours »**, avec un sous-titre nommant les deux
  situations : *« Pour eau trouble ou sonde douteuse : injecte un volume de
  chlore fixe sans tenir compte des sondes. »*
  - *Retenu contre* « Eau trouble » (évocateur mais trop étroit — le mode sert
    aussi à une sonde en panne) et « Dosage à l'aveugle » (exact mais inquiétant
    au point de dissuader de l'utiliser quand il le faudrait).
  - ⚠️ **Éviter absolument le mot « Programmée »** dans ce widget : c'est déjà le
    nom d'un mode de régulation permanent sur la page Chlore, et la confusion
    entre les deux serait immédiate.

### Cas limites
- **Durée d'expiration — c'est l'arbitrage central.** Minuit local (comme le
  Boost) force une réévaluation quotidienne de l'état de l'eau, ce qui est
  sain ; mais un épisode de boue dure **plusieurs jours** et un ré-armement
  quotidien oublié laisse l'eau sans chlore. À trancher par `pool-chemistry`,
  avec le coût des deux erreurs énoncé.
- **L'eau redevient claire avant l'expiration** : le mode continue de doser à
  l'aveugle alors que l'ORP est de nouveau exploitable. Faut-il le lever
  automatiquement quand les sondes redeviennent fiables, ou est-ce une
  sur-ingéniosité qui reproduirait le défaut qu'on fuit ?
- **Redémarrage pendant le mode** : survit-il ? Le Boost a déjà une réponse,
  s'en inspirer. Cf. aussi feature-136 (budget horaire non persisté).
- **Mode Manuel actif sur l'ORP** : que fait le secours ?
- **Filtration arrêtée** : rien ne doit être injecté, sans exception.
- **pH connu et > 7,6** : l'inhibition s'applique déjà en Programmée et doit
  continuer de s'appliquer — le chlore y est inefficace.
- **Plafond journalier déjà atteint** : le secours ne le franchit pas.

## Comportement actuel

- Le mode Programmée existe et fait le travail, mais c'est un **changement de
  configuration permanent**, invisible depuis le tableau de bord, qu'il faut
  penser à défaire.
- Le Boost a sa propre carte sur le tableau de bord
  (`data/index.html:241-271`, `#dashboard-boost-card`).
- Aucun chaînage entre un blocage constaté et l'action corrective.

## Contraintes techniques
- **Réutiliser la branche `scheduled` existante.** Aucun second chemin de dosage.
- Logique d'expiration **pure** (ADR-0017), testable nativement, repliement de
  `millis()` compris.
- Non destructif : aucune écriture de `orp_regulation_mode` en NVS.
- Plafond dur en `constants.h`, sur le modèle de `kBoostDailyHardCapMl`.
- Parité `/data` ↔ WebSocket pour tout champ nouveau — **trois récidives en trois
  versions**, prouver par grep des deux côtés.
- ⚠️ **Budget d'octets** : la trame WS n'a plus que **39 o** de marge sous
  `kWsFrameBufBytes` (ADR-0086, feature-138). **Tout champ nouveau doit être
  financé par un retrait.** Le compter, ne pas l'estimer.
- Auto-discovery Home Assistant si un nouvel état est publié.
- Aucune allocation sur chemin périodique (ADR-0082) ; `esp_task_wdt_reset()`.

## Fichiers potentiellement concernés
- `src/pump_controller.{h,cpp}` — surcouche sur la branche `scheduled`
- un module pur `src/temp_mode_logic.{h,cpp}` [nouveau] — expiration, effets combinés
- `src/constants.h` — plafond dur, durée, bornes
- `src/config.{h,cpp}` — persistance de l'état du mode
- `src/web_routes_control.cpp` — activation / arrêt
- `src/ws_manager.cpp`, `src/web_routes_data.cpp` — état, **parité + budget**
- `src/mqtt_manager.cpp` — topic + auto-discovery HA
- `data/index.html`, `data/app.js`, `data/app.css` — widget, chaînage depuis le bandeau
- `docs/API.md`, `docs/MQTT.md`, `docs/features/page-dashboard.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Surcouche non destructive empruntant la branche `scheduled` existante
- [ ] A2 — Expiration automatique, durée tranchée par `pool-chemistry`, logique pure
- [ ] A3 — Volume quotidien dédié + **plafond dur** dans `constants.h`
- [ ] A4 — Comportement au redémarrage tranché et testé
- [ ] A5 — État exposé à **parité** `/data` ↔ WS, budget d'octets **compté et financé**
- [ ] A6 — Topic MQTT + auto-discovery HA
- [ ] B1 — Widget « Modes temporaires » regroupant Boost et Chloration de secours, **deux leviers indépendants**
- [ ] B2 — Ligne d'**effet réellement obtenu**, étendant le patron de feature-055 aux cas combinés
- [ ] B3 — Échéance affichée par levier
- [ ] B4 — **Chaînage depuis le bandeau de feature-138** : proposer le mode quand l'ORP est bloqué sur mesure non représentative
- [ ] D1 — Tests natifs : expiration, repliement `millis()`, effets combinés, redémarrage, plafond dur
- [ ] D2 — **Mutation** : supprimer l'expiration ⇒ un test meurt ; retirer le plafond dur ⇒ un autre ; rendre les deux modes exclusifs ⇒ un troisième
- [ ] D3 — Non-régression : le Boost seul se comporte exactement comme avant
- [ ] C1 — `docs/features/page-dashboard.md`, `docs/subsystems/pump-controller.md`
- [ ] C2 — `docs/API.md`, `docs/MQTT.md`
- [ ] C3 — `CHANGELOG.md`
- [ ] C4 — `docs/user/src/guide-utilisation.html` **et** `guide-regulation.html` (le comportement de régulation visible change) + capture à signaler
- [ ] C5 — ADR si l'arbitrage de durée ou la non-exclusivité est structurant

## Sécurité chimique
- **Dosage impliqué : OUI, directement.** Chlore uniquement. **Jamais le pH.**
- **Le mode dose à l'aveugle, par conception.** C'est son objet, et c'est aussi
  tout son risque : si l'eau redevient claire et que le mode court toujours, on
  surdose sans le voir. Le bornage en **volume** et l'**expiration** sont les
  deux seules protections — elles ne sont pas négociables.
- ⚠️ **Fait chimique central, à instruire** : en eau chargée de matière
  organique, l'ORP **ne remonte pas** tant que le point de rupture n'est pas
  atteint. Mesure du terrain : **2 L/jour pendant trois jours** (plafond
  journalier atteint les 01, 03 et 04/10) et l'ORP est resté à **261 mV pour une
  consigne de 530**. Autrement dit : **le volume injecté n'était déjà pas le
  facteur limitant**, et le retrait mécanique de la boue reste le seul vrai
  remède. Le mode doit donc être dimensionné pour **maintenir une désinfection
  minimale**, pas pour atteindre une consigne ORP inatteignable.
- **Arbitrages demandés à `pool-chemistry`** :
  1. La **durée d'expiration** (minuit comme le Boost, ou plusieurs jours), avec
     le coût des deux erreurs : ré-armement oublié ⇒ eau sans chlore ; mode
     oublié ⇒ dosage aveugle prolongé.
  2. Le **volume quotidien par défaut** et le **plafond dur**, pour un bassin de
     30-50 m³, sachant que 2000 mL/j n'ont pas fait bouger l'ORP.
  3. Faut-il **lever automatiquement** le mode quand les sondes redeviennent
     fiables ?
  4. Le mode doit-il être refusé si le **pH est inconnu** ? (Aujourd'hui la
     branche `scheduled` l'accepte, délibérément.)
- Limites journalière/horaire, garde de présence d'eau, anti-cycling,
  anti-rafale, watchdog : **tous conservés**.
- **Gate bloquant avant code, et second passage sur le diff réel.**

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (toute surcouche temporaire future devra se
  positionner sur l'axe durée/bornage) · 2 (la non-exclusivité des deux modes est
  une décision invisible dans le code) · 3 (alternatives de durée crédibles)
- **Sujet pressenti** : « Ce qu'un mode temporaire a le droit de faire sans
  qu'on le lui rappelle »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Activation / arrêt du mode (à arrêter par l'architect, sur le modèle du Boost)

### Champs WebSocket
- État du mode + échéance — **mêmes clés que `/data`**, et **financés** sous les 39 o restants

### Topics MQTT
- État du mode + auto-discovery HA (switch), sur le modèle du Boost

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-dashboard.md` — le widget
- `docs/subsystems/pump-controller.md` — la surcouche

#### Docs transverses
- `docs/API.md`, `docs/MQTT.md`, `CHANGELOG.md`
- `docs/user/src/guide-utilisation.html` **et** `guide-regulation.html`

## Critères d'acceptation
- [ ] Sondes ORP déclarées non fiables + filtration active ⇒ **le chlore continue d'être injecté**
- [ ] Le mode **s'annule seul**, sans intervention
- [ ] `orp_regulation_mode` en NVS est **inchangé** après un cycle complet du mode
- [ ] Le Boost reste activable **en même temps**, et sa prolongation de filtration fonctionne
- [ ] La ligne d'effet dit la vérité dans **tous** les cas combinés
- [ ] Le bandeau de feature-138 propose le mode quand c'est pertinent
- [ ] Aucune injection sans filtration, sans watchdog, ni au-delà des limites
- [ ] Le pH n'est **jamais** dosé par ce mode
- [ ] Parité `/data` ↔ WS prouvée par grep ; trame WS sous `kWsFrameBufBytes`
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués

## Plan de test manuel
1. Provoquer un blocage ORP (purge), armer le mode, vérifier que le chlore est injecté.
2. Vérifier l'expiration automatique, puis que la régulation Automatique reprend seule.
3. Armer Boost **et** secours : vérifier que la filtration est prolongée ET que le chlore est injecté, et que la ligne d'effet le dit.
4. Couper la filtration pendant le mode : plus aucune injection.
5. Redémarrer pendant le mode : comportement conforme à ce qui a été tranché.
6. Vérifier dans `/get-config` que `orp_regulation_mode` n'a pas changé.

## Notes pour agent

⚠️ **Le mécanisme de dosage EXISTE — n'en écris pas un second.** La branche
`scheduled` (`pump_controller.cpp:4365`) fait déjà exactement le travail et a
déjà été validée par `pool-chemistry` lors de feature-011. Le travail est une
**surcouche**, de la découvrabilité et de la réversibilité, pas de la chimie
nouvelle.

⚠️ **Décalquer le Boost (feature-053), ne pas réinventer.** Surcouche non
destructive, un geste, auto-expirante, plafond dur dédié : tout est déjà écrit.

⚠️ **Les deux modes ne sont PAS exclusifs.** En Programmée, le Boost perd sa
surchloration mais conserve sa prolongation de filtration — utile avec de la
boue. Un widget qui en ferait un choix unique serait faux.

⚠️ **39 octets.** C'est tout ce qui reste dans la trame WS (ADR-0086). Tout champ
nouveau doit être financé par un retrait. Compter, pas estimer.

⚠️ **Ne pas promettre ce que la chimie ne tient pas.** 2 L/jour n'ont pas fait
bouger l'ORP pendant l'épisode. Ce mode **maintient une désinfection**, il ne
« règle » pas une eau boueuse — seul le retrait mécanique le fait. L'interface ne
doit pas laisser croire le contraire.

## Hors périmètre
- Le battement de la porte de mesure : **feature-139**.
- L'observabilité du blocage : **feature-138**, livrée.
- La cause physique du bruit de mesure : **feature-137**.
- Toute modification des seuils de la porte ou des gardes de dosage.
- Le dosage pH à l'aveugle — **exclu définitivement**, pas reporté.
