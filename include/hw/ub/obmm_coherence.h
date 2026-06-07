/*
 * OBMM Directory-Based MESI Coherence for QEMU UB Simulation
 *
 * See docs/qemu_obmm_directory_mesi_coherence_design.md for full design.
 */
#ifndef OBMM_COHERENCE_H
#define OBMM_COHERENCE_H

#include "qemu/osdep.h"
#include "exec/memattrs.h"

#define OBMM_COH_LINE_SIZE       64
#define OBMM_COH_LINE_SHIFT      6
#define OBMM_COH_MAX_SHARERS     64

typedef enum ObmmCoherenceState {
    OBMM_COH_I = 0,
    OBMM_COH_S = 1,
    OBMM_COH_E = 2,
    OBMM_COH_M = 3,
} ObmmCoherenceState;

typedef struct BusControllerDev BusControllerDev;

/* --- Coherence UB Link payload structures (all QEMU_PACKED for wire) --- */

typedef struct QEMU_PACKED ObmmCohMsgHdr {
    uint32_t req_id;
    uint32_t msg_type;
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t requester_cna;
    uint32_t token_id;
    uint32_t rsvd;
} ObmmCohMsgHdr;

typedef struct QEMU_PACKED ObmmCohDataPld {
    ObmmCohMsgHdr hdr;
    uint32_t data_len;
    uint32_t status;
    uint32_t grant_state;
    uint8_t  data[OBMM_COH_LINE_SIZE];
} ObmmCohDataPld;

typedef struct QEMU_PACKED ObmmCohInvPld {
    ObmmCohMsgHdr hdr;
} ObmmCohInvPld;

typedef struct QEMU_PACKED ObmmCohInvAckPld {
    ObmmCohMsgHdr hdr;
    uint32_t status;
} ObmmCohInvAckPld;

typedef struct QEMU_PACKED ObmmCohDowngradePld {
    ObmmCohMsgHdr hdr;
} ObmmCohDowngradePld;

typedef struct QEMU_PACKED ObmmCohDowngradeAckPld {
    ObmmCohMsgHdr hdr;
    uint32_t status;
} ObmmCohDowngradeAckPld;

typedef struct QEMU_PACKED ObmmCohWbPld {
    ObmmCohMsgHdr hdr;
    uint32_t data_len;
    uint8_t  data[OBMM_COH_LINE_SIZE];
} ObmmCohWbPld;

typedef struct QEMU_PACKED ObmmCohWbAckPld {
    ObmmCohMsgHdr hdr;
    uint32_t status;
} ObmmCohWbAckPld;

typedef struct QEMU_PACKED ObmmCohFencePld {
    ObmmCohMsgHdr hdr;
    uint64_t range_start;
    uint64_t range_len;
} ObmmCohFencePld;

typedef struct QEMU_PACKED ObmmCohFenceAckPld {
    ObmmCohMsgHdr hdr;
    uint32_t status;
} ObmmCohFenceAckPld;

/* --- QEMU-side coherence API --- */

/* Directory-coherent read/write. */
MemTxResult obmm_coh_read(BusControllerDev *ubc_dev,
                          uint64_t remote_uba, uint32_t token_id,
                          uint32_t dcna, uint8_t *buf, uint32_t len);
MemTxResult obmm_coh_write(BusControllerDev *ubc_dev,
                           uint64_t remote_uba, uint32_t token_id,
                           uint32_t dcna, const uint8_t *buf, uint32_t len);

/* Coherence message send (blocks until response) */
int obmm_coh_send_fence(BusControllerDev *ubc_dev, uint32_t home_cna,
                        uint64_t range_start, uint64_t range_len,
                        uint32_t token_id);
void obmm_coh_invalidate_local_range(BusControllerDev *ubc_dev,
                                     uint32_t home_cna,
                                     uint64_t range_start,
                                     uint64_t range_len,
                                     uint32_t token_id);

/* Coherence message receive handlers (called from ubc_msgq.c) */
void obmm_coh_handle_rx_fence(BusControllerDev *ubc_dev,
                              const ObmmCohFencePld *pld, uint32_t src_cna);
void obmm_coh_handle_rx_fence_ack(BusControllerDev *ubc_dev,
                                  const ObmmCohFenceAckPld *pld, uint32_t src_cna);
void obmm_coh_handle_rx_gets(BusControllerDev *ubc_dev,
                             const ObmmCohMsgHdr *pld, uint32_t src_cna);
void obmm_coh_handle_rx_data(BusControllerDev *ubc_dev,
                             const ObmmCohDataPld *pld, uint32_t src_cna);
void obmm_coh_handle_rx_getm(BusControllerDev *ubc_dev,
                             const ObmmCohMsgHdr *pld, uint32_t src_cna);
void obmm_coh_handle_rx_inv(BusControllerDev *ubc_dev,
                            const ObmmCohInvPld *pld, uint32_t src_cna);
void obmm_coh_handle_rx_inv_ack(BusControllerDev *ubc_dev,
                                const ObmmCohInvAckPld *pld, uint32_t src_cna);
void obmm_coh_handle_rx_downgrade(BusControllerDev *ubc_dev,
                                  const ObmmCohDowngradePld *pld,
                                  uint32_t src_cna);
void obmm_coh_handle_rx_downgrade_ack(BusControllerDev *ubc_dev,
                                      const ObmmCohDowngradeAckPld *pld,
                                      uint32_t src_cna);
void obmm_coh_handle_rx_wb(BusControllerDev *ubc_dev,
                           const ObmmCohWbPld *pld, uint32_t src_cna);
void obmm_coh_handle_rx_wb_ack(BusControllerDev *ubc_dev,
                               const ObmmCohWbAckPld *pld, uint32_t src_cna);

#endif /* OBMM_COHERENCE_H */
