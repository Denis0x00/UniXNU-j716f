/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef J716F_GENI_MMIO_H
#define J716F_GENI_MMIO_H
#include "J716fGeniBackend.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Board resource boundary, not a mapping or ownership provider. The platform
 * owns a Device-memory mapping and supplies synchronous fault-aware accesses.
 * Physical addresses are passed explicitly, including the real low aperture;
 * nothing here weakens the ADT converter's unrelated address restrictions.
 * All operations are serialized with the bus owner. Provider lifetime exceeds
 * this binding and every consumer. Revocation never frees outstanding storage. */
typedef struct {
 void *context;
 int (*read32)(void *,uint64_t lease,uint64_t physical,uint32_t *);
 int (*write32)(void *,uint64_t lease,uint64_t physical,uint32_t);
 int (*barrier)(void *,uint64_t lease);
 int (*permit)(void *,uint64_t lease,JBPermit *);
 int (*terminal)(void *,const JBTerminalRecord *);
} JBMProvider;
typedef struct {
 JBMProvider provider;
 uint64_t lease_id;
 uint8_t initialized, revoked, in_call;
} JBMMIO;

/* Fresh storage only; all callbacks mandatory. Does not call provider or MMIO.
 * Provider input may alias b->provider; output must not overlap b's storage.
 * The returned JBIO refers to this storage and must not outlive it. */
int jbm_bind(JBMMIO *,const JBMProvider *,uint64_t lease,JBIO *);
/* Permanent local denial only. Caller still owns quarantine/fencing/teardown. */
void jbm_revoke(JBMMIO *);
#ifdef __cplusplus
}
#endif
#endif
