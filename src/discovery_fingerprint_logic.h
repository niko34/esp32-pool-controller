#ifndef DISCOVERY_FINGERPRINT_LOGIC_H
#define DISCOVERY_FINGERPRINT_LOGIC_H

// =============================================================================
// discovery_fingerprint_logic — Décision PURE de republication de la découverte
// Home Assistant (bug-121, ADR-0017)
// =============================================================================
// PROBLÈME : `mqtt_manager.cpp:459` remettait `discoveryPublished = false` à
// CHAQUE connexion réussie, republiant inconditionnellement ~60 entités
// `retain = true` déjà conservées par le courtier — travail redondant, payé au
// moment le plus fragile (juste après une reconnexion, cf. bug-121).
//
// SOLUTION : une empreinte 32 bits résume tout ce qui peut changer le contenu
// de la découverte SANS republication d'entité (version firmware, préfixe HA,
// identifiant d'appareil, topic de base, serveur:port). Le jeu d'entités
// lui-même est figé à la COMPILATION (un `publishConfig()` par entité, en dur
// dans mqtt_manager.cpp) : il ne peut changer qu'avec une nouvelle version de
// firmware, donc FIRMWARE_VERSION en est un MAJORANT SAIN — et c'est le seul
// élément calculable AVANT de publier quoi que ce soit (le nombre réel
// d'entités publiées n'est connu qu'APRÈS la rafale, voir discovery_report_logic.h).
// server:port est inclus pour couvrir le cas « courtier neuf ou purgé » sans
// sentinelle dédiée : changer de courtier change l'empreinte, donc republie.
//
// Ce module ne fait QUE décider et hacher — aucune E/S (pas de NVS, pas de
// MQTT, pas de log). La coquille impure (Preferences, mqtt.publish(),
// systemLogger) vit dans mqtt_manager.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stdint.h> uniquement,
// fonctions inline (header-only, rien à ajouter au build natif).
// =============================================================================

#include <stdint.h>

// Décalage de départ FNV-1a 32 bits (offset basis standard). Tout hachage
// commence par ce point fixe — voir discoveryFnv1aStr/discoveryFnv1aU32.
constexpr uint32_t kDiscoveryFnvOffsetBasis = 2166136261u;
constexpr uint32_t kDiscoveryFnvPrime       = 16777619u;

/**
 * @brief Enchaîne UN octet dans un hachage FNV-1a 32 bits en cours.
 */
inline uint32_t discoveryFnv1aByte(uint32_t h, uint8_t b) {
  h ^= b;
  h *= kDiscoveryFnvPrime;
  return h;
}

/**
 * @brief Enchaîne une chaîne C dans un hachage FNV-1a 32 bits en cours.
 * @param h Accumulateur (démarrer à kDiscoveryFnvOffsetBasis pour un nouveau hachage).
 * @param s Chaîne à absorber. nullptr toléré (no-op, renvoie h inchangé).
 */
inline uint32_t discoveryFnv1aStr(uint32_t h, const char* s) {
  if (s == nullptr) return h;
  while (*s != '\0') {
    h = discoveryFnv1aByte(h, (uint8_t)*s);
    s++;
  }
  return h;
}

/**
 * @brief Enchaîne un entier 32 bits dans un hachage FNV-1a en cours.
 *
 * Octet par octet (LSB d'abord) plutôt qu'un memcpy de la représentation
 * mémoire : indépendant de l'endianness de la plateforme d'exécution (ESP32
 * comme harnais natif x86/ARM), donc reproductible entre les deux.
 */
inline uint32_t discoveryFnv1aU32(uint32_t h, uint32_t v) {
  h = discoveryFnv1aByte(h, (uint8_t)(v & 0xFFu));
  h = discoveryFnv1aByte(h, (uint8_t)((v >> 8) & 0xFFu));
  h = discoveryFnv1aByte(h, (uint8_t)((v >> 16) & 0xFFu));
  h = discoveryFnv1aByte(h, (uint8_t)((v >> 24) & 0xFFu));
  return h;
}

/**
 * @brief Enregistrement persisté (NVS) d'une rafale de découverte HA réussie.
 *
 * Vit en NVS sous forme d'octets bruts (Preferences::putBytes/getBytes) —
 * layout stable tant que les types des champs ne changent pas (uint32/uint16/
 * uint8, pas de padding caché sur ESP32 avec cet ordre de champs).
 */
struct DiscoveryRecord {
  uint32_t fingerprint;   // Empreinte de configuration (version+préfixe+device+topic+serveur:port)
  uint32_t entityHash;    // Hachage cumulé des topics d'entités RÉELLEMENT publiés (mesuré, jamais déclaré)
  uint16_t entityCount;   // Nombre d'entités tentées lors de la dernière rafale réussie
  uint8_t  complete;      // 1 = 0 échec lors de la dernière rafale, 0 = rafale précédente incomplète
};

/**
 * @brief Motif d'une rafale de découverte — ou son absence.
 *
 * `None` est le cas NOMINAL (silence en INFO, cf. A4) : toute autre valeur
 * motive une rafale complète et une ligne de journal.
 */
enum class DiscoveryReason : uint8_t {
  None,               // Rien n'a changé — aucune rafale
  FirstBoot,          // Aucun enregistrement NVS valide (1re connexion depuis un flash/factory reset)
  FingerprintChanged,  // Empreinte différente de celle enregistrée (version, topic, serveur, préfixe…)
  PreviousIncomplete, // La dernière rafale enregistrée avait au moins un échec
  Recovery,           // Récupération explicitement demandée (réservé — aucun appelant HTTP depuis bug-121)
  HaRestart,          // Message de naissance homeassistant/status == "online"
};

/**
 * @brief Libellé FRANÇAIS court d'un motif, pour la ligne de journal (A4).
 */
inline const char* discoveryReasonLabel(DiscoveryReason r) {
  switch (r) {
    case DiscoveryReason::FirstBoot:           return "1re connexion";
    case DiscoveryReason::FingerprintChanged:  return "empreinte changée";
    case DiscoveryReason::PreviousIncomplete:  return "rafale précédente incomplète";
    case DiscoveryReason::Recovery:            return "récupération demandée";
    case DiscoveryReason::HaRestart:           return "HA redémarré";
    case DiscoveryReason::None:
    default:                                   return "aucune";
  }
}

/**
 * @brief Deux enregistrements représentent-ils un état DIFFÉRENT ?
 *
 * Comparaison structurelle stricte des quatre champs. Sert de garde à
 * l'écriture NVS (« au plus une par rafale », voir mqtt_manager.cpp) : pas de
 * ré-écriture (donc pas d'usure NVS supplémentaire) quand la nouvelle rafale
 * confirme exactement l'état déjà enregistré.
 */
inline bool discoveryRecordDiffers(const DiscoveryRecord& a, const DiscoveryRecord& b) {
  return a.fingerprint != b.fingerprint ||
         a.entityHash  != b.entityHash  ||
         a.entityCount != b.entityCount ||
         a.complete    != b.complete;
}

/**
 * @brief Décide SI et POURQUOI une rafale de découverte doit avoir lieu.
 *
 * Priorité (du plus au moins prioritaire) : enregistrement absent/invalide →
 * redémarrage HA signalé → récupération explicitement demandée → rafale
 * précédente incomplète → empreinte différente → rien.
 *
 * @param stored Dernier enregistrement connu (ignoré si storedValid == false).
 * @param storedValid false si aucun enregistrement NVS valide n'a pu être chargé
 *                     (1er boot, NVS effacée, taille lue ≠ sizeof(DiscoveryRecord)).
 * @param currentFp Empreinte de la configuration ACTUELLE (voir discoveryFnv1a*).
 * @param forced true si une récupération a été explicitement demandée (réservé :
 *               aucun appelant ne pose ce booléen à true depuis bug-121, le
 *               bouton HTTP correspondant ayant été retiré du périmètre du lot).
 * @param haRestart true si le message de naissance HA vient d'être vu
 *                  (`homeassistant/status` == "online"). En pratique, ce
 *                  chemin est court-circuité côté appelant (mqtt_manager.cpp
 *                  appelle directement publishDiscovery(HaRestart) sans passer
 *                  par cette fonction) — le paramètre existe pour que la
 *                  fonction reste testable dans sa forme complète.
 * @return Le motif à journaliser, ou None si aucune rafale n'est nécessaire.
 */
inline DiscoveryReason discoveryDecide(const DiscoveryRecord& stored, bool storedValid,
                                        uint32_t currentFp, bool forced, bool haRestart) {
  if (!storedValid) return DiscoveryReason::FirstBoot;
  if (haRestart) return DiscoveryReason::HaRestart;
  if (forced) return DiscoveryReason::Recovery;
  if (stored.complete == 0) return DiscoveryReason::PreviousIncomplete;
  if (stored.fingerprint != currentFp) return DiscoveryReason::FingerprintChanged;
  return DiscoveryReason::None;
}

/**
 * @brief Un message de naissance HA (`homeassistant/status`=="online") vu à
 * `elapsedSinceConnectMs` de LA CONNEXION est-il un VRAI redémarrage HA, ou la
 * redélivrance d'un message RETENU côté courtier ?
 *
 * bug-121 (revue, bloquant 1 — 2ᵉ passe) : le discriminant est le temps depuis
 * LA CONNEXION, jamais depuis la dernière rafale de découverte. Un message
 * retenu est redélivré par le courtier en quelques millisecondes après le
 * `subscribe()` qui suit chaque connexion réussie — donc à CHAQUE reconnexion,
 * quel que soit l'espacement entre elles. Sur le relevé du 08/09 (13
 * reconnexions espacées de 15 min à 5 h 10), un debounce assis sur la
 * dernière rafale aurait laissé passer les 13 (aucune n'était à moins de
 * 60 s de la précédente) — c'est cette fonction, appliquée à la source dans
 * messageCallback(), qui les aurait toutes droppées : chacune arrive sur une
 * session TOUT JUSTE établie (`elapsedSinceConnectMs` proche de 0), jamais sur
 * une session ancienne.
 *
 * @param elapsedSinceConnectMs millis() - date de la connexion en cours
 *                               (soustraction non signée côté appelant —
 *                               correcte au repliement de millis()).
 * @param graceMs Fenêtre pendant laquelle un "online" est traité comme une
 *                redélivrance de session (kDiscoveryHaRestartGraceMs).
 * @return true si le message doit être traité comme un vrai redémarrage HA.
 */
inline bool discoveryHaBirthIsGenuineRestart(uint32_t elapsedSinceConnectMs, uint32_t graceMs) {
  return elapsedSinceConnectMs >= graceMs;
}

#endif  // DISCOVERY_FINGERPRINT_LOGIC_H
