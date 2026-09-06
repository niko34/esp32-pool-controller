---
name: Migration EZO pH et ORP
description: Migration des modules pH/ORP de DF Robot + ADS1115 vers Atlas Scientific EZO I2C
type: project
---

Module pH remplacé par Atlas EZO pH (I2C addr 0x63), module ORP par Atlas EZO ORP (I2C addr 0x62). Plus d'ADS1115.

**Why:** Nouveau PCB v2 sans ADS1115, modules EZO directement sur le bus I2C.

**How to apply:** 
- La calibration pH se fait via commandes EZO : `Cal,mid,7.00` (pH 7) puis `Cal,low,4.00` (pH 4)
- La calibration est stockée dans le module EZO lui-même (pas en EEPROM ESP32)
- Lecture non-bloquante via state machine dans `sensors.cpp` : T → R pH → lire pH → T → R ORP → lire ORP (cycle 5s)
- Slope de calibration exposé via `ph_slope` (ex: "99.7%,100.3%") dans /data, /get-config et réponses calibration
- `ph_voltage_mv`, `ph_cal_vn`, `ph_cal_va` supprimés — remplacés par `ph_slope`
- ORP : valeur retournée directement en mV par l'EZO, correction slope/offset optionnelle toujours disponible
- Librairies supprimées : DFRobot_PH, Adafruit ADS1X15
