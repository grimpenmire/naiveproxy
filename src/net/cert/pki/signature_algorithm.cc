// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/pki/signature_algorithm.h"

#include "base/check.h"
#include "net/cert/pki/cert_error_params.h"
#include "net/cert/pki/cert_errors.h"
#include "net/der/input.h"
#include "net/der/parse_values.h"
#include "net/der/parser.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/digest.h"

namespace net {

namespace {

// md2WithRSAEncryption
// In dotted notation: 1.2.840.113549.1.1.2
const uint8_t kOidMd2WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                            0x0d, 0x01, 0x01, 0x02};

// md4WithRSAEncryption
// In dotted notation: 1.2.840.113549.1.1.3
const uint8_t kOidMd4WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                            0x0d, 0x01, 0x01, 0x03};

// md5WithRSAEncryption
// In dotted notation: 1.2.840.113549.1.1.4
const uint8_t kOidMd5WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                            0x0d, 0x01, 0x01, 0x04};

// From RFC 5912:
//
//     sha1WithRSAEncryption OBJECT IDENTIFIER ::= {
//      iso(1) member-body(2) us(840) rsadsi(113549) pkcs(1)
//      pkcs-1(1) 5 }
//
// In dotted notation: 1.2.840.113549.1.1.5
const uint8_t kOidSha1WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                             0x0d, 0x01, 0x01, 0x05};

// sha1WithRSASignature is a deprecated equivalent of
// sha1WithRSAEncryption.
//
// It originates from the NIST Open Systems Environment (OSE)
// Implementor's Workshop (OIW).
//
// It is supported for compatibility with Microsoft's certificate APIs and
// tools, particularly makecert.exe, which default(ed/s) to this OID for SHA-1.
//
// See also: https://bugzilla.mozilla.org/show_bug.cgi?id=1042479
//
// In dotted notation: 1.3.14.3.2.29
const uint8_t kOidSha1WithRsaSignature[] = {0x2b, 0x0e, 0x03, 0x02, 0x1d};

// From RFC 5912:
//
//     pkcs-1  OBJECT IDENTIFIER  ::=
//         { iso(1) member-body(2) us(840) rsadsi(113549) pkcs(1) 1 }

// From RFC 5912:
//
//     sha256WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 11 }
//
// In dotted notation: 1.2.840.113549.1.1.11
const uint8_t kOidSha256WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                               0x0d, 0x01, 0x01, 0x0b};

// From RFC 5912:
//
//     sha384WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 12 }
//
// In dotted notation: 1.2.840.113549.1.1.11
const uint8_t kOidSha384WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                               0x0d, 0x01, 0x01, 0x0c};

// From RFC 5912:
//
//     sha512WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 13 }
//
// In dotted notation: 1.2.840.113549.1.1.13
const uint8_t kOidSha512WithRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                               0x0d, 0x01, 0x01, 0x0d};

// From RFC 5912:
//
//     ecdsa-with-SHA1 OBJECT IDENTIFIER ::= {
//      iso(1) member-body(2) us(840) ansi-X9-62(10045)
//      signatures(4) 1 }
//
// In dotted notation: 1.2.840.10045.4.1
const uint8_t kOidEcdsaWithSha1[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01};

// From RFC 5912:
//
//     ecdsa-with-SHA256 OBJECT IDENTIFIER ::= {
//      iso(1) member-body(2) us(840) ansi-X9-62(10045) signatures(4)
//      ecdsa-with-SHA2(3) 2 }
//
// In dotted notation: 1.2.840.10045.4.3.2
const uint8_t kOidEcdsaWithSha256[] = {0x2a, 0x86, 0x48, 0xce,
                                       0x3d, 0x04, 0x03, 0x02};

// From RFC 5912:
//
//     ecdsa-with-SHA384 OBJECT IDENTIFIER ::= {
//      iso(1) member-body(2) us(840) ansi-X9-62(10045) signatures(4)
//      ecdsa-with-SHA2(3) 3 }
//
// In dotted notation: 1.2.840.10045.4.3.3
const uint8_t kOidEcdsaWithSha384[] = {0x2a, 0x86, 0x48, 0xce,
                                       0x3d, 0x04, 0x03, 0x03};

// From RFC 5912:
//
//     ecdsa-with-SHA512 OBJECT IDENTIFIER ::= {
//      iso(1) member-body(2) us(840) ansi-X9-62(10045) signatures(4)
//      ecdsa-with-SHA2(3) 4 }
//
// In dotted notation: 1.2.840.10045.4.3.4
const uint8_t kOidEcdsaWithSha512[] = {0x2a, 0x86, 0x48, 0xce,
                                       0x3d, 0x04, 0x03, 0x04};

// From RFC 5912:
//
//     id-RSASSA-PSS  OBJECT IDENTIFIER  ::=  { pkcs-1 10 }
//
// In dotted notation: 1.2.840.113549.1.1.10
const uint8_t kOidRsaSsaPss[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                 0x0d, 0x01, 0x01, 0x0a};

// From RFC 5912:
//
//     dsa-with-sha1 OBJECT IDENTIFIER ::=  {
//      iso(1) member-body(2) us(840) x9-57(10040) x9algorithm(4) 3 }
//
// In dotted notation: 1.2.840.10040.4.3
const uint8_t kOidDsaWithSha1[] = {0x2a, 0x86, 0x48, 0xce, 0x38, 0x04, 0x03};

// From RFC 5912:
//
//     dsa-with-sha256 OBJECT IDENTIFIER  ::=  {
//      joint-iso-ccitt(2) country(16) us(840) organization(1) gov(101)
//      csor(3) algorithms(4) id-dsa-with-sha2(3) 2 }
//
// In dotted notation: 2.16.840.1.101.3.4.3.2
const uint8_t kOidDsaWithSha256[] = {0x60, 0x86, 0x48, 0x01, 0x65,
                                     0x03, 0x04, 0x03, 0x02};

// From RFC 5912:
//
//     id-mgf1  OBJECT IDENTIFIER  ::=  { pkcs-1 8 }
//
// In dotted notation: 1.2.840.113549.1.1.8
const uint8_t kOidMgf1[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                            0x0d, 0x01, 0x01, 0x08};

// Returns true if |input| is empty.
[[nodiscard]] bool IsEmpty(const der::Input& input) {
  return input.Length() == 0;
}

// Returns true if the entirety of the input is a NULL value.
[[nodiscard]] bool IsNull(const der::Input& input) {
  der::Parser parser(input);
  der::Input null_value;
  if (!parser.ReadTag(der::kNull, &null_value))
    return false;

  // NULL values are TLV encoded; the value is expected to be empty.
  if (!IsEmpty(null_value))
    return false;

  // By definition of this function, the entire input must be a NULL.
  return !parser.HasMore();
}

[[nodiscard]] bool IsNullOrEmpty(const der::Input& input) {
  return IsNull(input) || IsEmpty(input);
}

// Parses a MaskGenAlgorithm as defined by RFC 5912:
//
//     MaskGenAlgorithm ::= AlgorithmIdentifier{ALGORITHM,
//                             {PKCS1MGFAlgorithms}}
//
//     mgf1SHA1 MaskGenAlgorithm ::= {
//         algorithm id-mgf1,
//         parameters HashAlgorithm : sha1Identifier
//     }
//
//     --
//     --  Define the set of mask generation functions
//     --
//     --  If the identifier is id-mgf1, any of the listed hash
//     --    algorithms may be used.
//     --
//
//     PKCS1MGFAlgorithms ALGORITHM ::= {
//         { IDENTIFIER id-mgf1 PARAMS TYPE HashAlgorithm ARE required },
//         ...
//     }
//
// Note that the possible mask gen algorithms is extensible. However at present
// the only function supported is MGF1, as that is the singular mask gen
// function defined by RFC 4055 / RFC 5912.
[[nodiscard]] bool ParseMaskGenAlgorithm(const der::Input input,
                                         DigestAlgorithm* mgf1_hash) {
  der::Input oid;
  der::Input params;
  if (!ParseAlgorithmIdentifier(input, &oid, &params))
    return false;

  // MGF1 is the only supported mask generation algorithm.
  if (oid != der::Input(kOidMgf1))
    return false;

  return ParseHashAlgorithm(params, mgf1_hash);
}

// Parses the parameters for an RSASSA-PSS signature algorithm, as defined by
// RFC 5912:
//
//     sa-rsaSSA-PSS SIGNATURE-ALGORITHM ::= {
//         IDENTIFIER id-RSASSA-PSS
//         PARAMS TYPE RSASSA-PSS-params ARE required
//         HASHES { mda-sha1 | mda-sha224 | mda-sha256 | mda-sha384
//                      | mda-sha512 }
//         PUBLIC-KEYS { pk-rsa | pk-rsaSSA-PSS }
//         SMIME-CAPS { IDENTIFIED BY id-RSASSA-PSS }
//     }
//
//     RSASSA-PSS-params  ::=  SEQUENCE  {
//         hashAlgorithm     [0] HashAlgorithm DEFAULT sha1Identifier,
//         maskGenAlgorithm  [1] MaskGenAlgorithm DEFAULT mgf1SHA1,
//         saltLength        [2] INTEGER DEFAULT 20,
//         trailerField      [3] INTEGER DEFAULT 1
//     }
//
// Which is to say the parameters MUST be present, and of type
// RSASSA-PSS-params. Additionally, we only support the RSA-PSS parameter
// combinations representable by TLS 1.3 (RFC 8446).
//
// Note also that DER encoding (ITU-T X.690 section 11.5) prohibits
// specifying default values explicitly. The parameter should instead be
// omitted to indicate a default value.
absl::optional<SignatureAlgorithm> ParseRsaPss(const der::Input& params) {
  der::Parser parser(params);
  der::Parser params_parser;
  if (!parser.ReadSequence(&params_parser))
    return absl::nullopt;

  // There shouldn't be anything after the sequence (by definition the
  // parameters is a single sequence).
  if (parser.HasMore())
    return absl::nullopt;

  // The default values for hashAlgorithm, maskGenAlgorithm, and saltLength
  // correspond to SHA-1, which we do not support with RSA-PSS, so treat them as
  // required fields. Explicitly-specified defaults will be rejected later, when
  // we limit combinations. Additionally, as the trailerField is required to be
  // the default, we simply ignore it and reject it as any other trailing data.
  //
  //     hashAlgorithm     [0] HashAlgorithm DEFAULT sha1Identifier,
  //     maskGenAlgorithm  [1] MaskGenAlgorithm DEFAULT mgf1SHA1,
  //     saltLength        [2] INTEGER DEFAULT 20,
  //     trailerField      [3] INTEGER DEFAULT 1
  der::Input field;
  DigestAlgorithm hash, mgf1_hash;
  der::Parser salt_length_parser;
  uint64_t salt_length;
  if (!params_parser.ReadTag(der::ContextSpecificConstructed(0), &field) ||
      !ParseHashAlgorithm(field, &hash) ||
      !params_parser.ReadTag(der::ContextSpecificConstructed(1), &field) ||
      !ParseMaskGenAlgorithm(field, &mgf1_hash) ||
      !params_parser.ReadConstructed(der::ContextSpecificConstructed(2),
                                     &salt_length_parser) ||
      !salt_length_parser.ReadUint64(&salt_length) ||
      salt_length_parser.HasMore() || params_parser.HasMore()) {
    return absl::nullopt;
  }

  // Only combinations of RSASSA-PSS-params specified by TLS 1.3 (RFC 8446) are
  // supported.
  if (hash != mgf1_hash) {
    return absl::nullopt;  // TLS 1.3 always matches MGF-1 and message hash.
  }
  if (hash == DigestAlgorithm::Sha256 && salt_length == 32) {
    return SignatureAlgorithm::kRsaPssSha256;
  }
  if (hash == DigestAlgorithm::Sha384 && salt_length == 48) {
    return SignatureAlgorithm::kRsaPssSha384;
  }
  if (hash == DigestAlgorithm::Sha512 && salt_length == 64) {
    return SignatureAlgorithm::kRsaPssSha512;
  }

  return absl::nullopt;
}

DEFINE_CERT_ERROR_ID(kUnknownSignatureAlgorithm, "Unknown signature algorithm");

}  // namespace

[[nodiscard]] bool ParseAlgorithmIdentifier(const der::Input& input,
                                            der::Input* algorithm,
                                            der::Input* parameters) {
  der::Parser parser(input);

  der::Parser algorithm_identifier_parser;
  if (!parser.ReadSequence(&algorithm_identifier_parser))
    return false;

  // There shouldn't be anything after the sequence. This is by definition,
  // as the input to this function is expected to be a single
  // AlgorithmIdentifier.
  if (parser.HasMore())
    return false;

  if (!algorithm_identifier_parser.ReadTag(der::kOid, algorithm))
    return false;

  // Read the optional parameters to a der::Input. The parameters can be at
  // most one TLV (for instance NULL or a sequence).
  //
  // Note that nothing is allowed after the single optional "parameters" TLV.
  // This is because RFC 5912's notation for AlgorithmIdentifier doesn't
  // explicitly list an extension point after "parameters".
  *parameters = der::Input();
  if (algorithm_identifier_parser.HasMore() &&
      !algorithm_identifier_parser.ReadRawTLV(parameters)) {
    return false;
  }
  return !algorithm_identifier_parser.HasMore();
}

[[nodiscard]] bool ParseHashAlgorithm(const der::Input& input,
                                      DigestAlgorithm* out) {
  CBS cbs;
  CBS_init(&cbs, input.UnsafeData(), input.Length());
  const EVP_MD* md = EVP_parse_digest_algorithm(&cbs);

  if (md == EVP_sha1()) {
    *out = DigestAlgorithm::Sha1;
  } else if (md == EVP_sha256()) {
    *out = DigestAlgorithm::Sha256;
  } else if (md == EVP_sha384()) {
    *out = DigestAlgorithm::Sha384;
  } else if (md == EVP_sha512()) {
    *out = DigestAlgorithm::Sha512;
  } else {
    // TODO(eroman): Support MD2, MD4, MD5 for completeness?
    // Unsupported digest algorithm.
    return false;
  }

  return true;
}

absl::optional<SignatureAlgorithm> ParseSignatureAlgorithm(
    const der::Input& algorithm_identifier,
    CertErrors* errors) {
  der::Input oid;
  der::Input params;
  if (!ParseAlgorithmIdentifier(algorithm_identifier, &oid, &params))
    return absl::nullopt;

  // TODO(eroman): Each OID is tested for equality in order, which is not
  // particularly efficient.

  // RFC 5912 requires that the parameters for RSA PKCS#1 v1.5 algorithms be
  // NULL ("PARAMS TYPE NULL ARE required"), however an empty parameter is also
  // allowed for compatibility with non-compliant OCSP responders.
  //
  // TODO(svaldez): Add warning about non-strict parsing.
  if (oid == der::Input(kOidSha1WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Sha1;
  }
  if (oid == der::Input(kOidSha256WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Sha256;
  }
  if (oid == der::Input(kOidSha384WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Sha384;
  }
  if (oid == der::Input(kOidSha512WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Sha512;
  }
  if (oid == der::Input(kOidSha1WithRsaSignature) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Sha1;
  }
  if (oid == der::Input(kOidMd2WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Md2;
  }
  if (oid == der::Input(kOidMd4WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Md4;
  }
  if (oid == der::Input(kOidMd5WithRsaEncryption) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kRsaPkcs1Md5;
  }

  // RFC 5912 requires that the parameters for ECDSA algorithms be absent
  // ("PARAMS TYPE NULL ARE absent"):
  if (oid == der::Input(kOidEcdsaWithSha1) && IsEmpty(params)) {
    return SignatureAlgorithm::kEcdsaSha1;
  }
  if (oid == der::Input(kOidEcdsaWithSha256) && IsEmpty(params)) {
    return SignatureAlgorithm::kEcdsaSha256;
  }
  if (oid == der::Input(kOidEcdsaWithSha384) && IsEmpty(params)) {
    return SignatureAlgorithm::kEcdsaSha384;
  }
  if (oid == der::Input(kOidEcdsaWithSha512) && IsEmpty(params)) {
    return SignatureAlgorithm::kEcdsaSha512;
  }

  if (oid == der::Input(kOidRsaSsaPss)) {
    return ParseRsaPss(params);
  }

  // RFC 5912 requires that the parameters for DSA algorithms be absent.
  //
  // TODO(svaldez): Add warning about non-strict parsing.
  if (oid == der::Input(kOidDsaWithSha1) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kDsaSha1;
  }
  if (oid == der::Input(kOidDsaWithSha256) && IsNullOrEmpty(params)) {
    return SignatureAlgorithm::kDsaSha256;
  }

  // Unknown signature algorithm.
  if (errors) {
    errors->AddError(kUnknownSignatureAlgorithm,
                     CreateCertErrorParams2Der("oid", oid, "params", params));
  }
  return absl::nullopt;
}

absl::optional<DigestAlgorithm> GetTlsServerEndpointDigestAlgorithm(
    SignatureAlgorithm alg) {
  // See RFC 5929, section 4.1. RFC 5929 breaks the signature algorithm
  // abstraction by trying to extract individual digest algorithms. (While
  // common, this is not a universal property of signature algorithms.) We
  // implement this within the library, so callers do not need to condition over
  // all algorithms.
  switch (alg) {
    // If the single digest algorithm is MD5 or SHA-1, use SHA-256.
    case SignatureAlgorithm::kRsaPkcs1Md5:
    case SignatureAlgorithm::kRsaPkcs1Sha1:
    case SignatureAlgorithm::kEcdsaSha1:
      return DigestAlgorithm::Sha256;

    case SignatureAlgorithm::kRsaPkcs1Sha256:
    case SignatureAlgorithm::kEcdsaSha256:
      return DigestAlgorithm::Sha256;

    case SignatureAlgorithm::kRsaPkcs1Sha384:
    case SignatureAlgorithm::kEcdsaSha384:
      return DigestAlgorithm::Sha384;

    case SignatureAlgorithm::kRsaPkcs1Sha512:
    case SignatureAlgorithm::kEcdsaSha512:
      return DigestAlgorithm::Sha512;

    // It is ambiguous whether hash-matching RSASSA-PSS instantiations count as
    // using one or multiple digests, but the corresponding digest is the only
    // reasonable interpretation.
    case SignatureAlgorithm::kRsaPssSha256:
      return DigestAlgorithm::Sha256;
    case SignatureAlgorithm::kRsaPssSha384:
      return DigestAlgorithm::Sha384;
    case SignatureAlgorithm::kRsaPssSha512:
      return DigestAlgorithm::Sha512;

    // Do not return anything for these legacy algorithms.
    case SignatureAlgorithm::kDsaSha1:
    case SignatureAlgorithm::kDsaSha256:
    case SignatureAlgorithm::kRsaPkcs1Md2:
    case SignatureAlgorithm::kRsaPkcs1Md4:
      return absl::nullopt;
  }
  return absl::nullopt;
}

}  // namespace net
