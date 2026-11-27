/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Minimal Linux kernel compatibility shims required by Linux v7.0.14 ath11k (QCA6390). */
#ifndef LX_ATH11K_SHIMS_H
#define LX_ATH11K_SHIMS_H

typedef unsigned long kernel_ulong_t;

#ifndef swab32
#define swab32(x) __builtin_bswap32((u32)(x))
#endif
#ifndef swab16
#define swab16(x) __builtin_bswap16((u16)(x))
#endif
#ifndef prefetch
#define prefetch(x) __builtin_prefetch(x)
#endif
#ifndef memset32
static inline void *memset32(u32 *s, u32 v, size_t count) {
  for (size_t i = 0; i < count; i++) s[i] = v;
  return s;
}
#endif
#ifndef dma_alloc_noncoherent
#define dma_alloc_noncoherent(dev, sz, dma, dir, gfp) dma_alloc_coherent(dev, sz, dma, gfp)
#define dma_free_noncoherent(dev, sz, cpu, dma, dir) dma_free_coherent(dev, sz, cpu, dma)
#endif
#ifndef device_get_mac_address
static inline int device_get_mac_address(struct device *dev, char *addr) {
  (void)dev;
  (void)addr;
  return -ENOENT;
}
#endif
#ifndef of_find_node_by_type
#define of_find_node_by_type(from, type) ((struct device_node *)0)
#endif
#ifndef of_property_present
#define of_property_present(np, name) 0
#endif
#ifndef of_machine_is_compatible
#define of_machine_is_compatible(c) 0
#endif
#ifndef of_reserved_mem_region_to_resource
#define of_reserved_mem_region_to_resource(np, idx, res) (-ENOENT)
#endif
#ifndef resource_size
#define resource_size(res) (((res)->end) ? ((res)->end - (res)->start + 1) : 0)
#endif
#ifndef devm_ioremap
#define devm_ioremap(dev, pa, sz) lx_ioremap((pa), (sz))
#endif
#ifndef PM_SUSPEND_PREPARE
#define PM_HIBERNATION_PREPARE 0x0001
#define PM_POST_HIBERNATION    0x0002
#define PM_SUSPEND_PREPARE     0x0003
#define PM_POST_SUSPEND        0x0004
#define unregister_pm_notifier(n) 0
#endif
#ifndef DMI_MATCH
enum dmi_field {
  DMI_NONE, DMI_BIOS_VENDOR, DMI_BIOS_VERSION, DMI_BIOS_DATE,
  DMI_SYS_VENDOR, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_PRODUCT_SERIAL,
  DMI_PRODUCT_UUID, DMI_PRODUCT_FAMILY, DMI_BOARD_VENDOR, DMI_BOARD_NAME,
  DMI_BOARD_VERSION, DMI_CHASSIS_VENDOR, DMI_CHASSIS_TYPE
};
struct dmi_strmatch { unsigned char slot; char substr[79]; };
struct dmi_system_id {
  int (*callback)(const struct dmi_system_id *);
  const char *ident;
  struct dmi_strmatch matches[4];
  void *driver_data;
};
#define DMI_MATCH(a, b) { .slot = (a), .substr = (b) }
#define DMI_EXACT_MATCH(a, b) { .slot = (a), .substr = (b) }
static inline int dmi_check_system(const struct dmi_system_id *list) { (void)list; return 0; }
static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list) { (void)list; return 0; }
#endif
#ifndef hex_dump_to_buffer
static inline int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize,
                                     char *linebuf, size_t linebuflen, bool ascii) {
  (void)rowsize; (void)groupsize; (void)ascii;
  const u8 *p = (const u8 *)buf;
  size_t o = 0;
  for (size_t i = 0; i < len && o + 3 < linebuflen; i++) {
    static const char h[] = "0123456789abcdef";
    linebuf[o++] = h[p[i] >> 4];
    linebuf[o++] = h[p[i] & 15];
    if (i + 1 < len && o + 1 < linebuflen) linebuf[o++] = ' ';
  }
  if (linebuflen) linebuf[o < linebuflen ? o : linebuflen - 1] = 0;
  return (int)o;
}
#endif

#endif
