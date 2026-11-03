/* What clang lays out for c_struct_member_attributes.h's structs. */
#include <stddef.h>
#include "c_struct_member_attributes.h"
unsigned long specAlignedSize(void) { return sizeof(struct SpecAlignedMember); }
unsigned long specAlignedAfter(void) { return offsetof(struct SpecAlignedMember, after); }
unsigned long specNullableSize(void) { return sizeof(struct SpecNullableMember); }
unsigned long specNullableAfter(void) { return offsetof(struct SpecNullableMember, after); }
