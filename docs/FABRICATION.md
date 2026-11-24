# Contraintes de fabrication des cartes

Ce que **l'atelier d'assemblage et la personne qui met l'appareil en service** doivent
respecter pour que la carte mesure juste. Ce document ne décrit pas la conception — elle est
dans [`feature-215`](../specs/features/todo/feature-215-conception-du-pcb-v3.md) — mais ce
qui se passe **entre le fichier et l'appareil qui fonctionne**.

⚠️ **État au 2026-11-24 : document ouvert avec le chantier du PCB v3.** Plusieurs lignes sont
des contraintes **connues mais pas encore éprouvées sur une carte réelle**. Chaque
contrainte porte sa source ; celles qui restent à vérifier sont marquées ⏳.

**Règle de tenue** : toute contrainte découverte en fabrication ou en mise en service
s'ajoute ici, avec sa source et la date. Une contrainte sans source n'est pas une
contrainte, c'est une habitude.

---

## 1. Pourquoi ces cartes ne se fabriquent pas comme les autres

L'électrode de pH est un capteur à **très haute impédance** : une source de tension derrière
une résistance de l'ordre de la centaine de mégohms. Un courant parasite d'un nanoampère y
produit 100 mV, soit plus d'une unité et demie de pH. ⇒ **Une salissure invisible sur le
circuit suffit à fausser la mesure.**

Tout ce qui suit en découle. ⛔ Une carte de mesure qui « marche » à la sortie de l'atelier
peut mesurer faux : le défaut ne se voit pas, il se mesure.

---

## 2. Nettoyage après soudure — ⛔ OBLIGATOIRE sur la carte de mesure

**Source : documentation Atlas Scientific du pH OEM, V 4.7, p. 4 et p. 36.**

> *« After soldering the OEM circuit to your PCB, all flux residue MUST be removed. Failure
> to do so could lead to a high impedance short, making it impossible to get accurate
> readings. DO NOT SKIP THIS STEP »*

**Méthodes admises par Atlas** — l'une des trois :
1. bac à **ultrasons** ;
2. **produit de nettoyage de flux** du commerce ;
3. **bain d'alcool** d'environ 20 minutes.

**Le signe d'un nettoyage manqué** (Atlas, p. 4) : la lecture du pH reste **bloquée à 0, à 7
ou à 14**.

### ⚠️ Ce que fait l'atelier par défaut ne suffit pas

**JLCPCB assemble avec un flux « sans nettoyage »** et considère le film résiduel — fin,
légèrement collant, transparent ou jaune clair — comme **normal et conforme**
([leur page d'aide](https://jlcpcb.com/help/article/common-pcba-after-sales-issues-and-faq),
consultée le 2026-11-24).

⇒ **Conforme pour l'atelier, ⛔ inacceptable pour cette carte.** Les deux affirmations sont
vraies en même temps : le flux « sans nettoyage » est sans danger pour une carte ordinaire,
pas sous un capteur à haute impédance.

⏳ **À obtenir de l'atelier, par écrit, avant la première commande** : propose-t-il un
nettoyage, par quelle méthode, et le garantit-il sous le module ? ⛔ Aucune option de ce
type n'a été trouvée sur leur site le 2026-11-24 — absence de preuve, pas preuve d'absence.

**Tant que la réponse n'existe pas : le nettoyage se fait à la réception, sur CHAQUE carte
de mesure**, avant tout essai.

### ⚠️ Précautions du nettoyage lui-même

- ⚠️ **Un mauvais nettoyage est pire que pas de nettoyage** : un résidu de flux « sans
  nettoyage » mal dissous **s'étale** au lieu de partir, et se loge sous les composants.
  ⇒ Bain complet et rinçage, ⛔ pas un coup de pinceau.
- ⏳ **Ultrasons** : certains composants ne les supportent pas (résonateurs, capteurs
  mécaniques). À vérifier pour **chaque** composant de la carte de mesure avant d'employer
  cette méthode. ℹ️ C'est un avantage de la carte de mesure séparée : l'horloge, qui contient
  un résonateur, est sur la carte principale.
- **Séchage complet** avant mise sous tension et avant tout enrobage.

### Si un module est soudé ou repris à la main

(Atlas, p. 36)
- ⛔ **Pas de fil de soudure à âme de résine.**
- **Le moins de flux possible** ; ⛔ ne jamais laisser du flux liquide couler **sous** le module.
- Nettoyage complet ensuite, comme ci-dessus.

---

## 3. Protection contre l'humidité

**Source : documentation Atlas du pH OEM, p. 37.**

> *« Humidity levels above 65% can affect the pH readings. »*

Un local technique de piscine dépasse couramment ce seuil. Atlas préconise d'**enrober le
module dans de la résine époxy TRANSPARENTE**.

⛔ **Jamais de résine colorée, jamais de résine noire d'enrobage pour électronique** :
Atlas indique qu'elles **endommagent gravement** le module.

**L'ordre des opérations est imposé**, et l'enrobage est **irréversible** :
1. souder le module ;
2. nettoyer (section 2) ;
3. ⭐ **vérifier que la mesure est juste** (section 5) ;
4. **seulement alors**, enrober.

⏳ Référence de résine, épaisseur et étendue de l'enrobage : à définir sur la première carte.

---

## 4. Pose des modules Atlas

- Les modules sont livrés **en tube**, prévus pour la pose automatique ; Atlas donne un
  profil de refusion (pH OEM, p. 39 — ⏳ c'est un graphique, ses valeurs restent à relever
  et à transmettre à l'atelier).
- ℹ️ **Aspect** (Atlas, p. 40) : les modules peuvent présenter un excès de soudure ou des
  composants légèrement de travers ; chaque unité est testée. ⇒ **Prévenir l'atelier**, pour
  que son contrôle optique ne les rejette pas.
- ⛔ **Les broches marquées « non connectées » restent en l'air** — jamais reliées à
  l'alimentation ni à la masse. Un pont de soudure à cet endroit est un **défaut**.
- ⛔ **Aucun via sous le module** (règle de conception, à contrôler sur la carte reçue).

### Pièces fournies à l'atelier

Les modules Atlas ne sont pas au catalogue de l'atelier : ils lui sont **envoyés**
([procédure JLCPCB](https://jlcpcb.com/help/article/How-to-consign-parts-to-JLCPCB),
consultée le 2026-11-24).

- Envoi depuis l'étranger ⇒ entrepôt de **Hong Kong** ; ⚠️ le mauvais canal fait refuser le colis.
- **Transport et dédouanement à la charge de l'expéditeur.**
- ⛔ **L'atelier ne teste PAS les pièces fournies**, ni avant ni après la pose.
- ⏳ L'atelier suggère d'**étuver** les modules sensibles à l'humidité avant la pose. ⚠️ À
  valider auprès d'Atlas avant de l'accepter pour leurs modules.
- Prévoir **des modules en surplus** : une perte à la pose ne se rattrape pas sur place.

### L'isolateur ADM3260

Au catalogue de l'atelier sous la référence **C208558**
([fiche JLCPCB](https://jlcpcb.com/partdetail/AnalogDevices-ADM3260ARSZ/C208558) ·
[fiche LCSC](https://www.lcsc.com/product-detail/C208558.html)).

⚠️ **Relevé du 2026-11-24, à refaire avant chaque commande** : une soixantaine de pièces en
stock, entre 8 et 16 $ l'unité selon la quantité et la page consultée. ⇒ **Stock faible** :
deux par carte, une série de trente cartes le vide. Prévoir la commande anticipée ou
l'approvisionnement ailleurs.

---

## 5. Contrôle à la réception — ⛔ avant tout enrobage, avant toute livraison

L'atelier ne vérifie pas la mesure. **Chaque carte de mesure passe ce contrôle** :

1. **Examen visuel** : aucun résidu autour et sous les modules, aucun pont sur les broches
   non connectées.
2. **Nettoyage** si l'atelier ne l'a pas garanti (section 2).
3. **Essai dans un verre** : sonde seule dans une solution tampon, à l'écart de tout autre
   appareil. La lecture doit se **stabiliser** et **bouger** d'un tampon à l'autre.
   ⛔ Une lecture figée à 0, 7 ou 14 = résidu de flux ⇒ retour au nettoyage.
4. **Mesure du bruit** : l'écart entre la plus haute et la plus basse des 13 dernières
   mesures brutes, relevé et **noté avec le numéro de la carte**.
5. **Enrobage** (section 3), puis **refaire l'essai 3**.

⏳ Seuils d'acceptation chiffrés : à fixer sur les premières cartes réelles.

---

## 6. Ce qui ne relève PAS de la fabrication

- Les **règles de routage** contre les perturbations : `feature-215`, section
  « Perturbations électromagnétiques ».
- La **pose des câbles de sonde chez le client** (blindés, jamais avec le 230 V ni les
  pompes) : consigne d'installation, à porter dans la notice.
- La **conformité réglementaire** d'un appareil vendu : sujet distinct, à instruire avec un
  laboratoire.

## Sources

- Atlas Scientific — [documentation du pH OEM](https://files.atlas-scientific.com/oem_pH_datasheet.pdf) (V 4.7) · [documentation de l'ORP OEM](https://files.atlas-scientific.com/ORP_oem_datasheet.pdf), lues le 2026-11-24
- JLCPCB — [défauts courants après assemblage](https://jlcpcb.com/help/article/common-pcba-after-sales-issues-and-faq) · [envoyer ses propres pièces](https://jlcpcb.com/help/article/How-to-consign-parts-to-JLCPCB)
