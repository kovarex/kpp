/* For interop/kpp_declaration_of_an_imported_c_name.kpp: a C type and function, as stb_rect_pack.h
   declares stbrp_context and stbrp_init_target. */
typedef struct { int width, height, align, mode, heuristic, nodes; void* head; void* free_head; } SpecPackContext;
void specPackInit(SpecPackContext* context, int width, int height);
