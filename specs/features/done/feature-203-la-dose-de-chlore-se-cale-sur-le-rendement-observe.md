## Métadonnées
- **ID** : feature-203
- **Slug** : la-dose-de-chlore-se-cale-sur-le-rendement-observe
- **Type** : feature
- **Statut** : done — ⚠️ E1, E2 et H1 en attente d'observation terrain
- **Priorité** : moyenne
- **Version cible** : 3.3.0
- **Dernière mise à jour** : 2026-11-23
- **Spec(s) liée(s)** : ⛔ **[feature-202](../done/feature-202-la-pause-melange-orp-s-apprend-et-s-affiche.md) est un PRÉREQUIS DUR** (voir « Pourquoi 202 d'abord ») · [feature-085](../done/feature-085-dose-de-base-apprise-feedforward.md) (le patron d'apprentissage) · [feature-092](../done/feature-092-carte-debit-injection-ph-orp.md) (la composition de dose servie, à compléter)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP / WebSocket
- [x] Frontend (`data/`)
- [ ] MQTT / Home Assistant

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry *(⚠️ **deux passages** — P1 sur le DESIGN avant tout code, P2 sur le DIFF)*
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
L'appareil ne sait pas **ce que produit** un millilitre de chlore dans le bassin où il est
installé. Il dose donc à l'aveugle, et ne s'arrête que lorsqu'un **plafond** l'arrête.

Cette fiche lui fait **mesurer ce rendement** sur son propre bassin, et s'en servir pour **arrêter
plus tôt** quand la dose nécessaire est atteinte.

## Contexte
Demande de l'utilisateur du 2026-11-19, suite au diagnostic de l'injection du 2026-11-18.

### Le calcul, à partir des chiffres relevés sur l'appareil de production
| Grandeur | Valeur |
|---|---|
| Manque à combler | 479 mV mesurés, 530 de consigne → **51 mV** |
| Volume injecté | **456 mL** |
| Effet obtenu | montée jusqu'à **585 mV**, soit **+106 mV** |
| ⇒ **Rendement observé** | 106 ÷ 456 = **0,23 mV/mL** |
| ⇒ Dose qui aurait suffi | 51 ÷ 0,23 ≈ **220 mL** |
| ⇒ Budget RÉELLEMENT servi (marge de sécurité × 2) | 2 × 51 ÷ 0,2325 ≈ **439 mL** |
| ⇒ Écart | **×2,1** |

⚠️ **L'injection ne s'est pas arrêtée sur l'atteinte de la cible, mais sur le plafond horaire**
(09:59:55). Le plafond a fait son travail de garde-fou — ⛔ mais un garde-fou n'est pas un dosage
juste.

### ⭐ Pourquoi l'appareil ne peut pas faire mieux aujourd'hui — et c'est écrit dans son code
`src/pump_controller.cpp:5181-5182`, commentaire d'origine :
> « **Régulation P-PURE : aucun feedforward (ffServed = 0), Ki forcé à 0** depuis
> `applyRegulationSpeed()` — la demande VAUT donc le terme proportionnel borné. »

Vérifié par ailleurs : ⛔ **zéro** occurrence de terme intégral ou d'anticipation côté chlore dans
tout `src/`. La commande est **strictement** « je suis loin, donc je pompe fort ». Rien dans cette
chaîne ne relie un **volume** à un **effet**.

⇒ Côté pH, cette relation **existe déjà** (feature-085, dose de base apprise). ⛔ **Côté chlore,
elle n'a jamais existé.**

### ⛔ Pourquoi feature-202 d'abord — ce n'est pas un ordre de confort
Mesurer un rendement, c'est diviser un effet par un volume. **L'effet n'est complet qu'au bout de
la montée** — le 18/11, **88 minutes**.

Si l'appareil relevait l'effet au bout de sa pause actuelle de 20 minutes, il ne verrait
qu'environ **un quart** de la montée. Il en conclurait « 456 mL ne rapportent que ~26 mV », soit un
rendement **quatre fois trop pessimiste** — et injecterait ensuite **quatre fois trop**.

⇒ ⛔ **Un apprentissage du rendement bâti sur une fenêtre d'observation fausse est PIRE que pas
d'apprentissage du tout** : il transforme une erreur de mesure en surdosage systématique.
feature-202 livre la fenêtre. Sans elle, ⛔ **ne pas démarrer ce lot.**

## Comportement attendu

### 1. Observer un ÉPISODE, pas une impulsion
Un **épisode** s'ouvre à la première injection chlore après une période calme, cumule **tout** le
volume injecté, et se ferme quand la mesure cesse de monter (fenêtre apprise par feature-202).

⭐ **Le point de collecte du volume existe déjà et il est UNIQUE** :
`PumpControllerClass::creditInjectedVolume()` (`src/pump_controller.cpp:3444`) — **tout** millilitre
chlore y passe, quelle que soit l'origine (auto, manuel, programmé). ⇒ Y accrocher le cumul
d'épisode, ⛔ ne pas recompter ailleurs.

L'épisode retient : **mesure filtrée à l'ouverture**, **volume cumulé**, **mesure filtrée au
plateau**. Rendement = (plateau − ouverture) ÷ volume.

⚠️ **Épisode JETÉ** si, pendant la fenêtre : la filtration s'est arrêtée · une attente eau neuve
s'est déclenchée · la mesure est devenue non représentative · le bidon a été changé · le mode
maintenance s'est posé. ⇒ *Mieux vaut une journée manquante qu'une journée fausse.*

### 2. Apprendre — médiane, et un contexte
Médiane des épisodes propres, avec un **minimum** avant de servir (⚠️ à compter en **épisodes**,
pas en jours — le 18/11 n'a produit **qu'un seul** épisode ; même arbitrage qu'en feature-202).

⛔ **Remise à zéro de l'apprentissage** si le produit, sa concentration, le bidon ou la consigne
changent. Patron existant : l'**empreinte de contexte** de `PhIntegralSnapshot`
(`src/ph_persist_logic.h`).

### 3. Appliquer — ⭐ un PLAFOND DE VOLUME, surtout pas un régulateur de plus
> ⛔ **`computePID()` n'est pas touché. Aucun terme intégral, aucune anticipation, aucun nouveau
> chemin vers la pompe n'est créé.**

L'apprentissage produit **un budget de volume pour l'épisode** :

    budget = manque_en_mV ÷ rendement_appris

L'épisode **s'arrête** quand le volume cumulé atteint ce budget. C'est une **condition d'arrêt
supplémentaire**, empilée sur toutes les existantes.

⇒ ⭐ **Conséquence structurante, et c'est l'argument de sécurité central : ce lot ne peut
qu'arrêter PLUS TÔT.** Il n'allonge aucune injection, ne relève aucun plafond, n'augmente aucun
débit. Sans apprentissage disponible, ⛔ **le comportement est RIGOUREUSEMENT celui d'aujourd'hui**.

C'est le principe du dépôt appliqué tel quel : *les plafonds protègent, le reste informe.*

### 4. Afficher
- Le **rendement appris** et sur combien d'épisodes il repose.
- Quand le budget est **ce qui a arrêté** l'injection, le dire — au même titre que les autres
  causes d'arrêt déjà affichées.
- ⚠️ feature-092 sert déjà une **composition de dose** (`composeDose()`,
  `src/pump_controller.cpp:2569`) dont le champ anticipation vaut `0` côté chlore. ⇒ **S'y
  raccrocher**, ⛔ ne pas inventer une seconde surface d'observabilité à côté.

### Cas limites
- **Apprentissage absent ou insuffisant** ⇒ aucun budget, comportement d'aujourd'hui.
- **Manque important** (bassin très bas, sortie d'incident) ⇒ ⛔ **aucun budget appliqué** — voir la
  limite logarithmique en Sécurité chimique. Repli sur le comportement d'aujourd'hui.
- **Manque négatif** (mesure au-dessus de la consigne) ⇒ sans objet, aucune injection.
- **Redémarrage en cours d'épisode** ⇒ épisode **abandonné**, ⛔ jamais crédité à moitié.
- **Rendement appris aberrant** ⇒ écrêté à une plage plausible, ⛔ jamais appliqué tel quel.
- **Le bidon est changé pendant un épisode** ⇒ épisode jeté.

## Comportement actuel
La commande chlore est purement proportionnelle. L'appareil ignore l'effet de ce qu'il injecte, et
n'apprend rien d'une injection à la suivante. Seuls les plafonds l'arrêtent.

## Contraintes techniques
- **ADR-0017** : toute la décision dans un module **pur** (`src/orp_yield_learn_logic.h` attendu) —
  ⛔ pas de `millis()`, pas de `time()`, pas de NVS ; le temps est un **paramètre**.
- ⚠️ **Parité `/data` ↔ WebSocket sur l'EXPRESSION** — 4ᵉ récidive connue, la 4ᵉ forme était
  **invisible au grep**. Même accesseur des deux côtés.
- **ADR-0100** : ⛔ le navigateur ne recalcule **jamais** le rendement ni le budget — il **lit**.
- **ADR-0082** : aucune allocation sur le chemin de journalisation.
- ⚠️ **Usure NVS** : une écriture par épisode, ⛔ pas une par impulsion.
- ⚠️ **RESPONSIVE** : 390 px d'abord.

## Fichiers potentiellement concernés
- `src/orp_yield_learn_logic.h` *(neuf — logique pure)*
- `src/pump_controller.{h,cpp}` — cumul d'épisode sur `creditInjectedVolume()` (`:3444`), condition
  d'arrêt, accesseurs ; ⛔ **pas** `computePID()`
- `src/constants.h` — plage de plausibilité du rendement, seuil de manque au-delà duquel on
  n'applique rien, `static_assert` de bornes
- `src/config.cpp` — persistance NVS + empreinte de contexte
- `src/web_routes_data.cpp`, `src/ws_manager.cpp` — champs neufs, **même expression**
- `data/app.js`, `data/index.html`, `data/app.css`
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/subsystems/ws-manager.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Module pur `orp_yield_learn_logic.h`, compilable en natif
- [x] A2 — Épisode ouvert / cumulé / fermé / **jeté** selon les conditions de propreté
- [x] A3 — Médiane persistée, avec empreinte de contexte et CRC
- [x] A4 — Budget appliqué **en arrêt seul**, ⛔ `computePID()` inchangé (diff à l'appui)
- [x] A5 — Écrêtage de plausibilité du rendement appris
- [x] B1 — Rendement, profondeur d'apprentissage et cause d'arrêt servis dans `/data`
- [x] B2 — **Mêmes champs, même expression** dans la trame WS
- [x] B3 — Affichage raccroché à la composition de dose de feature-092, lisible à 390 px
- [x] B4 — ⭐ **Alerte de sous-dosage chronique** (voir Sécurité chimique, point 2)
- [x] C1 — Doc : API, ws-manager, pump-controller, page-orp, guide de régulation, CHANGELOG, ADR
- [x] D1 — Test de parité `/data` ↔ WS sur l'**expression**
- [x] D2 — ⭐ **Mutation exigée** : faire que le budget puisse **prolonger** une injection doit
  **tuer** un test
- [x] D3 — ⭐ **Mutation exigée** : retirer l'écrêtage de plausibilité doit **tuer** un test
- [x] D4 — ⭐ **Mutation exigée** : accepter un épisode contaminé doit **tuer** un test
- [x] D5 — Vecteurs natifs : apprentissage absent ⇒ sortie **identique** au comportement actuel
- [x] D6 — Vecteur du 18/11 : 456 mL / +106 mV ⇒ rendement **0,2325 mV/mL**, budget **439 mL** pour 51 mV
      ⚠️ **Corrigé le 2026-11-23** : les 220 mL écrits ici le 19/11 supposaient une marge de 1,
      **interdite depuis** par la condition C2 de `pool-chemistry` (marge ≥ 2, cf. ADR-0146).
      ⛔ Vecteur de TEST, jamais une valeur d'usine.
- [x] B5 — Coût de trame MESURÉ : **+142 o** ⇒ pire cas **3 806 o**, marge **290 o** sous
  `kWsFrameBufBytes = 4096`. ⭐ Mesure réelle sur l'appareil : `/data` en v3.2.0 = **2 704 o**,
  soit ≈ **2 846 o** avec les 5 champs — cas **nominal**, très en dessous du pire cas.
- [x] C2 — **ADR-0146** créé (plafond de volume plutôt qu'un régulateur)
- [x] C3 — `guide-regulation.html` **et** `guide-utilisation.html` à jour (sources seules, ⛔ PDF non régénérés)
- [x] D7 — MUTATION : budget au-delà de la médiane des manques ⇒ tue un test
- [x] D8 — Biais de comptage +6 % ⇒ budget 1,06×, du côté **généreux** (il s'annule au premier ordre :
  le même compteur sert à apprendre **et** à comparer)
- [x] D9 — ⭐ MUTATION : **figer** le budget à l'ouverture ⇒ tue un test *(le défaut le plus dangereux du lot)*
- [x] D10 — Désarmement après 3 arrêts consécutifs ⇒ tue un test
- [x] D11 — Épisode manuel/programmé : ne s'ouvre pas, et jette un épisode ouvert
- [x] D12 — Mesure NaN ⇒ **aucun** budget (⛔ pas un budget de 0)
- [x] D13 — Montée ≤ 0 ⇒ épisode jeté, ⛔ aucune division
- [x] D14 — Volume < plus petite dose délivrable ⇒ épisode jeté
- [ ] E1 — ⚠️ **EN ATTENTE D'OBSERVATION TERRAIN** — essai réel sur l'appareil, qui exige
  **trois épisodes propres** après déploiement. ⛔ **Non cochable dans ce cycle** : feature-202
  n'est elle-même pas encore déployée. ⛔ Ne pas cocher par commodité.
- [ ] E2 — ⚠️ **EN ATTENTE D'OBSERVATION TERRAIN** — l'alerte de sous-dosage ne se déclenche pas
  à tort pendant la phase d'apprentissage. ⭐ **Satisfait par construction en natif** (sous
  3 épisodes aucun budget n'est calculé, donc aucune séance ne peut être arrêtée par lui),
  reste à confirmer sur matériel.
- [ ] H1 — ⚠️ **ÉTAPE HUMAINE** — capture `screenshots/08_ORP.jpg` montrant « Effet du chlore »
  et « Quantité prévue ». ⭐ **Non bloquante** : la capture actuelle est *incomplète*, pas
  *fausse* — le guide ne décrit aucun rendu périmé. Dépend de E1.

## ADR attendu ?
**Oui.** Le trade-off à consigner : *pourquoi un plafond de volume plutôt qu'un terme intégral ou
une anticipation* — c'est-à-dire pourquoi on refuse d'ajouter un régulateur là où un plafond
suffit, et ce que cela coûte (l'appareil reste lent à combler un grand manque).

## Éléments visibles (pour doc-writer)
### Champs WebSocket
- Rendement appris, profondeur d'apprentissage, budget d'épisode, cause d'arrêt *(noms à arrêter)*
### Endpoints HTTP concernés
- `/data` — mêmes champs, **même expression**
### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/subsystems/pump-controller.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html` + `./tools/build_user_pdfs.sh`,
  `CHANGELOG.md`, ADR
- 📸 **Étape humaine** : capture de la page ORP montrant le rendement appris

## Sécurité chimique
- **Dosage impliqué** : **oui** — le lot décide d'arrêter une injection plus tôt
- **Limites concernées** : ⛔ **aucune n'est modifiée.** Plafond horaire, plafond journalier, nombre
  de cycles, présence d'eau, watchdog : **intacts**
- **Filtration requise** : sans objet
- **Calibration capteur modifiée** : non
- **Algorithme PID touché** : ⛔ **non** — `computePID()` est hors périmètre, et c'est **vérifiable
  au diff**
- **Sens de l'écart** : l'appareil ne peut qu'injecter **moins** qu'aujourd'hui

### ⚠️ Points à trancher par `pool-chemistry` AVANT tout code (passage P1)

1. ⭐ **La relation chlore ↔ millivolts est LOGARITHMIQUE, pas linéaire.** Les 0,23 mV/mL relevés
   valent **autour de 500 mV**, ⛔ pas partout. ⇒ Le budget doit être réservé aux **petites
   corrections courantes**. **Trancher le manque au-delà duquel on n'applique AUCUN budget** et où
   l'on retombe sur le comportement d'aujourd'hui. ⛔ Ne pas extrapoler un rattrapage de 200 mV
   depuis une pente mesurée à 50.

2. 🚨 **« Ne peut que réduire » est sûr pour UN épisode, ⛔ PAS pour une suite infinie d'épisodes.**
   Un rendement appris trop optimiste produit un budget trop petit **à chaque fois** : l'appareil
   s'arrête tôt, systématiquement, et le bassin reste **sous-chloré durablement**. ⚠️ C'est un
   **risque sanitaire réel**, et c'est le seul angle mort de l'argument de sécurité de ce lot.
   ⇒ Deux remèdes à arbitrer, ⛔ pas un seul : (a) l'**écrêtage de plausibilité** du rendement, et
   (b) une **alerte visible** quand le budget est ce qui arrête le dosage de façon répétée alors
   que la mesure reste sous la consigne. ⚠️ **(b) n'est pas optionnel** : sans lui, le défaut est
   **silencieux**, exactement le travers de bug-200.

3. **Le stabilisant déplace la relation.** Le rendement n'est **pas** une constante physique : il
   dérive avec la chimie de l'eau — l'épisode de surconsommation de **juillet 2026** était un
   stabilisant à 15 mg/L. ⇒ Arbitrer la **fenêtre d'oubli** : trop longue, elle retient une eau qui
   n'existe plus ; trop courte, elle n'apprend rien.

4. **Le comptage de volume de la pompe a un biais connu et NON constant** — relevé deux fois,
   **+5,11 %** (08/09) puis **+6,25 %** (08/10). ⇒ Le rendement appris sera légèrement
   **pessimiste**, donc le budget légèrement **généreux** : l'erreur va dans le sens prudent, ⚠️
   mais **l'écrire** plutôt que de la découvrir. Et ⛔ ne pas bâtir ce lot sur l'hypothèse d'un
   volume exact.

## Hors périmètre
- ⛔ Toute modification de `computePID()`, et tout ajout de terme intégral ou d'anticipation chlore
- ⛔ Toute modification des plafonds horaire, journalier ou de nombre de cycles
- ⛔ La voie **pH**, qui dispose déjà de son apprentissage de dose (feature-085)
- ⛔ Le compteur de stock de chlore (`orp_total_injected_ml` > `orp_container_ml` constaté le
  18/11 : le bidon n'a pas été remis à zéro au dernier remplissage) — défaut réel, **autre sujet**,
  ⚠️ **sans effet sur ce lot** : le volume par injection, lui, est juste

## Notes d'implémentation (2026-11-23, v3.3.0)

### La conception retenue, en une phrase
Après chaque séance **automatique** de chlore, l'appareil divise la montée observée par le volume
injecté. La médiane des **3** derniers épisodes propres lui donne un rendement, dont il tire un
**budget de volume** qui ne peut que **raccourcir** une injection.

### ⭐ Les six décisions structurantes
1. **L'épisode se ferme sur le VERDICT de l'observation de feature-202** (`kClosed`, plateau =
   `_orpObs.refOrp`). ⛔ Pas sur sa convergence, ⛔ pas sur une seconde détection.
   **Motif** : la mécanique de 202 avance par jalons **constants** jusqu'à 6×, elle voit donc le
   plateau réel **dès le premier épisode**. ⚠️ Un détecteur unique vaut mieux que deux détecteurs
   à synchroniser — et `isOrpMixingDelayLearned()` n'est **jamais** consulté.
2. **Marge de sécurité × 2** (`kOrpYieldBudgetMargin`). Le rendement appris est une **moyenne sur
   toute la montée**, or un manque courant petit signifie qu'on part **près de la consigne**, là où
   les millivolts sont les plus chers (rendement ∝ 1/C). Sans la marge, le budget serait trop petit
   près de la cible. ⭐ **C'est la SEULE valeur numérique posée à la main de tout le lot.**
3. **Le domaine d'application ne contient AUCUN nombre** : budget appliqué seulement si
   `manque ≤ médiane(manques déjà observés)`. ⇒ *ne jamais extrapoler hors du domaine mesuré.*
   Ça remplace le seuil chiffré `kOrpMaxError / 4` de la première conception, et ça se règle tout
   seul sur chaque installation.
4. ⭐ **Le budget est RECALCULÉ à chaque tick**, ⛔ jamais figé. Un budget figé sur un épisode qui
   ne se ferme pas aurait bloqué **la chloration automatique en permanence** — le pire scénario
   sanitaire du lot, trouvé **en conception**, pas en test.
5. ⭐ **L'épisode vit exactement aussi longtemps que l'observation qui le porte.** Il ne s'ouvre que
   sur une séance **automatique**, ⛔ ne peut pas se **ré-ouvrir** en cours de séance, et il est jeté
   avec elle pour **toutes** ses causes. ⚠️ **Conséquence assumée** : le chlore injecté à la main
   n'est **ni compté ni appris**, et l'apprentissage peut **ne jamais converger** sur un bassin qui
   réinjecte souvent ⇒ repli sur le comportement d'aujourd'hui, **qui est l'état sûr**.
6. ⭐ **Alerte ET désarmement.** « Ne peut que réduire » est sûr pour **un** épisode, ⛔ pas pour une
   suite infinie. Après 3 arrêts consécutifs sans atteindre la consigne, la condition « budget »
   **seule** cesse de s'appliquer. ⚠️ L'alerte sans le désarmement aurait laissé tourner l'état
   fautif — le travers exact de bug-200.

### Les défauts trouvés en revue, et corrigés
| Défaut | Trouvé par | Conséquence évitée |
|---|---|---|
| Épisode **ré-ouvert** en cours de séance avec un volume tronqué | `pool-chemistry` P2 | Rendement **1,4× trop haut**, budget 25 % trop petit ⇒ sous-chloration systématique |
| La pastille lisait l'**écrêtage du rendement** au lieu de la cause d'arrêt | `pool-chemistry` P2 | Libellé **faux** à l'écran : « quantité atteinte » affiché quand le rendement était jugé invraisemblable |
| Épisode ouvert par un geste **manuel** ne se fermant jamais | `pool-chemistry` P2-bis | Chloration automatique retenue **jusqu'à minuit** |
| « **Nouveau bidon** » ne remettait rien à zéro | `code-reviewer` | Rendement faux conservé après changement de produit (l'empreinte ne voit **ni produit ni concentration** — ces notions n'existent pas dans le code) |
| Comptabilité de trame **non cumulée** | `code-reviewer` | Marge annoncée 304 o au lieu de **290 o** ⇒ une trame > 4096 est **sautée en silence** |
| Accès DOM **non gardés** sur les lignes neuves | `code-reviewer` | Une `TypeError` aurait cassé **tout le rendu** de la page, pas seulement la ligne |

### ⚠️ Limites assumées, à ne pas redécouvrir
- **L'empreinte de contexte ne couvre ni la concentration du produit, ni la chimie de l'eau**
  (stabilisant, température, charge de baigneurs). Le rattrapage explicite est le bouton
  **« nouveau bidon »** ; le filet pour tout le reste est le **désarmement**, borné à 3 épisodes.
- **L'écrêtage de plausibilité est quasi inerte** : `kOrpYieldMaxMvPerMl` ≈ 52,7 mV/mL, soit
  ~226× le 0,23 observé. ⚠️ Le vrai remède au sous-dosage est le **désarmement**, ⛔ pas l'écrêtage.
- **`kOrpYieldLearnObsCount` porte trois sens** (taille d'anneau, seuil d'apprentissage, seuil
  d'alerte) : en changer un bougerait les trois. Documenté à sa déclaration.
- **Déclarer un nouveau bidon coûte 3 épisodes de réapprentissage même si c'est le même produit.**
  Volontaire : garder une valeur fausse ferait **sous-doser**.

### Chiffres mesurés (⛔ ne pas recopier, ils périment)
- `pio test -e native` : **99 suites, 2 799 cas, 0 échec** · suite neuve `test_native_orp_yield_learn`
  = **34 cas** · ⭐ **aucun mutant survivant** sur les 8 mutations exigées
- `pio run -e esp32dev_core3` : SUCCESS, 0 warning neuf · flash **53,0 %** · RAM **30,1 %**
- Trame WS : **+142 o** ⇒ pire cas **3 806 o**, marge **290 o** · mesure réelle `/data` v3.2.0 =
  **2 704 o** ⇒ ≈ **2 846 o** nominal
- Hygiène : `check_doc_links` **7 315 liens**, `check_specs`, `check_memory_discipline`,
  `check_fopen_guard` — tous verts
