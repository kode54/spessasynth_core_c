#include <stdio.h>

#include "tg300b_to_xg.h"



int main(void) {
    fprintf(stdout,
"{\n\
  \"soundFonts\": [\n\
    {\n\
      \"fileName\": \"GeneralUserXG-SFeTest.sf3\",\n\
      \"patchMappings\": [\n\
        {\n\
          \"source\": {\n\
            \"bank\": 0\n\
          },\n\
          \"destination\": {\n\
            \"bank\": 0\n\
          }\n\
        },\n\
        {\n\
          \"source\": {\n\
            \"bank\": 128\n\
          },\n\
          \"destination\": {\n\
            \"bank\": 0\n\
          }\n\
        }");

        for(int dest_program = 0; dest_program < 128; dest_program++) {
            for(int dest_bank = 0; dest_bank < 128; dest_bank++) {
                const _map_item *item = &tg300b_to_xg[dest_bank][dest_program];
                if(item->bank_msb || item->bank_lsb || item->program) {
                    int source_bank = (item->bank_msb & 0x7f) | ((item->bank_lsb & 0x7f) << 8);
                    fprintf(stdout,
",\n\
        {\n\
          \"source\": {\n\
            \"bank\": %d,\n\
            \"program\": %d\n\
          },\n\
          \"destination\": {\n\
            \"bank\": %d,\n\
            \"program\": %d\n\
          }\n\
        }", source_bank, item->program, dest_bank, dest_program);
                }
            }
        }

    fprintf(stdout,
"\n\
      ]\n\
    }\n\
  ]\n\
}\n");
}
