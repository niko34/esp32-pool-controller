# Câblage V14 — hypothèses et limites

Les trois PG7 sont alignés sur Z49, X44 (secteur), X68 (commande filtration), X88 (commande éclairage). Les commandes sont basse tension / contacts secs, selon confirmation de l'utilisateur. Ne pas y introduire du secteur sans nouvelle étude.

Le RPS-30 conserve X18,8/Y39,94 : entrée CN1 en bas, sortie CN2 en haut. Son cache et ses quatre pattes restent inchangés depuis V12. Deux encoches ouvertes largeur14/hauteur18, axeX44, permettent de poser le cache après raccordement ; elles ne sont pas des empreintes de fiches mesurées.

La cloison X54,5, épaisseur2,2, monte depuis la platine jusqu'à Z73,2. Elle se trouve à droite du PG7 secteur et sous le cache RPS ; chicane avec recouvrement3 et jeu0,5. C'est une organisation mécanique des chemins, pas un compartiment fermé ni une isolation électrique certifiée.

## Nouveau cheminement basse tension

Le PCB119,38 occupe X76,62..196 et Y13,5..132,88. L'ancien couloir de22 entre PCB et RPS disparaît. L'écart mécanique au cache n'est que1,02 ; il ne peut pas héberger des fiches latérales ordinaires.

La réserve PCBServiceBay est maintenant devant le bord gauche du PCB : X76,62..98,62, Y40..116, Z42..48. L'enveloppe provisoire des composants s'arrête à Z38,6. Le volume disponible pour les câbles est distinct du PCB et des pompes.

Routes nominales Ø6 dans l'assemblage :

- Sortie RPS : passe au-dessus des cartes, via Y137,5, puis avance à Z45 avant de rejoindre X85/Y112.
- Filtration : PG7X68/Z49, puis X82, virage devant la carte et terminaison X82/Y70/Z45.
- Éclairage : PG7X88/Z49, puis X94, terminaison X94/Y90/Z45.
- Secteur : PG7X44/Z49 vers l'encoche basse du cache, à gauche de la cloison.

Les bouts des routes ne sont PAS des raccordements terminaux. Les cylindres et sphères décrivent des volumes réservés, pas la flexion d'un vrai câble. L'orientation et l'encombrement des prises du PCB branchées ne sont pas connus : **fiches verticales ou compatibles avec ce cheminement indispensables**. Un connecteur latéral dépassant vers le RPS peut rendre cette implantation impossible.

Écrous PG7 réservés Ø18×8 ; corridors conservés, pas rétrécis pour faire passer les tests. Les chemins, la réserve PCB et le retrait du cache RPS sont contrôlés géométriquement, mais pas les fiches terminales, leurs rayons de courbure, les cosses moteur ni leur souplesse.

## Qualification avant fabrication

Montage à blanc hors tension avec PCB, alimentation, fiches, presse-étoupes et câbles réels. Vérifier accès aux fixations, retrait du cache RPS, puis pose et retrait du capot sans pincement. Vérifier aussi maintien mécanique des câbles et absence de passage dans le joint. Les petites marges mécaniques ne constituent pas des distances d'isolation électrique validées.

Plan de référence conservé : references/RPS-30-SPEC-2024-11-22.pdf. [Plan Mean Well RPS-30](https://www.meanwell.com/Upload/PDF/RPS-30/RPS-30-SPEC.PDF) ; [notice d'installation](https://www.meanwell.com/Upload/PDF/PCB_EN.pdf). L'intégration électrique, la ventilation, le matériau et la température de fonctionnement doivent être qualifiés pour le système réel. Aucun indice IP ni conformité électrique n'est annoncé.

