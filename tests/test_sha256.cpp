#include <gtest/gtest.h>

extern "C" {
#include "sha256.h"
}

#include <cstring>
#include <string>
#include <vector>

namespace {

std::string hex_of(const void* data, size_t len) {
    uint8_t digest[32];
    char hex[65];
    sha256(data, len, digest);
    sha256_hex(digest, hex);
    return std::string(hex);
}

} // namespace

TEST(Sha256, EmptyString) {
    EXPECT_EQ(hex_of("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, AbcVector) {
    EXPECT_EQ(hex_of("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, OneMillionA) {
    std::vector<uint8_t> data(1000000, 'a');
    EXPECT_EQ(hex_of(data.data(), data.size()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, StreamingMatchesOneShot) {
    const std::string message = "The quick brown fox jumps over the lazy dog";
    uint8_t one_shot[32];
    sha256(message.data(), message.size(), one_shot);

    sha256_ctx ctx;
    sha256_init(&ctx);
    for (size_t i = 0; i < message.size(); ++i) {
        sha256_update(&ctx, message.data() + i, 1);
    }
    uint8_t streamed[32];
    sha256_final(&ctx, streamed);
    EXPECT_EQ(0, std::memcmp(one_shot, streamed, 32));

    char hex[65];
    sha256_hex(streamed, hex);
    EXPECT_EQ(std::string(hex), "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}
