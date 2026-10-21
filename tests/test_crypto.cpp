#include "../src/auth/crypto.h"
#include "../src/auth/password.h"
#include "check.h"

using namespace metro::auth;

static std::string pbkdf2Hex(const std::string& pw, const std::string& salt, unsigned long it, size_t len) {
    return toHex(pbkdf2Sha256(pw, toBytes(salt), it, len));
}

static void testSha256() {
    // published test vectors (FIPS 180-2 examples)
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256Hex(std::string(1000000, 'a')), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// These check whichever PBKDF2 is compiled in: Windows' own (CNG) on Windows, the portable one elsewhere.
static void testPbkdf2() {
    CHECK_EQ(pbkdf2Hex("password", "salt", 1, 32), std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    CHECK_EQ(pbkdf2Hex("password", "salt", 2, 32), std::string("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    CHECK_EQ(pbkdf2Hex("password", "salt", 4096, 32), std::string("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
    CHECK_EQ(pbkdf2Hex("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40),
             std::string("348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"));
    CHECK_EQ(pbkdf2Hex("pw", "NaCl", 3, 64),   // output longer than one hash block
             std::string("3d84c997bf5876e9072b9501787e49d49fb1d182feeca9d0c8fa9b2432f9df02e1491a0c70dab8e6d2b43aa39195b34fe1e7e851da6f8fd5c5c0b8f3506a7855"));
    CHECK_EQ(pbkdf2Hex(std::string(100, 'p'), "salty", 5, 32),   // password longer than the HMAC block
             std::string("b485e8031d2cda0474a360580889090ba12f866afa83da2b77045be0673669b2"));
    Bytes salt;
    for (unsigned char i = 0; i < 16; ++i) salt.push_back(i);
    CHECK_EQ(toHex(pbkdf2Sha256("\xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xB8\xE0\xA4\xB5\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA1" "123", salt, 1000, 32)),   // Hindi password
             std::string("7ded821e409d6b72d9fee33ce065337d5ab3498cb44bc188682c994bd2c825ef"));
    // the real work factor, so a slow or broken implementation shows up here
    CHECK_EQ(toHex(pbkdf2Sha256("correct horse battery staple", salt, kDefaultIterations, 32)),
             std::string("ef177144eec9420cbc1093d2a8b344a92bc506d0d4ec9c028dd19f8324d8c1e6"));
    CHECK_THROWS(pbkdf2Sha256("x", salt, 0, 32));
    CHECK_THROWS(pbkdf2Sha256("x", salt, 1, 0));
}

static void testHelpers() {
    Bytes a = randomBytes(32), b = randomBytes(32);
    CHECK_EQ(a.size(), static_cast<size_t>(32));
    CHECK(a != b);                                         // two draws should differ
    bool allZero = true;
    for (unsigned char c : a) allZero = allZero && (c == 0);
    CHECK(!allZero);
    CHECK_EQ(randomBytes(0).size(), static_cast<size_t>(0));

    CHECK_EQ(toHex(fromHex("00ff10Ab")), std::string("00ff10ab"));
    CHECK_THROWS(fromHex("abc"));        // odd length
    CHECK_THROWS(fromHex("zz"));         // not hex
    CHECK(constantTimeEqual(a, a));
    CHECK(!constantTimeEqual(a, b));
    CHECK(!constantTimeEqual(a, Bytes(31)));
}

static void testPasswordHashes() {
    const unsigned long fast = 1000;
    std::string h1 = hashPassword("correct horse", fast), h2 = hashPassword("correct horse", fast);
    CHECK(h1.rfind("pbkdf2-sha256$1000$", 0) == 0);
    CHECK(h1 != h2);                                       // a fresh random salt each time
    CHECK(verifyPassword("correct horse", h1));
    CHECK(verifyPassword("correct horse", h2));
    CHECK(!verifyPassword("Correct horse", h1));
    CHECK(!verifyPassword("", h1));
    CHECK(h1.find("correct horse") == std::string::npos);  // the password itself is nowhere in the stored string

    // anything malformed must simply fail to verify, never crash
    for (const char* bad : {"", "plaintext", "pbkdf2-sha256$1000$abcd$ef", "pbkdf2-sha256$0$00$00", "pbkdf2-sha256$x$00$00", "md5$1000$aa$bb",
                            "pbkdf2-sha256$99999999999$00112233445566778899aabbccddeeff$00"})
        CHECK(!verifyPassword("correct horse", bad));
    CHECK_EQ(h1.substr(0, 19), std::string("pbkdf2-sha256$1000$"));

    CHECK(needsRehash(h1, 2000));                          // fewer iterations than wanted
    CHECK(!needsRehash(h1, 1000));
    CHECK(!needsRehash(h1, 500));
    CHECK(needsRehash("garbage", 1000));
    // an unreasonable stored iteration count is refused instead of burning CPU for minutes
    CHECK(!verifyPassword("x", "pbkdf2-sha256$4000000000$00112233445566778899aabbccddeeff$00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"));
}

static void testRules() {
    CHECK(checkUsername("abc").ok);
    CHECK(checkUsername("Vanshika2026").ok);
    CHECK(checkUsername(std::string(20, 'a')).ok);
    CHECK(!checkUsername("ab").ok);                        // too short
    CHECK(!checkUsername(std::string(21, 'a')).ok);        // too long
    CHECK(!checkUsername("ab cd").ok);                     // space
    CHECK(!checkUsername("ab_cd").ok);                     // underscore
    CHECK(!checkUsername("o'brien").ok);                   // quote
    CHECK(!checkUsername("\xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xB8").ok);   // not ASCII

    CHECK(checkPassword("12345678", "alice").ok);
    CHECK(!checkPassword("1234567", "alice").ok);          // 7 characters
    CHECK(!checkPassword(std::string(129, 'x'), "alice").ok);
    CHECK(checkPassword(std::string(128, 'x'), "alice").ok);
    CHECK(!checkPassword("ALICEALICE", "aliceALICE").ok);  // same as the username, ignoring case
    CHECK(checkPassword("alice12345", "alice").ok);
    // characters are counted, not bytes: 8 Hindi characters are 24 bytes but valid
    CHECK(checkPassword("\xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xB8\xE0\xA4\xB5\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA1\xE0\xA4\x95\xE0\xA4\xBE", "alice").ok);
    CHECK_EQ(utf8Length("abc"), static_cast<size_t>(3));
    CHECK_EQ(utf8Length("\xE0\xA4\xAA\xE0\xA4\xBE"), static_cast<size_t>(2));
    CHECK(!checkPassword("short", "alice").message.empty());   // there is always a reason to show
}

int main() {
    try {
        testSha256();
        testPbkdf2();
        testHelpers();
        testPasswordHashes();
        testRules();
    } catch (const std::exception& e) {
        std::cerr << "test_crypto: unexpected exception: " << e.what() << "\n";
        return 1;
    }
    return finish("test_crypto");
}
