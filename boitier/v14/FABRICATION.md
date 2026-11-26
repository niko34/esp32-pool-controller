# Fabrication V14
Unités mm, pièces actives uniquement sous freecad/, step/, stl/.
Corps 210 × 175 × 94 ; capot local 90,8 de profondeur.
Ne pas imprimer les composants achetés ni les références monobloc comme pièces supplémentaires.
Importer les volumes couleur à origine commune, pas comme objets recentrés séparément.
Capot : noyau clair + graphite + signature ; platine : noyau clair + peau noire.

## Prototype Genius
Profil inchangé sous cura/ :
PoolController_V12_Capot_SUNLU_PLAPlus_Genius_0.4_Supports.curaprofile.
SUNLU PLA+, buse 0,4, couche 0,20, supports Normal/Partout activés.
Lire cura/V12_CAPOT_PLA_PLUS.md ; retrancher la géométrie V14.
Contrôle statique du profil seulement, pas import GUI ni qualification par impression.
La taille 210 × 175 avec bordure 4 donne 218 × 183 ; vérifier supports,
zones interdites et purge. La Genius est mono-extrudeur : pas de co-impression
automatique ASA/ASA-CF/ABS. Ne pas employer ce profil PLA+ pour ces matières.
La correction conserve l'orientation de prototype, mais les supports des nouvelles
zones doivent être vérifiés dans Preview après tranchage.

## Coupons
Commencer par gabarit PCB 119,38 / 104,14, puis pompe, connecteurs PG7/BNC en D,
inserts M4, joint, accès mural droit et accès gauche avec cloison/PG7.
Essais dans la matière finale pour tolérances, supports et tenue des inserts.
M3 PCB : diamètre et retenue restent à qualifier, alésage lisse Ø3,2 inchangé.
ASA clair / ASA-CF graphite / ABS turquoise : matériaux de cible,
pas une qualification thermique ni électrique de ce package.
Ne pas lancer une impression finale avant confirmation des fiches PCB et montage réel.

