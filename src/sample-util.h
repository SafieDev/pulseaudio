#ifndef foosampleutilhfoo
#define foosampleutilhfoo

#include "sample.h"
#include "memblock.h"
#include "memchunk.h"

#define DEFAULT_SAMPLE_SPEC default_sample_spec

extern struct pa_sample_spec default_sample_spec;

#define VOLUME_NORM (0x100)
#define VOLUME_MUTE (0)

struct memblock *silence_memblock(struct memblock* b, struct pa_sample_spec *spec);
void silence_memchunk(struct memchunk *c, struct pa_sample_spec *spec);
void silence_memory(void *p, size_t length, struct pa_sample_spec *spec);

struct mix_info {
    struct memchunk chunk;
    uint32_t volume;
    void *userdata;
};

size_t mix_chunks(struct mix_info channels[], unsigned nchannels, void *data, size_t length, struct pa_sample_spec *spec, uint32_t volume);

void volume_memchunk(struct memchunk*c, struct pa_sample_spec *spec, uint32_t volume);

uint32_t volume_multiply(uint32_t a, uint32_t b);

#endif
