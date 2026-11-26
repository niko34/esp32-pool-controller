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
| Profondeur (sans les pompes) | 94 mm | **104** mm |
| Cartes | une, 119,4 × 119,4 mm | **principale** 119 × 100 mm, et **mesure** 73 × 54 mm dessous |
| Entretoises de carte | 4 | **8** (4 par carte), mêmes diamètre et hauteur (8,8 mm) |
| Prises des sondes | Amphenol, perçage à méplat | **Dosin 801-0080**, perçage rond de 12,5 mm, même position en largeur, 3,6 mm plus haut au-dessus du fond |
| Crochets du haut, fixations murales du haut, joint | — | remontés de 20 mm |
| Guides latéraux du capot | Y 135,9 | Y 163,5 : au-dessus de la carte principale |
| Fixation murale en bas à droite | X 190 | X 191 : 2 mm entre la tête de vis et la carte de mesure |
| Vis du bas, bord avant de la tablette | — | avancés de 10 mm, avec la profondeur |

Inchangés : alimentation Mean Well et son capot, pompes en (65 ; 91) et (145 ; 91), trois
entrées de câbles, fixations murales du bas à gauche, décor de la façade.

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

✅ Vérifié le 2026-11-26 (FreeCAD 1.0) : `pass: true`, les quinze pièces sortent d'un seul
bloc. Jeux rendus par le contrôle : capot à 1,39 mm des cartes, pompes à 20,5 mm de
l'enveloppe des composants (25 mm au-dessus des cartes), capot de l'alimentation à 1,35 mm
de la carte principale.

## Ce que le contrôle ne prouve pas

- **Les cotes de la prise des sondes** : la position de la paroi par rapport à la prise est
  mesurée sur le dessin du fabricant, pas cotée. Le bord de la carte de mesure est à 0,67 mm
  de la paroi et l'épaulement de la prise à 0,8 mm : ⏳ à vérifier pièce en main.
- **La hauteur des borniers avec leur fiche** : l'enveloppe de 25 mm est une hypothèse.
- **Les pompes** : cotes « nominales » du V14, jamais mesurées (profondeur, cosses).
- **Le trajet des câbles** : faisceaux théoriques de 6 mm, redessinés vers les borniers de
  la colonne de gauche ; les fils des pompes ne sont pas modélisés.
- **Sous le bord bas de la carte de mesure**, la réserve de 3 mm n'est pas tenue sur 1,6 mm
  de large (renfort de la tablette) : il y reste 2,5 mm. Le contrôle l'exclut explicitement.
- Étanchéité, tenue thermique, conformité électrique : rien n'est annoncé, comme pour le V14.

## Pas encore fait

- Les supports du **second format de carte d'alimentation** (feature-218) : ses dimensions
  ne sont pas décidées.
- Les documents d'assemblage et d'impression du V14 (`../v14/*.md`, profil Cura) ne sont pas
  repris ici : ils restent valables pour le principe, pas pour les cotes.
- Les scripts de vérification annexes du V14 (`verify_*.py`) ne sont pas portés.
