#include <boost/test/unit_test.hpp>
#include <hw/utility/KeyBuilder.hpp>
#include <vector>
#include <string>

// Payload Definition
struct Payload {
    uint32_t a = 0xAABBCCDD;
    uint16_t b = 0x1234;
    char c[3] = {'X', 'Y', 'Z'};
};

// Field Inserters
struct CopyFieldA {
    void operator()(const Payload& p, std::byte* dst) const noexcept {
        std::memcpy(dst, &p.a, 4);
    }
};

struct CopyFieldB {
    void operator()(const Payload& p, std::byte* dst) const noexcept {
        std::memcpy(dst, &p.b, 2);
    }
};

struct CopyFieldC {
    void operator()(const Payload& p, std::byte* dst) const noexcept {
        std::memcpy(dst, p.c, 3);
    }
};

using namespace hw::utility;
using namespace hw::type;

// Configuration
using MyKeyOptions = KeyBuilder<Payload, type_list<
    KeyAttribute<"FieldA", 4, CopyFieldA>,
    KeyAttribute<"FieldB", 2, CopyFieldB>,
    KeyAttribute<"FieldC", 3, CopyFieldC>
>>;

BOOST_AUTO_TEST_SUITE(KeyBuilderTestSuite)

BOOST_AUTO_TEST_CASE(test_single_field_construction) {
    using BuilderA = MyKeyOptions::Builder<"FieldA">;
    
    // Check Size: 4 -> rounded to 8
    static_assert(BuilderA::SIZE == 8);
    
    Payload p;
    BuilderA b(p);
    
    std::array<std::byte, BuilderA::SIZE> buffer;
    // fill with junk to ensure padding zeros it out
    std::memset(buffer.data(), 0xFF, buffer.size());
    
    b.make(buffer.data());
    
    // Verify data (FieldA = 0xAABBCCDD -> little endian DD CC BB AA)
    // Actually memcpy preserves native order. 
    uint32_t valA;
    std::memcpy(&valA, buffer.data(), 4);
    BOOST_CHECK_EQUAL(valA, 0xAABBCCDD);
    
    // Verify padding (bytes 4-7 should be 0)
    for (size_t i = 4; i < 8; ++i) {
        BOOST_CHECK_EQUAL((uint8_t)buffer[i], 0);
    }
}

BOOST_AUTO_TEST_CASE(test_multi_field_ordering) {
    // Order: B then A
    using BuilderBA = MyKeyOptions::Builder<"FieldB", "FieldA">;
    
    // Size: 2 + 4 = 6 -> rounded to 8
    static_assert(BuilderBA::SIZE == 8);
    
    Payload p; 
    p.b = 0x5566;
    p.a = 0x11223344;
    
    BuilderBA b(p);
    std::array<std::byte, 8> buffer;
    std::memset(buffer.data(), 0xFF, buffer.size());
    b.make(buffer.data());
    
    // Bytes 0-1: FieldB
    uint16_t valB;
    std::memcpy(&valB, buffer.data(), 2);
    BOOST_CHECK_EQUAL(valB, 0x5566);
    
    // Bytes 2-5: FieldA
    uint32_t valA;
    std::memcpy(&valA, buffer.data() + 2, 4);
    BOOST_CHECK_EQUAL(valA, 0x11223344);
    
    // Bytes 6-7: Padding
    BOOST_CHECK_EQUAL((uint8_t)buffer[6], 0);
    BOOST_CHECK_EQUAL((uint8_t)buffer[7], 0);
}

BOOST_AUTO_TEST_CASE(test_large_padding) {
    using BuilderC = MyKeyOptions::Builder<"FieldC">;
    // Size 3 -> 8. Padding 5 bytes.
    
    Payload p;
    BuilderC b(p);
    std::array<std::byte, 8> buffer;
    std::memset(buffer.data(), 0xFF, buffer.size());
    b.make(buffer.data());
    
    BOOST_CHECK_EQUAL((char)buffer[0], 'X');
    BOOST_CHECK_EQUAL((char)buffer[1], 'Y');
    BOOST_CHECK_EQUAL((char)buffer[2], 'Z');
    
    for(int i=3; i<8; ++i) 
        BOOST_CHECK_EQUAL((uint8_t)buffer[i], 0);
}

BOOST_AUTO_TEST_CASE(test_match_list) {
    using BuilderAB = MyKeyOptions::Builder<"FieldA", "FieldB">;

    // Exact match
    static_assert(BuilderAB::matchList("FieldA, FieldB"));
    
    // Reverse order match
    static_assert(BuilderAB::matchList("FieldB, FieldA"));
    
    // Spaces
    static_assert(BuilderAB::matchList("  FieldA  ,   FieldB  "));
    
    // Missing field
    static_assert(!BuilderAB::matchList("FieldA"));
    
    // Extra field
    static_assert(!BuilderAB::matchList("FieldA, FieldB, FieldC"));
    
    // Wrong field
    static_assert(!BuilderAB::matchList("FieldA, FieldX"));
    
    // Empty
    static_assert(!BuilderAB::matchList(""));
}

// -----------------------------------------------------------------------------
// New Feature Tests: PaddedBuilder (Include Unselected Fields)
// -----------------------------------------------------------------------------

// Setup for default byte test
struct KeyAttrWithDefault : KeyAttribute<"AttrDefault", 2, CopyFieldB> {
    static constexpr std::byte default_byte{0xEE};
};

// Define order: FieldA (4), AttrDefault (2), FieldC (3)
// Total raw size: 4 + 2 + 3 = 9. Aligned size: 16.
using DefaultByteOptions = KeyBuilder<Payload, type_list<
    KeyAttribute<"FieldA", 4, CopyFieldA>,
    KeyAttrWithDefault,
    KeyAttribute<"FieldC", 3, CopyFieldC>
>>;

BOOST_AUTO_TEST_CASE(test_padded_builder_include_unselected) {
    // Select "FieldC". 
    // PaddedBuilder should generate key with ALL fields in Definition Order.
    // 1. FieldA (Unselected -> Pad 0)
    // 2. AttrDefault (Unselected -> Pad 0xEE)
    // 3. FieldC (Selected -> Data)
    
    using PaddedC = DefaultByteOptions::PaddedBuilder<"FieldC">;
    
    static_assert(PaddedC::SIZE == 16);
    
    Payload p;
    PaddedC b(p);
    std::array<std::byte, 16> buffer;
    std::memset(buffer.data(), 0xFF, buffer.size()); // Fill with junk
    
    b.make(buffer.data());
    
    // 1. FieldA (Offset 0, Size 4) -> Padding 0x00
    BOOST_CHECK_EQUAL((uint8_t)buffer[0], 0x00);
    BOOST_CHECK_EQUAL((uint8_t)buffer[1], 0x00);
    BOOST_CHECK_EQUAL((uint8_t)buffer[2], 0x00);
    BOOST_CHECK_EQUAL((uint8_t)buffer[3], 0x00);
    
    // 2. AttrDefault (Offset 4, Size 2) -> Padding 0xEE
    BOOST_CHECK_EQUAL((uint8_t)buffer[4], 0xEE);
    BOOST_CHECK_EQUAL((uint8_t)buffer[5], 0xEE);
    
    // 3. FieldC (Offset 6, Size 3) -> Data ('X', 'Y', 'Z')
    BOOST_CHECK_EQUAL((char)buffer[6], 'X');
    BOOST_CHECK_EQUAL((char)buffer[7], 'Y');
    BOOST_CHECK_EQUAL((char)buffer[8], 'Z');
    
    // 4. Alignment Padding (Offset 9..15) -> 0x00
    for(int i=9; i<16; ++i) {
        BOOST_CHECK_EQUAL((uint8_t)buffer[i], 0);
    }
}

BOOST_AUTO_TEST_CASE(test_padded_builder_match_list) {
    using PaddedC = DefaultByteOptions::PaddedBuilder<"FieldC">;
    static_assert(PaddedC::matchList("FieldC"));
    static_assert(!PaddedC::matchList("FieldA"));
}

BOOST_AUTO_TEST_SUITE_END()
