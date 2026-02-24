# Guide de Calibration - ESP32 Pool Controller

## Fréquence de Calibration

- **Calibration initiale** : Obligatoire avant première utilisation
- **Contrôle mensuel** : Vérification avec solutions étalons
- **Recalibration complète** : Tous les 3 mois

## Matériel Nécessaire

### Solutions Étalons pH
- **pH 4.01** (solution acide) - ~5€
- **pH 7.00** (solution neutre) - ~5€

> La librairie DFRobot_PH ne supporte que pH 4.0 et 7.0.

### Solution Étalon ORP
- **Solution ORP 470 mV** (à 25°C) - ~15€

### Accessoires
- Béchers propres
- Eau distillée (rinçage)

Marques recommandées : Hanna Instruments, Milwaukee, Extech

## Dépannage

### pH : Lecture Instable
- Sonde usée (électrode de verre fissurée)
- Bulles d'air sur électrode → tapoter légèrement
- Mauvais contact électrique → vérifier câblage
- Si >1 an d'utilisation : remplacer sonde

### ORP : Réponse Lente
- Normal pour ORP (électrochimie lente, attendre 2-3 min)
- Nettoyer électrode de platine (solution HCl 10%)
- Polir légèrement avec papier très fin (600 grit)

### Valeurs Hors Limites
- **pH toujours 7.0** : Sonde non connectée ou HS
- **ORP toujours 0** : Électrode de référence sèche ou cassée

## Bonnes Pratiques

- Ne jamais toucher l'électrode de verre (pH) avec les doigts
- Stocker sonde pH dans solution KCl 3M (jamais eau distillée)
- Rincer à l'eau distillée entre chaque solution
- Remplacer électrolyte interne si sonde rechargeable
