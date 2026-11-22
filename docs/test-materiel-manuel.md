# Procédure de test du matériel — essais encadrés et traçables

> **Document frère** : [`test-on-target.md`](test-on-target.md) est la **check-list fonctionnelle**
> (quoi vérifier, quel log attendre). ⛔ **Cette page ne la réécrit pas.** Elle apporte ce qui lui
> manque : les **conditions de sécurité**, le **matériel requis par étape**, les **essais hors
> tension**, et surtout **comment consigner un résultat et une anomalie**.
> [`tests-automatises.md`](tests-automatises.md) dit ce qui se vérifie **sans** matériel — et ⛔ ce
> qu'un test logiciel ne prouve **jamais**.

---

## 0. 🔴 Sécurité et compétences — à lire avant toute mise sous tension

⛔ **Ce document ne contient AUCUNE procédure de câblage, et c'est délibéré** : le dépôt
n'en documente pas. Il fournit le **module** (ESP32-DevKitC-VE / WROVER-E, repère U2), les
**sources KiCad** et la **nomenclature** (`kicad/…/BOM.xlsx`) — ⛔ pas une marche à suivre
d'installation électrique. ⚠️ **En inventer une serait le défaut le plus grave que cette page
puisse commettre.**

**Conditions non négociables :**
- 🔴 **Toute intervention sur le secteur (230 V) ou sur le câblage de la pompe de filtration
  relève d'un électricien qualifié.** ⛔ Les étapes de ce document qui touchent à ces circuits
  **ne se font pas sans cette compétence**.
- 🔴 **Consignation avant toute intervention hors tension** : coupure **ET** condamnation du
  départ, **vérification d'absence de tension** avant de toucher quoi que ce soit.
- ⚠️ **Produits chimiques** : l'acide (pH−) et l'hypochlorite (chlore) exigent gants et lunettes.
  ⛔ **Ne jamais les mélanger**, même en traces, même dans un seau de rinçage.
- ⚠️ **Les pompes doseuses amorcent à sec sans le savoir.** Tant que les essais ne sont pas en
  eau, **déconnecter les tubulures des bidons** et les laisser dans un récipient d'eau claire.
- ⛔ **Ne jamais réinitialiser l'appareil de production** pour les besoins d'un essai.

---

## 1. Comment consigner — ⚠️ la partie qu'on saute, et qui fait perdre la mesure

⭐ **Un essai non consigné n'a pas eu lieu.** Ce projet en a la preuve : plusieurs diagnostics ont
échoué faute d'un relevé pris **avant** l'incident, ou d'un ELF perdu rendant un coredump
illisible.

**Fiche de relevé — à recopier et remplir pour chaque campagne :**

```
Campagne ..................  Date/heure (locale + UTC) ..................
Version firmware ...........  (interface → Système, ou src/version.h)
Appareil ...................  ⛔ production / banc d'essai  (rayer la mention inutile)
Opérateur ..................  Compétence secteur : oui / non
Conditions .................  (eau en circulation ? bidons connectés ? météo ?)

| # | Étape | Attendu | Observé | Verdict | Remarque |
|---|-------|---------|---------|---------|----------|
|   |       |         |         | OK / KO / N/A |    |
```

**Règles de consignation :**
- ⛔ **« OK » n'est jamais un verdict suffisant pour une mesure** : écrire **la valeur lue**.
  *« pH affiché 7,21 »* ⛔ pas *« pH correct »*.
- ⚠️ **N/A est un verdict à part entière**, et il doit porter **sa raison** (*« filtration à
  l'arrêt, étape non atteignable »*).
- ⭐ **Noter aussi ce qui a été observé sans être demandé.** Deux défauts réels de ce projet ont
  été trouvés comme ça.
- ⛔ **Ne jamais corriger un relevé a posteriori** : ajouter une ligne datée.

**En cas d'anomalie — ce qu'on joint, et c'est ce qui décide du diagnostic :**
| Pièce | Où la prendre | ⚠️ Pourquoi elle est périssable |
|---|---|---|
| **Journal persistant** | interface → `/download-logs` | il **tourne** : à prendre **tout de suite** |
| Journal volatil | `/get-logs` | quelques minutes seulement |
| **Coredump** | voir `docs/incidents/README.md` | 🔴 **écrasé au crash SUIVANT** — il n'existe **qu'une fois** |
| **L'ELF de la version qui tournait** | asset de la release, ou `builds/` | ⛔ sans lui, le coredump est **indécodable** (l'empreinte est vérifiée) |
| Capture d'écran | téléphone ou `./tools/ui_shot.sh` | — |

⇒ Un coredump analysé et sa trace décodée **se versionnent** dans `docs/incidents/` : ⭐ ce ne sont
pas des fichiers *générés*, ce sont des fichiers **relevés**. Rien ne les régénère.

---

## 2. Phase A — essais **HORS TENSION**

> 🔴 **Prérequis absolu** : alimentation coupée **et consignée**, absence de tension **vérifiée**.
> **Matériel** : multimètre, loupe ou binoculaire, le schéma (`screenshots/Schema.png` ou les
> sources KiCad), la nomenclature `BOM.xlsx`.

| # | Action | Résultat attendu | À consigner |
|---|---|---|---|
| A1 | Inspection visuelle de la carte | aucun composant déplacé, aucune soudure froide, aucun pont | photo si doute |
| A2 | Repérer les bornes contre le **schéma** | correspondance exacte repère ↔ bornier | la révision du schéma lue |
| A3 | Continuité des masses | continue | valeur mesurée |
| A4 | **Absence de court-circuit** entre l'alimentation et la masse | pas de court | valeur mesurée |
| A5 | Vérifier le **module** monté | ESP32-DevKitC-VE / **WROVER-E**, repère **U2** | référence lue sur le module |

⚠️ **A5 n'est pas une formalité** : un module **sans PSRAM** (WROOM-32) fonctionne, ⛔ mais la
**mise à jour en ligne depuis GitHub** devient non fiable (`BIGNUM - Memory allocation failed`).
⇒ Si le module n'a pas de PSRAM, **le noter** : plusieurs essais ultérieurs sont alors « N/A par
conception », ⛔ pas « en échec ».

⛔ **Ce que la phase A ⛔ NE prouve PAS** : qu'un relais commutera, qu'une pompe débitera, qu'une
sonde mesurera. Elle prouve seulement qu'on peut **mettre sous tension sans danger**.

---

## 3. Phase B — vérifications **fonctionnelles, hors eau**

> **Prérequis** : phase A soldée · appareil alimenté · ⛔ **tubulures des pompes DÉCONNECTÉES des
> bidons**, extrémités dans un récipient d'eau claire · réseau Wi-Fi joignable.
> **Matériel** : un poste avec navigateur, et — **si l'on veut le moniteur série** — un poste
> avec **port USB**.
> ⚠️ **Limite d'environnement connue** : le conteneur de développement **n'a pas de port série**
> (OTA seul, et **pas de mDNS** ⇒ utiliser **l'adresse IP**, ⛔ pas `poolcontroller.local`).
> ⇒ Prévoir une autre machine, ou s'en tenir au **journal dans l'interface**.

### B1 — Démarrage
| Prérequis | Action | Attendu | Consigner |
|---|---|---|---|
| appareil alimenté | observer le démarrage | pas de redémarrage cyclique ; **version firmware** = celle attendue | la version **lue**, ⛔ pas celle supposée |

⚠️ Un **redémarrage cyclique** arrête ici la campagne : ⛔ ne pas enchaîner, prendre le journal
persistant et le coredump s'il existe.

### B2 — Sondes et mesures *(sans eau : on vérifie la CHAÎNE, ⛔ pas la justesse)*
| Prérequis | Action | Attendu | Consigner |
|---|---|---|---|
| sondes connectées | lire le tableau de bord | pH / ORP / température **affichés** | les trois **valeurs** |
| — | débrancher brièvement une sonde EZO | l'appareil **signale** l'indisponibilité ; rebranchée, il revient | le libellé **exact** |
| 2 × DS18B20 | vérifier l'identification | les deux sondes reconnues (eau + circuit) **si applicable** | combien sont vues |

🔴 **Une valeur affichée hors eau ⛔ ne vaut RIEN comme mesure.** Une sonde **figée** rend une
valeur parfaitement plausible. ⇒ La justesse se vérifie **en phase C**, contre un test de
référence — ⛔ jamais ici.

### B3 — Commandes de relais
| Prérequis | Action | Attendu | Consigner |
|---|---|---|---|
| 🔴 **charge débranchée** si le relais pilote du secteur | commander la filtration depuis l'interface | le relais **commute** (clic audible / continuité du contact) | l'état lu **côté appareil** |
| — | commander l'éclairage | idem | idem |

⚠️ **Deux états à ne pas confondre, et c'est tout l'objet de cette étape** : ce que l'appareil
**affiche** (état **commandé**) et ce que le contact **fait**. ⛔ Le premier ne prouve pas le second
— ⭐ c'est exactement pour cela que cette vérification est **manuelle**.

### B4 — Pompes doseuses *(à sec, tubulures dans l'eau claire)*
| Prérequis | Action | Attendu | Consigner |
|---|---|---|---|
| ⛔ bidons déconnectés | déclencher une **injection manuelle** de faible volume | la pompe **tourne**, puis **s'arrête seule** | durée observée |
| — | observer les **refus** quand une garde s'y oppose | refus **affiché**, avec sa **cause nommée** | le libellé **exact** |

⚠️ **La garde de présence d'eau ne mesure aucun débit** : elle lit l'état **commandé** de la
filtration (ou le présume selon le mode d'installation). ⇒ ⛔ Un essai en B4 **ne valide pas** la
sécurité d'injection en conditions réelles.

### B5 — Communications avec l'interface
| Prérequis | Action | Attendu | Consigner |
|---|---|---|---|
| navigateur | charger l'interface | pages accessibles, mesures qui **se rafraîchissent** | — |
| — | couper puis rétablir le réseau | l'interface **signale** la perte, puis revient | comportement observé |
| MQTT configuré | observer un client abonné | états publiés | un exemple de message |

⇒ Pour le **détail fonctionnel** (modes de filtration, rollover de minuit, étalonnage,
persistance au redémarrage, OTA), ⛔ **ne pas réécrire ici** : dérouler
[`test-on-target.md`](test-on-target.md), et consigner dans la fiche de relevé ci-dessus.

---

## 4. Phase C — essais **nécessitant une installation en eau**

> 🔴 **Prérequis** : installation **en eau et en circulation**, phases A et B soldées,
> **compétence secteur** présente si l'on touche au circuit de la pompe.
> **Matériel** : trousse d'analyse (test au réactif), éprouvette, gants, lunettes.
> ⚠️ **Avant tout essai de dosage** : vérifier que **vannes et dérivations sont OUVERTES**.

| # | Prérequis | Action | Attendu | Consigner |
|---|---|---|---|---|
| C1 | filtration en marche ≥ 10 min | comparer la mesure de l'appareil à un **test au réactif** | écart **noté**, ⛔ pas jugé | les **deux** valeurs |
| C2 | C1 fait | laisser une régulation faire un cycle complet | démarrage **et** arrêt tracés, volume cohérent | durée et volume |
| C3 | dosage en cours | **couper la filtration** | injection **arrêtée**, refus **nommé** | le libellé exact |
| C4 | — | vérifier les **limites** horaire et journalière | plafonds **non dépassés** | compteurs lus |
| C5 | — | laisser passer **minuit** | compteurs journaliers remis à zéro | valeurs avant/après |

🔴 **C1 est l'étape la plus importante de toute la campagne, et la plus facile à rater.** ⚠️ Un
écart entre l'appareil et le test au réactif ⛔ **ne désigne PAS la sonde**. Ce projet a déjà
déclaré « à remplacer » une sonde **saine** : la cause était un décalage **de l'installation**
(courants de fuite de la pompe de filtration). ⇒ **Consigner l'écart**, ⛔ ne pas conclure.

⚠️ **Et un écart peut venir d'une eau non représentative** : si une dérivation est fermée, la sonde
baigne dans un produit concentré. ⭐ Deux incidents réels de ce projet ont cette cause — ⛔ et
l'appareil **ne peut pas** la détecter, faute de mesure de débit.

---

## 5. ⚠️ Informations manquantes — signalées, ⛔ pas inventées

1. ⛔ **Aucune procédure de câblage n'est documentée dans le dépôt.** Les valeurs de continuité,
   les sections de câble, les protections amont : **absents**. ⇒ Les étapes A2–A4 renvoient au
   **schéma**, ⛔ elles ne donnent pas de valeurs attendues.
2. ⛔ **Aucun seuil d'acceptation chiffré pour C1.** « Quel écart est acceptable » n'est écrit
   nulle part. ⇒ **Consigner**, et faire trancher.
3. ⛔ **Aucune procédure d'essai des entrées non encore utilisées** (bouton de réinitialisation
   d'usine, sorties réservées à de futures fonctions) : elles existent dans le code et sur la
   carte, ⛔ sans comportement documenté à vérifier.
4. ⚠️ **`test-on-target.md` annonce un nombre de tests natifs périmé** (« 118 »). ⇒ À corriger
   dans ce document, ⛔ pas ici.
5. ⛔ **Aucun relevé de référence** (« à quoi ressemble une campagne réussie ») : la première
   campagne menée avec cette page **devient** cette référence. ⇒ **La conserver.**
