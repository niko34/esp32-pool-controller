# Règles de conception des cartes — parasites et fiabilité

Ce document rassemble ce que l'étude du **2026-11-28** a établi sur les deux cartes du PCB v3
([carte principale](v3-principale/README.md), [carte de mesure](v3-mesure-std/README.md)) :
les règles à tenir, **pourquoi**, comment chacune est contrôlée, ce qui a été écarté, et ce qui
reste à mesurer sur le prototype.

Il existe pour une raison : qu'une modification future ne défasse pas, sans que personne le
voie, une protection qui a coûté une étude. **Toute modification d'une carte commence ici.**

L'objectif est un appareil de piscine fiable chez un particulier — pas un dispositif médical
ni spatial. Une règle sans gain réel pour cet usage n'a pas sa place ici.

## Mode d'emploi

| Quand | Quoi faire |
|---|---|
| Avant de modifier une carte | lire les règles communes et celles de la carte |
| Après la modification | `python3 scripts/verifier_regles_cartes.py v3-principale` (ou `v3-mesure-std`) |
| Avant de commander | `./scripts/build-jlcpcb.sh <carte>` : il lance ce contrôle et **s'arrête** si une règle tombe ; puis la [liste de contrôle](#liste-de-contrôle-avant-de-commander) |
| On ajoute une règle | l'écrire ici avec sa raison chiffrée, l'ajouter au vérificateur, ajouter **sa panne volontaire** dans `scripts/essai_verificateur_regles.py`, et lancer ce dernier |
| Une règle gêne | elle se discute **ici**, avec sa raison. ⛔ Elle ne se retire pas du vérificateur en silence |

Chaque règle porte un numéro : **C** commune, **P** carte principale, **M** carte de mesure.
« Calcul » = vérifiée par `scripts/verifier_regles_cartes.py`. « Relecture » = à contrôler
par une personne ou par l'agent `electronics-reviewer`.

⭐ **Le vérificateur est lui-même éprouvé** : `scripts/essai_verificateur_regles.py` casse
chaque règle exprès sur une copie de la carte (24 pannes) et vérifie qu'elle est vue.
Au 2026-11-28, les 24 sont vues — dont quatre que le contrôle de KiCad seul laisse passer.

## Règles communes aux deux cartes

| N° | Règle | Pourquoi | Contrôle |
|---|---|---|---|
| **C1** | Le contrôle des règles de KiCad rend 0 erreur, 0 liaison manquante, 0 écart entre schéma et carte | socle de tout le reste | calcul |
| **C2** | Le schéma du projet est exactement celui que produit `generer_schema.py` ; et la **carte de mesure**, celle que produit `generer_carte.py` | ce qui est retouché à la main serait écrasé à la prochaine génération, et la protection ajoutée disparaîtrait | calcul |
| **C3** | Les plans de cuivre internes sont **d'un seul tenant** : aucune piste dans une couche interne, aucune fente. L'empilement ne change pas : signaux / masse / alimentation / signaux | le courant de retour d'un signal passe dans le plan, juste sous sa piste. Une fente l'oblige à faire le tour : la boucle ainsi créée rayonne et capte | calcul |
| **C4** | Aucune piste de signal hors de son plan, donc aucune à moins de 0,5 mm du bord. **Toute piste nouvelle se trace à 1 mm au moins du bord** | les plans s'arrêtent à 0,5 mm du bord ; une piste au-delà n'a plus de retour sous elle. La carte principale en avait une de 50 mm à 0,3 mm du bord | calcul |
| **C5** | Le cuivre de masse des deux faces est **cousu** au plan interne : aucun point à plus de 15 mm d'une liaison (12 mm visés) | une plage tenue par un seul point est une antenne. 15 mm est le quart de la longueur d'onde du Wi-Fi dans la carte (≈ 60 mm) : au-delà, la plage peut s'accorder dessus. La carte principale avait une plage de 5 cm² tenue par une seule broche, à 49 mm de sa liaison | calcul |
| **C6** | Tout condensateur du 12 V porte sa tension de service dans sa valeur : 25 V au moins, 50 V de préférence | l'écrêteur d'entrée laisse monter le 12 V à 21,5 V ; un condensateur céramique perd en outre une grande part de sa capacité près de sa tension limite | calcul |
| **C7** | Aucun fil qui **sort du boîtier** ne va en direct sur une puce ni sur le 3,3 V : une résistance en série, puis une protection | un câble de plusieurs mètres ramène les décharges et les parasites ; un court-circuit du câble ne doit pas couper le 3,3 V de l'ESP32 | calcul |
| **C8** | Un condensateur de 100 nF au ras de chaque broche d'alimentation, son passage vers le plan au ras du condensateur | boucle de courant la plus petite possible | relecture |
| **C9** | Composants courants, plusieurs fabricants sur la même empreinte, la qualité restant un critère majeur (règle D8 de feature-215) | approvisionnement | relecture |

## Carte principale

| N° | Règle | Pourquoi | Contrôle |
|---|---|---|---|
| **P1** | **Deux interrupteurs en série par pompe** : un commun en haut (Q11), commandé par `ARME_POMPES` (IO4), et celui de chaque pompe en bas. Au repos, la commande est rappelée à la masse : rail coupé. Chaque sortie de pompe est **relue** par l'ESP32 (`LU_POMPE_PH` sur IO18, `LU_POMPE_ORP` sur IO23) | un transistor qui tombe en panne reste en général **passant** : la pompe dosait alors sans arrêt, et le logiciel ne pouvait ni le voir ni l'arrêter. Avec deux interrupteurs, il faut deux pannes ; avec la relecture, la première se voit | calcul |
| **P2** | Rien ne garde d'énergie en aval de l'interrupteur commun : **un 100 nF au pied de chaque diode de roue libre** (C21, C23), rien de plus gros. Les 10 µF (C3, C4) restent sur le 12 V, **en amont**. Une résistance (R34) vide le rail | rail coupé, la relecture doit lire 0 en quelques millisecondes ; avec les deux 10 µF en aval elle mentirait pendant 0,2 s, et l'armement provoquerait un appel de courant. Le 100 nF de chaque pompe referme au plus court son courant haché : sans celui de la pompe chlore, il parcourait 36 mm, dont 14 par le dessous | calcul |
| **P3** | Les diviseurs de relecture donnent entre 2,6 et 3,3 V pour 12 V (100 kΩ / 33 kΩ : 2,98 V). Les diodes de roue libre sont sur le rail **commuté** | au-dessus de 3,3 V la broche de l'ESP32 souffre ; en dessous de 2,5 V elle ne lit plus un niveau haut franc (il est garanti tant que le 12 V reste au-dessus de 10,2 V). Diode sur le rail commuté : le courant du moteur se referme sur lui-même quand l'interrupteur commun s'ouvre en marche | calcul |
| **P4** | Aucun cuivre dans la zone de l'antenne (elle dépasse dans une encoche de la carte). Rien de haut ni de métallique à moins de 15 mm | demande d'Espressif ; du métal près de l'antenne la désaccorde | calcul |
| **P5** | Rien ne tire IO12. IO0 et EN gardent leur rappel | IO12 fixe la tension de la mémoire au démarrage : tirée vers le haut, le module ne démarre plus | calcul |
| **P6** | Bornier de la sonde de température : 100 Ω sur l'alimentation (R39, boîtier 0805) et sur le fil de données (R40), 100 nF (C22), protection contre les décharges (U8) au ras du bornier | application de C7. 100 Ω : un court-circuit du câble ne tire que 33 mA (109 mW dans R39, d'où le 0805) et la sonde garde 3,15 V | calcul |
| **P7** | Les trois liaisons de sécurité vont du module aux pompes **par le dessous**, côte à côte, sous les deux plans internes. ⛔ Ne rien brancher sur les broches 13, 14 et 16 du connecteur de débogage quand l'appareil dose | sous les deux plans, elles sont à l'abri des pistes de puissance et des fils des pompes ; ce qu'on branche sur le connecteur agit directement sur l'armement et sur les relectures | relecture |
| **P8** | Pas de condensateur en parallèle sur un moteur **côté carte** | les pompes sont pilotées à 20 kHz : le transistor mettrait ce condensateur en court-circuit 20 000 fois par seconde. L'antiparasitage se pose sur le moteur et se règle à l'oscilloscope | relecture |
| **P9** | Les fils des pompes sont torsadés et s'éloignent de l'antenne (leurs borniers en sont à 17 mm) | ce sont les seuls conducteurs de la carte qui portent des fronts raides de 12 V | relecture au montage |
| **P10** | Le bornier 5 V externe (J12) reste **sans protection** | décision de l'utilisateur, 2026-11-28 : il n'existe que pour le prototype, il est seul à s'en servir, il sera supprimé pour la série. ⛔ Ne pas reproposer de protection | décision |

### Ce qui est toléré sur la carte principale, et pourquoi

- **`IO0` longe le bord droit à 0,75 mm sur 42 mm** (et `LED` à 1,2 mm). Elles existaient avant
  l'étude, restent au-dessus du plan, et sont des signaux qui ne bougent pas en marche. Les
  déplacer obligerait à retracer tout le couloir à droite du module. À reprendre si la carte
  est retracée un jour.
- **`IO19` et la broche 15 du connecteur de débogage ne sont plus reliées.** C'était la piste au
  bord (C4). Aucun autre tracé ne tenait loin du bord sans cinq ou six changements de face ;
  elle a donc été retirée, et la relecture de la pompe chlore a pris IO23, dont la liaison au
  connecteur existait déjà.
- **Une plage de masse de 87 mm² tient par une seule liaison** (vers X 73, Y 82). Elle est
  compacte : aucun de ses points n'est à plus de 8,5 mm de cette liaison.

### Sécurité des pompes : ce que le logiciel doit faire

Le matériel ne protège que si le logiciel s'en sert. Les dix exigences sont dans
[`v3-principale/README.md`, « Sécurité des pompes »](v3-principale/README.md#sécurité-des-pompes--rév-03-2026-11-28).
⛔ Tant qu'elles ne sont pas écrites et éprouvées, l'interrupteur commun n'apporte **rien** :
un logiciel qui met IO4 à 1 au démarrage et l'y laisse se retrouve dans la situation d'avant.

## Carte de mesure

| N° | Règle | Pourquoi | Contrôle |
|---|---|---|---|
| **M1** | Tout réseau de la carte est **déclaré dans son domaine** (côté ESP32, voie pH, voie ORP) dans `v3-mesure-std.kicad_pro` | les familles sont une liste de noms : un réseau nouveau n'y entre pas tout seul, et la règle d'isolement de KiCad ne le voit alors pas. Six réseaux étaient dans ce cas le 2026-11-28 | calcul |
| **M2** | Isolement mesuré **sur le cuivre**, toutes couches confondues : 0,8 mm au moins entre le côté ESP32 et une voie, 3,0 mm entre les deux voies | les deux sondes trempent dans la même eau : sans isolement, un courant circule de l'une à l'autre et fausse les deux mesures. Valeurs réelles : 0,94 mm aux broches des modules d'alimentation (rien ne peut les écarter), 3,5 mm entre les voies | calcul, indépendant de KiCad |
| **M3** | Seuls l'isolateur de bus et le module d'alimentation de chaque voie franchissent la barrière. ⛔ **Jamais de condensateur entre deux masses isolées** | la documentation générale conseille ce condensateur contre les parasites ; ici il laisserait repasser les courants de fuite de l'eau, c'est-à-dire ce que l'isolement sert à bloquer | calcul |
| **M4** | Un filtre entre l'amplificateur et le convertisseur : 470 Ω sur chaque entrée et 1 µF entre les deux (coupure à 169 Hz). **Chaque résistance reste sous 1 kΩ** | le convertisseur échantillonne à 250 kHz et le module d'alimentation isolé découpe dans la même zone (100 à 270 kHz selon les documents de son fabricant) : sans filtre, le battement des deux peut donner une dérive lente de la mesure. Au-delà de 1 kΩ, l'entrée du convertisseur fausse trop le gain (fiche de l'ADS1115, § 9.2.2.6). Avec 470 Ω : 0,02 à 0,13 % selon le calibre, une erreur constante que l'étalonnage absorbe | calcul |
| **M5** | Le corps de la prise (référence de la sonde) ne va en direct sur aucune puce : 100 nF vers la masse de la voie, puis la résistance du filtre. L'âme ne va qu'à sa résistance de 2,2 MΩ | on touche ce corps en branchant la sonde. Le convertisseur tient 2 kV ; une décharge au toucher se teste à 8 kV. 100 nF limitent à 12 V la tension **entre le corps de la prise et la masse de la voie** (1 nF, d'abord proposé, laissait 1 200 V). ⚠️ La voie est isolée : pendant la décharge, c'est la voie entière qui monte par rapport au reste de l'appareil, et c'est la barrière qui l'encaisse. Ce qu'elle tient ne se sait que par l'essai | calcul |
| **M6** | La garde est pilotée par l'amplificateur **à travers 1 kΩ**. Le cuivre de garde est **sans vernis** là où rien n'est posé dessus. ⛔ Jamais d'ouverture de vernis sous une prise, côté dessus | 1 kΩ : l'amplificateur pH n'a que 38° de marge de stabilité avec 20 pF de charge. Sans vernis : une fuite en surface du vernis passe par-dessus un anneau enterré sans être arrêtée ; avec 1 GΩ de fuite et une sonde de 250 MΩ, l'erreur de pente atteint 20 % (fiche du LMP7721, § 10.1). Sous la prise : son corps métallique, relié à la référence, est posé sur la carte | calcul |
| **M7** | La pastille « âme » de la résistance d'entrée est à 1,5 mm au moins du corps de la prise | elle en était à 0,58 mm ; avec les tolérances de pose, de 0,1 à 1 mm d'un métal à un autre potentiel, sur le point le plus sensible de la carte | calcul |
| **M8** | L'amplificateur pH a son alimentation filtrée (100 Ω, 10 µF, 100 nF) et son anneau de garde sur ses broches 2 et 7 | fiche du LMP7721, § 9. ⚠️ **Écart voulu** : elle demande 100 µF ; 10 µF sont posés. Ils coupent à 159 Hz, soit plus de 56 dB d'atténuation là où découpe le module d'alimentation (100 kHz et plus) ; ce rail ne porte pas de ronflement à 50 Hz | calcul |
| **M9** | La carte est **nettoyée** après soudure (option « Board Cleaning » de l'atelier) | un résidu de flux conduit assez pour fausser une mesure sous 250 MΩ | à la commande |
| **M10** | « pH » et « ORP » sont imprimés devant chaque prise | les deux fiches sont identiques et les voies ne sont pas interchangeables | relecture |

### Ce qui est attendu sur la carte de mesure

- Le contrôle de KiCad rend **cinq avertissements « piste sans suite »** : les deux doigts de
  garde de chaque voie et la traverse de la voie ORP. Ce sont des bouts de garde, voulus.
- Les plans de garde du dessous tiennent chacun par une seule liaison : la garde n'est pas une
  masse, elle suit la sortie de l'amplificateur.

## Étudié, puis écarté

Pour ne pas le reproposer sans élément nouveau.

| Idée | Pourquoi elle n'est pas retenue |
|---|---|
| Condensateur entre les masses isolées | voir M3 |
| Changer l'empilement pour deux plans de masse | le 3,3 V en couche 3 convient à des signaux lents, et Espressif le recommande pour quatre couches |
| Protéger le bornier 5 V externe | voir P10 |
| Résistance de charge sur les modules d'alimentation isolés | ils demandent 12 mA au minimum, chaque voie en tire environ 10 : à **mesurer** d'abord (voir plus bas). Une résistance ajoutée sans mesure chauffe pour rien |
| Interrupteur commun du côté de la masse | il mettrait −12 V entre grille et source des transistors des pompes, qui en tiennent 12 |
| Armement « dynamique » (l'interrupteur ne tient que si l'ESP32 envoie un signal qui bat) | protège d'un logiciel figé avec IO4 à 1 ; le chien de garde le fait déjà, et le rappel à la masse couvre le redémarrage. À rouvrir si un essai montre le contraire |
| Un condensateur sur chaque relecture | rendrait la lecture plus lente (10 ms au lieu de 1) sans besoin établi ; le logiciel relit plusieurs fois |
| Relier IO19 au connecteur de débogage par un autre chemin | cinq à six changements de face au milieu de la carte, ou 50 mm au bord |
| Déplacer `IO0` et `LED` du bord droit | gain de 0,2 mm pour un couloir entier à retracer |
| Fusionner les deux cartes | possible, et moins cher en série ; écarté pour ce premier tirage — décision de l'utilisateur en attente |

## À mesurer sur le prototype

Rien de ceci ne se décide sur plan.

| Essai | Ce qu'on regarde |
|---|---|
| Court-circuit volontaire du transistor d'une pompe (drain à la masse), appareil au repos | la relecture le signale, l'armement est refusé, la pompe ne tourne pas |
| Court-circuit volontaire de l'interrupteur commun (source et drain reliés) | rail coupé, les relectures lisent 1 : défaut signalé |
| Coupure du rail pendant qu'une pompe tourne | pas de surtension sur le rail (oscilloscope), arrêt franc |
| Pompe débranchée | à froid la relecture lit 0 ; à chaud elle peut lire 1, à cause de la fuite de la diode de roue libre. ⇒ ce n'est **pas** un moyen fiable de savoir qu'une pompe manque |
| IO4 à l'oscilloscope pendant un redémarrage et pendant une mise à jour | elle ne monte jamais d'elle-même |
| Température du régulateur 5 V et tenue du fusible réarmable, boîtier fermé, en été | le régulateur dissipe 1,75 W à 250 mA ; le fusible de 2 A se déclenche plus tôt à chaud |
| Wi-Fi et mesure du pH pendant que les pompes tournent | ni perte de liaison, ni saut de mesure |
| Courant tiré par chaque module d'alimentation isolé | au moins 12 mA ; sinon, ajouter la résistance de charge |
| Décharge au toucher sur le corps d'une prise de sonde et sur le bornier de la sonde de température | l'appareil continue de mesurer. Si une voie de mesure y reste : étudier un éclateur imprimé à travers la barrière — ⛔ pas un condensateur (M3) |
| Antiparasitage des moteurs | à régler à l'oscilloscope, sur le moteur |

## Pour la version vendue

- Vernis de protection des cartes contre l'air chloré — sauf sur les borniers, les prises et les
  anneaux de garde.
- Marquage CE : l'appareil contient un émetteur radio. Le module est déjà certifié ; l'appareil
  entier reste à évaluer.
- Supprimer le bornier 5 V externe (J12) et le connecteur de débogage (J11).
- Éloigner les borniers des pompes de l'antenne si la carte est redessinée.

## Liste de contrôle avant de commander

1. `./scripts/build-jlcpcb.sh <carte>` va au bout (contrôle de KiCad et règles de ce document).
2. `python3 scripts/verifier_pose_jlcpcb.py <carte>` : position et sens de chaque pièce.
3. Relecture par `electronics-reviewer` de ce qui a changé depuis la dernière commande.
4. Les avertissements ⚠️ du vérificateur sont **lus** : chacun figure plus haut comme toléré.
5. À l'écran de l'atelier : stock de chaque pièce, sens des pièces polarisées, nettoyage de la
   carte de mesure (M9). Voir [`COMMANDE-JLCPCB.md`](COMMANDE-JLCPCB.md).
6. Une règle a été ajoutée ou modifiée ? `python3 scripts/essai_verificateur_regles.py`.

## Pièges d'outillage rencontrés

- **KiCad renomme une piste d'après ce qu'elle touche.** À l'enregistrement, une piste posée
  par script contre un cuivre d'un autre réseau prend le nom de ce réseau, sans message. Ce qui
  prouve qu'un tracé par script est juste, c'est C1 — pas le fait que le script se soit terminé.
- **Les familles de réseaux sont des listes de noms** (M1).
- **`generer_schema.py` numérote les pièces dans l'ordre où elles sont écrites.** Toute pièce
  nouvelle s'ajoute dans la dernière section, sinon tous les numéros suivants se décalent et ne
  correspondent plus à la carte.
- **Sur la carte principale, c'est le fichier de la carte qui fait référence** : le routage ne
  se régénère pas. Les retouches sont les scripts numérotés de `v3-principale/outils/`.
  La carte de mesure, elle, se régénère en entier (`generer_schema.py` puis `generer_carte.py`).
- **Une recherche de chemin peut saturer la machine.** Le premier jet de `outils/routeur.py` a
  pris 1,3 Go et bloqué le conteneur ; il est borné depuis (400 Mo, quelques secondes).
- **Un passage de masse posé pour une face fait apparaître du cuivre sur l'autre** : KiCad
  retire les plages sans liaison, et les rend dès qu'un passage les touche. La couture se
  relance donc jusqu'à ce que plus rien ne manque (`outils/13_couture_des_plans.py` le fait).

## Sources

- Altium : [concevoir pour de faibles émissions](https://resources.altium.com/fr/p/how-to-design-pcbs-for-low-emi),
  [conformité : les bases](https://resources.altium.com/fr/p/emi-and-emc-compliance-101-pcb-designers),
  [plan d'alimentation comme chemin de retour](https://resources.altium.com/p/should-you-use-your-power-plane-as-a-return-path),
  [masse et décharges](https://resources.altium.com/p/using-esd-grounding-techniques-protect-your-pcb-electrical-damage),
  [brouillage auto-généré des objets radio](https://resources.altium.com/p/characterizing-troubleshooting-wireless-and-iot-self-generated-emi-altiumlive-2022),
  [certification](https://resources.altium.com/p/emc-certification-and-your-product) — 36 des 56
  articles de la rubrique lus ; 20 écartés comme hors sujet (médical, très haute vitesse, outils).
- Fiches des fabricants : [LMP7721](https://www.ti.com/lit/ds/symlink/lmp7721.pdf),
  [ADS1115](https://www.ti.com/lit/ds/symlink/ads1115.pdf),
  [ISO1540](https://www.ti.com/lit/ds/symlink/iso1540.pdf),
  [module B0509S](https://datasheet.lcsc.com/datasheet/pdf/8602493ba0eb6f3966b1954527e80ca4.pdf?productCode=C5369463).
- [Espressif, dessin de la carte autour du module](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html).
- Relecture indépendante des deux cartes par l'agent `electronics-reviewer`, 2026-11-28 : rien
  n'empêchait de commander ; un point corrigé (le 100 nF de la pompe chlore, C23, d'abord
  écarté ici comme inutile) et huit phrases ou chiffres de ce document et des README rectifiés.
- Avis de sécurité de l'agent `pool-chemistry` sur la coupure des pompes, 2026-11-28 :
  accord sous trois conditions, toutes tenues (P2, P3, et pas de relecture sur les broches de
  la sonde de débogage pas à pas).
