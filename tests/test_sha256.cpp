#include <doctest/doctest.h>

#include "core/sha256.hpp"

#include <string>
#include <vector>

using namespace pastiche;

namespace {

std::string hash_str(const std::string& s) { return sha256_hex(s.data(), s.size()); }

} // namespace

TEST_CASE("sha256 matches the published test vectors")
{
    // FIPS 180-4 / NIST examples.
    CHECK(hash_str("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hash_str("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hash_str("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hash_str("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("sha256 of a million 'a' characters")
{
    // The classic long vector; also exercises the streaming path across many
    // blocks rather than a single update().
    Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk.data(), chunk.size());
    CHECK(h.hex_digest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 is independent of how the input is chunked")
{
    const std::string text = "The quick brown fox jumps over the lazy dog, repeatedly and at length.";
    const std::string reference = hash_str(text);

    // Feeding one byte at a time must give the same digest as one bulk update.
    Sha256 h;
    for (char c : text) h.update(&c, 1);
    CHECK(h.hex_digest() == reference);

    // So must an awkward split that straddles the 64-byte block boundary.
    Sha256 h2;
    h2.update(text.data(), 60);
    h2.update(text.data() + 60, text.size() - 60);
    CHECK(h2.hex_digest() == reference);
}

TEST_CASE("sha256 handles inputs around the padding boundary")
{
    // 55, 56 and 64 bytes are where the length field stops fitting in the final
    // block and an extra block is appended.
    for (std::size_t len : {std::size_t(54), std::size_t(55), std::size_t(56),
                            std::size_t(63), std::size_t(64), std::size_t(65)}) {
        const std::string data(len, 'x');
        Sha256 streamed;
        for (char c : data) streamed.update(&c, 1);
        CAPTURE(len);
        CHECK(streamed.hex_digest() == sha256_hex(data.data(), data.size()));
    }
}

TEST_CASE("sha256 reports a missing file instead of returning a digest")
{
    std::string error;
    const std::string digest = sha256_file("no_such_file_hopefully.bin", error);
    CHECK(digest.empty());
    CHECK_FALSE(error.empty());
}
