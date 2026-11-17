# bug-171 — Une pompe lancée en test échappe au blocage global, y compris pendant un étalonnage

## Métadonnées
- **ID** : bug-171
- **Slug** : une-pompe-en-mode-manuel-echappe-au-blocage-global
- **Type** : bug — 🔴 **sécurité chimique** : une pompe doseuse continue de tourner alors que
  toutes les conditions de sa suspension sont réunies
- **Statut** : livré — tests verts, revue OK (v2.91.0). En attente du `git mv` vers `done/` (fait par l'agent principal, puis `check_doc_links.sh`)
- **Priorité** : **haute** — ⚠️ **troue en partie feature-163**, livrée la veille
- **Version cible** : 2.91.0
- **Dernière mise à jour** : 2026-11-17
- **Spec(s) liée(s)** :
  `done/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md`
  (⭐ **même mécanisme** ; bug-170 a fermé l'entrée **non gardée**, ⛔ pas la propriété structurelle) ·
  `done/feature-163-refus-de-dosage-pendant-un-etalonnage.md` (c'est elle qui est trouée) ·
  ADR-0115 (une garde sur un état partagé vit au **point d'usage final**)

## Domaines touchés
- [ ] Capteurs
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP** — `/pumpN/on`
- [ ] MQTT · [ ] UART · [ ] OTA · [ ] Authentification · [ ] Historique
- [x] **Frontend (`data/`)** — ⚠️ coché le 2026-11-17 : la limite de 10 s vit dans `data/app.js`, et le texte de la carte promet un comportement que l'appareil n'assure pas (voir l'ajout ci-dessous)

## Agents requis
- [x] spec-reader
- [x] architect — ⚠️ il y a un **arbitrage réel** (voir « Options »)
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT**
- [x] **security-reviewer** — c'est lui qui l'a trouvé ; route HTTP + ordre des gardes
- [ ] ux-ui-designer — ⚠️ **à reconsidérer** (2026-11-17) : « aucune surface UI » n'est plus acquis, le texte de la carte « Test manuel des pompes » est en cause
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

`POST /pumpN/on` démarre une pompe doseuse pour un **test**. La route est **correctement gardée à
l'entrée** (authentification, session d'étalonnage, présence d'eau). ⛔ **Mais une fois la pompe
démarrée, plus rien ne l'arrête** tant qu'un `/pumpN/off` explicite n'est pas envoyé.

## Le mécanisme, vérifié dans le code

`setManualPump()` pose `manualMode[i] = (duty > 0)`. Or les **trois** blocs de suspension de
`updateDosingCore()` (`pump_controller.cpp:3750`, `:3781`, `:3858`) coupent une pompe ainsi :

```cpp
if (!manualMode[0]) applyPumpDuty(0, 0);
```

⇒ ⛔ **Une pompe en mode manuel n'est jamais coupée par ces blocs.** Et seuls
`setManualPump(..., 0)` et l'OTA remettent `manualMode` à `false`.

### Ce qui est interrompu aujourd'hui, et ce qui ne l'est pas

| Chemin | Interrompu par la boucle ? | Interrompu à l'entrée en étalonnage ? |
|---|---|---|
| Injection manuelle (`/ph\|orp/inject/start`) | ✅ oui | ✅ oui (`web_routes_calibration.cpp`) |
| **Test de pompe (`/pumpN/on`)** | ⛔ **NON** | ⛔ **NON** |
| Assistant de mesure du débit | ✅ (timeout de session) | — |

## 🔴 Scénario de défaillance, concret

1. L'utilisateur lance un **test de pompe** filtration en marche. ✅ Les gardes d'entrée passent.
2. La filtration s'arrête — **horaire programmé** ou coupure.
3. ⛔ **La pompe doseuse continue à pleine puissance**, sans circulation : le produit **stagne au
   point d'injection**.

**Variante, et c'est celle qui fait mal** :
2'. L'utilisateur **ouvre un étalonnage** (sonde dans le tampon).
3'. ⛔ La pompe de test **continue de tourner** — alors que feature-163 vient précisément de
    garantir *« aucune injection pendant un étalonnage »*.

⚠️ **La garantie de feature-163 est donc exacte pour tout ce qui DÉMARRE pendant la session, et
fausse pour ce qui tournait DÉJÀ.** C'est écrit nulle part.

## 🔴 AJOUT 2026-11-17 — LA LIMITE DE 10 SECONDES EST DANS LE NAVIGATEUR, PAS DANS L'APPAREIL

⚠️ **Relevé en vérifiant le vocabulaire de la fiche pour l'utilisateur** — ⛔ cette fiche ne le
disait pas, et c'est l'aggravation principale du sujet.

**Ce que l'interface PROMET** (`data/index.html:2905`, sous « Test manuel des pompes ») :

> *« Démarre la pompe à puissance maximale. **Arrêt automatique après 10 secondes.** »*

**Qui tient cette promesse** : `startPumpTest()` (`data/app.js:13460`) arme un
`setTimeout(…, PUMP_TEST_DURATION * 1000)` — `PUMP_TEST_DURATION = 10` (`:13440`) — qui envoie
`POST /pumpN/off`. ⇒ **C'est le NAVIGATEUR qui compte, et c'est lui qui envoie l'arrêt.**

**Ce que l'appareil fait de son côté** : `/pumpN/on` (`src/web_routes_control.cpp:561` et `:577`)
appelle `setManualPump(i, duty)` puis répond `OK`. ⛔ **Aucune échéance n'est posée, nulle part.**
Vérifié : aucun `millis()`, aucune borne de durée associée à `manualMode` dans
`pump_controller.cpp`.

### ⇒ Le vrai énoncé du défaut
La pompe doseuse tourne **à pleine puissance**, et **la seule chose qui l'arrête est un minuteur
dans le téléphone de l'utilisateur**. Il suffit que ce minuteur ne survive pas 10 secondes :

- le téléphone se met en veille (⚠️ les navigateurs mobiles **suspendent** les minuteurs des
  onglets en arrière-plan — c'est le cas NOMINAL, pas une panne) ;
- l'onglet est fermé, rechargé, ou l'utilisateur change de page ;
- le Wi-Fi décroche entre le démarrage et l'arrêt ;
- le navigateur plante.

⇒ **Aucun de ces cas n'est exotique**, et dans chacun la pompe injecte sans limite jusqu'à une
action humaine (bouton « ⏹ Arrêter », `data/index.html:2909`) ou un redémarrage de l'appareil.

⚠️ **L'appareil a l'air d'être à l'arrêt** : rien dans la régulation ne tourne, et les trois blocs
de suspension de `updateDosingCore()` épargnent `manualMode`. C'est la formulation déjà posée plus
bas — **« l'appareil se croit à l'arrêt alors qu'une pompe tourne »** — mais sa cause première est
ici, pas dans les blocs de suspension.

### Ce que cela impose à l'arbitrage
⭐ **Cela tranche en grande partie le choix entre A, B et C** : l'option **B** (borner
`/pumpN/on` dans le temps **côté appareil**) ne devient plus « la plus proche des patrons
existants », elle devient **la seule qui ferme le cas nominal du téléphone en veille**.
⛔ A et C laissent la promesse des 10 secondes reposer sur le navigateur.

⚠️ **ET LA PROMESSE DE L'INTERFACE DOIT DEVENIR VRAIE** : tant que l'appareil n'a pas sa propre
échéance, la phrase « Arrêt automatique après 10 secondes » est un engagement que le produit ne
tient pas. ⇒ Soit l'appareil la tient, soit le texte change. ⛔ Pas de troisième voie.

### Conséquence sur les agents requis
⚠️ `ux-ui-designer` était décoché (« aucune surface UI »). ⇒ **À reconsidérer** : si le texte de
la carte doit changer, ou si un état « pompe en marche » doit devenir visible ailleurs que sur le
bouton qui l'a lancée, une surface est touchée.

## ⚠️ Ce que ce bug n'est PAS
⛔ Ce n'est **pas** un défaut de garde d'entrée : `/pumpN/on` est bien gardée.
⛔ Ce n'est **pas** l'argument « présence d'eau » — ⚠️ l'utilisateur a rappelé, **à juste titre**,
que la présence d'eau n'est pas **mesurée** mais **présumée** (`ManagedFiltration` croit l'eau
présente dès que le relais est **commandé** ON ; `PoweredByFiltration` renvoie **toujours** `true`).
⇒ L'argument qui tient est : **l'appareil se croit à l'arrêt alors qu'une pompe tourne.**

## Options — ⚠️ à arbitrer, ⛔ pas à présumer

| | Option | Ce qu'on gagne | Ce qu'on paie |
|---|---|---|---|
| **A** | Retirer l'exemption `!manualMode[i]` des trois blocs | Simple, ferme tout | ⚠️ **L'assistant de mesure du débit EN DÉPEND** (`pump_controller.cpp:3860-3863`) — il serait cassé |
| **B** | **Borner `/pumpN/on` dans le temps** côté serveur + l'interrompre comme une injection manuelle | ⭐ Patron **déjà en place** pour `manualInject*` ; l'assistant garde son exemption | Un peu plus de code ; durée max à faire fixer par `pool-chemistry` |
| **C** | Interrompre le test **uniquement** à l'entrée en étalonnage | Minimal | ⛔ Laisse le cas « filtration arrêtée » ouvert |

⚠️ **`security-reviewer` recommande (B)** : *« la plus proche des patrons existants »*.

## 🔴 DÉCISION UTILISATEUR (2026-11-17) — le bouton de test N'EST PAS soumis au plafond journalier

`pool-chemistry` a émis une **recommandation non bloquante** : faire refuser `POST /pumpN/on`
lorsque le plafond journalier (2 000 ml) est atteint, au motif que c'est le dernier chemin de
surdosage par répétition (2 000 ml atteignables en ~27 min d'appuis sous
`kMaxRequestsPerMinute = 120`).

**Réponse de l'utilisateur, mot pour mot** :
> *« Un bouton pour tester le matériel doit toujours fonctionner. Le test matériel n'est fait que
> rarement et pas pour doser »*

### Ce que cela fige
⛔ **`POST /pumpN/on` ne reçoit AUCUNE garde de plafond journalier.** La recommandation est
**écartée**, en connaissance du chiffre (12,3 ml par appui).

**Le raisonnement, à ne pas réduire à « l'utilisateur a dit non »** : un **outil de diagnostic
matériel** ne doit pas dépendre de l'état **chimique** du bassin. Les deux grandeurs n'ont rien à
voir : on appuie sur ce bouton pour savoir **si la pompe tourne**, pas pour doser. ⚠️ Une garde
qui refuse ce bouton se déclencherait précisément le jour où quelque chose ne va pas — c'est-à-dire
le jour où l'on a besoin de vérifier le matériel.

⭐ C'est une application directe du principe produit du projet : **« les plafonds protègent, le
reste informe » — une garde qui bloque plus qu'elle ne protège est un défaut.** Ici le plafond
protégerait un risque chiffré à 12,3 ml par appui (ΔpH < 0,01, chlore +0,035 mg/L dans 40 m³) et
bloquerait un geste de maintenance rare. ⇒ Le rapport est mauvais.

⚠️ **Ce que cela n'excuse PAS** : le risque réel du test n'est pas le bassin, c'est le **bouchon
local sans circulation** (12,3 ml ≈ 6 mm dans un PVC 50) et surtout **l'enchaînement des deux
chimies** (acide pH<1 puis hypochlorite pH 13 stagnants au même point ⇒ dégagement de Cl₂).
⇒ **La condition 3 de `pool-chemistry` — une seule pompe en marche manuelle à la fois — reste
BLOQUANTE**, et c'est elle qui traite ce risque. ⛔ Ne pas confondre les deux sujets.

⇒ ⛔ Un futur lot qui ajouterait un plafond à `/pumpN/on` **contredirait une décision produit
explicite** : il faudrait la rouvrir, pas la contourner.

## 🔴 ARBITRAGE 2026-11-17 — la condition « exclusivité » était justifiée par un risque INEXISTANT

⚠️ **Relevé en relisant le code livré, pas par un test ni par une revue.**

`pool-chemistry` avait posé comme **condition bloquante n° 3** : *« une seule pompe en marche
manuelle à la fois »*, au motif que *« ce qui est dangereux n'est pas une chimie mais les deux
**à la suite** (acide pH<1 + hypochlorite pH 13 stagnants → Cl₂) »*.

**Objection soulevée, et confirmée par `pool-chemistry` lui-même** :
⛔ **L'exclusivité n'empêche que le SIMULTANÉ, alors que le geste redouté est SÉQUENTIEL.**
Appuyer sur « ▶ Pompe 1 » (10 s d'acide, arrêt par l'échéance) puis sur « ▶ Pompe 2 » (10 s
d'hypochlorite, même endroit, sans circulation) reste parfaitement réalisable — et c'est le geste
**naturel** d'une séance de maintenance. La première pompe était déjà arrêtée : l'exclusivité ne
change **rien**. ⇒ *« Comme garde anti-Cl₂, elle ne protège de rien. »*

### ⭐ Et le risque lui-même était sur-pondéré — chiffré, puis RETIRÉ
- 12,3 ml de chlore 36° (114 g Cl₂/L) = **1,40 g de chlore actif** = 0,0198 mol ⇒ **0,44 L de Cl₂**
  au maximum **théorique**.
- ⚠️ Mais la canalisation est **pleine d'eau** : solubilité du Cl₂ ≈ **7 g/L à 20 °C** (davantage à
  13,2 °C), et un PVC 50 contient **~2 L par mètre** ⇒ capacité de dissolution **~14 g**, soit
  **dix fois** la quantité émise. ⇒ **Rien ne dégaze.**
- Les deux bouchons ne se rencontrent pas : diffusion en eau stagnante sur 10 min ≈ **1 mm**
  (D ≈ 10⁻⁹ m²/s) contre les **≥ 30 cm** qui séparent réglementairement deux points d'injection.
- ⇒ Un dégagement gazeux n'existerait que dans une canalisation **vidangée** — cas où ni un délai
  ni l'exclusivité ne changeraient quoi que ce soit.

⛔ **Décision : aucun délai inter-chimies, aucun refus du second test, aucun avertissement.**
⭐ **Ce qui protège réellement du bouchon stagnant est l'ÉCHÉANCE + la COUPURE SUR PERTE D'EAU** —
c'est-à-dire le cœur de `bug-171`. L'exclusivité n'en a jamais été un morceau.

### Ce qui est conservé, et pourquoi
Le **code** d'exclusivité est **gardé** — il est bon, et conforme à la décision produit du jour
(il **coupe** l'autre pompe au lieu de **refuser** la nouvelle : le bouton de test marche
toujours). Ses justifications **vraies**, désormais écrites dans le code :
1. borne le coût instantané du chemin test à **une** pompe (12,3 ml) au lieu de deux ;
2. **un seul créneau manuel propriétaire** ⇒ une seule échéance armée, comptabilité non ambiguë —
   c'est l'hygiène d'état dont dépend tout le correctif ;
3. aligné sur le verrou « une pompe à la fois » déjà posé par l'assistant de débit
   (`409 assist_busy`).

⚠️ **LA LEÇON, qui vaut au-delà de ce lot** : le commentaire du code promettait une protection
contre le Cl₂ que la garde **n'apportait pas**. ⛔ Une garde dont le commentaire promet une
protection inexistante est **pire que pas de garde** : elle ferme le sujet dans l'esprit du
lecteur suivant. Le commentaire de `pump_controller.cpp` porte désormais la rature **et** le
calcul, pour que la justification fausse ne soit pas réintroduite.

### Information de terrain souhaitée (⛔ non bloquante — ne changerait que le commentaire)
- (a) distance entre les deux points d'injection ;
- (b) le circuit est-il **vidangé** pendant l'hivernage ? ⚠️ Si oui, à noter : tester une pompe sur
  un circuit vide n'a de toute façon aucun sens.

## 🔴 DÉCISION UTILISATEUR (2026-11-17, 2ᵉ) — le test est REFUSÉ pendant une autre marche manuelle

**Défaut bloquant relevé par `code-reviewer`** : l'exclusivité coupait la pompe de la victime
**sans refermer sa session**. Deux conséquences réelles :
- injection ORP de 200 s en cours → test pH → pompe ORP coupée à 20 s, `manualInjectOrp.active`
  reste vrai **180 s** (décompte qui ne court pas — ⚠️ piège récurrent du projet), puis crédit
  plancher de **100 ml pour ~10 ml réellement écoulés** ;
- 🔴 **assistant de mesure de débit en marche** → test sur l'autre pompe → marche **tronquée en
  silence** (simple `warning`) ⇒ l'utilisateur pèse et valide un **débit ml/min FAUX**, qui
  alimente ensuite le modèle de dosage. ⭐ **C'est le seul effet à conséquence chimique du lot.**

**Question posée à l'utilisateur** : refuser le test, ou le laisser démarrer en soldant proprement
la marche interrompue ?

**Réponse : REFUSER, en l'expliquant.**

### Ce que cela fige
`POST /pumpN/on` répond **`409`** quand `manualInjectPh.active`, `manualInjectOrp.active` ou
`sFlowAssist.active`. Message d'interface explicite nommant l'opération en cours.

⚠️ **ET LA FRONTIÈRE AVEC LA DÉCISION PRÉCÉDENTE, à ne pas confondre** — les deux décisions du
jour peuvent sembler se contredire, elles ne le font pas :

| Refus | Motif | Décision |
|---|---|---|
| plafond journalier atteint (2 000 ml) | **état CHIMIQUE du bassin** | ⛔ **ÉCARTÉ** — *« un bouton pour tester le matériel doit toujours fonctionner »* |
| une autre marche manuelle est en cours | **conflit d'USAGE de l'actionneur** | ✅ **RETENU** |

⇒ **La règle sous-jacente** : un outil de diagnostic matériel ne doit pas être bloqué par l'état
**chimique** du bassin — il peut l'être par le fait qu'**on est déjà en train de s'en servir**.
Le premier refus serait arbitraire et tomberait le jour où l'on a besoin de vérifier ; le second
est actionnable en une seconde (arrêter l'opération en cours) et **protège une mesure que
l'utilisateur est sur le point de valider**.

⭐ Patron **déjà en place** dans le projet : `409 assist_busy` (`web_routes_control.cpp:1049`)
refuse un second assistant de débit. Ce lot **étend un verrou existant**, il n'en invente pas.

## Comportement attendu
1. Une pompe lancée en test ⛔ **ne survit pas** à une perte de présence d'eau.
2. Elle ⛔ **ne survit pas** à l'ouverture d'une session d'étalonnage.
3. ⚠️ L'**assistant de mesure du débit** continue de fonctionner — ⛔ son exemption est **voulue**.
4. L'arrêt (`/pumpN/off`, `duty=0`) reste **toujours** possible, sans condition.
5. L'interruption est **journalisée**, ⛔ jamais muette.

## Éléments visibles
- [x] **Log** — interruption d'un test
- [ ] Endpoint HTTP (⚠️ comportement changé, ⛔ pas la route)
- [ ] UI · [ ] WS · [ ] MQTT

## Critères d'acceptation
- [x] Test lancé, puis **filtration arrêtée** ⇒ pompe **coupée**, interruption **journalisée**
- [x] Test lancé, puis **étalonnage ouvert** ⇒ pompe **coupée**
- [x] ⛔ L'assistant de mesure du débit **n'est pas cassé** (c'est le piège de l'option A)
- [x] `/pumpN/off` reste inconditionnel
- [x] **Mutation** : rétablir l'exemption doit **tuer** un test
- [x] `pio run` 0 warning · natifs sans régression


### ⭐ Les 4 mutations, et ce qu'elles ont tué (2026-11-17)

⚠️ **Le détail, pas seulement le compte** — une suite qui « tue 4 mutants » sans dire lesquels ne
prouve rien de vérifiable. Suite : `test/test_native_manual_pump_guard/test_manual_pump_guard.cpp`
(10 cas). Restauration vérifiée après **chaque** mutation (`cmp` avec la copie d'origine, puis
`git status` propre — ⚠️ un incident de restauration avait laissé 4 mutations dans l'arbre le
2026-11-16).

| Mutation | Test(s) mort(s) | Assertion |
|---|---|---|
| **M1** — `evaluateManualCut` rendu indifférent à `manualOn` | `test_c5_manual_off_never_cuts` (l. 49) | attendu `kNone` (0), obtenu 1 |
| **M2** — `>=` → `>` dans `isManualRunExpired` | ⭐ **trois** : `test_c4` (l. 39), `test_c8` (l. 79), `test_boundary_equal_is_expired` (l. 84) | la borne exacte `now == deadline` doit être expirée ; et le cas de **repliement** `now == 0x64` après `0xFFFFFF00` |
| **M3** — branche `kCalSession` retirée | **trois** : `test_c3` (l. 24), `test_c5b` (l. 58), `test_c6` (l. 66) | une session d'étalonnage ouverte doit couper |
| **M4** — priorité échéance avant eau perdue | `test_c6_priority` (l. 63) | attendu `kWaterLost`, obtenu `kDeadline` |

⭐ **M2 est la plus instructive** : elle tue **trois** tests dont celui du **repliement de `millis()`
à 24,8 j**. Ce projet a déjà payé ce défaut en production, et il n'avait été trouvé que par
mutation. Une borne `>` au lieu de `>=` ne se voit dans aucune relecture.

⚠️ **Mesures prises, ⛔ pas recopiées** : `ls -d test/test_native_* | wc -l` → **88 suites** ;
dernière ligne de `pio test -e native` → **2 592 cas, 0 échec**. `pio run -t clean` puis build
complet → **SUCCESS**, les 2 seuls warnings venant de la bibliothèque tierce `OneWire`
(pré-existants, non introduits par ce lot).


## ✅ PREUVE DE TERRAIN — 2026-11-17 12:26, sur l'appareil de l'utilisateur

**Geste réel** : appui sur « Pompe 1 » dans l'interface, puis **fermeture immédiate de l'onglet** —
c'est-à-dire le scénario exact que le minuteur du navigateur ne couvrait pas.

```
[2026-11-17 12:26:03] INFO : Test manuel pompe 1 activée (duty=255)
[2026-11-17 12:26:03] INFO : [Test] Pompe 1 démarrée en mode manuel (duty=255)
[2026-11-17 12:26:13] INFO : [Test] Pompe 1 arrêtée — échéance appareil atteinte
```

⭐ **10,0 s exactement, et c'est l'APPAREIL qui a coupé.** La preuve tient au **libellé** : le tick
journalise « échéance appareil atteinte » (`tickManualPumpGuard`), tandis qu'un arrêt venu du
navigateur passerait par `setManualPump(i, 0)` et journaliserait « Test manuel pompe 1
**désactivée** ». ⇒ Cette seconde ligne est **absente** : aucun `/pump1/off` n'est arrivé, le
navigateur était fermé. ⛔ Sans ce correctif, la pompe aurait tourné jusqu'au retour de
l'utilisateur.

**Contexte de l'essai** : v2.91.0 déployée le même jour à 11:21 (programme + interface, signés,
acceptés par un appareil qui filtre — voir `feature-151` D4) · filtration **en marche** ·
eau 13,4 °C · pH 7,20 · ORP 556 mV. Volume injecté ≈ **12 ml**, conforme au chiffrage de
`pool-chemistry` (ΔpH < 0,01).

⚠️ **Restent NON prouvés sur le terrain** (⛔ ne pas les présenter comme éprouvés) :
- la coupure sur **perte de présence d'eau** (exigerait d'arrêter la filtration pendant un test) ;
- la coupure sur **ouverture d'une session d'étalonnage** ;
- le **refus `409`** pendant une injection ou une mesure de débit ;
- l'affichage du refus dans l'interface.
⇒ Les quatre sont couverts par les tests natifs et les mutations, ⛔ pas par un essai réel.

## Hors périmètre
- Les routes `/pumpN/duty/:duty` — **supprimées**, c'est bug-170
- La nature **présomptive** de la présence d'eau — ⚠️ matériel, voir `kicad/README.md`

## Notes d'implémentation (v2.91.0)

**Décision retenue : option B** — échéance appareil sur toute marche manuelle (ADR-0129 : [`0129-toute-marche-manuelle-porte-une-echeance-fournie-par-l-appelant.md`](../../../docs/adr/0129-toute-marche-manuelle-porte-une-echeance-fournie-par-l-appelant.md)). Les options A (casse l'assistant de débit) et C (laisse le cas filtration / téléphone en veille) sont écartées.

**Réalisation**
- `setManualPump(i, duty, budgetMs = kManualPumpTestMaxOnMs)` arme `_manualDeadlineMs[i]` (`std::atomic<uint32_t>`) ; ordre échéance → `manualMode` → duty ; `duty == 0` inconditionnel.
- `tickManualPumpGuard()` (appelé depuis `updateDosingCore()` **avant** les sorties « capteurs non initialisés » / « filtration coupée », `waterPresence` hissée) ; verdict par la logique pure `evaluateManualCut()` (`src/manual_pump_guard_logic.h`), priorité eau perdue > étalonnage > échéance, fail-closed si pas d'échéance.
- `kManualPumpTestMaxOnMs = 10 s` (zéro grâce) ; `kManualRunGraceMs = 2 s` ajouté aux injections manuelles et à l'assistant de débit (filet, jamais premier arrêt).
- Purge de l'échéance par OTA et mode maintenance. Alertes MQTT `manual_pump_cut_water` / `manual_pump_cut_calibration`.
- `POST /pumpN/on` : `409 already_injecting` (`manualRunBusyReject`), 3 messages (mesure de débit, injection pH, injection ORP). UI : refus affichés dans `#pump_test_msg`, boutons sans pictogramme, décompte démarré après acceptation.

**Verdict `pool-chemistry`** : GO sous conditions (1 ordre des causes, 2 purge maintenance, 3 exclusivité, 6 grâce de 2 s) ; recommandation non bloquante de plafond journalier sur `/pumpN/on` **écartée par l'utilisateur** (*« un bouton pour tester le matériel doit toujours fonctionner »*) — frontière : le refus 409 porte sur un conflit d'usage, jamais sur l'état chimique. Justification « anti-Cl₂ » de l'exclusivité **retirée** après calcul (dissolution ~10×, diffusion ≈ 1 mm).

**Écarté** : plafond journalier sur `/pumpN/on` ; délai inter-chimies ; refus du second test ; réordonnancement de `loop()` pour supprimer le doublon d'alerte (connu, attendu, accepté).

**Mesures** : `pio run` SUCCESS, flash 52,3 % ; `pio test -e native` 88 suites / 2 592 cas / 0 échec ; **4 mutations tuées** (suite `test/test_native_manual_pump_guard/`, 10 cas : échéance, eau perdue, étalonnage, assistant de débit non tronqué, arrêt inconditionnel, priorité, fail-closed, repliement de `millis()`, borne égale) ; `./build_fs.sh` SUCCESS.

**Fichiers** : `src/constants.h`, `src/pump_controller.{h,cpp}`, `src/manual_pump_guard_logic.h`, `src/web_routes_control.cpp`, `src/version.h`, `data/{app.js,app.css,index.html}`, `test/test_native_manual_pump_guard/`, `docs/{API.md,MQTT.md,DESIGN_SYSTEM.md,features/page-settings.md,subsystems/pump-controller.md,user/src/guide-utilisation.html,adr/0129-*.md}`, `CHANGELOG.md`.
