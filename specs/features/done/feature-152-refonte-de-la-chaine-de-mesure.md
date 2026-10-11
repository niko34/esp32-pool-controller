## Métadonnées
- **ID** : feature-152
- **Slug** : refonte-de-la-chaine-de-mesure
- **Type** : refactor
- **Statut** : doing — implémentation livrée (embedded + web-ui + tests + revue), voir Notes techniques en fin de fichier ; pas encore clôturée (aucun commit référencé à ce stade)
- **Priorité** : haute
- **Version cible** : à définir (lot majeur)
- **Dernière mise à jour** : 2026-10-11
- **Spec(s) liée(s)** : feature-145 (question ouverte soumise au gate, **plus un prérequis** — décision utilisateur du 2026-10-11) · feature-137
  (perturbation matérielle, hors firmware) · bug-135 · feature-069 · ADR-0027, 0039, 0041, 0049,
  0059, 0062, 0074, 0092

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (champs publiés)
- [x] MQTT / Home Assistant (champs publiés)
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — OBLIGATOIRE et **à DEUX passages** : P1 sur la conception (c'est un
      **retrait de mécanismes de sécurité**), P2 sur le diff.
- [x] **ux-ui-designer** — REQUIS (règle d'accumulation, CLAUDE.md) : l'écran de mesure a reçu
      bien plus de trois ajouts successifs au fil des 18 specs empilées. Brief explicite :
      *évalue l'écran de mesure ENTIER*, pas seulement les retraits de ce lot — voir B6 (11
      identifiants `fm-*`).
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Réécrire la chaîne qui transforme une lecture de sonde en valeur utilisée par l'application.
Objectif : **le filtre lisse, il ne juge pas** — c'est la couche de dosage qui juge. Passer de
**14 étages et 13 états bloquants** (pH) à une chaîne de **5 étages** qui **ne peut pas se
verrouiller**.

---

## ⚠️ EXIGENCE DE GÉNÉRICITÉ — contrainte dominante, posée par l'utilisateur le 2026-10-11

> « Aucun des principes ne doit être spécifique à ma piscine. Ça doit être générique pour
> fonctionner dans toutes les piscines. »

**Aucune valeur de réglage ne doit être calée sur le bruit d'une installation.** C'est la cause
racine du défaut principal (voir « Contexte ») : la bande de cohérence vaut 0,20 alors que le
bruit du bassin de référence vaut 0,36 — un réglage choisi pour un signal qui n'est pas celui
qu'on mesure, donc **inopérant**, et personne ne pouvait le voir sans comparer deux nombres
situés à des milliers de lignes l'un de l'autre.

**Règle de conception opposable** : toute constante de ce lot doit être justifiable par
**l'une** de ces trois sources, et la spec doit dire laquelle :
1. **la physique** (plage pH 0-14, plage ORP d'une électrode, dynamique d'un bassin) ;
2. **une décision produit déjà prise** (tolérance de régulation, plage de consigne) ;
3. **le matériel** (cadence de lecture, résolution du module).

⛔ **Interdit** : une constante dont la valeur vient d'une mesure de bruit, d'une capture
terrain, ou d'un « ça marche bien chez nous ». Si un mécanisme ne peut pas être réglé sans
mesurer une installation particulière, **c'est le mécanisme qu'il faut retirer**, pas la
constante qu'il faut ajuster.

⚠️ Corollaire pour les tests : la fixture `test/fixtures/f097_purge_noise_20260920.csv` sert à
**vérifier** qu'une conception tient sur du signal réel. Elle ne doit **jamais** servir à
**choisir** une valeur. Tout chiffre dont la justification serait « mesuré sur la fixture » est
à rejeter en revue.

---

## Contexte

### L'état des lieux, chiffré (recensement du 2026-10-11)

| | pH | ORP | **Température** |
|---|---|---|---|
| Étages de transformation | **14** (+1 côté décision) | 13 | **5** |
| Lignes de validation | ~470 (`sensors.cpp`) + 406 (`sensor_filter`) | partagées | **~12** |
| Constantes `k*` propres | 11 | 9 | 5, dont **1 inerte** |
| États pouvant bloquer | **13** | 12 | **1** |
| Verrous **collants** | 2 | 2 | **0** |
| Effet sur le dosage | bloquant | bloquant | **aucun chemin** (ADR-0074) |

`sensor_filter.{h,cpp}` = **959 lignes** (406 de code) ; ses tests = **2 201 lignes**.
**60 constantes `k*`** règlent ce seul domaine. `SensorFilter::Config` a **13 champs**.

**La température n'a jamais posé de problème.** C'est le point de comparaison le plus utile du
dépôt : même appareil, même équipe, chaîne cinq fois plus simple, zéro incident.

### L'accumulation
**18 specs et 11 ADR** se sont empilés sur la chaîne pH/ORP. **Un seul retrait** dans tout
l'historique : feature-139 / ADR-0092 (analyse de stabilité de la porte), plus la suppression du
rejet `maxStep` par feature-058.

Chaque ajout répondait à un vrai problème. Mais chacun a ajouté **un état où la mesure est
déclarée inutilisable**. Il y en a treize.

### Le mécanisme du piège, à comprendre avant de concevoir
1. Le filtre est rendu **méfiant** pour écarter les fausses mesures (rejet de pic).
2. Méfiant, il **se verrouille** : la médiane ne bouge que si des échantillons entrent, donc dès
   que la grandeur vraie s'éloigne durablement, chaque échantillon suivant est rejeté à son tour
   et la médiane est figée. **Incident de production : dosage pH ET chlore à l'arrêt ≥ 3 h.**
3. On ajoute une **sortie de secours** (`rearm`).
4. La sortie de secours pouvant être dangereuse, on la met sous **trois gardes cumulatives**.
5. L'une de ces gardes est **calée sur un bruit qui n'est pas celui du terrain** ⇒ la sortie de
   secours **ne peut pas se déclencher**. Retour au point 2.

### Les incidents constatés, tous sur un usage NORMAL
| Date | Geste | Conséquence |
|---|---|---|
| (v2.20.3) | écart légitime de la grandeur | dosage pH **et** chlore à l'arrêt **≥ 3 h** |
| 2026-10-11 | sonde plongée dans une solution d'étalonnage | filtre verrouillé, **141 rejets**, médiane figée à 4,62 contre un brut à 8,46 |
| 2026-10-11 | (même épisode) | valeur affichée **retombée sur le brut**, oscillant visiblement |

⚠️ **Aucun cas n'a été identifié où ces mécanismes sophistiqués ont démontrablement évité un
dommage.** Ce constat doit être réexaminé par `pool-chemistry` — s'il trouve un contre-exemple,
il doit être inscrit ici.

### Un défaut d'affichage qui vient de la même racine
`ws_manager.cpp:856`, `web_routes_data.cpp:302`, `mqtt_manager.cpp:951` :
`phVal = !isnan(phFiltered) ? phFiltered : getPh()`.
⇒ **Pendant toute la montée en régime, l'écran montre la mesure instantanée pendant que le
dosage refuse sur une valeur absente.** L'utilisateur voit 8,3, l'appareil ne voit rien.
Constaté le 2026-10-11.

---

## Comportement attendu

### La ligne directrice, en une phrase
> **Le filtre lisse. Il ne juge pas. C'est la couche de dosage qui juge.**

Aujourd'hui **les deux** jugent, et c'est le filtre — qui n'en a pas besoin — qui se bloque. La
couche de dosage dispose déjà de **bornes** : butées de plausibilité et plafonds de dose. Une
borne ne se verrouille jamais : elle compare, elle ne délibère pas.

### La chaîne cible : 5 étages, les mêmes que la température

| # | Étage | Justification (source obligatoire) |
|---|---|---|
| 1 | **Lecture** du module | matériel |
| 2 | **Rejet de l'absurde** : NaN, hors plage physique | physique (0-14 pH ; plage d'électrode ORP) |
| 3 | **Lissage** : médiane glissante **puis** lissage exponentiel | **dynamique du bassin** — voir ci-dessous |
| 4 | **Offset utilisateur**, en sortie d'accesseur | décision produit (ADR-0039, **inchangé**) |
| 5 | **Fraîcheur** : pas de lecture récente ⇒ pas de valeur | matériel (cadence de lecture) |

### 🔑 LE POINT CENTRAL : la médiane EST le rejet d'aberrations
Une médiane de N écarte **par construction** jusqu'à (N−1)/2 valeurs extrêmes, **sans aucun
seuil**. Elle n'a besoin d'aucun réglage dépendant de l'installation, et **elle ne peut pas se
verrouiller** : un échelon légitime traverse la fenêtre en (N+1)/2 échantillons, quoi qu'il
arrive.

⇒ **Tout l'appareillage de rejet explicite disparaît** : `spikeReject`, `rearm`, les trois
gardes, le cooldown, les bornes de ré-amorçage, le témoin collant. Non pas parce qu'on accepte
le risque, mais **parce que la médiane fait déjà le travail, sans jugement et sans verrou**.

Il n'y a donc **rien à régler** au sens d'un seuil : il y a une **largeur de fenêtre** à choisir.

### ⚖️ LA LARGEUR DE LA MÉDIANE EST UNE DÉCISION OUVERTE — architect + pool-chemistry

⛔ **La spec ne fixe PAS ce nombre et ne le suggère pas.** Décision explicite de l'utilisateur
le 2026-10-11 : le principe de la médiane est retenu, **le dimensionnement appartient à
l'`architect` et à `pool-chemistry`**.

Ce qui se joue dans ce choix, et qui doit être arbitré **avec les deux conséquences sous les
yeux** — elles varient ensemble et en sens inverse :

| Largeur N | Aberrations consécutives écartées = (N−1)/2 | Échantillons pour suivre un changement réel = (N+1)/2 |
|---|---|---|
| 3 | 1 | 2 |
| 5 | 2 | 3 |
| 7 | 3 | 4 |
| 9 | 4 | 5 |

**Plus la fenêtre est large, plus elle encaisse de perturbations consécutives — et plus elle met
de temps à suivre un changement réel.** Il n'y a pas de réglage gratuit : c'est un arbitrage, et
il relève du gate chimique parce que les deux côtés ont un coût de sécurité.
- **Trop étroite** : une salve de perturbations est crue. ⚠️ Pertinent ici — feature-137 décrit
  une perturbation de mode commun qui produit des séries fausses **mais cohérentes**.
- **Trop large** : le dosage réagit tard à un vrai écart, et la montée en régime s'allonge.

⚠️ **La justification devra respecter l'exigence de généricité** : la largeur se déduit de la
dynamique du procédé et de la cadence retenue, **jamais** d'un bruit mesuré sur une installation.

### Comment encadrer la fenêtre SANS mesurer une installation
⛔ **Pas** d'après le bruit. ✅ **D'après la dynamique du procédé** :

Le pH d'un bassin ne peut pas changer significativement en quelques secondes. Entre l'injection
et l'homogénéisation il s'écoule des **minutes** — le temps de renouvellement d'un volume par
la filtration se compte en **heures**. Un retard de lissage de l'ordre de **la dizaine de
secondes** est donc **négligeable devant le procédé**, et ce raisonnement vaut pour **n'importe
quel bassin**, parce qu'il repose sur de l'hydraulique et de la diffusion, pas sur une
électrode.

⇒ **La contrainte s'énonce en temps** (« le retard total du lissage reste petit devant le temps
d'homogénéisation »), **pas en nombre d'échantillons**, et surtout pas en niveau de bruit.
L'`architect` convertit ce temps en fenêtre **à partir de la cadence de lecture**.

⚠️ **Et la règle doit être exprimée en ÉCHANTILLONS dans le code, pas en secondes dérivées de la
cadence.** Aujourd'hui plusieurs durées de sécurité sont écrites comme « 12 rejets ≈ 60 s à 5 s »
ou « buffer ~25 min à 5 s ». **Si quelqu'un change la cadence, toutes ces durées changent en
silence.** Ce qui se compte se compte ; ce qui se chronomètre se chronomètre en millisecondes.

### 🔑 LA CADENCE DE LECTURE N'EST PAS UNE ENTRÉE DE CONCEPTION

**La spec ne fixe PAS de fréquence de lecture, délibérément.** Règle posée par l'utilisateur le
2026-10-11 : *« la cadence ne doit jamais être liée au fonctionnement des mesures »*.

⛔ **Le défaut actuel, à ne pas reproduire** : plusieurs durées de sécurité sont écrites comme
dérivées de la cadence — « 12 rejets ≈ 60 s à 5 s » (`constants.h:1455`), « buffer glissant
~25 min à 5 s » (`sensors.h:272`), « 4 × kPhOrpSensorIntervalMs = 20 s » (`constants.h:1503`).
**Si quelqu'un change la cadence, toutes ces durées changent EN SILENCE.** Le ré-amorçage
tomberait à 24 s et la fenêtre de diagnostic à 10 min, sans que rien ne le signale.

**La règle, opposable :**
- **Ce qui se compte, se compte en ÉCHANTILLONS** (largeur de médiane, nombre de mesures avant
  qu'un échelon traverse). Ces grandeurs n'ont pas d'horloge.
- **Ce qui se chronomètre, se chronomètre en MILLISECONDES** (fraîcheur, délai de mélange, purge).
  Ces grandeurs ne comptent pas d'échantillons.
- ⛔ **Aucun commentaire ne doit convertir l'une en l'autre** en multipliant par la cadence.

⇒ **La conception doit être juste à N'IMPORTE QUELLE cadence** — c'est le livrable **D3**.

**Comment la fréquence se détermine alors** (travail de l'`architect`, source : **matériel**) :
- **Plancher matériel** : les modules Atlas EZO exigent `kEzoReadDelayMs = 900 ms` après la
  commande de lecture. Deux modules lus en séquence, plus le bus partagé avec le DS18B20 ⇒ on ne
  descend pas raisonnablement sous ~2 s pour le couple.
- **Lire plus vite est un gain des DEUX côtés**, pas un compromis : à retard identique, une
  cadence plus rapide permet une médiane **plus large** (donc plus robuste). Une médiane de 7 à
  2 s couvre 14 s ; la même à 5 s en couvre 35.
- ⚠️ **Limite à VÉRIFIER, pas à supposer** : les modules ont leur propre temps de réponse interne.
  Les interroger plus vite qu'ils ne produisent une mesure réellement nouvelle rendrait les
  échantillons **corrélés entre eux** — et une médiane sur des valeurs corrélées ne protège plus.
  ⇒ L'`architect` doit établir le temps de rafraîchissement réel **par la documentation du
  module**, et en déduire la cadence utile. C'est une donnée **matérielle**, donc une
  justification recevable au regard de l'exigence de généricité.

### Ce qui reste, et pourquoi
| Mécanisme | Pourquoi il reste |
|---|---|
| Rejet NaN / hors plage physique | gratuit, **jamais faux**, borne physique |
| Médiane + lissage | enlève le bruit, **ne juge pas** |
| **Détecteur de sonde figée** | attrape une sonde morte ; sortie **immédiate** dès une lecture hors bande ; aucun verrou |
| **Fraîcheur** | fait, pas jugement : pas de lecture récente ⇒ pas de valeur |
| **Porte** (filtration arrêtée / purge) | **simplifiée** : à la fermeture on vide la fenêtre, à l'ouverture on la remplit. ⛔ Plus de plancher de gel, plus de `rearm` |
| Butées de plausibilité, plafonds de dose | **couche de dosage**, ce sont des bornes |

### Ce qui disparaît
`spikeReject` · `rearm()` et ses 3 gardes · cooldown de 30 min · bornes de ré-amorçage ·
`rearmDenied` (collant) · calcul de dispersion · état `unstable` · **latch EMI** (collant, 12
fenêtres propres pour se lever) · plancher de gel de la porte · tolérance aux trous · grâce de
péremption post-`rearm` · l'alarme « filtre non prêt durablement ».

**Les deux verrous collants disparaissent. C'est l'objectif principal du lot.**

### 🧹 NETTOYAGE DU VOCABULAIRE — demande utilisateur du 2026-10-11

> « Prévois aussi dans la spec de nettoyer tous les messages liés à l'ancien fonctionnement
> (blocages, filtres, arrêts, etc.). »

⚠️ **Ce n'est pas un travail cosmétique de fin de lot.** Un message qui décrit un mécanisme
supprimé est **pire qu'inutile** : il fait chercher à l'utilisateur une cause qui n'existe plus.
Et il entretient, dans le code comme dans la tête, le vocabulaire d'un fonctionnement abandonné.

**Recensement initial (2026-10-11, à re-mesurer au moment du lot, ne pas recopier) :**

| Surface | Constat |
|---|---|
| `data/app.js` | 12+ occurrences de « filtre non prêt », « Capteur instable », « Stabilisation du filtre », « instable » |
| `data/index.html` | **24** occurrences liées au filtre (chip, fenêtre d'état, champs `fm-*`) |
| `data/app.js` | 13 occurrences des mêmes surfaces |
| `DOSE_BLOCK_CODES` | codes **3, 4, 5** décrivent des états de filtre (« Mesure indisponible — cause indéterminée », « Filtre de mesure non prêt — cause indéterminée », « Capteur instable (mesures rejetées) ») |
| Alertes MQTT | `ph_filter_rearm_denied`, `orp_filter_rearm_denied`, `ph_filter_not_ready`, `orp_filter_not_ready` |
| Topics MQTT | `_ph_filtered`, `_orp_filtered`, `_ph_filter_ready` et équivalents |
| Journal | « Filtre pH réinitialisé — warmup en cours », « filtre non prêt durablement », logs de latch |

**Ce que le lot doit faire :**
- **Supprimer** tout message, chip, badge, champ et topic décrivant un mécanisme retiré —
  pas les masquer, pas les renommer : les **retirer**, avec le code qui les produit.
- **Retirer les codes de blocage** devenus sans objet de `DOSE_BLOCK_CODES` **et** de leur source
  firmware. ⚠️ Un code retiré ne doit **jamais** être recyclé pour autre chose : un appareil non
  mis à jour l'afficherait avec l'ancien libellé.
- **Retirer les alertes MQTT** correspondantes, et les **auto-discovery Home Assistant** associés
  (sinon des entités fantômes subsistent chez l'utilisateur — ⚠️ vérifier ADR-0085, empreinte de
  découverte HA).
- **Ne PAS remplacer un message retiré par un message neuf** « pour expliquer ». Si le mécanisme
  disparaît, la question qu'il soulevait disparaît aussi. Ajouter une explication à la place
  serait refaire, en mots, la complexité qu'on retire en code.
- ⚠️ **Exception** : la montée en régime reste une réalité (la fenêtre doit se remplir). Elle,
  **doit** être dite — c'est la correction du repli silencieux. Un message, pas dix.

**Et la documentation suit** : `docs/subsystems/`, `docs/features/page-*.md`, `docs/MQTT.md`,
`docs/API.md`, et le guide utilisateur. ⚠️ Signaler **nommément** les captures d'écran à refaire.

**Critère de clôture** : un `grep` des termes du vocabulaire retiré (« instable », « filtre non
prêt », « rearm », « latch », « stabilisation du filtre »…) ne doit plus rien rendre dans
`data/`, dans les libellés de `src/`, ni dans la doc utilisateur — **hors** mentions historiques
explicitement datées dans les ADR.

### Une seule valeur publiée
Aujourd'hui : `phRaw`, `ph`, `phMedian`, `phFiltered`, plus `getPhFilteredRaw()` pour les gardes.
⇒ **UNE valeur officielle** pour l'affichage, l'historique, MQTT **et** le dosage. Les autres
deviennent du **diagnostic**, accessibles mais non concurrentes.

⛔ **Et le repli silencieux de l'affichage DISPARAÎT.** Pendant la montée en régime, l'écran ne
substitue plus une autre valeur : il montre la valeur **et dit qu'elle se met en place**.
Afficher un nombre différent de celui qui décide, sans le dire, est le défaut constaté le 11/10.

### Cas limites
- **Sonde sortie de l'eau puis replongée** (étalonnage) : la médiane traverse l'échelon en
  (N+1)/2 échantillons. **Aucun verrou, aucune action utilisateur.** C'est le cas qui a motivé
  cette spec.
- **Sonde débranchée puis rebranchée** : idem, plus la fraîcheur pendant l'absence.
- **Démarrage à froid** : fenêtre incomplète ⇒ pas de valeur officielle ⇒ dosage refusé
  (fail-closed inchangé), et **l'écran le dit**.
- **Porte qui se ferme en pleine fenêtre** : on vide. Pas de médiane héritée de l'eau stagnante.
- **Cadence de lecture modifiée** : le comportement en échantillons est inchangé ; les durées en
  millisecondes sont inchangées. **Rien ne bouge en silence.** À vérifier par test.

---

## ⚖️ LA VOIE ORP : QUESTION OUVERTE, SOUMISE AU GATE — ce n'est PLUS un prérequis

**Décision de l'utilisateur, 2026-10-11** : feature-145 (butée de plausibilité sur l'ORP brut)
**n'est plus un prérequis bloquant**. feature-152 est traitée seule. Son argument, qui est fort :

> *« Je me demande si les butées ORP sont pertinentes. »*

### Pourquoi cet argument tient — et il vient du terrain
**Pendant l'épisode de boue (octobre 2026), l'ORP du bassin est descendu à −46 mV. C'était une
mesure JUSTE**, établie par le gate `pool-chemistry` : chlore libre nul + matière organique en
décomposition ⇒ les espèces réductrices imposent le potentiel, et 0 à −200 mV est courant.

⇒ **Une butée de plausibilité aurait refusé une mesure VRAIE.** Une garde qui se déclenche sur
la vérité n'est pas une garde.

**Et l'ORP n'est pas le pH.** Une eau de piscine hors de 6-9 est une anomalie, et y doser de
l'acide est dangereux : la butée pH protège réellement. L'ORP, lui, parcourt légitimement une
plage énorme, et **ses valeurs extrêmes sont précisément celles où il faut agir**. Un ORP de
−50 signifie soit « sonde morte », soit « eau en très mauvais état » — et **rien dans la valeur
ne permet de trancher**.

**feature-145 n'a jamais été implémentée** : la retirer du chemin critique ne dégrade donc rien
par rapport à l'existant.

### ⚠️ CE QUE LE GATE DOIT TRANCHER EN P1 — question explicite
> **Que reste-t-il pour protéger la voie ORP après ce lot, et une butée est-elle nécessaire ?**

Ce qui reste : rejet NaN / hors plage **physique** d'une électrode · médiane (qui écarte
(N−1)/2 aberrations **par construction**, sans seuil) · détecteur de sonde figée · fraîcheur ·
porte simplifiée · **plafonds horaire et journalier** — qui sont des bornes de **DOSE**, pas de
mesure, et qui bornent le dommage sans jamais se tromper sur ce que l'eau raconte.

⚠️ **Le trou résiduel honnête** : la perturbation de mode commun (feature-137) produit des
séries fausses **mais cohérentes**, exactement ce qu'une médiane ne distingue pas. Le gate doit
dire si les plafonds de dose suffisent à borner ce cas.

**Si le gate conclut qu'une butée est nécessaire**, il doit dire **laquelle** et **comment la
dériver sans mesurer une installation** (plage d'électrode + plage de consigne, modèle
ADR-0048). ⛔ Le transitoire matinal 56-74 mV (feature-069) est un relevé du bassin de
l'utilisateur : **irrecevable comme source de dimensionnement**, utilisable seulement en
vérification. S'il conclut que non, feature-145 reste ouverte et non bloquante.

## ✅ VERDICT `pool-chemistry` P1 (CONCEPTION) — GO SOUS CONDITIONS, 2026-10-11

### Décisions chiffrées validées ou corrigées
| Point | Verdict |
|---|---|
| **Médiane 9** | ✅ **validée** — retard total ≈ 67 s **à 5 s de cadence**, soit **1/13 de `kPhMixingDelayMs`** (900 s) : invisible pour la régulation |
| **Cadence 3 s** | ⛔ **REFUSÉE** — voir C1, la cadence **reste à 5000 ms** |
| **`kOrpFilterMin/Max` ±1019,9 mV** | ✅ validé (plage de sortie EZO-ORP, source **matérielle**) — resserre le haut (1500 était inatteignable) et **garde −46 mV valide** |
| **`FrozenSamples`** | ✅ **SOURÇABLE ⇒ le mécanisme RESTE** (C2) — epsilon ½ LSB (matériel) ; borne haute de N = décision produit |
| **τ unique à la place des deux α** | ✅ « correction réelle, à valider telle quelle » |

### 🔑 LA VOIE ORP — TRANCHÉE, AUCUNE BUTÉE N'EST NÉCESSAIRE
Chiffrage du gate : chlore 36 °chl ≈ **114 g Cl₂/L** ; plafond journalier d'usine **500 mL** ⇒ **57 g**
⇒ **2,85 mg/L/j à 20 m³**, **5,7 mg/L/j à 10 m³**, **8,6 mg/L/j à 10 m³ avec Boost**.
⇒ **Excursion RATTRAPABLE** : irritation, sortie de baignade 24-48 h, **aucun dommage liner ni
équipement** ; l'hypochlorite ne fait pas chuter le pH.

> **Aucune butée ORP n'est nécessaire. feature-145 reste ouverte et NON BLOQUANTE.**

⚠️ **Mais le gate situe le vrai défaut ailleurs** : `SafetyLimits` est **explicitement exclu**
de la table de réglages par taille (`regulation_defaults.h:24`) — **un bassin Small reçoit le
plafond d'un Medium**. ⇒ **C7, spec distincte.**

### Mécanisme par mécanisme — ce qui n'est plus arrêté
| Retiré | Ce que ça expose | Suffisant ? |
|---|---|---|
| `spikeReject` | salve d'aberrations crue | ✅ médiane 9 écarte 4 consécutives, sans seuil |
| `maxDispersion` / `unstable` / latch EMI | bruit soutenu non inhibé | ✅ **le latch se posait sur le bruit NORMAL** (dispersion 0,402 relevée, cap 0,5) — **il n'inhibait pas un danger** |
| `rearm*`, `GateFloor`, `LatchClear`, `NotReadyAlarm` | — | ✅ ces états n'existent que pour sortir d'un verrou supprimé |
| `MaxConsecutiveRejects` | rafale NaN non vue | ✅ fraîcheur 20 s = fail-closed |
| **`FrozenSamples`** | sonde morte rendant une constante plausible, **dosée indéfiniment** | ⛔ **NON — c'est le trou si on le retire** ⇒ conservé (C2) |

**Contre-exemple recherché, AUCUN trouvé** : la seule candidate (prise d'air du 05/09/2026) ne
montre aucune trace d'un dosage nuisible évité par `dispersion`/latch. La phrase de la spec
reste, avec cette réserve datée.

### ⛔ LES SEPT CONDITIONS — bloquantes, re-vérifiées en P2
1. **CADENCE MAINTENUE À 5000 ms.** `sensors.update()` (1,8 s bloquants) et
   `PumpController.update()` sont dans **la même boucle** (`main.cpp:506/521`). Passer de 36 % à
   60 % d'occupation porte le dépassement moyen de fin d'impulsion de **0,32 s à 0,54 s**, soit
   **+3,2 % → +5,4 % de volume non compté** sur un pouls de 10 s — **du même ordre et du même
   signe que le biais de comptage ORP déjà relevé (+5,11 % puis +6,25 %)**.
   ⚠️ **Ce lot fait reposer TOUTE la sécurité ORP sur le plafond journalier : dégrader le
   comptage qui l'alimente est inacceptable.** Exception possible uniquement sur **preuve
   chiffrée AVANT** que la fin d'impulsion ne dépend pas de `loopTask`.
2. **Détecteur de sonde figée CONSERVÉ** : epsilon ½ LSB, **sans verrou**, sortie immédiate ;
   `static_assert(N × cadence ≤ kOrpMixingDelayMs)` — une **garde explicite**, pas une
   conversion silencieuse.
3. `kOrpFilterMin/Max = ±1019,9` avec **comparaison STRICTE**, pour que la valeur de
   **saturation du module** soit rejetée et non lissée. Zéro constante nouvelle.
4. **Trou de fraîcheur ⇒ VIDAGE de la fenêtre** (même primitive que la porte) : **jamais de
   médiane bâtie à cheval sur une absence**. Valeur officielle = NaN si fenêtre incomplète
   **ou** périmée.
5. **Nommer la valeur que comparent les butées de plausibilité pH.** Ce doit être **celle qui
   décide** (offset compris). À écrire dans la spec et à tester.
   ⚠️ Articuler avec la correction ci-dessous : `getPhFilteredRaw()` **ne disparaît pas**.
6. **Plafond journalier atteint J jours consécutifs (J = 2 ou 3) ⇒ `systemLogger.critical()` +
   alerte MQTT**, en réutilisant `phLimitReached`/`orpLimitReached` (déjà publiés).
   ⛔ **PAS de nouveau verrou — alerte seulement.** Motif : sous les deux hypothèses (sonde qui
   ment / eau réellement mauvaise), poursuivre le goutte-à-goutte est faux — **sous le
   breakpoint il fabrique des chloramines**. C'est la borne qui manquait, et **elle ne juge pas
   la mesure**.
7. **Inscrire le chiffrage du §ORP** et le constat que le plafond d'usine 500 mL/j **n'est pas
   présélectionné par la taille du bassin**. ⚠️ **Spec distincte**, et formuler la correction
   comme une **présélection de défaut**, jamais comme une entrée de calcul (invariant absolu
   feature-090).

### ⚠️ CORRECTION À LA CHECK-LIST A8 — `getPhFilteredRaw()` NE PEUT PAS ÊTRE SUPPRIMÉ
Relevé par l'`architect`, confirmé : `pump_controller.cpp:853` et `:1269` l'utilisent pour les
**butées de plausibilité pH**, qui travaillent **volontairement sur l'échelle BRUTE** « pour
rester valides si l'offset est FAUX » (`pump_controller.h:312-314`). Le supprimer ferait
dépendre **la seule butée du projet** de l'offset qu'elle est censée contourner.
⚠️ Ce n'est pas théorique : la sonde de l'installation de référence présente **+151,57 mV** de
décalage de zéro au 2026-10-11.
⇒ **CONSERVÉ**, renommé `getPhValueRaw()`, reclassé « valeur officielle, échelle brute ».

### Recommandations non bloquantes
Journaliser en DEBUG `τ`, `N`, cadence et `dt` réel (le α recalculé est juste mais
**invérifiable sans trace**) · **D2 doit inclure le détecteur figé** dans son espace d'entrées :
c'est désormais **le seul chemin restant vers « pas de valeur » durable** · retirer les
constantes mortes de `web_routes_debug.cpp:177/194` **en même temps** que le mécanisme.

### Tests de sécurité demandés par le gate
Sonde figée : 30 lectures au bit près ⇒ refus ; **une** lecture hors bande ⇒ reprise immédiate,
sans acquittement · **Rail ORP** : série à exactement +1019,9 ⇒ rejetée, pas lissée (mutation :
passer en `<=` doit tuer le test) · Trou de fraîcheur 5 min puis retour : NaN pendant l'absence,
**pas de médiane à cheval**, valeur juste en 5 échantillons · **Échelon pH 4 → 8,46** : traversée
en 5 échantillons, aucun refus persistant (**c'est l'incident du 11/10**) · Mode commun : 60
échantillons faux mais cohérents ⇒ dosage borné par le plafond **et** alerte J-jours levée ·
**Mutation C1** : porter la cadence à 3000 ms doit laisser **tous** les tests en échantillons
verts et **toutes** les durées ms inchangées.

## Contraintes techniques
- **ADR-0017** : la logique reste un **module pur** testable en natif, la coquille journalise.
- **ADR-0039** : l'offset reste appliqué **en sortie d'accesseur**, jamais à l'ingestion.
- **ADR-0072** : NaN signifie « pas de valeur », jamais une sentinelle.
- Zéro allocation sur le chemin de mesure (buffer circulaire fixe).
- `esp_task_wdt_reset()` sur toute boucle longue.
- Français dans toute l'UI.
- ⚠️ **Le dosage reste fail-closed** : pas de valeur ⇒ pas d'injection. Ce lot ne touche pas à
  cette règle.

## Fichiers potentiellement concernés
`src/sensor_filter.{h,cpp}` (réécriture) · `src/sensors.{h,cpp}` (la part validation) ·
`src/constants.h` (**retrait massif** attendu) · `src/pump_controller.cpp` (consommation des
valeurs) · `src/ws_manager.cpp`, `src/web_routes_data.cpp`, `src/mqtt_manager.cpp` (champs
publiés, **fin du repli silencieux**) · `src/history.cpp` · `data/app.js`, `data/index.html` ·
`test/test_native_sensor_filter/` (réécriture) · docs + ADR.

## Livrables — check-list
- [x] A1 — **Verdict `pool-chemistry` P1 sur la CONCEPTION, avant toute ligne de code.** Voir
      section « VERDICT `pool-chemistry` P1 (CONCEPTION) » de cette spec.
- [x] A2 — Module pur réécrit : 5 étages, **aucun état collant**, aucune constante calée sur une
      installation (`src/sensor_filter.h`, 959 → 393 lignes).
- [x] A3 — **Tableau de justification des constantes** : voir [ADR-0098](../../../docs/adr/0098-toute-constante-de-mesure-declare-sa-source.md).
- [x] A4 — Porte simplifiée (vider / remplir), sans plancher ni ré-amorçage (`SensorFilter::setContext()`).
- [x] A5 — Une seule valeur officielle publiée ; les autres en diagnostic (`ph`/`orp` = officielle,
      `phMedian`/`orpMedian`/`phRejectedCount`/`orpRejectedCount` diagnostic seul).
- [x] A6 — Codes de refus tranchés code par code : `4`/`5`/`19`-`22` retirés et **jamais recyclés**,
      `15`-`18` conservés et qualifient désormais la seule garde générique (code `3`).
- [x] A7 — `SensorFilter::Config` réduit à **7 champs** (contre 13) ; `DoseFilterState` aligné.
- [x] A8 — Accesseurs sans objet supprimés de `sensors.h` (`isPh/OrpFilterUnstable`,
      `getPh/OrpDispersion`/`CleanStreak`, `isPh/OrpFilterNotReadyAlarm`, `getPhFilteredRaw`) ;
      `getPh/OrpRejectedCount` conservés, reclassés diagnostic seul.
- [x] B1 — Fin du repli silencieux d'affichage : `/data`+WS servent `null` pendant la montée en
      régime ; l'écran distingue via `measures_valid` / code de refus `15`.
- [x] B2 — Chip + modal « État filtre » séparés retirés, fusionnés dans le modal sonde (3 champs
      diagnostic contre 11 identifiants `fm-*` avant ce lot) — bloc « Dispersion » de l'onglet
      Avancé remplacé par « État filtre pH/ORP ».
- [x] B4 — `history.cpp` : `point.ph`/`point.orp` portent désormais la valeur officielle
      (`getPhFiltered()`/`getOrpFiltered()`), commentaire `feature-152 (B4)` au point d'écriture.
- [x] B5 — Parité `/data` ↔ WebSocket vérifiée par lecture des deux fichiers
      (`web_routes_data.cpp` / `ws_manager.cpp`) : mêmes clés, même expression des deux côtés.
- [x] B6 — 11 identifiants `fm-*` réduits à **3 par voie** (`*-fm-raw`, `*-fm-filtered`,
      `*-fm-age`), absorbés dans le modal sonde existant.
- [x] D1 — Tests du module pur : `test_D1_*` dans `test/test_native_sensor_filter/`, échelon
      légitime en (N+1)/2 échantillons.
- [x] D2 — Test de non-verrouillage : `test_D2_composite_hostile_sequence_never_locks` et
      `test_D2_gate_closing_clears_and_refills_in_kN_not_a_lock`.
- [x] D3 — Test d'indépendance à la cadence : `test_D3_*` (5000 ms vs 3000 ms, même comportement
      en échantillons).
- [x] D4 — Validation par mutation effectuée manuellement (voir commentaire d'en-tête du fichier
      de test) — pratique conforme à CLAUDE.md, non recomptée ici (ADR-0073).
- [x] D5 — Vérification sur signal réel (fixture f097) présente dans la suite native.
- [x] C1 — [ADR-0097](../../../docs/adr/0097-le-filtre-lisse-le-dosage-juge.md).
- [x] C2 — ADR-0027 (`Superseded by ADR-0097`), ADR-0041 (`Partiellement superseded`), ADR-0049
      (amendé, six codes retirés) ; ADR-0059 déjà `Superseded by ADR-0092` (antérieur, inchangé) ;
      ADR-0062 non retouché (contre-exemple signalé par l'ADR-0098, suite distincte).
- [x] C3 — `docs/subsystems/{sensors,ws-manager,pump-controller,measure-validity}.md`, `docs/API.md`,
      `CHANGELOG.md` (entrée `[2.68.0]`) mis à jour.
- [x] C4 — `docs/user/src/guide-regulation.html` mis à jour (fenêtre du filtre : 65 s → 45 s ;
      les deux autres sens de « filtre »/« médiane » du même document, eux, **non touchés**,
      conformément au piège signalé ci-dessus).
- [~] C5 — `docs/user/src/guide-utilisation.html` mis à jour (bannière « Régulation à l'arrêt » et
      bouton « Réarmer le filtre » retirés de la FAQ, cause de panne corrigée). ⚠️
      `./tools/build_user_pdfs.sh` **volontairement pas lancé** dans ce tour (les PDF ne se
      génèrent qu'aux releases, via `/release` — règle CLAUDE.md) ; à lancer à la prochaine release.
- [x] C6 — Capture à refaire **signalée nommément** : `screenshots/10_Paramètres_8_Avancé_2.jpg`
      (carte « Signaux capteurs », bloc Dispersion → État filtre pH/ORP).
- [x] C7 — Auto-discovery HA : 4 entrées + 2 topics `*_filter_unstable`/`*_filter_ready` dépubliés
      en charge vide retained, documenté dans `docs/MQTT.md` (ADR-0085 cité).
- [x] C8 — Chiffrage avant/après consigné dans [ADR-0097](../../../docs/adr/0097-le-filtre-lisse-le-dosage-juge.md)
      et répété dans cette spec (section Notes techniques, fin de fichier).
- [x] B3 — Nettoyage du vocabulaire effectué (messages, chips, codes, alertes MQTT) — voir D6/D7.
- [x] D6 — Garde anti-retour : [`tools/check_removed_measurement_vocabulary.sh`](../../../tools/check_removed_measurement_vocabulary.sh),
      job CI `removed-measurement-vocabulary` (durci en revue — marqueur `vocab-ok` explicite,
      les anciennes exemptions par mot-clé laissaient passer les phrases qu'elles visaient à
      intercepter).
- [x] D7 — Périmètre précis respecté : `data/` en texte brut, `src/` limité aux littéraux `"..."`
      hors fichiers où `rearm`/`latch` désignent des mécanismes indépendants (liste close dans le
      script).

## Sécurité chimique
- Dosage impliqué : **oui, indirectement mais totalement** — ce lot change la valeur sur laquelle
  le dosage décide.
- **Risque n° 1 — une mesure fausse mais stable est crue.** Couverture : butées de plausibilité
  (pH : quatre ; **ORP : aucune — question ouverte soumise au gate P1, feature-145 n'est PLUS un
  prérequis bloquant**, voir section dédiée) + plafonds horaire/journalier.
- **Risque n° 2 — perturbation de mode commun** (feature-137) : elle produit des séries fausses
  **mais cohérentes**, exactement ce qu'une médiane ne distingue pas. Le gate doit dire si les
  butées suffisent. ⚠️ C'est un défaut **matériel** documenté ; le firmware ne peut que le borner.
- **Risque écarté** : le verrouillage du filtre, qui a causé deux arrêts de dosage.
- **Invariant conservé** : pas de valeur ⇒ pas d'injection.

## ADR attendu ?
- **Probable** : **oui**, et plusieurs critères.
- **Critères** : 1 (contrainte structurante), 2 (décision invisible), 3 (alternatives), 4
  (verrouillage long terme).
- **Sujets** : (a) « le filtre lisse, le dosage juge » ; (b) l'exigence de généricité des
  constantes ; (c) le retrait des verrous collants et ce qu'il expose.

## Critères d'acceptation
- [x] La chaîne pH compte **5 étages**, comme la température
- [x] **Aucun état collant** ne subsiste (2 → 0, confirmé par `test_D2_*`)
- [x] **Aucune constante** du lot n'est justifiée par une mesure d'installation — voir [ADR-0098](../../../docs/adr/0098-toute-constante-de-mesure-declare-sa-source.md)
- [x] Sonde plongée dans un étalon puis replongée : valeur correcte en (N+1)/2 échantillons,
      **sans intervention** (`test_D1_*`, `test/test_native_sensor_filter/`)
- [x] La valeur affichée et la valeur qui décide sont **la même** (`ph`/`orp` = valeur officielle,
      fin du repli silencieux — B1)
- [x] Nombre de constantes du domaine **en forte baisse** — mesuré : ~60 → 12 (`constants.h`,
      domaine de la chaîne de mesure)
- [x] `pio run` sans warning · `pio test -e native` zéro régression — **mesuré par le
      coordinateur après les corrections de revue** (ADR-0073 : compté, jamais recopié) :
      `pio run` SUCCESS (RAM 29,3 %, Flash 49,8 %), **67 suites / 2 098 cas**, zéro échec,
      **11 contrôles d'hygiène verts**. ⚠️ Le compteur est passé de 2 097 à 2 098 APRÈS la
      passe doc-writer : un cas a été ajouté en toute fin de cycle (axe (q) de la sentinelle
      de parité, voir ci-dessous).
- [x] Verdicts `pool-chemistry` **P1 et P2** au dossier — P1 en section dédiée de cette spec,
      P2 consigné ci-dessous (il manquait au dossier, relevé par doc-writer).

## Plan de test manuel
1. Sonde dans un étalon pH 7, puis pH 4, puis retour bassin : suivi correct, **aucun blocage**.
2. Sonde débranchée 5 min puis rebranchée : reprise sans intervention.
3. Filtration arrêtée puis relancée : pas de médiane héritée de l'eau stagnante.
4. Observer l'écran pendant la montée en régime : **aucune substitution silencieuse**.
5. Dosage réel : la régulation se comporte comme avant (le lissage reste équivalent).

## Notes pour agent
- ⚠️ **C'est un lot de RETRAIT.** Le réflexe sera d'ajouter un mécanisme pour compenser chaque
  suppression. **C'est exactement ce qu'il ne faut pas faire** — c'est ainsi qu'on est arrivé à
  14 étages.
- ⚠️ **Ne pas ajouter de détection « sonde hors de l'eau ».** Tentant, mais ce serait un
  mécanisme interprétatif de plus. Avec une médiane sans verrou, le cas se résout seul.
- Le lissage **n'est pas** l'ennemi : mesuré sur signal réel, le retirer multiplierait par ~2,5
  le ballottement de la valeur qui décide. **Ce qu'on retire, c'est le droit de bloquer.**
- ⚠️ L'offset pH **n'est pas** une suspicion de sonde : il corrige un décalage de **mesure**.
  Aucun libellé ne doit suggérer une sonde défaillante.
- La température est le **modèle**, pas un parent pauvre : 5 étages, zéro incident.

## Hors périmètre
- La perturbation matérielle (feature-137) — étude hors firmware.
- Les butées de plausibilité elles-mêmes (feature-145 pour l'ORP) — lot distinct, **non
  bloquant** : question posée au gate P1 de ce lot (voir section dédiée), pas un prérequis.
- La boucle de régulation P/I et le signal lent de décision de session (ADR-0062) — ils
  consomment la valeur, ils ne la fabriquent pas.
- La chaîne **température**, qui est le modèle à imiter et n'a pas à être touchée.

## Notes techniques (passe doc-writer, 2026-10-11)

**Chiffrage avant/après** (ADR-0097, répété ici — ADR-0073 : ne pas recopier ce chiffre dans un
futur lot sans le remesurer) :

| Grandeur | Avant | Après |
|---|---|---|
| `src/sensor_filter.{h,cpp}` | 959 lignes | 393 lignes |
| Étages de la chaîne | 14 | 5 |
| États bloquants | 13 | 4 |
| Verrous collants | 2 | 0 |
| `SensorFilter::Config` | 13 champs | 7 champs |
| Constantes du domaine mesure (`constants.h`) | ~60 | 12 |

**Fichiers livrés** (non exhaustif, voir diff complet) : `src/sensor_filter.{h,cpp}`,
`src/dosing_logic.{h,cpp}`, `src/dose_available_logic.h`, `src/ph_offset.h`,
`src/ph_feedforward_logic.h`, `src/sensors.{h,cpp}`, `src/pump_controller.{h,cpp}`,
`src/mqtt_manager.{h,cpp}`, `src/web_routes_{control,data,debug}.cpp`, `src/ws_manager.cpp`,
`src/history.cpp`, `src/constants.h`, `src/daily_limit_streak_logic.h` (nouveau),
`data/app.js`, `data/index.html`, `data/app.css` ; suites de test
`test/test_native_{sensor_filter,dosing,daily_limit_streak,ph_offset,ph_session,
history,log_guard_messages,temp_compensation,dose_banner_silence}/` ;
`tools/check_removed_measurement_vocabulary.sh` (nouveau) +
`.github/workflows/hygiene-specs.yml` (job `removed-measurement-vocabulary`).

**Documentation mise à jour dans ce lot** (agrégat des deux passes doc-writer) :
`docs/API.md`, `docs/MQTT.md`, `docs/BUILD.md`, `CHANGELOG.md` ([2.68.0]),
`docs/adr/{0097,0098}` (nouveaux), `docs/adr/{0027,0041,0049}` (superseded/amendés),
`docs/adr/README.md`, `docs/subsystems/{sensors,ws-manager,pump-controller,measure-validity}.md`,
`docs/features/{page-ph,page-orp,page-settings,page-dashboard}.md`,
`docs/user/src/{guide-regulation,guide-utilisation}.html`.

## ✅ VERDICT `pool-chemistry` P2 (IMPLÉMENTATION) — GO SOUS CONDITIONS, 2026-10-11

Consigné après coup : il avait été rendu, mais n'avait pas été écrit dans la spec — écart
relevé par `doc-writer`, qui a refusé de promouvoir le lot sans lui. **Les deux conditions
sont levées.**

- **C1 — l'escalade « limite journalière atteinte N jours de suite » devait être une logique
  PURE et testée**, pas un calcul noyé dans `pump_controller`. ⇒ Extraite dans
  `src/daily_limit_streak_logic.h` (`evaluateDailyLimitStreak`), suite
  `test/test_native_daily_limit_streak/` (7 cas). Le compteur n'est **délibérément pas
  persisté** : il repart à zéro à chaque redémarrage. Le plafond lui-même, lui, reste signalé
  **chaque jour** par `checkSafetyLimits()` — seule l'escalade peut manquer un jour. Choix
  documenté au point d'appel et dans `docs/MQTT.md`. Réserve non bloquante du relecteur :
  l'état est porté par des `static` locales (correct en `loopTask`, mais inaccessible à un
  futur reset ou test).
- **C2 — un commentaire de `pump_controller` affirmait une chose devenue fausse.** Il disait
  que `getPhValueRaw()` était « le SEUL accesseur qui porte la garde d'âge ». Faux depuis ce
  lot : `getPhFiltered()` appelle lui aussi `value(millis())`. Laissé en place, il aurait
  **justifié un futur changement dangereux** en laissant croire qu'on peut substituer l'un à
  l'autre. Corrigé, et remplacé par la vraie raison, qui n'a pas changé : `getPhValueRaw()`
  rend l'**échelle brute**, et les butées de dernier recours comparent du brut à des seuils
  bruts pour rester valides **quand l'offset est faux**. ⚠️ Le jumeau de ce commentaire, dans
  `pump_controller.h`, avait été oublié à la première passe — corrigé depuis (revue de code).

## ✅ VERDICT `code-reviewer` — Request changes, puis levé, 2026-10-11

Un bloquant fonctionnel, deux points de clôture, tous traités.

- **🔴 BLOQUANT — `measures_valid` surchargé.** Une version intermédiaire de B1 y fusionnait
  `!isnan(phFiltered) && !isnan(orpFiltered)` pour signaler le remplissage de la fenêtre.
  **Couplage collatéral**, démontré côté client : `measuresAreValid()` gate aussi la
  **TEMPÉRATURE** (`data/app.js:6186`, chaîne saine et explicitement hors périmètre) et la
  **voie OPPOSÉE** (`:7305`/`:7322`). Une sonde ORP figée aurait donc masqué le pH **et** la
  température, et `measureStateSegments()` aurait annoncé « Reprise automatique imminente »
  sur une **panne matérielle** — une cause fausse, exactement ce que cette spec proscrit.
  ⇒ **Annulé des deux côtés** (`ws_manager.cpp`, `web_routes_data.cpp`). Le remplissage de la
  fenêtre est **déjà porté, PAR VOIE**, par le code de refus 15 « Les mesures se mettent en
  place » — et comme la garde NaN est évaluée **avant** la garde de mode (`dosing_logic.cpp`),
  le bandeau parle même en régulation manuelle. La valeur, elle, tombe à « -- » parce que
  `ph`/`orp` sont sérialisés `null` quand ils sont NaN. **Aucun mécanisme neuf n'était requis.**
- **Garde D6/D7 non étanche.** `tools/check_removed_measurement_vocabulary.sh` exemptait toute
  ligne contenant `sans|aucun|feedforward|ff|firmware`. Le relecteur l'a prouvé par mutation :
  « Filtre non prêt : aucune mesure acceptée » et « Capteur instable (…) sans dosage »
  **passaient** (exit 0) — `sans` et `aucun` sont deux des négations les plus courantes du
  français. Pire, `POOL_VOCAB_LIST=1` rend **zéro** occurrence brute : ces exemptions
  n'exemptaient **rien**, c'était de la surface d'attaque pure. ⇒ Remplacées par un marqueur
  explicite **`vocab-ok`**, visible en revue de diff. **Les deux mutants du relecteur meurent
  désormais** (rejoué par le coordinateur), baseline verte.
- **Points mineurs, tous corrigés** : jumeau du commentaire C2 dans `pump_controller.h` ;
  gardes 3b/3c encore documentées comme vivantes ; `ph_offset.h` citant « latch EMI… dispersion
  restent inchangés » ; `kSensorFilterCentralWindow` (constante disparue) cité dans `app.js` ;
  `app.css` décrivant `ph/orpFilterAlarm` ; alignement de colonne dans `constants.h`.
  Identifiants renommés : `#trace_disp_*` → `#trace_filter_*`, `_armDispersionRefresh` →
  `_armFilterStateRefresh`.
- **Point ÉCARTÉ sciemment** : la perte de `role="status" aria-live="polite"` sur
  `#orp-cal-chip` devenu `<button>`. Motif consigné dans `data/index.html` : un `<button>` ne
  peut pas porter `role="status"` sans écraser son propre rôle ; son jumeau `#ph-probe-chip`
  est un bouton sans région live depuis feature-034 ; et un **invariant `ux-ui-designer`**
  déjà consigné impose **une seule zone vocalisée par carte** — c'est la pastille du titre,
  `#orp-dosing-pill`, que cette carte possède déjà.
- **Question produit LAISSÉE OUVERTE à l'utilisateur** : `POST /reset-sensor-filter` est
  devenue **doublement orpheline** — plus aucun appelant dans `data/` (le bouton « Réarmer le
  filtre » a disparu), et sa raison d'être (« le filtre reste verrouillé », incident
  2026-08-16) n'existe plus puisqu'il n'y a **plus aucun verrou**. Elle est **conservée en
  l'état et non rebranchée** : retirer une route publique sous auth administrateur change le
  contrat d'API et relève d'une décision produit, pas de ce lot. Motif complet consigné
  au-dessus du handler dans `src/web_routes_control.cpp`.

## 🔍 DÉFAUT TROUVÉ PAR MUTATION EN FIN DE CYCLE — la sentinelle de parité était aveugle

En vérifiant que la parité `/data` ↔ WS protégeait la correction du bloquant ci-dessus, le
coordinateur a constaté qu'elle ne protégeait **rien** : désaligner volontairement un seul côté
(`d["measures_valid"] = mv.measuresValid && !isnan(phFiltered)`) laissait
`test_native_ws_data_parity` **entièrement verte (16/16)**.

**Les seize cas comparaient des JEUX DE CLÉS, jamais des EXPRESSIONS.** Or `latestSensorData`
est **remplacé en entier** par la dernière charge reçue (aucune fusion) : deux expressions
divergentes pour une même clé font **osciller sa valeur** au rythme des deux cadences
(~5 s / ~30 s). C'est le **piège n° 1 du projet** (3 récidives documentées), en version
**invisible** — la clé étant présente des deux côtés, aucune charge prise isolément ne paraît
fausse, et aucun des axes (a)-(p) ne le voit.

⇒ **Axe (q) ajouté** : table étroite de clés « à expression partagée » (`measures_valid`, avec
motif écrit), membre droit extrait des deux fichiers, alias de locales normalisés
(`phFiltered`↔`phFil`, `orpFiltered`↔`orpFil`), égalité exigée. **Quatre mutations jouées** :
divergence côté WS seul → **tuée** ; divergence côté `/data` seul → **tuée** ; divergence
**symétrique** (donc délibérée) → **tolérée**, c'est le comportement voulu ; ancre de portée
cassée → **échec explicite**, le test se déclare aveugle au lieu de passer en silence.

**Clôture** : spec promue vers `done/` le 2026-10-11, après feu vert explicite de l'utilisateur
sur les trois étapes (commit, push, déploiement). ⚠️ La promotion a cassé **4 liens** dans
ADR-0097 et ADR-0098, qui pointaient encore vers `doing/` — **6ᵉ occurrence** de ce piège dans
ce dépôt, attrapée AVANT le commit grâce à l'ordre imposé `git mv` → `check_doc_links.sh` →
commit. Les deux ADR ont été corrigés, contrôle revert au vert (5 156 liens).

**Capture d'écran à refaire** (seule étape humaine signalée par ce lot) :
`screenshots/10_Paramètres_8_Avancé_2.jpg` — la carte « Signaux capteurs » de l'onglet Avancé a
perdu son bloc « Dispersion pH/ORP » (lignes + sous-graphes) au profit de deux lignes
« État filtre pH/ORP » plus sobres.
