# Boîtier V15 — adapté au PCB v3

Dérivé du V14 le 2026-11-26. **Le dessin général et la position des pompes ne changent pas**
(contrainte de l'utilisateur) : le boîtier est seulement agrandi, et il porte deux cartes au
lieu d'une.

⚠️ **Jamais imprimé.** Tout ce qui suit sort du script et de son contrôle intégré, pas d'une
pièce réelle.

## Ce qui change par rapport au V14

| | V14 | V15 |
|---|---|---|
| Largeur × hauteur | 210 × 175 mm | 210 × **195** mm |
| Profondeur (sans les pompes) | 94 mm | **123** mm (104, puis 114 le 2026-11-26 pour un transformateur de 48 mm sous la pompe de gauche, puis 123 le 2026-11-28 : 10 mm libres derrière les cosses des pompes) |
| Pompes | variante Dosita MP1 (cotes jamais mesurées) | **pompes AliExpress 12V-S10B de l'utilisateur**, cotes mesurées par lui : trou de 29,5 mm pour un moteur de 28,9, deux vis M3 à 48,2 mm d'entraxe, à la hauteur du moteur |
| Cartes | une, 119,4 × 119,4 mm | **principale** 119 × 100 mm, et **mesure** 73 × 54 mm dessous |
| Entretoises de carte | 4 | **8** (4 par carte), mêmes diamètre et hauteur (8,8 mm) |
| Prises des sondes | Amphenol, perçage à méplat | **Dosin 801-0080**, perçage rond de **13 mm** (nez M12 : 0,5 mm de jeu autour), même position en largeur, axe à **23,4 mm** du dos du fond (3,2 de fond + 8,8 de plot + 1,6 de carte + 9,8 d'axe selon le plan du fabricant) |
| Crochets du haut, joint | — | remontés de 20 mm |
| Fixations murales du haut | Y 150 | Y 178 : au-dessus du capot d'alimentation allongé |
| Alimentation | Mean Well RPS-30-12 seule | la Mean Well **ou** une carte au second format, 52 × 120 mm (feature-218) : deux supports neufs en (22 ; 155) et (66,4 ; 155) |
| Capot de protection de l'alimentation | 60,8 × 86 mm, 34 mm de haut | un seul pour les trois alimentations : 60,8 × 128 mm, 58 mm de haut |
| Cloison du 230 V | 70 mm de profondeur | suit la profondeur du boîtier : 99 mm, à 2,8 mm du capot |
| Guides latéraux du capot | Y 135,9 ; ils ne dépassaient le cadre du joint que de 1,2 mm et ne guidaient rien | Y 163,5 à 180,5, **refaits** : 7 mm au-dessus du cadre, bord biseauté, 0,3 mm de jeu avec la bride du capot. Ils centrent le capot, ils ne le retiennent pas (constat de l'utilisateur, 2026-11-26) |
| Fixation murale en bas à droite | X 190 | X 191 : 2 mm entre la tête de vis et la carte de mesure |
| Vis du bas, bord avant de la tablette | — | suivent la profondeur (cotes calculées depuis `BODY_D`) |

Inchangés : supports de la Mean Well, pompes en (65 ; 91) et (145 ; 91), trois entrées de
câbles, fixation murale du bas à gauche, décor de la façade (la bande sombre reste centrée
sur les pompes, donc plus au milieu du boîtier).

**Positions** (origine : coin bas-gauche du boîtier vu de face) :
- carte principale : X 76,95 à 196,0 · Y 62,0 à 162,05 ;
- carte de mesure : X 112,07 à 185,12 · Y 3,87 à 57,92 ;
- les deux connecteurs de liaison sont face à face, à 12,5 mm l'un de l'autre ;
- trous des cartes : relevés sur les fichiers KiCad (`MAIN_HOLES_KICAD`, `MEAS_HOLES_KICAD`).

## Régénérer

Comme le V14, depuis `boitier/v15/` (environ cinq minutes) :

```sh
printf '%s\n' '__file__ = "generate_freecad.py"; exec(compile(open(__file__, "rb").read(), __file__, "exec"))' | freecadcmd -c
```

Seul `reports/validation_report.json` fait foi : il doit contenir `"pass": true`.

✅ Vérifié le 2026-11-28 (FreeCAD 1.0) : `pass: true`, les seize pièces sortent d'un seul
bloc. Jeux rendus par le contrôle : capot à 1,39 mm des cartes ; derrière les pompes,
**cosses branchées comprises** : **10,2 mm** jusqu'au capot de l'alimentation (pompe de
gauche) et **35,8 mm** jusqu'aux 25 mm réservés aux composants des cartes (pompe de
droite) ; cloison du 230 V à 2,8 mm du capot. Contrôle refait à part sur les pièces de
l'assemblage : le moteur passe dans son trou (0,3 mm de jeu), les quatre trous de vis sont
libres (ruban compris), rien d'autre à moins de 15 mm des pompes.

**Les pompes : ce qui est mesuré et ce qui ne l'est pas.** Mesuré par l'utilisateur
(`../v14/REFERENCES.md`) : moteur 28,9 mm, 38,4 mm derrière la bride, 43,6 mm avec les
bornes, vis M3 à 48,2 mm. ⚠️ **Hypothèse** : la cosse branchée sur chaque borne dépasse de
5 mm (cosse **à drapeau**, fil sortant de côté) — `PUMP_CONNECTOR_D`. La profondeur occupée
est donc 48,6 mm, et le contrôle exige 10 mm libres derrière (`PUMP_FREE_SPACE_MIN`,
consigne de l'utilisateur). Une cosse droite demanderait environ 24 mm au lieu de 5 :
il resterait alors moins de 10 mm à gauche, et il faudrait approfondir d'autant.
Largeur des bornes : **2,5 mm** (mesure de l'utilisateur, 2026-11-28). La cosse plate
normalisée la plus proche est la 2,8 mm, qui existe à drapeau ; elle aura 0,3 mm de jeu en
largeur. Épaisseur des bornes : **0,5 mm** (mesure de l'utilisateur, même jour) ⇒ cosse
femelle **2,8 × 0,5 mm à drapeau**. ⏳ Aucune référence vérifiée chez un vendeur ; tenue à
essayer sur une pompe avant d'acheter en nombre.
Non mesurés : la place des bornes autour de l'axe, la tête de la pompe
(dessinée à titre indicatif).

⚠️ **Le contrôle ne vérifie que ce qu'il modélise.** Le premier V15 (104 mm) passait le
contrôle avec une cloison du 230 V restée à 70 mm, soit 12,8 mm d'ouverture devant le
capot : le jeu était mesuré mais n'était pas un critère. Il l'est maintenant (3 mm au plus),
et le second format d'alimentation a son propre contrôle (`second_psu_format`).

**Alignement des prises des sondes, mesuré sur la pièce générée** (2026-11-26) : les deux
perçages ont leur axe à Z 23,40 et à X 123,10 / 159,40, diamètre 12,98 ; le dessus des quatre
plots de la carte de mesure est à Z 12,00. ⚠️ La chaîne réelle (plot imprimé, carte, prise
donnée à ± 0,3 mm, soudure) peut s'écarter d'environ 0,7 mm : d'où le perçage de 13 mm, et
la pièce d'essai `MeasureBoardFitCoupon` (les quatre plots et la paroi percée), **à imprimer
et à essayer avec la vraie carte avant d'imprimer le boîtier**.

## Ce que le contrôle ne prouve pas

- **Les cotes de la prise des sondes** : la position de la paroi par rapport à la prise est
  mesurée sur le dessin du fabricant, pas cotée. Le bord de la carte de mesure est à 0,67 mm
  de la paroi et l'épaulement de la prise à 0,8 mm : ⏳ à vérifier pièce en main.
- **La hauteur des borniers avec leur fiche** : l'enveloppe de 25 mm est une hypothèse.
- **Les pompes** : voir plus haut. La réserve des cosses est une hypothèse.
- **La Dosita MP1 est abandonnée pour ce boîtier** (2026-11-27). Son modèle ne correspondait
  pas au plan du fabricant (moteur d'environ 51,5 mm excentré de 26 mm, pour 25,5 mm centré
  dans le modèle), et sa version 12 V — motoréducteur à courant continu, vu sur la photo de
  Dosita — est plus longue que le plan (60 à 76 mm estimés, sans cote publiée). La reprendre
  demanderait son plan coté ou une pompe en main.
- **Le trajet des câbles** : faisceaux théoriques de 6 mm, redessinés vers les borniers de
  la colonne de gauche ; les fils des pompes ne sont pas modélisés. Le câble 12 V sort par
  le haut du capot d'alimentation et passe entre ses pattes de fixation puis à droite d'un
  crochet du capot : passage étroit, à essayer en vrai.
- **Le guidage latéral du capot** : calculé (0,3 mm de jeu), jamais essayé. Aucune retenue
  à mi-hauteur : la place manque des deux côtés (capot d'alimentation, carte principale).
- **Le second format d'alimentation** : seule son enveloppe est vérifiée (carte, 50 mm de
  composants). Aucune carte n'est dessinée ; la chaleur d'une carte à transformateur dans le
  boîtier fermé (3 à 8 W estimés) n'est pas traitée.
- **Sous le bord bas de la carte de mesure**, la réserve de 3 mm n'est pas tenue sur 1,6 mm
  de large (renfort de la tablette) : il y reste 2,5 mm. Le contrôle l'exclut explicitement.
- Étanchéité, tenue thermique, conformité électrique : rien n'est annoncé, comme pour le V14.

## Pas encore fait

- Les documents d'assemblage et d'impression du V14 (`../v14/*.md`, profil Cura) ne sont pas
  repris ici : ils restent valables pour le principe, pas pour les cotes.
- Les scripts de vérification annexes du V14 (`verify_*.py`) ne sont pas portés.
