# Boîtier

Sources du boîtier imprimé en 3D du contrôleur. Versé dans le dépôt le 2026-11-25 à la
demande de l'utilisateur ; jusque-là ces sources étaient conservées à part.

| Dossier | Contenu |
|---|---|
| [`v14/`](v14/README.md) | boîtier « V14 », pompes Dosita MP1, carte v2 de 119,38 mm — révision du 2026-10-07 |
| [`v15/`](v15/README.md) | boîtier « V15 », le V14 agrandi pour les deux cartes du PCB v3 — 2026-11-26, jamais imprimé |

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

Le V14 est dessiné pour la carte v2. **Le V15 (`v15/`) est son adaptation au PCB v3**, faite
le 2026-11-26 : 20 mm de hauteur et 29 mm de profondeur en plus (210 × 195 × 123), pompes AliExpress
de l'utilisateur à la place de la Dosita MP1 (2026-11-28), carte principale de
119 × 100 mm et carte de mesure séparée. Le dessin général et la position des pompes ne
changent pas. Le contrôle intégré du script rend `pass: true` ; ⚠️ rien n'est imprimé.
Voir [`v15/README.md`](v15/README.md).

⚠️ Les documents de `v14/` ont été écrits par un autre assistant pour un poste macOS : les
chemins et la commande de lancement qu'ils citent sont ceux de ce poste. Leurs réserves
restent entières — aucune étanchéité, conformité électrique ou tenue thermique n'est
annoncée, et les cotes des pompes sont « nominales », non mesurées.
