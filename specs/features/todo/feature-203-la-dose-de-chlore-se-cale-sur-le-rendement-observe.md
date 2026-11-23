## Métadonnées
- **ID** : feature-203
- **Slug** : la-dose-de-chlore-se-cale-sur-le-rendement-observe
- **Type** : feature
- **Statut** : todo
- **Priorité** : moyenne
- **Version cible** : 2.99.0
- **Dernière mise à jour** : 2026-11-19
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
| ⇒ Écart | **×2,1** |

⚠️ **L'injection ne s'est pas arrêtée sur l'atteinte de la cible, mais sur le plafond horaire**
(09:59:55). Le plafond a fait son travail de garde-fou — ⛔ mais un garde-fou n'est pas un dosage
juste.

### ⭐ Pourquoi l'appareil ne peut pas faire mieux aujourd'hui — et c'est écrit dans son code
`src/pump_controller.cpp:5058-5059`, commentaire d'origine :
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
`PumpControllerClass::creditInjectedVolume()` (`src/pump_controller.cpp:3344`) — **tout** millilitre
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
  `src/pump_controller.cpp:5069`) dont le champ anticipation vaut `0` côté chlore. ⇒ **S'y
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
- `src/pump_controller.{h,cpp}` — cumul d'épisode sur `creditInjectedVolume()` (`:3344`), condition
  d'arrêt, accesseurs ; ⛔ **pas** `computePID()`
- `src/constants.h` — plage de plausibilité du rendement, seuil de manque au-delà duquel on
  n'applique rien, `static_assert` de bornes
- `src/config.cpp` — persistance NVS + empreinte de contexte
- `src/web_routes_data.cpp`, `src/ws_manager.cpp` — champs neufs, **même expression**
- `data/app.js`, `data/index.html`, `data/app.css`
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/subsystems/ws-manager.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Module pur `orp_yield_learn_logic.h`, compilable en natif
- [ ] A2 — Épisode ouvert / cumulé / fermé / **jeté** selon les conditions de propreté
- [ ] A3 — Médiane persistée, avec empreinte de contexte et CRC
- [ ] A4 — Budget appliqué **en arrêt seul**, ⛔ `computePID()` inchangé (diff à l'appui)
- [ ] A5 — Écrêtage de plausibilité du rendement appris
- [ ] B1 — Rendement, profondeur d'apprentissage et cause d'arrêt servis dans `/data`
- [ ] B2 — **Mêmes champs, même expression** dans la trame WS
- [ ] B3 — Affichage raccroché à la composition de dose de feature-092, lisible à 390 px
- [ ] B4 — ⭐ **Alerte de sous-dosage chronique** (voir Sécurité chimique, point 2)
- [ ] C1 — Doc : API, ws-manager, pump-controller, page-orp, guide de régulation, CHANGELOG, ADR
- [ ] D1 — Test de parité `/data` ↔ WS sur l'**expression**
- [ ] D2 — ⭐ **Mutation exigée** : faire que le budget puisse **prolonger** une injection doit
  **tuer** un test
- [ ] D3 — ⭐ **Mutation exigée** : retirer l'écrêtage de plausibilité doit **tuer** un test
- [ ] D4 — ⭐ **Mutation exigée** : accepter un épisode contaminé doit **tuer** un test
- [ ] D5 — Vecteurs natifs : apprentissage absent ⇒ sortie **identique** au comportement actuel
- [ ] D6 — Vecteur du 18/11 : 456 mL / +106 mV ⇒ rendement 0,23 mV/mL, budget 220 mL pour 51 mV
- [ ] E1 — Essai réel sur l'appareil de production, après que feature-202 a convergé

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
