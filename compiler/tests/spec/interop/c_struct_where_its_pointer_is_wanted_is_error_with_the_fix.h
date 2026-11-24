typedef struct ZGuid { unsigned long a; unsigned short b, c; unsigned char d[8]; } ZGuid;
extern const ZGuid ZFOLDER_Documents;
int zKnownFolder(const ZGuid* id);
