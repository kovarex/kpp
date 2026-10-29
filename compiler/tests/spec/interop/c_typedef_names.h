/* For interop/c_typedef_names_of_anonymous_struct.kpp: windns.h's shape of DNS_KEY_DATA, a struct
   without a tag behind several typedef names, and DNS_RECORD's union naming it by its first. */
typedef unsigned short SpecWord;
typedef struct
{
  SpecWord flags;
  unsigned char protocol;
  unsigned char key[1];
}
SpecKeyData, *PSpecKeyData, SpecDnsKeyData, *PSpecDnsKeyData;
typedef struct
{
  int kind;
  union
  {
    int a;
    SpecKeyData KEY, Key;
  } Data;
} SpecRecord;
