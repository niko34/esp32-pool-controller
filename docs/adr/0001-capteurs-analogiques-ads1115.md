# ADR-0001 — Capteurs pH/ORP analogiques via ADS1115 + DFRobot_PH

- **Statut** : Accepté
- **Date** : 2024 (date d'origine du projet)
- **Spec(s) liée(s)** : aucune (décision antérieure aux specs documentaires)

## Contexte

Le contrôleur a besoin de mesurer pH et ORP en continu à partir de sondes standard. Deux familles de capteurs sont disponibles sur le marché grand public :

1. **Sondes analogiques BNC** reliées à une carte d'adaptation (DFRobot SEN0161 pour le pH, sonde ORP BNC) branchées sur un ADC. L'ESP32 n'embarque qu'un ADC 12 bits bruité, d'où l'ajout d'un **ADS1115** (ADC 16 bits I2C).
2. **Modules I2C tout-intégrés** (type Atlas Scientific EZO) qui gèrent eux-mêmes la calibration, la compensation de température et le filtrage, et retournent une valeur finale via I2C.

## Décision

Le projet utilise **des sondes analogiques BNC + ADS1115 I2C** pour les mesures pH et ORP.

- pH : `DFRobot_PH` (EEPROM calibration, compensation température) sur le canal A0 de l'ADS1115.
- ORP : lecture brute sur un autre canal de l'ADS1115, offset + slope calibrés côté firmware (NVS).
- Température : sonde 1-Wire DS18B20 (`DallasTemperature`), indépendante de l'ADS1115.

## Alternatives considérées

- **Modules Atlas EZO I2C** (rejeté à ce stade) — plus chers (≈ 80 €/module × 2), moins de visibilité sur le signal brut (utile pour diagnostiquer une sonde fatiguée), protocole série/I2C propriétaire plus verbeux.
- **ADC interne ESP32** (rejeté) — 12 bits bruités, sensible au bruit d'alimentation ; la sonde pH sort une tension faible (~500 mV max) autour de 1.65 V, résolution insuffisante pour un dosage propre.

## Conséquences

### Positives
- Coût total matériel réduit (ADS1115 ≈ 5 €, sondes BNC ≈ 15 €/unité).
- Accès au signal **brut** exposé dans l'UI (utile pour détecter une sonde morte : valeur figée, bruit anormal).
- Calibration entièrement maîtrisée côté firmware (formule `ORP_final = ORP_brut × slope + offset`, voir [`config.h`](../../src/config.h)).

### Négatives / dette assumée
- Compensation de température uniquement pour le pH (via `DFRobot_PH`), pas pour l'ORP.
- Calibration ORP manuelle (l'utilisateur entre la valeur de la solution de référence), pas d'auto-détection du tampon.
- Les pins et fréquence d'échantillonnage ADS1115 sont en dur dans `sensors.cpp` (8 SPS → 125 ms/échantillon).

### Ce que ça verrouille
- La bibliothèque `DFRobot_PH` stocke la calibration en EEPROM I2C : un factory reset NVS **n'efface pas** la calibration pH (c'est une partition distincte, voir `clearPhCalibration()` dans [`sensors.h`](../../src/sensors.h)).
- Un passage futur aux modules EZO nécessitera un ADR superseding celui-ci et une migration de la structure `MqttConfig` (champs `orpCalibrationOffset`, `orpCalibrationSlope`, etc.).

## Références

- Code : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp), [`src/config.h`](../../src/config.h) struct `MqttConfig`
- Dépendances : `DFRobot_PH`, `Adafruit_ADS1X15`, `OneWire`, `DallasTemperature` (voir `platformio.ini`)
