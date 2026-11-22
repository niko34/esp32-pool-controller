## Métadonnées
- **ID** : feature-209
- **Slug** : securiser-les-injections-par-un-detecteur-de-debit
- **Type** : feature — ⚠️ **MIXTE logiciel + MATÉRIEL** (premier lot du projet dans ce cas)
- **Statut** : todo — **étude d'abord, ⛔ pas d'implémentation**
- **Priorité** : à définir (⚠️ voir « Ce qui la rend sérieuse » : deux incidents réels)
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : `done/feature-065-…` (retrait de l'écran UART — ⭐ **GPIO 16/17 libérés**) ·
  `done/bug-200-…` (un état qui court-circuite le dosage doit NOMMER sa cause) ·
  ADR-0140 (quand un gate renonce à bloquer, la contrepartie est la visibilité permanente)

## Demande de l'utilisateur — 2026-11-22, mot pour mot
> *« Je voudrais plus tard ajouter la possibilité de sécuriser les injections en utilisant un
> détecteur de débit. Sans toutefois rendre la marque de détecteur obligatoire, un modèle
> AstralPool 76780 pourrait être utilisé. Cela touchera autant au logiciel qu'au matériel puisqu'il
> faudra des connecteurs supplémentaires sur le PCB pour brancher le détecteur de débit. Il n'y a
> pas pour le moment d'agent électronicien, mais cela pourrait évoluer dans ce sens avec l'ajout
> d'un nouvel agent. »*

## Domaines touchés
- [x] **Régulation / dosage** → ⛔ **passage obligatoire par `pool-chemistry`**
- [x] **Capteurs** — une entrée tout-ou-rien nouvelle
- [x] **Configuration** (NVS) — présence, polarité, temporisations
- [x] **Web — routes HTTP / WebSocket** (état du débit, ⚠️ **parité**)
- [x] **Frontend (`data/`)** — état visible, et ce qui se dit quand le dosage est refusé
- [x] **MQTT / Home Assistant** — un état de sécurité se publie
- [x] 🔴 **MATÉRIEL (PCB)** — connecteurs supplémentaires. ⛔ `kicad/` est **gelé sauf demande
      explicite** : cette fiche **n'autorise AUCUNE modification** du schéma ni du routage. Elle
      prépare la décision.

## Agents requis
- [x] spec-reader · [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT** : c'est une **garde de dosage**, la première du
      projet fondée sur une **MESURE** et non sur une **déduction**.
- [x] ux-ui-designer · [x] web-ui-developer · [x] embedded-developer
- [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] 🔴 **AGENT ÉLECTRONICIEN — N'EXISTE PAS AUJOURD'HUI.** Voir la section dédiée : ⛔ **ce lot
      ne doit pas démarrer son volet matériel sans lui.**

---

## ⭐ Ce qui rend cette demande sérieuse : la garde actuelle DÉDUIT, elle ne MESURE pas

Règle de sécurité n°1 de `CLAUDE.md` : *« TOUJOURS vérifier la présence d'eau (**filtration
active**) avant toute injection. »*
⚠️ **« Filtration active » n'est pas « de l'eau passe devant le point d'injection ».** C'est une
**déduction** à partir de l'état d'un relais. ⇒ Elle est **aveugle** à tout ce qui se trouve entre
la pompe et l'injection : vanne fermée, dérivation isolée, crépine bouchée, amorçage perdu,
canalisation vidée.

🔴 **Et ce n'est pas théorique — ce projet l'a payé DEUX FOIS :**

| Date | Ce qui s'est passé | Ce que la garde a vu |
|---|---|---|
| **23/08/2026** | **1ᵉʳ dosage réel, bypass des sondes FERMÉ**. 765 / 802 mL comptés, mesures non représentatives. | « filtration active » ⇒ ⛔ **elle n'a pas vu le bypass** |
| **11/10/2026** | **Chlore injecté dans la dérivation ORP FERMÉE** : la sonde a trempé dans un hypochlorite ~**1000× concentré**, ORP 561 mV non représentatif. | idem ⇒ ⛔ **rien** |

⭐ **Dans les deux cas, un détecteur de débit aurait refusé l'injection.** Et dans les deux cas,
c'est l'humain qui a découvert le problème **après**, en lisant des mesures incohérentes.

⇒ **La valeur de ce lot n'est pas « une garde de plus » : c'est la première garde qui CONSTATE au
lieu de SUPPOSER.** C'est exactement le motif pour lequel le projet distingue déjà *« les plafonds
protègent, le reste informe »* — ⭐ ici, on a de quoi faire un vrai plafond.

---

## 🔴 LA CONTRAINTE QUI STRUCTURE TOUT : le détecteur est OPTIONNEL

L'utilisateur l'a posée en premier : ⛔ **ne pas rendre une marque obligatoire.** Et le projet a
déjà une règle plus large : ⛔ **aucune solution ne doit exiger un équipement ou une configuration
chez un client.**

⇒ **Trois populations d'appareils doivent coexister, et le logiciel est le MÊME :**
1. **PCB sans connecteur** (le parc actuel) — ⛔ la fonction n'existe pas, **rien ne change**.
2. **PCB avec connecteur, détecteur ABSENT** — la fonction existe mais est **désactivée**.
   🔴 ⛔ **Une entrée non câblée ne doit JAMAIS être lue comme « pas de débit »** : ce serait un
   appareil qui refuse de doser **à cause d'un fil qui n'existe pas**.
3. **PCB avec connecteur, détecteur CÂBLÉ** — la garde est **active et bloquante**.

⚠️ **Le piège à nommer tout de suite** : le projet connaît déjà le travers du **garde-fou
décoratif** (le « parachute anti-OOM en PSRAM », décoratif pendant des mois sans que personne ne
s'en aperçoive). ⇒ Une garde **désactivée par défaut** et **jamais vérifiée** serait le même
travers. **Elle doit dire en permanence si elle est armée ou non** — c'est la leçon d'ADR-0140 :
*quand un gate renonce à bloquer, la contrepartie est la **visibilité permanente***.

---

## Le matériel — ⛔ questions ouvertes, ⛔ aucune réponse donnée ici

### Le modèle cité par l'utilisateur
**AstralPool réf. 76780** — présenté par les revendeurs comme un **kit flow switch** (capteur de
débit **+ collier de prise en charge**) destiné aux électrolyseurs *Clear Connect EVO*, et dont la
fonction est de **couper la production de chlore quand le débit est insuffisant**. Prix constaté
~78 €.
⚠️ **Ce qui n'est PAS établi, et qu'⛔ il ne faut pas supposer** : le **seuil de déclenchement**
(L/min), la **nature du contact** (NO / NF), le **pouvoir de coupure**, la **tension admissible**,
l'**indice IP**, le **diamètre** et le **filetage** du collier.
⇒ **Ces valeurs se lisent dans la notice du fabricant, ⛔ pas dans une fiche produit de
revendeur.** C'est le premier travail du volet matériel.
⚠️ **Ne pas confondre** avec la réf. **54041R0001** (~169 €), un autre flow switch AstralPool,
destiné à un électrolyseur au sel et exigeant un collier 50 mm × 1/2″ et un mamelon 3/4″ → 1/2″.

### Ce que le PCB doit recevoir — ⛔ à faire trancher par l'électronicien
- Combien d'entrées ? **Une** (débit général) ou **deux** (une par dérivation, pH et ORP) ?
  ⚠️ L'incident du 11/10 était sur la **dérivation ORP seule** : une entrée unique ne l'aurait
  **pas** attrapé si la filtration générale coulait.
- **Isolation** : opto-isolation ? Protection contre les transitoires de la pompe ?
  ⚠️ Le projet a **déjà** un décalage de mesure attribué aux **courants de fuite de la pompe de
  filtration** (c'est la raison d'être de la correction pH de +1,20). ⇒ Une entrée tout-ou-nu
  mal isolée s'ajouterait à un environnement électrique **déjà sale**.
- **Alimentation** du capteur : fournie par la carte ou externe ?
- **Type de contact admis** : NO et NF **tous deux** acceptés, polarité **configurable** —
  ⭐ c'est **la** condition pour ne pas imposer une marque.
- **Borniers** : quel pas, quel repérage, quelle place sur la carte actuelle ?
- ⚠️ **Le `BOM.xlsx` est la nomenclature de référence** ; `BOM.csv` en est un **dérivé généré** —
  ⛔ ne jamais le corriger à la main.

### GPIO — ⭐ un fait déjà acquis
**feature-065 (v2.17.0)** a retiré le support écran UART et **libéré GPIO 16 et 17**, explicitement
comme prérequis WROVER-E/PSRAM. ⇒ **Il existe des broches disponibles** ; ⛔ reste à vérifier
qu'elles conviennent (interruption, pull-up interne, état au démarrage, conflit avec le bus LCD
inexistant ici).

---

## Le logiciel — ce qu'il faudra décider (⛔ pas décidé ici)

1. **Où la garde s'insère.** ⛔ Tout nouveau chemin vers `pump_controller` exige
   `pool-chemistry` **et** `code-reviewer` (règle de `CLAUDE.md`).
2. **Anti-rebond et temporisations.** Un flow switch à palette **claque** au démarrage de la pompe.
   ⇒ Délai d'établissement avant de croire le débit, délai de retombée avant de bloquer.
   ⚠️ Le projet a **déjà** un précédent calibré : le gate de `POST /tune-ph-offset` exige
   **10 minutes de filtration** pour que l'eau testée soit représentative.
3. **Que fait-on quand le débit tombe PENDANT une injection ?** Arrêt immédiat ? Et le volume déjà
   compté ?
4. **Comment l'utilisateur l'apprend.** ⭐ **Acquis de bug-200, à ne pas réapprendre** : *un état
   qui court-circuite le dosage doit NOMMER sa cause dans le canal de refus* (ADR-0137). ⇒ Un
   refus « pas de débit » doit être **lisible à l'écran**, ⛔ pas seulement dans un journal.
5. **MQTT / Home Assistant** : un état de sécurité se publie. ⚠️ Et le canal est **borné à
   ~46 octets utiles** (ADR-0139) : le verdict se place **en tête**.
6. **Parité `/data` ↔ WebSocket** : ⛔ **5 récidives documentées**. Toute clé neuve se sert **par
   appel à un accesseur unique**, ⛔ jamais par deux expressions.
7. **Auto-test** : ⭐ comment l'appareil prouve-t-il que la garde est **vivante** et pas
   décorative ? (Au minimum : le débit vu au moins une fois depuis le démarrage, et la date du
   dernier changement d'état.)

---

## 🔴 L'AGENT ÉLECTRONICIEN — à créer avant le volet matériel

Il n'existe pas. ⛔ **Et aucun agent actuel ne couvre ce domaine** : `embedded-developer` écrit du
firmware, il ⛔ ne juge pas une isolation galvanique ni un pouvoir de coupure.

**Ce qu'il devrait trancher** (et qui, sans lui, serait tranché par quelqu'un qui n'en sait rien) :
protection des entrées · isolation face aux transitoires de la pompe · pouvoir de coupure et
tension du contact · compatibilité NO/NF · choix et repérage des borniers · impact sur la BOM et
sur le routage · conformité et sécurité électrique.

⚠️ **Modèle à suivre, il existe déjà dans ce projet** : `pool-chemistry` est un agent **au verdict
CONTRAIGNANT** sur un domaine où une erreur a des conséquences physiques. ⭐ L'électronique est
dans le même cas. ⇒ L'agent électronicien devrait, lui aussi, rendre **GO / GO SOUS CONDITIONS /
NO GO**, et ⛔ ne pas pouvoir être contourné.

⚠️ **`kicad/` reste gelé** tant qu'il n'a pas rendu.

---

## Comportement attendu (esquisse — ⛔ à valider par les gates)
1. Détecteur **absent ou non déclaré** ⇒ ⛔ **comportement d'aujourd'hui, à l'identique**.
2. Détecteur **déclaré et câblé** ⇒ **aucune injection sans débit constaté**, pompes pH **et** ORP.
3. L'état du débit est **visible en permanence**, et le refus **nommé** à l'écran.
4. ⛔ **La garde ne se contourne pas** depuis l'interface — ⚠️ à la différence des plafonds
   journaliers, c'est une garde de **présence d'eau** : elle protège le matériel **et** le bassin.
5. ⚠️ **Question ouverte, à poser à `pool-chemistry`** : une injection **manuelle** peut-elle
   passer outre ? ⛔ Mon avis provisoire est **non** — injecter de l'acide dans une canalisation
   sans eau est destructeur quel que soit le mode.

## Contraintes techniques
- ⛔ **Aucune marque imposée** : polarité et seuil **configurables**, contact sec **NO ou NF**.
- ⛔ Aucune régression pour le parc **sans** détecteur.
- ⛔ `kicad/` gelé jusqu'au verdict de l'agent électronicien.
- Watchdog actif sur tout chemin neuf · ADR-0082 (le chemin de journalisation n'alloue pas).
- ⚠️ **390 px d'abord** pour tout élément visible. Français, sans jargon.

## Livrables — check-list
- [ ] **A0** — 🔴 créer l'**agent électronicien**, ou décider explicitement de s'en passer **en
      écrivant pourquoi**
- [ ] **A1** — relever les **vraies** caractéristiques du 76780 dans la notice constructeur, et
      en déduire une **famille** de détecteurs compatibles (⛔ pas une marque)
- [ ] **A2** — verdict `pool-chemistry` sur la garde : bloquante ? dans quels modes ? injection
      manuelle comprise ?
- [ ] **A3** — étude matérielle : entrées, isolation, borniers, impact BOM et routage
- [ ] **B1** — firmware : lecture, anti-rebond, intégration à `pump_controller`
- [ ] **B2** — configuration (présence, polarité, temporisations) + champs servis, ⚠️ **parité**
- [ ] **B3** — interface : état permanent + **cause nommée** du refus (ADR-0137)
- [ ] **B4** — MQTT / Home Assistant
- [ ] **D1** — vecteurs, dont ⭐ **« entrée non câblée ⇒ ⛔ JAMAIS lu comme absence de débit »**
- [ ] **D2** — ⚠️ **essai sur matériel réel** : ⛔ aucun vecteur ne prouve un contact qui claque
- [ ] **C1** — docs, `CHANGELOG`, guide utilisateur, et **ADR** sur la garde par mesure

## Sécurité chimique
- Dosage impliqué : 🔴 **OUI, directement** — cette fonction **refuse des injections**.
- Risque identifié : ⚠️ **le risque est SYMÉTRIQUE, et c'est ce qui rend le lot délicat.**
  - **Garde trop laxiste** ⇒ on injecte à sec : ⛔ c'est l'incident du 23/08 et celui du 11/10.
  - **Garde trop stricte, ou faussement déclenchée** ⇒ **le bassin n'est plus traité**. ⚠️ Le gate
    l'a déjà chiffré ailleurs : *un bassin privé d'acide 48 h part à pH 8,2-8,4, où la fraction de
    chlore actif tombe sous 25 % — **une panne sanitaire certaine***.
  ⇒ ⭐ **Un détecteur qui ment est pire que pas de détecteur.** C'est l'argument central à
  soumettre à `pool-chemistry`, et la raison pour laquelle l'auto-test (point 7) n'est pas un
  confort.

## Hors périmètre
- ⛔ Toute modification de `kicad/` avant le verdict de l'agent électronicien.
- ⛔ Le choix d'un fournisseur ou d'une référence **imposée**.
- La mesure de **débit chiffré** (L/min) : cette fiche ne vise qu'un **tout-ou-rien**. ⚠️ Un
  débitmètre proportionnel est un autre sujet, à ouvrir séparément s'il devient utile.
