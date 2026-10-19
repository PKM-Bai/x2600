#include "player_amp.h"
#if defined(_MSC_VER)
#include <intrin.h>
#include <emmintrin.h>
#endif

void player_amp_barrier(void)
{
#if defined(__mips__)
    __asm__ volatile("sync" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile("fence iorw,iorw" ::: "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
    _mm_mfence();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

uint32_t player_amp_crc32(const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    uint32_t crc = UINT32_MAX;
    while (length--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

void player_amp_initialize(volatile struct player_amp_shared *s,
                           uint32_t session, uint32_t audio_owner,
                           uint32_t image_crc32, uint32_t image_bytes)
{
    /* Caller has held the remote core in reset and stopped audio DMA. */
    volatile unsigned char *p = (volatile unsigned char *)s;
    for (size_t i = 0; i < sizeof(*s); ++i) p[i] = 0;
    s->version = PLAYER_AMP_VERSION;
    s->memory_version = PLAYER_MEMORY_VERSION;
    s->struct_bytes = sizeof(*s);
    s->session = session;
    s->audio_owner = audio_owner;
    s->riscv_image_crc32 = image_crc32;
    s->riscv_image_bytes = image_bytes;
    s->host_state = PLAYER_AMP_BOOTING;
    player_amp_barrier();
    s->magic = PLAYER_AMP_MAGIC;
    player_amp_barrier();
}

int player_amp_header_valid(const volatile struct player_amp_shared *s)
{
    player_amp_barrier();
    return s->magic == PLAYER_AMP_MAGIC && s->version == PLAYER_AMP_VERSION &&
           s->memory_version == PLAYER_MEMORY_VERSION &&
           s->struct_bytes == sizeof(*s) && s->session != 0 &&
           (s->audio_owner == PLAYER_AUDIO_OWNER_MIPS ||
            s->audio_owner == PLAYER_AUDIO_OWNER_RISCV) &&
           s->riscv_image_bytes > 0 && s->riscv_image_bytes <= PLAYER_RISCV_BYTES;
}

int player_amp_send(volatile struct player_amp_ring *r,
                    const struct player_amp_message *message)
{
    uint32_t write = r->write_pos, used = write - r->read_pos;
    if (used > PLAYER_AMP_RING_CAPACITY) return -1;
    if (used == PLAYER_AMP_RING_CAPACITY) return 0;
    player_amp_barrier();
    struct player_amp_message copy = *message;
    copy.checksum = player_amp_crc32(&copy, offsetof(struct player_amp_message, checksum));
    volatile struct player_amp_message *out = &r->slots[write % PLAYER_AMP_RING_CAPACITY];
    out->session = copy.session;
    out->sequence = copy.sequence;
    out->opcode = copy.opcode;
    for (unsigned i = 0; i < 4; ++i) out->argument[i] = copy.argument[i];
    out->checksum = copy.checksum;
    player_amp_barrier();
    r->write_pos = write + 1u;
    player_amp_barrier();
    return 1;
}

int player_amp_receive(volatile struct player_amp_ring *r,
                       struct player_amp_message *message)
{
    uint32_t read = r->read_pos, used = r->write_pos - read;
    if (used > PLAYER_AMP_RING_CAPACITY) return -1;
    if (!used) return 0;
    player_amp_barrier();
    const volatile struct player_amp_message *in = &r->slots[read % PLAYER_AMP_RING_CAPACITY];
    message->session = in->session;
    message->sequence = in->sequence;
    message->opcode = in->opcode;
    for (unsigned i = 0; i < 4; ++i) message->argument[i] = in->argument[i];
    message->checksum = in->checksum;
    player_amp_barrier();
    r->read_pos = read + 1u;
    player_amp_barrier();
    return message->checksum == player_amp_crc32(message,
               offsetof(struct player_amp_message, checksum)) ? 1 : -1;
}
