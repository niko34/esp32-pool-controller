# PoolController V14 — Dosita MP1, PCB corrigé

Révision du 7 octobre 2026. Unités : mm. Corps **210 × 175 × 94**, inchangé.
V14 : PCB remonté de **1,5 mm** par rapport à V13, fixations murales inchangées.
Marge de passage de la tête de vis basse droite : **0,55 → 2,05 mm** ;
tige de tournevisØ6 : **1,30 → 2,80 mm**, capot retiré.
Ne pas mélanger les pièces V13/V14. Voir REVISION_NOTES.md.
Ouvrir freecad/PoolControllerV14MP1_Assembly.FCStd. Les anciens packages V12 sont conservés séparément ; leur PCB et leurs entraxes sont erronés. Ne pas mélanger V12 et V14.

## PCB et platine

PCB entier **119,38 × 119,38 × 1,6**, aucune réorganisation des composants. Quatre fixations sur entraxes horizontal et vertical **104,14**, centres à7,62 des bords. Carte déplacée àX76,62/Y13,5 ; entretoises8,8, arrièrePCB Z12. AxesX84,24/188,38 etY21,12/125,26.

PassagesPCB lissesØ3,2 : diamètre et retenue des visM3/inserts/écrous restent à qualifier. Les BNC suivent la carte àX123,102/159,424, Z19,8, décalages locaux historiques à vérifier. Guides latéraux au-dessus du PCB, Y135,9..156. RPS, cache, cloison, joint et fixations murales restent en place. PG7 X44/68/88, tous àZ49.

La platine est identique à celle de la variante AliExpress V14, contrôlée sur les solides relus. Les pompes MP1 conservent l'interface nominale précédente : voir PUMP_MP1.md. Marge axiale nominale devant les composantsPCB **10,5 mm**. Les cotes réelles des pompes et fixations doivent être confirmées.

## Limite de câblage importante

Le PCB plus large réduit l'écart au cacheRPS à1,02. L'ancien couloir latéral22 ne tient plus. La réserve22×76×6 est déplacée devant le bord gauche du PCB, Z42..48 ; sortieRPS et commandes passent devant les composants.

**Cela suppose des fiches verticales ou compatibles avec ce passage. Leur orientation et encombrement branché ne sont pas confirmés.** Cette version est une implantationCAD conditionnelle, pas une qualification des raccordements réels ni une autorisation de fabrication définitive. Lire SESSION_HANDOFF.md et RPS_WIRING.md.

## Package

16 FCStd (assemblage +15 pièces), 15 STEP, 13 STL dont7 coupons et un nouveau gabaritPCB119,38/104,14. Composants achetés non imprimables, monoblocs en FCStd/STEP seulement. Importer les volumes couleur à origine commune sans recentrage indépendant.

Générateur, documentation de reprise, scripts et rapports inclus. Exiger pass=true dans validation_report.json, exports_reloaded.json, pcb_reloaded.json et common_plate_reloaded.json. Quelques anciens libellés internes AliExpress/M3 sont hérités ; les paramètres et solides MP1 font foi, ne pas acheter des vis sur ces libellés.

Le profil cura/PoolController_V12_Capot_SUNLU_PLAPlus_Genius_0.4_Supports.curaprofile reste le profil prototypePLA+ Genius0,4 avec supports. NomV12 conservé ; il ne contient pas de modèle ni de G-code. Retrancher les volumesV14.

Taille compatible avec le volume nominal Genius220×220×250 : avec bordure4mm, empreinte218×183. Contrôler également supports, purge et zones interdites dans Cura. La Genius mono-extrudeur ne co-imprime pas automatiquement les matériaux/couleurs. PLA+ pour prototype uniquement, pas pour installation chaude définitive. Aucun indiceIP ni conformité électrique/mécanique annoncé.
