# feature-140 — Chloration de secours, et regroupement des modes temporaires

## Métadonnées
- **ID** : feature-140
- **Slug** : chloration-de-secours-et-widget-des-modes-temporaires
- **Type** : feature — **répond au besoin réel de l'incident du 2026-10-02**
- **Statut** : done — livré v2.55.0 (2026-10-05)
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

### ⚠️ CORRECTION DE CETTE SPEC (2026-10-05) — la surcouche naïve est MORTE À L'ARRIVÉE

Le paragraphe ci-dessus affirme que « quand le pH est indisponible, l'inhibition
ne s'applique pas dans ce mode ». **Ce n'est vrai que si `orp_regulation_mode`
vaut littéralement `"scheduled"` EN NVS.** Vérifié ce jour, `pump_controller.cpp:1105` :

```cpp
const bool requireKnownPh = (mqttCfg.orpRegulationMode == "automatic");
```

Or cette spec exige une surcouche qui **n'écrit pas la NVS**. La configuration
reste donc `"automatic"`, `requireKnownPh` reste vrai, un pH NaN pose
l'inhibition ORP, et `!isOrpInhibitedByPh()` ferme la branche `scheduled` :
**zéro injection** — précisément le scénario que la feature doit résoudre.

**Conséquence de conception, non négociable** : il faut un accesseur de **« mode
ORP EFFECTIF »** (NVS + surcouche), et router chaque site vers le bon, selon une
règle écrite :

> **décision de dosage ⇒ mode EFFECTIF · publication d'état de CONFIGURATION ⇒ valeur NVS**

Confondre les deux casse le contrat Home Assistant (une automatisation croirait
la config changée et pourrait la réécrire) ; les séparer mal tue la feature.

Sites recensés — **liste corrigée et complétée le 2026-10-05 par audit du code**,
deux erreurs de classement ayant été trouvées dans la première version :

- **DÉCISION ⇒ mode EFFECTIF** : `pump_controller.cpp` 1105, 1418, 2603, 2613,
  2645, 4154 · `web_routes_control.cpp:767` · `time_to_target.cpp:298` ·
  **`ws_manager.cpp:883`** — ⚠️ initialement classé « publication, c'était FAUX.
  Il publie `boost_chlorine_boosted = (orpRegulationMode == "automatic")`, qui
  n'est pas la config brute mais un **verdict dérivé** : « le Boost relève-t-il
  réellement le chlore ? ». Secours actif, la surchloration ne s'applique pas —
  lire la NVS ici ferait **mentir la ligne d'effet**, celle-là même qui doit
  rester honnête (feature-055).
- **PUBLICATION ⇒ valeur NVS (contrat externe)** : `ws_manager.cpp:1162` ·
  `mqtt_manager.cpp` 817, 825, 1044, 1057 · **`web_routes_config.cpp:159-160`**
  — ⚠️ **manquait à la liste** : c'est `/get-config`, contrat HTTP externe au même
  titre que le WS et MQTT. `mqtt_manager.cpp:1810-1812` reçoit la commande et
  écrit la NVS : correct, ne pas y toucher.
- **Point de passage à vérifier** : `dosing_logic.h:184` (`modeAutomatic`) — champ
  d'entrée du module pur, alimenté par l'appelant. Router
  `pump_controller.cpp:4154` vers le mode effectif le fait hériter
  automatiquement ; ce n'est pas un site séparé, mais il doit être constaté.

**Contrainte NVS mesurée** (clés Preferences ≤ 15 caractères, troncature
silencieuse au-delà) : `rescue_active` (13) et `rescue_until` (12) passent ;
`rescue_target_ml` (16) et `chlorine_rescue_active` (22) **ne passent pas**.

**Suites natives à surveiller** : `test_native_schedule/test_schedule_logic.cpp`
est le **premier candidat à casser** si la branche `scheduled` est touchée ;
`test_native_dosing/test_dosing_decision.cpp` couvre les calculs Boost (cible ORP
effective, limite chlore effective) et doit rester vert en non-régression.
⚠️ **CORRIGÉ le 2026-10-05 — cette affirmation était FAUSSE** et venait de
l'analyse d'impact, reprise sans vérification. `effectiveOrpTargetPure` et
`effectiveMaxChlorinePure` (le Boost seul) **sont testées nativement depuis le
2026-07-08** (feature-053) : 13 occurrences dans
`test/test_native_dosing/test_dosing_decision.cpp`, vérifié par `git log -S`.
Ce qui n'est effectivement pas couvert en natif : `isBoostActive`,
`saveBoostState`, `loadBoostState` — persistance Preferences, hors périmètre pur.
Seule la **composition** Boost × secours était neuve, et elle est désormais
couverte.

### ⚠️ État réel de l'appareil au moment d'écrire (2026-10-05)

L'utilisateur a **déjà basculé manuellement** son chlore en mode **Programmée à
1500 mL/jour**, en attendant cette feature. À la première activation de la
surcouche, sa NVS vaudra donc `"scheduled"`, **pas** `"automatic"`.

Deux cas limites qui en découlent, à traiter et à tester :
- **Idempotence** : activer la surcouche alors que la NVS est déjà `"scheduled"`.
- **Retour correct à l'expiration** : revenir à `"scheduled"` — la valeur réelle
  de l'utilisateur — et surtout **jamais** forcer `"automatic"`. Une surcouche
  qui « restaure » un mode que l'utilisateur n'avait pas choisi serait destructive
  au sens le plus littéral.

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

## Décisions d'architecture arrêtées (2026-10-05)

### Budget WS — RÉSOLU, et il s'améliore
Élagage de deux clés **mortes** de la seule trame WS (vérifié par grep dans
`data/`, formes dynamiques comprises — aucun consommateur ; les clés voisines
`filtration_ext_known`, `filtration_ext_age_s`, `boost_filtration_extended` sont
**différentes** et conservées) :

| | Octets |
|---|---:|
| `"filtration_state_source":"commanded",` retiré | −38 |
| `"filtration_ext_on":false,` retiré | −26 |
| `"rescue_until":1790000000,` ajouté | +26 |
| `"rescue_dosing":false,` ajouté | +22 |
| **Net** | **−16** |

Pire cas : 3545 − 64 + 48 = **3529 o** sous 3584 → **marge 55 o** (elle était de 39).
Les deux clés élaguées passent `/data`-seules, patron ADR-0086. **Pas de nouvel
ADR pour le financement** : même décision, même méthode, ADR-0086 couvre.

⚠️ **Pas d'entrée dans `kWsOnlyLive`** : son cliquet à 18 interdit toute clé
WS-seule neuve.

Écartés et chiffrés : durée restante en minutes (−8 o, mais rompt la convention
epoch du Boost et périme entre deux pushes) · relever `kWsFrameBufBytes`
(+512 o de `.bss` en famine 8 bits, alors qu'une option à coût nul existe) ·
un champ `rescue_active` séparé (inutile, `rescue_until > 0` ⟺ actif).

### Mode effectif — règle opposable
> **Une lecture du mode qui décide d'injecter, de combien ou de quand lit le mode
> EFFECTIF. Une lecture qui répond à « quelle est la configuration ? » lit la NVS.**

⚠️ **SITE SUPPLÉMENTAIRE trouvé par l'architecte, absent des deux audits
précédents** : `mqttCfg.orpDailyTargetMl` est lu en **2645, 4366, 4395 et 4470**.
Sans un `getEffectiveOrpDailyTargetMl()`, la branche `scheduled` reste **fermée**
par son test `> 0` quand la NVS est `"automatic"` avec 0 mL — second chemin par
lequel la feature serait morte à l'arrivée.

### Volume : une CONSTANTE, pas un réglage
Résout d'un coup A3 et la contrainte de longueur des clés NVS
(`rescue_target_ml` = 16 caractères, impossible). Deux clés seulement :
`rescue_active` (13) et `rescue_until` (12).

### Pas de repliement de `millis()` à tester
L'expiration est **epoch**, comme le Boost — il n'y a pas de `millis()` sur ce
chemin. ⚠️ Le livrable D1 mentionnait un test de repliement : **le retirer
plutôt que de tester du vide.** Un test qui ne peut pas échouer ne prouve rien.

### Non destructif par construction
À l'expiration on **relit** la NVS : il n'y a aucune valeur mémorisée à
restaurer. Le retour à `"scheduled"` (la valeur réelle de l'utilisateur) et le
fait de ne **jamais** forcer `"automatic"` en découlent sans code dédié.

### ADR
Un seul, « Ce qu'un mode temporaire a le droit de faire sans qu'on le lui
rappelle » — critères 1 (le couple mode effectif / mode publié s'impose à toute
surcouche future), 2 (la non-exclusivité Boost × Secours est invisible dans le
code) et 3 (alternatives de durée crédibles).

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
- [x] A0 — **Accesseur « mode ORP effectif »** et routage site par site (décision ⇒ effectif, publication ⇒ NVS). **Sans lui, la feature ne fonctionne pas.**
- [x] A1 — Surcouche non destructive empruntant la branche `scheduled` existante
- [x] A2 — Expiration automatique, durée tranchée par `pool-chemistry`, logique pure
- [x] A3 — Volume quotidien dédié + **plafond dur** dans `constants.h`
- [x] A4 — Comportement au redémarrage tranché et testé
- [x] A5 — État exposé à **parité** `/data` ↔ WS, budget d'octets **compté et financé**
- [x] A6 — Topic MQTT + auto-discovery HA
- [x] B1 — Widget « Modes temporaires » regroupant Boost et Chloration de secours, **deux leviers indépendants**
- [x] B2 — Ligne d'**effet réellement obtenu**, étendant le patron de feature-055 aux cas combinés
- [x] B3 — Échéance affichée par levier
- [x] B4 — **Chaînage depuis le bandeau de feature-138** : proposer le mode quand l'ORP est bloqué sur mesure non représentative
- [x] D0 — **Test avec pH NaN et NVS `"automatic"`** : l'injection DOIT avoir lieu. C'est le test qui prouve que la feature n'est pas morte à l'arrivée.
- [x] D1 — Tests natifs : expiration, effets combinés, redémarrage, plafond dur, **idempotence sur NVS déjà `"scheduled"`**, **retour à `"scheduled"` et jamais à `"automatic"`**. ⚠️ **Pas de test de repliement `millis()`** : l'expiration est epoch, il n'y a rien à replier — ne pas tester du vide.
- [x] D2 — **Mutation** : supprimer l'expiration ⇒ un test meurt ; retirer le plafond dur ⇒ un autre ; rendre les deux modes exclusifs ⇒ un troisième
- [x] D3 — Non-régression : le Boost seul se comporte exactement comme avant
- [x] C1 — `docs/features/page-dashboard.md`, `docs/subsystems/pump-controller.md`
- [x] C2 — `docs/API.md`, `docs/MQTT.md`
- [x] C3 — `CHANGELOG.md`
- [x] C4 — `docs/user/src/guide-utilisation.html` **et** `guide-regulation.html` (le comportement de régulation visible change) + capture à signaler
- [x] C5 — ADR si l'arbitrage de durée ou la non-exclusivité est structurant

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

## Verdict `pool-chemistry` — gate 1 (2026-10-05) : **GO SOUS CONDITIONS**

**Fait de terrain VALIDÉ** : 2000 mL/j × 3 jours, ORP bloqué à 261 mV — le volume
n'était pas le facteur limitant. Besoin d'entretien calculé pour 30-50 m³
(chlore 36° ≈ 114 g Cl₂/L, 2,5-3 mg/L à CYA 35 par la règle des 7,5 %) :
**350-530 mL/j**. 1500 mL/j ≈ 3× le besoin — marge correcte pour une demande
organique élevée. Dimensionnement **maintien**, jamais rattrapage.

### Les cinq arbitrages tranchés
1. **Durée = 48 h glissantes (epoch).** ⚠️ **Minuit local est DISQUALIFIÉ
   TECHNIQUEMENT** : l'expiration coïnciderait avec le rollover du compteur
   journalier — une activation à 10 h ne recevrait qu'une fraction de son volume
   *et* mourrait au reset. 72 h écarté (mode oublié, sans réévaluation).
2. **Volume = 1500 mL/j, plafond dur = 2000 mL/j** (le point terrain prouvé
   inutile : inutile de jamais le dépasser).
3. **Levée automatique OUI, mais À SENS UNIQUE** : elle *termine* le mode, ne le
   ré-arme jamais. Un état absorbant **ne peut pas battre par construction** — le
   battement de feature-139 exige une bascule à deux sens. Seuil : 30 min continues.
4. **pH inconnu : NE PAS refuser.** Le refus désarmerait la feature dans son
   scénario nominal (la boue encrasse les deux sondes). Le risque habituel du pH
   inconnu — la boucle compense par surdosage — **n'existe pas ici** : volume fixe
   borné, aucune poursuite de consigne.
5. **Mode `manual` : le secours est REFUSÉ, pas inerte.** « Je dose moi-même » +
   injection aveugle = double dosage. Désarmement automatique si l'utilisateur
   bascule en manuel pendant le mode.

### Les 12 conditions, vérifiables une par une
1. `kRescueChlorineDurationS = 172800` (48 h), échéance **epoch** en NVS.
2. `kRescueChlorineDailyMl = 1500.0f` et `kRescueChlorineDailyHardCapMl = 2000.0f`.
3. Volume effectif = `min(1500, safetyLimits.maxChlorineMlPerDay si > 0, 2000)` —
   **la limite utilisateur reste souveraine** si elle est plus basse.
4. ⚠️ **CHEMIN DE CONTOURNEMENT DU PLAFOND, à fermer explicitement** : Boost +
   secours simultanés, `kBoostDailyFactor` (×1,5) appliqué au volume de secours
   donnerait **2250 mL > plafond dur**. Le clamp sur
   `kRescueChlorineDailyHardCapMl` doit être appliqué **EN DERNIER**, après toute
   amplification Boost. **Test dédié obligatoire.**
5. Compteur journalier **PARTAGÉ** (`safetyLimits.dailyOrpInjectedMl`) — aucun
   compteur dédié. Armer après plafond atteint ⇒ 0 mL.
6. Au boot, `epoch < kMinValidEpoch` ⇒ mode **inactif** (fail-closed) jusqu'à
   heure valide. Jamais actif-pour-toujours.
7. Le reboot ne remet **pas** à zéro le compteur journalier (sinon boot-loop =
   contournement du plafond).
8. `isOrpInhibitedByPh()` **conservée telle quelle** : pH connu > 7,60 ⇒
   chloration suspendue, y compris en secours (41 % de HOCl à 7,70).
9. Filtration arrêtée ⇒ aucune injection, sans exception. Le secours **ne
   prolonge pas** la filtration (c'est le rôle du Boost).
10. Levée automatique à sens unique, 30 min, **annoncée** (log `info` + MQTT + UI).
11. Aucun chemin ne doit pouvoir atteindre une pompe **pH** depuis ce mode.
12. Libellés L1-L8 repris **MOT À MOT**.

### Libellés imposés — à reprendre SANS reformulation ni troncature
⚠️ Précédent feature-138 : une version tronquée avait perdu l'avertissement sur
les chloramines. **Toute évolution de ces textes exige un nouveau passage du gate.**

- **L1 — sous-titre du mode** : « Pour eau trouble ou sonde douteuse : injecte un volume de chlore fixe, sans tenir compte des sondes. Maintient une désinfection minimale — ne rattrape pas une eau boueuse : seul le retrait mécanique de la boue le fait. »
- **L2 — confirmation avant activation** : « Le dosage se fera à l'aveugle, sans contrôle de l'ORP, pendant 48 h au maximum. Volume plafonné à 1500 mL par jour. Le pH n'est jamais dosé dans ce mode. Désactivez-le dès que les sondes sont de nouveau fiables. »
- **L3 — filtration à l'arrêt** : « Sans effet pour l'instant : filtration à l'arrêt, aucune injection possible. »
- **L4 — pH connu et trop haut (gabarit)** : « Chloration suspendue : pH %s — le chlore y est peu actif. Faire baisser le pH d'abord. »
- **L5 — plafond journalier atteint** : « Volume quotidien atteint (1500 mL). Reprise demain. Si l'ORP ne monte pas malgré ce volume, le facteur limitant n'est pas le chlore. »
- **L6 — levée automatique** : « Chloration de secours levée automatiquement : les mesures ORP sont de nouveau fiables depuis 30 minutes. La régulation automatique a repris. »
- **L7 — chaînage depuis le bandeau feature-138** : « Dosage du chlore bloqué : mesures non représentatives. Vous pouvez activer la chloration de secours pour continuer à injecter un volume fixe. »
- **L8 — refus en mode Manuel** : « Chloration de secours indisponible : la régulation du chlore est en mode Manuel. Repassez en Automatique ou Programmée pour l'utiliser. »

**Interdits de vocabulaire** : « corrige l'eau », « assainit », « rétablit la
qualité de l'eau », « règle le problème de boue », « nettoie ».

### Recommandation non bloquante
Quand le secours est armé et la filtration non prolongée, **suggérer le Boost**
dans la même carte : la prolongation de filtration est le seul levier réellement
utile sur de la boue.

**Second passage obligatoire sur le diff réel.**

## Verdict `pool-chemistry` — gate 2 sur le diff réel (2026-10-05) : **GO SOUS CONDITIONS**

Les 12 conditions vérifiées **au fichier et à la ligne**. **C4 confirmée sur le
code et non sur le commentaire** : `temp_mode_logic.cpp:32-41` applique
`1500 → ×1,5 Boost → clamp utilisateur → clamp plafond dur EN DERNIER`, donc
2250 ⇒ 2000. Les libellés L1, L2 et L5 relus caractère par caractère par le gate
lui-même, tirets cadratins et espace insécable de « 48 h » compris.

### Trois points bloquants, tous traités
1. **Aucune suite native ne couvrait `temp_mode_logic`** — C4 n'était prouvée que
   par lecture. Confié au `test-engineer` avec une mutation dédiée : déplacer le
   clamp AVANT l'amplification Boost doit tuer un test.
2. **`mqtt_manager.cpp` loggait « activée via MQTT/HA » même sur un REFUS**
   (mode Manuel, heure non synchronisée) — journal contradictoire sur un chemin
   de dosage. **Corrigé** : l'état réel est constaté, et un `warning` explicite
   dit « REFUSÉE » le cas échéant.
3. **Le texte d'expiration proposé était FAUX** — voir L9 ci-dessous.

### L9 — nouveau libellé opposable (expiration des 48 h)
> « Chloration de secours terminée : les 48 h maximum sont écoulées. Rien ne
> prouve que l'eau est redevenue fiable. La régulation automatique a déjà repris
> selon votre configuration : contrôlez le chlore libre au réactif DPD et le pH
> avant de lui faire confiance, et réarmez la chloration de secours si les sondes
> restent douteuses. »

Ton `warning`, pas `info`. Affiché **uniquement** sur `rescue_lift == 2`.

⚠️ **Pourquoi la première version a été refusée** — à retenir : elle invitait à
contrôler « avant de reprendre la régulation automatique ». Or à l'expiration le
mode effectif retombe seul sur la NVS : **la régulation a DÉJÀ repris**. Le texte
promettait un délai inexistant et masquait le vrai risque — une sonde ORP dérivée
mais *stable*, qui passerait les gardes et ferait surdoser. Et « contrôlez l'ORP »
demandait de faire confiance à la mesure même qu'on soupçonne : d'où le **DPD
nommé**, indépendant de la sonde.

`rescue_lift == 3` (arrêt manuel) : **aucun message**, confirmé — l'utilisateur
vient d'agir.

### L5 scindé en une PAIRE opposable
Le seuil de déclenchement est désormais dérivé du volume effectif
`min(1500, limite utilisateur si > 0, 2000)`. Mais le texte portait le chiffre
**et une conclusion de diagnostic**.

- **L5a** — volume effectif ≥ 1500 mL (plein du mode), texte inchangé :
  > « Volume quotidien atteint (1500 mL). Reprise demain. Si l'ORP ne monte pas malgré ce volume, le facteur limitant n'est pas le chlore. »
- **L5b** — volume effectif < 1500 mL (plafonné par la limite utilisateur), `%s` = entier :
  > « Volume quotidien atteint (%s mL), plafonné par votre limite journalière. Reprise demain. Ce volume est inférieur aux 1500 mL du mode de secours : on ne peut pas en conclure que le chlore n'est pas le facteur limitant. »

⚠️ **Le chiffre n'était pas le vrai problème.** La seconde phrase de L5a porte une
**inférence** — « le facteur limitant n'est pas le chlore » — qui n'est valide que
si le volume atteint est le plein de 1500 mL. Plafonnée à 800 par une limite
utilisateur, elle devient fausse ET dangereuse : elle détournerait du seul
correctif réel, relever cette limite. **Retirer simplement le chiffre aurait
supprimé le chiffre faux en gardant l'inférence fausse** — la correction évidente
aurait semblé marcher en laissant le défaut intact.

Les deux variantes forment une **paire** : modifier l'une sans l'autre casse la
cohérence du raisonnement.

### Recommandation du gate devenue BLOQUANTE
Le déclenchement de L5 sur le **volume effectif** (et non sur 1500 en dur) est le
préalable technique de L5b. Appliqué.

### Reste non bloquant, à consigner
`_rescueStableSinceMs` n'est pas remis à zéro pendant une coupure de filtration —
rattrapé en pratique par `measuresValid()`, mais une remise à zéro explicite
serait plus sûre.


## Design arrêté (2026-10-05)

### Structure
Carte unique `#dashboard-temp-modes-card` « Modes temporaires », contenant deux
sous-blocs séparés par un `<hr class="temp-mode__sep">` : `#temp-mode-boost`
(markup du Boost **INCHANGÉ**, simplement remonté comme sous-bloc) et
`#temp-mode-rescue` (nouveau). Puis une ligne d'effet combiné
`#temp-modes-effect`. Aucune variable CSS nouvelle : réutilise `--border`,
`card__status-header`, `card__body`, `state-badge`, `detail-row`, `callout`.

### Ligne d'effet — 8 combinaisons, pas 24
⚠️ **Finesse à ne pas manquer** : `boost_chlorine_boosted` devient
**naturellement faux** dès que le mode ORP EFFECTIF n'est plus `automatic` —
donc déjà faux quand le secours est armé. **Aucun calcul client à ajouter**, et
`chlorBoost` × `rescueDosing` sont **exclusifs par construction firmware**.

| `filtExt` | `chlorBoost` | `rescueDosing` | Texte | Badge |
|---|---|---|---|---|
| ✗ | ✗ | ✗ | « Sans effet (filtration non gérée, ORP non automatique) » *(inchangé feature-055)* | warn |
| ✓ | ✗ | ✗ | « Filtration prolongée seule » | ok |
| ✗ | ✓ | ✗ | « Surchloration seule » | ok |
| ✓ | ✓ | ✗ | « Filtration prolongée + surchloration » | ok |
| ✗ | ✗ | ✓ | « Chloration de secours seule » | ok |
| ✓ | ✗ | ✓ | « Filtration prolongée + chloration de secours » | ok |
| — | ✓ | ✓ | **impossible** — ne pas coder la branche ; `console.warn` si jamais vue, ne pas planter | — |

### Armé mais non dosant
Les causes L3 / L4 / L5 n'entrent **pas** dans la ligne d'effet globale (qui doit
rester honnête sur ce que fait le Boost) : elles alimentent
`#rescue-reason-callout` **dans le sous-bloc secours**, dérivées côté client de
champs `/data` déjà existants (aucun champ WS de plus). Priorité d'affichage :
**L3 (filtration) > L4 (pH haut) > L5 (plafond)**. Badge « Actif · en attente »
avec un `aria-label` explicite — la distinction warn/ok par la couleur seule ne
porte aucun sens pour un lecteur d'écran.

### Échéances : deux lignes, jamais fusionnées
Le Boost expire à minuit, le secours court 48 h glissantes : deux natures
différentes. `#detail-rescue-until` au format `JJ/MM à HH:MM`, **sans** le libellé
« prochain minuit » du Boost. Masquée si `rescue_until <= 0`.

### Chaînage depuis le bandeau feature-138
Sur la **ligne ORP uniquement** (le secours ne concerne jamais le pH), même liste
blanche `[3,4,15,18,19,22]` — **aucune extension**. Bouton
`btn--primary` « Activer la chloration de secours » placé **avant** le
`btn--secondary` « Ouvrir l'injection manuelle » existant, avec le libellé **L7**
en contexte. ⚠️ **Le clic n'active pas** : il défile et met le focus sur
`#detail-rescue-start` — la confirmation **L2** reste un passage obligé.
Mode ORP effectif `manual` ⇒ pas de bouton, mais **L8** affiché à la place, pour
que l'absence s'explique au lieu de se chercher.

### ⚠️ Helpers — noms vérifiés, le rapport de design se trompait
- **`confirmDialog({ title, message, confirmLabel, cancelLabel })`**
  (`data/app.js:262`) — **PAS** `confirmModal(message)` : nom **et** signature
  différents (objet, pas chaîne).
- **`showToast(message, type)` EXISTE** (`data/app.js:229`) : l'utiliser pour
  **L6**. Le repli `callout--success` proposé est inutile — ne pas créer de
  composant pour un seul usage.

### Rétrocompatibilité
`rescue_until` / `rescue_dosing` absents (firmware ancien) ⇒ secours traité comme
inactif, **aucune erreur affichée** — patron déjà utilisé par
`boost_filtration_extended !== false`.

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
- [x] Sondes ORP déclarées non fiables + filtration active ⇒ **le chlore continue d'être injecté**
- [x] Le mode **s'annule seul**, sans intervention
- [x] `orp_regulation_mode` en NVS est **inchangé** après un cycle complet du mode
- [x] Le Boost reste activable **en même temps**, et sa prolongation de filtration fonctionne
- [x] La ligne d'effet dit la vérité dans **tous** les cas combinés
- [x] Le bandeau de feature-138 propose le mode quand c'est pertinent
- [x] Aucune injection sans filtration, sans watchdog, ni au-delà des limites
- [x] Le pH n'est **jamais** dosé par ce mode
- [x] Parité `/data` ↔ WS prouvée par grep ; trame WS sous `kWsFrameBufBytes`
- [x] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués

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

## Notes d'implémentation (2026-10-05, clôture)

### Décisions retenues
- **Mode ORP EFFECTIF** (`src/temp_mode_logic.h`/`.cpp`, module pur ADR-0017) :
  compose NVS + état secours. Routage audité en **deux passes** (un premier
  audit avait laissé passer quatre sites de lecture de `mqttCfg.orpDailyTargetMl`
  et un site de `ws_manager.cpp` mal classé publication/décision). Règle
  opposable formalisée dans [ADR-0087](../../docs/adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md).
- **Durée = 48 h glissantes (epoch)**, minuit local disqualifié techniquement
  (collision avec le rollover du compteur journalier de chlore) — pas une
  préférence, une incompatibilité structurelle. 72 h écarté (mode oublié).
- **Volume = constante** (`kRescueChlorineDailyMl = 1500`, plafond dur
  `kRescueChlorineDailyHardCapMl = 2000`), pas un réglage : la clé NVS
  `rescue_target_ml` (16 caractères) aurait dépassé la limite Preferences de
  15 caractères.
- **Levée automatique à SENS UNIQUE** (30 min de mesures ORP fiables) : termine
  le mode, ne le ré-arme jamais — condition posée pour ne pas reproduire le
  défaut de battement corrigé par feature-139.
- **Mode Manuel : refus, pas inertie** — désarmement automatique si
  l'utilisateur bascule en Manuel pendant que le mode est actif (HTTP et MQTT).
- **L5 scindé en paire opposable** (L5a volume plein / L5b plafonné par limite
  utilisateur) — la conclusion « le facteur limitant n'est pas le chlore »
  n'est valide que si le plein de 1500 mL a été atteint.
- **L9 réécrit** au gate 2 : la première version promettait un délai de
  contrôle inexistant (la régulation a déjà repris à l'expiration) et
  renvoyait à contrôler l'ORP — la mesure même qu'on soupçonne. Version
  retenue : contrôle au réactif DPD, indépendant de la sonde.

### Les deux verdicts `pool-chemistry`
- **Gate 1** (arbitrage initial) : GO SOUS CONDITIONS, 5 arbitrages tranchés
  (durée, volume, levée auto à sens unique, pH inconnu non bloquant, mode
  Manuel refusé) + 12 conditions vérifiables + libellés L1-L8 imposés.
- **Gate 2** (sur le diff réel) : GO SOUS CONDITIONS, 3 points bloquants tous
  traités avant livraison — (1) aucune suite native ne couvrait
  `temp_mode_logic` au premier passage, comblé par `test/test_native_temp_mode/`
  avec mutation dédiée à l'ordre des clamps ; (2) `mqtt_manager.cpp` loggait
  « activée » même sur un refus, corrigé (état réel constaté, `warning`
  explicite sur refus) ; (3) texte d'expiration initial faux, remplacé par L9.

### Corrections faites en cours de cycle (traçabilité)
- Classement décision/publication de `ws_manager.cpp:883` (`boost_chlorine_boosted`)
  corrigé : verdict dérivé, pas config brute.
- `web_routes_config.cpp:159-160` (`/get-config`) ajouté à la liste des sites
  de publication — absent du premier audit.
- Site supplémentaire `mqttCfg.orpDailyTargetMl` lu à quatre endroits
  (2645, 4366, 4395, 4470) — nécessite `getEffectiveOrpDailyTargetMl()`, pas
  seulement `effectiveOrpMode()`.
- Affirmation fausse de l'analyse d'impact initiale sur la couverture de test
  de `effectiveOrpTargetPure`/`effectiveMaxChlorinePure` — corrigée (elles
  étaient déjà couvertes depuis feature-053 ; seule la composition Boost ×
  secours était neuve).
- Retrait du test de repliement `millis()` prévu par erreur pour l'expiration
  (epoch, rien à replier) — conservé uniquement pour `rescue_lift` (fenêtre
  glissante en `millis()`, repliement réellement applicable).

### Tally d'octets final (WS `sensor_data`)
Retrait `filtration_state_source`/`filtration_ext_on` (−64 o) finance l'ajout
`rescue_until`/`rescue_dosing` (+48 o, net −16 o), puis `rescue_lift` (+16 o,
complément gate 2). Pire cas de trame final : **3545 o**, sous
`kWsFrameBufBytes = 3584` — **marge 39 o**. Cliquet `kWsOnlyLiveCeiling`
abaissé 18 → 16 (les deux clés migrées quittent `kWsOnlyLive`). `kDataOnly`
passe de 4 à 6 entrées.

### Tests et mutants
**56 suites natives / 2023 cas**, tous verts (`pio test -e native`). Flash
50,0 %. **4 mutants tués** par la suite `test/test_native_temp_mode/` :
suppression de l'expiration, retrait du plafond dur, modes rendus exclusifs,
et inversion de l'ordre des clamps (amplification Boost après le plafond dur
au lieu d'avant — condition #4 du gate, le chemin de contournement identifié
en gate 1).

### Fichiers modifiés
`src/temp_mode_logic.{h,cpp}` (nouveau), `src/pump_controller.{h,cpp}`,
`src/config.{h,cpp}`, `src/constants.h`, `src/main.cpp`,
`src/mqtt_manager.{h,cpp}`, `src/time_to_target.cpp`,
`src/web_routes_config.cpp`, `src/web_routes_control.cpp`,
`src/web_routes_data.cpp`, `src/ws_manager.cpp`, `data/index.html`,
`data/app.js`, `data/app.css`, `test/test_native_temp_mode/` (nouveau),
`test/test_native_ws_data_parity/test_ws_data_parity.cpp`.

### Correctifs post-production (2026-10-05, retour de production, juste après le déploiement 2.55.0)

Deux défauts d'affichage constatés sur `#dose-block-banner` (feature-138) juste
après un redémarrage OTA, sans rapport avec la logique de dosage elle-même —
corrigés dans `data/app.js` :

1. **Ton erroné sur la voie ORP pour le code de garde 250.** `_doseBlockInfo()`
   codait `tone: 'fault'` en dur pour `DOSE_BLOCK_CODE_GUARD`, sans tenir
   compte de la voie, alors que `DOSE_BLOCK_CODES[250]` porte déjà
   `toneOrp: 'normal'` (lu jusque-là uniquement par le rendu des pastilles).
   Résultat en production : un bandeau **rouge** d'alarme pour l'inhibition du
   chlore sur pH indisponible — bénigne et transitoire à chaque réamorçage du
   filtre. Corrigé : `_doseBlockInfo()` lit `toneOrp` quand `prefix === 'orp'`.
2. **Redondance de deux lignes décrivant la même cause.** « Mesures non
   représentatives (stabilisation) » (pH, codes {3,4,15,18,19,22}) et
   « Sécurité pH/ORP active » (ORP, code 250) sont la cause racine et sa
   conséquence, pas deux causes. Nouvelle règle de fusion, **strictement
   bornée** (`orp.code === 250` ET `ph.code` dans la liste blanche
   `{3,4,15,18,19,22}` uniquement — toute autre cause ORP sous 250 reste sur sa
   propre ligne), texte imposé mot à mot, ton `normal`, durée affichée = la
   plus ancienne des deux (cause racine, pas conséquence). Le bouton
   « Activer la chloration de secours » (B4) et le libellé L7 restent
   attachés à la ligne fusionnée.

**Leçon retenue** : une surface neuve (le bandeau feature-138) a réutilisé la
taxonomie `DOSE_BLOCK_CODES` existante mais n'en a repris que la moitié — le
ton générique, sans l'override par voie déjà présent dans la table pour le
code 250. Un test ne voit pas ce genre d'oubli ; un œil humain devant l'écran
l'attrape en une seconde. Voir
[`docs/features/page-dashboard.md`](../../docs/features/page-dashboard.md#bandeau--dosage-bloqué--feature-138-v2540)
pour le détail et la check-list de vigilance pour toute future consommation
de cette table.

### Correctif post-production (2026-10-05, v2.55.1, retour d'usage réel)

Demande de l'utilisateur après avoir vécu avec le widget quelques jours :
afficher en permanence le sous-bloc « Chloration de secours » (bouton
Activer compris) alourdissait inutilement le widget « Modes temporaires »
pour une action qui ne sert que dans une situation précise — et que le
bandeau « Dosage bloqué » (feature-138) propose déjà au bon moment (L7).

- **`#temp-mode-rescue` (séparateur `#temp-mode-rescue-sep` compris) est
  désormais masqué entièrement quand `rescue_until <= 0`** — le widget ne
  contient alors que le Boost. Il réapparaît quand le mode est **actif**,
  avec badge, échéance, cause de non-injection et bouton Arrêter.
- **Le bouton Activer est retiré du DOM** (`data/app.js`,
  `updateRescueCard()`) : l'activation ne se fait plus que depuis le
  bandeau de blocage (`_activateRescueChlorine()`, appelé directement par
  le bouton L7 — plus de simple défilement + focus) ou via l'interrupteur
  MQTT/HA.
- **Libellé L2 révisé** pour absorber l'avertissement que portait **L1**
  (qui n'est plus visible avant activation, le bouton du widget ayant
  disparu) : « Le dosage se fera à l'aveugle, sans contrôle de l'ORP,
  pendant 48 h au maximum. Volume plafonné à 1500 mL par jour. Le pH n'est
  jamais dosé dans ce mode. Il maintient une désinfection minimale — il ne
  rattrape pas une eau boueuse : seul le retrait mécanique de la boue le
  fait. Désactivez-le dès que les sondes sont de nouveau fiables. » L1 et
  L7 **inchangés** ; L1 reste affiché dans le bloc actif.

⚠️ **Condition posée par le gate `pool-chemistry` sur ce correctif, non
négociable** : le maintien du bloc à l'état **actif** (statut, échéance,
bouton Arrêter) n'est pas une question d'ergonomie — le bandeau qui propose
l'activation disparaît dès que le dosage reprend, ce que fait précisément
l'activation du secours. Un retrait total aurait laissé tourner jusqu'à
48 h de dosage aveugle sans statut ni interrupteur visibles depuis le
tableau de bord, et aurait rendu intenable la promesse de L2 (« désactivez-le
vous-même »). Le gate a également exigé que
[`docs/user/src/guide-regulation.html`](../../docs/user/src/guide-regulation.html)
dise explicitement que, hors blocage constaté par l'appareil, l'accès
permanent à l'équivalent de ce mode est le **mode Programmée** de la
régulation chlore (page Chlore) — sans quoi l'activation devient
introuvable entre deux épisodes de blocage — et que l'armement préventif du
secours n'a **pas** d'intérêt chimique (la régulation Automatique fait
mieux quand les sondes sont fiables) ; le cas réellement difficile — sonde
dérivée mais stable, donc jugée fiable — se diagnostique au **réactif DPD**
et se traite par le mode Programmée.

**Fichiers modifiés** : `data/app.js`, `data/index.html` (et leur CSS
inchangé). Aucun changement firmware. Voir
[`docs/features/page-dashboard.md`](../../docs/features/page-dashboard.md#sous-bloc-chloration-de-secours-feature-140-v2550--masquage-conditionnel-et-retrait-du-bouton-activer-en-v2551)
pour le détail à l'écran.

⚠️ **Capture d'écran à refaire** : `screenshots/03_Tableau_de_bord.jpg`
montre encore le widget avec le sous-bloc de secours visible en permanence
— à reprendre pour montrer le widget au repos (Boost seul).
