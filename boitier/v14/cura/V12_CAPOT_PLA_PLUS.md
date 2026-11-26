# Profil Cura — capot V12 en SUNLU PLA+

Ajout du 7 octobre 2026 :
`PoolController_V12_Capot_SUNLU_PLAPlus_Genius_0.4_Supports.curaprofile`.
Artillery Genius, buse 0,4 mm, SUNLU PLA+ 1,75 mm, Cura 5.9.0.
C'est un point de départ pour un prototype, pas un profil ASA/ASA-CF/ABS,
ni une qualification de fabrication. Aucun paramètre machine ou G-code de
démarrage/fin n'est remplacé : conserver la configuration de la Genius.

## Importer

1. Sélectionner l'Artillery Genius et la buse 0,4 mm dans Cura, avec un matériau PLA.
2. **Preferences > Configure Cura > Profiles > Import**
   (Préférences > Configurer Cura > Profils > Importer).
3. Importer le fichier `.curaprofile` sans le décompresser.
4. Activer **PoolController V12 - Capot - SUNLU PLA+ - Genius 0.4 - Supports**.
5. Si Cura propose de conserver les anciens réglages personnalisés, choisir ceux
   du nouveau profil. Vérifier aussi les réglages par objet et les bloqueurs de supports.
6. Trancher à nouveau ; dans **Preview**, choisir **Color scheme: Line Type**,
   afficher supports et interfaces dans la légende et parcourir les couches.

## Réglages enregistrés

| Paramètre | Valeur |
| --- | --- |
| Couche / première couche | 0,20 / 0,24 mm |
| Buse : première couche / suivantes | 215 / 210 °C |
| Plateau : première couche / suivantes | 60 / 55 °C |
| Parois ; dessus/dessous | 4 ; 5/5 couches |
| Remplissage | Gyroid, 30 % |
| Vitesse générale / paroi externe / première couche | 45 / 25 / 18 mm/s |
| Rétraction directe | 1 mm à 35 mm/s |
| Ventilation | 100 %, régime complet à la couche 4 |
| Bordure d'adhérence | 3 mm |
| Generate Support | Activé |
| Support Structure / Placement | Normal / Everywhere (Partout) |
| Support Overhang Angle | 50° |
| Support Pattern / Density | Zig Zag / 12 % |
| Enable Support Interface ; Roof ; Floor | Activés |
| Interface : épaisseur / densité / motif | 0,80 mm / 33,333 % / Grid |
| Support Z ; Top ; Bottom Distance | 0,20 mm chacun |
| Support X/Y Distance | 0,60 mm |
| Support Distance Priority | Z overrides X/Y |
| Support Horizontal Expansion | 0,82 mm |
| Support Stair Step Height / Join Distance | 0 / 2 mm |

Les températures, vitesses et parois du précédent profil PLA+ sont conservées.
L'interface reprend explicitement les valeurs du profil de base Artillery fourni
avec Cura 5.9 pour une couche de 0,20 mm : quatre couches, grille à 33,333 %.
Elle n'est pas pleine à 100 %. Le jeu Z aide au retrait mais ne garantit pas
un détachement propre sur les crochets et les portées du joint.

## Avant le capot entier

- Capot monobloc complet : façade extérieure contre le plateau, ouverture vers le haut.
  Ce profil ne contient ni modèle ni orientation. La Genius est mono-extrudeur :
  il ne fusionne pas les volumes couleur et ne permet pas la co-impression trois matières.
  Ne pas confondre capot complet de prototype et noyau clair seul ; les évidements
  décoratifs de ce dernier peuvent modifier les premières couches et les supports nécessaires.
- Vérifier les supports sous les crochets et surplombs utiles. « Everywhere » permet
  de les démarrer sur la façade intérieure ; « Touching Buildplate » peut ne pas suffire.
- Vérifier l'accès pour les retirer et les marques sur les portées du joint.
  Tester d'abord une petite zone représentative avec crochet et lèvre. Les coupons
  d'ajustement existants ne valident pas tous les surplombs du capot.
- Vérifier que le contour tranché, supports et bordures compris, tient dans
  les 220 × 220 mm du plateau, avec une marge à la ligne de purge.
  La largeur de 210 mm du corps ne garantit pas à elle seule cette marge.
- Inspecter les premières couches, cavités décoratives et passages de vis.
  Ne pas changer la compensation des trous sans mesure d'un coupon.
- Adapter température/débit au filament réel après essai. Ce PLA+ sert au prototype,
  pas au boîtier final dans un local très chaud.

Le temps et la masse ne sont pas garantis identiques à la capture précédente.
Ce fichier ne fournit pas de supports peints, de bloqueurs ou de G-code.

## Validation et reprise

Contrôle statique réalisé contre les définitions de Cura 5.9.0 installé sur le Mac :
clés, types, énumérations, métadonnées, intégrité ZIP et relecture des deux configurations.
L'import graphique, le retranchage du capot avec ce fichier et l'impression physique
ne sont pas encore vérifiés. Les contrôles FreeCAD ne qualifient pas les supports.

Sources : `v12_sources/`. Pour reconstruire/vérifier : `python3 build_v12_profile.py`.
L'archive existante n'est pas écrasée si elle diffère ; créer une nouvelle révision.
La définition `artillery_base` correspond à la qualité de la Genius dans Cura 5.9.
Le réglage global `retraction_combing` est placé dans la configuration globale,
avec la même valeur qu'avant, pour respecter sa portée dans Cura.

L'ancien profil sans supports est conservé intact et ne doit pas être utilisé
tel quel pour le capot actuel. Les archives CAD précédemment livrées restent
inchangées : profil et notice sont un ajout séparé dans le projet.
