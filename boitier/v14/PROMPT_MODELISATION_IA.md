# Prompt de modélisation — PoolController V14
Concevoir un boîtier mural moderne sobre conforme à la référence visuelle :
coque claire facettée, biseaux bas plus modestes, ruban graphite affleurant avec
léger retour sur les facettes, fine signature turquoise devant à ancrage caché.
Corps 210 × 175 × 94 mm. Platine arrière murale couvrant toute la silhouette,
tablette porte-connecteurs solidaire, capot cloche démontable sans fond.
Aucune vis ou cache-vis en façade ; trois M4 fraisées uniquement sous le boîtier.
Crochets supérieurs rigides en L, levée/dépose 4 mm, guides continus Y135,9..156.
Platine commune aux deux variantes de pompe.

PCB entier 119,38 × 119,38 × 1,6 à droite X76,62/Y13,5, arrière Z12.
V14 : garder accès de la vis murale droite basseX190/Y7,7 : têteØ7,5,
marge d'insertionPCB au moins2 (nominal2,05) et tigeØ6 (nominal2,80).
Axes de passagesBNC X/Z inchangés ; référence du corps suit lePCB+1,5 enY,
saillie extérieure indicative19, engagement fileté réel à confirmer.
Ne jamais modifier le PCB ou les positions locales des composants/connecteurs.
Quatre trous sur carré 104,14, à 7,62 des bords ; supports Ø7,5 × hauteur8,8.
Alésage provisoire Ø3,2, retenue M3/inserts non qualifiée et à résoudre avant série.
BNC en D X123,102/159,424 Z19,8, Ø13 / méplat5,70 ; décalages locaux à confirmer.
RPS-30-12 à gauche X18,8/Y39,94, 50,8 × 76,2, CN1 bas/CN2 haut, cache ventilé.
Cloison X54,5 T2,2 Y3,2..34,44 et profondeur70, chicane3 avec jeu0,5.
PG7 X44/68/88, ligne Z49 ; secteur à gauche, deux commandes basse tension/contacts secs.
Garder leur réserve de diamètre18, écrous internes8 et corridor jusqu'à Z75.
Réserve de câblage 22 × 76 × 6 devant bord gauche du PCB, Z42..48,
pas entre cartes : fiches verticales SUPPOSÉES, orientation réelle à confirmer.
Ne pas inventer une empreinte de fiche, un raccordement ou une certification électrique.

Joint cordon rond Ø2, gorge2,8 × profondeur1,4, compression géométrique30%.
Trajet spatial continu, branches droites, coudes circulaires exacts, contre-face plane.
Aucune traverse bloquant les câbles ; goussets obliques continus soutenant les brides.
Trois M4 X18/105/192 Z83,4, têtesØ8,5 à90°, longueur16, inserts courtsØ6,3×4,
siègesØ5,6×5. Quatre fixations murales accessibles avec PCB en place.
Supports, inserts, têtes entières, outils et passages de montage contrôlés.

Pompes fixées en façade, raccords/tuyaux vers le bas, aucun tuyau dans le boîtier.
Variante AliExpress : moteurØ28,9, passageØ29,5, derrière bride38,4,
bornes12V incluses43,6, M3 entraxe48,2 même hauteur que moteur, passageØ3,4.
Bornes/cosses radiales non mesurées ; tête visuelle approximative uniquement.
Variante Dosita MP1 : dimensions/interface héritées du package MP1, entraxe38,
décalage vertical26, passage moteur27,5, passage vis4,5 provisoire à mesurer.
Ne jamais mélanger ces interfaces ou les anciens packages V12 erronés pour le PCB.

Livrer FCStd, STEP, STL couleur à origine commune, coupons, notices, générateur,
contrôles relisant les exports et documentation de reprise complète.
Tester 117 poses capot+pumps+vis avec obstacles complets et câblage, 21 poses
de retrait cache RPS, présence de parois/joint et témoins négatifs.
Préserver les limites : contrôles discrets, pas déformation/serrage/fluage/IP,
fiches et fils réels non qualifiés, sécurité électrique à revoir séparément.
