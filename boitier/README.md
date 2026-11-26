# Boîtier

Sources du boîtier imprimé en 3D du contrôleur. Versé dans le dépôt le 2026-11-25 à la
demande de l'utilisateur ; jusque-là ces sources étaient conservées à part.

| Dossier | Contenu |
|---|---|
| [`v14/`](v14/README.md) | boîtier « V14 », pompes Dosita MP1, carte v2 de 119,38 mm — révision du 2026-10-07 |

## Ce qui est versionné, et ce qui ne l'est pas

Le boîtier n'est pas dessiné à la main : **`v14/generate_freecad.py` le construit en entier**
à partir de cotes maîtresses écrites en tête du script (largeur, hauteur, profondeur,
position de la carte, des pompes, de l'alimentation…). C'est lui la source.

Les fichiers FreeCAD, STEP et STL et les rapports de contrôle sont ses **sorties** : ils ne
sont pas versionnés (`v14/.gitignore`), conformément à la règle du projet « on versionne les
sources, jamais les fichiers générés ».

Non repris de l'archive d'origine : la fiche du constructeur de l'alimentation (document
tiers, lien dans `v14/REFERENCES.md`), l'image de référence visuelle (2 Mo) et le manifeste
de l'archive.

## Régénérer les pièces

Il faut FreeCAD (vérifié avec FreeCAD 1.0.0 sous Debian, le 2026-11-25). Depuis `boitier/v14/` :

```sh
printf '%s\n' '__file__ = "generate_freecad.py"; exec(compile(open(__file__, "rb").read(), __file__, "exec"))' | freecadcmd -c
```

Compter environ trois minutes. Le script écrit `freecad/` (16 fichiers, dont l'assemblage),
`step/` (15), `stl/` (13) et `reports/validation_report.json`.

⚠️ `freecadcmd` peut rendre la main sans erreur après une exception : **seul le rapport fait
foi**, il doit contenir `"pass": true`.

✅ Vérifié le 2026-11-25 : la régénération rend `pass: true` et les quinze pièces ont
exactement le volume inscrit dans le rapport de l'archive d'origine.

## Ce boîtier et le PCB v3

Le V14 est dessiné pour la carte v2. Le PCB v3 demande de l'adapter ; les cotes relevées et
les changements décidés (20 mm de hauteur et 10 mm de profondeur en plus, carte principale
de 119 × 100 mm, carte de mesure séparée) sont dans
[`../kicad/v3-principale/README.md`](../kicad/v3-principale/README.md).
⛔ Le boîtier n'est pas encore adapté : `v14/` est l'état d'avant le v3.

⚠️ Les documents de `v14/` ont été écrits par un autre assistant pour un poste macOS : les
chemins et la commande de lancement qu'ils citent sont ceux de ce poste. Leurs réserves
restent entières — aucune étanchéité, conformité électrique ou tenue thermique n'est
annoncée, et les cotes des pompes sont « nominales », non mesurées.
