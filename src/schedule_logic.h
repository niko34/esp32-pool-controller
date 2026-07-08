#ifndef SCHEDULE_LOGIC_H
#define SCHEDULE_LOGIC_H

// =============================================================================
// schedule_logic — Décision de planning horaire PURE (feature-038)
// =============================================================================
// Logique de planning (parsing "HH:MM", appartenance à une plage horaire, calcul
// du créneau auto selon la température, décision marche/arrêt) extraite de
// filtration.cpp pour la rendre testable en natif (sans ESP32, sans RTC, sans
// FreeRTOS). Module générique : réutilisable pour l'éclairage ou tout autre
// planning horaire.
//
// INVARIANT DIRECTEUR : ce module ne change AUCUN comportement de filtration.
// Il reproduit EXACTEMENT la logique d'origine (characterization refactor).
// Toute la collecte des entrées (RTC, millis(), NVS, deadband, timeout 4h,
// armement de stabilisation, publishState, UART, digitalWrite) reste dans la
// coquille filtration.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <vector>/<FS.h>/<String>.
// On utilise les en-têtes C <stdint.h>/<math.h> (et non <cstdint>/<cmath>) : la
// libc++ n'est pas disponible sur l'hôte de CI/dev, or le module doit compiler
// en natif.
// =============================================================================

#include <stdint.h>
#include <math.h>

// Convertit une chaîne "HH:MM" en minutes depuis minuit.
// Reproduit EXACTEMENT FiltrationManager::timeStringToMinutes :
//   - longueur < 5 OU caractère [2] != ':' → -1 (invalide)
//   - hh = 2 premiers chiffres, mm = 2 chiffres après le ':'
//   - hh hors [0,23] OU mm hors [0,59] → -1
//   - sinon hh*60 + mm
// Pour des entrées normalisées "HH:MM", reproduit la sémantique
// substring().toInt() d'origine (parse des 2 chiffres ; positions garanties par
// la garde [2]==':').
int timeStringToMinutes(const char* hhmm);

// true ssi `now` (minutes depuis minuit) est dans la plage [start, end).
// Reproduit EXACTEMENT FiltrationManager::isMinutesInRange (équivalent aussi de
// LightingManager::isMinutesInRange via le paramètre `equalMeansAlways`) :
//   - start==-1 OU end==-1 → false (garde AVANT le test start==end : -1==-1 ne
//     doit jamais renvoyer equalMeansAlways)
//   - start==end → `equalMeansAlways` (filtration: false=plage invalide ;
//     éclairage: true=allumé toute la journée)
//   - start<end → now>=start && now<end
//   - sinon (start>end, plage à cheval sur minuit) → now>=start || now<end
// equalMeansAlways=false par défaut → comportement filtration inchangé.
bool isMinutesInRange(int now, int start, int end, bool equalMeansAlways = false);

// Minutes restantes de la plage courante [start, end), BORNÉES À MINUIT
// (feature-011, répartition scheduled). Règles :
//   - hors plage ou plage invalide → 0 : la garde délègue à isMinutesInRange
//     (equalMeansAlways=false), qui renvoie déjà false pour start==-1, end==-1
//     ou start==end (plage invalide filtration) ;
//   - start<end (plage simple) → end - nowMin ;
//   - start>end (plage à cheval sur minuit) :
//       * nowMin >= start (partie du soir) → 1440 - nowMin, BORNÉ À MINUIT :
//         les compteurs journaliers se réinitialisent à minuit, l'horizon de
//         répartition ne le franchit donc jamais ;
//       * nowMin < end (partie du matin) → end - nowMin.
// Résultat toujours >= 1 quand nowMin est dans la plage (fin exclusive).
int remainingRangeMinutes(int nowMin, int startMin, int endMin);

// Créneau horaire en minutes depuis minuit (planning auto).
struct ScheduleWindow {
  int startMin;
  int endMin;
};

// Calcule le créneau de filtration auto selon la température.
// Reproduit EXACTEMENT le calcul de FiltrationManager::computeAutoSchedule :
//   - tempC < 0 → ramené à 0
//   - durationHours = tempC / 2, borné à [1, 24]
//   - startHour = pivot - duration/2 ; endHour = start + duration
//   - wrap des deux heures dans [0, 24)
//   - conversion heure flottante → minutes avec arrondi/carry identique :
//       h = (int)hour ; m = (int)round((hour-h)*60) ;
//       if (m>=60) { m-=60 ; h=(h+1)%24 } ; minutes = h*60 + m
// (Équivalent du float→"HH:MM"→re-parse d'origine, arrondi reproduit à l'identique.)
ScheduleWindow computeAutoWindow(float tempC, float pivotHour);

// Décision marche/arrêt de filtration (extrait pur de la décision de update()).
// Reproduit EXACTEMENT, avec le Mode Boost (feature-053) en priorité MAXIMALE :
//   - boostForce → true (turnover maximal pendant le Boost, chemin DÉDIÉ
//     indépendant du forceOn utilisateur et de son timeout)
//   - sinon forceOn → true
//   - sinon forceOff → false
//   - sinon (mode=="manual" || mode=="auto") →
//       haveTime ? isMinutesInRange(nowMin, startMin, endMin) : currentlyRunning
//   - sinon → false
// CONDITION pool-chemistry : quand haveTime=false sans forçage, renvoie
// currentlyRunning tel quel (pas de faux start/stop pendant OTA / perte RTC).
// `mode` doit arriver en minuscules (la coquille applique toLowerCase avant).
bool decideFiltrationRun(bool boostForce, const char* mode, bool forceOn, bool forceOff,
                         bool haveTime, int nowMin, int startMin, int endMin,
                         bool currentlyRunning);

// Décision marche/arrêt de l'éclairage (extrait pur de la décision de
// LightingManager::update()). Reproduit EXACTEMENT :
//   - manualOverride → enabledFlag
//   - sinon scheduleEnabled →
//       haveTime ? isMinutesInRange(nowMin, startMin, endMin, true) : currentlyOn
//   - sinon → enabledFlag
// L'éclairage utilise equalMeansAlways=true (start==end → allumé toute la
// journée). Quand haveTime=false sous programmation, conserve l'état courant
// pour éviter un faux toggle pendant un OTA / perte RTC.
bool decideLightingOn(bool manualOverride, bool enabledFlag, bool scheduleEnabled,
                      bool haveTime, int nowMin, int startMin, int endMin,
                      bool currentlyOn);

#endif // SCHEDULE_LOGIC_H
