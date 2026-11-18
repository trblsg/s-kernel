// SPDX-License-Identifier: MIT-0
/*
 * The cellular modem of the Galaxy S9 (starlte, SM-G960F): the Samsung "Shannon" CP that is part of the Exynos 9810
 * itself (Samsung's driver calls it ss360ap). Only with boot-arg cp=N on this phone; with cp absent or 0 nothing
 * below runs: no register is read, no memory mapped, no call made to the secure world.
 *
 * WHAT THE CP IS, AS THE AP SEES IT (the phone's device tree and Samsung's kernel, read as data)
 *   its memory     146 MB of the phone's DRAM at 0xF0000000 (the device tree's reserved "modem_if"), outside XNU's
 *                  RAM. The CP sees it at 0x40000000. The image's own table (below) divides it:
 *                    0x0000000  BOOT, then MAIN at 0x10000, NV at 0x4900000 (the boot loader's layout: cbd loads
 *                               each part at address & 0x3fffffff and never sends the table of contents; the image's
 *                               memory map is at BOOT + 0xA0)
 *                    0x7900000  VSS, the voice processor's, 7 MB
 *                    0x8000000  IPC, 5 MB: the memory both sides talk through
 *                    0x8500000  ZMC, 11 MB: downlink packets' buffers (not used here)
 *                    0x9000000  VPA, 2 MB
 *   its image      the phone's RADIO partition, read only, used exactly as it is. It starts with a table of
 *                  contents of 32-byte entries (name[12], offset in the partition, address for the CP, size, CRC-32,
 *                  number): TOC, BOOT, MAIN, VSS, NV, OFFSET. Each entry goes to (its address - BOOT's address) in the
 *                  CP's memory; the table of contents itself is not copied (cbd never sends it: BOOT at 0, MAIN at
 *                  0x10000, VSS at 0x7900000). NV has no bytes in RADIO: it is
 *                  the place of the phone's own non-volatile data
 *                  (Android's boot daemon copies /efs/nv_data.bin there). This driver never writes the EFS partition
 *                  and never mounts it. With cp_nv=2 (the owner's decision of 13 Nov 2026) it READS /nv_data.bin out
 *                  of EFS at boot through a read-only ext4 reader (hakios.ext4ro.h over hakios_ufs_read_partition,
 *                  which has no write path), keeps it in RAM for the run and gives it to the CP; only its size and a
 *                  CRC prefix are logged. With cp_nv=1 it takes a file "nv_data.bin" that the boot image's builder
 *                  put beside the kernel (hakios_image_file("fw", ...)); without either the place is filled with zeros.
 *   the secure     the image is checked and the CP's memory closed by Samsung's EL3 monitor, not by us:
 *   world          SMC 0x82000700 with x1 = the mode (2: open the memory for loading; 0, or'ed with the memory's
 *                  address when the image carries its memory table: check BOOT and MAIN and lock), x2 = BOOT's size,
 *                  x3 = MAIN's size, each between SMC 0x82001011 x1 = 0 and x1 = 1 (the hash engine's clock on and
 *                  off). Its answer is 0 or one of Samsung's error numbers (smc_error below). If it refuses, this
 *                  driver stops: nothing is patched, skipped or retried another way.
 *   the PMU        0x14060000. CP_CTRL_NS (0x30) and CP_CTRL_S (0x34) belong to the secure world and are read and
 *                  written through the same SMC (x1 = 3 read, with x2 = 0 or 1 for the low and high half, x3 = 0 for
 *                  _S and 1 for _NS; x1 = 4 write, x2 = the value). CP_STAT 0x38, CENTRAL_SEQ_CP_CONFIGURATION 0x280
 *                  and _STATUS 0x284 (bits 23:16: 0x80 while the CP is held in reset), RESET_SEQUENCER_STATUS 0x504
 *                  (bits 2:0: 0 before the CP has ever been powered, 5 when it has), the CP's power-down
 *                  configuration 0x1320-0x1338, CLEANY_BUS_CP 0x3920/0x3924.
 *   AP2CP_CFG      0x14820000 bit 0: EL3 prepares the CP's view of the memory at cold boot; writing 1 here lets the
 *                  CP's processor run.
 *   the mailbox    0x14170000. INTGR1 (0x1C) bit n raises interrupt n at the CP; INTSR0 (0x14) bits 31:16 are the
 *                  CP's interrupts to the AP, cleared through INTCR0 (0x0C) and masked by INTMR0 (0x10); 64 shared
 *                  words ISSRn at 0x80 + 4n. This phone's use: word 0 the AP's message, word 1 the CP's, word 2 the
 *                  AP's state (bit 0 wake lock, 4:1 state, 5 "PDA active", 13:6 the board's revision, 15:14 dual
 *                  SIM), word 3 the CP's state (4:1 state, 5 "LTE active"). Interrupt 0 each way says "word 0/1 has
 *                  a message"; the AP's 1 is "wake up". Read by polling here (the interrupts stay masked).
 *   a message      bit 7 valid; with bit 6 a command in bits 5:0 (1 INIT_START, 2 INIT_END, 8 PHONE_START,
 *                  0xD PIF_INIT_DONE, 7 CRASH_RESET, 9 CRASH_EXIT); without it bit 0 "there is data in the rings".
 *   the IPC        its first words are a magic number and an "access" word, then the old two queues' pointers; the
 *   memory         boot loader of the CP talks through the old "raw" queue (to the CP at 12288, 2084864 bytes;
 *                  from it at 2097152, 2097152 bytes; head and tail words at 24/28 and 32/36) with the magic "BOOT"
 *                  (0x424F4F54). The running CP talks through rings of buffers (Samsung's "SBD"): at 2048 a table
 *                  the AP writes (version, channels, per channel and direction the ring's length, buffer size and
 *                  where its arrays of offsets and sizes are), the rings' read and write counters after it, the
 *                  buffers from 64 KB up; magic 0xAA and access 1 once the link is up.
 *   a frame        Samsung's SIPC5: a byte 0xF8 (bit 2: padded), the channel, the length (16 bits, header included),
 *                  then the message. Channels: 215 the boot loader, 235 the phone's control messages ("FMT"),
 *                  245 the CP's file requests to the AP ("RFS").
 *
 * THE STAGES (cp=N)
 *   1  a look, nothing changed: the PMU's and the mailbox's registers, the two secure words (the SMC's read),
 *      the first words of the CP's memory as the boot loader left it, RADIO's table of contents.
 *   2  that, and the image loaded and checked: the CP's power domain started if it never was (CP_CTRL_NS bit 1,
 *      CP_CTRL_S bit 3), the memory opened (mode 2), the boot region cleared of whatever the loader left in it,
 *      BOOT, MAIN and VSS copied to the boot loader's layout (BOOT at 0, MAIN at 0x10000, VSS at 0x7900000; the
 *      table of contents is not copied) and compared with the table's CRCs, the NV place filled (zeros, or the
 *      builder's file with cp_nv=1), then the secure world's check (mode 0). Its answer is logged; if it is not 0
 *      the same check is made
 *      once more without the memory's address (the log shows whether the address is what is refused), and the
 *      driver stops there: the image is not locked by EL3 and the CP is not started.
 *   3  that, and the CP started: the mailbox cleared, the AP's state word set, AP2CP_CFG written, the CP's state
 *      waited for, the boot loader's two questions (0x900D, answer 0xA00D; 0x9F00, answer 0xAF00), then the
 *      running CP's INIT_START, the ring table written, PIF_INIT_DONE, and its PHONE_START: "THE CP IS UP".
 *      It is watched for cp_wait seconds (20) and then put back in reset.
 *   4  that, and the link, ONLY with the phone's own NV data loaded (cp_nv=1): the one command this driver ever
 *      sends that changes anything, "phone state: low power mode" (Samsung IPC's power group 0x01, number 0x07,
 *      "execute", the two bytes 01 00: what Android sends for airplane mode), is put in the ring first, then
 *      INIT_END is said (and said again on every PHONE_START until the CP stops repeating it: it looks at the one
 *      message word about every 100 ms, and anything written there sooner replaces INIT_END). The running
 *      CP otherwise goes to its NORMAL state by itself, receiver on, looking for a network (seen on 12 Nov 2026 with
 *      zeros for NV; never again: with zeros or without cp_nv=1 this stage is refused and the run stays at stage
 *      3). The CP is stopped at once if its next "phone state" is not low power, if none comes in 5 s, or if a
 *      registration state other than "none" is ever reported. Everything the CP sends is logged (the channel, the
 *      message's group, number and type, its length; the phone state, the SIM status bytes, the registration state;
 *      a file request's kind, name, offset and length), one harmless question is asked (the modem's software
 *      version) and its answer logged. The CP's file requests are NOT answered and nothing it wants stored is
 *      kept. After cp_wait seconds (25) the CP is put back in reset.
 *
 *   5  that, and, ONLY when the phone's owner has asked for it, the radio: with the CP confirmed in low power mode
 *      and the SIM's status "ready" (0x82 or 0x83; a locked SIM ends the run, no PIN is ever sent), "phone state:
 *      normal" is asked for (the bytes 02 02: what Android sends when airplane mode is switched off). The phone
 *      then registers on its network as it does under Android. Logged: the phone state, every registration state,
 *      the serving network's operator code, the signal words. Two minutes after it has registered (or 90 s after
 *      the request if it has not) low power mode is asked for again, confirmed, and the CP put back in reset.
 *      No call, message, network or band selection is ever asked for; a data session only in stage 6.
 *
 *   6  that, and, ONLY with an access point name given by the builder (cp_apn=NAME: the operator's published default
 *      one; none is built in), a data context: 5 s after the phone has registered the context is defined (Samsung
 *      IPC's packet group 0x0D, number 1, "set": enable, context 1, type 2, the name) and, if the CP's general
 *      response accepts that, activated (number 4, "set": enable, context 1, no user name or password). These are
 *      the layouts of the public numbering for older Samsung modems, NOT confirmed for this one: the CP's answer
 *      to each is the result of the stage, and a refusal ends it (no second form is tried). Logged: each result
 *      word, the packet group's notifications (the call status' bytes; of the address configuration its length,
 *      context and the first octet only), and how many packets the CP put on the data channels (their length and
 *      IP version; nothing is sent on them and none is passed on). 20 s later the context is deactivated, then
 *      low power mode, then reset, as in stage 5.
 *
 *   7  that, and three packets of our own through the context, made and checked in this file (no interface is
 *      made, nothing of the system's is carried): with the context's IPv4 address and first DNS server taken from
 *      the CP's address configuration (kept in memory for the run, never logged beyond one octet), one ICMP echo
 *      request (to cp_ping=A.B.C.D, or to that DNS server) and one DNS question for the A record of example.com go
 *      up, and whatever comes down is checked (lengths, header and data checksums, addresses, the echo's and the
 *      question's identifiers) and counted: answered, not ours, IPv6, malformed. The rings, as Samsung's kernel
 *      uses them on this phone (read as facts): a data packet has no frame header, its channel (10: context 1) is
 *      in the upper half of its ring slot's size word; up through the "data" ring; down either through the "high
 *      priority" ring or, for the "data" ring, into buffers of the 11 MB "ZMC" region behind the IPC memory: the
 *      AP writes each slot's 64-bit data offset (from the IPC memory's start) and moves the ring's read counter
 *      as its offer, the CP fills a buffer, sets the slot's size word and moves the write counter. Here slot k
 *      always has the same 4 KB cell k, the offsets are never read back for use, the CP's counter is checked
 *      against the ring's length and against what was offered, a length over 2048 is a malformed packet, and a
 *      packet is copied out before it is looked at.
 *
 * NEVER DONE HERE: no partition is written; EFS is not read; no request that sets an identity, a band, a power or
 * a calibration value, no test or factory mode, no PIN; the radio is on only in stage 5. An answer that could carry
 * an identity is logged as its length only.
 *
 * kern.hakios.cp.state reads where it got to (the stage reached, 100 + a stage that failed, 200 stopped);
 * kern.hakios.cp.go: 1 the state printed, 2 the CP stopped (reset asserted), 3 the version asked again.
 *
 * THE ARGUMENTS
 *   cp=N         the first N of the Galaxy S9's modem's six stages (1 a look, 2 its image loaded and checked, 3 started, 4 the link, 5 the network, 6 a data context, 7 three packets through it); 0 or none: left alone
 *   cp_sim=1     in low power mode, once the SIM is ready, one read of its administrative data file (EF_AD, 4 bytes: no identity) is asked, to see if the modem takes the public numbering's SIM read (seen twice on an SM-G960F: refused, result 0x8001)
 *   cp_ping=A.B.C.D  the address stage 7 sends its one echo request to (none: the context's first DNS server)
 *   cp_apn=NAME  the access point name stage 6 asks the data context for (the operator's published default; none: no stage 6)
 *   cp_nv=1      the phone's own NV data, a file nv_data.bin the image's builder put beside the kernel, is given to the modem (none: zeros, and no stage 4)
 *   cp_if=1      with cp=6 or 7 and cp_apn: the active data context becomes a network interface, pdp_ip0 (IPv4, point to point, the CP's address for it), and stays up: no deactivation, no return to low power, no two-minute limit; the modem runs until kern.hakios.cp.go=2. Off: the stages end as before
 *   cp_if_kb=N   with cp_if=1: at most N KB through pdp_ip0 in a boot, both ways (2048 by default; the owner's mobile plan), then it drops
 *   cp_route=1   with cp_if=1: an IPv4 default route through pdp_ip0 (only when there is none yet: Wi-Fi's wins)
 *   cp_nv=2      the phone's own NV data read at boot from the EFS partition (/nv_data.bin, read-only, never mounted, never written), kept in RAM only; the CP's file requests for it (RFS NV read/write) are answered from that copy and its writes kept there for the run, never written back
 *   cp_rfs=1     with cp_nv=2: the CP's file opens (RFS command 0x11, e.g. CP_OMC_AUDIO_SLSI.bin) answered "not there" (handle -1,
 *                ENOENT) from RAM; nothing is read or opened. Off: they stay unanswered
 *   cp_vss=0     the image's VSS part (the voice processor's) is not copied
 *   cp_rev=N     the board's revision told to the modem (26, this phone's)
 *   cp_ds=N      the dual SIM field told to the modem (0)
 *   cp_wait=N    seconds the started modem is watched before it is put back in reset (20; 25 with the link)
 *   cp_delay=N   seconds after the boot before the modem's driver does anything (30), so that its lines are in the log
 *   cp_bl=1      read the boot loader's BOTA0-2 partitions once, read-only, and log where the EL3 monitor's CP
 *                handler and messages might be (offsets and a few words around each hit; nothing is written)
 *   (at run time) kern.hakios.cp.probe="scan|find|dump|str|dist NAME ...", root only, works without cp=: one
 *                read-only look at any partition, on demand, so a question does not need a new image. Nothing is
 *                written, no partition is mapped and no SMC is made
 *
 * WHERE IT MAY RUN. The CP's memory is fixed by the boot chain at 0xF0000000. On a tree whose loader puts XNU itself
 * there this driver must not load anything: it checks that no page of the 146 MB is XNU's and that the kernel is not in
 * it, and does nothing if either fails. The 16.5 line's UEFI lands the payload there (devices/starlte/docs/modem-plan.md,
 * 1.5); its loader moves the payload to 0xD0000000 first and keeps the CP's memory out of XNU's RAM
 * (xnuloader/platforms/starlte.c: payload_home, keep_out), so the check passes there as it does on the ios-27 tree.
 * Built for the 16.5 kernel (xnu-8796) since 17 November 2026; not yet run on a phone under iOS 16.5.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/hakios.input.h>
#include <sys/hakios.log.h>
#include <sys/hakios.physmap.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.xnu.h>
#include "hakios.ext4ro.h"
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <sys/socket.h>

/* ---- addresses ---- */

#define CP_MEM_PA               0xF0000000ULL   /* the device tree's "modem_if" */
#define CP_MEM_BYTES            0x09200000ULL
#define CP_IPC_OFFSET           0x08000000u     /* the device tree's; the image's table is checked against it */
#define CP_IPC_BYTES            0x00500000u
#define CP_PMU_PA               0x14060000UL
#define CP_PMU_BYTES            0x4000
#define CP_MBOX_PA              0x14170000UL
#define CP_CFG_PA               0x14820000UL    /* AP2CP_CFG */
#define CP_GICD_PA              0x10100000UL    /* the interrupt controller: one "pending" word read, nothing else */

#define PMU_CP_CTRL_NS          0x0030          /* through the SMC */
#define PMU_CP_CTRL_S           0x0034          /* through the SMC */
#define PMU_CP_STAT             0x0038
#define PMU_CENTRAL_SEQ_CP_CFG  0x0280
#define PMU_CENTRAL_SEQ_CP_STAT 0x0284
#define PMU_RESET_SEQ_STAT      0x0504
#define PMU_CP_PWR_FIRST        0x1320          /* RESET_AHEAD, CLEANY_BUS, LOGIC_RESET, TCXO_GATE, DISABLE_ISO, RESET_ISO, DUMP_PC */
#define PMU_CLEANY_BUS_CP_CFG   0x3920
#define PMU_CLEANY_BUS_CP_STAT  0x3924

#define NS_PWRON                (1u << 1)
#define NS_RESET_SET            (1u << 2)
#define NS_DUMP_PC_NO_PG        (1u << 17)
#define S_START                 (1u << 3)

#define MBOX_MCUCTLR            0x00
#define MBOX_INTGR0             0x08
#define MBOX_INTCR0             0x0c
#define MBOX_INTMR0             0x10
#define MBOX_INTSR0             0x14
#define MBOX_INTMSR0            0x18
#define MBOX_INTGR1             0x1c
#define MBOX_INTCR1             0x20
#define MBOX_INTMR1             0x24
#define MBOX_INTSR1             0x28
#define MBOX_ISSR(n)            (0x80 + 4 * (n))
#define MBOX_WORDS              64

/* this phone's use of the mailbox (the device tree's mif_pdata) */
#define WORD_AP_MSG             0
#define WORD_CP_MSG             1
#define WORD_AP_STATE           2
#define WORD_CP_STATE           3
#define INT_AP_MSG              0
#define INT_AP_WAKEUP           1
#define IRQ_CP_MSG              0
#define AP_STATE_STATUS(v)      (((v) & 0xfu) << 1)
#define AP_STATE_PDA_ACTIVE     (1u << 5)
#define AP_STATE_REV(v)         (((v) & 0xffu) << 6)
#define AP_STATE_DS(v)          (((v) & 0x3u) << 14)
#define CP_STATE_STATUS(w)      (((w) >> 1) & 0xfu)
#define CP_STATE_ACTIVE(w)      (((w) >> 5) & 1u)

#define MSG_VALID               0x80
#define MSG_CMD                 0x40
#define MSG_DATA                0x01
#define CMD_INIT_START          0x01
#define CMD_INIT_END            0x02
#define CMD_CRASH_RESET         0x07
#define CMD_PHONE_START         0x08
#define CMD_CRASH_EXIT          0x09
#define CMD_PIF_INIT_DONE       0x0d

#define SMC_CP                  0x82000700u
#define SMC_CP_READ             3
#define SMC_CP_WRITE            4
#define SMC_CP_REG_S            0
#define SMC_CP_REG_NS           1
#define SMC_SSS_CLK             0x82001011u

/* the IPC memory */
#define IPC_MAGIC               0
#define IPC_ACCESS              4
#define IPC_RAW_TX_HEAD         24
#define IPC_RAW_TX_TAIL         28
#define IPC_RAW_RX_HEAD         32
#define IPC_RAW_RX_TAIL         36
#define IPC_RAW_TX_BUF          12288u
#define IPC_RAW_TX_BYTES        2084864u
#define IPC_RAW_RX_BUF          2097152u
#define IPC_RAW_RX_BYTES        2097152u
#define MAGIC_BOOT              0x424F4F54u
#define MAGIC_IPC               0xAAu

#define SBD_DESC                2048u           /* the table, in the first 64 KB */
#define SBD_BUFFERS             65536u
#define SBD_MAX_CHANNELS        32
#define SBD_RB_CH               12u             /* u32 version, channels, counters' offset; then rb_ch[32] of 16 bytes */
#define SBD_RB_DESC             (SBD_RB_CH + SBD_MAX_CHANNELS * 16u)   /* rb_desc[32][2] of 20 bytes */
#define SBD_GLOBAL_BYTES        (SBD_RB_DESC + SBD_MAX_CHANNELS * 2u * 20u)

#define CH_BOOT                 215
#define CH_FMT                  235
#define CH_RFS                  245
#define DATA_CID                1               /* the one data context this driver ever defines */

/* the channels of this phone's device tree that get a ring, in its order (the ones for other regions left out) */
static const struct ring_cfg {
	uint16_t ch;
	uint16_t ul_len, ul_size, dl_len, dl_size;
	const char *name;
} ring_cfgs[] = {
	{ 35, 16, 2048, 16, 2048, "cass" },
	{ CH_FMT, 16, 4096, 32, 4096, "ipc0" },
	{ 236, 16, 4096, 32, 4096, "ipc1" },
	{ CH_RFS, 16, 2048, 512, 2048, "rfs0" },
	{ 1, 32, 2048, 64, 2048, "csd" },
	{ 25, 16, 2048, 16, 2048, "router" },
	{ 28, 16, 2048, 128, 2048, "dm0" },
	{ 10, 256, 2048, 256, 2048, "data, high priority" },
	{ 11, 512, 2048, 2048, 8, "data" },     /* its downlink ring holds addresses of ZMC buffers: none given here */
};
#define NRINGS                  (sizeof(ring_cfgs) / sizeof(ring_cfgs[0]))

struct ring {
	uint16_t len, size;
	uint32_t addr_v, size_v;        /* offsets of the arrays in the IPC memory */
	uint32_t buffers;               /* offset of the first buffer */
	uint32_t rp, wp;                /* offsets of the 16-bit counters */
};

struct toc_entry {
	char name[12];
	uint32_t offset, address, size, crc, number;
};

static struct {
	uint32_t stage;                 /* cp=N */
	volatile uint32_t state;
	vm_offset_t pmu, mbox, cfg, gicd, mem;
	uint8_t *ipc;
	struct toc_entry toc[16];
	uint32_t ntoc;
	const struct toc_entry *boot, *main, *vss, *nv;
	bool mem_table;                 /* the image carries its memory table: the check gets the memory's address */
	bool started, up, link;
	bool nv_loaded;                 /* the phone's own NV data is in the CP's memory (not zeros) */
	bool stop;                      /* something seen that this driver stops the CP for */
	bool link_acked;                /* the CP has taken INIT_END: it no longer repeats PHONE_START */
	bool version_answered, bands_asked;
	/* stage 5 */
	int sim_status;                 /* the SIM's last status byte, -1 none yet */
	int reg_state;                  /* the last registration state reported, -1 none yet */
	bool normal_asked, back_asked;  /* "normal" was asked for; then "low power" again */
	uint64_t normal_ms, registered_ms, back_ms, sim_ms;
	uint32_t reg_changes, signals, writes_dropped;
	uint32_t sim_probe;             /* cp_sim=1 */
	bool sim_asked;
	/* stage 6 */
	char apn[64];                   /* cp_apn: letters, digits, '.' and '-' only */
	uint32_t data_step;             /* 0 not begun, 1 defined (asked), 2 activation asked, 3 active (watched), 4 deactivation asked, 5 over */
	uint64_t data_ms;
	int data_result;                /* the general response's result word to the request waited for, -1 none yet */
	uint8_t data_seq;               /* that request's sequence */
	uint32_t data_notes, data_configs, data_packets, data_bytes;
	/* stage 7: the context's addresses are here for the run only and are never logged */
	uint8_t ip4[4], dns4[4], ping4[4];
	bool have4, ping_given;
	uint16_t zc_alloc, zc_pre;      /* the zero-copy ring: the next slot to offer, the next to take: ours, never read back */
	uint16_t dns_port, dns_id;
	uint64_t ping_sent_ms, ping_ms, dns_sent_ms, dns_ms;
	uint32_t data_up, data_up_bytes, data_bad, data_v6, data_other, dns_rcode, dns_answers;
	uint64_t init_end_ms, phone_start_ms;
	uint64_t lpm_sent_ms;           /* when "low power mode" was asked */
	int phone_state;                /* the CP's last "phone state" notification: 1 low power, 2 normal; -1 none yet */
	struct ring ul[NRINGS], dl[NRINGS];
	uint8_t mseq;
	uint32_t frames[NRINGS], rfs_requests, fmt_messages, phone_starts;
	uint32_t rev, ds, wait_s, delay_s, nv_file, vss_on;
	uint32_t bl_scan;               /* cp_bl=1: read BOTA0-2 once, read-only */
	uint32_t more[NRINGS];          /* bytes still to come of a frame longer than one buffer */
	bool multi[NRINGS];             /* the frames now coming on a ring are the rest of a message already seen */
	uint32_t rfs_more;              /* bytes of a file request still to come in later frames */
	/* cp_nv=2: the NV data in RAM (kernel memory only; never written anywhere) and an NV write being received */
	uint8_t *nv_ram;
	uint32_t nv_ram_size;
	bool w_active;
	uint8_t w_id;
	uint32_t w_off, w_len, w_got;
	uint32_t rfs_answered, nv_writes_kept, nv_reads_answered;
	uint32_t if_on, route_on;       /* cp_if=1, cp_route=1 */
	uint32_t if_kb;                 /* cp_if_kb=N */
	uint32_t dropped[NRINGS];       /* such frames dropped */
	/* the modem's state for user space (kern.hakios.cp.info; kern.hakios.cp.addr for root): no identity in any of it */
	uint8_t dns4b[4];               /* the context's second DNS server (never logged) */
	int tech, bars, dbm;            /* the registration's technology, the icon's bars, the strength in dBm; -1/0 none yet */
	char oper[8];                   /* the serving network's operator code (country + network: the same for everyone on it) */
	uint32_t gen;                   /* bumped on every change of the above */
	uint32_t rfs_open, rfs_opens;   /* cp_rfs=1: the CP's file opens answered "not there" (from RAM: nothing is read) */
	uint64_t busy_ms;               /* the last time something came from the CP or waited to go to it */
	uint64_t lost_ms;               /* when a registration was lost (0: registered, or never was) */
} cp;

static int cp_go;
static int cp_state_out;

SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, cp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Galaxy S9's cellular modem (hakios.shannon.c, cp=N)");
SYSCTL_INT(_kern_hakios_cp, OID_AUTO, state, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &cp_state_out, 0, "the stage reached (1 looked, 2 loaded and checked, 3 the CP up, 4 the link up), 100 + the stage that failed, 200 stopped");
SYSCTL_INT(_kern_hakios_cp, OID_AUTO, go, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &cp_go, 0, "set: 1 the state printed, 2 the CP stopped, 3 the version asked");

/* ---- small things ---- */

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline uint32_t
ipc_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(void *)(cp.ipc + off);
}

static inline void
ipc_wr32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(void *)(cp.ipc + off) = v;
}

static inline uint16_t
ipc_rd16(uint32_t off)
{
	return *(volatile uint16_t *)(void *)(cp.ipc + off);
}

static inline void
ipc_wr16(uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(void *)(cp.ipc + off) = v;
}

static void
set_state(uint32_t s)
{
	cp.state = s;
	cp_state_out = (int)s;
}

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static uint64_t
smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* the same call with every answer register kept: the check's x1-x3 are part of its log */
static uint64_t
smc_full(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t regs[4])
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	regs[0] = x0;
	regs[1] = x1;
	regs[2] = x2;
	regs[3] = x3;
	return x0;
}

static const char *
smc_error(uint32_t e)
{
	static const char *const names[] = {
		"no error", "not aligned to 64 KB", "memory too big", "flag out of range", "wrong TZASC region number",
		"wrong boot loader size", "memory out of range", "not aligned to 16 bytes", "memory in secure DRAM",
		"ASP enable failed", "ASP disable failed", "CP not working", "CP already working", "CP already in dump mode",
		"not a valid magic", "CP should be disabled", "CP memory already enabled", "window already set",
		"failed to set the window", "invalid CP base", "corrupted CP memory information",
	};

	return e < sizeof(names) / sizeof(names[0]) ? names[e] : "not one of the listed errors";
}

/* one of the two secure words: false if the secure world would not say */
static bool
secure_read(uint32_t which, uint32_t *value)
{
	uint32_t lo = (uint32_t)smc(SMC_CP, SMC_CP_READ, 0, which);
	uint32_t hi;

	if (lo & 0xffff) {
		say("cp: the secure world would not read CP_CTRL_%s (low half): %#x\n", which == SMC_CP_REG_S ? "S" : "NS", lo);
		return false;
	}
	hi = (uint32_t)smc(SMC_CP, SMC_CP_READ, 1, which);
	if (hi & 0xffff) {
		say("cp: the secure world would not read CP_CTRL_%s (high half): %#x\n", which == SMC_CP_REG_S ? "S" : "NS", hi);
		return false;
	}
	*value = (hi & 0xffff0000u) | (lo >> 16);
	return true;
}

/* bits set or cleared in one of the two secure words, as Samsung's PMU sequences do it: read, change, write, read */
static bool
secure_update(uint32_t which, uint32_t clear, uint32_t set, const char *why)
{
	uint32_t before, after = 0;
	uint32_t answer;

	if (!secure_read(which, &before)) {
		return false;
	}
	answer = (uint32_t)smc(SMC_CP, SMC_CP_WRITE, (before & ~clear) | set, which);
	(void)secure_read(which, &after);
	say("cp: CP_CTRL_%s %#x -> %#x (%s), the secure world answered %#x\n", which == SMC_CP_REG_S ? "S" : "NS", before,
	    after, why, answer);
	return answer == 0;
}

static bool
pmu_wait(uint32_t off, uint32_t mask, uint32_t want, uint32_t ms, const char *name)
{
	uint64_t until = now_ms() + ms;
	uint32_t v;

	do {
		v = rd(cp.pmu, off);
		if ((v & mask) == want) {
			return true;
		}
		delay(100);
	} while (now_ms() < until);
	say("cp: %s stayed %#x (wanted %#x under %#x) for %u ms\n", name, v, want, mask, ms);
	return false;
}

/* the CP's own interrupt to the AP's interrupt controller, as Samsung's sequences wait for it: only looked at */
static bool
cp_irq_pending(void)
{
	return cp.gicd != 0 && (rd(cp.gicd, 0x238) & (1u << 28)) != 0;       /* GICD_ISPENDR14 (0x1238), bit 28 */
}

static void
mbox_raise(uint32_t n)
{
	wr(cp.mbox, MBOX_INTGR1, 1u << n);
}

static void
send_msg(uint16_t value)
{
	/* one word carries every message: the last one must have been taken (its interrupt no longer pending at the CP)
	 * before the next is written over it. Seen: a "data" message written 1 ms after INIT_END replaced it, and the CP
	 * never knew the link was up. 50 ms at most. */
	for (uint32_t i = 0; i < 200 && (rd(cp.mbox, MBOX_INTSR1) & (1u << INT_AP_MSG)) != 0; i++) {
		delay(250);
	}
	wr(cp.mbox, MBOX_ISSR(WORD_AP_MSG), value);
	mbox_raise(INT_AP_MSG);
}

static uint32_t
crc32_of(const uint8_t *p, uint32_t n)
{
	static const uint32_t nibble[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
		0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
	};
	uint32_t c = 0xffffffffu;

	while (n-- != 0) {
		c ^= *p++;
		c = nibble[c & 15] ^ (c >> 4);
		c = nibble[c & 15] ^ (c >> 4);
	}
	return ~c;
}

/* the CP's memory must be nobody's: not XNU's pages, not where the kernel itself was loaded */
extern boolean_t pmap_valid_page(ppnum_t pn);

static bool
memory_is_free(void)
{
	uint64_t here[2] = { kvtophys((vm_offset_t)&cp), kvtophys((vm_offset_t)&memory_is_free) };

	for (uint32_t i = 0; i < 2; i++) {
		if (here[i] >= CP_MEM_PA && here[i] < CP_MEM_PA + CP_MEM_BYTES) {
			say("cp: this kernel itself is loaded in the CP's memory (%#llx): the modem cannot be used on this boot path\n",
			    here[i]);
			return false;
		}
	}
	for (uint64_t pa = CP_MEM_PA; pa < CP_MEM_PA + CP_MEM_BYTES; pa += PAGE_SIZE) {
		if (pmap_valid_page((ppnum_t)(pa >> PAGE_SHIFT))) {
			say("cp: %#llx of the CP's memory is one of XNU's pages: the modem cannot be used with this memory layout\n", pa);
			return false;
		}
	}
	return true;
}

/* ---- stage 1: the look ---- */

static void
look_pmu(const char *when)
{
	uint32_t ns = 0, s = 0;
	bool have_ns = secure_read(SMC_CP_REG_NS, &ns), have_s = secure_read(SMC_CP_REG_S, &s);

	say("cp: PMU %s: CP_STAT %#x, CENTRAL_SEQ_CP config %#x status %#x, RESET_SEQUENCER_STATUS %#x, CLEANY_BUS_CP "
	    "config %#x status %#x\n", when, rd(cp.pmu, PMU_CP_STAT), rd(cp.pmu, PMU_CENTRAL_SEQ_CP_CFG),
	    rd(cp.pmu, PMU_CENTRAL_SEQ_CP_STAT), rd(cp.pmu, PMU_RESET_SEQ_STAT), rd(cp.pmu, PMU_CLEANY_BUS_CP_CFG),
	    rd(cp.pmu, PMU_CLEANY_BUS_CP_STAT));
	say("cp: PMU %s: the CP's power-down words 0x1320-0x1338: %#x %#x %#x %#x %#x %#x %#x\n", when,
	    rd(cp.pmu, 0x1320), rd(cp.pmu, 0x1324), rd(cp.pmu, 0x1328), rd(cp.pmu, 0x132c), rd(cp.pmu, 0x1330),
	    rd(cp.pmu, 0x1334), rd(cp.pmu, 0x1338));
	if (have_ns && have_s) {
		say("cp: PMU %s: the secure words: CP_CTRL_NS %#x, CP_CTRL_S %#x; the CP's interrupt %s\n", when, ns, s,
		    cp_irq_pending() ? "pending" : "not pending");
	}
	/* AP2CP_CFG is NOT read here: it is in the CP's own power domain, and reading it before that domain was ever
	 * started, while the CP is held in reset, or while the running CP has put itself to sleep is a bus error (seen
	 * three times: an SError panic). It is touched once, at the start, as Samsung's driver does. */
}

static void
look_mbox(const char *when)
{
	say("cp: mailbox %s: MCUCTLR %#x, to the AP: mask %#x status %#x, to the CP: mask %#x status %#x\n", when,
	    rd(cp.mbox, MBOX_MCUCTLR), rd(cp.mbox, MBOX_INTMR0), rd(cp.mbox, MBOX_INTSR0), rd(cp.mbox, MBOX_INTMR1),
	    rd(cp.mbox, MBOX_INTSR1));
	say("cp: mailbox %s: words 0-9: %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x (CP's state %u, active %u)\n", when,
	    rd(cp.mbox, MBOX_ISSR(0)), rd(cp.mbox, MBOX_ISSR(1)), rd(cp.mbox, MBOX_ISSR(2)), rd(cp.mbox, MBOX_ISSR(3)),
	    rd(cp.mbox, MBOX_ISSR(4)), rd(cp.mbox, MBOX_ISSR(5)), rd(cp.mbox, MBOX_ISSR(6)), rd(cp.mbox, MBOX_ISSR(7)),
	    rd(cp.mbox, MBOX_ISSR(8)), rd(cp.mbox, MBOX_ISSR(9)), CP_STATE_STATUS(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))),
	    CP_STATE_ACTIVE(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))));
}

/* how much of a stretch of the CP's memory is not zero: a count, never the bytes */
static uint32_t
not_zero(uint32_t off, uint32_t bytes)
{
	const volatile uint32_t *p = (const volatile uint32_t *)(void *)((uint8_t *)cp.mem + off);
	uint32_t n = 0;

	for (uint32_t i = 0; i < bytes / 4; i++) {
		n += p[i] != 0;
	}
	return n * 4;
}

static void
look_mem(void)
{
	const volatile uint32_t *m = (const volatile uint32_t *)cp.mem;

	say("cp: the CP's memory as found: first words %#x %#x %#x %#x; at 0xA0 %#x, at 0x2A0 %#x (\"MEM\" would be an image "
	    "left there by the boot loader)\n", m[0], m[1], m[2], m[3], m[0xa0 / 4], m[0x2a0 / 4]);
	say("cp: the CP's memory as found: bytes not zero in the first 64 KB %u, in MAIN's first MB %u, in the NV place %u "
	    "of 1048576, in the IPC memory's first 64 KB %u; IPC magic %#x access %#x\n", not_zero(0, 0x10000),
	    not_zero(0x10000, 0x100000), not_zero(0x4900000, 0x100000), not_zero(CP_IPC_OFFSET, 0x10000),
	    ipc_rd32(IPC_MAGIC), ipc_rd32(IPC_ACCESS));
}

static const struct toc_entry *
toc_find(const char *name)
{
	for (uint32_t i = 0; i < cp.ntoc; i++) {
		if (strncmp(cp.toc[i].name, name, sizeof(cp.toc[i].name)) == 0) {
			return &cp.toc[i];
		}
	}
	return NULL;
}

static bool
read_toc(void)
{
	static uint8_t head[4096];
	uint64_t size = 0;
	int error = ENXIO;

	/* the storage may come up after this thread: two minutes for it */
	for (uint32_t waited = 0; waited < 240; waited++) {
		error = hakios_ufs_read_partition("RADIO", 0, NULL, 0, &size);
		if (error == 0) {
			break;
		}
		IOSleep(500);
	}
	if (error != 0 || size < sizeof(head)) {
		say("cp: no RADIO partition to read (error %d, %llu bytes)\n", error, size);
		return false;
	}
	error = hakios_ufs_read_partition("RADIO", 0, head, sizeof(head), NULL);
	if (error != 0) {
		say("cp: RADIO's first block could not be read (error %d)\n", error);
		return false;
	}
	cp.ntoc = 0;
	for (uint32_t i = 0; i < 16; i++) {
		struct toc_entry *e = &cp.toc[cp.ntoc];

		memcpy(e, head + 32 * i, sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		e->name[sizeof(e->name) - 1] = 0;
		say("cp: RADIO (%llu bytes) entry %u: %s at %#x in the partition, for the CP at %#x, %u bytes, CRC %#x\n", size,
		    i, e->name, e->offset, e->address, e->size, e->crc);
		cp.ntoc++;
	}
	cp.boot = toc_find("BOOT");
	cp.main = toc_find("MAIN");
	cp.vss = toc_find("VSS");
	cp.nv = toc_find("NV");
	if (cp.ntoc == 0 || strcmp(cp.toc[0].name, "TOC") != 0 || cp.boot == NULL || cp.main == NULL ||
	    cp.boot->address != 0x40000000u || cp.main->address <= cp.boot->address ||
	    (uint64_t)(cp.main->address - cp.boot->address) + cp.main->size > CP_IPC_OFFSET ||
	    (uint64_t)cp.boot->offset + cp.boot->size > size || (uint64_t)cp.main->offset + cp.main->size > size ||
	    cp.boot->size > cp.main->address - cp.boot->address) {
		say("cp: RADIO's table of contents is not the one this driver knows: nothing is loaded\n");
		return false;
	}
	/* the image's own memory table, 0xA0 into BOOT: "MEMn", the CP's size, the rest's size, a count, then
	 * (tag, offset, size) entries. Samsung's kernel gives the secure world the memory's address when it is there. */
	{
		uint32_t at = cp.boot->offset + 0xa0;
		uint32_t tag = 0, count = 0;

		if (cp.boot->offset < sizeof(head) - 0xa0 - 16 - 12 * 5) {
			memcpy(&tag, head + at, 4);
			memcpy(&count, head + at + 12, 4);
		}
		cp.mem_table = (tag & 0xffffff00u) == 0x4d454d00u && count >= 1 && count <= 5;
		if (cp.mem_table) {
			for (uint32_t i = 0; i < count; i++) {
				uint32_t t, a, s;

				memcpy(&t, head + at + 16 + 12 * i, 4);
				memcpy(&a, head + at + 20 + 12 * i, 4);
				memcpy(&s, head + at + 24 + 12 * i, 4);
				say("cp: the image's memory table: %c%c%c at %#x, %#x bytes\n", (char)(t >> 16), (char)(t >> 8), (char)t,
				    a, s);
				if ((t & 0xffffff) == 0x495043 /* "IPC" */ && (a != CP_IPC_OFFSET || s != CP_IPC_BYTES)) {
					say("cp: the image puts the IPC memory somewhere this driver does not expect: nothing is loaded\n");
					return false;
				}
			}
		} else {
			say("cp: the image has no memory table: the device tree's layout is used\n");
		}
	}
	return true;
}

/* ---- stage 2: loaded and checked ---- */

static bool
cp_power_first(void)
{
	uint32_t seq = rd(cp.pmu, PMU_RESET_SEQ_STAT) & 7;

	if (seq != 0) {
		say("cp: the CP's power domain was started before us (RESET_SEQUENCER_STATUS %u): left as it is\n", seq);
		return true;
	}
	say("cp: the CP's power domain has never been started in this boot: starting it (Samsung's \"cp_init\")\n");
	if (!secure_update(SMC_CP_REG_NS, 0, NS_PWRON, "power on") || !secure_update(SMC_CP_REG_S, 0, S_START, "start")) {
		return false;
	}
	delay(1024);
	if (!pmu_wait(PMU_RESET_SEQ_STAT, 7, 5, 500, "RESET_SEQUENCER_STATUS")) {
		return false;
	}
	for (uint32_t i = 0; i < 500 && !cp_irq_pending(); i++) {
		IOSleep(1);
	}
	say("cp: the CP's power domain is up (RESET_SEQUENCER_STATUS %u), its interrupt %s\n",
	    rd(cp.pmu, PMU_RESET_SEQ_STAT) & 7, cp_irq_pending() ? "pending" : "not pending after 500 ms (gone on anyway)");
	return true;
}

/*
 * Where the boot loader puts an entry: cbd's m_offset, (address & 0x3fffffff), which here is the address minus
 * BOOT's (0x40000000): BOOT at 0, MAIN at 0x10000, VSS at 0x7900000, NV at 0x4900000. RADIO's table of contents
 * is never sent, so nothing is put at 0 before BOOT. (Build 41 kept the table at 0 and put BOOT at 0x200 and the
 * secure check answered the same 0x82001010: this is cbd's layout, not the check's answer.)
 */
static uint32_t
entry_off(const struct toc_entry *e)
{
	return e->address - cp.boot->address;
}

static bool
copy_entry(const struct toc_entry *e)
{
	uint32_t at = entry_off(e);
	uint8_t *dst = (uint8_t *)cp.mem + at;
	uint64_t t0 = now_ms();
	uint32_t crc;

	for (uint32_t done = 0; done < e->size;) {
		uint32_t n = MIN(e->size - done, 1u << 20);
		int error = hakios_ufs_read_partition("RADIO", (uint64_t)e->offset + done, dst + done, n, NULL);

		if (error != 0) {
			say("cp: %s: RADIO could not be read at %#x (error %d)\n", e->name, e->offset + done, error);
			return false;
		}
		done += n;
	}
	crc = crc32_of(dst, e->size);
	say("cp: %s: %u bytes of RADIO at %#x copied to the CP's memory at %#x in %llu ms; read back, their CRC is %#x, "
	    "the table's %#x: %s\n", e->name, e->size, e->offset, at, now_ms() - t0, crc, e->crc,
	    crc == e->crc ? "THE SAME" : "DIFFERENT");
	return crc == e->crc;
}

/*
 * cp_bl=1: a read-only look through the boot loader's partitions (BOTA0-2) for the secure monitor's CP check.
 * Nothing is written and no partition is mapped: every byte comes through hakios_ufs_read_partition(), the reader
 * the image uses. The patterns are the CP SMC number and the SSS numbers beside the answer, in a 32-bit
 * constant's byte order, and the strings its messages carry. Logged: how many of each, where the first one is,
 * and 16 bytes around it (never more of the partition).
 */
static const uint8_t bl_smc_cp[4] = { 0x00, 0x07, 0x00, 0x82 };
static const uint8_t bl_sss_clk[4] = { 0x11, 0x10, 0x00, 0x82 };
static const uint8_t bl_answer[4] = { 0x10, 0x10, 0x00, 0x82 };
static const struct {
	const char *what;
	const uint8_t *pat;
	uint32_t len;
} bl_patterns[] = {
	{ "SMC_CP 0x82000700", bl_smc_cp, sizeof(bl_smc_cp) },
	{ "SMC_SSS_CLK 0x82001011", bl_sss_clk, sizeof(bl_sss_clk) },
	{ "0x82001010", bl_answer, sizeof(bl_answer) },
	{ "\"el3_mon\"", (const uint8_t *)"el3_mon", 7 },
	{ "\"CP_CPU\"", (const uint8_t *)"CP_CPU", 6 },
	{ "\"CP_INVALID_CP_BASE\"", (const uint8_t *)"CP_INVALID_CP_BASE", 18 },
	{ "\"CP not working\"", (const uint8_t *)"CP not working", 14 },
	{ "\"CryptoManager\"", (const uint8_t *)"CryptoManager", 13 },
};
#define NBLPATTERNS     (sizeof(bl_patterns) / sizeof(bl_patterns[0]))
#define BL_CHUNK        (64u << 10)

static int
bl_find(const uint8_t *buf, uint32_t len, const uint8_t *pat, uint32_t plen, uint32_t from)
{
	for (uint32_t i = from; i + plen <= len; i++) {
		if (memcmp(buf + i, pat, plen) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static bool
probe_private(const char *name);

static void
scan_bootloader(const char *name, const char *tag)
{
	uint8_t *buf;
	uint64_t size = 0, t0 = now_ms();
	uint64_t first[NBLPATTERNS];
	uint32_t hits[NBLPATTERNS];
	bool have[NBLPATTERNS];
	uint32_t step = BL_CHUNK - 32;          /* 32 bytes of overlap: a hit on a chunk's edge is still seen once */
	int error;

	if (probe_private(name)) {
		say("cp: %s: %s holds identity or keys: its bytes are not shown\n", tag, name);
		return;
	}
	bzero(hits, sizeof(hits));
	bzero(have, sizeof(have));
	error = hakios_ufs_read_partition(name, 0, NULL, 0, &size);
	if (error != 0 || size == 0) {
		say("cp: %s: %s cannot be read (error %d, %llu bytes)\n", tag, name, error, size);
		return;
	}
	buf = kalloc_data(BL_CHUNK, Z_WAITOK);
	if (buf == NULL) {
		say("cp: %s: %s: no %u KB to read it into\n", tag, name, BL_CHUNK >> 10);
		return;
	}
	for (uint64_t at = 0; at < size; at += step) {
		uint32_t n = (uint32_t)MIN(size - at, (uint64_t)BL_CHUNK);

		error = hakios_ufs_read_partition(name, at, buf, n, NULL);
		if (error != 0) {
			say("cp: %s: %s at %#llx could not be read (error %d): stopped there\n", tag, name, at, error);
			break;
		}
		for (uint32_t p = 0; p < NBLPATTERNS; p++) {
			for (uint32_t from = 0;;) {
				int i = bl_find(buf, n, bl_patterns[p].pat, bl_patterns[p].len, from);

				if (i < 0) {
					break;
				}
				/* (a hit wholly inside the overlap was counted when the previous chunk was scanned) */
				if (at != 0 && (uint32_t)i + bl_patterns[p].len <= 32) {
					from = (uint32_t)i + 1;
					continue;
				}
				if (hits[p] == 0) {
					first[p] = at + (uint32_t)i;
					have[p] = true;
				}
				if (hits[p] != 0xffffffffu) {
					hits[p]++;
				}
				from = (uint32_t)i + 1;
			}
		}
	}
	say("cp: %s: %s (%llu bytes) read only and scanned in %llu ms\n", tag, name, size, now_ms() - t0);
	for (uint32_t p = 0; p < NBLPATTERNS; p++) {
		say("cp: %s: %s: %s: %u hit(s)\n", tag, name, bl_patterns[p].what, hits[p]);
	}
	for (uint32_t p = 0; p < NBLPATTERNS; p++) {
		uint8_t around[16];
		uint32_t w[4];
		uint64_t at;

		if (!have[p]) {
			continue;
		}
		at = first[p] < 8 ? 0 : (first[p] & ~3ull) - 8;
		if (at + sizeof(around) > size) {
			continue;
		}
		if (hakios_ufs_read_partition(name, at, around, sizeof(around), NULL) == 0) {
			memcpy(w, around, sizeof(w));
			say("cp: %s: %s: first %s at %#llx, 16 bytes around it: %08x %08x %08x %08x\n", tag, name,
			    bl_patterns[p].what, first[p], w[0], w[1], w[2], w[3]);
		}
	}
	kfree_data(buf, BL_CHUNK);
}

static void
scan_bootloaders(void)
{
	static const char *const names[] = { "BOTA0", "BOTA1", "BOTA2" };

	for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		scan_bootloader(names[i], "cp_bl=1");
	}
}

/*
 * kern.hakios.cp.probe: read-only probes from the console, so one boot answers several questions without another
 * build. Root only, and no boot argument is needed (it does not touch the CP). Commands:
 *   scan NAME           the eight patterns cp_bl=1 uses, on any partition
 *   find NAME HEX       any byte pattern of up to 32 bytes, as hex, on any partition: count and first offsets
 *   dump NAME OFF LEN   LEN bytes (at most 1024) from OFF, 16 to a line
 *   str NAME            up to 32 printable strings of 8 or more characters, with their offsets
 *   dist NAME           zero share, distinct bytes, 64 KB blocks and AArch64 opcodes: plaintext code or not
 * Every byte comes through hakios_ufs_read_partition(): nothing is written, no partition is mapped and no SMC is made.
 */
#define PROBE_DUMP_MAX  1024
#define PROBE_STR_MAX   32
#define PROBE_STR_LEN   64

static int
probe_hexdigit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool
probe_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* (the command's words: cut in place at whitespace) */
static char *
probe_word(char **p)
{
	char *w = *p, *e;

	while (probe_space(*w)) {
		w++;
	}
	if (*w == 0) {
		return NULL;
	}
	for (e = w; *e != 0 && !probe_space(*e); e++) {
	}
	if (*e != 0) {
		*e++ = 0;
	}
	*p = e;
	return w;
}

static int
probe_hex(const char *s, uint8_t *out, uint32_t max, uint32_t *len)
{
	uint32_t n = 0;

	if (s == NULL || s[0] == 0) {
		return EINVAL;
	}
	for (; s[0] != 0; s += 2) {
		int hi = probe_hexdigit(s[0]), lo;

		if (hi < 0 || s[1] == 0 || (lo = probe_hexdigit(s[1])) < 0) {
			return EINVAL;
		}
		if (n == max) {
			return E2BIG;
		}
		out[n++] = (uint8_t)((hi << 4) | lo);
	}
	*len = n;
	return 0;
}

/* (the partitions whose bytes are never printed: identity, keys and calibration; the statistics-only probe is
 * allowed, the ones that show data are not) */
static bool
probe_private(const char *name)
{
	static const char *const names[] = {
		"EFS", "SEC_EFS", "CPEFS", "KEYSTORAGE", "PERSISTENT", "NAD_REFER"
	};

	for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(name, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void
probe_find(const char *name, const uint8_t *pat, uint32_t plen)
{
	uint8_t *buf;
	uint64_t size = 0, first = 0, t0 = now_ms();
	uint32_t count = 0, told = 0, step = BL_CHUNK - 32;
	int error;

	if (probe_private(name)) {
		say("cp: probe: %s holds identity or keys: its bytes are not shown\n", name);
		return;
	}
	error = hakios_ufs_read_partition(name, 0, NULL, 0, &size);
	if (error != 0 || size == 0) {
		say("cp: probe: %s cannot be read (error %d, %llu bytes)\n", name, error, size);
		return;
	}
	buf = kalloc_data(BL_CHUNK, Z_WAITOK);
	if (buf == NULL) {
		say("cp: probe: %s: no %u KB to read it into\n", name, BL_CHUNK >> 10);
		return;
	}
	for (uint64_t at = 0; at < size; at += step) {
		uint32_t n = (uint32_t)MIN(size - at, (uint64_t)BL_CHUNK);

		error = hakios_ufs_read_partition(name, at, buf, n, NULL);
		if (error != 0) {
			say("cp: probe: %s at %#llx could not be read (error %d): stopped there\n", name, at, error);
			break;
		}
		for (uint32_t i = 0; i + plen <= n; i++) {
			if (at != 0 && i + plen <= 32) {
				continue;               /* (counted with the chunk before) */
			}
			if (memcmp(buf + i, pat, plen) != 0) {
				continue;
			}
			if (count == 0) {
				first = at + i;
			}
			if (told < 8) {
				say("cp: probe: %s: hit %u at %#llx\n", name, told + 1, at + i);
				told++;
			}
			count++;
		}
	}
	say("cp: probe: %s (%llu bytes): %u hit(s) in %llu ms\n", name, size, count, now_ms() - t0);
	if (count != 0) {
		uint8_t around[16];
		uint32_t w[4];
		uint64_t at = first & ~3ull;

		if (at + sizeof(around) <= size && hakios_ufs_read_partition(name, at, around, sizeof(around), NULL) == 0) {
			memcpy(w, around, sizeof(w));
			say("cp: probe: %s: first at %#llx, 16 bytes from %#llx: %08x %08x %08x %08x\n", name, first, at,
			    w[0], w[1], w[2], w[3]);
		}
	}
	kfree_data(buf, BL_CHUNK);
}

static void
probe_dump(const char *name, uint64_t off, uint32_t len)
{
	uint8_t row[16];
	uint64_t size = 0;
	int error;

	if (probe_private(name)) {
		say("cp: probe: %s holds identity or keys: its bytes are not shown\n", name);
		return;
	}
	error = hakios_ufs_read_partition(name, 0, NULL, 0, &size);
	if (error != 0 || size == 0) {
		say("cp: probe: %s cannot be read (error %d, %llu bytes)\n", name, error, size);
		return;
	}
	if (len > PROBE_DUMP_MAX) {
		len = PROBE_DUMP_MAX;
		say("cp: probe: %s: %u bytes at most\n", name, PROBE_DUMP_MAX);
	}
	for (uint32_t done = 0; done < len; ) {
		char line[160];
		size_t at = 0;
		uint32_t n = MIN(len - done, 16u);

		error = hakios_ufs_read_partition(name, off + done, row, n, NULL);
		if (error != 0) {
			say("cp: probe: %s at %#llx could not be read (error %d)\n", name, off + done, error);
			return;
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, "cp: probe: %s %#llx:", name, off + done);
		for (uint32_t i = 0; i < 16; i++) {
			if (i < n) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %02x", row[i]);
			} else {
				at += (size_t)snprintf(line + at, sizeof(line) - at, "   ");
			}
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, "  |");
		for (uint32_t i = 0; i < n; i++) {
			char c = (char)row[i];

			at += (size_t)snprintf(line + at, sizeof(line) - at, "%c", c >= 0x20 && c < 0x7f ? c : '.');
		}
		snprintf(line + at, sizeof(line) - at, "|");
		say("%s\n", line);
		done += n;
	}
}

static void
probe_str(const char *name)
{
	uint8_t *buf;
	uint64_t size = 0;
	char cur[PROBE_STR_LEN];
	uint64_t curat = 0;
	uint32_t curlen = 0, told = 0;
	int error;

	if (probe_private(name)) {
		say("cp: probe: %s holds identity or keys: its bytes are not shown\n", name);
		return;
	}
	error = hakios_ufs_read_partition(name, 0, NULL, 0, &size);
	if (error != 0 || size == 0) {
		say("cp: probe: %s cannot be read (error %d, %llu bytes)\n", name, error, size);
		return;
	}
	buf = kalloc_data(BL_CHUNK, Z_WAITOK);
	if (buf == NULL) {
		say("cp: probe: %s: no %u KB to read it into\n", name, BL_CHUNK >> 10);
		return;
	}
	for (uint64_t at = 0; at < size && told < PROBE_STR_MAX; at += BL_CHUNK) {
		uint32_t n = (uint32_t)MIN(size - at, (uint64_t)BL_CHUNK);

		error = hakios_ufs_read_partition(name, at, buf, n, NULL);
		if (error != 0) {
			say("cp: probe: %s at %#llx could not be read (error %d): stopped there\n", name, at, error);
			break;
		}
		for (uint32_t i = 0; i < n; i++) {
			uint8_t c = buf[i];

			if (c >= 0x20 && c < 0x7f) {
				if (curlen == 0) {
					curat = at + i;
				}
				if (curlen < sizeof(cur)) {
					cur[curlen++] = (char)c;
				}
				continue;
			}
			if (curlen >= 8) {
				say("cp: probe: %s: %#llx \"%.*s\"\n", name, curat, (int)curlen, cur);
				told++;
				if (told >= PROBE_STR_MAX) {
					break;
				}
			}
			curlen = 0;
		}
	}
	if (told < PROBE_STR_MAX && curlen >= 8) {
		say("cp: probe: %s: %#llx \"%.*s\"\n", name, curat, (int)curlen, cur);
		told++;
	}
	say("cp: probe: %s (%llu bytes): %u string(s) of 8 or more characters%s\n", name, size, told,
	    told >= PROBE_STR_MAX ? " (stopped at the limit)" : "");
	kfree_data(buf, BL_CHUNK);
}

static void
probe_dist(const char *name)
{
	static const uint32_t a64_nop = 0xd503201fu;
	static const uint32_t a64_ret = 0xd65f03c0u;
	static const uint32_t a64_stp = 0xa9bf7bfdu;
	uint8_t *buf;
	uint64_t size = 0, total = 0, zeros = 0, t0 = now_ms();
	uint32_t hist[256] = { 0 }, blocks = 0, min_d = 256, max_d = 0, all256 = 0;
	uint32_t nop = 0, ret = 0, stp = 0, distinct = 0, zpm;
	int error;

	error = hakios_ufs_read_partition(name, 0, NULL, 0, &size);
	if (error != 0 || size == 0) {
		say("cp: probe: %s cannot be read (error %d, %llu bytes)\n", name, error, size);
		return;
	}
	buf = kalloc_data(BL_CHUNK, Z_WAITOK);
	if (buf == NULL) {
		say("cp: probe: %s: no %u KB to read it into\n", name, BL_CHUNK >> 10);
		return;
	}
	for (uint64_t at = 0; at < size; at += BL_CHUNK) {
		uint32_t n = (uint32_t)MIN(size - at, (uint64_t)BL_CHUNK);
		uint32_t bh[256] = { 0 }, d = 0;

		error = hakios_ufs_read_partition(name, at, buf, n, NULL);
		if (error != 0) {
			say("cp: probe: %s at %#llx could not be read (error %d): stopped there\n", name, at, error);
			break;
		}
		for (uint32_t i = 0; i < n; i++) {
			uint8_t b = buf[i];

			hist[b]++;
			bh[b]++;
			total++;
			zeros += b == 0;
		}
		for (uint32_t v = 0; v < 256; v++) {
			d += bh[v] != 0;
		}
		blocks++;
		min_d = MIN(min_d, d);
		max_d = MAX(max_d, d);
		all256 += d == 256;
		for (uint32_t i = 0; i + 4 <= n; i += 4) {
			uint32_t w;

			memcpy(&w, buf + i, sizeof(w));
			nop += w == a64_nop;
			ret += w == a64_ret;
			stp += w == a64_stp;
		}
	}
	kfree_data(buf, BL_CHUNK);
	for (uint32_t v = 0; v < 256; v++) {
		distinct += hist[v] != 0;
	}
	zpm = total != 0 ? (uint32_t)((zeros * 1000 + total / 2) / total) : 0;
	say("cp: probe: %s (%llu bytes) in %llu ms: zero %u.%u%%, distinct %u\n", name, size, now_ms() - t0, zpm / 10,
	    zpm % 10, distinct);
	say("cp: probe: %s: %u 64 KB blocks: distinct bytes %u..%u, blocks with all 256 values %u\n", name, blocks, min_d,
	    max_d, all256);
	say("cp: probe: %s: AArch64 words (4-byte alignment): NOP %u, RET %u, STP x29/x30 %u\n", name, nop, ret, stp);
	{
		char top[160];
		size_t at = 0;
		bool used[256] = { false };

		at += (size_t)snprintf(top + at, sizeof(top) - at, "cp: probe: %s: top bytes:", name);
		for (uint32_t k = 0; k < 6; k++) {
			uint32_t best = 0, bestn = 0, pm;

			for (uint32_t v = 0; v < 256; v++) {
				if (!used[v] && hist[v] > bestn) {
					best = v;
					bestn = hist[v];
				}
			}
			if (bestn == 0) {
				break;
			}
			used[best] = true;
			pm = total != 0 ? (uint32_t)(((uint64_t)bestn * 1000 + total / 2) / total) : 0;
			at += (size_t)snprintf(top + at, sizeof(top) - at, " %02x %u.%u%%", best, pm / 10, pm % 10);
		}
		say("%s\n", top);
	}
}

/* cp_nv=2: EFS read through hakios_ufs_read_partition, which only reads (bsd/dev/herolte_ufs.c) */
static int
efs_read(__unused void *ctx, uint64_t off, void *dst, uint32_t len)
{
	return hakios_ufs_read_partition("EFS", off, dst, len, NULL);
}

/*
 * cp_nv=2: /nv_data.bin of the EFS partition into cp.nv_ram (size bytes, the NV place's size), read-only. EFS is not
 * mounted and nothing is ever written to it: the reader has no write path, and the copy stays in kernel memory for the
 * run. Logged: sizes, and the first four hex digits of the copy's CRC-32. Nothing of its bytes.
 */
static bool
nv_from_efs(uint32_t size)
{
	struct ext4ro fs = { .read = efs_read, .ctx = NULL };
	uint64_t part = 0, got = 0, t0 = now_ms();
	int e;

	if ((e = hakios_ufs_read_partition("EFS", 0, NULL, 0, &part)) != 0) {
		say("cp: cp_nv=2: the EFS partition cannot be read (errno %d): not going on\n", e);
		return false;
	}
	if ((e = ext4ro_open(&fs)) != 0) {
		say("cp: cp_nv=2: EFS (%llu bytes) is no ext4 this reader knows (errno %d): not going on\n", part, e);
		return false;
	}
	say("cp: cp_nv=2: EFS's superblock: incompat %#x, compat %#x, ro_compat %#x%s\n", fs.incompat, fs.compat,
	    fs.ro_compat, (fs.incompat & EXT4RO_INCOMPAT_ENCRYPT) ? "; the encrypt feature is on, so an encrypted inode "
	    "is refused (a plain nv_data.bin is read)" : "");
	fs.blk = kalloc_data(fs.block_size, Z_WAITOK);
	fs.blk2 = kalloc_data(fs.block_size, Z_WAITOK);
	cp.nv_ram = kalloc_data(size, Z_WAITOK | Z_ZERO);
	if (fs.blk == NULL || fs.blk2 == NULL || cp.nv_ram == NULL) {
		e = ENOMEM;
	} else {
		e = ext4ro_read_file(&fs, "/nv_data.bin", cp.nv_ram, size, &got);
	}
	if (fs.blk != NULL) {
		kfree_data(fs.blk, fs.block_size);
	}
	if (fs.blk2 != NULL) {
		kfree_data(fs.blk2, fs.block_size);
	}
	if (e != 0 || got != size) {
		say("cp: cp_nv=2: EFS's /nv_data.bin not read (errno %d, %llu bytes, the NV place has %u): not going on\n", e,
		    got, size);
		if (cp.nv_ram != NULL) {
			bzero(cp.nv_ram, size);
			kfree_data(cp.nv_ram, size);
			cp.nv_ram = NULL;
		}
		return false;
	}
	cp.nv_ram_size = size;
	say("cp: NV: EFS's /nv_data.bin read READ-ONLY (EFS %llu bytes, ext4 blocks of %u; the file %u bytes, CRC-32 %04x..., "
	    "%llu ms): kept in RAM only\n", part, fs.block_size, size, crc32_of(cp.nv_ram, size) >> 16, now_ms() - t0);
	return true;
}

static bool
fill_nv(void)
{
	uint32_t at, size;
	uint8_t *dst;
	const uint8_t *file = NULL;
	uint64_t file_size = 0;

	if (cp.nv == NULL) {
		say("cp: the image names no NV place\n");
		return true;
	}
	at = cp.nv->address - cp.boot->address;
	size = cp.nv->size;
	if (cp.nv->address < cp.main->address + cp.main->size || (uint64_t)at + size > CP_IPC_OFFSET || size > (4u << 20)) {
		say("cp: the NV place (%#x, %u bytes) is not where this driver expects one: left alone\n", at, size);
		return false;
	}
	dst = (uint8_t *)cp.mem + at;
	if (cp.nv_file == 2) {
		if (!nv_from_efs(size)) {
			return false;
		}
		memcpy(dst, cp.nv_ram, size);
		cp.nv_loaded = true;
		say("cp: NV: the RAM copy of EFS's nv_data.bin (%u bytes) copied to the CP's memory at %#x\n", size, at);
	} else if (cp.nv_file) {
		file = hakios_image_file("fw", "nv_data.bin", &file_size, NULL);
		if (file == NULL || file_size != size) {
			say("cp: cp_nv=1, but the boot image carries no nv_data.bin of %u bytes (found %llu): not going on\n", size,
			    file_size);
			return false;
		}
		memcpy(dst, file, size);
		cp.nv_loaded = true;
		say("cp: NV: the builder's nv_data.bin (%u bytes, never from the EFS partition by this kernel) copied to the "
		    "CP's memory at %#x\n", size, at);
	} else {
		bzero(dst, size);
		say("cp: NV: the place at %#x (%u bytes) filled with zeros: the CP gets no non-volatile data in this run\n", at,
		    size);
	}
	return true;
}

/*
 * Whatever the boot loader left in the boot region (build 40 found its payload text, "loader.e...", at +0) is wiped
 * before the images go in. If the secure world hashes a span rather than each entry alone, a leftover between BOOT and
 * MAIN would fail it. Only the CP's own memory is written, and only while its door is open (mode 2).
 */
static void
clear_boot_region(void)
{
	volatile uint64_t *p = (volatile uint64_t *)(void *)cp.mem;
	uint64_t t0 = now_ms();

	for (uint32_t off = 0; off < CP_IPC_OFFSET; off += 8) {
		p[off / 8] = 0;
	}
	__builtin_arm_dsb(0xf);
	say("cp: the boot region (+0 to +%#x, %u MB) cleared in %llu ms: nothing the loader left is between the images\n",
	    CP_IPC_OFFSET, CP_IPC_OFFSET >> 20, now_ms() - t0);
}

static bool
load_and_check(void)
{
	const volatile uint32_t *m = (const volatile uint32_t *)(void *)cp.mem;
	uint32_t answer, without, clk_on, clk_off, boot_at, map_at;
	uint64_t mode, regs[4];

	if (!cp_power_first()) {
		return false;
	}
	clk_on = (uint32_t)smc(SMC_SSS_CLK, 0, 0, 0);
	answer = (uint32_t)smc(SMC_CP, 2, 0, 0);
	clk_off = (uint32_t)smc(SMC_SSS_CLK, 1, 0, 0);
	say("cp: the hash engine's clock: on answered %#x, off %#x\n", clk_on, clk_off);
	say("cp: the secure world asked to open the CP's memory for loading (mode 2): it answered %#x (%s)\n", answer,
	    smc_error(answer));
	/* (Samsung's daemon goes on whatever this answers: "CP not working" is what a CP that never ran gives) */

	clear_boot_region();
	if (!copy_entry(cp.boot) || !copy_entry(cp.main)) {
		say("cp: the image in the CP's memory is not the image in RADIO: the secure world is not asked, nothing started\n");
		return false;
	}
	if (cp.vss != NULL && cp.vss_on) {
		if (cp.vss->address - cp.boot->address + (uint64_t)cp.vss->size > CP_IPC_OFFSET ||
		    cp.vss->address < cp.main->address + cp.main->size) {
			say("cp: VSS is not where this driver expects it: not copied\n");
		} else if (!copy_entry(cp.vss)) {
			return false;
		}
	}
	if (!fill_nv()) {
		return false;
	}
	/* the two places the secure world reads when it is given the memory's address: the CP memory's first word
	 * (BOOT's, at 0) and the image's memory map ("MEMn") at BOOT + 0xA0 */
	boot_at = entry_off(cp.boot);
	map_at = boot_at + 0xa0;
	say("cp: the CP's memory after the load: at 0 (BOOT) %#x %#x %#x %#x\n", m[0], m[1], m[2], m[3]);
	say("cp: the CP's memory after the load: at %#x (BOOT) %#x %#x %#x %#x; at %#x (its memory map) %#x %#x %#x %#x\n",
	    boot_at, m[boot_at / 4], m[boot_at / 4 + 1], m[boot_at / 4 + 2], m[boot_at / 4 + 3], map_at, m[map_at / 4],
	    m[map_at / 4 + 1], m[map_at / 4 + 2], m[map_at / 4 + 3]);

	mode = cp.mem_table ? CP_MEM_PA : 0;
	clk_on = (uint32_t)smc(SMC_SSS_CLK, 0, 0, 0);
	(void)smc_full(SMC_CP, mode, cp.boot->size, cp.main->size, regs);
	answer = (uint32_t)regs[0];
	clk_off = (uint32_t)smc(SMC_SSS_CLK, 1, 0, 0);
	say("cp: the hash engine's clock around the check: on answered %#x, off %#x\n", clk_on, clk_off);
	say("cp: THE SECURE WORLD'S CHECK of the image (mode 0, memory %#llx, BOOT %u bytes, MAIN %u bytes): it answered "
	    "%#x (%s); its other registers %#llx %#llx %#llx\n", mode, cp.boot->size, cp.main->size, answer,
	    smc_error(answer), regs[1], regs[2], regs[3]);
	if (answer != 0 && mode != 0) {
		/*
		 * The same check without the memory's address in the mode. Samsung adds the address only when the image
		 * carries its memory table (this one does), so this is not the stock call: it is made for the log, because
		 * on an image without the table mode 0 alone is all the secure world is ever given. If this answer is not
		 * the address error (CP_INVALID_CP_BASE, 19), the address is not what the first call refused.
		 */
		clk_on = (uint32_t)smc(SMC_SSS_CLK, 0, 0, 0);
		without = (uint32_t)smc(SMC_CP, 0, cp.boot->size, cp.main->size);
		clk_off = (uint32_t)smc(SMC_SSS_CLK, 1, 0, 0);
		say("cp: the same check with mode 0 alone (no memory address), clock on %#x off %#x: it answered %#x (%s)\n",
		    clk_on, clk_off, without, smc_error(without));
	}
	if (answer != 0) {
		say("cp: the secure world did not accept the image: stopping here, the CP is not started\n");
		return false;
	}
	return true;
}

/* ---- the CP back in reset (Samsung's "reset_assert") ---- */

static void
cp_stop(const char *why)
{
	bool started = cp.started;

	if (cp.pmu == 0) {
		return;                         /* (never mapped: no register of it was written) */
	}
	if (started) {
		say("cp: stopping the CP (%s)\n", why);
		mbox_raise(INT_AP_WAKEUP);
		IOSleep(5);
	} else {
		/*
		 * The load failed before start(): the domain is still powered (cp_power_first(), CP_CTRL_NS's NS_PWRON), so
		 * it is put back down even though the CP itself never ran. Its interrupt is not raised: nothing is listening.
		 */
		say("cp: powering the CP's domain down (%s)\n", why);
	}
	(void)secure_update(SMC_CP_REG_NS, 0, NS_DUMP_PC_NO_PG, "keep its program counter readable");
	wr(cp.pmu, 0x1320, rd(cp.pmu, 0x1320) & ~3u);
	wr(cp.pmu, 0x1324, rd(cp.pmu, 0x1324) & ~1u);
	wr(cp.pmu, 0x1328, rd(cp.pmu, 0x1328) & ~3u);
	wr(cp.pmu, 0x132c, rd(cp.pmu, 0x132c) & ~1u);
	wr(cp.pmu, 0x1330, rd(cp.pmu, 0x1330) | 1u);
	wr(cp.pmu, 0x1334, rd(cp.pmu, 0x1334) & ~1u);
	wr(cp.pmu, 0x1338, rd(cp.pmu, 0x1338) & ~1u);
	(void)secure_update(SMC_CP_REG_NS, 0, NS_RESET_SET, "reset");
	wr(cp.pmu, PMU_CENTRAL_SEQ_CP_CFG, rd(cp.pmu, PMU_CENTRAL_SEQ_CP_CFG) & ~(1u << 16));
	delay(1024);
	if (pmu_wait(PMU_CENTRAL_SEQ_CP_STAT, 0xffu << 16, 0x80u << 16, 200, "CENTRAL_SEQ_CP_STATUS")) {
		say("cp: THE CP IS IN RESET (CENTRAL_SEQ_CP_STATUS %#x)\n", rd(cp.pmu, PMU_CENTRAL_SEQ_CP_STAT));
	}
	/* the mailbox's interrupts to the AP masked and cleared, nothing pending for whoever boots next */
	wr(cp.mbox, MBOX_INTMR0, 0xffff0000u);
	wr(cp.mbox, MBOX_INTCR0, 0xffff0000u);
	cp.started = cp.up = cp.link = cp.link_acked = false;
	look_pmu("after the stop");
	look_mbox("after the stop");
}

/* ---- stage 3: started ---- */

/* a boot loader frame on the old raw queue */
static bool
boot_send(uint32_t word)
{
	uint8_t frame[8] = { 0xfc, CH_BOOT, 8, 0, (uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16), (uint8_t)(word >> 24) };
	uint32_t head = ipc_rd32(IPC_RAW_TX_HEAD), tail = ipc_rd32(IPC_RAW_TX_TAIL);

	if (head >= IPC_RAW_TX_BYTES || tail >= IPC_RAW_TX_BYTES) {
		say("cp: the boot queue's pointers are out of range (%u, %u)\n", head, tail);
		return false;
	}
	for (uint32_t i = 0; i < sizeof(frame); i++) {
		cp.ipc[IPC_RAW_TX_BUF + (head + i) % IPC_RAW_TX_BYTES] = frame[i];
	}
	__builtin_arm_dsb(0xf);
	ipc_wr32(IPC_RAW_TX_HEAD, (head + sizeof(frame)) % IPC_RAW_TX_BYTES);
	return true;
}

static bool
boot_receive(uint32_t *word, uint32_t ms)
{
	uint64_t until = now_ms() + ms;

	do {
		uint32_t head = ipc_rd32(IPC_RAW_RX_HEAD), tail = ipc_rd32(IPC_RAW_RX_TAIL);
		uint32_t have;

		if (head < IPC_RAW_RX_BYTES && tail < IPC_RAW_RX_BYTES &&
		    (have = (head + IPC_RAW_RX_BYTES - tail) % IPC_RAW_RX_BYTES) >= 8) {
			uint8_t frame[8];

			for (uint32_t i = 0; i < 8; i++) {
				frame[i] = cp.ipc[IPC_RAW_RX_BUF + (tail + i) % IPC_RAW_RX_BYTES];
			}
			/* everything that was there is taken: the boot loader says one thing at a time */
			ipc_wr32(IPC_RAW_RX_TAIL, head);
			memcpy(word, frame + 4, 4);
			if ((frame[0] & 0xf8) != 0xf8 || frame[1] != CH_BOOT) {
				say("cp: the boot queue held %u bytes that are no boot frame: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    have, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
				return false;
			}
			return true;
		}
		IOSleep(1);
	} while (now_ms() < until);
	return false;
}

static bool
boot_ask(uint32_t question, uint32_t expected)
{
	uint32_t answer = 0;
	uint64_t t0 = now_ms();

	if (!boot_send(question)) {
		return false;
	}
	if (!boot_receive(&answer, 5000)) {
		say("cp: the boot loader was asked %#x and said nothing in 5 s (queue to it: head %u tail %u; from it: head %u "
		    "tail %u; mailbox status %#x, word 1 %#x, CP's state word %#x)\n", question, ipc_rd32(IPC_RAW_TX_HEAD),
		    ipc_rd32(IPC_RAW_TX_TAIL), ipc_rd32(IPC_RAW_RX_HEAD), ipc_rd32(IPC_RAW_RX_TAIL), rd(cp.mbox, MBOX_INTSR0),
		    rd(cp.mbox, MBOX_ISSR(WORD_CP_MSG)), rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE)));
		return false;
	}
	say("cp: the boot loader was asked %#x and answered %#x after %llu ms (%s)\n", question, answer, now_ms() - t0,
	    answer == expected ? "as Android's daemon expects" : "NOT the expected answer");
	return answer == expected;
}

/* the table of rings the running CP reads when it is told PIF_INIT_DONE */
static void zc_offer(void);

static void
write_rings(void)
{
	uint32_t desc = SBD_DESC + SBD_GLOBAL_BYTES;    /* the next free byte of the table's 62 KB */
	uint32_t buffers = SBD_BUFFERS;
	uint32_t counters;

	bzero(cp.ipc + SBD_DESC, SBD_BUFFERS - SBD_DESC);
	counters = desc;
	desc += 2 * 2 * 2 * NRINGS;                      /* u16 ul_rp[], ul_wp[], dl_rp[], dl_wp[] */
	ipc_wr32(SBD_DESC + 0, 1);                      /* version */
	ipc_wr32(SBD_DESC + 4, NRINGS);
	ipc_wr32(SBD_DESC + 8, counters);
	for (uint32_t i = 0; i < NRINGS; i++) {
		const struct ring_cfg *c = &ring_cfgs[i];

		for (uint32_t dir = 0; dir < 2; dir++) {
			struct ring *r = dir == 0 ? &cp.ul[i] : &cp.dl[i];
			uint32_t d = SBD_DESC + SBD_RB_DESC + (i * 2 + dir) * 20;

			r->len = dir == 0 ? c->ul_len : c->dl_len;
			r->size = dir == 0 ? c->ul_size : c->dl_size;
			r->buffers = buffers;
			buffers += (uint32_t)r->len * r->size;
			r->rp = counters + 2 * (NRINGS * (dir * 2 + 0) + i);
			r->wp = counters + 2 * (NRINGS * (dir * 2 + 1) + i);
			r->addr_v = desc;
			desc += 4u * r->len;
			r->size_v = desc;
			desc += 4u * r->len;
			for (uint32_t k = 0; k < r->len; k++) {
				ipc_wr32(r->addr_v + 4 * k, r->buffers + k * r->size);
			}
			/* the ring's descriptor: channel, 0, direction, signalling by interrupt, the message to send for it,
			 * length, number, buffer size, payload offset */
			ipc_wr16(d + 0, c->ch);
			ipc_wr16(d + 2, 0);
			ipc_wr16(d + 4, (uint16_t)dir);
			ipc_wr16(d + 6, 1);
			ipc_wr32(d + 8, MSG_VALID | MSG_DATA);
			ipc_wr16(d + 12, r->len);
			ipc_wr16(d + 14, (uint16_t)i);
			ipc_wr16(d + 16, r->size);
			ipc_wr16(d + 18, 0);
			/* the channel's entry: where its two descriptors and their offset arrays are */
			ipc_wr32(SBD_DESC + SBD_RB_CH + i * 16 + dir * 8 + 0, d);
			ipc_wr32(SBD_DESC + SBD_RB_CH + i * 16 + dir * 8 + 4, r->addr_v);
		}
	}
	__builtin_arm_dsb(0xf);
	cp.zc_alloc = cp.zc_pre = 0;
	zc_offer();                     /* (stage 7 only) */
	say("cp: the ring table written: %u channels, the table ends at %u of 65536, the buffers at %u of %u\n",
	    (uint32_t)NRINGS, desc, buffers, CP_IPC_BYTES);
}

/* ---- stage 4: the link ---- */

static bool fmt_send(uint8_t group, uint8_t index, uint8_t type, const uint8_t *data, uint16_t n);

static bool
ring_send(uint32_t i, const uint8_t *frame, uint32_t n, bool tell, uint32_t tag)
{
	struct ring *r = &cp.ul[i];
	uint16_t wp = ipc_rd16(r->wp), rp = ipc_rd16(r->rp);

	if (n > r->size || wp >= r->len || rp >= r->len || (uint16_t)((wp + 1) % r->len) == rp) {
		say("cp: no room to send on channel %u (write %u, read %u of %u)\n", ring_cfgs[i].ch, wp, rp, r->len);
		return false;
	}
	memcpy(cp.ipc + r->buffers + (uint32_t)wp * r->size, frame, n);
	ipc_wr32(r->size_v + 4u * wp, n | tag);         /* (a data ring: the packet's channel in the upper half) */
	__builtin_arm_dsb(0xf);
	ipc_wr16(r->wp, (uint16_t)((wp + 1) % r->len));
	__builtin_arm_dsb(0xf);
	if (tell) {
		send_msg(MSG_VALID | MSG_DATA);
	}
	return true;
}

/* one control message to the CP: the group, the number in it, the type (2: "get") */
static bool
fmt_send(uint8_t group, uint8_t index, uint8_t type, const uint8_t *data, uint16_t n)
{
	uint8_t frame[160];
	uint16_t len = (uint16_t)(7 + n);

	if (!cp.link || n > sizeof(frame) - 11) {       /* (cp.link is set before INIT_END is sent) */
		return false;
	}
	frame[0] = 0xf8;
	frame[1] = CH_FMT;
	frame[2] = (uint8_t)(4 + len);
	frame[3] = (uint8_t)((4u + len) >> 8);
	frame[4] = (uint8_t)len;
	frame[5] = (uint8_t)(len >> 8);
	frame[6] = ++cp.mseq;
	frame[7] = 0;
	frame[8] = group;
	frame[9] = index;
	frame[10] = type;
	if (n != 0) {
		memcpy(frame + 11, data, n);
	}
	say("cp: sent to the CP on channel 235: group %#x number %#x type %#x, sequence %u, %u bytes of data\n", group, index,
	    type, cp.mseq, n);
	/* while the CP has not taken INIT_END the frame only waits in the ring: the "data" message would replace
	 * INIT_END in the one message word (the watch loop tells the CP once it has) */
	return ring_send(1, frame, 4u + len, cp.link_acked, 0);
}

/* the phone state asked for: 1 low power (bytes 01 00), 2 normal (02 02): Samsung IPC's power group 0x01, number
 * 0x07, "execute": what Android's RIL sends when airplane mode is switched on and off */
static void
ask_phone_state(int state)
{
	static const uint8_t low[2] = { 0x01, 0x00 }, normal[2] = { 0x02, 0x02 };

	(void)fmt_send(0x01, 0x07, 0x01, state == 2 ? normal : low, 2);
}

/* the band selection asked for (network group 0x08, number 7, "get", no data): what bands the modem is set to use.
 * It sets nothing and carries no identity; its answer is logged as its length and first bytes. */
static void
ask_bands(void)
{
	(void)fmt_send(0x08, 0x07, 0x02, NULL, 0);
}

/* the modem's software version asked for: Samsung IPC's "MISC" group (0x0A), number 1, "get", one byte 0xFF.
 * It sets nothing. (Number 3 of the same group is the serial number: never asked.) */
static void
ask_version(void)
{
	static const uint8_t all = 0xff;

	(void)fmt_send(0x0a, 0x01, 0x02, &all, 1);
}

static void
print_text(const char *what, const uint8_t *p, uint32_t n)
{
	char text[65];
	uint32_t k = 0;

	for (uint32_t i = 0; i < n && k < sizeof(text) - 1 && p[i] != 0; i++) {
		text[k++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
	}
	text[k] = 0;
	say("cp:   %s \"%s\"\n", what, text);
}

static void
fmt_received(const uint8_t *m, uint32_t n)
{
	uint16_t len;
	uint8_t group, index, type;

	if (n < 7) {
		say("cp: from the CP on channel 235: %u bytes, too short for a message\n", n);
		return;
	}
	memcpy(&len, m, 2);
	group = m[4];
	index = m[5];
	type = m[6];
	cp.fmt_messages++;
	say("cp: from the CP on channel 235: group %#x number %#x type %#x (%s), its sequence %u answering ours %u, %u "
	    "bytes of data (its length word %u)\n", group, index, type, type == 1 ? "indication" : type == 2 ? "response" :
	    type == 3 ? "notification" : "?", m[2], m[3], n - 7, len);
	if (group == 0x0a && index == 0x01 && type == 2 && n >= 7 + 64) {
		/* the version answer: text fields of 32 bytes: software, hardware, calibration date (no identity in them) */
		say("cp: THE MODEM ANSWERED THE VERSION QUESTION (%u bytes):\n", n - 7);
		print_text("software version", m + 7 + 1, 32);
		if (n >= 7 + 1 + 64) {
			print_text("hardware version", m + 7 + 33, 32);
		}
		if (n >= 7 + 1 + 96) {
			print_text("calibration date", m + 7 + 65, 32);
		}
		cp.version_answered = true;
	} else if (group == 0x08 && index == 0x07 && type == 2) {
		say("cp: THE MODEM ANSWERED THE BAND SELECTION QUESTION (%u bytes): %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    n - 7, n > 7 ? m[7] : 0, n > 8 ? m[8] : 0, n > 9 ? m[9] : 0, n > 10 ? m[10] : 0, n > 11 ? m[11] : 0,
		    n > 12 ? m[12] : 0, n > 13 ? m[13] : 0, n > 14 ? m[14] : 0);
	} else if (group == 0x80 && n >= 7 + 5) {
		/* the general response: which request it answers and a 16-bit result */
		say("cp:   a general response to group %#x number %#x type %#x: result %#x\n", m[7], m[8], m[9],
		    (uint32_t)(m[10] | m[11] << 8));
		if (m[7] == 0x0d && cp.data_step != 0 && cp.data_result < 0 && m[3] == cp.data_seq) {
			cp.data_result = m[10] | m[11] << 8;
		}
	} else if (group == 0x0d && index == 0x10 && n >= 7 + 2) {
		/* the data call's status: context, state, then a cause: state words, no identity */
		cp.data_notes++;
		say("cp:   DATA CALL STATUS: context %u, state %u, then %02x %02x %02x (of %u)\n", m[7], m[8], n > 9 ? m[9] : 0,
		    n > 10 ? m[10] : 0, n > 11 ? m[11] : 0, n - 7);
	} else if (group == 0x0d && index == 0x09 && n >= 7 + 4) {
		/* the context's address configuration: its length, context, flag byte and one octet; the addresses are not
		 * logged */
		cp.data_configs++;
		uint32_t set = 0;

		for (uint32_t k = 0; k < 5 && n >= 7 + 3 + 4 * (k + 1); k++) {
			set |= (m[10 + 4 * k] | m[11 + 4 * k] | m[12 + 4 * k] | m[13 + 4 * k]) != 0 ? 1u << k : 0;
		}
		say("cp:   DATA ADDRESS CONFIGURATION: %u bytes, context %u, flags %#x, the first address' first octet %u (the "
		    "rest is not logged); of the five 4-byte fields after the flags these are set: %#x\n", n - 7, m[7], m[8],
		    m[10], set);
		if (m[7] == DATA_CID && cp.data_step != 0 && n >= 7 + 3 + 8) {
			/* the address and the first DNS server, for stage 7: used only if both look like host addresses */
			memcpy(cp.ip4, m + 10, 4);
			memcpy(cp.dns4, m + 14, 4);
			if (n >= 7 + 3 + 12) {
				memcpy(cp.dns4b, m + 18, 4);
			}
			cp.gen++;
			cp.have4 = cp.ip4[0] != 0 && cp.ip4[0] != 127 && cp.ip4[0] < 224 && cp.dns4[0] != 0 &&
			    cp.dns4[0] != 127 && cp.dns4[0] < 224;
			if (!cp.ping_given) {
				memcpy(cp.ping4, cp.dns4, 4);
			}
		}
	} else if (group == 0x0d) {
		cp.data_notes++;
	} else if (group == 0x01 && index == 0x07 && n >= 7 + 1) {
		/* the phone state: 1 low power (the radio off), 2 normal */
		cp.phone_state = m[7];
		say("cp:   PHONE STATE %u (%s)\n", m[7], m[7] == 1 ? "low power mode: the radio is off" : m[7] == 2 ?
		    "NORMAL: the radio may be on" : "not a value this driver knows");
		if (m[7] != 1 && (!cp.normal_asked || cp.back_asked)) {
			say("cp:   the CP is not in low power mode: it is stopped now\n");
			cp.stop = true;
		}
	} else if (group == 0x01 && n <= 7 + 8) {
		/* power group: short state words, no identity */
		say("cp:   power group data: %02x %02x %02x %02x (of %u)\n", n > 7 ? m[7] : 0, n > 8 ? m[8] : 0, n > 9 ? m[9] : 0,
		    n > 10 ? m[10] : 0, n - 7);
	} else if (group == 0x05 && (index == 0x01 || index == 0x07) && n <= 7 + 2) {
		/* the SIM's status (number 1: 0x00 initialising, 0x80 no card, 0x82 ready, 0x83 phone book ready, 1-8 a lock)
		 * and the card's type (number 7): no identity in either */
		say("cp:   SIM %s: %02x %02x (of %u)\n", index == 1 ? "status" : "card type", n > 7 ? m[7] : 0, n > 8 ? m[8] : 0,
		    n - 7);
		if (index == 1 && n > 7) {
			cp.sim_status = m[7];
			cp.sim_ms = now_ms();
		}
	} else if (group == 0x05 && index == 0x05 && type == 2 && cp.sim_asked && n >= 7 + 3) {
		/* the answer to our one SIM file read (EF_AD): the card's two status words, the length, and of the data only
		 * how many digits the network code has (its fourth byte's low half) */
		uint32_t got = m[9];

		say("cp: THE SIM FILE READ WAS ANSWERED (EF_AD): status words %02x %02x, %u bytes of data; the network code's "
		    "digits: %u\n", m[7], m[8], got, got >= 4 && n >= 7 + 3 + 4 ? m[13] & 0xfu : 0);
	} else if (group == 0x08 && index == 0x05 && n >= 7 + 3) {
		/* the registration state: technology, domain, state (1 none, 2 home, 3 searching, 4 emergency only, 5 unknown,
		 * 6 roaming). The cell's numbers after them are not logged. */
		say("cp:   REGISTRATION: technology %u, domain %u, state %u (%s)\n", m[7], m[8], m[9], m[9] == 1 ? "none" :
		    m[9] == 2 ? "HOME" : m[9] == 3 ? "SEARCHING" : m[9] == 4 ? "EMERGENCY ONLY" : m[9] == 6 ? "ROAMING" : "?");
		if (m[9] != (uint8_t)cp.reg_state) {
			cp.reg_changes++;
		}
		if ((m[9] == 2 || m[9] == 6) != (cp.reg_state == 2 || cp.reg_state == 6)) {
			cp.lost_ms = (m[9] == 2 || m[9] == 6) ? 0 : now_ms();
		}
		cp.reg_state = m[9];
		cp.tech = m[7];
		cp.gen++;
		if ((m[9] == 2 || m[9] == 6) && cp.registered_ms == 0 && cp.normal_asked) {
			cp.registered_ms = now_ms();
			say("cp:   REGISTERED %llu ms after normal mode was asked for\n", cp.registered_ms - cp.normal_ms);
		}
		if (m[9] != 1 && !cp.normal_asked) {
			say("cp:   a registration state other than \"none\" without normal mode asked for: the CP is stopped now\n");
			cp.stop = true;
		}
	} else if (group == 0x08 && index == 0x03 && n >= 7 + 4) {
		/* the serving network: its operator code is the run of 5 or 6 digits in it (the country's and the network's
		 * numbers, the same for every subscriber of that network); nothing else of the message is logged */
		char code[8];
		uint32_t k = 0;

		for (uint32_t i = 7; i < n && k == 0; i++) {
			uint32_t d = 0;

			while (i + d < n && d < 6 && m[i + d] >= '0' && m[i + d] <= '9') {
				d++;
			}
			if (d >= 5) {
				memcpy(code, m + i, d);
				k = d;
			}
		}
		code[k] = 0;
		if (k != 0) {
			strlcpy(cp.oper, code, sizeof(cp.oper));
			cp.gen++;
		}
		say("cp:   SERVING NETWORK: operator code %s (of %u bytes)\n", k != 0 ? code : "not found in it", n - 7);
	} else if (group == 0x07 && (index == 0x01 || index == 0x06) && n >= 7 + 4) {
		/* signal: the display group's icon (number 1) and strength (number 6) words: levels, no identity */
		cp.signals++;
		if (index == 1 && (m[7] & 1) != 0) {
			cp.bars = m[8];         /* the icon's flags (bit 0: the signal's part is valid), then its bars */
		} else if (index == 6) {
			cp.dbm = -(int)m[7];    /* the strength report: minus dBm */
		}
		cp.gen++;
		if (cp.signals <= 6 || cp.signals % 10 == 0) {
			say("cp:   SIGNAL (%s, the %uth): %02x %02x %02x %02x %02x %02x\n", index == 1 ? "icons" : "strength",
			    cp.signals, m[7], m[8], m[9], m[10], n > 11 ? m[11] : 0, n > 12 ? m[12] : 0);
		}
	}
	/* everything else: the header above and the length only (a message may carry an identity) */
}

#define RING_RFS                3u              /* ring_cfgs' "rfs0" */

/* cp_nv=2: an answer to the CP on the file channel: Samsung IPC's RFS header (length, command, id) and the body */
static bool
rfs_answer(uint8_t cmd, uint8_t id, const uint8_t *body, uint32_t n)
{
	uint8_t frame[2048];
	uint32_t len = 6 + n;

	if (4 + len > sizeof(frame) || 4 + len > cp.ul[RING_RFS].size) {
		say("cp: an answer of %u bytes to file request command %#x does not fit one frame: not sent\n", len, cmd);
		return false;
	}
	frame[0] = 0xf8;
	frame[1] = CH_RFS;
	frame[2] = (uint8_t)(4 + len);
	frame[3] = (uint8_t)((4 + len) >> 8);
	frame[4] = (uint8_t)len;
	frame[5] = (uint8_t)(len >> 8);
	frame[6] = (uint8_t)(len >> 16);
	frame[7] = (uint8_t)(len >> 24);
	frame[8] = cmd;
	frame[9] = id;
	if (n != 0) {
		memcpy(frame + 10, body, n);
	}
	cp.rfs_answered++;
	return ring_send(RING_RFS, frame, 4 + len, cp.link_acked, 0);
}

/* the answer to an NV read or write: confirm (1 done, 0 refused), offset, length; a read's data follows */
static void
rfs_nv_answer(uint8_t cmd, uint8_t id, bool ok, uint32_t offset, uint32_t length)
{
	static uint8_t body[2048];
	uint32_t n = 9;

	body[0] = ok ? 1 : 0;
	memcpy(body + 1, &offset, 4);
	memcpy(body + 5, &length, 4);
	if (cmd == 1 && ok) {
		memcpy(body + 9, cp.nv_ram + offset, length);
		n += length;
	}
	(void)rfs_answer(cmd, id, body, n);
	bzero(body, n);
}

/* more bytes of an NV write being received (cp_nv=2): into the RAM copy only */
static void
rfs_write_data(const uint8_t *d, uint32_t n)
{
	uint32_t take = MIN(n, cp.w_len - cp.w_got);

	memcpy(cp.nv_ram + cp.w_off + cp.w_got, d, take);
	cp.w_got += take;
	if (cp.w_got == cp.w_len) {
		cp.w_active = false;
		cp.nv_writes_kept++;
		rfs_nv_answer(2, cp.w_id, true, cp.w_off, cp.w_len);
		say("cp: NV write %u of the CP (offset %#x, %u bytes) kept in RAM only, answered\n", cp.nv_writes_kept, cp.w_off,
		    cp.w_len);
	}
}

/*
 * Bytes that continue a file request, however the link brings them: a frame of their own, the rest of a frame longer
 * than one buffer (rings_receive's cp.more[]), or the next frames of a multi-frame message. They are counted against
 * the request's own length and nothing else (cp.rfs_more: its first four bytes, which count its 6-byte header, as
 * Samsung's RFS header does); an NV write takes its data from them only. A write is accepted only when its data fit
 * its request (rfs_received), so it is complete when the request is; one that is not is refused when the request ends
 * and never takes the bytes of a later request.
 */
static void
rfs_continued(const uint8_t *m, uint32_t n)
{
	uint32_t k = MIN(n, cp.rfs_more);

	if (cp.w_active && k != 0) {
		rfs_write_data(m, k);
	}
	cp.rfs_more -= k;
	if (cp.rfs_more == 0 && cp.w_active) {
		cp.w_active = false;
		rfs_nv_answer(2, cp.w_id, false, cp.w_off, cp.w_len);
		say("cp: NV write of the CP (offset %#x, %u bytes) ended after %u bytes: refused\n", cp.w_off, cp.w_len,
		    cp.w_got);
	}
}

/* a file request of the CP: logged; with cp_nv=2 an NV read or write is answered from the RAM copy, nothing else is */
static void
rfs_received(const uint8_t *m, uint32_t n)
{
	uint32_t len = 0;
	uint8_t cmd, id;

	/* a file request is a stream: its first six bytes say how long it is, and a long one (an NV write with its
	 * data) goes on through the next frames with nothing to mark them. Those frames are data: counted, dropped,
	 * never looked into (seen twice: taken for requests, their first bytes were printed) */
	if (cp.rfs_more != 0) {
		rfs_continued(m, n);
		cp.dropped[3]++;
		return;
	}
	cp.rfs_requests++;
	if (n < 6) {
		say("cp: from the CP on channel 245: %u bytes, too short for a file request\n", n);
		return;
	}
	memcpy(&len, m, 4);
	cmd = m[4];
	id = m[5];
	if (len > n) {
		cp.rfs_more = len - n;
	}
	if ((cmd == 1 || cmd == 2) && n >= 14) {
		uint32_t offset, length;

		memcpy(&offset, m + 6, 4);
		memcpy(&length, m + 10, 4);
		if (cp.nv_ram != NULL) {
			bool fits = offset <= cp.nv_ram_size && length <= cp.nv_ram_size - offset;

			if (cmd == 1) {
				bool ok = fits && length <= 2048u - 4u - 6u - 9u;     /* one frame */

				rfs_nv_answer(1, id, ok, offset, ok ? length : 0);
				cp.nv_reads_answered += ok;
				say("cp: FILE REQUEST %u from the CP: an NV READ, offset %#x, %u bytes: %s\n", cp.rfs_requests, offset,
				    length, ok ? "answered from the RAM copy" : "refused (out of the NV data or too long for one frame)");
				return;
			}
			if (!fits || len < 14 || length > len - 14) {
				/* (its data must be in the NV data and in the request: 6 bytes of header, offset, length, data) */
				rfs_nv_answer(2, id, false, offset, length);
				say("cp: FILE REQUEST %u from the CP: an NV WRITE out of the NV data or longer than its request (offset "
				    "%#x, %u bytes, request %u): refused\n", cp.rfs_requests, offset, length, len);
				return;
			}
			say("cp: FILE REQUEST %u from the CP: an NV WRITE, offset %#x, %u bytes: into the RAM copy only\n",
			    cp.rfs_requests, offset, length);
			cp.w_active = true;
			cp.w_id = id;
			cp.w_off = offset;
			cp.w_len = length;
			cp.w_got = 0;
			rfs_write_data(m + 14, MIN(n - 14, length));
			return;
		}
		say("cp: FILE REQUEST %u from the CP (not answered): %s of the NV data, offset %#x, %u bytes (request %u, id %u)\n",
		    cp.rfs_requests, cmd == 1 ? "a READ" : "a WRITE (its data dropped, never kept)", offset, length, len, id);
		cp.writes_dropped += cmd == 2;
		return;
	}
	if (len > n) {
		/* longer than this buffer and not an NV read or write: its header only, no look for a name in data */
		say("cp: FILE REQUEST %u from the CP (not answered): command %#x, id %u, %u bytes in all (data dropped)\n",
		    cp.rfs_requests, cmd, id, len);
		return;
	}
	if (cmd == 0x11 && cp.rfs_open != 0 && cp.nv_ram != NULL) {
		/* cp_rfs=1: an OPEN FILE answered from RAM: the file is not there. The confirmation is the RFS header and two
		 * 32-bit words, the handle (-1) and errno (2, ENOENT): 14 bytes, as Samsung's file server sends it. Nothing is
		 * read or opened anywhere */
		static const int32_t none[2] = { -1, 2 };

		cp.rfs_opens++;
		(void)rfs_answer(0x11, id, (const uint8_t *)none, sizeof(none));
		say("cp: FILE REQUEST %u from the CP: OPEN FILE (id %u, %u bytes): answered \"not there\" (ENOENT), the %uth\n",
		    cp.rfs_requests, id, len, cp.rfs_opens);
	} else {
		say("cp: FILE REQUEST %u from the CP (not answered): command %#x, id %u, %u bytes in all\n", cp.rfs_requests, cmd,
		    id, len);
	}
	/* a path, if the request starts with one after a few words: printable text of 4 characters or more with a '/' or '.' */
	for (uint32_t i = 6; i + 4 < n && i < 64; i++) {
		uint32_t k = i;
		bool path = false;

		while (k < n && k - i < 63 && m[k] >= 0x20 && m[k] < 0x7f) {
			path = path || m[k] == '/' || m[k] == '.';
			k++;
		}
		if (k - i >= 4 && path && k < n && m[k] == 0) {
			print_text("it names", m + i, k - i + 1);
			break;
		}
	}
}

/* ---- the data rings (stage 7) ---- */

#define RING_HIPRIO             7u              /* ring_cfgs' "data, high priority": ordinary buffers both ways */
#define RING_DATA               8u              /* ring_cfgs' "data": up ordinary buffers, down the zero-copy slots */
#define CH_PDP0                 10u             /* context 1's channel */
#define ZMB_OFFSET              CP_IPC_BYTES    /* the "ZMC" region is right behind the IPC memory */
#define ZMB_BYTES               0x00b00000u
#define ZMB_CELL                4096u
#define ZMB_HEADROOM            64u
#define DATA_MAX                2048u
#define PING_ID                 0x5339u

/* where slot k's packet goes, counted from the IPC memory's start */
static inline uint32_t
zc_offset(uint32_t slot)
{
	return ZMB_OFFSET + slot * ZMB_CELL + ZMB_HEADROOM;
}

/* every free slot of the zero-copy ring offered to the CP */
static void
zc_offer(void)
{
	struct ring *r = &cp.dl[RING_DATA];
	uint32_t n = 0;

	if (cp.stage < 7 || r->len < 2 || r->size != 8 || (uint32_t)r->len * ZMB_CELL > ZMB_BYTES || cp.zc_alloc >= r->len ||
	    cp.zc_pre >= r->len) {
		return;
	}
	while ((uint16_t)((cp.zc_alloc + 1u) % r->len) != cp.zc_pre) {
		uint64_t offset = zc_offset(cp.zc_alloc);

		memcpy(cp.ipc + r->buffers + (uint32_t)cp.zc_alloc * 8u, &offset, 8);
		cp.zc_alloc = (uint16_t)((cp.zc_alloc + 1u) % r->len);
		n++;
	}
	if (n != 0) {
		__builtin_arm_dsb(0xf);
		ipc_wr16(r->rp, cp.zc_alloc);
		__builtin_arm_dsb(0xf);
	}
}

/* the Internet checksum over n bytes, begun with sum */
static uint16_t
ip_sum(const uint8_t *p, uint32_t n, uint32_t sum)
{
	for (uint32_t i = 0; i + 1 < n; i += 2) {
		sum += (uint32_t)p[i] << 8 | p[i + 1];
	}
	if (n & 1) {
		sum += (uint32_t)p[n - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

/* a packet from the CP (a copy, n >= 1 bytes of it): checked, counted, never passed on; the answers to our two */
/* ---- cp_if=1: the data context as the network interface pdp_ip0 ---- */

#define PDP_QUEUE               64u
#define PDP_MTU                 1400u

static LCK_GRP_DECLARE(pdp_lck_grp, "hakios.pdp");
static LCK_MTX_DECLARE(pdp_lock, &pdp_lck_grp);

/* up: packets the stack gave pdp_ip0, waiting for the modem's thread to put them in the data ring */
static struct {
	ifnet_t ifp;
	uint16_t head, tail;
	uint16_t len[PDP_QUEUE];
	uint8_t pkt[PDP_QUEUE][DATA_MAX];
	uint64_t in, in_bytes, out, out_bytes, dropped_in, dropped_out;
	bool addressed, routed, capped;
} pdp;

/* cp_if_kb: the boot's budget through pdp_ip0, both ways; past it everything is dropped */
static bool
pdp_over_budget(uint32_t n)
{
	if (pdp.in_bytes + pdp.out_bytes + n <= (uint64_t)cp.if_kb * 1024) {
		return false;
	}
	if (!pdp.capped) {
		pdp.capped = true;
		say("cp: pdp_ip0: the boot's budget of %u KB is used up (cp_if_kb): everything is dropped from now on\n", cp.if_kb);
	}
	return true;
}

static errno_t
pdp_output(ifnet_t ifp, mbuf_t m)
{
	size_t n = mbuf_pkthdr_len(m);
	uint16_t next;

	lck_mtx_lock(&pdp_lock);
	next = (uint16_t)((pdp.tail + 1) % PDP_QUEUE);
	if (n < 20 || n > DATA_MAX || next == pdp.head || mbuf_copydata(m, 0, n, pdp.pkt[pdp.tail]) != 0) {
		pdp.dropped_out++;
		lck_mtx_unlock(&pdp_lock);
		ifnet_stat_increment_out(ifp, 0, 0, 1);
		mbuf_freem(m);
		return 0;
	}
	pdp.len[pdp.tail] = (uint16_t)n;
	pdp.tail = next;
	lck_mtx_unlock(&pdp_lock);
	ifnet_stat_increment_out(ifp, 1, (u_int32_t)n, 0);
	mbuf_freem(m);
	return 0;
}

/* the modem's thread: the waiting packets into the data ring of context DATA_CID */
static void
pdp_drain(void)
{
	static uint8_t b[DATA_MAX];

	for (;;) {
		uint16_t n;

		lck_mtx_lock(&pdp_lock);
		if (pdp.head == pdp.tail) {
			lck_mtx_unlock(&pdp_lock);
			return;
		}
		n = pdp.len[pdp.head];
		memcpy(b, pdp.pkt[pdp.head], n);
		pdp.head = (uint16_t)((pdp.head + 1) % PDP_QUEUE);
		lck_mtx_unlock(&pdp_lock);
		if ((b[0] >> 4) != 4 || pdp_over_budget(n) || !ring_send(RING_DATA, b, n, true, (CH_PDP0 + DATA_CID - 1u) << 16)) {
			pdp.dropped_out++;
		} else {
			pdp.out++;
			pdp.out_bytes += n;
			cp.data_up++;
			cp.data_up_bytes += n;
		}
		bzero(b, n);
	}
}

/* down: an IPv4 packet of the context to the stack */
static void
pdp_input(const uint8_t *p, uint32_t n)
{
	struct ifnet_stat_increment_param st;
	unsigned int chunks = 1;
	mbuf_t m = NULL;

	if (pdp.ifp == NULL || n == 0 || n > DATA_MAX || pdp_over_budget(n) ||
	    mbuf_allocpacket(MBUF_DONTWAIT, n, &chunks, &m) != 0) {
		pdp.dropped_in++;
		return;
	}
	if (mbuf_copyback(m, 0, n, p, MBUF_DONTWAIT) != 0) {
		mbuf_freem(m);
		pdp.dropped_in++;
		return;
	}
	mbuf_pkthdr_setrcvif(m, pdp.ifp);
	mbuf_pkthdr_setheader(m, mbuf_data(m));
	bzero(&st, sizeof(st));
	st.packets_in = 1;
	st.bytes_in = n;
	pdp.in++;
	pdp.in_bytes += n;
	ifnet_input(pdp.ifp, m, &st);
}

static errno_t
pdp_demux(__unused ifnet_t ifp, __unused mbuf_t m, __unused char *frame_header, protocol_family_t *protocol)
{
	*protocol = PF_INET;
	return 0;
}

static errno_t
pdp_add_proto(__unused ifnet_t ifp, protocol_family_t protocol, __unused const struct ifnet_demux_desc *d,
    __unused u_int32_t n)
{
	return protocol == PF_INET ? 0 : ENOPROTOOPT;
}

static errno_t
pdp_del_proto(__unused ifnet_t ifp, __unused protocol_family_t protocol)
{
	return 0;
}

static errno_t
pdp_ioctl(ifnet_t ifp, u_long command, __unused void *data)
{
	switch (command) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
	case SIOCSIFDSTADDR:
		ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
		return 0;
	case SIOCSIFFLAGS:
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		return 0;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static errno_t
pdp_proto_input(__unused ifnet_t ifp, protocol_family_t protocol, mbuf_t m, __unused char *header)
{
	if (proto_input(protocol, m) != 0) {
		mbuf_freem(m);
	}
	return 0;
}

static errno_t
pdp_proto_pre_output(__unused ifnet_t ifp, protocol_family_t protocol, __unused mbuf_t *m,
    __unused const struct sockaddr *dest, __unused void *route, char *frame_type, __unused char *link_layer_dest)
{
	*(protocol_family_t *)(void *)frame_type = protocol;
	return 0;
}

/* pdp_ip0 made and attached once, the context's address put on it, a default route if asked and there is none */
static void
pdp_attach(void)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "hakios-pdp0",
		.uniqueid_len = 11,
		.name = "pdp_ip",
		.unit = 0,
		.family = IFNET_FAMILY_CELLULAR,
		.type = IFT_CELLULAR,
		.output = pdp_output,
		.demux = pdp_demux,
		.add_proto = pdp_add_proto,
		.del_proto = pdp_del_proto,
		.ioctl = pdp_ioctl,
	};
	struct ifnet_attach_proto_param proto;
	struct in_aliasreq ifra;
	socket_t so = NULL;
	ifnet_t ifp = NULL;
	errno_t e;

	if (pdp.ifp == NULL) {
		if ((e = ifnet_allocate_extended(&init, &ifp)) != 0) {
			say("cp: pdp_ip0 cannot be made: error %d\n", e);
			cp.if_on = 0;
			return;
		}
		ifnet_set_mtu(ifp, PDP_MTU);
		ifnet_set_flags(ifp, IFF_POINTOPOINT | IFF_MULTICAST | IFF_NOARP, 0xffff);
		ifnet_set_hdrlen(ifp, 0);
		ifnet_set_addrlen(ifp, 0);
		ifnet_set_baudrate(ifp, 50 * 1000 * 1000);
		if ((e = ifnet_attach(ifp, NULL)) != 0) {
			say("cp: pdp_ip0 cannot be attached: error %d\n", e);
			ifnet_release(ifp);
			cp.if_on = 0;
			return;
		}
		bzero(&proto, sizeof(proto));
		proto.input = pdp_proto_input;
		proto.pre_output = pdp_proto_pre_output;
		if ((e = ifnet_attach_protocol(ifp, PF_INET, &proto)) != 0) {
			say("cp: pdp_ip0: IPv4 cannot be attached: error %d\n", e);
		}
		pdp.ifp = ifp;
		say("cp: pdp_ip0 attached (IPv4, point to point, MTU %u): the data context %u's packets go through it\n", PDP_MTU,
		    DATA_CID);
	}
	if (!pdp.addressed && sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so) == 0) {
		struct sockaddr_in *a = (struct sockaddr_in *)&ifra.ifra_addr;
		struct sockaddr_in *d = (struct sockaddr_in *)&ifra.ifra_broadaddr;
		struct sockaddr_in *m = (struct sockaddr_in *)&ifra.ifra_mask;

		bzero(&ifra, sizeof(ifra));
		strlcpy(ifra.ifra_name, "pdp_ip0", sizeof(ifra.ifra_name));
		a->sin_len = d->sin_len = m->sin_len = sizeof(struct sockaddr_in);
		a->sin_family = d->sin_family = m->sin_family = AF_INET;
		memcpy(&a->sin_addr, cp.ip4, 4);
		memcpy(&d->sin_addr, cp.ip4, 4);        /* point to point: the peer as the CP gives it, its own address */
		m->sin_addr.s_addr = 0xffffffffu;
		e = sock_ioctl(so, SIOCAIFADDR, &ifra);
		pdp.addressed = e == 0;
		say("cp: pdp_ip0: the context's IPv4 address (first octet %u; the rest not logged) put on it: %s (%d)\n", cp.ip4[0],
		    e == 0 ? "done" : "FAILED", e);
		sock_close(so);
		so = NULL;
	}
	if (pdp.addressed && cp.route_on && !pdp.routed && sock_socket(PF_ROUTE, SOCK_RAW, AF_INET, NULL, NULL, &so) == 0) {
		struct {
			struct rt_msghdr h;
			struct sockaddr_in dst;
			struct sockaddr_dl gw;
			struct sockaddr_in mask;
		} msg;
		struct iovec iov = { &msg, sizeof(msg) };
		struct msghdr mh;
		size_t sent = 0;

		bzero(&msg, sizeof(msg));
		msg.h.rtm_msglen = sizeof(msg);
		msg.h.rtm_version = RTM_VERSION;
		msg.h.rtm_type = RTM_ADD;
		msg.h.rtm_flags = RTF_UP | RTF_STATIC;
		msg.h.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
		msg.h.rtm_seq = 1;
		msg.dst.sin_len = msg.mask.sin_len = sizeof(struct sockaddr_in);
		msg.dst.sin_family = msg.mask.sin_family = AF_INET;
		msg.gw.sdl_len = sizeof(msg.gw);
		msg.gw.sdl_family = AF_LINK;
		msg.gw.sdl_index = (u_short)ifnet_index(pdp.ifp);
		msg.gw.sdl_type = IFT_CELLULAR;
		bzero(&mh, sizeof(mh));
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		e = sock_send(so, &mh, 0, &sent);
		pdp.routed = e == 0;
		say("cp: pdp_ip0: an IPv4 default route through it: %s (%d)\n", e == 0 ? "added" :
		    e == EEXIST ? "there is one already (Wi-Fi's?): none added" : "FAILED", e);
		sock_close(so);
	}
}

/* kern.hakios.cp.info: the modem's state as one line for user space (scion-celld, the status bar's library):
 * registration, technology, bars, dBm, operator code, SIM status, phone state, data (1: pdp_ip0 up with its address),
 * bytes through pdp_ip0 this boot, the budget, and a counter that moves on every change. No identity in it. */
static int
cp_info_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[200];

	snprintf(line, sizeof(line), "stage=%u state=%u reg=%d tech=%d bars=%d dbm=%d oper=%s sim=%d phone=%d data=%d used=%llu "
	    "budget=%llu gen=%u", cp.stage, cp.state, cp.reg_state, cp.tech, cp.bars, cp.dbm, cp.oper[0] != 0 ? cp.oper : "-",
	    cp.sim_status, cp.phone_state, pdp.addressed && cp.data_step == 3 ? 1 : 0, pdp.in_bytes + pdp.out_bytes,
	    (uint64_t)cp.if_kb * 1024, cp.gen);
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

/* kern.hakios.cp.addr: "ADDRESS DNS1 DNS2" of the active context, root only (empty while there is none): what
 * scion-celld gives configd. Never logged. */
static int
cp_addr_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[64] = "";

	if (req->newptr != USER_ADDR_NULL) {
		return EPERM;
	}
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	if (pdp.addressed && cp.have4 && cp.data_step == 3) {
		snprintf(line, sizeof(line), "%u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u", cp.ip4[0], cp.ip4[1], cp.ip4[2], cp.ip4[3],
		    cp.dns4[0], cp.dns4[1], cp.dns4[2], cp.dns4[3], cp.dns4b[0], cp.dns4b[1], cp.dns4b[2], cp.dns4b[3]);
	}
	int e = SYSCTL_OUT(req, line, strlen(line) + 1);
	bzero(line, sizeof(line));
	return e;
}

SYSCTL_PROC(_kern_hakios_cp, OID_AUTO, info, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, cp_info_sysctl, "A",
    "the modem's state for user space: registration, technology, bars, dBm, operator code, SIM, phone state, data");
SYSCTL_PROC(_kern_hakios_cp, OID_AUTO, addr, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, cp_addr_sysctl, "A",
    "the data context's address and DNS servers (root only; empty without one)");

/*
 * kern.hakios.cp.probe="COMMAND ...": one read-only probe, made now (root only, works without cp=). This is the
 * fast path for the boot chain's questions: no new image for each partition, offset or pattern. The commands are
 * in the comment above probe_find(). Nothing is written, no partition is mapped and no SMC is made.
 */
static int
cp_probe_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char buf[128], *p, *cmd, *a1, *a2, *a3;
	size_t len;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		static const char help[] = "scan|find|dump|str|dist NAME ...";

		return SYSCTL_OUT(req, help, sizeof(help));
	}
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	if (req->newlen == 0 || req->newlen >= sizeof(buf)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, buf, req->newlen);
	if (error) {
		return error;
	}
	len = req->newlen;
	while (len > 0 && (buf[len - 1] == 0 || probe_space(buf[len - 1]))) {
		len--;
	}
	buf[len] = 0;
	p = buf;
	cmd = probe_word(&p);
	a1 = probe_word(&p);
	a2 = probe_word(&p);
	a3 = probe_word(&p);
	if (cmd == NULL) {
		return EINVAL;
	}
	if (strcmp(cmd, "scan") == 0 && a1 != NULL) {
		scan_bootloader(a1, "probe");
		return 0;
	}
	if (strcmp(cmd, "str") == 0 && a1 != NULL) {
		probe_str(a1);
		return 0;
	}
	if (strcmp(cmd, "dist") == 0 && a1 != NULL) {
		probe_dist(a1);
		return 0;
	}
	if (strcmp(cmd, "find") == 0 && a1 != NULL && a2 != NULL) {
		uint8_t pat[32];
		uint32_t plen = 0;

		error = probe_hex(a2, pat, sizeof(pat), &plen);
		if (error != 0) {
			say("cp: probe: find's pattern must be 2 to 64 hex digits (an even number)\n");
			return error;
		}
		probe_find(a1, pat, plen);
		return 0;
	}
	if (strcmp(cmd, "dump") == 0 && a1 != NULL && a2 != NULL && a3 != NULL) {
		char *end;
		unsigned long off = strtoul(a2, &end, 0), n;

		if (end == a2 || *end != 0) {
			return EINVAL;
		}
		n = strtoul(a3, &end, 0);
		if (end == a3 || *end != 0 || n == 0 || n > PROBE_DUMP_MAX) {
			return EINVAL;
		}
		probe_dump(a1, (uint64_t)off, (uint32_t)n);
		return 0;
	}
	say("cp: probe: commands: scan NAME | find NAME HEX | dump NAME OFF LEN | str NAME | dist NAME\n");
	return EINVAL;
}

SYSCTL_PROC(_kern_hakios_cp, OID_AUTO, probe, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    cp_probe_sysctl, "A", "one read-only probe, now (root only): \"scan|find|dump|str|dist NAME ...\"");

static void
data_received(uint8_t ch, const uint8_t *p, uint32_t n)
{
	uint32_t ihl, total, version = p[0] >> 4;

	cp.data_packets++;
	cp.data_bytes += n;
	if (cp.data_packets <= 4) {
		say("cp: from the CP on data channel %u: a packet of %u bytes, IP version %u, protocol %u (addresses not logged)\n",
		    ch, n, version, version == 4 && n >= 20 ? p[9] : version == 6 && n >= 40 ? p[6] : 0);
	}
	if (version == 6 && n >= 40) {
		cp.data_v6++;
		return;
	}
	ihl = (p[0] & 0xfu) * 4u;
	if (version != 4 || n < 20 || ihl < 20 || ihl > n) {
		cp.data_bad++;
		return;
	}
	total = (uint32_t)p[2] << 8 | p[3];
	if (total < ihl || total > n || ip_sum(p, ihl, 0) != 0) {
		cp.data_bad++;
		return;
	}
	if (pdp.ifp != NULL && cp.have4 && memcmp(p + 16, cp.ip4, 4) == 0) {
		pdp_input(p, total);            /* cp_if=1: to the stack (and still looked at below, for stage 7's own) */
	}
	if (!cp.have4 || memcmp(p + 16, cp.ip4, 4) != 0 || (p[6] & 0x3f) != 0 || p[7] != 0) {
		cp.data_other++;                /* not to our address, or a fragment */
		return;
	}
	if (p[9] == 1 && total >= ihl + 8) {
		const uint8_t *c = p + ihl;

		if (ip_sum(c, total - ihl, 0) != 0) {
			cp.data_bad++;
		} else if (c[0] == 0 && c[1] == 0 && memcmp(p + 12, cp.ping4, 4) == 0 && c[4] == (PING_ID >> 8) &&
		    c[5] == (PING_ID & 0xff) && c[6] == 0 && c[7] == 1 && cp.ping_sent_ms != 0 && cp.ping_ms == 0) {
			cp.ping_ms = MAX(now_ms() - cp.ping_sent_ms, 1);
			say("cp: STAGE 7: THE ECHO REQUEST WAS ANSWERED after %llu ms (a reply of %u bytes, its checksums right)\n",
			    cp.ping_ms, total);
		} else {
			cp.data_other++;
		}
	} else if (p[9] == 17 && total >= ihl + 8 + 12) {
		const uint8_t *u = p + ihl;
		uint32_t ulen = (uint32_t)u[4] << 8 | u[5];
		uint32_t pseudo = ((uint32_t)p[12] << 8 | p[13]) + ((uint32_t)p[14] << 8 | p[15]) + ((uint32_t)p[16] << 8 | p[17]) +
		    ((uint32_t)p[18] << 8 | p[19]) + 17u + ulen;

		if (ulen < 8 + 12 || ulen > total - ihl || ((u[6] | u[7]) != 0 && ip_sum(u, ulen, pseudo) != 0)) {
			cp.data_bad++;
		} else if (u[0] == 0 && u[1] == 53 && ((uint32_t)u[2] << 8 | u[3]) == cp.dns_port && memcmp(p + 12, cp.dns4, 4) == 0 &&
		    ((uint32_t)u[8] << 8 | u[9]) == cp.dns_id && (u[10] & 0x80) != 0 && cp.dns_sent_ms != 0 && cp.dns_ms == 0) {
			cp.dns_ms = MAX(now_ms() - cp.dns_sent_ms, 1);
			cp.dns_rcode = u[11] & 0xfu;
			cp.dns_answers = (uint32_t)u[14] << 8 | u[15];
			say("cp: STAGE 7: THE DNS QUESTION WAS ANSWERED after %llu ms: response code %u, %u answer records (%u bytes, "
			    "its checksums right)\n", cp.dns_ms, cp.dns_rcode, cp.dns_answers, total);
		} else {
			cp.data_other++;
		}
	} else {
		cp.data_other++;
	}
}


/* what the CP put into the zero-copy slots */
static void
zc_receive(void)
{
	struct ring *r = &cp.dl[RING_DATA];
	uint16_t fill;
	uint32_t budget = 64;

	if (cp.stage < 7 || r->len < 2 || r->size != 8 || cp.zc_pre >= r->len) {
		return;
	}
	fill = ipc_rd16(r->wp);
	if (fill >= r->len) {
		cp.data_bad++;                  /* a counter of the CP's outside the ring */
		return;
	}
	while (cp.zc_pre != fill && budget-- != 0) {
		static uint8_t packet[DATA_MAX];
		uint32_t word, n;
		uint64_t offset = 0;

		if (cp.zc_pre == cp.zc_alloc) {
			cp.data_bad++;          /* a slot filled that was never offered */
			break;
		}
		word = ipc_rd32(r->size_v + 4u * cp.zc_pre);
		n = word & 0xffff;
		memcpy(&offset, cp.ipc + r->buffers + (uint32_t)cp.zc_pre * 8u, 8);
		if (offset != zc_offset(cp.zc_pre) || n == 0 || n > DATA_MAX) {
			cp.data_bad++;          /* the slot's offset is not the one given, or no packet's length */
		} else {
			memcpy(packet, cp.ipc + zc_offset(cp.zc_pre), n);
			data_received((uint8_t)(word >> 16), packet, n);
			bzero(packet, n);
		}
		cp.zc_pre = (uint16_t)((cp.zc_pre + 1u) % r->len);
	}
	zc_offer();
}

/* one IPv4 packet of ours up through the data ring: the header made here, from the context's address */
static bool
data_send(uint8_t protocol, const uint8_t *to, const uint8_t *payload, uint32_t n)
{
	uint8_t packet[128];
	uint16_t sum;

	if (!cp.have4 || cp.data_step != 3 || n > sizeof(packet) - 20) {
		return false;
	}
	bzero(packet, 20);
	packet[0] = 0x45;
	packet[2] = (uint8_t)((20 + n) >> 8);
	packet[3] = (uint8_t)(20 + n);
	packet[4] = (uint8_t)(cp.dns_id >> 8);
	packet[5] = (uint8_t)(cp.dns_id + protocol);
	packet[6] = 0x40;               /* not to be fragmented */
	packet[8] = 64;
	packet[9] = protocol;
	memcpy(packet + 12, cp.ip4, 4);
	memcpy(packet + 16, to, 4);
	sum = ip_sum(packet, 20, 0);
	packet[10] = (uint8_t)(sum >> 8);
	packet[11] = (uint8_t)sum;
	memcpy(packet + 20, payload, n);
	if (!ring_send(RING_DATA, packet, 20 + n, true, (CH_PDP0 + DATA_CID - 1u) << 16)) {
		return false;
	}
	cp.data_up++;
	cp.data_up_bytes += 20 + n;
	return true;
}

static void
send_ping(void)
{
	uint8_t c[8 + 32];
	uint16_t sum;

	bzero(c, sizeof(c));
	c[0] = 8;                       /* echo request */
	c[4] = (uint8_t)(PING_ID >> 8);
	c[5] = (uint8_t)PING_ID;
	c[7] = 1;
	for (uint32_t i = 0; i < 32; i++) {
		c[8 + i] = (uint8_t)i;
	}
	sum = ip_sum(c, sizeof(c), 0);
	c[2] = (uint8_t)(sum >> 8);
	c[3] = (uint8_t)sum;
	cp.ping_sent_ms = now_ms();
	say("cp: STAGE 7: one echo request of %u bytes sent up (%s): %s\n", 20 + (uint32_t)sizeof(c), cp.ping_given ?
	    "to the builder's cp_ping address" : "to the context's first DNS server", data_send(1, cp.ping4, c, sizeof(c)) ?
	    "in the ring" : "NOT SENT");
}

static void
send_dns(void)
{
	static const uint8_t question[] = { 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
	uint8_t u[8 + 12 + sizeof(question)];
	uint32_t pseudo, ulen = sizeof(u);
	uint16_t sum;

	bzero(u, sizeof(u));
	u[0] = (uint8_t)(cp.dns_port >> 8);
	u[1] = (uint8_t)cp.dns_port;
	u[3] = 53;
	u[4] = (uint8_t)(ulen >> 8);
	u[5] = (uint8_t)ulen;
	u[8] = (uint8_t)(cp.dns_id >> 8);
	u[9] = (uint8_t)cp.dns_id;
	u[10] = 0x01;                   /* recursion wanted */
	u[13] = 1;                      /* one question */
	memcpy(u + 20, question, sizeof(question));
	pseudo = ((uint32_t)cp.ip4[0] << 8 | cp.ip4[1]) + ((uint32_t)cp.ip4[2] << 8 | cp.ip4[3]) + ((uint32_t)cp.dns4[0] << 8 |
	    cp.dns4[1]) + ((uint32_t)cp.dns4[2] << 8 | cp.dns4[3]) + 17u + ulen;
	sum = ip_sum(u, ulen, pseudo);
	if (sum == 0) {
		sum = 0xffff;
	}
	u[6] = (uint8_t)(sum >> 8);
	u[7] = (uint8_t)sum;
	cp.dns_sent_ms = now_ms();
	say("cp: STAGE 7: one DNS question of %u bytes (the A record of example.com) sent up to the context's first DNS "
	    "server: %s\n", 20 + ulen, data_send(17, cp.dns4, u, ulen) ? "in the ring" : "NOT SENT");
}

static void
rings_receive(void)
{
	for (uint32_t i = 0; i < NRINGS; i++) {
		struct ring *r = &cp.dl[i];
		uint16_t wp, rp;

		if (i == RING_DATA) {
			zc_receive();
			continue;
		}
		if (r->size < 64) {
			continue;       /* the data ring of addresses: nothing can arrive, no buffers were given */
		}
		wp = ipc_rd16(r->wp);
		rp = ipc_rd16(r->rp);
		while (rp != wp && wp < r->len && rp < r->len) {
			static uint8_t frame[4096];
			uint32_t word = ipc_rd32(r->size_v + 4u * rp);
			uint32_t n = word & 0xffff;
			uint32_t hdr;

			if (n > r->size) {
				n = r->size;
			}
			if (i == RING_HIPRIO) {
				/* a data ring: the packet itself, no frame header; its channel is in the size word's upper half */
				memcpy(frame, cp.ipc + r->buffers + (uint32_t)rp * r->size, n);
				rp = (uint16_t)((rp + 1) % r->len);
				ipc_wr16(r->rp, rp);
				cp.frames[i]++;
				if (n == 0) {
					cp.data_bad++;
				} else {
					data_received((uint8_t)(word >> 16), frame, n);
				}
				bzero(frame, n);
				continue;
			}
			if (cp.more[i] != 0) {
				/* the rest of a frame longer than one buffer (a file request with data): counted, not looked
				 * into; on the file ring the bytes of the request it continues (rfs_continued) */
				if (i == RING_RFS && cp.rfs_more != 0) {
					uint32_t k = MIN(n, cp.more[i]);

					memcpy(frame, cp.ipc + r->buffers + (uint32_t)rp * r->size, k);
					rfs_continued(frame, k);
					bzero(frame, k);
				}
				cp.more[i] -= MIN(cp.more[i], n);
				rp = (uint16_t)((rp + 1) % r->len);
				ipc_wr16(r->rp, rp);
				continue;
			}
			memcpy(frame, cp.ipc + r->buffers + (uint32_t)rp * r->size, n);
			rp = (uint16_t)((rp + 1) % r->len);
			ipc_wr16(r->rp, rp);
			cp.frames[i]++;
			hdr = n >= 1 && (frame[0] & 2) ? ((frame[0] & 3) == 3 ? 5 : 6) : 4;
			if (n >= 6 && (frame[0] & 0xf8) == 0xf8) {
				uint32_t total = hdr == 6 ? (uint32_t)(frame[2] | frame[3] << 8 | frame[4] << 16) | (uint32_t)frame[5] << 24 :
				    (uint32_t)(frame[2] | frame[3] << 8);

				if (total > n) {
					cp.more[i] = total - n;
				}
			}
			if (n >= 5 && (frame[0] & 0xf8) == 0xf8 && hdr == 5) {
				/* a message cut into several frames ("multi frame": a fifth header byte, bit 7 "more to come"):
				 * the CP's file requests with data come so. Only the first frame is looked into; the others are
				 * data, counted and dropped (seen: taken for requests, their first bytes were printed) */
				bool rest = cp.multi[i];

				cp.multi[i] = (frame[4] & 0x80) != 0;
				if (rest && i == RING_RFS && cp.rfs_more != 0) {
					rfs_continued(frame + hdr, n - hdr);
				}
				if (rest) {
					cp.frames[i]--;
					cp.dropped[i]++;
					bzero(frame, n);
					continue;
				}
			}
			if (n < hdr || (frame[0] & 0xf8) != 0xf8) {
				say("cp: from the CP on ring %u (%s): %u bytes that are no frame\n", i, ring_cfgs[i].name, n);
			} else if (frame[1] == CH_FMT || frame[1] == 236) {
				fmt_received(frame + hdr, n - hdr);
			} else if (frame[1] == CH_RFS) {
				rfs_received(frame + hdr, n - hdr);
			} else {
				say("cp: from the CP on channel %u (%s): a frame of %u bytes (not looked into)\n", frame[1],
				    ring_cfgs[i].name, n);
			}
			bzero(frame, n);
		}
	}
}

/* ---- the CP's messages ---- */

/* false when the CP said it crashed */
static bool
handle_msg(uint16_t v)
{
	if (!(v & MSG_VALID)) {
		say("cp: the CP raised its message interrupt with %#x in its word (not a valid message)\n", v);
		return true;
	}
	if (v & MSG_CMD) {
		switch (v & 0x3f) {
		case CMD_INIT_START:
			say("cp: the CP says INIT_START (it runs its main program)\n");
			write_rings();
			send_msg(MSG_VALID | MSG_CMD | CMD_PIF_INIT_DONE);
			say("cp: told the CP PIF_INIT_DONE\n");
			break;
		case CMD_PHONE_START:
			cp.phone_starts++;
			if (!cp.up) {
				/* the link's words as Samsung's kernel sets them when the CP reports itself started */
				ipc_wr32(IPC_ACCESS, 0);
				ipc_wr32(IPC_MAGIC, 0);
				ipc_wr32(IPC_RAW_TX_HEAD, 0);
				ipc_wr32(IPC_RAW_TX_TAIL, 0);
				ipc_wr32(IPC_RAW_RX_HEAD, 0);
				ipc_wr32(IPC_RAW_RX_TAIL, 0);
				ipc_wr32(8, 0);
				ipc_wr32(12, 0);
				ipc_wr32(16, 0);
				ipc_wr32(20, 0);
				ipc_wr32(IPC_MAGIC, MAGIC_IPC);
				ipc_wr32(IPC_ACCESS, 1);
				cp.up = true;
				set_state(3);
				say("cp: the CP says PHONE_START: THE CP IS UP (its state word %#x: state %u, active %u)\n",
				    rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE)), CP_STATE_STATUS(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))),
				    CP_STATE_ACTIVE(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))));
			} else if (cp.phone_starts < 5 || cp.phone_starts % 20 == 0) {
				say("cp: the CP says PHONE_START again (%u times now)\n", cp.phone_starts);
			}
			cp.phone_start_ms = now_ms();
			if (cp.link && !cp.link_acked) {
				/* it has not taken INIT_END yet (it looks at the message word about every 100 ms): told again, as
				 * Samsung's kernel does on every repeated PHONE_START */
				send_msg(MSG_VALID | MSG_CMD | CMD_INIT_END);
			}
			if (!cp.link && cp.stage >= 4 && !cp.nv_loaded) {
				if (cp.phone_starts == 1) {
					say("cp: stage 4 REFUSED: the CP has no NV data of its own (zeros): INIT_END is not sent, its protocol "
					    "stack is not started, the run stays at stage 3 (a CP with blank NV goes to its normal state by "
					    "itself with a default identity and no calibration: never near a network)\n");
				}
			} else if (!cp.link && cp.stage >= 4) {
				static const uint8_t lpm[2] = { 0x01, 0x00 };

				cp.link = true;
				set_state(4);
				/* first, so that it is the first thing the CP finds when its stack starts: the radio off. It waits in
				 * the ring; then INIT_END */
				(void)fmt_send(0x01, 0x07, 0x01, lpm, sizeof(lpm));
				send_msg(MSG_VALID | MSG_CMD | CMD_INIT_END);
				cp.init_end_ms = cp.lpm_sent_ms = now_ms();
				say("cp: \"LOW POWER MODE\" (the radio off) put in the ring for the CP, the first thing it will find; "
				    "then told the CP INIT_END\n");
			}
			break;
		case CMD_CRASH_RESET:
		case CMD_CRASH_EXIT:
			say("cp: THE CP SAYS IT CRASHED (%s)\n", (v & 0x3f) == CMD_CRASH_RESET ? "CRASH_RESET" : "CRASH_EXIT");
			/* its reason, if it left text at the place Samsung's kernel reads one (the IPC memory's 2 KB at 2048 -
			 * the old layout - or 63488): printable text only */
			print_text("text at the IPC memory's 0x800:", cp.ipc + 0x800 + 4, 64);
			print_text("text at the IPC memory's 0xF800:", cp.ipc + 0xf800 + 4, 64);
			return false;
		default:
			say("cp: the CP sent command %#x (not one this driver knows)\n", v & 0x3f);
			break;
		}
	}
	if (cp.up && (v & MSG_DATA || !(v & MSG_CMD))) {
		rings_receive();
	}
	return true;
}

/* the mailbox looked at once: false when the CP crashed */
static bool
poll_once(void)
{
	static uint32_t last_state = 0xffffffffu;
	uint32_t pending = rd(cp.mbox, MBOX_INTSR0) & 0xffff0000u;
	uint32_t state = rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE));
	bool alive = true;

	if (pending != 0) {
		uint16_t msg = (uint16_t)rd(cp.mbox, MBOX_ISSR(WORD_CP_MSG));

		cp.busy_ms = now_ms();

		wr(cp.mbox, MBOX_INTCR0, pending);
		if (pending & (1u << (16 + IRQ_CP_MSG))) {
			alive = handle_msg(msg);
		}
		if (pending & ~(1u << (16 + IRQ_CP_MSG))) {
			say("cp: the CP raised interrupts %#x (2 state, 3 active, 5-7 clock requests, 8 wake lock, 9 radio mode); "
			    "its state word %#x, words 5-7 %#x %#x %#x\n", pending >> 16, state, rd(cp.mbox, MBOX_ISSR(5)),
			    rd(cp.mbox, MBOX_ISSR(6)), rd(cp.mbox, MBOX_ISSR(7)));
		}
	}
	if (state != last_state) {
		say("cp: the CP's state word is now %#x (state %u, active %u)\n", state, CP_STATE_STATUS(state),
		    CP_STATE_ACTIVE(state));
		last_state = state;
	}
	if (cp.up) {
		rings_receive();
	}
	return alive;
}

static bool
start(void)
{
	uint64_t until;
	uint32_t state = 0;

	/* the mailbox as Samsung's kernel has it before the CP runs: every word 0, then the AP's state */
	wr(cp.mbox, MBOX_INTMR0, 0xffff0000u);          /* its interrupts to the AP: looked at here, never delivered */
	wr(cp.mbox, MBOX_INTCR0, 0xffff0000u);
	wr(cp.mbox, MBOX_INTCR1, 0xffffu);
	for (uint32_t i = 0; i < MBOX_WORDS; i++) {
		wr(cp.mbox, MBOX_ISSR(i), 0);
	}
	wr(cp.mbox, MBOX_ISSR(WORD_AP_STATE), AP_STATE_REV(cp.rev) | AP_STATE_DS(cp.ds) | AP_STATE_PDA_ACTIVE);
	say("cp: the mailbox cleared; the AP's state word %#x (revision %u, dual SIM field %u, PDA active)\n",
	    rd(cp.mbox, MBOX_ISSR(WORD_AP_STATE)), cp.rev, cp.ds);

	say("cp: the IPC memory before the start: magic %#x access %#x\n", ipc_rd32(IPC_MAGIC), ipc_rd32(IPC_ACCESS));

	/*
	 * AP2CP_CFG is written, never read. Samsung's ss310ap_on reads it first, but a read after a refused check is a
	 * data abort at 0x14820000 (seen on 18 Nov 2026 when the refusal was ignored), and the write is all that is
	 * needed here ("CP_CPU will work right now!!!").
	 */
	cp.started = true;
	wr(cp.cfg, 0, 1);
	say("cp: AP2CP_CFG 0x14820000 written (bit 0): the CP's processor may run\n");

	until = now_ms() + 3000;
	do {
		state = rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE));
		if (CP_STATE_STATUS(state) != 0) {
			break;
		}
		IOSleep(5);
	} while (now_ms() < until);
	if (CP_STATE_STATUS(state) == 0) {
		say("cp: the CP did not report a state in 3 s (its word %#x)\n", state);
		look_pmu("with the CP silent");
		look_mbox("with the CP silent");
		return false;
	}
	say("cp: THE CP'S BOOT LOADER RUNS: its state word %#x (state %u) %llu ms after the start\n", state,
	    CP_STATE_STATUS(state), now_ms() - (until - 3000));

	/* Android's order: only now, with the boot loader running, the old queues' pointers are zeroed and the magic
	 * "BOOT" written (the boot loader clears the memory's first words itself when it starts: a magic written before
	 * the start was gone, and the first question was never taken) */
	say("cp: the IPC memory with the boot loader running: magic %#x access %#x, queue words %#x %#x %#x %#x\n",
	    ipc_rd32(IPC_MAGIC), ipc_rd32(IPC_ACCESS), ipc_rd32(IPC_RAW_TX_HEAD), ipc_rd32(IPC_RAW_TX_TAIL),
	    ipc_rd32(IPC_RAW_RX_HEAD), ipc_rd32(IPC_RAW_RX_TAIL));
	for (uint32_t off = 8; off <= IPC_RAW_RX_TAIL; off += 4) {
		ipc_wr32(off, 0);
	}
	ipc_wr32(IPC_MAGIC, MAGIC_BOOT);
	__builtin_arm_dsb(0xf);
	if (ipc_rd32(IPC_MAGIC) != MAGIC_BOOT) {
		say("cp: the IPC memory's magic reads back %#x, not \"BOOT\"\n", ipc_rd32(IPC_MAGIC));
		return false;
	}
	/* and the AP's "boot confirm": its own state field 1 in its state word. The boot loader takes no question
	 * before it (seen: without it the first question lay in the queue untouched for 5 s) */
	wr(cp.mbox, MBOX_ISSR(WORD_AP_STATE), rd(cp.mbox, MBOX_ISSR(WORD_AP_STATE)) | AP_STATE_STATUS(1));
	say("cp: the magic \"BOOT\" written, the AP's state word now %#x (its state field 1: boot confirmed)\n",
	    rd(cp.mbox, MBOX_ISSR(WORD_AP_STATE)));
	if (!boot_ask(0x0000900d, 0x0000a00d) || !boot_ask(0x00009f00, 0x0000af00)) {
		return false;
	}
	say("cp: the boot loader's handshake is complete; waiting for the CP's main program\n");

	until = now_ms() + 15000;
	while (!cp.up && now_ms() < until) {
		if (!poll_once()) {
			return false;
		}
		IOSleep(1);
	}
	if (!cp.up) {
		say("cp: the CP's main program did not report itself in 15 s\n");
		look_pmu("with no PHONE_START");
		look_mbox("with no PHONE_START");
		return false;
	}
	return true;
}

/* ---- stage 6: the data context ---- */


/* one step of the data stage, called while the phone is registered: NULL while it goes on, else why it is over */
static const char *
data_stage(void)
{
	uint64_t now = now_ms();
	uint8_t d[127];

	bzero(d, sizeof(d));
	switch (cp.data_step) {
	case 0:
		if (now - cp.registered_ms < 5000) {
			return NULL;
		}
		/* the context defined: enable, the context's number, type 2, the access point's name in 124 bytes */
		d[0] = 1;
		d[1] = DATA_CID;
		d[2] = 2;
		strlcpy((char *)d + 3, cp.apn, 124);
		say("cp: STAGE 6: defining data context %u with the builder's access point name (%u characters)\n", DATA_CID,
		    (uint32_t)strlen(cp.apn));
		cp.data_result = -1;
		if (!fmt_send(0x0d, 0x01, 0x03, d, 127)) {
			return "the data context's definition could not be sent";
		}
		cp.data_seq = cp.mseq;
		cp.data_step = 1;
		cp.data_ms = now;
		return NULL;
	case 1:
		if (cp.data_result < 0) {
			return now - cp.data_ms > 10000 ? "no answer to the data context's definition in 10 s" : NULL;
		}
		if (cp.data_result != 0x8000) {
			say("cp: STAGE 6: THE CP REFUSED THE DEFINITION (result %#x): nothing else is tried\n", cp.data_result);
			return "the CP refused the data context's definition";
		}
		/* accepted: the context activated: enable, the context's number, no user name, no password */
		d[0] = 1;
		d[1] = DATA_CID;
		say("cp: STAGE 6: the definition is accepted; asking for the context's activation\n");
		cp.data_result = -1;
		if (!fmt_send(0x0d, 0x04, 0x03, d, 101)) {
			return "the activation could not be sent";
		}
		cp.data_seq = cp.mseq;
		cp.data_step = 2;
		cp.data_ms = now;
		return NULL;
	case 2:
		if (cp.data_result < 0) {
			return now - cp.data_ms > 30000 ? "no answer to the activation in 30 s" : NULL;
		}
		if (cp.data_result != 0x8000) {
			say("cp: STAGE 6: THE CP REFUSED THE ACTIVATION (result %#x): nothing else is tried\n", cp.data_result);
			return "the CP refused the activation";
		}
		say("cp: STAGE 6: THE ACTIVATION IS ACCEPTED %llu ms after it was asked; watching it\n", now - cp.data_ms);
		cp.data_step = 3;
		cp.data_ms = now;
		return NULL;
	case 3:
		if (cp.if_on && cp.have4) {
			/* cp_if=1: the context stays; its packets go through pdp_ip0 until asked to stop */
			static uint64_t next_try, next_say;

			if ((pdp.ifp == NULL || !pdp.addressed || (cp.route_on && !pdp.routed && now - cp.data_ms < 60000)) &&
			    now - cp.data_ms >= 1000 && now >= next_try) {
				next_try = now + 5000;
				pdp_attach();
			}
			pdp_drain();
			if (next_say == 0) {
				next_say = now + 60000;
			} else if (now >= next_say) {
				next_say = now + 60000;
				say("cp: pdp_ip0: up %llu packets (%llu bytes), down %llu (%llu bytes), dropped %llu up %llu down\n", pdp.out,
				    pdp.out_bytes, pdp.in, pdp.in_bytes, pdp.dropped_out, pdp.dropped_in);
			}
			return NULL;
		}
		if (cp.stage >= 7 && cp.have4) {
			/* stage 7: our two packets, 2 and 3 s after the activation; done 5 s after it at the earliest */
			if (cp.ping_sent_ms == 0 && now - cp.data_ms >= 2000) {
				send_ping();
			}
			if (cp.dns_sent_ms == 0 && now - cp.data_ms >= 3000) {
				send_dns();
			}
			if (now - cp.data_ms < 5000 || ((cp.ping_ms == 0 || cp.dns_ms == 0) && now - cp.data_ms < 12000)) {
				return NULL;
			}
		} else if (now - cp.data_ms < (cp.stage >= 7 ? 6000 : 20000)) {
			return NULL;
		}
		if (cp.stage >= 7 && !cp.have4) {
			say("cp: STAGE 7: no IPv4 address and DNS server were read from the CP's address configuration: no packet "
			    "is sent\n");
		}
		d[1] = DATA_CID;
		say("cp: STAGE 6: %llu s with the context (%u status notes, %u address configurations, %u packets of %u bytes "
		    "from the CP): asking for its deactivation\n", (now - cp.data_ms) / 1000, cp.data_notes, cp.data_configs,
		    cp.data_packets, cp.data_bytes);
		cp.data_result = -1;
		if (!fmt_send(0x0d, 0x04, 0x03, d, 101)) {
			return "the deactivation could not be sent";
		}
		cp.data_seq = cp.mseq;
		cp.data_step = 4;
		cp.data_ms = now;
		return NULL;
	case 4:
		if (cp.data_result < 0 && now - cp.data_ms < 5000) {
			return NULL;
		}
		if (cp.data_result < 0) {
			say("cp: STAGE 6: no answer to the deactivation in 5 s (low power mode ends the context anyway)\n");
		} else {
			say("cp: STAGE 6: the deactivation's result %#x\n", cp.data_result);
		}
		cp.data_step = 5;
		return "the data stage is over";
	default:
		return "the data stage is over";
	}
}

static void
print_state(void)
{
	say("cp: state %u; frames from the CP by ring:", cp.state);
	for (uint32_t i = 0; i < NRINGS; i++) {
		say(" %s %u", ring_cfgs[i].name, cp.frames[i]);
	}
	say("; control messages %u, file requests %u (%u answered: %u NV writes kept in RAM, %u NV reads; %u frames of other "
	    "data dropped unread), PHONE_START %u times, last phone state %d\n", cp.fmt_messages, cp.rfs_requests,
	    cp.rfs_answered, cp.nv_writes_kept, cp.nv_reads_answered, cp.dropped[3], cp.phone_starts, cp.phone_state);
	if (cp.mbox != 0) {
		look_mbox("now");
		look_pmu("now");
	}
}

static void
cp_thread(__unused void *arg, __unused wait_result_t result)
{
	uint64_t until, asked = 0, kick = 0;
	const char *why = NULL, *why5 = NULL;
	bool alive = true;

	thread_set_thread_name(current_thread(), "hakios.shannon");
	/* not at once: the boot's own flood of lines would push these out of the log before the USB console has it */
	IOSleep(cp.delay_s * 1000);
	say("cp: stage %u asked for (1 a look, 2 load and check, 3 start, 4 the link, 5 the network)%s\n", cp.stage,
	    cp.nv_file == 2 ? ", with EFS's nv_data.bin (RAM copy)" : cp.nv_file ? ", with the builder's nv_data.bin" :
	    ", no non-volatile data (zeros)");

	if (!memory_is_free()) {
		set_state(101);
		return;
	}
	cp.pmu = ml_io_map(CP_PMU_PA, CP_PMU_BYTES);
	cp.mbox = ml_io_map(CP_MBOX_PA, 0x1000);
	cp.cfg = ml_io_map(CP_CFG_PA, 0x1000);
	cp.gicd = ml_io_map(CP_GICD_PA + 0x1000, 0x1000);       /* the page of the "set pending" words */
	cp.mem = hakios_physmap_wc(CP_MEM_PA, CP_MEM_BYTES);
	if (cp.pmu == 0 || cp.mbox == 0 || cp.cfg == 0 || cp.mem == 0) {
		say("cp: a mapping failed (PMU %d, mailbox %d, AP2CP_CFG %d, the CP's memory %d)\n", cp.pmu != 0, cp.mbox != 0,
		    cp.cfg != 0, cp.mem != 0);
		set_state(101);
		return;
	}
	cp.ipc = (uint8_t *)cp.mem + CP_IPC_OFFSET;

	look_pmu("as found");
	look_mbox("as found");
	look_mem();
	if (!read_toc()) {
		set_state(101);
		return;
	}
	if (cp.bl_scan) {
		scan_bootloaders();
	}
	set_state(1);
	if (cp.stage < 2) {
		say("cp: stage 1 done: looked, nothing changed\n");
		return;
	}

	if (!load_and_check()) {
		set_state(102);
		look_pmu("after the failed load");      /* (before the domain is put back down) */
		cp_stop("the load failed");
		return;
	}
	set_state(2);
	look_pmu("after the check");
	if (cp.stage < 3) {
		say("cp: stage 2 done: the image is loaded and accepted, the CP is not started\n");
		return;
	}

	if (!start()) {
		set_state(103);
		cp_stop("the start did not complete");
		return;
	}

	/* watching: everything the CP says, until the time is up or we are told to stop */
	until = now_ms() + (uint64_t)cp.wait_s * 1000;
	while (alive) {
		int go = cp_go;

		alive = poll_once();
		if (cp.stop && (!cp.normal_asked || cp.back_asked)) {
			why = "it was not in low power mode, or reported a registration state";
			break;
		}
		if (cp.link && !cp.link_acked && now_ms() - cp.init_end_ms > 150 && now_ms() - cp.phone_start_ms > 350) {
			cp.link_acked = true;
			cp.lpm_sent_ms = now_ms();
			say("cp: the CP has stopped repeating PHONE_START %llu ms after INIT_END: it took it, the link is up\n",
			    now_ms() - cp.init_end_ms);
		}
		if (cp.link && !cp.link_acked && now_ms() - cp.init_end_ms > 4000) {
			say("cp: the CP still repeats PHONE_START 4 s after INIT_END: it is stopped now\n");
			why = "it did not take INIT_END";
			break;
		}
		if (cp.link_acked && now_ms() >= kick && ipc_rd16(cp.ul[1].rp) != ipc_rd16(cp.ul[1].wp)) {
			/* what we sent is still in the ring: the CP is told again that there is data (it may not have been
			 * listening yet when it was first told) */
			send_msg(MSG_VALID | MSG_DATA);
			kick = now_ms() + 200;
		}
		if (cp.link_acked && cp.phone_state != 1 && !cp.normal_asked && now_ms() - cp.lpm_sent_ms > 5000) {
			say("cp: no \"low power mode\" from the CP 5 s after it was asked (its last phone state: %d): it is stopped now\n",
			    cp.phone_state);
			why = "it did not confirm low power mode";
			break;
		}
		if (cp.stage >= 5 && cp.link_acked && cp.phone_state == 1 && !cp.normal_asked && cp.version_answered) {
			/* stage 5, step 1: the SIM's status, read in low power mode */
			uint64_t waited = now_ms() - cp.lpm_sent_ms;

			if ((cp.sim_status == 0x82 || cp.sim_status == 0x83) && now_ms() - cp.sim_ms > 2000) {
				say("cp: STAGE 5: the SIM is ready (status %#x) and the CP is in low power mode: asking for NORMAL mode "
				    "(the radio on; the phone will register on its network)\n", cp.sim_status);
				cp.normal_asked = true;
				cp.normal_ms = now_ms();
				until = cp.normal_ms + 100000;          /* 100 s to register, at most */
				ask_phone_state(2);
			} else if (cp.sim_status >= 1 && cp.sim_status <= 8 && now_ms() - cp.sim_ms > 3000) {
				say("cp: STAGE 5: the SIM is locked (status %#x: it wants a PIN or PUK). NO attempt is made. Staying in low "
				    "power mode and stopping\n", cp.sim_status);
				why5 = "the SIM is locked";
				break;
			} else if (waited > 20000) {
				say("cp: STAGE 5: no SIM ready 20 s after the link came up (its last status %#x; 0x80 no card, 0x81 card "
				    "error, 0 initialising): normal mode is NOT asked for\n", cp.sim_status);
				why5 = "no SIM ready";
				break;
			}
		}
		if (cp.normal_asked && !cp.back_asked) {
			/* stage 5, steps 2 and 3: registered for two minutes, or not in time, then low power again */
			const char *back = NULL;

			if (cp.registered_ms != 0 && now_ms() - cp.registered_ms > 120000 && !cp.if_on) {
				back = "registered for two minutes";
			} else if (cp.registered_ms == 0 && now_ms() - cp.normal_ms > 90000) {
				back = "NOT registered 90 s after normal mode was asked for";
				why5 = "not registered in time";
			} else if (cp.registered_ms != 0 && cp.reg_state != 2 && cp.reg_state != 6 &&
			    now_ms() - cp.registered_ms > 5000 && (!cp.if_on || now_ms() - cp.lost_ms > 600000)) {
				/* (cp_if=1, the everyday mode: the CP looks for its network again by itself; given 10 minutes) */
				back = "the registration was lost";
				why5 = "registration lost";
			} else if (cp.stop || go == 2) {
				back = "asked to stop";
			}
			if (back == NULL && cp.stage >= 6 && cp.apn[0] != 0 && cp.registered_ms != 0) {
				back = data_stage();
			}
			if (back != NULL) {
				say("cp: STAGE 5: %s (last registration state %d, %u changes, %u signal reports): asking for LOW POWER MODE\n",
				    back, cp.reg_state, cp.reg_changes, cp.signals);
				cp.back_asked = true;
				cp.back_ms = now_ms();
				cp.phone_state = -1;
				cp.stop = false;
				until = cp.back_ms + 12000;
				ask_phone_state(1);
			} else {
				until = now_ms() + 30000;       /* the stage's own clocks decide */
			}
		}
		if (cp.back_asked && cp.phone_state == 1 && now_ms() - cp.back_ms > 1500) {
			say("cp: STAGE 5: the CP is in low power mode again %llu ms after it was asked\n", now_ms() - cp.back_ms);
			break;
		}
		if (go != 0) {
			cp_go = 0;
			if (go == 1) {
				print_state();
			} else if (go == 2 && !cp.normal_asked) {
				break;                          /* (in normal mode: low power is asked for first, above) */
			} else if (go == 3 && cp.link) {
				ask_version();
			}
		}
		if (cp.link_acked && cp.phone_state == 1 && asked == 0) {
			asked = now_ms() + 2000;        /* only once it has said it is in low power mode */
		}
		if (asked > 1 && now_ms() >= asked) {
			asked = 1;
			ask_version();
		}
		if (cp.sim_probe != 0 && !cp.sim_asked && cp.version_answered && cp.phone_state == 1 && !cp.normal_asked &&
		    cp.sim_status == 0x83 && now_ms() - cp.sim_ms > 2000) {
			/* (only once the SIM says "all its files are ready", 0x83: asked 2 s after "ready", 0x82, the read was
			 * refused with result 0x8001)
			 * Samsung IPC's SIM group (0x05), number 5 "get": the command (0xB0: read), the file (0x6FAD, low byte
			 * first), three parameters (offset 0, 0, 4 bytes): the public numbering's layout for older modems */
			static const uint8_t read_ad[6] = { 0xb0, 0xad, 0x6f, 0x00, 0x00, 0x04 };

			cp.sim_asked = true;
			say("cp: one SIM file read asked (EF_AD, 4 bytes, no identity in it)\n");
			(void)fmt_send(0x05, 0x05, 0x02, read_ad, sizeof(read_ad));
		}
		if (cp.version_answered && !cp.bands_asked && cp.phone_state == 1) {
			cp.bands_asked = true;
			ask_bands();
		}
		if (now_ms() >= until) {
			break;
		}
		if (cp.if_on && cp.data_step == 3 && pdp.ifp != NULL && cp.normal_asked && !cp.back_asked) {
			/* the everyday steady state (cp_if=1, the context up): every 1 ms while anything moves, every 10 ms after
			 * 0.3 s of quiet, every 50 ms after 5 s (the mailbox is polled: its interrupts stay masked) */
			uint64_t quiet;

			if (pdp.head != pdp.tail) {
				cp.busy_ms = now_ms();
			}
			quiet = now_ms() - cp.busy_ms;
			IOSleep(quiet > 5000 ? 50 : quiet > 300 ? 10 : 1);
		} else {
			IOSleep(1);
		}
	}
	if (cp.normal_asked) {
		say("cp: STAGE 5 SUMMARY: normal mode asked; registered: %s; last registration state %d; %u changes of it; %u "
		    "signal reports; low power again: %s; NV writes the CP asked for and that were dropped: %u\n",
		    cp.registered_ms != 0 ? "YES" : "no", cp.reg_state, cp.reg_changes, cp.signals,
		    cp.back_asked && cp.phone_state == 1 ? "confirmed" : "NOT confirmed", cp.writes_dropped);
	}
	if (cp.data_step != 0) {
		say("cp: STAGE 6 SUMMARY: the data stage got to step %u (1 defined, 2 activation asked, 3 active, 4 deactivation "
		    "asked, 5 over); %u status notes, %u address configurations, %u packets of %u bytes from the CP on the data "
		    "channels\n", cp.data_step, cp.data_notes, cp.data_configs, cp.data_packets,
		    cp.data_bytes);
	}
	if (cp.stage >= 7 && cp.data_step != 0) {
		say("cp: STAGE 7 SUMMARY: up %u packets of %u bytes; down %u packets of %u bytes: echo %s (%llu ms), DNS %s (%llu ms, "
		    "response code %u, %u answers), not ours %u, IPv6 %u, MALFORMED %u; the zero-copy ring's next slot to offer %u, to "
		    "take %u\n", cp.data_up, cp.data_up_bytes, cp.data_packets, cp.data_bytes, cp.ping_ms != 0 ? "ANSWERED" :
		    "not answered", cp.ping_ms, cp.dns_ms != 0 ? "ANSWERED" : "not answered", cp.dns_ms, cp.dns_rcode,
		    cp.dns_answers, cp.data_other, cp.data_v6, cp.data_bad, cp.zc_alloc, cp.zc_pre);
	}
	bzero(cp.ip4, sizeof(cp.ip4));
	bzero(cp.dns4, sizeof(cp.dns4));
	cp.have4 = false;
	print_state();
	cp_stop(!alive ? "it crashed" : why != NULL ? why : why5 != NULL ? why5 : "the watch is over, or asked to");
	set_state(alive && why == NULL ? 200 : 104);
}

/* loader_return(): the CP in reset before the phone is handed back, if this run started it */
void hakios_shannon_quiesce(void);
void
hakios_shannon_quiesce(void)
{
	if (cp.started) {
		cp_go = 2;
		for (uint32_t i = 0; i < 500 && cp.started; i++) {
			IOSleep(10);
		}
	}
}

void hakios_shannon_init(void);
void
hakios_shannon_init(void)
{
	thread_t thread;
	uint32_t n = 0;

	if (!PE_parse_boot_argn("cp", &n, sizeof(n)) || n == 0) {
		return;
	}
	cp.stage = n > 7 ? 7 : n;
	cp.rev = 26;
	cp.wait_s = cp.stage >= 4 ? 25 : 20;
	cp.delay_s = 30;
	cp.phone_state = cp.sim_status = cp.reg_state = -1;
	cp.tech = cp.bars = -1;
	PE_parse_boot_argn("cp_rfs", &cp.rfs_open, sizeof(cp.rfs_open));
	cp.vss_on = 1;
	PE_parse_boot_argn("cp_nv", &cp.nv_file, sizeof(cp.nv_file));
	PE_parse_boot_argn("cp_if", &cp.if_on, sizeof(cp.if_on));
	PE_parse_boot_argn("cp_route", &cp.route_on, sizeof(cp.route_on));
	cp.if_kb = 2048;
	PE_parse_boot_argn("cp_if_kb", &cp.if_kb, sizeof(cp.if_kb));
	PE_parse_boot_argn("cp_vss", &cp.vss_on, sizeof(cp.vss_on));
	PE_parse_boot_argn("cp_rev", &cp.rev, sizeof(cp.rev));
	PE_parse_boot_argn("cp_ds", &cp.ds, sizeof(cp.ds));
	PE_parse_boot_argn("cp_wait", &cp.wait_s, sizeof(cp.wait_s));
	PE_parse_boot_argn("cp_delay", &cp.delay_s, sizeof(cp.delay_s));
	PE_parse_boot_argn("cp_sim", &cp.sim_probe, sizeof(cp.sim_probe));
	PE_parse_boot_argn("cp_bl", &cp.bl_scan, sizeof(cp.bl_scan));
	if (PE_parse_boot_argn("cp_apn", cp.apn, sizeof(cp.apn))) {
		cp.apn[sizeof(cp.apn) - 1] = 0;
		for (uint32_t i = 0; cp.apn[i] != 0; i++) {
			char c = cp.apn[i];

			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
				cp.apn[0] = 0;          /* not a name: no stage 6 */
				break;
			}
		}
	}
	cp.data_result = -1;
	{
		char text[20] = "";
		uint32_t part = 0, value = 0, digits = 0;
		bool good = PE_parse_boot_argn("cp_ping", text, sizeof(text));

		/* A.B.C.D, four numbers of at most 255 */
		text[sizeof(text) - 1] = 0;
		for (uint32_t i = 0; good; i++) {
			char c = text[i];

			if (c >= '0' && c <= '9') {
				value = value * 10 + (uint32_t)(c - '0');
				good = ++digits <= 3 && value <= 255;
			} else if (((c == '.' && part < 3) || (c == 0 && part == 3)) && digits != 0) {
				cp.ping4[part++] = (uint8_t)value;
				value = digits = 0;
				if (c == 0) {
					break;
				}
			} else {
				good = false;
			}
		}
		cp.ping_given = good && part == 4 && cp.ping4[0] != 0 && cp.ping4[0] != 127 && cp.ping4[0] < 224;
	}
	cp.dns_id = (uint16_t)(mach_absolute_time() >> 4);
	cp.dns_port = (uint16_t)(0xc000u | (mach_absolute_time() & 0x3fffu));
	if (cp.delay_s > 600) {
		cp.delay_s = 600;
	}
	if (cp.wait_s > 600) {
		cp.wait_s = 600;
	}
	if (kernel_thread_start(cp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("cp: no thread\n");
	}
}
