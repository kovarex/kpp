// tests\convert_bitfield: BITFIELD expanded with the members its own definition has: Util\Container\BitField.hpp is
// Factorio 2.1.24's, without master's operator^, operator~ and operator^= (thread P, 2026-11-13), and the class it
// writes (tests\convert_bitfield.expected) has none of them either.
#include "Util/Container/BitField.hpp"

BITFIELD(Flags, A, B, C);
