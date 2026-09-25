/**
 * tests/secure_enclave_test.cpp
 *
 * Unit tests for ADR-028 SecureEnclave.
 *
 * Covers seal / encrypt / decrypt / clear and HKDF domain separation.
 * No I/O, no filesystem; runs in-process only.
 */
#include "agentos/secure_enclave.h"
#include "agentos/types.h"

#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <string>

using namespace agentos;

namespace
{
constexpr std::array<uint8_t, 32> kFakeVaultKey = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F,
};

constexpr std::array<uint8_t, 32> kOtherVaultKey = {
    0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    0x0F, 0xFE, 0xED, 0xDC, 0xCB, 0xBA, 0xA9, 0x98,
    0x87, 0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10,
};
} // namespace

// ---------------------------------------------------------------------------
// Sealed state
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, NewEnclaveIsNotSealed)
{
  SecureEnclave enc;
  EXPECT_FALSE (enc.is_sealed ());
}

TEST (SecureEnclaveTest, AfterSealIsSealed)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());
  EXPECT_TRUE (enc.is_sealed ());
}

TEST (SecureEnclaveTest, ClearMakesEnclaveUnusable)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());
  ASSERT_TRUE (enc.is_sealed ());
  enc.clear ();
  EXPECT_FALSE (enc.is_sealed ());

  // After clear, encrypt must report "not sealed".
  auto result = enc.encrypt ("hello", "ctx");
  EXPECT_FALSE (result.has_value ());
}

// ---------------------------------------------------------------------------
// encrypt / decrypt without seal
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, EncryptWithoutSealReturnsError)
{
  SecureEnclave enc;
  auto result = enc.encrypt ("plaintext", "ctx");
  ASSERT_FALSE (result.has_value ());
}

TEST (SecureEnclaveTest, DecryptWithoutSealReturnsError)
{
  SecureEnclave enc;
  CipherBlob blob;
  blob.ciphertext.assign (32, 0); // dummy
  blob.nonce.assign (12, 0);
  auto result = enc.decrypt (blob, "ctx");
  ASSERT_FALSE (result.has_value ());
}

// ---------------------------------------------------------------------------
// Roundtrip
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, EncryptDecryptRoundtrip)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  const std::string plaintext = "deepseek-api-key-secret-token-123";
  const std::string info      = "credential-id-42";

  auto ct = enc.encrypt (plaintext, info);
  ASSERT_TRUE (ct.has_value ()) << ct.error ();
  EXPECT_EQ (ct->nonce.size (), 12u);
  EXPECT_GE (ct->ciphertext.size (), plaintext.size () + 16); // +tag

  auto pt = enc.decrypt (*ct, info);
  ASSERT_TRUE (pt.has_value ()) << pt.error ();
  EXPECT_EQ (*pt, plaintext);
}

TEST (SecureEnclaveTest, EmptyPlaintextRoundtrip)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  auto ct = enc.encrypt ("", "ctx");
  ASSERT_TRUE (ct.has_value ());
  EXPECT_EQ (ct->ciphertext.size (), 16u); // tag only

  auto pt = enc.decrypt (*ct, "ctx");
  ASSERT_TRUE (pt.has_value ());
  EXPECT_TRUE (pt->empty ());
}

TEST (SecureEnclaveTest, LargePlaintextRoundtrip)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  std::string plaintext (16 * 1024, 'A');
  for (size_t i = 0; i < plaintext.size (); ++i)
    plaintext[i] = static_cast<char> (i & 0xFF);

  auto ct = enc.encrypt (plaintext, "ctx");
  ASSERT_TRUE (ct.has_value ());

  auto pt = enc.decrypt (*ct, "ctx");
  ASSERT_TRUE (pt.has_value ());
  EXPECT_EQ (*pt, plaintext);
}

// ---------------------------------------------------------------------------
// Nonce uniqueness — two encryptions of the same plaintext must differ
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, EncryptTwiceProducesDifferentCiphertexts)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  auto ct1 = enc.encrypt ("same-input", "ctx");
  auto ct2 = enc.encrypt ("same-input", "ctx");
  ASSERT_TRUE (ct1.has_value ());
  ASSERT_TRUE (ct2.has_value ());

  EXPECT_NE (ct1->nonce, ct2->nonce);
  EXPECT_NE (ct1->ciphertext, ct2->ciphertext);
}

// ---------------------------------------------------------------------------
// HKDF domain separation — different `info` derives a different key
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, DifferentInfoFailsAuthentication)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  auto ct = enc.encrypt ("payload", "credential-A");
  ASSERT_TRUE (ct.has_value ());

  // Decrypt with the wrong info: HKDF derives a different key → auth fail.
  auto pt = enc.decrypt (*ct, "credential-B");
  EXPECT_FALSE (pt.has_value ());
}

TEST (SecureEnclaveTest, DifferentVaultKeyFailsAuthentication)
{
  // Encrypt with key A, then re-seal with key B, then try to decrypt.
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());
  auto ct = enc.encrypt ("payload", "ctx");
  ASSERT_TRUE (ct.has_value ());

  enc.clear ();

  SecureEnclave enc2;
  enc2.seal (kOtherVaultKey.data (), kOtherVaultKey.size ());
  auto pt = enc2.decrypt (*ct, "ctx");
  EXPECT_FALSE (pt.has_value ());
}

// ---------------------------------------------------------------------------
// Tampering detection (GCM auth tag)
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, TamperedCiphertextFailsAuthentication)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  auto ct = enc.encrypt ("payload", "ctx");
  ASSERT_TRUE (ct.has_value ());
  ASSERT_FALSE (ct->ciphertext.empty ());

  CipherBlob tampered = *ct;
  tampered.ciphertext[0] ^= 0x01; // flip one bit

  auto pt = enc.decrypt (tampered, "ctx");
  EXPECT_FALSE (pt.has_value ());
}

TEST (SecureEnclaveTest, TamperedTagFailsAuthentication)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  auto ct = enc.encrypt ("payload", "ctx");
  ASSERT_TRUE (ct.has_value ());

  CipherBlob tampered = *ct;
  tampered.ciphertext.back () ^= 0x80; // flip a bit in the tag

  auto pt = enc.decrypt (tampered, "ctx");
  EXPECT_FALSE (pt.has_value ());
}

TEST (SecureEnclaveTest, TamperedNonceFailsAuthentication)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  auto ct = enc.encrypt ("payload", "ctx");
  ASSERT_TRUE (ct.has_value ());

  CipherBlob tampered = *ct;
  tampered.nonce[0] ^= 0x01;

  auto pt = enc.decrypt (tampered, "ctx");
  EXPECT_FALSE (pt.has_value ());
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST (SecureEnclaveTest, CiphertextShorterThanTagReturnsError)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  CipherBlob blob;
  blob.ciphertext.assign (8, 0); // < 16 byte tag
  blob.nonce.assign (12, 0);

  auto pt = enc.decrypt (blob, "ctx");
  EXPECT_FALSE (pt.has_value ());
}

TEST (SecureEnclaveTest, WrongNonceLengthReturnsError)
{
  SecureEnclave enc;
  enc.seal (kFakeVaultKey.data (), kFakeVaultKey.size ());

  CipherBlob blob;
  blob.ciphertext.assign (32, 0);
  blob.nonce.assign (8, 0); // GCM expects 12

  auto pt = enc.decrypt (blob, "ctx");
  EXPECT_FALSE (pt.has_value ());
}
