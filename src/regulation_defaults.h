#ifndef REGULATION_DEFAULTS_H
#define REGULATION_DEFAULTS_H

// =============================================================================
// feature-088 (v2.31.0) puis feature-090 (v2.32.0) — VALEURS D'USINE des
// réglages de RÉGULATION, désormais PAR TAILLE DE BASSIN
// =============================================================================
// SOURCE UNIQUE des 7 réglages de « tuning » restaurables par le bouton
// « Restaurer les valeurs par défaut » (route POST /reset-regulation-defaults)
// et PRÉSÉLECTIONNÉS par le réglage « Taille du bassin » (feature-090).
// Les initialiseurs de MqttConfig (config.h), la restauration
// (restoreRegulationDefaults, config.cpp), l'application au changement de
// taille (/save-config) et les objets `regulation_defaults[_by_size]` de
// /get-config référencent TOUS cette table : au prochain recalibrage
// (précédent : feature-087), une seule colonne change et rien ne peut diverger.
//
// INVARIANT ABSOLU (feature-090) : la taille du bassin ne sert qu'à
// PRÉSÉLECTIONNER ces défauts, modifiables ensuite. Elle n'entre JAMAIS dans
// un calcul de dosage, de débit, de quota ou de correction — la régulation
// continue d'apprendre la consommation réelle (anneau ph_load7, feedforward).
//
// PÉRIMÈTRE ARBITRÉ par pool-chemistry (visa du 31/08/2026) : uniquement du
// tuning. JAMAIS de calibration de pompe, de type de produit (phCorrectionType),
// de mode de régulation, de trace phAutoManual*, d'offset pH, de SafetyLimits
// ni de sélection de pompe — ces champs décrivent un ÉTAT physique ou une
// SÉCURITÉ, pas un réglage.
//
// HEADER PUR (compilable en natif) : n'inclut que constants.h (self-contained)
// et <string.h>, pour que les static_assert de bornes, la sérialisation de la
// taille et la copie des champs soient atteignables par `pio test -e native`
// (ADR-0017).

#include <string.h>     // strcmp (poolSizeFromString)
#include "constants.h"  // kPhTargetMin/Max, kOrpTargetPlausibleMin/Max (header pur)

// =============================================================================
// feature-090 — TAILLE DU BASSIN (sérialisation stable NVS/JSON)
// =============================================================================
// Trois classes : Small (< 20 m³), Medium (20-60 m³), Large (> 60 m³). Les
// valeurs numériques (0/1/2) indexent kRegulationDefaultsBySize — ne pas les
// réordonner sans retoucher la table ET les static_assert de monotonie.
enum class PoolSize : uint8_t { Small = 0, Medium = 1, Large = 2 };

// Chaîne stable ("small"/"medium"/"large") pour la NVS (clé pool_size) et le
// JSON (/get-config, /save-config).
inline const char* poolSizeToString(PoolSize s) {
  switch (s) {
    case PoolSize::Small: return "small";
    case PoolSize::Large: return "large";
    case PoolSize::Medium:
    default:              return "medium";
  }
}

// Parse tolérant : nullptr, chaîne inconnue ou NVS corrompue → Medium, la
// colonne des défauts v2.30.0 (zéro changement de comportement pour la flotte
// existante — cf. migration feature-090).
inline PoolSize poolSizeFromString(const char* s) {
  if (s == nullptr) return PoolSize::Medium;
  if (strcmp(s, "small") == 0) return PoolSize::Small;
  if (strcmp(s, "large") == 0) return PoolSize::Large;
  return PoolSize::Medium;
}

// =============================================================================
// TABLE DES DÉFAUTS PAR TAILLE — visa pool-chemistry du 31/08/2026
// =============================================================================
// Une COLONNE = les 7 réglages de tuning d'une taille de bassin.
// feature-091 (v2.34.0) : stabilizationDelayMin RETIRÉ — la purge des mesures
// attend un PLANCHER fixe (kPurgeFloorMs, constants.h ; l'analyse de
// stabilité a été retirée par feature-139, v2.62.0), le délai n'est plus un
// réglage utilisateur (champ NVS stab_delay conservé, inerte).
struct RegulationDefaults {
  int phInjectionLimitMin;      // Max min d'injection pH par fenêtre d'1 h
  int orpInjectionLimitMin;     // Max min d'injection ORP par fenêtre d'1 h
  const char* regulationSpeed;  // Vitesse PID : "slow"/"normal"/"fast"
  float phTarget;               // Consigne pH
  float orpTarget;              // Consigne ORP (mV)
  int phDailyTargetMl;          // Volume quotidien cible pH, mode programmé (mL)
  int orpDailyTargetMl;         // Volume quotidien cible ORP, mode programmé (mL)
};

// Logique de la table (spec feature-090) : excursion de mélange aveugle
// ~constante (~0,10-0,13 pH) à travers les tailles, capacité journalière qui
// suit le besoin (le délai de mélange n'est plus un réglage, feature-091). Vitesse :
// petit bassin = gain du procédé élevé (chaque mL a plus d'effet) → correction
// douce ; grand bassin → l'inverse. Les consignes pH/ORP ne dépendent PAS de
// la taille (chimie de l'eau, pas volume).
constexpr RegulationDefaults kRegulationDefaultsBySize[3] = {
    // [0] Small (< 20 m³) — EXTRAPOLATION (non mesurée) : limites redescendues
    // à 5 min/h (le 10 min/h de la v2.30.0 donnait ~0,20 pH d'excursion à
    // 20 m³, cf. justification kDefaultPhInjectionLimitMin ci-dessous) et
    // correction douce (gain du procédé élevé).
    {5, 5, "slow", 7.3f, 600.0f, 0, 0},
    // [1] Medium (20-60 m³) — adossée aux MESURES du bassin de référence
    // (30-50 m³, v2.30.0, visa pool-chemistry du 31/08/2026). C'est la colonne
    // dont les kDefault* ci-dessous sont les alias : elle est VERROUILLÉE par
    // static_assert sur les littéraux v2.30.0 (garantie de migration).
    {10, 10, "normal", 7.3f, 600.0f, 0, 0},
    // [2] Large (> 60 m³) — EXTRAPOLATION (non mesurée) : 15 min/h suit le
    // besoin d'un grand volume, correction
    // rapide (gain du procédé faible). ATTENTION : le quota journalier d'usine
    // maxPhMlPerDay = 300 mL/j ≈ 11 min d'injection/jour à 28 mL/min plafonne
    // en pratique bien avant la promesse 15 min/h — protection acceptable,
    // sens sûr (détail dans UPDATE_GUIDE, note feature-090).
    {15, 15, "fast", 7.3f, 600.0f, 0, 0},
};

// Colonne d'une taille — l'UNIQUE chemin de lecture de la table (constexpr :
// utilisable dans les static_assert comme à l'exécution).
constexpr const RegulationDefaults& regulationDefaultsFor(PoolSize s) {
  return kRegulationDefaultsBySize[static_cast<uint8_t>(s)];
}

// ==== Alias historiques (feature-088) = colonne MEDIUM ====
// Les initialiseurs de MqttConfig (config.h) et les défauts de lecture NVS
// référencent ces noms : ils restent INCHANGÉS, ce qui garantit qu'une mise à
// jour seule (clé NVS pool_size absente → Medium) ne modifie AUCUN réglage.
//
// Justifications v2.30.0 (feature-087, visa pool-chemistry du 31/08/2026) :
// 7,3 = centre de la plage de consigne [7,20 ; 7,50] — à pH plus bas l'acide
// surconsomme, à plus haut c'est le chlore (fraction HOCl plus faible à ORP
// égale). 600 mV : sur un bassin stabilisé (CYA 30-50, le cas commun en
// extérieur) 650 est quasi inatteignable ⇒ dosage de chlore permanent
// silencieux (incident 07/2026) ; bassin intérieur non stabilisé : relever à
// 650 (cf. UPDATE_GUIDE). Limites 5 → 10 min/h : à 5 min/h le maximum
// délivrable (~911 mL/j sur 7 h de filtration, mesuré 765-802 mL le 23/08) est
// SOUS le besoin estival d'un bassin médian (~1 800 mL/j à 30-50 m³, TAC 112).
// À 10 min/h : ~4,7 mL/min soutenables (~2 000 mL/j sur 7 h), volume de
// mélange aveugle ~280 mL/h ≈ 0,10 pH à 40 m³ / 0,20 à 20 m³ — TAILLE
// MINIMALE DE BASSIN POUR CE DÉFAUT : ~20 m³ (en dessous, la colonne Small
// redescend à 5). Depuis la v2.29.0, l'alerte de sous-dimensionnement
// (ADR-0056) guide le relèvement au-delà de 10 quand la charge le justifie.
constexpr float kDefaultPhTarget  = regulationDefaultsFor(PoolSize::Medium).phTarget;
constexpr float kDefaultOrpTarget = regulationDefaultsFor(PoolSize::Medium).orpTarget;
constexpr int kDefaultPhInjectionLimitMin  = regulationDefaultsFor(PoolSize::Medium).phInjectionLimitMin;
constexpr int kDefaultOrpInjectionLimitMin = regulationDefaultsFor(PoolSize::Medium).orpInjectionLimitMin;
// feature-091 : kDefaultStabilizationDelayMin conservé UNIQUEMENT pour la
// lecture NVS du champ inerte mqttCfg.stabilizationDelayMin (compat A8).
constexpr int kDefaultStabilizationDelayMin = 5;
constexpr int kDefaultPhDailyTargetMl  = regulationDefaultsFor(PoolSize::Medium).phDailyTargetMl;
constexpr int kDefaultOrpDailyTargetMl = regulationDefaultsFor(PoolSize::Medium).orpDailyTargetMl;
constexpr const char* kDefaultRegulationSpeed = regulationDefaultsFor(PoolSize::Medium).regulationSpeed;

// ==== Verrous compile-time (conditions du visa pool-chemistry, feature-090) ====
// Un recalibrage futur qui sortirait un défaut de sa fenêtre de sécurité, qui
// casserait la monotonie de la table ou qui toucherait la colonne Medium sans
// le savoir doit casser la COMPILATION, pas être découvert au premier flash.

// Égalité de chaînes constexpr (les libellés de vitesse sont des const char*).
constexpr bool regulationDefaultsStrEq(const char* a, const char* b) {
  return (*a == *b) && (*a == '\0' || regulationDefaultsStrEq(a + 1, b + 1));
}
// Rang d'une vitesse : slow=0 < normal=1 < fast=2 ; -1 = libellé inconnu.
constexpr int regulationSpeedRank(const char* s) {
  return regulationDefaultsStrEq(s, "slow")   ? 0
       : regulationDefaultsStrEq(s, "normal") ? 1
       : regulationDefaultsStrEq(s, "fast")   ? 2
                                              : -1;
}

// --- Bornes chimiques d'UNE colonne (mêmes fenêtres que la feature-088) ---
// Consignes dans les fenêtres du projet ; limites horaires = clamp serveur de
// /save-config ([1 ; 60]) ; volumes programmés jamais négatifs ; vitesse = un libellé connu.
// Plage ORP : bornes NOMMÉES du projet = fenêtre de plausibilité de consigne
// [300 ; 950] mV (kOrpTargetPlausibleMin/Max, feature-152) — le chemin de
// commande MQTT accepte [400 ; 900], 600 est confortablement dans les deux.
constexpr bool regulationDefaultsColumnValid(const RegulationDefaults& d) {
  return d.phTarget >= kPhTargetMin && d.phTarget <= kPhTargetMax &&
         d.orpTarget >= kOrpTargetPlausibleMin && d.orpTarget <= kOrpTargetPlausibleMax &&
         d.phInjectionLimitMin >= 1 && d.phInjectionLimitMin <= 60 &&
         d.orpInjectionLimitMin >= 1 && d.orpInjectionLimitMin <= 60 &&
         d.phDailyTargetMl >= 0 && d.orpDailyTargetMl >= 0 &&
         regulationSpeedRank(d.regulationSpeed) >= 0;
}
static_assert(regulationDefaultsColumnValid(regulationDefaultsFor(PoolSize::Small)),
              "Colonne Small hors des fenetres chimiques de securite");
static_assert(regulationDefaultsColumnValid(regulationDefaultsFor(PoolSize::Medium)),
              "Colonne Medium hors des fenetres chimiques de securite");
static_assert(regulationDefaultsColumnValid(regulationDefaultsFor(PoolSize::Large)),
              "Colonne Large hors des fenetres chimiques de securite");

// --- Monotonie : limites horaires et vitesses CROISSANTES avec la taille ---
// (un plus grand bassin a besoin d'au moins autant de capacité de correction).
static_assert(
    regulationDefaultsFor(PoolSize::Small).phInjectionLimitMin <=
        regulationDefaultsFor(PoolSize::Medium).phInjectionLimitMin &&
    regulationDefaultsFor(PoolSize::Medium).phInjectionLimitMin <=
        regulationDefaultsFor(PoolSize::Large).phInjectionLimitMin,
    "Limite d'injection pH non croissante avec la taille du bassin");
static_assert(
    regulationDefaultsFor(PoolSize::Small).orpInjectionLimitMin <=
        regulationDefaultsFor(PoolSize::Medium).orpInjectionLimitMin &&
    regulationDefaultsFor(PoolSize::Medium).orpInjectionLimitMin <=
        regulationDefaultsFor(PoolSize::Large).orpInjectionLimitMin,
    "Limite d'injection ORP non croissante avec la taille du bassin");
static_assert(
    regulationSpeedRank(regulationDefaultsFor(PoolSize::Small).regulationSpeed) <=
        regulationSpeedRank(regulationDefaultsFor(PoolSize::Medium).regulationSpeed) &&
    regulationSpeedRank(regulationDefaultsFor(PoolSize::Medium).regulationSpeed) <=
        regulationSpeedRank(regulationDefaultsFor(PoolSize::Large).regulationSpeed),
    "Vitesse de correction non croissante avec la taille du bassin");

// --- VERROU DE MIGRATION : Medium == littéraux v2.30.0, EN DUR ---
// La flotte existante (clé NVS pool_size absente → Medium) ne doit subir AUCUN
// changement de comportement : cette colonne est comparée aux valeurs v2.30.0
// écrites en littéraux — la retoucher exige de retoucher AUSSI cet assert, en
// connaissance de cause (nouveau visa pool-chemistry requis).
static_assert(
    regulationDefaultsFor(PoolSize::Medium).phInjectionLimitMin == 10 &&
    regulationDefaultsFor(PoolSize::Medium).orpInjectionLimitMin == 10 &&
    regulationDefaultsStrEq(regulationDefaultsFor(PoolSize::Medium).regulationSpeed, "normal") &&
    regulationDefaultsFor(PoolSize::Medium).phTarget == 7.3f &&
    regulationDefaultsFor(PoolSize::Medium).orpTarget == 600.0f &&
    regulationDefaultsFor(PoolSize::Medium).phDailyTargetMl == 0 &&
    regulationDefaultsFor(PoolSize::Medium).orpDailyTargetMl == 0,
    "Colonne Medium != defauts v2.30.0 : migration de flotte cassee "
    "(zero changement attendu a la mise a jour, visa pool-chemistry requis)");

// =============================================================================
// LISTE DES CHAMPS COPIÉS — logique PURE, testable en natif (condition 3)
// =============================================================================
// L'application des défauts (restauration 088 OU changement de taille 090)
// n'écrit QUE ces 7 champs, depuis la colonne de la taille demandée. Template
// plutôt que fonction sur MqttConfig : config.h dépend d'Arduino.h (String),
// hors d'atteinte de `env:native`. Les tests instancient ce template sur un
// DOUBLE portant TOUS les champs de MqttConfig et prouvent PAR MUTATION que
// rien d'autre ne bouge (invariants du visa : phAutoManualEpoch/Reason, modes,
// enabled, phCorrectionType, phOffset, calibration pompes, pompes affectées…
// intouchés). NOTE : le champ poolSize de MqttConfig n'est PAS copié ici — la
// taille est l'ENTRÉE de la sélection, pas un réglage restauré.
template <typename Cfg>
inline void applyRegulationDefaults(Cfg& c, PoolSize size) {
  const RegulationDefaults& d = regulationDefaultsFor(size);
  c.phTarget                 = d.phTarget;
  c.orpTarget                = d.orpTarget;
  c.phInjectionLimitMinutes  = d.phInjectionLimitMin;
  c.orpInjectionLimitMinutes = d.orpInjectionLimitMin;
  c.phDailyTargetMl          = d.phDailyTargetMl;
  c.orpDailyTargetMl         = d.orpDailyTargetMl;
  c.regulationSpeed          = d.regulationSpeed;
}

// Prédicat PUR du niveau de journal (condition 1 du visa, GÉNÉRALISÉ par la
// feature-090) : l'application de la colonne CIBLE `target` AUGMENTE-t-elle la
// capacité de dosage par rapport à l'état courant ? Oui si la consigne ORP
// MONTE (plus de chlore visé) ou si une limite horaire d'injection MONTE —
// restauration sur un réglage abaissé à la main, OU passage à une taille
// SUPÉRIEURE (5 → 10 → 15 min/h) → le journal passe en WARNING.
// La consigne pH, la vitesse et les volumes
// programmés (remis à 0, donc jamais en hausse) restent au niveau info.
inline bool regulationDefaultsRaiseDosing(const RegulationDefaults& target,
                                          float curOrpTarget,
                                          int curPhInjectionLimitMin,
                                          int curOrpInjectionLimitMin) {
  return (target.orpTarget > curOrpTarget) ||
         (target.phInjectionLimitMin > curPhInjectionLimitMin) ||
         (target.orpInjectionLimitMin > curOrpInjectionLimitMin);
}

#endif  // REGULATION_DEFAULTS_H
