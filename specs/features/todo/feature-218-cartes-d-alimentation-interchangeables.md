# feature-218 — Trois alimentations interchangeables dans le même boîtier

## Métadonnées
- **ID** : feature-218
- **Slug** : cartes-d-alimentation-interchangeables
- **Type** : feature — ⚠️ **ÉTUDE d'abord** : aucune carte ne se dessine avant la relecture de cette fiche
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : matériel — sans objet
- **Dernière mise à jour** : 2026-11-26
- **Spec(s) liée(s)** : [feature-215](feature-215-conception-du-pcb-v3.md) (PCB v3 : besoin 6,
  décision D4, partie B « étude d'alimentation », livrables P4, P7 et M3),
  [feature-217](feature-217-carte-de-mesure-a-composants-courants.md) (carte de mesure)

## Domaines touchés
- [x] Matériel — carte d'alimentation (230 V)
- [x] Matériel — boîtier (`boitier/`)
- [x] Capteurs — c'est la qualité de la mesure du pH qui juge
- [ ] Régulation · Filtration · Web · MQTT · OTA · Authentification · Historique · Frontend

## Agents requis
- [x] electronics-reviewer — **en premier**, sur cette fiche, puis sur chaque schéma et chaque carte
- [x] spec-reader · architect
- [ ] pool-chemistry — non requis : aucun code de dosage. ⚠️ À invoquer si une mesure de bruit
      conduit à changer un seuil de la régulation
- [x] doc-writer
- [ ] embedded-developer · web-ui-developer · ux-ui-designer · test-engineer · code-reviewer —
      aucun code attendu

## Description

Demande de l'utilisateur (2026-11-26) :

> « Comme il est difficile de faire un test avec une alim qui n'est pas à découpage, je
> propose de prévoir dans le boîtier des supports pour les vis de l'alim Mean Well et des
> supports pour un deuxième format de carte d'alim qui sera un peu plus grand. On concevra
> 2 cartes d'alim : une à découpage pour comparer à la Mean Well en terme de prix et de
> qualité au sens de la qualité des mesures de pH, une autre avec un transfo qui aura la
> même taille et qu'on pourra aussi tester. Le même boîtier pourra servir aux 3
> alimentations interchangeables. »

Résultat attendu : un seul boîtier, qui reçoit au choix la Mean Well RPS-30-12, une carte
d'alimentation à découpage dessinée par nous, ou une carte d'alimentation à transformateur
dessinée par nous. Les trois se comparent sur l'appareil réel, à prix et à bruit de mesure
du pH mesurés.

## Contexte

- **Le bruit de mesure du pH vient de l'alimentation secteur** : douze essais de
  l'utilisateur l'établissent (feature-215, partie B). Sur batterie le delta brut tombe à
  0,09–0,15, contre 0,30 avec la RPS-30-12 et 0,52 avec l'ancienne HDR-30-12.
- **L'essai décisif « M3 » n'a pas pu être fait** : il demande une alimentation sans
  découpage. Constat du 2026-11-25 : les blocs à transformateur ne se trouvent presque plus,
  et une alimentation linéaire toute faite coûte une soixantaine d'euros pour un seul essai.
- **Le besoin 6 de feature-215** (« une carte d'alimentation séparée, alimentée en 230 V »)
  était suspendu à M3. Cette fiche le reprend autrement : au lieu de trancher par un essai
  sur la carte v2, on rend l'alimentation **interchangeable** et on compare sur le v3.
- **La carte principale du v3 est déjà prête pour ça** : elle a un régulateur 5 V linéaire,
  un pont à couper et un bornier « 5 V externe » (`kicad/v3-principale/README.md`).

## Comportement attendu

### Le boîtier
- Les quatre supports actuels de la RPS-30-12 restent (relevés sur l'assemblage du boîtier
  V14 : entretoises en (22 ; 43,1), (22 ; 113), (66,4 ; 43,1) et (66,4 ; 113) — carte de
  50,8 × 76,2 mm).
- S'y ajoutent les supports d'un **second format de carte, un peu plus grand**, commun aux
  deux cartes que nous dessinons.
- Le capot de protection de l'alimentation couvre les trois cas, ou existe en deux tailles.
- Les trois alimentations se montent **sans retoucher le boîtier**, et se raccordent à la
  carte principale par le **même connecteur, au même endroit**.

### Les deux cartes à dessiner
| Carte | Principe | Ce qu'on en attend |
|---|---|---|
| A — à découpage | convertisseur 230 V → 12 V à découpage, dessiné par nous | comparer à la RPS-30-12 en **prix** et en **bruit de mesure du pH** |
| B — à transformateur | transformateur 50 Hz, redressement, régulation linéaire | savoir si un transformateur fait aussi bien que la batterie, en restant branché au secteur |

Les deux ont **la même taille et les mêmes fixations**.

### La comparaison
Même appareil, même bassin, mêmes conditions, les trois alimentations l'une après l'autre.
Métrique : le delta pH **brut** (max − min sur 13 mesures brutes), sur au moins une heure,
filtration notée. ⛔ Jamais un delta brut contre un delta lissé (erreur déjà commise,
feature-215 partie B).

### Cas limites
- Une des deux cartes ne tient pas la consommation de pointe (Wi-Fi de l'ESP32, démarrage
  d'une pompe) : c'est un résultat, à consigner.
- La carte à transformateur chauffe trop dans un boîtier fermé : idem.
- Aucune des deux ne fait mieux que la RPS-30-12 : on garde la RPS, et les supports du
  second format restent inutilisés.

## Comportement actuel

Le boîtier V14 ne reçoit que la RPS-30-12, sous son capot (`boitier/v14/`). Aucune carte
d'alimentation maison n'existe. L'essai M3 n'a jamais été fait.

## Contraintes techniques

- ⛔ **230 V sur une carte que nous dessinons.** La RPS-30-12 est certifiée (classe médicale
  2 × MOPP) ; une carte maison met la sécurité électrique à notre charge : distances
  d'isolement et lignes de fuite entre primaire et secondaire, fusible, tenue au feu,
  échauffement en boîtier fermé. ⚠️ L'appareil est **vendu à des clients**, en local humide.
  ⇒ Ces deux cartes sont d'abord des **cartes d'essai** ; les vendre est une décision à part,
  qui demande d'avoir traité la conformité.
- ⛔ **Ne jamais relier l'eau à la terre ou à la masse pour un essai** sans analyse préalable
  (rappel de feature-215).
- **Règle d'approvisionnement D8** (feature-215) : composants courants, posés par l'atelier,
  un équivalent d'un autre fabricant sur la même empreinte, la qualité restant un critère
  majeur. ⚠️ Un transformateur et un contrôleur de découpage secteur ont rarement un second
  fabricant sur la même empreinte : les exceptions seront à écrire et à chiffrer.
- **Fabrication chez JLCPCB** : vérifier ce que l'atelier accepte de poser sur une carte
  secteur (transformateur traversant lourd, condensateurs haute tension, fentes d'isolement).
- **Le boîtier ne change pas de dessin général** et la position des pompes ne bouge pas
  (utilisateur, 2026-11-25). Le moteur de la pompe de gauche passe au-dessus de
  l'alimentation : 13,1 mm de jeu avec le capot une fois le boîtier approfondi de 10 mm.
  ⇒ La hauteur des composants des deux cartes compte, en particulier celle du
  transformateur. ⚠️ **Elle n'est pas une limite dure** (utilisateur, 2026-11-26) : la
  profondeur du boîtier peut encore augmenter. ⇒ On choisit d'abord le bon transformateur,
  puis on chiffre la profondeur qu'il demande ; on n'écarte pas une pièce pour quelques
  millimètres.
- ⛔ **Toute alimentation fournit un 12 V RÉGULÉ, sous 13 V.** La carte principale porte un
  écrêteur SMBJ13A sur son entrée. Un 12 V non régulé sortant d'un transformateur monte à
  16 à 20 V à vide : l'écrêteur grillerait.
- **Aucune des deux cartes ne doit exiger de réglage chez le client.**

## Points à trancher AVANT de dessiner

| # | Point | Pourquoi il décide |
|---|---|---|
| V1 | **Ce que fournit une carte d'alimentation** : 12 V seul, ou 12 V et 5 V séparés | 12 V seul = échange direct avec la RPS. Mais les essais E03, E07 et E08 montrent qu'assainir un seul rail ne gagne rien : pour la carte à transformateur, fournir aussi le 5 V de la logique et de la mesure est peut-être ce qui compte |
| V2 | **Puissance** : la RPS-30-12 donne 2,5 A. Consommation réelle à mesurer (deux pompes, bobines de contacteur, ESP32) | un transformateur de 30 VA est gros et lourd ; 10 à 15 VA suffisent peut-être si les pompes ne tournent jamais ensemble |
| V3 | **Le format commun** : dimensions, quatre trous, hauteur des composants (et la profondeur de boîtier qui en découle), position des borniers d'entrée et de sortie | c'est le contrat qui rend les trois alimentations interchangeables. À figer une fois, comme la liaison entre carte principale et carte de mesure |
| V4 | **Le connecteur vers la carte principale** et la longueur du câble | même fiche pour les trois ; la RPS a ses propres connecteurs, il lui faudra peut-être un cordon d'adaptation |
| V5 | **Le capot** : un seul, ou un par format | il sépare le 230 V du reste du boîtier |
| V6 | **Topologie de la carte à découpage** et son isolement | c'est la capacité entre primaire et secondaire qui est soupçonnée de porter le bruit, pas le découpage en soi (feature-215 : un convertisseur « médical » a triplé le bruit) |
| V7 | **Le transformateur** : référence, capacité primaire-secondaire, échauffement, protection | ⚠️ le Myrra 44266 étudié dans feature-215 ne fait que **10 VA** (environ 0,4 A en 5 V) : il ne peut pas fournir le 12 V des pompes et des contacteurs. Sa construction « double chambre » reste le bon critère ; contradiction non levée sur sa protection et son fusible |
| V8 | **Le protocole de comparaison** | sans conditions identiques, les trois chiffres ne se comparent pas |
| V9 | **Le prix** de chaque carte, posée et livrée, contre celui de la RPS-30-12 | c'est un des deux critères de la demande |
| V10 | **Les cotes de sécurité 230 V** (voir « Sécurité ») | la surface de carte ne coûte rien : on dessine dès l'essai aux cotes d'un produit vendu |
| V11 | **La chaleur dégagée dans le boîtier fermé** | une carte à transformateur de 12 V complète dissiperait jusqu'à 15 W dans son régulateur, contre environ 1,5 W pour la RPS |
| V12 | **L'approvisionnement du transformateur** selon D8 | non trouvé au catalogue de l'atelier : à expédier ou à souder à la main, donc une exception à écrire |
| V13 | **La compatibilité avec l'entrée de la carte principale** | sortie ≤ 13 V obligatoire (voir « Contraintes »), et fusible réarmable de 2 A en entrée |

**Ordre conseillé par le relecteur** : V0, puis V2, V1, V7 et V6, V3, V4 et V5, V8, V9.

### V3 — le format commun : ✅ FIXÉ le 2026-11-26

Étude de `electronics-reviewer` (cotes lues sur les fiches des fabricants), demandée par
l'utilisateur : « détermine la taille pour permettre de concevoir les alimentations avec les
composants idéaux ».

| | Valeur |
|---|---|
| Carte | **52 × 120 mm**, même axe que la RPS : X 18,2 à 70,2 · Y 39 à 159 (repère du boîtier) |
| Fixations | les deux plots du bas de la RPS, (22 ; 43,1) et (66,4 ; 43,1), et deux plots neufs, (22 ; 155) et (66,4 ; 155). Les plots du haut de la RPS (Y 113) restent : la carte s'y appuie **sans vis** — prévoir au dos un rond de 9 mm sans cuivre à ces deux endroits |
| Hauteur des composants | 50 mm sous un capot unique de 60,8 × 128 × 58 mm |
| Profondeur du boîtier | **114 mm** (accord de l'utilisateur, 2026-11-26) : reçoit le transformateur de 22 VA (48 mm) comme celui de 16 VA (39 mm) |
| Arrivée 230 V | en bas, vers X 44, face au passe-câble |
| Sortie 12 V | en haut |

**Composants qui ont dimensionné le format**
- Carte B : transformateur **Hahn EI54** (16 VA : 47,7 × 75,8 × 39,3 mm, 0,40 kg ; 22 VA :
  même empreinte, 47,9 mm de haut, 0,55 kg), garanti 70 °C, **non protégé** : fusible au
  secondaire obligatoire. À visser (pattes M4), pas à tenir par ses soudures.
- Carte A : module **Mean Well MFM-30-12** à souder (65,5 × 35 × 23 mm, fuite < 80 µA comme
  la RPS). Un découpage dessiné de zéro est écarté.
- Séparation primaire / secondaire : 23 mm sur la carte, pour 8 mm visés.

**Écartés** : un format plus large (57 × 126 mm, transformateur EI60 de 28 VA), qui
obligerait à élargir le boîtier pour un courant que la chaleur ne permettrait pas d'utiliser ;
le **Myrra 44266** du point V7, limité à 50 °C ambiants par sa fiche.

**Hypothèse de consommation** (rien n'est mesuré) : 0,3 A en usage courant, 0,6 à 0,9 A
pendant un dosage, 1,2 à 1,7 A au pire pendant quelques minutes.

⏳ **Non vérifié** : la capacité entre primaire et secondaire des Hahn (c'est pourtant le
critère du bruit de mesure), leur prix et leur disponibilité (catalogue de 2013, un seul
fabricant), la valeur du fusible secondaire, la chaleur dans le boîtier fermé (3 à 8 W
estimés pour la carte B, contre 1,4 W pour la RPS — c'est la vraie limite de cette carte).

### V0 — le point de référence, AVANT toute carte 230 V (ajout de la relecture)

Mesurer d'abord le v3 lui-même, sans rien dessiner : une fois sur la RPS-30-12, une fois sur
batterie (pont JP1 coupé, batterie USB sur le bornier 5 V externe, 12 V débranché — montage
déjà décrit dans `kicad/v3-principale/README.md`).
- Si le v3 sur RPS est déjà au niveau de la batterie (0,09 à 0,15), **les deux cartes sont
  inutiles** : l'isolation par voie a suffi.
- Sinon, l'écart entre les deux chiffres est ce que les cartes A et B ont à gagner.
⇒ Sans ce point, la comparaison des trois alimentations n'a pas de repère.

✅ **Ordre accepté par l'utilisateur (2026-11-26)** :
1. fabriquer la carte principale et la carte de mesure du v3 ;
2. faire la mesure V0 (Mean Well, puis batterie) ;
3. **décider alors** de dessiner ou non les cartes d'alimentation A et B.

⇒ ⛔ Aucune carte d'alimentation ne se dessine avant V0.
⇒ ✅ Les **supports du second format** dans le boîtier se prévoient dès l'adaptation du
boîtier au v3 : ils ne coûtent presque rien, même s'ils restent vides. Il faut donc figer
le format commun (V3) à ce moment-là, sans attendre V0.

## Fichiers potentiellement concernés

- `kicad/v3-alim-decoupage/` [nouveau] — carte A
- `kicad/v3-alim-transfo/` [nouveau] — carte B
- `boitier/` — supports du second format, capot ; adaptation du générateur
- `kicad/v3-principale/README.md` — connecteur d'arrivée, usage du bornier 5 V externe
- `specs/features/todo/feature-215-conception-du-pcb-v3.md` — renvoi vers cette fiche
- `test/fixtures/` — relevés bruts des comparaisons

## Livrables — check-list

**Étude**
- [ ] E0 — **Mesure V0** sur le v3 : Mean Well, puis batterie — ⛔ préalable à A1, A2, B1 et B2
- [ ] E1 — Points V1 à V13 tranchés par écrit, relus par `electronics-reviewer`
- [x] E2 — Format commun des deux cartes écrit dans cette fiche (dimensions, trous, hauteur) — 2026-11-26, voir « V3 ». ⏳ Le connecteur vers la carte principale (V4) reste à fixer

**Cartes**
- [ ] A1 — Carte A (découpage) : composants, connexions, schéma, relus
- [ ] A2 — Carte A : placement, routage, contrôle des règles sans erreur, distances d'isolement vérifiées
- [ ] B1 — Carte B (transformateur) : composants, connexions, schéma, relus
- [ ] B2 — Carte B : placement, routage, contrôle des règles sans erreur, distances d'isolement vérifiées

**Boîtier**
- [x] C1 — Supports du second format ajoutés au générateur, ceux de la RPS-30-12 conservés — `boitier/v15/`, 2026-11-26, contrôle `second_psu_format`
- [x] C2 — Capot adapté ; jeu avec le moteur de la pompe de gauche vérifié pour les trois alimentations — un seul capot, 4,9 mm sous l'arrière de la pompe (cote de pompe nominale, jamais mesurée). ⚠️ Jamais imprimé

**Mesures**
- [ ] M1 — Protocole de comparaison écrit
- [ ] M2 — Les trois alimentations mesurées sur le même appareil : delta pH brut sur ≥ 1 h chacune
- [ ] M3 — Prix de revient des trois, relevés et comparés
- [ ] M4 — Décision écrite : quelle alimentation équipe l'appareil vendu, et pourquoi

## Critères d'acceptation
- [ ] Les trois alimentations se montent dans le même boîtier **sans le retoucher**
- [ ] Elles se raccordent à la carte principale par le même connecteur
- [ ] Les deux cartes ont les mêmes dimensions et les mêmes fixations
- [ ] Les distances d'isolement entre primaire et secondaire sont **chiffrées et vérifiées** sur chaque carte
- [ ] Chaque exception à la règle D8 est écrite, chiffrée et acceptée par l'utilisateur
- [ ] Les trois deltas pH sont mesurés **en brut**, dans des conditions notées
- [ ] Le prix de chaque solution est relevé
- [ ] La décision finale est écrite, avec les mesures qui la justifient

## Sécurité
- Dosage impliqué : non
- ⚠️ **230 V** : toute mise sous tension d'une carte maison se fait derrière un différentiel
  30 mA, boîtier fermé ou carte sous capot. Première mise sous tension sans la carte
  principale, tensions vérifiées à vide puis en charge.
- Régulation du pH et du chlore en **Arrêt** pendant toute manipulation d'alimentation ou
  de sonde.
- ⛔ **Pas de transformateur d'isolement pendant les mesures de bruit** : il change le chemin
  de fuite vers la terre, qui est justement ce qu'on mesure. ⚠️ Le différentiel 30 mA ne
  protège pas d'un contact entre phase et neutre.

**Cotes à viser dès la carte d'essai** — données par le relecteur **de mémoire**
(CEI 60664-1, CEI 62368-1), ⏳ **à confirmer dans le texte des normes** avant de dessiner :

| Élément | Valeur visée |
|---|---|
| Distance dans l'air entre primaire et secondaire | 4 mm au moins |
| Ligne de fuite, isolation renforcée, 250 V, milieu humide | 8 mm, avec une fente fraisée d'au moins 1,5 mm sous le transformateur |
| Fusible temporisé sur l'arrivée secteur | celui que prescrit le fabricant du module ou du transformateur |
| Varistance | 275 V, après le fusible |
| Classe | **classe II, sans fil de terre**, comme la RPS-30-12 |
| Capot et supports | plastique classé V-0 pour un produit vendu ; l'impression 3D courante est classée HB, acceptable pour un essai seulement |
| Transformateur | prévu pour 70 °C ambiant |

## Relecture du 2026-11-26 (`electronics-reviewer`)

Verdict : on peut avancer, **à condition de faire d'abord la mesure V0**.

**Corrigé dans la fiche** : le Myrra 44266 est trop petit pour le 12 V (V7) ; une sortie
12 V doit être régulée sous 13 V ; pas de transformateur d'isolement pendant les mesures ;
ajout de V0 et de V10 à V13 ; cotes de sécurité.

**Ses avis, ⏳ À TRANCHER PAR L'UTILISATEUR — ils modifient la demande d'origine** :

1. **V1 : 12 V seul.** La carte principale n'a qu'une masse. Les essais E03, E07 et E08
   montrent que ce qui compte est le couplage de *la* source reliée à cette masse, pas le
   rail qu'elle alimente. Un 5 V séparé tiré du même transformateur ne changerait rien.
2. **V2 : consommation estimée** (de mémoire, à mesurer) — environ 0,8 A en usage courant,
   jusqu'à 2 A au pire. Une carte B complète de 1,5 A demande un transformateur d'environ
   40 VA, haut de 45 à 50 mm, pesant 0,5 kg, et dissipe jusqu'à 15 W.
3. **Carte A : un module secteur encapsulé plutôt qu'un découpage dessiné de zéro.** Un
   découpage maison demande un transformateur bobiné sur mesure (fabricant unique) et des
   essais de perturbations en laboratoire, de l'ordre de 1 à 3 k€ : le relecteur l'écarte.
   Avec un module (par exemple HLK-20M12, au catalogue de l'atelier, 4 à 5 $), la carte se
   réduit à un fusible, une varistance et deux borniers. ⚠️ Ce n'est alors plus « un
   découpage dessiné par nous », et un module garanti moins bien que la RPS en courant de
   fuite fera probablement plus de bruit, pour moins cher.
4. **Carte B : commencer petit.** Pour la seule question du bruit, une carte de 10 VA qui
   sort du 5 V sur le bornier 5 V externe, pompes à l'arrêt, suffit (c'est l'essai E12 de
   feature-215). La carte 12 V complète ne se justifie que si celle-là gagne.
5. **Deux mesures presque gratuites, à faire avant de dessiner** : la capacité entre
   primaire et secondaire de chaque alimentation candidate, hors tension, au capacimètre ;
   et le courant de fuite entre le 0 V de sortie et la terre, au multimètre (⚠️ il ne voit
   pas les fréquences de découpage). La RPS est garantie sous 80 µA.
6. **Protocole** : médiane, maximum et 95ᵉ centile sur au moins 1 h ; alterner les
   alimentations le même jour (RPS, puis l'autre, puis RPS), car la sonde dérive ; garder la
   sonde de température et le détecteur de débit branchés de la même façon pour les trois.

**Non vérifié par le relecteur** : texte des normes, courant de fuite du HLK-20M12, prix de
la RPS-30-12, présence de transformateurs au catalogue de l'atelier, ce que l'atelier accepte
sur une carte secteur (aucune règle publiée trouvée ; il ne fait aucun essai de tenue en
tension).

## Notes pour agent
- ⛔ **Aucune modification de code n'est attendue de cette fiche.**
- ⚠️ Un résultat négatif de la carte B ne prouverait pas qu'aucun transformateur ne
  convient : il dirait que celui-là ne suffit pas (feature-215, partie B).
- ⚠️ La comparaison se fait sur le **v3**, dont la carte de mesure a une isolation par voie.
  Les chiffres de bruit du v2 ne s'y transposent pas.
- Tout choix d'isolement, de transformateur ou de topologie passe par `electronics-reviewer`
  avant d'être dessiné.

## Hors périmètre
- La mise en conformité pour la vente d'une alimentation maison (sécurité électrique,
  marquage) : à traiter à part si l'une des deux cartes est retenue.
- La carte principale et la carte de mesure (feature-215, feature-217).
- Le pilote logiciel de la mesure.
