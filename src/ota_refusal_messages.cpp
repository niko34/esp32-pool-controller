#include "ota_refusal_messages.h"

// Phrases VERBATIM (validées) — ne pas réécrire. Voir l'en-tête pour la contrainte de tampon.
static const char kPhraseSigMissing[] =
    "Signature manquante : choisissez aussi le fichier .sig livré avec cette version, puis "
    "recommencez. Rien n'a été modifié.";
static const char kPhraseSigNotReceived[] =
    "Signature non reçue : cette page est périmée. Rechargez-la (Ctrl+F5, ou tirez vers le bas "
    "sur téléphone) puis recommencez. Hors navigateur : joignez le .sig. Rien n'a été modifié.";
static const char kPhraseSigMalformed[] =
    "Fichier de signature illisible (incomplet ou modifié). Retéléchargez le .sig avec son "
    "image, puis recommencez. Rien n'a été modifié.";
static const char kPhraseSigInvalid[] =
    "Signature non reconnue : fichier non officiel ou modifié. Ne l'installez pas ; "
    "retéléchargez la version officielle. Rien n'a été modifié.";
static const char kPhraseVerifyImpossible[] =
    "L'appareil n'a pas pu vérifier la signature (problème interne). Redémarrez-le puis "
    "recommencez ; si cela persiste, contactez le support. Rien n'a été modifié.";
static const char kPhraseKindMismatch[] =
    "Mauvais type de fichier : la signature annonce un firmware, vous envoyez l'interface (ou "
    "l'inverse). Prenez le .bin et le .sig du même type. Rien n'a été modifié.";
static const char kPhraseLateFirmware[] =
    "Fichier non conforme à sa signature. L'appareil tourne toujours, mais son image de secours "
    "(retour arrière) est perdue. Retéléchargez image et signature ensemble, puis recommencez.";
static const char kPhraseLateFilesystem[] =
    "Fichier non conforme à sa signature : l'interface a été effacée. La régulation continue. "
    "Ouvrez la page de secours (même adresse) et envoyez-y l'image et sa signature, "
    "téléchargées ensemble.";
static const char kPhraseFallback[] = "Mise à jour refusée. Consultez le journal (onglet Avancé).";

OtaRefusalDecision otaRefusalDecide(OtaAuthenticityVerdict verdict) {
  switch (verdict) {
    case OtaAuthenticityVerdict::kAbsent:
      return {true, 403, "sig_absente"};
    case OtaAuthenticityVerdict::kMalformed:
      return {true, 400, "sig_malformee"};
    case OtaAuthenticityVerdict::kInvalid:
      return {true, 403, "sig_invalide"};
    case OtaAuthenticityVerdict::kVerificationImpossible:
      return {true, 503, "sig_verif_impossible"};
    case OtaAuthenticityVerdict::kKindMismatch:
      return {true, 415, "sig_genre_discordant"};
    case OtaAuthenticityVerdict::kBelowFloor:
      return {true, 403, "sig_sous_plancher"};  // inatteignable (plancher 0.0.0), par prudence
    case OtaAuthenticityVerdict::kValid:
      return {false, 0, ""};
  }
  return {true, 403, "sig_invalide"};  // valeur hors énumération : échec vers « refusé »
}

const char* otaRefusalPhrase(OtaAuthenticityVerdict verdict, bool pageAnnouncedVersion,
                             OtaRefusalSite site) {
  if (site == OtaRefusalSite::kLateFirmware) return kPhraseLateFirmware;
  if (site == OtaRefusalSite::kLateFilesystem) return kPhraseLateFilesystem;
  switch (verdict) {
    case OtaAuthenticityVerdict::kAbsent:
      return pageAnnouncedVersion ? kPhraseSigMissing : kPhraseSigNotReceived;
    case OtaAuthenticityVerdict::kMalformed:
      return kPhraseSigMalformed;
    case OtaAuthenticityVerdict::kInvalid:
      return kPhraseSigInvalid;
    case OtaAuthenticityVerdict::kVerificationImpossible:
      return kPhraseVerifyImpossible;
    case OtaAuthenticityVerdict::kKindMismatch:
      return kPhraseKindMismatch;
    case OtaAuthenticityVerdict::kBelowFloor:
    case OtaAuthenticityVerdict::kValid:
      break;
  }
  return kPhraseFallback;
}
