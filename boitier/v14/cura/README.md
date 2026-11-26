# Profil Cura — prototype V14 / SUNLU PLA+ / Genius

Utiliser uniquement PoolController_V12_Capot_SUNLU_PLAPlus_Genius_0.4_Supports.curaprofile. Son nom V12 est conservé pour conserver le profil déjà validé ; il est utilisable pour retrancher la nouvelle géométrie V14. Lire V12_CAPOT_PLA_PLUS.md pour les réglages et les limites de validation. Ce profil n'est pas un fichier modèle ni un G-code.

Import dans Cura5.9 : sélectionner Artillery Genius, buse0,4 ; Préférences > Profils > Importer ; sélectionner ce fichier puis activer le profil. Vérifier supports Normal/Partout, angle50°, densité12%, interface, Z0,20 etXY0,60, prioritéZ. Épaisseur de couche0,20,4parois, gyroid30%, bordure3mm. Retrancher puis contrôler en Preview les couches et supports réellement générés.

Avec corps210×175 et bordure4, empreinte nominale218×183 sur le plateau220×220. Contrôler aussi l'étendue des supports, la purge et les zones interdites. Il n'y a pas de projet Cura V14 déjà tranché ni d'essai machine.

Le profil précédent sans supports n'est pas inclus dans les archives V14 pour éviter une sélection accidentelle. PLA+ pour prototype uniquement, pas pour l'installation chaude définitive. La Genius mono-extrudeur ne peut pas co-imprimer automatiquement ASA clair/ASA-CF/ABS ; ne pas lancer les seuls volumes clairs en pensant que les peaux seront présentes.
