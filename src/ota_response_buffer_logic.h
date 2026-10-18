#ifndef OTA_RESPONSE_BUFFER_LOGIC_H
#define OTA_RESPONSE_BUFFER_LOGIC_H

// =============================================================================
// Logique PURE de la lecture BORNÉE du corps de /check-update dans un tampon
// PSRAM (bug-173, ADR-0032 révisé par ADR-0112). Header sans dépendance
// Arduino/ArduinoJson/ESP-IDF → testable en natif (ADR-0017).
//
// Cause corrigée : `deserializeJson(doc, https.getStream(), Filter)` lisait le
// corps GitHub EN FLUX — le lecteur de flux d'ArduinoJson traite un read() qui
// rend -1 comme une FIN DE DOCUMENT. Sur TLS, le tampon réseau se vide
// TRANSITOIREMENT au milieu d'un transfert de 16,7 Ko (mesuré en production :
// 55 ms entre l'ouverture TLS et l'erreur, bien trop court pour avoir tout
// lu) : le parseur concluait à tort que le document était fini.
//
// Correctif : mémoriser la réponse dans un tampon PSRAM AVANT de parser
// (filtre ArduinoJson conservé), avec une boucle de lecture qui continue tant
// que la connexion reste exploitable ET que l'échéance n'est pas atteinte. Ce
// fichier porte la DÉCISION de cette boucle (quand continuer / quand arrêter
// et pourquoi) — testable sans aucun matériel ni réseau. La boucle elle-même
// (impure : Stream/millis()) vit dans src/web_routes_ota.cpp.
// =============================================================================

#include <stddef.h>

// Action à prendre à CHAQUE itération de la boucle de lecture, décidée AVANT
// tout read().
enum class OtaBufferedReadAction {
  kContinue,      // de la place dans le tampon, l'échéance tient, il reste quelque chose à lire
  kStopTooLarge,  // borne nommée du tampon atteinte : réponse anormalement grosse, entrée tierce refusée
  kStopTimedOut,  // échéance totale de lecture dépassée : lecture trop lente / bloquée
  kStopOk,        // plus de données disponibles ET connexion fermée : fin normale du flux
};

// Décide l'action à prendre, à l'entrée de chaque itération.
//
// ⚠️ ORDRE DES TESTS délibéré : la borne de taille et l'échéance priment sur
// l'état de connexion — un pair qui resterait ouvert en envoyant des octets
// au-delà de la borne, ou une connexion qui traîne sans jamais se refermer,
// doivent être coupés même si `connectionOpen` reste vrai.
//
// `dataAvailable` : des octets sont IMMÉDIATEMENT lisibles (Stream::available()
// > 0). Sans ce paramètre, une connexion encore ouverte MAIS momentanément
// sans donnée disponible serait interprétée comme « plus rien à lire » — c'est
// très exactement le défaut corrigé : ne JAMAIS conclure à la fin du document
// sur une absence de donnée TRANSITOIRE. `connectionOpen` seul ne suffit pas
// non plus : un flux peut livrer ses derniers octets puis se fermer entre deux
// lectures, `dataAvailable` doit encore être consulté une dernière fois.
inline OtaBufferedReadAction otaBufferedReadNextAction(size_t bytesReadSoFar,
                                                        size_t maxBytes,
                                                        bool connectionOpen,
                                                        bool dataAvailable,
                                                        bool deadlineExceeded) {
  if (bytesReadSoFar >= maxBytes) {
    return OtaBufferedReadAction::kStopTooLarge;
  }
  if (deadlineExceeded) {
    return OtaBufferedReadAction::kStopTimedOut;
  }
  if (!connectionOpen && !dataAvailable) {
    return OtaBufferedReadAction::kStopOk;
  }
  return OtaBufferedReadAction::kContinue;
}

// Verdict final de la lecture bornée, consommé par le handler HTTP pour
// choisir le message de refus (journal + réponse) — DISTINGUE explicitement
// les causes listées par la spec (bug-173) : jamais un refus muet ou ambigu.
enum class OtaBufferedReadResult {
  kOk,          // lecture complète et cohérente avec le content-length annoncé (si connu)
  kTooLarge,    // borne nommée dépassée : réponse anormalement grosse
  kIncomplete,  // connexion fermée/EOF avant la fin attendue : pair coupé ou mismatch content-length
  kTimedOut,    // échéance totale dépassée avant la fin de la lecture
};

// Le content-length HTTP est une entrée TIERCE : on s'en sert pour détecter
// une troncature silencieuse, mais on ne lui fait jamais une confiance
// aveugle. `declaredContentLength <= 0` signifie « absent ou non exploitable »
// (jamais observé avec l'API GitHub, qui interdit le Transfer-Encoding
// chunked sous HTTP/1.0) : dans ce cas, aucune vérification n'est possible —
// ce n'est PAS une anomalie en soi, juste une absence d'information.
inline bool otaBufferedReadLengthMismatch(size_t bytesRead, long declaredContentLength) {
  if (declaredContentLength <= 0) return false;
  return bytesRead != static_cast<size_t>(declaredContentLength);
}

// Combine l'action qui a arrêté la boucle et la vérification content-length
// pour produire le verdict final. `kStopOk` ne devient réellement `kOk` que si
// le nombre d'octets lus correspond à ce que le serveur avait annoncé — sinon
// c'est une troncature SILENCIEUSE (le symptôme exact de ce bug) qu'il faut
// refuser au lieu d'accepter un document partiel.
inline OtaBufferedReadResult otaBufferedReadResultFor(OtaBufferedReadAction stopAction,
                                                       bool lengthMismatch) {
  switch (stopAction) {
    case OtaBufferedReadAction::kStopTooLarge:
      return OtaBufferedReadResult::kTooLarge;
    case OtaBufferedReadAction::kStopTimedOut:
      return OtaBufferedReadResult::kTimedOut;
    case OtaBufferedReadAction::kStopOk:
      return lengthMismatch ? OtaBufferedReadResult::kIncomplete : OtaBufferedReadResult::kOk;
    default:
      // kContinue n'est pas une action d'arrêt : ne devrait jamais atteindre
      // ce point (défensif, jamais observé en pratique) — classé incomplet
      // plutôt que silencieusement « ok ».
      return OtaBufferedReadResult::kIncomplete;
  }
}

#endif  // OTA_RESPONSE_BUFFER_LOGIC_H
