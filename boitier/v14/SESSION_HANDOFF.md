# Reprise — PoolController V14, PCB réel
Révision du 7 octobre 2026. Ce document et le générateur de CE package font foi.
V14 : PCB remonté de 1,5 par rapport à V13, axes muraux inchangés. Accès bas droit :
tête Ø7,5 avec marge nominale 2,05, tournevis Ø6 avec marge 2,80, capot retiré.
Guides et retenues remontés ensemble de 1,5 ; route supérieure RPS Y137,5.
Axes traversants BNC X/Z inchangés, référence du corps déplacée +1,5 en Y.
Saillie/engagement filetés réels à confirmer ; voir REVISION_NOTES.
Les fichiers synchronisés du projet sous sources/ restent en lecture seule.
Les archives V12 et précédentes sont conservées dans le projet, jamais modifiées.
Leur PCB 104,14 et leur entraxe 88,9 sont erronés ; ne pas les réutiliser.

## Mesures et implantation
PCB utilisateur : 119,38 × 119,38, entraxes 104,14 dans les deux directions.
Centres à 7,62 des bords, épaisseur 1,6 conservée faute de nouvelle mesure.
Carte déplacée ENTIÈRE, aucune modification de composants/connecteurs ou rotation.
Repère X droite, Y haut, Z mur vers façade ; capot local translaté +3,2.
Corps 210 × 175 × 94. PCB X76,62/Y13,5, entretoises 8,8, arrière Z12.
Axes des quatre entretoises X84,24/188,38, Y21,12/125,26 ; trous lisses Ø3,2,
diamètre extérieur 7,5. Retenue des vis M3/inserts/écrous non qualifiée.
Ne pas prétendre qu'un passage lisse assure un filetage ou le serrage du PCB.
BNC X123,102/159,424, Z19,8 ; mêmes décalages locaux 46,482/82,804 et 7,8
depuis la face arrière PCB que V12. Ces décalages n'ont pas été remesurés.
La vis murale droite reste X190/Y7,7, sous le PCB : accès tête/outil vérifié.
Guides latéraux X13/192, Y135,9..156 ; longueur 20,1, profondeur 7.
Crochets supérieurs en L et levée de pose 4 conservés.

## Contrainte de câblage — ne pas masquer cette hypothèse
RPS inchangée X18,8/Y39,94, 50,8 × 76,2, capot et cloison inchangés.
PSU_Y est maintenant explicite : ne plus le calculer à partir de la taille PCB.
Écart latéral carte RPS/PCB 7,02 ; cache/PCB 1,02. L'ancien couloir latéral
de 22 mm ne tient plus. Il n'a pas été simplement supprimé pour faire passer
le contrôle : une réserve de 22 × 76 × 6 est déplacée devant le PCB,
X76,62..98,62, Y40..116, Z42..48. Composants PCB jusqu'à Z38,6.
Sortie RPS et commandes Ø6 passent devant les composants. PG7 X44/68/88,
ligne commune Z49 ; réservations de câbles toujours larges de 18, jusqu'à Z75.
RPS CN1 en bas, CN2 en haut. Secteur séparé à gauche de la cloison X54,5.
Deux PG7 de commande : basse tension / contacts secs, confirmé par l'utilisateur.
Cette implantation SUPPOSE des fiches verticales ou compatibles avec ce passage.
La question sur l'orientation/dépassement des fiches a été posée, pas confirmée.
Ne pas déclarer les vrais connecteurs, courbures et câbles qualifiés ni lancer
une fabrication finale sans montage réel hors tension.
Les faisceaux sont des réservations nominales, pas des connexions électriques.

## Fonctions géométriques à conserver
Joint cordon rond plein Ø2, gorge 2,8 × 1,4, compression géométrique 30 %.
Gorge en U spatial : branches X11,3/198,7 droites, coudes YZ en secteurs
circulaires extrudés R16,8, retour avant Z77,8. Contre-face mobile plane
large de 3,6, à 1 mm du corridor câble. Pas de traverse traversant les câbles.
Cadre fixe relié aux bords ; bride mobile renforcée par goussets à 45°.
Ne pas réintroduire les lofts ouverts ni les dégagements extérieurs aux coins.
Trois vis M4 sous le boîtier X18/105/192, Z83,4, tête fraisée 90° Ø8,5,
longueur 16 tête comprise ; inserts courts Ø6,3 × 4, siège Ø5,6 × 5.
Les têtes sont coniques vers tige Ø4 ; fraises Ø8,5 vers passage Ø4,5.
Têtes/vis/outil et distances à gorge conservés et contrôlés, pas seulement axes.
Pas d'ergots bas, pas de vis visibles ni caches en façade.
Quatre fixations murales : 24/7,7, 190/7,7, 25/150, 185/150.
Deux têtes basses Ø7,5 max × hauteur 4 ; outil Ø6. Deux têtes hautes Ø9.
Le gousset est évidé AU-DESSUS de l'assise seulement ; fond/tablette non perforés.
RPS : cloison X54,5, épaisseur 2,2, Y3,2..34,44, profondeur 70.
Cache avec pattes X24,8/65,6, Y30,94/125,14 ; chicane 3 de recouvrement,
jeu 0,5. Quatre pilotes Ø2,8 et passages Ø3,4 ; retenue à qualifier.
Retrait du cache vers Z+ après dépose du capot extérieur et retrait de ses vis.
Ruban graphite affleurant, retours sur facettes ; signature turquoise à ancrage
élargi derrière la façade. Volumes couleur co-imprimés à origine commune.
Couleurs/matières : noyau ASA clair, ruban ASA-CF, signature ABS turquoise.

## Génération et livraison
Générateur autonome generate_freecad.py, runtime Mac :
/Applications/FreeCAD.app/Contents/Resources/bin/FreeCADCmd
Python système n'a pas FreeCAD/Part. Depuis le dossier du package :
```sh
printf '%s\n' '__file__ = "generate_freecad.py"; exec(compile(open(__file__, "rb").read(), __file__, "exec"))' |
  /Applications/FreeCAD.app/Contents/Resources/bin/FreeCADCmd \
  -u /private/tmp/poolcontroller-freecad-user.cfg \
  -s /private/tmp/poolcontroller-freecad-system.cfg -c
```
Rejouer ensuite validate_existing.py, verify_exports.py, verify_pcb_export.py
avec le même lancement. Exiger pass=true dans les trois JSON correspondants.
FreeCADCmd peut retourner zéro après une exception : seuls les rapports font foi.
Le générateur écrit les exports AVANT les tests ; présence de fichiers ≠ succès.
Avertissements de cache utilisateur sans rapport avec la validité géométrique.
16 FCStd actifs (assemblage + 15 pièces), 15 STEP, 13 STL dont sept coupons.
Les monoblocs sont des références FCStd/STEP, pas des STL.
Le script groupé build_v14_packages.py, conservé dans chaque package, vérifie
les trois rapports, leurs empreintes, la platine identique entre variantes,
le profil Cura et le manifeste ZIP. Ne jamais écraser une archive existante.

## Contrôles et pièges importants
write_validation_report : 117 poses capot+pumps+vis, obstacles fixes complets,
PCB/composants, RPS/cache, fixations murales, BNC, PG7, écrous et quatre routes.
Les solides mobiles sont testés SÉPARÉMENT : un compound superposé créait
un faux positif OCC de 347 mm³ en V12 AliExpress. Ne pas retirer les vis,
les câbles ou les obstacles pour obtenir un succès ; seuil 0,1 mm³ conservé.
La nouvelle réserve de câbles devant PCB est aussi un obstacle dans ces poses.
pcb_interface_checks vérifie présence des quatre supports, passages, disparition
des anciens supports hors nouveaux, PCB/composants et réserve de câbles.
Les 3 mm SOUS PCB sont testés par un volume vertical hors assises des supports,
pas par la distance euclidienne au gousset situé en dehors du bord Y13,5.
Cette distance doit être relue dans le rapport courant ; ce n'est pas une hauteur libre sous PCB.
Conserver les témoins négatifs : intersection PCB volontaire, traverse bloquant
les câbles, trous de paroi, cache sans ouvertures/retour et anciens accès masqués.
verify_pcb_export.py relit PCB, composants, réserve, gabarit et passages BNC,
et les compare à des cotes littérales indépendantes 119,38 / 104,14.
Il vérifie le gabarit STL avec bordure nominale 4 mm sur volume Genius.
verify_exports.py : FCStd/STEP un solide valide, STL fermé/connecté et sans
auto-intersection ; volumes, assemblage stocké, PG7/faisceaux/chicane/accessibilité.
MeshPart : déflexion linéaire 0,05, angulaire 0,15, non relative.
safe_refine conserve le BRep original si removeSplitter l'invalide.
BoundBox OCC parfois surestimée : les dimensions STL font foi.
Essai rapide trial_v14_layout.py ; les anciens diagnostics conservés ne sont pas
des validations V14 et leurs chemins historiques peuvent manquer.
preview_interface.FCMacro optionnelle dans FreeCAD GUI, ouvre ses propres documents.
La référence visuelle sous references/ n'est pas un dessin coté.
Lire README, ASSEMBLY, FABRICATION, BOM, PROMPT_MODELISATION_IA, REFERENCES,
RPS_WIRING et les JSON avant modification. Mettre ces informations à jour.

## Impression et limites
Profil Cura PLA+ avec supports inchangé, nom V12 conservé, sous cura/.
Genius 0,4, SUNLU PLA+, Cura 5.9 ; pas un profil ASA final ni du G-code.
Supports Normal/Partout 50°, 12 %, interface, Z0,20 / XY0,60, priorité Z.
La Genius mono-extrudeur ne co-imprime pas automatiquement les matériaux.
À 210 × 175, bordure 4 → 218 × 183 ; vérifier aussi supports et ligne de purge.
Pas de promesse sans supports avant contrôle de l'aperçu du trancheur.
Contrôles discrets, pas simulation de déformation/serrage/fluage ou indice IP.
Plugs/cosses, diamètres et courbures réels, composants sous PCB et retenue M3
restent à qualifier. La RPS et son cache ne sont pas une isolation certifiée :
distances, ventilation et support isolant à confronter à la notice Mean Well.
Ne pas annoncer une autorisation de mise sous tension à partir de ces rapports.
