/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_RAM_RANGES_H
#define B2L_RAM_RANGES_H
/* SM-G975F 8GiB recovery DT: 2026-10-25. Fixed DT reservations plus
 * active firmware/stack/framebuffer/EL2 are excluded. A range is admitted
 * only after actual EFI conventional-memory allocation, never from this
 * whitelist alone. No dynamic Linux CMA allocation is treated as a firmware
 * reservation: RamManagerDxe also removes live secure-world memory holes. */
#define B2L_RAM_MAX_RANGES 128u
#define B2L_RAM_PAGE 16384ULL

#define B2L_RAM_MAX_BYTES_6G    (6ULL * 1024 * 1024 * 1024)
#define B2L_RAM_PHYS_CEILING_6G 0x980000000ULL
#define B2L_RAM_UPPER_SIZE_6G   0x0f2b00000ULL

#define B2L_RAM_MAX_BYTES_8G    (8ULL * 1024 * 1024 * 1024)
#define B2L_RAM_PHYS_CEILING_8G 0xa00000000ULL
#define B2L_RAM_UPPER_SIZE_8G   0x172b00000ULL

#if defined(B2L_BOARD_BEYOND0LTE) || defined(TARGET_BEYOND0LTE)
#define B2L_RAM_MAX_BYTES    B2L_RAM_MAX_BYTES_6G
#define B2L_RAM_PHYS_CEILING B2L_RAM_PHYS_CEILING_6G
#define B2L_RAM_UPPER_SIZE   B2L_RAM_UPPER_SIZE_6G
#else
#define B2L_RAM_MAX_BYTES    B2L_RAM_MAX_BYTES_8G
#define B2L_RAM_PHYS_CEILING B2L_RAM_PHYS_CEILING_8G
#define B2L_RAM_UPPER_SIZE   B2L_RAM_UPPER_SIZE_8G
#endif

struct b2l_ram_range { unsigned long long base, size; };
static const struct b2l_ram_range b2l_ram_allowlist[] = {
    {0x80004000ULL,0x6ffc000ULL},
    {0x87200000ULL,0xee00000ULL},
    {0xb4800000ULL,0x6300000ULL},
    {0xc0080000ULL,0x8f80000ULL},
    {0xc9440000ULL,0xbc0000ULL},
    {0xcb400000ULL,0x3c00000ULL},
    {0xcf050000ULL,0xfb0000ULL},
    {0xd1600000ULL,0xea00000ULL},
    {0xe1900000ULL,0x9a08000ULL},
    {0xed210000ULL,0x2df0000ULL},
    {0x88ad00000ULL,B2L_RAM_UPPER_SIZE},
};
static inline int b2l_ram_allowed(unsigned long long base,unsigned long long size)
{
 if(!size||base>~0ULL-size)return 0;
 for(unsigned i=0;i<sizeof(b2l_ram_allowlist)/sizeof(b2l_ram_allowlist[0]);i++)
  if(base>=b2l_ram_allowlist[i].base&&base+size<=b2l_ram_allowlist[i].base+b2l_ram_allowlist[i].size)return 1;
 return 0;
}
static inline int b2l_ram_validate_limits(const struct b2l_ram_range *r,unsigned n,
 unsigned long long primary,unsigned long long primary_size,
 unsigned long long ceiling,unsigned long long max_bytes,
 unsigned long long upper_max_end)
{
 unsigned primary_count=0;unsigned long long total=0,last=0;
 if(!r||!n||n>B2L_RAM_MAX_RANGES)return -1;
 for(unsigned i=0;i<n;i++){
  if((r[i].base&(B2L_RAM_PAGE-1))||(r[i].size&(B2L_RAM_PAGE-1))||
   !b2l_ram_allowed(r[i].base,r[i].size)||(i&&r[i].base<last)||
   (r[i].base+r[i].size>ceiling))return -1;
  if(upper_max_end&&r[i].base>=0x88ad00000ULL&&(r[i].base+r[i].size>upper_max_end))return -1;
  if(r[i].base==primary&&r[i].size==primary_size)primary_count++;
  last=r[i].base+r[i].size;total+=r[i].size;
 }
 return primary_count!=1||total>max_bytes?-1:0;
}
static inline int b2l_ram_validate_board(const struct b2l_ram_range *r,unsigned n,
 unsigned long long primary,unsigned long long primary_size,int is_6g)
{
 return b2l_ram_validate_limits(r,n,primary,primary_size,
  is_6g?B2L_RAM_PHYS_CEILING_6G:B2L_RAM_PHYS_CEILING_8G,
  is_6g?B2L_RAM_MAX_BYTES_6G:B2L_RAM_MAX_BYTES_8G,
  is_6g?0x97D800000ULL:0x9FD800000ULL);
}
static inline int b2l_ram_validate(const struct b2l_ram_range *r,unsigned n,
 unsigned long long primary,unsigned long long primary_size)
{
#if defined(B2L_BOARD_BEYOND0LTE) || defined(TARGET_BEYOND0LTE)
 return b2l_ram_validate_limits(r,n,primary,primary_size,B2L_RAM_PHYS_CEILING_6G,B2L_RAM_MAX_BYTES_6G,0x97D800000ULL);
#else
 return b2l_ram_validate_limits(r,n,primary,primary_size,B2L_RAM_PHYS_CEILING_8G,B2L_RAM_MAX_BYTES_8G,0x9FD800000ULL);
#endif
}
static inline unsigned long long b2l_ram_bytes(const struct b2l_ram_range *r,unsigned n)
{unsigned long long t=0;for(unsigned i=0;i<n;i++)t+=r[i].size;return t;}
static inline int b2l_ram_contains_range(const struct b2l_ram_range *r,unsigned n,
 unsigned long long pa,unsigned long long size)
{if(!size||pa>~0ULL-size)return 0;for(unsigned i=0;i<n;i++)if(pa>=r[i].base&&pa+size<=r[i].base+r[i].size)return 1;return 0;}
static inline unsigned long long b2l_ram_range_end(const struct b2l_ram_range *r,unsigned n,unsigned long long pa)
{for(unsigned i=0;i<n;i++)if(pa>=r[i].base&&pa<r[i].base+r[i].size)return r[i].base+r[i].size;return 0;}
static inline unsigned b2l_ram_pai(const struct b2l_ram_range *r,unsigned n,unsigned long long pa)
{unsigned idx=0;for(unsigned i=0;i<n;i++){if(pa>=r[i].base&&pa<r[i].base+r[i].size)return idx+(unsigned)((pa-r[i].base)/B2L_RAM_PAGE);idx+=(unsigned)(r[i].size/B2L_RAM_PAGE);}return n&&pa==r[n-1].base+r[n-1].size?idx:~0u;}
static inline unsigned long long b2l_ram_pa(const struct b2l_ram_range *r,unsigned n,unsigned idx)
{for(unsigned i=0;i<n;i++){unsigned pages=(unsigned)(r[i].size/B2L_RAM_PAGE);if(idx<pages)return r[i].base+(unsigned long long)idx*B2L_RAM_PAGE;idx-=pages;}return 0;}
#endif
