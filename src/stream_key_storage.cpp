#include "stream_key_storage.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__APPLE__) && defined(STREAM_KEY_USE_OPENSSL)
#include <sys/stat.h>
#include <unistd.h>
#endif

static std::mutex g_crypto_mutex;

// --- Base64 (RFC 4648) -------------------------------------------------

static const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool base64_encode(const std::vector<uint8_t> &raw, std::string *out)
{
    if (!out) {
        return false;
    }
    out->clear();
    const size_t n = raw.size();
    if (n == 0) {
        return true;
    }
    out->reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t b0 = raw[i];
        const uint32_t b1 = (i + 1 < n) ? raw[i + 1] : 0u;
        const uint32_t b2 = (i + 2 < n) ? raw[i + 2] : 0u;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out->push_back(kB64Table[(triple >> 18) & 63]);
        out->push_back(kB64Table[(triple >> 12) & 63]);
        if (i + 1 < n) {
            out->push_back(kB64Table[(triple >> 6) & 63]);
        } else {
            out->push_back('=');
        }
        if (i + 2 < n) {
            out->push_back(kB64Table[triple & 63]);
        } else {
            out->push_back('=');
        }
    }
    return true;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static bool base64_decode(const std::string &b64, std::vector<uint8_t> *out)
{
    if (!out) {
        return false;
    }
    out->clear();
    if (b64.empty()) {
        return true;
    }
    size_t len = b64.size();
    while (len > 0 && (b64[len - 1] == '\n' || b64[len - 1] == '\r' || b64[len - 1] == ' ')) {
        --len;
    }
    if (len % 4 == 1) {
        return false;
    }
    out->reserve(len / 4 * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = b64[i];
        if (c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int v = b64_value(c);
        if (v < 0) {
            return false;
        }
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

// --- Windows DPAPI ------------------------------------------------------

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>

static bool win_dpapi_protect(const std::string &plain, std::string *out_b64)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"obs-multistream stream key", nullptr, nullptr, nullptr, 0, &out)) {
        return false;
    }
    std::vector<uint8_t> raw(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64_encode(raw, out_b64);
}

static bool win_dpapi_unprotect(const std::string &b64, std::string *out_plain)
{
    std::vector<uint8_t> raw;
    if (!base64_decode(b64, &raw) || raw.empty()) {
        return false;
    }
    DATA_BLOB in{};
    in.pbData = raw.data();
    in.cbData = static_cast<DWORD>(raw.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return false;
    }
    out_plain->assign(reinterpret_cast<const char *>(out.pbData), reinterpret_cast<const char *>(out.pbData) + out.cbData);
    LocalFree(out.pbData);
    return true;
}
#endif

// --- macOS: Keychain master + AES-256-CBC -------------------------------

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonRandom.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

static const char kKeychainService[] = "com.obsproject.plugin.multistream";
static const char kKeychainAccount[] = "stream-key-aes256-master";

static bool keychain_get_or_create_master(std::vector<uint8_t> *out32)
{
    if (!out32) {
        return false;
    }
    out32->clear();
    CFStringRef service = CFStringCreateWithCString(kCFAllocatorDefault, kKeychainService, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(kCFAllocatorDefault, kKeychainAccount, kCFStringEncodingUTF8);
    if (!service || !account) {
        if (service) {
            CFRelease(service);
        }
        if (account) {
            CFRelease(account);
        }
        return false;
    }

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFDataRef keyData = nullptr;
    OSStatus st = SecItemCopyMatching(query, reinterpret_cast<CFTypeRef *>(&keyData));
    if (st == errSecSuccess && keyData) {
        const size_t len = CFDataGetLength(keyData);
        const uint8_t *bytes = CFDataGetBytePtr(keyData);
        if (len == kCCKeySizeAES256) {
            out32->assign(bytes, bytes + len);
            CFRelease(keyData);
            CFRelease(query);
            CFRelease(service);
            CFRelease(account);
            return true;
        }
        CFRelease(keyData);
    }

    std::vector<uint8_t> new_key(kCCKeySizeAES256);
    if (SecRandomCopyBytes(kSecRandomDefault, new_key.size(), new_key.data()) != errSecSuccess) {
        CFRelease(query);
        CFRelease(service);
        CFRelease(account);
        return false;
    }

    CFDataRef newKeyData = CFDataCreate(kCFAllocatorDefault, new_key.data(), static_cast<CFIndex>(new_key.size()));
    if (!newKeyData) {
        CFRelease(query);
        CFRelease(service);
        CFRelease(account);
        return false;
    }

    CFMutableDictionaryRef addQuery = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(addQuery, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(addQuery, kSecAttrService, service);
    CFDictionarySetValue(addQuery, kSecAttrAccount, account);
    CFDictionarySetValue(addQuery, kSecValueData, newKeyData);
    CFDictionarySetValue(addQuery, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    st = SecItemAdd(addQuery, nullptr);
    CFRelease(newKeyData);
    CFRelease(addQuery);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);

    if (st == errSecSuccess) {
        *out32 = std::move(new_key);
        return true;
    }
    if (st == errSecDuplicateItem) {
        return keychain_get_or_create_master(out32);
    }
    return false;
}

static bool apple_aes_protect(const std::string &plain, std::string *out_b64)
{
    std::vector<uint8_t> master;
    if (!keychain_get_or_create_master(&master) || master.size() != kCCKeySizeAES256) {
        return false;
    }

    uint8_t iv[kCCBlockSizeAES128];
    if (SecRandomCopyBytes(kSecRandomDefault, sizeof(iv), iv) != errSecSuccess) {
        return false;
    }

    size_t out_len = plain.size() + kCCBlockSizeAES128;
    std::vector<uint8_t> cipher(out_len);
    size_t moved = 0;
    CCCryptorStatus cs = CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, master.data(), kCCKeySizeAES256, iv,
                                 plain.data(), plain.size(), cipher.data(), cipher.size(), &moved);
    if (cs != kCCSuccess) {
        return false;
    }
    cipher.resize(moved);

    std::vector<uint8_t> packed;
    packed.reserve(sizeof(iv) + cipher.size());
    packed.insert(packed.end(), iv, iv + sizeof(iv));
    packed.insert(packed.end(), cipher.begin(), cipher.end());
    return base64_encode(packed, out_b64);
}

static bool apple_aes_unprotect(const std::string &b64, std::string *out_plain)
{
    std::vector<uint8_t> packed;
    if (!base64_decode(b64, &packed) || packed.size() <= kCCBlockSizeAES128) {
        return false;
    }

    std::vector<uint8_t> master;
    if (!keychain_get_or_create_master(&master) || master.size() != kCCKeySizeAES256) {
        return false;
    }

    const uint8_t *iv = packed.data();
    const uint8_t *ct = packed.data() + kCCBlockSizeAES128;
    const size_t ct_len = packed.size() - kCCBlockSizeAES128;

    std::vector<uint8_t> plain(ct_len + kCCBlockSizeAES128);
    size_t moved = 0;
    CCCryptorStatus cs = CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, master.data(), kCCKeySizeAES256, iv,
                                 ct, ct_len, plain.data(), plain.size(), &moved);
    if (cs != kCCSuccess) {
        return false;
    }
    out_plain->assign(reinterpret_cast<const char *>(plain.data()), reinterpret_cast<const char *>(plain.data()) + moved);
    return true;
}
#endif

// --- Linux: OpenSSL file master + AES-256-CBC ---------------------------

#if !defined(_WIN32) && !defined(__APPLE__) && defined(STREAM_KEY_USE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/rand.h>

static std::string g_master_key_path;

void stream_key_set_master_key_file_path(const char *path)
{
    if (path) {
        g_master_key_path = path;
    } else {
        g_master_key_path.clear();
    }
}

static bool read_or_create_master_file(std::vector<uint8_t> *out32)
{
    if (!out32 || g_master_key_path.empty()) {
        return false;
    }
    out32->clear();
    {
        std::ifstream f(g_master_key_path, std::ios::binary);
        if (f) {
            std::vector<uint8_t> buf(32);
            f.read(reinterpret_cast<char *>(buf.data()), 32);
            if (f.gcount() == 32) {
                *out32 = std::move(buf);
                return true;
            }
        }
    }
    std::vector<uint8_t> gen(32);
    if (RAND_bytes(gen.data(), static_cast<int>(gen.size())) != 1) {
        return false;
    }
    {
        std::ofstream f(g_master_key_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(reinterpret_cast<const char *>(gen.data()), static_cast<std::streamsize>(gen.size()));
    }
#if !defined(_WIN32)
    chmod(g_master_key_path.c_str(), 0600);
#endif
    *out32 = std::move(gen);
    return true;
}

static bool openssl_aes_protect(const std::string &plain, std::string *out_b64)
{
    std::vector<uint8_t> master;
    if (!read_or_create_master_file(&master) || master.size() != 32) {
        return false;
    }

    uint8_t iv[16];
    if (RAND_bytes(iv, static_cast<int>(sizeof(iv))) != 1) {
        return false;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    int len = 0;
    std::vector<uint8_t> cipher(plain.size() + EVP_MAX_BLOCK_LENGTH);
    int ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, master.data(), iv);
    if (ok != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    ok = EVP_EncryptUpdate(ctx, cipher.data(), &len, reinterpret_cast<const uint8_t *>(plain.data()),
                           static_cast<int>(plain.size()));
    if (ok != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    int total = len;
    ok = EVP_EncryptFinal_ex(ctx, cipher.data() + total, &len);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) {
        return false;
    }
    total += len;
    cipher.resize(static_cast<size_t>(total));

    std::vector<uint8_t> packed;
    packed.reserve(sizeof(iv) + cipher.size());
    packed.insert(packed.end(), iv, iv + sizeof(iv));
    packed.insert(packed.end(), cipher.begin(), cipher.end());
    return base64_encode(packed, out_b64);
}

static bool openssl_aes_unprotect(const std::string &b64, std::string *out_plain)
{
    std::vector<uint8_t> packed;
    if (!base64_decode(b64, &packed) || packed.size() <= 16) {
        return false;
    }

    std::vector<uint8_t> master;
    if (!read_or_create_master_file(&master) || master.size() != 32) {
        return false;
    }

    const uint8_t *iv = packed.data();
    const uint8_t *ct = packed.data() + 16;
    const int ct_len = static_cast<int>(packed.size() - 16);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    int len = 0;
    std::vector<uint8_t> plain(static_cast<size_t>(ct_len) + EVP_MAX_BLOCK_LENGTH);
    int ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, master.data(), iv);
    if (ok != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    ok = EVP_DecryptUpdate(ctx, plain.data(), &len, ct, ct_len);
    if (ok != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    int total = len;
    ok = EVP_DecryptFinal_ex(ctx, plain.data() + total, &len);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) {
        return false;
    }
    total += len;
    plain.resize(static_cast<size_t>(total));
    out_plain->assign(reinterpret_cast<const char *>(plain.data()), reinterpret_cast<const char *>(plain.data()) + total);
    return true;
}
#endif

#if defined(_WIN32) || defined(__APPLE__) || !defined(STREAM_KEY_USE_OPENSSL)
void stream_key_set_master_key_file_path(const char *) {}
#endif

bool stream_key_protect_for_save(const std::string &plain, std::string *out_blob, std::string *out_encoding)
{
    if (!out_blob || !out_encoding) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_crypto_mutex);

#if defined(_WIN32)
    if (win_dpapi_protect(plain, out_blob)) {
        *out_encoding = "dpapi_v1";
        return true;
    }
#elif defined(__APPLE__)
    if (apple_aes_protect(plain, out_blob)) {
        *out_encoding = "kc_aes256cbc_v1";
        return true;
    }
#elif defined(STREAM_KEY_USE_OPENSSL)
    if (openssl_aes_protect(plain, out_blob)) {
        *out_encoding = "ossl_aes256cbc_v1";
        return true;
    }
#endif
    return false;
}

bool stream_key_unprotect_load(const std::string &blob, const std::string &encoding, std::string *out_plain)
{
    if (!out_plain) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_crypto_mutex);

    if (encoding.empty()) {
        *out_plain = blob;
        return true;
    }

#if defined(_WIN32)
    if (encoding == "dpapi_v1") {
        return win_dpapi_unprotect(blob, out_plain);
    }
#elif defined(__APPLE__)
    if (encoding == "kc_aes256cbc_v1") {
        return apple_aes_unprotect(blob, out_plain);
    }
#elif defined(STREAM_KEY_USE_OPENSSL)
    if (encoding == "ossl_aes256cbc_v1") {
        return openssl_aes_unprotect(blob, out_plain);
    }
#endif
    return false;
}
