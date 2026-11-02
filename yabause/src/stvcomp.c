/* Sega 315-5838 / 317-0229 / 317-0230 / 317-0231 compression + encryption
 *
 * Ported from MAME src/mame/sega/315-5838_317-0229_comp.cpp.  The cipher and
 * the prefix decompressor are kept table for table; only the MAME device
 * plumbing (save states, side-effect guards) is left out.
 */
#include <string.h>

#include "stvcomp.h"

#define BIT(v, b)      (((v) >> (b)) & 1u)
#define BITS(v, o, l)  (((v) >> (o)) & ((1u << (l)) - 1u))

typedef struct
{
   u8  routing[4];
   u8  sboxes[4][8];
   u8  affine[4];
} stvcomp_cipher;

static const stvcomp_cipher CIPHERS[4] =
{
   {   /* 315-5838 */
      { 0, 1, 2, 3 },
      { { 6, 7, 2, 0, 5, 1, 3, 4 }, { 5, 3, 7, 2, 1, 6, 4, 0 },
        { 2, 0, 6, 1, 3, 7, 4, 5 }, { 6, 5, 7, 4, 1, 3, 0, 2 } },
      { 0x14, 0x18, 0x02, 0x15 }
   },
   {   /* 317-0229 */
      { 3, 2, 0, 1 },
      { { 6, 0, 5, 4, 7, 2, 1, 3 }, { 5, 6, 1, 3, 7, 0, 4, 2 },
        { 2, 6, 0, 1, 3, 4, 7, 5 }, { 6, 5, 7, 4, 1, 3, 0, 2 } },
      { 0x12, 0x11, 0x04, 0x1A }
   },
   {   /* 317-0230 */
      { 2, 3, 1, 0 },
      { { 6, 2, 7, 0, 5, 3, 1, 4 }, { 5, 3, 7, 2, 1, 6, 4, 0 },
        { 2, 1, 3, 5, 6, 0, 4, 7 }, { 6, 1, 3, 5, 7, 0, 2, 4 } },
      { 0x14, 0x18, 0x02, 0x15 }
   },
   {   /* 317-0231 */
      { 1, 0, 3, 2 },
      { { 6, 0, 5, 4, 7, 2, 1, 3 }, { 5, 1, 6, 3, 7, 4, 0, 2 },
        { 2, 1, 3, 5, 6, 0, 4, 7 }, { 6, 3, 1, 5, 7, 2, 0, 4 } },
      { 0x12, 0x11, 0x04, 0x1A }
   }
};

static const u8 OUTPUT_BITS[4][3] = { { 0, 1, 7 }, { 2, 5, 9 }, { 4, 10, 13 }, { 6, 11, 15 } };
static const u8 AFFINE_BITS[4] = { 3, 8, 12, 14 };

typedef struct
{
   int variant;
   int enabled;

   u16 tree[24];
   u8  dictionary[256];
   u8  tree_words;
   u16 dictionary_bytes;
   u8  upload_dictionary;

   u32 source;
   u16 word;
   u8  bits;
   u16 output;
   u8  abort;

   int bank;
   u16 control;
} stvcomp_state;

static stvcomp_state s;
static STVCompSourceFn s_source_reader = NULL;

static u16 stvcomp_decipher(u16 ciphertext)
{
   const stvcomp_cipher *cipher = &CIPHERS[s.variant];
   const u8 lanes[4] =
   {
      (u8)((BIT(ciphertext, 5) ^ BIT(ciphertext, 12)) |
           ((BIT(ciphertext, 7) ^ BIT(ciphertext, 9)) << 1) |
           (BIT(ciphertext, 14) << 2)),
      (u8)(BIT(ciphertext, 6) | (BIT(ciphertext, 8) << 1) | (BIT(ciphertext, 15) << 2)),
      (u8)((BIT(ciphertext, 1) ^ BIT(ciphertext, 8)) |
           (BIT(ciphertext, 7) << 1) | (BIT(ciphertext, 12) << 2)),
      (u8)(BIT(ciphertext, 3) | (BIT(ciphertext, 4) << 1) | (BIT(ciphertext, 10) << 2))
   };
   u8 affine = (u8)(BIT(ciphertext, 0) |
                    ((BIT(ciphertext, 0) ^ BIT(ciphertext, 2)) << 1) |
                    ((BIT(ciphertext, 11) ^ BIT(ciphertext, 14)) << 2) |
                    ((BIT(ciphertext, 4) ^ BIT(ciphertext, 13)) << 3) | 16);
   u32 result = 0;
   int group;

   for (group = 0; group < 4; ++group)
   {
      u8 value = cipher->sboxes[group][lanes[cipher->routing[group]]];
      int bit;

      for (bit = 0; bit < 3; ++bit)
         result |= (u32)BIT(value, bit) << OUTPUT_BITS[group][bit];

      {
         u8 parity = (u8)(affine & cipher->affine[group]);
         parity ^= parity >> 4;
         parity ^= parity >> 2;
         parity ^= parity >> 1;
         result |= (u32)BIT(parity, 0) << AFFINE_BITS[group];
      }
   }
   return (u16)result;
}

static u8 stvcomp_decompress_byte(void)
{
   u16 code = 0;
   u32 length;

   if (s.abort || s.tree_words != 24 || s.dictionary_bytes != 256)
      return 0xFF;

   for (length = 1; length <= 12; ++length)
   {
      u32 entry;

      if (!s.bits)
      {
         s.word = s_source_reader ? stvcomp_decipher(s_source_reader(s.source)) : 0xFFFF;
         s.source = (s.source + 1) & 0x007FFFFF;
         s.bits = 16;
      }
      code = (u16)((code << 1) | BIT(s.word, --s.bits));

      for (entry = 0; entry < 24; entry += 2)
      {
         u16 first, limit;
         u32 next;

         if (BITS(s.tree[entry], 8, 8) != length)
            continue;

         first = (u16)(s.tree[entry + 1] >> (12 - length));
         limit = (u16)(1u << length);
         for (next = entry + 2; next < 24; next += 2)
         {
            if (s.tree[next + 1] != s.tree[entry + 1])
            {
               limit = (u16)(s.tree[next + 1] >> (12 - length));
               break;
            }
         }

         if (code >= first && code < limit)
         {
            u32 index = BITS(s.tree[entry], 0, 8) + code - first;
            if (index < 256)
               return s.dictionary[index];
         }
      }
   }

   /* Invalid tables or data must not loop for ever or read past the
      dictionary. */
   s.abort = 1;
   return 0xFF;
}

void STVCompSetSourceReader(STVCompSourceFn fn)
{
   s_source_reader = fn;
}

void STVCompEnable(int variant)
{
   memset(&s, 0, sizeof(s));
   s.variant = (variant >= 0 && variant <= 3) ? variant : 0;
   s.enabled = 1;
   s.output = 0xFFFF;
   s.control = 2;   /* DecAthlete starts decoding without writing the control */
}

void STVCompDisable(void)
{
   s.enabled = 0;
}

int STVCompIsEnabled(void)
{
   return s.enabled;
}

void STVCompSetBank(int bank)
{
   s.bank = bank & 3;
}

int STVCompGetBank(void)
{
   return s.bank;
}

void STVCompSetControl(u16 val)
{
   s.control = val;
}

int STVCompDecodeEnabled(void)
{
   return (s.control >> 1) & 1;
}

void STVCompSourceWrite(u32 data, u32 mem_mask)
{
   s.source = (s.source & ~mem_mask) | (data & mem_mask);
   s.source &= 0x007FFFFF;
   s.word = 0;
   s.bits = 0;
   s.abort = 0;
}

void STVCompTableWrite(u32 offset, u16 data)
{
   if (!BIT(offset, 0))
   {
      s.upload_dictionary = (u8)BIT(data, 7);
      if (s.upload_dictionary)
         s.dictionary_bytes = 0;
      else
         s.tree_words = 0;
   }
   else if (s.upload_dictionary)
   {
      if (s.dictionary_bytes < 256)
      {
         s.dictionary[s.dictionary_bytes++] = (u8)(data >> 8);
         s.dictionary[s.dictionary_bytes++] = (u8)(data & 0xFF);
      }
   }
   else if (s.tree_words < 24)
   {
      s.tree[s.tree_words++] = data;
   }
}

u16 STVCompDataRead(void)
{
   s.output = (u16)(stvcomp_decompress_byte() << 8);
   s.output |= stvcomp_decompress_byte();
   return s.output;
}
